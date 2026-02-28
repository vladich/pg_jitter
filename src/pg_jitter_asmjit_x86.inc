/*
 * pg_jitter_asmjit_x86.inc — x86_64 code generation for AsmJIT backend
 *
 * Included by pg_jitter_asmjit.cpp when compiling for x86_64.
 * Defines asmjit_emit_all() and its helper asmjit_emit_deform_inline().
 *
 * Mechanical translation of pg_jitter_asmjit_arm64.inc.
 */

/*
 * When true, indicates we are compiling code that will be shared via DSM
 * with parallel workers. On x86_64, cc.mov(reg, imm64) already emits the
 * full 10-byte MOVABS instruction, so no special emit function is needed —
 * the relocation scanner in pg_jitter_common.c handles patching natively.
 */
static bool asmjit_shared_code_mode = false;

/* Helper: emit x86 equivalent of ARM64 cbz (compare-and-branch-if-zero) */
static inline void x86_cbz(x86::Compiler &cc, x86::Gp reg, Label &label)
{
	cc.test(reg, reg);
	cc.je(label);
}

/* Helper: emit x86 equivalent of ARM64 cbnz (compare-and-branch-if-nonzero) */
static inline void x86_cbnz(x86::Compiler &cc, x86::Gp reg, Label &label)
{
	cc.test(reg, reg);
	cc.jne(label);
}

/* Helper: emit x86 cset equivalent (setcc + movzx) */
static inline void x86_cset(x86::Compiler &cc, x86::Gp dst, x86::CondCode cond)
{
	switch (cond)
	{
		case x86::CondCode::kEqual:
			cc.sete(dst.r8());
			break;
		case x86::CondCode::kNotEqual:
			cc.setne(dst.r8());
			break;
		case x86::CondCode::kSignedLT:
			cc.setl(dst.r8());
			break;
		case x86::CondCode::kSignedLE:
			cc.setle(dst.r8());
			break;
		case x86::CondCode::kSignedGT:
			cc.setg(dst.r8());
			break;
		case x86::CondCode::kSignedGE:
			cc.setge(dst.r8());
			break;
		default:
			Assert(false);
			break;
	}
	cc.movzx(dst, dst.r8());
}

/*
 * Emit deform code inline into the AsmJIT x86 Compiler.
 */
static bool
asmjit_emit_deform_inline(x86::Compiler &cc,
                           TupleDesc desc,
                           const TupleTableSlotOps *ops,
                           int natts,
                           x86::Gp slot_reg,
                           x86::Gp tmp1,
                           x86::Gp tmp2,
                           x86::Gp tmp3)
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
    x86::Gp v_tts_values = cc.new_gp64("d_values");
    x86::Gp v_tts_isnull = cc.new_gp64("d_isnull");
    x86::Gp v_tupdata_base = cc.new_gp64("d_tupdata");
    x86::Gp v_t_bits = cc.new_gp64("d_tbits");
    x86::Gp v_off = cc.new_gp64("d_off");
    x86::Gp v_hasnulls = cc.new_gp64("d_hasnulls");
    x86::Gp v_maxatt = cc.new_gp64("d_maxatt");
    x86::Gp v_attdatap = cc.new_gp64("d_attdatap");
    x86::Gp dtmp1 = cc.new_gp64("dt1");
    x86::Gp dtmp2 = cc.new_gp64("dt2");

    /* ---- PROLOGUE: load fields from slot ---- */

    /* v_tts_values = slot->tts_values */
    cc.mov(v_tts_values, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
    /* v_tts_isnull = slot->tts_isnull */
    cc.mov(v_tts_isnull, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));

    /* dtmp1 = HeapTuple ptr from slot-type-specific offset */
    cc.mov(dtmp1, x86::qword_ptr(slot_reg, (int32_t) tuple_off));
    /* dtmp1 = tuplep = heaptuple->t_data (HeapTupleHeader) */
    cc.mov(dtmp1, x86::qword_ptr(dtmp1, offsetof(HeapTupleData, t_data)));

    /* v_hasnulls = tuplep->t_infomask & HEAP_HASNULL */
    cc.movzx(dtmp2.r32(), x86::word_ptr(dtmp1, offsetof(HeapTupleHeaderData, t_infomask)));
    cc.mov(v_hasnulls, dtmp2);
    cc.and_(v_hasnulls, Imm(HEAP_HASNULL));

    /* v_maxatt = tuplep->t_infomask2 & HEAP_NATTS_MASK */
    cc.movzx(dtmp2.r32(), x86::word_ptr(dtmp1, offsetof(HeapTupleHeaderData, t_infomask2)));
    cc.mov(v_maxatt, dtmp2);
    cc.and_(v_maxatt, Imm(HEAP_NATTS_MASK));

    /* v_t_bits = &tuplep->t_bits[0] */
    cc.lea(v_t_bits, x86::qword_ptr(dtmp1, offsetof(HeapTupleHeaderData, t_bits)));

    /* t_hoff -> dtmp2 (uint8) */
    cc.movzx(dtmp2.r32(), x86::byte_ptr(dtmp1, offsetof(HeapTupleHeaderData, t_hoff)));
    /* v_tupdata_base = tuplep + t_hoff */
    cc.mov(v_tupdata_base, dtmp1);
    cc.add(v_tupdata_base, dtmp2);

    /* v_off = slot->off (uint32, zero-extended) */
    cc.mov(v_off.r32(), x86::dword_ptr(slot_reg, (int32_t) slot_off));

    /* ---- MISSING ATTRIBUTES CHECK ---- */
    if ((natts - 1) > guaranteed_column_number)
    {
        Label skip_missing = cc.new_label();

        cc.cmp(v_maxatt, natts);
        cc.jge(skip_missing);

        /* call slot_getmissingattrs(slot, maxatt, natts) */
        {
            x86::Gp fn_reg = cc.new_gp64();
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
        x86::Gp v_nvalid = cc.new_gp64("d_nvalid");
        cc.movsx(v_nvalid.r32(), x86::word_ptr(slot_reg, offsetof(TupleTableSlot, tts_nvalid)));

        for (attnum = 0; attnum < natts; attnum++)
        {
            cc.cmp(v_nvalid, attnum);
            cc.je(att_labels[attnum]);
        }
        /* Default: already deformed enough */
        cc.jmp(deform_out);
    }

    /* ---- PER-ATTRIBUTE CODE ---- */
    for (attnum = 0; attnum < natts; attnum++)
    {
        CompactAttribute *att = TupleDescCompactAttr(desc, attnum);
        int alignto = JITTER_ATTALIGNBY(att);

        cc.bind(att_labels[attnum]);

        /* If attnum == 0: reset offset */
        if (attnum == 0)
            cc.xor_(v_off.r32(), v_off.r32());

        /* ---- Availability check ---- */
        if (attnum > guaranteed_column_number)
        {
            cc.cmp(v_maxatt, attnum + 1);
            cc.jl(deform_out);
        }

        /* ---- Null check ---- */
        if (!JITTER_ATT_IS_NOTNULL(att))
        {
            Label notnull = cc.new_label();

            /* if (!hasnulls) skip to notnull */
            x86_cbz(cc, v_hasnulls, notnull);

            /* byte = t_bits[attnum >> 3]; test bit (1 << (attnum & 7)) */
            cc.movzx(dtmp1.r32(), x86::byte_ptr(v_t_bits, attnum >> 3));
            cc.test(dtmp1.r32(), Imm(1 << (attnum & 0x07)));
            cc.jne(notnull);  /* bit set = NOT null */

            /* Column IS NULL: tts_values[attnum] = 0, tts_isnull[attnum] = 1 */
            cc.mov(x86::qword_ptr(v_tts_values, attnum * (int32_t) sizeof(Datum)), Imm(0));
            cc.mov(dtmp1.r32(), 1);
            cc.mov(x86::byte_ptr(v_tts_isnull, attnum), dtmp1.r8());

            /* Jump to next attcheck (or out if last) */
            if (attnum + 1 < natts)
                cc.jmp(att_labels[attnum + 1]);
            else
                cc.jmp(deform_out);

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
                    x86::Gp addr = cc.new_gp64();
                    cc.mov(addr, v_tupdata_base);
                    cc.add(addr, v_off);
                    cc.movzx(dtmp1.r32(), x86::byte_ptr(addr));
                }
                x86_cbnz(cc, dtmp1, skip_align);

                /* Align off */
                cc.add(v_off, Imm(alignto - 1));
                cc.and_(v_off, Imm(~((int64_t)(alignto - 1))));

                cc.bind(skip_align);
            }
            else
            {
                /* Fixed-width: always align */
                cc.add(v_off, Imm(alignto - 1));
                cc.and_(v_off, Imm(~((int64_t)(alignto - 1))));
            }

            if (known_alignment >= 0)
                known_alignment = TYPEALIGN(alignto, known_alignment);
        }

        if (attguaranteedalign)
        {
            Assert(known_alignment >= 0);
            cc.mov(v_off, Imm(known_alignment));
        }

        /* ---- Value extraction ---- */
        /* v_attdatap = v_tupdata_base + v_off */
        cc.mov(v_attdatap, v_tupdata_base);
        cc.add(v_attdatap, v_off);

        /* tts_isnull[attnum] = false */
        cc.mov(x86::byte_ptr(v_tts_isnull, attnum), Imm(0));

        if (att->attbyval)
        {
            switch (att->attlen)
            {
                case 1:
                    cc.movsx(dtmp1, x86::byte_ptr(v_attdatap));
                    break;
                case 2:
                    cc.movsx(dtmp1, x86::word_ptr(v_attdatap));
                    break;
                case 4:
                    cc.movsxd(dtmp1, x86::dword_ptr(v_attdatap));
                    break;
                case 8:
                    cc.mov(dtmp1, x86::qword_ptr(v_attdatap));
                    break;
                default:
                    pfree(nvalid_labels); pfree(att_labels);
                    pfree(null_labels); pfree(has_null_jump);
                    pfree(avail_labels); pfree(has_avail_jump);
                    return false;
            }
            cc.mov(x86::qword_ptr(v_tts_values, attnum * (int32_t) sizeof(Datum)), dtmp1);
        }
        else
        {
            /* Store pointer: tts_values[attnum] = attdatap */
            cc.mov(x86::qword_ptr(v_tts_values, attnum * (int32_t) sizeof(Datum)), v_attdatap);
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
                cc.mov(v_off, Imm(known_alignment));
            }
            else
            {
                cc.add(v_off, Imm(att->attlen));
            }
        }
        else if (att->attlen == -1)
        {
            /* Varlena: off += varsize_any(attdatap) */
            x86::Gp fn_reg = cc.new_gp64();
            cc.mov(fn_reg, (uint64_t)(void *) varsize_any);
            InvokeNode *invoke;
            cc.invoke(Out(invoke), fn_reg,
                      FuncSignature::build<int64_t, void *>());
            invoke->set_arg(0, v_attdatap);
            invoke->set_ret(0, dtmp1);
            cc.add(v_off, dtmp1);
        }
        else if (att->attlen == -2)
        {
            /* Cstring: off += strlen(attdatap) + 1 */
            x86::Gp fn_reg = cc.new_gp64();
            cc.mov(fn_reg, (uint64_t)(void *) strlen);
            InvokeNode *invoke;
            cc.invoke(Out(invoke), fn_reg,
                      FuncSignature::build<int64_t, void *>());
            invoke->set_arg(0, v_attdatap);
            invoke->set_ret(0, dtmp1);
            cc.add(v_off, dtmp1);
            cc.add(v_off, Imm(1));
        }
    }

    /* ---- EPILOGUE ---- */
    cc.bind(deform_out);

    /* tts_nvalid = natts */
    cc.mov(dtmp1.r32(), natts);
    cc.mov(x86::word_ptr(slot_reg, offsetof(TupleTableSlot, tts_nvalid)), dtmp1.r16());

    /* slot->off = (uint32) off */
    cc.mov(x86::dword_ptr(slot_reg, (int32_t) slot_off), v_off.r32());

    /* tts_flags |= TTS_FLAG_SLOW */
    cc.movzx(dtmp1.r32(), x86::word_ptr(slot_reg, offsetof(TupleTableSlot, tts_flags)));
    cc.or_(dtmp1, Imm(TTS_FLAG_SLOW));
    cc.mov(x86::word_ptr(slot_reg, offsetof(TupleTableSlot, tts_flags)), dtmp1.r16());

    pfree(nvalid_labels);
    pfree(att_labels);
    pfree(null_labels);
    pfree(has_null_jump);
    pfree(avail_labels);
    pfree(has_avail_jump);

    return true;
}

