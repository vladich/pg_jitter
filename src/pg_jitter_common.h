/*
 * pg_jitter_common.h — Shared types and declarations for pg_jitter providers
 */
#ifndef PG_JITTER_COMMON_H
#define PG_JITTER_COMMON_H

#include "postgres.h"
#include "pg_jitter_compat.h"
#include "jit/jit.h"
#include "executor/execExpr.h"
#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "executor/tuptable.h"
#include "access/tupdesc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "access/parallel.h"
#include "storage/dsm.h"
#include "port/atomics.h"
/*
 * pg_jitter_collation_is_c — check if a collation OID resolves to C/POSIX.
 * Used at JIT compile time to decide whether SIMD text ops are safe.
 * Handles DEFAULT_COLLATION_OID (100) in addition to C_COLLATION_OID (950).
 */
extern bool pg_jitter_collation_is_c(Oid collid);

/*
 * pg_jitter_collation_is_deterministic — check if a collation is deterministic.
 * Used to gate bytewise StringZilla text/LIKE fast paths.  PCRE2 has an
 * additional encoding/collation eligibility gate in pg_jitter_pcre2.c.
 */
extern bool pg_jitter_collation_is_deterministic(Oid collid);

/*
 * SharedJitCompiledCode / SharedJitCodeEntry — structures for sharing
 * compiled JIT code between leader and parallel workers via DSM.
 */
typedef struct SharedJitCompiledCode
{
	pg_atomic_uint32 lock;			/* simple spinlock for concurrent writes */
	int			num_entries;
	Size		capacity;			/* total allocated size */
	Size		used;				/* bytes used so far (header + entries) */

	/* Shared deform for parallel I-cache sharing */
	uint64		deform_addr;		/* leader's mmap VA (0 = not set) */
	Size		deform_page_size;	/* page-aligned allocation size */
	Size		deform_used_size;	/* exact DSM bytes: code bytes + descs */
	Size		deform_code_size;	/* code bytes only */
	Size		deform_desc_offset;	/* descriptor offset within page */
	int			deform_natts;		/* columns */
	uint32		deform_attrs_hash;	/* descriptor shape hash */
	uint32		deform_ops_kind;		/* stable TupleTableSlotOps kind */
	Oid			deform_tdtypeid;	/* descriptor type OID */
	int32		deform_tdtypmod;	/* descriptor typmod */
	/* compact [code][descs] bytes stored at capacity - deform_used_size */

	/* SharedJitCodeEntry entries follow */
} SharedJitCompiledCode;

typedef struct SharedJitCodeEntry
{
	int			plan_node_id;
	int			expr_index;			/* ordinal within plan node */
	uint64		expr_fingerprint;	/* guards against ordinal mismatches */
	Size		code_size;
	uint64		dylib_ref_addr;		/* leader's pg_jitter_fallback_step address */
	char		code_bytes[FLEXIBLE_ARRAY_MEMBER];
} SharedJitCodeEntry;

/* Linked-list node tracking one compiled function's native code */
typedef struct CompiledCode
{
	struct CompiledCode *next;
	void	(*free_fn)(void *data);		/* backend-specific free function */
	void   *data;						/* backend-specific compiled code handle */
} CompiledCode;

/*
 * Shared memory slot table for passing DSM handles between leader and workers.
 * Lazily allocated via ShmemInitStruct from PG's spare shmem pool (~100KB).
 * Each slot holds one pg_atomic_uint32 DSM handle, indexed by proc index.
 */
typedef struct JitDsmSlotTable
{
	int					num_slots;
	pg_atomic_uint32	handles[FLEXIBLE_ARRAY_MEMBER];
} JitDsmSlotTable;

extern dsm_handle pg_jitter_shmem_get_dsm_handle(int proc_index);
extern void pg_jitter_shmem_set_dsm_handle(int proc_index, dsm_handle handle);
extern void pg_jitter_shmem_clear_dsm_handle(int proc_index);
extern bool pg_jitter_shmem_available(void);

/*
 * Process-local state for DSM-based JIT code sharing.
 *
 * Leader creates DSM via dsm_create(), stores handle in shmem slot table.
 * Workers read the leader's slot, attach via dsm_attach().
 * Both pin their mapping and explicitly unpin+detach in release_context.
 */
