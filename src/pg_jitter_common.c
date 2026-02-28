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
#include "utils/guc.h"
#include "fmgr.h"

#include <stdlib.h>  /* for atoi */

#ifdef __linux__
#include <unistd.h>  /* for sysconf */
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/* GUC: pg_jitter.parallel_mode — shared across backends */
int pg_jitter_parallel_mode = 2;	/* PARALLEL_JIT_SHARED */

/* GUC: pg_jitter.shared_code_max — cap in KB */
int pg_jitter_shared_code_max_kb = 4096;	/* 4 MB default */

/*
 * Read the current pg_jitter.parallel_mode from PG's GUC system.
 *
 * This is needed because when backends are loaded via the meta module,
 * the GUC variable pointer may belong to a different dylib's copy of
 * pg_jitter_parallel_mode.  Reading from the GUC system ensures we
 * always get the current value regardless of which module defined it.
 */
int
pg_jitter_get_parallel_mode(void)
{
	const char *val = GetConfigOption("pg_jitter.parallel_mode", true, false);

	if (val == NULL)
		return pg_jitter_parallel_mode;		/* fallback to local default */

	if (strcmp(val, "off") == 0)
		return PARALLEL_JIT_OFF;
	else if (strcmp(val, "per_worker") == 0)
		return PARALLEL_JIT_PER_WORKER;
	else if (strcmp(val, "shared") == 0)
		return PARALLEL_JIT_SHARED;

	return pg_jitter_parallel_mode;		/* unknown value, use local */
}

/*
 * Read pg_jitter.shared_code_max from the GUC system (in KB).
 */
static int
pg_jitter_get_shared_code_max_kb(void)
{
	const char *val = GetConfigOption("pg_jitter.shared_code_max", true, false);

	if (val != NULL)
		return atoi(val);

	return pg_jitter_shared_code_max_kb;
}

#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif



/*
 * Resource owner support — follows llvmjit.c pattern.
 *
 * PG17+ uses the generic ResourceOwnerDesc API.
 * PG14-16 use the JIT-specific ResourceOwnerEnlargeJIT/RememberJIT/ForgetJIT.
 */
#if PG_VERSION_NUM >= 170000

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

#else /* PG14-16: JIT-specific resource owner API */

#include "utils/resowner_private.h"

static inline void
PgJitterRememberContext(ResourceOwner owner, PgJitterContext *handle)
{
	ResourceOwnerRememberJIT(owner, PointerGetDatum(handle));
}

static inline void
PgJitterForgetContext(ResourceOwner owner, PgJitterContext *handle)
{
	ResourceOwnerForgetJIT(owner, PointerGetDatum(handle));
}

#endif /* PG_VERSION_NUM >= 170000 */

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

#if PG_VERSION_NUM >= 170000
	ResourceOwnerEnlarge(CurrentResourceOwner);
#else
	ResourceOwnerEnlargeJIT(CurrentResourceOwner);
#endif

	ctx = (PgJitterContext *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(PgJitterContext));
	ctx->base.flags = parent->state->es_jit_flags;
#if PG_VERSION_NUM < 170000
	/*
	 * PG14-16: JitContext has a resowner field that PG's jit_release_context()
	 * reads to call ResourceOwnerForgetJIT.  Must be set or release crashes.
	 */
	ctx->base.resowner = CurrentResourceOwner;
#endif
	ctx->compiled_list = NULL;
	ctx->resowner = CurrentResourceOwner;
	ctx->last_plan_node_id = -1;
	ctx->expr_ordinal = 0;

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

	/* Clean up DSM shared code state before freeing compiled code */
	pg_jitter_cleanup_shared_dsm(ctx);

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
		PgJitterForgetContext(ctx->resowner, ctx);
#endif
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
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_oldtuple);
			break;
		case EEOP_NEW_SYSVAR:
			ExecEvalSysVar(state, op, econtext, econtext->ecxt_newtuple);
			break;