/*
 * Emit all expression steps as x86_64 native code.
 *
 * This is the arch-specific codegen body called from asmjit_compile_expr().
 */
static bool
asmjit_emit_all(CodeHolder &code, ExprState *state,
                PgJitterContext *ctx,
                ExprEvalStep *steps, int steps_len)
{
	x86::Compiler cc(&code);

	/* Function: Datum fn(ExprState*, ExprContext*, bool*) */
	FuncNode *funcNode = cc.add_func(
		FuncSignature::build<int64_t, void *, void *, void *>());

	/* Arguments */
	x86::Gp v_state = cc.new_gp64("state");
	x86::Gp v_econtext = cc.new_gp64("econtext");
	x86::Gp v_isnullp = cc.new_gp64("isNull");

	funcNode->set_arg(0, v_state);
	funcNode->set_arg(1, v_econtext);
	funcNode->set_arg(2, v_isnullp);

	/* Scratch registers */
	x86::Gp tmp1 = cc.new_gp64("tmp1");
	x86::Gp tmp2 = cc.new_gp64("tmp2");
	x86::Gp tmp3 = cc.new_gp64("tmp3");
	x86::Gp slot_reg = cc.new_gp64("slot");

	/* Cached pointers (loaded once in prologue) */
	x86::Gp v_resvaluep = cc.new_gp64("resvaluep");
	x86::Gp v_resnullp = cc.new_gp64("resnullp");
	x86::Gp v_resultvals = cc.new_gp64("resultvals");
	x86::Gp v_resultnulls = cc.new_gp64("resultnulls");

	/* Load cached pointers */
	/* resvaluep = &state->resvalue */
	cc.lea(v_resvaluep, x86::qword_ptr(v_state, (int32_t) offsetof(ExprState, resvalue)));
	/* resnullp = &state->resnull */
	cc.lea(v_resnullp, x86::qword_ptr(v_state, (int32_t) offsetof(ExprState, resnull)));
	/* resultslot = state->resultslot (may be NULL for non-projecting exprs) */
	cc.mov(slot_reg, x86::qword_ptr(v_state, offsetof(ExprState, resultslot)));
	{
		Label skip_rs = cc.new_label();
		x86_cbz(cc, slot_reg, skip_rs);
		/* resultvals = resultslot->tts_values */
		cc.mov(v_resultvals, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
		/* resultnulls = resultslot->tts_isnull */
		cc.mov(v_resultnulls, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
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
				cc.mov(tmp1, x86::qword_ptr(v_state, offsetof(ExprState, resvalue)));
				cc.movzx(tmp2.r32(), x86::byte_ptr(v_state, offsetof(ExprState, resnull)));
				/* *isNull = resnull */
				cc.mov(x86::byte_ptr(v_isnullp), tmp2.r8());
				/* return resvalue */
				cc.ret(tmp1);
				break;
			}

#ifdef HAVE_EEOP_DONE_SPLIT
			case EEOP_DONE_NO_RETURN:
			{
				cc.xor_(tmp1.r32(), tmp1.r32());
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
				cc.mov(slot_reg, x86::qword_ptr(v_econtext, (int32_t) soff));
				/* tts_nvalid (AttrNumber = int16) */
				cc.movsx(tmp1.r32(), x86::word_ptr(slot_reg, offsetof(TupleTableSlot, tts_nvalid)));
				cc.cmp(tmp1.r32(), op->d.fetch.last_var);
				cc.jge(skip);

				/* Try inline deform if conditions allow */
				if (op->d.fetch.fixed && op->d.fetch.known_desc &&
					(ctx->base.flags & PGJIT_DEFORM))
				{
					deform_template_fn tmpl = asmjit_deform_match_template(
						op->d.fetch.known_desc,
						op->d.fetch.kind,
						op->d.fetch.last_var);

					if (tmpl)
					{
						x86::Gp tmpl_fn = cc.new_gp64();
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
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) pg_jitter_compiled_deform_dispatch);
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

				cc.mov(slot_reg, x86::qword_ptr(v_econtext, (int32_t) soff));

				/* values_ptr = slot->tts_values; value = values_ptr[attnum] */
				cc.mov(tmp1, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				cc.mov(tmp2, x86::qword_ptr(tmp1, attnum * (int32_t) sizeof(Datum)));
				/* *op->resvalue = value */
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp2);

				/* isnull_ptr = slot->tts_isnull; isnull = isnull_ptr[attnum] */
				cc.mov(tmp1, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, attnum * (int32_t) sizeof(bool)));
				/* *op->resnull = isnull */
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), tmp2.r8());
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
				cc.mov(slot_reg, x86::qword_ptr(v_econtext, (int32_t) soff));

				/* Load value from source */
				cc.mov(tmp1, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_values)));
				cc.mov(tmp2, x86::qword_ptr(tmp1, attnum * (int32_t) sizeof(Datum)));
				/* Store to result slot */
				cc.mov(x86::qword_ptr(v_resultvals, resultnum * (int32_t) sizeof(Datum)), tmp2);

				/* Load null from source */
				cc.mov(tmp1, x86::qword_ptr(slot_reg, offsetof(TupleTableSlot, tts_isnull)));
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, attnum * (int32_t) sizeof(bool)));
				/* Store to result slot */
				cc.mov(x86::byte_ptr(v_resultnulls, resultnum * (int32_t) sizeof(bool)), tmp2.r8());
				break;
			}

			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
			{
				int resultnum = op->d.assign_tmp.resultnum;

				/* Load state->resvalue and state->resnull */
				cc.mov(tmp1, x86::qword_ptr(v_state, offsetof(ExprState, resvalue)));
				cc.movzx(tmp2.r32(), x86::byte_ptr(v_state, offsetof(ExprState, resnull)));

				/* Store null to result slot */
				cc.mov(x86::byte_ptr(v_resultnulls, resultnum * (int32_t) sizeof(bool)), tmp2.r8());

				if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				{
					Label skip_ro = cc.new_label();
					x86_cbnz(cc, tmp2, skip_ro);  /* if null, skip */

					/* tmp1 = MakeExpandedObjectReadOnlyInternal(tmp1) */
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) MakeExpandedObjectReadOnlyInternal);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					invoke->set_arg(0, tmp1);
					invoke->set_ret(0, tmp1);

					cc.bind(skip_ro);
				}

				/* Store value to result slot */
				cc.mov(x86::qword_ptr(v_resultvals, resultnum * (int32_t) sizeof(Datum)), tmp1);
				break;
			}

			case EEOP_CONST:
			{
				/* *op->resvalue = constval.value */
				cc.mov(tmp1, (int64_t) op->d.constval.value);
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);
				/* *op->resnull = constval.isnull */
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(tmp1.r32(), op->d.constval.isnull ? 1 : 0);
				cc.mov(x86::byte_ptr(tmp2), tmp1.r8());
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
					/* Set resnull = true */
					cc.mov(tmp1, (uint64_t) op->resnull);
					cc.mov(tmp2.r32(), 1);
					cc.mov(x86::byte_ptr(tmp1), tmp2.r8());

					cc.mov(tmp1, (uint64_t) fcinfo);
					if (nargs > 1 && nargs <= 4)
					{
						int64_t null_off_0 =
							(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
						cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, (int32_t) null_off_0));
						for (int argno = 1; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull - (char *)fcinfo);
							x86::Gp t = cc.new_gp64();
							cc.movzx(t.r32(), x86::byte_ptr(tmp1, (int32_t) null_off));
							cc.or_(tmp2, t);
						}
						x86_cbnz(cc, tmp2, done_label);
					}
					else
					{
						for (int argno = 0; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull - (char *)fcinfo);
							cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, (int32_t) null_off));
							x86_cbnz(cc, tmp2, done_label);
						}
					}
				}

				/*
				 * Try direct native call - bypasses fcinfo entirely.
				 */
				{
				const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);

				if (dfn && dfn->inline_op != JIT_INLINE_NONE)
				{
					/*
					 * TIER 0 — INLINE: emit the operation as native x86_64
					 * instructions, no function call at all.
					 */
					x86::Gp fci_base = cc.new_gp64();
					x86::Gp a0 = cc.new_gp64(), a1 = cc.new_gp64();
					cc.mov(fci_base, (uint64_t) fcinfo);
					int64_t val_off_0 =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					int64_t val_off_1 =
						(int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					cc.mov(a0, x86::qword_ptr(fci_base, (int32_t) val_off_0));
					cc.mov(a1, x86::qword_ptr(fci_base, (int32_t) val_off_1));

					switch ((JitInlineOp) dfn->inline_op)
					{
					/* ---- int32 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT4_ADD:
					{
						Label ok = cc.new_label();
						cc.add(a0.r32(), a1.r32());
						cc.jno(ok);
						x86::Gp err_fn = cc.new_gp64();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.movsxd(tmp1, a0.r32());
						break;
					}
					case JIT_INLINE_INT4_SUB:
					{
						Label ok = cc.new_label();
						cc.sub(a0.r32(), a1.r32());
						cc.jno(ok);
						x86::Gp err_fn = cc.new_gp64();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.movsxd(tmp1, a0.r32());
						break;
					}
					case JIT_INLINE_INT4_MUL:
					{
						Label ok = cc.new_label();
						/*
						 * Sign-extend both operands to 64-bit, multiply,
						 * check if result fits in 32-bit.
						 */
						cc.movsxd(a0, a0.r32());
						cc.movsxd(a1, a1.r32());
						cc.imul(a0, a1);
						/* Check: does sign-extending low 32 bits give back the full result? */
						x86::Gp ext = cc.new_gp64();
						cc.movsxd(ext, a0.r32());
						cc.cmp(a0, ext);
						cc.je(ok);
						x86::Gp err_fn = cc.new_gp64();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.mov(tmp1, a0);
						break;
					}
					case JIT_INLINE_INT4_DIV:
					{
						Label not_zero = cc.new_label();
						Label not_minmax = cc.new_label();
						/* Check divisor == 0 */
						cc.test(a1.r32(), a1.r32());
						cc.jne(not_zero);
						{
							x86::Gp err_fn = cc.new_gp64();
							cc.mov(err_fn, (uint64_t)(void *) jit_error_division_by_zero);
							InvokeNode *inv;
							cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						}
						cc.bind(not_zero);
						/* Check INT32_MIN / -1 overflow */
						cc.cmp(a0.r32(), (int32_t) PG_INT32_MIN);
						cc.jne(not_minmax);
						{
							Label not_neg1 = cc.new_label();
							cc.cmp(a1.r32(), -1);
							cc.jne(not_neg1);
							x86::Gp err_fn = cc.new_gp64();
							cc.mov(err_fn, (uint64_t)(void *) jit_error_int4_overflow);
							InvokeNode *inv;
							cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
							cc.bind(not_neg1);
						}
						cc.bind(not_minmax);
						/* idiv: EDX:EAX / a1 → EAX=quotient, EDX=remainder */
						{
							x86::Gp dividend = cc.new_gp32();
							x86::Gp remainder = cc.new_gp32();
							cc.mov(dividend, a0.r32());
							cc.cdq(remainder, dividend);
							cc.idiv(remainder, dividend, a1.r32());
							cc.movsxd(tmp1, dividend);
						}
						break;
					}
					case JIT_INLINE_INT4_MOD:
					{
						Label not_zero = cc.new_label();
						Label not_minmax = cc.new_label();
						Label zero_result = cc.new_label();
						Label after = cc.new_label();
						/* Check divisor == 0 */
						cc.test(a1.r32(), a1.r32());
						cc.jne(not_zero);
						{
							x86::Gp err_fn = cc.new_gp64();
							cc.mov(err_fn, (uint64_t)(void *) jit_error_division_by_zero);
							InvokeNode *inv;
							cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						}
						cc.bind(not_zero);
						/* Check INT32_MIN % -1 → 0 */
						cc.cmp(a0.r32(), (int32_t) PG_INT32_MIN);
						cc.jne(not_minmax);
						cc.cmp(a1.r32(), -1);
						cc.je(zero_result);
						cc.bind(not_minmax);
						{
							x86::Gp dividend = cc.new_gp32();
							x86::Gp remainder = cc.new_gp32();
							cc.mov(dividend, a0.r32());
							cc.cdq(remainder, dividend);
							cc.idiv(remainder, dividend, a1.r32());
							cc.movsxd(tmp1, remainder);
						}
						cc.jmp(after);
						cc.bind(zero_result);
						cc.xor_(tmp1.r32(), tmp1.r32());
						cc.bind(after);
						break;
					}
					/* ---- int64 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT8_ADD:
					{
						Label ok = cc.new_label();
						cc.add(a0, a1);
						cc.jno(ok);
						x86::Gp err_fn = cc.new_gp64();
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
						cc.sub(a0, a1);
						cc.jno(ok);
						x86::Gp err_fn = cc.new_gp64();
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
						/*
						 * x86_64: use imul 2-op for low result,
						 * then check overflow via sign extension.
						 */
						cc.imul(a0, a1);
						cc.jno(ok);
						x86::Gp err_fn = cc.new_gp64();
						cc.mov(err_fn, (uint64_t)(void *) jit_error_int8_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), err_fn, FuncSignature::build<void>());
						cc.bind(ok);
						cc.mov(tmp1, a0);
						break;
					}
					/* ---- int32 comparison ---- */
					case JIT_INLINE_INT4_EQ:
						cc.cmp(a0.r32(), a1.r32());
						x86_cset(cc, tmp1, x86::CondCode::kEqual);
						break;
					case JIT_INLINE_INT4_NE:
						cc.cmp(a0.r32(), a1.r32());
						x86_cset(cc, tmp1, x86::CondCode::kNotEqual);
						break;
					case JIT_INLINE_INT4_LT:
						cc.cmp(a0.r32(), a1.r32());
						x86_cset(cc, tmp1, x86::CondCode::kSignedLT);
						break;
					case JIT_INLINE_INT4_LE:
						cc.cmp(a0.r32(), a1.r32());
						x86_cset(cc, tmp1, x86::CondCode::kSignedLE);
						break;
					case JIT_INLINE_INT4_GT:
						cc.cmp(a0.r32(), a1.r32());
						x86_cset(cc, tmp1, x86::CondCode::kSignedGT);
						break;
					case JIT_INLINE_INT4_GE:
						cc.cmp(a0.r32(), a1.r32());
						x86_cset(cc, tmp1, x86::CondCode::kSignedGE);
						break;
					/* ---- int64 comparison ---- */
					case JIT_INLINE_INT8_EQ:
						cc.cmp(a0, a1);
						x86_cset(cc, tmp1, x86::CondCode::kEqual);
						break;
					case JIT_INLINE_INT8_NE:
						cc.cmp(a0, a1);
						x86_cset(cc, tmp1, x86::CondCode::kNotEqual);
						break;
					case JIT_INLINE_INT8_LT:
						cc.cmp(a0, a1);
						x86_cset(cc, tmp1, x86::CondCode::kSignedLT);
						break;
					case JIT_INLINE_INT8_LE:
						cc.cmp(a0, a1);
						x86_cset(cc, tmp1, x86::CondCode::kSignedLE);
						break;
					case JIT_INLINE_INT8_GT:
						cc.cmp(a0, a1);
						x86_cset(cc, tmp1, x86::CondCode::kSignedGT);
						break;
					case JIT_INLINE_INT8_GE:
						cc.cmp(a0, a1);
						x86_cset(cc, tmp1, x86::CondCode::kSignedGE);
						break;
					default:
						Assert(false);
						break;
					}

					/* Store *op->resvalue = tmp1 */
					cc.mov(tmp2, (uint64_t) op->resvalue);
					cc.mov(x86::qword_ptr(tmp2), tmp1);

					/* *op->resnull = false */
					cc.mov(tmp2, (uint64_t) op->resnull);
					cc.mov(x86::byte_ptr(tmp2), Imm(0));
				}
				else if (dfn && (dfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
						|| (dfn->jit_fn_name &&
							mir_find_precompiled_fn(dfn->jit_fn_name))
#endif
						))
				{
					void *call_target = dfn->jit_fn;
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					if (!call_target)
						call_target = mir_find_precompiled_fn(dfn->jit_fn_name);
#endif
					x86::Gp args[4];
					x86::Gp fci_base = cc.new_gp64();
					cc.mov(fci_base, (uint64_t) fcinfo);
					for (int i = 0; i < dfn->nargs; i++)
					{
						args[i] = cc.new_gp64();
						int64_t val_off =
							(int64_t)((char *)&fcinfo->args[i].value - (char *)fcinfo);
						cc.mov(args[i], x86::qword_ptr(fci_base, (int32_t) val_off));
					}

					x86::Gp fn_reg = cc.new_gp64();
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

					cc.mov(tmp2, (uint64_t) op->resvalue);
					cc.mov(x86::qword_ptr(tmp2), tmp1);

					cc.mov(tmp2, (uint64_t) op->resnull);
					cc.mov(x86::byte_ptr(tmp2), Imm(0));
				}
				else
				{
				/* Fallback: generic fcinfo path */
				{
					x86::Gp fci = cc.new_gp64();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.mov(x86::byte_ptr(fci, offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
				}
				{
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.func.fn_addr);
					x86::Gp fcinfo_reg = cc.new_gp64();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);
				cc.mov(tmp1, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, offsetof(FunctionCallInfoBaseData, isnull)));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), tmp2.r8());
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
					cc.mov(x86::byte_ptr(tmp1), Imm(0));
				}

				/* Load resnull */
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbnz(cc, tmp2, null_handler);

				/* Not null: check false */
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				x86_cbz(cc, tmp2, step_labels[op->d.boolexpr.jumpdone]);

				cc.jmp(cont);

				/* Null handler: set anynull */
				cc.bind(null_handler);
				cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
				cc.mov(x86::byte_ptr(tmp1), Imm(1));

				cc.bind(cont);

				if (opcode == EEOP_BOOL_AND_STEP_LAST)
				{
					Label no_anynull = cc.new_label();
					cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
					cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
					x86_cbz(cc, tmp2, no_anynull);

					cc.mov(tmp1, (uint64_t) op->resnull);
					cc.mov(x86::byte_ptr(tmp1), Imm(1));
					cc.mov(tmp1, (uint64_t) op->resvalue);
					cc.mov(x86::qword_ptr(tmp1), Imm(0));

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
					cc.mov(x86::byte_ptr(tmp1), Imm(0));
				}

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbnz(cc, tmp2, null_handler);

				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				x86_cbnz(cc, tmp2, step_labels[op->d.boolexpr.jumpdone]);

				cc.jmp(cont);

				cc.bind(null_handler);
				cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
				cc.mov(x86::byte_ptr(tmp1), Imm(1));

				cc.bind(cont);

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					Label no_anynull = cc.new_label();
					cc.mov(tmp1, (uint64_t) op->d.boolexpr.anynull);
					cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
					x86_cbz(cc, tmp2, no_anynull);

					cc.mov(tmp1, (uint64_t) op->resnull);
					cc.mov(x86::byte_ptr(tmp1), Imm(1));
					cc.mov(tmp1, (uint64_t) op->resvalue);
					cc.mov(x86::qword_ptr(tmp1), Imm(0));

					cc.bind(no_anynull);
				}
				break;
			}

			case EEOP_BOOL_NOT_STEP:
			{
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				cc.test(tmp2, tmp2);
				x86_cset(cc, tmp2, x86::CondCode::kEqual);
				cc.mov(x86::qword_ptr(tmp1), tmp2);
				break;
			}

			case EEOP_QUAL:
			{
				Label qualfail = cc.new_label();
				Label cont = cc.new_label();

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbnz(cc, tmp2, qualfail);

				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				x86_cbz(cc, tmp2, qualfail);

				cc.jmp(cont);

				cc.bind(qualfail);
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp1), Imm(0));
				cc.jmp(step_labels[op->d.qualexpr.jumpdone]);

				cc.bind(cont);
				break;
			}

			case EEOP_JUMP:
				cc.jmp(step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NULL:
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbnz(cc, tmp2, step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NOT_NULL:
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbz(cc, tmp2, step_labels[op->d.jump.jumpdone]);
				break;

			case EEOP_JUMP_IF_NOT_TRUE:
			{
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbnz(cc, tmp2, step_labels[op->d.jump.jumpdone]);

				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				x86_cbz(cc, tmp2, step_labels[op->d.jump.jumpdone]);
				break;
			}

			case EEOP_NULLTEST_ISNULL:
			{
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp2);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				cc.test(tmp2, tmp2);
				x86_cset(cc, tmp2, x86::CondCode::kEqual);
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp2);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));
				break;
			}

