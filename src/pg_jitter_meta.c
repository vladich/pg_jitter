/*
 * pg_jitter_meta.c — Meta JIT provider for runtime backend switching
 *
 * This thin dispatcher registers as a JIT provider (jit_provider = 'pg_jitter')
 * and exposes a pg_jitter.backend GUC (PGC_USERSET) so users can switch between
 * sljit, asmjit, and mir backends at runtime without restarting PostgreSQL.
 *
 * Backend .dylib/.so files are loaded lazily on first use via
 * load_external_function() and cached for the lifetime of the process.
 *
 * Each backend remains independently usable (jit_provider = 'pg_jitter_sljit').
 *
 * Resource owner coordination: each backend dylib compiles its own copy of
 * pg_jitter_common.c, giving each a separate pg_jitter_resowner_desc address.
 * ResourceOwnerForget matches by pointer, so release_context must use the same
 * desc that Remember used. The meta-provider solves this by pre-creating the
 * JIT context (with the meta's own resowner desc) before delegating compile to
 * backends. The backend's pg_jitter_get_context() finds es_jit already set and
 * returns it without re-registering.
 */
#include "postgres.h"
#include "pg_jitter_compat.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "nodes/execnodes.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "executor/execExpr.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#ifdef _WIN64
#include "pg_crc32c_compat.h"
#endif
#include "portability/instr_time.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
#if PG_VERSION_NUM >= 150000
#include "common/pg_prng.h"
#endif

#define META_DEFORM_AVX512_MIN_COLS_DEFAULT 1200

/*
 * PG 14 doesn't mark pkglib_path with PGDLLIMPORT, so it can't be linked
 * from extension DLLs on Windows.  Derive it from my_exec_path (which IS
 * exported) at init time.
 */
#if PG_VERSION_NUM < 150000 && defined(_WIN32)
static char meta_pkglib_path[MAXPGPATH];
#define pkglib_path meta_pkglib_path
#endif

PG_MODULE_MAGIC_EXT(.name = "pg_jitter");

/* ----------------------------------------------------------------
 * Shmem slot table — minimal local copy for clearing DSM handles.
 * Must match JitDsmSlotTable layout in pg_jitter_common.h.
 * ---------------------------------------------------------------- */
typedef struct MetaDsmSlotTable
{
	int					num_slots;
	pg_atomic_uint32	handles[FLEXIBLE_ARRAY_MEMBER];
} MetaDsmSlotTable;

static MetaDsmSlotTable *meta_dsm_slots = NULL;
static bool meta_shmem_init_attempted = false;

static void
meta_shmem_clear_dsm_handle(void)
{
	int		proc_index = JITTER_MY_PROC_INDEX();
	bool	found;
	Size	size;

	/* Try to find existing shmem slot table */
	if (meta_dsm_slots == NULL)
	{
		if (meta_shmem_init_attempted)
			return;
		meta_shmem_init_attempted = true;

		size = offsetof(MetaDsmSlotTable, handles) +
			   sizeof(pg_atomic_uint32) * MaxBackends;

		PG_TRY();
		{
			meta_dsm_slots = (MetaDsmSlotTable *)
				ShmemInitStruct("pg_jitter_dsm_slots", size, &found);
		}
		PG_CATCH();
		{
			FlushErrorState();
			return;
		}
		PG_END_TRY();

		if (!found)
			return;		/* table doesn't exist yet, nothing to clear */
	}

	if (proc_index >= 0 && proc_index < meta_dsm_slots->num_slots)
		pg_atomic_write_u32(&meta_dsm_slots->handles[proc_index], 0);
}

/* Forward declaration for adaptive timing node (defined after BackendEntry) */
typedef struct AdaptiveTimingNode AdaptiveTimingNode;

/* ----------------------------------------------------------------
 * PgJitterContext — must match the layout in pg_jitter_common.h
 * ---------------------------------------------------------------- */
typedef struct MetaCompiledCode
{
	struct MetaCompiledCode *next;
	void	(*free_fn)(void *data);
	void   *data;
} MetaCompiledCode;

/*
 * JitShareState — must match pg_jitter_common.h for layout compatibility.
 * Duplicated here because meta.c doesn't include pg_jitter_common.h.
 */
typedef struct MetaJitShareState
{
	dsm_segment *dsm_seg;
	void	   *sjc;			/* SharedJitCompiledCode * */
	bool		initialized;
	bool		is_leader;
} MetaJitShareState;

typedef struct MetaJitterContext
{
	JitContext			base;			/* must be first */
	MetaCompiledCode   *compiled_list;
	ResourceOwner		resowner;

	/*
	 * Must mirror PgJitterContext layout for shared code support.
	 * The sljit/asmjit/mir backends cast es_jit to PgJitterContext
	 * and access these fields.
	 */
	int					last_plan_node_id;
	int					expr_ordinal;

	/* DSM-based shared code state — mirrors JitShareState */
	MetaJitShareState	share_state;

	/* Mirrors PgJitterContext.aux_context for backend helper allocations */
	MemoryContext		aux_context;

	/* Bitmask of backends that compiled something in this context */
	uint8				backends_used;

	/* Adaptive timing contexts active for this JIT context */
	AdaptiveTimingNode *adaptive_timings;
} MetaJitterContext;

/* ----------------------------------------------------------------
 * Resource owner support — the meta's own copy
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 170000

static void MetaResOwnerRelease(Datum res);

static const ResourceOwnerDesc meta_resowner_desc =
{
	.name = "pg_jitter JIT context",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_JIT_CONTEXTS,
	.ReleaseResource = MetaResOwnerRelease,
	.DebugPrint = NULL
};

static inline void
MetaRememberContext(ResourceOwner owner, MetaJitterContext *handle)
{
	ResourceOwnerRemember(owner, PointerGetDatum(handle), &meta_resowner_desc);
}

static inline void
MetaForgetContext(ResourceOwner owner, MetaJitterContext *handle)
{
	ResourceOwnerForget(owner, PointerGetDatum(handle), &meta_resowner_desc);
}

static void
MetaResOwnerRelease(Datum res)
{
	MetaJitterContext *context = (MetaJitterContext *) DatumGetPointer(res);

	context->resowner = NULL;
	jit_release_context(&context->base);
}

#else /* PG14-16: JIT-specific resource owner API */

#include "utils/resowner_private.h"

static inline void
MetaRememberContext(ResourceOwner owner, MetaJitterContext *handle)
{
	ResourceOwnerRememberJIT(owner, PointerGetDatum(handle));
}

static inline void
MetaForgetContext(ResourceOwner owner, MetaJitterContext *handle)
{
	ResourceOwnerForgetJIT(owner, PointerGetDatum(handle));
}

#endif /* PG_VERSION_NUM >= 170000 */

/* ----------------------------------------------------------------
 * Backend enum and GUC options
 * ---------------------------------------------------------------- */
enum PgJitterBackend
{
	PG_JITTER_BACKEND_SLJIT = 0,
	PG_JITTER_BACKEND_ASMJIT = 1,
	PG_JITTER_BACKEND_MIR = 2,
	PG_JITTER_NUM_BACKENDS,
	PG_JITTER_BACKEND_AUTO,
};