#endif

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
#ifdef HAVE_EEOP_PARAM_SET
		case EEOP_PARAM_SET:
			ExecEvalParamSet(state, op, econtext);
			break;
#endif

		/* Case test values */
		case EEOP_CASE_TESTVAL:
			*op->resvalue = *op->d.casetest.value;
			*op->resnull = *op->d.casetest.isnull;
			break;
#ifdef HAVE_EEOP_TESTVAL_EXT
		case EEOP_CASE_TESTVAL_EXT:
			*op->resvalue = econtext->caseValue_datum;
			*op->resnull = econtext->caseValue_isNull;
			break;
#endif

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
#ifdef HAVE_EEOP_IOCOERCE_SAFE
		case EEOP_IOCOERCE_SAFE:
			ExecEvalCoerceViaIOSafe(state, op);
			break;
#endif

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

#ifdef HAVE_EEOP_RETURNINGEXPR
		case EEOP_RETURNINGEXPR:
			if (state->flags & op->d.returningexpr.nullflag)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
				return op->d.returningexpr.jumpdone;
			}
			break;
#endif

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

			/* force NULL result if strict fn and NULL input */
			if (op->d.rowcompare_step.finfo->fn_strict &&
				(fcinfo->args[0].isnull || fcinfo->args[1].isnull))
			{
				*op->resnull = true;
				return op->d.rowcompare_step.jumpnull;
			}

			fcinfo->isnull = false;
			d = op->d.rowcompare_step.fn_addr(fcinfo);
			*op->resvalue = d;

			if (fcinfo->isnull)
			{
				*op->resnull = true;
				return op->d.rowcompare_step.jumpnull;
			}
			*op->resnull = false;

			if (DatumGetInt32(d) != 0)
				return op->d.rowcompare_step.jumpdone;
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
#ifdef HAVE_EEOP_TESTVAL_EXT
		case EEOP_DOMAIN_TESTVAL_EXT:
			*op->resvalue = econtext->domainValue_datum;
			*op->resnull = econtext->domainValue_isNull;
			break;
#endif
		case EEOP_DOMAIN_NOTNULL:
			ExecEvalConstraintNotNull(state, op);
			break;
		case EEOP_DOMAIN_CHECK:
			ExecEvalConstraintCheck(state, op);
			break;

		/* Hash datum */
#ifdef HAVE_EEOP_HASHDATUM
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
#endif /* HAVE_EEOP_HASHDATUM */

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
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
		case EEOP_JSON_CONSTRUCTOR:
			ExecEvalJsonConstructor(state, op, econtext);
			break;
		case EEOP_IS_JSON:
			ExecEvalJsonIsPredicate(state, op);
			break;
#endif
#ifdef HAVE_EEOP_JSONEXPR
		case EEOP_JSONEXPR_PATH:
			return ExecEvalJsonExprPath(state, op, econtext);
		case EEOP_JSONEXPR_COERCION:
			ExecEvalJsonCoercion(state, op, econtext);
			break;
		case EEOP_JSONEXPR_COERCION_FINISH:
			ExecEvalJsonCoercionFinish(state, op);
			break;
#endif

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
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
		case EEOP_MERGE_SUPPORT_FUNC:
			ExecEvalMergeSupportFunc(state, op, econtext);
			break;
#endif
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
#ifdef HAVE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1
		case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
#endif
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

#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
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
#endif

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
#ifdef HAVE_EEOP_DONE_SPLIT
		case EEOP_DONE_NO_RETURN:
#endif
			/* Should not reach fallback — these are always native */
			elog(ERROR, "pg_jitter: DONE opcode in fallback");
			break;

		case EEOP_INNER_FETCHSOME:
		case EEOP_OUTER_FETCHSOME:
		case EEOP_SCAN_FETCHSOME:
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_FETCHSOME:
		case EEOP_NEW_FETCHSOME:
#endif
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
#ifdef HAVE_EEOP_OLD_NEW
				case EEOP_OLD_FETCHSOME:
					slot = econtext->ecxt_oldtuple; break;
				case EEOP_NEW_FETCHSOME:
					slot = econtext->ecxt_newtuple; break;
#endif
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
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_VAR:
		case EEOP_NEW_VAR:
#endif
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
#ifdef HAVE_EEOP_OLD_NEW
				case EEOP_OLD_VAR:
					slot = econtext->ecxt_oldtuple; break;
				case EEOP_NEW_VAR:
					slot = econtext->ecxt_newtuple; break;
#endif
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
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_ASSIGN_OLD_VAR:
		case EEOP_ASSIGN_NEW_VAR:
#endif
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
#ifdef HAVE_EEOP_OLD_NEW
				case EEOP_ASSIGN_OLD_VAR:
					srcslot = econtext->ecxt_oldtuple; break;
				case EEOP_ASSIGN_NEW_VAR:
					srcslot = econtext->ecxt_newtuple; break;
#endif
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
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
		case EEOP_FUNCEXPR_STRICT_1:
		case EEOP_FUNCEXPR_STRICT_2:
#endif
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


/* ----------------------------------------------------------------
 * Shared JIT code helpers for parallel query
 * ----------------------------------------------------------------
 */

/*
 * Store compiled code bytes in DSM shared memory (leader only).
 *
 * Uses a simple spinlock (pg_atomic_uint32) to protect concurrent writes,
 * though in practice only the leader stores code.
 */
bool
pg_jitter_store_shared_code(void *shared, const void *code,
							Size code_size, int node_id, int expr_idx,
							uint64 dylib_ref_addr)
{
	SharedJitCompiledCode *sjc = (SharedJitCompiledCode *) shared;
	SharedJitCodeEntry *entry;
	Size		entry_size;

	entry_size = MAXALIGN(offsetof(SharedJitCodeEntry, code_bytes) + code_size);

	/* Spinlock acquire */
	while (pg_atomic_exchange_u32(&sjc->lock, 1) != 0)
		pg_spin_delay();

	/* Check if there's enough space */
	if (sjc->used + entry_size > sjc->capacity)
	{
		elog(DEBUG1, "pg_jitter: DSM full: need %zu bytes, have %zu/%zu",
			 entry_size, sjc->capacity - sjc->used, sjc->capacity);
		pg_atomic_write_u32(&sjc->lock, 0);
		return false;
	}

	entry = (SharedJitCodeEntry *) ((char *) sjc + sjc->used);
	entry->plan_node_id = node_id;
	entry->expr_index = expr_idx;
	entry->code_size = code_size;
	entry->dylib_ref_addr = dylib_ref_addr;
	memcpy(entry->code_bytes, code, code_size);

	sjc->used += entry_size;
	sjc->num_entries++;

	/* Release lock */
	pg_atomic_write_u32(&sjc->lock, 0);

	return true;
}

/*
 * Find compiled code in DSM shared memory (worker).
 *
 * Scans all entries for a matching (plan_node_id, expr_index) pair.
 * Returns true if found, with code_bytes and code_size set.
 */
bool
pg_jitter_find_shared_code(void *shared, int node_id, int expr_idx,
						   const void **code_bytes, Size *code_size,
						   uint64 *dylib_ref_addr)
{
	SharedJitCompiledCode *sjc = (SharedJitCompiledCode *) shared;
	Size		offset;
	int			i;

	offset = MAXALIGN(sizeof(SharedJitCompiledCode));

	for (i = 0; i < sjc->num_entries; i++)
	{
		SharedJitCodeEntry *entry = (SharedJitCodeEntry *) ((char *) sjc + offset);

		if (entry->plan_node_id == node_id && entry->expr_index == expr_idx)
		{
			*code_bytes = entry->code_bytes;
			*code_size = entry->code_size;
			if (dylib_ref_addr)
				*dylib_ref_addr = entry->dylib_ref_addr;
			return true;
		}

		offset += MAXALIGN(offsetof(SharedJitCodeEntry, code_bytes) +
						   entry->code_size);
	}

	return false;
}

