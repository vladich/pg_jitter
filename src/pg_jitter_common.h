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
	/* SharedJitCodeEntry entries follow */
} SharedJitCompiledCode;

typedef struct SharedJitCodeEntry
{
	int			plan_node_id;
	int			expr_index;			/* ordinal within plan node */
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
 * Process-local state for DSM-based JIT code sharing.
 *
 * Leader creates DSM via dsm_create(), stores handle in a GUC.
 * Workers read the GUC, attach via dsm_attach().
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
} PgJitterContext;

/* Get-or-create a PgJitterContext from the EState */
extern PgJitterContext *pg_jitter_get_context(ExprState *state);

/* Register compiled code in context for cleanup */
extern void pg_jitter_register_compiled(PgJitterContext *ctx,
										void (*free_fn)(void *),
										void *data);

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
										int expr_idx, uint64 dylib_ref_addr);

/* Find compiled code in DSM (worker). Returns true if found. */
extern bool pg_jitter_find_shared_code(void *shared, int node_id,
									   int expr_idx,
									   const void **code_bytes,
									   Size *code_size,
									   uint64 *dylib_ref_addr);

/* Copy code bytes to local executable memory. */
extern void *pg_jitter_copy_to_executable(const void *code_bytes,
										  Size code_size);

/* Free executable memory allocated by pg_jitter_copy_to_executable. */
extern void pg_jitter_exec_free(void *ptr);

/* Get the executable code pointer from a handle returned by pg_jitter_copy_to_executable. */
extern void *pg_jitter_exec_code_ptr(void *handle);

/*
 * Relocate dylib function addresses in copied executable code.
 *
 * When JIT code is shared between leader and worker processes, any absolute
 * addresses pointing into pg_jitter_sljit.dylib are WRONG in the worker
 * because the dylib loads at a different ASLR base address.
 *
 * This function scans ARM64 MOVZ+3xMOVK sequences in the code, identifies
 * addresses in the leader's dylib range, and patches them to the worker's
 * addresses using the delta between leader_ref_addr and worker_ref_addr
 * (both pg_jitter_fallback_step addresses).
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

/*
 * DSM-based shared code management for parallel queries.
 *
 * Leader calls pg_jitter_init_shared_dsm() during first compile_expr to
 * create the DSM and store its handle in a GUC.
 * Workers call pg_jitter_attach_shared_dsm() to read the GUC and attach.
 * Both call pg_jitter_cleanup_shared_dsm() during release_context.
 */
extern void pg_jitter_init_shared_dsm(PgJitterContext *ctx);
extern void pg_jitter_attach_shared_dsm(PgJitterContext *ctx);
extern void pg_jitter_cleanup_shared_dsm(PgJitterContext *ctx);

/* Parallel JIT mode enum — same values in all backends */
#define PARALLEL_JIT_OFF        0
#define PARALLEL_JIT_PER_WORKER 1
#define PARALLEL_JIT_SHARED     2

/* GUC variables shared across backends (defined in pg_jitter_common.c) */
extern int pg_jitter_parallel_mode;
extern int pg_jitter_shared_code_max_kb;

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

#endif /* PG_JITTER_COMMON_H */