#ifdef HAVE_EEOP_HASHDATUM
			case EEOP_HASHDATUM_SET_INITVAL:
			{
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(tmp1, (int64_t) op->d.hashdatum_initvalue.init_value);
				cc.mov(x86::qword_ptr(tmp3), tmp1);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(0));
				break;
			}

			case EEOP_HASHDATUM_FIRST:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label store_zero = cc.new_label();
				Label store_result = cc.new_label();

				x86::Gp fci_reg = cc.new_gp64();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(fci_reg, (int32_t) arg0_null_off));
				x86_cbnz(cc, tmp2, store_zero);

				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					x86::Gp hash_arg = cc.new_gp64();
					cc.mov(hash_arg, x86::qword_ptr(fci_reg, (int32_t) val_off));
					x86::Gp hfn_reg = cc.new_gp64();
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
					cc.mov(x86::byte_ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.jmp(store_result);

				cc.bind(store_zero);
				cc.xor_(tmp1.r32(), tmp1.r32());

				cc.bind(store_result);
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp1);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(0));
				break;
			}

			case EEOP_HASHDATUM_FIRST_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t arg0_null_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				Label is_null = cc.new_label();
				Label done = cc.new_label();

				x86::Gp fci_reg = cc.new_gp64();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(fci_reg, (int32_t) arg0_null_off));
				x86_cbnz(cc, tmp2, is_null);

				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					x86::Gp hash_arg = cc.new_gp64();
					cc.mov(hash_arg, x86::qword_ptr(fci_reg, (int32_t) val_off));
					x86::Gp hfn_reg = cc.new_gp64();
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
					cc.mov(x86::byte_ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp1);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(0));
				cc.jmp(done);

				cc.bind(is_null);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(1));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), Imm(0));
				cc.jmp(step_labels[op->d.hashdatum.jumpdone]);

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

				x86::Gp hash = cc.new_gp64("hash");
				cc.mov(tmp1, (uint64_t) &iresult->value);
				cc.mov(hash.r32(), x86::dword_ptr(tmp1));

				/* Rotate left 1 = rotate right 31 */
				cc.ror(hash.r32(), Imm(31));

				x86::Gp fci_reg = cc.new_gp64();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(fci_reg, (int32_t) arg0_null_off));
				x86_cbnz(cc, tmp2, skip_hash);

				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					x86::Gp hash_arg = cc.new_gp64();
					cc.mov(hash_arg, x86::qword_ptr(fci_reg, (int32_t) val_off));
					x86::Gp hfn_reg = cc.new_gp64();
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
					cc.mov(x86::byte_ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				cc.xor_(hash.r32(), tmp1.r32());

				cc.bind(skip_hash);
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), hash);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(0));
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

				x86::Gp fci_reg = cc.new_gp64();
				cc.mov(fci_reg, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(fci_reg, (int32_t) arg0_null_off));
				x86_cbnz(cc, tmp2, is_null);

				x86::Gp hash = cc.new_gp64("hash");
				cc.mov(tmp1, (uint64_t) &iresult->value);
				cc.mov(hash.r32(), x86::dword_ptr(tmp1));

				cc.ror(hash.r32(), Imm(31));

				{
				const JitDirectFn *hdfn = jit_find_direct_fn(op->d.hashdatum.fn_addr);
				if (hdfn && (hdfn->jit_fn
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
					|| (hdfn->jit_fn_name && mir_find_precompiled_fn(hdfn->jit_fn_name))
#endif
					))
				{
					int64_t val_off =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					x86::Gp hash_arg = cc.new_gp64();
					cc.mov(hash_arg, x86::qword_ptr(fci_reg, (int32_t) val_off));
					x86::Gp hfn_reg = cc.new_gp64();
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
					cc.mov(x86::byte_ptr(fci_reg, offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.hashdatum.fn_addr);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fci_reg);
					invoke->set_ret(0, tmp1);
				}
				}
				cc.xor_(hash.r32(), tmp1.r32());

				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), hash);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(0));
				cc.jmp(done);

				cc.bind(is_null);
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), Imm(1));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), Imm(0));
				cc.jmp(step_labels[op->d.hashdatum.jumpdone]);

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

				/* Compute pergroup at runtime */
				x86::Gp pergroup = cc.new_gp64();
				{
					x86::Gp aggst = cc.new_gp64();
					cc.mov(aggst, x86::qword_ptr(v_state, offsetof(ExprState, parent)));
					cc.mov(pergroup, x86::qword_ptr(aggst, offsetof(AggState, all_pergroups)));
					cc.mov(pergroup, x86::qword_ptr(pergroup, setoff * (int32_t)sizeof(AggStatePerGroup)));
					if (transno != 0)
						cc.add(pergroup, Imm(transno * (int32_t)sizeof(AggStatePerGroupData)));
				}

				if (is_init)
				{
					Label no_init = cc.new_label();
					cc.movzx(tmp1.r32(), x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, noTransValue)));
					x86_cbz(cc, tmp1, no_init);

					{
						x86::Gp aggst = cc.new_gp64();
						cc.mov(aggst, x86::qword_ptr(v_state, offsetof(ExprState, parent)));
						x86::Gp pt = cc.new_gp64();
						cc.mov(pt, (uint64_t) pertrans);
						x86::Gp ac = cc.new_gp64();
						cc.mov(ac, (uint64_t) aggcontext);
						x86::Gp fn = cc.new_gp64();
						cc.mov(fn, (uint64_t)(void *) ExecAggInitGroup);
						InvokeNode *inv;
						cc.invoke(Out(inv), fn, FuncSignature::build<void, void*, void*, void*, void*>());
						inv->set_arg(0, aggst);
						inv->set_arg(1, pt);
						inv->set_arg(2, pergroup);
						inv->set_arg(3, ac);
					}
					cc.jmp(end_label);
					cc.bind(no_init);
				}

				if (is_strict)
				{
					cc.movzx(tmp1.r32(), x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
					x86_cbnz(cc, tmp1, end_label);
				}

				if (!is_byref &&
					(fn_addr == int8inc || fn_addr == int8inc_any))
				{
					x86::Gp tv = cc.new_gp64();
					cc.mov(tv, x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					Label ok = cc.new_label();
					cc.add(tv, Imm(1));
					cc.jno(ok);
					{
						x86::Gp fn = cc.new_gp64();
						cc.mov(fn, (uint64_t)(void *) jit_error_int8_overflow);
						InvokeNode *inv;
						cc.invoke(Out(inv), fn, FuncSignature::build<void>());
					}
					cc.bind(ok);
					cc.mov(x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)), tv);
					cc.mov(x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)), Imm(0));
				}
				else if (!is_byref && fn_addr == int4_sum)
				{
					Label arg_not_null = cc.new_label();
					Label trans_not_null = cc.new_label();
					Label after_sum = cc.new_label();

					int64_t isnull1_off = (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
					x86::Gp fci = cc.new_gp64();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.movzx(tmp1.r32(), x86::byte_ptr(fci, (int32_t) isnull1_off));
					x86_cbz(cc, tmp1, arg_not_null);
					cc.jmp(end_label);

					cc.bind(arg_not_null);
					x86::Gp arg1 = cc.new_gp64();
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					cc.mov(arg1, x86::qword_ptr(fci, (int32_t) val1_off));
					cc.movsxd(arg1, arg1.r32());

					cc.movzx(tmp1.r32(), x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)));
					x86_cbz(cc, tmp1, trans_not_null);

					cc.mov(x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)), arg1);
					cc.mov(x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)), Imm(0));
					cc.jmp(after_sum);

					cc.bind(trans_not_null);
					{
						x86::Gp tv = cc.new_gp64();
						cc.mov(tv, x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
						cc.add(tv, arg1);
						cc.mov(x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)), tv);
					}
					cc.bind(after_sum);
				}
				else if (!is_byref &&
						 (fn_addr == int4smaller || fn_addr == int4larger))
				{
					bool is_min = (fn_addr == int4smaller);
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					x86::Gp tv = cc.new_gp64(), nv = cc.new_gp64();
					cc.mov(tv, x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					x86::Gp fci = cc.new_gp64();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.mov(nv, x86::qword_ptr(fci, (int32_t) val1_off));

					cc.movsxd(tv, tv.r32());
					cc.movsxd(nv, nv.r32());

					Label skip_update = cc.new_label();
					cc.cmp(tv, nv);
					if (is_min)
						cc.jle(skip_update);
					else
						cc.jge(skip_update);
					cc.mov(x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)), nv);
					cc.bind(skip_update);
					cc.mov(x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)), Imm(0));
				}
				else if (is_byref &&
						 (fn_addr == int4_avg_accum || fn_addr == int2_avg_accum))
				{
					#define INT8_TRANS_DATA_OFFSET_ASM 24
					StaticAssertDecl(
						ARR_OVERHEAD_NONULLS(1) == INT8_TRANS_DATA_OFFSET_ASM,
						"Int8TransTypeData offset must be 24");

					x86::Gp tv_ptr = cc.new_gp64();
					cc.mov(tv_ptr, x86::qword_ptr(pergroup, offsetof(AggStatePerGroupData, transValue)));
					cc.add(tv_ptr, Imm(INT8_TRANS_DATA_OFFSET_ASM));

					x86::Gp cnt = cc.new_gp64();
					cc.mov(cnt, x86::qword_ptr(tv_ptr, offsetof(JitInt8TransTypeData, count)));
					cc.add(cnt, Imm(1));
					cc.mov(x86::qword_ptr(tv_ptr, offsetof(JitInt8TransTypeData, count)), cnt);

					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					x86::Gp arg1 = cc.new_gp64();
					x86::Gp fci = cc.new_gp64();
					cc.mov(fci, (uint64_t) fcinfo);
					cc.mov(arg1, x86::qword_ptr(fci, (int32_t) val1_off));
					if (fn_addr == int4_avg_accum)
						cc.movsxd(arg1, arg1.r32());
					else
						cc.movsx(arg1, arg1.r16());

					x86::Gp sum = cc.new_gp64();
					cc.mov(sum, x86::qword_ptr(tv_ptr, offsetof(JitInt8TransTypeData, sum)));
					cc.add(sum, arg1);
					cc.mov(x86::qword_ptr(tv_ptr, offsetof(JitInt8TransTypeData, sum)), sum);

					cc.mov(x86::byte_ptr(pergroup, offsetof(AggStatePerGroupData, transValueIsNull)), Imm(0));
					#undef INT8_TRANS_DATA_OFFSET_ASM
				}
				else
				{
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
					x86::Gp afn_reg = cc.new_gp64();
					cc.mov(afn_reg, (uint64_t) helper_fn);
					x86::Gp aop_reg = cc.new_gp64();
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

					x86::Gp fci_reg = cc.new_gp64("fci");
					cc.mov(fci_reg, (uint64_t) fcinfo);
					cc.movzx(tmp2.r32(), x86::byte_ptr(fci_reg, (int32_t) off_arg0_isnull));
					x86_cbnz(cc, tmp2, lbl_null_result);

					x86::Gp scalar = cc.new_gp64("scalar");
					cc.mov(scalar, x86::qword_ptr(fci_reg, (int32_t) off_arg0_value));

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
								cc.jmp(lbl_not_found);
								continue;
							}

							if (lo == hi)
							{
								x86::Gp cval = cc.new_gp64();
								cc.mov(cval,
									(uint64_t) sorted_vals[lo]);
								cc.cmp(scalar, cval);
								cc.je(lbl_found);
								cc.jmp(lbl_not_found);
								continue;
							}

							mid = lo + (hi - lo) / 2;

							x86::Gp cval = cc.new_gp64();
							cc.mov(cval,
								(uint64_t) sorted_vals[mid]);
							cc.cmp(scalar, cval);
							cc.je(lbl_found);

							Label lbl_lt = cc.new_label();
							cc.jl(lbl_lt);

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
						cc.mov(tmp3, (uint64_t) op->resvalue);
						cc.mov(tmp1,
							(uint64_t)(int64_t)(inclause ? 1 : 0));
						cc.mov(x86::qword_ptr(tmp3), tmp1);
						cc.mov(tmp3, (uint64_t) op->resnull);
						cc.mov(x86::byte_ptr(tmp3), Imm(0));
						cc.jmp(lbl_done);
					}

					cc.bind(lbl_not_found);
					{
						if (array_has_nulls)
						{
							cc.mov(tmp3, (uint64_t) op->resvalue);
							cc.mov(x86::qword_ptr(tmp3), Imm(0));
							cc.mov(tmp3, (uint64_t) op->resnull);
							cc.mov(x86::byte_ptr(tmp3), Imm(1));
						}
						else
						{
							cc.mov(tmp3, (uint64_t) op->resvalue);
							cc.mov(tmp1,
								(uint64_t)(int64_t)(inclause ? 0 : 1));
							cc.mov(x86::qword_ptr(tmp3), tmp1);
							cc.mov(tmp3, (uint64_t) op->resnull);
							cc.mov(x86::byte_ptr(tmp3), Imm(0));
						}
						cc.jmp(lbl_done);
					}

					cc.bind(lbl_null_result);
					{
						cc.mov(tmp3, (uint64_t) op->resvalue);
						cc.mov(x86::qword_ptr(tmp3), Imm(0));
						cc.mov(tmp3, (uint64_t) op->resnull);
						cc.mov(x86::byte_ptr(tmp3), Imm(1));
					}

					cc.bind(lbl_done);

					pfree(sorted_vals);
				}
				else
				{
					if (sorted_vals)
						pfree(sorted_vals);

					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) ExecEvalHashedScalarArrayOp);
					x86::Gp op_reg = cc.new_gp64();
					cc.mov(op_reg, (uint64_t) op);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<void, void *, void *, void *>());
					invoke->set_arg(0, v_state);
					invoke->set_arg(1, op_reg);
					invoke->set_arg(2, v_econtext);
				}