static const struct config_enum_entry backend_options[] = {
	{"sljit", PG_JITTER_BACKEND_SLJIT, false},
	{"asmjit", PG_JITTER_BACKEND_ASMJIT, false},
	{"mir", PG_JITTER_BACKEND_MIR, false},
	{"auto", PG_JITTER_BACKEND_AUTO, false},
	{NULL, 0, false}
};

static const char *backend_libnames[] = {
	"pg_jitter_sljit",
	"pg_jitter_asmjit",
	"pg_jitter_mir",
};

static int pg_jitter_backend = PG_JITTER_BACKEND_SLJIT;

/* GUC: pg_jitter.parallel_mode — defined here so it's available before backends load */
static int meta_parallel_mode = 1;		/* PARALLEL_JIT_PER_WORKER */
static int meta_shared_code_max_kb = 4096;	/* 4 MB */
static bool meta_deform_cache = true;
static bool meta_deform_avx512 = true;
static int meta_deform_avx512_min_cols = META_DEFORM_AVX512_MIN_COLS_DEFAULT;
static int meta_min_expr_steps = 4;
static bool meta_adaptive = true;
static int meta_adaptive_samples = 64;
static double meta_adaptive_epsilon = 0.05;
#define PARALLEL_JIT_OFF        0
#define PARALLEL_JIT_PER_WORKER 1
#define PARALLEL_JIT_SHARED     2

/* ----------------------------------------------------------------
 * Cached backend state
 * ---------------------------------------------------------------- */
typedef struct BackendEntry
{
	bool				attempted;	/* tried to load */
	bool				available;	/* successfully loaded */
	JitProviderCallbacks cb;
	void			  (*deform_reset)(void);	/* pg_jitter_deform_dispatch_reset_fastpath */
} BackendEntry;

static BackendEntry backends[PG_JITTER_NUM_BACKENDS];

/* ----------------------------------------------------------------
 * Adaptive statistics — expression profile stats
 *
 * Collects per-(expression_profile, backend) timing data across all
 * backends in the cluster.  Each expression is profiled by its opcode
 * shape (step count, deform natts, hashdatum count, etc.) and hashed
 * with CRC32C.  Timing data is collected via a self-removing wrapper
 * around evalfunc that measures the first N calls, then disappears.
 *
 * The stats table lives in process-local memory (TopMemoryContext).
 * Each backend learns independently — convergence is fast since
 * expression profiles are deterministic.
 * ---------------------------------------------------------------- */
#define ADAPTIVE_STATS_MAX_ENTRIES  256
#define ADAPTIVE_NUM_BACKENDS       3   /* sljit=0, asmjit=1, mir=2 */
#define ADAPTIVE_IDX_SLJIT          0
#define ADAPTIVE_IDX_ASMJIT         1
#define ADAPTIVE_IDX_MIR            2

/* Expression profile — captures the "shape" of an expression */
typedef struct ExprProfile
{
	uint16	nsteps;
	uint16	fetchsome_natts;
	uint8	n_hashdatum;
	uint8	n_funcexpr;
	uint8	n_qual;
	uint8	n_agg;
} ExprProfile;

/* Per-profile, per-backend timing stats (process-local) */
typedef struct AdaptiveStatsEntry
{
	uint32		profile_hash;		/* 0 = empty slot */

	/* Expression profile details (set once at insertion, never updated) */
	uint16		nsteps;
	uint16		fetchsome_natts;
	uint8		n_hashdatum;
	uint8		n_funcexpr;
	uint8		n_qual;
	uint8		n_agg;

	/* Per-backend stats */
	uint32		call_count[ADAPTIVE_NUM_BACKENDS];
	uint64		compile_ns[ADAPTIVE_NUM_BACKENDS];	/* compilation time */
	uint64		exec_ns[ADAPTIVE_NUM_BACKENDS];		/* execution time */
} AdaptiveStatsEntry;

typedef struct AdaptiveStatsTable
{
	int			num_entries;
	int			max_entries;
	AdaptiveStatsEntry	entries[FLEXIBLE_ARRAY_MEMBER];
} AdaptiveStatsTable;

static AdaptiveStatsTable *adaptive_stats = NULL;
static bool adaptive_shmem_attempted = false;

/* Per-expression timing context (process-local, heap-allocated) */
typedef struct AdaptiveTimingCtx
{
	ExprStateEvalFunc	original_evalfunc;
	void			   *original_evalfunc_priv;
	uint32				profile_hash;
	ExprProfile			profile;		/* expression profile for stats insertion */
	int					backend_idx;	/* ADAPTIVE_IDX_SLJIT, _ASMJIT, or _MIR */
	int					call_count;
	int64				exec_ns;		/* accumulated execution time */
	int64				compile_ns;		/* compilation time (set once) */
	bool				done;			/* true = samples collected, wrapper removed */
} AdaptiveTimingCtx;

/* List of active timing contexts for cleanup on context release */
struct AdaptiveTimingNode
{
	struct AdaptiveTimingNode  *next;
	AdaptiveTimingCtx          *tctx;
	ExprState                  *state;
};

/* ----------------------------------------------------------------
 * Backend loading
 * ---------------------------------------------------------------- */

/*
 * Attempt to load a backend by index. Returns true if available.
 * Probes for the .dylib/.so file first (avoids ERROR from
 * load_external_function on missing files).
 */
static bool
meta_load_backend(int idx)
{
	char			path[MAXPGPATH];
	JitProviderInit init_fn;

	Assert(idx >= 0 && idx < PG_JITTER_NUM_BACKENDS);

	if (backends[idx].attempted)
		return backends[idx].available;

	backends[idx].attempted = true;
	backends[idx].available = false;

	/* Probe: does the shared library file exist? */
	snprintf(path, MAXPGPATH, "%s/%s%s",
			 pkglib_path, backend_libnames[idx], DLSUFFIX);

	if (!pg_file_exists(path))
	{
		elog(DEBUG1, "pg_jitter: backend %s not found at %s",
			 backend_libnames[idx], path);
		return false;
	}

	/* Load and initialize — catch any errors (e.g., missing symbols) */
	PG_TRY();
	{
		init_fn = (JitProviderInit)
			load_external_function(path, "_PG_jit_provider_init", true, NULL);

		init_fn(&backends[idx].cb);
		backends[idx].available = true;

		/* Look up the deform cache reset function (each backend has its own copy) */
		backends[idx].deform_reset = (void (*)(void))
			load_external_function(path,
								   "pg_jitter_deform_dispatch_reset_fastpath",
								   false, NULL);

		elog(DEBUG1, "pg_jitter: loaded backend %s", backend_libnames[idx]);
	}
	PG_CATCH();
	{
		FlushErrorState();
		elog(WARNING, "pg_jitter: failed to load backend %s",
			 backend_libnames[idx]);

		/* Mark as attempted-but-unavailable */
		backends[idx].available = false;
		backends[idx].deform_reset = NULL;
	}
	PG_END_TRY();

	return backends[idx].available;
}

/* ----------------------------------------------------------------
 * GUC assign hook — eagerly attempt load on SET
 * ---------------------------------------------------------------- */
static void
meta_backend_assign(int newval, void *extra)
{
	if (newval == PG_JITTER_BACKEND_AUTO)
	{
		/* Eagerly load all available backends for auto mode */
		for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
			meta_load_backend(idx);
		return;
	}

	if (!meta_load_backend(newval))
		ereport(WARNING,
				(errmsg("pg_jitter backend \"%s\" is not installed, will fall back",
						backend_libnames[newval])));
}