typedef struct JitShareState
{
	dsm_segment *dsm_seg;
	SharedJitCompiledCode *sjc;		/* mapped DSM address */
	bool		initialized;
	bool		is_leader;			/* true if we created the DSM */
} JitShareState;

/* Our JIT context — extends JitContext by embedding it as the first member */
typedef struct PgJitterContext
{
	JitContext		base;				/* must be first */
	CompiledCode   *compiled_list;		/* linked list of compiled code */
	ResourceOwner	resowner;

	/* Expression identity tracking for shared code in parallel queries */
	int			last_plan_node_id;	/* reset expr_ordinal when this changes */
	int			expr_ordinal;		/* incremented per compile_expr call */

	/* DSM-based shared code state for parallel queries */
	JitShareState	share_state;

	/* Query-lifetime helper allocations embedded in generated code */
	MemoryContext	aux_context;
} PgJitterContext;

/* Get-or-create a PgJitterContext from the EState */
extern PgJitterContext *pg_jitter_get_context(ExprState *state);

/* Register compiled code in context for cleanup */
extern void pg_jitter_register_compiled(PgJitterContext *ctx,
										void (*free_fn)(void *),
										void *data);
extern void pg_jitter_register_compiled_or_free(PgJitterContext *ctx,
												void (*free_fn)(void *),
												void *data);
extern void *pg_jitter_context_alloc(PgJitterContext *ctx, Size size);
extern void *pg_jitter_context_alloc_zero(PgJitterContext *ctx, Size size);

/*
 * Windows x64 unwind table registration for JIT code.
 *
 * On Windows, longjmp() uses SEH-based unwinding and requires every stack
 * frame to have registered UNWIND_INFO.  JIT-generated code has no such
 * metadata, so longjmp() from ereport(ERROR) crashes (STATUS_STACK_BUFFER_OVERRUN)
 * when it tries to unwind through a JIT frame.
 *
 * pg_jitter_win64_register_unwind() registers a minimal RUNTIME_FUNCTION entry
 * covering the JIT code region so the SEH unwinder can traverse it.
 * pg_jitter_win64_deregister_unwind() removes it before freeing the code.
 */
#ifdef _WIN64
extern void pg_jitter_win64_register_unwind(void *code, size_t code_size);
extern void pg_jitter_win64_deregister_unwind(void *code);
#else
#define pg_jitter_win64_register_unwind(code, size) ((void)0)
#define pg_jitter_win64_deregister_unwind(code) ((void)0)
#endif

/*
 * Fallback dispatch: calls the appropriate ExecEval* C function.
 * Returns -1 to continue to next step, or >= 0 for jump target step index.
 * Uses int64 to match machine word size for sljit/MIR calling convention.
 */
extern int64 pg_jitter_fallback_step(ExprState *state,
									 ExprEvalStep *op,
									 ExprContext *econtext);

/* Provider callbacks (shared across backends) */
extern void pg_jitter_reset_after_error(void);
extern void pg_jitter_release_context(JitContext *context);

/*
 * Direct aggregate transition helpers — called by JIT code without
 * going through the fallback_step switch dispatch.
 */
extern void pg_jitter_agg_trans_init_strict_byval(ExprState *state,
												  ExprEvalStep *op);
extern void pg_jitter_agg_trans_strict_byval(ExprState *state,
											 ExprEvalStep *op);
extern void pg_jitter_agg_trans_byval(ExprState *state,
									  ExprEvalStep *op);
extern void pg_jitter_agg_trans_init_strict_byref(ExprState *state,
												  ExprEvalStep *op);
extern void pg_jitter_agg_trans_strict_byref(ExprState *state,
											 ExprEvalStep *op);
extern void pg_jitter_agg_trans_byref(ExprState *state,
									  ExprEvalStep *op);

/*
 * BYREF aggregate finish helper for inline JIT code.
 * Handles the pointer comparison + ExecAggCopyTransValue call
 * (which needs 6 args, exceeding sljit's 4-arg icall limit).
 */
extern void pg_jitter_agg_byref_finish(AggState *aggstate,
										AggStatePerTrans pertrans,
										Datum newVal,
										AggStatePerGroup pergroup);

