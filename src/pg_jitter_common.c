/*
 * pg_jitter_common.c — Shared context management and fallback dispatch
 *
 * This file is compiled into each pg_jitter provider library.
 */
#include "postgres.h"
#include "pg_jitter_common.h"
#include "pg_jit_funcs.h"

#include "catalog/pg_collation_d.h"
#include "executor/execExpr.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/shmem.h"
#include "utils/expandeddatum.h"
#include "utils/fmgrprotos.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"            /* VARDATA_ANY, VARSIZE_ANY_EXHDR */
#endif
#include "access/detoast.h"    /* DatumGetTextPP */
#include "utils/resowner.h"

#include <stdlib.h> /* for atoi */

#ifdef __linux__
#include <unistd.h> /* for sysconf */
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/* GUC: pg_jitter.parallel_mode — shared across backends */
int pg_jitter_parallel_mode = 1; /* PARALLEL_JIT_PER_WORKER */

/* GUC: pg_jitter.shared_code_max — cap in KB */
int pg_jitter_shared_code_max_kb = 4096; /* 4 MB default */

/* GUC: pg_jitter.deform_cache — cache compiled deform functions across queries */
bool pg_jitter_deform_cache = true;

/* GUC: pg_jitter.min_expr_steps — skip JIT for expressions with fewer steps */
int pg_jitter_min_expr_steps = 4;

/* GUC: pg_jitter.in_hash — hash table strategy for large IN lists */
int pg_jitter_in_hash_strategy = IN_HASH_CRC32;

/* GUC: pg_jitter.in_bsearch_max — max IN list size for inline bsearch tree */
int pg_jitter_in_bsearch_max = IN_BSEARCH_MAX_DEFAULT;

/* ----------------------------------------------------------------
 * Shared memory slot table for DSM handle passing
 *
 * Lazily allocated from PG's spare shmem pool via ShmemInitStruct.
 * Each backend slot holds one atomic uint32 DSM handle.
 * Size: sizeof(int) + MaxBackends * sizeof(pg_atomic_uint32) ≈ 4KB.
 * ---------------------------------------------------------------- */
static JitDsmSlotTable *jit_dsm_slots = NULL;
static bool shmem_init_attempted = false;
static bool shmem_init_failed = false;

/*
 * Lazy init: try to allocate from PG's spare shmem pool.
 * Returns true if the slot table is usable.
 */
static bool pg_jitter_shmem_init(void) {
  bool found;
  Size size;

  if (jit_dsm_slots != NULL)
    return true;

  if (shmem_init_failed)
    return false;

  if (shmem_init_attempted)
    return false;

  shmem_init_attempted = true;

  size = offsetof(JitDsmSlotTable, handles) +
         sizeof(pg_atomic_uint32) * MaxBackends;

  PG_TRY();
  {
    jit_dsm_slots =
        (JitDsmSlotTable *)ShmemInitStruct("pg_jitter_dsm_slots", size, &found);
  }
  PG_CATCH();
  {
    /* Shmem pool exhausted — fall back to per-worker compilation */
    FlushErrorState();
    shmem_init_failed = true;
    elog(DEBUG1, "pg_jitter: shmem allocation failed, "
                 "falling back to per-worker JIT compilation");
    return false;
  }
  PG_END_TRY();

  if (!found) {
    jit_dsm_slots->num_slots = MaxBackends;
    for (int i = 0; i < MaxBackends; i++)
      pg_atomic_init_u32(&jit_dsm_slots->handles[i], 0);
  }

  return true;
}

bool pg_jitter_shmem_available(void) { return pg_jitter_shmem_init(); }

void pg_jitter_shmem_set_dsm_handle(int proc_index, dsm_handle handle) {
  if (!pg_jitter_shmem_init())
    return;
  if (proc_index < 0 || proc_index >= jit_dsm_slots->num_slots)
    return;
  pg_atomic_write_u32(&jit_dsm_slots->handles[proc_index], handle);
}

dsm_handle pg_jitter_shmem_get_dsm_handle(int proc_index) {
  if (!pg_jitter_shmem_init())
    return 0;
  if (proc_index < 0 || proc_index >= jit_dsm_slots->num_slots)
    return 0;
  return pg_atomic_read_u32(&jit_dsm_slots->handles[proc_index]);
}

void pg_jitter_shmem_clear_dsm_handle(int proc_index) {
  if (jit_dsm_slots == NULL)
    return;
  if (proc_index < 0 || proc_index >= jit_dsm_slots->num_slots)
    return;
  pg_atomic_write_u32(&jit_dsm_slots->handles[proc_index], 0);
}

/*
 * Read the current pg_jitter.parallel_mode from PG's GUC system.
 *
 * This is needed because when backends are loaded via the meta module,
 * the GUC variable pointer may belong to a different dylib's copy of
 * pg_jitter_parallel_mode.  Reading from the GUC system ensures we
 * always get the current value regardless of which module defined it.
 */
int pg_jitter_get_parallel_mode(void) {
  const char *val = GetConfigOption("pg_jitter.parallel_mode", true, false);

  if (val == NULL)
    return pg_jitter_parallel_mode; /* fallback to local default */

  if (strcmp(val, "off") == 0)
    return PARALLEL_JIT_OFF;
  else if (strcmp(val, "per_worker") == 0)
    return PARALLEL_JIT_PER_WORKER;
  else if (strcmp(val, "shared") == 0)
    return PARALLEL_JIT_SHARED;

  return pg_jitter_parallel_mode; /* unknown value, use local */
}

/*
 * Read pg_jitter.min_expr_steps from the GUC system.
 */
int pg_jitter_get_min_expr_steps(void) {
  const char *val = GetConfigOption("pg_jitter.min_expr_steps", true, false);

  if (val != NULL)
    return atoi(val);

  return pg_jitter_min_expr_steps;
}

/*
 * Read pg_jitter.shared_code_max from the GUC system (in KB).
 */
static int pg_jitter_get_shared_code_max_kb(void) {
  const char *val = GetConfigOption("pg_jitter.shared_code_max", true, false);

  if (val != NULL)
    return atoi(val);

  return pg_jitter_shared_code_max_kb;
}

#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

/*
 * Resource owner support — follows llvmjit.c pattern.
 *
 * PG17+ uses the generic ResourceOwnerDesc API.
 * PG14-16 use the JIT-specific ResourceOwnerEnlargeJIT/RememberJIT/ForgetJIT.
 */
#if PG_VERSION_NUM >= 170000

static void ResOwnerReleasePgJitterContext(Datum res);

static const ResourceOwnerDesc pg_jitter_resowner_desc = {
    .name = "pg_jitter JIT context",
    .release_phase = RESOURCE_RELEASE_BEFORE_LOCKS,
    .release_priority = RELEASE_PRIO_JIT_CONTEXTS,
    .ReleaseResource = ResOwnerReleasePgJitterContext,
    .DebugPrint = NULL};

static inline void PgJitterRememberContext(ResourceOwner owner,
                                           PgJitterContext *handle) {
  ResourceOwnerRemember(owner, PointerGetDatum(handle),
                        &pg_jitter_resowner_desc);
}

static inline void PgJitterForgetContext(ResourceOwner owner,
                                         PgJitterContext *handle) {
  ResourceOwnerForget(owner, PointerGetDatum(handle), &pg_jitter_resowner_desc);
}

static void ResOwnerReleasePgJitterContext(Datum res) {
  PgJitterContext *context = (PgJitterContext *)DatumGetPointer(res);

  context->resowner = NULL;
  jit_release_context(&context->base);
}

#else /* PG14-16: JIT-specific resource owner API */

#include "utils/resowner_private.h"

static inline void PgJitterRememberContext(ResourceOwner owner,
                                           PgJitterContext *handle) {
  ResourceOwnerRememberJIT(owner, PointerGetDatum(handle));
}

static inline void PgJitterForgetContext(ResourceOwner owner,
                                         PgJitterContext *handle) {
  ResourceOwnerForgetJIT(owner, PointerGetDatum(handle));
}

#endif /* PG_VERSION_NUM >= 170000 */

/*
 * Get or create a PgJitterContext for the current query.
 * Follows the pattern in llvmjit.c:222-246.
 */
