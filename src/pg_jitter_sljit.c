/*
 * pg_jitter_sljit.c — sljit-based PostgreSQL JIT provider
 *
 * Walks ExprState->steps[] and emits native code using sljit's LIR.
 * Hot-path opcodes (~30) get native code; everything else falls back to
 * pg_jitter_fallback_step().
 */
#include "postgres.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "executor/execExpr.h"
#include "executor/nodeAgg.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "utils/expandeddatum.h"
#include "utils/palloc.h"

#include "pg_jitter_common.h"
#include "pg_jit_funcs.h"
#include "pg_jit_deform_templates.h"
#include "utils/fmgrprotos.h"
#include "sljitLir.h"

#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "access/parallel.h"
#include "utils/guc.h"
#include "common/hashfn.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

/*
 * Mirror of Int8TransTypeData from numeric.c (not exported in headers).
 * Used by int2_avg_accum / int4_avg_accum: an ArrayType wrapping {count, sum}.
 */
typedef struct
{
	int64		count;
	int64		sum;
} JitInt8TransTypeData;

/* W^X and I-cache support for code patching (precompiled blobs) */
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>  /* sys_icache_invalidate */
#endif

/* MIR precompiled blob support (shared infrastructure) */
#include "pg_jit_mir_blobs.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_jitter_sljit",
);

/* GUC enum options for pg_jitter.parallel_mode */
static const struct config_enum_entry parallel_jit_options[] = {
	{"off", PARALLEL_JIT_OFF, false},
	{"per_worker", PARALLEL_JIT_PER_WORKER, false},
	{"shared", PARALLEL_JIT_SHARED, false},
	{NULL, 0, false}
};

/*
 * Per-compilation flag: when true, EMIT_ICALL forces SLJIT_REWRITABLE_JUMP
 * to prevent sljit from converting calls to PC-relative BL instructions.
 * Set by sljit_compile_expr when the code will be shared via DSM.
 * File-scoped so helper functions (emit_inline_funcexpr etc.) can access it.
 */
static bool sljit_shared_code_mode = false;

/* GUC variable for _shared_dsm (needed when loaded standalone without meta) */
static char *sljit_shared_dsm_guc = NULL;

/* Forward declarations */
static bool sljit_compile_expr(ExprState *state);
static void sljit_code_free(void *data);

/*
 * Provider entry point — called by PG when loading the JIT provider.
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = pg_jitter_reset_after_error;
	cb->release_context = pg_jitter_release_context;
	cb->compile_expr = sljit_compile_expr;

	/*
	 * Define GUCs only if not already registered (avoids conflict when
	 * loaded via the meta module after another backend already defined them).
	 */
	if (!GetConfigOption("pg_jitter.shared_code_max", true, false))
	{
		DefineCustomIntVariable(
			"pg_jitter.shared_code_max",
			"Maximum shared JIT code DSM size in KB.",
			NULL,
			&pg_jitter_shared_code_max_kb,
			4096,		/* 4 MB default */
			64,			/* 64 KB minimum */
			1048576,	/* 1 GB maximum */
			PGC_USERSET,
			GUC_UNIT_KB | GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.parallel_mode", true, false))
	{
		DefineCustomEnumVariable(
			"pg_jitter.parallel_mode",
			"Controls JIT behavior in parallel workers: "
			"off (workers use interpreter), "
			"per_worker (each worker compiles independently), "
			"shared (leader shares compiled code via DSM)",
			NULL,
			&pg_jitter_parallel_mode,
			PARALLEL_JIT_SHARED,
			parallel_jit_options,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter._shared_dsm", true, false))
	{
		DefineCustomStringVariable(
			"pg_jitter._shared_dsm",
			"Internal: DSM handle for parallel JIT code sharing.",
			NULL,
			&sljit_shared_dsm_guc,
			"",
			PGC_USERSET,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
			NULL, NULL, NULL);
	}
}

/*
 * Free sljit compiled code.
 */
static void
sljit_code_free(void *data)
{
	if (data)
		sljit_free_code(data, NULL);
}

/*
 * Compile an expression using sljit.
 *
 * Generated function signature:
 *   Datum fn(ExprState *state, ExprContext *econtext, bool *isNull)
 *
 * Register allocation (saved registers survive function calls):
 *   S0 = ExprState *state
 *   S1 = ExprContext *econtext
 *   S2 = bool *isNull
 *   S3 = AggState *aggstate      (aggregate expressions only)
 *   S4 = &CurrentMemoryContext   (aggregate expressions only)
 *   R0..R3 = scratch
 *
 * Local stack layout:
 *   [SP + 0]   = (unused, formerly state->resvalue pointer)
 *   [SP + 8]   = (unused, formerly state->resnull pointer)
 *   [SP + 16]  = resultslot pointer (TupleTableSlot*)
 *   [SP + 24]  = resultslot->tts_values (Datum*)
 *   [SP + 32]  = resultslot->tts_isnull (bool*)
 *   [SP + 72..144] = cached source-slot tts_values/tts_isnull
 *                     (set by FETCHSOME, used by VAR/ASSIGN_VAR)
 *
 * Source-slot value/null pointers are cached after each FETCHSOME call
 * (either compiled deform or slot_getsomeattrs_int) so that subsequent
 * VAR and ASSIGN_VAR opcodes avoid re-chasing the slot pointer.
 */

/* Stack offsets for cached pointers */
#define SOFF_RESULTSLOT   16
#define SOFF_AGG_OLDCTX   24	/* saved CurrentMemoryContext across fn_addr */
#define SOFF_AGG_PERGROUP 32	/* runtime pergroup pointer (changes per tuple) */
#define SOFF_AGG_FCINFO   40	/* fcinfo pointer, avoid reloading 64-bit IMM */
#define SOFF_AGG_CURRMCTXP 48	/* &CurrentMemoryContext (was in S4, now stack) */
#define SOFF_TEMP         56	/* temporary scratch across function calls */
/* Cached source-slot tts_values/tts_isnull pointers (set by FETCHSOME) */
#define SOFF_INNER_VALS   64
#define SOFF_INNER_NULLS  72
#define SOFF_OUTER_VALS   80
#define SOFF_OUTER_NULLS  88
#define SOFF_SCAN_VALS    96
#define SOFF_SCAN_NULLS   104
#define SOFF_OLD_VALS     112
#define SOFF_OLD_NULLS    120
#define SOFF_NEW_VALS     128
#define SOFF_NEW_NULLS    136
#define SOFF_STEPS        144	/* state->steps pointer (for steps-relative addr) */
#define SOFF_TOTAL        152

/*
 * Inline deform temporaries — reuse AGG stack slots (no temporal overlap:
 * AGG slots are used only during AGG_PLAIN_TRANS steps, deform temporaries
 * are used only within the FETCHSOME handler).
 *
 * Register-optimized layout: S3=tupdata_base, S4=tts_values, S5=tts_isnull,
 * R3=deform_off. Saved registers are spilled to stack during deform and
 * restored in the epilogue.
 */
#define SOFF_DEFORM_SAVE_S3   SOFF_AGG_OLDCTX     /* 24: saved S3 (resultvals) */
#define SOFF_DEFORM_SAVE_S4   SOFF_AGG_PERGROUP   /* 32: saved S4 (resultnulls) */
#define SOFF_DEFORM_SAVE_S5   SOFF_AGG_FCINFO     /* 40: saved S5 (aggstate/hash) */
#define SOFF_DEFORM_TBITS     SOFF_RESULTSLOT     /* 16: t_bits pointer (resultslot not used during deform) */
#define SOFF_DEFORM_MAXATT    SOFF_TEMP           /* 56: maxatt from infomask2 */
/*
 * SOFF_DEFORM_HASNULLS: stored at vals_off+8 (the slot-cache area for tts_isnull).
 * During inline deform, tts_values lives in S4, so vals_off is unused.
 * Note: NOT at SOFF_AGG_CURRMCTXP (48) — that holds &CurrentMemoryContext
 * across the entire expression and must not be overwritten.
 */

/*
 * Saved register assignments:
 *   S0 = ExprState *state  (function argument)
 *   S1 = ExprContext *econtext  (function argument)
 *   S2 = bool *isNull  (function argument)
 *   S3 = resultslot->tts_values  (loaded in prologue)
 *   S4 = resultslot->tts_isnull  (loaded in prologue)
 *   S5 = AggState *aggstate  (when has_agg) OR sreg_hash (when has_hash_next && !has_agg)
 *
 * Max 6 saved registers — safe on all sljit architectures
 * (SLJIT_NUMBER_OF_SAVED_REGISTERS >= 6 everywhere, including x86-64).
 * &CurrentMemoryContext is accessed only twice per AGG_TRANS (save + restore)
 * so it moves to SOFF_AGG_CURRMCTXP on the stack.
 */
#define SREG_RESULTVALS  SLJIT_S3
#define SREG_RESULTNULLS SLJIT_S4

/*
 * Helper: emit code to load a slot pointer from econtext (in S1).
 */
static void
emit_load_econtext_slot(struct sljit_compiler *C, sljit_s32 dst,
						ExprEvalOp opcode)
{
	sljit_sw	offset;

	switch (opcode)
	{
		/* FETCHSOME/VAR/ASSIGN variants: determine slot by opcode suffix */
		case EEOP_INNER_FETCHSOME:
		case EEOP_INNER_VAR:
		case EEOP_ASSIGN_INNER_VAR:
			offset = offsetof(ExprContext, ecxt_innertuple);
			break;
		case EEOP_OUTER_FETCHSOME:
		case EEOP_OUTER_VAR:
		case EEOP_ASSIGN_OUTER_VAR:
			offset = offsetof(ExprContext, ecxt_outertuple);
			break;
		case EEOP_SCAN_FETCHSOME:
		case EEOP_SCAN_VAR:
		case EEOP_ASSIGN_SCAN_VAR:
			offset = offsetof(ExprContext, ecxt_scantuple);
			break;

#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_FETCHSOME:
		case EEOP_OLD_VAR:
		case EEOP_ASSIGN_OLD_VAR:
			offset = offsetof(ExprContext, ecxt_oldtuple);
			break;
		case EEOP_NEW_FETCHSOME:
		case EEOP_NEW_VAR:
		case EEOP_ASSIGN_NEW_VAR:
			offset = offsetof(ExprContext, ecxt_newtuple);
			break;
#endif
		default:
			offset = offsetof(ExprContext, ecxt_scantuple);
			break;
	}

	sljit_emit_op1(C, SLJIT_MOV, dst, 0,
				   SLJIT_MEM1(SLJIT_S1), offset);
}

/*
 * Check if the expression matches one of PG's interpreter fast-path
 * patterns (ExecReadyInterpretedExpr in execExprInterp.c).  These are
 * hand-optimized C functions (ExecJustInnerVar, ExecJustHashInnerVar,
 * etc.) that beat our sljit JIT for tiny 2-5 step expressions because
 * they skip all dispatch overhead.  Return true if the expression would
 * be eligible for a fast-path, so the caller can bail out and let the
 * interpreter claim it.
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


#ifdef HAVE_EEOP_HASHDATUM
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
	else
#endif /* HAVE_EEOP_HASHDATUM */
	if (nsteps == 3)
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
			(step1 == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			 || step1 == EEOP_FUNCEXPR_STRICT_1
			 || step1 == EEOP_FUNCEXPR_STRICT_2
#endif
			))
			return true;

#ifdef HAVE_EEOP_HASHDATUM
		/* VAR + HASHDATUM_FIRST (virtual slot hash, no fetchsome) */
		if (step0 == EEOP_INNER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_OUTER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
#endif
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
 * Deform compilation functions (pg_jitter_compile_deform,
 * pg_jitter_compile_deform_loop, pg_jitter_compiled_deform_dispatch) are in
 * pg_jitter_deform_jit.c — shared across all backends.
 */

typedef void (*deform_func_t)(TupleTableSlot *slot);

/* Legacy wrappers for find_or_compile_deform (file-local cache) */
static void *
sljit_compile_deform(TupleDesc desc,
                     const TupleTableSlotOps *ops,
                     int natts)
{
    return pg_jitter_compile_deform(desc, ops, natts);
}

/* (old sljit_compile_deform body removed — now in pg_jitter_deform_jit.c) */

static void *
sljit_compile_deform_loop(TupleDesc desc,
                          const TupleTableSlotOps *ops,
                          int natts)
{
    return pg_jitter_compile_deform_loop(desc, ops, natts, NULL, NULL, NULL);
}


/*
 * Try to match a pre-compiled deform template for the given tuple descriptor.
 * Returns a template function pointer if all columns are fixed-width, byval,
 * NOT NULL, and within the coverage matrix; NULL otherwise.
 *
 * Pre-compiled templates live in the shared library's .text section, so all
 * parallel workers share the same virtual address — eliminating L1 I-cache
 * coldness from per-worker sljit compilation.
 */
static deform_template_fn
deform_match_template(TupleDesc desc,
					  const TupleTableSlotOps *ops,
					  int natts)
{
	int16	attlens[5];

	if (natts < 1 || natts > 5)
		return NULL;

	/* All physical slot types supported (heap, buffer-heap, minimal) */
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

/*
 * Deform cache entry: maps (TupleDesc, slot_ops, natts) → compiled deform fn.
 */
#define MAX_DEFORM_CACHE 8

typedef struct DeformCacheEntry
{
    TupleDesc                   desc;
    const TupleTableSlotOps    *ops;
    int                         natts;
    void                       *code;      /* compiled deform function */
} DeformCacheEntry;

/*
 * Look up or compile a deform function for the given (desc, ops, natts).
 * Returns compiled function pointer, or NULL if deform cannot be compiled.
 * Newly compiled functions are appended to the cache.
 */
static void *
find_or_compile_deform(PgJitterContext *ctx,
                       DeformCacheEntry *cache, int *ncache,
                       TupleDesc desc,
                       const TupleTableSlotOps *ops,
                       int natts)
{
    /* Check cache for existing entry */
    for (int i = 0; i < *ncache; i++)
    {
        if (cache[i].desc == desc &&
            cache[i].ops == ops &&
            cache[i].natts == natts)
            return cache[i].code;
    }

    /* Compile new deform function */
    if (*ncache >= MAX_DEFORM_CACHE)
        return NULL;

    {
        instr_time  deform_start, deform_end;
        void       *code;

        INSTR_TIME_SET_CURRENT(deform_start);
        if (natts > pg_jitter_deform_threshold())
            code = sljit_compile_deform_loop(desc, ops, natts);
        else
            code = sljit_compile_deform(desc, ops, natts);
        INSTR_TIME_SET_CURRENT(deform_end);
        JITTER_INSTR_DEFORM_ACCUM(ctx->base.instr,
                              deform_end, deform_start);

        if (code)
        {
            /* Register for cleanup */
            pg_jitter_register_compiled(ctx, sljit_code_free, code);
            ctx->base.instr.created_functions++;

            cache[*ncache].desc = desc;
            cache[*ncache].ops = ops;
            cache[*ncache].natts = natts;
            cache[*ncache].code = code;
            (*ncache)++;
        }

        return code;
    }
}

/*
 * pg_jitter_compiled_deform_dispatch is now in pg_jitter_deform_jit.c
 * (shared across all backends).
 */

/*
 * Indirect call helper.
 *
 * When generating code that will be shared via DSM (sljit_shared_code_mode),
 * we need absolute addressing so that the relocation scanner can find and
 * patch embedded function pointers.
 *
 * Without SLJIT_REWRITABLE_JUMP, sljit may optimize calls to relative form:
 *   - ARM64: PC-relative BL (26-bit offset)
 *   - x86_64: relative CALL (E8 + 32-bit offset)
 * Both break when code is memcpy'd to a different address in a parallel worker.
 *
 * With SLJIT_REWRITABLE_JUMP, sljit always emits absolute addressing:
 *   - ARM64: MOVZ+3×MOVK+BLR
 *   - x86_64: MOVABS r, imm64 + CALL r
 * The relocation scanner can then find and patch the embedded addresses.
 *
 * Note: SLJIT_REWRITABLE_JUMP is only valid on sljit_emit_call/sljit_emit_jump,
 * NOT on sljit_emit_icall — hence the sljit_emit_call + sljit_set_target pattern.
 */
#define EMIT_ICALL(C, type, arg_types, fn) \
    do { \
        if (sljit_shared_code_mode) { \
            struct sljit_jump *_j = sljit_emit_call(C, \
                (type) | SLJIT_REWRITABLE_JUMP, arg_types); \
            sljit_set_target(_j, (sljit_uw)(fn)); \
        } else { \
            sljit_emit_icall(C, type, arg_types, SLJIT_IMM, (sljit_sw)(fn)); \
        } \
    } while (0)

/*
 * Emit deform code inline into the expression function body.
 *
 * Unlike sljit_compile_deform() which creates a separate function with its
 * own prologue/epilogue, this emits deform code directly into the expression
 * function's instruction stream. The benefit is zero call overhead and
 * contiguous I-cache locality — matching asmjit's inline deform approach.
 *
 * Register usage (within deform code only):
 *   R0-R3 = scratch (caller-saved, clobbered by icalls)
 *   S0 = state, S1 = econtext (expression function's saved regs, read-only)
 *   Stack slots SOFF_DEFORM_* for temporaries (reuse AGG slots)
 *
 * The slot's tts_values/tts_isnull pointers are written to the slot-cache
 * stack area (vals_off / vals_off+8), so the caller can skip the
 * post-FETCHSOME cache population.
 *
 * Returns true if deform code was emitted, false otherwise (caller should
 * fall back to compiled deform or slot_getsomeattrs_int).
 */
static bool
sljit_emit_deform_inline(struct sljit_compiler *C,
                          TupleDesc desc,
                          const TupleTableSlotOps *ops,
                          int natts,
                          ExprEvalOp fetch_opcode,
                          sljit_sw vals_off,
                          bool sljit_shared_code_mode)
{
    int     attnum;
    int     known_alignment = 0;
    bool    attguaranteedalign = true;
    int     guaranteed_column_number = -1;
    sljit_sw tuple_off;
    sljit_sw slot_off;

    /* Forward-jump arrays */
    struct sljit_jump **nvalid_jumps;
    struct sljit_jump **avail_jumps;
    struct sljit_jump **null_jumps;
    struct sljit_label **att_labels;
    struct sljit_jump  *nvalid_default;

    /* --- Guards --- */
    if (ops == &TTSOpsVirtual)
        return false;
    if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple &&
        ops != &TTSOpsMinimalTuple)
        return false;
    if (natts <= 0 || natts > desc->natts)
        return false;

    /* Wide tables: skip inline deform, fall through to compiled loop deform */
    if (natts > pg_jitter_deform_threshold())
        return false;

    /*
     * Skip inline deform for MinimalTupleTableSlot. The inline deform
     * emits its code directly in the expression function body, sharing
     * the stack frame. For MinimalTuple slots (used by Sort, tuplesort),
     * this causes data corruption when the expression also contains
     * aggregate transition steps that reuse the same stack slots
     * (SOFF_DEFORM_* overlap with SOFF_AGG_*). Fall back to compiled
     * deform (separate function) or slot_getsomeattrs_int.
     */
    if (ops == &TTSOpsMinimalTuple)
        return false;

    /* Determine slot-type-specific field offsets */
    if (ops == &TTSOpsHeapTuple || ops == &TTSOpsBufferHeapTuple)
    {
        tuple_off = offsetof(HeapTupleTableSlot, tuple);
        slot_off = offsetof(HeapTupleTableSlot, off);
    }
    else
    {
        tuple_off = offsetof(MinimalTupleTableSlot, tuple);
        slot_off = offsetof(MinimalTupleTableSlot, off);
    }

    /* --- Pre-scan: find guaranteed_column_number --- */
    for (attnum = 0; attnum < natts; attnum++)
    {
        CompactAttribute *att = TupleDescCompactAttr(desc, attnum);

        if (JITTER_ATT_IS_NOTNULL(att) &&
            !att->atthasmissing &&
            !att->attisdropped)
            guaranteed_column_number = attnum;
    }

    /* Allocate forward-jump tracking arrays */
    nvalid_jumps = palloc0(sizeof(struct sljit_jump *) * natts);
    avail_jumps = palloc0(sizeof(struct sljit_jump *) * natts);
    null_jumps = palloc0(sizeof(struct sljit_jump *) * natts);
    att_labels = palloc0(sizeof(struct sljit_label *) * natts);

    /*
     * PROLOGUE: load slot fields.  We temporarily repurpose S3-S5 for
     * deform state to avoid per-column stack traffic.  The original
     * S3/S4/S5 values (resultvals, resultnulls, aggstate/hash) are
     * saved to stack and restored in the epilogue.
     *
     * Register allocation during inline deform:
     *   S3 = tupdata_base   (was on stack)
     *   S4 = tts_values     (was loaded from stack every column)
     *   S5 = tts_isnull     (was loaded from stack every column)
     *   R3 = deform_off     (was loaded/stored to stack every column)
     *   R0-R2 = scratch
     */
    /* Save S3, S4, S5 to stack (one-time cost) */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S3,
                   SLJIT_S3, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S4,
                   SLJIT_S4, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S5,
                   SLJIT_S5, 0);

    emit_load_econtext_slot(C, SLJIT_R0, fetch_opcode);

    /* S4 = slot->tts_values */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S4, 0,
                   SLJIT_MEM1(SLJIT_R0),
                   offsetof(TupleTableSlot, tts_values));

    /* S5 = slot->tts_isnull */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S5, 0,
                   SLJIT_MEM1(SLJIT_R0),
                   offsetof(TupleTableSlot, tts_isnull));

    /* R1 = HeapTuple ptr from slot-type-specific offset */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                   SLJIT_MEM1(SLJIT_R0), tuple_off);

    /* R1 = tuplep = heaptuple->t_data (HeapTupleHeader) */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleData, t_data));

    /* t_infomask -> R2 (uint16) */
    sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleHeaderData, t_infomask));
    /* hasnulls = infomask & HEAP_HASNULL → stack */
    sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
                   SLJIT_R2, 0, SLJIT_IMM, HEAP_HASNULL);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), vals_off + 8,
                   SLJIT_R2, 0);  /* hasnulls → vals_off+8 (slot cache unused during deform) */

    /* t_infomask2 -> maxatt = infomask2 & HEAP_NATTS_MASK → stack */
    sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleHeaderData, t_infomask2));
    sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
                   SLJIT_R2, 0, SLJIT_IMM, HEAP_NATTS_MASK);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_MAXATT,
                   SLJIT_R2, 0);

    /* t_bits → stack */
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0,
                   SLJIT_R1, 0,
                   SLJIT_IMM, offsetof(HeapTupleHeaderData, t_bits));
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_TBITS,
                   SLJIT_R2, 0);

    /* t_hoff -> R2 (zero-extended uint8) */
    sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleHeaderData, t_hoff));

    /* S3 = tupdata_base = (char *)tuplep + t_hoff */
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_S3, 0,
                   SLJIT_R1, 0, SLJIT_R2, 0);

    /* R3 = deform_off (loaded from slot->off) */
    emit_load_econtext_slot(C, SLJIT_R0, fetch_opcode);
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R3, 0,
                   SLJIT_MEM1(SLJIT_R0), slot_off);

    /* ============================================================
     * MISSING ATTRIBUTES CHECK
     * ============================================================ */
    if ((natts - 1) > guaranteed_column_number)
    {
        struct sljit_jump *skip_missing;

        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_MAXATT);
        skip_missing = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
                                      SLJIT_R0, 0,
                                      SLJIT_IMM, natts);

        /* call slot_getmissingattrs(slot, maxatt, natts)
         * icall clobbers R0-R3, save R3 (deform_off) around it. */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), vals_off,
                       SLJIT_R3, 0);
        emit_load_econtext_slot(C, SLJIT_R0, fetch_opcode);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_MAXATT);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                       SLJIT_IMM, natts);
        EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, 32, 32), slot_getmissingattrs);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                       SLJIT_MEM1(SLJIT_SP), vals_off);

        sljit_set_label(skip_missing, sljit_emit_label(C));
    }

    /* ============================================================
     * NVALID DISPATCH: comparison chain
     * ============================================================ */
    {
        /* Reload slot to read tts_nvalid */
        emit_load_econtext_slot(C, SLJIT_R0, fetch_opcode);
        sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_R0),
                       offsetof(TupleTableSlot, tts_nvalid));

        for (attnum = 0; attnum < natts; attnum++)
        {
            nvalid_jumps[attnum] = sljit_emit_cmp(C, SLJIT_EQUAL,
                                                  SLJIT_R0, 0,
                                                  SLJIT_IMM, attnum);
        }
        /* Default: already deformed enough -> goto out */
        nvalid_default = sljit_emit_jump(C, SLJIT_JUMP);
    }

    /* ============================================================
     * PER-ATTRIBUTE CODE EMISSION (unrolled loop)
     * ============================================================ */
    for (attnum = 0; attnum < natts; attnum++)
    {
        CompactAttribute *att = TupleDescCompactAttr(desc, attnum);
        int     alignto = JITTER_ATTALIGNBY(att);

        /* ---- Emit attcheck label and wire up nvalid dispatch ---- */
        att_labels[attnum] = sljit_emit_label(C);
        sljit_set_label(nvalid_jumps[attnum], att_labels[attnum]);

        /* Patch previous null-path forward jump if it targeted this label */
        if (attnum > 0 && null_jumps[attnum - 1] != NULL)
            sljit_set_label(null_jumps[attnum - 1], att_labels[attnum]);

        /* If attnum == 0: reset offset to 0 */
        if (attnum == 0)
        {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_IMM, 0);
        }

        /* ---- Availability check ---- */
        if (attnum > guaranteed_column_number)
        {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_MAXATT);
            /* if attnum >= maxatt -> goto out (patched later) */
            avail_jumps[attnum] = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
                                                 SLJIT_IMM, attnum,
                                                 SLJIT_R0, 0);
        }

        /* ---- Null check ---- */
        if (!JITTER_ATT_IS_NOTNULL(att))
        {
            struct sljit_jump *no_hasnulls;
            struct sljit_jump *bit_is_set;

            /* if (!hasnulls) skip to not-null path */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_SP), vals_off + 8);  /* hasnulls */
            no_hasnulls = sljit_emit_cmp(C, SLJIT_EQUAL,
                                         SLJIT_R0, 0,
                                         SLJIT_IMM, 0);

            /* byte = t_bits[attnum >> 3]; test bit (1 << (attnum & 7)) */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_TBITS);
            sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R0), attnum >> 3);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
                           SLJIT_R0, 0,
                           SLJIT_IMM, 1 << (attnum & 0x07));
            /* if bit set -> column is NOT null, skip to alignment */
            bit_is_set = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                                        SLJIT_R0, 0,
                                        SLJIT_IMM, 0);

            /* ---- Column IS NULL ---- */
            /* tts_values[attnum] = 0 */
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_S4),
                           attnum * (sljit_sw) sizeof(Datum),
                           SLJIT_IMM, 0);
            /* tts_isnull[attnum] = true */
            sljit_emit_op1(C, SLJIT_MOV_U8,
                           SLJIT_MEM1(SLJIT_S5), attnum,
                           SLJIT_IMM, 1);

            null_jumps[attnum] = sljit_emit_jump(C, SLJIT_JUMP);

            /* ---- NOT NULL path continues here ---- */
            {
                struct sljit_label *notnull_label = sljit_emit_label(C);
                sljit_set_label(no_hasnulls, notnull_label);
                sljit_set_label(bit_is_set, notnull_label);
            }

            attguaranteedalign = false;
        }

        /* ---- Alignment ---- */
        if (alignto > 1 &&
            (known_alignment < 0 ||
             known_alignment != TYPEALIGN(alignto, known_alignment)))
        {
            if (att->attlen == -1)
            {
                struct sljit_jump *is_short;

                attguaranteedalign = false;

                /* Peek first byte: if nonzero → short varlena, skip align */
                sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
                               SLJIT_MEM2(SLJIT_S3, SLJIT_R3), 0);
                is_short = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                                          SLJIT_R0, 0,
                                          SLJIT_IMM, 0);

                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
                               SLJIT_R3, 0, SLJIT_IMM, alignto - 1);
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R3, 0,
                               SLJIT_R3, 0, SLJIT_IMM, ~((sljit_sw)(alignto - 1)));

                sljit_set_label(is_short, sljit_emit_label(C));
            }
            else
            {
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
                               SLJIT_R3, 0, SLJIT_IMM, alignto - 1);
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R3, 0,
                               SLJIT_R3, 0, SLJIT_IMM, ~((sljit_sw)(alignto - 1)));
            }

            if (known_alignment >= 0)
                known_alignment = TYPEALIGN(alignto, known_alignment);
        }

        if (attguaranteedalign)
        {
            Assert(known_alignment >= 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_IMM, known_alignment);
        }

        /* ---- Value extraction ---- */
        /* R1 = tupdata_base + off (attdatap) */
        sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
                       SLJIT_S3, 0, SLJIT_R3, 0);

        /* tts_isnull[attnum] = false */
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S5), attnum,
                       SLJIT_IMM, 0);

        if (att->attbyval)
        {
            sljit_s32 mov_op;

            switch (att->attlen)
            {
                case 1: mov_op = SLJIT_MOV_S8; break;
                case 2: mov_op = SLJIT_MOV_S16; break;
                case 4: mov_op = SLJIT_MOV_S32; break;
                case 8: mov_op = SLJIT_MOV; break;
                default:
                    pfree(nvalid_jumps); pfree(avail_jumps);
                    pfree(null_jumps); pfree(att_labels);
                    return false;
            }
            /* R0 = *(mov_op *)(tupdata_base + off) */
            sljit_emit_op1(C, mov_op, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_R1), 0);
            /* tts_values[attnum] = R0 */
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_S4),
                           attnum * (sljit_sw) sizeof(Datum),
                           SLJIT_R0, 0);
        }
        else
        {
            /* tts_values[attnum] = pointer to data (R1 = attdatap) */
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_S4),
                           attnum * (sljit_sw) sizeof(Datum),
                           SLJIT_R1, 0);
        }

        /* ---- Compute alignment tracking for NEXT column ---- */
        if (att->attlen < 0)
        {
            known_alignment = -1;
            attguaranteedalign = false;
        }
        else if (JITTER_ATT_IS_NOTNULL(att) &&
                 attguaranteedalign && known_alignment >= 0)
        {
            Assert(att->attlen > 0);
            known_alignment += att->attlen;
        }
        else if (JITTER_ATT_IS_NOTNULL(att) &&
                 (att->attlen % alignto) == 0)
        {
            Assert(att->attlen > 0);
            known_alignment = alignto;
            attguaranteedalign = false;
        }
        else
        {
            known_alignment = -1;
            attguaranteedalign = false;
        }

        /* ---- Offset advance ---- */
        if (att->attlen > 0)
        {
            if (attguaranteedalign)
            {
                Assert(known_alignment >= 0);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                               SLJIT_IMM, known_alignment);
            }
            else
            {
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
                               SLJIT_R3, 0, SLJIT_IMM, att->attlen);
            }
        }
        else if (att->attlen == -1)
        {
            /*
             * Varlena: off += varsize_any(attdatap).
             * R1 still holds attdatap from value extraction.
             * icall clobbers R0-R3, so save R3 (deform_off) to stack
             * first. S3-S5 are callee-saved, so they survive the call.
             */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), vals_off,
                           SLJIT_R3, 0);  /* save R3 (vals_off is unused during deform) */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R1, 0);
            EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), varsize_any);
            /* R0 = varsize_any result. Restore R3 and add. */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_SP), vals_off);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
                           SLJIT_R3, 0, SLJIT_R0, 0);
        }
        else if (att->attlen == -2)
        {
            /*
             * Cstring: off += strlen(attdatap) + 1.
             * Save R3 around icall.
             */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), vals_off,
                           SLJIT_R3, 0);  /* save R3 (vals_off is unused during deform) */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R1, 0);
            EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), strlen);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_IMM, 1);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_SP), vals_off);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
                           SLJIT_R3, 0, SLJIT_R0, 0);
        }
    }

    /* ============================================================
     * EPILOGUE: patch jumps, store tts_nvalid, off, flags,
     * write vals/nulls to slot cache, restore S3/S4/S5.
     * ============================================================ */
    {
        struct sljit_label *deform_out = sljit_emit_label(C);

        /* Patch all forward jumps that target out */
        sljit_set_label(nvalid_default, deform_out);
        for (attnum = 0; attnum < natts; attnum++)
        {
            if (avail_jumps[attnum] != NULL)
                sljit_set_label(avail_jumps[attnum], deform_out);
        }
        /* Null-path jump for last attribute */
        if (null_jumps[natts - 1] != NULL)
            sljit_set_label(null_jumps[natts - 1], deform_out);

        /* Reload slot pointer */
        emit_load_econtext_slot(C, SLJIT_R0, fetch_opcode);

        /* tts_nvalid = natts (int16 store) */
        sljit_emit_op1(C, SLJIT_MOV_S16,
                       SLJIT_MEM1(SLJIT_R0),
                       offsetof(TupleTableSlot, tts_nvalid),
                       SLJIT_IMM, natts);

        /* slot->off = (uint32) deform_off (R3) */
        sljit_emit_op1(C, SLJIT_MOV_U32,
                       SLJIT_MEM1(SLJIT_R0), slot_off,
                       SLJIT_R3, 0);

        /* tts_flags |= TTS_FLAG_SLOW */
        sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_R0),
                       offsetof(TupleTableSlot, tts_flags));
        sljit_emit_op2(C, SLJIT_OR, SLJIT_R1, 0,
                       SLJIT_R1, 0, SLJIT_IMM, TTS_FLAG_SLOW);
        sljit_emit_op1(C, SLJIT_MOV_U16,
                       SLJIT_MEM1(SLJIT_R0),
                       offsetof(TupleTableSlot, tts_flags),
                       SLJIT_R1, 0);

        /*
         * Write S4/S5 (tts_values/tts_isnull) to the slot cache area
         * so post-deform VAR steps can find them.
         */
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_SP), vals_off,
                       SLJIT_S4, 0);
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_SP), vals_off + 8,
                       SLJIT_S5, 0);

        /* Restore S3, S4, S5 from saved slots */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_S3, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S3);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_S4, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S4);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_S5, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S5);
    }

    /* No return — we're inline in the expression function body */

    pfree(nvalid_jumps);
    pfree(avail_jumps);
    pfree(null_jumps);
    pfree(att_labels);

    return true;
}