/*
 * ExecMemInfo: tracks mmap allocation for proper cleanup.
 * Returned as an opaque handle by pg_jitter_copy_to_executable;
 * callers must use pg_jitter_exec_code_ptr to get the executable address.
 */
typedef struct ExecMemInfo
{
	void   *code_ptr;	/* the executable memory (mmap'd) */
	Size	alloc_size;
} ExecMemInfo;

/*
 * Copy code bytes to local executable memory.
 *
 * On macOS ARM64, uses mmap(MAP_JIT) + pthread_jit_write_protect_np for W^X.
 * On Linux, uses mmap + mprotect.
 * Returns an ExecMemInfo handle (use pg_jitter_exec_code_ptr for code address).
 */

void *
pg_jitter_copy_to_executable(const void *code_bytes, Size code_size)
{
	void		   *mem;
	ExecMemInfo	   *info;
	Size			alloc_size;

	/*
	 * Allocate a fresh MAP_JIT page for the shared code.
	 * Each allocation gets its own mmap — avoids interference with sljit's
	 * allocator state and ensures clean W^X transitions.
	 */
	alloc_size = (code_size + 4095) & ~((Size)4095);	/* page-align */

#if defined(__APPLE__) && defined(__aarch64__)
	mem = mmap(NULL, alloc_size,
			   PROT_READ | PROT_WRITE | PROT_EXEC,
			   MAP_PRIVATE | MAP_ANON | MAP_JIT,
			   -1, 0);
	if (mem == MAP_FAILED)
		return NULL;

	pthread_jit_write_protect_np(0);
	memcpy(mem, code_bytes, code_size);
	sys_icache_invalidate(mem, code_size);
	pthread_jit_write_protect_np(1);

	/* Verify: read back the code bytes and compare */
	if (memcmp(mem, code_bytes, code_size) != 0)
		elog(WARNING, "pg_jitter: VERIFY FAILED — code bytes don't match after copy!");
	else
		elog(DEBUG1, "pg_jitter: copy verified OK (%zu bytes)", code_size);
#else
	mem = mmap(NULL, alloc_size,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANON,
			   -1, 0);
	if (mem == MAP_FAILED)
		return NULL;

	memcpy(mem, code_bytes, code_size);
	if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0)
	{
		munmap(mem, alloc_size);
		return NULL;
	}
#if defined(__aarch64__)
	__builtin___clear_cache((char *) mem, (char *) mem + code_size);
#endif
#endif

	info = (ExecMemInfo *)
		MemoryContextAlloc(TopMemoryContext, sizeof(ExecMemInfo));
	info->code_ptr = mem;
	info->alloc_size = alloc_size;

	return info;
}

/*
 * Free executable memory allocated by pg_jitter_copy_to_executable.
 */

void
pg_jitter_exec_free(void *ptr)
{
	ExecMemInfo *info = (ExecMemInfo *) ptr;

	if (info)
	{
		munmap(info->code_ptr, info->alloc_size);
		pfree(info);
	}
}

void *
pg_jitter_exec_code_ptr(void *handle)
{
	ExecMemInfo *info = (ExecMemInfo *) handle;
	return info->code_ptr;
}

/*
 * Relocate dylib function addresses in copied executable code.
 *
 * ARM64: Scans for MOVZ+3×MOVK sequences (the pattern sljit uses for
 * 64-bit immediates when SLJIT_REWRITABLE_JUMP is set).
 *
 * x86_64: Scans for MOV r, imm64 (MOVABS) instructions — REX.W prefix
 * (0x48 or 0x49) followed by opcode 0xB8-0xBF with an 8-byte immediate.
 *
 * For each matched instruction, reconstructs the embedded 64-bit address.
 * If the address falls within a generous range around the leader's dylib
 * reference address, it's assumed to be a dylib function pointer and is
 * relocated by adding the delta between leader and worker addresses.
 *
 * Returns the number of addresses patched.
 */
