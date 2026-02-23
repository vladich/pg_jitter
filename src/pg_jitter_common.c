/*
 * pg_jitter_common.c — Shared context management and fallback dispatch
 *
 * This file is compiled into each pg_jitter provider library.
 */
#include "pg_jitter_common.h"

#include "postgres.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "executor/execExpr.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/expandeddatum.h"
#include "access/cmptype.h"
#include "fmgr.h"


/*
 * Resource owner support — follows llvmjit.c pattern.
 */
static void ResOwnerReleasePgJitterContext(Datum res);

static const ResourceOwnerDesc pg_jitter_resowner_desc =
{
	.name = "pg_jitter JIT context",
	.release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
	.release_priority = RELEASE_PRIO_JIT_CONTEXTS,
	.ReleaseResource = ResOwnerReleasePgJitterContext,
	.DebugPrint = NULL
};

static inline void
PgJitterRememberContext(ResourceOwner owner, PgJitterContext *handle)
{
	ResourceOwnerRemember(owner, PointerGetDatum(handle), &pg_jitter_resowner_desc);
}

static inline void
PgJitterForgetContext(ResourceOwner owner, PgJitterContext *handle)
{
	ResourceOwnerForget(owner, PointerGetDatum(handle), &pg_jitter_resowner_desc);
}

static void
ResOwnerReleasePgJitterContext(Datum res)
{
	PgJitterContext *context = (PgJitterContext *) DatumGetPointer(res);

	context->resowner = NULL;
	jit_release_context(&context->base);
}

/*
 * Get or create a PgJitterContext for the current query.
 * Follows the pattern in llvmjit.c:222-246.
 */
PgJitterContext *
pg_jitter_get_context(ExprState *state)
{
	PlanState  *parent = state->parent;
	PgJitterContext *ctx;

	Assert(parent != NULL);

	if (parent->state->es_jit)
		return (PgJitterContext *) parent->state->es_jit;

	ResourceOwnerEnlarge(CurrentResourceOwner);

	ctx = (PgJitterContext *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PgJitterContext));
	ctx->base.flags = parent->state->es_jit_flags;
	ctx->compiled_list = NULL;
	ctx->resowner = CurrentResourceOwner;

	PgJitterRememberContext(CurrentResourceOwner, ctx);

	parent->state->es_jit = &ctx->base;
	return ctx;
}

/*
 * Register compiled code in the context for cleanup on release.
 */
void
pg_jitter_register_compiled(PgJitterContext *ctx,
							void (*free_fn)(void *),
							void *data)
{
	CompiledCode *cc;

	cc = (CompiledCode *)
		MemoryContextAlloc(TopMemoryContext, sizeof(CompiledCode));
	cc->free_fn = free_fn;
	cc->data = data;
	cc->next = ctx->compiled_list;
	ctx->compiled_list = cc;
}

/*
 * Release all compiled code in the context.
 * Note: PG's jit.c calls pfree(context) after this callback, so we must NOT
 * pfree the context itself.
 */
void
pg_jitter_release_context(JitContext *context)
{
	PgJitterContext *ctx = (PgJitterContext *) context;
	CompiledCode *cc, *next;

	for (cc = ctx->compiled_list; cc != NULL; cc = next)
	{
		next = cc->next;
		if (cc->free_fn)
			cc->free_fn(cc->data);
		pfree(cc);
	}
	ctx->compiled_list = NULL;

	if (ctx->resowner)
		PgJitterForgetContext(ctx->resowner, ctx);
}

/*
 * Reset after error — no-op for our backends (no global state to clean up).
 */
void
pg_jitter_reset_after_error(void)
{
	/* Nothing to do — our backends don't maintain global mutable state */
}

/*
 * Fallback step dispatch.
 *
 * For opcodes that the JIT backends don't emit native code for, this
 * function implements them by calling the appropriate ExecEval* function
 * or executing the logic inline (matching the interpreter in execExprInterp.c).
 *
 * The JIT code emits: call pg_jitter_fallback_step(state, &steps[i], econtext)
 * and then handles any jump logic itself (based on op->resvalue/resnull).
 */