/*
 * Install a compiled expression with a validation wrapper.
 * On first call, CheckExprStillValid() verifies that slot types still
 * match the compiled code's assumptions (catches ALTER COLUMN TYPE etc.).
 * After validation, the wrapper replaces itself with the actual JIT code.
 */
extern void pg_jitter_install_expr(ExprState *state,
								   ExprStateEvalFunc compiled_func);

/*
 * Shared JIT code helpers for parallel query.
 * Leader stores compiled code bytes in DSM; workers copy them to local
 * executable memory instead of recompiling.
 */

/* Store compiled code in DSM (leader only). Returns true on success. */
extern bool pg_jitter_store_shared_code(void *shared, const void *code,
										Size code_size, int node_id,
										int expr_idx,
										uint64 expr_fingerprint,
										uint64 dylib_ref_addr);

/* Find compiled code in DSM (worker). Returns true if found. */
extern bool pg_jitter_find_shared_code(void *shared, int node_id,
									   int expr_idx,
									   uint64 expr_fingerprint,
									   const void **code_bytes,
									   Size *code_size,
									   uint64 *dylib_ref_addr);

/* Copy code bytes to local executable memory. */
extern void *pg_jitter_copy_to_executable(const void *code_bytes,
										  Size code_size);

/* Free executable memory allocated by pg_jitter_copy_to_executable. */
extern void pg_jitter_exec_free(void *ptr);

/* Register copied executable memory, freeing it if registration throws. */
extern void pg_jitter_register_exec_handle(PgJitterContext *ctx, void *handle);

/* Get the executable code pointer from a handle returned by pg_jitter_copy_to_executable. */
extern void *pg_jitter_exec_code_ptr(void *handle);

/*
 * Relocate dylib function addresses in copied executable code.
 *
 * When JIT code is shared between leader and worker processes, any absolute
 * addresses pointing into pg_jitter_sljit.dylib are WRONG in the worker
 * because the dylib loads at a different ASLR base address.
 *
 * This function scans absolute call targets in the code, identifies exact
 * known pg_jitter helper addresses from the leader process, and patches them
 * to the worker's addresses using the delta between leader_ref_addr and
 * worker_ref_addr (both pg_jitter_fallback_step addresses).
 *
 * Must be called AFTER pg_jitter_copy_to_executable and BEFORE the code runs.
 * The handle must point to a writable+executable MAP_JIT region.
 */
extern int pg_jitter_relocate_dylib_addrs(void *handle, Size code_size,
										   uint64 leader_ref_addr,
										   uint64 worker_ref_addr);

/* Get expression identity (plan_node_id, expr_ordinal) for the current expr. */
extern void pg_jitter_get_expr_identity(PgJitterContext *ctx,
										ExprState *state,
										int *node_id, int *expr_idx);
extern uint64 pg_jitter_expr_fingerprint(ExprState *state);

/*
 * DSM-based shared code management for parallel queries.
 *
 * Leader calls pg_jitter_init_shared_dsm() during first compile_expr to
 * create the DSM and store its handle in the shmem slot table.
 * Workers call pg_jitter_attach_shared_dsm() to read the leader's slot.
 * Both call pg_jitter_cleanup_shared_dsm() during release_context.
 */
extern void pg_jitter_init_shared_dsm(PgJitterContext *ctx);
extern void pg_jitter_attach_shared_dsm(PgJitterContext *ctx);
extern void pg_jitter_cleanup_shared_dsm(PgJitterContext *ctx);

/* Parallel JIT mode enum — same values in all backends */
#define PARALLEL_JIT_OFF        0
#define PARALLEL_JIT_PER_WORKER 1
#define PARALLEL_JIT_SHARED     2

/* IN-list strategy for HASHED_SCALARARRAYOP */
#define IN_HASH_PG       0  /* PG's built-in Jenkins hash probe */
#define IN_HASH_CRC32    1  /* CRC32C open-addressing */

/*
 * Default threshold: ≤ this → inline bsearch tree, > this → CRC32 hash.
 *
 * The hashed scalar-array SIMD linear scan is opt-in only.  PostgreSQL starts
 * using HASHED_SCALARARRAYOP for lists large enough that a linear scan loses on
 * common miss-heavy workloads.
 */
