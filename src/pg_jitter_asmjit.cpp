/*
 * pg_jitter_asmjit.cpp — AsmJIT-based PostgreSQL JIT provider
 *
 * Uses AsmJIT's a64::Compiler for ARM64 with virtual register allocation.
 * Hot-path opcodes get native code; everything else uses fallback.
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

#include <asmjit/a64.h>

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
	else if (nsteps == 4)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);

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
			(step1 == EEOP_FUNCEXPR_STRICT ||
			 step1 == EEOP_FUNCEXPR_STRICT_1 ||
			 step1 == EEOP_FUNCEXPR_STRICT_2))
			return true;

		if (step0 == EEOP_INNER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_OUTER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
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

/*
 * Emit deform code inline into the AsmJIT Compiler.
 *
 * Uses virtual registers — the AsmJIT register allocator handles
 * physical register assignment. No save/restore needed.
 *
 * Returns true if deform code was emitted, false otherwise.
 */
static bool
asmjit_emit_deform_inline(a64::Compiler &cc,
                           TupleDesc desc,
                           const TupleTableSlotOps *ops,
                           int natts,
                           a64::Gp slot_reg,
                           a64::Gp tmp1,
                           a64::Gp tmp2,
                           a64::Gp tmp3)
{
    int     attnum;
    int     known_alignment = 0;
    bool    attguaranteedalign = true;
    int     guaranteed_column_number = -1;
    int64_t tuple_off;
    int64_t slot_off;

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

    /* Allocate forward-jump arrays */
    Label *nvalid_labels = (Label *) palloc(sizeof(Label) * natts);
    Label *att_labels = (Label *) palloc(sizeof(Label) * natts);
    Label *null_labels = (Label *) palloc(sizeof(Label) * natts);
    bool  *has_null_jump = (bool *) palloc0(sizeof(bool) * natts);
    Label *avail_labels = (Label *) palloc(sizeof(Label) * natts);
    bool  *has_avail_jump = (bool *) palloc0(sizeof(bool) * natts);

    for (int i = 0; i < natts; i++)
    {
        nvalid_labels[i] = cc.new_label();
        att_labels[i] = cc.new_label();
        null_labels[i] = cc.new_label();
        avail_labels[i] = cc.new_label();
    }

    Label deform_out = cc.new_label();

    /* Virtual registers for deform */
    a64::Gp v_tts_values = cc.new_gpx("d_values");
    a64::Gp v_tts_isnull = cc.new_gpx("d_isnull");
    a64::Gp v_tupdata_base = cc.new_gpx("d_tupdata");
    a64::Gp v_t_bits = cc.new_gpx("d_tbits");
    a64::Gp v_off = cc.new_gpx("d_off");
    a64::Gp v_hasnulls = cc.new_gpx("d_hasnulls");
    a64::Gp v_maxatt = cc.new_gpx("d_maxatt");
    a64::Gp v_attdatap = cc.new_gpx("d_attdatap");
    a64::Gp dtmp1 = cc.new_gpx("dt1");
    a64::Gp dtmp2 = cc.new_gpx("dt2");

    /* ---- PROLOGUE: load fields from slot ---- */

    /* v_tts_values = slot->tts_values */
    cc.ldr(v_tts_values, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
    /* v_tts_isnull = slot->tts_isnull */
    cc.ldr(v_tts_isnull, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));

    /* dtmp1 = HeapTuple ptr from slot-type-specific offset */
    cc.ldr(dtmp1, a64::ptr(slot_reg, (int32_t) tuple_off));
    /* dtmp1 = tuplep = heaptuple->t_data (HeapTupleHeader) */
    cc.ldr(dtmp1, a64::ptr(dtmp1, offsetof(HeapTupleData, t_data)));

    /* v_hasnulls = tuplep->t_infomask & HEAP_HASNULL */
    cc.ldrh(dtmp2.w(), a64::ptr(dtmp1, offsetof(HeapTupleHeaderData, t_infomask)));
    cc.and_(v_hasnulls, dtmp2, Imm(HEAP_HASNULL));

    /* v_maxatt = tuplep->t_infomask2 & HEAP_NATTS_MASK */
    cc.ldrh(dtmp2.w(), a64::ptr(dtmp1, offsetof(HeapTupleHeaderData, t_infomask2)));
    cc.and_(v_maxatt, dtmp2, Imm(HEAP_NATTS_MASK));

    /* v_t_bits = &tuplep->t_bits[0] */
    cc.add(v_t_bits, dtmp1, Imm(offsetof(HeapTupleHeaderData, t_bits)));

    /* t_hoff -> dtmp2 (uint8) */
    cc.ldrb(dtmp2.w(), a64::ptr(dtmp1, offsetof(HeapTupleHeaderData, t_hoff)));
    /* v_tupdata_base = tuplep + t_hoff */
    cc.add(v_tupdata_base, dtmp1, dtmp2);

    /* v_off = slot->off (uint32, zero-extended) */
    cc.ldr(v_off.w(), a64::ptr(slot_reg, (int32_t) slot_off));

    /* ---- MISSING ATTRIBUTES CHECK ---- */
    if ((natts - 1) > guaranteed_column_number)
    {
        Label skip_missing = cc.new_label();

        cc.cmp(v_maxatt, natts);
        cc.b_ge(skip_missing);

        /* call slot_getmissingattrs(slot, maxatt, natts) */
        {
            a64::Gp fn_reg = cc.new_gpx();
            cc.mov(fn_reg, (uint64_t)(void *) slot_getmissingattrs);
            InvokeNode *invoke;
            cc.invoke(Out(invoke), fn_reg,
                      FuncSignature::build<void, void *, int, int>());
            invoke->set_arg(0, slot_reg);
            invoke->set_arg(1, v_maxatt);
            invoke->set_arg(2, Imm(natts));
        }

        cc.bind(skip_missing);
    }

    /* ---- NVALID DISPATCH ---- */
    {
        a64::Gp v_nvalid = cc.new_gpx("d_nvalid");
        cc.ldrsh(v_nvalid.w(), a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_nvalid)));

        for (attnum = 0; attnum < natts; attnum++)
        {
            cc.cmp(v_nvalid, attnum);
            cc.b(a64::CondCode::kEQ, att_labels[attnum]);
        }
        /* Default: already deformed enough */
        cc.b(deform_out);
    }

    /* ---- PER-ATTRIBUTE CODE ---- */
    for (attnum = 0; attnum < natts; attnum++)
    {
        CompactAttribute *att = TupleDescCompactAttr(desc, attnum);
        int alignto = att->attalignby;

        cc.bind(att_labels[attnum]);

        /* If attnum == 0: reset offset */
        if (attnum == 0)
            cc.mov(v_off, 0);

        /* ---- Availability check ---- */
        if (attnum > guaranteed_column_number)
        {
            cc.cmp(v_maxatt, attnum + 1);  /* if maxatt <= attnum, goto out */
            cc.b(a64::CondCode::kLT, deform_out);  /* maxatt < attnum+1 means attnum >= maxatt */
        }

        /* ---- Null check ---- */
        if (att->attnullability != ATTNULLABLE_VALID)
        {
            Label notnull = cc.new_label();

            /* if (!hasnulls) skip to notnull */
            cc.cbz(v_hasnulls, notnull);

            /* byte = t_bits[attnum >> 3]; test bit (1 << (attnum & 7)) */
            cc.ldrb(dtmp1.w(), a64::ptr(v_t_bits, attnum >> 3));
            cc.tst(dtmp1.w(), Imm(1 << (attnum & 0x07)));
            cc.b(a64::CondCode::kNE, notnull);  /* bit set = NOT null */

            /* Column IS NULL: tts_values[attnum] = 0, tts_isnull[attnum] = 1 */
            cc.str(a64::xzr, a64::ptr(v_tts_values, attnum * (int64_t) sizeof(Datum)));
            cc.mov(dtmp1, 1);
            cc.strb(dtmp1.w(), a64::ptr(v_tts_isnull, attnum));

            /* Jump to next attcheck (or out if last) */
            if (attnum + 1 < natts)
                cc.b(att_labels[attnum + 1]);
            else
                cc.b(deform_out);

            cc.bind(notnull);
            attguaranteedalign = false;
        }

        /* ---- Alignment ---- */
        if (alignto > 1 &&
            (known_alignment < 0 ||
             known_alignment != TYPEALIGN(alignto, known_alignment)))
        {
            if (att->attlen == -1)
            {
                Label skip_align = cc.new_label();

                attguaranteedalign = false;

                /* Peek first byte: if nonzero -> short varlena, skip align */
                {
                    a64::Gp addr = cc.new_gpx();
                    cc.add(addr, v_tupdata_base, v_off);
                    cc.ldrb(dtmp1.w(), a64::ptr(addr));
                }
                cc.cbnz(dtmp1, skip_align);

                /* Align off */
                cc.add(v_off, v_off, Imm(alignto - 1));
                cc.and_(v_off, v_off, Imm(~((int64_t)(alignto - 1))));

                cc.bind(skip_align);
            }
            else
            {
                /* Fixed-width: always align */
                cc.add(v_off, v_off, Imm(alignto - 1));
                cc.and_(v_off, v_off, Imm(~((int64_t)(alignto - 1))));
            }

            if (known_alignment >= 0)
                known_alignment = TYPEALIGN(alignto, known_alignment);
        }

        if (attguaranteedalign)
        {
            Assert(known_alignment >= 0);
            cc.mov(v_off, known_alignment);
        }

        /* ---- Value extraction ---- */
        /* v_attdatap = v_tupdata_base + v_off */
        cc.add(v_attdatap, v_tupdata_base, v_off);

        /* tts_isnull[attnum] = false */
        cc.strb(a64::wzr, a64::ptr(v_tts_isnull, attnum));

        if (att->attbyval)
        {
            switch (att->attlen)
            {
                case 1:
                    cc.ldrsb(dtmp1, a64::ptr(v_attdatap));
                    break;
                case 2:
                    cc.ldrsh(dtmp1, a64::ptr(v_attdatap));
                    break;
                case 4:
                    cc.ldrsw(dtmp1, a64::ptr(v_attdatap));
                    break;
                case 8:
                    cc.ldr(dtmp1, a64::ptr(v_attdatap));
                    break;
                default:
                    pfree(nvalid_labels); pfree(att_labels);
                    pfree(null_labels); pfree(has_null_jump);
                    pfree(avail_labels); pfree(has_avail_jump);
                    return false;
            }
            cc.str(dtmp1, a64::ptr(v_tts_values, attnum * (int64_t) sizeof(Datum)));
        }
        else
        {
            /* Store pointer: tts_values[attnum] = attdatap */
            cc.str(v_attdatap, a64::ptr(v_tts_values, attnum * (int64_t) sizeof(Datum)));
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
                cc.mov(v_off, known_alignment);
            }
            else
            {
                cc.add(v_off, v_off, Imm(att->attlen));
            }
        }
        else if (att->attlen == -1)
        {
            /* Varlena: off += varsize_any(attdatap) */
            a64::Gp fn_reg = cc.new_gpx();
            cc.mov(fn_reg, (uint64_t)(void *) varsize_any);
            InvokeNode *invoke;
            cc.invoke(Out(invoke), fn_reg,
                      FuncSignature::build<int64_t, void *>());
            invoke->set_arg(0, v_attdatap);
            invoke->set_ret(0, dtmp1);
            cc.add(v_off, v_off, dtmp1);
        }
        else if (att->attlen == -2)
        {
            /* Cstring: off += strlen(attdatap) + 1 */
            a64::Gp fn_reg = cc.new_gpx();
            cc.mov(fn_reg, (uint64_t)(void *) strlen);
            InvokeNode *invoke;
            cc.invoke(Out(invoke), fn_reg,
                      FuncSignature::build<int64_t, void *>());
            invoke->set_arg(0, v_attdatap);
            invoke->set_ret(0, dtmp1);
            cc.add(v_off, v_off, dtmp1);
            cc.add(v_off, v_off, Imm(1));
        }
    }

    /* ---- EPILOGUE ---- */
    cc.bind(deform_out);

    /* tts_nvalid = natts */
    cc.mov(dtmp1, natts);
    cc.strh(dtmp1.w(), a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_nvalid)));

    /* slot->off = (uint32) off */
    cc.str(v_off.w(), a64::ptr(slot_reg, (int32_t) slot_off));

    /* tts_flags |= TTS_FLAG_SLOW */
    cc.ldrh(dtmp1.w(), a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_flags)));
    cc.orr(dtmp1, dtmp1, Imm(TTS_FLAG_SLOW));
    cc.strh(dtmp1.w(), a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_flags)));

    pfree(nvalid_labels);
    pfree(att_labels);
    pfree(null_labels);
    pfree(has_null_jump);
    pfree(avail_labels);
    pfree(has_avail_jump);

    return true;
}

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

	a64::Compiler cc(&code);

	/* Function: Datum fn(ExprState*, ExprContext*, bool*) */
	FuncNode *funcNode = cc.add_func(
		FuncSignature::build<int64_t, void *, void *, void *>());

	/* Arguments */
	a64::Gp v_state = cc.new_gpx("state");
	a64::Gp v_econtext = cc.new_gpx("econtext");
	a64::Gp v_isnullp = cc.new_gpx("isNull");

	funcNode->set_arg(0, v_state);
	funcNode->set_arg(1, v_econtext);
	funcNode->set_arg(2, v_isnullp);

	/* Scratch registers */
	a64::Gp tmp1 = cc.new_gpx("tmp1");
	a64::Gp tmp2 = cc.new_gpx("tmp2");
	a64::Gp tmp3 = cc.new_gpx("tmp3");
	a64::Gp slot_reg = cc.new_gpx("slot");

	/* Cached pointers (loaded once in prologue) */
	a64::Gp v_resvaluep = cc.new_gpx("resvaluep");
	a64::Gp v_resnullp = cc.new_gpx("resnullp");
	a64::Gp v_resultvals = cc.new_gpx("resultvals");
	a64::Gp v_resultnulls = cc.new_gpx("resultnulls");

	/* Load cached pointers */
	/* resvaluep = &state->resvalue */
	cc.mov(v_resvaluep, v_state);
	cc.add(v_resvaluep, v_resvaluep, (int64_t) offsetof(ExprState, resvalue));
	/* resnullp = &state->resnull */
	cc.mov(v_resnullp, v_state);
	cc.add(v_resnullp, v_resnullp, (int64_t) offsetof(ExprState, resnull));
	/* resultslot = state->resultslot (may be NULL for non-projecting exprs) */
	cc.ldr(slot_reg, a64::ptr(v_state, offsetof(ExprState, resultslot)));
	{
		Label skip_rs = cc.new_label();
		cc.cbz(slot_reg, skip_rs);
		/* resultvals = resultslot->tts_values */
		cc.ldr(v_resultvals, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
		/* resultnulls = resultslot->tts_isnull */
		cc.ldr(v_resultnulls, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
		cc.bind(skip_rs);
	}

	/* Create labels for each step */
	Label *step_labels = (Label *) palloc(sizeof(Label) * steps_len);
	for (int i = 0; i < steps_len; i++)
		step_labels[i] = cc.new_label();

	/* Main loop: emit code for each step */
	for (int opno = 0; opno < steps_len; opno++)
	{
		ExprEvalStep *op = &steps[opno];
		ExprEvalOp	opcode = ExecEvalStepOp(state, op);

		cc.bind(step_labels[opno]);

		switch (opcode)
		{
			case EEOP_DONE_RETURN:
			{
				/* Load state->resvalue and state->resnull */
				cc.ldr(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				/* *isNull = resnull */
				cc.strb(tmp2.w(), a64::ptr(v_isnullp));
				/* return resvalue */
				cc.ret(tmp1);
				break;
			}

			case EEOP_DONE_NO_RETURN:
			{
				cc.mov(tmp1, 0);
				cc.ret(tmp1);
				break;
			}

			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
			case EEOP_OLD_FETCHSOME:
			case EEOP_NEW_FETCHSOME:
			{
				Label skip = cc.new_label();
				int64_t soff = slot_offset_for_opcode(opcode);
				bool deform_emitted = false;

				/* slot = econtext->ecxt_*tuple */
				cc.ldr(slot_reg, a64::ptr(v_econtext, soff));
				/* tts_nvalid (AttrNumber = int16) */
				cc.ldrsh(tmp1.w(), a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_nvalid)));
				cc.cmp(tmp1.w(), op->d.fetch.last_var);
				cc.b_ge(skip);

				/* Try inline deform if conditions allow */
				if (op->d.fetch.fixed && op->d.fetch.known_desc &&
					(ctx->base.flags & PGJIT_DEFORM))
				{
					/*
					 * Try pre-compiled template first (I-cache friendly:
					 * same virtual address across all parallel workers).
					 */
					deform_template_fn tmpl = asmjit_deform_match_template(
						op->d.fetch.known_desc,
						op->d.fetch.kind,
						op->d.fetch.last_var);

					if (tmpl)
					{
						/* slot_reg already has the slot pointer; call tmpl(slot) */
						a64::Gp tmpl_fn = cc.new_gpx();
						cc.mov(tmpl_fn, (uint64_t)(void *) tmpl);
						InvokeNode *inv;
						cc.invoke(Out(inv), tmpl_fn,
								  FuncSignature::build<void, void *>());
						inv->set_arg(0, slot_reg);
						deform_emitted = true;
					}
					else
					{
						instr_time  deform_start, deform_end;

						INSTR_TIME_SET_CURRENT(deform_start);
						deform_emitted = asmjit_emit_deform_inline(cc,
						                                            op->d.fetch.known_desc,
						                                            op->d.fetch.kind,
						                                            op->d.fetch.last_var,
						                                            slot_reg,
						                                            tmp1, tmp2, tmp3);
						INSTR_TIME_SET_CURRENT(deform_end);
						INSTR_TIME_ACCUM_DIFF(ctx->base.instr.deform_counter,
						                      deform_end, deform_start);
					}
				}

				if (!deform_emitted)
				{
					/* Fallback: call slot_getsomeattrs_int(slot, last_var) */
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) slot_getsomeattrs_int);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, int>());
					invoke->set_arg(0, slot_reg);
					invoke->set_arg(1, Imm(op->d.fetch.last_var));
				}

				cc.bind(skip);
				break;
			}

			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
			case EEOP_OLD_VAR:
			case EEOP_NEW_VAR:
			{
				int attnum = op->d.var.attnum;
				int64_t soff = slot_offset_for_opcode(opcode);

				cc.ldr(slot_reg, a64::ptr(v_econtext, soff));

				/* values_ptr = slot->tts_values; value = values_ptr[attnum] */
				cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				cc.ldr(tmp2, a64::ptr(tmp1, attnum * (int64_t) sizeof(Datum)));
				/* *op->resvalue = value */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.str(tmp2, a64::ptr(tmp3));

				/* isnull_ptr = slot->tts_isnull; isnull = isnull_ptr[attnum] */
				cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1, attnum * (int64_t) sizeof(bool)));
				/* *op->resnull = isnull */
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.strb(tmp2.w(), a64::ptr(tmp3));
				break;
			}

			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
			case EEOP_ASSIGN_OLD_VAR:
			case EEOP_ASSIGN_NEW_VAR:
			{
				int attnum = op->d.assign_var.attnum;
				int resultnum = op->d.assign_var.resultnum;
				int64_t soff = slot_offset_for_opcode(opcode);

				/* source slot */
				cc.ldr(slot_reg, a64::ptr(v_econtext, soff));

				/* Load value from source */
				cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				cc.ldr(tmp2, a64::ptr(tmp1, attnum * (int64_t) sizeof(Datum)));
				/* Store to result slot */
				cc.str(tmp2, a64::ptr(v_resultvals, resultnum * (int64_t) sizeof(Datum)));

				/* Load null from source */
				cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1, attnum * (int64_t) sizeof(bool)));
				/* Store to result slot */
				cc.strb(tmp2.w(), a64::ptr(v_resultnulls, resultnum * (int64_t) sizeof(bool)));
				break;
			}

			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
			{
				int resultnum = op->d.assign_tmp.resultnum;

				/* Load state->resvalue and state->resnull */
				cc.ldr(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));

				/* Store null to result slot */
				cc.strb(tmp2.w(), a64::ptr(v_resultnulls, resultnum * (int64_t) sizeof(bool)));

				if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				{
					Label skip_ro = cc.new_label();
					cc.cbnz(tmp2, skip_ro);  /* if null, skip */

					/* tmp1 = MakeExpandedObjectReadOnlyInternal(tmp1) */
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) MakeExpandedObjectReadOnlyInternal);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					invoke->set_arg(0, tmp1);
					invoke->set_ret(0, tmp1);

					cc.bind(skip_ro);
				}

				/* Store value to result slot */
				cc.str(tmp1, a64::ptr(v_resultvals, resultnum * (int64_t) sizeof(Datum)));
				break;
			}

			case EEOP_CONST:
			{
				/* *op->resvalue = constval.value */
				cc.mov(tmp1, (int64_t) op->d.constval.value);
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.str(tmp1, a64::ptr(tmp2));
				/* *op->resnull = constval.isnull */
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(tmp1, op->d.constval.isnull ? 1 : 0);
				cc.strb(tmp1.w(), a64::ptr(tmp2));
				break;
			}

			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int nargs = op->d.func.nargs;
				Label done_label = cc.new_label();

				if (opcode == EEOP_FUNCEXPR_STRICT ||
					opcode == EEOP_FUNCEXPR_STRICT_1 ||
					opcode == EEOP_FUNCEXPR_STRICT_2)
				{
					/* Set resnull = true (will be overwritten if func called) */
					cc.mov(tmp1, (uint64_t) op->resnull);
					cc.mov(tmp2, 1);
					cc.strb(tmp2.w(), a64::ptr(tmp1));

					/*
					 * Batched null check: OR all isnull flags together,
					 * single branch at the end.  Reduces branch mispredicts
					 * for multi-arg strict functions.
					 */
					cc.mov(tmp1, (uint64_t) fcinfo);
					if (nargs > 1 && nargs <= 4)
					{
						int64_t null_off_0 =
							(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
						cc.ldrb(tmp2.w(), a64::ptr(tmp1, null_off_0));
						for (int argno = 1; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull - (char *)fcinfo);
							a64::Gp t = cc.new_gpx();
							cc.ldrb(t.w(), a64::ptr(tmp1, null_off));
							cc.orr(tmp2, tmp2, t);
						}
						cc.cbnz(tmp2, done_label);
					}
					else
					{
						/* 1 arg or >4 args: per-arg check */
						for (int argno = 0; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull - (char *)fcinfo);
							cc.ldrb(tmp2.w(), a64::ptr(tmp1, null_off));
							cc.cbnz(tmp2, done_label);
						}
					}
				}

				/*
				 * Try direct native call - bypasses fcinfo entirely.
				 * Dispatch order: inline → direct call → fcinfo fallback.
				 */
				{
				const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);

				if (dfn && dfn->inline_op != JIT_INLINE_NONE)
				{
					/*
					 * TIER 0 — INLINE: emit the operation as native ARM64
					 * instructions, no function call at all.
					 */
					a64::Gp fci_base = cc.new_gpx();
					a64::Gp a0 = cc.new_gpx(), a1 = cc.new_gpx();
					cc.mov(fci_base, (uint64_t) fcinfo);
					int64_t val_off_0 =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					int64_t val_off_1 =
						(int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					cc.ldr(a0, a64::ptr(fci_base, val_off_0));
					cc.ldr(a1, a64::ptr(fci_base, val_off_1));

					switch ((JitInlineOp) dfn->inline_op)
					{
					/* ---- int32 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT4_ADD:
					{
						Label ok = cc.new_label();
						cc.adds(a0.w(), a0.w(), a1.w());
						cc.b_vc(ok);
						a64::Gp err_fn = cc.new_gpx();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.sxtw(tmp1, a0.w());
						break;
					}
					case JIT_INLINE_INT4_SUB:
					{
						Label ok = cc.new_label();
						cc.subs(a0.w(), a0.w(), a1.w());
						cc.b_vc(ok);
						a64::Gp err_fn = cc.new_gpx();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.sxtw(tmp1, a0.w());
						break;
					}
					case JIT_INLINE_INT4_MUL:
					{
						Label ok = cc.new_label();
						/* smull: 32×32→64, overflow if result != sign-extend(low32) */
						cc.smull(tmp1, a0.w(), a1.w());
						a64::Gp ext = cc.new_gpx();
						cc.sxtw(ext, tmp1.w());
						cc.cmp(tmp1, ext);
						cc.b_eq(ok);
						a64::Gp err_fn = cc.new_gpx();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						/* tmp1 already holds sign-extended result from smull */
						break;
					}
					case JIT_INLINE_INT4_DIV:
					{
						Label not_zero = cc.new_label();
						Label not_minmax = cc.new_label();
						/* Check divisor == 0 */
						cc.cmp(a1.w(), 0);
						cc.b_ne(not_zero);
						{
							a64::Gp err_fn = cc.new_gpx();
							cc.mov(err_fn, (uint64_t)(void *) jit_error_division_by_zero);
							InvokeNode *inv;
							cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						}
						cc.bind(not_zero);
						/* Check INT32_MIN / -1 overflow */
						{
							a64::Gp minval = cc.new_gpx();
							cc.mov(minval.w(), (int32_t) PG_INT32_MIN);
							cc.cmp(a0.w(), minval.w());
						}
						cc.b_ne(not_minmax);
						{
							Label not_neg1 = cc.new_label();
							cc.cmn(a1.w(), 1);  /* cmn w, 1 == cmp w, -1 */
							cc.b_ne(not_neg1);
							a64::Gp err_fn = cc.new_gpx();
							cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
							InvokeNode *inv;
							cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
							cc.bind(not_neg1);
						}
						cc.bind(not_minmax);
						cc.sdiv(a0.w(), a0.w(), a1.w());
						cc.sxtw(tmp1, a0.w());
						break;
					}
					case JIT_INLINE_INT4_MOD:
					{
						Label not_zero = cc.new_label();
						Label not_minmax = cc.new_label();
						Label zero_result = cc.new_label();
						Label after = cc.new_label();
						/* Check divisor == 0 */
						cc.cmp(a1.w(), 0);
						cc.b_ne(not_zero);
						{
							a64::Gp err_fn = cc.new_gpx();
							cc.mov(err_fn, (uint64_t)(void *) jit_error_division_by_zero);
							InvokeNode *inv;
							cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						}
						cc.bind(not_zero);
						/* Check INT32_MIN % -1 → 0 */
						{
							a64::Gp minval = cc.new_gpx();
							cc.mov(minval.w(), (int32_t) PG_INT32_MIN);
							cc.cmp(a0.w(), minval.w());
						}
						cc.b_ne(not_minmax);
						cc.cmn(a1.w(), 1);
						cc.b_eq(zero_result);
						cc.bind(not_minmax);
						{
							a64::Gp q = cc.new_gpx();
							cc.sdiv(q.w(), a0.w(), a1.w());
							cc.msub(a0.w(), q.w(), a1.w(), a0.w());
						}
						cc.sxtw(tmp1, a0.w());
						cc.b(after);
						cc.bind(zero_result);
						cc.mov(tmp1, 0);
						cc.bind(after);
						break;
					}
					/* ---- int64 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT8_ADD:
					{
						Label ok = cc.new_label();
						cc.adds(a0, a0, a1);
						cc.b_vc(ok);
						a64::Gp err_fn = cc.new_gpx();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int8_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.mov(tmp1, a0);
						break;
					}
					case JIT_INLINE_INT8_SUB:
					{
						Label ok = cc.new_label();
						cc.subs(a0, a0, a1);
						cc.b_vc(ok);
						a64::Gp err_fn = cc.new_gpx();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int8_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.mov(tmp1, a0);
						break;
					}
					case JIT_INLINE_INT8_MUL:
					{
						Label ok = cc.new_label();
						/* Use smulh to check overflow */
						a64::Gp hi = cc.new_gpx();
						cc.smulh(hi, a0, a1);
						cc.mul(a0, a0, a1);
						/* Overflow if hi != (a0 >> 63) (arithmetic shift) */
						a64::Gp sign = cc.new_gpx();
						cc.asr(sign, a0, 63);
						cc.cmp(hi, sign);
						cc.b_eq(ok);
						a64::Gp err_fn = cc.new_gpx();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int8_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.mov(tmp1, a0);
						break;
					}
					/* ---- int32 comparison ---- */
					case JIT_INLINE_INT4_EQ:
						cc.cmp(a0.w(), a1.w());
						cc.cset(tmp1, a64::CondCode::kEQ);
						break;
					case JIT_INLINE_INT4_NE:
						cc.cmp(a0.w(), a1.w());
						cc.cset(tmp1, a64::CondCode::kNE);
						break;
					case JIT_INLINE_INT4_LT:
						cc.cmp(a0.w(), a1.w());
						cc.cset(tmp1, a64::CondCode::kLT);
						break;
					case JIT_INLINE_INT4_LE:
						cc.cmp(a0.w(), a1.w());
						cc.cset(tmp1, a64::CondCode::kLE);
						break;
					case JIT_INLINE_INT4_GT:
						cc.cmp(a0.w(), a1.w());
						cc.cset(tmp1, a64::CondCode::kGT);
						break;
					case JIT_INLINE_INT4_GE:
						cc.cmp(a0.w(), a1.w());
						cc.cset(tmp1, a64::CondCode::kGE);
						break;
					/* ---- int64 comparison ---- */
					case JIT_INLINE_INT8_EQ:
						cc.cmp(a0, a1);
						cc.cset(tmp1, a64::CondCode::kEQ);
						break;
					case JIT_INLINE_INT8_NE:
						cc.cmp(a0, a1);
						cc.cset(tmp1, a64::CondCode::kNE);
						break;
					case JIT_INLINE_INT8_LT:
						cc.cmp(a0, a1);
						cc.cset(tmp1, a64::CondCode::kLT);
						break;
					case JIT_INLINE_INT8_LE:
						cc.cmp(a0, a1);
						cc.cset(tmp1, a64::CondCode::kLE);
						break;
					case JIT_INLINE_INT8_GT:
						cc.cmp(a0, a1);
						cc.cset(tmp1, a64::CondCode::kGT);
						break;
					case JIT_INLINE_INT8_GE:
						cc.cmp(a0, a1);
						cc.cset(tmp1, a64::CondCode::kGE);
						break;
					default:
						Assert(false); /* all inline ops handled above */
						break;
					}

					/* Store *op->resvalue = tmp1 */
					cc.mov(tmp2, (uint64_t) op->resvalue);
					cc.str(tmp1, a64::ptr(tmp2));

					/* *op->resnull = false */
					cc.mov(tmp2, (uint64_t) op->resnull);
					cc.mov(tmp1, 0);
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				else if (dfn && (dfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
						|| (dfn->jit_fn_name &&
							mir_find_precompiled_fn(dfn->jit_fn_name))
#endif
						))
				{
					/*
					 * Direct native call — either from dfn->jit_fn or
					 * MIR-precompiled function pointer.
					 */
					void *call_target = dfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!call_target)
						call_target = mir_find_precompiled_fn(dfn->jit_fn_name);