/* ----------------------------------------------------------------
 * Context pre-creation
 *
 * Pre-create the JIT context with the meta-provider's resowner desc
 * so that release_context can use the matching desc for ForgetContext.
 * The backend's pg_jitter_get_context() sees es_jit already set and
 * returns it without re-registering with its own desc.
 * ---------------------------------------------------------------- */
static void
meta_ensure_context(ExprState *state)
{
	PlanState	   *parent = state->parent;
	MetaJitterContext *ctx;

	Assert(parent != NULL);

	if (parent->state->es_jit)
		return;		/* context already exists */

#if PG_VERSION_NUM >= 170000
	ResourceOwnerEnlarge(CurrentResourceOwner);
#else
	ResourceOwnerEnlargeJIT(CurrentResourceOwner);
#endif

	ctx = (MetaJitterContext *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(MetaJitterContext));
	ctx->base.flags = parent->state->es_jit_flags;
#if PG_VERSION_NUM < 170000
	ctx->base.resowner = CurrentResourceOwner;
#endif
	ctx->compiled_list = NULL;
	ctx->resowner = CurrentResourceOwner;
	ctx->last_plan_node_id = -1;
	ctx->expr_ordinal = 0;
	memset(&ctx->share_state, 0, sizeof(MetaJitShareState));
	ctx->aux_context = NULL;
	ctx->adaptive_timings = NULL;

	MetaRememberContext(CurrentResourceOwner, ctx);

	parent->state->es_jit = &ctx->base;
}

/* ----------------------------------------------------------------
 * Adaptive statistics — management
 * ---------------------------------------------------------------- */

/*
 * Convert backend enum (PG_JITTER_BACKEND_*) to adaptive index (0/1/2).
 * Returns -1 for invalid backends.
 */
static inline int
adaptive_backend_to_idx(int backend)
{
	switch (backend)
	{
		case PG_JITTER_BACKEND_SLJIT:	return ADAPTIVE_IDX_SLJIT;
		case PG_JITTER_BACKEND_ASMJIT:	return ADAPTIVE_IDX_ASMJIT;
		case PG_JITTER_BACKEND_MIR:		return ADAPTIVE_IDX_MIR;
		default:						return -1;
	}
}

static inline int
adaptive_idx_to_backend(int idx)
{
	switch (idx)
	{
		case ADAPTIVE_IDX_SLJIT:	return PG_JITTER_BACKEND_SLJIT;
		case ADAPTIVE_IDX_ASMJIT:	return PG_JITTER_BACKEND_ASMJIT;
		case ADAPTIVE_IDX_MIR:		return PG_JITTER_BACKEND_MIR;
		default:					return PG_JITTER_BACKEND_SLJIT;
	}
}

/*
 * Initialize the adaptive stats table in process-local memory.
 *
 * The JIT provider is loaded lazily (not via shared_preload_libraries),
 * so we cannot use ShmemInitStruct.  Each backend process maintains its
 * own stats table in TopMemoryContext.  Stats are per-process but
 * expressions are deterministic — same query structure → same profile
 * hash → converges quickly within a single session.
 */
static void
adaptive_stats_init(void)
{
	Size	size;

	if (adaptive_stats != NULL || adaptive_shmem_attempted)
		return;

	adaptive_shmem_attempted = true;

	size = offsetof(AdaptiveStatsTable, entries) +
		   sizeof(AdaptiveStatsEntry) * ADAPTIVE_STATS_MAX_ENTRIES;

	adaptive_stats = (AdaptiveStatsTable *)
		MemoryContextAllocZero(TopMemoryContext, size);

	adaptive_stats->num_entries = 0;
	adaptive_stats->max_entries = ADAPTIVE_STATS_MAX_ENTRIES;
}

/*
 * Build an expression profile by scanning the step array.
 */
static void
meta_build_profile(ExprState *state, ExprProfile *profile)
{
	ExprEvalStep *steps = state->steps;
	int			  nsteps = state->steps_len;

	memset(profile, 0, sizeof(ExprProfile));
	profile->nsteps = (uint16) Min(nsteps, UINT16_MAX);

	for (int i = 0; i < nsteps; i++)
	{
		ExprEvalOp opcode = ExecEvalStepOp(state, &steps[i]);

		switch (opcode)
		{
			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
				profile->fetchsome_natts += steps[i].d.fetch.last_var;
				break;

#ifdef HAVE_EEOP_HASHDATUM
			case EEOP_HASHDATUM_SET_INITVAL:
			case EEOP_HASHDATUM_FIRST:
			case EEOP_HASHDATUM_FIRST_STRICT:
			case EEOP_HASHDATUM_NEXT32:
			case EEOP_HASHDATUM_NEXT32_STRICT:
				profile->n_hashdatum++;
				break;
#endif

			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
			case EEOP_FUNCEXPR_FUSAGE:
			case EEOP_FUNCEXPR_STRICT_FUSAGE:
				profile->n_funcexpr++;
				break;

			case EEOP_QUAL:
				profile->n_qual++;
				break;

			case EEOP_AGG_PLAIN_TRANS_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
				profile->n_agg++;
				break;

			default:
				break;
		}
	}
}

/*
 * Hash an expression profile using CRC32C.
 * The profile struct is 8 bytes — one hardware CRC instruction on ARM64/x86.
 */
static uint32
meta_profile_hash(const ExprProfile *profile)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, profile, sizeof(ExprProfile));
	FIN_CRC32C(crc);
	return (uint32) crc;
}

/*
 * Look up a profile hash in the stats table.
 * Returns the entry index, or -1 if not found.
 */
static int
adaptive_stats_lookup(uint32 profile_hash)
{
	int	n;

	if (adaptive_stats == NULL)
		return -1;

	n = adaptive_stats->num_entries;
	for (int i = 0; i < n; i++)
	{
		if (adaptive_stats->entries[i].profile_hash == profile_hash)
			return i;
	}
	return -1;
}

/*
 * Insert a new profile hash into the stats table, storing the profile details.
 * Returns the entry index, or -1 if the table is full.
 */
static int
adaptive_stats_insert(uint32 profile_hash, const ExprProfile *profile)
{
	int		slot;
	AdaptiveStatsEntry *e;

	if (adaptive_stats == NULL)
		return -1;

	/* Check if already inserted */
	{
		int existing = adaptive_stats_lookup(profile_hash);
		if (existing >= 0)
			return existing;
	}

	if (adaptive_stats->num_entries >= adaptive_stats->max_entries)
		return -1;		/* table full */

	slot = adaptive_stats->num_entries++;
	e = &adaptive_stats->entries[slot];

	e->nsteps = profile->nsteps;
	e->fetchsome_natts = profile->fetchsome_natts;
	e->n_hashdatum = profile->n_hashdatum;
	e->n_funcexpr = profile->n_funcexpr;
	e->n_qual = profile->n_qual;
	e->n_agg = profile->n_agg;
	e->profile_hash = profile_hash;

	return slot;
}

/*
 * Record timing data into the stats table.
 * compile_ns is the one-time compilation cost; exec_ns is accumulated execution time.
 * profile may be NULL if the entry already exists.
 */
