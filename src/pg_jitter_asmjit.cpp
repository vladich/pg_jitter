/*
 * pg_jitter_asmjit.cpp — AsmJIT-based PostgreSQL JIT provider
 *
 * Uses AsmJIT's Compiler for virtual register allocation.
 * Architecture-specific code generation is in .inc files.
 */

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "executor/execExpr.h"
#include "executor/tuptable.h"
#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "utils/expandeddatum.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "pg_jitter_common.h"
#include "pg_jit_funcs.h"
#include "pg_jit_deform_templates.h"
#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "utils/lsyscache.h"
#include "common/hashfn.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_jitter_asmjit",
);

/*
 * Mirror of Int8TransTypeData from numeric.c.
 */
typedef struct
{
	int64		count;
	int64		sum;
} JitInt8TransTypeData;

/* MIR precompiled blob support (shared infrastructure) */
#include "pg_jit_mir_blobs.h"
}

#if defined(__aarch64__) || defined(_M_ARM64)
#include <asmjit/a64.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <asmjit/x86.h>
#else
#error "Unsupported architecture for pg_jitter_asmjit"
#endif

using namespace asmjit;

/*
 * Try to match a tuple descriptor against pre-compiled deform templates.
 * Returns a function pointer to a shared-text deform function, or NULL.
 */
static deform_template_fn
asmjit_deform_match_template(TupleDesc desc, const TupleTableSlotOps *ops, int natts)
{
	int16	attlens[5];

	if (natts < 1 || natts > 5)
		return NULL;
	if (ops == &TTSOpsVirtual)
		return NULL;
	for (int i = 0; i < natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);
		if (!att->attbyval)
			return NULL;
		if (att->attlen != 1 && att->attlen != 2 &&
			att->attlen != 4 && att->attlen != 8)
			return NULL;
		if (att->attisdropped || att->atthasmissing)
			return NULL;
		attlens[i] = att->attlen;
	}
	return jit_deform_find_template(deform_signature(natts, attlens));
}

/* Per-expression compiled code handle */
struct AsmjitCode {
	JitRuntime	rt;
	void	   *func = nullptr;
};

/* Forward declarations */
static bool asmjit_compile_expr(ExprState *state);
static bool asmjit_emit_all(CodeHolder &code, ExprState *state,
                            PgJitterContext *ctx,
                            ExprEvalStep *steps, int steps_len);

static void
asmjit_code_free(void *data)
{
	AsmjitCode *ac = (AsmjitCode *) data;
	if (ac)
	{
		if (ac->func)
			ac->rt.release(ac->func);
		delete ac;
	}
}

extern "C" void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = pg_jitter_reset_after_error;
	cb->release_context = pg_jitter_release_context;
	cb->compile_expr = asmjit_compile_expr;
}

/*
 * Helper: determine econtext slot offset for a given opcode.
 */
static int64_t
slot_offset_for_opcode(ExprEvalOp opcode)
{
	switch (opcode)
	{
		case EEOP_INNER_FETCHSOME:
		case EEOP_INNER_VAR:
		case EEOP_ASSIGN_INNER_VAR:
			return offsetof(ExprContext, ecxt_innertuple);
		case EEOP_OUTER_FETCHSOME:
		case EEOP_OUTER_VAR:
		case EEOP_ASSIGN_OUTER_VAR:
			return offsetof(ExprContext, ecxt_outertuple);
		case EEOP_SCAN_FETCHSOME:
		case EEOP_SCAN_VAR:
		case EEOP_ASSIGN_SCAN_VAR:
			return offsetof(ExprContext, ecxt_scantuple);
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_FETCHSOME:
		case EEOP_OLD_VAR:
		case EEOP_ASSIGN_OLD_VAR:
			return offsetof(ExprContext, ecxt_oldtuple);
		case EEOP_NEW_FETCHSOME:
		case EEOP_NEW_VAR:
		case EEOP_ASSIGN_NEW_VAR:
			return offsetof(ExprContext, ecxt_newtuple);
#endif
		default:
			return offsetof(ExprContext, ecxt_scantuple);
	}
}

/*
 * Check if the expression matches one of PG's interpreter fast-path
 * patterns (ExecReadyInterpretedExpr).  These are hand-optimized C
 * functions that beat JIT for tiny 2-5 step expressions.
 */