#endif
					/* Load fcinfo once, then load all args from offsets */
					a64::Gp args[4];
					a64::Gp fci_base = cc.new_gpx();
					cc.mov(fci_base, (uint64_t) fcinfo);
					for (int i = 0; i < dfn->nargs; i++)
					{
						args[i] = cc.new_gpx();
						int64_t val_off =
							(int64_t)((char *)&fcinfo->args[i].value - (char *)fcinfo);
						cc.ldr(args[i], a64::ptr(fci_base, val_off));
					}

					/* Direct call */
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t) call_target);

					InvokeNode *invoke;
					switch (dfn->nargs) {
					case 0:
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t>());
						break;
					case 1:
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int64_t>());
						invoke->set_arg(0, args[0]);
						break;
					case 2:
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int64_t, int64_t>());
						invoke->set_arg(0, args[0]);
						invoke->set_arg(1, args[1]);
						break;
					default:
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int64_t, int64_t, int64_t>());
						invoke->set_arg(0, args[0]);
						invoke->set_arg(1, args[1]);
						invoke->set_arg(2, args[2]);
						break;
					}
					invoke->set_ret(0, tmp1);

					/* *op->resvalue = result */
					cc.mov(tmp2, (uint64_t) op->resvalue);
					cc.str(tmp1, a64::ptr(tmp2));

					/* *op->resnull = false */
					cc.mov(tmp2, (uint64_t) op->resnull);
					cc.mov(tmp1, 0);
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				else
				{
				/* Fallback: generic fcinfo path */
				{
					a64::Gp fci = cc.new_gpx();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci, offsetof(FunctionCallInfoBaseData, isnull)));
				}
				{
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.func.fn_addr);
					a64::Gp fcinfo_reg = cc.new_gpx();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.str(tmp1, a64::ptr(tmp2));
				cc.mov(tmp1, (uint64_t) fcinfo);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1, offsetof(FunctionCallInfoBaseData, isnull)));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.strb(tmp2.w(), a64::ptr(tmp1));
				} /* end else fallback */
				} /* end direct-call dispatch */

				cc.bind(done_label);
				break;
			}
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_LAST:
			{
				Label null_handler = cc.new_label();
				Label cont = cc.new_label();

				if (opcode == EEOP_BOOL_AND_STEP_FIRST)
				{
					cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				/* Load resnull */
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				cc.cbnz(tmp2, null_handler);

				/* Not null: check false */
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.ldr(tmp2, a64::ptr(tmp1));
				cc.cbz(tmp2, step_labels[op->d.boolexpr.jumpdone]);

				cc.b(cont);

				/* Null handler: set anynull */
				cc.bind(null_handler);
				cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
				cc.mov(tmp2, 1);
				cc.strb(tmp2.w(), a64::ptr(tmp1));

				cc.bind(cont);

				/* On last step: if anynull, set result to NULL */
				if (opcode == EEOP_BOOL_AND_STEP_LAST)
				{
					Label no_anynull = cc.new_label();
					cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					cc.cbz(tmp2, no_anynull);

					cc.mov(tmp1, (uint64_t) op->resnull);
					cc.mov(tmp2, 1);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
					cc.mov(tmp1, (uint64_t) op->resvalue);
					cc.mov(tmp2, 0);
					cc.str(tmp2, a64::ptr(tmp1));

					cc.bind(no_anynull);
				}
				break;
			}

			case EEOP_BOOL_OR_STEP_FIRST:
			case EEOP_BOOL_OR_STEP:
			case EEOP_BOOL_OR_STEP_LAST:
			{
				Label null_handler = cc.new_label();
				Label cont = cc.new_label();

				if (opcode == EEOP_BOOL_OR_STEP_FIRST)
				{
					cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				cc.cbnz(tmp2, null_handler);

				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.ldr(tmp2, a64::ptr(tmp1));
				cc.cbnz(tmp2, step_labels[op->d.boolexpr.jumpdone]);

				cc.b(cont);

				cc.bind(null_handler);
				cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
				cc.mov(tmp2, 1);
				cc.strb(tmp2.w(), a64::ptr(tmp1));

				cc.bind(cont);

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					Label no_anynull = cc.new_label();
					cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					cc.cbz(tmp2, no_anynull);

					cc.mov(tmp1, (uint64_t) op->resnull);
					cc.mov(tmp2, 1);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
					cc.mov(tmp1, (uint64_t) op->resvalue);
					cc.mov(tmp2, 0);
					cc.str(tmp2, a64::ptr(tmp1));

					cc.bind(no_anynull);
				}
				break;
			}

			case EEOP_BOOL_NOT_STEP:
			{
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.ldr(tmp2, a64::ptr(tmp1));
				cc.cmp(tmp2, 0);
				cc.cset(tmp2, a64::CondCode::kEQ);
				cc.str(tmp2, a64::ptr(tmp1));
				break;
			}

			case EEOP_QUAL:
			{
				Label qualfail = cc.new_label();
				Label cont = cc.new_label();

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				cc.cbnz(tmp2, qualfail);

				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.ldr(tmp2, a64::ptr(tmp1));
				cc.cbz(tmp2, qualfail);

				cc.b(cont);

				cc.bind(qualfail);
				/* Set resvalue=0, resnull=false, jump to done */
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(tmp2, 0);
				cc.strb(tmp2.w(), a64::ptr(tmp1));
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.str(tmp2, a64::ptr(tmp1));
				cc.b(step_labels[op->d.qualexpr.jumpdone]);

				cc.bind(cont);
				break;
			}

			case EEOP_JUMP:
				cc.b(step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NULL:
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				cc.cbnz(tmp2, step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NOT_NULL:
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				cc.cbz(tmp2, step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NOT_TRUE:
			{
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				cc.cbnz(tmp2, step_labels[op->d.jump.jumpdone]);

				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.ldr(tmp2, a64::ptr(tmp1));
				cc.cbz(tmp2, step_labels[op->d.jump.jumpdone]);
				break;
			}

			case EEOP_NULLTEST_ISNULL:
			{
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				/* resvalue = (resnull == true) ? 1 : 0 */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				/* tmp2 already has the null flag as 0/1 */
				cc.str(tmp2, a64::ptr(tmp3));
				/* resnull = false */
				cc.mov(tmp2, 0);
				cc.strb(tmp2.w(), a64::ptr(tmp1));
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				/* resvalue = (resnull == 0) ? 1 : 0 */
				cc.cmp(tmp2, 0);
				cc.cset(tmp2, a64::CondCode::kEQ);
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.str(tmp2, a64::ptr(tmp3));
				/* resnull = false */
				cc.mov(tmp2, 0);
				cc.strb(tmp2.w(), a64::ptr(tmp1));
				break;
			}

			case EEOP_HASHDATUM_SET_INITVAL:
			{
				/* *op->resvalue = init_value */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(tmp1, (int64_t) op->d.hashdatum_initvalue.init_value);
				cc.str(tmp1, a64::ptr(tmp3));
				/* *op->resnull = false */
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 0);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				break;
			}

			case EEOP_HASHDATUM_FIRST:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label store_zero = cc.new_label();
				Label store_result = cc.new_label();

				/* Check if arg is null */
				a64::Gp fci_reg = cc.new_gpx();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, store_zero);

				/* Not null: call hash function (direct or fcinfo) */
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: reuse fci_reg for arg load */
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					cc.mov(hfn_reg, (uint64_t) hash_target);
				}
					InvokeNode *hinv;
					cc.invoke(Out(hinv), hfn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					hinv->set_arg(0, hash_arg);
					hinv->set_ret(0, tmp1);
				}
				else
				{
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)));
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.b(store_result);

				cc.bind(store_zero);
				cc.mov(tmp1, 0);

				cc.bind(store_result);
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.str(tmp1, a64::ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 0);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				break;
			}

			case EEOP_HASHDATUM_FIRST_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label is_null = cc.new_label();
				Label done = cc.new_label();

				/* Check if arg is null */
				a64::Gp fci_reg = cc.new_gpx();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, is_null);

				/* Not null: call hash function (direct or fcinfo) */
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: reuse fci_reg for arg load */
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					cc.mov(hfn_reg, (uint64_t) hash_target);
				}
					InvokeNode *hinv;
					cc.invoke(Out(hinv), hfn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					hinv->set_arg(0, hash_arg);
					hinv->set_ret(0, tmp1);
				}
				else
				{
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)));
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				/* *op->resvalue = result */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.str(tmp1, a64::ptr(tmp3));
				/* *op->resnull = false */
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 0);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				cc.b(done);

				/* Null path: resnull=1, resvalue=0, jump to jumpdone */
				cc.bind(is_null);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 1);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(tmp1, 0);
				cc.str(tmp1, a64::ptr(tmp3));
				cc.b(step_labels[op->d.hashdatum.jumpdone]);

				cc.bind(done);
				break;
			}

			case EEOP_HASHDATUM_NEXT32:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				NullableDatum *iresult = op->d.hashdatum.iresult;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label skip_hash = cc.new_label();

				/* Load existing hash from iresult->value (32-bit) */
				a64::Gp hash = cc.new_gpx("hash");
				cc.mov(tmp1, (uint64_t) &iresult->value);
				cc.ldr(hash.w(), a64::ptr(tmp1));

				/* Rotate left 1 = rotate right 31 */
				cc.ror(hash.w(), hash.w(), 31);

				/* Check if arg is null */
				a64::Gp fci_reg = cc.new_gpx();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, skip_hash);

				/* Not null: call hash function (direct or fcinfo), XOR */
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: reuse fci_reg for arg load */
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					cc.mov(hfn_reg, (uint64_t) hash_target);
				}
					InvokeNode *hinv;
					cc.invoke(Out(hinv), hfn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					hinv->set_arg(0, hash_arg);
					hinv->set_ret(0, tmp1);
				}
				else
				{
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)));
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.eor(hash.w(), hash.w(), tmp1.w());

				cc.bind(skip_hash);
				/* Store result (upper 32 bits zero from .w() ops) */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.str(hash, a64::ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 0);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				break;
			}

			case EEOP_HASHDATUM_NEXT32_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				NullableDatum *iresult = op->d.hashdatum.iresult;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label is_null = cc.new_label();
				Label done = cc.new_label();

				/* Check if arg is null first (strict) */
				a64::Gp fci_reg = cc.new_gpx();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, is_null);

				/* Load existing hash from iresult->value (32-bit) */
				a64::Gp hash = cc.new_gpx("hash");
				cc.mov(tmp1, (uint64_t) &iresult->value);
				cc.ldr(hash.w(), a64::ptr(tmp1));

				/* Rotate left 1 = rotate right 31 */
				cc.ror(hash.w(), hash.w(), 31);

				/* Call hash function (direct or fcinfo), XOR */
				{
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					/* Direct hash call: reuse fci_reg for arg load */
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					cc.mov(hfn_reg, (uint64_t) hash_target);
				}
					InvokeNode *hinv;
					cc.invoke(Out(hinv), hfn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					hinv->set_arg(0, hash_arg);
					hinv->set_ret(0, tmp1);
				}
				else
				{
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)));
					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				}
				cc.eor(hash.w(), hash.w(), tmp1.w());

				/* Store result */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.str(hash, a64::ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 0);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				cc.b(done);

				/* Null path: resnull=1, resvalue=0, jump to jumpdone */
				cc.bind(is_null);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(tmp1, 1);
				cc.strb(tmp1.w(), a64::ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(tmp1, 0);
				cc.str(tmp1, a64::ptr(tmp3));
				cc.b(step_labels[op->d.hashdatum.jumpdone]);

				cc.bind(done);
				break;
			}

			/*
			 * ---- AGG_PLAIN_TRANS (all 6 variants) ----
			 * Inline common aggregate transition functions to avoid
			 * function call overhead on the per-group hot path.
			 */
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_BYREF:
			{
				bool is_init = (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
								opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF);
				bool is_strict = (opcode != EEOP_AGG_PLAIN_TRANS_BYVAL &&
								  opcode != EEOP_AGG_PLAIN_TRANS_BYREF);
				bool is_byref = (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
								 opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF ||
								 opcode == EEOP_AGG_PLAIN_TRANS_BYREF);

				AggState *aggstate = castNode(AggState, state->parent);
				AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
				int setoff = op->d.agg_trans.setoff;
				int transno = op->d.agg_trans.transno;
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				PGFunction fn_addr = fcinfo->flinfo->fn_addr;
				ExprContext *aggcontext = op->d.agg_trans.aggcontext;

				Label end_label = cc.new_label();

				/* Compute pergroup at runtime: all_pergroups[setoff] + transno */
				a64::Gp pergroup = cc.new_gpx();
				{
					a64::Gp aggst = cc.new_gpx();
					cc.ldr(aggst, a64::ptr(v_state, offsetof(ExprState, parent)));
					cc.ldr(pergroup, a64::ptr(aggst, offsetof(AggState, all_pergroups)));
					cc.ldr(pergroup, a64::ptr(pergroup, setoff * (int64_t)sizeof(AggStatePerGroup)));
					if (transno != 0)
						cc.add(pergroup, pergroup, transno * (int64_t)sizeof(AggStatePerGroupData));
				}

				/* INIT check */
				if (is_init)
				{
					Label no_init = cc.new_label();
					cc.ldrb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, noTransValue)));
					cc.cbz(tmp1, no_init);

					/* Call ExecAggInitGroup(aggstate, pertrans, pergroup, aggcontext) */
					{
						a64::Gp aggst = cc.new_gpx();
						cc.ldr(aggst, a64::ptr(v_state, offsetof(ExprState, parent)));
						a64::Gp pt = cc.new_gpx();
						cc.mov(pt, (uint64_t) pertrans);
						a64::Gp ac = cc.new_gpx();
						cc.mov(ac, (uint64_t) aggcontext);
						a64::Gp fn = cc.new_gpx();
						cc.mov(fn, (uint64_t)(void *) ExecAggInitGroup);
						InvokeNode *inv;
						cc.invoke(Out(inv), fn, FuncSignature::build<void, void*, void*, void*, void*>());
						inv->set_arg(0, aggst);
						inv->set_arg(1, pt);
						inv->set_arg(2, pergroup);
						inv->set_arg(3, ac);
					}
					cc.b(end_label);
					cc.bind(no_init);
				}

				/* STRICT check: skip if transValueIsNull */
				if (is_strict)
				{
					cc.ldrb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
					cc.cbnz(tmp1, end_label);
				}

				/*
				 * Transition function dispatch — inline hot aggs.
				 */
				if (!is_byref &&
					(fn_addr == int8inc || fn_addr == int8inc_any))
				{
					/* COUNT: transValue += 1 (int64, overflow-checked) */
					a64::Gp tv = cc.new_gpx();
					cc.ldr(tv, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					Label ok = cc.new_label();
					cc.adds(tv, tv, 1);
					cc.b_vc(ok);
					{
						a64::Gp fn = cc.new_gpx();
						cc.mov(fn, (uint64_t)(void *) jit_error_int8_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), fn, FuncSignature::build<void>());
					}
					cc.bind(ok);
					cc.str(tv, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					cc.mov(tmp1, 0);
					cc.strb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
				}
				else if (!is_byref && fn_addr == int4_sum)
				{
					/*
					 * SUM(int4): transValue += (int64)arg1.
					 * Non-strict — handles NULL arg1 and first-call inline.
					 */
					Label arg_not_null = cc.new_label();
					Label trans_not_null = cc.new_label();
					Label after_sum = cc.new_label();

					/* Check arg1 isnull */
					int64_t isnull1_off = (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
					a64::Gp fci = cc.new_gpx();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.ldrb(tmp1.w(), a64::ptr(fci, isnull1_off));
					cc.cbz(tmp1, arg_not_null);
					cc.b(end_label); /* arg is null, skip */

					cc.bind(arg_not_null);
					/* Load arg1 value, sign-extend int32 → int64 */
					a64::Gp arg1 = cc.new_gpx();
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					cc.ldr(arg1, a64::ptr(fci, val1_off));
					cc.sxtw(arg1, arg1.w());

					/* Check transValueIsNull */
					cc.ldrb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
					cc.cbz(tmp1, trans_not_null);

					/* First non-null: transValue = (int64)arg1 */
					cc.str(arg1, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					cc.mov(tmp1, 0);
					cc.strb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
					cc.b(after_sum);

					/* Normal: transValue += (int64)arg1 */
					cc.bind(trans_not_null);
					{
						a64::Gp tv = cc.new_gpx();
						cc.ldr(tv, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
						cc.add(tv, tv, arg1);
						cc.str(tv, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					}
					cc.bind(after_sum);
				}
				else if (!is_byref &&
						 (fn_addr == int4smaller || fn_addr == int4larger))
				{
					/* MIN/MAX(int4): compare and conditionally update */
					bool is_min = (fn_addr == int4smaller);
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					a64::Gp tv = cc.new_gpx(), nv = cc.new_gpx();
					cc.ldr(tv, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					a64::Gp fci = cc.new_gpx();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.ldr(nv, a64::ptr(fci, val1_off));

					/* Sign-extend both for correct signed comparison */
					cc.sxtw(tv, tv.w());
					cc.sxtw(nv, nv.w());

					Label skip_update = cc.new_label();
					cc.cmp(tv, nv);
					if (is_min)
						cc.b_le(skip_update);
					else
						cc.b_ge(skip_update);
					cc.str(nv, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					cc.bind(skip_update);
					cc.mov(tmp1, 0);
					cc.strb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
				}
				else if (is_byref &&
						 (fn_addr == int4_avg_accum || fn_addr == int2_avg_accum))
				{
					/*
					 * AVG(int4/int2): in-place update of Int8TransTypeData
					 * inside the array at transValue + ARR_OVERHEAD_NONULLS(1).
					 */
					#define INT8_TRANS_DATA_OFFSET_ASM 24
					StaticAssertDecl(
						ARR_OVERHEAD_NONULLS(1) == INT8_TRANS_DATA_OFFSET_ASM,
						"Int8TransTypeData offset must be 24");

					a64::Gp tv_ptr = cc.new_gpx();
					cc.ldr(tv_ptr, a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					cc.add(tv_ptr, tv_ptr, INT8_TRANS_DATA_OFFSET_ASM);

					/* count++ */
					a64::Gp cnt = cc.new_gpx();
					cc.ldr(cnt, a64::ptr(tv_ptr, offsetof(JitInt8TransTypeData, count)));
					cc.add(cnt, cnt, 1);
					cc.str(cnt, a64::ptr(tv_ptr, offsetof(JitInt8TransTypeData, count)));

					/* Load arg1, sign-extend */
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					a64::Gp arg1 = cc.new_gpx();
					a64::Gp fci = cc.new_gpx();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.ldr(arg1, a64::ptr(fci, val1_off));
					if (fn_addr == int4_avg_accum)
						cc.sxtw(arg1, arg1.w());
					else
						cc.sxth(arg1, arg1.w());

					/* sum += arg1 */
					a64::Gp sum = cc.new_gpx();
					cc.ldr(sum, a64::ptr(tv_ptr, offsetof(JitInt8TransTypeData, sum)));
					cc.add(sum, sum, arg1);
					cc.str(sum, a64::ptr(tv_ptr, offsetof(JitInt8TransTypeData, sum)));

					cc.mov(tmp1, 0);
					cc.strb(tmp1.w(), a64::ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
					#undef INT8_TRANS_DATA_OFFSET_ASM
				}
				else
				{
					/* Generic fallback: call helper function */
					void *helper_fn;
					switch (opcode) {
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
					default:
						helper_fn = NULL; break;
					}
					a64::Gp afn_reg = cc.new_gpx();
					cc.mov(afn_reg, (uint64_t) helper_fn);
					a64::Gp aop_reg = cc.new_gpx();
					cc.mov(aop_reg, (uint64_t) op);
					InvokeNode *ainvoke;
					cc.invoke(Out(ainvoke), afn_reg,
							  FuncSignature::build<void, void *, void *>());
					ainvoke->set_arg(0, v_state);
					ainvoke->set_arg(1, aop_reg);
				}

				cc.bind(end_label);
				break;
			}

			/*
			 * ---- HASHED_SCALARARRAYOP (binary search) ----
			 *
			 * For constant byval IN lists: extract values at compile
			 * time, sort them, and emit an inline balanced binary
			 * search tree (~5 CMP+branch levels for 20 elements).
			 * For non-constant or byref types: C function fallback.
			 */
			case EEOP_HASHED_SCALARARRAYOP:
			{
				FunctionCallInfo fcinfo =
					op->d.hashedscalararrayop.fcinfo_data;
				bool inclause = op->d.hashedscalararrayop.inclause;
				ScalarArrayOpExpr *saop =
					op->d.hashedscalararrayop.saop;

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
									Datum vtmp = sorted_vals[a];
									int b = a - 1;
									while (b >= 0 &&
										   (int64) sorted_vals[b] >
										   (int64) vtmp)
									{
										sorted_vals[b + 1] =
											sorted_vals[b];
										b--;
									}
									sorted_vals[b + 1] = vtmp;
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
					 *   - equal -> found
					 *   - less -> go left subtree
					 *   - greater -> go right subtree (fall-through)
					 *   - leaf with no match -> not found
					 *
					 * All BEQ branches target lbl_found directly.
					 * All not-found leaves branch to lbl_not_found.
					 * BLT branches target per-node labels that are
					 * bound when the left subtree is emitted.
					 */
					Label lbl_found = cc.new_label();
					Label lbl_not_found = cc.new_label();
					Label lbl_null_result = cc.new_label();
					Label lbl_done = cc.new_label();

					int64_t off_arg0_value =
						(int64_t)((char *)&fcinfo->args[0].value -
								  (char *)fcinfo);
					int64_t off_arg0_isnull =
						(int64_t)((char *)&fcinfo->args[0].isnull -
								  (char *)fcinfo);

					/*
					 * Step 1: Check scalar not NULL (strict function).
					 */
					a64::Gp fci_reg = cc.new_gpx("fci");
					cc.mov(fci_reg, (uint64_t) fcinfo);
					cc.ldrb(tmp2.w(), a64::ptr(fci_reg, off_arg0_isnull));
					cc.cbnz(tmp2, lbl_null_result);

					/*
					 * Step 2: Load scalar value.
					 */
					a64::Gp scalar = cc.new_gpx("scalar");
					cc.ldr(scalar, a64::ptr(fci_reg, off_arg0_value));

					/*
					 * Step 3: Emit binary search tree.
					 *
					 * Work stack (LIFO) for iterative tree emission.
					 * Each item is (lo, hi, entry_label).
					 * Right subtree is pushed last (processed first =
					 * emitted as fall-through). Left subtree is pushed
					 * first (processed later = BLT target).
					 */
					{
						struct {
							int lo, hi;
							Label entry_label;
							bool has_entry;
						} work[128];
						int work_top = 0;

						/* Push initial range (no entry jump needed) */
						work[work_top].lo = 0;
						work[work_top].hi = nvals - 1;
						work[work_top].has_entry = false;
						work_top++;

						while (work_top > 0)
						{
							int lo, hi, mid;

							work_top--;
							lo = work[work_top].lo;
							hi = work[work_top].hi;

							/*
							 * Bind entry label if this node is a BLT
							 * target from a parent node.
							 */
							if (work[work_top].has_entry)
								cc.bind(work[work_top].entry_label);

							if (lo > hi)
							{
								/* Empty range -> not found */
								cc.b(lbl_not_found);
								continue;
							}

							if (lo == hi)
							{
								/*
								 * Single element — compare and branch.
								 * Load constant into register for CMP.
								 */
								a64::Gp cval = cc.new_gpx();
								cc.mov(cval,
									(uint64_t) sorted_vals[lo]);
								cc.cmp(scalar, cval);
								cc.b(a64::CondCode::kEQ, lbl_found);
								/* Not equal -> not found */
								cc.b(lbl_not_found);
								continue;
							}

							/*
							 * Pick median element.
							 */
							mid = lo + (hi - lo) / 2;

							/*
							 * CMP scalar, sorted_vals[mid]
							 * BEQ -> found
							 * BLT -> left subtree [lo, mid-1]
							 * fall-through -> right subtree [mid+1, hi]
							 */
							a64::Gp cval = cc.new_gpx();
							cc.mov(cval,
								(uint64_t) sorted_vals[mid]);
							cc.cmp(scalar, cval);
							cc.b(a64::CondCode::kEQ, lbl_found);

							Label lbl_lt = cc.new_label();
							cc.b(a64::CondCode::kLT, lbl_lt);

							/*
							 * Push left subtree first (processed
							 * last = emitted later = BLT target),
							 * then right subtree (processed next =
							 * emitted immediately = fall-through).
							 */
							work[work_top].lo = lo;
							work[work_top].hi = mid - 1;
							work[work_top].entry_label = lbl_lt;
							work[work_top].has_entry = true;
							work_top++;

							work[work_top].lo = mid + 1;
							work[work_top].hi = hi;
							work[work_top].has_entry = false;
							work_top++;
						}
					}

					/*
					 * ---- Found path ----
					 */
					cc.bind(lbl_found);
					{
						cc.mov(tmp3, (uint64_t) op->resvalue);
						cc.mov(tmp1,
							(uint64_t)(int64_t)(inclause ? 1 : 0));
						cc.str(tmp1, a64::ptr(tmp3));
						cc.mov(tmp3, (uint64_t) op->resnull);
						cc.mov(tmp1, 0);
						cc.strb(tmp1.w(), a64::ptr(tmp3));
						cc.b(lbl_done);
					}

					/*
					 * ---- Not found path ----
					 */
					cc.bind(lbl_not_found);
					{
						if (array_has_nulls)
						{
							/*
							 * Array had NULLs and value not found ->
							 * result is NULL (indeterminate for strict
							 * equality).
							 */
							cc.mov(tmp3, (uint64_t) op->resvalue);
							cc.mov(tmp1, 0);
							cc.str(tmp1, a64::ptr(tmp3));
							cc.mov(tmp3, (uint64_t) op->resnull);
							cc.mov(tmp1, 1);
							cc.strb(tmp1.w(), a64::ptr(tmp3));
						}
						else
						{
							cc.mov(tmp3, (uint64_t) op->resvalue);
							cc.mov(tmp1,
								(uint64_t)(int64_t)(inclause ? 0 : 1));
							cc.str(tmp1, a64::ptr(tmp3));
							cc.mov(tmp3, (uint64_t) op->resnull);
							cc.mov(tmp1, 0);
							cc.strb(tmp1.w(), a64::ptr(tmp3));
						}
						cc.b(lbl_done);
					}

					/*
					 * ---- Null scalar path ----
					 */
					cc.bind(lbl_null_result);
					{
						cc.mov(tmp3, (uint64_t) op->resvalue);
						cc.mov(tmp1, 0);
						cc.str(tmp1, a64::ptr(tmp3));
						cc.mov(tmp3, (uint64_t) op->resnull);
						cc.mov(tmp1, 1);
						cc.strb(tmp1.w(), a64::ptr(tmp3));
					}

					/* ---- Done ---- */
					cc.bind(lbl_done);

					pfree(sorted_vals);
				}
				else
				{
					/* Non-constant or byref — C function fallback */
					if (sorted_vals)
						pfree(sorted_vals);

					a64::Gp fn_reg = cc.new_gpx();
					cc.mov(fn_reg, (uint64_t)(void *) ExecEvalHashedScalarArrayOp);
					a64::Gp op_reg = cc.new_gpx();
					cc.mov(op_reg, (uint64_t) op);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
				}
				break;
			}

			default:
			{
				/* Fallback: call pg_jitter_fallback_step -> int */
				a64::Gp fn_reg = cc.new_gpx();
				cc.mov(fn_reg, (uint64_t)(void *) pg_jitter_fallback_step);
				a64::Gp op_reg = cc.new_gpx();
				cc.mov(op_reg, (uint64_t) op);
				InvokeNode *invoke;
				a64::Gp ret_reg = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_ret(0, ret_reg);

				/*
				 * Handle possible jump from fallback.
				 * Check the opcode at code-gen time to determine jump target.
				 */
				int fb_jump_target = -1;
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
					int jnull = op->d.rowcompare_step.jumpnull;
					int jdone = op->d.rowcompare_step.jumpdone;
					if (jnull >= 0 && jnull < steps_len)
					{
						cc.cmp(ret_reg, jnull);
						cc.b(a64::CondCode::kEQ, step_labels[jnull]);
					}
					if (jdone >= 0 && jdone < steps_len)
					{
						cc.cmp(ret_reg, jdone);
						cc.b(a64::CondCode::kEQ, step_labels[jdone]);
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
							cc.cmp(ret_reg, targets[t]);
							cc.b(a64::CondCode::kEQ, step_labels[targets[t]]);
						}
					}
				}
				else if (fb_jump_target >= 0 && fb_jump_target < steps_len)
				{
					cc.cmp(ret_reg, 0);
					cc.b(a64::CondCode::kGE, step_labels[fb_jump_target]);
				}
				break;
			}
		}
	}

	cc.end_func();

	Error err = cc.finalize();
	if (err != kErrorOk)
	{
		delete ac;
		pfree(step_labels);
		return false;
	}

	err = ac->rt.add(&ac->func, &code);
	if (err != kErrorOk)
	{
		delete ac;
		pfree(step_labels);
		return false;
	}

	/* Register for cleanup */
	pg_jitter_register_compiled(ctx, asmjit_code_free, ac);

	/* Set the eval function (with validation wrapper on first call) */
	pg_jitter_install_expr(state, (ExprStateEvalFunc) ac->func);

	pfree(step_labels);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(ctx->base.instr.generation_counter,
						  endtime, starttime);
	ctx->base.instr.created_functions++;

	return true;
}
