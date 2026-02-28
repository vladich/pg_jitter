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
#include "nodes/execnodes.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "storage/dsm.h"

PG_MODULE_MAGIC_EXT(.name = "pg_jitter");

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
	PG_JITTER_NUM_BACKENDS
};

static const struct config_enum_entry backend_options[] = {
	{"sljit", PG_JITTER_BACKEND_SLJIT, false},
	{"asmjit", PG_JITTER_BACKEND_ASMJIT, false},
	{"mir", PG_JITTER_BACKEND_MIR, false},
	{NULL, 0, false}
};

static const char *backend_libnames[] = {
	"pg_jitter_sljit",
	"pg_jitter_asmjit",
	"pg_jitter_mir",
};

static int pg_jitter_backend = PG_JITTER_BACKEND_SLJIT;

/* GUC: pg_jitter.parallel_mode — defined here so it's available before backends load */
static int meta_parallel_mode = 2;		/* PARALLEL_JIT_SHARED */
static int meta_shared_code_max_kb = 4096;	/* 4 MB */
static char *meta_shared_dsm_guc = NULL;	/* DSM handle for parallel JIT sharing */

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
} BackendEntry;

static BackendEntry backends[PG_JITTER_NUM_BACKENDS];

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

	/* Load and initialize */
	init_fn = (JitProviderInit)
		load_external_function(path, "_PG_jit_provider_init", true, NULL);

	init_fn(&backends[idx].cb);
	backends[idx].available = true;

	elog(DEBUG1, "pg_jitter: loaded backend %s", backend_libnames[idx]);
	return true;
}

/* ----------------------------------------------------------------
 * GUC assign hook — eagerly attempt load on SET
 * ---------------------------------------------------------------- */
static void
meta_backend_assign(int newval, void *extra)
{
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

	MetaRememberContext(CurrentResourceOwner, ctx);

	parent->state->es_jit = &ctx->base;
}

/* ----------------------------------------------------------------
 * Provider callbacks — dispatch to loaded backend
 * ---------------------------------------------------------------- */

static bool
meta_compile_expr(ExprState *state)
{
	int		idx = pg_jitter_backend;

	/*
	 * Pre-create the context with our resowner desc before the backend
	 * gets a chance to create it with its own desc.
	 */
	if (state->parent)
		meta_ensure_context(state);

	/* Try selected backend first */
	if (meta_load_backend(idx))
		return backends[idx].cb.compile_expr(state);

	/* Fallback: try each in order */
	for (idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		if (meta_load_backend(idx))
			return backends[idx].cb.compile_expr(state);
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
	 * Clean up DSM shared code state.
	 *
	 * Note: we do NOT call SetConfigOption here to reset the _shared_dsm GUC.
	 * This function may be called from a ResourceOwner release callback during
	 * subtransaction abort, and SetConfigOption allocates memory — which
	 * triggers "ResourceOwnerEnlarge called after release started".  The GUC
	 * is session-scoped and harmless if stale; each new parallel query creates
	 * a fresh DSM handle.
	 */
	if (ctx->share_state.initialized && ctx->share_state.dsm_seg)
	{
		/* Just detach — see comment in pg_jitter_cleanup_shared_dsm() */
		dsm_detach(ctx->share_state.dsm_seg);

		memset(&ctx->share_state, 0, sizeof(MetaJitShareState));
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
	const char *name = backend_options[pg_jitter_backend].name;

	PG_RETURN_TEXT_P(cstring_to_text(name));
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

	for (int idx = 0; idx < PG_JITTER_NUM_BACKENDS; idx++)
	{
		snprintf(path, MAXPGPATH, "%s/%s%s",
				 pkglib_path, backend_libnames[idx], DLSUFFIX);
		if (pg_file_exists(path))
			return idx;
	}

	/* Nothing installed — keep sljit as nominal default; fallback handles it */
	return PG_JITTER_BACKEND_SLJIT;
}

void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	int		boot_default;

	cb->reset_after_error = meta_reset_after_error;
	cb->release_context = meta_release_context;
	cb->compile_expr = meta_compile_expr;

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
			PARALLEL_JIT_SHARED,
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

	/*
	 * Internal GUC for passing DSM handle from leader to workers.
	 * Serialized via SerializeGUCState automatically.
	 * Hidden from pg_settings and config files.
	 *
	 * Must be defined BEFORE pg_jitter.backend, because the backend assign
	 * hook eagerly loads the backend .dylib, which also tries to define this
	 * GUC.  If we define it afterwards, the backend defines it first, and
	 * then our define hits "attempt to redefine parameter".
	 */
	DefineCustomStringVariable(
		"pg_jitter._shared_dsm",
		"Internal: DSM handle for parallel JIT code sharing.",
		NULL,
		&meta_shared_dsm_guc,
		"",
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
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