int
pg_jitter_relocate_dylib_addrs(void *handle, Size code_size,
							   uint64 leader_ref_addr,
							   uint64 worker_ref_addr)
{
#if defined(__aarch64__)
	ExecMemInfo *info = (ExecMemInfo *) handle;
	uint32_t   *insns;
	int			ninsns;
	int			patched = 0;
	int64		delta;

	if (!info || leader_ref_addr == worker_ref_addr)
		return 0;

	delta = (int64)(worker_ref_addr - leader_ref_addr);
	insns = (uint32_t *) info->code_ptr;
	ninsns = code_size / 4;

	/*
	 * Toggle to write mode for patching.
	 * The page was set to exec mode by pg_jitter_copy_to_executable.
	 */
#if defined(__APPLE__)
	pthread_jit_write_protect_np(0);
#endif

	/*
	 * Scan for MOVZ+MOVK+MOVK+MOVK sequences targeting the same register.
	 *
	 * ARM64 encoding:
	 *   MOVZ: 1 10 100101 hw(2) imm16(16) Rd(5)
	 *   MOVK: 1 11 100101 hw(2) imm16(16) Rd(5)
	 *
	 * sljit emits them in order: hw=0, hw=1, hw=2, hw=3.
	 * We look for MOVZ(hw=0) followed by 3 MOVK(hw=1,2,3) with same Rd.
	 */
	for (int i = 0; i + 3 < ninsns; i++)
	{
		uint32_t w0 = insns[i];
		uint32_t w1 = insns[i + 1];
		uint32_t w2 = insns[i + 2];
		uint32_t w3 = insns[i + 3];

		/* Check MOVZ X?, #imm, LSL #0 */
		if ((w0 & 0xFFE00000) != 0xD2800000)
			continue;

		int rd = w0 & 0x1F;

		/* Check MOVK X?, #imm, LSL #16 with same Rd */
		if ((w1 & 0xFFE0001F) != (0xF2A00000 | rd))
			continue;

		/* Check MOVK X?, #imm, LSL #32 with same Rd */
		if ((w2 & 0xFFE0001F) != (0xF2C00000 | rd))
			continue;

		/* Check MOVK X?, #imm, LSL #48 with same Rd */
		if ((w3 & 0xFFE0001F) != (0xF2E00000 | rd))
			continue;

		/* Reconstruct the 64-bit address */
		uint64	addr;
		addr  = (uint64)((w0 >> 5) & 0xFFFF);
		addr |= (uint64)((w1 >> 5) & 0xFFFF) << 16;
		addr |= (uint64)((w2 >> 5) & 0xFFFF) << 32;
		addr |= (uint64)((w3 >> 5) & 0xFFFF) << 48;

		/*
		 * Check if this address is within the dylib's address range.
		 * The dylib is ~512KB, so any function should be within ±512KB
		 * of the reference (pg_jitter_fallback_step).
		 */
		int64	offset_from_ref = (int64)(addr - leader_ref_addr);

		elog(DEBUG1, "pg_jitter: relocate scan insn[%d] rd=x%d addr=%lx "
			 "offset_from_ref=%ld",
			 i, rd, (unsigned long) addr, (long) offset_from_ref);

		if (offset_from_ref >= -0x80000 && offset_from_ref <= 0x80000)
		{
			/* Relocate: add delta to get worker's address */
			uint64 new_addr = addr + delta;

			elog(DEBUG1, "pg_jitter: relocate PATCH insn[%d] rd=x%d "
				 "old=%lx new=%lx delta=%ld",
				 i, rd, (unsigned long) addr, (unsigned long) new_addr,
				 (long) delta);

			/* Patch the 4 instructions with new imm16 values */
			insns[i]     = (w0 & 0xFFE0001F) | (((new_addr >>  0) & 0xFFFF) << 5);
			insns[i + 1] = (w1 & 0xFFE0001F) | (((new_addr >> 16) & 0xFFFF) << 5);
			insns[i + 2] = (w2 & 0xFFE0001F) | (((new_addr >> 32) & 0xFFFF) << 5);
			insns[i + 3] = (w3 & 0xFFE0001F) | (((new_addr >> 48) & 0xFFFF) << 5);

			patched++;
			i += 3; /* skip the 3 MOVK instructions */
		}
	}

	if (patched > 0)
	{
		/* Flush I-cache after patching */
		sys_icache_invalidate(info->code_ptr, code_size);
	}

#if defined(__APPLE__)
	/* Restore exec mode */
	pthread_jit_write_protect_np(1);
#endif

	return patched;
#elif defined(__x86_64__) || defined(_M_X64)
	ExecMemInfo *info = (ExecMemInfo *) handle;
	uint8_t    *bytes;
	int			patched = 0;
	int64		delta;
	Size		alloc_size;

	if (!info || leader_ref_addr == worker_ref_addr)
		return 0;

	delta = (int64)(worker_ref_addr - leader_ref_addr);
	bytes = (uint8_t *) info->code_ptr;
	alloc_size = info->alloc_size;

	/*
	 * Make memory writable for patching.
	 * pg_jitter_copy_to_executable() sets PROT_READ|PROT_EXEC on Linux.
	 */
	if (mprotect(info->code_ptr, alloc_size, PROT_READ | PROT_WRITE) != 0)
	{
		elog(WARNING, "pg_jitter: relocate mprotect(RW) failed: %m");
		return 0;
	}

	/*
	 * Scan for MOV r, imm64 (MOVABS) instructions.
	 *
	 * x86_64 encoding: REX.W prefix (1 byte) + opcode (1 byte) + imm64 (8 bytes)
	 *   REX byte: 0x48 (REX.W) for r0-r7, 0x49 (REX.W|REX.B) for r8-r15
	 *   Opcode:   0xB8 + (reg & 7) — i.e., (byte & 0xF8) == 0xB8
	 *   Immediate: 8 bytes little-endian at offset +2
	 *
	 * Total instruction size: 10 bytes.
	 */
	for (Size i = 0; i + 9 < code_size; i++)
	{
		uint8_t rex = bytes[i];
		uint8_t opc = bytes[i + 1];

		/* Check REX.W prefix (0x48 or 0x49) */
		if (rex != 0x48 && rex != 0x49)
			continue;

		/* Check MOV r, imm64 opcode: 0xB8-0xBF */
		if ((opc & 0xF8) != 0xB8)
			continue;

		/* Extract 8-byte little-endian immediate from bytes[i+2..i+9] */
		uint64	addr;
		memcpy(&addr, &bytes[i + 2], sizeof(uint64));

		/*
		 * Check if this address is within the dylib's address range.
		 * Same heuristic as ARM64: ±512KB of the reference address.
		 */
		int64	offset_from_ref = (int64)(addr - leader_ref_addr);

		elog(DEBUG1, "pg_jitter: relocate scan byte[%zu] rex=0x%02x opc=0x%02x "
			 "addr=%lx offset_from_ref=%ld",
			 i, rex, opc, (unsigned long) addr, (long) offset_from_ref);

		if (offset_from_ref >= -0x80000 && offset_from_ref <= 0x80000)
		{
			/* Relocate: add delta to get worker's address */
			uint64 new_addr = addr + delta;

			elog(DEBUG1, "pg_jitter: relocate PATCH byte[%zu] "
				 "old=%lx new=%lx delta=%ld",
				 i, (unsigned long) addr, (unsigned long) new_addr,
				 (long) delta);

			/* Patch the 8-byte immediate in place */
			memcpy(&bytes[i + 2], &new_addr, sizeof(uint64));

			patched++;
			i += 9; /* skip past this 10-byte instruction */
		}
	}

	/* Restore executable permission */
	if (mprotect(info->code_ptr, alloc_size, PROT_READ | PROT_EXEC) != 0)
		elog(WARNING, "pg_jitter: relocate mprotect(RX) failed: %m");

	/* No I-cache flush needed on x86_64 (coherent I-cache) */

	return patched;
#else
	/* Unsupported architecture */
	return 0;
#endif
}

