/*
 * pg_jitter_asmjit_arm64.inc — ARM64 code generation for AsmJIT backend
 *
 * Included by pg_jitter_asmjit.cpp when compiling for aarch64.
 * Defines asmjit_emit_all() and its helper asmjit_emit_deform_inline().
 */

/*
 * When true, emit 4-instruction MOVZ+3×MOVK sequences for all function
 * pointer loads, making the code relocatable across ASLR address spaces.
 * The relocation scanner in pg_jitter_common.c identifies these sequences
 * and patches them when workers load shared code from DSM.
 */
static bool asmjit_shared_code_mode = false;

/*
 * Force a 4-instruction absolute-address load for relocatable code sharing.
 * AsmJIT's cc.mov() may optimize to fewer instructions when upper bits are
 * zero, but the relocation scanner needs exactly MOVZ+3×MOVK to identify
 * and patch addresses.
 */
static void
emit_relocatable_mov(a64::Compiler &cc, a64::Gp dst, uint64_t val)
{
	cc.movz(dst, Imm((val >>  0) & 0xFFFF), a64::lsl(0));
	cc.movk(dst, Imm((val >> 16) & 0xFFFF), a64::lsl(16));
	cc.movk(dst, Imm((val >> 32) & 0xFFFF), a64::lsl(32));
	cc.movk(dst, Imm((val >> 48) & 0xFFFF), a64::lsl(48));
}

/* Load a function pointer into a register, using relocatable addressing
 * when asmjit_shared_code_mode is active. */
#define ASMJIT_MOV_FN(cc, dst, fn_ptr) \
	do { \
		if (asmjit_shared_code_mode) \
			emit_relocatable_mov(cc, dst, (uint64_t)(fn_ptr)); \
		else \
			cc.mov(dst, (uint64_t)(fn_ptr)); \
	} while (0)

/*
 * Detect a run of consecutive fixed-width NOT NULL byval columns
 * suitable for NEON batch loading.  Returns count of batchable columns
 * (0 if no batch possible).  Requires guaranteed alignment tracking.
 */
static int
asmjit_detect_neon_batch(TupleDesc desc, int start, int natts,
                         int known_alignment, bool attguaranteedalign,
                         int *out_attlen)
{
    if (!attguaranteedalign || known_alignment < 0)
        return 0;

    CompactAttribute *first = TupleDescCompactAttr(desc, start);
    if (!first->attbyval || !JITTER_ATT_IS_NOTNULL(first) ||
        first->atthasmissing || first->attisdropped)
        return 0;

    int attlen = first->attlen;
    if (attlen != 4 && attlen != 8)
        return 0;

    int count = 1;
    int ka = known_alignment + attlen;

    for (int j = start + 1; j < natts; j++)
    {
        CompactAttribute *att = TupleDescCompactAttr(desc, j);
        if (!att->attbyval || !JITTER_ATT_IS_NOTNULL(att) ||
            att->attlen != attlen ||
            att->atthasmissing || att->attisdropped)
            break;

        int alignto = JITTER_ATTALIGNBY(att);
        if (ka != TYPEALIGN(alignto, ka))
            break;

        count++;
        ka += attlen;
    }

    /* Minimum batch: 4 for int32, 2 for int64 */
    int min_batch = (attlen == 4) ? 4 : 2;
    if (count < min_batch)
        return 0;

    /* Round down to multiple of min_batch */
    count = (count / min_batch) * min_batch;

    *out_attlen = attlen;
    return count;
}

/*
 * Emit NEON batch load/store for a run of consecutive fixed-width
 * NOT NULL byval columns.  Loads from tuple data using compile-time
 * known offsets, sign-extends int32->int64 if needed, stores to
 * tts_values[], and zeros tts_isnull[].
 */
static void
asmjit_emit_neon_batch(a64::Compiler &cc,
                       a64::Gp v_tupdata_base,
                       a64::Gp v_tts_values,
                       a64::Gp v_tts_isnull,
                       a64::Gp v_off,
                       int attnum, int count, int attlen,
                       int known_alignment)
{
    if (attlen == 4)
    {
        /* int32 batch: load 4 at a time, sign-extend to int64 Datums */
        for (int i = 0; i < count; i += 4)
        {
            int src_off = known_alignment + i * 4;
            int dst_off = (attnum + i) * (int) sizeof(Datum);

            a64::Vec vi32 = cc.new_vec_q();
            a64::Vec vlo  = cc.new_vec_q();
            a64::Vec vhi  = cc.new_vec_q();

            a64::Gp saddr = cc.new_gpx();
            cc.add(saddr, v_tupdata_base, Imm(src_off));
            cc.ld1(vi32.s4(), a64::ptr(saddr));

            cc.sxtl(vlo.d2(), vi32.s2());
            cc.sxtl2(vhi.d2(), vi32.s4());

            a64::Gp daddr = cc.new_gpx();
            cc.add(daddr, v_tts_values, Imm(dst_off));
            cc.st1(vlo.d2(), a64::ptr(daddr));
            cc.add(daddr, daddr, Imm(16));
            cc.st1(vhi.d2(), a64::ptr(daddr));
        }
    }
    else /* attlen == 8 */
    {
        /* int64 batch: load 2 at a time, store directly as Datums */
        for (int i = 0; i < count; i += 2)
        {
            int src_off = known_alignment + i * 8;
            int dst_off = (attnum + i) * (int) sizeof(Datum);

            a64::Vec vi64 = cc.new_vec_q();

            a64::Gp saddr = cc.new_gpx();
            cc.add(saddr, v_tupdata_base, Imm(src_off));
            cc.ld1(vi64.d2(), a64::ptr(saddr));

            a64::Gp daddr = cc.new_gpx();
            cc.add(daddr, v_tts_values, Imm(dst_off));
            cc.st1(vi64.d2(), a64::ptr(daddr));
        }
    }

    /* Zero tts_isnull[] for the batch columns */
    int remain = count;
    int off = 0;
    while (remain >= 8)
    {
        cc.str(a64::xzr, a64::ptr(v_tts_isnull, attnum + off));
        off += 8;
        remain -= 8;
    }
    if (remain >= 4)
    {
        cc.str(a64::wzr, a64::ptr(v_tts_isnull, attnum + off));
        off += 4;
        remain -= 4;
    }
    if (remain >= 2)
    {
        cc.strh(a64::wzr, a64::ptr(v_tts_isnull, attnum + off));
        off += 2;
        remain -= 2;
    }
    if (remain >= 1)
        cc.strb(a64::wzr, a64::ptr(v_tts_isnull, attnum + off));

    /* Set v_off for the column after the batch */
    cc.mov(v_off, known_alignment + count * attlen);
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
    if (natts > pg_jitter_wide_deform_limit())
        return false;
    /* Wide tables: fall back to slot_getsomeattrs_int */
    if (natts > pg_jitter_deform_threshold())
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
            ASMJIT_MOV_FN(cc, fn_reg, (void *) slot_getmissingattrs);
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
        int alignto = JITTER_ATTALIGNBY(att);

        cc.bind(att_labels[attnum]);

        /* If attnum == 0: reset offset */
        if (attnum == 0)
            cc.mov(v_off, 0);

        /* ---- NEON batch acceleration ---- */
        {
            int neon_attlen = 0;
            int neon_count = asmjit_detect_neon_batch(desc, attnum, natts,
                                                       known_alignment,
                                                       attguaranteedalign,
                                                       &neon_attlen);
            if (neon_count > 0)
            {
                asmjit_emit_neon_batch(cc, v_tupdata_base, v_tts_values,
                                       v_tts_isnull, v_off,
                                       attnum, neon_count, neon_attlen,
                                       known_alignment);
                if (attnum + neon_count < natts)
                    cc.b(att_labels[attnum + neon_count]);
                else
                    cc.b(deform_out);
            }
        }

        /* ---- Availability check ---- */
        if (attnum > guaranteed_column_number)
        {
            cc.cmp(v_maxatt, attnum + 1);  /* if maxatt <= attnum, goto out */
            cc.b(a64::CondCode::kLT, deform_out);  /* maxatt < attnum+1 means attnum >= maxatt */
        }

        /* ---- Null check ---- */
        if (!JITTER_ATT_IS_NOTNULL(att))
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
            ASMJIT_MOV_FN(cc, fn_reg, (void *) varsize_any);
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
            ASMJIT_MOV_FN(cc, fn_reg, (void *) strlen);
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

/*
 * Map opcode to slot cache index (0=inner, 1=outer, 2=scan, 3=old, 4=new).
 * Returns -1 for unknown opcodes.
 */
static int
slot_cache_index(ExprEvalOp opcode)
{
	switch (opcode)
	{
		case EEOP_INNER_FETCHSOME:
		case EEOP_INNER_VAR:
		case EEOP_ASSIGN_INNER_VAR:
			return 0; /* SLOT_CACHE_INNER */
		case EEOP_OUTER_FETCHSOME:
		case EEOP_OUTER_VAR:
		case EEOP_ASSIGN_OUTER_VAR:
			return 1; /* SLOT_CACHE_OUTER */
		case EEOP_SCAN_FETCHSOME:
		case EEOP_SCAN_VAR:
		case EEOP_ASSIGN_SCAN_VAR:
			return 2; /* SLOT_CACHE_SCAN */
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_FETCHSOME:
		case EEOP_OLD_VAR:
		case EEOP_ASSIGN_OLD_VAR:
			return 3; /* SLOT_CACHE_OLD */
		case EEOP_NEW_FETCHSOME:
		case EEOP_NEW_VAR:
		case EEOP_ASSIGN_NEW_VAR:
			return 4; /* SLOT_CACHE_NEW */
#endif
		default:
			return -1;
	}
}

/*
 * Emit all expression steps as ARM64 native code.
 *
 * This is the arch-specific codegen body called from asmjit_compile_expr().
 */
static bool
asmjit_emit_all(CodeHolder &code, ExprState *state,
                PgJitterContext *ctx,
                ExprEvalStep *steps, int steps_len)
{
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

	/*
	 * Steps-relative addressing register.
	 * Loaded once from state->steps in the prologue.  Used to compute
	 * per-expression pointers (resvalue, resnull, fcinfo, etc.) at runtime
	 * instead of embedding absolute 64-bit immediates.  This makes the
	 * generated code position-independent (PIC) and shareable across
	 * parallel workers via DSM.
	 *
	 * Cost: 2 instructions (LDR steps + LDR field) vs 2-4 instructions
	 * (MOVZ+MOVK sequence) for absolute loads.
	 */
	a64::Gp v_steps = cc.new_gpx("steps");

	/* Load cached pointers */
	/* resvaluep = &state->resvalue */
	cc.mov(v_resvaluep, v_state);
	cc.add(v_resvaluep, v_resvaluep, (int64_t) offsetof(ExprState, resvalue));
	/* resnullp = &state->resnull */
	cc.mov(v_resnullp, v_state);
	cc.add(v_resnullp, v_resnullp, (int64_t) offsetof(ExprState, resnull));
	/* v_steps = state->steps (for steps-relative addressing) */
	cc.ldr(v_steps, a64::ptr(v_state, offsetof(ExprState, steps)));
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

	/*
	 * Steps-relative helper macros.
	 *
	 * STEP_OFFSET(opno, field): byte offset from steps base to field within
	 *                           steps[opno].
	 *
	 * These helpers load pointers from the steps array at runtime (PIC)
	 * instead of embedding absolute addresses.  Each load costs 1 LDR
	 * instruction using v_steps as the base register.
	 *
	 * The ARM64 LDR with unscaled immediate supports offsets up to ±256,
	 * while the scaled unsigned variant supports up to 32760 (for 8-byte).
	 * sizeof(ExprEvalStep) is typically 64 or 128 bytes, so for reasonable
	 * expression lengths the offset fits in a single instruction.  For very
	 * large offsets (>32760), AsmJIT's Compiler automatically handles
	 * materialization.
	 */
#define STEP_OFFSET(opno, field) \
	((int64_t)(opno) * (int64_t)sizeof(ExprEvalStep) + \
	 (int64_t)offsetof(ExprEvalStep, field))

	/*
	 * Slot caching: after FETCHSOME, cache the slot's tts_values and
	 * tts_isnull pointers so subsequent VAR/ASSIGN_VAR ops don't need
	 * to reload them via econtext->slot->tts_values each time.
	 *
	 * Indices: 0=inner, 1=outer, 2=scan, 3=old, 4=new
	 */
#define SLOT_CACHE_INNER  0
#define SLOT_CACHE_OUTER  1
#define SLOT_CACHE_SCAN   2
#define SLOT_CACHE_OLD    3
#define SLOT_CACHE_NEW    4
#define NUM_SLOT_CACHES   5

	a64::Gp v_cached_vals[NUM_SLOT_CACHES];
	a64::Gp v_cached_nulls[NUM_SLOT_CACHES];
	uint32_t slots_cached = 0;

	/* Pre-allocate virtual registers for each slot cache entry */
	for (int i = 0; i < NUM_SLOT_CACHES; i++)
	{
		v_cached_vals[i] = cc.new_gpx();
		v_cached_nulls[i] = cc.new_gpx();
	}

	/* Create labels for each step */
	Label *step_labels = (Label *) palloc(sizeof(Label) * steps_len);
	for (int i = 0; i < steps_len; i++)
		step_labels[i] = cc.new_label();

	/*
	 * Build jump-target bitmap: is_jump_target[i] is true if any other
	 * step can jump to step i.  Used to prevent batch VAR/ASSIGN_VAR
	 * from merging steps that are branch targets -- a jump into the
	 * middle of a batch would skip the batch's load code.
	 */
	bool *is_jump_target = (bool *) palloc0(sizeof(bool) * steps_len);
	for (int i = 0; i < steps_len; i++)
	{
		ExprEvalStep *s = &steps[i];
		ExprEvalOp sop = ExecEvalStepOp(state, s);
		int tgt = -1;

		switch (sop)
		{
			case EEOP_JUMP:
			case EEOP_JUMP_IF_NULL:
			case EEOP_JUMP_IF_NOT_NULL:
			case EEOP_JUMP_IF_NOT_TRUE:
				tgt = s->d.jump.jumpdone;
				break;
			case EEOP_QUAL:
				tgt = s->d.qualexpr.jumpdone;
				break;
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP_LAST:
			case EEOP_BOOL_OR_STEP:
			case EEOP_BOOL_OR_STEP_FIRST:
			case EEOP_BOOL_OR_STEP_LAST:
				tgt = s->d.boolexpr.jumpdone;
				break;
			case EEOP_ROWCOMPARE_STEP:
				if (s->d.rowcompare_step.jumpnull >= 0 &&
					s->d.rowcompare_step.jumpnull < steps_len)
					is_jump_target[s->d.rowcompare_step.jumpnull] = true;
				tgt = s->d.rowcompare_step.jumpdone;
				break;
			case EEOP_SBSREF_SUBSCRIPTS:
				tgt = s->d.sbsref_subscript.jumpdone;
				break;
#ifdef HAVE_EEOP_HASHDATUM
			case EEOP_HASHDATUM_FIRST:
			case EEOP_HASHDATUM_FIRST_STRICT:
			case EEOP_HASHDATUM_NEXT32:
			case EEOP_HASHDATUM_NEXT32_STRICT:
				tgt = s->d.hashdatum.jumpdone;
				break;
#endif
			case EEOP_AGG_DESERIALIZE:
			case EEOP_AGG_STRICT_DESERIALIZE:
				tgt = s->d.agg_deserialize.jumpnull;
				break;
			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
				tgt = s->d.agg_strict_input_check.jumpnull;
				break;
			case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
				tgt = s->d.agg_plain_pergroup_nullcheck.jumpnull;
				break;
#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
			case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
			case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
				tgt = s->d.agg_presorted_distinctcheck.jumpdistinct;
				break;
#endif
#ifdef HAVE_EEOP_RETURNINGEXPR
			case EEOP_RETURNINGEXPR:
				tgt = s->d.returningexpr.jumpdone;
				break;
#endif
			default:
				break;
		}
		if (tgt >= 0 && tgt < steps_len)
			is_jump_target[tgt] = true;
	}

	/*
	 * Vectorized QUAL batch detection.
	 *
	 * Find groups of QUAL steps with the same jumpdone target where:
	 *   (a) no intermediate step is a jump target
	 *   (b) all intermediate steps are provably side-effect-free
	 *
	 * Eliminating short-circuit evaluation changes observable behavior if
	 * any intermediate function has side effects (NOTICE, error, etc.).
	 * We only batch when all steps between QUALs are safe to speculate:
	 *   - VAR (inner/outer/scan/old/new): pure memory loads
	 *   - CONST: immediate value
	 *   - FUNCEXPR_STRICT where jit_find_direct_fn() succeeds: built-in
	 *     comparison/arithmetic, guaranteed side-effect-free
	 *   - ASSIGN_*_VAR: pure stores to result slot
	 *   - FETCHSOME: deform dispatch (reads only)
	 *
	 * qual_batch_last[i] = index of last QUAL in the batch containing
	 * step i (0 if step i is not a QUAL in a batch of size >= 2).
	 *
	 * Fast-path: skip the entire scan if fewer than 2 QUALs exist, since
	 * batching requires at least 2.  This avoids compile-time overhead
	 * and register pressure for expressions that can't benefit.
	 */
	int *qual_batch_last = NULL;
	bool has_qual_batches = false;
	{
		/* Quick count of QUAL steps */
		int qual_count = 0;
		for (int i = 0; i < steps_len && qual_count < 2; i++)
		{
			if (ExecEvalStepOp(state, &steps[i]) == EEOP_QUAL)
				qual_count++;
		}

		if (qual_count >= 2)
		{
			qual_batch_last = (int *) palloc0(sizeof(int) * steps_len);
			int i = 0;
			while (i < steps_len)
			{
				ExprEvalOp sop = ExecEvalStepOp(state, &steps[i]);
				if (sop != EEOP_QUAL)
				{
					i++;
					continue;
				}

				/* Found a QUAL — scan forward for more with same jumpdone */
				int jumpdone = steps[i].d.qualexpr.jumpdone;
				int first_qual = i;
				int last_qual = i;
				bool safe = true;

				for (int j = i + 1; j < steps_len && safe; j++)
				{
					if (is_jump_target[j])
						break;
					ExprEvalOp jop = ExecEvalStepOp(state, &steps[j]);
					if (jop == EEOP_QUAL)
					{
						if (steps[j].d.qualexpr.jumpdone == jumpdone)
							last_qual = j;
						else
							break;
					}
					else
					{
						/* Check if this step is safe to speculate */
						switch (jop)
						{
							case EEOP_INNER_VAR:
							case EEOP_OUTER_VAR:
							case EEOP_SCAN_VAR:
#ifdef HAVE_EEOP_OLD_NEW
							case EEOP_OLD_VAR:
							case EEOP_NEW_VAR:
#endif
							case EEOP_CONST:
							case EEOP_ASSIGN_INNER_VAR:
							case EEOP_ASSIGN_OUTER_VAR:
							case EEOP_ASSIGN_SCAN_VAR:
							case EEOP_INNER_FETCHSOME:
							case EEOP_OUTER_FETCHSOME:
							case EEOP_SCAN_FETCHSOME:
								break;  /* safe */

							case EEOP_FUNCEXPR_STRICT:
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
							case EEOP_FUNCEXPR_STRICT_1:
							case EEOP_FUNCEXPR_STRICT_2:
#endif
							{
								/* Only safe if it's a known direct-call fn */
								const JitDirectFn *dfn =
									jit_find_direct_fn(steps[j].d.func.fn_addr);
								if (!dfn)
									safe = false;
								break;
							}

							default:
								safe = false;
								break;
						}
					}
				}

				if (safe && last_qual > first_qual)
				{
					/* Batch of 2+ QUALs with same jumpdone, all steps safe */
					has_qual_batches = true;
					for (int q = first_qual; q <= last_qual; q++)
					{
						if (ExecEvalStepOp(state, &steps[q]) == EEOP_QUAL)
							qual_batch_last[q] = last_qual;
					}
				}

				i = last_qual + 1;
			}

			/* If no batches were found, free the array */
			if (!has_qual_batches)
			{
				pfree(qual_batch_last);
				qual_batch_last = NULL;
			}
		}
	}

	/*
	 * Register for accumulating QUAL batch failures (branchless).
	 * Only allocated when qual batching is active — avoids stealing a
	 * callee-saved register on expressions that don't benefit.
	 */
	a64::Gp v_qual_fail;
	if (has_qual_batches)
		v_qual_fail = cc.new_gpx("qfail");

	/* CASE binary search pre-scan */
	CaseBSearchInfo case_bsearch[16];
	int num_case_bsearch = 0;
	int next_bsearch_idx = 0;

	num_case_bsearch = pg_jitter_detect_case_bsearch(
		state, steps, steps_len, case_bsearch, 16);

	if (num_case_bsearch > 0) {
		pg_jitter_setup_case_bsearch_arrays(state, steps, steps_len);
		elog(DEBUG1, "pg_jitter[asmjit]: detected %d CASE bsearch pattern(s)",
			 num_case_bsearch);
	}

	/* Main loop: emit code for each step */
	for (int opno = 0; opno < steps_len; opno++)
	{
		ExprEvalStep *op = &steps[opno];
		ExprEvalOp	opcode = ExecEvalStepOp(state, op);

		cc.bind(step_labels[opno]);

		/* CASE binary search: inject call after VAR step, skip pattern */
		if (next_bsearch_idx < num_case_bsearch &&
			opno == case_bsearch[next_bsearch_idx].start_opno + 1)
		{
			CaseBSearchInfo *cbi = &case_bsearch[next_bsearch_idx];
			int var_opno = cbi->start_opno;
			ExprEvalStep *var_op = &steps[var_opno];

			Label null_path = cc.new_label();
			Label done_path = cc.new_label();

			/* Load resnull from VAR step */
			if (var_op->resnull == &state->resnull)
				cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
			else
			{
				cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(var_opno, resnull)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
			}
			cc.cbnz(tmp2, null_path);

			/* Load resvalue from VAR step */
			if (var_op->resvalue == &state->resvalue)
				cc.ldr(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
			else
			{
				cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(var_opno, resvalue)));
				cc.ldr(tmp1, a64::ptr(tmp3));
			}

			/* Load desc pointer from step data (start_opno+2, dead JINT step) */
			a64::Gp desc_reg = cc.new_gpx("bsdesc");
			cc.ldr(desc_reg, a64::ptr(v_steps,
				STEP_OFFSET(cbi->start_opno + 2, d.constval.value)));

			void *helper_fn = pg_jitter_select_bsearch_helper(cbi);

			/* Call helper: (val, desc) -> Datum */
			a64::Gp fn_reg = cc.new_gpx("bsfn");
			ASMJIT_MOV_FN(cc, fn_reg, helper_fn);

			InvokeNode *bs_invoke;
			if (cbi->var_type == JIT_TYPE_32)
			{
				cc.invoke(Out(bs_invoke), fn_reg,
						  FuncSignature::build<int64_t, int32_t, void *>());
			}
			else
			{
				cc.invoke(Out(bs_invoke), fn_reg,
						  FuncSignature::build<int64_t, int64_t, void *>());
			}
			bs_invoke->set_arg(0, tmp1);
			bs_invoke->set_arg(1, desc_reg);
			bs_invoke->set_ret(0, tmp1);

			/* Store result to ELSE step's resvalue, resnull = false */
			{
				ExprEvalStep *else_op = &steps[cbi->end_opno];
				if (else_op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(cbi->end_opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}
				cc.mov(tmp1, 0);
				if (else_op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(cbi->end_opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
			}
			cc.b(step_labels[cbi->else_end_opno]);

			/* Null path: use default result */
			cc.bind(null_path);
			{
				ExprEvalStep *else_op = &steps[cbi->end_opno];
				cc.mov(tmp1, (int64_t)cbi->default_result);
				if (else_op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(cbi->end_opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}
				cc.mov(tmp1, cbi->default_is_null ? 1 : 0);
				if (else_op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(cbi->end_opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
			}
			cc.b(step_labels[cbi->else_end_opno]);

			/* Bind labels for skipped steps */
			for (int skip = opno + 1; skip < cbi->else_end_opno && skip < steps_len; skip++)
				cc.bind(step_labels[skip]);

			opno = cbi->else_end_opno - 1;
			next_bsearch_idx++;
			continue;
		}

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

#ifdef HAVE_EEOP_DONE_SPLIT
			case EEOP_DONE_NO_RETURN:
			{
				cc.mov(tmp1, 0);
				cc.ret(tmp1);
				break;
			}
#endif

			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_FETCHSOME:
			case EEOP_NEW_FETCHSOME:
#endif
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
					 *
					 * Skip in shared mode: templates are in dylib .text,
					 * workers have different ASLR base, and template
					 * addresses fall outside the ±512KB relocation range.
					 */
					deform_template_fn tmpl = (!asmjit_shared_code_mode)
						? asmjit_deform_match_template(
							op->d.fetch.known_desc,
							op->d.fetch.kind,
							op->d.fetch.last_var)
						: NULL;

					if (tmpl)
					{
						/* slot_reg already has the slot pointer; call tmpl(slot) */
						a64::Gp tmpl_fn = cc.new_gpx();
						ASMJIT_MOV_FN(cc, tmpl_fn, (void *) tmpl);
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
						JITTER_INSTR_DEFORM_ACCUM(ctx->base.instr,
						                      deform_end, deform_start);
					}
				}

				if (!deform_emitted)
				{
					/*
					 * Fallback: call compiled deform dispatch which JIT-compiles
					 * and caches the deform, falling back to slot_getsomeattrs_int
					 * only for unsupported slot types.
					 */
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) pg_jitter_compiled_deform_dispatch);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, int>());
					invoke->set_arg(0, slot_reg);
					invoke->set_arg(1, Imm(op->d.fetch.last_var));
				}

				cc.bind(skip);

				/*
				 * Cache slot's tts_values and tts_isnull for subsequent
				 * VAR/ASSIGN_VAR ops, avoiding redundant pointer chasing.
				 * slot_reg is already loaded with the slot pointer.
				 */
				{
					int ci = slot_cache_index(opcode);
					if (ci >= 0)
					{
						cc.ldr(v_cached_vals[ci], a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
						cc.ldr(v_cached_nulls[ci], a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
						slots_cached |= (1u << ci);
					}
				}
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
				int64_t soff = slot_offset_for_opcode(opcode);
				int ci = slot_cache_index(opcode);
				bool use_cache = (ci >= 0) && (slots_cached & (1u << ci)) != 0;

				/*
				 * Batch lookahead: count consecutive VARs from the same slot.
				 * Load tts_values/tts_isnull once instead of per-VAR.
				 * Stop before any step that is a jump target.
				 */
				int batch_end = opno;
				while (batch_end + 1 < steps_len)
				{
					if (is_jump_target[batch_end + 1])
						break;
					ExprEvalOp next_opc = ExecEvalStepOp(state, &steps[batch_end + 1]);
					if (next_opc != opcode)
						break;
					batch_end++;
				}
				int batch_count = batch_end - opno + 1;

				/* Phase 1: load all values from tts_values */
				if (use_cache)
					cc.mov(tmp1, v_cached_vals[ci]);
				else
				{
					cc.ldr(slot_reg, a64::ptr(v_econtext, soff));
					cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				}
				for (int bi = 0; bi < batch_count; bi++)
				{
					ExprEvalStep *cur = &steps[opno + bi];
					int attnum = cur->d.var.attnum;

					cc.ldr(tmp2, a64::ptr(tmp1, attnum * (int64_t) sizeof(Datum)));
					if (cur->resvalue == &state->resvalue)
						cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno + bi, resvalue)));
						cc.str(tmp2, a64::ptr(tmp3));
					}
				}

				/* Phase 2: load all isnulls from tts_isnull */
				if (use_cache)
					cc.mov(tmp1, v_cached_nulls[ci]);
				else
					cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				for (int bi = 0; bi < batch_count; bi++)
				{
					ExprEvalStep *cur = &steps[opno + bi];
					int attnum = cur->d.var.attnum;

					cc.ldrb(tmp2.w(), a64::ptr(tmp1, attnum * (int64_t) sizeof(bool)));
					if (cur->resnull == &state->resnull)
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno + bi, resnull)));
						cc.strb(tmp2.w(), a64::ptr(tmp3));
					}
				}

				/* Bind labels for skipped steps */
				for (int bi = 1; bi < batch_count; bi++)
					cc.bind(step_labels[opno + bi]);
				opno = batch_end; /* for-loop will increment past batch */
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
				int64_t soff = slot_offset_for_opcode(opcode);
				int ci = slot_cache_index(opcode);
				bool use_cache = (ci >= 0) && (slots_cached & (1u << ci)) != 0;

				/* Batch lookahead: count consecutive same-slot ASSIGN_VARs.
				 * Stop before any step that is a jump target. */
				int batch_end = opno;
				while (batch_end + 1 < steps_len)
				{
					if (is_jump_target[batch_end + 1])
						break;
					ExprEvalOp next_opc = ExecEvalStepOp(state, &steps[batch_end + 1]);
					if (next_opc != opcode)
						break;
					batch_end++;
				}
				int batch_count = batch_end - opno + 1;

				/* Phase 1: load all values from source, store to result */
				if (use_cache)
					cc.mov(tmp1, v_cached_vals[ci]);
				else
				{
					cc.ldr(slot_reg, a64::ptr(v_econtext, soff));
					cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				}
				for (int bi = 0; bi < batch_count; bi++)
				{
					ExprEvalStep *cur = &steps[opno + bi];
					int attnum = cur->d.assign_var.attnum;
					int resultnum = cur->d.assign_var.resultnum;

					cc.ldr(tmp2, a64::ptr(tmp1, attnum * (int64_t) sizeof(Datum)));
					cc.str(tmp2, a64::ptr(v_resultvals, resultnum * (int64_t) sizeof(Datum)));
				}

				/* Phase 2: load all isnulls from source, store to result */
				if (use_cache)
					cc.mov(tmp1, v_cached_nulls[ci]);
				else
					cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				for (int bi = 0; bi < batch_count; bi++)
				{
					ExprEvalStep *cur = &steps[opno + bi];
					int attnum = cur->d.assign_var.attnum;
					int resultnum = cur->d.assign_var.resultnum;

					cc.ldrb(tmp2.w(), a64::ptr(tmp1, attnum * (int64_t) sizeof(bool)));
					cc.strb(tmp2.w(), a64::ptr(v_resultnulls, resultnum * (int64_t) sizeof(bool)));
				}

				/* Bind labels for skipped steps */
				for (int bi = 1; bi < batch_count; bi++)
					cc.bind(step_labels[opno + bi]);
				opno = batch_end; /* for-loop will increment past batch */
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
					ASMJIT_MOV_FN(cc, fn_reg, (void *) MakeExpandedObjectReadOnlyInternal);
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
				/* *op->resvalue = constval.value (steps-relative) */
				if (asmjit_shared_code_mode) {
					/*
					 * In shared mode, load constval.value from step data
					 * at runtime.  Pass-by-reference Datums (text, bytea)
					 * are pointers into process-local heap memory that
					 * differ between leader and worker.
					 */
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, d.constval.value)));
				} else {
					cc.mov(tmp1, (int64_t) op->d.constval.value);
				}
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}
				/* *op->resnull = constval.isnull (steps-relative) */
				cc.mov(tmp1, op->d.constval.isnull ? 1 : 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				break;
			}

			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
