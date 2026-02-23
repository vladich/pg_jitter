/*
 * pg_jitter_common.h — Shared types and declarations for pg_jitter providers
 */
#ifndef PG_JITTER_COMMON_H
#define PG_JITTER_COMMON_H

#include "postgres.h"
#include "jit/jit.h"
#include "executor/execExpr.h"
#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "executor/tuptable.h"
#include "access/tupdesc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/* Linked-list node tracking one compiled function's native code */
typedef struct CompiledCode
{
	struct CompiledCode *next;
	void	(*free_fn)(void *data);		/* backend-specific free function */
	void   *data;						/* backend-specific compiled code handle */
} CompiledCode;

/* Our JIT context — extends JitContext by embedding it as the first member */
typedef struct PgJitterContext
{
	JitContext		base;				/* must be first */
	CompiledCode   *compiled_list;		/* linked list of compiled code */
	ResourceOwner	resowner;
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

#endif /* PG_JITTER_COMMON_H */