#define IN_BSEARCH_MAX_DEFAULT 4096
#define IN_SIMD_MAX_DEFAULT 0
#define DEFORM_AVX512_MIN_COLS_DEFAULT 1200

/* GUC variables shared across backends (defined in pg_jitter_common.c) */
extern int pg_jitter_parallel_mode;
extern int pg_jitter_shared_code_max_kb;
extern bool pg_jitter_deform_cache;
extern bool pg_jitter_deform_avx512;
extern int pg_jitter_deform_avx512_min_cols;
extern int pg_jitter_min_expr_steps;
extern int pg_jitter_in_hash_strategy;
extern int pg_jitter_in_bsearch_max;
extern int pg_jitter_in_simd_max;

/*
 * x86 CPU feature checks with OS XSAVE/XCR0 validation.
 *
 * These helpers intentionally report whether the current process may execute
 * the instruction class, not just whether the physical CPU advertises it.
 */
extern bool pg_jitter_cpu_has_sse42(void);
extern bool pg_jitter_cpu_has_avx2(void);
extern bool pg_jitter_cpu_has_avx512f(void);
extern bool pg_jitter_cpu_has_avx512bw(void);
extern bool pg_jitter_cpu_has_avx512vl(void);

/*
 * Read the current parallel mode from the GUC system.
 *
 * When a backend is loaded via the meta module, the GUC variable pointer
 * belongs to whichever module defined the GUC first (the meta or the first
 * backend loaded). This function reads the actual GUC value from PG's GUC
 * system, which is always up-to-date regardless of which variable pointer
 * was registered.
 */
extern int pg_jitter_get_parallel_mode(void);
extern int pg_jitter_get_min_expr_steps(void);
extern bool pg_jitter_get_deform_avx512(void);
extern int pg_jitter_get_deform_avx512_min_cols(void);
extern bool pg_jitter_in_raw_datum_bsearch_safe(PGFunction fn);
extern bool pg_jitter_in_int32_hash_safe(PGFunction fn);
extern bool pg_jitter_text_hash_saop_eligible(ExprEvalStep *op,
											  FunctionCallInfo fcinfo);

/*
 * Loop-based deform for wide tables.
 *
 * Unrolled deform emits ~140 bytes per column. For wide tables (>100 cols),
 * this exceeds L1I cache and makes JIT slower than the interpreter.
 * pg_jitter_deform_threshold() returns the column count above which we
 * switch from unrolled to a compact loop-based deform.
 *
 * Threshold = L1I_cache_size / DEFORM_BYTES_PER_COL / 2
 */
#define DEFORM_BYTES_PER_COL  140

/*
 * Safety cap on JIT deform column count.  Returns MaxTupleAttributeNumber,
 * PostgreSQL's tuple descriptor limit.
 */
extern int pg_jitter_wide_deform_limit(void);

/* Per-column descriptor for loop-based deform */
typedef struct DeformColDesc {
	int16	attlen;		/* >0 fixed, -1 varlena, -2 cstring */
	int8	attalign;	/* alignment in bytes (1, 2, 4, 8) */
	int8	attbyval;	/* pass-by-value? */
	int8	attnotnull;	/* guaranteed not null? */
	int8	pad[3];		/* pad to 8 bytes */
} DeformColDesc;

extern int pg_jitter_deform_threshold(void);

/*
 * sljit-based deform compilation (shared across all backends via
 * pg_jitter_deform_jit.c).  Produces standalone void fn(TupleTableSlot *)
 * functions.  The dispatch function caches compiled deform functions and
 * selects loop vs unrolled based on pg_jitter_deform_threshold().
 */
extern void *pg_jitter_compile_deform(TupleDesc desc,
									  const TupleTableSlotOps *ops,
									  int natts);
struct sljit_generate_code_buffer;		/* forward declaration */
extern void *pg_jitter_compile_deform_loop(TupleDesc desc,
										   const TupleTableSlotOps *ops,
										   int natts,
										   DeformColDesc *target_descs,
										   Size *code_size_out,
										   Size *code_buffer_size_out,
										   struct sljit_generate_code_buffer *gen_buf);
extern void pg_jitter_compiled_deform_dispatch(TupleTableSlot *slot,
											   int natts);