#endif
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int nargs = op->d.func.nargs;
				Label done_label = cc.new_label();

				if (opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
					|| opcode == EEOP_FUNCEXPR_STRICT_1
					|| opcode == EEOP_FUNCEXPR_STRICT_2
#endif
					)
				{
					/* Set resnull = true (steps-relative) */
					if (op->resnull == &state->resnull)
					{
						cc.mov(tmp2, 1);
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					}
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.mov(tmp2, 1);
						cc.strb(tmp2.w(), a64::ptr(tmp1));
					}

					/*
					 * Batched null check: OR all isnull flags together,
					 * single branch at the end.  Reduces branch mispredicts
					 * for multi-arg strict functions.
					 *
					 * Load fcinfo from steps-relative addressing.
					 */
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
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

				if (dfn && (dfn->inline_op == JIT_INLINE_TEXT_EQ ||
				            dfn->inline_op == JIT_INLINE_TEXT_NE) &&
				    pg_jitter_collation_is_deterministic(fcinfo->fncollation))
				{
					/*
					 * TIER 0a — FULLY INLINE TEXT EQ/NE.
					 * Short varlena ≤ 7 data bytes: zero function calls.
					 * Longer short varlena: memcmp call only.
					 * Non-short (toast/compressed): jit_text_datum_eq/ne.
					 * Only for deterministic collations; non-deterministic
					 * falls through to V1.
					 */
					{
						bool is_eq = (dfn->inline_op == JIT_INLINE_TEXT_EQ);

						Label lbl_result_eq = cc.new_label();
						Label lbl_result_ne = cc.new_label();
						Label lbl_slow = cc.new_label();
						Label lbl_memcmp = cc.new_label();
						Label lbl_store = cc.new_label();

						a64::Gp fci_base = cc.new_gpx();
						a64::Gp ta0 = cc.new_gpx(), ta1 = cc.new_gpx();
						a64::Gp hdr_a = cc.new_gpx(), hdr_b = cc.new_gpx();
						a64::Gp data_len = cc.new_gpx();

						int64_t val_off_0 =
							(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
						int64_t val_off_1 =
							(int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

						/* Load fcinfo, then args */
						cc.ldr(fci_base, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
						cc.ldr(ta0, a64::ptr(fci_base, val_off_0));
						cc.ldr(ta1, a64::ptr(fci_base, val_off_1));

						/* 1. Pointer equality → result_eq */
						cc.cmp(ta0, ta1);
						cc.b(a64::CondCode::kEQ, lbl_result_eq);

						/* 2. Load uint8 headers */
						cc.ldrb(hdr_a.w(), a64::ptr(ta0));
						cc.ldrb(hdr_b.w(), a64::ptr(ta1));

						/* 3. Check both short: bit 0 set, != 1 */
						cc.tbz(hdr_a.w(), 0, lbl_slow);
						cc.cmp(hdr_a.w(), 1);
						cc.b(a64::CondCode::kEQ, lbl_slow);
						cc.tbz(hdr_b.w(), 0, lbl_slow);
						cc.cmp(hdr_b.w(), 1);
						cc.b(a64::CondCode::kEQ, lbl_slow);

						/* 4. Headers equal? */
						cc.cmp(hdr_a.w(), hdr_b.w());
						cc.b(a64::CondCode::kNE, lbl_result_ne);

						/* 5. data_len = (hdr >> 1) - 1 */
						cc.lsr(data_len.w(), hdr_a.w(), 1);
						cc.sub(data_len.w(), data_len.w(), 1);

						/* data_len == 0 → result_eq */
						cc.cbz(data_len.w(), lbl_result_eq);

						/* 6. data_len > 7 → memcmp */
						cc.cmp(data_len.w(), 7);
						cc.b(a64::CondCode::kGT, lbl_memcmp);

						/* 7. Inline word comparison (data_len 1-7):
						 *    Load 8 bytes, shift left by (7-dl)*8, compare */
						{
							a64::Gp wa = cc.new_gpx(), wb = cc.new_gpx();
							a64::Gp shift = cc.new_gpx();
							cc.ldr(wa, a64::ptr(ta0));
							cc.ldr(wb, a64::ptr(ta1));
							/* shift = (7 - data_len) * 8 */
							cc.mov(shift.w(), 7);
							cc.sub(shift.w(), shift.w(), data_len.w());
							cc.lsl(shift.w(), shift.w(), 3);
							/* Zero-extend shift to 64-bit for LSL */
							cc.mov(shift.w(), shift.w());
							cc.lsl(wa, wa, shift);
							cc.lsl(wb, wb, shift);
							cc.cmp(wa, wb);
							cc.b(a64::CondCode::kEQ, lbl_result_eq);
							cc.b(lbl_result_ne);
						}

						/* 8. memcmp path (data_len > 7) */
						cc.bind(lbl_memcmp);
						{
							a64::Gp ma0 = cc.new_gpx(), ma1 = cc.new_gpx();
							a64::Gp mlen = cc.new_gpx();
							cc.add(ma0, ta0, 1);
							cc.add(ma1, ta1, 1);
							/* Zero-extend data_len for memcmp size_t */
							cc.mov(mlen.w(), data_len.w());

							a64::Gp fn_mc = cc.new_gpx();
							ASMJIT_MOV_FN(cc, fn_mc, (void *)memcmp);
							InvokeNode *inv_mc;
							cc.invoke(Out(inv_mc), fn_mc,
									  FuncSignature::build<int32_t, const void *, const void *, uint64_t>());
							inv_mc->set_arg(0, ma0);
							inv_mc->set_arg(1, ma1);
							inv_mc->set_arg(2, mlen);
							inv_mc->set_ret(0, tmp1);
						}
						cc.cbz(tmp1.w(), lbl_result_eq);
						cc.b(lbl_result_ne);

						/* result_ne */
						cc.bind(lbl_result_ne);
						cc.mov(tmp1, is_eq ? 0 : 1);
						cc.b(lbl_store);

						/* result_eq */
						cc.bind(lbl_result_eq);
						cc.mov(tmp1, is_eq ? 1 : 0);
						cc.b(lbl_store);

						/* slow path: call jit_text_datum_eq/ne */
						cc.bind(lbl_slow);
						{
							a64::Gp fci2 = cc.new_gpx();
							a64::Gp sa0 = cc.new_gpx(), sa1 = cc.new_gpx();
							cc.ldr(fci2, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
							cc.ldr(sa0, a64::ptr(fci2, val_off_0));
							cc.ldr(sa1, a64::ptr(fci2, val_off_1));

							a64::Gp fn_tc = cc.new_gpx();
							ASMJIT_MOV_FN(cc, fn_tc,
										  is_eq ? (void *)jit_text_datum_eq
										        : (void *)jit_text_datum_ne);
							InvokeNode *inv_tc;
							cc.invoke(Out(inv_tc), fn_tc,
									  FuncSignature::build<int32_t, int64_t, int64_t>());
							inv_tc->set_arg(0, sa0);
							inv_tc->set_arg(1, sa1);
							inv_tc->set_ret(0, tmp1);
							cc.sxtw(tmp1, tmp1.w());
						}

						/* store_result */
						cc.bind(lbl_store);
					}

					/* Store *op->resvalue = tmp1 (steps-relative) */
					if (op->resvalue == &state->resvalue)
						cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp1, a64::ptr(tmp2));
					}

					/* *op->resnull = false (steps-relative) */
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}
				else if (dfn && dfn->inline_op >= JIT_INLINE_INT4_ADD
					&& dfn->inline_op <= JIT_INLINE_INT8_GE)
				{
					/*
					 * TIER 0 — INLINE: emit int32/int64 ops as native
					 * ARM64 instructions. Float8 inline ops are not
					 * handled here — they fall through to the direct-
					 * call path below.
					 */
					a64::Gp fci_base = cc.new_gpx();
					a64::Gp a0 = cc.new_gpx(), a1 = cc.new_gpx();
					cc.ldr(fci_base, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
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
						ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int4_overflow);
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
						ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int4_overflow);
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
						ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int4_overflow);
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
							ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_division_by_zero);
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
							ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int4_overflow);
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
							ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_division_by_zero);
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
						ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int8_overflow);
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
						ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int8_overflow);
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
						ASMJIT_MOV_FN(cc, err_fn, (void *) jit_error_int8_overflow);
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

					/* Store *op->resvalue = tmp1 (steps-relative) */
					if (op->resvalue == &state->resvalue)
						cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp1, a64::ptr(tmp2));
					}

					/* *op->resnull = false (steps-relative) */
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}
				else if (dfn && (dfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
						|| (!asmjit_shared_code_mode &&
							dfn->jit_fn_name &&
							mir_find_precompiled_fn(dfn->jit_fn_name))
#endif
						))
				{
					/*
					 * Direct native call — either from dfn->jit_fn or
					 * MIR-precompiled function pointer.
					 *
					 * Skip MIR-precompiled in shared mode: blob addresses
					 * are in separately mmap'd memory, outside the ±512KB
					 * dylib relocation range.
					 */
					void *call_target = dfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!call_target && !asmjit_shared_code_mode)
						call_target = mir_find_precompiled_fn(dfn->jit_fn_name);