int64
pg_jitter_fallback_step(ExprState *state,
						ExprEvalStep *op,
						ExprContext *econtext)
{
	ExprEvalOp	opcode = ExecEvalStepOp(state, op);

	switch (opcode)
	{
		/* System variable access */
		case EEOP_INNER_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_innertuple);
			break;
		case EEOP_OUTER_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_outertuple);
			break;
		case EEOP_SCAN_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_scantuple);
			break;
		case EEOP_OLD_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_oldtuple);
			break;
		case EEOP_NEW_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_newtuple);
			break;

		/* Whole-row variable */
		case EEOP_WHOLEROW:
			ExecEvalWholeRowVar(state, op, econtext);
			break;

		/* Function calls with fusage tracking */
		case EEOP_FUNCEXPR_FUSAGE:
			ExecEvalFuncExprFusage(state, op, econtext);
			break;
		case EEOP_FUNCEXPR_STRICT_FUSAGE:
			ExecEvalFuncExprStrictFusage(state, op, econtext);
			break;

		/* Row null tests */
		case EEOP_NULLTEST_ROWISNULL:
			ExecEvalRowNull(state, op, econtext);
			break;
		case EEOP_NULLTEST_ROWISNOTNULL:
			ExecEvalRowNotNull(state, op, econtext);
			break;

		/* Boolean tests — inline implementation matching interpreter */
		case EEOP_BOOLTEST_IS_TRUE:
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			break;
		case EEOP_BOOLTEST_IS_NOT_TRUE:
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));
			break;
		case EEOP_BOOLTEST_IS_FALSE:
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));
			break;
		case EEOP_BOOLTEST_IS_NOT_FALSE:
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			break;

		/* Parameters */
		case EEOP_PARAM_EXEC:
			ExecEvalParamExec(state, op, econtext);
			break;
		case EEOP_PARAM_EXTERN:
			ExecEvalParamExtern(state, op, econtext);
			break;
		case EEOP_PARAM_CALLBACK:
			op->d.cparam.paramfunc(state, op, econtext);
			break;
		case EEOP_PARAM_SET:
			ExecEvalParamSet(state, op, econtext);
			break;

		/* Case test values */
		case EEOP_CASE_TESTVAL:
			*op->resvalue = *op->d.casetest.value;
			*op->resnull = *op->d.casetest.isnull;
			break;
		case EEOP_CASE_TESTVAL_EXT:
			*op->resvalue = econtext->caseValue_datum;
			*op->resnull = econtext->caseValue_isNull;
			break;

		/* Make expanded object read-only */
		case EEOP_MAKE_READONLY:
			if (!*op->d.make_readonly.isnull)
				*op->resvalue =
					MakeExpandedObjectReadOnlyInternal(*op->d.make_readonly.value);
			*op->resnull = *op->d.make_readonly.isnull;
			break;

		/* IO coercion */
		case EEOP_IOCOERCE:
		{
			FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
			FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;
			char	   *str;

			if (*op->resnull)
				break;

			fcinfo_out->args[0].value = *op->resvalue;
			fcinfo_out->args[0].isnull = false;
			fcinfo_out->isnull = false;
			str = DatumGetCString(FunctionCallInvoke(fcinfo_out));

			fcinfo_in->args[0].value = CStringGetDatum(str);
			fcinfo_in->args[0].isnull = false;
			fcinfo_in->isnull = false;
			*op->resvalue = FunctionCallInvoke(fcinfo_in);
			*op->resnull = fcinfo_in->isnull;
			break;
		}
		case EEOP_IOCOERCE_SAFE:
			ExecEvalCoerceViaIOSafe(state, op);
			break;

		/* Distinct / not distinct / nullif */
		case EEOP_DISTINCT:
		case EEOP_NOT_DISTINCT:
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			bool		arg_null0 = fcinfo->args[0].isnull;
			bool		arg_null1 = fcinfo->args[1].isnull;

			if (arg_null0 != arg_null1)
			{
				*op->resvalue = BoolGetDatum(opcode == EEOP_DISTINCT);
				*op->resnull = false;
			}
			else if (arg_null0)
			{
				*op->resvalue = BoolGetDatum(opcode != EEOP_DISTINCT);
				*op->resnull = false;
			}
		else
			{
				fcinfo->isnull = false;
				Datum		result = op->d.func.fn_addr(fcinfo);

				*op->resnull = false;
				if (opcode == EEOP_DISTINCT)
					*op->resvalue = BoolGetDatum(!DatumGetBool(result));
				else
					*op->resvalue = result;
			}
			break;
		}
		case EEOP_NULLIF:
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			if (fcinfo->args[0].isnull)
			{
				*op->resnull = true;
				break;
			}
			if (!fcinfo->args[1].isnull)
			{
				Datum		result;

				fcinfo->isnull = false;
				result = op->d.func.fn_addr(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(result))
				{
					*op->resnull = true;
					break;
				}
			}
			*op->resvalue = fcinfo->args[0].value;
			*op->resnull = false;
			break;
		}

		case EEOP_SQLVALUEFUNCTION:
			ExecEvalSQLValueFunction(state, op);
			break;
		case EEOP_CURRENTOFEXPR:
			ExecEvalCurrentOfExpr(state, op);
			break;
		case EEOP_NEXTVALUEEXPR:
			ExecEvalNextValueExpr(state, op);
			break;

		case EEOP_RETURNINGEXPR:
			if (state->flags & op->d.returningexpr.nullflag)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
				return op->d.returningexpr.jumpdone;
			}
			break;

		/* Arrays and rows */
		case EEOP_ARRAYEXPR:
			ExecEvalArrayExpr(state, op);
			break;
		case EEOP_ARRAYCOERCE:
			ExecEvalArrayCoerce(state, op, econtext);
			break;
		case EEOP_ROW:
			ExecEvalRow(state, op);
			break;

		/* Row comparison */
		case EEOP_ROWCOMPARE_STEP:
		{
			FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
			Datum		d;

			fcinfo->isnull = false;
			d = op->d.rowcompare_step.fn_addr(fcinfo);

			if (fcinfo->isnull)
			{
				*op->resnull = true;
				return op->d.rowcompare_step.jumpnull;
			}
			else
			{
				int32		cmpresult = DatumGetInt32(d);

				if (cmpresult != 0)
				{
					*op->resvalue = Int32GetDatum(cmpresult);
					*op->resnull = false;
					return op->d.rowcompare_step.jumpdone;
				}
			}
			break;
		}
		case EEOP_ROWCOMPARE_FINAL:
		{
			int32		cmpresult = DatumGetInt32(*op->resvalue);
			CompareType cmptype = op->d.rowcompare_final.cmptype;

			switch (cmptype)
			{
				case COMPARE_LT:
					*op->resvalue = BoolGetDatum(cmpresult < 0);
					break;
				case COMPARE_LE:
					*op->resvalue = BoolGetDatum(cmpresult <= 0);
					break;
				case COMPARE_GE:
					*op->resvalue = BoolGetDatum(cmpresult >= 0);
					break;
				case COMPARE_GT:
					*op->resvalue = BoolGetDatum(cmpresult > 0);
					break;
				default:
					elog(ERROR, "unexpected compare type: %d", (int) cmptype);
					break;
			}
			*op->resnull = false;
			break;
		}

		case EEOP_MINMAX:
			ExecEvalMinMax(state, op);
			break;
		case EEOP_FIELDSELECT:
			ExecEvalFieldSelect(state, op, econtext);
			break;
		case EEOP_FIELDSTORE_DEFORM:
			ExecEvalFieldStoreDeForm(state, op, econtext);
			break;
		case EEOP_FIELDSTORE_FORM:
			ExecEvalFieldStoreForm(state, op, econtext);
			break;

		/* Subscript reference */
		case EEOP_SBSREF_SUBSCRIPTS:
			if (!op->d.sbsref_subscript.subscriptfunc(state, op, econtext))
				return op->d.sbsref_subscript.jumpdone;
			break;
		case EEOP_SBSREF_OLD:
		case EEOP_SBSREF_ASSIGN:
		case EEOP_SBSREF_FETCH:
			op->d.sbsref.subscriptfunc(state, op, econtext);
			break;

		/* Domain checks — DOMAIN_TESTVAL uses casetest union member */
		case EEOP_DOMAIN_TESTVAL:
			*op->resvalue = *op->d.casetest.value;
			*op->resnull = *op->d.casetest.isnull;
			break;
		case EEOP_DOMAIN_TESTVAL_EXT:
			*op->resvalue = econtext->domainValue_datum;
			*op->resnull = econtext->domainValue_isNull;
			break;
		case EEOP_DOMAIN_NOTNULL:
			ExecEvalConstraintNotNull(state, op);
			break;
		case EEOP_DOMAIN_CHECK:
			ExecEvalConstraintCheck(state, op);
			break;

		/* Hash datum */
		case EEOP_HASHDATUM_SET_INITVAL:
			*op->resvalue = op->d.hashdatum_initvalue.init_value;
			*op->resnull = false;
			break;
		case EEOP_HASHDATUM_FIRST:
		case EEOP_HASHDATUM_FIRST_STRICT:
		case EEOP_HASHDATUM_NEXT32:
		case EEOP_HASHDATUM_NEXT32_STRICT:
		{
			FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
			Datum		hashval;

			if (opcode == EEOP_HASHDATUM_FIRST_STRICT ||
				opcode == EEOP_HASHDATUM_NEXT32_STRICT)
			{
				if (fcinfo->args[0].isnull)
				{
					*op->resnull = true;
					*op->resvalue = (Datum) 0;
					return op->d.hashdatum.jumpdone;
				}
			}

			if (opcode == EEOP_HASHDATUM_FIRST ||
				opcode == EEOP_HASHDATUM_FIRST_STRICT)
			{
				if (!fcinfo->args[0].isnull)
				{
					fcinfo->isnull = false;
					*op->resvalue = op->d.hashdatum.fn_addr(fcinfo);
				}
				else
					*op->resvalue = (Datum) 0;
			}
			else
			{
				/* NEXT32 variants: read from iresult, not *op->resvalue */
				uint32		existing = DatumGetUInt32(op->d.hashdatum.iresult->value);

				existing = pg_rotate_left32(existing, 1);

				if (!fcinfo->args[0].isnull)
				{
					fcinfo->isnull = false;
					hashval = op->d.hashdatum.fn_addr(fcinfo);
					existing ^= DatumGetUInt32(hashval);
				}
				*op->resvalue = UInt32GetDatum(existing);
			}
			*op->resnull = false;
			break;
		}

		/* Type conversion */
		case EEOP_CONVERT_ROWTYPE:
			ExecEvalConvertRowtype(state, op, econtext);
			break;

		/* Scalar array op */
		case EEOP_SCALARARRAYOP:
			ExecEvalScalarArrayOp(state, op);
			break;
		case EEOP_HASHED_SCALARARRAYOP:
			ExecEvalHashedScalarArrayOp(state, op, econtext);
			break;

		/* XML */
		case EEOP_XMLEXPR:
			ExecEvalXmlExpr(state, op);
			break;

		/* JSON */
		case EEOP_JSON_CONSTRUCTOR:
			ExecEvalJsonConstructor(state, op, econtext);
			break;
		case EEOP_IS_JSON:
			ExecEvalJsonIsPredicate(state, op);
			break;
		case EEOP_JSONEXPR_PATH:
			ExecEvalJsonExprPath(state, op, econtext);
			break;
		case EEOP_JSONEXPR_COERCION:
			ExecEvalJsonCoercion(state, op, econtext);
			break;
		case EEOP_JSONEXPR_COERCION_FINISH:
			ExecEvalJsonCoercionFinish(state, op);
			break;

		/* Aggregate references */
		case EEOP_AGGREF:
		{
			int			aggno = op->d.aggref.aggno;

			*op->resvalue = econtext->ecxt_aggvalues[aggno];
			*op->resnull = econtext->ecxt_aggnulls[aggno];
			break;
		}
		case EEOP_GROUPING_FUNC:
			ExecEvalGroupingFunc(state, op);
			break;
		case EEOP_WINDOW_FUNC:
		{
			WindowFuncExprState *wfunc = op->d.window_func.wfstate;

			*op->resvalue = econtext->ecxt_aggvalues[wfunc->wfuncno];
			*op->resnull = econtext->ecxt_aggnulls[wfunc->wfuncno];
			break;
		}
		case EEOP_MERGE_SUPPORT_FUNC:
			ExecEvalMergeSupportFunc(state, op, econtext);
			break;
		case EEOP_SUBPLAN:
			ExecEvalSubPlan(state, op, econtext);
			break;

		/* Aggregate deserialization */
		case EEOP_AGG_STRICT_DESERIALIZE:
		case EEOP_AGG_DESERIALIZE:
		{
			FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;

			if (opcode == EEOP_AGG_STRICT_DESERIALIZE &&
				fcinfo->args[0].isnull)
				return op->d.agg_deserialize.jumpnull;

			fcinfo->isnull = false;
			*op->resvalue = FunctionCallInvoke(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		}

		case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
		case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
		{
			NullableDatum *args = op->d.agg_strict_input_check.args;
			int nargs = op->d.agg_strict_input_check.nargs;

			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
					return op->d.agg_strict_input_check.jumpnull;
			}
			break;
		}

		case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
		{
			bool *nulls = op->d.agg_strict_input_check.nulls;
			int nargs = op->d.agg_strict_input_check.nargs;

			for (int argno = 0; argno < nargs; argno++)
			{
				if (nulls[argno])
					return op->d.agg_strict_input_check.jumpnull;
			}
			break;
		}

		case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerGroup pergroup_allaggs =
				aggstate->all_pergroups[op->d.agg_plain_pergroup_nullcheck.setoff];

			if (pergroup_allaggs == NULL)
				return op->d.agg_plain_pergroup_nullcheck.jumpnull;
			break;
		}

		case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			if (pergroup->noTransValue)
			{
				ExecAggInitGroup(aggstate, pertrans, pergroup,
								 op->d.agg_trans.aggcontext);
			}
			else if (likely(!pergroup->transValueIsNull))
			{
				/* Inline ExecAggPlainTransByVal */
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				MemoryContext oldContext;
				Datum newVal;

				aggstate->curaggcontext = op->d.agg_trans.aggcontext;
				aggstate->current_set = op->d.agg_trans.setno;
				aggstate->curpertrans = pertrans;

				oldContext = MemoryContextSwitchTo(
					aggstate->tmpcontext->ecxt_per_tuple_memory);
				fcinfo->args[0].value = pergroup->transValue;
				fcinfo->args[0].isnull = pergroup->transValueIsNull;
				fcinfo->isnull = false;
				newVal = FunctionCallInvoke(fcinfo);
				pergroup->transValue = newVal;
				pergroup->transValueIsNull = fcinfo->isnull;
				MemoryContextSwitchTo(oldContext);
			}
			break;
		}

		case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			if (likely(!pergroup->transValueIsNull))
			{
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				MemoryContext oldContext;
				Datum newVal;

				aggstate->curaggcontext = op->d.agg_trans.aggcontext;
				aggstate->current_set = op->d.agg_trans.setno;
				aggstate->curpertrans = pertrans;

				oldContext = MemoryContextSwitchTo(
					aggstate->tmpcontext->ecxt_per_tuple_memory);
				fcinfo->args[0].value = pergroup->transValue;
				fcinfo->args[0].isnull = pergroup->transValueIsNull;
				fcinfo->isnull = false;
				newVal = FunctionCallInvoke(fcinfo);
				pergroup->transValue = newVal;
				pergroup->transValueIsNull = fcinfo->isnull;
				MemoryContextSwitchTo(oldContext);
			}
			break;
		}

		case EEOP_AGG_PLAIN_TRANS_BYVAL:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
			MemoryContext oldContext;
			Datum newVal;

			aggstate->curaggcontext = op->d.agg_trans.aggcontext;
			aggstate->current_set = op->d.agg_trans.setno;
			aggstate->curpertrans = pertrans;

			oldContext = MemoryContextSwitchTo(
				aggstate->tmpcontext->ecxt_per_tuple_memory);
			fcinfo->args[0].value = pergroup->transValue;
			fcinfo->args[0].isnull = pergroup->transValueIsNull;
			fcinfo->isnull = false;
			newVal = FunctionCallInvoke(fcinfo);
			pergroup->transValue = newVal;
			pergroup->transValueIsNull = fcinfo->isnull;
			MemoryContextSwitchTo(oldContext);
			break;
		}

		case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			if (pergroup->noTransValue)
			{
				ExecAggInitGroup(aggstate, pertrans, pergroup,
								 op->d.agg_trans.aggcontext);
			}
			else if (likely(!pergroup->transValueIsNull))
			{
				/* Inline ExecAggPlainTransByRef */
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				MemoryContext oldContext;
				Datum newVal;

				aggstate->curaggcontext = op->d.agg_trans.aggcontext;
				aggstate->current_set = op->d.agg_trans.setno;
				aggstate->curpertrans = pertrans;

				oldContext = MemoryContextSwitchTo(
					aggstate->tmpcontext->ecxt_per_tuple_memory);
				fcinfo->args[0].value = pergroup->transValue;
				fcinfo->args[0].isnull = pergroup->transValueIsNull;
				fcinfo->isnull = false;
				newVal = FunctionCallInvoke(fcinfo);
				if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
					newVal = ExecAggCopyTransValue(aggstate, pertrans,
												   newVal, fcinfo->isnull,
												   pergroup->transValue,
												   pergroup->transValueIsNull);
				pergroup->transValue = newVal;
				pergroup->transValueIsNull = fcinfo->isnull;
				MemoryContextSwitchTo(oldContext);
			}
			break;
		}

		case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			if (likely(!pergroup->transValueIsNull))
			{
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				MemoryContext oldContext;
				Datum newVal;

				aggstate->curaggcontext = op->d.agg_trans.aggcontext;
				aggstate->current_set = op->d.agg_trans.setno;
				aggstate->curpertrans = pertrans;

				oldContext = MemoryContextSwitchTo(
					aggstate->tmpcontext->ecxt_per_tuple_memory);
				fcinfo->args[0].value = pergroup->transValue;
				fcinfo->args[0].isnull = pergroup->transValueIsNull;
				fcinfo->isnull = false;
				newVal = FunctionCallInvoke(fcinfo);
				if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
					newVal = ExecAggCopyTransValue(aggstate, pertrans,
												   newVal, fcinfo->isnull,
												   pergroup->transValue,
												   pergroup->transValueIsNull);
				pergroup->transValue = newVal;
				pergroup->transValueIsNull = fcinfo->isnull;
				MemoryContextSwitchTo(oldContext);
			}
			break;
		}

		case EEOP_AGG_PLAIN_TRANS_BYREF:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
			MemoryContext oldContext;
			Datum newVal;

			aggstate->curaggcontext = op->d.agg_trans.aggcontext;
			aggstate->current_set = op->d.agg_trans.setno;
			aggstate->curpertrans = pertrans;

			oldContext = MemoryContextSwitchTo(
				aggstate->tmpcontext->ecxt_per_tuple_memory);
			fcinfo->args[0].value = pergroup->transValue;
			fcinfo->args[0].isnull = pergroup->transValueIsNull;
			fcinfo->isnull = false;
			newVal = FunctionCallInvoke(fcinfo);
			if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
				newVal = ExecAggCopyTransValue(aggstate, pertrans,
											   newVal, fcinfo->isnull,
											   pergroup->transValue,
											   pergroup->transValueIsNull);
			pergroup->transValue = newVal;
			pergroup->transValueIsNull = fcinfo->isnull;
			MemoryContextSwitchTo(oldContext);
			break;
		}

		case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;

			if (!ExecEvalPreOrderedDistinctSingle(aggstate, pertrans))
				return op->d.agg_presorted_distinctcheck.jumpdistinct;
			break;
		}

		case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
		{
			AggState *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;

			if (!ExecEvalPreOrderedDistinctMulti(aggstate, pertrans))
				return op->d.agg_presorted_distinctcheck.jumpdistinct;
			break;
		}

		case EEOP_AGG_ORDERED_TRANS_DATUM:
			ExecEvalAggOrderedTransDatum(state, op, econtext);
			break;
		case EEOP_AGG_ORDERED_TRANS_TUPLE:
			ExecEvalAggOrderedTransTuple(state, op, econtext);
			break;

		/*
		 * Hot-path opcodes — normally handled natively by JIT backends,
		 * but included here as safety net for debugging/fallback.
		 */
		case EEOP_DONE_RETURN:
		case EEOP_DONE_NO_RETURN:
			/* Should not reach fallback — these are always native */
			elog(ERROR, "pg_jitter: DONE opcode in fallback");
			break;

		case EEOP_INNER_FETCHSOME:
		case EEOP_OUTER_FETCHSOME:
		case EEOP_SCAN_FETCHSOME:
		case EEOP_OLD_FETCHSOME:
		case EEOP_NEW_FETCHSOME:
		{
			TupleTableSlot *slot;

			switch (opcode)
			{
				case EEOP_INNER_FETCHSOME:
					slot = econtext->ecxt_innertuple; break;
				case EEOP_OUTER_FETCHSOME:
					slot = econtext->ecxt_outertuple; break;
				case EEOP_SCAN_FETCHSOME:
					slot = econtext->ecxt_scantuple; break;
				case EEOP_OLD_FETCHSOME:
					slot = econtext->ecxt_oldtuple; break;
				case EEOP_NEW_FETCHSOME:
					slot = econtext->ecxt_newtuple; break;
				default:
					slot = econtext->ecxt_scantuple; break;
			}
			if (slot->tts_nvalid < op->d.fetch.last_var)
				slot_getsomeattrs_int(slot, op->d.fetch.last_var);
			break;
		}

		case EEOP_INNER_VAR:
		case EEOP_OUTER_VAR:
		case EEOP_SCAN_VAR:
		case EEOP_OLD_VAR:
		case EEOP_NEW_VAR:
		{
			TupleTableSlot *slot;
			int attnum = op->d.var.attnum;

			switch (opcode)
			{
				case EEOP_INNER_VAR:
					slot = econtext->ecxt_innertuple; break;
				case EEOP_OUTER_VAR:
					slot = econtext->ecxt_outertuple; break;
				case EEOP_SCAN_VAR:
					slot = econtext->ecxt_scantuple; break;
				case EEOP_OLD_VAR:
					slot = econtext->ecxt_oldtuple; break;
				case EEOP_NEW_VAR:
					slot = econtext->ecxt_newtuple; break;
				default:
					slot = econtext->ecxt_scantuple; break;
			}
			*op->resvalue = slot->tts_values[attnum];
			*op->resnull = slot->tts_isnull[attnum];
			break;
		}

		case EEOP_ASSIGN_INNER_VAR:
		case EEOP_ASSIGN_OUTER_VAR:
		case EEOP_ASSIGN_SCAN_VAR:
		case EEOP_ASSIGN_OLD_VAR:
		case EEOP_ASSIGN_NEW_VAR:
		{
			TupleTableSlot *srcslot;
			TupleTableSlot *resultslot = state->resultslot;
			int attnum = op->d.assign_var.attnum;
			int resultnum = op->d.assign_var.resultnum;

			switch (opcode)
			{
				case EEOP_ASSIGN_INNER_VAR:
					srcslot = econtext->ecxt_innertuple; break;
				case EEOP_ASSIGN_OUTER_VAR:
					srcslot = econtext->ecxt_outertuple; break;
				case EEOP_ASSIGN_SCAN_VAR:
					srcslot = econtext->ecxt_scantuple; break;
				case EEOP_ASSIGN_OLD_VAR:
					srcslot = econtext->ecxt_oldtuple; break;
				case EEOP_ASSIGN_NEW_VAR:
					srcslot = econtext->ecxt_newtuple; break;
				default:
					srcslot = econtext->ecxt_scantuple; break;
			}
			resultslot->tts_values[resultnum] = srcslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = srcslot->tts_isnull[attnum];
			break;
		}

		case EEOP_ASSIGN_TMP:
		case EEOP_ASSIGN_TMP_MAKE_RO:
		{
			TupleTableSlot *resultslot = state->resultslot;
			int resultnum = op->d.assign_tmp.resultnum;

			if (opcode == EEOP_ASSIGN_TMP_MAKE_RO && !state->resnull)
				state->resvalue = MakeExpandedObjectReadOnlyInternal(state->resvalue);

			resultslot->tts_values[resultnum] = state->resvalue;
			resultslot->tts_isnull[resultnum] = state->resnull;
			break;
		}

		case EEOP_CONST:
			*op->resvalue = op->d.constval.value;
			*op->resnull = op->d.constval.isnull;
			break;

		case EEOP_FUNCEXPR:
		case EEOP_FUNCEXPR_STRICT:
		case EEOP_FUNCEXPR_STRICT_1:
		case EEOP_FUNCEXPR_STRICT_2:
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			int nargs = op->d.func.nargs;
			bool strictfail = false;

			if (opcode != EEOP_FUNCEXPR)
			{
				for (int argno = 0; argno < nargs; argno++)
				{
					if (fcinfo->args[argno].isnull)
					{
						*op->resnull = true;
						strictfail = true;
						break;
					}
				}
			}
			if (!strictfail)
			{
				fcinfo->isnull = false;
				*op->resvalue = op->d.func.fn_addr(fcinfo);
				*op->resnull = fcinfo->isnull;
			}
			break;
		}

		case EEOP_BOOL_AND_STEP_FIRST:
		case EEOP_BOOL_AND_STEP:
		case EEOP_BOOL_AND_STEP_LAST:
		{
			if (opcode == EEOP_BOOL_AND_STEP_FIRST)
				*op->d.boolexpr.anynull = false;

			if (*op->resnull)
				*op->d.boolexpr.anynull = true;
			else if (!DatumGetBool(*op->resvalue))
			{
				/* false result, short circuit — JIT handles jump */
			}

			if (opcode == EEOP_BOOL_AND_STEP_LAST && *op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			break;
		}

		case EEOP_BOOL_OR_STEP_FIRST:
		case EEOP_BOOL_OR_STEP:
		case EEOP_BOOL_OR_STEP_LAST:
		{
			if (opcode == EEOP_BOOL_OR_STEP_FIRST)
				*op->d.boolexpr.anynull = false;

			if (*op->resnull)
				*op->d.boolexpr.anynull = true;
			else if (DatumGetBool(*op->resvalue))
			{
				/* true result, short circuit — JIT handles jump */
			}

			if (opcode == EEOP_BOOL_OR_STEP_LAST && *op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			break;
		}

		case EEOP_BOOL_NOT_STEP:
			*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));
			break;

		case EEOP_QUAL:
			if (*op->resnull || !DatumGetBool(*op->resvalue))
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = false;
				/* JIT handles jump to jumpdone */
			}
			break;

		case EEOP_JUMP:
		case EEOP_JUMP_IF_NULL:
		case EEOP_JUMP_IF_NOT_NULL:
		case EEOP_JUMP_IF_NOT_TRUE:
			/* JIT handles the actual jump — fallback just evaluates the condition */
			break;

		case EEOP_NULLTEST_ISNULL:
			*op->resvalue = BoolGetDatum(*op->resnull);
			*op->resnull = false;
			break;

		case EEOP_NULLTEST_ISNOTNULL:
			*op->resvalue = BoolGetDatum(!*op->resnull);
			*op->resnull = false;
			break;

		default:
			elog(ERROR, "pg_jitter: unhandled opcode %d in fallback dispatch",
				 (int) opcode);
			break;
	}

	return -1;
}

