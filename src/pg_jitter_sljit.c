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
#include "utils/fmgrprotos.h"
#include "sljitLir.h"

#include "access/htup_details.h"
#include "access/tupdesc_details.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_jitter_sljit",
);

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
}

/*
 * Overflow error for inlined int8inc (COUNT).
 * Called from JIT code when int64 addition overflows.
 */
static void
pg_jitter_int8_overflow(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("bigint out of range")));
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
 *   [SP + 0]   = state->resvalue pointer (Datum*)
 *   [SP + 8]   = state->resnull pointer (bool*)
 *   [SP + 16]  = resultslot pointer (TupleTableSlot*)
 *   [SP + 24]  = resultslot->tts_values (Datum*)
 *   [SP + 32]  = resultslot->tts_isnull (bool*)
 *
 * Slot value/null pointers are loaded from the slot on demand since
 * FETCHSOME may cause reallocation of slot arrays.
 */

/* Stack offsets for cached pointers */
#define SOFF_RESVALUEP    0
#define SOFF_RESNULLP     8
#define SOFF_RESULTSLOT   16
#define SOFF_RESULTVALS   24
#define SOFF_RESULTNULLS  32
#define SOFF_AGG_OLDCTX   40	/* saved CurrentMemoryContext across fn_addr */
#define SOFF_AGG_PERGROUP 48	/* runtime pergroup pointer (changes per tuple) */
#define SOFF_AGG_FCINFO   56	/* fcinfo pointer, avoid reloading 64-bit IMM */
#define SOFF_TEMP         64	/* temporary scratch across function calls */
#define SOFF_DEFORM_OFF       72   /* deform: current byte offset */
#define SOFF_DEFORM_HASNULLS  80   /* deform: hasnulls flag */
#define SOFF_DEFORM_MAXATT    88   /* deform: maxatt from tuple */
#define SOFF_DEFORM_SAVE_S3   96   /* deform: saved S3 (tupdata_base clobbers it) */
#define SOFF_DEFORM_SAVE_S4   104  /* deform: saved S4 (t_bits clobbers it) */
#define SOFF_TOTAL            112

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
 * Emit deform code inline into the expression's sljit compiler.
 *
 * This replaces the separate-function approach: instead of compiling a
 * deform function and calling it via indirect pointer, we emit the deform
 * loop directly into the expression's code stream.  Benefits:
 * - No function call overhead (save/restore, branch)
 * - Registers stay live across the deform boundary
 *
 * slot_reg contains the TupleTableSlot pointer (already loaded by caller).
 * After this code, slot_reg still points to the slot.
 * Uses R0-R3 as scratch, and 3 stack slots for deform state.
 * Also uses S3/S4 temporarily for tupdata_base/t_bits — these are only
 * used for aggregates which are set up AFTER FETCHSOME, so they are safe.
 *
 * Returns true if deform code was emitted, false if deform cannot be
 * JIT-compiled (virtual slots, unknown types, etc).
 */