#endif
					/* Load fcinfo once (steps-relative), then load all args from offsets */
					a64::Gp args[4];
					a64::Gp fci_base = cc.new_gpx();
					cc.ldr(fci_base, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
					for (int i = 0; i < dfn->nargs; i++)
					{
						args[i] = cc.new_gpx();
						int64_t val_off =
							(int64_t)((char *)&fcinfo->args[i].value - (char *)fcinfo);
						cc.ldr(args[i], a64::ptr(fci_base, val_off));
					}

					/* Load collation if this function needs it */
					bool has_coll = (dfn->flags & JIT_FN_FLAG_COLLATION) != 0;
					a64::Gp coll_reg;
					if (has_coll)
					{
						coll_reg = cc.new_gpx();
						cc.ldr(coll_reg.w(), a64::ptr(fci_base,
							   (int64_t)offsetof(FunctionCallInfoBaseData, fncollation)));
						cc.sxtw(coll_reg, coll_reg.w());
					}

					/* Direct call */
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, call_target);

					InvokeNode *invoke;
					int native_nargs = jit_native_nargs(dfn);
					switch (native_nargs) {
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
						invoke->set_arg(1, has_coll ? coll_reg : args[1]);
						break;
					case 3:
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int64_t, int64_t, int64_t>());
						invoke->set_arg(0, args[0]);
						invoke->set_arg(1, args[1]);
						invoke->set_arg(2, has_coll ? coll_reg : args[2]);
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

					/* *op->resvalue = result (steps-relative) */
					if (op->resvalue == &state->resvalue)
						cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp1, a64::ptr(tmp2));
					}

					/* *op->resnull = false (steps-relative) */
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}
				else
				{
				asmjit_funcexpr_v1_fallback:
				/*
				 * TIER 2 — V1 FALLBACK (or Vectorscan LIKE/regex).
				 */
				bool vs_handled = false;

				/*
				 * LIKE/regex FAST PATH: StringZilla + Vectorscan.
				 * Detects textlike, textnlike, texticlike, texticnlike,
				 * textregexeq, textregexne, texticregexeq, texticregexne
				 * with a compile-time constant pattern.  Only in non-shared
				 * mode (embedded pointers are process-local).
				 */
				if (!asmjit_shared_code_mode)
				{
					PGFunction fn = op->d.func.fn_addr;
					bool is_like_fn = (fn == textlike || fn == textnlike);
					bool is_ilike_fn = (fn == texticlike || fn == texticnlike);
					bool is_regex_fn = (fn == textregexeq || fn == textregexne);
					bool is_iregex_fn = (fn == texticregexeq || fn == texticregexne);
					bool vs_negate = (fn == textnlike || fn == texticnlike ||
									  fn == textregexne || fn == texticregexne);

					if ((is_like_fn || is_ilike_fn || is_regex_fn || is_iregex_fn) &&
						fcinfo->nargs == 2)
					{
						/*
						 * Check if args[1] (the pattern) is a compile-time constant.
						 * PG's ExecInitFunc directly assigns Const values into
						 * fcinfo->args[argno] without generating an EEOP_CONST step.
						 * So if no step writes to args[1], it was pre-initialized
						 * from a Const node — treat it as constant.
						 */
						bool pat_const = true;
						for (int j = opno - 1; j >= 0; j--)
						{
							if (steps[j].resvalue == &fcinfo->args[1].value)
							{
								pat_const = (steps[j].opcode == EEOP_CONST);
								break;
							}
						}

						/*
						 * LIKE/regex: Vectorscan/StringZilla use POSIX character
						 * classes, not ICU.  Only safe for C/POSIX collation.
						 */
						if (pat_const && !fcinfo->args[1].isnull &&
							pg_jitter_collation_is_c(fcinfo->fncollation))
						{
							text *pat_text = DatumGetTextPP(fcinfo->args[1].value);
							char *pat_str = VARDATA_ANY(pat_text);
							int pat_len = VARSIZE_ANY_EXHDR(pat_text);

							/*
							 * StringZilla fast path for simple LIKE patterns.
							 * Only for LIKE/NOT LIKE (not ILIKE/regex).
							 */
							if (is_like_fn)
							{
								const char *literal;
								int literal_len;
								int match_type = simd_like_classify(
									pat_str, pat_len, &literal, &literal_len);

								if (match_type >= 0 && literal != NULL)
								{
									a64::Gp fci_base = cc.new_gpx();
									cc.ldr(fci_base, a64::ptr(v_steps,
										   STEP_OFFSET(opno, d.func.fcinfo_data)));
									int64_t val_off_0 =
										(int64_t)((char *)&fcinfo->args[0].value -
												  (char *)fcinfo);
									a64::Gp arg0 = cc.new_gpx();
									cc.ldr(arg0, a64::ptr(fci_base, val_off_0));

									a64::Gp arg1 = cc.new_gpx();
									cc.mov(arg1, (uint64_t)(uintptr_t)literal);
									a64::Gp arg2 = cc.new_gpx();
									cc.mov(arg2, (uint64_t)literal_len);
									a64::Gp arg3 = cc.new_gpx();
									cc.mov(arg3, (uint64_t)match_type);

									a64::Gp fn_reg = cc.new_gpx();
									ASMJIT_MOV_FN(cc, fn_reg,
												   (void *)simd_like_match_text);

									InvokeNode *invoke;
									cc.invoke(Out(invoke), fn_reg,
											  FuncSignature::build<int32_t,
											  int64_t, int64_t, int32_t,
											  int32_t>());
									invoke->set_arg(0, arg0);
									invoke->set_arg(1, arg1);
									invoke->set_arg(2, arg2);
									invoke->set_arg(3, arg3);
									invoke->set_ret(0, tmp1);

									cc.sxtw(tmp1, tmp1.w());

									if (vs_negate)
										cc.eor(tmp1, tmp1, Imm(1));

									if (op->resvalue == &state->resvalue)
										cc.str(tmp1, a64::ptr(v_state,
											   offsetof(ExprState, resvalue)));
									else
									{
										cc.ldr(tmp2, a64::ptr(v_steps,
											   STEP_OFFSET(opno, resvalue)));
										cc.str(tmp1, a64::ptr(tmp2));
									}

									cc.mov(tmp1, 0);
									if (op->resnull == &state->resnull)
										cc.strb(tmp1.w(), a64::ptr(v_state,
											    offsetof(ExprState, resnull)));
									else
									{
										cc.ldr(tmp2, a64::ptr(v_steps,
											   STEP_OFFSET(opno, resnull)));
										cc.strb(tmp1.w(), a64::ptr(tmp2));
									}

									vs_handled = true;
								}
							}

							/* Compiled LIKE path for anchored patterns */
							bool skip_vectorscan = false;
							if (!vs_handled && is_like_fn)
							{
								SzLikeCompiled *compiled =
									simd_like_compile(pat_str, pat_len);
								if (compiled == SIMD_LIKE_USE_V1)
								{
									skip_vectorscan = true;
								}
								else if (compiled)
								{
									a64::Gp fci_base = cc.new_gpx();
									cc.ldr(fci_base, a64::ptr(v_steps,
										   STEP_OFFSET(opno, d.func.fcinfo_data)));
									int64_t val_off_0 =
										(int64_t)((char *)&fcinfo->args[0].value -
												  (char *)fcinfo);
									a64::Gp arg0 = cc.new_gpx();
									cc.ldr(arg0, a64::ptr(fci_base, val_off_0));

									a64::Gp comp_reg = cc.new_gpx();
									cc.mov(comp_reg, (uint64_t)(uintptr_t)compiled);

									a64::Gp fn_reg = cc.new_gpx();
									ASMJIT_MOV_FN(cc, fn_reg,
												   (void *)simd_like_match_compiled);

									InvokeNode *invoke;
									cc.invoke(Out(invoke), fn_reg,
											  FuncSignature::build<int32_t,
											  int64_t, int64_t>());
									invoke->set_arg(0, arg0);
									invoke->set_arg(1, comp_reg);
									invoke->set_ret(0, tmp1);

									cc.sxtw(tmp1, tmp1.w());

									if (vs_negate)
										cc.eor(tmp1, tmp1, Imm(1));

									if (op->resvalue == &state->resvalue)
										cc.str(tmp1, a64::ptr(v_state,
											   offsetof(ExprState, resvalue)));
									else
									{
										cc.ldr(tmp2, a64::ptr(v_steps,
											   STEP_OFFSET(opno, resvalue)));
										cc.str(tmp1, a64::ptr(tmp2));
									}

									cc.mov(tmp1, 0);
									if (op->resnull == &state->resnull)
										cc.strb(tmp1.w(), a64::ptr(v_state,
											    offsetof(ExprState, resnull)));
									else
									{
										cc.ldr(tmp2, a64::ptr(v_steps,
											   STEP_OFFSET(opno, resnull)));
										cc.strb(tmp1.w(), a64::ptr(tmp2));
									}

									vs_handled = true;
								}
							}

#ifdef PG_JITTER_HAVE_VECTORSCAN
							/* Vectorscan path for complex LIKE or regex */
							if (!vs_handled && !skip_vectorscan)
							{
								bool is_utf8 = (GetDatabaseEncoding() == PG_UTF8);
								VsCacheEntry *entry = pg_jitter_vs_compile(
									pat_str, pat_len,
									is_like_fn || is_ilike_fn,
									is_ilike_fn || is_iregex_fn,
									is_utf8);

								if (entry)
								{
									a64::Gp fci_base = cc.new_gpx();
									cc.ldr(fci_base, a64::ptr(v_steps,
										   STEP_OFFSET(opno, d.func.fcinfo_data)));
									int64_t val_off_0 =
										(int64_t)((char *)&fcinfo->args[0].value -
												  (char *)fcinfo);
									a64::Gp arg0 = cc.new_gpx();
									cc.ldr(arg0, a64::ptr(fci_base, val_off_0));

									a64::Gp entry_reg = cc.new_gpx();
									cc.mov(entry_reg, (uint64_t)(uintptr_t)entry);

									a64::Gp fn_reg = cc.new_gpx();
									ASMJIT_MOV_FN(cc, fn_reg,
												   (void *)pg_jitter_vs_match_text);

									InvokeNode *invoke;
									cc.invoke(Out(invoke), fn_reg,
											  FuncSignature::build<int32_t,
											  int64_t, int64_t>());
									invoke->set_arg(0, arg0);
									invoke->set_arg(1, entry_reg);
									invoke->set_ret(0, tmp1);

									cc.sxtw(tmp1, tmp1.w());

									if (vs_negate)
										cc.eor(tmp1, tmp1, Imm(1));

									if (op->resvalue == &state->resvalue)
										cc.str(tmp1, a64::ptr(v_state,
											   offsetof(ExprState, resvalue)));
									else
									{
										cc.ldr(tmp2, a64::ptr(v_steps,
											   STEP_OFFSET(opno, resvalue)));
										cc.str(tmp1, a64::ptr(tmp2));
									}

									cc.mov(tmp1, 0);
									if (op->resnull == &state->resnull)
										cc.strb(tmp1.w(), a64::ptr(v_state,
											    offsetof(ExprState, resnull)));
									else
									{
										cc.ldr(tmp2, a64::ptr(v_steps,
											   STEP_OFFSET(opno, resnull)));
										cc.strb(tmp1.w(), a64::ptr(tmp2));
									}

									vs_handled = true;
								}
							}
#endif /* PG_JITTER_HAVE_VECTORSCAN */
						}
					}
				}

				if (!vs_handled)
				{
				/* Fallback: generic fcinfo path (steps-relative) */
				{
					a64::Gp fci = cc.new_gpx();
					cc.ldr(fci, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci, offsetof(FunctionCallInfoBaseData, isnull)));
				}
				{
					a64::Gp fn_reg = cc.new_gpx();
					if (asmjit_shared_code_mode) {
						/*
						 * In shared mode, fn_addr may point into a dynamically-
						 * loaded library whose address differs per process.
						 * Load from step data at runtime.
						 */
						cc.ldr(fn_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fn_addr)));
					} else {
						ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.func.fn_addr);
					}
					a64::Gp fcinfo_reg = cc.new_gpx();
					cc.ldr(fcinfo_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}
				/* *op->resvalue = result (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}
				/* fcinfo->isnull → *op->resnull (steps-relative) */
				{
					a64::Gp fci2 = cc.new_gpx();
					cc.ldr(fci2, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
					cc.ldrb(tmp2.w(), a64::ptr(fci2, offsetof(FunctionCallInfoBaseData, isnull)));
					if (op->resnull == &state->resnull)
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp2.w(), a64::ptr(tmp1));
					}
				}
				} /* end if (!vs_handled) */
				} /* end else fallback / vectorscan */
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
					/* *anynull = false (steps-relative) */
					a64::Gp anp = cc.new_gpx();
					cc.add(anp, v_steps, STEP_OFFSET(opno, d.boolexpr.anynull));
					cc.ldr(tmp1, a64::ptr(anp));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				/* Load resnull (steps-relative) */
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbnz(tmp2, null_handler);

				/* Not null: check false (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp2, a64::ptr(tmp1));
				}
				cc.cbz(tmp2, step_labels[op->d.boolexpr.jumpdone]);

				cc.b(cont);

				/* Null handler: set anynull (steps-relative) */
				cc.bind(null_handler);
				{
					a64::Gp anp = cc.new_gpx();
					cc.add(anp, v_steps, STEP_OFFSET(opno, d.boolexpr.anynull));
					cc.ldr(tmp1, a64::ptr(anp));
					cc.mov(tmp2, 1);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				cc.bind(cont);

				/* On last step: if anynull, set result to NULL (steps-relative) */
				if (opcode == EEOP_BOOL_AND_STEP_LAST)
				{
					Label no_anynull = cc.new_label();
					a64::Gp anp = cc.new_gpx();
					cc.add(anp, v_steps, STEP_OFFSET(opno, d.boolexpr.anynull));
					cc.ldr(tmp1, a64::ptr(anp));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					cc.cbz(tmp2, no_anynull);

					/* resnull = true, resvalue = 0 (steps-relative) */
					if (op->resnull == &state->resnull)
					{
						cc.mov(tmp2, 1);
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					}
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.mov(tmp2, 1);
						cc.strb(tmp2.w(), a64::ptr(tmp1));
					}
					if (op->resvalue == &state->resvalue)
					{
						cc.mov(tmp2, 0);
						cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					}
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.mov(tmp2, 0);
						cc.str(tmp2, a64::ptr(tmp1));
					}

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
					a64::Gp anp = cc.new_gpx();
					cc.add(anp, v_steps, STEP_OFFSET(opno, d.boolexpr.anynull));
					cc.ldr(tmp1, a64::ptr(anp));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				/* Load resnull (steps-relative) */
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbnz(tmp2, null_handler);

				/* Load resvalue (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp2, a64::ptr(tmp1));
				}
				cc.cbnz(tmp2, step_labels[op->d.boolexpr.jumpdone]);

				cc.b(cont);

				cc.bind(null_handler);
				{
					a64::Gp anp = cc.new_gpx();
					cc.add(anp, v_steps, STEP_OFFSET(opno, d.boolexpr.anynull));
					cc.ldr(tmp1, a64::ptr(anp));
					cc.mov(tmp2, 1);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				cc.bind(cont);

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					Label no_anynull = cc.new_label();
					a64::Gp anp = cc.new_gpx();
					cc.add(anp, v_steps, STEP_OFFSET(opno, d.boolexpr.anynull));
					cc.ldr(tmp1, a64::ptr(anp));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					cc.cbz(tmp2, no_anynull);

					if (op->resnull == &state->resnull)
					{
						cc.mov(tmp2, 1);
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					}
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.mov(tmp2, 1);
						cc.strb(tmp2.w(), a64::ptr(tmp1));
					}
					if (op->resvalue == &state->resvalue)
					{
						cc.mov(tmp2, 0);
						cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					}
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.mov(tmp2, 0);
						cc.str(tmp2, a64::ptr(tmp1));
					}

					cc.bind(no_anynull);
				}
				break;
			}

			case EEOP_BOOL_NOT_STEP:
			{
				/* Load resvalue ptr (steps-relative) */
				if (op->resvalue == &state->resvalue)
				{
					cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				}
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp2, a64::ptr(tmp1));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.str(tmp2, a64::ptr(tmp1));
				}
				break;
			}

			case EEOP_QUAL:
			{
				int batch_last = qual_batch_last ? qual_batch_last[opno] : 0;

				if (batch_last > 0)
				{
					/*
					 * Vectorized QUAL: this QUAL is part of a batch with same
					 * jumpdone.  Instead of branching per-predicate, accumulate
					 * failure flags branchlessly and branch once at the end.
					 *
					 * For each QUAL: fail if resnull OR resvalue==0
					 *   orr  qfail, qfail, resnull
					 *   cmp  resvalue, #0
					 *   cset t, eq
					 *   orr  qfail, qfail, t
					 *
					 * First QUAL initializes v_qual_fail; last QUAL branches.
					 */
					bool is_first = true;
					/* Check if a prior QUAL in the same batch already ran */
					for (int q = opno - 1; q >= 0; q--)
					{
						if (qual_batch_last[q] == batch_last &&
							ExecEvalStepOp(state, &steps[q]) == EEOP_QUAL)
						{
							is_first = false;
							break;
						}
					}

					if (is_first)
						cc.mov(v_qual_fail, 0);

					/* Load resnull */
					if (op->resnull == &state->resnull)
						cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					}
					cc.orr(v_qual_fail, v_qual_fail, tmp2);

					/* Load resvalue, fail if zero */
					if (op->resvalue == &state->resvalue)
						cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.ldr(tmp2, a64::ptr(tmp1));
					}
					cc.cmp(tmp2, 0);
					a64::Gp fail_bit = cc.new_gpx();
					cc.cset(fail_bit, arm::CondCode::kEQ);
					cc.orr(v_qual_fail, v_qual_fail, fail_bit);

					if (opno == batch_last)
					{
						/* Last QUAL in batch: emit single branch */
						Label qualfail = cc.new_label();
						Label cont = cc.new_label();

						cc.cbnz(v_qual_fail, qualfail);
						cc.b(cont);

						cc.bind(qualfail);
						/* Set resvalue=0, resnull=false for THIS step */
						cc.mov(tmp2, 0);
						if (op->resnull == &state->resnull)
							cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(tmp2.w(), a64::ptr(tmp1));
						}
						if (op->resvalue == &state->resvalue)
							cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(tmp2, a64::ptr(tmp1));
						}
						cc.b(step_labels[op->d.qualexpr.jumpdone]);

						cc.bind(cont);
					}
					/* Non-last QUALs: no branch, just accumulate */
				}
				else
				{
					/* Non-batched QUAL: original sequential logic */
					Label qualfail = cc.new_label();
					Label cont = cc.new_label();

					/* Load resnull (steps-relative) */
					if (op->resnull == &state->resnull)
						cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					}
					cc.cbnz(tmp2, qualfail);

					/* Load resvalue (steps-relative) */
					if (op->resvalue == &state->resvalue)
						cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.ldr(tmp2, a64::ptr(tmp1));
					}
					cc.cbz(tmp2, qualfail);

					cc.b(cont);

					cc.bind(qualfail);
					/* Set resvalue=0, resnull=false (steps-relative), jump to done */
					cc.mov(tmp2, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp2.w(), a64::ptr(tmp1));
					}
					if (op->resvalue == &state->resvalue)
						cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp2, a64::ptr(tmp1));
					}
					cc.b(step_labels[op->d.qualexpr.jumpdone]);

					cc.bind(cont);
				}
				break;
			}

			case EEOP_JUMP:
				cc.b(step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NULL:
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbnz(tmp2, step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NOT_NULL:
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbz(tmp2, step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NOT_TRUE:
			{
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbnz(tmp2, step_labels[op->d.jump.jumpdone]);

				if (op->resvalue == &state->resvalue)
					cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp2, a64::ptr(tmp1));
				}
				cc.cbz(tmp2, step_labels[op->d.jump.jumpdone]);
				break;
			}

			case EEOP_NULLTEST_ISNULL:
			{
				/* Load resnull (steps-relative) */
				if (op->resnull == &state->resnull)
				{
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					/* resvalue = (resnull == true) ? 1 : 0 */
					if (op->resvalue == &state->resvalue)
						cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp2, a64::ptr(tmp3));
					}
					/* resnull = false */
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				}
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp3));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				/* Load resnull (steps-relative) */
				if (op->resnull == &state->resnull)
				{
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					if (op->resvalue == &state->resvalue)
						cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp2, a64::ptr(tmp3));
					}
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				}
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp3));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}
				break;
			}

