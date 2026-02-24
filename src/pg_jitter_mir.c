/*
 * pg_jitter_mir.c — MIR-based PostgreSQL JIT provider
 *
 * Uses MIR's medium-level IR with optimization pipeline.
 * Hot-path opcodes (~30) get native code; everything else falls back to
 * pg_jitter_fallback_step().
 */
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

/*
 * Mirror of Int8TransTypeData from numeric.c.
 */
typedef struct
{
	int64		count;
	int64		sum;
} MirInt8TransTypeData;
#include "mir.h"
#include "mir-gen.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_jitter_mir",
);

/* Forward declarations */
static bool mir_compile_expr(ExprState *state);
static void mir_reset_after_error(void);

/* Counter for unique register/proto names within a single expression */
static int mir_name_counter = 0;

/*
 * Persistent MIR context — shared across all expressions in the backend.
 *
 * MIR_init() and MIR_gen_init() are expensive (~0.7 ms each).  By keeping
 * the context alive we pay that cost once per backend instead of once per
 * expression (20× per query).  Generated code pointers remain valid as long
 * as the context lives.
 */
static MIR_context_t	mir_persistent_ctx = NULL;
static int				mir_module_counter = 0;

/*
 * Lazy-init the persistent MIR context.
 */
static MIR_context_t
mir_ensure_ctx(void)
{
	if (!mir_persistent_ctx)
	{
		mir_persistent_ctx = MIR_init();
		MIR_gen_init(mir_persistent_ctx);
		MIR_gen_set_optimize_level(mir_persistent_ctx, 0);
		mir_module_counter = 0;
	}
	return mir_persistent_ctx;
}

/*
 * Tear down the persistent MIR context (error reset / cleanup).
 */
static void
mir_destroy_ctx(void)
{
	if (mir_persistent_ctx)
	{
		MIR_gen_finish(mir_persistent_ctx);
		MIR_finish(mir_persistent_ctx);
		mir_persistent_ctx = NULL;
		mir_module_counter = 0;
	}
}

/*
 * Provider entry point.
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = mir_reset_after_error;
	cb->release_context = pg_jitter_release_context;
	cb->compile_expr = mir_compile_expr;
}

/*
 * Error reset — intentionally a no-op.
 *
 * We must NOT destroy the persistent MIR context here because other
 * transactions/portals (e.g. cursors surviving a ROLLBACK TO SAVEPOINT)
 * may still hold evalfunc pointers into MIR-generated code.  Destroying
 * the context would free those code buffers → SIGSEGV on next FETCH.
 *
 * Per-expression cleanup happens via mir_code_free() called from
 * pg_jitter_release_context() through the ResourceOwner machinery.
 * The persistent context's arena is reclaimed when the backend exits.
 */
static void
mir_reset_after_error(void)
{
	/* nothing to do */
}

/*
 * Per-expression free — the generated code lives in the persistent context's
 * code buffer, so there's nothing to free except our tiny tracking struct.
 */
static void
mir_code_free(void *data)
{
	if (data)
		pfree(data);
}

/*
 * Helper: get econtext slot offset for a given opcode.
 */
static int64_t
mir_slot_offset(ExprEvalOp opcode)
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
		case EEOP_OLD_FETCHSOME:
		case EEOP_OLD_VAR:
		case EEOP_ASSIGN_OLD_VAR:
			return offsetof(ExprContext, ecxt_oldtuple);
		case EEOP_NEW_FETCHSOME:
		case EEOP_NEW_VAR:
		case EEOP_ASSIGN_NEW_VAR:
			return offsetof(ExprContext, ecxt_newtuple);
		default:
			return offsetof(ExprContext, ecxt_scantuple);
	}
}

/*
 * Helper: create a uniquely-named MIR register.
 */
static MIR_reg_t
mir_new_reg(MIR_context_t ctx, MIR_func_t f, MIR_type_t type, const char *prefix)
{
	char	name[64];

	snprintf(name, sizeof(name), "%s_%d", prefix, mir_name_counter++);
	return MIR_new_func_reg(ctx, f, type, name);
}

/*
 * Check whether PG's hand-optimized fast-path evalfuncs handle this
 * expression better than JIT-compiled code.  Tiny 2-5 step expressions
 * that match these patterns should be left alone.
 */