/*
 * Direct aggregate transition helpers.
 *
 * These replicate the interpreter's inline logic for aggregate transitions,
 * callable directly from JIT code without fallback_step switch overhead.
 * ExecAggPlainTransByVal/ByRef are static in the interpreter, so we inline.
 */

void
pg_jitter_agg_trans_init_strict_byval(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	AggStatePerGroup pergroup =
		&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

	if (pergroup->noTransValue)
	{
		ExecAggInitGroup(aggstate, pertrans, pergroup,
						 op->d.agg_trans.aggcontext);
	}
	else if (likely(!pergroup->transValueIsNull))
	{
		FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
		MemoryContext oldContext;

		aggstate->curaggcontext = op->d.agg_trans.aggcontext;
		aggstate->current_set = op->d.agg_trans.setno;
		aggstate->curpertrans = pertrans;
		oldContext = MemoryContextSwitchTo(
			aggstate->tmpcontext->ecxt_per_tuple_memory);
		fcinfo->args[0].value = pergroup->transValue;
		fcinfo->args[0].isnull = pergroup->transValueIsNull;
		fcinfo->isnull = false;
		pergroup->transValue = FunctionCallInvoke(fcinfo);
		pergroup->transValueIsNull = fcinfo->isnull;
		MemoryContextSwitchTo(oldContext);
	}
}