static void
adaptive_stats_record(uint32 profile_hash, int adaptive_idx,
					  int64 compile_ns, int64 exec_ns, int call_count,
					  const ExprProfile *profile)
{
	int		slot;
	static const ExprProfile empty_profile = {0};

	if (adaptive_stats == NULL || adaptive_idx < 0)
		return;

	slot = adaptive_stats_lookup(profile_hash);
	if (slot < 0)
		slot = adaptive_stats_insert(profile_hash,
									 profile ? profile : &empty_profile);
	if (slot < 0)
		return;		/* table full, discard stats */

	adaptive_stats->entries[slot].call_count[adaptive_idx] += (uint32) call_count;
	if (compile_ns > 0)
		adaptive_stats->entries[slot].compile_ns[adaptive_idx] += (uint64) compile_ns;
	if (exec_ns > 0)
		adaptive_stats->entries[slot].exec_ns[adaptive_idx] += (uint64) exec_ns;
}

/*
 * Select a backend using adaptive stats.
 *
 * Cost model: total_cost = compile_ns + exec_ns (over the sample window).
 * This amortizes compilation overhead over the observed invocations —
 * backends with high compilation cost (MIR) need proportionally faster
 * per-call execution to win.
 *
 * Default priority: sljit > asmjit > mir.  A non-default backend is
 * selected only if its total cost is strictly lower than sljit's.
 *
 * Returns a PG_JITTER_BACKEND_* value, or -1 if insufficient data
 * (caller should use the default: sljit).
 */
static int
adaptive_select(uint32 profile_hash)
{
	int		slot;
	uint32	counts[ADAPTIVE_NUM_BACKENDS];
	double	costs[ADAPTIVE_NUM_BACKENDS];
	int		n_measured = 0;
	int		best_idx = ADAPTIVE_IDX_SLJIT;
	double	best_cost;
	AdaptiveStatsEntry *e;

	slot = adaptive_stats_lookup(profile_hash);
	if (slot < 0)
		return -1;

	e = &adaptive_stats->entries[slot];

	/* Read stats for all backends */
	for (int b = 0; b < ADAPTIVE_NUM_BACKENDS; b++)
	{
		counts[b] = e->call_count[b];
		if (counts[b] >= (uint32) meta_adaptive_samples)
		{
			uint64 comp = e->compile_ns[b];
			uint64 exec = e->exec_ns[b];
			costs[b] = (double) comp + (double) exec;
			n_measured++;
		}
		else
			costs[b] = -1.0;	/* not yet measured */
	}

	/* Need sljit measured plus at least one other */
	if (costs[ADAPTIVE_IDX_SLJIT] < 0.0 || n_measured < 2)
	{
		/*
		 * Bootstrap exploration: the heuristic has data for the primary
		 * backend, occasionally try an unmeasured backend to validate.
		 * ~1.5% chance, uniformly distributed among unmeasured backends.
		 */
		if (counts[ADAPTIVE_IDX_SLJIT] > 0)
		{
			int unmeasured[ADAPTIVE_NUM_BACKENDS];
			int n_unmeasured = 0;
			uint32 rand_val;

			for (int b = 0; b < ADAPTIVE_NUM_BACKENDS; b++)
			{
				if (counts[b] == 0 && backends[adaptive_idx_to_backend(b)].available)
					unmeasured[n_unmeasured++] = b;
			}

			if (n_unmeasured > 0)
			{
#if PG_VERSION_NUM >= 150000
				rand_val = pg_prng_uint32(&pg_global_prng_state);
#else
				rand_val = (uint32) random();
#endif
				/* ~1.5% chance to explore unmeasured backends */
				if ((rand_val % 64) == 0)
				{
					int pick = unmeasured[rand_val / 64 % n_unmeasured];
					elog(DEBUG1, "pg_jitter adaptive: bootstrap explore %s "
						 "(profile 0x%08x)",
						 backend_libnames[adaptive_idx_to_backend(pick)],
						 profile_hash);
					return adaptive_idx_to_backend(pick);
				}
			}
		}
		return -1;
	}

	/* Find the backend with lowest total cost */
	best_cost = costs[ADAPTIVE_IDX_SLJIT];
	for (int b = 0; b < ADAPTIVE_NUM_BACKENDS; b++)
	{
		if (costs[b] >= 0.0 && costs[b] < best_cost)
		{
			best_cost = costs[b];
			best_idx = b;
		}
	}

	/* Epsilon-greedy exploration among measured backends */
	if (meta_adaptive_epsilon > 0.0)
	{
		uint32 rand_val;

#if PG_VERSION_NUM >= 150000
		rand_val = pg_prng_uint32(&pg_global_prng_state);
#else
		rand_val = (uint32) random();
#endif

		if ((double)(rand_val % 10000) / 10000.0 < meta_adaptive_epsilon)
		{
			/* Pick a random measured backend that isn't the best */
			int candidates[ADAPTIVE_NUM_BACKENDS];
			int n_cand = 0;

			for (int b = 0; b < ADAPTIVE_NUM_BACKENDS; b++)
			{
				if (b != best_idx && costs[b] >= 0.0)
					candidates[n_cand++] = b;
			}
			if (n_cand > 0)
			{
				int pick = candidates[rand_val / 10000 % n_cand];
				elog(DEBUG2, "pg_jitter adaptive: explore %s (profile 0x%08x)",
					 backend_libnames[adaptive_idx_to_backend(pick)],
					 profile_hash);
				return adaptive_idx_to_backend(pick);
			}
		}
	}

	elog(DEBUG2, "pg_jitter adaptive: exploit %s (profile 0x%08x, "
		 "costs: sljit=%.0f asmjit=%.0f mir=%.0f)",
		 backend_libnames[adaptive_idx_to_backend(best_idx)],
		 profile_hash,
		 costs[ADAPTIVE_IDX_SLJIT],
		 costs[ADAPTIVE_IDX_ASMJIT] >= 0 ? costs[ADAPTIVE_IDX_ASMJIT] : -1.0,
		 costs[ADAPTIVE_IDX_MIR] >= 0 ? costs[ADAPTIVE_IDX_MIR] : -1.0);

	return adaptive_idx_to_backend(best_idx);
}

/* ----------------------------------------------------------------
 * Adaptive timing wrapper — self-removing evalfunc wrapper
 * ---------------------------------------------------------------- */