static bool
sljit_emit_deform_inline(struct sljit_compiler *C,
                         TupleDesc desc,
                         const TupleTableSlotOps *ops,
                         int natts,
                         sljit_s32 slot_reg)
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

        if (att->attnullability == ATTNULLABLE_VALID &&
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
     * Register usage within inline deform:
     *   slot_reg (R0 from caller) = slot pointer
     *   S3 = tupdata_base (char *, tuplep + t_hoff)
     *   S4 = t_bits (bits8 *)
     *   R0-R3 = scratch
     *
     * We save slot_reg to SOFF_TEMP and use S3/S4 for the
     * deform-specific values. Since FETCHSOME is always the first
     * opcode, S3/S4 have not been set up for aggregates yet.
     */

    /* Save S3/S4 — they may hold aggstate/CurrentMemoryContext */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S3,
                   SLJIT_S3, 0);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S4,
                   SLJIT_S4, 0);

    /* Save slot pointer to SOFF_TEMP (survives across function calls) */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_TEMP,
                   slot_reg, 0);

    /* R0 = HeapTuple ptr from slot-type-specific offset */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                   SLJIT_MEM1(slot_reg), tuple_off);

    /* R1 = tuplep = heaptuple->t_data (HeapTupleHeader) */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                   SLJIT_MEM1(SLJIT_R0),
                   offsetof(HeapTupleData, t_data));

    /* t_infomask -> R2 (uint16) */
    sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleHeaderData, t_infomask));
    /* hasnulls = infomask & HEAP_HASNULL */
    sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
                   SLJIT_R2, 0, SLJIT_IMM, HEAP_HASNULL);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_HASNULLS,
                   SLJIT_R2, 0);

    /* t_infomask2 -> maxatt = infomask2 & HEAP_NATTS_MASK */
    sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleHeaderData, t_infomask2));
    sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
                   SLJIT_R2, 0, SLJIT_IMM, HEAP_NATTS_MASK);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_MAXATT,
                   SLJIT_R2, 0);

    /* S4 = &tuplep->t_bits[0] */
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
                   SLJIT_R1, 0,
                   SLJIT_IMM, offsetof(HeapTupleHeaderData, t_bits));

    /* t_hoff -> R2 (zero-extended uint8) */
    sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
                   SLJIT_MEM1(SLJIT_R1),
                   offsetof(HeapTupleHeaderData, t_hoff));

    /* S3 = tupdata_base = (char *)tuplep + t_hoff */
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_S3, 0,
                   SLJIT_R1, 0, SLJIT_R2, 0);

    /* Load saved offset from slot->off -> [SP+SOFF_DEFORM_OFF] */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
    sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
                   SLJIT_MEM1(SLJIT_R0), slot_off);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                   SLJIT_R0, 0);

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

        /* call slot_getmissingattrs(slot, maxatt_as_int, natts) */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);  /* slot */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_MAXATT);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                       SLJIT_IMM, natts);
        sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, 32, 32),
                         SLJIT_IMM, (sljit_sw) slot_getmissingattrs);

        sljit_set_label(skip_missing, sljit_emit_label(C));
    }

    /* ============================================================
     * NVALID DISPATCH: comparison chain
     * ============================================================ */
    /* Reload slot from SOFF_TEMP */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
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

    /* ============================================================
     * PER-ATTRIBUTE CODE EMISSION (unrolled loop)
     * ============================================================ */
    for (attnum = 0; attnum < natts; attnum++)
    {
        CompactAttribute *att = TupleDescCompactAttr(desc, attnum);
        int     alignto = att->attalignby;

        /* ---- Emit attcheck label and wire up nvalid dispatch ---- */
        att_labels[attnum] = sljit_emit_label(C);
        sljit_set_label(nvalid_jumps[attnum], att_labels[attnum]);

        /* Patch previous null-path forward jump if it targeted this label */
        if (attnum > 0 && null_jumps[attnum - 1] != NULL)
            sljit_set_label(null_jumps[attnum - 1], att_labels[attnum]);

        /* If attnum == 0: reset offset to 0 */
        if (attnum == 0)
        {
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
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
        if (att->attnullability != ATTNULLABLE_VALID)
        {
            struct sljit_jump *no_hasnulls;
            struct sljit_jump *bit_is_set;

            /* if (!hasnulls) skip to not-null path */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_HASNULLS);
            no_hasnulls = sljit_emit_cmp(C, SLJIT_EQUAL,
                                         SLJIT_R0, 0,
                                         SLJIT_IMM, 0);

            /* byte = t_bits[attnum >> 3]; test bit (1 << (attnum & 7)) */
            sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_S4), attnum >> 3);
            sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
                           SLJIT_R0, 0,
                           SLJIT_IMM, 1 << (attnum & 0x07));
            /* if bit set -> column is NOT null, skip to alignment */
            bit_is_set = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                                        SLJIT_R0, 0,
                                        SLJIT_IMM, 0);

            /* ---- Column IS NULL ---- */
            /* Load slot from SOFF_TEMP to get tts_values/tts_isnull */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R1),
                           offsetof(TupleTableSlot, tts_values));
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_R2),
                           attnum * (sljit_sw) sizeof(Datum),
                           SLJIT_IMM, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                           SLJIT_MEM1(SLJIT_R1),
                           offsetof(TupleTableSlot, tts_isnull));
            sljit_emit_op1(C, SLJIT_MOV_U8,
                           SLJIT_MEM1(SLJIT_R2), attnum,
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

                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
                sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
                               SLJIT_MEM2(SLJIT_S3, SLJIT_R0), 0);
                is_short = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
                                          SLJIT_R1, 0,
                                          SLJIT_IMM, 0);

                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
                               SLJIT_R0, 0, SLJIT_IMM, alignto - 1);
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
                               SLJIT_R0, 0, SLJIT_IMM, ~((sljit_sw)(alignto - 1)));
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                               SLJIT_R0, 0);

                sljit_set_label(is_short, sljit_emit_label(C));
            }
            else
            {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
                               SLJIT_R0, 0, SLJIT_IMM, alignto - 1);
                sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
                               SLJIT_R0, 0, SLJIT_IMM, ~((sljit_sw)(alignto - 1)));
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                               SLJIT_R0, 0);
            }

            if (known_alignment >= 0)
                known_alignment = TYPEALIGN(alignto, known_alignment);
        }

        if (attguaranteedalign)
        {
            Assert(known_alignment >= 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                           SLJIT_IMM, known_alignment);
        }

        /* ---- Value extraction ---- */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
        sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
                       SLJIT_S3, 0, SLJIT_R0, 0);

        /* Load slot from SOFF_TEMP for tts_isnull */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
                       SLJIT_MEM1(SLJIT_R2),
                       offsetof(TupleTableSlot, tts_isnull));
        /* tts_isnull[attnum] = false */
        sljit_emit_op1(C, SLJIT_MOV_U8,
                       SLJIT_MEM1(SLJIT_R3), attnum,
                       SLJIT_IMM, 0);

        /* R2 = tts_values (reload from slot) */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
                       SLJIT_MEM1(SLJIT_R2),
                       offsetof(TupleTableSlot, tts_values));

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
            sljit_emit_op1(C, mov_op, SLJIT_R3, 0,
                           SLJIT_MEM1(SLJIT_R1), 0);
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_R2),
                           attnum * (sljit_sw) sizeof(Datum),
                           SLJIT_R3, 0);
        }
        else
        {
            sljit_emit_op1(C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_R2),
                           attnum * (sljit_sw) sizeof(Datum),
                           SLJIT_R1, 0);
        }

        /* ---- Compute alignment tracking for NEXT column ---- */
        if (att->attlen < 0)
        {
            known_alignment = -1;
            attguaranteedalign = false;
        }
        else if (att->attnullability == ATTNULLABLE_VALID &&
                 attguaranteedalign && known_alignment >= 0)
        {
            Assert(att->attlen > 0);
            known_alignment += att->attlen;
        }
        else if (att->attnullability == ATTNULLABLE_VALID &&
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
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                               SLJIT_IMM, known_alignment);
            }
            else
            {
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                               SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
                sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
                               SLJIT_R0, 0, SLJIT_IMM, att->attlen);
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                               SLJIT_R0, 0);
            }
        }
        else if (att->attlen == -1)
        {
            /* Varlena: off += varsize_any(attdatap) */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
                             SLJIT_IMM, (sljit_sw) varsize_any);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
                           SLJIT_R1, 0, SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                           SLJIT_R1, 0);
        }
        else if (att->attlen == -2)
        {
            /* Cstring: off += strlen(attdatap) + 1 */
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
                             SLJIT_IMM, (sljit_sw) strlen);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
                           SLJIT_R0, 0, SLJIT_IMM, 1);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                           SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
            sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
                           SLJIT_R1, 0, SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF,
                           SLJIT_R1, 0);
        }
    }

    /* ============================================================
     * EPILOGUE: patch jumps, store tts_nvalid, off, flags
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

        /* Reload slot from SOFF_TEMP */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);

        /* tts_nvalid = natts (int16 store) */
        sljit_emit_op1(C, SLJIT_MOV_S16,
                       SLJIT_MEM1(SLJIT_R0),
                       offsetof(TupleTableSlot, tts_nvalid),
                       SLJIT_IMM, natts);

        /* slot->off = (uint32) off */
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_OFF);
        sljit_emit_op1(C, SLJIT_MOV_U32,
                       SLJIT_MEM1(SLJIT_R0), slot_off,
                       SLJIT_R1, 0);

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
    }

    /* Restore S3/S4 (may hold aggstate/CurrentMemoryContext) */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S3, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S3);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_S4, 0,
                   SLJIT_MEM1(SLJIT_SP), SOFF_DEFORM_SAVE_S4);

    /* No return — falls through to the caller's skip label */

    pfree(nvalid_jumps);
    pfree(avail_jumps);
    pfree(null_jumps);
    pfree(att_labels);

    return true;
}