extern void pg_jitter_deform_dispatch_reset_fastpath(void);

/*
 * Shared deform for parallel I-cache sharing.
 * Leader compiles once and places code+descriptor at a fixed virtual address.
 * Workers mmap at the same VA via MAP_FIXED_NOREPLACE (Linux) to share L1I.
 */
extern bool pg_jitter_compile_shared_deform(SharedJitCompiledCode *sjc,
											TupleDesc desc,
											const TupleTableSlotOps *ops,
											int natts);
extern bool pg_jitter_attach_shared_deform(SharedJitCompiledCode *sjc);
extern void pg_jitter_reset_shared_deform(void);

/*
 * CASE expression binary search optimization.
 *
 * Detects monotonic CASE patterns at JIT compile time and replaces
 * linear O(N) branch scanning with O(log N) binary search via helper
 * functions.  Supports both searched CASE (< / <= / > / >=) and
 * simple CASE (=) patterns with all ordered types (int2/int4/int8,
 * float4/float8, date, timestamp, oid, bool, and byref types like
 * text, numeric, uuid, interval, bpchar, bytea, jsonb, network).
 *
 * For shared mode compatibility, array pointers are stored in step
 * data fields of skipped steps.  Each process (leader + workers) runs
 * detection independently and allocates its own arrays.  JIT code
 * loads array pointers from step data at runtime.
 *
 * Step fields used for storing runtime pointers:
 *   steps[start_opno + 1].d.constval.value  →  thresholds array pointer
 *   steps[start_opno + 4].d.constval.value  →  results array pointer
 */

/* Minimum branches to trigger binary search (below this, linear is faster) */
#define CASE_BSEARCH_MIN_BRANCHES  4

/*
 * CaseBSearchType — discriminator for binary search helper selection.
 * Determines threshold storage format, comparison semantics, and
 * which helper function to call at runtime.
 */
typedef enum CaseBSearchType {
	CASE_BS_I32,       /* int4, date — signed int32 */
	CASE_BS_I64,       /* int8, timestamp/tz, bool — signed int64 / Datum */
	CASE_BS_U32,       /* oid — unsigned uint32 */
	CASE_BS_I16,       /* int2 — cast to int16 for comparison */
	CASE_BS_F4,        /* float4 — NaN-aware float comparison */
	CASE_BS_F8,        /* float8 — NaN-aware float comparison */
	CASE_BS_GENERIC,   /* byref types: call PG cmp_fn via V1 */
} CaseBSearchType;

/* Map bs_type to JIT calling convention var_type.
 * Returns 0 (JIT_TYPE_32) for 32-bit types, 1 (JIT_TYPE_64) for 64-bit. */
static inline int8 bs_type_to_var_type(CaseBSearchType t) {
	switch (t) {
		case CASE_BS_I32: case CASE_BS_U32: case CASE_BS_I16:
			return 0; /* JIT_TYPE_32 */
		default:
			return 1; /* JIT_TYPE_64 */
	}
}

typedef struct CaseBSearchInfo {
	int         start_opno;     /* first step of the pattern (VAR/CASE_TESTVAL) */
	int         end_opno;       /* step AFTER last JUMP (= first step of ELSE) */
	int         else_end_opno;  /* step AFTER ELSE const (= CASE end) */
	int         num_branches;
	bool        is_equality;    /* Pattern B (=) vs Pattern A (</<=/>/>=) */
	bool        is_less;        /* true for </<=, false for >/>=  */
	bool        is_inclusive;   /* true for <=/>= */
	bool        is_range;       /* true for range CASE (WHEN x >= lo AND x < hi) */
	int         range_stride;   /* steps per branch (9 for range, 5 for single) */
	PGFunction  cmp_fn;         /* comparison function address */
	CaseBSearchType bs_type;    /* type discriminator for helper selection */
	/* Arrays allocated in CurrentMemoryContext, length = num_branches */
	Datum      *thresholds;     /* sorted thresholds/values */
	Datum      *results;        /* corresponding THEN results */
	Datum       default_result; /* ELSE result */
	bool        default_is_null;
	/* For the comparison variable */
	int         var_opno;       /* step index of representative VAR step */
	int8        var_type;       /* JIT_TYPE_32 or JIT_TYPE_64 (derived from bs_type) */
} CaseBSearchInfo;