/*
 * Get expression identity for shared code matching.
 *
 * Uses (plan_node_id, expr_ordinal) as the key. plan_node_id is stable
 * across leader/workers (from the plan). expr_ordinal is a counter
 * incremented per compile_expr call within each PlanState — deterministic
 * because ExecInitExpr processes expressions in the same order.
 */
void
pg_jitter_get_expr_identity(PgJitterContext *ctx, ExprState *state,
							int *node_id, int *expr_idx)
{
	/*
	 * Use (plan_node_id, global_ordinal) as identity key.
	 *
	 * The ordinal is a globally incrementing counter across all compile_expr
	 * calls within this JitContext. Both leader and worker traverse the plan
	 * tree in the same deterministic order (ExecInitNode), so the global
	 * counter matches between them.
	 *
	 * We do NOT reset expr_ordinal per plan_node_id because the same node can
	 * be visited multiple times during ExecInitNode (e.g., in nested loops
	 * where the inner Gather node's expressions are compiled, then outer
	 * expressions referencing the same plan node are compiled later).
	 * Resetting would produce duplicate (node_id, expr_idx) keys, causing
	 * workers to load the wrong code → SIGSEGV.
	 */
	*node_id = state->parent->plan->plan_node_id;
	*expr_idx = ctx->expr_ordinal++;
}