void
pg_jitter_agg_trans_strict_byval(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	AggStatePerGroup pergroup =
		&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

	if (likely(!pergroup->transValueIsNull))
	{
		FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
		MemoryContext oldContext;

		aggstate->curaggcontext = op->d.agg_trans.aggcontext;
		aggstate->current_set = op->d.agg_trans.setno;
		aggstate->curpertrans = pertrans;
		oldContext = MemoryContextSwitchTo(
			aggstate->tmpcontext->ecxt_per_tuple_memory);
		fcinfo->args[0].value = pergroup->transValue;
		fcinfo->args[0].isnull = pergroup->transValueIsNull;
		fcinfo->isnull = false;
		pergroup->transValue = FunctionCallInvoke(fcinfo);
		pergroup->transValueIsNull = fcinfo->isnull;
		MemoryContextSwitchTo(oldContext);
	}
}

void
pg_jitter_agg_trans_byval(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	AggStatePerGroup pergroup =
		&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;

	aggstate->curaggcontext = op->d.agg_trans.aggcontext;
	aggstate->current_set = op->d.agg_trans.setno;
	aggstate->curpertrans = pertrans;
	oldContext = MemoryContextSwitchTo(
		aggstate->tmpcontext->ecxt_per_tuple_memory);
	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;
	pergroup->transValue = FunctionCallInvoke(fcinfo);
	pergroup->transValueIsNull = fcinfo->isnull;
	MemoryContextSwitchTo(oldContext);
}