/*
 * Helper: get stack offset pair (vals, nulls) for a given opcode's slot type.
 * Returns the SOFF_*_VALS offset; caller adds 8 for SOFF_*_NULLS.
 * Returns -1 for unknown opcodes.
 */
static sljit_sw
slot_cache_offset(ExprEvalOp opcode)
{
    switch (opcode)
    {
        case EEOP_INNER_FETCHSOME:
        case EEOP_INNER_VAR:
        case EEOP_ASSIGN_INNER_VAR:
            return SOFF_INNER_VALS;
        case EEOP_OUTER_FETCHSOME:
        case EEOP_OUTER_VAR:
        case EEOP_ASSIGN_OUTER_VAR:
            return SOFF_OUTER_VALS;
        case EEOP_SCAN_FETCHSOME:
        case EEOP_SCAN_VAR:
        case EEOP_ASSIGN_SCAN_VAR:
            return SOFF_SCAN_VALS;

#ifdef HAVE_EEOP_OLD_NEW
        case EEOP_OLD_FETCHSOME:
        case EEOP_OLD_VAR:
        case EEOP_ASSIGN_OLD_VAR:
            return SOFF_OLD_VALS;
        case EEOP_NEW_FETCHSOME:
        case EEOP_NEW_VAR:
        case EEOP_ASSIGN_NEW_VAR:
            return SOFF_NEW_VALS;
#endif
        default:
            return -1;
    }
}

/*
 * Helper: get the bitmask bit for a slot type.
 * Bit 0=inner, 1=outer, 2=scan, 3=old, 4=new.
 */
static uint32
slot_cache_bit(ExprEvalOp opcode)
{
    switch (opcode)
    {
        case EEOP_INNER_FETCHSOME:
        case EEOP_INNER_VAR:
        case EEOP_ASSIGN_INNER_VAR:
            return 1;
        case EEOP_OUTER_FETCHSOME:
        case EEOP_OUTER_VAR:
        case EEOP_ASSIGN_OUTER_VAR:
            return 2;
        case EEOP_SCAN_FETCHSOME:
        case EEOP_SCAN_VAR:
        case EEOP_ASSIGN_SCAN_VAR:
            return 4;
#ifdef HAVE_EEOP_OLD_NEW
        case EEOP_OLD_FETCHSOME:
        case EEOP_OLD_VAR:
        case EEOP_ASSIGN_OLD_VAR:
            return 8;
        case EEOP_NEW_FETCHSOME:
        case EEOP_NEW_VAR:
        case EEOP_ASSIGN_NEW_VAR:
            return 16;
#endif
        default:
            return 0;
    }
}

/*
 * S0-offset addressing helpers.
 *
 * Most expression steps have op->resvalue == &state->resvalue and
 * op->resnull == &state->resnull.  When true, we can use direct
 * [S0, #offset] addressing (1 ARM64 insn) instead of loading a
 * 64-bit immediate pointer (2-4 ARM64 insns) + indirect access.
 * This saves 180-360 ARM64 instructions per compiled expression.
 */

/* Load a per-expression pointer as an immediate (legacy, for non-PIC paths). */
#define EMIT_PTR(C, reg, value) \
    sljit_emit_op1(C, SLJIT_MOV, reg, 0, SLJIT_IMM, (sljit_sw)(value))

/*
 * Steps-relative addressing helpers.
 *
 * Load a per-expression pointer from state->steps[opno] at runtime,
 * making the compiled code position-independent (same bytes work in
 * any parallel worker).  Costs 2 ARM64 instructions (LDR+LDR) vs
 * 4 (MOVZ+3×MOVK) for EMIT_PTR.
 */

/* Load the value of a field from steps[opno].  E.g., for resvalue:
 *   emit_load_step_field(C, opno, offsetof(ExprEvalStep, resvalue), R1)
 * → R1 = steps[opno].resvalue (a Datum* pointer)
 */
static inline void
emit_load_step_field(struct sljit_compiler *C, int opno,
                     sljit_sw field_offset, int dst_reg)
{
    sljit_emit_op1(C, SLJIT_MOV, dst_reg, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_STEPS);
    sljit_emit_op1(C, SLJIT_MOV, dst_reg, 0,
                   SLJIT_MEM1(dst_reg),
                   opno * (sljit_sw)sizeof(ExprEvalStep) + field_offset);
}

/* Load the address of steps[opno] into dst_reg.  Used for fallback calls
 * where the C function needs a pointer to the step itself.
 *   emit_load_step_addr(C, opno, R1)
 * → R1 = &steps[opno]
 */
static inline void
emit_load_step_addr(struct sljit_compiler *C, int opno, int dst_reg)
{
    sljit_emit_op1(C, SLJIT_MOV, dst_reg, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_STEPS);
    if (opno != 0)
        sljit_emit_op2(C, SLJIT_ADD, dst_reg, 0, dst_reg, 0,
                       SLJIT_IMM, opno * (sljit_sw)sizeof(ExprEvalStep));
}

/* Load the address of a field within steps[opno] into dst_reg.
 *   emit_load_step_field_addr(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), R1)
 * → R1 = &steps[opno].d.boolexpr.anynull
 */
static inline void
emit_load_step_field_addr(struct sljit_compiler *C, int opno,
                          sljit_sw field_offset, int dst_reg)
{
    sljit_emit_op1(C, SLJIT_MOV, dst_reg, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_STEPS);
    sljit_emit_op2(C, SLJIT_ADD, dst_reg, 0, dst_reg, 0,
                   SLJIT_IMM,
                   opno * (sljit_sw)sizeof(ExprEvalStep) + field_offset);
}

/*
 * Paired resvalue/resnull store helpers.
 *
 * When op->resvalue and op->resnull point into the same NullableDatum
 * (i.e. resnull == resvalue + sizeof(Datum)), combined store helpers
 * (emit_store_res_pair_*) emit both stores with a single EMIT_PTR,
 * saving 4 ARM64 instructions per call.  For a 20-column SUM expression,
 * this eliminates ~150 instructions from the hot loop.
 *
 * Individual helpers remain for cases where only one is stored, or
 * where resvalue and resnull are stored at different control flow points.
 */

/* Is resnull adjacent to resvalue in the same NullableDatum? */
#define RESNULL_IS_PAIRED(op) \
    ((char *)(op)->resnull == (char *)(op)->resvalue + sizeof(Datum))

static inline void
emit_store_resvalue(struct sljit_compiler *C, ExprState *state,
                    int opno, ExprEvalStep *op, int src_reg)
{
    if (op->resvalue == &state->resvalue)
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resvalue),
                       src_reg, 0);
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), 0,
                       src_reg, 0);
    }
}

static inline void
emit_store_resnull_false(struct sljit_compiler *C, ExprState *state,
                         int opno, ExprEvalStep *op)
{
    if (op->resnull == &state->resnull)
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull),
                       SLJIT_IMM, 0);
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resnull), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_R1), 0,
                       SLJIT_IMM, 0);
    }
}

static inline void
emit_store_resnull_true(struct sljit_compiler *C, ExprState *state,
                        int opno, ExprEvalStep *op)
{
    if (op->resnull == &state->resnull)
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull),
                       SLJIT_IMM, 1);
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resnull), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_R1), 0,
                       SLJIT_IMM, 1);
    }
}

static inline void
emit_store_resnull_reg(struct sljit_compiler *C, ExprState *state,
                       int opno, ExprEvalStep *op, int src_reg)
{
    if (op->resnull == &state->resnull)
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull),
                       src_reg, 0);
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resnull), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_R1), 0,
                       src_reg, 0);
    }
}

static inline void
emit_store_resvalue_imm(struct sljit_compiler *C, ExprState *state,
                        int opno, ExprEvalStep *op, sljit_sw imm)
{
    if (op->resvalue == &state->resvalue)
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resvalue),
                       SLJIT_IMM, imm);
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), 0,
                       SLJIT_IMM, imm);
    }
}

/*
 * Combined store: resvalue + resnull = false, in one go.
 * When resvalue/resnull are in the same NullableDatum (resnull at
 * resvalue + 8), loads the resvalue pointer once via steps-relative
 * and stores both.  R1 is clobbered.
 */
static inline void
emit_store_res_pair_false(struct sljit_compiler *C, ExprState *state,
                          int opno, ExprEvalStep *op, int value_reg)
{
    if (op->resvalue == &state->resvalue)
    {
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resvalue),
                       value_reg, 0);
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull),
                       SLJIT_IMM, 0);
    }
    else if (RESNULL_IS_PAIRED(op))
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), 0,
                       value_reg, 0);
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_R1), (sljit_sw) sizeof(Datum),
                       SLJIT_IMM, 0);
    }
    else
    {
        emit_store_resvalue(C, state, opno, op, value_reg);
        emit_store_resnull_false(C, state, opno, op);
    }
}

/*
 * Combined store: resvalue (imm) + resnull = false.
 */
static inline void
emit_store_res_pair_false_imm(struct sljit_compiler *C, ExprState *state,
                              int opno, ExprEvalStep *op, sljit_sw imm)
{
    if (op->resvalue == &state->resvalue)
    {
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resvalue),
                       SLJIT_IMM, imm);
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull),
                       SLJIT_IMM, 0);
    }
    else if (RESNULL_IS_PAIRED(op))
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), 0,
                       SLJIT_IMM, imm);
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_R1), (sljit_sw) sizeof(Datum),
                       SLJIT_IMM, 0);
    }
    else
    {
        emit_store_resvalue_imm(C, state, opno, op, imm);
        emit_store_resnull_false(C, state, opno, op);
    }
}

/*
 * Combined store: resvalue (imm) + resnull = true.
 */