/* Detect CASE patterns suitable for binary search optimization.
 * Returns number of detected patterns.  Caller provides pre-allocated array. */
extern int pg_jitter_detect_case_bsearch(ExprState *state,
										 ExprEvalStep *steps, int steps_len,
										 CaseBSearchInfo *out, int max_patterns);

/* Set up binary search arrays for a detected pattern and store pointers
 * in step data fields for runtime access by JIT code. Called by both
 * leader (during compilation) and workers (after attaching shared code). */
extern void pg_jitter_setup_case_bsearch_arrays(ExprState *state,
												ExprEvalStep *steps,
												int steps_len);

/*
 * Compact descriptor for binary search at runtime.
 * Allocated once per detected pattern, stored in step data for JIT access.
 * Contains all data needed by the binary search helpers.
 */
typedef struct CaseBSearchDesc {
	int         n;              /* number of branches */
	uint8       bs_type;        /* CaseBSearchType discriminator */
	PGFunction  cmp_fn;         /* for CASE_BS_GENERIC: PG comparison function (the operator) */
	PGFunction  cmp_order_fn;   /* for CASE_BS_GENERIC eq: _lt function for binary search order */
	Oid         cmp_collation;  /* for CASE_BS_GENERIC: collation OID */
	Datum       default_val;    /* ELSE result */
	Datum       range_min;      /* for range CASE: lower bound of first range */
	bool        has_range_min;  /* true if range_min should be checked */
	/* thresholds: int32[n] or int64[n] or Datum[n] immediately after this header */
	/* results:    Datum[n] after thresholds */
} CaseBSearchDesc;

/* Binary search helpers called from JIT code.
 * Take (val, desc_ptr) — desc contains thresholds, results, n, default.
 * Returns the matching result Datum. */

/* signed int32 (int4, date) */
extern Datum pg_jitter_case_bsearch_lt_i32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_i32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_i32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_i32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_i32(int32 val, const CaseBSearchDesc *desc);

/* signed int64 (int8, timestamp/tz, bool) */
extern Datum pg_jitter_case_bsearch_lt_i64(int64 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_i64(int64 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_i64(int64 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_i64(int64 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_i64(int64 val, const CaseBSearchDesc *desc);

/* unsigned int32 (oid) */
extern Datum pg_jitter_case_bsearch_lt_u32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_u32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_u32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_u32(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_u32(int32 val, const CaseBSearchDesc *desc);

/* int16 (int2) — val passed as int32, cast to int16 for comparison */
extern Datum pg_jitter_case_bsearch_lt_i16(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_i16(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_i16(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_i16(int32 val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_i16(int32 val, const CaseBSearchDesc *desc);

/* float4 — NaN-aware, val passed as Datum */
extern Datum pg_jitter_case_bsearch_lt_f4(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_f4(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_f4(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_f4(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_f4(Datum val, const CaseBSearchDesc *desc);

/* float8 — NaN-aware, val passed as Datum */
extern Datum pg_jitter_case_bsearch_lt_f8(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_f8(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_f8(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_f8(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_f8(Datum val, const CaseBSearchDesc *desc);

/* generic (byref types) — calls PG cmp_fn via V1 */
extern Datum pg_jitter_case_bsearch_lt_generic(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_le_generic(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_gt_generic(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_ge_generic(Datum val, const CaseBSearchDesc *desc);
extern Datum pg_jitter_case_bsearch_eq_generic(Datum val, const CaseBSearchDesc *desc);

/* Returns the appropriate helper function for the given pattern. */
extern void *pg_jitter_select_bsearch_helper(const CaseBSearchInfo *cbi);

/*
 * Shared-mode setup for IN-list hash tables.
 *
 * Called by both leader (during compilation) and workers (after DSM
 * attachment). Scans expression steps for HASHED_SCALARARRAYOP with
 * text arrays and builds process-local TextHashTable, storing the
 * pointer in fcinfo->args[1].value.
 */
extern void pg_jitter_setup_shared_in_hash(ExprState *state,
                                            ExprEvalStep *steps,
                                            int steps_len);

#endif /* PG_JITTER_COMMON_H */