/* ----------------------------------------------------------------
 * DSM-based shared code management for parallel queries
 *
 * Leader creates a DSM segment during the first compile_expr call,
 * initializes it, and stores the dsm_handle in a GUC string.
 * PG serializes all GUCs (including extension-defined ones) to
 * parallel workers via SerializeGUCState in InitializeParallelDSM.
 * Workers read the GUC, attach to the DSM, and copy pre-compiled
 * code instead of recompiling.
 * ----------------------------------------------------------------
 */

/*
 * Leader: create DSM for shared JIT code and store handle in GUC.
 */
void
pg_jitter_init_shared_dsm(PgJitterContext *ctx)
{
	Size		dsm_size;
	char		buf[32];

	if (ctx->share_state.initialized)
		return;

	dsm_size = (Size) pg_jitter_get_shared_code_max_kb() * 1024;

	ctx->share_state.dsm_seg = dsm_create(dsm_size, 0);
	dsm_pin_mapping(ctx->share_state.dsm_seg);

	ctx->share_state.sjc = (SharedJitCompiledCode *)
		dsm_segment_address(ctx->share_state.dsm_seg);

	/* Initialize header */
	pg_atomic_init_u32(&ctx->share_state.sjc->lock, 0);
	ctx->share_state.sjc->num_entries = 0;
	ctx->share_state.sjc->capacity = dsm_size;
	ctx->share_state.sjc->used = MAXALIGN(sizeof(SharedJitCompiledCode));

	/* Store handle in GUC for worker discovery */
	snprintf(buf, sizeof(buf), "%u",
			 dsm_segment_handle(ctx->share_state.dsm_seg));
	SetConfigOption("pg_jitter._shared_dsm", buf,
					PGC_USERSET, PGC_S_SESSION);

	ctx->share_state.is_leader = true;
	ctx->share_state.initialized = true;

	elog(DEBUG1, "pg_jitter: leader created shared DSM handle=%s size=%zu",
		 buf, dsm_size);
}

/*
 * Worker: read DSM handle from GUC and attach.
 */