static Datum
meta_adaptive_timing_wrapper(ExprState *state, ExprContext *econtext,
							 bool *isNull)
{
	AdaptiveTimingCtx *tctx = (AdaptiveTimingCtx *) state->evalfunc_private;
	ExprStateEvalFunc  saved_func = tctx->original_evalfunc;
	void			  *saved_priv = tctx->original_evalfunc_priv;
	instr_time		   start_time, end_time;
	Datum			   result;

	/*
	 * Restore original evalfunc/private so the real function works.
	 * The validation wrapper (pg_jitter_run_compiled_expr) may replace
	 * evalfunc with the raw compiled function on its first call — that's
	 * fine, we'll capture whatever it leaves behind.
	 */
	state->evalfunc = saved_func;
	state->evalfunc_private = saved_priv;

	INSTR_TIME_SET_CURRENT(start_time);
	result = state->evalfunc(state, econtext, isNull);
	INSTR_TIME_SET_CURRENT(end_time);

	tctx->call_count++;
	tctx->exec_ns += INSTR_TIME_GET_NANOSEC(end_time) -
					 INSTR_TIME_GET_NANOSEC(start_time);

	/*
	 * Capture what evalfunc/private are NOW (after the real function ran).
	 * The validation wrapper replaces itself on first call, so subsequent
	 * calls should use the updated values.
	 */
	tctx->original_evalfunc = state->evalfunc;
	tctx->original_evalfunc_priv = state->evalfunc_private;

	if (tctx->call_count >= meta_adaptive_samples)
	{
		/* Flush accumulated stats to shared memory */
		adaptive_stats_record(tctx->profile_hash, tctx->backend_idx,
							  tctx->compile_ns, tctx->exec_ns,
							  tctx->call_count, &tctx->profile);

		elog(DEBUG1, "pg_jitter adaptive: recorded %d calls, "
			 "compile %lld ns, exec %lld ns for %s "
			 "(profile 0x%08x, avg %.0f ns/call)",
			 tctx->call_count,
			 (long long) tctx->compile_ns,
			 (long long) tctx->exec_ns,
			 backend_libnames[adaptive_idx_to_backend(tctx->backend_idx)],
			 tctx->profile_hash,
			 (double) tctx->exec_ns / tctx->call_count);

		/*
		 * Mark as done — leave evalfunc/private as the real function.
		 * The tctx is freed during meta_release_context, not here, to
		 * avoid dangling pointers in the adaptive_timings tracking list.
		 */
		tctx->done = true;
		return result;
	}

	/* Re-install wrapper for next call */
	state->evalfunc_private = tctx;
	state->evalfunc = meta_adaptive_timing_wrapper;

	return result;
}

/*
 * Install the adaptive timing wrapper on a compiled expression.
 * Must be called AFTER the backend has compiled and installed its evalfunc.
 * compile_ns is the time spent compiling this expression (measured in meta_compile_expr).
 */
static void
meta_install_timing_wrapper(ExprState *state, int backend_enum,
							uint32 profile_hash, int64 compile_ns,
							const ExprProfile *profile)
{
	AdaptiveTimingCtx  *tctx;
	MetaJitterContext  *ctx;
	AdaptiveTimingNode *node;
	int					adaptive_idx;

	adaptive_idx = adaptive_backend_to_idx(backend_enum);
	if (adaptive_idx < 0)
		return;		/* invalid backend */

	/* Only wrap if the expression was actually compiled */
	if (state->evalfunc == NULL)
		return;

	/* Skip wrapping for expressions without a parent (can't track for cleanup) */
	if (!state->parent || !state->parent->state->es_jit)
		return;

	/*
	 * Skip wrapping if this backend already has sufficient data for this
	 * profile.  The wrapper only measures the backend that was selected;
	 * there's no point re-measuring a backend we already have stats for.
	 */
	{
		int slot = adaptive_stats_lookup(profile_hash);
		if (slot >= 0 &&
			adaptive_stats->entries[slot].call_count[adaptive_idx]
				>= (uint32) meta_adaptive_samples)
			return;		/* already have enough data for this backend */
	}

	tctx = (AdaptiveTimingCtx *)
		MemoryContextAlloc(TopMemoryContext, sizeof(AdaptiveTimingCtx));
	tctx->original_evalfunc = state->evalfunc;
	tctx->original_evalfunc_priv = state->evalfunc_private;
	tctx->profile_hash = profile_hash;
	tctx->profile = *profile;
	tctx->backend_idx = adaptive_idx;
	tctx->call_count = 0;
	tctx->exec_ns = 0;
	tctx->compile_ns = compile_ns;
	tctx->done = false;

	state->evalfunc = meta_adaptive_timing_wrapper;
	state->evalfunc_private = tctx;

	/*
	 * Track this timing context so we can clean it up if the JIT context
	 * is released before the sample count is reached.
	 */
	if (state->parent && state->parent->state->es_jit)
	{
		ctx = (MetaJitterContext *) state->parent->state->es_jit;
		node = (AdaptiveTimingNode *)
			MemoryContextAlloc(TopMemoryContext, sizeof(AdaptiveTimingNode));
		node->tctx = tctx;
		node->state = state;
		node->next = ctx->adaptive_timings;
		ctx->adaptive_timings = node;
	}
}

/*
 * SQL function registration is handled manually:
 *
 *   CREATE FUNCTION pg_jitter_current_backend() RETURNS text
 *     LANGUAGE c STRICT AS 'pg_jitter', 'pg_jitter_current_backend';
 *
 *   CREATE FUNCTION pg_jitter_adaptive_stats(
 *     OUT profile_hash text, OUT nsteps int, OUT fetchsome_natts int,
 *     OUT n_hashdatum int, OUT n_funcexpr int, OUT n_qual int, OUT n_agg int,
 *     OUT sljit_calls int, OUT sljit_compile_ns float8, OUT sljit_avg_ns float8,
 *     OUT asmjit_calls int, OUT asmjit_compile_ns float8, OUT asmjit_avg_ns float8,
 *     OUT mir_calls int, OUT mir_compile_ns float8, OUT mir_avg_ns float8,
 *     OUT selected text, OUT margin_pct float8
 *   ) RETURNS SETOF record
 *     LANGUAGE c STRICT AS 'pg_jitter', 'pg_jitter_adaptive_stats';
 *
 *   CREATE FUNCTION pg_jitter_adaptive_stats_reset() RETURNS void
 *     LANGUAGE c STRICT AS 'pg_jitter', 'pg_jitter_adaptive_stats_reset';
 */

/* ----------------------------------------------------------------
 * Provider callbacks — dispatch to loaded backend
 * ---------------------------------------------------------------- */

/*
 * Default backend priority: sljit > asmjit > mir.
 *
 * Always returns sljit as the default.  The adaptive statistics system
 * may override this if runtime data shows a different backend is faster
 * (considering both compilation cost and per-invocation execution time).
 */
static int
meta_default_backend(void)
{
	if (backends[PG_JITTER_BACKEND_SLJIT].available)
		return PG_JITTER_BACKEND_SLJIT;
	if (backends[PG_JITTER_BACKEND_ASMJIT].available)
		return PG_JITTER_BACKEND_ASMJIT;
	if (backends[PG_JITTER_BACKEND_MIR].available)
		return PG_JITTER_BACKEND_MIR;
	return PG_JITTER_BACKEND_SLJIT;	/* nominal fallback */
}

/*
 * Analyze expression steps and select the best backend for this expression.
 *
 * First tries adaptive selection (stats-driven), falls back to the static
 * heuristic if insufficient data.  Stores the profile hash in *profile_hash_out
 * for use by the timing wrapper.
 */