static inline void
emit_store_res_pair_true_imm(struct sljit_compiler *C, ExprState *state,
                             int opno, ExprEvalStep *op, sljit_sw imm)
{
    if (op->resvalue == &state->resvalue)
    {
        sljit_emit_op1(C, SLJIT_MOV,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resvalue),
                       SLJIT_IMM, imm);
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull),
                       SLJIT_IMM, 1);
    }
    else if (RESNULL_IS_PAIRED(op))
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), SLJIT_R1);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), 0,
                       SLJIT_IMM, imm);
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_R1), (sljit_sw) sizeof(Datum),
                       SLJIT_IMM, 1);
    }
    else
    {
        emit_store_resvalue_imm(C, state, opno, op, imm);
        emit_store_resnull_true(C, state, opno, op);
    }
}

static inline void
emit_load_resvalue(struct sljit_compiler *C, ExprState *state,
                   int opno, ExprEvalStep *op, int dst_reg)
{
    if (op->resvalue == &state->resvalue)
        sljit_emit_op1(C, SLJIT_MOV, dst_reg, 0,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resvalue));
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), dst_reg);
        sljit_emit_op1(C, SLJIT_MOV, dst_reg, 0,
                       SLJIT_MEM1(dst_reg), 0);
    }
}

static inline void
emit_load_resnull(struct sljit_compiler *C, ExprState *state,
                  int opno, ExprEvalStep *op, int dst_reg)
{
    if (op->resnull == &state->resnull)
        sljit_emit_op1(C, SLJIT_MOV_U8, dst_reg, 0,
                       SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resnull));
    else
    {
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resnull), dst_reg);
        sljit_emit_op1(C, SLJIT_MOV_U8, dst_reg, 0,
                       SLJIT_MEM1(dst_reg), 0);
    }
}

static inline void
emit_load_resvalue_addr(struct sljit_compiler *C, ExprState *state,
                        int opno, ExprEvalStep *op, int dst_reg)
{
    if (op->resvalue == &state->resvalue)
        sljit_emit_op2(C, SLJIT_ADD, dst_reg, 0,
                       SLJIT_S0, 0,
                       SLJIT_IMM, offsetof(ExprState, resvalue));
    else
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resvalue), dst_reg);
}

static inline void
emit_load_resnull_addr(struct sljit_compiler *C, ExprState *state,
                       int opno, ExprEvalStep *op, int dst_reg)
{
    if (op->resnull == &state->resnull)
        sljit_emit_op2(C, SLJIT_ADD, dst_reg, 0,
                       SLJIT_S0, 0,
                       SLJIT_IMM, offsetof(ExprState, resnull));
    else
        emit_load_step_field(C, opno,
                             offsetof(ExprEvalStep, resnull), dst_reg);
}

/*
 * emit_inline_funcexpr — emit inline sljit instructions for hot int ops.
 *
 * Args in R0, R1; result left in R0. Returns true if the op was handled.
 * Overflow/division-by-zero errors call cold-path helpers (never return).
 */
static bool
emit_inline_funcexpr(struct sljit_compiler *C, JitInlineOp op)
{
	struct sljit_jump *j_ok;

	switch (op)
	{
		/* ---- int32 arithmetic (overflow-checked, 32-bit ops) ---- */
		case JIT_INLINE_INT4_ADD:
			sljit_emit_op2(C, SLJIT_ADD32 | SLJIT_SET_OVERFLOW,
						   SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int4_overflow);
			sljit_set_label(j_ok, sljit_emit_label(C));
			/* Sign-extend 32-bit result to 64-bit Datum */
			sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
			return true;

		case JIT_INLINE_INT4_SUB:
			sljit_emit_op2(C, SLJIT_SUB32 | SLJIT_SET_OVERFLOW,
						   SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int4_overflow);
			sljit_set_label(j_ok, sljit_emit_label(C));
			sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
			return true;

		case JIT_INLINE_INT4_MUL:
			sljit_emit_op2(C, SLJIT_MUL32 | SLJIT_SET_OVERFLOW,
						   SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int4_overflow);
			sljit_set_label(j_ok, sljit_emit_label(C));
			sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
			return true;

		case JIT_INLINE_INT4_DIV:
		{
			struct sljit_jump *j_not_zero, *j_not_minmax, *j_done;

			/* Check divisor == 0 */
			j_not_zero = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R1, 0, SLJIT_IMM, 0);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_division_by_zero);
			sljit_set_label(j_not_zero, sljit_emit_label(C));

			/* Check INT32_MIN / -1 overflow */
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z,
							SLJIT_R0, 0, SLJIT_IMM, (sljit_s32) PG_INT32_MIN);
			j_not_minmax = sljit_emit_jump(C, SLJIT_NOT_EQUAL);
			{
				struct sljit_jump *j_not_neg1;
				j_not_neg1 = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R1, 0, SLJIT_IMM, -1);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int4_overflow);
				sljit_set_label(j_not_neg1, sljit_emit_label(C));
			}
			sljit_set_label(j_not_minmax, sljit_emit_label(C));

			sljit_emit_op0(C, SLJIT_DIV_S32);
			sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
			return true;
		}

		case JIT_INLINE_INT4_MOD:
		{
			struct sljit_jump *j_not_zero, *j_not_minmax, *j_zero_result;

			/* Check divisor == 0 */
			j_not_zero = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R1, 0, SLJIT_IMM, 0);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_division_by_zero);
			sljit_set_label(j_not_zero, sljit_emit_label(C));

			/* Check INT32_MIN % -1 → return 0 */
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z,
							SLJIT_R0, 0, SLJIT_IMM, (sljit_s32) PG_INT32_MIN);
			j_not_minmax = sljit_emit_jump(C, SLJIT_NOT_EQUAL);
			{
				j_zero_result = sljit_emit_cmp(C, SLJIT_EQUAL,
											   SLJIT_R1, 0, SLJIT_IMM, -1);
			}
			sljit_set_label(j_not_minmax, sljit_emit_label(C));

			sljit_emit_op0(C, SLJIT_DIVMOD_S32);
			/* Remainder is in R1 after DIVMOD */
			sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R1, 0);
			{
				struct sljit_jump *j_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* MIN % -1 = 0 */
				sljit_set_label(j_zero_result, sljit_emit_label(C));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0);

				sljit_set_label(j_done, sljit_emit_label(C));
			}
			return true;
		}

		/* ---- int64 arithmetic (overflow-checked, 64-bit ops) ---- */
		case JIT_INLINE_INT8_ADD:
			sljit_emit_op2(C, SLJIT_ADD | SLJIT_SET_OVERFLOW,
						   SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int8_overflow);
			sljit_set_label(j_ok, sljit_emit_label(C));
			return true;

		case JIT_INLINE_INT8_SUB:
			sljit_emit_op2(C, SLJIT_SUB | SLJIT_SET_OVERFLOW,
						   SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int8_overflow);
			sljit_set_label(j_ok, sljit_emit_label(C));
			return true;

		case JIT_INLINE_INT8_MUL:
			sljit_emit_op2(C, SLJIT_MUL | SLJIT_SET_OVERFLOW,
						   SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
			j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
			EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int8_overflow);
			sljit_set_label(j_ok, sljit_emit_label(C));
			return true;

		/* ---- int32 comparison ---- */
		case JIT_INLINE_INT4_EQ:
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_EQUAL);
			return true;

		case JIT_INLINE_INT4_NE:
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_NOT_EQUAL);
			return true;

		case JIT_INLINE_INT4_LT:
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_LESS);
			return true;

		case JIT_INLINE_INT4_LE:
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS_EQUAL,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_LESS_EQUAL);
			return true;

		case JIT_INLINE_INT4_GT:
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_GREATER,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_GREATER);
			return true;

		case JIT_INLINE_INT4_GE:
			sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_GREATER_EQUAL,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_GREATER_EQUAL);
			return true;

		/* ---- int64 comparison ---- */
		case JIT_INLINE_INT8_EQ:
			sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_EQUAL);
			return true;

		case JIT_INLINE_INT8_NE:
			sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_NOT_EQUAL);
			return true;

		case JIT_INLINE_INT8_LT:
			sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_SIG_LESS,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_LESS);
			return true;

		case JIT_INLINE_INT8_LE:
			sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_SIG_LESS_EQUAL,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_LESS_EQUAL);
			return true;

		case JIT_INLINE_INT8_GT:
			sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_SIG_GREATER,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_GREATER);
			return true;

		case JIT_INLINE_INT8_GE:
			sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_SIG_GREATER_EQUAL,
							SLJIT_R0, 0, SLJIT_R1, 0);
			sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_SIG_GREATER_EQUAL);
			return true;

		default:
			return false;
	}
}

/*
 * Pre-compiled inline blob support.
 *
 * When PG_JITTER_HAVE_PRECOMPILED is defined, we can emit clang-optimized
 * native code instead of hand-written sljit instruction sequences for
 * Tier 1 functions.
 *
 * The approach:
 * 1. Load args into R0, R1 from fcinfo (same as hand-written path)
 * 2. Copy pre-compiled instruction bytes via sljit_emit_op_custom()
 * 3. Patch the `ret` instruction to a forward branch (skip error path)
 * 4. After sljit_generate_code(), fix up BL/CALL relocations
 */
#if defined(PG_JITTER_HAVE_PRECOMPILED) || defined(PG_JITTER_HAVE_MIR_PRECOMPILED)
#define PG_JITTER_HAVE_INLINE_BLOBS
#endif

#ifdef PG_JITTER_HAVE_INLINE_BLOBS

/* Maximum pending relocations across all precompiled blobs in one expression */
#define MAX_PRECOMPILED_RELOCS 256

/*
 * Pending relocation: tracks a BL/CALL instruction in a precompiled blob
 * that needs post-generation patching. Uses sljit labels for reliable
 * address resolution instead of raw byte offsets.
 */
typedef struct PendingReloc {
	struct sljit_label *blob_label;	/* label at start of blob in code stream */
	uint16_t	offset_in_blob;		/* byte offset of BL/CALL within blob */
	uint8_t		type;				/* RELOC_* type */
	const char *symbol;				/* symbol name to resolve */
} PendingReloc;

/*
 * Map a symbol name to its runtime address.
 * All 7 BRANCH26 relocation targets across the 197 inlineable blobs:
 *   - 6 error handlers (never return, used by 70 functions)
 *   - hash_bytes_uint32 (used by 6 hash functions)
 */
static void *
resolve_precompiled_symbol(const char *symbol)
{
	/* Error handlers — 6 symbols, 70 functions */
	if (strcmp(symbol, "jit_error_int4_overflow") == 0)
		return (void *) jit_error_int4_overflow;
	if (strcmp(symbol, "jit_error_int8_overflow") == 0)
		return (void *) jit_error_int8_overflow;
	if (strcmp(symbol, "jit_error_division_by_zero") == 0)
		return (void *) jit_error_division_by_zero;
	if (strcmp(symbol, "jit_error_int2_overflow") == 0)
		return (void *) jit_error_int2_overflow;
	if (strcmp(symbol, "jit_error_float_overflow") == 0)
		return (void *) jit_error_float_overflow;
	if (strcmp(symbol, "jit_error_float_underflow") == 0)
		return (void *) jit_error_float_underflow;
	/* Utility — hash_bytes_uint32 from PG's common/hashfn.h */
	if (strcmp(symbol, "hash_bytes_uint32") == 0)
		return (void *) hash_bytes_uint32;
	/* Unknown symbol — can't resolve, blob won't be used */
	return NULL;
}

/*
 * Emit a pre-compiled inline blob into the sljit code stream.
 *
 * Copies the code bytes, patching the `ret` instruction to a forward branch
 * that skips the error-handler tail. BL/CALL relocations are recorded for
 * post-generation fixup using sljit labels for reliable address tracking.
 *
 * Args should already be in R0, R1. Result will be in R0 (per ABI).
 */
static bool
emit_precompiled_inline(struct sljit_compiler *C,
						const PrecompiledInline *pi,
						PendingReloc *relocs, int *nrelocs)
{
	uint8_t buf[512];
	struct sljit_label *blob_label;

	if (!pi || pi->code_len == 0 || pi->code_len > sizeof(buf))
		return false;

	memcpy(buf, pi->code, pi->code_len);

#if defined(__aarch64__) || defined(_M_ARM64)
	/*
	 * ARM64: Patch ret (0xd65f03c0) → unconditional branch past the
	 * error-handler tail. The error handler starts after ret and includes
	 * frame setup + BL to the error function.
	 *
	 * For jit_int4pl (24 bytes): ret is at offset 8, code_len=24
	 *   remaining = (24 - 8) / 4 = 4 instructions to skip
	 *   B +4 jumps 4*4=16 bytes forward from PC, landing at offset 24
	 *   (just past the blob). Correct.
	 */
	if (pi->ret_offset >= 0)
	{
		uint32_t remaining = (pi->code_len - pi->ret_offset) / 4;
		uint32_t *ret_instr = (uint32_t *)(buf + pi->ret_offset);
		/* B (unconditional branch): 0x14000000 | imm26 */
		*ret_instr = 0x14000000 | (remaining & 0x3FFFFFF);
	}
#elif defined(__x86_64__) || defined(_M_X64)
	/*
	 * x86_64: Patch ret (0xC3) → JMP forward past the error-handler
	 * tail. x86_64 JMP near: 0xE9 + 32-bit displacement.
	 * The displacement is relative to the end of the 5-byte JMP instruction.
	 */
	if (pi->ret_offset >= 0)
	{
		int remaining = pi->code_len - pi->ret_offset - 5;
		buf[pi->ret_offset] = 0xE9;  /* JMP near */
		int32_t disp = remaining;
		memcpy(buf + pi->ret_offset + 1, &disp, 4);
	}
#else
	return false;
#endif

	/*
	 * Place a label right before the blob so we can find its final
	 * address after sljit_generate_code(). This is the key to
	 * reliable relocation: sljit resolves label addresses during
	 * code generation, so we get the exact executable address.
	 */
	blob_label = sljit_emit_label(C);

	/* Emit the raw instruction bytes */
#if defined(__aarch64__) || defined(_M_ARM64)
	for (int off = 0; off < pi->code_len; off += 4)
	{
		sljit_emit_op_custom(C, buf + off, 4);
	}
#elif defined(__x86_64__) || defined(_M_X64)
	/*
	 * x86_64: Variable-length instructions. Emit individual bytes.
	 * sljit_emit_op_custom() on x86 accepts 1..16 byte instructions.
	 * We emit the entire blob as a sequence of single-byte emissions.
	 */
	for (int off = 0; off < pi->code_len; off++)
	{
		sljit_emit_op_custom(C, buf + off, 1);
	}
#endif

	/* Record BL/CALL relocations for post-generation fixup */
	for (int i = 0; i < pi->n_relocs && *nrelocs < MAX_PRECOMPILED_RELOCS; i++)
	{
		relocs[*nrelocs].blob_label = blob_label;
		relocs[*nrelocs].offset_in_blob = pi->relocs[i].offset;
		relocs[*nrelocs].type = pi->relocs[i].type;
		relocs[*nrelocs].symbol = pi->relocs[i].symbol;
		(*nrelocs)++;
	}

	return true;
}

/*
 * After sljit_generate_code(), patch all pending BL/CALL relocations
 * in pre-compiled blobs to point to the actual runtime addresses.
 *
 * Uses sljit label addresses for exact blob positioning. Handles W^X
 * by toggling write protection on macOS ARM64 (MAP_JIT memory).
 */
static void
fixup_precompiled_relocs(void *code, sljit_uw code_size,
						 PendingReloc *relocs, int nrelocs)
{
	if (nrelocs == 0)
		return;

#if defined(__APPLE__) && defined(__aarch64__)
	/* Toggle JIT memory to writable mode (per-thread on Apple Silicon) */
	pthread_jit_write_protect_np(0);
#endif

	for (int i = 0; i < nrelocs; i++)
	{
		void *target = resolve_precompiled_symbol(relocs[i].symbol);
		if (!target)
			continue;

		/* Get the blob's final address from the sljit label */
		sljit_uw blob_addr = sljit_get_label_addr(relocs[i].blob_label);

#if defined(__aarch64__) || defined(_M_ARM64)
		if (relocs[i].type == RELOC_BRANCH26)
		{
			uint32_t *instr = (uint32_t *)(blob_addr + relocs[i].offset_in_blob);
			sljit_sw pc_rel = ((sljit_sw)target - (sljit_sw)instr) >> 2;
			*instr = (*instr & ~0x3FFFFFF) | ((uint32_t)pc_rel & 0x3FFFFFF);
		}
		else if (relocs[i].type == RELOC_MOVZ_MOVK64)
		{
			/*
			 * Patch MOVZ+3×MOVK sequence (4 instructions, 16 bytes).
			 * Each instruction has a 16-bit immediate in bits [20:5].
			 */
			uint32_t *insn = (uint32_t *)(blob_addr + relocs[i].offset_in_blob);
			uintptr_t addr = (uintptr_t)target;
			insn[0] = (insn[0] & ~(0xFFFFU << 5))
					| (((uint32_t)(addr & 0xFFFF)) << 5);
			insn[1] = (insn[1] & ~(0xFFFFU << 5))
					| (((uint32_t)((addr >> 16) & 0xFFFF)) << 5);
			insn[2] = (insn[2] & ~(0xFFFFU << 5))
					| (((uint32_t)((addr >> 32) & 0xFFFF)) << 5);
			insn[3] = (insn[3] & ~(0xFFFFU << 5))
					| (((uint32_t)((addr >> 48) & 0xFFFF)) << 5);
		}
#elif defined(__x86_64__) || defined(_M_X64)
		if (relocs[i].type == RELOC_PC32)
		{
			uint8_t *instr_addr = (uint8_t *)blob_addr + relocs[i].offset_in_blob;
			/* x86 CALL E8: displacement is from end of 5-byte instruction */
			int32_t *disp = (int32_t *)(instr_addr + 1);
			*disp = (int32_t)((sljit_sw)target - (sljit_sw)(instr_addr + 5));
		}
		else if (relocs[i].type == RELOC_ABS64)
		{
			/* Patch 8-byte absolute address in const pool */
			uintptr_t addr = (uintptr_t)target;
			memcpy((uint8_t *)blob_addr + relocs[i].offset_in_blob, &addr, 8);
		}
#endif
	}

#if defined(__APPLE__) && defined(__aarch64__)
	/* Toggle back to executable mode */
	pthread_jit_write_protect_np(1);
	/* Flush instruction cache for the entire code region */
	sys_icache_invalidate((void *)code, (size_t)code_size);
#elif defined(__aarch64__)
	/* Non-Apple ARM64: use GCC builtin */
	__builtin___clear_cache((char *)code,
							(char *)code + code_size);
#endif
}

#endif /* PG_JITTER_HAVE_INLINE_BLOBS */

static bool
sljit_compile_expr(ExprState *state)
{
	PgJitterContext *ctx;
	struct sljit_compiler *C;
	ExprEvalStep   *steps;
	int				steps_len;
	int				opno;
	ExprEvalOp		opcode;

	/* Deform function cache — shared across all FETCHSOME steps */
	DeformCacheEntry deform_cache[MAX_DEFORM_CACHE];
	int				ndeform_cache = 0;

	/*
	 * Bitmask tracking which slot types have had FETCHSOME emitted
	 * (and thus have cached tts_values/tts_isnull on the stack).
	 * Bit 0=inner, 1=outer, 2=scan, 3=old, 4=new.
	 */
	uint32			slots_cached = 0;

	struct sljit_label **step_labels;
	instr_time		starttime, endtime;

	/* Pending jumps for fixup after all code is emitted */
	struct {
		struct sljit_jump *jump;
		int				target;
	}			   *pending_jumps;
	int				npending = 0;

#ifdef PG_JITTER_HAVE_INLINE_BLOBS
	/* Pending relocations for pre-compiled inline blobs */
	PendingReloc	precompiled_relocs[MAX_PRECOMPILED_RELOCS];
	int				n_precompiled_relocs = 0;
#endif

	/* Expression identity for shared code in parallel queries */
	int				shared_node_id = 0;
	int				shared_expr_idx = 0;

	/* For COMPARE mode: saved DSM code for post-compilation comparison */

	/*
	 * When true, EMIT_ICALL forces SLJIT_REWRITABLE_JUMP to prevent
	 * sljit from converting calls to PC-relative BL instructions.
	 * PC-relative branches break when code is memcpy'd to a different
	 * address in a parallel worker.
	 */
	/* Must have a parent PlanState */
	if (!state->parent)
		return false;

	/* Let PG's hand-optimized fast-path evalfuncs handle tiny expressions */
	if (expr_has_fast_path(state))
		return false;

	/* Skip expression JIT in parallel workers if mode is 'off' */
	if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_OFF && IsParallelWorker())
		return false;

	/* JIT is active */

#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
	/* Lazy-load MIR precompiled blobs on first compile */
	mir_load_precompiled_blobs(NULL);