void
pg_jitter_attach_shared_dsm(PgJitterContext *ctx)
{
	const char *val;
	dsm_handle	handle;

	if (ctx->share_state.initialized)
		return;

	val = GetConfigOption("pg_jitter._shared_dsm", true, false);
	if (val == NULL || val[0] == '\0')
	{
		elog(DEBUG1, "pg_jitter: worker found no shared DSM GUC");
		return;
	}

	handle = (dsm_handle) strtoul(val, NULL, 10);
	if (handle == 0)
	{
		elog(DEBUG1, "pg_jitter: worker got invalid DSM handle from GUC");
		return;
	}

	ctx->share_state.dsm_seg = dsm_attach(handle);
	if (ctx->share_state.dsm_seg == NULL)
	{
		elog(DEBUG1, "pg_jitter: worker failed to attach DSM handle=%u", handle);
		return;
	}

	dsm_pin_mapping(ctx->share_state.dsm_seg);

	ctx->share_state.sjc = (SharedJitCompiledCode *)
		dsm_segment_address(ctx->share_state.dsm_seg);
	ctx->share_state.is_leader = false;
	ctx->share_state.initialized = true;

	elog(DEBUG1, "pg_jitter: worker attached to shared DSM handle=%u "
		 "entries=%d used=%zu/%zu",
		 handle, ctx->share_state.sjc->num_entries,
		 ctx->share_state.sjc->used, ctx->share_state.sjc->capacity);
}

/*
 * Cleanup: unpin mapping and detach from DSM.
 *
 * Note: we do NOT call SetConfigOption here to reset the _shared_dsm GUC.
 * This function may be called from a ResourceOwner release callback during
 * subtransaction abort, and SetConfigOption allocates memory — which
 * triggers "ResourceOwnerEnlarge called after release started".  The GUC
 * is session-scoped and harmless if stale; each new parallel query creates
 * a fresh DSM handle.
 */
void
pg_jitter_cleanup_shared_dsm(PgJitterContext *ctx)
{
	if (!ctx->share_state.initialized)
		return;

	/* Reset shared deform mmap before detaching DSM */
	pg_jitter_reset_shared_deform();

	if (ctx->share_state.dsm_seg)
	{
		/*
		 * Just detach — do NOT call dsm_unpin_mapping() first.
		 * dsm_unpin_mapping() calls ResourceOwnerEnlarge() to re-register the
		 * segment with a resource owner, but this function may be called during
		 * ResourceOwnerRelease (subtransaction abort), where new registrations
		 * are forbidden.  dsm_detach() works fine on pinned segments.
		 */
		dsm_detach(ctx->share_state.dsm_seg);
	}

	memset(&ctx->share_state, 0, sizeof(JitShareState));
}

/*
 * pg_jitter_deform_threshold — column count above which loop-based deform
 * is used instead of unrolled deform.
 *
 * Unrolled deform emits ~140 bytes per column. When the total exceeds half
 * the L1I cache, instruction cache thrashing makes JIT slower than the
 * interpreter. The loop-based deform emits ~530 bytes of code regardless
 * of column count.
 *
 * Threshold = L1I_cache_size / DEFORM_BYTES_PER_COL / 2
 * On a typical 32KB L1I CPU this gives ~117 columns.
 */
int
pg_jitter_deform_threshold(void)
{
	static int	threshold = 0;

	if (threshold > 0)
		return threshold;

	{
		long	l1i_size = 0;

#ifdef __linux__
		l1i_size = sysconf(_SC_LEVEL1_ICACHE_SIZE);
		if (l1i_size <= 0)
			l1i_size = 32768;
#elif defined(__APPLE__)
		{
			size_t	len = sizeof(l1i_size);
			if (sysctlbyname("hw.l1icachesize", &l1i_size, &len, NULL, 0) != 0)
				l1i_size = 32768;
		}
#else
		l1i_size = 32768;
#endif

		threshold = l1i_size / DEFORM_BYTES_PER_COL / 2;
		if (threshold < 16)
			threshold = 16;		/* never go below 16 columns */

		return threshold;
	}
}