static bool
expr_has_fast_path(ExprState *state)
{
	int		nsteps = state->steps_len;
	ExprEvalOp step0, step1, step2, step3;

	if (nsteps < 2 || nsteps > 5)
		return false;

	step0 = ExecEvalStepOp(state, &state->steps[0]);

#ifdef HAVE_EEOP_HASHDATUM
	if (nsteps == 5)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);
		step3 = ExecEvalStepOp(state, &state->steps[3]);

		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_HASHDATUM_SET_INITVAL &&
			step2 == EEOP_INNER_VAR &&
			step3 == EEOP_HASHDATUM_NEXT32)
			return true;
	}
	else
#endif
	if (nsteps == 4)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);

#ifdef HAVE_EEOP_HASHDATUM
		if (step0 == EEOP_OUTER_FETCHSOME &&
			step1 == EEOP_OUTER_VAR &&
			step2 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_INNER_VAR &&
			step2 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_OUTER_FETCHSOME &&
			step1 == EEOP_OUTER_VAR &&
			step2 == EEOP_HASHDATUM_FIRST_STRICT)
			return true;
#endif
	}
	else if (nsteps == 3)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);

		if (step0 == EEOP_INNER_FETCHSOME && step1 == EEOP_INNER_VAR)
			return true;
		if (step0 == EEOP_OUTER_FETCHSOME && step1 == EEOP_OUTER_VAR)
			return true;
		if (step0 == EEOP_SCAN_FETCHSOME && step1 == EEOP_SCAN_VAR)
			return true;

		if (step0 == EEOP_INNER_FETCHSOME && step1 == EEOP_ASSIGN_INNER_VAR)
			return true;
		if (step0 == EEOP_OUTER_FETCHSOME && step1 == EEOP_ASSIGN_OUTER_VAR)
			return true;
		if (step0 == EEOP_SCAN_FETCHSOME && step1 == EEOP_ASSIGN_SCAN_VAR)
			return true;

		if (step0 == EEOP_CASE_TESTVAL &&
			(step1 == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			 || step1 == EEOP_FUNCEXPR_STRICT_1
			 || step1 == EEOP_FUNCEXPR_STRICT_2
#endif
			))
			return true;

#ifdef HAVE_EEOP_HASHDATUM
		if (step0 == EEOP_INNER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_OUTER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
#endif
	}
	else if (nsteps == 2)
	{
		if (step0 == EEOP_CONST ||
			step0 == EEOP_INNER_VAR ||
			step0 == EEOP_OUTER_VAR ||
			step0 == EEOP_SCAN_VAR ||
			step0 == EEOP_ASSIGN_INNER_VAR ||
			step0 == EEOP_ASSIGN_OUTER_VAR ||
			step0 == EEOP_ASSIGN_SCAN_VAR)
			return true;
	}

	return false;
}

/* Include architecture-specific code generation */
#if defined(__aarch64__) || defined(_M_ARM64)
#include "pg_jitter_asmjit_arm64.inc"
#elif defined(__x86_64__) || defined(_M_X64)
#include "pg_jitter_asmjit_x86.inc"
#endif

static bool
asmjit_compile_expr(ExprState *state)
{
	PgJitterContext *ctx;
	AsmjitCode	   *ac;
	ExprEvalStep   *steps;
	int				steps_len;
	instr_time		starttime, endtime;

	if (!state->parent)
		return false;

	if (expr_has_fast_path(state))
		return false;

#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
	/* Lazy-load MIR precompiled blobs on first compile */
	mir_load_precompiled_blobs(NULL);
#endif

	ctx = pg_jitter_get_context(state);

	INSTR_TIME_SET_CURRENT(starttime);

	steps = state->steps;
	steps_len = state->steps_len;

	ac = new AsmjitCode();

	CodeHolder code;
	code.init(ac->rt.environment());

	if (!asmjit_emit_all(code, state, ctx, steps, steps_len))
	{
		delete ac;
		return false;
	}

	Error err = ac->rt.add(&ac->func, &code);
	if (err != kErrorOk)
	{
		delete ac;
		return false;
	}

	/* Register for cleanup */
	pg_jitter_register_compiled(ctx, asmjit_code_free, ac);

	/* Set the eval function (with validation wrapper on first call) */
	pg_jitter_install_expr(state, (ExprStateEvalFunc) ac->func);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(ctx->base.instr.generation_counter,
						  endtime, starttime);
	ctx->base.instr.created_functions++;

	return true;
}