#ifdef HAVE_EEOP_HASHDATUM
			case EEOP_HASHDATUM_SET_INITVAL:
			{
				/* *op->resvalue = init_value (steps-relative) */
				cc.mov(tmp1, (int64_t) op->d.hashdatum_initvalue.init_value);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp3));
				}
				/* *op->resnull = false (steps-relative) */
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				break;
			}

			case EEOP_HASHDATUM_FIRST:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label store_zero = cc.new_label();
				Label store_result = cc.new_label();

				/* Check if arg is null (steps-relative fcinfo) */
				a64::Gp fci_reg = cc.new_gpx();
				cc.ldr(fci_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.hashdatum.fcinfo_data)));
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, store_zero);

				/* Not null: call hash function (direct or fcinfo) */
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (!asmjit_shared_code_mode &&
						hdfn->jit_fn_name &&
						mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target && !asmjit_shared_code_mode)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					ASMJIT_MOV_FN(cc, hfn_reg, hash_target);
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
					ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.hashdatum.fn_addr);
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
				/* *op->resvalue = result (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp3));
				}
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				break;
			}

			case EEOP_HASHDATUM_FIRST_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label is_null = cc.new_label();
				Label done = cc.new_label();

				/* Check if arg is null (steps-relative fcinfo) */
				a64::Gp fci_reg = cc.new_gpx();
				cc.ldr(fci_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.hashdatum.fcinfo_data)));
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, is_null);

				/* Not null: call hash function (direct or fcinfo) */
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (!asmjit_shared_code_mode &&
						hdfn->jit_fn_name &&
						mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target && !asmjit_shared_code_mode)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					ASMJIT_MOV_FN(cc, hfn_reg, hash_target);
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
					ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				/* *op->resvalue = result (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp3));
				}
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				cc.b(done);

				/* Null path: resnull=1, resvalue=0 (steps-relative) */
				cc.bind(is_null);
				cc.mov(tmp1, 1);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				cc.mov(tmp1, 0);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp3));
				}
				cc.b(step_labels[op->d.hashdatum.jumpdone]);

				cc.bind(done);
				break;
			}

			case EEOP_HASHDATUM_NEXT32:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label skip_hash = cc.new_label();

				/* Load existing hash from iresult->value (steps-relative) */
				a64::Gp hash = cc.new_gpx("hash");
				{
					a64::Gp ir = cc.new_gpx();
					cc.add(ir, v_steps, STEP_OFFSET(opno, d.hashdatum.iresult));
					cc.ldr(ir, a64::ptr(ir));  /* ir = iresult pointer */
					cc.ldr(hash.w(), a64::ptr(ir, offsetof(NullableDatum, value)));
				}

				/* Rotate left 1 = rotate right 31 */
				cc.ror(hash.w(), hash.w(), 31);

				/* Check if arg is null (steps-relative fcinfo) */
				a64::Gp fci_reg = cc.new_gpx();
				cc.ldr(fci_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.hashdatum.fcinfo_data)));
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, skip_hash);

				/* Not null: call hash function (direct or fcinfo), XOR */
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (!asmjit_shared_code_mode &&
						hdfn->jit_fn_name &&
						mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target && !asmjit_shared_code_mode)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					ASMJIT_MOV_FN(cc, hfn_reg, hash_target);
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
					ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.eor(hash.w(), hash.w(), tmp1.w());

				cc.bind(skip_hash);
				/* Store result (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(hash, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(hash, a64::ptr(tmp3));
				}
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				break;
			}

			case EEOP_HASHDATUM_NEXT32_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label is_null = cc.new_label();
				Label done = cc.new_label();

				/* Check if arg is null first (steps-relative fcinfo) */
				a64::Gp fci_reg = cc.new_gpx();
				cc.ldr(fci_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.hashdatum.fcinfo_data)));
				cc.ldrb(tmp2.w(), a64::ptr(fci_reg, arg0_null_off));
				cc.cbnz(tmp2, is_null);

				/* Load existing hash from iresult->value (steps-relative) */
				a64::Gp hash = cc.new_gpx("hash");
				{
					a64::Gp ir = cc.new_gpx();
					cc.add(ir, v_steps, STEP_OFFSET(opno, d.hashdatum.iresult));
					cc.ldr(ir, a64::ptr(ir));
					cc.ldr(hash.w(), a64::ptr(ir, offsetof(NullableDatum, value)));
				}

				/* Rotate left 1 = rotate right 31 */
				cc.ror(hash.w(), hash.w(), 31);

				/* Call hash function (direct or fcinfo), XOR */
				{
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (!asmjit_shared_code_mode &&
						hdfn->jit_fn_name &&
						mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					a64::Gp hash_arg = cc.new_gpx();
					cc.ldr(hash_arg, a64::ptr(fci_reg, val_off));
					a64::Gp hfn_reg = cc.new_gpx();
				{
					void *hash_target = hdfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!hash_target && !asmjit_shared_code_mode)
						hash_target = mir_find_precompiled_fn(hdfn->jit_fn_name);
#endif
					ASMJIT_MOV_FN(cc, hfn_reg, hash_target);
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
					ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				}
				cc.eor(hash.w(), hash.w(), tmp1.w());

				/* Store result (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(hash, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(hash, a64::ptr(tmp3));
				}
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				cc.b(done);

				/* Null path: resnull=1, resvalue=0 (steps-relative) */
				cc.bind(is_null);
				cc.mov(tmp1, 1);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				cc.mov(tmp1, 0);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp3));
				}
				cc.b(step_labels[op->d.hashdatum.jumpdone]);

				cc.bind(done);
				break;
			}
#endif /* HAVE_EEOP_HASHDATUM */

			/*
			 * ---- AGG_PLAIN_TRANS (all 6 variants) ----
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
						cc.ldr(pt, a64::ptr(v_steps, STEP_OFFSET(opno, d.agg_trans.pertrans)));
						a64::Gp ac = cc.new_gpx();
						cc.ldr(ac, a64::ptr(v_steps, STEP_OFFSET(opno, d.agg_trans.aggcontext)));
						a64::Gp fn = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn, (void *) ExecAggInitGroup);
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
						ASMJIT_MOV_FN(cc, fn, (void *) jit_error_int8_overflow);
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
					 */
					Label arg_not_null = cc.new_label();
					Label trans_not_null = cc.new_label();
					Label after_sum = cc.new_label();

					/* Load fcinfo from pertrans (steps-relative) */
					int64_t isnull1_off = (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
					a64::Gp fci = cc.new_gpx();
					{
						a64::Gp pt = cc.new_gpx();
						cc.ldr(pt, a64::ptr(v_steps, STEP_OFFSET(opno, d.agg_trans.pertrans)));
						cc.ldr(fci, a64::ptr(pt, offsetof(AggStatePerTransData, transfn_fcinfo)));
					}
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
					{
						a64::Gp pt = cc.new_gpx();
						cc.ldr(pt, a64::ptr(v_steps, STEP_OFFSET(opno, d.agg_trans.pertrans)));
						cc.ldr(fci, a64::ptr(pt, offsetof(AggStatePerTransData, transfn_fcinfo)));
					}
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

					/* Load arg1, sign-extend (steps-relative fcinfo) */
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					a64::Gp arg1 = cc.new_gpx();
					a64::Gp fci = cc.new_gpx();
					{
						a64::Gp pt = cc.new_gpx();
						cc.ldr(pt, a64::ptr(v_steps, STEP_OFFSET(opno, d.agg_trans.pertrans)));
						cc.ldr(fci, a64::ptr(pt, offsetof(AggStatePerTransData, transfn_fcinfo)));
					}
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
					ASMJIT_MOV_FN(cc, afn_reg, helper_fn);
					/* op = &steps[opno] (steps-relative) */
					a64::Gp aop_reg = cc.new_gpx();
					cc.add(aop_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *ainvoke;
					cc.invoke(Out(ainvoke), afn_reg,
							  FuncSignature::build<void, void *, void *>());
					ainvoke->set_arg(0, v_state);
					ainvoke->set_arg(1, aop_reg);
				}

				cc.bind(end_label);
				break;
			}

			case EEOP_HASHED_SCALARARRAYOP:
			{
#if PG_VERSION_NUM >= 150000
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

					a64::Gp fci_reg = cc.new_gpx("fci");
					cc.ldr(fci_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.hashedscalararrayop.fcinfo_data)));
					cc.ldrb(tmp2.w(), a64::ptr(fci_reg, off_arg0_isnull));
					cc.cbnz(tmp2, lbl_null_result);

					a64::Gp scalar = cc.new_gpx("scalar");
					cc.ldr(scalar, a64::ptr(fci_reg, off_arg0_value));

					{
						struct {
							int lo, hi;
							Label entry_label;
							bool has_entry;
						} work[128];
						int work_top = 0;

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

							if (work[work_top].has_entry)
								cc.bind(work[work_top].entry_label);

							if (lo > hi)
							{
								cc.b(lbl_not_found);
								continue;
							}

							if (lo == hi)
							{
								a64::Gp cval = cc.new_gpx();
								cc.mov(cval,
									(uint64_t) sorted_vals[lo]);
								cc.cmp(scalar, cval);
								cc.b(a64::CondCode::kEQ, lbl_found);
								cc.b(lbl_not_found);
								continue;
							}

							mid = lo + (hi - lo) / 2;

							a64::Gp cval = cc.new_gpx();
							cc.mov(cval,
								(uint64_t) sorted_vals[mid]);
							cc.cmp(scalar, cval);
							cc.b(a64::CondCode::kEQ, lbl_found);

							Label lbl_lt = cc.new_label();
							cc.b(a64::CondCode::kLT, lbl_lt);

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

					cc.bind(lbl_found);
					{
						cc.mov(tmp1,
							(uint64_t)(int64_t)(inclause ? 1 : 0));
						if (op->resvalue == &state->resvalue)
							cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(tmp1, a64::ptr(tmp3));
						}
						cc.mov(tmp1, 0);
						if (op->resnull == &state->resnull)
							cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(tmp1.w(), a64::ptr(tmp3));
						}
						cc.b(lbl_done);
					}

					cc.bind(lbl_not_found);
					{
						if (array_has_nulls)
						{
							cc.mov(tmp1, 0);
							if (op->resvalue == &state->resvalue)
								cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
							else
							{
								cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
								cc.str(tmp1, a64::ptr(tmp3));
							}
							cc.mov(tmp1, 1);
							if (op->resnull == &state->resnull)
								cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
							else
							{
								cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
								cc.strb(tmp1.w(), a64::ptr(tmp3));
							}
						}
						else
						{
							cc.mov(tmp1,
								(uint64_t)(int64_t)(inclause ? 0 : 1));
							if (op->resvalue == &state->resvalue)
								cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
							else
							{
								cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
								cc.str(tmp1, a64::ptr(tmp3));
							}
							cc.mov(tmp1, 0);
							if (op->resnull == &state->resnull)
								cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
							else
							{
								cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
								cc.strb(tmp1.w(), a64::ptr(tmp3));
							}
						}
						cc.b(lbl_done);
					}

					cc.bind(lbl_null_result);
					{
						cc.mov(tmp1, 0);
						if (op->resvalue == &state->resvalue)
							cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(tmp1, a64::ptr(tmp3));
						}
						cc.mov(tmp1, 1);
						if (op->resnull == &state->resnull)
							cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(tmp1.w(), a64::ptr(tmp3));
						}
					}

					cc.bind(lbl_done);

					pfree(sorted_vals);
				}
				else
				{
					if (sorted_vals)
						pfree(sorted_vals);

					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalHashedScalarArrayOp);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
				}
#else /* PG14: no inclause/saop -- always use fallback */
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalHashedScalarArrayOp);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
				}
#endif /* PG_VERSION_NUM >= 150000 */
				break;
			}

			/*
			 * ---- PRESORTED DISTINCT ----
			 * Direct call to ExecEvalPreOrderedDistinct{Single,Multi},
			 * branch on result.
			 */
#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
			case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
			case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
			{
				int jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;
				void *fn = (opcode == EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
					? (void *) ExecEvalPreOrderedDistinctSingle
					: (void *) ExecEvalPreOrderedDistinctMulti;

				/* aggstate = state->parent */
				a64::Gp aggst = cc.new_gpx();
				cc.ldr(aggst, a64::ptr(v_state, offsetof(ExprState, parent)));

				/* pertrans = op->d.agg_presorted_distinctcheck.pertrans */
				a64::Gp pt = cc.new_gpx();
				cc.ldr(pt, a64::ptr(v_steps, STEP_OFFSET(opno,
					d.agg_presorted_distinctcheck.pertrans)));

				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, fn);
				InvokeNode *invoke;
				a64::Gp ret_reg = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *>());
				invoke->set_arg(0, aggst);
				invoke->set_arg(1, pt);
				invoke->set_ret(0, ret_reg);

				/* If result == 0 (not distinct), jump to jumpdistinct */
				if (jumpdistinct >= 0 && jumpdistinct < steps_len)
					cc.cbz(ret_reg, step_labels[jumpdistinct]);
				break;
			}
#endif /* HAVE_EEOP_AGG_PRESORTED_DISTINCT */

			/*
			 * ---- BOOLTEST_IS_TRUE / IS_NOT_TRUE / IS_FALSE / IS_NOT_FALSE ----
			 */
			case EEOP_BOOLTEST_IS_TRUE:
			{
				/* If null: resvalue=false, resnull=false. Else keep. */
				Label not_null = cc.new_label();

				/* Load resnull (steps-relative) */
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbz(tmp2, not_null);

				/* Null path: resvalue=false, resnull=false */
				cc.mov(tmp2, 0);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp1));
				}
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				cc.bind(not_null);
				break;
			}

			case EEOP_BOOLTEST_IS_NOT_TRUE:
			{
				/* If null: resvalue=true, resnull=false. Else invert. */
				Label not_null = cc.new_label();
				Label done = cc.new_label();

				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbz(tmp2, not_null);

				/* Null path: resvalue=true, resnull=false */
				cc.mov(tmp2, 1);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp1));
				}
				cc.mov(tmp2, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.b(done);

				/* Not null: invert resvalue */
				cc.bind(not_null);
				if (op->resvalue == &state->resvalue)
				{
					cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				}
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp2, a64::ptr(tmp1));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.str(tmp2, a64::ptr(tmp1));
				}

				cc.bind(done);
				break;
			}

			case EEOP_BOOLTEST_IS_FALSE:
			{
				/* If null: resvalue=false, resnull=false. Else invert. */
				Label not_null = cc.new_label();
				Label done = cc.new_label();

				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbz(tmp2, not_null);

				/* Null path: resvalue=false, resnull=false */
				cc.mov(tmp2, 0);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp1));
				}
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.b(done);

				/* Not null: invert resvalue */
				cc.bind(not_null);
				if (op->resvalue == &state->resvalue)
				{
					cc.ldr(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				}
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp2, a64::ptr(tmp1));
					cc.cmp(tmp2, 0);
					cc.cset(tmp2, a64::CondCode::kEQ);
					cc.str(tmp2, a64::ptr(tmp1));
				}

				cc.bind(done);
				break;
			}

			case EEOP_BOOLTEST_IS_NOT_FALSE:
			{
				/* If null: resvalue=true, resnull=false. Else keep. */
				Label not_null = cc.new_label();

				if (op->resnull == &state->resnull)
					cc.ldrb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.cbz(tmp2, not_null);

				/* Null path: resvalue=true, resnull=false */
				cc.mov(tmp2, 1);
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp1));
				}
				cc.mov(tmp2, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				cc.bind(not_null);
				break;
			}

			/*
			 * ---- AGGREF ----
			 */
			case EEOP_AGGREF:
			{
				int aggno = op->d.aggref.aggno;

				/* resvalue = econtext->ecxt_aggvalues[aggno] */
				cc.ldr(tmp1, a64::ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggvalues)));
				cc.ldr(tmp2, a64::ptr(tmp1,
					aggno * (int64_t) sizeof(Datum)));
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp3));
				}

				/* resnull = econtext->ecxt_aggnulls[aggno] */
				cc.ldr(tmp1, a64::ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggnulls)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1,
					aggno * (int64_t) sizeof(bool)));
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp3));
				}
				break;
			}

			/*
			 * ---- WINDOW_FUNC ----
			 * wfuncno read at runtime (assigned after compilation).
			 */
			case EEOP_WINDOW_FUNC:
			{
				/* tmp1 = wfstate (steps-relative) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.window_func.wfstate)));
				/* tmp2 = wfstate->wfuncno (read at runtime, int32) */
				cc.ldrsw(tmp2, a64::ptr(tmp1,
					offsetof(WindowFuncExprState, wfuncno)));

				/* tmp3 = econtext->ecxt_aggvalues */
				cc.ldr(tmp3, a64::ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggvalues)));
				/* tmp1 = tmp3 + tmp2*8 (Datum is 8 bytes) */
				{
					a64::Gp idx = cc.new_gpx();
					cc.lsl(idx, tmp2, 3);
					cc.ldr(tmp1, a64::ptr(tmp3, idx));
				}
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp3));
				}

				/* Reload wfuncno for nulls */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.window_func.wfstate)));
				cc.ldrsw(tmp2, a64::ptr(tmp1,
					offsetof(WindowFuncExprState, wfuncno)));
				cc.ldr(tmp3, a64::ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggnulls)));
				cc.ldrb(tmp1.w(), a64::ptr(tmp3, tmp2));
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp3));
				}
				break;
			}

			/*
			 * ---- CASE_TESTVAL / DOMAIN_TESTVAL ----
			 * Copy from cached pointer (steps-relative).
			 */
			case EEOP_CASE_TESTVAL:
			case EEOP_DOMAIN_TESTVAL:
			{
				/*
				 * If d.casetest.value is non-NULL, dereference it.
				 * Otherwise read from econtext->caseValue (or domainValue).
				 * Matches interpreter NULL-check for callers like JSON_TABLE
				 * that set caseValue via econtext at runtime.
				 */
				if (op->d.casetest.value) {
					cc.ldr(tmp1, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.casetest.value)));
					cc.ldr(tmp2, a64::ptr(tmp1));
				} else {
					int cv_off = (opcode == EEOP_CASE_TESTVAL)
						? offsetof(ExprContext, caseValue_datum)
						: offsetof(ExprContext, domainValue_datum);
					cc.ldr(tmp2, a64::ptr(v_econtext, cv_off));
				}
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp3));
				}

				if (op->d.casetest.isnull) {
					cc.ldr(tmp1, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.casetest.isnull)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1));
				} else {
					int cn_off = (opcode == EEOP_CASE_TESTVAL)
						? offsetof(ExprContext, caseValue_isNull)
						: offsetof(ExprContext, domainValue_isNull);
					cc.ldrb(tmp2.w(), a64::ptr(v_econtext, cn_off));
				}
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp3));
				}
				break;
			}