PgJitterContext *pg_jitter_get_context(ExprState *state) {
  PlanState *parent = state->parent;
  PgJitterContext *ctx;

  Assert(parent != NULL);

  if (parent->state->es_jit)
    return (PgJitterContext *)parent->state->es_jit;

#if PG_VERSION_NUM >= 170000
  ResourceOwnerEnlarge(CurrentResourceOwner);
#else
  ResourceOwnerEnlargeJIT(CurrentResourceOwner);
#endif

  ctx = (PgJitterContext *)MemoryContextAllocZero(TopMemoryContext,
                                                  sizeof(PgJitterContext));
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
void pg_jitter_register_compiled(PgJitterContext *ctx, void (*free_fn)(void *),
                                 void *data) {
  CompiledCode *cc;

  cc = (CompiledCode *)MemoryContextAlloc(TopMemoryContext,
                                          sizeof(CompiledCode));
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
void pg_jitter_release_context(JitContext *context) {
  PgJitterContext *ctx = (PgJitterContext *)context;
  CompiledCode *cc, *next;

  /* Clear deform dispatch fast-path cache — TupleDesc pointers from this
   * query may be reused by palloc for different layouts in future queries. */
#ifdef PG_JITTER_HAVE_SLJIT_DEFORM
  pg_jitter_deform_dispatch_reset_fastpath();
#endif

  /* Clean up DSM shared code state before freeing compiled code */
  pg_jitter_cleanup_shared_dsm(ctx);

  for (cc = ctx->compiled_list; cc != NULL; cc = next) {
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
void pg_jitter_reset_after_error(void) {
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
int64 pg_jitter_fallback_step(ExprState *state, ExprEvalStep *op,
                              ExprContext *econtext) {
  ExprEvalOp opcode = ExecEvalStepOp(state, op);

  switch (opcode) {
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
    if (*op->resnull) {
      *op->resvalue = BoolGetDatum(false);
      *op->resnull = false;
    }
    break;
  case EEOP_BOOLTEST_IS_NOT_TRUE:
    if (*op->resnull) {
      *op->resvalue = BoolGetDatum(true);
      *op->resnull = false;
    } else
      *op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));
    break;
  case EEOP_BOOLTEST_IS_FALSE:
    if (*op->resnull) {
      *op->resvalue = BoolGetDatum(false);
      *op->resnull = false;
    } else
      *op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));
    break;
  case EEOP_BOOLTEST_IS_NOT_FALSE:
    if (*op->resnull) {
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
  case EEOP_IOCOERCE: {
    FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
    FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;
    char *str;

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
  case EEOP_NOT_DISTINCT: {
    FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
    bool arg_null0 = fcinfo->args[0].isnull;
    bool arg_null1 = fcinfo->args[1].isnull;

    if (arg_null0 != arg_null1) {
      *op->resvalue = BoolGetDatum(opcode == EEOP_DISTINCT);
      *op->resnull = false;
    } else if (arg_null0) {
      *op->resvalue = BoolGetDatum(opcode != EEOP_DISTINCT);
      *op->resnull = false;
    } else {
      fcinfo->isnull = false;
      Datum result = op->d.func.fn_addr(fcinfo);

      *op->resnull = false;
      if (opcode == EEOP_DISTINCT)
        *op->resvalue = BoolGetDatum(!DatumGetBool(result));
      else
        *op->resvalue = result;
    }
    break;
  }
  case EEOP_NULLIF: {
    FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

    if (fcinfo->args[0].isnull) {
      *op->resnull = true;
      break;
    }
    if (!fcinfo->args[1].isnull) {
      Datum result;

      fcinfo->isnull = false;
      result = op->d.func.fn_addr(fcinfo);
      if (!fcinfo->isnull && DatumGetBool(result)) {
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
    if (state->flags & op->d.returningexpr.nullflag) {
      *op->resvalue = (Datum)0;
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
  case EEOP_ROWCOMPARE_STEP: {
    FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
    Datum d;

    /* force NULL result if strict fn and NULL input */
    if (op->d.rowcompare_step.finfo->fn_strict &&
        (fcinfo->args[0].isnull || fcinfo->args[1].isnull)) {
      *op->resnull = true;
      return op->d.rowcompare_step.jumpnull;
    }

    fcinfo->isnull = false;
    d = op->d.rowcompare_step.fn_addr(fcinfo);
    *op->resvalue = d;

    if (fcinfo->isnull) {
      *op->resnull = true;
      return op->d.rowcompare_step.jumpnull;
    }
    *op->resnull = false;

    if (DatumGetInt32(d) != 0)
      return op->d.rowcompare_step.jumpdone;
    break;
  }
  case EEOP_ROWCOMPARE_FINAL: {
    int32 cmpresult = DatumGetInt32(*op->resvalue);
    CompareType cmptype = op->d.rowcompare_final.cmptype;

    switch (cmptype) {
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
      elog(ERROR, "unexpected compare type: %d", (int)cmptype);
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
  case EEOP_HASHDATUM_NEXT32_STRICT: {
    FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
    Datum hashval;

    if (opcode == EEOP_HASHDATUM_FIRST_STRICT ||
        opcode == EEOP_HASHDATUM_NEXT32_STRICT) {
      if (fcinfo->args[0].isnull) {
        *op->resnull = true;
        *op->resvalue = (Datum)0;
        return op->d.hashdatum.jumpdone;
      }
    }

    if (opcode == EEOP_HASHDATUM_FIRST ||
        opcode == EEOP_HASHDATUM_FIRST_STRICT) {
      if (!fcinfo->args[0].isnull) {
        fcinfo->isnull = false;
        *op->resvalue = op->d.hashdatum.fn_addr(fcinfo);
      } else
        *op->resvalue = (Datum)0;
    } else {
      /* NEXT32 variants: read from iresult, not *op->resvalue */
      uint32 existing = DatumGetUInt32(op->d.hashdatum.iresult->value);

      existing = pg_rotate_left32(existing, 1);

      if (!fcinfo->args[0].isnull) {
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
  case EEOP_AGGREF: {
    int aggno = op->d.aggref.aggno;

    *op->resvalue = econtext->ecxt_aggvalues[aggno];
    *op->resnull = econtext->ecxt_aggnulls[aggno];
    break;
  }
  case EEOP_GROUPING_FUNC:
    ExecEvalGroupingFunc(state, op);
    break;
  case EEOP_WINDOW_FUNC: {
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
  case EEOP_AGG_DESERIALIZE: {
    FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;

    if (opcode == EEOP_AGG_STRICT_DESERIALIZE && fcinfo->args[0].isnull)
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

    for (int argno = 0; argno < nargs; argno++) {
      if (args[argno].isnull)
        return op->d.agg_strict_input_check.jumpnull;
    }
    break;
  }

  case EEOP_AGG_STRICT_INPUT_CHECK_NULLS: {
    bool *nulls = op->d.agg_strict_input_check.nulls;
    int nargs = op->d.agg_strict_input_check.nargs;

    for (int argno = 0; argno < nargs; argno++) {
      if (nulls[argno])
        return op->d.agg_strict_input_check.jumpnull;
    }
    break;
  }

  case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerGroup pergroup_allaggs =
        aggstate->all_pergroups[op->d.agg_plain_pergroup_nullcheck.setoff];

    if (pergroup_allaggs == NULL)
      return op->d.agg_plain_pergroup_nullcheck.jumpnull;
    break;
  }

  case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
    AggStatePerGroup pergroup =
        &aggstate
             ->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

    if (pergroup->noTransValue) {
      ExecAggInitGroup(aggstate, pertrans, pergroup,
                       op->d.agg_trans.aggcontext);
    } else if (likely(!pergroup->transValueIsNull)) {
      /* Inline ExecAggPlainTransByVal */
      FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
      MemoryContext oldContext;
      Datum newVal;

      aggstate->curaggcontext = op->d.agg_trans.aggcontext;
      aggstate->current_set = op->d.agg_trans.setno;
      aggstate->curpertrans = pertrans;

      oldContext =
          MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
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

  case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
    AggStatePerGroup pergroup =
        &aggstate
             ->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

    if (likely(!pergroup->transValueIsNull)) {
      FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
      MemoryContext oldContext;
      Datum newVal;

      aggstate->curaggcontext = op->d.agg_trans.aggcontext;
      aggstate->current_set = op->d.agg_trans.setno;
      aggstate->curpertrans = pertrans;

      oldContext =
          MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
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

  case EEOP_AGG_PLAIN_TRANS_BYVAL: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
    AggStatePerGroup pergroup =
        &aggstate
             ->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    MemoryContext oldContext;
    Datum newVal;

    aggstate->curaggcontext = op->d.agg_trans.aggcontext;
    aggstate->current_set = op->d.agg_trans.setno;
    aggstate->curpertrans = pertrans;

    oldContext =
        MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
    fcinfo->args[0].value = pergroup->transValue;
    fcinfo->args[0].isnull = pergroup->transValueIsNull;
    fcinfo->isnull = false;
    newVal = FunctionCallInvoke(fcinfo);
    pergroup->transValue = newVal;
    pergroup->transValueIsNull = fcinfo->isnull;
    MemoryContextSwitchTo(oldContext);
    break;
  }

  case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
    AggStatePerGroup pergroup =
        &aggstate
             ->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

    if (pergroup->noTransValue) {
      ExecAggInitGroup(aggstate, pertrans, pergroup,
                       op->d.agg_trans.aggcontext);
    } else if (likely(!pergroup->transValueIsNull)) {
      /* Inline ExecAggPlainTransByRef */
      FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
      MemoryContext oldContext;
      Datum newVal;

      aggstate->curaggcontext = op->d.agg_trans.aggcontext;
      aggstate->current_set = op->d.agg_trans.setno;
      aggstate->curpertrans = pertrans;

      oldContext =
          MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
      fcinfo->args[0].value = pergroup->transValue;
      fcinfo->args[0].isnull = pergroup->transValueIsNull;
      fcinfo->isnull = false;
      newVal = FunctionCallInvoke(fcinfo);
      if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
        newVal = ExecAggCopyTransValue(aggstate, pertrans, newVal,
                                       fcinfo->isnull, pergroup->transValue,
                                       pergroup->transValueIsNull);
      pergroup->transValue = newVal;
      pergroup->transValueIsNull = fcinfo->isnull;
      MemoryContextSwitchTo(oldContext);
    }
    break;
  }

  case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
    AggStatePerGroup pergroup =
        &aggstate
             ->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

    if (likely(!pergroup->transValueIsNull)) {
      FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
      MemoryContext oldContext;
      Datum newVal;

      aggstate->curaggcontext = op->d.agg_trans.aggcontext;
      aggstate->current_set = op->d.agg_trans.setno;
      aggstate->curpertrans = pertrans;

      oldContext =
          MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
      fcinfo->args[0].value = pergroup->transValue;
      fcinfo->args[0].isnull = pergroup->transValueIsNull;
      fcinfo->isnull = false;
      newVal = FunctionCallInvoke(fcinfo);
      if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
        newVal = ExecAggCopyTransValue(aggstate, pertrans, newVal,
                                       fcinfo->isnull, pergroup->transValue,
                                       pergroup->transValueIsNull);
      pergroup->transValue = newVal;
      pergroup->transValueIsNull = fcinfo->isnull;
      MemoryContextSwitchTo(oldContext);
    }
    break;
  }

  case EEOP_AGG_PLAIN_TRANS_BYREF: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
    AggStatePerGroup pergroup =
        &aggstate
             ->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    MemoryContext oldContext;
    Datum newVal;

    aggstate->curaggcontext = op->d.agg_trans.aggcontext;
    aggstate->current_set = op->d.agg_trans.setno;
    aggstate->curpertrans = pertrans;

    oldContext =
        MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
    fcinfo->args[0].value = pergroup->transValue;
    fcinfo->args[0].isnull = pergroup->transValueIsNull;
    fcinfo->isnull = false;
    newVal = FunctionCallInvoke(fcinfo);
    if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
      newVal = ExecAggCopyTransValue(aggstate, pertrans, newVal, fcinfo->isnull,
                                     pergroup->transValue,
                                     pergroup->transValueIsNull);
    pergroup->transValue = newVal;
    pergroup->transValueIsNull = fcinfo->isnull;
    MemoryContextSwitchTo(oldContext);
    break;
  }

#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
  case EEOP_AGG_PRESORTED_DISTINCT_SINGLE: {
    AggState *aggstate = castNode(AggState, state->parent);
    AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;

    if (!ExecEvalPreOrderedDistinctSingle(aggstate, pertrans))
      return op->d.agg_presorted_distinctcheck.jumpdistinct;
    break;
  }

  case EEOP_AGG_PRESORTED_DISTINCT_MULTI: {
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

    switch (opcode) {
    case EEOP_INNER_FETCHSOME:
      slot = econtext->ecxt_innertuple;
      break;
    case EEOP_OUTER_FETCHSOME:
      slot = econtext->ecxt_outertuple;
      break;
    case EEOP_SCAN_FETCHSOME:
      slot = econtext->ecxt_scantuple;
      break;
#ifdef HAVE_EEOP_OLD_NEW
    case EEOP_OLD_FETCHSOME:
      slot = econtext->ecxt_oldtuple;
      break;
    case EEOP_NEW_FETCHSOME:
      slot = econtext->ecxt_newtuple;
      break;
#endif
    default:
      slot = econtext->ecxt_scantuple;
      break;
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

    switch (opcode) {
    case EEOP_INNER_VAR:
      slot = econtext->ecxt_innertuple;
      break;
    case EEOP_OUTER_VAR:
      slot = econtext->ecxt_outertuple;
      break;
    case EEOP_SCAN_VAR:
      slot = econtext->ecxt_scantuple;
      break;
#ifdef HAVE_EEOP_OLD_NEW
    case EEOP_OLD_VAR:
      slot = econtext->ecxt_oldtuple;
      break;
    case EEOP_NEW_VAR:
      slot = econtext->ecxt_newtuple;
      break;
#endif
    default:
      slot = econtext->ecxt_scantuple;
      break;
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

    switch (opcode) {
    case EEOP_ASSIGN_INNER_VAR:
      srcslot = econtext->ecxt_innertuple;
      break;
    case EEOP_ASSIGN_OUTER_VAR:
      srcslot = econtext->ecxt_outertuple;
      break;
    case EEOP_ASSIGN_SCAN_VAR:
      srcslot = econtext->ecxt_scantuple;
      break;
#ifdef HAVE_EEOP_OLD_NEW
    case EEOP_ASSIGN_OLD_VAR:
      srcslot = econtext->ecxt_oldtuple;
      break;
    case EEOP_ASSIGN_NEW_VAR:
      srcslot = econtext->ecxt_newtuple;
      break;
#endif
    default:
      srcslot = econtext->ecxt_scantuple;
      break;
    }
    resultslot->tts_values[resultnum] = srcslot->tts_values[attnum];
    resultslot->tts_isnull[resultnum] = srcslot->tts_isnull[attnum];
    break;
  }

  case EEOP_ASSIGN_TMP:
  case EEOP_ASSIGN_TMP_MAKE_RO: {
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

    if (opcode != EEOP_FUNCEXPR) {
      for (int argno = 0; argno < nargs; argno++) {
        if (fcinfo->args[argno].isnull) {
          *op->resnull = true;
          strictfail = true;
          break;
        }
      }
    }
    if (!strictfail) {
      fcinfo->isnull = false;
      *op->resvalue = op->d.func.fn_addr(fcinfo);
      *op->resnull = fcinfo->isnull;
    }
    break;
  }

  case EEOP_BOOL_AND_STEP_FIRST:
  case EEOP_BOOL_AND_STEP:
  case EEOP_BOOL_AND_STEP_LAST: {
    if (opcode == EEOP_BOOL_AND_STEP_FIRST)
      *op->d.boolexpr.anynull = false;

    if (*op->resnull)
      *op->d.boolexpr.anynull = true;
    else if (!DatumGetBool(*op->resvalue)) {
      /* false result, short circuit */
      return op->d.boolexpr.jumpdone;
    }

    if (opcode == EEOP_BOOL_AND_STEP_LAST && *op->d.boolexpr.anynull) {
      *op->resvalue = (Datum)0;
      *op->resnull = true;
    }
    break;
  }

  case EEOP_BOOL_OR_STEP_FIRST:
  case EEOP_BOOL_OR_STEP:
  case EEOP_BOOL_OR_STEP_LAST: {
    if (opcode == EEOP_BOOL_OR_STEP_FIRST)
      *op->d.boolexpr.anynull = false;

    if (*op->resnull)
      *op->d.boolexpr.anynull = true;
    else if (DatumGetBool(*op->resvalue)) {
      /* true result, short circuit */
      return op->d.boolexpr.jumpdone;
    }

    if (opcode == EEOP_BOOL_OR_STEP_LAST && *op->d.boolexpr.anynull) {
      *op->resvalue = (Datum)0;
      *op->resnull = true;
    }
    break;
  }

  case EEOP_BOOL_NOT_STEP:
    *op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));
    break;

  case EEOP_QUAL:
    if (*op->resnull || !DatumGetBool(*op->resvalue)) {
      *op->resvalue = (Datum)0;
      *op->resnull = false;
      return op->d.qualexpr.jumpdone;
    }
    break;

  case EEOP_JUMP:
    return op->d.jump.jumpdone;

  case EEOP_JUMP_IF_NULL:
    if (*op->resnull)
      return op->d.jump.jumpdone;
    break;

  case EEOP_JUMP_IF_NOT_NULL:
    if (!*op->resnull)
      return op->d.jump.jumpdone;
    break;

  case EEOP_JUMP_IF_NOT_TRUE:
    if (*op->resnull || !DatumGetBool(*op->resvalue)) {
      *op->resvalue = BoolGetDatum(false);
      *op->resnull = false;
      return op->d.jump.jumpdone;
    }
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
         (int)opcode);
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

void pg_jitter_agg_trans_init_strict_byval(ExprState *state, ExprEvalStep *op) {
  AggState *aggstate = castNode(AggState, state->parent);
  AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
  AggStatePerGroup pergroup =
      &aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

  if (pergroup->noTransValue) {
    ExecAggInitGroup(aggstate, pertrans, pergroup, op->d.agg_trans.aggcontext);
  } else if (likely(!pergroup->transValueIsNull)) {
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    MemoryContext oldContext;

    aggstate->curaggcontext = op->d.agg_trans.aggcontext;
    aggstate->current_set = op->d.agg_trans.setno;
    aggstate->curpertrans = pertrans;
    oldContext =
        MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
    fcinfo->args[0].value = pergroup->transValue;
    fcinfo->args[0].isnull = pergroup->transValueIsNull;
    fcinfo->isnull = false;
    pergroup->transValue = FunctionCallInvoke(fcinfo);
    pergroup->transValueIsNull = fcinfo->isnull;
    MemoryContextSwitchTo(oldContext);
  }
}

void pg_jitter_agg_trans_strict_byval(ExprState *state, ExprEvalStep *op) {
  AggState *aggstate = castNode(AggState, state->parent);
  AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
  AggStatePerGroup pergroup =
      &aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

  if (likely(!pergroup->transValueIsNull)) {
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    MemoryContext oldContext;

    aggstate->curaggcontext = op->d.agg_trans.aggcontext;
    aggstate->current_set = op->d.agg_trans.setno;
    aggstate->curpertrans = pertrans;
    oldContext =
        MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
    fcinfo->args[0].value = pergroup->transValue;
    fcinfo->args[0].isnull = pergroup->transValueIsNull;
    fcinfo->isnull = false;
    pergroup->transValue = FunctionCallInvoke(fcinfo);
    pergroup->transValueIsNull = fcinfo->isnull;
    MemoryContextSwitchTo(oldContext);
  }
}

void pg_jitter_agg_trans_byval(ExprState *state, ExprEvalStep *op) {
  AggState *aggstate = castNode(AggState, state->parent);
  AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
  AggStatePerGroup pergroup =
      &aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
  FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
  MemoryContext oldContext;

  aggstate->curaggcontext = op->d.agg_trans.aggcontext;
  aggstate->current_set = op->d.agg_trans.setno;
  aggstate->curpertrans = pertrans;
  oldContext =
      MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
  fcinfo->args[0].value = pergroup->transValue;
  fcinfo->args[0].isnull = pergroup->transValueIsNull;
  fcinfo->isnull = false;
  pergroup->transValue = FunctionCallInvoke(fcinfo);
  pergroup->transValueIsNull = fcinfo->isnull;
  MemoryContextSwitchTo(oldContext);
}

void pg_jitter_agg_trans_init_strict_byref(ExprState *state, ExprEvalStep *op) {
  AggState *aggstate = castNode(AggState, state->parent);
  AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
  AggStatePerGroup pergroup =
      &aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

  if (pergroup->noTransValue) {
    ExecAggInitGroup(aggstate, pertrans, pergroup, op->d.agg_trans.aggcontext);
  } else if (likely(!pergroup->transValueIsNull)) {
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    MemoryContext oldContext;
    Datum newVal;

    aggstate->curaggcontext = op->d.agg_trans.aggcontext;
    aggstate->current_set = op->d.agg_trans.setno;
    aggstate->curpertrans = pertrans;
    oldContext =
        MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
    fcinfo->args[0].value = pergroup->transValue;
    fcinfo->args[0].isnull = pergroup->transValueIsNull;
    fcinfo->isnull = false;
    newVal = FunctionCallInvoke(fcinfo);
    if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
      newVal = ExecAggCopyTransValue(aggstate, pertrans, newVal, fcinfo->isnull,
                                     pergroup->transValue,
                                     pergroup->transValueIsNull);
    pergroup->transValue = newVal;
    pergroup->transValueIsNull = fcinfo->isnull;
    MemoryContextSwitchTo(oldContext);
  }
}

void pg_jitter_agg_trans_strict_byref(ExprState *state, ExprEvalStep *op) {
  AggState *aggstate = castNode(AggState, state->parent);
  AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
  AggStatePerGroup pergroup =
      &aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

  if (likely(!pergroup->transValueIsNull)) {
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    MemoryContext oldContext;
    Datum newVal;

    aggstate->curaggcontext = op->d.agg_trans.aggcontext;
    aggstate->current_set = op->d.agg_trans.setno;
    aggstate->curpertrans = pertrans;
    oldContext =
        MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
    fcinfo->args[0].value = pergroup->transValue;
    fcinfo->args[0].isnull = pergroup->transValueIsNull;
    fcinfo->isnull = false;
    newVal = FunctionCallInvoke(fcinfo);
    if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
      newVal = ExecAggCopyTransValue(aggstate, pertrans, newVal, fcinfo->isnull,
                                     pergroup->transValue,
                                     pergroup->transValueIsNull);
    pergroup->transValue = newVal;
    pergroup->transValueIsNull = fcinfo->isnull;
    MemoryContextSwitchTo(oldContext);
  }
}

void pg_jitter_agg_trans_byref(ExprState *state, ExprEvalStep *op) {
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
  oldContext =
      MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
  fcinfo->args[0].value = pergroup->transValue;
  fcinfo->args[0].isnull = pergroup->transValueIsNull;
  fcinfo->isnull = false;
  newVal = FunctionCallInvoke(fcinfo);
  if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
    newVal =
        ExecAggCopyTransValue(aggstate, pertrans, newVal, fcinfo->isnull,
                              pergroup->transValue, pergroup->transValueIsNull);
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
void pg_jitter_agg_byref_finish(AggState *aggstate, AggStatePerTrans pertrans,
                                Datum newVal, AggStatePerGroup pergroup) {
  FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;

  if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
    newVal =
        ExecAggCopyTransValue(aggstate, pertrans, newVal, fcinfo->isnull,
                              pergroup->transValue, pergroup->transValueIsNull);
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
static Datum pg_jitter_run_compiled_expr(ExprState *state,
                                         ExprContext *econtext, bool *isNull) {
  ExprStateEvalFunc func = (ExprStateEvalFunc)state->evalfunc_private;
  Datum result;

  CheckExprStillValid(state, econtext);

  /*
   * After validation, replace the wrapper with the raw JIT function pointer
   * for zero-overhead subsequent calls.
   *
   * On Windows x64, longjmp through JIT frames is safe because we register
   * proper SEH unwind metadata via RtlInstallFunctionTableCallback.
   */
  state->evalfunc = func;

  result = func(state, econtext, isNull);

  return result;
}

void pg_jitter_install_expr(ExprState *state, ExprStateEvalFunc compiled_func) {
  state->evalfunc = pg_jitter_run_compiled_expr;
  state->evalfunc_private = (void *)compiled_func;
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
bool pg_jitter_store_shared_code(void *shared, const void *code, Size code_size,
                                 int node_id, int expr_idx,
                                 uint64 dylib_ref_addr) {
  SharedJitCompiledCode *sjc = (SharedJitCompiledCode *)shared;
  SharedJitCodeEntry *entry;
  Size entry_size;

  entry_size = MAXALIGN(offsetof(SharedJitCodeEntry, code_bytes) + code_size);

  /* Spinlock acquire */
  while (pg_atomic_exchange_u32(&sjc->lock, 1) != 0)
    pg_spin_delay();

  /* Check if there's enough space */
  if (sjc->used + entry_size > sjc->capacity) {
    elog(DEBUG1, "pg_jitter: DSM full: need %zu bytes, have %zu/%zu",
         entry_size, sjc->capacity - sjc->used, sjc->capacity);
    pg_atomic_write_u32(&sjc->lock, 0);
    return false;
  }

  entry = (SharedJitCodeEntry *)((char *)sjc + sjc->used);
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
bool pg_jitter_find_shared_code(void *shared, int node_id, int expr_idx,
                                const void **code_bytes, Size *code_size,
                                uint64 *dylib_ref_addr) {
  SharedJitCompiledCode *sjc = (SharedJitCompiledCode *)shared;
  Size offset;
  int i;

  offset = MAXALIGN(sizeof(SharedJitCompiledCode));

  for (i = 0; i < sjc->num_entries; i++) {
    SharedJitCodeEntry *entry = (SharedJitCodeEntry *)((char *)sjc + offset);

    if (entry->plan_node_id == node_id && entry->expr_index == expr_idx) {
      *code_bytes = entry->code_bytes;
      *code_size = entry->code_size;
      if (dylib_ref_addr)
        *dylib_ref_addr = entry->dylib_ref_addr;
      return true;
    }

    offset +=
        MAXALIGN(offsetof(SharedJitCodeEntry, code_bytes) + entry->code_size);
  }

  return false;
}

/*
 * ExecMemInfo: tracks mmap allocation for proper cleanup.
 * Returned as an opaque handle by pg_jitter_copy_to_executable;
 * callers must use pg_jitter_exec_code_ptr to get the executable address.
 */
typedef struct ExecMemInfo {
  void *code_ptr; /* the executable memory (mmap'd) */
  Size alloc_size;
} ExecMemInfo;

/*
 * Copy code bytes to local executable memory.
 *
 * On macOS ARM64, uses mmap(MAP_JIT) + pthread_jit_write_protect_np for W^X.
 * On Linux, uses mmap + mprotect.
 * Returns an ExecMemInfo handle (use pg_jitter_exec_code_ptr for code address).
 */

void *pg_jitter_copy_to_executable(const void *code_bytes, Size code_size) {
  void *mem;
  ExecMemInfo *info;
  Size alloc_size;

  /*
   * Allocate a fresh MAP_JIT page for the shared code.
   * Each allocation gets its own mmap — avoids interference with sljit's
   * allocator state and ensures clean W^X transitions.
   */
  alloc_size = (code_size + 4095) & ~((Size)4095); /* page-align */

#if defined(__APPLE__) && defined(__aarch64__)
  mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (mem == MAP_FAILED)
    return NULL;

  pthread_jit_write_protect_np(0);
  memcpy(mem, code_bytes, code_size);
  sys_icache_invalidate(mem, code_size);
  pthread_jit_write_protect_np(1);

  /* Verify: read back the code bytes and compare */
  if (memcmp(mem, code_bytes, code_size) != 0)
    elog(WARNING,
         "pg_jitter: VERIFY FAILED — code bytes don't match after copy!");
  else
    elog(DEBUG1, "pg_jitter: copy verified OK (%zu bytes)", code_size);
#else
  mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
             -1, 0);
  if (mem == MAP_FAILED)
    return NULL;

  memcpy(mem, code_bytes, code_size);
  if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
    munmap(mem, alloc_size);
    return NULL;
  }
#if defined(__aarch64__)
  __builtin___clear_cache((char *)mem, (char *)mem + code_size);
#endif
#endif

  info =
      (ExecMemInfo *)MemoryContextAlloc(TopMemoryContext, sizeof(ExecMemInfo));
  info->code_ptr = mem;
  info->alloc_size = alloc_size;

  return info;
}

/*
 * Free executable memory allocated by pg_jitter_copy_to_executable.
 */

void pg_jitter_exec_free(void *ptr) {
  ExecMemInfo *info = (ExecMemInfo *)ptr;

  if (info) {
    munmap(info->code_ptr, info->alloc_size);
    pfree(info);
  }
}

void *pg_jitter_exec_code_ptr(void *handle) {
  ExecMemInfo *info = (ExecMemInfo *)handle;
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
int pg_jitter_relocate_dylib_addrs(void *handle, Size code_size,
                                   uint64 leader_ref_addr,
                                   uint64 worker_ref_addr) {
#if defined(__aarch64__)
  ExecMemInfo *info = (ExecMemInfo *)handle;
  uint32_t *insns;
  int ninsns;
  int patched = 0;
  int64 delta;

  if (!info || leader_ref_addr == worker_ref_addr)
    return 0;

  delta = (int64)(worker_ref_addr - leader_ref_addr);
  insns = (uint32_t *)info->code_ptr;
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
  for (int i = 0; i + 3 < ninsns; i++) {
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
    uint64 addr;
    addr = (uint64)((w0 >> 5) & 0xFFFF);
    addr |= (uint64)((w1 >> 5) & 0xFFFF) << 16;
    addr |= (uint64)((w2 >> 5) & 0xFFFF) << 32;
    addr |= (uint64)((w3 >> 5) & 0xFFFF) << 48;

    /*
     * Check if this address is within the dylib's address range.
     * The dylib is ~512KB, so any function should be within ±512KB
     * of the reference (pg_jitter_fallback_step).
     */
    int64 offset_from_ref = (int64)(addr - leader_ref_addr);

    /* Check what instruction follows the MOVZ+3×MOVK sequence */
    uint32_t w4 = (i + 4 < ninsns) ? insns[i + 4] : 0;
    bool is_blr = (w4 & 0xFFFFFC1F) == (0xD63F0000 | rd); /* BLR Xrd */
    bool is_br = (w4 & 0xFFFFFC1F) == (0xD61F0000 | rd);  /* BR Xrd */

    elog(DEBUG1,
         "pg_jitter: relocate scan insn[%d] rd=x%d addr=%lx "
         "offset_from_ref=%ld next_insn=%08x is_call=%d",
         i, rd, (unsigned long)addr, (long)offset_from_ref, w4,
         (int)(is_blr || is_br));

    if (offset_from_ref >= -0x80000 && offset_from_ref <= 0x80000) {
      /* Relocate: add delta to get worker's address */
      uint64 new_addr = addr + delta;

      elog(DEBUG1,
           "pg_jitter: relocate PATCH insn[%d] rd=x%d "
           "old=%lx new=%lx delta=%ld",
           i, rd, (unsigned long)addr, (unsigned long)new_addr, (long)delta);

      /* Patch the 4 instructions with new imm16 values */
      insns[i] = (w0 & 0xFFE0001F) | (((new_addr >> 0) & 0xFFFF) << 5);
      insns[i + 1] = (w1 & 0xFFE0001F) | (((new_addr >> 16) & 0xFFFF) << 5);
      insns[i + 2] = (w2 & 0xFFE0001F) | (((new_addr >> 32) & 0xFFFF) << 5);
      insns[i + 3] = (w3 & 0xFFE0001F) | (((new_addr >> 48) & 0xFFFF) << 5);

      patched++;
      i += 3; /* skip the 3 MOVK instructions */
    }
  }

  if (patched > 0) {
    /* Flush I-cache after patching */
#if defined(__APPLE__)
    sys_icache_invalidate(info->code_ptr, code_size);
#else
    __builtin___clear_cache((char *)info->code_ptr,
                            (char *)info->code_ptr + code_size);
#endif
  }

#if defined(__APPLE__)
  /* Restore exec mode */
  pthread_jit_write_protect_np(1);
#endif

  return patched;
#elif defined(__x86_64__) || defined(_M_X64)
  ExecMemInfo *info = (ExecMemInfo *)handle;
  uint8_t *bytes;
  int patched = 0;
  int64 delta;
  Size alloc_size;

  if (!info || leader_ref_addr == worker_ref_addr)
    return 0;

  delta = (int64)(worker_ref_addr - leader_ref_addr);
  bytes = (uint8_t *)info->code_ptr;
  alloc_size = info->alloc_size;

  /*
   * Make memory writable for patching.
   * pg_jitter_copy_to_executable() sets PROT_READ|PROT_EXEC on Linux.
   */
  if (mprotect(info->code_ptr, alloc_size, PROT_READ | PROT_WRITE) != 0) {
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
  for (Size i = 0; i + 9 < code_size; i++) {
    uint8_t rex = bytes[i];
    uint8_t opc = bytes[i + 1];

    /* Check REX.W prefix (0x48 or 0x49) */
    if (rex != 0x48 && rex != 0x49)
      continue;

    /* Check MOV r, imm64 opcode: 0xB8-0xBF */
    if ((opc & 0xF8) != 0xB8)
      continue;

    /* Extract 8-byte little-endian immediate from bytes[i+2..i+9] */
    uint64 addr;
    memcpy(&addr, &bytes[i + 2], sizeof(uint64));

    /*
     * Check if this address is within the dylib's address range.
     * Same heuristic as ARM64: ±512KB of the reference address.
     */
    int64 offset_from_ref = (int64)(addr - leader_ref_addr);

    elog(DEBUG1,
         "pg_jitter: relocate scan byte[%zu] rex=0x%02x opc=0x%02x "
         "addr=%lx offset_from_ref=%ld",
         i, rex, opc, (unsigned long)addr, (long)offset_from_ref);

    if (offset_from_ref >= -0x80000 && offset_from_ref <= 0x80000) {
      /* Relocate: add delta to get worker's address */
      uint64 new_addr = addr + delta;

      elog(DEBUG1,
           "pg_jitter: relocate PATCH byte[%zu] "
           "old=%lx new=%lx delta=%ld",
           i, (unsigned long)addr, (unsigned long)new_addr, (long)delta);

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
void pg_jitter_get_expr_identity(PgJitterContext *ctx, ExprState *state,
                                 int *node_id, int *expr_idx) {
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
 * Leader: create DSM for shared JIT code and store handle in shmem slot.
 */
void pg_jitter_init_shared_dsm(PgJitterContext *ctx) {
  Size dsm_size;
  dsm_handle handle;
  int proc_index;

  if (ctx->share_state.initialized)
    return;

  /* If shmem slot table is unavailable, skip DSM (fall back to per-worker) */
  if (!pg_jitter_shmem_available()) {
    elog(DEBUG1, "pg_jitter: shmem unavailable, skipping shared DSM");
    return;
  }

  dsm_size = (Size)pg_jitter_get_shared_code_max_kb() * 1024;

  ctx->share_state.dsm_seg = dsm_create(dsm_size, 0);
  dsm_pin_mapping(ctx->share_state.dsm_seg);

  ctx->share_state.sjc =
      (SharedJitCompiledCode *)dsm_segment_address(ctx->share_state.dsm_seg);

  /* Initialize header */
  pg_atomic_init_u32(&ctx->share_state.sjc->lock, 0);
  ctx->share_state.sjc->num_entries = 0;
  ctx->share_state.sjc->capacity = dsm_size;
  ctx->share_state.sjc->used = MAXALIGN(sizeof(SharedJitCompiledCode));

  /* Store handle in shmem slot for worker discovery */
  handle = dsm_segment_handle(ctx->share_state.dsm_seg);
  proc_index = JITTER_MY_PROC_INDEX();
  pg_jitter_shmem_set_dsm_handle(proc_index, handle);

  ctx->share_state.is_leader = true;
  ctx->share_state.initialized = true;

  elog(DEBUG1,
       "pg_jitter: leader created shared DSM handle=%u size=%zu "
       "proc_index=%d",
       handle, dsm_size, proc_index);
}

/*
 * Worker: read DSM handle from leader's shmem slot and attach.
 */
void pg_jitter_attach_shared_dsm(PgJitterContext *ctx) {
  dsm_handle handle;
  int leader_index;

  if (ctx->share_state.initialized)
    return;

  leader_index = JITTER_LEADER_PROC_INDEX();
  handle = pg_jitter_shmem_get_dsm_handle(leader_index);
  if (handle == 0) {
    elog(DEBUG1, "pg_jitter: worker found no shared DSM in leader slot %d",
         leader_index);
    return;
  }

  /*
   * If this handle is already mapped by another JitContext (e.g. the outer
   * query's context when fmgr_sql evaluates a SQL function body), skip
   * attachment.  The DSM entries belong to the outer plan; this nested
   * context's plan_node_ids would produce false matches.  Fall through to
   * local (per-worker) compilation instead.
   */
  if (dsm_find_mapping(handle) != NULL) {
    elog(DEBUG1,
         "pg_jitter: worker DSM handle=%u already mapped, "
         "skipping nested attach",
         handle);
    return;
  }

  ctx->share_state.dsm_seg = dsm_attach(handle);
  if (ctx->share_state.dsm_seg == NULL) {
    elog(DEBUG1, "pg_jitter: worker failed to attach DSM handle=%u", handle);
    return;
  }
  dsm_pin_mapping(ctx->share_state.dsm_seg);

  ctx->share_state.sjc =
      (SharedJitCompiledCode *)dsm_segment_address(ctx->share_state.dsm_seg);
  ctx->share_state.is_leader = false;
  ctx->share_state.initialized = true;

  elog(DEBUG1,
       "pg_jitter: worker attached to shared DSM handle=%u "
       "entries=%d used=%zu/%zu (leader slot %d)",
       handle, ctx->share_state.sjc->num_entries, ctx->share_state.sjc->used,
       ctx->share_state.sjc->capacity, leader_index);
}

/*
 * Cleanup: clear shmem slot, unpin mapping and detach from DSM.
 */
void pg_jitter_cleanup_shared_dsm(PgJitterContext *ctx) {
  if (!ctx->share_state.initialized)
    return;

  /* Clear our shmem slot so workers don't find a stale handle */
  if (ctx->share_state.is_leader)
    pg_jitter_shmem_clear_dsm_handle(JITTER_MY_PROC_INDEX());

  /* Reset shared deform mmap before detaching DSM */
#ifdef PG_JITTER_HAVE_SLJIT_DEFORM
  pg_jitter_reset_shared_deform();
#endif

  if (ctx->share_state.dsm_seg) {
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
 * Unrolled deform emits ~140 bytes per column.  When the total code
 * exceeds the L1I budget, instruction-cache thrashing makes JIT slower
 * than the interpreter.  The loop-based deform emits ~530 bytes
 * regardless of column count.
 *
 * Architecture-specific tuning:
 *
 *   ARM64 (Apple M1–M3)     — 192 KB L1I, 64 KB L1D
 *     Unrolled threshold: 192K / 140 / 2 ≈ 468 columns.
 *     Wide tables benefit from the large I-cache; unrolled deform
 *     stays profitable up to ~460 columns.
 *
 *   ARM64 (Apple M4+)       — 192 KB L1I, 128 KB L1D
 *     Unrolled threshold: same ≈ 468 columns.
 *
 *   ARM64 (Graviton 2/3)    — 64 KB L1I, 64 KB L1D
 *     Unrolled threshold: 64K / 140 / 2 ≈ 234 columns.
 *
 *   ARM64 (Ampere Altra)    — 64 KB L1I, 64 KB L1D
 *     Unrolled threshold: 64K / 140 / 2 ≈ 234 columns.
 *
 *   ARM64 (AmpereOne)       — 16 KB L1I, 64 KB L1D
 *     Unrolled threshold: 16K / 140 / 2 ≈ 57 columns.
 *     Tiny I-cache forces early switch to loop-based deform.
 *
 *   x86-64 (Intel/AMD)      — 32 KB L1I, 32–48 KB L1D
 *     Unrolled threshold: 32K / 140 / 2 ≈ 117 columns.
 *     Smaller I-cache means unrolled code hurts sooner.
 */
int pg_jitter_deform_threshold(void) {
  static int threshold = 0;

  if (threshold > 0)
    return threshold;

  {
    long l1i_size = 0;

#ifdef __linux__
    l1i_size = sysconf(_SC_LEVEL1_ICACHE_SIZE);
    if (l1i_size <= 0)
      l1i_size = 32768;
#elif defined(__APPLE__)
    {
      size_t len = sizeof(l1i_size);
      if (sysctlbyname("hw.l1icachesize", &l1i_size, &len, NULL, 0) != 0)
        l1i_size = 32768;
    }
#else
    l1i_size = 32768;
#endif

    threshold = l1i_size / DEFORM_BYTES_PER_COL / 2;
    if (threshold < 16)
      threshold = 16; /* never go below 16 columns */

    return threshold;
  }
}

/*
 * pg_jitter_wide_deform_limit — hard cap on JIT deform column count.
 *
 * Above this limit, JIT deform (both unrolled and loop-based) is
 * slower than the interpreter's slot_getsomeattrs_int().  The
 * crossover depends on per-tuple iteration cost, which scales with
 * the number of columns and is bounded by data-cache capacity.
 *
 * The dispatch-table general loop uses a 1-byte dispatch array
 * (vs 8-byte descriptors) and specialized handlers with all
 * properties as compile-time immediates.  Benchmarks at 1000 cols:
 *   Wide1000_last: JIT 112ms vs interpreter 109ms (3% slower)
 *   Wide1000_grpby: JIT 116ms vs interpreter 111ms (4% slower)
 *
 * The crossover is around 700-800 columns.  We set the limit at
 * ~680 to safely cover 300-col tables with margin.
 *
 * Architecture-specific limits:
 *
 *   ARM64 (Apple M1–M3)     — 64 KB L1D → 682.
 *   ARM64 (Apple M4+)       — 128 KB L1D → 1365.
 *   ARM64 (Graviton 2/3)    — 64 KB L1D → 682.
 *   x86-64 (Skylake, Zen 3) — 32 KB L1D → 682.
 *   x86-64 (Ice Lake+, Zen 4+) — 48 KB L1D → 1024.
 */
int pg_jitter_wide_deform_limit(void) {
  static int limit = 0;

  if (limit > 0)
    return limit;

  {
    long l1d_size = 0;

#ifdef __linux__
    l1d_size = sysconf(_SC_LEVEL1_DCACHE_SIZE);
#elif defined(__APPLE__)
    {
      size_t len = sizeof(l1d_size);
      sysctlbyname("hw.l1dcachesize", &l1d_size, &len, NULL, 0);
    }
#elif defined(_WIN32)
    {
      SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf = NULL;
      DWORD sz = 0;
      GetLogicalProcessorInformation(NULL, &sz);
      buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(sz);
      if (buf && GetLogicalProcessorInformation(buf, &sz)) {
        DWORD n = sz / sizeof(*buf);
        for (DWORD i = 0; i < n; i++) {
          if (buf[i].Relationship == RelationCache &&
              buf[i].Cache.Level == 1 &&
              buf[i].Cache.Type == CacheData) {
            l1d_size = buf[i].Cache.Size;
            break;
          }
        }
      }
      free(buf);
    }
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    /*
     * ARM64: efficient fixed-width instructions, generous caches.
     * Apple M1-M3: 64 KB → 682,  M4+: 128 KB → 1365.
     * Graviton: 64 KB → 682.
     */
    if (l1d_size <= 0)
      l1d_size = 65536;           /* assume 64 KB if detection fails */
    limit = l1d_size / 96;
#elif defined(__x86_64__) || defined(_M_X64)
    /*
     * x86-64: variable-length encoding, more register pressure.
     * Skylake/Zen3: 32 KB → 682,  Ice Lake/Zen4+: 48 KB → 1024.
     */
    if (l1d_size <= 0)
      l1d_size = 32768;           /* assume 32 KB if detection fails */
    limit = l1d_size / 48;
#else
    if (l1d_size <= 0)
      l1d_size = 32768;
    limit = l1d_size / 96;
#endif

    if (limit < 100)
      limit = 100;                /* never go below 100 columns */

    return limit;
  }
}

/*
 * pg_jitter_collation_is_c — check if a collation OID resolves to C/POSIX.
 *
 * Returns true for C_COLLATION_OID (950), and also for DEFAULT_COLLATION_OID
 * (100) when the database default collation is C.  This is needed because
 * expressions like `col ILIKE 'pattern'` use DEFAULT_COLLATION_OID even on
 * databases created with --no-locale (C collation).
 */
bool
pg_jitter_collation_is_c(Oid collid)
{
  static Oid cached_collid = InvalidOid;
  static bool cached_result = false;

  if (collid == InvalidOid)
    return false;       /* unresolved collation — let V1 raise error */
  if (collid == C_COLLATION_OID)
    return true;
  if (collid == cached_collid)
    return cached_result;

#if PG_VERSION_NUM >= 180000
  {
    pg_locale_t locale = pg_newlocale_from_collation(collid);
    cached_result = locale->collate_is_c;
  }
#else
  cached_result = lc_collate_is_c(collid);
#endif
  cached_collid = collid;
  return cached_result;
}

/*
 * pg_jitter_collation_is_deterministic — check if collation is deterministic.
 *
 * LIKE and regex matching in PostgreSQL are byte-level operations that produce
 * correct results for any deterministic collation. This is used to gate the
 * StringZilla/PCRE2 LIKE/regex fast paths, which are also byte-level.
 */
bool
pg_jitter_collation_is_deterministic(Oid collid)
{
  static Oid cached_collid = InvalidOid;
  static bool cached_result = false;

  /* Unresolved collation — fall through to V1 to raise proper error */
  if (collid == InvalidOid)
    return false;
  /* C and POSIX are always deterministic */
  if (collid == C_COLLATION_OID)
    return true;
  if (collid == cached_collid)
    return cached_result;

#if PG_VERSION_NUM >= 120000
  {
    pg_locale_t locale = pg_newlocale_from_collation(collid);
    /*
     * On PG12-17, pg_newlocale_from_collation() returns NULL for the
     * default libc collation.  NULL means deterministic (byte-equal).
     * PG18+ always returns a valid locale struct.
     */
    cached_result = (locale == NULL) ? true : locale->deterministic;
  }
#else
  /* PG < 12 has no non-deterministic collations */
  cached_result = true;
#endif
  cached_collid = collid;
  return cached_result;
}

/* ----------------------------------------------------------------
 * CASE expression binary search optimization
 *
 * Detects monotonic CASE patterns and replaces linear O(N) branch
 * scanning with O(log N) binary search.
 * ---------------------------------------------------------------- */

/*
 * Comparison kind enumeration for classify_cmp_fn.
 */
typedef enum CmpKind {
  CMP_EQ, CMP_LT, CMP_LE, CMP_GT, CMP_GE
} CmpKind;

/*
 * Static table entry mapping PGFunction address to (CmpKind, CaseBSearchType).
 * Built lazily on first call to classify_cmp_fn.
 */
typedef struct CmpClassEntry {
  PGFunction      fn;
  CmpKind         kind;
  CaseBSearchType bs_type;
} CmpClassEntry;

static CmpClassEntry *cmp_class_entries = NULL;
static int cmp_class_nentries = 0;

/*
 * Helper: add one entry to the classification table.
 */
static inline void
add_cmp_entry(CmpClassEntry *e, int *n, PGFunction fn,
              CmpKind kind, CaseBSearchType bs_type)
{
  e[*n].fn = fn;
  e[*n].kind = kind;
  e[*n].bs_type = bs_type;
  (*n)++;
}

/*
 * Helper: add a 5-entry {eq, lt, le, gt, ge} group.
 */
static void
add_cmp_group(CmpClassEntry *e, int *n,
              PGFunction eq_fn, PGFunction lt_fn, PGFunction le_fn,
              PGFunction gt_fn, PGFunction ge_fn, CaseBSearchType bs_type)
{
  if (eq_fn) add_cmp_entry(e, n, eq_fn, CMP_EQ, bs_type);
  if (lt_fn) add_cmp_entry(e, n, lt_fn, CMP_LT, bs_type);
  if (le_fn) add_cmp_entry(e, n, le_fn, CMP_LE, bs_type);
  if (gt_fn) add_cmp_entry(e, n, gt_fn, CMP_GT, bs_type);
  if (ge_fn) add_cmp_entry(e, n, ge_fn, CMP_GE, bs_type);
}

/*
 * Build the classification table using PG function addresses directly.
 * Called once, results cached in static variables.
 */
static void
build_cmp_class_table(void)
{
  CmpClassEntry *e = malloc(sizeof(CmpClassEntry) * 128);
  int n = 0;

  /* --- Tier 1: byval types --- */

  /* int4 → CASE_BS_I32 */
  add_cmp_group(e, &n, (PGFunction)int4eq, (PGFunction)int4lt,
                (PGFunction)int4le, (PGFunction)int4gt,
                (PGFunction)int4ge, CASE_BS_I32);

  /* int8 → CASE_BS_I64 */
  add_cmp_group(e, &n, (PGFunction)int8eq, (PGFunction)int8lt,
                (PGFunction)int8le, (PGFunction)int8gt,
                (PGFunction)int8ge, CASE_BS_I64);

  /* int2 → CASE_BS_I16 */
  add_cmp_group(e, &n, (PGFunction)int2eq, (PGFunction)int2lt,
                (PGFunction)int2le, (PGFunction)int2gt,
                (PGFunction)int2ge, CASE_BS_I16);

  /* date → CASE_BS_I32 (DateADT = signed int32) */
  add_cmp_group(e, &n, (PGFunction)date_eq, (PGFunction)date_lt,
                (PGFunction)date_le, (PGFunction)date_gt,
                (PGFunction)date_ge, CASE_BS_I32);

  /* oid → CASE_BS_U32 */
  add_cmp_group(e, &n, (PGFunction)oideq, (PGFunction)oidlt,
                (PGFunction)oidle, (PGFunction)oidgt,
                (PGFunction)oidge, CASE_BS_U32);

  /* timestamp → CASE_BS_I64 */
  add_cmp_group(e, &n, (PGFunction)timestamp_eq, (PGFunction)timestamp_lt,
                (PGFunction)timestamp_le, (PGFunction)timestamp_gt,
                (PGFunction)timestamp_ge, CASE_BS_I64);

  /* float4 → CASE_BS_F4 */
  add_cmp_group(e, &n, (PGFunction)float4eq, (PGFunction)float4lt,
                (PGFunction)float4le, (PGFunction)float4gt,
                (PGFunction)float4ge, CASE_BS_F4);

  /* float8 → CASE_BS_F8 */
  add_cmp_group(e, &n, (PGFunction)float8eq, (PGFunction)float8lt,
                (PGFunction)float8le, (PGFunction)float8gt,
                (PGFunction)float8ge, CASE_BS_F8);

  /* bool → CASE_BS_I64 */
  add_cmp_group(e, &n, (PGFunction)booleq, (PGFunction)boollt,
                (PGFunction)boolle, (PGFunction)boolgt,
                (PGFunction)boolge, CASE_BS_I64);

  /* --- Tier 2: byref types → CASE_BS_GENERIC --- */

  /* text */
  add_cmp_group(e, &n, (PGFunction)texteq, (PGFunction)text_lt,
                (PGFunction)text_le, (PGFunction)text_gt,
                (PGFunction)text_ge, CASE_BS_GENERIC);

  /* numeric */
  add_cmp_group(e, &n, (PGFunction)numeric_eq, (PGFunction)numeric_lt,
                (PGFunction)numeric_le, (PGFunction)numeric_gt,
                (PGFunction)numeric_ge, CASE_BS_GENERIC);

  /* uuid */
  add_cmp_group(e, &n, (PGFunction)uuid_eq, (PGFunction)uuid_lt,
                (PGFunction)uuid_le, (PGFunction)uuid_gt,
                (PGFunction)uuid_ge, CASE_BS_GENERIC);

  /* interval */
  add_cmp_group(e, &n, (PGFunction)interval_eq, (PGFunction)interval_lt,
                (PGFunction)interval_le, (PGFunction)interval_gt,
                (PGFunction)interval_ge, CASE_BS_GENERIC);

  /* bpchar */
  add_cmp_group(e, &n, (PGFunction)bpchareq, (PGFunction)bpcharlt,
                (PGFunction)bpcharle, (PGFunction)bpchargt,
                (PGFunction)bpcharge, CASE_BS_GENERIC);

  /* bytea */
  add_cmp_group(e, &n, (PGFunction)byteaeq, (PGFunction)bytealt,
                (PGFunction)byteale, (PGFunction)byteagt,
                (PGFunction)byteage, CASE_BS_GENERIC);

  /* jsonb */
  add_cmp_group(e, &n, (PGFunction)jsonb_eq, (PGFunction)jsonb_lt,
                (PGFunction)jsonb_le, (PGFunction)jsonb_gt,
                (PGFunction)jsonb_ge, CASE_BS_GENERIC);

  /* network */
  add_cmp_group(e, &n, (PGFunction)network_eq, (PGFunction)network_lt,
                (PGFunction)network_le, (PGFunction)network_gt,
                (PGFunction)network_ge, CASE_BS_GENERIC);

  /* array */
  add_cmp_group(e, &n, (PGFunction)array_eq, (PGFunction)array_lt,
                (PGFunction)array_le, (PGFunction)array_gt,
                (PGFunction)array_ge, CASE_BS_GENERIC);

  cmp_class_entries = e;
  cmp_class_nentries = n;
}

/*
 * find_generic_lt_fn — for a CASE_BS_GENERIC equality function, find the
 * corresponding _lt function for binary search ordering.
 * Returns NULL if no lt function is found (fallback to linear scan).
 */
static PGFunction
find_generic_lt_fn(PGFunction eq_fn)
{
  static struct { PGFunction eq; PGFunction lt; } map[] = {
    {(PGFunction)texteq,       (PGFunction)text_lt},
    {(PGFunction)numeric_eq,   (PGFunction)numeric_lt},
    {(PGFunction)uuid_eq,      (PGFunction)uuid_lt},
    {(PGFunction)interval_eq,  (PGFunction)interval_lt},
    {(PGFunction)bpchareq,     (PGFunction)bpcharlt},
    {(PGFunction)byteaeq,      (PGFunction)bytealt},
    {(PGFunction)jsonb_eq,     (PGFunction)jsonb_lt},
    {(PGFunction)network_eq,   (PGFunction)network_lt},
    {(PGFunction)array_eq,     (PGFunction)array_lt},
  };
  for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
    if (map[i].eq == eq_fn)
      return map[i].lt;
  return NULL;
}

/*
 * classify_cmp_fn — classify a comparison function for CASE binary search.
 *
 * Matches against known PG comparison functions.  Returns true if the
 * function is a supported comparison, filling out the category fields.
 */
static bool
classify_cmp_fn(PGFunction fn, const JitDirectFn *dfn,
                bool *is_equality, bool *is_less, bool *is_inclusive,
                CaseBSearchType *bs_type)
{
  if (cmp_class_entries == NULL)
    build_cmp_class_table();

  /* Linear scan — table is small (~90 entries), called once per CASE branch */
  for (int i = 0; i < cmp_class_nentries; i++)
  {
    if (cmp_class_entries[i].fn == fn)
    {
      CmpKind k = cmp_class_entries[i].kind;
      *bs_type = cmp_class_entries[i].bs_type;
      *is_equality = (k == CMP_EQ);
      *is_less = (k == CMP_LT || k == CMP_LE);
      *is_inclusive = (k == CMP_LE || k == CMP_GE);
      return true;
    }
  }

  return false;
}

/*
 * Check if an opcode is a VAR load (INNER_VAR, OUTER_VAR, SCAN_VAR).
 */
static bool
is_var_opcode(ExprEvalOp op)
{
  return op == EEOP_INNER_VAR || op == EEOP_OUTER_VAR || op == EEOP_SCAN_VAR;
}

/*
 * Check if two VAR steps load the same variable (same opcode + same attnum).
 */
static bool
same_var(ExprState *state, ExprEvalStep *a, ExprEvalStep *b)
{
  ExprEvalOp opa = ExecEvalStepOp(state, a);
  ExprEvalOp opb = ExecEvalStepOp(state, b);
  if (opa != opb)
    return false;
  return a->d.var.attnum == b->d.var.attnum;
}

/*
 * Check if an opcode is FUNCEXPR_STRICT (including PG18 _1/_2 variants).
 */
static bool
is_funcexpr_strict(ExprEvalOp op)
{
  if (op == EEOP_FUNCEXPR_STRICT)
    return true;
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
  if (op == EEOP_FUNCEXPR_STRICT_1 || op == EEOP_FUNCEXPR_STRICT_2)
    return true;
#endif
  return false;
}

/*
 * Comparator for qsort: sort (threshold, result) pairs by threshold for
 * Pattern B (equality). Operates on parallel arrays via index permutation.
 */
typedef struct {
  Datum threshold;
  Datum result;
} ThresholdResult;

static int
cmp_threshold_result_i32(const void *a, const void *b)
{
  int32 va = DatumGetInt32(((const ThresholdResult *)a)->threshold);
  int32 vb = DatumGetInt32(((const ThresholdResult *)b)->threshold);
  return (va > vb) - (va < vb);
}

static int
cmp_threshold_result_i64(const void *a, const void *b)
{
  int64 va = DatumGetInt64(((const ThresholdResult *)a)->threshold);
  int64 vb = DatumGetInt64(((const ThresholdResult *)b)->threshold);
  return (va > vb) - (va < vb);
}

static int
cmp_threshold_result_u32(const void *a, const void *b)
{
  uint32 va = DatumGetUInt32(((const ThresholdResult *)a)->threshold);
  uint32 vb = DatumGetUInt32(((const ThresholdResult *)b)->threshold);
  return (va > vb) - (va < vb);
}

static int
cmp_threshold_result_f4(const void *a, const void *b)
{
  float4 va = DatumGetFloat4(((const ThresholdResult *)a)->threshold);
  float4 vb = DatumGetFloat4(((const ThresholdResult *)b)->threshold);
  /* NaN-aware: NaN sorts after everything */
  if (isnan(va)) return isnan(vb) ? 0 : 1;
  if (isnan(vb)) return -1;
  return (va > vb) - (va < vb);
}

static int
cmp_threshold_result_f8(const void *a, const void *b)
{
  float8 va = DatumGetFloat8(((const ThresholdResult *)a)->threshold);
  float8 vb = DatumGetFloat8(((const ThresholdResult *)b)->threshold);
  if (isnan(va)) return isnan(vb) ? 0 : 1;
  if (isnan(vb)) return -1;
  return (va > vb) - (va < vb);
}

/* Context for generic threshold comparator */
typedef struct {
  PGFunction  lt_fn;
  Oid         collation;
} GenericCmpCtx;

static int
cmp_threshold_result_generic(const void *a, const void *b, void *arg)
{
  GenericCmpCtx *ctx = (GenericCmpCtx *)arg;
  Datum da = ((const ThresholdResult *)a)->threshold;
  Datum db = ((const ThresholdResult *)b)->threshold;
  LOCAL_FCINFO(fcinfo, 2);
  InitFunctionCallInfoData(*fcinfo, NULL, 2, ctx->collation, NULL, NULL);
  fcinfo->args[0].value = da;
  fcinfo->args[0].isnull = false;
  fcinfo->args[1].value = db;
  fcinfo->args[1].isnull = false;
  /* a < b → -1 */
  if (DatumGetBool(ctx->lt_fn(fcinfo)))
    return -1;
  /* b < a → +1 */
  fcinfo->args[0].value = db;
  fcinfo->args[1].value = da;
  if (DatumGetBool(ctx->lt_fn(fcinfo)))
    return 1;
  return 0;
}

/* memcmp-based comparator for text Datums — used for deterministic non-C
 * collations where we only need a consistent total order for binary search. */
static int
cmp_threshold_result_text_memcmp(const void *a, const void *b)
{
  Datum da = ((const ThresholdResult *)a)->threshold;
  Datum db = ((const ThresholdResult *)b)->threshold;
  text *t1 = DatumGetTextPP(da);
  text *t2 = DatumGetTextPP(db);
  int len1 = VARSIZE_ANY_EXHDR(t1);
  int len2 = VARSIZE_ANY_EXHDR(t2);
  int minlen = (len1 < len2) ? len1 : len2;
  int cmp = memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2), minlen);
  if (cmp == 0)
    cmp = (len1 > len2) - (len1 < len2);
  return cmp;
}

/*
 * pg_jitter_detect_case_bsearch — scan ExprEvalStep array for CASE patterns
 * suitable for binary search optimization.
 *
 * Detects two patterns:
 *   Pattern A: searched CASE with monotonic < / <= / > / >= comparisons
 *   Pattern B: simple CASE with = comparisons (values sorted at detect time)
 *
 * Each pattern has 5 steps per branch:
 *   [i+0] VAR or CASE_TESTVAL
 *   [i+1] EEOP_FUNCEXPR_STRICT (comparison; threshold baked into fcinfo->args[1])
 *   [i+2] EEOP_JUMP_IF_NOT_TRUE (→ next branch)
 *   [i+3] EEOP_CONST (result)
 *   [i+4] EEOP_JUMP (→ end of CASE)
 *
 * Note: PG's ExecInitFunc() assigns Const argument values directly into
 * fcinfo->args[] WITHOUT generating EEOP_CONST steps. The threshold for
 * comparison is therefore in steps[i+1].d.func.fcinfo_data->args[1].value.
 *
 * Returns number of detected patterns.
 */
int
pg_jitter_detect_case_bsearch(ExprState *state,
                               ExprEvalStep *steps, int steps_len,
                               CaseBSearchInfo *out, int max_patterns)
{
  int nfound = 0;
  int i = 0;
  const int STRIDE = 5;  /* steps per branch */

  while (i < steps_len - STRIDE && nfound < max_patterns)
  {
    ExprEvalOp op0 = ExecEvalStepOp(state, &steps[i]);
    ExprEvalOp op1, op2, op3, op4;
    bool is_var_pattern, is_case_testval;
    const JitDirectFn *dfn;
    bool is_equality, is_less, is_inclusive;
    CaseBSearchType bs_type;
    int8 var_type;
    PGFunction cmp_fn;
    int branch_count;
    int end_jump_target;
    int j;

    /* Step 0 must be a VAR load or CASE_TESTVAL */
    is_var_pattern = is_var_opcode(op0);
    is_case_testval = (op0 == EEOP_CASE_TESTVAL);
    if (!is_var_pattern && !is_case_testval)
    {
      i++;
      continue;
    }

    /* Check remaining 4 steps of first branch exist */
    if (i + 4 >= steps_len)
      break;

    op1 = ExecEvalStepOp(state, &steps[i + 1]);
    op2 = ExecEvalStepOp(state, &steps[i + 2]);
    op3 = ExecEvalStepOp(state, &steps[i + 3]);
    op4 = ExecEvalStepOp(state, &steps[i + 4]);

    /* Verify first branch structure:
     * [+1] FUNCEXPR_STRICT, [+2] JINT, [+3] CONST(result), [+4] JUMP */
    if (!is_funcexpr_strict(op1) ||
        op2 != EEOP_JUMP_IF_NOT_TRUE || op3 != EEOP_CONST ||
        op4 != EEOP_JUMP)
    {
      i++;
      continue;
    }

    /* Result CONST at i+3 must be non-null */
    if (steps[i + 3].d.constval.isnull)
    {
      i++;
      continue;
    }

    /* Threshold is baked into fcinfo->args[1] — must be non-null */
    if (steps[i + 1].d.func.fcinfo_data->args[1].isnull)
    {
      i++;
      continue;
    }

    /* Identify comparison function */
    cmp_fn = steps[i + 1].d.func.fn_addr;
    dfn = jit_find_direct_fn(cmp_fn);
    if (!dfn || dfn->nargs != 2)
    {
      i++;
      continue;
    }

    /* Classify the comparison */
    if (!classify_cmp_fn(cmp_fn, dfn, &is_equality, &is_less,
                         &is_inclusive, &bs_type))
    {
      i++;
      continue;
    }
    var_type = bs_type_to_var_type(bs_type);

    /* Record end-of-CASE jump target from first branch */
    end_jump_target = steps[i + 4].d.jump.jumpdone;

    /* Count consecutive branches with same structure */
    branch_count = 1;
    j = i + STRIDE;

    while (j + STRIDE - 1 < steps_len)
    {
      ExprEvalOp bop0 = ExecEvalStepOp(state, &steps[j]);
      ExprEvalOp bop1 = ExecEvalStepOp(state, &steps[j + 1]);
      ExprEvalOp bop2 = ExecEvalStepOp(state, &steps[j + 2]);
      ExprEvalOp bop3 = ExecEvalStepOp(state, &steps[j + 3]);
      ExprEvalOp bop4 = ExecEvalStepOp(state, &steps[j + 4]);

      /* Same 5-step structure */
      if (!is_funcexpr_strict(bop1) ||
          bop2 != EEOP_JUMP_IF_NOT_TRUE || bop3 != EEOP_CONST ||
          bop4 != EEOP_JUMP)
        break;

      /* Step 0: same kind of load */
      if (is_var_pattern)
      {
        if (!is_var_opcode(bop0) || !same_var(state, &steps[i], &steps[j]))
          break;
      }
      else
      {
        if (bop0 != EEOP_CASE_TESTVAL)
          break;
      }

      /* Same comparison function */
      if (steps[j + 1].d.func.fn_addr != cmp_fn)
        break;

      /* Threshold must be non-null */
      if (steps[j + 1].d.func.fcinfo_data->args[1].isnull)
        break;

      /* Result CONST must be non-null */
      if (steps[j + 3].d.constval.isnull)
        break;

      /* Same end-of-CASE jump target */
      if (steps[j + 4].d.jump.jumpdone != end_jump_target)
        break;

      /* JUMP_IF_NOT_TRUE must point to next branch (j + STRIDE) */
      if (steps[j + 2].d.jump.jumpdone != j + STRIDE)
        break;

      branch_count++;
      j += STRIDE;
    }

    /* Verify JUMP_IF_NOT_TRUE of each branch except last points to next */
    {
      bool valid = true;
      for (int k = 0; k < branch_count - 1; k++)
      {
        int step_idx = i + k * STRIDE + 2;
        if (steps[step_idx].d.jump.jumpdone != i + (k + 1) * STRIDE)
        {
          valid = false;
          break;
        }
      }
      if (!valid)
      {
        i++;
        continue;
      }
    }

    /* Last branch's JUMP_IF_NOT_TRUE should point to j (= the ELSE part) */
    {
      int last_jint = i + (branch_count - 1) * STRIDE + 2;
      if (steps[last_jint].d.jump.jumpdone != j)
      {
        i++;
        continue;
      }
    }

    /* Check for ELSE: step at j should be EEOP_CONST (the ELSE result) */
    {
      int else_start = j;

      if (else_start >= steps_len)
      {
        i++;
        continue;
      }

      ExprEvalOp else_op = ExecEvalStepOp(state, &steps[else_start]);
      if (else_op != EEOP_CONST)
      {
        i++;
        continue;
      }

      /* The ELSE CONST is followed by end_jump_target or is end_jump_target-1 */
      if (else_start + 1 != end_jump_target)
      {
        /* Maybe there's a JUMP after the ELSE CONST */
        if (else_start + 1 < steps_len &&
            ExecEvalStepOp(state, &steps[else_start + 1]) == EEOP_JUMP &&
            steps[else_start + 1].d.jump.jumpdone == end_jump_target)
        {
          /* OK — ELSE const + JUMP to end */
        }
        else
        {
          i++;
          continue;
        }
      }
    }

    /* Minimum branch count check */
    if (branch_count < CASE_BSEARCH_MIN_BRANCHES)
    {
      i = j;
      continue;
    }

    /* For Pattern A (non-equality): verify monotonicity of thresholds */
    if (!is_equality)
    {
      bool monotonic = true;
      for (int k = 1; k < branch_count; k++)
      {
        Datum prev = steps[i + (k - 1) * STRIDE + 1].d.func.fcinfo_data->args[1].value;
        Datum curr = steps[i + k * STRIDE + 1].d.func.fcinfo_data->args[1].value;
        int cmp_result;

        switch (bs_type)
        {
          case CASE_BS_I32:
          {
            int32 pv = DatumGetInt32(prev);
            int32 cv = DatumGetInt32(curr);
            cmp_result = (cv > pv) - (cv < pv);
            break;
          }
          case CASE_BS_I64:
          {
            int64 pv = DatumGetInt64(prev);
            int64 cv = DatumGetInt64(curr);
            cmp_result = (cv > pv) - (cv < pv);
            break;
          }
          case CASE_BS_U32:
          {
            uint32 pv = (uint32)DatumGetInt32(prev);
            uint32 cv = (uint32)DatumGetInt32(curr);
            cmp_result = (cv > pv) - (cv < pv);
            break;
          }
          case CASE_BS_I16:
          {
            int16 pv = (int16)DatumGetInt32(prev);
            int16 cv = (int16)DatumGetInt32(curr);
            cmp_result = (cv > pv) - (cv < pv);
            break;
          }
          case CASE_BS_F4:
          {
            float4 pv = DatumGetFloat4(prev);
            float4 cv = DatumGetFloat4(curr);
            /* NaN-aware: NaN > all other values */
            if (isnan(cv))
              cmp_result = isnan(pv) ? 0 : 1;
            else if (isnan(pv))
              cmp_result = -1;
            else
              cmp_result = (cv > pv) - (cv < pv);
            break;
          }
          case CASE_BS_F8:
          {
            float8 pv = DatumGetFloat8(prev);
            float8 cv = DatumGetFloat8(curr);
            if (isnan(cv))
              cmp_result = isnan(pv) ? 0 : 1;
            else if (isnan(pv))
              cmp_result = -1;
            else
              cmp_result = (cv > pv) - (cv < pv);
            break;
          }
          case CASE_BS_GENERIC:
          {
            /* Call PG comparison function to determine ordering.
             * We use the lt function: if lt(prev, curr) → ascending. */
            LOCAL_FCINFO(mono_fcinfo, 2);
            Oid collation = steps[i + 1].d.func.fcinfo_data->fncollation;
            InitFunctionCallInfoData(*mono_fcinfo, NULL, 2, collation, NULL, NULL);
            mono_fcinfo->args[0].value = prev;
            mono_fcinfo->args[0].isnull = false;
            mono_fcinfo->args[1].value = curr;
            mono_fcinfo->args[1].isnull = false;

            /* Use the same comparison function — if is_less, we expect
             * thresholds in ascending order; otherwise descending. */
            bool lt_result = DatumGetBool(cmp_fn(mono_fcinfo));
            if (is_less)
              cmp_result = lt_result ? 1 : -1;
            else
              cmp_result = lt_result ? -1 : 1;

            /* Check for equality (not strictly monotonic) */
            if (!lt_result)
            {
              /* Check if they're equal */
              mono_fcinfo->args[0].value = curr;
              mono_fcinfo->args[1].value = prev;
              bool reverse = DatumGetBool(cmp_fn(mono_fcinfo));
              if (!reverse)
                cmp_result = 0;  /* equal: not monotonic */
            }
            break;
          }
        }

        if (is_less)
        {
          if (cmp_result <= 0) { monotonic = false; break; }
        }
        else
        {
          if (cmp_result >= 0) { monotonic = false; break; }
        }
      }

      if (!monotonic)
      {
        i = j;
        continue;
      }
    }

    /* Build the CaseBSearchInfo */
    {
      CaseBSearchInfo *cbi = &out[nfound];

      cbi->start_opno = i;
      cbi->end_opno = j;  /* first step of ELSE result */
      cbi->else_end_opno = end_jump_target;
      cbi->num_branches = branch_count;
      cbi->is_equality = is_equality;
      cbi->is_less = is_less;
      cbi->is_inclusive = is_inclusive;
      cbi->cmp_fn = cmp_fn;
      cbi->bs_type = bs_type;
      cbi->var_opno = i;  /* representative VAR step */
      cbi->var_type = var_type;

      /* ELSE result */
      cbi->default_result = steps[j].d.constval.value;
      cbi->default_is_null = steps[j].d.constval.isnull;

      /* Build CaseBSearchDesc: header + thresholds[n] + results[n] */
      {
        Size thresh_elem;
        switch (bs_type)
        {
          case CASE_BS_I32: case CASE_BS_U32: case CASE_BS_I16:
            thresh_elem = sizeof(int32);
            break;
          case CASE_BS_I64:
            thresh_elem = sizeof(int64);
            break;
          default: /* F4, F8, GENERIC */
            thresh_elem = sizeof(Datum);
            break;
        }
        Size desc_size = sizeof(CaseBSearchDesc) +
                         thresh_elem * branch_count +
                         sizeof(Datum) * branch_count;
        CaseBSearchDesc *desc = palloc(desc_size);
        char *ptr = (char *)desc + sizeof(CaseBSearchDesc);

        desc->n = branch_count;
        desc->bs_type = (uint8)bs_type;
        desc->cmp_fn = (bs_type == CASE_BS_GENERIC) ? cmp_fn : NULL;
        desc->cmp_order_fn = NULL;
        if (bs_type == CASE_BS_GENERIC && is_equality)
          desc->cmp_order_fn = find_generic_lt_fn(cmp_fn);
        desc->cmp_collation = (bs_type == CASE_BS_GENERIC)
            ? steps[i + 1].d.func.fcinfo_data->fncollation : InvalidOid;
        desc->default_val = steps[j].d.constval.value;

        /* Copy thresholds from fcinfo->args[1].value */
        switch (bs_type)
        {
          case CASE_BS_I32: case CASE_BS_U32: case CASE_BS_I16:
          {
            int32 *thresh = (int32 *)ptr;
            for (int k = 0; k < branch_count; k++)
              thresh[k] = DatumGetInt32(
                steps[i + k * STRIDE + 1].d.func.fcinfo_data->args[1].value);
            cbi->thresholds = (Datum *)thresh;
            break;
          }
          case CASE_BS_I64:
          {
            int64 *thresh = (int64 *)ptr;
            for (int k = 0; k < branch_count; k++)
              thresh[k] = DatumGetInt64(
                steps[i + k * STRIDE + 1].d.func.fcinfo_data->args[1].value);
            cbi->thresholds = (Datum *)thresh;
            break;
          }
          default: /* F4, F8, GENERIC — store as Datum */
          {
            Datum *thresh = (Datum *)ptr;
            for (int k = 0; k < branch_count; k++)
              thresh[k] = steps[i + k * STRIDE + 1].d.func.fcinfo_data->args[1].value;
            cbi->thresholds = thresh;
            break;
          }
        }
        ptr += thresh_elem * branch_count;

        /* Copy results from CONST at +3 */
        {
          Datum *res = (Datum *)ptr;
          for (int k = 0; k < branch_count; k++)
            res[k] = steps[i + k * STRIDE + 3].d.constval.value;
          cbi->results = res;
        }

        /* For Pattern B (equality): sort by threshold, reorder results */
        if (is_equality && bs_type != CASE_BS_GENERIC)
        {
          ThresholdResult *pairs = palloc(sizeof(ThresholdResult) * branch_count);
          int (*cmp_func)(const void *, const void *) = NULL;

          for (int k = 0; k < branch_count; k++)
          {
            switch (bs_type)
            {
              case CASE_BS_I32:
                pairs[k].threshold = Int32GetDatum(((int32 *)cbi->thresholds)[k]);
                break;
              case CASE_BS_I64:
                pairs[k].threshold = Int64GetDatum(((int64 *)cbi->thresholds)[k]);
                break;
              case CASE_BS_U32:
                pairs[k].threshold = UInt32GetDatum(((uint32 *)cbi->thresholds)[k]);
                break;
              case CASE_BS_I16:
                pairs[k].threshold = Int32GetDatum(((int32 *)cbi->thresholds)[k]);
                break;
              default:
                pairs[k].threshold = ((Datum *)cbi->thresholds)[k];
                break;
            }
            pairs[k].result = cbi->results[k];
          }

          switch (bs_type)
          {
            case CASE_BS_I32: case CASE_BS_I16:
              cmp_func = cmp_threshold_result_i32;
              break;
            case CASE_BS_I64:
              cmp_func = cmp_threshold_result_i64;
              break;
            case CASE_BS_U32:
              cmp_func = cmp_threshold_result_u32;
              break;
            case CASE_BS_F4:
              cmp_func = cmp_threshold_result_f4;
              break;
            case CASE_BS_F8:
              cmp_func = cmp_threshold_result_f8;
              break;
            default:
              break;
          }

          if (cmp_func)
          {
            qsort(pairs, branch_count, sizeof(ThresholdResult), cmp_func);

            for (int k = 0; k < branch_count; k++)
            {
              switch (bs_type)
              {
                case CASE_BS_I32: case CASE_BS_I16:
                  ((int32 *)cbi->thresholds)[k] = DatumGetInt32(pairs[k].threshold);
                  break;
                case CASE_BS_I64:
                  ((int64 *)cbi->thresholds)[k] = DatumGetInt64(pairs[k].threshold);
                  break;
                case CASE_BS_U32:
                  ((int32 *)cbi->thresholds)[k] = (int32)DatumGetUInt32(pairs[k].threshold);
                  break;
                default:
                  ((Datum *)cbi->thresholds)[k] = pairs[k].threshold;
                  break;
              }
              cbi->results[k] = pairs[k].result;
            }
          }
          pfree(pairs);
        }
        /* For generic equality: sort thresholds for binary search.
         * For deterministic non-C collations, use memcmp sort (fast)
         * instead of locale-aware text_lt (pg_strncoll is expensive). */
        if (is_equality && bs_type == CASE_BS_GENERIC && desc->cmp_order_fn != NULL)
        {
          ThresholdResult *pairs = palloc(sizeof(ThresholdResult) * branch_count);
          for (int k = 0; k < branch_count; k++)
          {
            pairs[k].threshold = ((Datum *)cbi->thresholds)[k];
            pairs[k].result = cbi->results[k];
          }

          bool use_memcmp_sort = false;
          if (OidIsValid(desc->cmp_collation) &&
              desc->cmp_collation != C_COLLATION_OID)
          {
            pg_locale_t locale = pg_newlocale_from_collation(desc->cmp_collation);
            bool deterministic = (locale == NULL) ? true : locale->deterministic;
            if (deterministic && !pg_jitter_collation_is_c(desc->cmp_collation))
              use_memcmp_sort = true;
          }

          if (use_memcmp_sort)
          {
            qsort(pairs, branch_count, sizeof(ThresholdResult),
                  cmp_threshold_result_text_memcmp);
          }
          else
          {
            GenericCmpCtx cmp_ctx;
            cmp_ctx.lt_fn = desc->cmp_order_fn;
            cmp_ctx.collation = desc->cmp_collation;
            qsort_arg(pairs, branch_count, sizeof(ThresholdResult),
                      cmp_threshold_result_generic, &cmp_ctx);
          }

          for (int k = 0; k < branch_count; k++)
          {
            ((Datum *)cbi->thresholds)[k] = pairs[k].threshold;
            cbi->results[k] = pairs[k].result;
          }
          pfree(pairs);
        }
      }

      nfound++;
    }

    /* Skip past this pattern */
    i = end_jump_target;
  }

  return nfound;
}

/*
 * pg_jitter_setup_case_bsearch_arrays — detect patterns and store desc
 * pointers in step data fields for runtime access by JIT code.
 *
 * Called by:
 *  - Leader/single backend: during compilation, after detection
 *  - Workers: after attaching shared code, before first execution
 *
 * For each detected pattern, stores the CaseBSearchDesc pointer in:
 *   steps[start_opno + 1].d.constval.value = (Datum)desc_ptr
 */
void
pg_jitter_setup_case_bsearch_arrays(ExprState *state,
                                     ExprEvalStep *steps, int steps_len)
{
  CaseBSearchInfo patterns[16];
  int n = pg_jitter_detect_case_bsearch(state, steps, steps_len, patterns, 16);

  for (int i = 0; i < n; i++)
  {
    CaseBSearchInfo *cbi = &patterns[i];

    /* Compute desc pointer from thresholds (which points into the desc).
     * The desc was allocated as: header | thresholds[n] | results[n]
     * So: desc = thresholds - sizeof(CaseBSearchDesc) */
    CaseBSearchDesc *desc = (CaseBSearchDesc *)
        ((char *)cbi->thresholds - sizeof(CaseBSearchDesc));

    /* Store desc pointer in step data field of a skipped step.
     * Use start_opno+2 (JUMP_IF_NOT_TRUE) which is dead in JIT mode.
     * We avoid start_opno+1 (FUNCEXPR_STRICT) because workers need its
     * d.func data intact for re-detection. */
    steps[cbi->start_opno + 2].d.constval.value = PointerGetDatum(desc);
  }
}

/* ----------------------------------------------------------------
 * Binary search helper functions
 *
 * Called from JIT code.  Each takes (val, desc_ptr) where desc contains
 * the thresholds, results, count, and default value.
 *
 * Memory layout of CaseBSearchDesc:
 *   { int n; Datum default_val; int32[n] OR int64[n] thresholds; Datum[n] results; }
 * ---------------------------------------------------------------- */

/* Accessor macros for CaseBSearchDesc inline arrays.
 * Arrays start immediately after the CaseBSearchDesc header. */
#define BSEARCH_THRESH_I32(desc) \
  ((const int32 *)((const char *)(desc) + sizeof(CaseBSearchDesc)))
#define BSEARCH_THRESH_I64(desc) \
  ((const int64 *)((const char *)(desc) + sizeof(CaseBSearchDesc)))
#define BSEARCH_THRESH_DATUM(desc) \
  ((const Datum *)((const char *)(desc) + sizeof(CaseBSearchDesc)))
#define BSEARCH_RESULTS_I32(desc) \
  ((const Datum *)(BSEARCH_THRESH_I32(desc) + (desc)->n))
#define BSEARCH_RESULTS_I64(desc) \
  ((const Datum *)(BSEARCH_THRESH_I64(desc) + (desc)->n))
#define BSEARCH_RESULTS_DATUM(desc) \
  (BSEARCH_THRESH_DATUM(desc) + (desc)->n)

/* Pattern A: searched CASE with < (strictly less than).
 * Finds first i where val < thresholds[i], returns results[i]. */
Datum
pg_jitter_case_bsearch_lt_i32(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *thresholds = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val < thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

Datum
pg_jitter_case_bsearch_le_i32(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *thresholds = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val <= thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

Datum
pg_jitter_case_bsearch_lt_i64(int64 val, const CaseBSearchDesc *desc)
{
  const int64 *thresholds = BSEARCH_THRESH_I64(desc);
  const Datum *results = BSEARCH_RESULTS_I64(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val < thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

Datum
pg_jitter_case_bsearch_le_i64(int64 val, const CaseBSearchDesc *desc)
{
  const int64 *thresholds = BSEARCH_THRESH_I64(desc);
  const Datum *results = BSEARCH_RESULTS_I64(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val <= thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

/* Pattern A: searched CASE with > (strictly greater than).
 * Thresholds are strictly decreasing.  Finds first i where val > thresholds[i]. */
Datum
pg_jitter_case_bsearch_gt_i32(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *thresholds = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val > thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

Datum
pg_jitter_case_bsearch_ge_i32(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *thresholds = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val >= thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

Datum
pg_jitter_case_bsearch_gt_i64(int64 val, const CaseBSearchDesc *desc)
{
  const int64 *thresholds = BSEARCH_THRESH_I64(desc);
  const Datum *results = BSEARCH_RESULTS_I64(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val > thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

Datum
pg_jitter_case_bsearch_ge_i64(int64 val, const CaseBSearchDesc *desc)
{
  const int64 *thresholds = BSEARCH_THRESH_I64(desc);
  const Datum *results = BSEARCH_RESULTS_I64(desc);
  int n = desc->n;
  int lo = 0, hi = n;
  while (lo < hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (val >= thresholds[mid])
      hi = mid;
    else
      lo = mid + 1;
  }
  return lo < n ? results[lo] : desc->default_val;
}

/* Pattern B: simple CASE with = comparison.
 * sorted_vals is sorted ascending.  Binary search for exact match. */
Datum
pg_jitter_case_bsearch_eq_i32(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *sorted_vals = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int lo = 0, hi = desc->n - 1;
  while (lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (sorted_vals[mid] == val)
      return results[mid];
    else if (sorted_vals[mid] < val)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return desc->default_val;
}

Datum
pg_jitter_case_bsearch_eq_i64(int64 val, const CaseBSearchDesc *desc)
{
  const int64 *sorted_vals = BSEARCH_THRESH_I64(desc);
  const Datum *results = BSEARCH_RESULTS_I64(desc);
  int lo = 0, hi = desc->n - 1;
  while (lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    if (sorted_vals[mid] == val)
      return results[mid];
    else if (sorted_vals[mid] < val)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return desc->default_val;
}

/* ---- Unsigned 32-bit (OID) helpers ---- */

#define BSEARCH_BODY_U32(OP) \
  const int32 *thresholds = BSEARCH_THRESH_I32(desc); \
  const Datum *results = BSEARCH_RESULTS_I32(desc); \
  int n = desc->n; \
  int lo = 0, hi = n; \
  while (lo < hi) { \
    int mid = lo + (hi - lo) / 2; \
    if ((uint32)val OP (uint32)thresholds[mid]) hi = mid; \
    else lo = mid + 1; \
  } \
  return lo < n ? results[lo] : desc->default_val;

Datum pg_jitter_case_bsearch_lt_u32(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_U32(<) }
Datum pg_jitter_case_bsearch_le_u32(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_U32(<=) }
Datum pg_jitter_case_bsearch_gt_u32(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_U32(>) }
Datum pg_jitter_case_bsearch_ge_u32(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_U32(>=) }

Datum
pg_jitter_case_bsearch_eq_u32(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *sorted_vals = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int lo = 0, hi = desc->n - 1;
  while (lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    if ((uint32)sorted_vals[mid] == (uint32)val)
      return results[mid];
    else if ((uint32)sorted_vals[mid] < (uint32)val)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return desc->default_val;
}

#undef BSEARCH_BODY_U32

/* ---- Int16 helpers ---- */

#define BSEARCH_BODY_I16(OP) \
  const int32 *thresholds = BSEARCH_THRESH_I32(desc); \
  const Datum *results = BSEARCH_RESULTS_I32(desc); \
  int n = desc->n; \
  int lo = 0, hi = n; \
  while (lo < hi) { \
    int mid = lo + (hi - lo) / 2; \
    if ((int16)val OP (int16)thresholds[mid]) hi = mid; \
    else lo = mid + 1; \
  } \
  return lo < n ? results[lo] : desc->default_val;

Datum pg_jitter_case_bsearch_lt_i16(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_I16(<) }
Datum pg_jitter_case_bsearch_le_i16(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_I16(<=) }
Datum pg_jitter_case_bsearch_gt_i16(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_I16(>) }
Datum pg_jitter_case_bsearch_ge_i16(int32 val, const CaseBSearchDesc *desc) { BSEARCH_BODY_I16(>=) }

Datum
pg_jitter_case_bsearch_eq_i16(int32 val, const CaseBSearchDesc *desc)
{
  const int32 *sorted_vals = BSEARCH_THRESH_I32(desc);
  const Datum *results = BSEARCH_RESULTS_I32(desc);
  int lo = 0, hi = desc->n - 1;
  while (lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    if ((int16)sorted_vals[mid] == (int16)val)
      return results[mid];
    else if ((int16)sorted_vals[mid] < (int16)val)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return desc->default_val;
}

#undef BSEARCH_BODY_I16

/* ---- Float4 helpers (NaN-aware) ---- */

static inline int
float4_cmp_jit(float4 a, float4 b)
{
  if (isnan(a)) return isnan(b) ? 0 : 1;
  if (isnan(b)) return -1;
  return (a > b) - (a < b);
}

#define BSEARCH_BODY_F4(OP_CHECK) \
  const Datum *thresholds = BSEARCH_THRESH_DATUM(desc); \
  const Datum *results = BSEARCH_RESULTS_DATUM(desc); \
  float4 fval = DatumGetFloat4(val); \
  int n = desc->n; \
  int lo = 0, hi = n; \
  while (lo < hi) { \
    int mid = lo + (hi - lo) / 2; \
    int c = float4_cmp_jit(fval, DatumGetFloat4(thresholds[mid])); \
    if (OP_CHECK) hi = mid; \
    else lo = mid + 1; \
  } \
  return lo < n ? results[lo] : desc->default_val;

Datum pg_jitter_case_bsearch_lt_f4(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F4(c < 0) }
Datum pg_jitter_case_bsearch_le_f4(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F4(c <= 0) }
Datum pg_jitter_case_bsearch_gt_f4(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F4(c > 0) }
Datum pg_jitter_case_bsearch_ge_f4(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F4(c >= 0) }

Datum
pg_jitter_case_bsearch_eq_f4(Datum val, const CaseBSearchDesc *desc)
{
  const Datum *sorted_vals = BSEARCH_THRESH_DATUM(desc);
  const Datum *results = BSEARCH_RESULTS_DATUM(desc);
  float4 fval = DatumGetFloat4(val);
  int lo = 0, hi = desc->n - 1;
  while (lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    int c = float4_cmp_jit(fval, DatumGetFloat4(sorted_vals[mid]));
    if (c == 0) return results[mid];
    else if (c > 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return desc->default_val;
}

#undef BSEARCH_BODY_F4

/* ---- Float8 helpers (NaN-aware) ---- */

static inline int
float8_cmp_jit(float8 a, float8 b)
{
  if (isnan(a)) return isnan(b) ? 0 : 1;
  if (isnan(b)) return -1;
  return (a > b) - (a < b);
}

#define BSEARCH_BODY_F8(OP_CHECK) \
  const Datum *thresholds = BSEARCH_THRESH_DATUM(desc); \
  const Datum *results = BSEARCH_RESULTS_DATUM(desc); \
  float8 fval = DatumGetFloat8(val); \
  int n = desc->n; \
  int lo = 0, hi = n; \
  while (lo < hi) { \
    int mid = lo + (hi - lo) / 2; \
    int c = float8_cmp_jit(fval, DatumGetFloat8(thresholds[mid])); \
    if (OP_CHECK) hi = mid; \
    else lo = mid + 1; \
  } \
  return lo < n ? results[lo] : desc->default_val;

Datum pg_jitter_case_bsearch_lt_f8(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F8(c < 0) }
Datum pg_jitter_case_bsearch_le_f8(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F8(c <= 0) }
Datum pg_jitter_case_bsearch_gt_f8(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F8(c > 0) }
Datum pg_jitter_case_bsearch_ge_f8(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_F8(c >= 0) }

Datum
pg_jitter_case_bsearch_eq_f8(Datum val, const CaseBSearchDesc *desc)
{
  const Datum *sorted_vals = BSEARCH_THRESH_DATUM(desc);
  const Datum *results = BSEARCH_RESULTS_DATUM(desc);
  float8 fval = DatumGetFloat8(val);
  int lo = 0, hi = desc->n - 1;
  while (lo <= hi)
  {
    int mid = lo + (hi - lo) / 2;
    int c = float8_cmp_jit(fval, DatumGetFloat8(sorted_vals[mid]));
    if (c == 0) return results[mid];
    else if (c > 0) lo = mid + 1;
    else hi = mid - 1;
  }
  return desc->default_val;
}

#undef BSEARCH_BODY_F8

/* ---- Generic (byref) helpers — call PG cmp_fn via V1 ---- */

#define BSEARCH_BODY_GENERIC(CMP_FN_IS_OP) \
  const Datum *thresholds = BSEARCH_THRESH_DATUM(desc); \
  const Datum *results = BSEARCH_RESULTS_DATUM(desc); \
  int n = desc->n; \
  int lo = 0, hi = n; \
  LOCAL_FCINFO(fcinfo, 2); \
  InitFunctionCallInfoData(*fcinfo, NULL, 2, desc->cmp_collation, NULL, NULL); \
  fcinfo->args[0].value = val; \
  fcinfo->args[0].isnull = false; \
  while (lo < hi) { \
    int mid = lo + (hi - lo) / 2; \
    fcinfo->args[1].value = thresholds[mid]; \
    fcinfo->args[1].isnull = false; \
    Datum result = desc->cmp_fn(fcinfo); \
    if (CMP_FN_IS_OP) hi = mid; \
    else lo = mid + 1; \
  } \
  return lo < n ? results[lo] : desc->default_val;

/* For lt/le/gt/ge: the comparison function IS the operator (e.g., text_lt).
 * DatumGetBool(result) == true means the comparison holds. */
Datum pg_jitter_case_bsearch_lt_generic(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_GENERIC(DatumGetBool(result)) }
Datum pg_jitter_case_bsearch_le_generic(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_GENERIC(DatumGetBool(result)) }
Datum pg_jitter_case_bsearch_gt_generic(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_GENERIC(DatumGetBool(result)) }
Datum pg_jitter_case_bsearch_ge_generic(Datum val, const CaseBSearchDesc *desc) { BSEARCH_BODY_GENERIC(DatumGetBool(result)) }

/* For equality: binary search using cmp_order_fn (_lt function) for direction,
 * then verify with cmp_fn (_eq function). Falls back to linear scan if
 * cmp_order_fn is NULL (unknown type). */
Datum
pg_jitter_case_bsearch_eq_generic(Datum val, const CaseBSearchDesc *desc)
{
  const Datum *thresholds = BSEARCH_THRESH_DATUM(desc);
  const Datum *results = BSEARCH_RESULTS_DATUM(desc);
  LOCAL_FCINFO(fcinfo, 2);
  InitFunctionCallInfoData(*fcinfo, NULL, 2, desc->cmp_collation, NULL, NULL);
  fcinfo->args[0].isnull = false;
  fcinfo->args[1].isnull = false;

  if (desc->cmp_order_fn != NULL)
  {
    bool use_memcmp = false;

    /*
     * For deterministic non-C collations, the _lt ordering function uses
     * pg_strncoll (very expensive).  Since we only need to find an equal
     * element, any consistent total order works — use memcmp instead.
     * Thresholds are pre-sorted by memcmp at setup time for this case.
     */
    if (OidIsValid(desc->cmp_collation) &&
        desc->cmp_collation != C_COLLATION_OID)
    {
      pg_locale_t locale = pg_newlocale_from_collation(desc->cmp_collation);
      /*
       * PG14-17: pg_newlocale_from_collation() returns NULL for the
       * default libc collation.  NULL means deterministic (byte-equal).
       * PG18+ always returns a valid locale struct.
       */
      bool deterministic = (locale == NULL) ? true : locale->deterministic;
      if (deterministic && !pg_jitter_collation_is_c(desc->cmp_collation))
        use_memcmp = true;
    }

    /* Binary search: thresholds are sorted ascending */
    int lo = 0, hi = desc->n - 1;
    while (lo <= hi)
    {
      int mid = lo + (hi - lo) / 2;
      int cmp;

      if (use_memcmp)
      {
        text *t1 = DatumGetTextPP(val);
        text *t2 = DatumGetTextPP(thresholds[mid]);
        int len1 = VARSIZE_ANY_EXHDR(t1);
        int len2 = VARSIZE_ANY_EXHDR(t2);
        int minlen = (len1 < len2) ? len1 : len2;
        cmp = memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2), minlen);
        if (cmp == 0)
          cmp = (len1 > len2) - (len1 < len2);
        if ((Pointer)t1 != DatumGetPointer(val))
          pfree(t1);
        if ((Pointer)t2 != DatumGetPointer(thresholds[mid]))
          pfree(t2);
      }
      else
      {
        /* Check val < thresholds[mid] */
        fcinfo->args[0].value = val;
        fcinfo->args[1].value = thresholds[mid];
        if (DatumGetBool(desc->cmp_order_fn(fcinfo)))
          cmp = -1;
        else
        {
          /* Check thresholds[mid] < val */
          fcinfo->args[0].value = thresholds[mid];
          fcinfo->args[1].value = val;
          cmp = DatumGetBool(desc->cmp_order_fn(fcinfo)) ? 1 : 0;
        }
      }

      if (cmp < 0)
        hi = mid - 1;
      else if (cmp > 0)
        lo = mid + 1;
      else
        return results[mid];
    }
    return desc->default_val;
  }

  /* Fallback: linear scan with eq function */
  fcinfo->args[0].value = val;
  for (int i = 0; i < desc->n; i++)
  {
    fcinfo->args[1].value = thresholds[i];
    if (DatumGetBool(desc->cmp_fn(fcinfo)))
      return results[i];
  }
  return desc->default_val;
}

#undef BSEARCH_BODY_GENERIC

/* ---- Helper selection function ---- */

void *
pg_jitter_select_bsearch_helper(const CaseBSearchInfo *cbi)
{
  /* Table indexed by [bs_type][op_idx], where op_idx:
   * 0=eq, 1=lt, 2=le, 3=gt, 4=ge */
  static void *table[7][5] = {
    [CASE_BS_I32]    = { (void *)pg_jitter_case_bsearch_eq_i32,
                         (void *)pg_jitter_case_bsearch_lt_i32,
                         (void *)pg_jitter_case_bsearch_le_i32,
                         (void *)pg_jitter_case_bsearch_gt_i32,
                         (void *)pg_jitter_case_bsearch_ge_i32 },
    [CASE_BS_I64]    = { (void *)pg_jitter_case_bsearch_eq_i64,
                         (void *)pg_jitter_case_bsearch_lt_i64,
                         (void *)pg_jitter_case_bsearch_le_i64,
                         (void *)pg_jitter_case_bsearch_gt_i64,
                         (void *)pg_jitter_case_bsearch_ge_i64 },
    [CASE_BS_U32]    = { (void *)pg_jitter_case_bsearch_eq_u32,
                         (void *)pg_jitter_case_bsearch_lt_u32,
                         (void *)pg_jitter_case_bsearch_le_u32,
                         (void *)pg_jitter_case_bsearch_gt_u32,
                         (void *)pg_jitter_case_bsearch_ge_u32 },
    [CASE_BS_I16]    = { (void *)pg_jitter_case_bsearch_eq_i16,
                         (void *)pg_jitter_case_bsearch_lt_i16,
                         (void *)pg_jitter_case_bsearch_le_i16,
                         (void *)pg_jitter_case_bsearch_gt_i16,
                         (void *)pg_jitter_case_bsearch_ge_i16 },
    [CASE_BS_F4]     = { (void *)pg_jitter_case_bsearch_eq_f4,
                         (void *)pg_jitter_case_bsearch_lt_f4,
                         (void *)pg_jitter_case_bsearch_le_f4,
                         (void *)pg_jitter_case_bsearch_gt_f4,
                         (void *)pg_jitter_case_bsearch_ge_f4 },
    [CASE_BS_F8]     = { (void *)pg_jitter_case_bsearch_eq_f8,
                         (void *)pg_jitter_case_bsearch_lt_f8,
                         (void *)pg_jitter_case_bsearch_le_f8,
                         (void *)pg_jitter_case_bsearch_gt_f8,
                         (void *)pg_jitter_case_bsearch_ge_f8 },
    [CASE_BS_GENERIC]= { (void *)pg_jitter_case_bsearch_eq_generic,
                         (void *)pg_jitter_case_bsearch_lt_generic,
                         (void *)pg_jitter_case_bsearch_le_generic,
                         (void *)pg_jitter_case_bsearch_gt_generic,
                         (void *)pg_jitter_case_bsearch_ge_generic },
  };

  int op_idx;
  if (cbi->is_equality)
    op_idx = 0;
  else if (cbi->is_less)
    op_idx = cbi->is_inclusive ? 2 : 1;
  else
    op_idx = cbi->is_inclusive ? 4 : 3;

  return table[cbi->bs_type][op_idx];
}

/* ================================================================
 * Windows x64: JIT code unwind table registration
 *
 * On Windows x64, longjmp() uses RtlUnwindEx which requires every stack
 * frame to have a RUNTIME_FUNCTION entry in the process unwind tables.
 * JIT-generated code lacks this metadata, causing longjmp() to crash with
 * STATUS_STACK_BUFFER_OVERRUN (0xC00000FF) when ereport(ERROR) fires
 * from a JIT-called helper (e.g., overflow error handlers).
 *
 * We use RtlInstallFunctionTableCallback to register a dynamic function
 * table for each JIT code block.  The callback returns a RUNTIME_FUNCTION
 * pointing to a minimal UNWIND_INFO (leaf function: no prologue, no saved
 * registers).  This tells the SEH unwinder that the return address is at
 * [RSP], which is sufficient for longjmp to skip past JIT frames.
 *
 * Lifecycle: register after code generation, deregister before freeing code.
 * All allocations are freed in deregister — no leaks.
 * ================================================================ */
#ifdef _WIN64

#include <windows.h>

/*
 * Per-JIT-block unwind context.  Allocated in register, freed in deregister.
 * Stored in a singly-linked list keyed by code base address.
 *
 * The unwind_info[] buffer holds a proper UNWIND_INFO structure generated by
 * parsing the sljit prologue.  Max size: 4 bytes header + 2*16 UNWIND_CODEs
 * (up to 16 register pushes) + 2 for alloc = 40 bytes.
 */
#define WIN64_MAX_UNWIND_CODES 34  /* 16 pushes + 1 large alloc (3 slots) */
#define WIN64_UNWIND_INFO_SIZE (4 + WIN64_MAX_UNWIND_CODES * 2)

typedef struct Win64UnwindCtx {
  struct Win64UnwindCtx *next;
  DWORD64 code_base;
  DWORD   code_size_dw;
  RUNTIME_FUNCTION rf;
  uint8_t unwind_info[WIN64_UNWIND_INFO_SIZE];
} Win64UnwindCtx;

/* Global list of registered unwind contexts (process-local, single-threaded PG backend) */
static Win64UnwindCtx *win64_unwind_list = NULL;

/*
 * Windows x64 UNWIND_CODE operations.
 */
#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3

/*
 * x64 register numbers for UNWIND_CODE (same as Windows convention):
 * RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7,
 * R8=8, R9=9, R10=10, R11=11, R12=12, R13=13, R14=14, R15=15
 */

/*
 * Parse the sljit-generated x64 prologue to build accurate UNWIND_INFO.
 *
 * sljit's prologue on x64 is a sequence of:
 *   [ENDBR64]                        -- optional CET (F3 0F 1E FA)
 *   PUSH r64 ...                     -- callee-saved registers
 *   SUB RSP, imm                     -- local variable space
 *   [MOVAPS [RSP+off], XMMn ...]     -- XMM saves (Windows)
 *
 * We parse PUSHes and the SUB RSP to generate UWOP_PUSH_NONVOL and
 * UWOP_ALLOC_SMALL/LARGE codes.  XMM saves are not needed for correct
 * unwinding (only for restoring XMM values, which longjmp handles).
 *
 * Returns the number of UNWIND_CODE slots written, or 0 on failure.
 */
static int
win64_parse_prologue(const uint8_t *code, size_t code_size,
                     uint8_t *unwind_codes, int max_codes,
                     uint8_t *prolog_size_out,
                     uint8_t *frame_reg_out, uint8_t *frame_offset_out)
{
  const uint8_t *p = code;
  const uint8_t *end = code + (code_size < 256 ? code_size : 256);
  int ncodes = 0;
  int pushes_offset[16];
  int push_reg[16];
  int npushes = 0;
  uint32_t alloc_size = 0;
  uint8_t alloc_offset = 0;
  uint8_t set_fpreg_offset = 0;

  *frame_reg_out = 0;
  *frame_offset_out = 0;

  /* Skip ENDBR64 if present */
  if (p + 4 <= end && p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA)
    p += 4;

  /*
   * Parse PUSH instructions and inline MOV/LEA instructions that compilers
   * (sljit, AsmJIT, MIR) place between PUSHes in the prologue.
   *
   * AsmJIT's Compiler may generate:
   *   push rbp; mov rbp, rsp; push rbx; push rsi; push rdi; sub rsp, N
   * or:
   *   push rbp; push rbx; push rsi; sub rsp, N; lea rbp, [rsp+off]
   *
   * We parse PUSHes and skip MOV/LEA instructions (up to a limit)
   * until we hit something that isn't part of the prologue.
   */
  {
    int non_push_skips = 0;
    const int max_non_push_skips = 8;

    while (p < end && npushes < 16) {
      if (*p >= 0x50 && *p <= 0x57) {
        /* PUSH r64 (registers RAX-RDI) */
        push_reg[npushes] = *p - 0x50;
        pushes_offset[npushes] = (int)(p - code);
        npushes++;
        p += 1;
        non_push_skips = 0;
      } else if (p + 1 < end && *p == 0x41 && p[1] >= 0x50 && p[1] <= 0x57) {
        /* PUSH r64 with REX.B (registers R8-R15) */
        push_reg[npushes] = 8 + (p[1] - 0x50);
        pushes_offset[npushes] = (int)(p - code);
        npushes++;
        p += 2;
        non_push_skips = 0;
      } else if (p + 2 < end && p[0] == 0x48 && p[1] == 0x89 && p[2] == 0xE5) {
        /* MOV RBP, RSP (48 89 E5) — AsmJIT frame pointer setup */
        /* Record as UWOP_SET_FPREG — frame pointer = RBP at RSP+0 */
        set_fpreg_offset = (uint8_t)(p + 3 - code);
        *frame_reg_out = 5; /* RBP */
        *frame_offset_out = 0;
        p += 3;
        non_push_skips++;
      } else if (p + 2 < end && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xEC) {
        /* MOV RBP, RSP (48 8B EC) — alternate encoding */
        set_fpreg_offset = (uint8_t)(p + 3 - code);
        *frame_reg_out = 5; /* RBP */
        *frame_offset_out = 0;
        p += 3;
        non_push_skips++;
      } else if (non_push_skips < max_non_push_skips) {
        /*
         * Try to skip other MOV/LEA instructions that compilers place
         * in prologues (e.g., saving argument registers).
         * We use a simple heuristic: if the byte has a REX prefix (0x48-0x4F)
         * followed by MOV (0x89, 0x8B) or LEA (0x8D), skip the instruction.
         */
        if (p + 2 < end && (*p & 0xF0) == 0x40) {
          uint8_t opcode = p[1];
          if (opcode == 0x89 || opcode == 0x8B || opcode == 0x8D) {
            /* REX + MOV/LEA — determine instruction length from ModR/M */
            uint8_t modrm = p[2];
            uint8_t mod = modrm >> 6;
            uint8_t rm = modrm & 7;
            int len = 3; /* REX + opcode + ModR/M */
            if (rm == 4 && mod != 3) len++; /* SIB byte */
            if (mod == 1) len += 1;  /* disp8 */
            else if (mod == 2) len += 4;  /* disp32 */
            else if (mod == 0 && rm == 5) len += 4; /* RIP-relative */
            if (p + len <= end) {
              p += len;
              non_push_skips++;
              continue;
            }
          }
        }
        break; /* Unknown instruction — stop */
      } else {
        break;
      }
    }
  }

  /*
   * Parse SUB RSP, imm.
   *
   * sljit may insert MOV instructions between the PUSHes and SUB RSP
   * (e.g., to move argument registers to callee-saved registers).
   * Scan forward up to 64 bytes looking for the SUB RSP pattern.
   * All instructions between the last PUSH and SUB RSP are part of
   * the prologue from the unwinder's perspective.
   */
  {
    const uint8_t *scan = p;
    const uint8_t *scan_end = p + 64;
    if (scan_end > end) scan_end = end;

    while (scan + 4 <= scan_end) {
      if (scan[0] == 0x48 && scan[1] == 0x83 && scan[2] == 0xEC) {
        /* SUB RSP, imm8 */
        alloc_size = scan[3];
        alloc_offset = (uint8_t)(scan + 4 - code);
        p = scan + 4;
        break;
      } else if (scan + 7 <= scan_end &&
                 scan[0] == 0x48 && scan[1] == 0x81 && scan[2] == 0xEC) {
        /* SUB RSP, imm32 */
        alloc_size = *(uint32_t *)(scan + 3);
        alloc_offset = (uint8_t)(scan + 7 - code);
        p = scan + 7;
        break;
      }
      scan++;
    }
  }
  /* If no SUB RSP found: might be a very small leaf function */

  /*
   * Detect MIR-style frame pointer setup after SUB RSP:
   *   MOV [RSP+disp8], RBP    → 48 89 6C 24 XX (5 bytes)
   *   MOV RBP, R10            → 49 8B EA       (3 bytes)
   *
   * MIR's prologue with MIR_NO_RED_ZONE_ABI:
   *   LEA R10, [RSP-8]        (before SUB RSP, already skipped)
   *   SUB RSP, N
   *   MOV [RSP+N-8], RBP      (save old RBP)
   *   MOV RBP, R10             (set frame pointer = old_RSP - 8)
   */
  if (alloc_size > 0 && p + 7 <= end) {
    const uint8_t *q = NULL;
    if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x6C && p[3] == 0x24) {
      /* MOV [RSP+disp8], RBP — 48 89 6C 24 XX (5 bytes) */
      q = p + 5;
    } else if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x2C && p[3] == 0x24) {
      /* MOV [RSP], RBP — 48 89 2C 24 (4 bytes, zero displacement) */
      q = p + 4;
    }
    if (q != NULL) {
      /* Check for MOV RBP, R10 (49 8B EA) or MOV RBP, RAX (48 8B E8) */
      if (q + 3 <= end &&
          ((q[0] == 0x49 && q[1] == 0x8B && q[2] == 0xEA) ||   /* MOV RBP, R10 */
           (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0xE8))) {  /* MOV RBP, RAX */
        /* Frame pointer is established: RBP = old_RSP - 8 = RSP + alloc_size - 8 */
        uint32_t fp_rsp_offset = alloc_size - 8;
        set_fpreg_offset = (uint8_t)(q + 3 - code);
        p = q + 3;
        *frame_reg_out = 5; /* RBP */
        *frame_offset_out = (uint8_t)(fp_rsp_offset / 16);
      }
    }
  }

  *prolog_size_out = (uint8_t)(p - code);

  /*
   * Build UNWIND_CODE array in reverse chronological order
   * (last prologue instruction first = highest offset first).
   *
   * We have three types of entries to merge:
   *   - ALLOC (SUB RSP): offset varies by backend
   *   - SET_FPREG (MOV RBP,RSP or similar): offset varies by backend
   *   - PUSH_NONVOL: in pushes_offset[] array, ascending order
   *
   * The relative order varies by backend:
   *   sljit/AsmJIT: PUSH...; [MOV RBP,RSP;] SUB RSP,N → ALLOC last
   *   MIR:          SUB RSP,N; MOV [RSP+off],RBP; MOV RBP,R10 → SET_FPREG last
   *
   * Strategy: merge all three in descending offset order.  We track which
   * of ALLOC, SET_FPREG, and the push array has the highest remaining
   * offset and emit that one next.
   */
  {
    bool alloc_emitted = (alloc_size == 0);
    bool fpreg_emitted = (set_fpreg_offset == 0);
    int pi = npushes - 1;  /* current push index, descending */

    while (!alloc_emitted || !fpreg_emitted || pi >= 0) {
      uint8_t push_off = (pi >= 0) ? (uint8_t)(pushes_offset[pi] + 1) : 0;
      uint8_t a_off = alloc_emitted ? 0 : alloc_offset;
      uint8_t f_off = fpreg_emitted ? 0 : set_fpreg_offset;

      /* Find which has the highest offset */
      if (!alloc_emitted && a_off >= f_off && a_off >= push_off) {
        /* Emit ALLOC */
        if (alloc_size <= 128 && (alloc_size % 8) == 0) {
          if (ncodes >= max_codes) return 0;
          unwind_codes[ncodes * 2]     = alloc_offset;
          unwind_codes[ncodes * 2 + 1] = (uint8_t)(UWOP_ALLOC_SMALL | (((alloc_size / 8) - 1) << 4));
          ncodes++;
        } else if (alloc_size <= 512 * 1024 - 8 && (alloc_size % 8) == 0) {
          if (ncodes + 1 >= max_codes) return 0;
          unwind_codes[ncodes * 2]     = alloc_offset;
          unwind_codes[ncodes * 2 + 1] = (uint8_t)(UWOP_ALLOC_LARGE | (0 << 4));
          ncodes++;
          *(uint16_t *)(unwind_codes + ncodes * 2) = (uint16_t)(alloc_size / 8);
          ncodes++;
        } else {
          if (ncodes + 2 >= max_codes) return 0;
          unwind_codes[ncodes * 2]     = alloc_offset;
          unwind_codes[ncodes * 2 + 1] = (uint8_t)(UWOP_ALLOC_LARGE | (1 << 4));
          ncodes++;
          *(uint32_t *)(unwind_codes + ncodes * 2) = alloc_size;
          ncodes += 2;
        }
        alloc_emitted = true;
      } else if (!fpreg_emitted && f_off >= push_off) {
        /* Emit SET_FPREG */
        if (ncodes >= max_codes) return 0;
        unwind_codes[ncodes * 2]     = set_fpreg_offset;
        unwind_codes[ncodes * 2 + 1] = (uint8_t)(UWOP_SET_FPREG | (0 << 4));
        ncodes++;
        fpreg_emitted = true;
      } else if (pi >= 0) {
        /* Emit PUSH_NONVOL */
        if (ncodes >= max_codes) return 0;
        unwind_codes[ncodes * 2]     = push_off;
        unwind_codes[ncodes * 2 + 1] = (uint8_t)(UWOP_PUSH_NONVOL | (push_reg[pi] << 4));
        ncodes++;
        pi--;
      } else {
        break; /* shouldn't happen */
      }
    }
  }

  return ncodes;
}

void pg_jitter_win64_register_unwind(void *code, size_t code_size) {
  Win64UnwindCtx *ctx;
  DWORD64 base = (DWORD64)code;
  int ncodes;
  uint8_t prolog_size;
  uint8_t frame_reg, frame_offset;
  uint8_t codes_buf[WIN64_MAX_UNWIND_CODES * 2];

  if (!code || code_size == 0)
    return;

  /* Parse the prologue to build accurate unwind codes */
  ncodes = win64_parse_prologue((const uint8_t *)code, code_size,
                                codes_buf, WIN64_MAX_UNWIND_CODES,
                                &prolog_size, &frame_reg, &frame_offset);

  /*
   * If we couldn't parse any unwind codes, don't register bogus unwind info.
   * A zero-code UNWIND_INFO claims "leaf function" which is incorrect for
   * functions that push registers or allocate stack, causing 0xC0000028.
   *
   * Log the first 32 bytes of the prologue so we can diagnose what the
   * compiler generated. Use LOG level to avoid polluting client output
   * (WARNING would appear in regression test results).
   */
  {
    const uint8_t *bytes = (const uint8_t *)code;
    int dump_len = code_size < 32 ? (int)code_size : 32;
    char hex[97]; /* 32*3 + 1 */
    for (int i = 0; i < dump_len; i++)
      snprintf(hex + i * 3, 4, "%02X ", bytes[i]);
    hex[dump_len * 3] = '\0';
    elog(DEBUG2, "pg_jitter: win64 unwind prologue: %s "
         "(ncodes=%d prolog=%u frame_reg=%u frame_off=%u)",
         hex, ncodes, prolog_size, frame_reg, frame_offset);

    if (ncodes == 0) {
      elog(DEBUG2, "pg_jitter: could not parse prologue for unwind info, "
           "error handlers through this JIT frame may crash");
      return;
    }
  }

  ctx = (Win64UnwindCtx *)malloc(sizeof(Win64UnwindCtx));
  if (!ctx)
    return;

  ctx->code_base = base;
  ctx->code_size_dw = (DWORD)code_size;

  /* Build UNWIND_INFO header */
  ctx->unwind_info[0] = 0x01;       /* Version=1, Flags=0 */
  ctx->unwind_info[1] = prolog_size; /* SizeOfProlog */
  ctx->unwind_info[2] = (uint8_t)ncodes; /* CountOfCodes */
  ctx->unwind_info[3] = (uint8_t)((frame_reg & 0x0F) | ((frame_offset & 0x0F) << 4));

  /* Copy unwind codes */
  if (ncodes > 0)
    memcpy(ctx->unwind_info + 4, codes_buf, ncodes * 2);

  /*
   * RUNTIME_FUNCTION RVAs are relative to the BaseAddress passed to
   * RtlAddFunctionTable.  We use min(ctx, code) as the base so all RVAs
   * are positive DWORDs, regardless of whether the heap-allocated ctx is
   * above or below the JIT code in the address space.
   *
   * We use RtlAddFunctionTable instead of RtlInstallFunctionTableCallback
   * because the callback approach registers a dynamic function table that
   * covers a large span [min_addr, min_addr+span).  This span can shadow
   * static function tables (.pdata) of other modules loaded in the same
   * address range, causing STATUS_STACK_BUFFER_OVERRUN (0xC00000FF) during
   * longjmp when the unwinder can't find proper unwind info for non-JIT
   * frames within the span.
   *
   * RtlAddFunctionTable registers a table that only covers the specific
   * address ranges declared in the RUNTIME_FUNCTION entries, avoiding
   * the shadowing problem entirely.
   */
  {
    DWORD64 ctx_addr = (DWORD64)ctx;
    DWORD64 min_addr = (ctx_addr < base) ? ctx_addr : base;
    DWORD64 code_end = base + code_size;
    DWORD64 ctx_end  = ctx_addr + sizeof(Win64UnwindCtx);
    DWORD64 max_end  = (code_end > ctx_end) ? code_end : ctx_end;
    DWORD64 span     = max_end - min_addr;

    /* Sanity: span must fit in DWORD */
    if (span > 0x7FFFFFFF) {
      elog(WARNING, "pg_jitter: unwind span too large (%llu), skipping",
           (unsigned long long)span);
      free(ctx);
      return;
    }

    ctx->rf.BeginAddress = (DWORD)(base - min_addr);
    ctx->rf.EndAddress   = (DWORD)(code_end - min_addr);
    ctx->rf.UnwindData   = (DWORD)((DWORD64)ctx->unwind_info - min_addr);

    elog(DEBUG2, "pg_jitter: win64 unwind registered code=%p size=%zu "
         "prolog=%u ncodes=%d base=0x%llX",
         code, code_size, prolog_size, ncodes,
         (unsigned long long)min_addr);

    if (!RtlAddFunctionTable(&ctx->rf, 1, min_addr)) {
      elog(WARNING, "pg_jitter: RtlAddFunctionTable FAILED");
      free(ctx);
      return;
    }
  }

  /* Link into list for lookup during deregistration */
  ctx->next = win64_unwind_list;
  win64_unwind_list = ctx;
}

void pg_jitter_win64_deregister_unwind(void *code) {
  Win64UnwindCtx **pp, *ctx;
  DWORD64 base;

  if (!code)
    return;

  base = (DWORD64)code;

  /* Find and unlink from list */
  for (pp = &win64_unwind_list; *pp != NULL; pp = &(*pp)->next) {
    if ((*pp)->code_base == base) {
      ctx = *pp;
      *pp = ctx->next;

      /* Deregister: pass the same RUNTIME_FUNCTION pointer used in RtlAddFunctionTable */
      RtlDeleteFunctionTable(&ctx->rf);

      free(ctx);
      return;
    }
  }
}

#endif /* _WIN64 */