static int
meta_auto_select_backend(ExprState *state, uint32 *profile_hash_out,
						 ExprProfile *profile_out)
{
	ExprProfile		profile;
	uint32			profile_hash;
	int				selected;

	/* Build profile and hash it */
	meta_build_profile(state, &profile);
	profile_hash = meta_profile_hash(&profile);
	*profile_hash_out = profile_hash;
	*profile_out = profile;

	/*
	 * Static heuristic: pick backend based on expression profile.
	 *
	 * asmjit excels at deform-heavy workloads (hash joins, wide tables).
	 * sljit excels at expression-heavy workloads (CASE, arithmetic, IN).
	 *
	 * Rules (applied in order):
	 * 1. Hash join with deform → asmjit
	 * 2. Deform-heavy (natts >= 10, deform > 1/3 of steps) → asmjit
	 * 3. Everything else → sljit
	 */
	if (backends[PG_JITTER_BACKEND_ASMJIT].available)
	{
		bool use_asmjit = false;

		/* Hash join build/probe with deform work */
		if (profile.n_hashdatum > 0 && profile.fetchsome_natts > 0)
			use_asmjit = true;

		/* Deform-heavy: natts >= 10 and deform represents > 1/3 of steps */
#ifdef __aarch64__
		/* ARM64: asmjit deform advantage is larger, lower threshold */
		else if (profile.fetchsome_natts >= 10 &&
				 profile.fetchsome_natts * 3 > profile.nsteps)
			use_asmjit = true;
#else
		/* x86_64: need wider tuples for asmjit to win */
		else if (profile.fetchsome_natts >= 20 &&
				 profile.fetchsome_natts * 3 > profile.nsteps)
			use_asmjit = true;
#endif

		selected = use_asmjit ? PG_JITTER_BACKEND_ASMJIT
							  : PG_JITTER_BACKEND_SLJIT;
	}
	else
	{
		selected = meta_default_backend();
	}

	/* Adaptive override: if we have enough data showing a different winner,
	 * use that instead of the static heuristic. */
	if (meta_adaptive && adaptive_stats != NULL)
	{
		int adaptive_choice = adaptive_select(profile_hash);
		if (adaptive_choice >= 0 && backends[adaptive_choice].available)
			selected = adaptive_choice;
	}

	elog(DEBUG1, "pg_jitter auto: steps=%d natts=%d hashdatum=%d -> %s",
		 profile.nsteps, profile.fetchsome_natts, profile.n_hashdatum,
		 backend_libnames[selected]);

	return selected;
}

static bool
meta_compile_expr(ExprState *state)
{
	int			idx = pg_jitter_backend;
	bool		is_auto = (idx == PG_JITTER_BACKEND_AUTO);
	uint32		profile_hash = 0;
	ExprProfile	profile;

	memset(&profile, 0, sizeof(ExprProfile));

	/*
	 * Pre-create the context with our resowner desc before the backend
	 * gets a chance to create it with its own desc.
	 */
	if (state->parent)
		meta_ensure_context(state);

	/* Initialize adaptive stats table lazily on first auto compile */
	if (is_auto && meta_adaptive && adaptive_stats == NULL)
		adaptive_stats_init();

	/* Resolve auto mode to a concrete backend */
	if (is_auto)
		idx = meta_auto_select_backend(state, &profile_hash, &profile);

	/* Try selected backend first */
	if (meta_load_backend(idx))
	{
		instr_time	compile_start, compile_end;
		int64		compile_ns = 0;
		bool		ok;

		/* Time the compilation */
		if (is_auto && meta_adaptive && adaptive_stats != NULL)
			INSTR_TIME_SET_CURRENT(compile_start);

		ok = backends[idx].cb.compile_expr(state);

		if (is_auto && meta_adaptive && adaptive_stats != NULL)
		{
			INSTR_TIME_SET_CURRENT(compile_end);
			compile_ns = INSTR_TIME_GET_NANOSEC(compile_end) -
						 INSTR_TIME_GET_NANOSEC(compile_start);
		}

		/* Install timing wrapper for adaptive data collection */
		if (ok && is_auto && meta_adaptive && adaptive_stats != NULL)
			meta_install_timing_wrapper(state, idx, profile_hash, compile_ns,
										&profile);

		/* Track which backends were used for deform reset optimization */
		if (ok && state->parent && state->parent->state->es_jit)
		{
			MetaJitterContext *ctx =
				(MetaJitterContext *) state->parent->state->es_jit;
			ctx->backends_used |= (1 << idx);
		}

		return ok;
	}

	/* Fallback: try each in order */
	for (idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		if (meta_load_backend(idx))
		{
			bool ok = backends[idx].cb.compile_expr(state);
			if (ok && state->parent && state->parent->state->es_jit)
			{
				MetaJitterContext *fctx =
					(MetaJitterContext *) state->parent->state->es_jit;
				fctx->backends_used |= (1 << idx);
			}
			return ok;
		}
	}

	/* No backend available — PG interpreter will run */
	return false;
}

static void
meta_release_context(JitContext *context)
{
	MetaJitterContext *ctx = (MetaJitterContext *) context;
	MetaCompiledCode *cc, *next;

	/*
	 * Reset deform dispatch fast-path cache for ALL available backends.
	 *
	 * Each backend .dylib has its own static dispatch_fast[] cache keyed by
	 * TupleDesc pointer.  After context release, TupleDesc pointers may be
	 * reused by palloc for different table layouts, causing stale cache hits
	 * returning deform functions compiled for wrong column types.
	 *
	 * We reset ALL backends rather than just ctx->backends_used because on
	 * Linux (RTLD_GLOBAL), all backends' calls to
	 * pg_jitter_compiled_deform_dispatch() resolve to the first-loaded
	 * backend's copy (typically sljit).  When asmjit or mir is selected as
	 * the expression compiler, their JIT code still populates sljit's
	 * dispatch_fast[] via the PLT, but backends_used only records the
	 * expression-compiler backend.  Resetting only that backend's cache
	 * leaves sljit's dispatch_fast[] stale, causing crashes on TupleDesc
	 * pointer reuse.  Resetting all three is cheap (one integer store each).
	 */
	for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		if (backends[idx].available && backends[idx].deform_reset)
			backends[idx].deform_reset();
	}

	/* Clean up DSM shared code state */
	if (ctx->share_state.initialized && ctx->share_state.dsm_seg)
	{
		/* Clear shmem slot so workers don't find a stale handle */
		if (ctx->share_state.is_leader)
			meta_shmem_clear_dsm_handle();

		/* Just detach — see comment in pg_jitter_cleanup_shared_dsm() */
		dsm_detach(ctx->share_state.dsm_seg);

		memset(&ctx->share_state, 0, sizeof(MetaJitShareState));
	}

	/*
	 * Flush and free any active adaptive timing contexts.
	 * These are expressions that haven't yet reached their sample count.
	 * Flush whatever data we have before freeing.
	 */
	{
		AdaptiveTimingNode *anode, *anext;

		for (anode = ctx->adaptive_timings; anode != NULL; anode = anext)
		{
			anext = anode->next;
			if (anode->tctx)
			{
				if (!anode->tctx->done)
				{
					/* Flush partial stats if we have any calls recorded */
					if (anode->tctx->call_count > 0)
						adaptive_stats_record(anode->tctx->profile_hash,
											  anode->tctx->backend_idx,
											  anode->tctx->compile_ns,
											  anode->tctx->exec_ns,
											  anode->tctx->call_count,
											  NULL);

					/*
					 * Do NOT try to restore evalfunc on anode->state here.
					 * The ExprState lives in per-query memory and may already
					 * be freed when release_context runs via resource owner
					 * cleanup (e.g. error paths).  Dereferencing anode->state
					 * would be a use-after-free.  The ExprState is never
					 * reused after its JIT context is released, so restoring
					 * evalfunc is unnecessary.
					 */
				}

				pfree(anode->tctx);
			}
			pfree(anode);
		}
		ctx->adaptive_timings = NULL;
	}

	/* Free all compiled code — each node carries its own free_fn */
	for (cc = ctx->compiled_list; cc != NULL; cc = next)
	{
		next = cc->next;
		if (cc->free_fn)
			cc->free_fn(cc->data);
		pfree(cc);
	}
	ctx->compiled_list = NULL;

	if (ctx->aux_context)
	{
		MemoryContextDelete(ctx->aux_context);
		ctx->aux_context = NULL;
	}

	/*
	 * On PG17+ we manage our own ResourceOwnerDesc, so we must call
	 * ResourceOwnerForget ourselves.  On PG14-16, PG's jit_release_context()
	 * calls ResourceOwnerForgetJIT() after our callback returns — doing it
	 * here too would be a double-forget and corrupt the resource owner.
	 */