#endif

	ctx = pg_jitter_get_context(state);

	/*
	 * Shared code for parallel queries: compute expression identity.
	 *
	 * Leader: creates DSM on first compile, writes code directly.
	 * Worker: attaches to DSM via GUC, looks up pre-compiled code.
	 */
	if (state->parent->state->es_jit_flags & PGJIT_EXPR)
	{
		pg_jitter_get_expr_identity(ctx, state,
									&shared_node_id, &shared_expr_idx);

		sljit_shared_code_mode = (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED)
								&& state->parent->state->es_plannedstmt->parallelModeNeeded;

		elog(DEBUG1, "pg_jitter: compile_expr node=%d expr=%d is_worker=%d "
			 "shared_mode=%d share_init=%d",
			 shared_node_id, shared_expr_idx, IsParallelWorker(),
			 sljit_shared_code_mode,
			 ctx->share_state.initialized);

		/* Leader: create DSM on first compile */
		if (sljit_shared_code_mode && !IsParallelWorker() &&
			!ctx->share_state.initialized)
			pg_jitter_init_shared_dsm(ctx);
	}

	/*
	 * Parallel worker: try to use pre-compiled code from the leader.
	 * If found in DSM, copy to local executable memory and skip compilation.
	 */
	if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED &&
		IsParallelWorker())
	{
		const void *code_bytes;
		Size		code_size;
		uint64		leader_dylib_ref;

		if (!ctx->share_state.initialized)
			pg_jitter_attach_shared_dsm(ctx);

		/* Try to attach shared deform (same VA as leader for L1I sharing) */
		if (ctx->share_state.sjc)
			pg_jitter_attach_shared_deform(ctx->share_state.sjc);

		if (ctx->share_state.sjc &&
			pg_jitter_find_shared_code(ctx->share_state.sjc,
									   shared_node_id, shared_expr_idx,
									   &code_bytes, &code_size,
									   &leader_dylib_ref))
		{
			void   *handle;
			void   *code_ptr;

			handle = pg_jitter_copy_to_executable(code_bytes, code_size);
			if (handle)
			{
				/* Relocate dylib addresses (ASLR differs between processes) */
				uint64	worker_ref = (uint64)(uintptr_t) pg_jitter_fallback_step;
				int		npatched;

				npatched = pg_jitter_relocate_dylib_addrs(handle, code_size,
														  leader_dylib_ref,
														  worker_ref);

				code_ptr = pg_jitter_exec_code_ptr(handle);

				elog(DEBUG1, "pg_jitter: worker reused shared code "
					 "node=%d expr=%d (%zu bytes, patched=%d)",
					 shared_node_id, shared_expr_idx, code_size,
					 npatched);

				pg_jitter_register_compiled(ctx, pg_jitter_exec_free, handle);
				pg_jitter_install_expr(state,
									  (ExprStateEvalFunc) code_ptr);

				sljit_shared_code_mode = false;

				ctx->base.instr.created_functions++;
				return true;
			}

			elog(WARNING, "pg_jitter: failed to allocate executable memory "
				 "for shared code node=%d expr=%d, compiling locally",
				 shared_node_id, shared_expr_idx);
			/* Fall through to normal compilation */
		}
		else
			elog(DEBUG1, "pg_jitter: worker did not find shared code "
				 "node=%d expr=%d, compiling locally",
				 shared_node_id, shared_expr_idx);
		/* Fall through to normal compilation */
	}

	INSTR_TIME_SET_CURRENT(starttime);

	steps = state->steps;
	steps_len = state->steps_len;

	/*
	 * Pre-scan: check if expression has aggregate transition steps.
	 * Aggregates need S5 for aggstate.
	 * HASHDATUM_NEXT32 uses S5 for the rotated hash when no agg.
	 * Deform is compiled as separate functions (no S3-S5 conflict).
	 *
	 * Register layout (see SREG_RESULTVALS/SREG_RESULTNULLS defines):
	 *   S0-S2: state, econtext, isNull (always)
	 *   S3-S4: resultvals, resultnulls (always, loaded in prologue)
	 *   S5:    aggstate (when has_agg) OR sreg_hash (when has_hash_next)
	 * Max 6 saved regs — SLJIT_NUMBER_OF_SAVED_REGISTERS >= 6 on all archs.
	 */
	int sreg_hash = 0;		/* saved register for rotated hash in NEXT32 */
	bool use_sreg_hash = false;	/* true if sreg_hash is a register, not stack */
	{
		bool has_agg = false;
		bool has_hash_next = false;
		int nsaved;

		for (int i = 0; i < steps_len; i++)
		{
			ExprEvalOp op = ExecEvalStepOp(state, &steps[i]);

			if (op >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
				op <= EEOP_AGG_PLAIN_TRANS_BYREF)
				has_agg = true;

#ifdef HAVE_EEOP_HASHDATUM
			if (op == EEOP_HASHDATUM_NEXT32 ||
				op == EEOP_HASHDATUM_NEXT32_STRICT)
				has_hash_next = true;
#endif
		}

		/*
		 * Compute saved register count.
		 * Always use 6 saved registers (S0-S5):
		 *   S0=state, S1=econtext, S2=isNull,
		 *   S3=resultvals, S4=resultnulls (always)
		 *   S5=aggstate (agg) / sreg_hash (hash_next) / temp (deform)
		 *
		 * Inline deform temporarily repurposes S3-S5 for tupdata_base,
		 * tts_values, tts_isnull (saving/restoring original values).
		 * This requires S5 to be allocated even when there's no agg
		 * or hash_next.
		 *
		 * Max 6 saved registers — the guaranteed minimum across all
		 * sljit architectures (x86-64 non-Windows has exactly 6).
		 * When agg+hash coexist, hash falls back to SOFF_TEMP.
		 */
		nsaved = 6;
		if (has_hash_next && !has_agg)
		{
			sreg_hash = SLJIT_S5;
			use_sreg_hash = true;
		}

		C = sljit_create_compiler(NULL);
		if (!C)
			return false;

		step_labels = palloc0(sizeof(struct sljit_label *) * steps_len);
		pending_jumps = palloc(sizeof(*pending_jumps) * steps_len * 4);

		/*
		 * Function prologue.
		 * Saved regs: S0=state, S1=econtext, S2=isNull,
		 *             S3=resultvals, S4=resultnulls
		 * For aggregate expressions: S5=aggstate
		 * For HASHDATUM_NEXT32 (no agg): S5=rotated hash
		 * 4 scratch regs (R0-R3).
		 * EMIT_ICALL uses SOFF_TEMP stack slot + SLJIT_MEM1(SP) for
		 * position-independent calls (no extra scratch register needed).
		 */
		sljit_emit_enter(C, 0,
						 SLJIT_ARGS3(W, P, P, P),
						 4, nsaved, SOFF_TOTAL);

		if (has_agg)
		{
			/* S5 = state->parent (aggstate) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_S5, 0,
						   SLJIT_MEM1(SLJIT_S0),
						   offsetof(ExprState, parent));
			/* &CurrentMemoryContext → stack (only used twice per AGG_TRANS) */
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_CURRMCTXP,
						   SLJIT_IMM,
						   (sljit_sw) &CurrentMemoryContext);
		}
	}

	/* Cache state->steps pointer on the stack for steps-relative addressing.
	 * This enables PIC code: per-expression pointers are loaded via
	 * steps[stepno].field instead of 64-bit immediates (2 insn vs 4). */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, steps));
	sljit_emit_op1(C, SLJIT_MOV,
				   SLJIT_MEM1(SLJIT_SP), SOFF_STEPS,
				   SLJIT_R0, 0);

	/* Load resultslot values/nulls into saved registers S3/S4 */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0), offsetof(ExprState, resultslot));
	sljit_emit_op1(C, SLJIT_MOV,
				   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTSLOT,
				   SLJIT_R0, 0);

	{
		struct sljit_jump *skip_rs;

		/* If resultslot == NULL, skip dereferencing it */
		skip_rs = sljit_emit_cmp(C, SLJIT_EQUAL,
								 SLJIT_R0, 0, SLJIT_IMM, 0);

		/* S3 = resultslot->tts_values */
		sljit_emit_op1(C, SLJIT_MOV, SREG_RESULTVALS, 0,
					   SLJIT_MEM1(SLJIT_R0), offsetof(TupleTableSlot, tts_values));

		/* S4 = resultslot->tts_isnull */
		sljit_emit_op1(C, SLJIT_MOV, SREG_RESULTNULLS, 0,
					   SLJIT_MEM1(SLJIT_R0), offsetof(TupleTableSlot, tts_isnull));

		sljit_set_label(skip_rs, sljit_emit_label(C));
	}

	/*
	 * Main loop: emit code for each step.
	 */
	for (opno = 0; opno < steps_len; opno++)
	{
		ExprEvalStep   *op = &steps[opno];

		step_labels[opno] = sljit_emit_label(C);
		opcode = ExecEvalStepOp(state, op);

		/*
		 * For each opcode, emit either: (a) inline native code for hot-path
		 * opcodes, or (b) a call to pg_jitter_fallback_step for everything
		 * else. The fallback writes to op->resvalue/resnull, and the JIT
		 * code handles any jumps after the fallback returns.
		 *
		 * For opcodes that read/write resvalue/resnull, we use the per-step
		 * op->resvalue and op->resnull pointers rather than maintaining our
		 * own — this matches how the PG interpreter works.
		 */
		switch (opcode)
		{
			/*
			 * ---- DONE ----
			 */
			case EEOP_DONE_RETURN:
			{
				/* Load state->resvalue → R0 (return value) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, resvalue));
				/* Load state->resnull → R1 */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, resnull));
				/* *isNull = state->resnull */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_S2), 0,
							   SLJIT_R1, 0);
				/* return state->resvalue */
				sljit_emit_return(C, SLJIT_MOV, SLJIT_R0, 0);
				break;
			}

#ifdef HAVE_EEOP_DONE_SPLIT
			case EEOP_DONE_NO_RETURN:
			{
				sljit_emit_return(C, SLJIT_MOV, SLJIT_IMM, 0);
				break;
			}
#endif

			/* All hot-path opcodes below get native code */

			/*
			 * ---- FETCHSOME ----
			 * Fast path: if slot->tts_nvalid >= last_var, skip.
			 * Slow path: call slot_getsomeattrs_int(slot, last_var).
			 */
			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_FETCHSOME:
			case EEOP_NEW_FETCHSOME:
#endif
			{
				struct sljit_jump *skip_j;
				bool deform_emitted = false;
				sljit_sw vals_off = slot_cache_offset(opcode);

				/* R0 = slot pointer from econtext */
				emit_load_econtext_slot(C, SLJIT_R0, opcode);

				/* R1 = slot->tts_nvalid (AttrNumber = int16) */
				sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(TupleTableSlot, tts_nvalid));

				/* if (tts_nvalid >= last_var) skip */
				skip_j = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
										SLJIT_R1, 0,
										SLJIT_IMM, op->d.fetch.last_var);

				/* Try compiled deform function if conditions allow */
				if (op->d.fetch.fixed && op->d.fetch.known_desc &&
					(ctx->base.flags & PGJIT_DEFORM))
				{
					/*
					 * Try pre-compiled template first (I-cache friendly:
					 * same virtual address across all parallel workers).
					 */
					deform_template_fn tmpl = deform_match_template(
						op->d.fetch.known_desc,
						op->d.fetch.kind,
						op->d.fetch.last_var);

					if (tmpl)
					{
						/* R0 still has slot pointer; call template(slot) */
						EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1V(P), tmpl);
						deform_emitted = true;
					}
					else
					{
						/*
						 * Try inline deform (zero call overhead, contiguous
						 * I-cache). Emits code directly into the expression
						 * function body.
						 */
						instr_time  deform_start, deform_end;

						INSTR_TIME_SET_CURRENT(deform_start);
						deform_emitted = sljit_emit_deform_inline(
							C,
							op->d.fetch.known_desc,
							op->d.fetch.kind,
							op->d.fetch.last_var,
							opcode,
							vals_off,
							sljit_shared_code_mode);
						INSTR_TIME_SET_CURRENT(deform_end);
						JITTER_INSTR_DEFORM_ACCUM(ctx->base.instr,
						                      deform_end, deform_start);

						if (!deform_emitted && !sljit_shared_code_mode)
						{
							/*
							 * Fall back to compiled deform (separate
							 * function, called via BL).
							 *
							 * Skip when sharing code: the compiled
							 * deform address is per-process sljit memory
							 * (not PIC). Fall through to
							 * slot_getsomeattrs_int instead.
							 */
							emit_load_econtext_slot(C, SLJIT_R0, opcode);
							void *deform_fn = find_or_compile_deform(
								ctx, deform_cache, &ndeform_cache,
								op->d.fetch.known_desc,
								op->d.fetch.kind,
								op->d.fetch.last_var);

							if (deform_fn)
							{
								EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1V(P), deform_fn);
								deform_emitted = true;
							}
						}
					}
				}

				if (!deform_emitted && sljit_shared_code_mode)
				{
					/*
					 * Leader: try to compile shared deform for wide tables.
					 * Places code+descriptor at a fixed VA that workers will
					 * mmap at the same address for L1I cache sharing.
					 */
					if (!IsParallelWorker() &&
						op->d.fetch.known_desc &&
						op->d.fetch.last_var > pg_jitter_deform_threshold() &&
						ctx->share_state.sjc)
					{
						pg_jitter_compile_shared_deform(
							ctx->share_state.sjc,
							op->d.fetch.known_desc,
							op->d.fetch.kind,
							op->d.fetch.last_var);
					}

					/*
					 * Shared mode: call the dylib-resident dispatch function
					 * which JIT-compiles the deform per-process and caches it.
					 * Being a dylib function, its address is properly relocated.
					 * The dispatch function checks shared_deform_fn first.
					 */
					emit_load_econtext_slot(C, SLJIT_R0, opcode);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, op->d.fetch.last_var);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS2V(P, 32),
							   pg_jitter_compiled_deform_dispatch);
					deform_emitted = true;
				}

				if (!deform_emitted)
				{
					/* Fallback: call slot_getsomeattrs_int(slot, last_var) */
					emit_load_econtext_slot(C, SLJIT_R0, opcode);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, op->d.fetch.last_var);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS2V(P, 32), slot_getsomeattrs_int);
				}

				/* Skip label (both skip and deform paths converge here) */
				sljit_set_label(skip_j, sljit_emit_label(C));

				/*
				 * Cache the slot's tts_values and tts_isnull pointers
				 * on the stack for subsequent VAR/ASSIGN_VAR opcodes.
				 * Must be after skip label so cache is set for both paths.
				 */
				if (vals_off >= 0)
				{
					emit_load_econtext_slot(C, SLJIT_R0, opcode);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(TupleTableSlot, tts_values));
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_SP), vals_off,
								   SLJIT_R1, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(TupleTableSlot, tts_isnull));
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_SP), vals_off + 8,
								   SLJIT_R1, 0);
					slots_cached |= slot_cache_bit(opcode);
				}
				break;
			}

			/*
			 * ---- VAR ----
			 * Load slot->tts_values[attnum] → *op->resvalue
			 * Load slot->tts_isnull[attnum] → *op->resnull
			 *
			 * Optimization: when multiple consecutive VARs read from
			 * the same slot, batch them to load tts_values/tts_isnull
			 * once instead of per-VAR.  This saves 2*(N-1) memory
			 * loads for N consecutive same-slot VARs.
			 */
			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_VAR:
			case EEOP_NEW_VAR:
#endif
			{
				sljit_sw	vals_off = slot_cache_offset(opcode);
				bool		use_cache = (slots_cached & slot_cache_bit(opcode)) != 0;

				/*
				 * Look ahead: count consecutive VARs from the same slot.
				 * We batch them to avoid reloading tts_values/tts_isnull.
				 */
				int			batch_end = opno;
				while (batch_end + 1 < steps_len)
				{
					ExprEvalStep *next_op = &steps[batch_end + 1];
					ExprEvalOp    next_opc = ExecEvalStepOp(state, next_op);
					if (next_opc != opcode)
						break;
					batch_end++;
				}
				int batch_count = batch_end - opno + 1;

				if (batch_count == 1 && RESNULL_IS_PAIRED(op) &&
					op->resvalue != &state->resvalue)
				{
					/*
					 * Single VAR with paired resvalue/resnull: load value and
					 * isnull, store both using a single EMIT_PTR base.
					 * Saves 4 ARM64 instructions vs separate EMIT_PTRs.
					 */
					int attnum = op->d.var.attnum;

					/* Load value from tts_values[attnum] → R2 */
					if (use_cache)
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_SP), vals_off);
					else
					{
						emit_load_econtext_slot(C, SLJIT_R0, opcode);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R0),
									   offsetof(TupleTableSlot, tts_values));
					}
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   attnum * (sljit_sw) sizeof(Datum));

					/* R1 = resvalue ptr (via steps-relative, shared for both stores) */
					emit_load_step_field(C, opno,
										 offsetof(ExprEvalStep, resvalue), SLJIT_R1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_R2, 0);

					/* Load isnull from tts_isnull[attnum] → R2 */
					if (use_cache)
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_SP), vals_off + 8);
					else
					{
						emit_load_econtext_slot(C, SLJIT_R0, opcode);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R0),
									   offsetof(TupleTableSlot, tts_isnull));
					}
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   attnum * (sljit_sw) sizeof(bool));

					/* Store isnull via paired offset from R1 */
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1), (sljit_sw) sizeof(Datum),
								   SLJIT_R2, 0);
				}
				else
				{
					/* Multi-VAR batch or S0-relative: use phased approach */

					/* Phase 1: load all values from tts_values */
					if (use_cache)
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_SP), vals_off);
					else
					{
						emit_load_econtext_slot(C, SLJIT_R0, opcode);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R0),
									   offsetof(TupleTableSlot, tts_values));
					}
					/* R0 = tts_values, stays live (emit_store uses R1) */
					for (int bi = 0; bi < batch_count; bi++)
					{
						ExprEvalStep *cur = &steps[opno + bi];
						int attnum = cur->d.var.attnum;
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_R0),
									   attnum * (sljit_sw) sizeof(Datum));
						emit_store_resvalue(C, state, opno + bi, cur, SLJIT_R2);
					}

					/* Phase 2: load all isnulls from tts_isnull */
					if (use_cache)
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_SP), vals_off + 8);
					else
					{
						emit_load_econtext_slot(C, SLJIT_R0, opcode);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R0),
									   offsetof(TupleTableSlot, tts_isnull));
					}
					for (int bi = 0; bi < batch_count; bi++)
					{
						ExprEvalStep *cur = &steps[opno + bi];
						int attnum = cur->d.var.attnum;
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_R0),
									   attnum * (sljit_sw) sizeof(bool));
						emit_store_resnull_reg(C, state, opno + bi, cur, SLJIT_R2);
					}
				}

				/* Emit labels for skipped steps so jump targets work */
				for (int bi = 1; bi < batch_count; bi++)
					step_labels[opno + bi] = sljit_emit_label(C);
				opno = batch_end;  /* advance past batch */
				break;
			}

			/*
			 * ---- ASSIGN_*_VAR ----
			 * Load source slot's values[attnum] → resultslot->tts_values[resultnum]
			 * Load source slot's isnull[attnum] → resultslot->tts_isnull[resultnum]
			 *
			 * Batched like VAR above for consecutive same-slot assignments.
			 */
			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_ASSIGN_OLD_VAR:
			case EEOP_ASSIGN_NEW_VAR:
#endif
			{
				sljit_sw	vals_off = slot_cache_offset(opcode);
				bool		use_cache = (slots_cached & slot_cache_bit(opcode)) != 0;

				/* Look ahead for consecutive same-slot ASSIGN_VARs */
				int			batch_end = opno;
				while (batch_end + 1 < steps_len)
				{
					ExprEvalStep *next_op = &steps[batch_end + 1];
					ExprEvalOp    next_opc = ExecEvalStepOp(state, next_op);
					if (next_opc != opcode)
						break;
					batch_end++;
				}
				int batch_count = batch_end - opno + 1;

				/* Phase 1: load all values from source tts_values */
				if (use_cache)
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP), vals_off);
				else
				{
					emit_load_econtext_slot(C, SLJIT_R0, opcode);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(TupleTableSlot, tts_values));
				}
				/* R0 = tts_values, stays live (STR to S3/S4 doesn't touch R0) */
				for (int bi = 0; bi < batch_count; bi++)
				{
					ExprEvalStep *cur = &steps[opno + bi];
					int attnum = cur->d.assign_var.attnum;
					int resultnum = cur->d.assign_var.resultnum;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   attnum * (sljit_sw) sizeof(Datum));
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SREG_RESULTVALS),
								   resultnum * (sljit_sw) sizeof(Datum),
								   SLJIT_R2, 0);
				}

				/* Phase 2: load all isnulls from source tts_isnull */
				if (use_cache)
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP), vals_off + 8);
				else
				{
					emit_load_econtext_slot(C, SLJIT_R0, opcode);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(TupleTableSlot, tts_isnull));
				}
				for (int bi = 0; bi < batch_count; bi++)
				{
					ExprEvalStep *cur = &steps[opno + bi];
					int attnum = cur->d.assign_var.attnum;
					int resultnum = cur->d.assign_var.resultnum;
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   attnum * (sljit_sw) sizeof(bool));
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SREG_RESULTNULLS),
								   resultnum * (sljit_sw) sizeof(bool),
								   SLJIT_R2, 0);
				}

				/* Emit labels for skipped steps so jump targets work */
				for (int bi = 1; bi < batch_count; bi++)
					step_labels[opno + bi] = sljit_emit_label(C);
				opno = batch_end;  /* advance past batch */
				break;
			}

			/*
			 * ---- ASSIGN_TMP / ASSIGN_TMP_MAKE_RO ----
			 * Copy state->resvalue/resnull → resultslot columns.
			 */
			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
			{
				int			resultnum = op->d.assign_tmp.resultnum;

				/* R0 = state->resvalue */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, resvalue));
				/* R1 = state->resnull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, resnull));

				if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				{
					struct sljit_jump *skip_ro;

					/* if resnull, skip MakeReadOnly */
					skip_ro = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											 SLJIT_R1, 0,
											 SLJIT_IMM, 0);

					/* R0 = MakeExpandedObjectReadOnlyInternal(R0) */
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, W), MakeExpandedObjectReadOnlyInternal);
					/* Re-load R1 = 0 (not null, since we skipped for null) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, 0);

					sljit_set_label(skip_ro, sljit_emit_label(C));
				}

				/* Store to resultslot->tts_isnull[resultnum] (S4 = resultnulls) */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SREG_RESULTNULLS),
							   resultnum * (sljit_sw) sizeof(bool),
							   SLJIT_R1, 0);

				/* Store to resultslot->tts_values[resultnum] (S3 = resultvals) */
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SREG_RESULTVALS),
							   resultnum * (sljit_sw) sizeof(Datum),
							   SLJIT_R0, 0);
				break;
			}

			/*
			 * ---- CONST ----
			 */
			case EEOP_CONST:
			{
				/* *op->resvalue = constval.value */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) op->d.constval.value);
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* *op->resnull = constval.isnull */
				if (op->d.constval.isnull)
					emit_store_resnull_true(C, state, opno, op);
				else
					emit_store_resnull_false(C, state, opno, op);
				break;
			}

			/*
			 * ---- FUNCEXPR / FUNCEXPR_STRICT / STRICT_1 / STRICT_2 ----
			 * V1 calling convention: result = fn_addr(fcinfo_data)
			 */
			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