#ifdef HAVE_EEOP_TESTVAL_EXT
			case EEOP_CASE_TESTVAL_EXT:
			{
				/* *op->resvalue = econtext->caseValue_datum */
				cc.ldr(tmp1, a64::ptr(v_econtext,
					offsetof(ExprContext, caseValue_datum)));
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}

				/* *op->resnull = econtext->caseValue_isNull */
				cc.ldrb(tmp1.w(), a64::ptr(v_econtext,
					offsetof(ExprContext, caseValue_isNull)));
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				break;
			}

			case EEOP_DOMAIN_TESTVAL_EXT:
			{
				/* *op->resvalue = econtext->domainValue_datum */
				cc.ldr(tmp1, a64::ptr(v_econtext,
					offsetof(ExprContext, domainValue_datum)));
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}

				/* *op->resnull = econtext->domainValue_isNull */
				cc.ldrb(tmp1.w(), a64::ptr(v_econtext,
					offsetof(ExprContext, domainValue_isNull)));
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				break;
			}
#endif /* HAVE_EEOP_TESTVAL_EXT */

			/*
			 * ---- MAKE_READONLY ----
			 */
			case EEOP_MAKE_READONLY:
			{
				Label skip_null = cc.new_label();

				/* tmp2 = *op->d.make_readonly.isnull (steps-relative) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.make_readonly.isnull)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));

				/* *op->resnull = isnull */
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				/* If null, skip */
				cc.cbnz(tmp2, skip_null);

				/* Not null: resvalue = MakeExpandedObjectReadOnlyInternal(*value) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.make_readonly.value)));
				cc.ldr(tmp1, a64::ptr(tmp1));
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) MakeExpandedObjectReadOnlyInternal);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					invoke->set_arg(0, tmp1);
					invoke->set_ret(0, tmp1);
				}
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}

				cc.bind(skip_null);
				break;
			}

			/*
			 * ---- AGG_STRICT_INPUT_CHECK_ARGS ----
			 */
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
#ifdef HAVE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1
			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1:
#endif
			{
				NullableDatum *args = op->d.agg_strict_input_check.args;
				int nargs = op->d.agg_strict_input_check.nargs;
				int jumpnull = op->d.agg_strict_input_check.jumpnull;

				int64_t isnull_off0 = offsetof(NullableDatum, isnull);
				int64_t nd_size = (int64_t) sizeof(NullableDatum);

				/* tmp1 = args base (steps-relative) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.agg_strict_input_check.args)));

				if (nargs > 1 && nargs <= 4)
				{
					cc.ldrb(tmp2.w(), a64::ptr(tmp1, isnull_off0));
					for (int argno = 1; argno < nargs; argno++)
					{
						a64::Gp t = cc.new_gpx();
						cc.ldrb(t.w(), a64::ptr(tmp1,
							argno * nd_size + isnull_off0));
						cc.orr(tmp2, tmp2, t);
					}
					cc.cbnz(tmp2, step_labels[jumpnull]);
				}
				else
				{
					for (int argno = 0; argno < nargs; argno++)
					{
						cc.ldrb(tmp2.w(), a64::ptr(tmp1,
							argno * nd_size + isnull_off0));
						cc.cbnz(tmp2, step_labels[jumpnull]);
					}
				}
				break;
			}

			/*
			 * ---- AGG_STRICT_INPUT_CHECK_NULLS ----
			 */
			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
			{
				bool *nulls = op->d.agg_strict_input_check.nulls;
				int nargs = op->d.agg_strict_input_check.nargs;
				int jumpnull = op->d.agg_strict_input_check.jumpnull;

				/* tmp1 = nulls base (steps-relative) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.agg_strict_input_check.nulls)));

				if (nargs > 1 && nargs <= 4)
				{
					cc.ldrb(tmp2.w(), a64::ptr(tmp1, 0));
					for (int argno = 1; argno < nargs; argno++)
					{
						a64::Gp t = cc.new_gpx();
						cc.ldrb(t.w(), a64::ptr(tmp1,
							argno * (int64_t) sizeof(bool)));
						cc.orr(tmp2, tmp2, t);
					}
					cc.cbnz(tmp2, step_labels[jumpnull]);
				}
				else
				{
					for (int argno = 0; argno < nargs; argno++)
					{
						cc.ldrb(tmp2.w(), a64::ptr(tmp1,
							argno * (int64_t) sizeof(bool)));
						cc.cbnz(tmp2, step_labels[jumpnull]);
					}
				}
				break;
			}

			/*
			 * ---- AGG_PLAIN_PERGROUP_NULLCHECK ----
			 */
			case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
			{
				int setoff = op->d.agg_plain_pergroup_nullcheck.setoff;
				int jumpnull = op->d.agg_plain_pergroup_nullcheck.jumpnull;

				/* tmp1 = state->parent (AggState*) */
				cc.ldr(tmp1, a64::ptr(v_state,
					offsetof(ExprState, parent)));
				/* tmp1 = aggstate->all_pergroups */
				cc.ldr(tmp1, a64::ptr(tmp1,
					offsetof(AggState, all_pergroups)));
				/* tmp1 = all_pergroups[setoff] */
				cc.ldr(tmp1, a64::ptr(tmp1,
					setoff * (int64_t) sizeof(AggStatePerGroup)));

				/* If NULL, jump to jumpnull */
				cc.cbz(tmp1, step_labels[jumpnull]);
				break;
			}

			/*
			 * ---- AGG_STRICT_DESERIALIZE / AGG_DESERIALIZE ----
			 */
			case EEOP_AGG_STRICT_DESERIALIZE:
			case EEOP_AGG_DESERIALIZE:
			{
				FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;

				if (opcode == EEOP_AGG_STRICT_DESERIALIZE)
				{
					/* Check args[0].isnull → jump to jumpnull (steps-relative) */
					int64_t null_off =
						(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
					cc.ldr(tmp1, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.agg_deserialize.fcinfo_data)));
					cc.ldrb(tmp2.w(), a64::ptr(tmp1, null_off));
					cc.cbnz(tmp2, step_labels[op->d.agg_deserialize.jumpnull]);
				}

				/* fcinfo->isnull = false (steps-relative) */
				{
					a64::Gp fci = cc.new_gpx();
					cc.ldr(fci, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.agg_deserialize.fcinfo_data)));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci,
						offsetof(FunctionCallInfoBaseData, isnull)));

					/* R0 = fn_addr(fcinfo) */
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) fcinfo->flinfo->fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci);
					invoke->set_ret(0, tmp1);
				}

				/* *op->resvalue = result (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}

				/* *op->resnull = fcinfo->isnull (steps-relative) */
				{
					a64::Gp fci2 = cc.new_gpx();
					cc.ldr(fci2, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.agg_deserialize.fcinfo_data)));
					cc.ldrb(tmp2.w(), a64::ptr(fci2,
						offsetof(FunctionCallInfoBaseData, isnull)));
					if (op->resnull == &state->resnull)
						cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp2.w(), a64::ptr(tmp1));
					}
				}
				break;
			}

			/*
			 * ---- DISTINCT / NOT_DISTINCT ----
			 * 3-way null logic + equality function call.
			 */
			case EEOP_DISTINCT:
			case EEOP_NOT_DISTINCT:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				Label one_null = cc.new_label();
				Label both_null = cc.new_label();
				Label done = cc.new_label();

				int64_t null0_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				int64_t null1_off =
					(int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);

				/* Load fcinfo (steps-relative) and both isnull flags */
				cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
				cc.ldrb(tmp1.w(), a64::ptr(tmp3, null0_off));
				cc.ldrb(tmp2.w(), a64::ptr(tmp3, null1_off));

				/* If null flags differ → one_null */
				cc.cmp(tmp1, tmp2);
				cc.b(a64::CondCode::kNE, one_null);

				/* Null flags equal. If both null (tmp1 != 0) → both_null */
				cc.cbnz(tmp1, both_null);

				/* Neither null: call equality function */
				cc.mov(tmp2, 0);
				cc.strb(tmp2.w(), a64::ptr(tmp3,
					offsetof(FunctionCallInfoBaseData, isnull)));
				{
					a64::Gp fn_reg = cc.new_gpx();
					if (asmjit_shared_code_mode)
						cc.ldr(fn_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fn_addr)));
					else
						ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.func.fn_addr);
					a64::Gp fcinfo_reg = cc.new_gpx();
					cc.ldr(fcinfo_reg, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.func.fcinfo_data)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}

				/* For DISTINCT: invert result */
				if (opcode == EEOP_DISTINCT)
				{
					cc.cmp(tmp1, 0);
					cc.cset(tmp1, a64::CondCode::kEQ);
				}
				/* Store result, resnull=false */
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				cc.b(done);

				/* one_null: nulls differ → DISTINCT=true, NOT_DISTINCT=false */
				cc.bind(one_null);
				{
					cc.mov(tmp1, (opcode == EEOP_DISTINCT) ? 1 : 0);
					if (op->resvalue == &state->resvalue)
						cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp1, a64::ptr(tmp2));
					}
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}
				cc.b(done);

				/* both_null: DISTINCT=false, NOT_DISTINCT=true */
				cc.bind(both_null);
				{
					cc.mov(tmp1, (opcode == EEOP_DISTINCT) ? 0 : 1);
					if (op->resvalue == &state->resvalue)
						cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp1, a64::ptr(tmp2));
					}
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}

				cc.bind(done);
				break;
			}

			/*
			 * ---- NULLIF ----
			 */
			case EEOP_NULLIF:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				Label a_null = cc.new_label();
				Label b_null = cc.new_label();
				Label not_equal = cc.new_label();
				Label done = cc.new_label();

				int64_t null0_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				int64_t null1_off =
					(int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
				int64_t val0_off =
					(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);

				/* Load fcinfo (steps-relative) */
				cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));

				/* Check if arg0 is null */
				cc.ldrb(tmp1.w(), a64::ptr(tmp3, null0_off));
				cc.cbnz(tmp1, a_null);

				/* Check if arg1 is null → return a */
				cc.ldrb(tmp1.w(), a64::ptr(tmp3, null1_off));
				cc.cbnz(tmp1, b_null);

				/* Both non-null: call equality function */
				cc.mov(tmp2, 0);
				cc.strb(tmp2.w(), a64::ptr(tmp3,
					offsetof(FunctionCallInfoBaseData, isnull)));
				{
					a64::Gp fn_reg = cc.new_gpx();
					if (asmjit_shared_code_mode)
						cc.ldr(fn_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fn_addr)));
					else
						ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.func.fn_addr);
					a64::Gp fcinfo_reg = cc.new_gpx();
					cc.ldr(fcinfo_reg, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.func.fcinfo_data)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}

				/* If fcinfo->isnull, treat as not equal */
				{
					a64::Gp fci2 = cc.new_gpx();
					cc.ldr(fci2, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.func.fcinfo_data)));
					cc.ldrb(tmp2.w(), a64::ptr(fci2,
						offsetof(FunctionCallInfoBaseData, isnull)));
					cc.cbnz(tmp2, not_equal);
				}

				/* If result == false (0), not equal */
				cc.cbz(tmp1, not_equal);

				/* Equal: return null */
				cc.mov(tmp2, 1);
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}
				cc.b(done);

				/* not_equal / b_null: return a's value */
				cc.bind(not_equal);
				cc.bind(b_null);
				{
					a64::Gp fci3 = cc.new_gpx();
					cc.ldr(fci3, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.func.fcinfo_data)));
					cc.ldr(tmp1, a64::ptr(fci3, val0_off));
				}
				if (op->resvalue == &state->resvalue)
					cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp1, a64::ptr(tmp2));
				}
				cc.mov(tmp1, 0);
				if (op->resnull == &state->resnull)
					cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp1.w(), a64::ptr(tmp2));
				}
				cc.b(done);

				/* a_null: resnull = true */
				cc.bind(a_null);
				cc.mov(tmp2, 1);
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp1));
				}

				cc.bind(done);
				break;
			}

			/*
			 * ---- PARAM_EXEC (inlined) ----
			 * Fast path: load value/isnull directly from econtext->ecxt_param_exec_vals.
			 * Slow path (execPlan != NULL, i.e. lazy InitPlan): call ExecEvalParamExec.
			 */
			case EEOP_PARAM_EXEC:
			{
				int paramid = op->d.param.paramid;
				Label slow_path = cc.new_label();
				Label done = cc.new_label();

				/* prm = econtext->ecxt_param_exec_vals[paramid] */
				a64::Gp prm = cc.new_gpx();
				cc.ldr(prm, a64::ptr(v_econtext,
						offsetof(ExprContext, ecxt_param_exec_vals)));
				cc.add(prm, prm,
						(int64_t)(paramid * sizeof(ParamExecData)));

				/* if (unlikely(prm->execPlan != NULL)) goto slow_path */
				a64::Gp exec_plan = cc.new_gpx();
				cc.ldr(exec_plan, a64::ptr(prm,
						offsetof(ParamExecData, execPlan)));
				cc.cbnz(exec_plan, slow_path);

				/* Fast path: *resvalue = prm->value; *resnull = prm->isnull */
				{
					a64::Gp val = cc.new_gpx();
					a64::Gp null_flag = cc.new_gpx();
					cc.ldr(val, a64::ptr(prm, offsetof(ParamExecData, value)));
					cc.ldrb(null_flag.w(), a64::ptr(prm,
							offsetof(ParamExecData, isnull)));

					if (op->resvalue == &state->resvalue)
						cc.str(val, a64::ptr(v_state,
								offsetof(ExprState, resvalue)));
					else
					{
						a64::Gp dst = cc.new_gpx();
						cc.ldr(dst, a64::ptr(v_steps,
								STEP_OFFSET(opno, resvalue)));
						cc.str(val, a64::ptr(dst));
					}
					if (op->resnull == &state->resnull)
						cc.strb(null_flag.w(), a64::ptr(v_state,
								offsetof(ExprState, resnull)));
					else
					{
						a64::Gp dst = cc.new_gpx();
						cc.ldr(dst, a64::ptr(v_steps,
								STEP_OFFSET(opno, resnull)));
						cc.strb(null_flag.w(), a64::ptr(dst));
					}
				}
				cc.b(done);

				/* Slow path: execPlan set — call ExecEvalParamExec */
				cc.bind(slow_path);
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalParamExec);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps,
							(int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							FuncSignature::build<void, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
				}
				cc.bind(done);
				break;
			}

			/*
			 * ---- DIRECT-CALL OPCODES ----
			 * These opcodes have PG-exported functions with matching signatures.
			 * Instead of going through fallback_step's switch, we call the
			 * function directly.
			 */

			/*
			 * ---- FIELDSELECT (partially inlined) ----
			 * If input is null, result is already null — skip the C call.
			 * Otherwise call ExecEvalFieldSelect(state, op, econtext).
			 */
			case EEOP_FIELDSELECT:
			{
				/* If *op->resnull, input is null → output is null, skip */
				a64::Gp resnull_val = cc.new_gpx();
				if (op->resnull == &state->resnull)
					cc.ldrb(resnull_val.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					a64::Gp rn_addr = cc.new_gpx();
					cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(resnull_val.w(), a64::ptr(rn_addr));
				}
				Label fs_done = cc.new_label();
				cc.cbnz(resnull_val.w(), fs_done);

				/* Non-null: call ExecEvalFieldSelect(state, op, econtext) */
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalFieldSelect);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);

				cc.bind(fs_done);
				break;
			}

			/*
			 * ---- NEXTVALUEEXPR (inlined) ----
			 * Embed seqid as immediate, call nextval_internal directly.
			 * Compile-time switch on seqtypid eliminates runtime type dispatch.
			 */
			case EEOP_NEXTVALUEEXPR:
			{
				Oid seqid = op->d.nextvalueexpr.seqid;
				Oid seqtypid = op->d.nextvalueexpr.seqtypid;

				/* nextval_internal(seqid, false) -> int64 */
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) nextval_internal);
				a64::Gp seqid_reg = cc.new_gpx();
				cc.mov(seqid_reg, (uint64_t) seqid);
				a64::Gp false_reg = cc.new_gpx();
				cc.mov(false_reg, 0);
				InvokeNode *invoke;
				a64::Gp result = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, int32_t, int32_t>());
				invoke->set_arg(0, seqid_reg);
				invoke->set_arg(1, false_reg);
				invoke->set_ret(0, result);

				/* Compile-time cast based on seqtypid */
				switch (seqtypid) {
				case INT2OID:
					cc.sxth(result, result);
					break;
				case INT4OID:
					cc.sxtw(result, result);
					break;
				case INT8OID:
					break; /* int64 -> Datum identity */
				}

				/* *op->resvalue = result */
				if (op->resvalue == &state->resvalue)
					cc.str(result, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					a64::Gp rv_addr = cc.new_gpx();
					cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(result, a64::ptr(rv_addr));
				}
				/* *op->resnull = false */
				{
					a64::Gp zero = cc.new_gpx();
					cc.mov(zero, 0);
					if (op->resnull == &state->resnull)
						cc.strb(zero.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						a64::Gp rn_addr = cc.new_gpx();
						cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(zero.w(), a64::ptr(rn_addr));
					}
				}
				break;
			}

			/*
			 * ---- ROW (inlined) ----
			 * Direct call to heap_form_tuple + inline HeapTupleGetDatum.
			 */
			case EEOP_ROW:
			{
				/* Load args from step data */
				a64::Gp tupdesc_reg = cc.new_gpx();
				cc.ldr(tupdesc_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.row.tupdesc)));
				a64::Gp vals_reg = cc.new_gpx();
				cc.ldr(vals_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.row.elemvalues)));
				a64::Gp nulls_reg = cc.new_gpx();
				cc.ldr(nulls_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.row.elemnulls)));

				/* Call heap_form_tuple(tupdesc, values, isnull) -> HeapTuple */
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) heap_form_tuple);
				InvokeNode *invoke;
				a64::Gp tuple_reg = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void *, void *, void *, void *>());
				invoke->set_arg(0, tupdesc_reg);
				invoke->set_arg(1, vals_reg);
				invoke->set_arg(2, nulls_reg);
				invoke->set_ret(0, tuple_reg);

				/* HeapTupleGetDatum(tuple) = HeapTupleHeaderGetDatum(tuple->t_data)
				 * Must call HeapTupleHeaderGetDatum to flatten external TOAST ptrs */
				a64::Gp tdata_reg = cc.new_gpx();
				cc.ldr(tdata_reg, a64::Mem(tuple_reg, (int32_t) offsetof(HeapTupleData, t_data)));

				a64::Gp fn_htgd = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_htgd, (void *) HeapTupleHeaderGetDatum);
				InvokeNode *invoke_htgd;
				a64::Gp datum_reg = cc.new_gpx();
				cc.invoke(Out(invoke_htgd), fn_htgd,
						  FuncSignature::build<uint64_t, void *>());
				invoke_htgd->set_arg(0, tdata_reg);
				invoke_htgd->set_ret(0, datum_reg);

				/* *op->resvalue = datum, *op->resnull = false */
				if (op->resvalue == &state->resvalue)
					cc.str(datum_reg, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					a64::Gp rv_addr = cc.new_gpx();
					cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(datum_reg, a64::ptr(rv_addr));
				}
				{
					a64::Gp zero = cc.new_gpx();
					cc.mov(zero, 0);
					if (op->resnull == &state->resnull)
						cc.strb(zero.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						a64::Gp rn_addr = cc.new_gpx();
						cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(zero.w(), a64::ptr(rn_addr));
					}
				}
				break;
			}

			/*
			 * ---- AGG_ORDERED_TRANS_DATUM ----
			 * Direct call (AggStatePerTransData is opaque in headers).
			 */
			case EEOP_AGG_ORDERED_TRANS_DATUM:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalAggOrderedTransDatum);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/*
			 * ---- AGG_ORDERED_TRANS_TUPLE ----
			 * Direct call (AggStatePerTransData is opaque in headers).
			 */
			case EEOP_AGG_ORDERED_TRANS_TUPLE:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalAggOrderedTransTuple);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/*
			 * ---- CONVERT_ROWTYPE (partially inlined) ----
			 * Null check: if *op->resnull, result stays null — skip C call.
			 */
			case EEOP_CONVERT_ROWTYPE:
			{
				Label cvt_done = cc.new_label();

				/* Check *op->resnull */
				a64::Gp resnull_val = cc.new_gpx();
				if (op->resnull == &state->resnull)
					cc.ldrb(resnull_val.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					a64::Gp rn_addr = cc.new_gpx();
					cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(resnull_val.w(), a64::ptr(rn_addr));
				}
				cc.cbnz(resnull_val.w(), cvt_done);

				/* Non-null: call ExecEvalConvertRowtype(state, op, econtext) */
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalConvertRowtype);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
				}

				cc.bind(cvt_done);
				break;
			}