#if PG_VERSION_NUM >= 170000
	if (ctx->resowner)
		MetaForgetContext(ctx->resowner, ctx);
#endif
}

static void
meta_reset_after_error(void)
{
	/* Call reset_after_error on ALL loaded backends */
	for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		if (backends[idx].available)
			backends[idx].cb.reset_after_error();
	}
}

/* ----------------------------------------------------------------
 * SQL function: pg_jitter_backend() returns text
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_jitter_current_backend);

Datum
pg_jitter_current_backend(PG_FUNCTION_ARGS)
{
	int val = pg_jitter_backend;

	for (int i = 0; backend_options[i].name != NULL; i++)
	{
		if (backend_options[i].val == val)
			PG_RETURN_TEXT_P(cstring_to_text(backend_options[i].name));
	}

	PG_RETURN_TEXT_P(cstring_to_text("unknown"));
}

/* ----------------------------------------------------------------
 * SQL function: pg_jitter_adaptive_stats() returns SETOF record
 *
 * Returns the contents of the adaptive stats table
 * with profile details and separate compilation/execution timing.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_jitter_adaptive_stats);

Datum
pg_jitter_adaptive_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc		tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	int				n_entries;

#define ADAPTIVE_STATS_NCOLS 18

	/* Switch to per-query memory context for tuplestore */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tupdesc for our result type */
	tupdesc = CreateTemplateTupleDesc(ADAPTIVE_STATS_NCOLS);
	TupleDescInitEntry(tupdesc, (AttrNumber)  1, "profile_hash",     TEXTOID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  2, "nsteps",           INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  3, "fetchsome_natts",  INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  4, "n_hashdatum",      INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  5, "n_funcexpr",       INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  6, "n_qual",           INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  7, "n_agg",            INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  8, "sljit_calls",      INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber)  9, "sljit_compile_ns", FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "sljit_avg_ns",     FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 11, "asmjit_calls",     INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 12, "asmjit_compile_ns",FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 13, "asmjit_avg_ns",    FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 14, "mir_calls",        INT4OID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 15, "mir_compile_ns",   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 16, "mir_avg_ns",       FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 17, "selected",         TEXTOID,   -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 18, "margin_pct",       FLOAT8OID, -1, 0);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	/* Lazily initialize if needed */
	if (adaptive_stats == NULL && !adaptive_shmem_attempted)
		adaptive_stats_init();

	if (adaptive_stats != NULL)
	{
		n_entries = adaptive_stats->num_entries;

		for (int i = 0; i < n_entries; i++)
		{
			Datum		values[ADAPTIVE_STATS_NCOLS];
			bool		nulls[ADAPTIVE_STATS_NCOLS];
			AdaptiveStatsEntry *e = &adaptive_stats->entries[i];
			uint32		hash_val;
			uint32		sc, ac;
			uint64		s_compile, a_compile, s_exec, a_exec;
			double		savg, aavg;
			char		hashbuf[11];	/* "0x" + 8 hex + NUL */

			memset(nulls, false, sizeof(nulls));

			hash_val = e->profile_hash;
			if (hash_val == 0)
				continue;

			sc = e->call_count[ADAPTIVE_IDX_SLJIT];
			ac = e->call_count[ADAPTIVE_IDX_ASMJIT];
			s_compile = e->compile_ns[ADAPTIVE_IDX_SLJIT];
			a_compile = e->compile_ns[ADAPTIVE_IDX_ASMJIT];
			s_exec = e->exec_ns[ADAPTIVE_IDX_SLJIT];
			a_exec = e->exec_ns[ADAPTIVE_IDX_ASMJIT];

			savg = sc > 0 ? (double) s_exec / sc : 0.0;
			aavg = ac > 0 ? (double) a_exec / ac : 0.0;

			/* MIR stats */
			{
				uint32	mc = e->call_count[ADAPTIVE_IDX_MIR];
				uint64	m_compile = e->compile_ns[ADAPTIVE_IDX_MIR];
				uint64	m_exec = e->exec_ns[ADAPTIVE_IDX_MIR];
				double	mavg = mc > 0 ? (double) m_exec / mc : 0.0;

				snprintf(hashbuf, sizeof(hashbuf), "0x%08x", hash_val);
				values[0]  = CStringGetTextDatum(hashbuf);
				values[1]  = Int32GetDatum((int32) e->nsteps);
				values[2]  = Int32GetDatum((int32) e->fetchsome_natts);
				values[3]  = Int32GetDatum((int32) e->n_hashdatum);
				values[4]  = Int32GetDatum((int32) e->n_funcexpr);
				values[5]  = Int32GetDatum((int32) e->n_qual);
				values[6]  = Int32GetDatum((int32) e->n_agg);
				values[7]  = Int32GetDatum((int32) sc);
				values[8]  = Float8GetDatum((double) s_compile);
				values[9]  = Float8GetDatum(savg);
				values[10] = Int32GetDatum((int32) ac);
				values[11] = Float8GetDatum((double) a_compile);
				values[12] = Float8GetDatum(aavg);
				values[13] = Int32GetDatum((int32) mc);
				values[14] = Float8GetDatum((double) m_compile);
				values[15] = Float8GetDatum(mavg);

				/*
				 * Determine winner using total cost = compile_ns + exec_ns.
				 * Need at least 2 backends with sufficient samples.
				 */
				{
					double	total_costs[ADAPTIVE_NUM_BACKENDS];
					uint32	call_counts[ADAPTIVE_NUM_BACKENDS];
					int		n_measured = 0;
					int		best_b = -1;
					double	best_cost = 0;
					double	second_cost = 0;

					call_counts[ADAPTIVE_IDX_SLJIT] = sc;
					call_counts[ADAPTIVE_IDX_ASMJIT] = ac;
					call_counts[ADAPTIVE_IDX_MIR] = mc;
					total_costs[ADAPTIVE_IDX_SLJIT] = (double) s_compile + (double) s_exec;
					total_costs[ADAPTIVE_IDX_ASMJIT] = (double) a_compile + (double) a_exec;
					total_costs[ADAPTIVE_IDX_MIR] = (double) m_compile + (double) m_exec;

					for (int b = 0; b < ADAPTIVE_NUM_BACKENDS; b++)
					{
						if (call_counts[b] >= (uint32) meta_adaptive_samples)
						{
							if (best_b < 0 || total_costs[b] < best_cost)
							{
								second_cost = best_cost;
								best_cost = total_costs[b];
								best_b = b;
							}
							else if (n_measured == 1 || total_costs[b] < second_cost)
							{
								second_cost = total_costs[b];
							}
							n_measured++;
						}
					}

					if (n_measured >= 2 && best_b >= 0)
					{
						static const char *idx_names[] = {"sljit", "asmjit", "mir"};
						double margin = second_cost > 0
							? ((second_cost - best_cost) / second_cost) * 100.0
							: 0.0;

						values[16] = CStringGetTextDatum(idx_names[best_b]);
						values[17] = Float8GetDatum(margin);
					}
					else
					{
						values[16] = CStringGetTextDatum("pending");
						values[17] = Float8GetDatum(0.0);
					}
				}
			}

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	MemoryContextSwitchTo(oldcontext);
	return (Datum) 0;
}

/* ----------------------------------------------------------------
 * SQL function: pg_jitter_adaptive_stats_reset() returns void
 *
 * Resets all adaptive statistics counters.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_jitter_adaptive_stats_reset);

Datum
pg_jitter_adaptive_stats_reset(PG_FUNCTION_ARGS)
{
	/* Lazily initialize if needed */
	if (adaptive_stats == NULL && !adaptive_shmem_attempted)
		adaptive_stats_init();

	if (adaptive_stats != NULL)
	{
		int n = adaptive_stats->num_entries;

		for (int i = 0; i < n; i++)
		{
			memset(&adaptive_stats->entries[i], 0, sizeof(AdaptiveStatsEntry));
		}
		adaptive_stats->num_entries = 0;
	}

	PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * Provider entry point
 * ---------------------------------------------------------------- */
/*
 * Probe which backend .dylib/.so files are installed and return the
 * highest-priority one as the boot default.  Priority: sljit > asmjit > mir.
 * Does NOT load anything — just checks file existence.
 */
static int
meta_detect_default(void)
{
	char	path[MAXPGPATH];
	int		n_available = 0;
	int		first_available = -1;

	for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		snprintf(path, MAXPGPATH, "%s/%s%s",
				 pkglib_path, backend_libnames[idx], DLSUFFIX);
		if (pg_file_exists(path))
		{
			n_available++;
			if (first_available < 0)
				first_available = idx;
		}
	}

	/* Two or more backends installed — default to auto */
	if (n_available >= 2)
		return PG_JITTER_BACKEND_AUTO;

	/* Exactly one backend — use it directly */
	if (first_available >= 0)
		return first_available;

	/* Nothing installed — keep sljit as nominal default; fallback handles it */
	return PG_JITTER_BACKEND_SLJIT;
}