#else /* PG14: no inclause/saop -- always use fallback */
				{
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) ExecEvalHashedScalarArrayOp);
					x86::Gp op_reg = cc.new_gp64();
					cc.mov(op_reg, (uint64_t) op);
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
				x86::Gp aggst = cc.new_gp64();
				cc.mov(aggst, x86::qword_ptr(v_state, offsetof(ExprState, parent)));

				/* pertrans = op->d.agg_presorted_distinctcheck.pertrans */
				x86::Gp pt = cc.new_gp64();
				cc.mov(pt, (uint64_t) op->d.agg_presorted_distinctcheck.pertrans);

				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t) fn);
				InvokeNode *invoke;
				x86::Gp ret_reg = cc.new_gp64();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *>());
				invoke->set_arg(0, aggst);
				invoke->set_arg(1, pt);
				invoke->set_ret(0, ret_reg);

				/* If result == 0 (not distinct), jump to jumpdistinct */
				if (jumpdistinct >= 0 && jumpdistinct < steps_len)
					x86_cbz(cc, ret_reg, step_labels[jumpdistinct]);
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

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbz(cc, tmp2, not_null);

				/* Null path */
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp1), Imm(0));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));

				cc.bind(not_null);
				break;
			}

			case EEOP_BOOLTEST_IS_NOT_TRUE:
			{
				/* If null: resvalue=true, resnull=false. Else invert. */
				Label not_null = cc.new_label();
				Label done = cc.new_label();

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbz(cc, tmp2, not_null);

				/* Null path: resvalue=true, resnull=false */
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp1), Imm(1));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));
				cc.jmp(done);

				/* Not null: invert resvalue */
				cc.bind(not_null);
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				cc.test(tmp2, tmp2);
				x86_cset(cc, tmp2, x86::CondCode::kEqual);
				cc.mov(x86::qword_ptr(tmp1), tmp2);

				cc.bind(done);
				break;
			}

			case EEOP_BOOLTEST_IS_FALSE:
			{
				/* If null: resvalue=false, resnull=false. Else invert. */
				Label not_null = cc.new_label();
				Label done = cc.new_label();

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbz(cc, tmp2, not_null);

				/* Null path: resvalue=false, resnull=false */
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp1), Imm(0));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));
				cc.jmp(done);

				/* Not null: invert resvalue */
				cc.bind(not_null);
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				cc.test(tmp2, tmp2);
				x86_cset(cc, tmp2, x86::CondCode::kEqual);
				cc.mov(x86::qword_ptr(tmp1), tmp2);

				cc.bind(done);
				break;
			}

			case EEOP_BOOLTEST_IS_NOT_FALSE:
			{
				/* If null: resvalue=true, resnull=false. Else keep. */
				Label not_null = cc.new_label();

				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				x86_cbz(cc, tmp2, not_null);

				/* Null path */
				cc.mov(tmp1, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp1), Imm(1));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), Imm(0));

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
				cc.mov(tmp1, x86::qword_ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggvalues)));
				cc.mov(tmp2, x86::qword_ptr(tmp1,
					aggno * (int32_t) sizeof(Datum)));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp2);

				/* resnull = econtext->ecxt_aggnulls[aggno] */
				cc.mov(tmp1, x86::qword_ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggnulls)));
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1,
					aggno * (int32_t) sizeof(bool)));
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), tmp2.r8());
				break;
			}

			/*
			 * ---- WINDOW_FUNC ----
			 * wfuncno read at runtime (assigned after compilation).
			 */
			case EEOP_WINDOW_FUNC:
			{
				/* tmp1 = wfstate (compile-time known ptr) */
				cc.mov(tmp1, (uint64_t) op->d.window_func.wfstate);
				/* tmp2 = wfstate->wfuncno (read at runtime) */
				cc.movsxd(tmp2, x86::dword_ptr(tmp1,
					offsetof(WindowFuncExprState, wfuncno)));

				/* tmp3 = econtext->ecxt_aggvalues */
				cc.mov(tmp3, x86::qword_ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggvalues)));
				/* tmp1 = tmp2 * 8 (sizeof Datum) */
				cc.shl(tmp2, 3);
				cc.add(tmp3, tmp2);
				cc.mov(tmp1, x86::qword_ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp1);

				/* Reload wfuncno for nulls */
				cc.mov(tmp1, (uint64_t) op->d.window_func.wfstate);
				cc.movsxd(tmp2, x86::dword_ptr(tmp1,
					offsetof(WindowFuncExprState, wfuncno)));
				/* tmp3 = econtext->ecxt_aggnulls */
				cc.mov(tmp3, x86::qword_ptr(v_econtext,
					offsetof(ExprContext, ecxt_aggnulls)));
				cc.add(tmp3, tmp2);
				cc.movzx(tmp1.r32(), x86::byte_ptr(tmp3));
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), tmp1.r8());
				break;
			}

			/*
			 * ---- CASE_TESTVAL / DOMAIN_TESTVAL ----
			 * Copy from cached pointer.
			 */
			case EEOP_CASE_TESTVAL:
			case EEOP_DOMAIN_TESTVAL:
			{
				/* *op->resvalue = *op->d.casetest.value */
				cc.mov(tmp1, (uint64_t) op->d.casetest.value);
				cc.mov(tmp2, x86::qword_ptr(tmp1));
				cc.mov(tmp3, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp3), tmp2);

				/* *op->resnull = *op->d.casetest.isnull */
				cc.mov(tmp1, (uint64_t) op->d.casetest.isnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));
				cc.mov(tmp3, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp3), tmp2.r8());
				break;
			}