static bool
sljit_compile_expr(ExprState *state)
{
	PgJitterContext *ctx;
	struct sljit_compiler *C;
	ExprEvalStep   *steps;
	int				steps_len;
	int				opno;
	ExprEvalOp		opcode;

	struct sljit_label **step_labels;
	instr_time		starttime, endtime;

	/* Pending jumps for fixup after all code is emitted */
	struct {
		struct sljit_jump *jump;
		int				target;
	}			   *pending_jumps;
	int				npending = 0;

	/* Must have a parent PlanState */
	if (!state->parent)
		return false;

	/* Let PG's hand-optimized fast-path evalfuncs handle tiny expressions */
	if (expr_has_fast_path(state))
		return false;

	/* JIT is active */

	ctx = pg_jitter_get_context(state);

	INSTR_TIME_SET_CURRENT(starttime);

	steps = state->steps;
	steps_len = state->steps_len;

	/*
	 * Pre-scan: check if expression has aggregate transition steps
	 * or deform-eligible FETCHSOME steps.  Both need S3/S4 saved
	 * registers (aggs for aggstate/CurrentMemoryContext caching,
	 * deform for tupdata_base/t_bits).
	 */
	{
		bool has_agg = false;
		bool has_deform = false;

		for (int i = 0; i < steps_len; i++)
		{
			ExprEvalOp op = ExecEvalStepOp(state, &steps[i]);

			if (op >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
				op <= EEOP_AGG_PLAIN_TRANS_BYREF)
			{
				has_agg = true;
			}
			if ((op >= EEOP_INNER_FETCHSOME && op <= EEOP_NEW_FETCHSOME) &&
				steps[i].d.fetch.fixed && steps[i].d.fetch.known_desc &&
				(ctx->base.flags & PGJIT_DEFORM))
			{
				has_deform = true;
			}
		}

		C = sljit_create_compiler(NULL);
		if (!C)
			return false;

		step_labels = palloc0(sizeof(struct sljit_label *) * steps_len);
		pending_jumps = palloc(sizeof(*pending_jumps) * steps_len * 4);

		/*
		 * Function prologue.
		 * Saved regs: S0=state, S1=econtext, S2=isNull
		 * For aggregate/deform expressions: S3, S4
		 * 4 scratch regs (R0-R3).
		 */
		sljit_emit_enter(C, 0,
						 SLJIT_ARGS3(W, P, P, P),
						 4, (has_agg || has_deform) ? 5 : 3, SOFF_TOTAL);

		if (has_agg)
		{
			/* S3 = state->parent (aggstate) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_S3, 0,
						   SLJIT_MEM1(SLJIT_S0),
						   offsetof(ExprState, parent));
			/* S4 = &CurrentMemoryContext */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_S4, 0,
						   SLJIT_IMM,
						   (sljit_sw) &CurrentMemoryContext);
		}
	}

	/* Cache &state->resvalue and &state->resnull on the stack */
	/* resvaluep = &state->resvalue (address of the Datum field) */
	sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
				   SLJIT_S0, 0,
				   SLJIT_IMM, offsetof(ExprState, resvalue));
	sljit_emit_op1(C, SLJIT_MOV,
				   SLJIT_MEM1(SLJIT_SP), SOFF_RESVALUEP,
				   SLJIT_R0, 0);

	/* resnullp = &state->resnull */
	sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
				   SLJIT_S0, 0,
				   SLJIT_IMM, offsetof(ExprState, resnull));
	sljit_emit_op1(C, SLJIT_MOV,
				   SLJIT_MEM1(SLJIT_SP), SOFF_RESNULLP,
				   SLJIT_R0, 0);

	/* Cache resultslot and its values/nulls pointers (if resultslot exists) */
	/* resultslot = state->resultslot */
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

		/* result_values = resultslot->tts_values */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
					   SLJIT_MEM1(SLJIT_R0), offsetof(TupleTableSlot, tts_values));
		sljit_emit_op1(C, SLJIT_MOV,
					   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTVALS,
					   SLJIT_R1, 0);

		/* result_nulls = resultslot->tts_isnull */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
					   SLJIT_MEM1(SLJIT_R0), offsetof(TupleTableSlot, tts_isnull));
		sljit_emit_op1(C, SLJIT_MOV,
					   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTNULLS,
					   SLJIT_R1, 0);

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

			case EEOP_DONE_NO_RETURN:
			{
				sljit_emit_return(C, SLJIT_MOV, SLJIT_IMM, 0);
				break;
			}

			/* All hot-path opcodes below get native code */

			/*
			 * ---- FETCHSOME ----
			 * Fast path: if slot->tts_nvalid >= last_var, skip.
			 * Slow path: call slot_getsomeattrs_int(slot, last_var).
			 */
			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
			case EEOP_OLD_FETCHSOME:
			case EEOP_NEW_FETCHSOME:
			{
				struct sljit_jump *skip_j;
				bool deform_emitted = false;

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

				/* Try inline deform if conditions allow */
				if (op->d.fetch.fixed && op->d.fetch.known_desc &&
					(ctx->base.flags & PGJIT_DEFORM))
				{
					instr_time  deform_start, deform_end;

					INSTR_TIME_SET_CURRENT(deform_start);
					deform_emitted = sljit_emit_deform_inline(C,
															  op->d.fetch.known_desc,
															  op->d.fetch.kind,
															  op->d.fetch.last_var,
															  SLJIT_R0);
					INSTR_TIME_SET_CURRENT(deform_end);
					INSTR_TIME_ACCUM_DIFF(ctx->base.instr.deform_counter,
										  deform_end, deform_start);
				}

				if (!deform_emitted)
				{
					/* Fallback: call slot_getsomeattrs_int(slot, last_var) */
					/* Need to reload R0 since inline deform may have
					 * clobbered it if it returned false after emitting
					 * some code (which shouldn't happen with current
					 * guards, but be safe) */
					emit_load_econtext_slot(C, SLJIT_R0, opcode);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, op->d.fetch.last_var);
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS2V(P, 32),
									 SLJIT_IMM,
									 (sljit_sw) slot_getsomeattrs_int);
				}

				/* Skip label */
				sljit_set_label(skip_j, sljit_emit_label(C));
				break;
			}

			/*
			 * ---- VAR ----
			 * Load slot->tts_values[attnum] → *op->resvalue
			 * Load slot->tts_isnull[attnum] → *op->resnull
			 */
			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
			case EEOP_OLD_VAR:
			case EEOP_NEW_VAR:
			{
				int			attnum = op->d.var.attnum;

				/* R0 = slot */
				emit_load_econtext_slot(C, SLJIT_R0, opcode);

				/* R1 = slot->tts_values (pointer to Datum array) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(TupleTableSlot, tts_values));
				/* R2 = values[attnum] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   attnum * (sljit_sw) sizeof(Datum));
				/* *op->resvalue = R2 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R3), 0,
							   SLJIT_R2, 0);

				/* R1 = slot->tts_isnull (pointer to bool array) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(TupleTableSlot, tts_isnull));
				/* R2 = isnull[attnum] */
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   attnum * (sljit_sw) sizeof(bool));
				/* *op->resnull = R2 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R3), 0,
							   SLJIT_R2, 0);
				break;
			}

			/*
			 * ---- ASSIGN_*_VAR ----
			 * Load source slot's values[attnum] → resultslot->tts_values[resultnum]
			 * Load source slot's isnull[attnum] → resultslot->tts_isnull[resultnum]
			 */
			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
			case EEOP_ASSIGN_OLD_VAR:
			case EEOP_ASSIGN_NEW_VAR:
			{
				int			attnum = op->d.assign_var.attnum;
				int			resultnum = op->d.assign_var.resultnum;

				/* R0 = source slot */
				emit_load_econtext_slot(C, SLJIT_R0, opcode);

				/* R1 = source->tts_values; R2 = values[attnum] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(TupleTableSlot, tts_values));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   attnum * (sljit_sw) sizeof(Datum));

				/* R3 = resultslot->tts_values (cached on stack) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTVALS);
				/* result_values[resultnum] = R2 */
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R3),
							   resultnum * (sljit_sw) sizeof(Datum),
							   SLJIT_R2, 0);

				/* Now do nulls: R1 = source->tts_isnull; R2 = isnull[attnum] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(TupleTableSlot, tts_isnull));
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   attnum * (sljit_sw) sizeof(bool));

				/* R3 = resultslot->tts_isnull (cached on stack) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTNULLS);
				/* result_nulls[resultnum] = R2 */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R3),
							   resultnum * (sljit_sw) sizeof(bool),
							   SLJIT_R2, 0);
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
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS1(W, W),
									 SLJIT_IMM,
									 (sljit_sw) MakeExpandedObjectReadOnlyInternal);
					/* Re-load R1 = 0 (not null, since we skipped for null) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, 0);

					sljit_set_label(skip_ro, sljit_emit_label(C));
				}

				/* Store to resultslot->tts_isnull[resultnum] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTNULLS);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R2),
							   resultnum * (sljit_sw) sizeof(bool),
							   SLJIT_R1, 0);

				/* Store to resultslot->tts_values[resultnum] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_RESULTVALS);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R2),
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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = constval.isnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, op->d.constval.isnull ? 1 : 0);
				break;
			}

			/*
			 * ---- FUNCEXPR / FUNCEXPR_STRICT / STRICT_1 / STRICT_2 ----
			 * V1 calling convention: result = fn_addr(fcinfo_data)
			 */
			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int			nargs = op->d.func.nargs;
				struct sljit_jump *skip_null = NULL;

				if (opcode == EEOP_FUNCEXPR_STRICT ||
					opcode == EEOP_FUNCEXPR_STRICT_1 ||
					opcode == EEOP_FUNCEXPR_STRICT_2)
				{
					/*
					 * Check args for NULL. If any arg is null, set resnull=true
					 * and skip the function call.
					 *
					 * First, set resnull to true (will be reset by the function).
					 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 1);

					/*
					 * Check each arg's isnull. fcinfo->args[i].isnull is at
					 * offsetof(FunctionCallInfoBaseData, args) + i * sizeof(NullableDatum) + offsetof(NullableDatum, isnull)
					 */
					for (int argno = 0; argno < nargs; argno++)
					{
						sljit_sw	null_off =
							(sljit_sw) &fcinfo->args[argno].isnull -
							(sljit_sw) fcinfo;

						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
									   SLJIT_IMM, (sljit_sw) fcinfo);
						sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
									   SLJIT_MEM1(SLJIT_R0), null_off);

						/* If arg is null, jump to skip */
						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										   SLJIT_R0, 0,
										   SLJIT_IMM, 0);
						/* Chain: all null checks jump to the same skip label */
						if (skip_null == NULL)
							skip_null = j;
						else
						{
							/* Set previous jump to a temporary label, create new */
							/* Actually, we need all of them to go to the same place.
							 * Use pending_jumps with a special target. */
							/* Simpler: just use the fallback for strict with >2 args */
						}
						/* Actually, let's set each jump to the skip label after the loop */
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = -1; /* special: skip_null */
						npending++;
					}
				}

				/*
				 * Try direct native call — bypasses fcinfo entirely.
				 * Args are loaded from fcinfo->args[].value (already
				 * populated by preceding VAR/CONST steps).
				 */
				{
				const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);

				if (dfn && dfn->jit_fn)
				{
					/* Load fcinfo once, then load all args from offsets */
					if (dfn->nargs > 0)
					{
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
									   SLJIT_IMM, (sljit_sw) fcinfo);
						for (int i = 0; i < dfn->nargs; i++)
						{
							sljit_sw val_off =
								(sljit_sw) &fcinfo->args[i].value -
								(sljit_sw) fcinfo;
							sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0 + i, 0,
										   SLJIT_MEM1(SLJIT_R2), val_off);
						}
					}

					/* Direct call with native arg types */
					sljit_emit_icall(C, SLJIT_CALL,
									 jit_sljit_call_type(dfn),
									 SLJIT_IMM,
									 (sljit_sw) dfn->jit_fn);

					/* Store result to *op->resvalue.
					 * For T32 returns, R0 holds a 32-bit value that needs
					 * zero-extension to Datum (64-bit). sljit already
					 * zero-extends 32-bit results in the W register. */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) op->resvalue);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_R0, 0);

					/* *op->resnull = false (direct fns with non-null inputs
					 * always return non-null) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_IMM, 0);
				}
				else
				{
				/* Fallback: generic fcinfo path */

				/* fcinfo->isnull = false (must reset before each call;
				 * PG_RETURN_* macros don't clear it) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(FunctionCallInfoBaseData, isnull),
							   SLJIT_IMM, 0);

				/* Call fn_addr(fcinfo) */
				/* R0 still = fcinfo */
				sljit_emit_icall(C, SLJIT_CALL,
								 SLJIT_ARGS1(W, P),
								 SLJIT_IMM,
								 (sljit_sw) op->d.func.fn_addr);

				/* *op->resvalue = R0 (return value) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = fcinfo->isnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(FunctionCallInfoBaseData, isnull));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);
				} /* end else fallback */
				} /* end direct-call dispatch block */

				/* Fix up all null-check jumps to land here */
				if (opcode == EEOP_FUNCEXPR_STRICT ||
					opcode == EEOP_FUNCEXPR_STRICT_1 ||
					opcode == EEOP_FUNCEXPR_STRICT_2)
				{
					struct sljit_label *after = sljit_emit_label(C);
					/* Go back through the pending_jumps we just added */
					for (int j = npending - nargs; j < npending; j++)
					{
						if (pending_jumps[j].target == -1)
						{
							sljit_set_label(pending_jumps[j].jump, after);
							pending_jumps[j].target = -2; /* mark as resolved */
						}
					}
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
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->d.boolexpr.anynull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);
				}

				/* R0 = *op->resnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* If null, set anynull and continue */
				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: check if value is false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

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
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->d.boolexpr.anynull);
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

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->d.boolexpr.anynull);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), 0);

					j_no_anynull = sljit_emit_cmp(C, SLJIT_EQUAL,
												  SLJIT_R0, 0, SLJIT_IMM, 0);

					/* Set result to NULL */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resvalue);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);

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
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->d.boolexpr.anynull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);
				}

				/* Check null */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: check if true */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

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
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->d.boolexpr.anynull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 1);

					sljit_set_label(j_skip, sljit_emit_label(C));
				}

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					struct sljit_jump *j_no_anynull;

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->d.boolexpr.anynull);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), 0);

					j_no_anynull = sljit_emit_cmp(C, SLJIT_EQUAL,
												  SLJIT_R0, 0, SLJIT_IMM, 0);

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resvalue);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);

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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* R0 = (R0 == 0) ? 1 : 0 */
				sljit_emit_op2u(C, SLJIT_SUB | SLJIT_SET_Z,
								SLJIT_R0, 0, SLJIT_IMM, 0);
				sljit_emit_op_flags(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_EQUAL);

				/* Store back (as Datum, which is pointer-sized) */
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);
				break;
			}

			/*
			 * ---- QUAL ----
			 * If null or false, jump to jumpdone.
			 */
			case EEOP_QUAL:
			{
				struct sljit_jump *j_null, *j_false;

				/* Check null */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				j_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Check false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				j_false = sljit_emit_cmp(C, SLJIT_EQUAL,
										 SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Continue (not null, not false) — skip the qual-fail block */
				{
					struct sljit_jump *j_cont = sljit_emit_jump(C, SLJIT_JUMP);

					/* Qual fail: set resvalue=0, resnull=false, jump to done */
					struct sljit_label *fail_label = sljit_emit_label(C);
					sljit_set_label(j_null, fail_label);
					sljit_set_label(j_false, fail_label);

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) op->resvalue);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R0), 0,
								   SLJIT_IMM, 0);

					struct sljit_jump *j_done = sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_done;
					pending_jumps[npending].target = op->d.qualexpr.jumpdone;
					npending++;

					sljit_set_label(j_cont, sljit_emit_label(C));
				}
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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);
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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);
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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* If null, jump */
				struct sljit_jump *j1 = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
													   SLJIT_R0, 0,
													   SLJIT_IMM, 0);
				pending_jumps[npending].jump = j1;
				pending_jumps[npending].target = op->d.jump.jumpdone;
				npending++;

				/* If false, jump */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);
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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* Store as Datum (extend to 64-bit) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R2), 0,
							   SLJIT_R0, 0);

				/* resnull = false */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1), 0);

				/* R0 = !R0: XOR with 1 */
				sljit_emit_op2(C, SLJIT_XOR, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);

				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R2), 0,
							   SLJIT_R0, 0);

				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);
				break;
			}

			/*
			 * ---- AGGREGATE INPUT NULL CHECKS ----
			 * Inline: load isnull bool, conditional jump. No function call.
			 */
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
			{
				NullableDatum *args = op->d.agg_strict_input_check.args;
				int			nargs = op->d.agg_strict_input_check.nargs;
				int			jumpnull = op->d.agg_strict_input_check.jumpnull;

				for (int argno = 0; argno < nargs; argno++)
				{
					sljit_sw isnull_addr =
						(sljit_sw) &args[argno].isnull;

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, isnull_addr);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), 0);

					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = jumpnull;
					npending++;
				}
				break;
			}

			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
			{
				bool	   *nulls = op->d.agg_strict_input_check.nulls;
				int			nargs = op->d.agg_strict_input_check.nargs;
				int			jumpnull = op->d.agg_strict_input_check.jumpnull;

				for (int argno = 0; argno < nargs; argno++)
				{
					sljit_sw null_addr = (sljit_sw) &nulls[argno];

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, null_addr);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), 0);

					struct sljit_jump *j =
						sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);
					pending_jumps[npending].jump = j;
					pending_jumps[npending].target = jumpnull;
					npending++;
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
				MemoryContext tuple_mctx =
					aggstate->tmpcontext->ecxt_per_tuple_memory;

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
				 * S3 = aggstate, so load all_pergroups directly
				 * from [S3 + offset] (1 LDR, no IMM load needed).
				 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_S3),
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
								   SLJIT_S3, 0);	/* R0 = aggstate */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) pertrans);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
								   SLJIT_IMM, (sljit_sw) aggcontext);
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS4V(P, P, P, P),
									 SLJIT_IMM,
									 (sljit_sw) ExecAggInitGroup);

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
						sljit_emit_icall(C, SLJIT_CALL,
										 SLJIT_ARGS0V(),
										 SLJIT_IMM,
										 (sljit_sw) pg_jitter_int8_overflow);
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
					 * inline.
					 */
					struct sljit_jump *j_not_null;

					/* R0 = pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP),
								   SOFF_AGG_PERGROUP);
					/* R2 = fcinfo->args[1].value (int4 as Datum) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_IMM,
								   (sljit_sw) &fcinfo->args[1].value);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_MEM1(SLJIT_R2), 0);
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
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_IMM,
								   (sljit_sw) &fcinfo->args[1].value);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_MEM1(SLJIT_R2), 0);

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
				else
				{
					/*
					 * Generic path: aggstate field setup,
					 * MemoryContextSwitchTo, fcinfo marshaling,
					 * fn_addr call, result store, context restore.
					 */
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_S3),
								   offsetof(AggState, curaggcontext),
								   SLJIT_IMM, (sljit_sw) aggcontext);
					sljit_emit_op1(C, SLJIT_MOV_S32,
								   SLJIT_MEM1(SLJIT_S3),
								   offsetof(AggState, current_set),
								   SLJIT_IMM, setno);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_S3),
								   offsetof(AggState, curpertrans),
								   SLJIT_IMM, (sljit_sw) pertrans);

					/* MemoryContextSwitchTo(tuple_mctx) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_S4), 0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_OLDCTX,
								   SLJIT_R0, 0);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_S4), 0,
								   SLJIT_IMM, (sljit_sw) tuple_mctx);

					/* Setup fcinfo->args[0] from pergroup */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
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
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS1(W, P),
									 SLJIT_IMM, (sljit_sw) fn_addr);

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
									   SLJIT_S3, 0);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
									   SLJIT_IMM, (sljit_sw) pertrans);
						sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
									   SLJIT_MEM1(SLJIT_SP),
									   SOFF_AGG_PERGROUP);
						sljit_emit_icall(C, SLJIT_CALL,
										 SLJIT_ARGS4V(P, P, W, P),
										 SLJIT_IMM,
										 (sljit_sw) pg_jitter_agg_byref_finish);
					}

					/* Restore memory context */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP), SOFF_AGG_OLDCTX);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_S4), 0,
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
			case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
			case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
			{
				int jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;
				void *fn = (opcode == EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
					? (void *) pg_jitter_fallback_step
					: (void *) pg_jitter_fallback_step;

				/* These are rare, use fallback */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_S0, 0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				sljit_emit_icall(C, SLJIT_CALL,
								 SLJIT_ARGS3(W, P, P, P),
								 SLJIT_IMM,
								 (sljit_sw) pg_jitter_fallback_step);

				struct sljit_jump *j =
					sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
								   SLJIT_R0, 0, SLJIT_IMM, 0);
				pending_jumps[npending].jump = j;
				pending_jumps[npending].target = jumpdistinct;
				npending++;
				break;
			}

			/*
			 * ---- HASHDATUM_SET_INITVAL ----
			 * Store init_value → *op->resvalue, set *op->resnull = false.
			 */
			case EEOP_HASHDATUM_SET_INITVAL:
			{
				/* *op->resvalue = op->d.hashdatum_initvalue.init_value */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM,
							   (sljit_sw) op->d.hashdatum_initvalue.init_value);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);
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

				/* Load fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull != 0, jump to store_zero */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null path: call hash function */
				if (hdfn && hdfn->jit_fn)
				{
					/* Direct hash call: load arg from fcinfo->args[0].value */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), val_off);
					sljit_emit_icall(C, SLJIT_CALL,
									 jit_sljit_call_type(hdfn),
									 SLJIT_IMM,
									 (sljit_sw) hdfn->jit_fn);
				}
				else
				{
					/* Fallback: fcinfo path */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS1(W, P),
									 SLJIT_IMM,
									 (sljit_sw) op->d.hashdatum.fn_addr);
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

				/* *op->resvalue = R0 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);
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

				/* Load fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull != 0, jump to null_path */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null path: call hash function */
				if (hdfn && hdfn->jit_fn)
				{
					/* Direct hash call */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), val_off);
					sljit_emit_icall(C, SLJIT_CALL,
									 jit_sljit_call_type(hdfn),
									 SLJIT_IMM,
									 (sljit_sw) hdfn->jit_fn);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS1(W, P),
									 SLJIT_IMM,
									 (sljit_sw) op->d.hashdatum.fn_addr);
				}

				/* *op->resvalue = R0 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);

				/* Jump past null_path (fall through to next step) */
				{
					struct sljit_jump *j_skip_null = sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_skip_null;
					pending_jumps[npending].target = -1; /* resolved below */
					npending++;

					/* null_path: */
					struct sljit_label *lbl_null = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_null);

					/* Resolve the skip jump to land after null_path */
					/* We'll use a label at the end */

					/* *op->resnull = true */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_IMM, 1);

					/* *op->resvalue = 0 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) op->resvalue);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_IMM, 0);

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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) iresult);
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(NullableDatum, value));

				/* Rotate left 1 using native instruction */
				sljit_emit_op2(C, SLJIT_ROTL32, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);

				/* Save rotated hash to stack (survives function call) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
							   SOFF_TEMP, SLJIT_R0, 0);

				/* Load fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R1),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull, skip hash call */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null: call hash function */
				if (hdfn && hdfn->jit_fn)
				{
					/* Direct hash call */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), val_off);
					sljit_emit_icall(C, SLJIT_CALL,
									 jit_sljit_call_type(hdfn),
									 SLJIT_IMM,
									 (sljit_sw) hdfn->jit_fn);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS1(W, P),
									 SLJIT_IMM,
									 (sljit_sw) op->d.hashdatum.fn_addr);
				}

				/* Restore rotated hash from stack, XOR with hash result */
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
				sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0,
							   SLJIT_R1, 0, SLJIT_R0, 0);

				/* Save combined hash back to stack for store_result */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
							   SOFF_TEMP, SLJIT_R0, 0);

				j_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* skip_hash: (isnull path falls through here) */
				{
					struct sljit_label *lbl_skip = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_skip);
				}

				/* store_result: both paths converge */
				{
					struct sljit_label *lbl_store = sljit_emit_label(C);
					sljit_set_label(j_done, lbl_store);
				}

				/* Load hash from stack, zero-extend, store to *op->resvalue */
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);
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

				/* Load fcinfo->args[0].isnull */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) fcinfo);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   (sljit_sw) ((char *) &fcinfo->args[0].isnull -
										   (char *) fcinfo));

				/* If isnull, jump to null_path */
				j_isnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Not null path: load existing hash, rotate, call, XOR */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) iresult);
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R0),
							   offsetof(NullableDatum, value));

				/* Rotate left 1 using native instruction */
				sljit_emit_op2(C, SLJIT_ROTL32, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);

				/* Save rotated hash to stack (survives function call) */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
							   SOFF_TEMP, SLJIT_R0, 0);

				/* Call hash function */
				if (hdfn && hdfn->jit_fn)
				{
					/* Direct hash call */
					sljit_sw val_off =
						(sljit_sw) &fcinfo->args[0].value - (sljit_sw) fcinfo;
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_R0), val_off);
					sljit_emit_icall(C, SLJIT_CALL,
									 jit_sljit_call_type(hdfn),
									 SLJIT_IMM,
									 (sljit_sw) hdfn->jit_fn);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) fcinfo);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R0),
								   offsetof(FunctionCallInfoBaseData, isnull),
								   SLJIT_IMM, 0);
					sljit_emit_icall(C, SLJIT_CALL,
									 SLJIT_ARGS1(W, P),
									 SLJIT_IMM,
									 (sljit_sw) op->d.hashdatum.fn_addr);
				}

				/* Restore rotated hash from stack, XOR with hash result */
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R1, 0,
							   SLJIT_MEM1(SLJIT_SP), SOFF_TEMP);
				sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0,
							   SLJIT_R1, 0, SLJIT_R0, 0);

				/* Store UInt32GetDatum(existing) → *op->resvalue */
				sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
							   SLJIT_R0, 0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resvalue);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_R0, 0);

				/* *op->resnull = false */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op->resnull);
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM1(SLJIT_R1), 0,
							   SLJIT_IMM, 0);

				/* Jump past null_path */
				{
					struct sljit_jump *j_skip_null = sljit_emit_jump(C, SLJIT_JUMP);
					pending_jumps[npending].jump = j_skip_null;
					pending_jumps[npending].target = -1;
					npending++;

					/* null_path: */
					struct sljit_label *lbl_null = sljit_emit_label(C);
					sljit_set_label(j_isnull, lbl_null);

					/* *op->resnull = true */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) op->resnull);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_IMM, 1);

					/* *op->resvalue = 0 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, (sljit_sw) op->resvalue);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM1(SLJIT_R1), 0,
								   SLJIT_IMM, 0);

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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
							   SLJIT_IMM, (sljit_sw) op);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_S1, 0);
				sljit_emit_icall(C, SLJIT_CALL,
								 SLJIT_ARGS3(W, P, P, P),
								 SLJIT_IMM,
								 (sljit_sw) pg_jitter_fallback_step);

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
					/* HASHDATUM_FIRST_STRICT and NEXT32_STRICT are compiled natively */
					case EEOP_ROWCOMPARE_STEP:
						/*
						 * ROWCOMPARE can jump to jumpnull or jumpdone.
						 * Both are returned by fallback as the step number.
						 * We use jumpdone here; jumpnull is also valid
						 * since fallback returns whichever applies.
						 */
						fb_jump_target = op->d.rowcompare_step.jumpdone;
						break;
					/* EEOP_IOCOERCE_SAFE has no jump */
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
						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_EQUAL,
										   SLJIT_R0, 0,
										   SLJIT_IMM, jnull);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = jnull;
						npending++;
					}
					if (jdone >= 0 && jdone < steps_len)
					{
						struct sljit_jump *j =
							sljit_emit_cmp(C, SLJIT_EQUAL,
										   SLJIT_R0, 0,
										   SLJIT_IMM, jdone);
						pending_jumps[npending].jump = j;
						pending_jumps[npending].target = jdone;
						npending++;
					}
				}
				else if (fb_jump_target >= 0 && fb_jump_target < steps_len)
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
			return false;
		}

		INSTR_TIME_SET_CURRENT(emit_end);
		INSTR_TIME_ACCUM_DIFF(ctx->base.instr.emission_counter,
							  emit_end, emit_start);

		/* Register for cleanup */
		pg_jitter_register_compiled(ctx, sljit_code_free, code);

		/* Set the eval function (with validation wrapper on first call) */
		pg_jitter_install_expr(state, (ExprStateEvalFunc) code);

		sljit_free_compiler(C);
	}

	pfree(step_labels);
	pfree(pending_jumps);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(ctx->base.instr.generation_counter,
						  endtime, starttime);
	ctx->base.instr.created_functions++;

	return true;
}