#if defined(_MSC_VER) && PG_VERSION_NUM < 160000
#pragma comment(linker, "/EXPORT:_PG_jit_provider_init")
#endif
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	int		boot_default;

	cb->reset_after_error = meta_reset_after_error;
	cb->release_context = meta_release_context;
	cb->compile_expr = meta_compile_expr;

#if PG_VERSION_NUM < 150000 && defined(_WIN32)
	get_pkglib_path(my_exec_path, meta_pkglib_path);
#endif

	boot_default = meta_detect_default();
	pg_jitter_backend = boot_default;

	/*
	 * Define parallel_mode and shared_code_max BEFORE pg_jitter.backend,
	 * because the backend assign hook (meta_backend_assign) eagerly loads
	 * the backend .dylib.  That backend's _PG_jit_provider_init will try
	 * to define these same GUCs — the GetConfigOption guard skips the
	 * define only if the GUC already exists.  So we must define them first.
	 */
	{
		static const struct config_enum_entry parallel_jit_options[] = {
			{"off", PARALLEL_JIT_OFF, false},
			{"per_worker", PARALLEL_JIT_PER_WORKER, false},
			{"shared", PARALLEL_JIT_SHARED, false},
			{NULL, 0, false}
		};

		DefineCustomEnumVariable(
			"pg_jitter.parallel_mode",
			"Controls JIT behavior in parallel workers: "
			"off (workers use interpreter), "
			"per_worker (each worker compiles independently), "
			"shared (leader shares compiled code via DSM)",
			NULL,
			&meta_parallel_mode,
			PARALLEL_JIT_PER_WORKER,
			parallel_jit_options,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	DefineCustomIntVariable(
		"pg_jitter.shared_code_max",
		"Maximum shared JIT code DSM size in KB.",
		NULL,
		&meta_shared_code_max_kb,
		4096,		/* 4 MB default */
		64,			/* 64 KB minimum */
		1048576,	/* 1 GB maximum */
		PGC_USERSET,
		GUC_UNIT_KB | GUC_ALLOW_IN_PARALLEL,
		NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_jitter.deform_cache",
							 "Cache compiled deform functions across queries. "
							 "When off, deform is recompiled each query.",
							 NULL,
							 &meta_deform_cache,
							 true, /* on by default */
							 PGC_USERSET,
							 GUC_ALLOW_IN_PARALLEL,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_jitter.deform_avx512",
							 "Use AVX-512F for supported tuple deform batches "
							 "when the CPU and OS allow executing AVX-512 "
							 "instructions.",
							 NULL,
							 &meta_deform_avx512,
							 true,
							 PGC_USERSET,
							 GUC_ALLOW_IN_PARALLEL,
							 NULL, NULL, NULL);

	DefineCustomIntVariable(
		"pg_jitter.deform_avx512_min_cols",
		"Minimum uniform int4 deform batch width required before using "
		"AVX-512. 0 allows AVX-512 for every eligible batch.",
		NULL,
		&meta_deform_avx512_min_cols,
		META_DEFORM_AVX512_MIN_COLS_DEFAULT,
		0,
		MaxTupleAttributeNumber,
		PGC_USERSET,
		GUC_ALLOW_IN_PARALLEL,
		NULL, NULL, NULL);

	DefineCustomIntVariable(
		"pg_jitter.min_expr_steps",
		"Minimum expression step count for JIT compilation. "
		"Expressions with fewer steps use the interpreter.",
		NULL,
		&meta_min_expr_steps,
		4,			/* skip JIT for expressions with fewer than 4 steps */
		0,			/* minimum */
		1000,		/* maximum */
		PGC_USERSET,
		GUC_ALLOW_IN_PARALLEL,
		NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_jitter.adaptive",
							 "Enable adaptive backend selection based on "
							 "runtime performance statistics. "
							 "Only effective when pg_jitter.backend = 'auto'.",
							 NULL,
							 &meta_adaptive,
							 true,
							 PGC_USERSET,
							 GUC_ALLOW_IN_PARALLEL,
							 NULL, NULL, NULL);

	DefineCustomIntVariable(
		"pg_jitter.adaptive_samples",
		"Number of expression evaluations to time before considering "
		"a backend profiled for adaptive selection.",
		NULL,
		&meta_adaptive_samples,
		64,			/* default: 64 calls */
		4,			/* minimum: need at least a few samples */
		10000,		/* maximum */
		PGC_USERSET,
		GUC_ALLOW_IN_PARALLEL,
		NULL, NULL, NULL);

	DefineCustomRealVariable(
		"pg_jitter.adaptive_epsilon",
		"Exploration probability for adaptive backend selection. "
		"0.0 = always pick the best measured backend, "
		"1.0 = always pick randomly.",
		NULL,
		&meta_adaptive_epsilon,
		0.05,		/* 5% exploration by default */
		0.0,
		1.0,
		PGC_USERSET,
		GUC_ALLOW_IN_PARALLEL,
		NULL, NULL, NULL);

	DefineCustomEnumVariable("pg_jitter.backend",
							 "Selects the active pg_jitter JIT backend.",
							 NULL,
							 &pg_jitter_backend,
							 boot_default,
							 backend_options,
							 PGC_USERSET,
							 GUC_ALLOW_IN_PARALLEL,
							 NULL,
							 meta_backend_assign,
							 NULL);
}