void
pg_jitter_agg_trans_init_strict_byref(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	AggStatePerGroup pergroup =
		&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

	if (pergroup->noTransValue)
	{
		ExecAggInitGroup(aggstate, pertrans, pergroup,
						 op->d.agg_trans.aggcontext);
	}
	else if (likely(!pergroup->transValueIsNull))
	{
		FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
		MemoryContext oldContext;
		Datum newVal;

		aggstate->curaggcontext = op->d.agg_trans.aggcontext;
		aggstate->current_set = op->d.agg_trans.setno;
		aggstate->curpertrans = pertrans;
		oldContext = MemoryContextSwitchTo(
			aggstate->tmpcontext->ecxt_per_tuple_memory);
		fcinfo->args[0].value = pergroup->transValue;
		fcinfo->args[0].isnull = pergroup->transValueIsNull;
		fcinfo->isnull = false;
		newVal = FunctionCallInvoke(fcinfo);
		if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
			newVal = ExecAggCopyTransValue(aggstate, pertrans,
										   newVal, fcinfo->isnull,
										   pergroup->transValue,
										   pergroup->transValueIsNull);
		pergroup->transValue = newVal;
		pergroup->transValueIsNull = fcinfo->isnull;
		MemoryContextSwitchTo(oldContext);
	}
}