#endif
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int			nargs = op->d.func.nargs;
				struct sljit_jump *skip_null = NULL;
				bool		r1_has_fcinfo = false;
				int			null_check_start = npending;

				if (opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
					|| opcode == EEOP_FUNCEXPR_STRICT_1
					|| opcode == EEOP_FUNCEXPR_STRICT_2
#endif
					)
				{
					/*
					 * Check args for NULL. If any arg is null, jump to
					 * null_path (sets resnull=true).  We defer the resnull
					 * write to the null path only — the hot (non-null) path
					 * avoids the store.
					 *
					 * Load fcinfo into R1 once; it survives all null checks
					 * and is reused for arg value loads in the inline/direct
					 * paths below.
					 *
					 * For nargs <= 4, OR-batch the isnull flags into R0
					 * and emit a single branch.  Saves (nargs-1) branches.
					 */
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R1);

					if (nargs <= 4 && nargs > 1)
					{
						/* Batch null checks: load all, OR together, 1 branch */
						sljit_sw null_off0 =
							(sljit_sw) &fcinfo->args[0].isnull -
							(sljit_sw) fcinfo;
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R1), null_off0);

						for (int argno = 1; argno < nargs; argno++)
						{
							sljit_sw null_off =
								(sljit_sw) &fcinfo->args[argno].isnull -
								(sljit_sw) fcinfo;
							sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
										   SLJIT_MEM1(SLJIT_R1), null_off);
							sljit_emit_op2(C, SLJIT_OR, SLJIT_R0, 0,
										   SLJIT_R0, 0, SLJIT_R2, 0);
						}

						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										   SLJIT_R0, 0,
										   SLJIT_IMM, 0);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = -1;
						npending++;
					}
					else
					{
						/* 1 arg or >4 args: per-arg check (original path) */
						for (int argno = 0; argno < nargs; argno++)
						{
							sljit_sw	null_off =
								(sljit_sw) &fcinfo->args[argno].isnull -
								(sljit_sw) fcinfo;

							sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
										   SLJIT_MEM1(SLJIT_R1), null_off);

							struct sljit_jump *j =
								sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											   SLJIT_R0, 0,
											   SLJIT_IMM, 0);
							pending_jumps[npending].jump = j;
							pending_jumps[npending].target = -1;
							npending++;
						}
					}
					/* R1 = fcinfo still valid for use below */
					r1_has_fcinfo = true;
				}

				/*
				 * Try direct native call — bypasses fcinfo entirely.
				 * Args are loaded from fcinfo->args[].value (already
				 * populated by preceding VAR/CONST steps).
				 */
				{
				const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);
#ifdef PG_JITTER_HAVE_INLINE_BLOBS
				bool used_precompiled = false;
#endif

#ifdef PG_JITTER_HAVE_INLINE_BLOBS
				/*
				 * PRECOMPILED PATH: try to emit optimized native code
				 * for ANY Tier 1 function that has a precompiled blob.
				 * Covers all 192+ functions (int, float, bool, date, ts, oid,
				 * hash, aggregates) — not just the 20 with inline_op tags.
				 *
				 * Skip functions whose blobs have no ret (tail calls to
				 * hash_bytes_uint32) — those 5 hash functions fall through
				 * to the direct-call path.
				 */
				if (dfn && dfn->jit_fn_name)
				{
					const PrecompiledInline *pi;
#ifdef PG_JITTER_HAVE_PRECOMPILED
					pi = jit_find_precompiled(dfn->jit_fn_name);
#elif defined(PG_JITTER_HAVE_MIR_PRECOMPILED)
					pi = mir_find_precompiled_blob(dfn->jit_fn_name);
#endif

					/*
					 * Only inline blobs that:
					 * - Have a ret instruction (skip tail-call stubs)
					 * - Are ≤48 bytes (avoid I-cache bloat from large
					 *   float/div blobs; those fall through to direct call)
					 */
					if (pi && pi->ret_offset >= 0 && pi->code_len <= 48)
					{
						/* Load args from fcinfo→args[].value into R0..R3 */
						if (dfn->nargs > 0)
						{
							int base_reg;
							if (r1_has_fcinfo)
							{
								sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
											   SLJIT_R1, 0);
								base_reg = SLJIT_R2;
							}
							else
							{
								emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);
								base_reg = SLJIT_R2;
							}
							for (int i = 0; i < dfn->nargs && i < 4; i++)
							{
								sljit_sw val_off =
									(sljit_sw) &fcinfo->args[i].value -
									(sljit_sw) fcinfo;
								sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0 + i, 0,
											   SLJIT_MEM1(base_reg), val_off);
							}
						}

						used_precompiled = emit_precompiled_inline(
							C, pi,
							precompiled_relocs,
							&n_precompiled_relocs);

						if (used_precompiled)
						{
							/* Store *op->resvalue = R0, *op->resnull = false */
							emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
						}
					}
				}

				if (!used_precompiled) {
#endif /* PG_JITTER_HAVE_INLINE_BLOBS */
				if (dfn && dfn->inline_op != JIT_INLINE_NONE)
				{
					/*
					 * TIER 0 — INLINE: emit the operation as sljit
					 * instructions, no function call at all.
					 * If strict, R1 already holds fcinfo from null checks.
					 */
					sljit_sw off0 =
						(sljit_sw) &fcinfo->args[0].value -
						(sljit_sw) fcinfo;
					sljit_sw off1 =
						(sljit_sw) &fcinfo->args[1].value -
						(sljit_sw) fcinfo;
					int fcinfo_reg = r1_has_fcinfo ? SLJIT_R1 : SLJIT_R2;

					if (!r1_has_fcinfo)
						emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);
					/* Load arg0 first, then arg1 (overwrites fcinfo_reg if R1) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(fcinfo_reg), off0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(fcinfo_reg), off1);

					emit_inline_funcexpr(C, (JitInlineOp) dfn->inline_op);

					/* Store *op->resvalue = R0, *op->resnull = false */
					emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				}
				else if (dfn && (dfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
						|| (dfn->jit_fn_name &&
							mir_find_precompiled_fn(dfn->jit_fn_name))
#endif
						))
				{
					/*
					 * TIER 1 — DIRECT CALL: native unwrapped function.
					 * Uses dfn->jit_fn or MIR-precompiled function pointer.
					 * If strict, R1 already holds fcinfo from null checks.
					 */
					void *call_target = dfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!call_target)
						call_target = mir_find_precompiled_fn(dfn->jit_fn_name);
#endif
					if (dfn->nargs > 0)
					{
						int base_reg;
						if (r1_has_fcinfo)
						{
							/*
							 * R1 = fcinfo from strict null checks.
							 * Move to R2 so R0/R1 are free for args.
							 */
							sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
										   SLJIT_R1, 0);
							base_reg = SLJIT_R2;
						}
						else
						{
							emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);
							base_reg = SLJIT_R2;
						}
						for (int i = 0; i < dfn->nargs; i++)
						{
							sljit_sw val_off =
								(sljit_sw) &fcinfo->args[i].value -
								(sljit_sw) fcinfo;
							sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0 + i, 0,
										   SLJIT_MEM1(base_reg), val_off);
						}
					}

					/* Direct call with native arg types */
					EMIT_ICALL(C, SLJIT_CALL, jit_sljit_call_type(dfn), call_target);

					/* Store *op->resvalue = R0, *op->resnull = false */
					emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				}
				else
				{
				/*
				 * TIER 2 — V1 FALLBACK: generic fcinfo path.
				 * If strict, R1 already holds fcinfo from null checks.
				 */

				/* fcinfo->isnull = false (must reset before each call;
				 * PG_RETURN_* macros don't clear it) */
				if (r1_has_fcinfo)
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_R1, 0);
				else
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);

				/* Call fn_addr(fcinfo) */
				/* R0 still = fcinfo */
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.func.fn_addr);

				/* *op->resvalue = R0 (return value) */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* *op->resnull = fcinfo->isnull */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(FunctionCallInfoBaseData, isnull));
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				} /* end V1 fallback */
#ifdef PG_JITTER_HAVE_INLINE_BLOBS
				} /* end if (!used_precompiled) */
#endif
				} /* end direct-call dispatch block */

				/* Fix up null-check jumps: emit null_path that sets resnull=true */
				if (opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
					|| opcode == EEOP_FUNCEXPR_STRICT_1
					|| opcode == EEOP_FUNCEXPR_STRICT_2
#endif
					)
				{
					/* Jump over null_path from the normal (non-null) path */
					struct sljit_jump *j_skip_null = sljit_emit_jump(C, SLJIT_JUMP);

					/* null_path: set *resnull = true */
					struct sljit_label *null_path = sljit_emit_label(C);
					emit_store_resnull_true(C, state, opno, op);

					/* All null-check jumps target null_path */
					for (int j = null_check_start; j < npending; j++)
					{
						if (pending_jumps[j].target == -1)
						{
							sljit_set_label(pending_jumps[j].jump, null_path);
							pending_jumps[j].target = -2; /* mark as resolved */
						}
					}

					/* after: both paths converge */
					struct sljit_label *after = sljit_emit_label(C);
					sljit_set_label(j_skip_null, after);
				}
				break;
			}

			/*
			 * ---- BOOL_AND_STEP ----
			 */
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_LAST:
			{
				struct sljit_jump *j_null, *j_false;
				struct sljit_label *cont;

				if (opcode == EEOP_BOOL_AND_STEP_FIRST)
				{
					/* *anynull = false */
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);
				}

				/* R0 = *op->resnull */
				emit_load_resnull(C, state, opno, op, SLJIT_R0);

				/* If null, set anynull and continue */
				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: check if value is false */
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);

				/* If false (value == 0), short-circuit: jump to done */
				j_false = sljit_emit_cmp(C, SLJIT_EQUAL,
										 SLJIT_R0, 0, SLJIT_IMM, 0);
				pending_jumps[npending].jump = j_false;
				pending_jumps[npending].target = op->d.boolexpr.jumpdone;
				npending++;

				/* Jump over the null handler to continuation */
				{
					struct sljit_jump *j_skip = sljit_emit_jump(C, SLJIT_JUMP);

					/* Null handler: set *anynull = true */
					sljit_set_label(j_null, sljit_emit_label(C));
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 1);

					cont = sljit_emit_label(C);
					sljit_set_label(j_skip, cont);
				}

				/* At end: if anynull, set resvalue=0, resnull=true */
				if (opcode == EEOP_BOOL_AND_STEP_LAST)
				{
					struct sljit_jump *j_no_anynull;

					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), 0);

					j_no_anynull = sljit_emit_cmp(C, SLJIT_EQUAL,
												  SLJIT_R0, 0, SLJIT_IMM, 0);

					/* Set result to NULL */
					emit_store_resnull_true(C, state, opno, op);
					emit_store_resvalue_imm(C, state, opno, op, 0);

					sljit_set_label(j_no_anynull, sljit_emit_label(C));
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
				struct sljit_jump *j_null, *j_true;

				if (opcode == EEOP_BOOL_OR_STEP_FIRST)
				{
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);
				}

				/* Check null */
				emit_load_resnull(C, state, opno, op, SLJIT_R0);

				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: check if true */
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);

				/* If true (value != 0), short-circuit */
				j_true = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);
				pending_jumps[npending].jump = j_true;
				pending_jumps[npending].target = op->d.boolexpr.jumpdone;
				npending++;

				{
					struct sljit_jump *j_skip = sljit_emit_jump(C, SLJIT_JUMP);

					/* Null handler */
					sljit_set_label(j_null, sljit_emit_label(C));
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 1);

					sljit_set_label(j_skip, sljit_emit_label(C));
				}

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					struct sljit_jump *j_no_anynull;

					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.boolexpr.anynull), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), 0);

					j_no_anynull = sljit_emit_cmp(C, SLJIT_EQUAL,
												  SLJIT_R0, 0, SLJIT_IMM, 0);

					emit_store_resnull_true(C, state, opno, op);
					emit_store_resvalue_imm(C, state, opno, op, 0);

					sljit_set_label(j_no_anynull, sljit_emit_label(C));
				}
				break;
			}

			/*
			 * ---- BOOL_NOT_STEP ----
			 */
			case EEOP_BOOL_NOT_STEP:
			{
				/* R0 = *op->resvalue */
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);

				/* R0 = (R0 == 0) ? 1 : 0 */
				sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
								SLJIT_R0, 0, SLJIT_IMM, 0);
				sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_EQUAL);

				/* Store back (as Datum, which is pointer-sized) */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- QUAL ----
			 * If null or false, jump to jumpdone.
			 */
			case EEOP_QUAL:
			{
				struct sljit_jump *j_null, *j_true;

				/*
				 * If null or false → set resvalue=0, resnull=false, jump
				 * to jumpdone.  Otherwise continue to next step.
				 *
				 * Layout: null check → value check → fail path →
				 * continue label.
				 */

				/* Check null: if *resnull != 0, jump to fail */
				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Check true: if *resvalue != 0, jump to continue */
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);
				j_true = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Fail: resvalue=0, resnull=false, jump to jumpdone. */
				{
					struct sljit_label *fail_label = sljit_emit_label(C);
					sljit_set_label(j_null, fail_label);

					emit_store_resnull_false(C, state, opno, op);
					emit_store_resvalue_imm(C, state, opno, op, 0);

					struct sljit_jump *j_done = sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_done;
					pending_jumps[npending].target = op->d.qualexpr.jumpdone;
					npending++;
				}

				/* Continue: not null and not false */
				sljit_set_label(j_true, sljit_emit_label(C));
				break;
			}

			/*
			 * ---- JUMP / JUMP_IF_NULL / JUMP_IF_NOT_NULL / JUMP_IF_NOT_TRUE ----
			 */
			case EEOP_JUMP:
			{
				struct sljit_jump *j = sljit_emit_jump(C, SLJIT_JUMP);
				pending_jumps[npending].jump = j;
				pending_jumps[npending].target = op->d.jump.jumpdone;
				npending++;
				break;
			}

			case EEOP_JUMP_IF_NULL:
			{
				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				struct sljit_jump *j = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
													  SLJIT_R0, 0,
													  SLJIT_IMM, 0);
				pending_jumps[npending].jump = j;
				pending_jumps[npending].target = op->d.jump.jumpdone;
				npending++;
				break;
			}

			case EEOP_JUMP_IF_NOT_NULL:
			{
				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				struct sljit_jump *j = sljit_emit_cmp(C, SLJIT_EQUAL,
													  SLJIT_R0, 0,
													  SLJIT_IMM, 0);
				pending_jumps[npending].jump = j;
				pending_jumps[npending].target = op->d.jump.jumpdone;
				npending++;
				break;
			}

			case EEOP_JUMP_IF_NOT_TRUE:
			{
				/* Jump if null OR false */
				emit_load_resnull(C, state, opno, op, SLJIT_R0);

				/* If null, jump */
				struct sljit_jump *j1 = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
													   SLJIT_R0, 0,
													   SLJIT_IMM, 0);
				pending_jumps[npending].jump = j1;
				pending_jumps[npending].target = op->d.jump.jumpdone;
				npending++;

				/* If false, jump */
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);
				struct sljit_jump *j2 = sljit_emit_cmp(C, SLJIT_EQUAL,
													   SLJIT_R0, 0,
													   SLJIT_IMM, 0);
				pending_jumps[npending].jump = j2;
				pending_jumps[npending].target = op->d.jump.jumpdone;
				npending++;
				break;
			}

			/*
			 * ---- NULLTEST ----
			 */
			case EEOP_NULLTEST_ISNULL:
			{
				/* resvalue = (resnull ? 1 : 0); resnull = false */
				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				emit_load_resnull(C, state, opno, op, SLJIT_R0);

				/* R0 = !R0: XOR with 1 */
				sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);
				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- AGGREGATE INPUT NULL CHECKS ----
			 * Inline: load isnull bool, conditional jump. No function call.
			 */
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
#ifdef HAVE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
#endif
			{
				NullableDatum *args = op->d.agg_strict_input_check.args;
				int			nargs = op->d.agg_strict_input_check.nargs;
				int			jumpnull = op->d.agg_strict_input_check.jumpnull;

				/*
				 * Use base+offset addressing: load args base once,
				 * then use argno * sizeof(NullableDatum) + offsetof(isnull).
				 * OR-batch for nargs <= 4 to emit a single branch.
				 */
				sljit_sw isnull_off0 = offsetof(NullableDatum, isnull);
				sljit_sw nd_size = (sljit_sw) sizeof(NullableDatum);

				/* R1 = args base pointer */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_strict_input_check.args), SLJIT_R1);

				if (nargs <= 4 && nargs > 1)
				{
					/* OR-batch: load all isnull flags, OR together, 1 branch */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), isnull_off0);
					for (int argno = 1; argno < nargs; argno++)
					{
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_R1),
									   argno * nd_size + isnull_off0);
						sljit_emit_op2(C, SLJIT_OR, SLJIT_R0, 0,
									   SLJIT_R0, 0, SLJIT_R2, 0);
					}
					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = jumpnull;
					npending++;
				}
				else
				{
					for (int argno = 0; argno < nargs; argno++)
					{
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R1),
									   argno * nd_size + isnull_off0);
						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										   SLJIT_R0, 0, SLJIT_IMM, 0);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = jumpnull;
						npending++;
					}
				}
				break;
			}

			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
			{
				bool	   *nulls = op->d.agg_strict_input_check.nulls;
				int			nargs = op->d.agg_strict_input_check.nargs;
				int			jumpnull = op->d.agg_strict_input_check.jumpnull;

				/* R1 = nulls base pointer */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_strict_input_check.nulls), SLJIT_R1);

				if (nargs <= 4 && nargs > 1)
				{
					/* OR-batch null checks */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), 0);
					for (int argno = 1; argno < nargs; argno++)
					{
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_R1),
									   argno * (sljit_sw) sizeof(bool));
						sljit_emit_op2(C, SLJIT_OR, SLJIT_R0, 0,
									   SLJIT_R0, 0, SLJIT_R2, 0);
					}
					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = jumpnull;
					npending++;
				}
				else
				{
					for (int argno = 0; argno < nargs; argno++)
					{
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R1),
									   argno * (sljit_sw) sizeof(bool));
						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										   SLJIT_R0, 0, SLJIT_IMM, 0);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = jumpnull;
						npending++;
					}
				}
				break;
			}

			/*
			 * ---- AGGREGATE PERGROUP NULL CHECK ----
			 */
			case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
			{
				int setoff = op->d.agg_plain_pergroup_nullcheck.setoff;
				int jumpnull = op->d.agg_plain_pergroup_nullcheck.jumpnull;

				/* R0 = state->parent (AggState*) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, parent));
				/* R0 = aggstate->all_pergroups */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(AggState, all_pergroups));
				/* R0 = all_pergroups[setoff] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   setoff * (sljit_sw) sizeof(AggStatePerGroup));

				/* if NULL, jump */
				struct sljit_jump *j =
					sljit_emit_cmp(C, SLJIT_EQUAL,
								   SLJIT_R0, 0, SLJIT_IMM, 0);
				pending_jumps[npending].jump = j;
				pending_jumps[npending].target = jumpnull;
				npending++;
				break;
			}

			/*
			 * ---- AGGREGATE TRANSITIONS (inline) ----
			 * All addresses known at codegen time.  Only fn_addr
			 * and rarely ExecAggInitGroup / ExecAggCopyTransValue
			 * remain as function calls.
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

				/*
				 * Compile-time known pointers.
				 * aggstate is in S3, &CurrentMemoryContext is in S4.
				 */
				AggState   *aggstate = castNode(AggState, state->parent);
				AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
				int			setoff = op->d.agg_trans.setoff;
				int			transno = op->d.agg_trans.transno;
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				PGFunction	fn_addr = fcinfo->flinfo->fn_addr;
				ExprContext *aggcontext = op->d.agg_trans.aggcontext;
				int			setno = op->d.agg_trans.setno;

				/* Precompute offsets for fcinfo->args[0] */
				sljit_sw off_args0_val =
					(sljit_sw) &fcinfo->args[0].value -
					(sljit_sw) fcinfo;
				sljit_sw off_args0_null =
					(sljit_sw) &fcinfo->args[0].isnull -
					(sljit_sw) fcinfo;

				/* Local jumps that target the end label */
				struct sljit_jump *j_to_end[2];
				int		n_to_end = 0;

				/*
				 * Compute pergroup at runtime — all_pergroups[setoff]
				 * changes per tuple for hash aggregation.
				 *
				 * S5 = aggstate, so load all_pergroups directly
				 * from [S5 + offset] (1 LDR, no IMM load needed).
				 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S5),
							   offsetof(AggState, all_pergroups));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   setoff * (sljit_sw) sizeof(AggStatePerGroup));
				if (transno != 0)
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
								   SLJIT_R0, 0,
								   SLJIT_IMM,
								   transno * (sljit_sw) sizeof(AggStatePerGroupData));
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_PERGROUP,
							   SLJIT_R0, 0);

				/* -- INIT check (INIT_STRICT variants only) -- */
				/* R0 = pergroup from above */
				if (is_init)
				{
					struct sljit_jump *j_no_init;

					/* R1 = pergroup->noTransValue */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											noTransValue));

					/* if noTransValue == 0, skip init */
					j_no_init = sljit_emit_cmp(C, SLJIT_EQUAL,
											   SLJIT_R1, 0, SLJIT_IMM, 0);

					/*
					 * ExecAggInitGroup(aggstate, pertrans, pergroup,
					 *                  aggcontext)
					 * R0 = aggstate from S3 (register move, no IMM).
					 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_R0, 0);	/* R2 = pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_S5, 0);	/* R0 = aggstate */
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R1);
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.aggcontext), SLJIT_R3);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, P), ExecAggInitGroup);

					/* Jump to end (skip transition body) */
					j_to_end[n_to_end++] =
						sljit_emit_jump(C, SLJIT_JUMP);

					sljit_set_label(j_no_init, sljit_emit_label(C));
				}

				/* -- STRICT check (all STRICT variants) -- */
				if (is_strict)
				{
					/* Reload pergroup from stack */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					/* R1 = pergroup->transValueIsNull */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull));

					/* if transValueIsNull != 0, skip transition */
					j_to_end[n_to_end++] =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R1, 0, SLJIT_IMM, 0);
				}

				/*
				 * ---- Transition body ----
				 *
				 * For BYVAL aggregates with recognized simple transition
				 * functions, emit inline arithmetic instead of calling
				 * through fn_addr.  This eliminates aggstate field setup,
				 * MemoryContextSwitchTo, fcinfo marshaling, and the
				 * indirect function call (~30 instructions saved).
				 */
				if (!is_byref &&
					(fn_addr == int8inc || fn_addr == int8inc_any))
				{
					/*
					 * Inline int8inc / int8inc_any: transValue += 1.
					 * Used by COUNT(*) and COUNT(col).
					 */
					/* R0 = pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					/* R1 = pergroup->transValue (int64) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue));
					/* R1 += 1, check overflow */
					sljit_emit_op2(C, SLJIT_ADD | SLJIT_SET_OVERFLOW,
								   SLJIT_R1, 0, SLJIT_R1, 0,
								   SLJIT_IMM, 1);
					{
						struct sljit_jump *j_ok;

						j_ok = sljit_emit_jump(C, SLJIT_NOT_OVERFLOW);
						EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS0V(), jit_error_int8_overflow);
						sljit_set_label(j_ok, sljit_emit_label(C));
					}
					/* Store result */
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue),
								   SLJIT_R1, 0);
					/* transValueIsNull = false */
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull),
								   SLJIT_IMM, 0);
				}
				else if (!is_byref && fn_addr == int4_sum)
				{
					/*
					 * Inline int4_sum: transValue += (int64)arg1.
					 * Used by SUM(int4).  No overflow possible
					 * (int32 range fits in int64 headroom).
					 *
					 * int4_sum is NOT strict (handles NULL arg0 itself),
					 * so BYVAL variant has no INIT/STRICT checks.
					 * We handle the first-call case (transValueIsNull)
					 * and arg1-is-null case inline.
					 */
					struct sljit_jump *j_not_null;

					/* Check if arg1 is null — if so, skip entirely.
					 * Load fcinfo base into R1, reuse for arg1.value below. */
					{
						sljit_sw isnull_off =
							(sljit_sw) &fcinfo->args[1].isnull -
							(sljit_sw) fcinfo;
						emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R1);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1), offsetof(AggStatePerTransData, transfn_fcinfo));
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R1), isnull_off);
						struct sljit_jump *j_arg_null =
							sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										   SLJIT_R0, 0, SLJIT_IMM, 0);
						j_to_end[n_to_end++] = j_arg_null;
					}

					/* R0 = pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					/* R2 = fcinfo->args[1].value (int4 as Datum)
					 * R1 still holds fcinfo base from null check above */
					{
						sljit_sw val_off =
							(sljit_sw) &fcinfo->args[1].value -
							(sljit_sw) fcinfo;
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_R1), val_off);
					}
					/* Sign-extend int32 → int64 */
					sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R2, 0,
								   SLJIT_R2, 0);

					/* Check transValueIsNull */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R3, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull));
					j_not_null = sljit_emit_cmp(C, SLJIT_EQUAL,
												SLJIT_R3, 0,
												SLJIT_IMM, 0);

					/* First non-null input: transValue = (int64)arg1 */
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue),
								   SLJIT_R2, 0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull),
								   SLJIT_IMM, 0);
					j_to_end[n_to_end++] =
						sljit_emit_jump(C, SLJIT_JUMP);

					/* Normal case: transValue += (int64)arg1 */
					sljit_set_label(j_not_null, sljit_emit_label(C));
					/* R1 = pergroup->transValue (int64) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue));
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
								   SLJIT_R1, 0, SLJIT_R2, 0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue),
								   SLJIT_R1, 0);
				}
				else if (!is_byref &&
						 (fn_addr == int4smaller ||
						  fn_addr == int4larger))
				{
					/*
					 * Inline int4smaller/int4larger: MIN/MAX(int4).
					 * Compare transValue vs new input, conditionally
					 * update.  Both values are int32 stored as
					 * sign-extended Datum; 64-bit signed compare is
					 * correct.
					 */
					bool is_min = (fn_addr == int4smaller);
					struct sljit_jump *j_skip;

					/* R0 = pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					/* R1 = pergroup->transValue (current min/max) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue));
					/* R2 = fcinfo->args[1].value (new input) */
					{
						sljit_sw val_off =
							(sljit_sw) &fcinfo->args[1].value -
							(sljit_sw) fcinfo;
						emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R2);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R2), offsetof(AggStatePerTransData, transfn_fcinfo));
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_R2), val_off);
					}

					/*
					 * Sign-extend both values from int32 to int64
					 * before comparing.  PG stores int32 in Datum via
					 * Int32GetDatum() which zero-extends on 64-bit,
					 * so 64-bit signed compare would be wrong for
					 * negative values (e.g. -1 appears as 0xFFFFFFFF
					 * = 4294967295).
					 */
					sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R1, 0,
								   SLJIT_R1, 0);
					sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R2, 0,
								   SLJIT_R2, 0);

					/*
					 * MIN: skip update if current <= new
					 * MAX: skip update if current >= new
					 */
					j_skip = sljit_emit_cmp(C,
						is_min ? SLJIT_SIG_LESS_EQUAL
							   : SLJIT_SIG_GREATER_EQUAL,
						SLJIT_R1, 0, SLJIT_R2, 0);

					/* Update transValue = new input */
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue),
								   SLJIT_R2, 0);

					sljit_set_label(j_skip, sljit_emit_label(C));

					/* transValueIsNull = false */
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull),
								   SLJIT_IMM, 0);
				}
				else if (is_byref &&
						 (fn_addr == int4_avg_accum ||
						  fn_addr == int2_avg_accum))
				{
					/*
					 * Inline int4_avg_accum / int2_avg_accum.
					 *
					 * These are BYREF aggregates but modify the
					 * transition array in-place when called in
					 * aggregate context (which we always are).
					 * The array holds an Int8TransTypeData {count,
					 * sum} at offset ARR_OVERHEAD_NONULLS(1) = 24
					 * from the ArrayType pointer.  We just do:
					 *   transdata->count++
					 *   transdata->sum += newval
					 *
					 * No MemoryContextSwitchTo, no fcinfo, no call.
					 */
#define INT8_TRANS_DATA_OFFSET	24
					StaticAssertDecl(
						ARR_OVERHEAD_NONULLS(1) == INT8_TRANS_DATA_OFFSET,
						"Int8TransTypeData offset must be 24");

					/* R0 = pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					/* R1 = DatumGetPointer(pergroup->transValue) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue));
					/* R2 = R1 + 24 = pointer to Int8TransTypeData */
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0,
								   SLJIT_R1, 0,
								   SLJIT_IMM, INT8_TRANS_DATA_OFFSET);

					/* transdata->count++ (int64 at offset 0) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
								   SLJIT_MEM1(SLJIT_R2), 0);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
								   SLJIT_R3, 0, SLJIT_IMM, 1);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R2), 0,
								   SLJIT_R3, 0);

					/* R3 = fcinfo->args[1].value (new input) */
					{
						sljit_sw val_off =
							(sljit_sw) &fcinfo->args[1].value -
							(sljit_sw) fcinfo;
						emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R3);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_R3), offsetof(AggStatePerTransData, transfn_fcinfo));
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
									   SLJIT_MEM1(SLJIT_R3), val_off);
					}
					/*
					 * Sign-extend: int32 for int4_avg_accum,
					 * int16 for int2_avg_accum.
					 */
					if (fn_addr == int4_avg_accum)
						sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R3, 0,
									   SLJIT_R3, 0);
					else
						sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R3, 0,
									   SLJIT_R3, 0);

					/* transdata->sum += newval (int64 at offset 8) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R2),
								   (sljit_sw) offsetof(JitInt8TransTypeData,
														sum));
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
								   SLJIT_R1, 0, SLJIT_R3, 0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R2),
								   (sljit_sw) offsetof(JitInt8TransTypeData,
														sum),
								   SLJIT_R1, 0);

					/*
					 * transValue unchanged (same array pointer),
					 * just ensure transValueIsNull = false.
					 */
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull),
								   SLJIT_IMM, 0);