#ifdef HAVE_EEOP_TESTVAL_EXT
			case EEOP_CASE_TESTVAL_EXT:
			{
				/* *op->resvalue = econtext->caseValue_datum */
				cc.mov(tmp1, x86::qword_ptr(v_econtext,
					offsetof(ExprContext, caseValue_datum)));
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);

				/* *op->resnull = econtext->caseValue_isNull */
				cc.movzx(tmp1.r32(), x86::byte_ptr(v_econtext,
					offsetof(ExprContext, caseValue_isNull)));
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), tmp1.r8());
				break;
			}

			case EEOP_DOMAIN_TESTVAL_EXT:
			{
				/* *op->resvalue = econtext->domainValue_datum */
				cc.mov(tmp1, x86::qword_ptr(v_econtext,
					offsetof(ExprContext, domainValue_datum)));
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);

				/* *op->resnull = econtext->domainValue_isNull */
				cc.movzx(tmp1.r32(), x86::byte_ptr(v_econtext,
					offsetof(ExprContext, domainValue_isNull)));
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), tmp1.r8());
				break;
			}
#endif /* HAVE_EEOP_TESTVAL_EXT */

			/*
			 * ---- MAKE_READONLY ----
			 */
			case EEOP_MAKE_READONLY:
			{
				Label skip_null = cc.new_label();

				/* tmp1 = *op->d.make_readonly.isnull */
				cc.mov(tmp1, (uint64_t) op->d.make_readonly.isnull);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1));

				/* *op->resnull = isnull */
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), tmp2.r8());

				/* If null, skip */
				x86_cbnz(cc, tmp2, skip_null);

				/* Not null: resvalue = MakeExpandedObjectReadOnlyInternal(*value) */
				cc.mov(tmp1, (uint64_t) op->d.make_readonly.value);
				cc.mov(tmp1, x86::qword_ptr(tmp1));
				{
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) MakeExpandedObjectReadOnlyInternal);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, int64_t>());
					invoke->set_arg(0, tmp1);
					invoke->set_ret(0, tmp1);
				}
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);

				cc.bind(skip_null);
				break;
			}

			/*
			 * ---- AGG_STRICT_INPUT_CHECK_ARGS ----
			 * Check NullableDatum args[i].isnull for i=0..nargs-1,
			 * jump to jumpnull if any null.
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

				/* tmp1 = args base */
				cc.mov(tmp1, (uint64_t) args);

				if (nargs > 1 && nargs <= 4)
				{
					/* OR-batch: load all isnull, OR together, 1 branch */
					cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, (int32_t) isnull_off0));
					for (int argno = 1; argno < nargs; argno++)
					{
						x86::Gp t = cc.new_gp64();
						cc.movzx(t.r32(), x86::byte_ptr(tmp1,
							(int32_t)(argno * nd_size + isnull_off0)));
						cc.or_(tmp2, t);
					}
					x86_cbnz(cc, tmp2, step_labels[jumpnull]);
				}
				else
				{
					for (int argno = 0; argno < nargs; argno++)
					{
						cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1,
							(int32_t)(argno * nd_size + isnull_off0)));
						x86_cbnz(cc, tmp2, step_labels[jumpnull]);
					}
				}
				break;
			}

			/*
			 * ---- AGG_STRICT_INPUT_CHECK_NULLS ----
			 * Check bool nulls[i] for i=0..nargs-1,
			 * jump to jumpnull if any null.
			 */
			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
			{
				bool *nulls = op->d.agg_strict_input_check.nulls;
				int nargs = op->d.agg_strict_input_check.nargs;
				int jumpnull = op->d.agg_strict_input_check.jumpnull;

				/* tmp1 = nulls base */
				cc.mov(tmp1, (uint64_t) nulls);

				if (nargs > 1 && nargs <= 4)
				{
					cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, 0));
					for (int argno = 1; argno < nargs; argno++)
					{
						x86::Gp t = cc.new_gp64();
						cc.movzx(t.r32(), x86::byte_ptr(tmp1,
							argno * (int32_t) sizeof(bool)));
						cc.or_(tmp2, t);
					}
					x86_cbnz(cc, tmp2, step_labels[jumpnull]);
				}
				else
				{
					for (int argno = 0; argno < nargs; argno++)
					{
						cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1,
							argno * (int32_t) sizeof(bool)));
						x86_cbnz(cc, tmp2, step_labels[jumpnull]);
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
				cc.mov(tmp1, x86::qword_ptr(v_state,
					offsetof(ExprState, parent)));
				/* tmp1 = aggstate->all_pergroups */
				cc.mov(tmp1, x86::qword_ptr(tmp1,
					offsetof(AggState, all_pergroups)));
				/* tmp1 = all_pergroups[setoff] */
				cc.mov(tmp1, x86::qword_ptr(tmp1,
					setoff * (int32_t) sizeof(AggStatePerGroup)));

				/* If NULL, jump to jumpnull */
				x86_cbz(cc, tmp1, step_labels[jumpnull]);
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
					/* Check args[0].isnull → jump to jumpnull */
					int64_t null_off =
						(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
					cc.mov(tmp1, (uint64_t) fcinfo);
					cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1, (int32_t) null_off));
					x86_cbnz(cc, tmp2, step_labels[op->d.agg_deserialize.jumpnull]);
				}

				/* fcinfo->isnull = false */
				cc.mov(tmp1, (uint64_t) fcinfo);
				cc.mov(x86::byte_ptr(tmp1,
					offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));

				/* R0 = fcinfo->flinfo->fn_addr(fcinfo) */
				{
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) fcinfo->flinfo->fn_addr);
					x86::Gp fcinfo_reg = cc.new_gp64();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}

				/* *op->resvalue = result */
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);

				/* *op->resnull = fcinfo->isnull */
				cc.mov(tmp1, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp1,
					offsetof(FunctionCallInfoBaseData, isnull)));
				cc.mov(tmp1, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp1), tmp2.r8());
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

				/* Load both isnull flags */
				cc.mov(tmp3, (uint64_t) fcinfo);
				cc.movzx(tmp1.r32(), x86::byte_ptr(tmp3, (int32_t) null0_off));
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp3, (int32_t) null1_off));

				/* If null flags differ → one_null */
				cc.cmp(tmp1, tmp2);
				cc.jne(one_null);

				/* Null flags equal. If both null (tmp1 != 0) → both_null */
				x86_cbnz(cc, tmp1, both_null);

				/* Neither null: call equality function */
				cc.mov(x86::byte_ptr(tmp3,
					offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
				{
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.func.fn_addr);
					x86::Gp fcinfo_reg = cc.new_gp64();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}

				/* For DISTINCT: invert result */
				if (opcode == EEOP_DISTINCT)
				{
					cc.test(tmp1, tmp1);
					x86_cset(cc, tmp1, x86::CondCode::kEqual);
				}
				/* Store result, resnull=false */
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), Imm(0));
				cc.jmp(done);

				/* one_null: nulls differ → DISTINCT=true, NOT_DISTINCT=false */
				cc.bind(one_null);
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2),
					Imm((opcode == EEOP_DISTINCT) ? 1 : 0));
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), Imm(0));
				cc.jmp(done);

				/* both_null: DISTINCT=false, NOT_DISTINCT=true */
				cc.bind(both_null);
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2),
					Imm((opcode == EEOP_DISTINCT) ? 0 : 1));
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), Imm(0));

				cc.bind(done);
				break;
			}

			/*
			 * ---- NULLIF ----
			 * If a null: resnull=true. If b null: return a.
			 * Else call equality; if equal return null, else return a.
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

				/* Load fcinfo base */
				cc.mov(tmp3, (uint64_t) fcinfo);

				/* Check if arg0 is null */
				cc.movzx(tmp1.r32(), x86::byte_ptr(tmp3, (int32_t) null0_off));
				x86_cbnz(cc, tmp1, a_null);

				/* Check if arg1 is null → return a */
				cc.movzx(tmp1.r32(), x86::byte_ptr(tmp3, (int32_t) null1_off));
				x86_cbnz(cc, tmp1, b_null);

				/* Both non-null: call equality function */
				cc.mov(x86::byte_ptr(tmp3,
					offsetof(FunctionCallInfoBaseData, isnull)), Imm(0));
				{
					x86::Gp fn_reg = cc.new_gp64();
					cc.mov(fn_reg, (uint64_t)(void *) op->d.func.fn_addr);
					x86::Gp fcinfo_reg = cc.new_gp64();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo);
					InvokeNode *invoke;
					cc.invoke(Out(invoke), fn_reg,
							  FuncSignature::build<int64_t, void *>());
					invoke->set_arg(0, fcinfo_reg);
					invoke->set_ret(0, tmp1);
				}

				/* If fcinfo->isnull, treat as not equal */
				cc.mov(tmp3, (uint64_t) fcinfo);
				cc.movzx(tmp2.r32(), x86::byte_ptr(tmp3,
					offsetof(FunctionCallInfoBaseData, isnull)));
				x86_cbnz(cc, tmp2, not_equal);

				/* If result == false (0), not equal */
				x86_cbz(cc, tmp1, not_equal);

				/* Equal: return null */
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), Imm(1));
				cc.jmp(done);

				/* not_equal / b_null: return a's value */
				cc.bind(not_equal);
				cc.bind(b_null);
				cc.mov(tmp3, (uint64_t) fcinfo);
				cc.mov(tmp1, x86::qword_ptr(tmp3, (int32_t) val0_off));
				cc.mov(tmp2, (uint64_t) op->resvalue);
				cc.mov(x86::qword_ptr(tmp2), tmp1);
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), Imm(0));
				cc.jmp(done);

				/* a_null: resnull = true */
				cc.bind(a_null);
				cc.mov(tmp2, (uint64_t) op->resnull);
				cc.mov(x86::byte_ptr(tmp2), Imm(1));

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

				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t) fn);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
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

				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t) fn);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
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
				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) op->d.cparam.paramfunc);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/* SYSVAR: ExecEvalSysVar(state, op, econtext, slot) */
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

				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) ExecEvalSysVar);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
				/* slot = econtext->ecxt_*tuple */
				x86::Gp slot_reg = cc.new_gp64();
				cc.mov(slot_reg, x86::ptr(v_econtext, (int32_t) slot_offset));
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_arg(3, slot_reg);
				break;
			}

			/* SBSREF_OLD / SBSREF_ASSIGN / SBSREF_FETCH: indirect 3-arg void */
			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
			{
				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) op->d.sbsref.subscriptfunc);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
				InvokeNode *invoke;
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<void, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				break;
			}

			/* SBSREF_SUBSCRIPTS: indirect 3-arg returning bool + conditional jump */
			case EEOP_SBSREF_SUBSCRIPTS:
			{
				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) op->d.sbsref_subscript.subscriptfunc);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
				InvokeNode *invoke;
				x86::Gp ret_reg = cc.new_gp64();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_ret(0, ret_reg);
				/* If false (0), jump to jumpdone */
				int jumpdone = op->d.sbsref_subscript.jumpdone;
				cc.test(ret_reg, ret_reg);
				cc.je(step_labels[jumpdone]);
				break;
			}

