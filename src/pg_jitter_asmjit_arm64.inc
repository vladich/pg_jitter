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
					 * Call deform dispatch: JIT-compiles deform via sljit
					 * (loop-based for wide, unrolled for narrow) and caches.
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
				int attnum = op->d.var.attnum;
				int64_t soff = slot_offset_for_opcode(opcode);

				cc.ldr(slot_reg, a64::ptr(v_econtext, soff));

				/* values_ptr = slot->tts_values; value = values_ptr[attnum] */
				cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				cc.ldr(tmp2, a64::ptr(tmp1, attnum * (int64_t) sizeof(Datum)));
				/* *op->resvalue = value (steps-relative) */
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp3));
				}

				/* isnull_ptr = slot->tts_isnull; isnull = isnull_ptr[attnum] */
				cc.ldr(tmp1, a64::ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1, attnum * (int64_t) sizeof(bool)));
				/* *op->resnull = isnull (steps-relative) */
				if (op->resnull == &state->resnull)
					cc.strb(tmp2.w(), a64::ptr(v_state, offsetof(ExprState, resnull)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resnull)));
					cc.strb(tmp2.w(), a64::ptr(tmp3));
				}
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
				cc.mov(tmp1, (int64_t) op->d.constval.value);
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

				if (dfn && dfn->inline_op != JIT_INLINE_NONE)
				{
					/*
					 * TIER 0 — INLINE: emit the operation as native ARM64
					 * instructions, no function call at all.
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

					/* Direct call */
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, call_target);

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
				/* Fallback: generic fcinfo path (steps-relative) */
				{
					a64::Gp fci = cc.new_gpx();
					cc.ldr(fci, a64::ptr(v_steps, STEP_OFFSET(opno, d.func.fcinfo_data)));
					cc.mov(tmp2, 0);
					cc.strb(tmp2.w(), a64::ptr(fci, offsetof(FunctionCallInfoBaseData, isnull)));
				}
				{
					a64::Gp fn_reg = cc.new_gpx();
					ASMJIT_MOV_FN(cc, fn_reg, (void *) op->d.func.fn_addr);
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
				/* *op->resvalue = *op->d.casetest.value (steps-relative) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.casetest.value)));
				cc.ldr(tmp2, a64::ptr(tmp1));
				if (op->resvalue == &state->resvalue)
					cc.str(tmp2, a64::ptr(v_state, offsetof(ExprState, resvalue)));
				else
				{
					cc.ldr(tmp3, a64::ptr(v_steps, STEP_OFFSET(opno, resvalue)));
					cc.str(tmp2, a64::ptr(tmp3));
				}

				/* *op->resnull = *op->d.casetest.isnull (steps-relative) */
				cc.ldr(tmp1, a64::ptr(v_steps,
					STEP_OFFSET(opno, d.casetest.isnull)));
				cc.ldrb(tmp2.w(), a64::ptr(tmp1));
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
			 * ---- DIRECT-CALL OPCODES ----
			 * These opcodes have PG-exported functions with matching signatures.
			 * Instead of going through fallback_step's switch, we call the
			 * function directly.
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
			case EEOP_PARAM_EXEC:
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
					case EEOP_FIELDSELECT:
						fn = (void *) ExecEvalFieldSelect; break;
					case EEOP_FIELDSTORE_DEFORM:
						fn = (void *) ExecEvalFieldStoreDeForm; break;
					case EEOP_FIELDSTORE_FORM:
						fn = (void *) ExecEvalFieldStoreForm; break;
					case EEOP_CONVERT_ROWTYPE:
						fn = (void *) ExecEvalConvertRowtype; break;
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
					case EEOP_JSON_CONSTRUCTOR:
						fn = (void *) ExecEvalJsonConstructor; break;
#endif
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
					case EEOP_WHOLEROW:
						fn = (void *) ExecEvalWholeRowVar; break;
					case EEOP_AGG_ORDERED_TRANS_DATUM:
						fn = (void *) ExecEvalAggOrderedTransDatum; break;
					case EEOP_AGG_ORDERED_TRANS_TUPLE:
						fn = (void *) ExecEvalAggOrderedTransTuple; break;
					case EEOP_PARAM_EXEC:
						fn = (void *) ExecEvalParamExec; break;
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
						fn = (void *) ExecEvalCoerceViaIOSafe; break;
#endif
					case EEOP_SCALARARRAYOP:
						fn = (void *) ExecEvalScalarArrayOp; break;
					case EEOP_SQLVALUEFUNCTION:
						fn = (void *) ExecEvalSQLValueFunction; break;
					case EEOP_CURRENTOFEXPR:
						fn = (void *) ExecEvalCurrentOfExpr; break;
					case EEOP_NEXTVALUEEXPR:
						fn = (void *) ExecEvalNextValueExpr; break;
					case EEOP_ARRAYEXPR:
						fn = (void *) ExecEvalArrayExpr; break;
					case EEOP_ROW:
						fn = (void *) ExecEvalRow; break;
					case EEOP_MINMAX:
						fn = (void *) ExecEvalMinMax; break;
					case EEOP_DOMAIN_NOTNULL:
						fn = (void *) ExecEvalConstraintNotNull; break;
					case EEOP_DOMAIN_CHECK:
						fn = (void *) ExecEvalConstraintCheck; break;
					case EEOP_XMLEXPR:
						fn = (void *) ExecEvalXmlExpr; break;
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
					case EEOP_IS_JSON:
						fn = (void *) ExecEvalJsonIsPredicate; break;
#endif
#ifdef HAVE_EEOP_JSONEXPR
					case EEOP_JSONEXPR_COERCION_FINISH:
						fn = (void *) ExecEvalJsonCoercionFinish; break;
#endif
					case EEOP_GROUPING_FUNC:
						fn = (void *) ExecEvalGroupingFunc; break;
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

			/* SYSVAR: 4-arg call to ExecEvalSysVar(state, op, econtext, slot) */
			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_SYSVAR:
			case EEOP_NEW_SYSVAR:
#endif
			{
				int64_t slot_offset;
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

				a64::Gp fn_reg = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_reg, (void *) ExecEvalSysVar);
				a64::Gp op_reg = cc.new_gpx();
				cc.add(op_reg, v_steps, (int64_t)(opno * (int64_t)sizeof(ExprEvalStep)));
				a64::Gp slot_reg = cc.new_gpx();
				cc.ldr(slot_reg, a64::Mem(v_econtext, (int32_t) slot_offset));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_arg(3, slot_reg);
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
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *, void *>());
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
				cc.ldrb(resnull_reg, a64::Mem(resnull_addr));
				Label skip_null = cc.newLabel();
				cc.cbnz(resnull_reg, skip_null);

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
				cc.strb(zero_reg, a64::Mem(fcinfo_out_reg, (int32_t) arg0_null_off));
				/* fcinfo_out->isnull = false */
				cc.strb(zero_reg, a64::Mem(fcinfo_out_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

				/* Call output fn */
				a64::Gp fn_out = cc.new_gpx();
				ASMJIT_MOV_FN(cc, fn_out, (void *) fcinfo_out->flinfo->fn_addr);
				InvokeNode *invoke_out;
				a64::Gp ret_out = cc.new_gpx();
				cc.invoke(Out(invoke_out), fn_out,
						  FuncSignature::build<int64_t, void *>());
				invoke_out->set_arg(0, fcinfo_out_reg);
				invoke_out->set_ret(0, ret_out);

				/* fcinfo_in->args[0].value = ret_out */
				a64::Gp fcinfo_in_reg = cc.new_gpx();
				cc.mov(fcinfo_in_reg, (uint64_t) fcinfo_in);
				int64_t in_arg0_val_off = (int64_t)((char*)&fcinfo_in->args[0].value - (char*)fcinfo_in);
				int64_t in_arg0_null_off = (int64_t)((char*)&fcinfo_in->args[0].isnull - (char*)fcinfo_in);
				cc.str(ret_out, a64::Mem(fcinfo_in_reg, (int32_t) in_arg0_val_off));
				cc.strb(zero_reg, a64::Mem(fcinfo_in_reg, (int32_t) in_arg0_null_off));
				cc.strb(zero_reg, a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

				/* Call input fn */
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
				cc.ldrb(isnull_in, a64::Mem(fcinfo_in_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));
				cc.strb(isnull_in, a64::Mem(resnull_addr));

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
					cc.ldrb(a0null, a64::Mem(fcinfo_base, (int32_t) a0null_off));
					cc.ldrb(a1null, a64::Mem(fcinfo_base, (int32_t) a1null_off));
					cc.orr(a0null, a0null, a1null);

					Label not_null = cc.newLabel();
					cc.cbz(a0null, not_null);

					/* Null path */
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					a64::Gp one_reg = cc.new_gpx();
					cc.mov(one_reg, 1);
					cc.strb(one_reg, a64::Mem(resnull_addr));
					cc.b(step_labels[jnull]);

					cc.bind(not_null);
				}

				/* fcinfo->isnull = false */
				a64::Gp fcinfo_reg = cc.new_gpx();
				cc.mov(fcinfo_reg, (uint64_t) fcinfo);
				a64::Gp zero_reg = cc.new_gpx();
				cc.mov(zero_reg, 0);
				cc.strb(zero_reg, a64::Mem(fcinfo_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));

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
				cc.ldrb(isnull_reg, a64::Mem(fcinfo_reg, (int32_t) offsetof(FunctionCallInfoBaseData, isnull)));
				Label not_null2 = cc.newLabel();
				cc.cbz(isnull_reg, not_null2);
				{
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					a64::Gp one = cc.new_gpx();
					cc.mov(one, 1);
					cc.strb(one, a64::Mem(resnull_addr));
					cc.b(step_labels[jnull]);
				}
				cc.bind(not_null2);

				/* *resnull = false */
				{
					a64::Gp resnull_addr = cc.new_gpx();
					cc.mov(resnull_addr, (uint64_t) op->resnull);
					cc.strb(zero_reg, a64::Mem(resnull_addr));
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
				cc.strb(zero_reg, a64::Mem(resnull_addr));
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
				Label cont = cc.newLabel();
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
				cc.strb(one_reg, a64::Mem(resnull_addr));
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

				int fb_jump_target = -1;
				switch (opcode)
				{
#ifdef HAVE_EEOP_HASHDATUM
					case EEOP_HASHDATUM_FIRST_STRICT:
					case EEOP_HASHDATUM_NEXT32_STRICT:
						fb_jump_target = op->d.hashdatum.jumpdone;
						break;
#endif
					default:
						break;
				}

				if (fb_jump_target >= 0 && fb_jump_target < steps_len)
				{
					cc.cmp(ret_reg, 0);
					cc.b(a64::CondCode::kGE, step_labels[fb_jump_target]);
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