#ifdef HAVE_EEOP_JSONEXPR
			/*
			 * ---- JSONEXPR_COERCION_FINISH (partially inlined, PG17+) ----
			 * Happy path: if escontext.error_occurred is false, no-op.
			 * Error path: fall back to ExecEvalJsonCoercionFinish.
			 */
			case EEOP_JSONEXPR_COERCION_FINISH:
			{
				Label jcf_done = cc.new_label();

				/* Compute struct offset at JIT compile time */
				JsonExprState *jsestate = op->d.jsonexpr.jsestate;
				int32_t esctx_err_off = (int32_t)(
					(char *)&jsestate->escontext.error_occurred - (char *)jsestate);

				/* Load jsestate from step data */
				a64::Gp jse_reg = cc.new_gpx();
				cc.ldr(jse_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.jsonexpr.jsestate)));

				/* Check error_occurred flag */
				a64::Gp err_flag = cc.new_gpx();
				cc.ldrb(err_flag.w(), a64::Mem(jse_reg, esctx_err_off));
				cc.cbz(err_flag.w(), jcf_done);

				/* Error: call ExecEvalJsonCoercionFinish(state, op) */
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalJsonCoercionFinish);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
				}

				cc.bind(jcf_done);
				break;
			}
#endif /* HAVE_EEOP_JSONEXPR */

			/*
			 * ---- WHOLEROW ----
			 * Direct call to ExecEvalWholeRowVar (complex first-time init).
			 */
			case EEOP_WHOLEROW:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalWholeRowVar);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
			/*
			 * ---- JSON_CONSTRUCTOR (PG16+) ----
			 * Direct call to ExecEvalJsonConstructor.
			 */
			case EEOP_JSON_CONSTRUCTOR:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalJsonConstructor);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}
#endif /* HAVE_EEOP_JSON_CONSTRUCTOR */

			/* 3-arg: fn(ExprState *state, ExprEvalStep *op, ExprContext *econtext) */
			case EEOP_FUNCEXPR_FUSAGE:
			case EEOP_FUNCEXPR_STRICT_FUSAGE:
			case EEOP_NULLTEST_ROWISNULL:
			case EEOP_NULLTEST_ROWISNOTNULL:
#ifdef HAVE_EEOP_PARAM_SET
			case EEOP_PARAM_SET:
#endif
			case EEOP_ARRAYCOERCE:
			case EEOP_FIELDSTORE_DEFORM:
			case EEOP_FIELDSTORE_FORM:
#ifdef HAVE_EEOP_JSONEXPR
			case EEOP_JSONEXPR_COERCION:
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
			case EEOP_MERGE_SUPPORT_FUNC:
#endif
			case EEOP_SUBPLAN:
			case EEOP_PARAM_EXTERN:
			{
				void *fn;

				switch (opcode)
				{
					case EEOP_FUNCEXPR_FUSAGE:
						fn = (void *) ExecEvalFuncExprFusage; break;
					case EEOP_FUNCEXPR_STRICT_FUSAGE:
						fn = (void *) ExecEvalFuncExprStrictFusage; break;
					case EEOP_NULLTEST_ROWISNULL:
						fn = (void *) ExecEvalRowNull; break;
					case EEOP_NULLTEST_ROWISNOTNULL:
						fn = (void *) ExecEvalRowNotNull; break;
#ifdef HAVE_EEOP_PARAM_SET
					case EEOP_PARAM_SET:
						fn = (void *) ExecEvalParamSet; break;
#endif
					case EEOP_ARRAYCOERCE:
						fn = (void *) ExecEvalArrayCoerce; break;
					case EEOP_FIELDSTORE_DEFORM:
						fn = (void *) ExecEvalFieldStoreDeForm; break;
					case EEOP_FIELDSTORE_FORM:
						fn = (void *) ExecEvalFieldStoreForm; break;
#ifdef HAVE_EEOP_JSONEXPR
					case EEOP_JSONEXPR_COERCION:
						fn = (void *) ExecEvalJsonCoercion; break;
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
					case EEOP_MERGE_SUPPORT_FUNC:
						fn = (void *) ExecEvalMergeSupportFunc; break;
#endif
					case EEOP_SUBPLAN:
						fn = (void *) ExecEvalSubPlan; break;
					case EEOP_PARAM_EXTERN:
						fn = (void *) ExecEvalParamExtern; break;
					default:
						pg_unreachable();
				}

				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, fn);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/*
			 * ---- DOMAIN_NOTNULL (inlined) ----
			 * Fast path: if *op->resnull is false, done.
			 * Slow path: call ExecEvalConstraintNotNull(state, op).
			 */
			case EEOP_DOMAIN_NOTNULL:
			{
				Label lbl_ok = cc.new_label();

				/* tmp1 = *op->resnull */
				if (op->resnull == &state->resnull)
					cc.ldrb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp1.w(), a64::ptr(tmp2));
				}
				cc.cbz(tmp1, lbl_ok);  /* not null → done */

				/* Slow path: call ExecEvalConstraintNotNull(state, op) */
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalConstraintNotNull);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
				}

				cc.bind(lbl_ok);
				break;
			}

			/*
			 * ---- DOMAIN_CHECK (inlined) ----
			 * Fast path: if *checknull → done (NULL input bypasses check).
			 *            else if DatumGetBool(*checkvalue) → done.
			 * Slow path: call ExecEvalConstraintCheck(state, op).
			 */
			case EEOP_DOMAIN_CHECK:
			{
				Label lbl_done = cc.new_label();

				/* tmp1 = *op->d.domaincheck.checknull */
				cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, d.domaincheck.checknull)));
				cc.ldrb(tmp1.w(), a64::ptr(tmp2));
				cc.cbnz(tmp1, lbl_done);  /* checknull → done */

				/* tmp1 = DatumGetBool(*op->d.domaincheck.checkvalue) */
				cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, d.domaincheck.checkvalue)));
				cc.ldr(tmp1, a64::ptr(tmp2));
				cc.cbnz(tmp1, lbl_done);  /* check passed → done */

				/* Slow path: call ExecEvalConstraintCheck(state, op) */
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalConstraintCheck);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
				}

				cc.bind(lbl_done);
				break;
			}

			/*
			 * ---- GROUPING_FUNC (inlined) ----
			 * Unroll the clause loop at compile time (cap at 8).
			 */
			case EEOP_GROUPING_FUNC:
			{
				List *clauses = op->d.grouping_func.clauses;
				int nclauses = list_length(clauses);

				if (nclauses > 8) {
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalGroupingFunc);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
				} else {
					a64::Gp v_result = cc.new_gpx("grp_result");
					a64::Gp v_grouped_cols = cc.new_gpx("grouped_cols");
					a64::Gp v_parent = cc.new_gpx("parent");

					/* v_parent = state->parent (AggState*) */
					cc.ldr(v_parent, a64::ptr(v_state, offsetof(ExprState, parent)));
					/* v_grouped_cols = aggstate->grouped_cols */
					cc.ldr(v_grouped_cols, a64::ptr(v_parent, offsetof(AggState, grouped_cols)));
					cc.mov(v_result, 0);

					ListCell *lc;
					foreach (lc, clauses) {
						int attnum = lfirst_int(lc);
						Label lbl_is_member = cc.new_label();

						/* result <<= 1 */
						cc.lsl(v_result, v_result, 1);

						/* call bms_is_member(attnum, grouped_cols) */
						{
							a64::Gp bms_fn = cc.new_gpx();
							ASMJIT_MOV_FN(cc, bms_fn, (void *) bms_is_member);
							InvokeNode *invoke;
							cc.invoke(Out(invoke), bms_fn,
									  FuncSignature::build<int32_t, int32_t, void *>());
							invoke->set_arg(0, Imm(attnum));
							invoke->set_arg(1, v_grouped_cols);
							invoke->set_ret(0, tmp1);
						}

						/* if (bms_is_member) skip OR */
						cc.cbnz(tmp1, lbl_is_member);
						cc.orr(v_result, v_result, 1);
						cc.bind(lbl_is_member);
					}

					/* *op->resvalue = Int32GetDatum(result) */
					if (op->resvalue == &state->resvalue)
						cc.str(v_result, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(v_result, a64::ptr(tmp2));
					}
					/* *op->resnull = false */
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}
				break;
			}

			/*
			 * ---- MINMAX (GREATEST/LEAST, partially inlined) ----
			 * For nelems <= 4: unroll the loop.
			 * For nelems > 4: fall back to C call.
			 */
			case EEOP_MINMAX:
			{
				int nelems = op->d.minmax.nelems;

				if (nelems > 4) {
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalMinMax);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
				} else {
					/* Set *op->resnull = true initially */
					cc.mov(tmp1, 1);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}

					for (int i = 0; i < nelems; i++) {
						Label lbl_null = cc.new_label();
						Label lbl_skip_cmp = cc.new_label();
						Label lbl_not_first = cc.new_label();

						/* tmp1 = op->d.minmax.nulls[i] */
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, d.minmax.nulls)));
						cc.ldrb(tmp1.w(), a64::Mem(tmp2, (int32_t)(i * sizeof(bool))));
						cc.cbnz(tmp1, lbl_null);

						/* tmp1 = op->d.minmax.values[i] */
						a64::Gp v_newval = cc.new_gpx("newval");
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, d.minmax.values)));
						cc.ldr(v_newval, a64::Mem(tmp2, (int32_t)(i * sizeof(Datum))));

						/* Check if *resnull */
						if (op->resnull == &state->resnull)
							cc.ldrb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.ldrb(tmp1.w(), a64::ptr(tmp2));
						}
						cc.cbz(tmp1, lbl_not_first);

						/* First non-null: adopt value */
						if (op->resvalue == &state->resvalue)
							cc.str(v_newval, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(v_newval, a64::ptr(tmp2));
						}
						cc.mov(tmp1, 0);
						if (op->resnull == &state->resnull)
							cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(tmp1.w(), a64::ptr(tmp2));
						}
						cc.b(lbl_skip_cmp);

						/* Not first: compare */
						cc.bind(lbl_not_first);
						{
							/* Load fcinfo */
							a64::Gp v_fcinfo = cc.new_gpx("mm_fcinfo");
							cc.ldr(v_fcinfo, a64::ptr(v_steps, STEP_OFFSET(opno, d.minmax.fcinfo_data)));
							/* fcinfo->args[0].value = *resvalue */
							a64::Gp v_curval = cc.new_gpx("curval");
							if (op->resvalue == &state->resvalue)
								cc.ldr(v_curval, a64::ptr(v_state, offsetof(ExprState, resvalue)));
							else
							{
								cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
								cc.ldr(v_curval, a64::ptr(tmp2));
							}
							cc.str(v_curval, a64::Mem(v_fcinfo, (int32_t)offsetof(FunctionCallInfoBaseData, args[0].value)));
							cc.str(v_newval, a64::Mem(v_fcinfo, (int32_t)offsetof(FunctionCallInfoBaseData, args[1].value)));
							/* fcinfo->isnull = false */
							cc.mov(tmp1, 0);
							cc.strb(tmp1.w(), a64::Mem(v_fcinfo, (int32_t)offsetof(FunctionCallInfoBaseData, isnull)));

							/* Load fn_addr from fcinfo->flinfo->fn_addr */
							a64::Gp v_flinfo = cc.new_gpx();
							cc.ldr(v_flinfo, a64::Mem(v_fcinfo, (int32_t)offsetof(FunctionCallInfoBaseData, flinfo)));
							a64::Gp v_fn_addr = cc.new_gpx();
							cc.ldr(v_fn_addr, a64::Mem(v_flinfo, (int32_t)offsetof(FmgrInfo, fn_addr)));

							/* Call comparison: cmpresult = fn_addr(fcinfo) */
							InvokeNode *invoke;
							cc.invoke(Out(invoke), v_fn_addr,
									  FuncSignature::build<int64_t, void *>());
							invoke->set_arg(0, v_fcinfo);
							invoke->set_ret(0, tmp1);

							/* Check cmpresult */
							Label lbl_no_update = cc.new_label();
							if (op->d.minmax.op == IS_LEAST) {
								/* LEAST: update if cmpresult > 0 */
								cc.cmp(tmp1, 0);
								cc.b_le(lbl_no_update);
							} else {
								/* GREATEST: update if cmpresult < 0 */
								cc.cmp(tmp1, 0);
								cc.b_ge(lbl_no_update);
							}
							if (op->resvalue == &state->resvalue)
								cc.str(v_newval, a64::ptr(v_state, offsetof(ExprState, resvalue)));
							else
							{
								cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
								cc.str(v_newval, a64::ptr(tmp2));
							}
							cc.bind(lbl_no_update);
						}

						cc.bind(lbl_skip_cmp);
						cc.bind(lbl_null);
					}
				}
				break;
			}

			/*
			 * ---- SCALARARRAYOP (partially inlined) ----
			 * Inline the NULL-array check, then call helper with SIMD dispatch.
			 */
			case EEOP_SCALARARRAYOP:
			{
				Label lbl_done = cc.new_label();

				/* 1. Check *op->resnull — if true, array is NULL → skip */
				if (op->resnull == &state->resnull) {
					cc.ldrb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				} else {
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.ldrb(tmp1.w(), a64::ptr(tmp1, 0));
				}
				cc.cbnz(tmp1, lbl_done);

				/* 2. Load *op->resvalue → call DatumGetArrayTypeP */
				if (op->resvalue == &state->resvalue) {
					cc.ldr(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				} else {
					cc.ldr(tmp1, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.ldr(tmp1, a64::ptr(tmp1, 0));
				}
				{
					a64::Gp fn_detoast = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_detoast, (void *) pg_detoast_datum);
					InvokeNode *inv_detoast;
					cc.invoke(Out(inv_detoast), fn_detoast,
							  FuncSignature::build<void *, void *>());
					inv_detoast->set_arg(0, tmp1);
					inv_detoast->set_ret(0, tmp1);
				}
				/* tmp1 = arr pointer — save it */
				a64::Gp v_arr = cc.new_gpx("arr");
				cc.mov(v_arr, tmp1);

				/* 3. Load scalar value and null from fcinfo->args[0] */
				a64::Gp v_scalar_val = cc.new_gpx("scalar_val");
				a64::Gp v_scalar_null = cc.new_gpx("scalar_null");
				{
					a64::Gp v_fcinfo = cc.new_gpx("fcinfo");
					cc.ldr(v_fcinfo, a64::ptr(v_steps,
						STEP_OFFSET(opno, d.scalararrayop.fcinfo_data)));
					cc.ldr(v_scalar_val, a64::ptr(v_fcinfo,
						(int64_t)offsetof(FunctionCallInfoBaseData, args[0].value)));
					cc.ldrb(v_scalar_null.w(), a64::ptr(v_fcinfo,
						(int64_t)offsetof(FunctionCallInfoBaseData, args[0].isnull)));
				}

				/* 4. Call pg_jitter_scalararrayop_loop(op, arr, scalar_value, scalar_null) */
				{
					a64::Gp op_reg = cc.new_gpx("sao_op");
					cc.add(op_reg, v_steps,
						   (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					a64::Gp fn_loop = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_loop,
								  (void *) pg_jitter_scalararrayop_loop);
					InvokeNode *inv_loop;
					cc.invoke(Out(inv_loop), fn_loop,
							  FuncSignature::build<void, void *, void *,
													int64_t, int32_t>());
					inv_loop->set_arg(0, op_reg);
					inv_loop->set_arg(1, v_arr);
					inv_loop->set_arg(2, v_scalar_val);
					inv_loop->set_arg(3, v_scalar_null);
				}

				cc.bind(lbl_done);
				break;
			}

			/*
			 * ---- IS_JSON (simdjson-accelerated for text input) ----
			 */
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
			case EEOP_IS_JSON:
			{
#ifdef PG_JITTER_HAVE_SIMDJSON
				JsonIsPredicate *pred = op->d.is_json.pred;
				if (exprType(pred->expr) == TEXTOID && !pred->unique_keys)
				{
					/* simdjson fast path */
					a64::Gp resval_addr = cc.new_gpx();
					cc.mov(resval_addr, (uint64_t) op->resvalue);
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					a64::Gp resnull_reg = cc.new_gpx();
					cc.ldrb(resnull_reg.w(), a64::Mem(resnull_addr));
					Label skip_null = cc.new_label();
					cc.cbnz(resnull_reg.w(), skip_null);

					/* Load *op->resvalue (text Datum) */
					a64::Gp datum_reg = cc.new_gpx();
					cc.ldr(datum_reg, a64::Mem(resval_addr));

					/* Call pg_jitter_sj_is_json_datum(datum, item_type) */
					a64::Gp item_type_reg = cc.new_gpx();
					cc.mov(item_type_reg, (int64_t) pred->item_type);
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) pg_jitter_sj_is_json_datum);
					InvokeNode *invoke;
					a64::Gp ret_reg = cc.new_gpx();
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, int64_t, int64_t>());
					invoke->set_arg(0, datum_reg);
					invoke->set_arg(1, item_type_reg);
					invoke->set_ret(0, ret_reg);

					/* Zero-extend int32 return to Datum (int64) —
					 * ARM64 leaves upper 32 bits of x0 undefined for
					 * int32 returns, but Datum comparison needs full 64-bit.
					 * Mask to lower 32 bits since function returns int32. */
					cc.and_(ret_reg, ret_reg, Imm(0xFFFFFFFF));

					/* *op->resvalue = BoolGetDatum(ret) */
					cc.str(ret_reg, a64::Mem(resval_addr));

					Label done = cc.new_label();
					cc.b(done);

					/* Null path: *op->resvalue = false */
					cc.bind(skip_null);
					a64::Gp zero_reg = cc.new_gpx();
					cc.mov(zero_reg, 0);
					cc.str(zero_reg, a64::Mem(resval_addr));

					cc.bind(done);
					break;
				}