static bool
expr_has_fast_path(ExprState *state)
{
	int		nsteps = state->steps_len;
	ExprEvalOp step0, step1, step2, step3;

	/* Fast-paths only exist for 2-5 step expressions */
	if (nsteps < 2 || nsteps > 5)
		return false;

	step0 = ExecEvalStepOp(state, &state->steps[0]);

	if (nsteps == 5)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);
		step3 = ExecEvalStepOp(state, &state->steps[3]);

		/* INNER_FETCHSOME + HASHDATUM_SET_INITVAL + INNER_VAR + HASHDATUM_NEXT32 + DONE */
		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_HASHDATUM_SET_INITVAL &&
			step2 == EEOP_INNER_VAR &&
			step3 == EEOP_HASHDATUM_NEXT32)
			return true;
	}
	else if (nsteps == 4)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);

		/* (INNER|OUTER)_FETCHSOME + (INNER|OUTER)_VAR + HASHDATUM_FIRST(_STRICT) + DONE */
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
	}
	else if (nsteps == 3)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);

		/* FETCHSOME + VAR */
		if (step0 == EEOP_INNER_FETCHSOME && step1 == EEOP_INNER_VAR)
			return true;
		if (step0 == EEOP_OUTER_FETCHSOME && step1 == EEOP_OUTER_VAR)
			return true;
		if (step0 == EEOP_SCAN_FETCHSOME && step1 == EEOP_SCAN_VAR)
			return true;

		/* FETCHSOME + ASSIGN_VAR */
		if (step0 == EEOP_INNER_FETCHSOME && step1 == EEOP_ASSIGN_INNER_VAR)
			return true;
		if (step0 == EEOP_OUTER_FETCHSOME && step1 == EEOP_ASSIGN_OUTER_VAR)
			return true;
		if (step0 == EEOP_SCAN_FETCHSOME && step1 == EEOP_ASSIGN_SCAN_VAR)
			return true;

		/* CASE_TESTVAL + FUNCEXPR_STRICT variants */
		if (step0 == EEOP_CASE_TESTVAL &&
			(step1 == EEOP_FUNCEXPR_STRICT ||
			 step1 == EEOP_FUNCEXPR_STRICT_1 ||
			 step1 == EEOP_FUNCEXPR_STRICT_2))
			return true;

		/* VAR + HASHDATUM_FIRST (virtual slot hash, no fetchsome) */
		if (step0 == EEOP_INNER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_OUTER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
	}
	else if (nsteps == 2)
	{
		/* CONST, VAR (inner/outer/scan), ASSIGN_VAR (inner/outer/scan) */
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

/*
 * Compile an expression using MIR.
 *
 * Generated function signature:
 *   Datum fn(ExprState *state, ExprContext *econtext, bool *isNull)
 */
static bool
mir_compile_expr(ExprState *state)
{
	PgJitterContext *jctx;
	MIR_context_t	ctx;
	MIR_module_t	m;
	MIR_item_t		func_item;
	MIR_func_t		f;
	ExprEvalStep   *steps;
	int				steps_len;
	instr_time		starttime, endtime;
	char			modname[32];

	MIR_reg_t		r_state, r_econtext, r_isnullp;
	MIR_reg_t		r_resvaluep, r_resnullp;
	MIR_reg_t		r_resultvals, r_resultnulls;
	MIR_reg_t		r_tmp1, r_tmp2, r_tmp3, r_slot;

	MIR_insn_t	   *step_labels;

	/* Prototypes and imports for function calls */
	MIR_item_t		proto_fallback, import_fallback;
	MIR_item_t		proto_getsomeattrs, import_getsomeattrs;
	MIR_item_t		proto_v1func, import_v1func;	/* per-step, see below */
	MIR_item_t		proto_makero, import_makero;
	MIR_type_t		res_type;

	if (!state->parent)
		return false;

	/* Let PG's hand-optimized fast-path evalfuncs handle tiny expressions */
	if (expr_has_fast_path(state))
		return false;

	jctx = pg_jitter_get_context(state);

	INSTR_TIME_SET_CURRENT(starttime);

	steps = state->steps;
	steps_len = state->steps_len;

	mir_name_counter = 0;

	ctx = mir_ensure_ctx();
	if (!ctx)
		return false;

	snprintf(modname, sizeof(modname), "m_%d", mir_module_counter++);
	m = MIR_new_module(ctx, modname);

	/*
	 * Create prototypes and imports at module level (before the function).
	 */

	/* Proto for pg_jitter_fallback_step(state, op, econtext) -> int */
	{
		MIR_type_t res_type = MIR_T_I64;
		proto_fallback = MIR_new_proto(ctx, "p_fallback",
									   1, &res_type,
									   3, MIR_T_P, "s", MIR_T_P, "o", MIR_T_P, "e");
	}
	import_fallback = MIR_new_import(ctx, "fallback_step");

	/* Proto for slot_getsomeattrs_int(slot, last_var) -> void */
	proto_getsomeattrs = MIR_new_proto(ctx, "p_getsomeattrs",
										0, NULL,
										2, MIR_T_P, "sl", MIR_T_I32, "n");
	import_getsomeattrs = MIR_new_import(ctx, "getsomeattrs");

	/* Proto for V1 function: fn(FunctionCallInfo) -> Datum */
	res_type = MIR_T_I64;
	proto_v1func = MIR_new_proto(ctx, "p_v1func",
								  1, &res_type,
								  1, MIR_T_P, "fci");

	/* Proto for MakeExpandedObjectReadOnlyInternal(Datum) -> Datum */
	proto_makero = MIR_new_proto(ctx, "p_makero",
								 1, &res_type,
								 1, MIR_T_I64, "d");
	import_makero = MIR_new_import(ctx, "make_ro");

	/* Protos for direct native calls (1-arg and 2-arg, int64 args/ret) */
	MIR_item_t proto_direct1, proto_direct2;
	{
		MIR_type_t rt = MIR_T_I64;
		proto_direct1 = MIR_new_proto(ctx, "p_d1", 1, &rt, 1, MIR_T_I64, "a0");
		proto_direct2 = MIR_new_proto(ctx, "p_d2", 1, &rt, 2, MIR_T_I64, "a0", MIR_T_I64, "a1");
	}

	/* Proto for agg_trans helpers: (ExprState*, ExprEvalStep*) -> void */
	MIR_item_t proto_agg_helper;
	proto_agg_helper = MIR_new_proto(ctx, "p_aggh", 0, NULL,
									  2, MIR_T_P, "s", MIR_T_P, "o");

	/* Proto + imports for inline error handlers: void -> void (noreturn) */
	MIR_item_t proto_err_void;
	proto_err_void = MIR_new_proto(ctx, "p_err", 0, NULL, 0);
	MIR_item_t import_err_int4_overflow = MIR_new_import(ctx, "err_i4ov");
	MIR_item_t import_err_int8_overflow = MIR_new_import(ctx, "err_i8ov");
	MIR_item_t import_err_div_by_zero = MIR_new_import(ctx, "err_divz");

	/*
	 * Per-step imports for V1 function addresses.
	 * We need unique import names for each distinct fn_addr.
	 */
	MIR_item_t *step_fn_imports = palloc0(sizeof(MIR_item_t) * steps_len);
	MIR_item_t *step_direct_imports = palloc0(sizeof(MIR_item_t) * steps_len);
	for (int i = 0; i < steps_len; i++)
	{
		ExprEvalStep *op = &steps[i];
		ExprEvalOp opcode = ExecEvalStepOp(state, op);

		if (opcode == EEOP_FUNCEXPR ||
			opcode == EEOP_FUNCEXPR_STRICT ||
			opcode == EEOP_FUNCEXPR_STRICT_1 ||
			opcode == EEOP_FUNCEXPR_STRICT_2)
		{
			const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);
			if (dfn && dfn->jit_fn)
			{
				/* Create import for the direct native function */
				char name[32];
				snprintf(name, sizeof(name), "dfn_%d", i);
				step_direct_imports[i] = MIR_new_import(ctx, name);
			}
			else
			{
				char name[32];
				snprintf(name, sizeof(name), "fn_%d", i);
				step_fn_imports[i] = MIR_new_import(ctx, name);
			}
		}
		else if (opcode == EEOP_HASHDATUM_FIRST ||
				 opcode == EEOP_HASHDATUM_FIRST_STRICT ||
				 opcode == EEOP_HASHDATUM_NEXT32 ||
				 opcode == EEOP_HASHDATUM_NEXT32_STRICT)
		{
			PGFunction hash_fn = op->d.hashdatum.fn_addr;
			const JitDirectFn *dfn = jit_find_direct_fn(hash_fn);
			if (dfn && dfn->jit_fn)
			{
				char name[32];
				snprintf(name, sizeof(name), "dhfn_%d", i);
				step_direct_imports[i] = MIR_new_import(ctx, name);
			}
			else
			{
				char name[32];
				snprintf(name, sizeof(name), "fn_%d", i);
				step_fn_imports[i] = MIR_new_import(ctx, name);
			}
		}
		else if (opcode >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
				 opcode <= EEOP_AGG_PLAIN_TRANS_BYREF)
		{
			/* Pre-create import for agg_trans helper */
			char name[32];
			snprintf(name, sizeof(name), "ah_%d", i);
			step_direct_imports[i] = MIR_new_import(ctx, name);
		}
	}

	/*
	 * Create the JIT function.
	 */
	func_item = MIR_new_func(ctx, "jit_eval",
							  1, &res_type,
							  3, MIR_T_P, "state", MIR_T_P, "econtext", MIR_T_P, "isNull");
	f = func_item->u.func;

	/* Get parameter registers */
	r_state = MIR_reg(ctx, "state", f);
	r_econtext = MIR_reg(ctx, "econtext", f);
	r_isnullp = MIR_reg(ctx, "isNull", f);

	/* Create local registers */
	r_resvaluep = mir_new_reg(ctx, f, MIR_T_I64, "rvp");
	r_resnullp = mir_new_reg(ctx, f, MIR_T_I64, "rnp");
	r_resultvals = mir_new_reg(ctx, f, MIR_T_I64, "rvals");
	r_resultnulls = mir_new_reg(ctx, f, MIR_T_I64, "rnulls");
	r_tmp1 = mir_new_reg(ctx, f, MIR_T_I64, "t1");
	r_tmp2 = mir_new_reg(ctx, f, MIR_T_I64, "t2");
	r_tmp3 = mir_new_reg(ctx, f, MIR_T_I64, "t3");
	r_slot = mir_new_reg(ctx, f, MIR_T_I64, "sl");

	/*
	 * Prologue: cache frequently-used pointers.
	 */

	/* r_resvaluep = &state->resvalue */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_ADD,
			MIR_new_reg_op(ctx, r_resvaluep),
			MIR_new_reg_op(ctx, r_state),
			MIR_new_int_op(ctx, offsetof(ExprState, resvalue))));

	/* r_resnullp = &state->resnull */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_ADD,
			MIR_new_reg_op(ctx, r_resnullp),
			MIR_new_reg_op(ctx, r_state),
			MIR_new_int_op(ctx, offsetof(ExprState, resnull))));

	/* r_slot = state->resultslot (may be NULL for non-projecting exprs) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_slot),
			MIR_new_mem_op(ctx, MIR_T_P,
				offsetof(ExprState, resultslot),
				r_state, 0, 1)));

	{
		MIR_label_t skip_rs_label = MIR_new_label(ctx);

		/* if (r_slot == 0) goto skip_rs_label */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, skip_rs_label),
				MIR_new_reg_op(ctx, r_slot), MIR_new_int_op(ctx, 0)));

		/* r_resultvals = resultslot->tts_values */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, r_resultvals),
				MIR_new_mem_op(ctx, MIR_T_P,
					offsetof(TupleTableSlot, tts_values),
					r_slot, 0, 1)));

		/* r_resultnulls = resultslot->tts_isnull */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, r_resultnulls),
				MIR_new_mem_op(ctx, MIR_T_P,
					offsetof(TupleTableSlot, tts_isnull),
					r_slot, 0, 1)));

		MIR_append_insn(ctx, func_item, skip_rs_label);
	}

	/*
	 * Create labels for each step.
	 */
	step_labels = palloc(sizeof(MIR_insn_t) * steps_len);
	for (int i = 0; i < steps_len; i++)
		step_labels[i] = MIR_new_label(ctx);

	/*
	 * Emit code for each step.
	 */
	for (int opno = 0; opno < steps_len; opno++)
	{
		ExprEvalStep   *op = &steps[opno];
		ExprEvalOp		opcode = ExecEvalStepOp(state, op);

		MIR_append_insn(ctx, func_item, step_labels[opno]);

		switch (opcode)
		{
			/*
			 * ---- DONE ----
			 */
			case EEOP_DONE_RETURN:
			{
				/* r_tmp1 = state->resvalue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64,
							offsetof(ExprState, resvalue),
							r_state, 0, 1)));
				/* r_tmp2 = state->resnull (byte) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(ExprState, resnull),
							r_state, 0, 1)));
				/* *isNull = resnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_isnullp, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				/* return resvalue */
				MIR_append_insn(ctx, func_item,
					MIR_new_ret_insn(ctx, 1,
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			case EEOP_DONE_NO_RETURN:
			{
				MIR_append_insn(ctx, func_item,
					MIR_new_ret_insn(ctx, 1,
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- FETCHSOME ----
			 */
			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
			case EEOP_OLD_FETCHSOME:
			case EEOP_NEW_FETCHSOME:
			{
				MIR_insn_t	skip_label = MIR_new_label(ctx);
				int64_t		soff = mir_slot_offset(opcode);

				/* r_slot = econtext->ecxt_*tuple */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_mem_op(ctx, MIR_T_P, soff,
							r_econtext, 0, 1)));

				/* r_tmp1 = slot->tts_nvalid (int16) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I16,
							offsetof(TupleTableSlot, tts_nvalid),
							r_slot, 0, 1)));

				/* if tts_nvalid >= last_var, skip */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BGE,
						MIR_new_label_op(ctx, skip_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, op->d.fetch.last_var)));

				/* Call slot_getsomeattrs_int(slot, last_var) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_getsomeattrs),
						MIR_new_ref_op(ctx, import_getsomeattrs),
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_int_op(ctx, op->d.fetch.last_var)));

				MIR_append_insn(ctx, func_item, skip_label);
				break;
			}

			/*
			 * ---- VAR ----
			 */
			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
			case EEOP_OLD_VAR:
			case EEOP_NEW_VAR:
			{
				int			attnum = op->d.var.attnum;
				int64_t		soff = mir_slot_offset(opcode);

				/* r_slot = econtext->ecxt_*tuple */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_mem_op(ctx, MIR_T_P, soff,
							r_econtext, 0, 1)));

				/* r_tmp1 = slot->tts_values (pointer) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_values),
							r_slot, 0, 1)));
				/* r_tmp2 = values[attnum] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							attnum * (int64_t) sizeof(Datum),
							r_tmp1, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp1 = slot->tts_isnull (pointer) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_isnull),
							r_slot, 0, 1)));
				/* r_tmp2 = isnull[attnum] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							attnum * (int64_t) sizeof(bool),
							r_tmp1, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- ASSIGN_*_VAR ----
			 */
			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
			case EEOP_ASSIGN_OLD_VAR:
			case EEOP_ASSIGN_NEW_VAR:
			{
				int			attnum = op->d.assign_var.attnum;
				int			resultnum = op->d.assign_var.resultnum;
				int64_t		soff = mir_slot_offset(opcode);

				/* r_slot = source slot */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_mem_op(ctx, MIR_T_P, soff,
							r_econtext, 0, 1)));

				/* Load value from source */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_values),
							r_slot, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							attnum * (int64_t) sizeof(Datum),
							r_tmp1, 0, 1)));
				/* Store to result values */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64,
							resultnum * (int64_t) sizeof(Datum),
							r_resultvals, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* Load null from source */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_isnull),
							r_slot, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							attnum * (int64_t) sizeof(bool),
							r_tmp1, 0, 1)));
				/* Store to result nulls */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							resultnum * (int64_t) sizeof(bool),
							r_resultnulls, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- ASSIGN_TMP / ASSIGN_TMP_MAKE_RO ----
			 */
			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
			{
				int			resultnum = op->d.assign_tmp.resultnum;

				/* r_tmp1 = state->resvalue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64,
							offsetof(ExprState, resvalue),
							r_state, 0, 1)));
				/* r_tmp2 = state->resnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(ExprState, resnull),
							r_state, 0, 1)));

				/* Store null first */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							resultnum * (int64_t) sizeof(bool),
							r_resultnulls, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				{
					MIR_insn_t	skip_ro = MIR_new_label(ctx);
					MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "mro");

					/* If null, skip MakeReadOnly */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNE,
							MIR_new_label_op(ctx, skip_ro),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));

					/* r_tmp1 = MakeExpandedObjectReadOnlyInternal(r_tmp1) */
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_makero),
							MIR_new_ref_op(ctx, import_makero),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_tmp1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_reg_op(ctx, r_ret)));

					MIR_append_insn(ctx, func_item, skip_ro);
				}

				/* Store value */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64,
							resultnum * (int64_t) sizeof(Datum),
							r_resultvals, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- CONST ----
			 */
			case EEOP_CONST:
			{
				/* *op->resvalue = constval.value */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, (int64_t) op->d.constval.value)));

				/* *op->resnull = constval.isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, op->d.constval.isnull ? 1 : 0)));
				break;
			}

			/*
			 * ---- FUNCEXPR / FUNCEXPR_STRICT ----
			 */
			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int			nargs = op->d.func.nargs;
				MIR_insn_t	done_label = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "fret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "fci");

				if (opcode == EEOP_FUNCEXPR_STRICT ||
					opcode == EEOP_FUNCEXPR_STRICT_1 ||
					opcode == EEOP_FUNCEXPR_STRICT_2)
				{
					/* Set resnull = true */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));

					/*
					 * Batched null check: OR all isnull flags, single branch.
					 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_fci),
							MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					if (nargs > 1 && nargs <= 4)
					{
						int64_t null_off_0 =
							(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_mem_op(ctx, MIR_T_U8, null_off_0,
									r_fci, 0, 1)));
						for (int argno = 1; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull -
										  (char *)fcinfo);
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_MOV,
									MIR_new_reg_op(ctx, r_tmp2),
									MIR_new_mem_op(ctx, MIR_T_U8, null_off,
										r_fci, 0, 1)));
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_OR,
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_reg_op(ctx, r_tmp2)));
						}
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNE,
								MIR_new_label_op(ctx, done_label),
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_int_op(ctx, 0)));
					}
					else
					{
						for (int argno = 0; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull -
										  (char *)fcinfo);
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_MOV,
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_mem_op(ctx, MIR_T_U8, null_off,
										r_fci, 0, 1)));
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_BNE,
									MIR_new_label_op(ctx, done_label),
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_int_op(ctx, 0)));
						}
					}
				}

				/*
				 * Try direct native call — bypasses fcinfo entirely.
				 * Dispatch order: inline → direct call → fcinfo fallback.
				 */
				{
				const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);

				if (dfn && dfn->inline_op != JIT_INLINE_NONE)
				{
					/*
					 * TIER 0 — INLINE: emit the operation as MIR
					 * instructions, no function call at all.
					 */
					MIR_reg_t a0 = mir_new_reg(ctx, f, MIR_T_I64, "ia0");
					MIR_reg_t a1 = mir_new_reg(ctx, f, MIR_T_I64, "ia1");
					int64_t val_off_0 =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					int64_t val_off_1 =
						(int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_fci),
							MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, a0),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off_0,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, a1),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off_1,
								r_fci, 0, 1)));

					switch ((JitInlineOp) dfn->inline_op)
					{
					/* ---- int32 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT4_ADD:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_ADDOS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						/* Sign-extend 32→64 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_SUB:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_SUBOS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_MUL:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MULOS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_DIV:
					{
						MIR_insn_t not_zero = MIR_new_label(ctx);
						MIR_insn_t not_minmax = MIR_new_label(ctx);
						MIR_insn_t not_neg1 = MIR_new_label(ctx);
						/* Check divisor == 0 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_zero),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, 0)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_div_by_zero)));
						MIR_append_insn(ctx, func_item, not_zero);
						/* Check INT32_MIN / -1 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_minmax),
								MIR_new_reg_op(ctx, a0),
								MIR_new_int_op(ctx, (int32_t) PG_INT32_MIN)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_neg1),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, -1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, not_neg1);
						MIR_append_insn(ctx, func_item, not_minmax);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_DIVS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_MOD:
					{
						MIR_insn_t not_zero = MIR_new_label(ctx);
						MIR_insn_t not_minmax = MIR_new_label(ctx);
						MIR_insn_t zero_result = MIR_new_label(ctx);
						MIR_insn_t after = MIR_new_label(ctx);
						/* Check divisor == 0 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_zero),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, 0)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_div_by_zero)));
						MIR_append_insn(ctx, func_item, not_zero);
						/* Check INT32_MIN % -1 → 0 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_minmax),
								MIR_new_reg_op(ctx, a0),
								MIR_new_int_op(ctx, (int32_t) PG_INT32_MIN)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BEQS,
								MIR_new_label_op(ctx, zero_result),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, -1)));
						MIR_append_insn(ctx, func_item, not_minmax);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MODS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_JMP,
								MIR_new_label_op(ctx, after)));
						MIR_append_insn(ctx, func_item, zero_result);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_int_op(ctx, 0)));
						MIR_append_insn(ctx, func_item, after);
						break;
					}
					/* ---- int64 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT8_ADD:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_ADDO,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int8_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						break;
					}
					case JIT_INLINE_INT8_SUB:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_SUBO,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int8_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						break;
					}
					case JIT_INLINE_INT8_MUL:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MULO,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int8_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						break;
					}
					/* ---- int32 comparison ---- */
					case JIT_INLINE_INT4_EQ:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EQS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_NE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_NES,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_LT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LTS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_LE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LES,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_GT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GTS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_GE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GES,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					/* ---- int64 comparison ---- */
					case JIT_INLINE_INT8_EQ:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EQ,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_NE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_NE,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_LT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LT,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_LE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LE,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_GT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GT,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_GE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GE,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					default:
						Assert(false);
						break;
					}

					/* *op->resvalue = r_ret */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_reg_op(ctx, r_ret)));

					/* *op->resnull = false */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else if (dfn && dfn->jit_fn && step_direct_imports[opno])
				{
					/* Load fcinfo once, then load all args from offsets */
					MIR_reg_t r_args[4];
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_fci),
							MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					for (int i = 0; i < dfn->nargs; i++)
					{
						char aname[16];
						snprintf(aname, sizeof(aname), "da%d_%d", i, opno);
						r_args[i] = mir_new_reg(ctx, f, MIR_T_I64, aname);
						int64_t val_off = (int64_t)((char *)&fcinfo->args[i].value -
													(char *)fcinfo);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_args[i]),
								MIR_new_mem_op(ctx, MIR_T_I64, val_off,
									r_fci, 0, 1)));
					}

					/* Direct call */
					MIR_item_t d_proto = (dfn->nargs == 1)
						? proto_direct1 : proto_direct2;
					if (dfn->nargs == 1)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 4,
								MIR_new_ref_op(ctx, d_proto),
								MIR_new_ref_op(ctx, step_direct_imports[opno]),
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_args[0])));
					}
					else
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 5,
								MIR_new_ref_op(ctx, d_proto),
								MIR_new_ref_op(ctx, step_direct_imports[opno]),
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_args[0]),
								MIR_new_reg_op(ctx, r_args[1])));
					}

					/* *op->resvalue = result */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_reg_op(ctx, r_ret)));

					/* *op->resnull = false */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else
				{
				/* Fallback: generic fcinfo path */

				/* fcinfo->isnull = false (caller must clear before V1 call) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_fci),
						MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* Call fn_addr(fcinfo) → result */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));

				/* *op->resvalue = result */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resnull = fcinfo->isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				} /* end else fallback */
				} /* end direct-call dispatch */

				MIR_append_insn(ctx, func_item, done_label);
				break;
			}

			/*
			 * ---- BOOL_AND_STEP ----
			 */
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_LAST:
			{
				MIR_insn_t	null_handler = MIR_new_label(ctx);
				MIR_insn_t	cont = MIR_new_label(ctx);

				if (opcode == EEOP_BOOL_AND_STEP_FIRST)
				{
					/* *anynull = false */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->d.boolexpr.anynull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}

				/* Load resnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				/* If null, go to null_handler */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_handler),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Not null: check if false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

				/* If false (value == 0), short-circuit */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, step_labels[op->d.boolexpr.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Jump over null handler */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, cont)));

				/* Null handler: set *anynull = true */
				MIR_append_insn(ctx, func_item, null_handler);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->d.boolexpr.anynull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));

				MIR_append_insn(ctx, func_item, cont);

				/* On last step: if anynull, set result to NULL */
				if (opcode == EEOP_BOOL_AND_STEP_LAST)
				{
					MIR_insn_t	no_anynull = MIR_new_label(ctx);

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->d.boolexpr.anynull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, no_anynull),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));

					/* Set resnull = true, resvalue = 0 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));

					MIR_append_insn(ctx, func_item, no_anynull);
				}
				break;
			}

			/*
			 * ---- BOOL_OR_STEP ----
			 */
			case EEOP_BOOL_OR_STEP_FIRST:
			case EEOP_BOOL_OR_STEP:
			case EEOP_BOOL_OR_STEP_LAST:
			{
				MIR_insn_t	null_handler = MIR_new_label(ctx);
				MIR_insn_t	cont = MIR_new_label(ctx);

				if (opcode == EEOP_BOOL_OR_STEP_FIRST)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->d.boolexpr.anynull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}

				/* Load resnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_handler),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Not null: check if true */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

				/* If true (value != 0), short-circuit */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[op->d.boolexpr.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, cont)));

				/* Null handler */
				MIR_append_insn(ctx, func_item, null_handler);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->d.boolexpr.anynull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));

				MIR_append_insn(ctx, func_item, cont);

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					MIR_insn_t	no_anynull = MIR_new_label(ctx);

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->d.boolexpr.anynull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, no_anynull),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));

					MIR_append_insn(ctx, func_item, no_anynull);
				}
				break;
			}

			/*
			 * ---- BOOL_NOT_STEP ----
			 */
			case EEOP_BOOL_NOT_STEP:
			{
				/* Load resvalue, negate, store back */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				/* r_tmp1 = (r_tmp1 == 0) ? 1 : 0 */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_EQ,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- QUAL ----
			 */
			case EEOP_QUAL:
			{
				MIR_insn_t	qualfail = MIR_new_label(ctx);
				MIR_insn_t	cont = MIR_new_label(ctx);

				/* Check null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, qualfail),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Check false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, qualfail),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Pass: continue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, cont)));

				/* Qual fail: set resvalue=0, resnull=false, jump to done */
				MIR_append_insn(ctx, func_item, qualfail);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.qualexpr.jumpdone])));

				MIR_append_insn(ctx, func_item, cont);
				break;
			}

			/*
			 * ---- JUMP variants ----
			 */
			case EEOP_JUMP:
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone])));
				break;

			case EEOP_JUMP_IF_NULL:
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				break;

			case EEOP_JUMP_IF_NOT_NULL:
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				break;

			case EEOP_JUMP_IF_NOT_TRUE:
			{
				/* Jump if null OR false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				/* If null, jump */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				/* If false, jump */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- NULLTEST ----
			 */
			case EEOP_NULLTEST_ISNULL:
			{
				/* resvalue = resnull ? 1 : 0; resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				/* Store as Datum */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				/* resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				/* r_tmp1 = !r_tmp1 */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_EQ,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				/* Store as Datum */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				/* resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_SET_INITVAL ----
			 */
			case EEOP_HASHDATUM_SET_INITVAL:
			{
				/* *op->resvalue = op->d.hashdatum_initvalue.init_value */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, (int64_t) op->d.hashdatum_initvalue.init_value)));

				/* *op->resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_FIRST (non-strict) ----
			 */
			case EEOP_HASHDATUM_FIRST:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	store_zero = MIR_new_label(ctx);
				MIR_insn_t	store_result = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");

				/* r_fci = fcinfo */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_fci),
						MIR_new_uint_op(ctx, (uint64_t) fcinfo)));

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, jump to store_zero */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, store_zero),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Call hash function (direct or fcinfo) */
				if (step_direct_imports[opno])
				{
					/* Direct: load arg from fcinfo->args[0].value */
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(FunctionCallInfoBaseData, isnull),
								r_fci, 0, 1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_v1func),
							MIR_new_ref_op(ctx, step_fn_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_fci)));
				}

				/* r_tmp1 = r_ret */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_ret)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, store_result)));

				/* store_zero: r_tmp1 = 0 */
				MIR_append_insn(ctx, func_item, store_zero);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* store_result: *op->resvalue = r_tmp1, *op->resnull = false */
				MIR_append_insn(ctx, func_item, store_result);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_FIRST_STRICT ----
			 */
			case EEOP_HASHDATUM_FIRST_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	null_path = MIR_new_label(ctx);
				MIR_insn_t	after_null = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");

				/* r_fci = fcinfo */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_fci),
						MIR_new_uint_op(ctx, (uint64_t) fcinfo)));

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, jump to null_path */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_path),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				if (step_direct_imports[opno])
				{
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = call fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));
				}

				/* *op->resvalue = r_ret */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, after_null)));

				/* null_path: set resnull=1, resvalue=0, jump to jumpdone */
				MIR_append_insn(ctx, func_item, null_path);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.hashdatum.jumpdone])));

				MIR_append_insn(ctx, func_item, after_null);
				break;
			}

			/*
			 * ---- HASHDATUM_NEXT32 (non-strict, rotate + XOR) ----
			 */
			case EEOP_HASHDATUM_NEXT32:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				NullableDatum *iresult = op->d.hashdatum.iresult;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	skip_hash = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");
				MIR_reg_t	r_hash = mir_new_reg(ctx, f, MIR_T_I64, "hash");

				/* Load existing hash: r_hash = iresult->value (as U32) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_uint_op(ctx, (uint64_t) &iresult->value)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_mem_op(ctx, MIR_T_U32, 0, r_tmp1, 0, 1)));

				/* Rotate left 1: r_hash = (r_hash << 1) | (r_hash >> 31) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_LSHS,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_URSHS,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 31)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_fci = fcinfo */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_fci),
						MIR_new_uint_op(ctx, (uint64_t) fcinfo)));

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, skip hash call */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, skip_hash),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				if (step_direct_imports[opno])
				{
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = call fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));
				}

				/* r_hash = r_hash XOR r_ret (32-bit) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_XORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_ret)));

				/* skip_hash: store result */
				MIR_append_insn(ctx, func_item, skip_hash);

				/* *op->resvalue = r_hash (already zero-extended from 32-bit ops) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_hash)));

				/* *op->resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_NEXT32_STRICT ----
			 */
			case EEOP_HASHDATUM_NEXT32_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				NullableDatum *iresult = op->d.hashdatum.iresult;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	null_path = MIR_new_label(ctx);
				MIR_insn_t	after_null = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");
				MIR_reg_t	r_hash = mir_new_reg(ctx, f, MIR_T_I64, "hash");

				/* r_fci = fcinfo */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_fci),
						MIR_new_uint_op(ctx, (uint64_t) fcinfo)));

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, jump to null_path */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_path),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Load existing hash: r_hash = iresult->value (as U32) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_uint_op(ctx, (uint64_t) &iresult->value)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_mem_op(ctx, MIR_T_U32, 0, r_tmp1, 0, 1)));

				/* Rotate left 1: r_hash = (r_hash << 1) | (r_hash >> 31) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_LSHS,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_URSHS,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 31)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp2)));

				if (step_direct_imports[opno])
				{
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = call fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));
				}

				/* r_hash = r_hash XOR r_ret (32-bit) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_XORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resvalue = r_hash */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_hash)));

				/* *op->resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, after_null)));

				/* null_path: set resnull=1, resvalue=0, jump to jumpdone */
				MIR_append_insn(ctx, func_item, null_path);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resnull)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_uint_op(ctx, (uint64_t) op->resvalue)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.hashdatum.jumpdone])));

				MIR_append_insn(ctx, func_item, after_null);
				break;
			}

			/*
			 * ---- AGG_PLAIN_TRANS (all 6 variants) ----
			 * Call the specific agg_trans helper directly instead of
			 * going through fallback_step's switch dispatch.
			 */
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_BYREF:
			{
				bool	is_init = (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
							   opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF);
				bool	is_strict = (opcode != EEOP_AGG_PLAIN_TRANS_BYVAL &&
							 opcode != EEOP_AGG_PLAIN_TRANS_BYREF);
				bool	is_byref = (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
							opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF ||
							opcode == EEOP_AGG_PLAIN_TRANS_BYREF);

				AggState   *aggstate = castNode(AggState, state->parent);
				AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
				int			setoff = op->d.agg_trans.setoff;
				int			transno = op->d.agg_trans.transno;
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				PGFunction	fn_addr = fcinfo->flinfo->fn_addr;
				ExprContext *aggcontext = op->d.agg_trans.aggcontext;

				MIR_insn_t	end_label = MIR_new_label(ctx);
				MIR_reg_t	r_pergroup = mir_new_reg(ctx, f, MIR_T_I64, "pg");
				MIR_reg_t	r_aggst = mir_new_reg(ctx, f, MIR_T_I64, "agst");

				/* Compute pergroup at runtime: aggstate->all_pergroups[setoff] + transno */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_aggst),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprState, parent),
							r_state, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_pergroup),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(AggState, all_pergroups),
							r_aggst, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_pergroup),
						MIR_new_mem_op(ctx, MIR_T_P,
							setoff * (int64_t)sizeof(AggStatePerGroup),
							r_pergroup, 0, 1)));
				if (transno != 0)
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_pergroup),
							MIR_new_reg_op(ctx, r_pergroup),
							MIR_new_int_op(ctx,
								transno * (int64_t)sizeof(AggStatePerGroupData))));

				/* INIT check */
				if (is_init)
				{
					MIR_insn_t no_init = MIR_new_label(ctx);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, noTransValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, no_init),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
					/* noTransValue=true: call helper(state, op) which handles
					 * ExecAggInitGroup internally. */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_uint_op(ctx, (uint64_t) op)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_agg_helper),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_state),
							MIR_new_reg_op(ctx, r_tmp1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, end_label)));
					MIR_append_insn(ctx, func_item, no_init);
				}

				/* STRICT check: skip if transValueIsNull */
				if (is_strict)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNE,
							MIR_new_label_op(ctx, end_label),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
				}

				/*
				 * Transition function dispatch — inline hot aggs.
				 */
				if (!is_byref &&
					(fn_addr == int8inc || fn_addr == int8inc_any))
				{
					/* COUNT: transValue += 1 (int64, overflow-checked) */
					MIR_insn_t ok = MIR_new_label(ctx);
					MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADDO,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNO,
							MIR_new_label_op(ctx, ok)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 2,
							MIR_new_ref_op(ctx, proto_err_void),
							MIR_new_ref_op(ctx, import_err_int8_overflow)));
					MIR_append_insn(ctx, func_item, ok);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1),
							MIR_new_reg_op(ctx, r_tv)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else if (!is_byref && fn_addr == int4_sum)
				{
					/* SUM(int4): transValue += (int64)arg1 */
					MIR_insn_t arg_not_null = MIR_new_label(ctx);
					MIR_insn_t trans_not_null = MIR_new_label(ctx);
					MIR_insn_t after_sum = MIR_new_label(ctx);
					MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "sfci");
					MIR_reg_t r_arg1 = mir_new_reg(ctx, f, MIR_T_I64, "arg1");
					int64_t isnull1_off = (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_fci),
							MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					/* Check arg1 isnull */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8, isnull1_off, r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, arg_not_null),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, end_label)));

					MIR_append_insn(ctx, func_item, arg_not_null);
					/* Load arg1, sign-extend int32→int64 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_arg1),
							MIR_new_mem_op(ctx, MIR_T_I64, val1_off, r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EXT32,
							MIR_new_reg_op(ctx, r_arg1),
							MIR_new_reg_op(ctx, r_arg1)));

					/* Check transValueIsNull */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, trans_not_null),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));

					/* First non-null: transValue = (int64)arg1 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1),
							MIR_new_reg_op(ctx, r_arg1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, after_sum)));

					/* Normal: transValue += (int64)arg1 */
					MIR_append_insn(ctx, func_item, trans_not_null);
					{
						MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tv),
								MIR_new_mem_op(ctx, MIR_T_I64,
									offsetof(AggStatePerGroupData, transValue),
									r_pergroup, 0, 1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_ADD,
								MIR_new_reg_op(ctx, r_tv),
								MIR_new_reg_op(ctx, r_tv),
								MIR_new_reg_op(ctx, r_arg1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_mem_op(ctx, MIR_T_I64,
									offsetof(AggStatePerGroupData, transValue),
									r_pergroup, 0, 1),
								MIR_new_reg_op(ctx, r_tv)));
					}
					MIR_append_insn(ctx, func_item, after_sum);
				}
				else if (!is_byref &&
						 (fn_addr == int4smaller || fn_addr == int4larger))
				{
					/* MIN/MAX(int4) */
					bool is_min = (fn_addr == int4smaller);
					MIR_insn_t skip_update = MIR_new_label(ctx);
					MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
					MIR_reg_t r_nv = mir_new_reg(ctx, f, MIR_T_I64, "nv");
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_nv),
							MIR_new_mem_op(ctx, MIR_T_I64, val1_off, r_tmp1, 0, 1)));
					/* Sign-extend both */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EXT32,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_reg_op(ctx, r_tv)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EXT32,
							MIR_new_reg_op(ctx, r_nv),
							MIR_new_reg_op(ctx, r_nv)));
					/* MIN: skip if tv <= nv; MAX: skip if tv >= nv */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, is_min ? MIR_BLE : MIR_BGE,
							MIR_new_label_op(ctx, skip_update),
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_reg_op(ctx, r_nv)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1),
							MIR_new_reg_op(ctx, r_nv)));
					MIR_append_insn(ctx, func_item, skip_update);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else if (is_byref &&
						 (fn_addr == int4_avg_accum || fn_addr == int2_avg_accum))
				{
					/* AVG(int4/int2): in-place update of {count, sum} */
					#define MIR_INT8_TRANS_OFFSET 24
					MIR_reg_t r_td = mir_new_reg(ctx, f, MIR_T_I64, "td");
					MIR_reg_t r_cnt = mir_new_reg(ctx, f, MIR_T_I64, "cnt");
					MIR_reg_t r_sum = mir_new_reg(ctx, f, MIR_T_I64, "sum");
					MIR_reg_t r_a1 = mir_new_reg(ctx, f, MIR_T_I64, "a1v");
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					/* r_td = transValue + 24 = pointer to Int8TransTypeData */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_td),
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_td),
							MIR_new_reg_op(ctx, r_td),
							MIR_new_int_op(ctx, MIR_INT8_TRANS_OFFSET)));

					/* count++ */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_cnt),
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_td, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_cnt),
							MIR_new_reg_op(ctx, r_cnt),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_td, 0, 1),
							MIR_new_reg_op(ctx, r_cnt)));

					/* Load arg1, sign-extend */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_a1),
							MIR_new_mem_op(ctx, MIR_T_I64, val1_off, r_tmp1, 0, 1)));
					if (fn_addr == int4_avg_accum)
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_a1),
								MIR_new_reg_op(ctx, r_a1)));
					else
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT16,
								MIR_new_reg_op(ctx, r_a1),
								MIR_new_reg_op(ctx, r_a1)));

					/* sum += arg1 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_sum),
							MIR_new_mem_op(ctx, MIR_T_I64,
								(int64_t) offsetof(MirInt8TransTypeData, sum),
								r_td, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_sum),
							MIR_new_reg_op(ctx, r_sum),
							MIR_new_reg_op(ctx, r_a1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								(int64_t) offsetof(MirInt8TransTypeData, sum),
								r_td, 0, 1),
							MIR_new_reg_op(ctx, r_sum)));

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
					#undef MIR_INT8_TRANS_OFFSET
				}
				else
				{
					/* Generic fallback: call helper function */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_uint_op(ctx, (uint64_t) op)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_agg_helper),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_state),
							MIR_new_reg_op(ctx, r_tmp1)));
				}

				MIR_append_insn(ctx, func_item, end_label);
				break;
			}

			/*
			 * ---- DEFAULT: fallback ----
			 */
			default:
			{
				int fb_jump_target = -1;

				/* Call pg_jitter_fallback_step(state, op, econtext) -> int */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_uint_op(ctx, (uint64_t) op)));
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 6,
						MIR_new_ref_op(ctx, proto_fallback),
						MIR_new_ref_op(ctx, import_fallback),
						MIR_new_reg_op(ctx, r_tmp2),   /* return value */
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));

				/* Check if this opcode could jump */
				switch (opcode)
				{
					case EEOP_AGG_STRICT_DESERIALIZE:
						fb_jump_target = op->d.agg_deserialize.jumpnull;
						break;
					case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
					case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
					case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
						fb_jump_target = op->d.agg_strict_input_check.jumpnull;
						break;
					case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
						fb_jump_target = op->d.agg_plain_pergroup_nullcheck.jumpnull;
						break;
					case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
					case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
						fb_jump_target = op->d.agg_presorted_distinctcheck.jumpdistinct;
						break;
					case EEOP_HASHDATUM_FIRST_STRICT:
					case EEOP_HASHDATUM_NEXT32_STRICT:
						fb_jump_target = op->d.hashdatum.jumpdone;
						break;
					case EEOP_ROWCOMPARE_STEP:
						fb_jump_target = op->d.rowcompare_step.jumpdone;
						break;
					case EEOP_SBSREF_SUBSCRIPTS:
						fb_jump_target = op->d.sbsref_subscript.jumpdone;
						break;
					case EEOP_RETURNINGEXPR:
						fb_jump_target = op->d.returningexpr.jumpdone;
						break;
					default:
						break;
				}

				if (opcode == EEOP_ROWCOMPARE_STEP)
				{
					/*
					 * ROWCOMPARE_STEP has two jump targets:
					 * jumpnull and jumpdone.  Fallback returns the
					 * actual step number, so check against each.
					 */
					int jnull = op->d.rowcompare_step.jumpnull;
					int jdone = op->d.rowcompare_step.jumpdone;

					if (jnull >= 0 && jnull < steps_len)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BEQ,
								MIR_new_label_op(ctx, step_labels[jnull]),
								MIR_new_reg_op(ctx, r_tmp2),
								MIR_new_int_op(ctx, jnull)));
					}
					if (jdone >= 0 && jdone < steps_len)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BEQ,
								MIR_new_label_op(ctx, step_labels[jdone]),
								MIR_new_reg_op(ctx, r_tmp2),
								MIR_new_int_op(ctx, jdone)));
					}
				}
				else if (opcode == EEOP_JSONEXPR_PATH)
				{
					/*
					 * JSONEXPR_PATH always jumps (unconditional).
					 * Compare return value against each possible target.
					 */
					JsonExprState *jsestate = op->d.jsonexpr.jsestate;
					int targets[4];
					int ntargets = 0;

					targets[ntargets++] = jsestate->jump_end;
					if (jsestate->jump_empty >= 0 &&
						jsestate->jump_empty != jsestate->jump_end)
						targets[ntargets++] = jsestate->jump_empty;
					if (jsestate->jump_error >= 0 &&
						jsestate->jump_error != jsestate->jump_end)
						targets[ntargets++] = jsestate->jump_error;
					if (jsestate->jump_eval_coercion >= 0 &&
						jsestate->jump_eval_coercion != jsestate->jump_end)
						targets[ntargets++] = jsestate->jump_eval_coercion;

					for (int t = 0; t < ntargets; t++)
					{
						if (targets[t] >= 0 && targets[t] < steps_len)
						{
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_BEQ,
									MIR_new_label_op(ctx, step_labels[targets[t]]),
									MIR_new_reg_op(ctx, r_tmp2),
									MIR_new_int_op(ctx, targets[t])));
						}
					}
				}
				else if (fb_jump_target >= 0 && fb_jump_target < steps_len)
				{
					/* If r_tmp2 >= 0, jump to target */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BGE,
							MIR_new_label_op(ctx, step_labels[fb_jump_target]),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));
				}
				break;
			}
		}
	}

	/*
	 * Finalize the function and module.
	 */
	MIR_finish_func(ctx);
	MIR_finish_module(ctx);

	/*
	 * Load, link, and generate code.
	 */
	{
		void *code;

		MIR_load_module(ctx, m);

		/* Load external symbols */
		MIR_load_external(ctx, "fallback_step", (void *) pg_jitter_fallback_step);
		MIR_load_external(ctx, "getsomeattrs", (void *) slot_getsomeattrs_int);
		MIR_load_external(ctx, "make_ro", (void *) MakeExpandedObjectReadOnlyInternal);
		/* Inline error handlers */
		MIR_load_external(ctx, "err_i4ov", (void *) jit_error_int4_overflow);
		MIR_load_external(ctx, "err_i8ov", (void *) jit_error_int8_overflow);
		MIR_load_external(ctx, "err_divz", (void *) jit_error_division_by_zero);

	for (int i = 0; i < steps_len; i++)
	{
		if (step_fn_imports[i])
		{
			ExprEvalOp op = ExecEvalStepOp(state, &steps[i]);
			void *addr;
			char name[32];

			if (op == EEOP_HASHDATUM_FIRST ||
				op == EEOP_HASHDATUM_FIRST_STRICT ||
				op == EEOP_HASHDATUM_NEXT32 ||
				op == EEOP_HASHDATUM_NEXT32_STRICT)
				addr = (void *) steps[i].d.hashdatum.fn_addr;
			else
				addr = (void *) steps[i].d.func.fn_addr;

			snprintf(name, sizeof(name), "fn_%d", i);
			MIR_load_external(ctx, name, addr);
		}
		if (step_direct_imports[i])
		{
			ExprEvalOp opc = ExecEvalStepOp(state, &steps[i]);
			char name[32];

			if (opc == EEOP_HASHDATUM_FIRST ||
				opc == EEOP_HASHDATUM_FIRST_STRICT ||
				opc == EEOP_HASHDATUM_NEXT32 ||
				opc == EEOP_HASHDATUM_NEXT32_STRICT)
			{
				const JitDirectFn *dfn = jit_find_direct_fn(steps[i].d.hashdatum.fn_addr);
				snprintf(name, sizeof(name), "dhfn_%d", i);
				if (dfn && dfn->jit_fn)
					MIR_load_external(ctx, name, dfn->jit_fn);
			}
			else if (opc >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
					 opc <= EEOP_AGG_PLAIN_TRANS_BYREF)
			{
				/* Resolve agg_trans helper address */
				void *helper_fn = NULL;
				switch (opc) {
				case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
					helper_fn = (void *) pg_jitter_agg_trans_init_strict_byval; break;
				case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
					helper_fn = (void *) pg_jitter_agg_trans_strict_byval; break;
				case EEOP_AGG_PLAIN_TRANS_BYVAL:
					helper_fn = (void *) pg_jitter_agg_trans_byval; break;
				case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
					helper_fn = (void *) pg_jitter_agg_trans_init_strict_byref; break;
				case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
					helper_fn = (void *) pg_jitter_agg_trans_strict_byref; break;
				case EEOP_AGG_PLAIN_TRANS_BYREF:
					helper_fn = (void *) pg_jitter_agg_trans_byref; break;
				default: break;
				}
				snprintf(name, sizeof(name), "ah_%d", i);
				if (helper_fn)
					MIR_load_external(ctx, name, helper_fn);
			}
			else
			{
				const JitDirectFn *dfn = jit_find_direct_fn(steps[i].d.func.fn_addr);
				snprintf(name, sizeof(name), "dfn_%d", i);
				if (dfn && dfn->jit_fn)
					MIR_load_external(ctx, name, dfn->jit_fn);
			}
		}
	}

		MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);

		code = MIR_gen(ctx, func_item);
		if (!code)
		{
			pfree(step_labels);
			pfree(step_fn_imports);
			pfree(step_direct_imports);
			return false;
		}

		/* Set the eval function (with validation wrapper on first call) */
		pg_jitter_install_expr(state, (ExprStateEvalFunc) code);
	}

	/*
	 * Register a small tracking struct for cleanup accounting.
	 * The generated code lives in the persistent MIR context's code buffer
	 * and doesn't need individual freeing.
	 */
	{
		void *mc = MemoryContextAllocZero(TopMemoryContext, sizeof(void *));
		pg_jitter_register_compiled(jctx, mir_code_free, mc);
	}

	pfree(step_labels);
	pfree(step_fn_imports);
	pfree(step_direct_imports);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(jctx->base.instr.generation_counter,
						  endtime, starttime);
	jctx->base.instr.created_functions++;

	return true;
}