#undef INT8_TRANS_DATA_OFFSET
				}
				else
				{
					/*
					 * Generic path: aggstate field setup,
					 * MemoryContextSwitchTo, fcinfo marshaling,
					 * fn_addr call, result store, context restore.
					 */
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.aggcontext), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_S5),
								   offsetof(AggState, curaggcontext),
								   SLJIT_R0, 0);
					sljit_emit_op1(C, SLJIT_MOV_S32,
								   SLJIT_MEM1(SLJIT_S5),
								   offsetof(AggState, current_set),
								   SLJIT_IMM, setno);
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_S5),
								   offsetof(AggState, curpertrans),
								   SLJIT_R0, 0);

					/* MemoryContextSwitchTo(tuple_mctx) */
					/* R1 = &CurrentMemoryContext (from stack) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_CURRMCTXP);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), 0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_OLDCTX,
								   SLJIT_R0, 0);
					/*
					 * Load tuple_mctx at runtime:
					 * aggstate->tmpcontext->ecxt_per_tuple_memory
					 * (PIC: same code works in leader and worker)
					 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_S5),
								   offsetof(AggState, tmpcontext));
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(ExprContext, ecxt_per_tuple_memory));
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_R0, 0);

					/* Setup fcinfo->args[0] from pergroup */
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R2);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R2), offsetof(AggStatePerTransData, transfn_fcinfo));
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_FCINFO,
								   SLJIT_R2, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValue));
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R2), off_args0_val,
								   SLJIT_R3, 0);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R3, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(AggStatePerGroupData,
											transValueIsNull));
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R2), off_args0_null,
								   SLJIT_R3, 0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R2),
								   offsetof(FunctionCallInfoBaseData,
											isnull),
								   SLJIT_IMM, 0);

					/* Call fn_addr(fcinfo) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_R2, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), fn_addr);

					/* Store result back to pergroup */
					if (!is_byref)
					{
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
									   SLJIT_MEM1(SLJIT_SP),
									   SOFF_AGG_PERGROUP);
						sljit_emit_op1(C, SLJIT_MOV,
									   SLJIT_MEM1(SLJIT_R1),
									   offsetof(AggStatePerGroupData,
												transValue),
									   SLJIT_R0, 0);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_MEM1(SLJIT_SP),
									   SOFF_AGG_FCINFO);
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R2),
									   offsetof(FunctionCallInfoBaseData,
												isnull));
						sljit_emit_op1(C, SLJIT_MOV_U8,
									   SLJIT_MEM1(SLJIT_R1),
									   offsetof(AggStatePerGroupData,
												transValueIsNull),
									   SLJIT_R0, 0);
					}
					else
					{
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_R0, 0);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_S5, 0);
						emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_trans.pertrans), SLJIT_R1);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
									   SLJIT_MEM1(SLJIT_SP),
									   SOFF_AGG_PERGROUP);
						EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS4V(P, P, W, P), pg_jitter_agg_byref_finish);
					}

					/* Restore memory context */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_OLDCTX);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_CURRMCTXP);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_R0, 0);
				}

				/* End label: skip-jumps land here */
				{
					struct sljit_label *lbl_end = sljit_emit_label(C);

					for (int j = 0; j < n_to_end; j++)
						sljit_set_label(j_to_end[j], lbl_end);
				}
				break;
			}

			/*
			 * ---- PRESORTED DISTINCT ----
			 * Call extern function, branch on result.
			 */
#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
			case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
			case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
			{
				int jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;
				void *fn = (opcode == EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
					? (void *) ExecEvalPreOrderedDistinctSingle
					: (void *) ExecEvalPreOrderedDistinctMulti;

				/*
				 * Direct call: fn(aggstate, pertrans) -> bool.
				 * If returns false (not distinct), jump to jumpdistinct.
				 * R0 = aggstate (from parent), R1 = pertrans.
				 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, parent));
				emit_load_step_field(C, opno,
					offsetof(ExprEvalStep,
							 d.agg_presorted_distinctcheck.pertrans),
					SLJIT_R1);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS2(W, P, P), fn);

				/* If result == 0 (not distinct), jump */
				struct sljit_jump *j =
					sljit_emit_cmp(C, SLJIT_EQUAL,
								   SLJIT_R0, 0, SLJIT_IMM, 0);
				pending_jumps[npending].jump = j;
				pending_jumps[npending].target = jumpdistinct;
				npending++;
				break;
			}
#endif /* HAVE_EEOP_AGG_PRESORTED_DISTINCT */

			/*
			 * ---- HASHDATUM_SET_INITVAL ----
			 * Store init_value → *op->resvalue, set *op->resnull = false.
			 */
#ifdef HAVE_EEOP_HASHDATUM
			case EEOP_HASHDATUM_SET_INITVAL:
			{
				/* *op->resvalue = op->d.hashdatum_initvalue.init_value */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM,
							   (sljit_sw) op->d.hashdatum_initvalue.init_value);
				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- HASHDATUM_FIRST ----
			 * Non-strict: if arg is null, store 0; else call hash fn.
			 * Always sets *op->resnull = false.
			 */
			case EEOP_HASHDATUM_FIRST:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				struct sljit_jump *j_isnull;
				struct sljit_jump *j_done;

				/* R1 = fcinfo (kept alive for value load below) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.hashdatum.fcinfo_data), SLJIT_R1);
				/* R0 = fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull != 0, jump to store_zero */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null path: call hash function (R1 still = fcinfo) */
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: load arg from fcinfo->args[0].value */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), val_off);
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					EMIT_ICALL(C, SLJIT_CALL, jit_sljit_call_type(hdfn), hash_target);
				}
				}
				else
				{
					/* Fallback: fcinfo path (R1 = fcinfo, use as R0 arg) */
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_R1, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.hashdatum.fn_addr);
				}

				/* Jump past store_zero */
				j_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* store_zero: R0 = 0 */
				{
					struct sljit_label *lbl_zero = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_zero);
				}
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, 0);

				/* store_result: */
				{
					struct sljit_label *lbl_result = sljit_emit_label(C);
					sljit_set_label(j_done, lbl_result);
				}

				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- HASHDATUM_FIRST_STRICT ----
			 * Strict: if arg is null, set resnull=true, resvalue=0,
			 * jump to jumpdone.  Else call hash fn.
			 */
			case EEOP_HASHDATUM_FIRST_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				struct sljit_jump *j_isnull;

				/* R1 = fcinfo (kept alive for value load) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.hashdatum.fcinfo_data), SLJIT_R1);
				/* R0 = fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull != 0, jump to null_path */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null path: call hash function (R1 still = fcinfo) */
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), val_off);
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					EMIT_ICALL(C, SLJIT_CALL, jit_sljit_call_type(hdfn), hash_target);
				}
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_R1, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.hashdatum.fn_addr);
				}

				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);

				/* Jump past null_path (fall through to next step) */
				{
					struct sljit_jump *j_skip_null = sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_skip_null;
					pending_jumps[npending].target = -1; /* resolved below */
					npending++;

					/* null_path: */
					struct sljit_label *lbl_null = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_null);

					/* *op->resnull = true, *op->resvalue = 0 */
					emit_store_res_pair_true_imm(C, state, opno, op, 0);

					/* Jump to jumpdone */
					struct sljit_jump *j_jumpdone =
						sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_jumpdone;
					pending_jumps[npending].target = op->d.hashdatum.jumpdone;
					npending++;

					/* Label after null_path for the skip jump */
					struct sljit_label *lbl_after = sljit_emit_label(C);
					/* Fix up the skip jump (target == -1) */
					sljit_set_label(j_skip_null, lbl_after);
					/* Mark it resolved */
					pending_jumps[npending - 2].target = -2;
				}
				break;
			}

			/*
			 * ---- HASHDATUM_NEXT32 ----
			 * Non-strict: rotate existing hash left 1, optionally XOR
			 * with new hash value.  Always sets *op->resnull = false.
			 */
			case EEOP_HASHDATUM_NEXT32:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				NullableDatum *iresult = op->d.hashdatum.iresult;
				struct sljit_jump *j_isnull;
				struct sljit_jump *j_done;

				/* Load existing hash from iresult->value as uint32 into R0 */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.hashdatum.iresult), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(NullableDatum, value));

				/*
				 * Rotate left 1.  If we have a dedicated saved register,
				 * store there (survives call without stack traffic).
				 * Otherwise spill to SOFF_TEMP on the stack.
				 */
				if (use_sreg_hash)
				{
					sljit_emit_op2(C, SLJIT_ROTL32, sreg_hash, 0,
								   SLJIT_R0, 0, SLJIT_IMM, 1);
				}
				else
				{
					sljit_emit_op2(C, SLJIT_ROTL32, SLJIT_R0, 0,
								   SLJIT_R0, 0, SLJIT_IMM, 1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
								   SOFF_TEMP, SLJIT_R0, 0);
				}

				/* R1 = fcinfo (kept alive for value load) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.hashdatum.fcinfo_data), SLJIT_R1);
				/* R0 = fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull, skip hash call */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: call hash function (R1 still = fcinfo) */
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: reuse R1 for arg load */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), val_off);
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					EMIT_ICALL(C, SLJIT_CALL, jit_sljit_call_type(hdfn), hash_target);
				}
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_R1, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.hashdatum.fn_addr);
				}

				/* XOR rotated hash with hash result (R0) */
				if (use_sreg_hash)
				{
					sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0,
								   sreg_hash, 0, SLJIT_R0, 0);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
					sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0,
								   SLJIT_R1, 0, SLJIT_R0, 0);
				}

				j_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* skip_hash: R0 = rotated hash (from register or stack) */
				{
					struct sljit_label *lbl_skip = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_skip);
				}
				if (use_sreg_hash)
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   sreg_hash, 0);
				else
					sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);

				/* store_result: both paths converge, R0 = hash */
				{
					struct sljit_label *lbl_store = sljit_emit_label(C);
					sljit_set_label(j_done, lbl_store);
				}

				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- HASHDATUM_NEXT32_STRICT ----
			 * Strict: if arg is null, set resnull=true, resvalue=0,
			 * jump to jumpdone.  Else rotate+XOR as NEXT32.
			 */
			case EEOP_HASHDATUM_NEXT32_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				NullableDatum *iresult = op->d.hashdatum.iresult;
				struct sljit_jump *j_isnull;

				/* R1 = fcinfo (kept alive for value load) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.hashdatum.fcinfo_data), SLJIT_R1);
				/* R0 = fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull, jump to null_path */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null path: load existing hash, rotate, call, XOR */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.hashdatum.iresult), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(NullableDatum, value));

				/* Rotate left 1, store in saved reg or stack */
				if (use_sreg_hash)
				{
					sljit_emit_op2(C, SLJIT_ROTL32, sreg_hash, 0,
								   SLJIT_R0, 0, SLJIT_IMM, 1);
				}
				else
				{
					sljit_emit_op2(C, SLJIT_ROTL32, SLJIT_R0, 0,
								   SLJIT_R0, 0, SLJIT_IMM, 1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
								   SOFF_TEMP, SLJIT_R0, 0);
				}

				/* Call hash function (R1 still = fcinfo) */
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: reuse R1 for arg load */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R1), val_off);
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					EMIT_ICALL(C, SLJIT_CALL, jit_sljit_call_type(hdfn), hash_target);
				}
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_R1, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.hashdatum.fn_addr);
				}

				/* XOR rotated hash with hash result (R0) */
				if (use_sreg_hash)
				{
					sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0,
								   sreg_hash, 0, SLJIT_R0, 0);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
					sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0,
								   SLJIT_R1, 0, SLJIT_R0, 0);
				}
				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);

				/* Jump past null_path */
				{
					struct sljit_jump *j_skip_null = sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_skip_null;
					pending_jumps[npending].target = -1;
					npending++;

					/* null_path: */
					struct sljit_label *lbl_null = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_null);

					/* *op->resnull = true, *op->resvalue = 0 */
					emit_store_res_pair_true_imm(C, state, opno, op, 0);

					/* Jump to jumpdone */
					struct sljit_jump *j_jumpdone =
						sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_jumpdone;
					pending_jumps[npending].target = op->d.hashdatum.jumpdone;
					npending++;

					/* Label after null_path for skip jump */
					struct sljit_label *lbl_after = sljit_emit_label(C);
					sljit_set_label(j_skip_null, lbl_after);
					pending_jumps[npending - 2].target = -2;
				}
				break;
			}
#endif /* HAVE_EEOP_HASHDATUM */

			/*
			 * ---- CASE_TESTVAL / CASE_TESTVAL_EXT ----
			 * Copy casetest value/null into resvalue/resnull.
			 */
			case EEOP_CASE_TESTVAL:
			{
				/* *op->resvalue = *op->d.casetest.value */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.casetest.value), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0), 0);
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* *op->resnull = *op->d.casetest.isnull */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.casetest.isnull), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0), 0);
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}


#ifdef HAVE_EEOP_TESTVAL_EXT
			case EEOP_CASE_TESTVAL_EXT:
			{
				/* *op->resvalue = econtext->caseValue_datum */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, caseValue_datum));
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* *op->resnull = econtext->caseValue_isNull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, caseValue_isNull));
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}
#endif

			/*
			 * ---- DOMAIN_TESTVAL / DOMAIN_TESTVAL_EXT ----
			 * Identical logic to CASE_TESTVAL / CASE_TESTVAL_EXT:
			 * copy from cached pointer or from econtext fields.
			 */
			case EEOP_DOMAIN_TESTVAL:
			{
				/* *op->resvalue = *op->d.casetest.value */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.casetest.value), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0), 0);
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* *op->resnull = *op->d.casetest.isnull */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.casetest.isnull), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0), 0);
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}


#ifdef HAVE_EEOP_TESTVAL_EXT
			case EEOP_DOMAIN_TESTVAL_EXT:
			{
				/* *op->resvalue = econtext->domainValue_datum */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, domainValue_datum));
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* *op->resnull = econtext->domainValue_isNull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, domainValue_isNull));
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}
#endif

			/*
			 * ---- SYSVAR ----
			 * Direct call to ExecEvalSysVar(state, op, econtext, slot).
			 * 4-arg call; slot determined by opcode variant.
			 */
			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_SYSVAR:
			case EEOP_NEW_SYSVAR:
#endif
			{
				sljit_sw slot_offset;

				switch (opcode)
				{
					case EEOP_INNER_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_innertuple);
						break;
					case EEOP_OUTER_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_outertuple);
						break;
					case EEOP_SCAN_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_scantuple);
						break;

