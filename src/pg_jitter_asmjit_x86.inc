/*
 * pg_jitter_asmjit_x86.inc — x86_64 code generation for AsmJIT backend
 *
 * Included by pg_jitter_asmjit.cpp when compiling for x86_64.
 * Defines asmjit_emit_all() and its helper asmjit_emit_deform_inline().
 *
 * Mechanical translation of pg_jitter_asmjit_arm64.inc.
 */

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
        int alignto = att->attalignby;

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
        if (att->attnullability != ATTNULLABLE_VALID)
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

			case EEOP_DONE_NO_RETURN:
			{
				cc.xor_(tmp1.r32(), tmp1.r32());
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
						INSTR_TIME_ACCUM_DIFF(ctx->base.instr.deform_counter,
						                      deform_end, deform_start);
					}
				}

				if (!deform_emitted)
				{
					/* Fallback: call slot_getsomeattrs_int(slot, last_var) */
					x86::Gp fn_reg = cc.new_gp64();
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
			case EEOP_ASSIGN_OLD_VAR:
			case EEOP_ASSIGN_NEW_VAR:
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
				break;
			}

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
						cc.je(step_labels[jnull]);
					}
					if (jdone >= 0 && jdone < steps_len)
					{
						cc.cmp(ret_reg, jdone);
						cc.je(step_labels[jdone]);
					}
				}
				else if (opcode == EEOP_JSONEXPR_PATH)
				{
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
				}
				else if (fb_jump_target >= 0 && fb_jump_target < steps_len)
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