#endif /* PG_JITTER_HAVE_SIMDJSON */
				/* Fallback: generic 2-arg call */
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalJsonIsPredicate);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				break;
			}
#endif


#ifdef HAVE_EEOP_IOCOERCE_SAFE
			/*
			 * ---- IOCOERCE_SAFE (inlined, PG17+) ----
			 * Like IOCOERCE but checks SOFT_ERROR_OCCURRED after input fn.
			 * On soft error: *op->resnull = true, *op->resvalue = 0.
			 */
			case EEOP_IOCOERCE_SAFE:
			{
				FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

				/* if (*op->resnull) skip */
				a64::Gp resnull_addr = cc.new_gpx();
				a64::Gp resnull_reg = cc.new_gpx();
				cc.mov(resnull_addr, (uint64_t) op->resnull);
				cc.ldrb(resnull_reg.w(), a64::Mem(resnull_addr));
				Label skip_null = cc.new_label();
				cc.cbnz(resnull_reg.w(), skip_null);

				/* fcinfo_out->args[0].value = *op->resvalue */
				a64::Gp resval_addr = cc.new_gpx();
				cc.mov(resval_addr, (uint64_t) op->resvalue);
				a64::Gp resval_reg = cc.new_gpx();
				cc.ldr(resval_reg, a64::Mem(resval_addr));
				a64::Gp fcinfo_out_reg = cc.new_gpx();
				cc.mov(fcinfo_out_reg, (uint64_t) fcinfo_out);
				int64_t arg0_val_off = (int64_t)((char*)&fcinfo_out->args[0].value - (char*)fcinfo_out);
				int64_t arg0_null_off = (int64_t)((char*)&fcinfo_out->args[0].isnull - (char*)fcinfo_out);
				cc.str(resval_reg, a64::Mem(fcinfo_out_reg, (int32_t) arg0_val_off));
				/* fcinfo_out->args[0].isnull = false */
				a64::Gp zero_reg = cc.new_gpx();
				cc.mov(zero_reg, 0);
				cc.strb(zero_reg.w(), a64::Mem(fcinfo_out_reg, (int32_t) arg0_null_off));
				/* fcinfo_out->isnull = false */
				cc.strb(zero_reg.w(), a64::Mem(fcinfo_out_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

				/* Call output fn */
				a64::Gp fn_out = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_out, (void *) fcinfo_out->flinfo->fn_addr);
				InvokeNode *invoke_out;
				a64::Gp ret_out = cc.new_gpx();
				cc.invoke(Out(invoke_out), fn_out,
						  FuncSignature::build<int64_t, void *>());
				invoke_out->set_arg(0, fcinfo_out_reg);
				invoke_out->set_ret(0, ret_out);

				/* ret_out = cstring Datum; call input function (or simdjson) */
#ifdef PG_JITTER_HAVE_SIMDJSON
				if (fcinfo_in->flinfo->fn_oid == 321 /* F_JSON_IN */ ||
					fcinfo_in->flinfo->fn_oid == 3806 /* F_JSONB_IN */)
				{
					/* simdjson fast path: sj_fn(cstring, fcinfo_in) -> Datum */
					void *sj_fn = (fcinfo_in->flinfo->fn_oid == 321)
								  ? (void *) pg_jitter_sj_json_in
								  : (void *) pg_jitter_sj_jsonb_in;
					a64::Gp fcinfo_in_reg = cc.new_gpx();
					cc.mov(fcinfo_in_reg, (uint64_t) fcinfo_in);
					a64::Gp sj_fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, sj_fn_reg, sj_fn);
					InvokeNode *invoke_sj;
					a64::Gp ret_sj = cc.new_gpx();
					cc.invoke(Out(invoke_sj), sj_fn_reg,
							  FuncSignature::build<int64_t, int64_t, void *>());
					invoke_sj->set_arg(0, ret_out);
					invoke_sj->set_arg(1, fcinfo_in_reg);
					invoke_sj->set_ret(0, ret_sj);

					/* *op->resvalue = ret_sj */
					cc.str(ret_sj, a64::Mem(resval_addr));
					/* *op->resnull = false (simdjson ereport's on error) */
					cc.strb(zero_reg.w(), a64::Mem(resnull_addr));
					/* simdjson never uses soft errors, skip the check */
				}
				else
#endif /* PG_JITTER_HAVE_SIMDJSON */
				{
					/* Standard input function call */
					a64::Gp fcinfo_in_reg = cc.new_gpx();
					cc.mov(fcinfo_in_reg, (uint64_t) fcinfo_in);
					int64_t in_arg0_val_off = (int64_t)((char*)&fcinfo_in->args[0].value - (char*)fcinfo_in);
					int64_t in_arg0_null_off = (int64_t)((char*)&fcinfo_in->args[0].isnull - (char*)fcinfo_in);
					cc.str(ret_out, a64::Mem(fcinfo_in_reg, (int32_t) in_arg0_val_off));
					cc.strb(zero_reg.w(), a64::Mem(fcinfo_in_reg, (int32_t) in_arg0_null_off));
					cc.strb(zero_reg.w(), a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

					a64::Gp fn_in = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_in, (void *) fcinfo_in->flinfo->fn_addr);
					InvokeNode *invoke_in;
					a64::Gp ret_in = cc.new_gpx();
					cc.invoke(Out(invoke_in), fn_in,
							  FuncSignature::build<int64_t, void *>());
					invoke_in->set_arg(0, fcinfo_in_reg);
					invoke_in->set_ret(0, ret_in);

					/* Check SOFT_ERROR_OCCURRED(fcinfo_in->context) */
					a64::Gp ctx_reg = cc.new_gpx();
					cc.ldr(ctx_reg, a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, context)));
					a64::Gp err_reg = cc.new_gpx();
					cc.ldrb(err_reg.w(), a64::Mem(ctx_reg, (int32_t) offsetof(ErrorSaveContext, error_occurred)));
					Label no_soft_error = cc.new_label();
					cc.cbz(err_reg.w(), no_soft_error);

					/* Soft error path: *op->resnull = true, *op->resvalue = 0 */
					{
						a64::Gp one_reg = cc.new_gpx();
						cc.mov(one_reg, 1);
						cc.strb(one_reg.w(), a64::Mem(resnull_addr));
						cc.str(zero_reg, a64::Mem(resval_addr));
					}
					Label iocoerce_safe_done = cc.new_label();
					cc.b(iocoerce_safe_done);

					/* No error: store result normally */
					cc.bind(no_soft_error);
					cc.str(ret_in, a64::Mem(resval_addr));
					/* *op->resnull = fcinfo_in->isnull */
					a64::Gp isnull_in = cc.new_gpx();
					cc.ldrb(isnull_in.w(), a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));
					cc.strb(isnull_in.w(), a64::Mem(resnull_addr));

					cc.bind(iocoerce_safe_done);
				}

				cc.bind(skip_null);
				break;
			}
#endif /* HAVE_EEOP_IOCOERCE_SAFE */

			/*
			 * ---- SQLVALUEFUNCTION (inlined) ----
			 * Resolve svf->op at compile time, emit direct call to getter.
			 * Time functions: direct call with typmod immediate.
			 * Identity functions: fall back to ExecEvalSQLValueFunction.
			 */
			case EEOP_SQLVALUEFUNCTION:
			{
				SQLValueFunction *svf = op->d.sqlvaluefunction.svf;

				switch (svf->op)
				{
					case SVFOP_CURRENT_DATE:
					{
						/* GetSQLCurrentDate() -> int32 (DateADT) */
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) GetSQLCurrentDate);
						InvokeNode *invoke;
						a64::Gp ret = cc.new_gpx();
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int32_t>());
						invoke->set_ret(0, ret);
						/* Sign-extend int32 to Datum (64-bit) — DateADT is signed */
						cc.sxtw(ret, ret);
						/* *op->resvalue = ret */
						if (op->resvalue == &state->resvalue)
							cc.str(ret, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							a64::Gp rv_addr = cc.new_gpx();
							cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(ret, a64::ptr(rv_addr));
						}
						/* *op->resnull = false */
						a64::Gp zero_r = cc.new_gpx();
						cc.mov(zero_r, 0);
						if (op->resnull == &state->resnull)
							cc.strb(zero_r.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							a64::Gp rn_addr = cc.new_gpx();
							cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(zero_r.w(), a64::ptr(rn_addr));
						}
						break;
					}

					case SVFOP_CURRENT_TIME:
					case SVFOP_CURRENT_TIME_N:
					{
						/* GetSQLCurrentTime(int32 typmod) -> pointer (TimeTzADT*) */
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) GetSQLCurrentTime);
						a64::Gp typmod_reg = cc.new_gpx();
						cc.mov(typmod_reg, (uint64_t)(uint32_t) svf->typmod);
						InvokeNode *invoke;
						a64::Gp ret = cc.new_gpx();
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int32_t>());
						invoke->set_arg(0, typmod_reg);
						invoke->set_ret(0, ret);
						/* *op->resvalue = ret (pointer Datum) */
						if (op->resvalue == &state->resvalue)
							cc.str(ret, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							a64::Gp rv_addr = cc.new_gpx();
							cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(ret, a64::ptr(rv_addr));
						}
						/* *op->resnull = false */
						a64::Gp zero_r = cc.new_gpx();
						cc.mov(zero_r, 0);
						if (op->resnull == &state->resnull)
							cc.strb(zero_r.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							a64::Gp rn_addr = cc.new_gpx();
							cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(zero_r.w(), a64::ptr(rn_addr));
						}
						break;
					}

					case SVFOP_CURRENT_TIMESTAMP:
					case SVFOP_CURRENT_TIMESTAMP_N:
					{
						/* GetSQLCurrentTimestamp(int32 typmod) -> int64 */
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) GetSQLCurrentTimestamp);
						a64::Gp typmod_reg = cc.new_gpx();
						cc.mov(typmod_reg, (uint64_t)(uint32_t) svf->typmod);
						InvokeNode *invoke;
						a64::Gp ret = cc.new_gpx();
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int32_t>());
						invoke->set_arg(0, typmod_reg);
						invoke->set_ret(0, ret);
						if (op->resvalue == &state->resvalue)
							cc.str(ret, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							a64::Gp rv_addr = cc.new_gpx();
							cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(ret, a64::ptr(rv_addr));
						}
						a64::Gp zero_r = cc.new_gpx();
						cc.mov(zero_r, 0);
						if (op->resnull == &state->resnull)
							cc.strb(zero_r.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							a64::Gp rn_addr = cc.new_gpx();
							cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(zero_r.w(), a64::ptr(rn_addr));
						}
						break;
					}

					case SVFOP_LOCALTIME:
					case SVFOP_LOCALTIME_N:
					{
						/* GetSQLLocalTime(int32 typmod) -> int64 */
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) GetSQLLocalTime);
						a64::Gp typmod_reg = cc.new_gpx();
						cc.mov(typmod_reg, (uint64_t)(uint32_t) svf->typmod);
						InvokeNode *invoke;
						a64::Gp ret = cc.new_gpx();
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int32_t>());
						invoke->set_arg(0, typmod_reg);
						invoke->set_ret(0, ret);
						if (op->resvalue == &state->resvalue)
							cc.str(ret, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							a64::Gp rv_addr = cc.new_gpx();
							cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(ret, a64::ptr(rv_addr));
						}
						a64::Gp zero_r = cc.new_gpx();
						cc.mov(zero_r, 0);
						if (op->resnull == &state->resnull)
							cc.strb(zero_r.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							a64::Gp rn_addr = cc.new_gpx();
							cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(zero_r.w(), a64::ptr(rn_addr));
						}
						break;
					}

					case SVFOP_LOCALTIMESTAMP:
					case SVFOP_LOCALTIMESTAMP_N:
					{
						/* GetSQLLocalTimestamp(int32 typmod) -> int64 */
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) GetSQLLocalTimestamp);
						a64::Gp typmod_reg = cc.new_gpx();
						cc.mov(typmod_reg, (uint64_t)(uint32_t) svf->typmod);
						InvokeNode *invoke;
						a64::Gp ret = cc.new_gpx();
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<int64_t, int32_t>());
						invoke->set_arg(0, typmod_reg);
						invoke->set_ret(0, ret);
						if (op->resvalue == &state->resvalue)
							cc.str(ret, a64::ptr(v_state, offsetof(ExprState, resvalue)));
						else
						{
							a64::Gp rv_addr = cc.new_gpx();
							cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
							cc.str(ret, a64::ptr(rv_addr));
						}
						a64::Gp zero_r = cc.new_gpx();
						cc.mov(zero_r, 0);
						if (op->resnull == &state->resnull)
							cc.strb(zero_r.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
						else
						{
							a64::Gp rn_addr = cc.new_gpx();
							cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
							cc.strb(zero_r.w(), a64::ptr(rn_addr));
						}
						break;
					}

					default:
					{
						/* Identity functions (CURRENT_USER etc): fall back to C */
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalSQLValueFunction);
						a64::Gp op_reg = cc.new_gpx();
						cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
						InvokeNode *invoke;
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<void, void *, void *>());
						invoke->set_arg(0, v_state);
						invoke->set_arg(1, op_reg);
						break;
					}
				}
				break;
			}

			/*
			 * ---- ARRAYEXPR (inlined for fixed-width by-value) ----
			 * For simple 1-D arrays of fixed-width by-value elements with no
			 * nulls, we inline: palloc(known_size) + header init + element copy.
			 * All other cases fall back to ExecEvalArrayExpr.
			 */
			case EEOP_ARRAYEXPR:
			{
				int nelems = op->d.arrayexpr.nelems;
				int16 elemlength = op->d.arrayexpr.elemlength;
				bool elembyval = op->d.arrayexpr.elembyval;
				bool multidims = op->d.arrayexpr.multidims;

				if (!multidims && elemlength > 0 && elembyval &&
					nelems > 0 && nelems <= 64)
				{
					/*
					 * Fast path: fixed-width by-value, 1-D, small array.
					 * Pre-compute total size at compile time.
					 */
					int dataoff = ARR_OVERHEAD_NONULLS(1);
					int data_size = nelems * elemlength;
					int total_size = dataoff + data_size;

					/* Check elemnulls[] for any nulls; jump to fallback if found */
					a64::Gp elemnulls_reg = cc.new_gpx();
					cc.ldr(elemnulls_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.arrayexpr.elemnulls)));
					Label arr_fallback = cc.new_label();
					for (int k = 0; k < nelems; k++)
					{
						a64::Gp null_byte = cc.new_gpx();
						cc.ldrb(null_byte.w(), a64::Mem(elemnulls_reg, k * (int)sizeof(bool)));
						cc.cbnz(null_byte.w(), arr_fallback);
					}

					/* No nulls: palloc(total_size) */
					a64::Gp palloc_fn = cc.new_gpx();
					ASMJIT_MOV_FN(cc, palloc_fn, (void *) palloc);
					a64::Gp size_reg = cc.new_gpx();
					cc.mov(size_reg, (uint64_t) total_size);
					InvokeNode *invoke_palloc;
					a64::Gp arr_reg = cc.new_gpx();
					cc.invoke(Out(invoke_palloc), palloc_fn,
							  FuncSignature::build<int64_t, int64_t>());
					invoke_palloc->set_arg(0, size_reg);
					invoke_palloc->set_ret(0, arr_reg);

					/* Initialize ArrayType header */
					/* SET_VARSIZE: store (total_size << 2) in first 4 bytes */
					a64::Gp hdr_val = cc.new_gpx();
					cc.mov(hdr_val, (uint64_t)(total_size << 2));
					cc.str(hdr_val.w(), a64::Mem(arr_reg, 0));
					/* ndim = 1 */
					a64::Gp one_reg = cc.new_gpx();
					cc.mov(one_reg, 1);
					cc.str(one_reg.w(), a64::Mem(arr_reg, (int32_t) offsetof(ArrayType, ndim)));
					/* dataoffset = 0 (no null bitmap) */
					a64::Gp zero_reg = cc.new_gpx();
					cc.mov(zero_reg, 0);
					cc.str(zero_reg.w(), a64::Mem(arr_reg, (int32_t) offsetof(ArrayType, dataoffset)));
					/* elemtype */
					a64::Gp etype_reg = cc.new_gpx();
					cc.mov(etype_reg, (uint64_t) op->d.arrayexpr.elemtype);
					cc.str(etype_reg.w(), a64::Mem(arr_reg, (int32_t) offsetof(ArrayType, elemtype)));
					/* dims[0] = nelems (at offset sizeof(ArrayType)) */
					a64::Gp nelems_reg = cc.new_gpx();
					cc.mov(nelems_reg, (uint64_t) nelems);
					cc.str(nelems_reg.w(), a64::Mem(arr_reg, (int32_t) sizeof(ArrayType)));
					/* lbound[0] = 1 (at offset sizeof(ArrayType) + sizeof(int)) */
					cc.str(one_reg.w(), a64::Mem(arr_reg, (int32_t)(sizeof(ArrayType) + sizeof(int))));

					/* Copy elements from elemvalues[] into data area */
					a64::Gp elemvals_reg = cc.new_gpx();
					cc.ldr(elemvals_reg, a64::ptr(v_steps, STEP_OFFSET(opno, d.arrayexpr.elemvalues)));
					for (int k = 0; k < nelems; k++)
					{
						a64::Gp elem = cc.new_gpx();
						cc.ldr(elem, a64::Mem(elemvals_reg, k * (int)sizeof(Datum)));
						if (elemlength == 8)
							cc.str(elem, a64::Mem(arr_reg, (int32_t)(dataoff + k * elemlength)));
						else if (elemlength == 4)
							cc.str(elem.w(), a64::Mem(arr_reg, (int32_t)(dataoff + k * elemlength)));
						else if (elemlength == 2)
							cc.strh(elem.w(), a64::Mem(arr_reg, (int32_t)(dataoff + k * elemlength)));
						else /* elemlength == 1 */
							cc.strb(elem.w(), a64::Mem(arr_reg, (int32_t)(dataoff + k * elemlength)));
					}

					/* *op->resvalue = PointerGetDatum(arr_reg) */
					if (op->resvalue == &state->resvalue)
						cc.str(arr_reg, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						a64::Gp rv_addr = cc.new_gpx();
						cc.ldr(rv_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(arr_reg, a64::ptr(rv_addr));
					}
					/* *op->resnull = false */
					if (op->resnull == &state->resnull)
						cc.strb(zero_reg.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						a64::Gp rn_addr = cc.new_gpx();
						cc.ldr(rn_addr, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(zero_reg.w(), a64::ptr(rn_addr));
					}

					Label arr_done = cc.new_label();
					cc.b(arr_done);

					/* Fallback: has nulls or complex case -> call C function */
					cc.bind(arr_fallback);
					{
						a64::Gp fn_reg = cc.new_gpx();
						ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalArrayExpr);
						a64::Gp op_reg = cc.new_gpx();
						cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
						InvokeNode *invoke;
						cc.invoke(Out(invoke), fn_reg,
								  FuncSignature::build<void, void *, void *>());
						invoke->set_arg(0, v_state);
						invoke->set_arg(1, op_reg);
					}

					cc.bind(arr_done);
				}
				else
				{
					/* Fallback to C function for varlena/multidims/complex cases */
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalArrayExpr);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
				}
				break;
			}

			/* 2-arg: fn(ExprState *state, ExprEvalStep *op) */
			case EEOP_CURRENTOFEXPR:
			case EEOP_XMLEXPR:
			{
				void *fn;

				switch (opcode)
				{
					case EEOP_CURRENTOFEXPR:
						fn = (void *) ExecEvalCurrentOfExpr; break;
					case EEOP_XMLEXPR:
						fn = (void *) ExecEvalXmlExpr; break;
					default:
						pg_unreachable();
				}

				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, fn);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				break;
			}

			/* PARAM_CALLBACK: indirect call through op->d.cparam.paramfunc */
			case EEOP_PARAM_CALLBACK:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.cparam.paramfunc);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/* SYSVAR (partially inlined): tableoid and ctid inline, rest C call */
			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_SYSVAR:
			case EEOP_NEW_SYSVAR:
#endif
			{
				int64_t slot_offset;
				int attnum = op->d.var.attnum;
				switch (opcode)
				{
					case EEOP_INNER_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_innertuple); break;
					case EEOP_OUTER_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_outertuple); break;
					case EEOP_SCAN_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_scantuple); break;
#ifdef HAVE_EEOP_OLD_NEW
					case EEOP_OLD_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_oldtuple); break;
					case EEOP_NEW_SYSVAR:
						slot_offset = offsetof(ExprContext, ecxt_newtuple); break;
#endif
					default:
						pg_unreachable();
				}

				bool can_inline = (attnum == TableOidAttributeNumber ||
								   attnum == SelfItemPointerAttributeNumber);
#ifdef HAVE_EEOP_OLD_NEW
				if (opcode == EEOP_OLD_SYSVAR || opcode == EEOP_NEW_SYSVAR)
					can_inline = false;
#endif
				if (!can_inline) {
					/* Fall back to C call */
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalSysVar);
					a64::Gp op_reg = cc.new_gpx();
					cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
					a64::Gp slot_reg2 = cc.new_gpx();
					cc.ldr(slot_reg2, a64::Mem(v_econtext, (int32_t) slot_offset));
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
					invoke->set_arg(3, slot_reg2);
				} else {
					/* Inline: slot = econtext->ecxt_*tuple */
					cc.ldr(slot_reg, a64::Mem(v_econtext, (int32_t) slot_offset));

					if (attnum == TableOidAttributeNumber) {
						/* *resvalue = (Datum)slot->tts_tableOid */
						cc.ldr(tmp1.w(), a64::Mem(slot_reg, (int32_t)offsetof(TupleTableSlot, tts_tableOid)));
						/* zero-extend w register to x is implicit in AsmJIT */
					} else {
						/* ctid: *resvalue = PointerGetDatum(&slot->tts_tid) */
						cc.add(tmp1, slot_reg, (int64_t)offsetof(TupleTableSlot, tts_tid));
					}
					if (op->resvalue == &state->resvalue)
						cc.str(tmp1, a64::ptr(v_state, offsetof(ExprState, resvalue)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
						cc.str(tmp1, a64::ptr(tmp2));
					}
					/* *resnull = false */
					cc.mov(tmp1, 0);
					if (op->resnull == &state->resnull)
						cc.strb(tmp1.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
					else
					{
						cc.ldr(tmp2, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
						cc.strb(tmp1.w(), a64::ptr(tmp2));
					}
				}
				break;
			}

			/* SBSREF_OLD/ASSIGN/FETCH: indirect 3-arg void call */
			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.sbsref.subscriptfunc);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/* SBSREF_SUBSCRIPTS: indirect 3-arg call returning bool + conditional jump */
			case EEOP_SBSREF_SUBSCRIPTS:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.sbsref_subscript.subscriptfunc);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				a64::Gp ret_reg = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_ret(0, ret_reg);
				/* If false (0), jump to jumpdone */
				int jumpdone = op->d.sbsref_subscript.jumpdone;
				cc.cbz(ret_reg, step_labels[jumpdone]);
				break;
			}

#ifdef HAVE_EEOP_JSONEXPR
			/* JSONEXPR_PATH: 3-arg call returning int + multi-target jumps */
			case EEOP_JSONEXPR_PATH:
			{
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalJsonExprPath);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				a64::Gp ret_reg = cc.new_gpx();
				/*
				 * ExecEvalJsonExprPath returns int (32-bit).  Use int32_t
				 * in signature so AsmJIT properly sign-extends the return
				 * value on ARM64 (upper 32 bits of x0 are undefined).
				 */
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int32_t, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_ret(0, ret_reg);

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
				break;
			}
#endif

			/* IOCOERCE: output fn → input fn pipeline */
			case EEOP_IOCOERCE:
			{
				FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

				/* if (*op->resnull) skip */
				a64::Gp resnull_addr = cc.new_gpx();
				a64::Gp resnull_reg = cc.new_gpx();
				cc.mov(resnull_addr, (uint64_t) op->resnull);
				cc.ldrb(resnull_reg.w(), a64::Mem(resnull_addr));
				Label skip_null = cc.new_label();
				cc.cbnz(resnull_reg.w(), skip_null);

				/* fcinfo_out->args[0].value = *op->resvalue */
				a64::Gp resval_addr = cc.new_gpx();
				cc.mov(resval_addr, (uint64_t) op->resvalue);
				a64::Gp resval_reg = cc.new_gpx();
				cc.ldr(resval_reg, a64::Mem(resval_addr));
				a64::Gp fcinfo_out_reg = cc.new_gpx();
				cc.mov(fcinfo_out_reg, (uint64_t) fcinfo_out);
				int64_t arg0_val_off = (int64_t)((char*)&fcinfo_out->args[0].value - (char*)fcinfo_out);
				int64_t arg0_null_off = (int64_t)((char*)&fcinfo_out->args[0].isnull - (char*)fcinfo_out);
				cc.str(resval_reg, a64::Mem(fcinfo_out_reg, (int32_t) arg0_val_off));
				/* fcinfo_out->args[0].isnull = false */
				a64::Gp zero_reg = cc.new_gpx();
				cc.mov(zero_reg, 0);
				cc.strb(zero_reg.w(), a64::Mem(fcinfo_out_reg, (int32_t) arg0_null_off));
				/* fcinfo_out->isnull = false */
				cc.strb(zero_reg.w(), a64::Mem(fcinfo_out_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

				/* Call output fn */
				a64::Gp fn_out = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_out, (void *) fcinfo_out->flinfo->fn_addr);
				InvokeNode *invoke_out;
				a64::Gp ret_out = cc.new_gpx();
				cc.invoke(Out(invoke_out), fn_out,
						  FuncSignature::build<int64_t, void *>());
				invoke_out->set_arg(0, fcinfo_out_reg);
				invoke_out->set_ret(0, ret_out);

				/* ret_out = cstring Datum; call input function (or simdjson) */
#ifdef PG_JITTER_HAVE_SIMDJSON
				if (fcinfo_in->flinfo->fn_oid == 321 /* F_JSON_IN */ ||
					fcinfo_in->flinfo->fn_oid == 3806 /* F_JSONB_IN */)
				{
					/* simdjson fast path: sj_fn(cstring, fcinfo_in) → Datum */
					void *sj_fn = (fcinfo_in->flinfo->fn_oid == 321)
								  ? (void *) pg_jitter_sj_json_in
								  : (void *) pg_jitter_sj_jsonb_in;
					a64::Gp fcinfo_in_reg = cc.new_gpx();
					cc.mov(fcinfo_in_reg, (uint64_t) fcinfo_in);
					a64::Gp sj_fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, sj_fn_reg, sj_fn);
					InvokeNode *invoke_sj;
					a64::Gp ret_sj = cc.new_gpx();
					cc.invoke(Out(invoke_sj), sj_fn_reg,
							  FuncSignature::build<int64_t, int64_t, void *>());
					invoke_sj->set_arg(0, ret_out);
					invoke_sj->set_arg(1, fcinfo_in_reg);
					invoke_sj->set_ret(0, ret_sj);

					/* *op->resvalue = ret_sj */
					cc.str(ret_sj, a64::Mem(resval_addr));
					/* *op->resnull = false */
					cc.strb(zero_reg.w(), a64::Mem(resnull_addr));
				}
				else
#endif /* PG_JITTER_HAVE_SIMDJSON */
				{
					/* Standard input function call */
					a64::Gp fcinfo_in_reg = cc.new_gpx();
					cc.mov(fcinfo_in_reg, (uint64_t) fcinfo_in);
					int64_t in_arg0_val_off = (int64_t)((char*)&fcinfo_in->args[0].value - (char*)fcinfo_in);
					int64_t in_arg0_null_off = (int64_t)((char*)&fcinfo_in->args[0].isnull - (char*)fcinfo_in);
					cc.str(ret_out, a64::Mem(fcinfo_in_reg, (int32_t) in_arg0_val_off));
					cc.strb(zero_reg.w(), a64::Mem(fcinfo_in_reg, (int32_t) in_arg0_null_off));
					cc.strb(zero_reg.w(), a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

					a64::Gp fn_in = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_in, (void *) fcinfo_in->flinfo->fn_addr);
					InvokeNode *invoke_in;
					a64::Gp ret_in = cc.new_gpx();
					cc.invoke(Out(invoke_in), fn_in,
							  FuncSignature::build<int64_t, void *>());
					invoke_in->set_arg(0, fcinfo_in_reg);
					invoke_in->set_ret(0, ret_in);

					/* *op->resvalue = ret_in */
					cc.str(ret_in, a64::Mem(resval_addr));
					/* *op->resnull = fcinfo_in->isnull */
					a64::Gp isnull_in = cc.new_gpx();
					cc.ldrb(isnull_in.w(), a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));
					cc.strb(isnull_in.w(), a64::Mem(resnull_addr));
				}

				cc.bind(skip_null);
				break;
			}

			/* ROWCOMPARE_STEP: call fn, check null, check nonzero */
			case EEOP_ROWCOMPARE_STEP:
			{
				FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
				int jnull = op->d.rowcompare_step.jumpnull;
				int jdone = op->d.rowcompare_step.jumpdone;

				if (op->d.rowcompare_step.finfo->fn_strict)
				{
					a64::Gp a0null = cc.new_gpx();
					a64::Gp a1null = cc.new_gpx();
					a64::Gp fcinfo_base = cc.new_gpx();
					cc.mov(fcinfo_base, (uint64_t) fcinfo);
					int64_t a0null_off = (int64_t)((char*)&fcinfo->args[0].isnull - (char*)fcinfo);
					int64_t a1null_off = (int64_t)((char*)&fcinfo->args[1].isnull - (char*)fcinfo);
					cc.ldrb(a0null.w(), a64::Mem(fcinfo_base, (int32_t) a0null_off));
					cc.ldrb(a1null.w(), a64::Mem(fcinfo_base, (int32_t) a1null_off));
					cc.orr(a0null, a0null, a1null);

					Label not_null = cc.new_label();
					cc.cbz(a0null, not_null);

					/* Null path */
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					a64::Gp one_reg = cc.new_gpx();
					cc.mov(one_reg, 1);
					cc.strb(one_reg.w(), a64::Mem(resnull_addr));
					cc.b(step_labels[jnull]);

					cc.bind(not_null);
				}

				/* fcinfo->isnull = false */
				a64::Gp fcinfo_reg = cc.new_gpx();
				cc.mov(fcinfo_reg, (uint64_t) fcinfo);
				a64::Gp zero_reg = cc.new_gpx();
				cc.mov(zero_reg, 0);
				cc.strb(zero_reg.w(), a64::Mem(fcinfo_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

				/* Call fn_addr(fcinfo) */
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.rowcompare_step.fn_addr);
				InvokeNode *invoke;
				a64::Gp ret_reg = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *>());
				invoke->set_arg(0, fcinfo_reg);
				invoke->set_ret(0, ret_reg);

				/* *op->resvalue = ret_reg */
				a64::Gp resval_addr = cc.new_gpx();
				cc.mov(resval_addr, (uint64_t) op->resvalue);
				cc.str(ret_reg, a64::Mem(resval_addr));

				/* If fcinfo->isnull -> resnull=true, jump to jnull */
				a64::Gp isnull_reg = cc.new_gpx();
				cc.ldrb(isnull_reg.w(), a64::Mem(fcinfo_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));
				Label not_null2 = cc.new_label();
				cc.cbz(isnull_reg, not_null2);
				{
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					a64::Gp one = cc.new_gpx();
					cc.mov(one, 1);
					cc.strb(one.w(), a64::Mem(resnull_addr));
					cc.b(step_labels[jnull]);
				}
				cc.bind(not_null2);

				/* *resnull = false */
				{
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					cc.strb(zero_reg.w(), a64::Mem(resnull_addr));
				}

				/* If int32(ret_reg) != 0, jump to jumpdone */
				a64::Gp ret32 = cc.new_gp32();
				cc.mov(ret32, ret_reg.r32());
				cc.cmp(ret32, 0);
				cc.b(a64::CondCode::kNE, step_labels[jdone]);
				break;
			}

			/* ROWCOMPARE_FINAL: compare result against zero using cmptype */
			case EEOP_ROWCOMPARE_FINAL:
			{
				CompareType cmptype = op->d.rowcompare_final.cmptype;

				a64::Gp resval_addr = cc.new_gpx();
				cc.mov(resval_addr, (uint64_t) op->resvalue);
				a64::Gp cmpres = cc.new_gp32();
				cc.ldr(cmpres, a64::Mem(resval_addr));

				cc.cmp(cmpres, 0);

				a64::Gp result = cc.new_gpx();
				a64::CondCode cond;
				switch (cmptype)
				{
					case COMPARE_LT: cond = a64::CondCode::kLT; break;
					case COMPARE_LE: cond = a64::CondCode::kLE; break;
					case COMPARE_GE: cond = a64::CondCode::kGE; break;
					case COMPARE_GT: cond = a64::CondCode::kGT; break;
					default: cond = a64::CondCode::kLT; break;
				}
				cc.cset(result, cond);

				cc.str(result, a64::Mem(resval_addr));
				a64::Gp resnull_addr = cc.new_gpx();
				cc.mov(resnull_addr, (uint64_t) op->resnull);
				a64::Gp zero_reg = cc.new_gpx();
				cc.mov(zero_reg, 0);
				cc.strb(zero_reg.w(), a64::Mem(resnull_addr));
				break;
			}

#ifdef HAVE_EEOP_RETURNINGEXPR
			/* RETURNINGEXPR: check flags, conditionally null result + jump */
			case EEOP_RETURNINGEXPR:
			{
				/* Load state->flags */
				a64::Gp flags_reg = cc.new_gp32();
				cc.ldr(flags_reg, a64::Mem(v_state, (int32_t) offsetof(ExprState, flags)));

				/* Test nullflag */
				cc.tst(flags_reg, (int32_t) op->d.returningexpr.nullflag);
				Label cont = cc.new_label();
				cc.b(a64::CondCode::kEQ, cont);

				/* Flag set: *resvalue = 0, *resnull = true, jump */
				a64::Gp resval_addr = cc.new_gpx();
				cc.mov(resval_addr, (uint64_t) op->resvalue);
				a64::Gp zero_reg = cc.new_gpx();
				cc.mov(zero_reg, 0);
				cc.str(zero_reg, a64::Mem(resval_addr));
				a64::Gp resnull_addr = cc.new_gpx();
				cc.mov(resnull_addr, (uint64_t) op->resnull);
				a64::Gp one_reg = cc.new_gpx();
				cc.mov(one_reg, 1);
				cc.strb(one_reg.w(), a64::Mem(resnull_addr));
				cc.b(step_labels[op->d.returningexpr.jumpdone]);

				cc.bind(cont);
				break;
			}
#endif

			default:
			{
				/* Fallback: call pg_jitter_fallback_step -> int (steps-relative) */
				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) pg_jitter_fallback_step);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				InvokeNode *invoke;
				a64::Gp ret_reg = cc.new_gpx();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_ret(0, ret_reg);

				/*
				 * Generic jump dispatch: fallback_step returns >= 0 when a
				 * jump is needed (the return value is the target step index).
				 * Build a jump table for all reachable labels.
				 */
				{
					Label no_jump = cc.new_label();

					cc.cmp(ret_reg, 0);
					cc.b(a64::CondCode::kLT, no_jump);

					/*
					 * Emit a chain of comparisons for each possible jump
					 * target step.  For most expressions this is 1-3 targets.
					 */
					for (int t = 0; t < steps_len; t++)
					{
						cc.cmp(ret_reg, t);
						cc.b(a64::CondCode::kEQ, step_labels[t]);
					}
					/* Should not reach here, but fall through to no_jump */
					cc.bind(no_jump);
				}
				break;
			}
		}
	}

	cc.end_func();

#undef STEP_OFFSET

	Error err = cc.finalize();
	if (err != kErrorOk)
	{
		pfree(step_labels);
		return false;
	}

	pfree(step_labels);
	return true;
}