#ifdef HAVE_EEOP_OLD_NEW
					case EEOP_OLD_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_oldtuple);
						break;
					case EEOP_NEW_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_newtuple);
						break;
#endif
					default:
						pg_unreachable();
				}
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				/* R3 = econtext->ecxt_*tuple (slot) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
							   SLJIT_MEM1(SLJIT_S1), slot_offset);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, P), ExecEvalSysVar);
				break;
			}

			/*
			 * ---- IOCOERCE ----
			 * Two function calls: output fn → cstring → input fn.
			 * If *resnull, skip entirely.
			 */
			case EEOP_IOCOERCE:
			{
				FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

				struct sljit_jump *j_skip_null;

				/* if (*op->resnull) skip */
				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				j_skip_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											 SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Setup and call output function */
				/* fcinfo_out->args[0].value = *op->resvalue */
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.iocoerce.fcinfo_data_out), SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) &fcinfo_out->args[0].value -
							   (sljit_sw) fcinfo_out,
							   SLJIT_R0, 0);
				/* fcinfo_out->args[0].isnull = false */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) &fcinfo_out->args[0].isnull -
							   (sljit_sw) fcinfo_out,
							   SLJIT_IMM, 0);
				/* fcinfo_out->isnull = false */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);
				/* R0 = fcinfo_out->flinfo->fn_addr(fcinfo_out) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_R1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), fcinfo_out->flinfo->fn_addr);

				/* R0 = cstring result; setup input function */
				/* fcinfo_in->args[0].value = R0 (cstring as Datum) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.iocoerce.fcinfo_data_in), SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) &fcinfo_in->args[0].value -
							   (sljit_sw) fcinfo_in,
							   SLJIT_R0, 0);
				/* fcinfo_in->args[0].isnull = false */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) &fcinfo_in->args[0].isnull -
							   (sljit_sw) fcinfo_in,
							   SLJIT_IMM, 0);
				/* fcinfo_in->isnull = false */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);
				/* R0 = fcinfo_in->flinfo->fn_addr(fcinfo_in) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_R1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), fcinfo_in->flinfo->fn_addr);

				/* *op->resvalue = R0 */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);
				/* *op->resnull = fcinfo_in->isnull */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.iocoerce.fcinfo_data_in), SLJIT_R2);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R2),
							   offsetof(FunctionCallInfoBaseData, isnull));
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);

				sljit_set_label(j_skip_null, sljit_emit_label(C));
				break;
			}

			/*
			 * ---- ROWCOMPARE_FINAL ----
			 * Read int32 result from *op->resvalue, apply comparison
			 * (cmptype known at compile time), store bool.
			 */
			case EEOP_ROWCOMPARE_FINAL:
			{
				CompareType cmptype = op->d.rowcompare_final.cmptype;
				sljit_s32 cmp_flag;

				/*
				 * Pick comparison flag: cmpresult is int32, test
				 * against 0 to produce a boolean.
				 */
				switch (cmptype)
				{
					case COMPARE_LT:
						cmp_flag = SLJIT_SIG_LESS; break;
					case COMPARE_LE:
						cmp_flag = SLJIT_SIG_LESS_EQUAL; break;
					case COMPARE_GE:
						cmp_flag = SLJIT_SIG_GREATER_EQUAL; break;
					case COMPARE_GT:
						cmp_flag = SLJIT_SIG_GREATER; break;
					default:
						cmp_flag = SLJIT_SIG_LESS; break;
				}

				/* R0 = (int32) *op->resvalue */
				emit_load_resvalue_addr(C, state, opno, op, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* Compare R0 against 0 → set result bool */
				{
					struct sljit_jump *j_true;

					/* Default: false */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_IMM, 0);

					j_true = sljit_emit_cmp(C, cmp_flag,
											SLJIT_R0, 0, SLJIT_IMM, 0);

					/* false path — skip over true assignment */
					{
						struct sljit_jump *j_done =
							sljit_emit_jump(C, SLJIT_JUMP);

						/* true path */
						sljit_set_label(j_true, sljit_emit_label(C));
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_IMM, 1);
						sljit_set_label(j_done, sljit_emit_label(C));
					}
				}

				/* *op->resvalue = BoolGetDatum(result) — R1 still has resvalue addr */
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R2, 0);
				/* *op->resnull = false */
				emit_store_resnull_false(C, state, opno, op);
				break;
			}

			/*
			 * ---- RETURNINGEXPR ----
			 * If state->flags & nullflag: set NULL result, jump to jumpdone.
			 * Otherwise continue.
			 */
#ifdef HAVE_EEOP_RETURNINGEXPR
			case EEOP_RETURNINGEXPR:
			{
				struct sljit_jump *j_continue;

				/* R0 = state->flags */
				sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S0),
							   offsetof(ExprState, flags));
				/* Test against nullflag */
				sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
								SLJIT_R0, 0,
								SLJIT_IMM, op->d.returningexpr.nullflag);

				/* If zero (flag not set), continue to next step */
				j_continue = sljit_emit_jump(C, SLJIT_ZERO);

				/* Flag is set: *resvalue = 0, *resnull = true, jump */
				emit_store_resvalue_imm(C, state, opno, op, 0);
				emit_store_resnull_true(C, state, opno, op);

				{
					struct sljit_jump *j_done =
						sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_done;
					pending_jumps[npending].target =
						op->d.returningexpr.jumpdone;
					npending++;
				}

				/* Continue label */
				sljit_set_label(j_continue, sljit_emit_label(C));
				break;
			}
#endif /* HAVE_EEOP_RETURNINGEXPR */

			/*
			 * ---- AGG_STRICT_DESERIALIZE / AGG_DESERIALIZE ----
			 * STRICT: check fcinfo->args[0].isnull → jump to jumpnull.
			 * Both: call FunctionCallInvoke, store result.
			 */
			case EEOP_AGG_STRICT_DESERIALIZE:
			case EEOP_AGG_DESERIALIZE:
			{
				FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;

				if (opcode == EEOP_AGG_STRICT_DESERIALIZE)
				{
					/* Check args[0].isnull */
					sljit_sw null_off =
						(sljit_sw) &fcinfo->args[0].isnull -
						(sljit_sw) fcinfo;
					emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_deserialize.fcinfo_data), SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), null_off);
					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target =
						op->d.agg_deserialize.jumpnull;
					npending++;
				}

				/* fcinfo->isnull = false */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_deserialize.fcinfo_data), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);
				/* R0 = fn_addr(fcinfo) */
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), fcinfo->flinfo->fn_addr);

				/* *op->resvalue = R0 */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);
				/* *op->resnull = fcinfo->isnull */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.agg_deserialize.fcinfo_data), SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   offsetof(FunctionCallInfoBaseData, isnull));
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- PARAM_EXEC / PARAM_EXTERN ----
			 * Direct call to ExecEvalParamExec/Extern instead of fallback.
			 */
			case EEOP_PARAM_EXEC:
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), ExecEvalParamExec);
				break;
			}

			case EEOP_PARAM_EXTERN:
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), ExecEvalParamExtern);
				break;
			}

			/*
			 * ---- DISTINCT / NOT_DISTINCT ----
			 * 1. If null flags differ → result is true (DISTINCT) / false (NOT_DISTINCT)
			 * 2. If both null → result is false (DISTINCT) / true (NOT_DISTINCT)
			 * 3. If neither null → call equality fn, invert for DISTINCT
			 */
			case EEOP_DISTINCT:
			case EEOP_NOT_DISTINCT:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				struct sljit_jump *j_one_null, *j_both_null, *j_done1, *j_done2;
				sljit_sw null0_off = (sljit_sw) &fcinfo->args[0].isnull -
									 (sljit_sw) fcinfo;
				sljit_sw null1_off = (sljit_sw) &fcinfo->args[1].isnull -
									 (sljit_sw) fcinfo;

				/* R2 = fcinfo base */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);

				/* R0 = args[0].isnull, R1 = args[1].isnull */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R2), null0_off);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R2), null1_off);

				/* If null flags differ (R0 != R1) → one_null path */
				j_one_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R0, 0, SLJIT_R1, 0);

				/* Null flags are equal. If both null (R0 != 0) → both_null */
				j_both_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											 SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Neither null → call equality function */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R2),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_R2, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.func.fn_addr);

				/* For DISTINCT: invert the equality result */
				if (opcode == EEOP_DISTINCT)
				{
					sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
									SLJIT_R0, 0, SLJIT_IMM, 0);
					sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0,
										SLJIT_EQUAL);
				}
				/* *op->resvalue = R0, *op->resnull = false */
				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				j_done1 = sljit_emit_jump(C, SLJIT_JUMP);

				/* one_null: nulls differ → DISTINCT=true, NOT_DISTINCT=false */
				sljit_set_label(j_one_null, sljit_emit_label(C));
				emit_store_res_pair_false_imm(C, state, opno, op,
							   (opcode == EEOP_DISTINCT) ? 1 : 0);
				j_done2 = sljit_emit_jump(C, SLJIT_JUMP);

				/* both_null: both nulls → DISTINCT=false, NOT_DISTINCT=true */
				sljit_set_label(j_both_null, sljit_emit_label(C));
				emit_store_res_pair_false_imm(C, state, opno, op,
							   (opcode == EEOP_DISTINCT) ? 0 : 1);

				/* All paths converge */
				{
					struct sljit_label *lbl_end = sljit_emit_label(C);
					sljit_set_label(j_done1, lbl_end);
					sljit_set_label(j_done2, lbl_end);
				}
				break;
			}

			/*
			 * ---- NULLIF ----
			 * NULLIF(a,b): if a is null → return null.
			 * If b is not null → call equality fn.
			 *   If equal → return null.
			 *   Else → return a's value.
			 * If b is null → return a's value.
			 */
			case EEOP_NULLIF:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				struct sljit_jump *j_a_null, *j_b_null, *j_not_equal;
				struct sljit_jump *j_done1, *j_done2, *j_done3;
				sljit_sw null0_off = (sljit_sw) &fcinfo->args[0].isnull -
									 (sljit_sw) fcinfo;
				sljit_sw null1_off = (sljit_sw) &fcinfo->args[1].isnull -
									 (sljit_sw) fcinfo;
				sljit_sw val0_off = (sljit_sw) &fcinfo->args[0].value -
									(sljit_sw) fcinfo;

				/* R2 = fcinfo base */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);

				/* Check if arg0 is null */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R2), null0_off);
				j_a_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Check if arg1 is null → skip to return_a */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R2), null1_off);
				j_b_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Both non-null: call equality function */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R2),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_R2, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P), op->d.func.fn_addr);

				/* If !fcinfo->isnull && result is true → return null */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R2),
							   offsetof(FunctionCallInfoBaseData, isnull));
				/* If fcinfo->isnull, treat as not equal */
				j_not_equal = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											 SLJIT_R1, 0, SLJIT_IMM, 0);
				/* If result == false (0), not equal */
				{
					struct sljit_jump *j_false;
					j_false = sljit_emit_cmp(C, SLJIT_EQUAL,
											 SLJIT_R0, 0, SLJIT_IMM, 0);

					/* Equal: return null */
					emit_store_resnull_true(C, state, opno, op);
					j_done1 = sljit_emit_jump(C, SLJIT_JUMP);

					sljit_set_label(j_false, sljit_emit_label(C));
				}

				/* not_equal label: fall through to return_a */
				sljit_set_label(j_not_equal, sljit_emit_label(C));

				/* return_a: *op->resvalue = args[0].value, *op->resnull = false */
				sljit_set_label(j_b_null, sljit_emit_label(C));
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.func.fcinfo_data), SLJIT_R2);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R2), val0_off);
				emit_store_res_pair_false(C, state, opno, op, SLJIT_R0);
				j_done2 = sljit_emit_jump(C, SLJIT_JUMP);

				/* a_null: *op->resnull = true */
				sljit_set_label(j_a_null, sljit_emit_label(C));
				emit_store_resnull_true(C, state, opno, op);
				j_done3 = sljit_emit_jump(C, SLJIT_JUMP);

				/* All paths converge */
				{
					struct sljit_label *lbl_end = sljit_emit_label(C);
					sljit_set_label(j_done1, lbl_end);
					sljit_set_label(j_done2, lbl_end);
					sljit_set_label(j_done3, lbl_end);
				}
				break;
			}

			/*
			 * ---- BOOLTEST_IS_TRUE / IS_NOT_TRUE / IS_FALSE / IS_NOT_FALSE ----
			 */
			case EEOP_BOOLTEST_IS_TRUE:
			{
				/* If null → resvalue=false, resnull=false. Else keep as-is. */
				struct sljit_jump *j_not_null;

				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				j_not_null = sljit_emit_cmp(C, SLJIT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Null path: *resvalue = false, *resnull = false */
				emit_store_res_pair_false_imm(C, state, opno, op, 0);

				sljit_set_label(j_not_null, sljit_emit_label(C));
				break;
			}

			case EEOP_BOOLTEST_IS_NOT_TRUE:
			{
				/* If null → resvalue=true, resnull=false. Else invert value. */
				struct sljit_jump *j_not_null, *j_done;

				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				j_not_null = sljit_emit_cmp(C, SLJIT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Null path: *resvalue = true, *resnull = false */
				emit_store_res_pair_false_imm(C, state, opno, op, 1);
				j_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* Not null: *resvalue = !*resvalue */
				sljit_set_label(j_not_null, sljit_emit_label(C));
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);
				sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
								SLJIT_R0, 0, SLJIT_IMM, 0);
				sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0,
									SLJIT_EQUAL);
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				sljit_set_label(j_done, sljit_emit_label(C));
				break;
			}

			case EEOP_BOOLTEST_IS_FALSE:
			{
				/* If null → resvalue=false, resnull=false. Else invert value. */
				struct sljit_jump *j_not_null, *j_done;

				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				j_not_null = sljit_emit_cmp(C, SLJIT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Null path: *resvalue = false, *resnull = false */
				emit_store_res_pair_false_imm(C, state, opno, op, 0);
				j_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* Not null: *resvalue = !*resvalue */
				sljit_set_label(j_not_null, sljit_emit_label(C));
				emit_load_resvalue(C, state, opno, op, SLJIT_R0);
				sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
								SLJIT_R0, 0, SLJIT_IMM, 0);
				sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0,
									SLJIT_EQUAL);
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				sljit_set_label(j_done, sljit_emit_label(C));
				break;
			}

			case EEOP_BOOLTEST_IS_NOT_FALSE:
			{
				/* If null → resvalue=true, resnull=false. Else keep as-is. */
				struct sljit_jump *j_not_null;

				emit_load_resnull(C, state, opno, op, SLJIT_R0);
				j_not_null = sljit_emit_cmp(C, SLJIT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Null path: *resvalue = true, *resnull = false */
				emit_store_res_pair_false_imm(C, state, opno, op, 1);

				sljit_set_label(j_not_null, sljit_emit_label(C));
				break;
			}

			/*
			 * ---- MAKE_READONLY ----
			 * If not null, call MakeExpandedObjectReadOnlyInternal.
			 */
			case EEOP_MAKE_READONLY:
			{
				struct sljit_jump *j_null;

				/* R0 = *op->d.make_readonly.isnull */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.make_readonly.isnull), SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* *op->resnull = isnull */
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);

				/* If null, skip */
				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: R0 = MakeExpandedObjectReadOnlyInternal(*value) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.make_readonly.value), SLJIT_R0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0), 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, W), MakeExpandedObjectReadOnlyInternal);

				/* *op->resvalue = R0 */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				sljit_set_label(j_null, sljit_emit_label(C));
				break;
			}

			/*
			 * ---- AGGREF ----
			 * Load from econtext->ecxt_aggvalues/ecxt_aggnulls.
			 */
			case EEOP_AGGREF:
			{
				int aggno = op->d.aggref.aggno;

				/* R0 = econtext->ecxt_aggvalues */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, ecxt_aggvalues));
				/* R0 = aggvalues[aggno] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   aggno * (sljit_sw) sizeof(Datum));
				/* *op->resvalue = R0 */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* R0 = econtext->ecxt_aggnulls */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, ecxt_aggnulls));
				/* R0 = aggnulls[aggno] */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   aggno * (sljit_sw) sizeof(bool));
				/* *op->resnull = R0 */
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- WINDOW_FUNC ----
			 * Load from econtext->ecxt_aggvalues/ecxt_aggnulls[wfuncno].
			 *
			 * IMPORTANT: wfuncno must be read at RUNTIME, not compile time,
			 * because ExecInitWindowAgg assigns wfuncno AFTER the projection
			 * expressions are compiled (ExecAssignProjectionInfo comes first).
			 */
			case EEOP_WINDOW_FUNC:
			{
				WindowFuncExprState *wfunc = op->d.window_func.wfstate;

				/* R2 = wfunc->wfuncno (read at runtime, could be int32) */
				emit_load_step_field(C, opno, offsetof(ExprEvalStep, d.window_func.wfstate), SLJIT_R2);
				sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_R2),
							   offsetof(WindowFuncExprState, wfuncno));

				/* R0 = econtext->ecxt_aggvalues */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, ecxt_aggvalues));
				/* R0 = aggvalues[wfuncno] (R2 * sizeof(Datum) + base) */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R3, 0,
							   SLJIT_R2, 0,
							   SLJIT_IMM, 3);  /* * 8 for sizeof(Datum) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_R0, SLJIT_R3), 0);
				/* *op->resvalue = R0 */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* R0 = econtext->ecxt_aggnulls */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S1),
							   offsetof(ExprContext, ecxt_aggnulls));
				/* R0 = aggnulls[wfuncno] (R2 still has wfuncno) */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_R0, SLJIT_R2), 0);
				/* *op->resnull = R0 */
				emit_store_resnull_reg(C, state, opno, op, SLJIT_R0);
				break;
			}

			/*
			 * ---- HASHED_SCALARARRAYOP ----
			 *
			 * For constant byval IN lists: extract values at compile
			 * time, sort them, emit an inline binary search tree.
			 * ~5 comparisons for 20 elements vs 25+ instructions for
			 * Bob Jenkins hash + probe.
			 *
			 * Falls back to C function for byref types or non-constant
			 * arrays.
			 */
			case EEOP_HASHED_SCALARARRAYOP:
			{
#if PG_VERSION_NUM >= 150000
				FunctionCallInfo fcinfo =
					op->d.hashedscalararrayop.fcinfo_data;
				bool inclause = op->d.hashedscalararrayop.inclause;
				ScalarArrayOpExpr *saop =
					op->d.hashedscalararrayop.saop;

				/*
				 * Detect byval type at compile time.
				 */
				const JitDirectFn *eq_dfn =
					jit_find_direct_fn(
						op->d.hashedscalararrayop.finfo->fn_addr);

				/*
				 * Try to extract constant array values at compile time
				 * for inline binary search.
				 */
				Datum *sorted_vals = NULL;
				int nvals = 0;
				bool array_has_nulls = false;

				if (eq_dfn && eq_dfn->jit_fn)
				{
					/*
					 * Check if the array argument is a Const node.
					 * saop->args = list of (scalar, array).
					 */
					Expr *arrayarg = (Expr *) lsecond(saop->args);

					if (IsA(arrayarg, Const))
					{
						Const *arrayconst = (Const *) arrayarg;

						if (!arrayconst->constisnull)
						{
							ArrayType *arr = DatumGetArrayTypeP(
								arrayconst->constvalue);
							int16 typlen;
							bool typbyval;
							char typalign;
							int nitems;

							nitems = ArrayGetNItems(ARR_NDIM(arr),
													 ARR_DIMS(arr));
							get_typlenbyvalalign(ARR_ELEMTYPE(arr),
												 &typlen, &typbyval,
												 &typalign);

							if (typbyval && nitems > 0 && nitems <= 64)
							{
								/*
								 * Extract all values. Check for NULLs.
								 */
								bits8 *bitmap = ARR_NULLBITMAP(arr);
								char *s = (char *) ARR_DATA_PTR(arr);
								int bitmask = 1;

								sorted_vals = (Datum *) palloc(
									nitems * sizeof(Datum));
								nvals = 0;

								for (int k = 0; k < nitems; k++)
								{
									if (bitmap &&
										(*bitmap & bitmask) == 0)
									{
										array_has_nulls = true;
									}
									else
									{
										Datum d = fetch_att(s, true,
															typlen);
										sorted_vals[nvals++] = d;
										s = att_addlength_pointer(s,
											typlen, s);
										s = (char *) att_align_nominal(
											s, typalign);
									}

									if (bitmap)
									{
										bitmask <<= 1;
										if (bitmask == 0x100)
										{
											bitmap++;
											bitmask = 1;
										}
									}
								}

								/*
								 * Sort values for binary search.
								 * Simple insertion sort — at most 64
								 * elements at compile time.
								 */
								for (int a = 1; a < nvals; a++)
								{
									Datum tmp = sorted_vals[a];
									int b = a - 1;
									while (b >= 0 &&
										   (int64) sorted_vals[b] >
										   (int64) tmp)
									{
										sorted_vals[b + 1] =
											sorted_vals[b];
										b--;
									}
									sorted_vals[b + 1] = tmp;
								}
							}
						}
					}
				}

				if (sorted_vals && nvals > 0)
				{
					/*
					 * ---- Binary search path ----
					 *
					 * Emit a balanced binary search tree as inline
					 * CMP + conditional branch instructions.
					 *
					 * For 20 elements: ~5 levels = 5 CMP+branch pairs.
					 * Total: ~10-15 instructions vs 30+ for hash probe.
					 *
					 * Strategy: recursive structure emitted iteratively
					 * using a work stack. At each node, compare scalar
					 * against the median value:
					 *   - equal → found
					 *   - less → go left subtree
					 *   - greater → go right subtree
					 *   - leaf with no match → not found
					 */
					struct sljit_jump *j_scalar_null;
					struct sljit_jump *j_done_found;
					struct sljit_jump *j_done_notfound;
					struct sljit_jump *j_done_null;
					struct sljit_label *lbl_found;
					struct sljit_label *lbl_not_found;
					struct sljit_label *lbl_null_result;
					struct sljit_label *lbl_done;

					sljit_sw off_arg0_value =
						(sljit_sw) &fcinfo->args[0].value -
						(sljit_sw) fcinfo;
					sljit_sw off_arg0_isnull =
						(sljit_sw) &fcinfo->args[0].isnull -
						(sljit_sw) fcinfo;

					/*
					 * Step 1: Check scalar not NULL (strict function).
					 * R0 = fcinfo (loaded from steps array for PIC)
					 */
					emit_load_step_field(C, opno,
						offsetof(ExprEvalStep, d.hashedscalararrayop.fcinfo_data),
						SLJIT_R0);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   off_arg0_isnull);
					j_scalar_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
												   SLJIT_R1, 0,
												   SLJIT_IMM, 0);

					/*
					 * Step 2: Load scalar value into R0.
					 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0),
								   off_arg0_value);

					/*
					 * Step 3: Emit binary search tree.
					 *
					 * We use a work stack to emit the tree iteratively.
					 * Each work item is (lo, hi) range into sorted_vals.
					 * For each range, pick median, emit CMP against it,
					 * then recurse into left and right halves.
					 *
					 * R0 = scalar value (preserved across all compares)
					 *
					 * We collect jump targets for "found" and
					 * "not found" to patch at the end.
					 */
					{
						/* Max depth for 64 elements = 7 levels.
						 * Work stack needs at most 2^7 = 128 entries,
						 * but binary search never stacks more than
						 * 2*depth entries. Use generous size. */
						struct {
							int lo, hi;
							struct sljit_jump *entry_jump;
						} work[128];
						int work_top = 0;

						/* Jumps that need to go to "found" label */
						struct sljit_jump *found_jumps[64];
						int n_found = 0;

						/* Jumps that need to go to "not found" label */
						struct sljit_jump *notfound_jumps[128];
						int n_notfound = 0;

						/* Push initial range */
						work[work_top].lo = 0;
						work[work_top].hi = nvals - 1;
						work[work_top].entry_jump = NULL;
						work_top++;

						while (work_top > 0)
						{
							int lo, hi, mid;
							struct sljit_jump *j_lt, *j_eq;
							struct sljit_label *lbl_node;

							work_top--;
							lo = work[work_top].lo;
							hi = work[work_top].hi;

							/* Emit label for this node */
							lbl_node = sljit_emit_label(C);
							if (work[work_top].entry_jump)
								sljit_set_label(
									work[work_top].entry_jump,
									lbl_node);

							if (lo > hi)
							{
								/*
								 * Empty range → not found.
								 * Jump to not_found label.
								 */
								notfound_jumps[n_notfound++] =
									sljit_emit_jump(C, SLJIT_JUMP);
								continue;
							}

							if (lo == hi)
							{
								/*
								 * Single element — just compare.
								 */
								j_eq = sljit_emit_cmp(C, SLJIT_EQUAL,
									SLJIT_R0, 0,
									SLJIT_IMM,
									(sljit_sw) sorted_vals[lo]);
								found_jumps[n_found++] = j_eq;
								/* Not equal → not found */
								notfound_jumps[n_notfound++] =
									sljit_emit_jump(C, SLJIT_JUMP);
								continue;
							}

							/*
							 * Pick median element.
							 */
							mid = lo + (hi - lo) / 2;

							/*
							 * CMP R0, sorted_vals[mid]
							 * BEQ → found
							 * BLT → left subtree [lo, mid-1]
							 * fall-through → right subtree [mid+1, hi]
							 */
							j_eq = sljit_emit_cmp(C, SLJIT_EQUAL,
								SLJIT_R0, 0,
								SLJIT_IMM,
								(sljit_sw) sorted_vals[mid]);
							found_jumps[n_found++] = j_eq;

							j_lt = sljit_emit_cmp(C,
								SLJIT_SIG_LESS,
								SLJIT_R0, 0,
								SLJIT_IMM,
								(sljit_sw) sorted_vals[mid]);

							/*
							 * Push left subtree first (processed
							 * last = emitted later = j_lt target),
							 * then right subtree (processed next =
							 * emitted immediately = fall-through).
							 */
							/* Left subtree [lo, mid-1]: j_lt target */
							work[work_top].lo = lo;
							work[work_top].hi = mid - 1;
							work[work_top].entry_jump = j_lt;
							work_top++;

							/* Right subtree [mid+1, hi]: falls through */
							work[work_top].lo = mid + 1;
							work[work_top].hi = hi;
							work[work_top].entry_jump = NULL;
							work_top++;
						}

						/*
						 * ---- Found path ----
						 */
						lbl_found = sljit_emit_label(C);
						for (int k = 0; k < n_found; k++)
							sljit_set_label(found_jumps[k], lbl_found);

						if (inclause)
							emit_store_resvalue_imm(C, state, opno, op, 1);
						else
							emit_store_resvalue_imm(C, state, opno, op, 0);
						emit_store_resnull_false(C, state, opno, op);
						j_done_found =
							sljit_emit_jump(C, SLJIT_JUMP);

						/*
						 * ---- Not found path ----
						 */
						lbl_not_found = sljit_emit_label(C);
						for (int k = 0; k < n_notfound; k++)
							sljit_set_label(notfound_jumps[k],
											lbl_not_found);

						if (array_has_nulls)
						{
							/*
							 * Array had NULLs — result is NULL
							 * (for strict equality, not finding a
							 * match with NULLs present means
							 * indeterminate).
							 */
							if (inclause)
							{
								emit_store_resvalue_imm(C, state, opno, op, 0);
								emit_store_resnull_true(C, state, opno, op);
							}
							else
							{
								emit_store_resvalue_imm(C, state, opno, op, 0);
								emit_store_resnull_true(C, state, opno, op);
							}
						}
						else
						{
							if (inclause)
								emit_store_resvalue_imm(C, state, opno, op, 0);
							else
								emit_store_resvalue_imm(C, state, opno, op, 1);
							emit_store_resnull_false(C, state, opno, op);
						}
						j_done_notfound =
							sljit_emit_jump(C, SLJIT_JUMP);
					}

					/*
					 * ---- Null scalar path ----
					 */
					lbl_null_result = sljit_emit_label(C);
					sljit_set_label(j_scalar_null, lbl_null_result);

					emit_store_resvalue_imm(C, state, opno, op, 0);
					emit_store_resnull_true(C, state, opno, op);
					j_done_null = sljit_emit_jump(C, SLJIT_JUMP);

					/* ---- Done ---- */
					lbl_done = sljit_emit_label(C);
					sljit_set_label(j_done_found, lbl_done);
					sljit_set_label(j_done_notfound, lbl_done);
					sljit_set_label(j_done_null, lbl_done);

					pfree(sorted_vals);
				}
				else
				{
					/*
					 * Non-constant array or byref type — fall back to
					 * C function call.
					 */
					if (sorted_vals)
						pfree(sorted_vals);

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_S0, 0);
					emit_load_step_addr(C, opno, SLJIT_R1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_S1, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), ExecEvalHashedScalarArrayOp);
				}