void
pg_jitter_agg_trans_strict_byref(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	AggStatePerGroup pergroup =
		&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

	if (likely(!pergroup->transValueIsNull))
	{
		FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
		MemoryContext oldContext;
		Datum newVal;

		aggstate->curaggcontext = op->d.agg_trans.aggcontext;
		aggstate->current_set = op->d.agg_trans.setno;
		aggstate->curpertrans = pertrans;
		oldContext = MemoryContextSwitchTo(
			aggstate->tmpcontext->ecxt_per_tuple_memory);
		fcinfo->args[0].value = pergroup->transValue;
		fcinfo->args[0].isnull = pergroup->transValueIsNull;
		fcinfo->isnull = false;
		newVal = FunctionCallInvoke(fcinfo);
		if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
			newVal = ExecAggCopyTransValue(aggstate, pertrans,
										   newVal, fcinfo->isnull,
										   pergroup->transValue,
										   pergroup->transValueIsNull);
		pergroup->transValue = newVal;
		pergroup->transValueIsNull = fcinfo->isnull;
		MemoryContextSwitchTo(oldContext);
	}
}

void
pg_jitter_agg_trans_byref(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	AggStatePerGroup pergroup =
		&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum newVal;

	aggstate->curaggcontext = op->d.agg_trans.aggcontext;
	aggstate->current_set = op->d.agg_trans.setno;
	aggstate->curpertrans = pertrans;
	oldContext = MemoryContextSwitchTo(
		aggstate->tmpcontext->ecxt_per_tuple_memory);
	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;
	newVal = FunctionCallInvoke(fcinfo);
	if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
		newVal = ExecAggCopyTransValue(aggstate, pertrans,
									   newVal, fcinfo->isnull,
									   pergroup->transValue,
									   pergroup->transValueIsNull);
	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;
	MemoryContextSwitchTo(oldContext);
}