#ifdef HAVE_EEOP_JSONEXPR
			/* JSONEXPR_PATH: 3-arg returning int + multi-target jumps */
			case EEOP_JSONEXPR_PATH:
			{
				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) ExecEvalJsonExprPath);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
				InvokeNode *invoke;
				x86::Gp ret_reg = cc.new_gp64();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *, void *, void *>());
				invoke->set_arg(0, v_state);
				invoke->set_arg(1, op_reg);
				invoke->set_arg(2, v_econtext);
				invoke->set_ret(0, ret_reg);

				/* Multi-target jump resolution */
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
						cc.je(step_labels[targets[t]]);
					}
				}
				break;
			}
#endif

			/* IOCOERCE: two function calls — output fn -> cstring -> input fn */
			case EEOP_IOCOERCE:
			{
				FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

				/* if (*op->resnull) skip */
				x86::Gp resnull_reg = cc.new_gp64();
				cc.movzx(resnull_reg, x86::byte_ptr((uint64_t) op->resnull));
				Label skip_null = cc.new_label();
				cc.test(resnull_reg, resnull_reg);
				cc.jne(skip_null);

				/* Setup output function call */
				/* fcinfo_out->args[0].value = *op->resvalue */
				x86::Gp resval_reg = cc.new_gp64();
				cc.mov(resval_reg, x86::ptr((uint64_t) op->resvalue));
				cc.mov(x86::ptr((uint64_t) &fcinfo_out->args[0].value), resval_reg);
				/* fcinfo_out->args[0].isnull = false */
				cc.mov(x86::byte_ptr((uint64_t) &fcinfo_out->args[0].isnull), 0);
				/* fcinfo_out->isnull = false */
				cc.mov(x86::byte_ptr((uint64_t) &fcinfo_out->isnull), 0);

				/* R0 = fn_addr_out(fcinfo_out) */
				{
					x86::Gp fn_out = cc.new_gp64();
					cc.mov(fn_out, (uint64_t)(void *) fcinfo_out->flinfo->fn_addr);
					x86::Gp fcinfo_reg = cc.new_gp64();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo_out);
					InvokeNode *invoke_out;
					x86::Gp ret_out = cc.new_gp64();
					cc.invoke(Out(invoke_out), fn_out,
							  FuncSignature::build<int64_t, void *>());
					invoke_out->set_arg(0, fcinfo_reg);
					invoke_out->set_ret(0, ret_out);

					/* Setup input function call */
					/* fcinfo_in->args[0].value = ret_out */
					cc.mov(x86::ptr((uint64_t) &fcinfo_in->args[0].value), ret_out);
				}
				/* fcinfo_in->args[0].isnull = false */
				cc.mov(x86::byte_ptr((uint64_t) &fcinfo_in->args[0].isnull), 0);
				/* fcinfo_in->isnull = false */
				cc.mov(x86::byte_ptr((uint64_t) &fcinfo_in->isnull), 0);

				/* R0 = fn_addr_in(fcinfo_in) */
				{
					x86::Gp fn_in = cc.new_gp64();
					cc.mov(fn_in, (uint64_t)(void *) fcinfo_in->flinfo->fn_addr);
					x86::Gp fcinfo_reg = cc.new_gp64();
					cc.mov(fcinfo_reg, (uint64_t) fcinfo_in);
					InvokeNode *invoke_in;
					x86::Gp ret_in = cc.new_gp64();
					cc.invoke(Out(invoke_in), fn_in,
							  FuncSignature::build<int64_t, void *>());
					invoke_in->set_arg(0, fcinfo_reg);
					invoke_in->set_ret(0, ret_in);

					/* *op->resvalue = ret_in */
					cc.mov(x86::ptr((uint64_t) op->resvalue), ret_in);
				}
				/* *op->resnull = fcinfo_in->isnull */
				{
					x86::Gp isnull_reg = cc.new_gp64();
					cc.movzx(isnull_reg, x86::byte_ptr((uint64_t) &fcinfo_in->isnull));
					cc.mov(x86::byte_ptr((uint64_t) op->resnull), isnull_reg.r8());
				}

				cc.bind(skip_null);
				break;
			}

			/* ROWCOMPARE_STEP: inline with null checks + dual jumps */
			case EEOP_ROWCOMPARE_STEP:
			{
				FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
				int jnull = op->d.rowcompare_step.jumpnull;
				int jdone = op->d.rowcompare_step.jumpdone;

				/* fn_strict: check args[0].isnull || args[1].isnull */
				if (op->d.rowcompare_step.finfo->fn_strict)
				{
					x86::Gp a0null = cc.new_gp64();
					cc.movzx(a0null, x86::byte_ptr((uint64_t) &fcinfo->args[0].isnull));
					x86::Gp a1null = cc.new_gp64();
					cc.movzx(a1null, x86::byte_ptr((uint64_t) &fcinfo->args[1].isnull));
					cc.or_(a0null, a1null);

					Label not_null = cc.new_label();
					cc.je(not_null);  /* ZF set by or_ if both 0 */

					/* Null path: *resnull = true, jump to jumpnull */
					cc.mov(x86::byte_ptr((uint64_t) op->resnull), 1);
					cc.jmp(step_labels[jnull]);

					cc.bind(not_null);
				}

				/* fcinfo->isnull = false */
				cc.mov(x86::byte_ptr((uint64_t) &fcinfo->isnull), 0);

				/* R0 = fn_addr(fcinfo) */
				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) op->d.rowcompare_step.fn_addr);
				x86::Gp fcinfo_reg = cc.new_gp64();
				cc.mov(fcinfo_reg, (uint64_t) fcinfo);
				InvokeNode *invoke;
				x86::Gp ret_reg = cc.new_gp64();
				cc.invoke(Out(invoke), fn_reg,
						  FuncSignature::build<int64_t, void *>());
				invoke->set_arg(0, fcinfo_reg);
				invoke->set_ret(0, ret_reg);

				/* *op->resvalue = ret_reg */
				cc.mov(x86::ptr((uint64_t) op->resvalue), ret_reg);

				/* If fcinfo->isnull, *resnull = true, jump to jumpnull */
				{
					x86::Gp isnull_reg = cc.new_gp64();
					cc.movzx(isnull_reg, x86::byte_ptr((uint64_t) &fcinfo->isnull));
					cc.test(isnull_reg, isnull_reg);
					Label not_null2 = cc.new_label();
					cc.je(not_null2);
					cc.mov(x86::byte_ptr((uint64_t) op->resnull), 1);
					cc.jmp(step_labels[jnull]);
					cc.bind(not_null2);
				}

				/* *resnull = false */
				cc.mov(x86::byte_ptr((uint64_t) op->resnull), 0);

				/* If DatumGetInt32(ret_reg) != 0, jump to jumpdone */
				cc.test(ret_reg.r32(), ret_reg.r32());
				cc.jne(step_labels[jdone]);
				break;
			}

			/* ROWCOMPARE_FINAL: inline comparison dispatch */
			case EEOP_ROWCOMPARE_FINAL:
			{
				CompareType cmptype = op->d.rowcompare_final.cmptype;

				/* Load int32 cmpresult from *op->resvalue */
				x86::Gp cmpres = cc.new_gp32();
				cc.mov(cmpres, x86::dword_ptr((uint64_t) op->resvalue));

				/* Compare against 0 */
				cc.test(cmpres, cmpres);

				/* Set result based on comparison type */
				x86::Gp result = cc.new_gp64();
				switch (cmptype)
				{
					case COMPARE_LT:
						cc.setl(result.r8()); break;
					case COMPARE_LE:
						cc.setle(result.r8()); break;
					case COMPARE_GE:
						cc.setge(result.r8()); break;
					case COMPARE_GT:
						cc.setg(result.r8()); break;
					default:
						cc.setl(result.r8()); break;
				}
				cc.movzx(result, result.r8());

				/* *op->resvalue = BoolGetDatum(result) */
				cc.mov(x86::ptr((uint64_t) op->resvalue), result);
				/* *op->resnull = false */
				cc.mov(x86::byte_ptr((uint64_t) op->resnull), 0);
				break;
			}