#else /* PG14: no inclause/saop — always use fallback */
				{
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_S0, 0);
					emit_load_step_addr(C, opno, SLJIT_R1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_S1, 0);
					EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), ExecEvalHashedScalarArrayOp);
				}
#endif /* PG_VERSION_NUM >= 150000 */
				break;
			}

			/*
			 * ---- DIRECT-CALL OPCODES (Phase 1) ----
			 * These opcodes have PG-exported functions with matching signatures.
			 * Instead of going through fallback_step's 120-case switch, we call
			 * the function directly. Same semantics, just skip the dispatch.
			 */

			/* 3-arg: fn(ExprState *state, ExprEvalStep *op, ExprContext *econtext) */
			case EEOP_FUNCEXPR_FUSAGE:
			case EEOP_FUNCEXPR_STRICT_FUSAGE:
			case EEOP_NULLTEST_ROWISNULL:
			case EEOP_NULLTEST_ROWISNOTNULL:
#ifdef HAVE_EEOP_PARAM_SET
			case EEOP_PARAM_SET:
#endif
			case EEOP_ARRAYCOERCE:
			case EEOP_FIELDSELECT:
			case EEOP_FIELDSTORE_DEFORM:
			case EEOP_FIELDSTORE_FORM:
			case EEOP_CONVERT_ROWTYPE:
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
			case EEOP_JSON_CONSTRUCTOR:
#endif
#ifdef HAVE_EEOP_JSONEXPR
			case EEOP_JSONEXPR_COERCION:
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
			case EEOP_MERGE_SUPPORT_FUNC:
#endif
			case EEOP_SUBPLAN:
			case EEOP_WHOLEROW:
			case EEOP_AGG_ORDERED_TRANS_DATUM:
			case EEOP_AGG_ORDERED_TRANS_TUPLE:
			{
				void *fn;

				switch (opcode)
				{
					case EEOP_FUNCEXPR_FUSAGE:
						fn = ExecEvalFuncExprFusage; break;
					case EEOP_FUNCEXPR_STRICT_FUSAGE:
						fn = ExecEvalFuncExprStrictFusage; break;
					case EEOP_NULLTEST_ROWISNULL:
						fn = ExecEvalRowNull; break;
					case EEOP_NULLTEST_ROWISNOTNULL:
						fn = ExecEvalRowNotNull; break;

#ifdef HAVE_EEOP_PARAM_SET
					case EEOP_PARAM_SET:
						fn = ExecEvalParamSet; break;
#endif
					case EEOP_ARRAYCOERCE:
						fn = ExecEvalArrayCoerce; break;
					case EEOP_FIELDSELECT:
						fn = ExecEvalFieldSelect; break;
					case EEOP_FIELDSTORE_DEFORM:
						fn = ExecEvalFieldStoreDeForm; break;
					case EEOP_FIELDSTORE_FORM:
						fn = ExecEvalFieldStoreForm; break;
					case EEOP_CONVERT_ROWTYPE:
						fn = ExecEvalConvertRowtype; break;
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
					case EEOP_JSON_CONSTRUCTOR:
						fn = ExecEvalJsonConstructor; break;
#endif
#ifdef HAVE_EEOP_JSONEXPR
					case EEOP_JSONEXPR_COERCION:
						fn = ExecEvalJsonCoercion; break;
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
					case EEOP_MERGE_SUPPORT_FUNC:
						fn = ExecEvalMergeSupportFunc; break;
#endif
					case EEOP_SUBPLAN:
						fn = ExecEvalSubPlan; break;
					case EEOP_WHOLEROW:
						fn = ExecEvalWholeRowVar; break;
					case EEOP_AGG_ORDERED_TRANS_DATUM:
						fn = ExecEvalAggOrderedTransDatum; break;
					case EEOP_AGG_ORDERED_TRANS_TUPLE:
						fn = ExecEvalAggOrderedTransTuple; break;
					default:
						pg_unreachable();
				}
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), fn);
				break;
			}

			/* 2-arg: fn(ExprState *state, ExprEvalStep *op) */
#ifdef HAVE_EEOP_IOCOERCE_SAFE
			case EEOP_IOCOERCE_SAFE:
#endif
			case EEOP_SCALARARRAYOP:
			case EEOP_SQLVALUEFUNCTION:
			case EEOP_CURRENTOFEXPR:
			case EEOP_NEXTVALUEEXPR:
			case EEOP_ARRAYEXPR:
			case EEOP_ROW:
			case EEOP_MINMAX:
			case EEOP_DOMAIN_NOTNULL:
			case EEOP_DOMAIN_CHECK:
			case EEOP_XMLEXPR:
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
			case EEOP_IS_JSON:
#endif
#ifdef HAVE_EEOP_JSONEXPR
			case EEOP_JSONEXPR_COERCION_FINISH:
#endif
			case EEOP_GROUPING_FUNC:
			{
				void *fn;

				switch (opcode)
				{

#ifdef HAVE_EEOP_IOCOERCE_SAFE
					case EEOP_IOCOERCE_SAFE:
						fn = ExecEvalCoerceViaIOSafe; break;
#endif
					case EEOP_SCALARARRAYOP:
						fn = ExecEvalScalarArrayOp; break;
					case EEOP_SQLVALUEFUNCTION:
						fn = ExecEvalSQLValueFunction; break;
					case EEOP_CURRENTOFEXPR:
						fn = ExecEvalCurrentOfExpr; break;
					case EEOP_NEXTVALUEEXPR:
						fn = ExecEvalNextValueExpr; break;
					case EEOP_ARRAYEXPR:
						fn = ExecEvalArrayExpr; break;
					case EEOP_ROW:
						fn = ExecEvalRow; break;
					case EEOP_MINMAX:
						fn = ExecEvalMinMax; break;
					case EEOP_DOMAIN_NOTNULL:
						fn = ExecEvalConstraintNotNull; break;
					case EEOP_DOMAIN_CHECK:
						fn = ExecEvalConstraintCheck; break;
					case EEOP_XMLEXPR:
						fn = ExecEvalXmlExpr; break;

#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
					case EEOP_IS_JSON:
						fn = ExecEvalJsonIsPredicate; break;
#endif
#ifdef HAVE_EEOP_JSONEXPR
					case EEOP_JSONEXPR_COERCION_FINISH:
						fn = ExecEvalJsonCoercionFinish; break;
#endif
					case EEOP_GROUPING_FUNC:
						fn = ExecEvalGroupingFunc; break;
					default:
						pg_unreachable();
				}
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS2V(P, P), fn);
				break;
			}

			/* PARAM_CALLBACK: indirect call through op->d.cparam.paramfunc */
			case EEOP_PARAM_CALLBACK:
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), op->d.cparam.paramfunc);
				break;
			}

			/*
			 * ---- ROWCOMPARE_STEP ----
			 * Call comparison fn via fcinfo; jump to jumpnull on NULL
			 * result, jumpdone on non-zero result.
			 */
			case EEOP_ROWCOMPARE_STEP:
			{
				FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
				int jnull = op->d.rowcompare_step.jumpnull;
				int jdone = op->d.rowcompare_step.jumpdone;

				/*
				 * If fn_strict and either arg is null → set resnull=true,
				 * jump to jumpnull.
				 */
				if (op->d.rowcompare_step.finfo->fn_strict)
				{
					/* R0 = fcinfo->args[0].isnull */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
						SLJIT_MEM0(),
						(sljit_sw) &fcinfo->args[0].isnull);
					/* R1 = fcinfo->args[1].isnull */
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
						SLJIT_MEM0(),
						(sljit_sw) &fcinfo->args[1].isnull);
					sljit_emit_op2(C, SLJIT_OR, SLJIT_R0, 0,
								   SLJIT_R0, 0, SLJIT_R1, 0);

					struct sljit_jump *j_not_null =
						sljit_emit_cmp(C, SLJIT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);

					/* Null path: *resnull = true, jump to jumpnull */
					emit_store_resnull_true(C, state, opno, op);
					{
						struct sljit_jump *j =
							sljit_emit_jump(C, SLJIT_JUMP);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = jnull;
						npending++;
					}

					sljit_set_label(j_not_null, sljit_emit_label(C));
				}

				/* fcinfo->isnull = false */
				sljit_emit_op1(C, SLJIT_MOV_U8,
					SLJIT_MEM0(),
					(sljit_sw) &fcinfo->isnull,
					SLJIT_IMM, 0);

				/* R0 = fn_addr(fcinfo) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
						   op->d.rowcompare_step.fn_addr);

				/* *op->resvalue = R0 */
				emit_store_resvalue(C, state, opno, op, SLJIT_R0);

				/* If fcinfo->isnull, set resnull=true and jump to jumpnull */
				{
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
						SLJIT_MEM0(),
						(sljit_sw) &fcinfo->isnull);
					struct sljit_jump *j_not_null2 =
						sljit_emit_cmp(C, SLJIT_EQUAL,
									   SLJIT_R1, 0, SLJIT_IMM, 0);

					emit_store_resnull_true(C, state, opno, op);
					{
						struct sljit_jump *j =
							sljit_emit_jump(C, SLJIT_JUMP);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = jnull;
						npending++;
					}

					sljit_set_label(j_not_null2, sljit_emit_label(C));
				}

				/* *resnull = false */
				emit_store_resnull_false(C, state, opno, op);

				/* If DatumGetInt32(d) != 0, jump to jumpdone */
				{
					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = jdone;
					npending++;
				}
				break;
			}

			/*
			 * ---- SBSREF_SUBSCRIPTS ----
			 * Call subscriptfunc(state, op, econtext) -> bool.
			 * If false (null subscript), jump to jumpdone.
			 */
			case EEOP_SBSREF_SUBSCRIPTS:
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3(W, P, P, P),
						   op->d.sbsref_subscript.subscriptfunc);

				/* If R0 == 0 (false), jump to jumpdone */
				{
					int jumpdone = op->d.sbsref_subscript.jumpdone;
					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = jumpdone;
					npending++;
				}
				break;
			}

			/*
			 * ---- SBSREF_OLD / SBSREF_ASSIGN / SBSREF_FETCH ----
			 * Call subscriptfunc(state, op, econtext) -> void.
			 */
			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, P),
						   op->d.sbsref.subscriptfunc);
				break;
			}

#ifdef HAVE_EEOP_JSONEXPR
			/*
			 * ---- JSONEXPR_PATH ----
			 * Call ExecEvalJsonExprPath(state, op, econtext) -> int.
			 * Return value is the step number to jump to.
			 */
			case EEOP_JSONEXPR_PATH:
			{
				JsonExprState *jsestate = op->d.jsonexpr.jsestate;
				int targets[4];
				int ntargets = 0;

				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3(W, P, P, P),
						   ExecEvalJsonExprPath);

				/* Collect unique valid targets */
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

				/* Compare R0 against each possible target */
				for (int t = 0; t < ntargets; t++)
				{
					if (targets[t] >= 0 && targets[t] < steps_len)
					{
						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_EQUAL,
										   SLJIT_R0, 0,
										   SLJIT_IMM, targets[t]);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = targets[t];
						npending++;
					}
				}
				break;
			}
#endif /* HAVE_EEOP_JSONEXPR */

			/*
			 * ---- DEFAULT: fallback to C function ----
			 * Handles all remaining opcodes via pg_jitter_fallback_step.
			 */
			default:
			{
				int fb_jump_target = -1;

				/* Call pg_jitter_fallback_step(state, op, econtext) -> int64 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				emit_load_step_addr(C, opno, SLJIT_R1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				EMIT_ICALL(C, SLJIT_CALL, SLJIT_ARGS3(W, P, P, P), pg_jitter_fallback_step);

				/*
				 * Check if this opcode could jump. We know the jump target
				 * at code-gen time from the op struct.
				 */
				switch (opcode)
				{
					case EEOP_AGG_STRICT_DESERIALIZE:
						fb_jump_target = op->d.agg_deserialize.jumpnull;
						break;
					case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
#ifdef HAVE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1
					case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
#endif
					case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
						fb_jump_target = op->d.agg_strict_input_check.jumpnull;
						break;
					case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
						fb_jump_target = op->d.agg_plain_pergroup_nullcheck.jumpnull;
						break;
#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
					case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
					case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
						fb_jump_target = op->d.agg_presorted_distinctcheck.jumpdistinct;
						break;
#endif
#ifdef HAVE_EEOP_RETURNINGEXPR
					case EEOP_RETURNINGEXPR:
						fb_jump_target = op->d.returningexpr.jumpdone;
						break;
#endif
					default:
						break;
				}

				if (fb_jump_target >= 0 && fb_jump_target < steps_len)
				{
					/* R0 >= 0 means jump to target step (signed compare!) */
					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = fb_jump_target;
					npending++;
				}
				break;
			}
		}
	}

	/*
	 * Fix up all pending jumps.
	 */
	for (int j = 0; j < npending; j++)
	{
		int		target = pending_jumps[j].target;

		if (target >= 0 && target < steps_len)
			sljit_set_label(pending_jumps[j].jump, step_labels[target]);
		/* target == -2 means already resolved (null check jumps) */
	}

	/*
	 * Generate native code (emission).
	 */
	{
		instr_time	emit_start, emit_end;
		void	   *code;

		INSTR_TIME_SET_CURRENT(emit_start);
		code = sljit_generate_code(C, 0, NULL);

		if (!code)
		{
			sljit_free_compiler(C);
			pfree(step_labels);
			pfree(pending_jumps);
			sljit_shared_code_mode = false;
			return false;
		}

		INSTR_TIME_SET_CURRENT(emit_end);
		INSTR_TIME_ACCUM_DIFF(ctx->base.instr.emission_counter,
							  emit_end, emit_start);

#ifdef PG_JITTER_HAVE_INLINE_BLOBS
		/* Patch BL/CALL relocations in pre-compiled blobs.
		 * Uses sljit label addresses for reliable blob positioning,
		 * with W^X toggling on macOS ARM64. */
		if (n_precompiled_relocs > 0)
			fixup_precompiled_relocs(code,
									sljit_get_generated_code_size(C),
									precompiled_relocs,
									n_precompiled_relocs);
#endif

		/*
		 * Leader: store compiled code directly in DSM.
		 * The DSM was created during the first compile_expr call above.
		 */
		if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED &&
			!IsParallelWorker() &&
			(state->parent->state->es_jit_flags & PGJIT_EXPR) &&
			ctx->share_state.sjc)
		{
			Size	gen_code_size = sljit_get_generated_code_size(C);

			elog(DEBUG1, "pg_jitter: leader storing code "
				 "node=%d expr=%d (%zu bytes) at %p fallback=%p",
				 shared_node_id, shared_expr_idx,
				 gen_code_size, code, (void *) pg_jitter_fallback_step);

			pg_jitter_store_shared_code(ctx->share_state.sjc,
										code, gen_code_size,
										shared_node_id, shared_expr_idx,
										(uint64)(uintptr_t) pg_jitter_fallback_step);
		}

		/* Register for cleanup */
		pg_jitter_register_compiled(ctx, sljit_code_free, code);

		/* Set the eval function (with validation wrapper on first call) */
		pg_jitter_install_expr(state, (ExprStateEvalFunc) code);

		sljit_free_compiler(C);
	}

	pfree(step_labels);
	pfree(pending_jumps);

	/* Reset shared code mode for next compilation */
	sljit_shared_code_mode = false;

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(ctx->base.instr.generation_counter,
						  endtime, starttime);
	ctx->base.instr.created_functions++;

	return true;
}