/*
 * BYREF aggregate finish helper for inline JIT code.
 *
 * Called by the sljit inline aggregate code after the transition function
 * returns for BYREF variants.  Handles the pointer comparison and calls
 * ExecAggCopyTransValue if needed, then stores the result to pergroup.
 * This wrapper exists because ExecAggCopyTransValue takes 6 args, which
 * exceeds sljit's 4-arg icall limit.
 */
void
pg_jitter_agg_byref_finish(AggState *aggstate,
							AggStatePerTrans pertrans,
							Datum newVal,
							AggStatePerGroup pergroup)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;

	if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
		newVal = ExecAggCopyTransValue(aggstate, pertrans,
									   newVal, fcinfo->isnull,
									   pergroup->transValue,
									   pergroup->transValueIsNull);
	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;
}


/*
 * Validation wrapper for JIT-compiled expressions.
 *
 * On first call, CheckExprStillValid() verifies that slot types still match
 * the compiled code's assumptions (catches ALTER COLUMN TYPE after JIT
 * compilation).  After validation passes, the wrapper replaces itself with
 * the actual compiled code for zero overhead on subsequent calls.
 *
 * This mirrors llvmjit's ExecRunCompiledExpr pattern.
 */
static Datum
pg_jitter_run_compiled_expr(ExprState *state, ExprContext *econtext, bool *isNull)
{
	ExprStateEvalFunc func = (ExprStateEvalFunc) state->evalfunc_private;

	CheckExprStillValid(state, econtext);

	state->evalfunc = func;

	return func(state, econtext, isNull);
}

void
pg_jitter_install_expr(ExprState *state, ExprStateEvalFunc compiled_func)
{
	state->evalfunc = pg_jitter_run_compiled_expr;
	state->evalfunc_private = (void *) compiled_func;
}