#ifdef HAVE_EEOP_RETURNINGEXPR
			/* RETURNINGEXPR: flag test + conditional jump */
			case EEOP_RETURNINGEXPR:
			{
				/* Load state->flags (int32) */
				x86::Gp flags_reg = cc.new_gp32();
				cc.mov(flags_reg, x86::dword_ptr(v_state, (int32_t) offsetof(ExprState, flags)));

				/* Test against nullflag */
				cc.test(flags_reg, (int32_t) op->d.returningexpr.nullflag);
				Label cont = cc.new_label();
				cc.je(cont);

				/* Flag set: *resvalue = 0, *resnull = true, jump to jumpdone */
				cc.mov(x86::ptr((uint64_t) op->resvalue), 0);
				cc.mov(x86::byte_ptr((uint64_t) op->resnull), 1);
				cc.jmp(step_labels[op->d.returningexpr.jumpdone]);

				cc.bind(cont);
				break;
			}
#endif

			default:
			{
				/* Fallback: call pg_jitter_fallback_step -> int */
				x86::Gp fn_reg = cc.new_gp64();
				cc.mov(fn_reg, (uint64_t)(void *) pg_jitter_fallback_step);
				x86::Gp op_reg = cc.new_gp64();
				cc.mov(op_reg, (uint64_t) op);
				InvokeNode *invoke;
				x86::Gp ret_reg = cc.new_gp64();
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
					cc.test(ret_reg, ret_reg);
					cc.jns(step_labels[fb_jump_target]);
				}
				break;
			}
		}
	}

	cc.end_func();

	Error err = cc.finalize();
	if (err != kErrorOk)
	{
		pfree(step_labels);
		return false;
	}

	pfree(step_labels);
	return true;
}
