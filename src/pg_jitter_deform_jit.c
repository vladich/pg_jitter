/*
 * pg_jitter_deform_jit.c — sljit-based tuple deform JIT compilation
 *
 * Contains standalone deform functions (unrolled and loop-based) and a
 * dispatch cache.  Compiled into ALL pg_jitter backends so that every
 * backend gets JIT-compiled deform, regardless of which expression engine
 * it uses.  The sljit dependency is lightweight (one C file, no externals).
 *
 * Public API:
 *   pg_jitter_compile_deform()      — unrolled deform for narrow tables
 *   pg_jitter_compile_deform_loop() — loop-based deform for wide tables
 *   pg_jitter_compiled_deform_dispatch() — per-process dispatch + cache
 */
#include "postgres.h"
#include "executor/tuptable.h"
#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "utils/expandeddatum.h"
#include "utils/memutils.h"

#include "pg_jitter_common.h"
#include "sljitLir.h"

#include <sys/mman.h>

#ifdef __linux__
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#endif

/* ================================================================
 * sljit_compile_deform — unrolled per-column deform
 *
 * Generates void deform_fn(TupleTableSlot *slot) with one code block
 * per column.  Efficient for narrow tables (< deform_threshold cols).
 *
 * Register allocation:
 *   S0 = slot, S1 = tts_values, S2 = tts_isnull,
 *   S3 = tupdata_base, S4 = t_bits
 *   R0-R3 = scratch
 * ================================================================ */

/* Stack layout (separate from expression function) */
#define DOFF_DEFORM_OFF       0		/* current byte offset */
#define DOFF_DEFORM_HASNULLS  8		/* hasnulls flag */
#define DOFF_DEFORM_MAXATT    16	/* maxatt from tuple */
#define DOFF_TOTAL            24

void *
pg_jitter_compile_deform(TupleDesc desc,
						 const TupleTableSlotOps *ops,
						 int natts)
{
	struct sljit_compiler *C;
	int		attnum;
	int		known_alignment = 0;
	bool	attguaranteedalign = true;
	int		guaranteed_column_number = -1;
	sljit_sw tuple_off;
	sljit_sw slot_off;
	void   *code;

	/* Forward-jump arrays */
	struct sljit_jump **nvalid_jumps;
	struct sljit_jump **avail_jumps;
	struct sljit_jump **null_jumps;
	struct sljit_label **att_labels;
	struct sljit_jump  *nvalid_default;

	/* --- Guards --- */
	if (ops == &TTSOpsVirtual)
		return NULL;
	if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple &&
		ops != &TTSOpsMinimalTuple)
		return NULL;
	if (natts <= 0 || natts > desc->natts)
		return NULL;

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

	C = sljit_create_compiler(NULL);
	if (!C)
		return NULL;

	/* Allocate forward-jump tracking arrays */
	nvalid_jumps = palloc0(sizeof(struct sljit_jump *) * natts);
	avail_jumps = palloc0(sizeof(struct sljit_jump *) * natts);
	null_jumps = palloc0(sizeof(struct sljit_jump *) * natts);
	att_labels = palloc0(sizeof(struct sljit_label *) * natts);

	/*
	 * Function prologue: void deform_fn(TupleTableSlot *slot)
	 * S0 = slot (input arg)
	 * S1 = tts_values, S2 = tts_isnull (loaded at entry)
	 * S3 = tupdata_base, S4 = t_bits
	 * 4 scratch regs (R0-R3), 5 saved regs (S0-S4)
	 */
	sljit_emit_enter(C, 0,
					 SLJIT_ARGS1V(P),
					 4, 5, DOFF_TOTAL);

	/* S1 = slot->tts_values */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_values));
	/* S2 = slot->tts_isnull */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_S2, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_isnull));

	/* R0 = HeapTuple ptr from slot-type-specific offset */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0), tuple_off);

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
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_HASNULLS,
				   SLJIT_R2, 0);

	/* t_infomask2 -> maxatt = infomask2 & HEAP_NATTS_MASK */
	sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
				   SLJIT_MEM1(SLJIT_R1),
				   offsetof(HeapTupleHeaderData, t_infomask2));
	sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
				   SLJIT_R2, 0, SLJIT_IMM, HEAP_NATTS_MASK);
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_MAXATT,
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

	/* Load saved offset from slot->off -> [SP+DOFF_DEFORM_OFF] */
	sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0), slot_off);
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
				   SLJIT_R0, 0);

	/* ============================================================
	 * MISSING ATTRIBUTES CHECK
	 * ============================================================ */
	if ((natts - 1) > guaranteed_column_number)
	{
		struct sljit_jump *skip_missing;

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_MAXATT);
		skip_missing = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
									  SLJIT_R0, 0,
									  SLJIT_IMM, natts);

		/* call slot_getmissingattrs(slot, maxatt_as_int, natts) */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
					   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_MAXATT);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_IMM, natts);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, 32, 32),
						 SLJIT_IMM, (sljit_sw) slot_getmissingattrs);

		sljit_set_label(skip_missing, sljit_emit_label(C));
	}

	/* ============================================================
	 * NVALID DISPATCH: comparison chain
	 * ============================================================ */
	sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0),
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
		int		alignto = JITTER_ATTALIGNBY(att);

		/* ---- Emit attcheck label and wire up nvalid dispatch ---- */
		att_labels[attnum] = sljit_emit_label(C);
		sljit_set_label(nvalid_jumps[attnum], att_labels[attnum]);

		/* Patch previous null-path forward jump if it targeted this label */
		if (attnum > 0 && null_jumps[attnum - 1] != NULL)
			sljit_set_label(null_jumps[attnum - 1], att_labels[attnum]);

		/* If attnum == 0: reset offset to 0 */
		if (attnum == 0)
		{
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
						   SLJIT_IMM, 0);
		}

		/* ---- Availability check ---- */
		if (attnum > guaranteed_column_number)
		{
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_MAXATT);
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
						   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_HASNULLS);
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
			/* tts_values[attnum] = 0 */
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM1(SLJIT_S1),
						   attnum * (sljit_sw) sizeof(Datum),
						   SLJIT_IMM, 0);
			/* tts_isnull[attnum] = true */
			sljit_emit_op1(C, SLJIT_MOV_U8,
						   SLJIT_MEM1(SLJIT_S2), attnum,
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
							   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R1, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_R0), 0);
				is_short = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										  SLJIT_R1, 0,
										  SLJIT_IMM, 0);

				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, alignto - 1);
				sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, ~((sljit_sw)(alignto - 1)));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
							   SLJIT_R0, 0);

				sljit_set_label(is_short, sljit_emit_label(C));
			}
			else
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, alignto - 1);
				sljit_emit_op2(C, SLJIT_AND, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, ~((sljit_sw)(alignto - 1)));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
							   SLJIT_R0, 0);
			}

			if (known_alignment >= 0)
				known_alignment = TYPEALIGN(alignto, known_alignment);
		}

		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
						   SLJIT_IMM, known_alignment);
		}

		/* ---- Value extraction ---- */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
					   SLJIT_S3, 0, SLJIT_R0, 0);

		/* tts_isnull[attnum] = false */
		sljit_emit_op1(C, SLJIT_MOV_U8,
					   SLJIT_MEM1(SLJIT_S2), attnum,
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
					sljit_free_compiler(C);
					pfree(nvalid_jumps); pfree(avail_jumps);
					pfree(null_jumps); pfree(att_labels);
					return NULL;
			}
			/* R3 = *(mov_op *)(tupdata_base + off) */
			sljit_emit_op1(C, mov_op, SLJIT_R3, 0,
						   SLJIT_MEM1(SLJIT_R1), 0);
			/* tts_values[attnum] = R3 */
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM1(SLJIT_S1),
						   attnum * (sljit_sw) sizeof(Datum),
						   SLJIT_R3, 0);
		}
		else
		{
			/* tts_values[attnum] = pointer to data */
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM1(SLJIT_S1),
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
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
							   SLJIT_IMM, known_alignment);
			}
			else
			{
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, att->attlen);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
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
						   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
						   SLJIT_R1, 0, SLJIT_R0, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
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
						   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
						   SLJIT_R1, 0, SLJIT_R0, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF,
						   SLJIT_R1, 0);
		}
	}

	/* ============================================================
	 * EPILOGUE: patch jumps, store tts_nvalid, off, flags, return
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

		/* tts_nvalid = natts (int16 store) */
		sljit_emit_op1(C, SLJIT_MOV_S16,
					   SLJIT_MEM1(SLJIT_S0),
					   offsetof(TupleTableSlot, tts_nvalid),
					   SLJIT_IMM, natts);

		/* slot->off = (uint32) off */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
					   SLJIT_MEM1(SLJIT_SP), DOFF_DEFORM_OFF);
		sljit_emit_op1(C, SLJIT_MOV_U32,
					   SLJIT_MEM1(SLJIT_S0), slot_off,
					   SLJIT_R1, 0);

		/* tts_flags |= TTS_FLAG_SLOW */
		sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R1, 0,
					   SLJIT_MEM1(SLJIT_S0),
					   offsetof(TupleTableSlot, tts_flags));
		sljit_emit_op2(C, SLJIT_OR, SLJIT_R1, 0,
					   SLJIT_R1, 0, SLJIT_IMM, TTS_FLAG_SLOW);
		sljit_emit_op1(C, SLJIT_MOV_U16,
					   SLJIT_MEM1(SLJIT_S0),
					   offsetof(TupleTableSlot, tts_flags),
					   SLJIT_R1, 0);
	}

	/* Return void */
	sljit_emit_return_void(C);

	/* Generate native code */
	code = sljit_generate_code(C, 0, NULL);
	sljit_free_compiler(C);

	pfree(nvalid_jumps);
	pfree(avail_jumps);
	pfree(null_jumps);
	pfree(att_labels);

	return code;
}


/* ================================================================
 * pg_jitter_compile_deform_loop — loop-based deform for wide tables
 *
 * Instead of unrolling per-column code (~140 bytes/col), emits a
 * compact loop that iterates over a DeformColDesc descriptor array.
 *
 * Compile-time specialization based on descriptor pre-scan:
 *   - all_fixed_byval_notnull: tight loop with zero branches
 *   - all_notnull + uniform byval: hasnulls-gated fast path
 *   - general: full null/alignment/byval/varlena handling
 *
 * Register allocation (offset in register, not stack):
 *   S0 = slot (input arg)
 *   S1 = tts_values
 *   S2 = tts_isnull
 *   S3 = tupdata_base
 *   S4 = byte offset into tuple data (kept in register!)
 *   R0-R3 = scratch
 * ================================================================ */

/* Stack layout for loop-based deform */
#define DLOFF_HASNULLS   0
#define DLOFF_MAXATT     8
#define DLOFF_TBITS      16
#define DLOFF_ATTNUM     24
#define DLOFF_DESCPTR    32
#define DLOFF_TOTAL      40

void *
pg_jitter_compile_deform_loop(TupleDesc desc,
							  const TupleTableSlotOps *ops,
							  int natts,
							  DeformColDesc *target_descs,
							  Size *code_size_out,
							  struct sljit_generate_code_buffer *gen_buf)
{
	struct sljit_compiler *C;
	void   *code;
	sljit_sw tuple_off;
	sljit_sw slot_off;
	int		guaranteed_column_number = -1;
	DeformColDesc *descriptors;
	bool	all_notnull = true;
	bool	all_fixed_byval = true;
	bool	uniform_attlen = true;
	int16	first_attlen = 0;

	/* --- Guards --- */
	if (ops == &TTSOpsVirtual)
		return NULL;
	if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple &&
		ops != &TTSOpsMinimalTuple)
		return NULL;
	if (natts <= 0 || natts > desc->natts)
		return NULL;

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

	/* Build descriptor array */
	if (target_descs)
	{
		/* Caller provided pre-allocated descriptor area (shared deform) */
		descriptors = target_descs;
	}
	else
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
		descriptors = palloc(sizeof(DeformColDesc) * natts);
		MemoryContextSwitchTo(old);
	}

	first_attlen = TupleDescCompactAttr(desc, 0)->attlen;

	for (int attnum = 0; attnum < natts; attnum++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, attnum);

		descriptors[attnum].attlen   = att->attlen;
		descriptors[attnum].attalign = (int8) JITTER_ATTALIGNBY(att);
		descriptors[attnum].attbyval = att->attbyval ? 1 : 0;
		descriptors[attnum].attnotnull = JITTER_ATT_IS_NOTNULL(att) ? 1 : 0;
		memset(descriptors[attnum].pad, 0, sizeof(descriptors[attnum].pad));

		if (!JITTER_ATT_IS_NOTNULL(att))
			all_notnull = false;
		if (!att->attbyval || att->attlen <= 0)
			all_fixed_byval = false;
		if (att->attlen != first_attlen)
			uniform_attlen = false;

		if (JITTER_ATT_IS_NOTNULL(att) &&
			!att->atthasmissing &&
			!att->attisdropped)
			guaranteed_column_number = attnum;
	}

	C = sljit_create_compiler(NULL);
	if (!C)
		return NULL;

	/*
	 * PROLOGUE — common to all specializations.
	 */
	sljit_emit_enter(C, 0,
					 SLJIT_ARGS1V(P),
					 4, 5, DLOFF_TOTAL);

	/* S1 = slot->tts_values */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_S1, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_values));
	/* S2 = slot->tts_isnull */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_S2, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_isnull));

	/* R0 = HeapTuple ptr */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0), tuple_off);
	/* R1 = heaptuple->t_data */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
				   SLJIT_MEM1(SLJIT_R0),
				   offsetof(HeapTupleData, t_data));

	/* hasnulls = t_infomask & HEAP_HASNULL -> stack */
	sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
				   SLJIT_MEM1(SLJIT_R1),
				   offsetof(HeapTupleHeaderData, t_infomask));
	sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
				   SLJIT_R2, 0, SLJIT_IMM, HEAP_HASNULL);
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS,
				   SLJIT_R2, 0);

	/* maxatt = t_infomask2 & HEAP_NATTS_MASK -> stack */
	sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R2, 0,
				   SLJIT_MEM1(SLJIT_R1),
				   offsetof(HeapTupleHeaderData, t_infomask2));
	sljit_emit_op2(C, SLJIT_AND, SLJIT_R2, 0,
				   SLJIT_R2, 0, SLJIT_IMM, HEAP_NATTS_MASK);
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT,
				   SLJIT_R2, 0);

	if (!all_notnull)
	{
		/* t_bits -> stack (only needed when nulls are possible) */
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0,
					   SLJIT_R1, 0,
					   SLJIT_IMM, offsetof(HeapTupleHeaderData, t_bits));
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS,
					   SLJIT_R2, 0);
	}

	/* S3 = tupdata_base = (char *)tuplep + t_hoff */
	sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
				   SLJIT_MEM1(SLJIT_R1),
				   offsetof(HeapTupleHeaderData, t_hoff));
	sljit_emit_op2(C, SLJIT_ADD, SLJIT_S3, 0,
				   SLJIT_R1, 0, SLJIT_R2, 0);

	/* ============================================================
	 * MISSING ATTRIBUTES CHECK
	 * ============================================================ */
	if ((natts - 1) > guaranteed_column_number)
	{
		struct sljit_jump *skip_missing;

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		skip_missing = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
									  SLJIT_R0, 0,
									  SLJIT_IMM, natts);

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_IMM, natts);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, 32, 32),
						 SLJIT_IMM, (sljit_sw) slot_getmissingattrs);

		sljit_set_label(skip_missing, sljit_emit_label(C));
	}

	/* ============================================================
	 * INIT: attnum = tts_nvalid, offset from slot
	 * ============================================================ */
	/* R0 = tts_nvalid -> stack as attnum */
	sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_nvalid));
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM,
				   SLJIT_R0, 0);

	/* S4 = offset. if attnum == 0: 0, else slot->off */
	{
		struct sljit_jump *j_nonzero;

		j_nonzero = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
								   SLJIT_R0, 0, SLJIT_IMM, 0);

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_S4, 0, SLJIT_IMM, 0);

		{
			struct sljit_jump *j_skip = sljit_emit_jump(C, SLJIT_JUMP);
			sljit_set_label(j_nonzero, sljit_emit_label(C));

			sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S4, 0,
						   SLJIT_MEM1(SLJIT_S0), slot_off);

			sljit_set_label(j_skip, sljit_emit_label(C));
		}
	}

	/* desc_ptr = &descriptors[attnum] -> stack */
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
				   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
	sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
				   SLJIT_R0, 0, SLJIT_IMM, 3);
	sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
				   SLJIT_R0, 0, SLJIT_IMM, (sljit_sw) descriptors);
	sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR,
				   SLJIT_R0, 0);

	/* ============================================================
	 * SPECIALIZATION: all-fixed-byval-notnull uniform tables
	 * ============================================================ */
	if (all_fixed_byval && all_notnull && uniform_attlen && first_attlen > 0)
	{
		sljit_s32 mov_op;
		struct sljit_label *l_fast_top;
		struct sljit_jump *j_fast_done;
		int attlen = first_attlen;

		switch (attlen)
		{
			case 1: mov_op = SLJIT_MOV_S8; break;
			case 2: mov_op = SLJIT_MOV_S16; break;
			case 4: mov_op = SLJIT_MOV_S32; break;
			default: mov_op = SLJIT_MOV; break;  /* 8 */
		}

		/* Compute loop limit: min(natts, maxatt) -> R2 */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		{
			struct sljit_jump *j_maxatt_smaller;
			j_maxatt_smaller = sljit_emit_cmp(C, SLJIT_SIG_LESS,
											  SLJIT_R2, 0, SLJIT_IMM, natts);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, natts);
			sljit_set_label(j_maxatt_smaller, sljit_emit_label(C));
		}
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT,
					   SLJIT_R2, 0);

		/* Bulk-clear tts_isnull */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
					   SLJIT_S2, 0, SLJIT_R0, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, natts);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
					   SLJIT_R2, 0, SLJIT_R3, 0);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
						 SLJIT_IMM, (sljit_sw) memset);

		/* Tight inner loop — pointer-based do-while */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
					   SLJIT_R3, 0, SLJIT_IMM, 3);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
					   SLJIT_S1, 0, SLJIT_R1, 0);

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
					   SLJIT_R2, 0, SLJIT_IMM, 3);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0,
					   SLJIT_S1, 0, SLJIT_R2, 0);

		j_fast_done = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL,
									 SLJIT_R1, 0, SLJIT_R2, 0);

		l_fast_top = sljit_emit_label(C);

		sljit_emit_op1(C, mov_op, SLJIT_R0, 0,
					   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
		sljit_emit_op1(C, SLJIT_MOV,
					   SLJIT_MEM1(SLJIT_R1), 0,
					   SLJIT_R0, 0);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
					   SLJIT_S4, 0, SLJIT_IMM, attlen);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
					   SLJIT_R1, 0, SLJIT_IMM, sizeof(Datum));

		sljit_set_label(
			sljit_emit_cmp(C, SLJIT_LESS,
						   SLJIT_R1, 0, SLJIT_R2, 0),
			l_fast_top);

		/* EPILOGUE */
		sljit_set_label(j_fast_done, sljit_emit_label(C));
	}
	else if (all_fixed_byval && uniform_attlen && first_attlen > 0)
	{
		/* ============================================================
		 * SEMI-FAST PATH: uniform fixed byval, nullable schema
		 * ============================================================ */
		sljit_s32 mov_op;
		struct sljit_label *l_semi_top;
		struct sljit_jump *j_semi_done, *j_has_nulls_general;
		int attlen = first_attlen;

		switch (attlen)
		{
			case 1: mov_op = SLJIT_MOV_S8; break;
			case 2: mov_op = SLJIT_MOV_S16; break;
			case 4: mov_op = SLJIT_MOV_S32; break;
			default: mov_op = SLJIT_MOV; break;  /* 8 */
		}

		/* Check hasnulls — if nulls present, fall to general loop */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
		j_has_nulls_general = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											  SLJIT_R0, 0, SLJIT_IMM, 0);

		/* No nulls! Bulk-clear isnull, then tight loop */

		/* Compute loop limit: min(natts, maxatt) */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		{
			struct sljit_jump *j_maxatt_smaller;
			j_maxatt_smaller = sljit_emit_cmp(C, SLJIT_SIG_LESS,
											  SLJIT_R2, 0, SLJIT_IMM, natts);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, natts);
			sljit_set_label(j_maxatt_smaller, sljit_emit_label(C));
		}
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT,
					   SLJIT_R2, 0);

		/* Bulk-clear tts_isnull */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
					   SLJIT_S2, 0, SLJIT_R0, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, natts);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
					   SLJIT_R2, 0, SLJIT_R3, 0);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
						 SLJIT_IMM, (sljit_sw) memset);

		/* Pointer-based do-while loop */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
					   SLJIT_R3, 0, SLJIT_IMM, 3);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
					   SLJIT_S1, 0, SLJIT_R1, 0);

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
					   SLJIT_R2, 0, SLJIT_IMM, 3);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R2, 0,
					   SLJIT_S1, 0, SLJIT_R2, 0);

		j_semi_done = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL,
									 SLJIT_R1, 0, SLJIT_R2, 0);

		l_semi_top = sljit_emit_label(C);

		sljit_emit_op1(C, mov_op, SLJIT_R0, 0,
					   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
		sljit_emit_op1(C, SLJIT_MOV,
					   SLJIT_MEM1(SLJIT_R1), 0,
					   SLJIT_R0, 0);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
					   SLJIT_S4, 0, SLJIT_IMM, attlen);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
					   SLJIT_R1, 0, SLJIT_IMM, sizeof(Datum));

		sljit_set_label(
			sljit_emit_cmp(C, SLJIT_LESS,
						   SLJIT_R1, 0, SLJIT_R2, 0),
			l_semi_top);

		sljit_set_label(j_semi_done, sljit_emit_label(C));

		/* Skip over the general loop if we used the fast path */
		{
			struct sljit_jump *j_skip_general = sljit_emit_jump(C, SLJIT_JUMP);

			/* GENERAL LOOP entry point when hasnulls is set */
			sljit_set_label(j_has_nulls_general, sljit_emit_label(C));

			/* General loop for the hasnulls case */
			{
				struct sljit_label *l_gn_top;
				struct sljit_jump *j_gn_done, *j_gn_avail;

				l_gn_top = sljit_emit_label(C);

				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
							   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
				j_gn_done = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
										   SLJIT_R3, 0, SLJIT_IMM, natts);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
				j_gn_avail = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
											SLJIT_R3, 0, SLJIT_R0, 0);

				/* Load desc_ptr */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
							   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);

				/* NULL CHECK: bitmap check (hasnulls is known true here) */
				{
					struct sljit_jump *j_bit_set;

					/* byte = t_bits[attnum >> 3] */
					sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0,
								   SLJIT_R3, 0, SLJIT_IMM, 3);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM2(SLJIT_R1, SLJIT_R0), 0);

					/* bit = 1 << (attnum & 7) */
					sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
								   SLJIT_R3, 0, SLJIT_IMM, 7);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 1);
					sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
								   SLJIT_R2, 0, SLJIT_R1, 0);

					sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
								   SLJIT_R0, 0, SLJIT_R2, 0);
					j_bit_set = sljit_emit_jump(C, SLJIT_NOT_ZERO);

					/* IS NULL */
					sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
								   SLJIT_R3, 0, SLJIT_IMM, 3);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM2(SLJIT_S1, SLJIT_R0), 0,
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM2(SLJIT_S2, SLJIT_R3), 0,
								   SLJIT_IMM, 1);

					/* Advance and loop */
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
								   SLJIT_R3, 0, SLJIT_IMM, 1);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM,
								   SLJIT_R3, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
								   SLJIT_R0, 0, SLJIT_IMM, (sljit_sw) sizeof(DeformColDesc));
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR,
								   SLJIT_R0, 0);
					sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_gn_top);

					sljit_set_label(j_bit_set, sljit_emit_label(C));
				}

				/* NOT NULL: tts_isnull[attnum] = false, extract int value */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM2(SLJIT_S2, SLJIT_R3), 0,
							   SLJIT_IMM, 0);

				/* Load value: since uniform attlen, use fixed mov_op */
				sljit_emit_op1(C, mov_op, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);

				/* Store: tts_values[attnum] = R0 */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
							   SLJIT_R3, 0, SLJIT_IMM, 3);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
							   SLJIT_R0, 0);

				/* Advance offset by fixed attlen */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_IMM, attlen);

				/* Advance attnum and desc_ptr */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
							   SLJIT_R3, 0, SLJIT_IMM, 1);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM,
							   SLJIT_R3, 0);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, (sljit_sw) sizeof(DeformColDesc));
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR,
							   SLJIT_R0, 0);

				sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_gn_top);

				{
					struct sljit_label *l_gn_epilogue = sljit_emit_label(C);
					sljit_set_label(j_gn_done, l_gn_epilogue);
					sljit_set_label(j_gn_avail, l_gn_epilogue);
				}
			}

			sljit_set_label(j_skip_general, sljit_emit_label(C));
		}
	}
	else
	{
		/* ============================================================
		 * GENERAL LOOP (handles nulls, varlena, mixed types)
		 * ============================================================ */
		struct sljit_label *l_loop_top;
		struct sljit_label *l_epilogue;
		struct sljit_jump *j_done, *j_avail_out;

		l_loop_top = sljit_emit_label(C);

		/* R3 = attnum (from stack) */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);

		/* if attnum >= natts -> epilogue */
		j_done = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
								SLJIT_R3, 0, SLJIT_IMM, natts);

		/* if attnum >= maxatt -> epilogue */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		j_avail_out = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
									 SLJIT_R3, 0, SLJIT_R0, 0);

		/* R2 = desc_ptr (from stack) */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);

		/* ---- NULL CHECK ---- */
		if (!all_notnull)
		{
			struct sljit_jump *j_notnull_attr, *j_no_tuple_nulls;
			struct sljit_jump *j_bit_set;
			struct sljit_label *l_not_null;

			/* if desc->attnotnull -> skip */
			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R2), offsetof(DeformColDesc, attnotnull));
			j_notnull_attr = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

			/* if !hasnulls -> skip */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
			j_no_tuple_nulls = sljit_emit_cmp(C, SLJIT_EQUAL,
											  SLJIT_R0, 0, SLJIT_IMM, 0);

			/* byte = t_bits[attnum >> 3] */
			sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0,
						   SLJIT_R3, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
			sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_R1, SLJIT_R0), 0);

			/* bit = 1 << (attnum & 7) */
			sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
						   SLJIT_R3, 0, SLJIT_IMM, 7);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_R2, 0, SLJIT_R1, 0);

			/* if (byte & bit) -> not null */
			sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
						   SLJIT_R0, 0, SLJIT_R2, 0);
			j_bit_set = sljit_emit_jump(C, SLJIT_NOT_ZERO);

			/* IS NULL: tts_values[attnum] = 0, tts_isnull[attnum] = true */
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
						   SLJIT_R3, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R0), 0,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV_U8,
						   SLJIT_MEM2(SLJIT_S2, SLJIT_R3), 0,
						   SLJIT_IMM, 1);

			/* Advance attnum and desc_ptr, then loop */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
						   SLJIT_R3, 0, SLJIT_IMM, 1);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM,
						   SLJIT_R3, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_IMM, (sljit_sw) sizeof(DeformColDesc));
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR,
						   SLJIT_R0, 0);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

			l_not_null = sljit_emit_label(C);
			sljit_set_label(j_notnull_attr, l_not_null);
			sljit_set_label(j_no_tuple_nulls, l_not_null);
			sljit_set_label(j_bit_set, l_not_null);

			/* Re-load R2 = desc_ptr (clobbered by bit-shift above) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
		}

		/* ---- ALIGNMENT ---- */
		{
			struct sljit_jump *j_no_align;

			/* R1 = attlen, R0 = attalign */
			sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_R2), offsetof(DeformColDesc, attlen));
			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R2), offsetof(DeformColDesc, attalign));

			j_no_align = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 1);

			/* Varlena short-header check */
			{
				struct sljit_jump *j_not_varlena, *j_is_short;

				j_not_varlena = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											   SLJIT_R1, 0, SLJIT_IMM, -1);

				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R3, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
				j_is_short = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R3, 0, SLJIT_IMM, 0);

				sljit_set_label(j_not_varlena, sljit_emit_label(C));

				/* TYPEALIGN: S4 = (S4 + (align-1)) & ~(align-1) */
				sljit_emit_op2(C, SLJIT_SUB, SLJIT_R3, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R3, 0);
				sljit_emit_op2(C, SLJIT_XOR, SLJIT_R3, 0,
							   SLJIT_R3, 0, SLJIT_IMM, -1);
				sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R3, 0);

				{
					struct sljit_label *l_aligned = sljit_emit_label(C);
					sljit_set_label(j_no_align, l_aligned);
					sljit_set_label(j_is_short, l_aligned);
				}
			}
		}

		/* ---- EXTRACT VALUE ---- */
		{
			struct sljit_jump *j_not_byval, *j_byval_done;

			/* Reload desc fields: R1 = attlen, R0 = attbyval */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
			sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_R2), offsetof(DeformColDesc, attlen));
			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R2), offsetof(DeformColDesc, attbyval));

			/* R3 = attnum (for indexing) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);

			/* tts_isnull[attnum] = false */
			sljit_emit_op1(C, SLJIT_MOV_U8,
						   SLJIT_MEM2(SLJIT_S2, SLJIT_R3), 0,
						   SLJIT_IMM, 0);

			j_not_byval = sljit_emit_cmp(C, SLJIT_EQUAL,
										 SLJIT_R0, 0, SLJIT_IMM, 0);

			/* BYVAL: switch on attlen (R1) */
			{
				struct sljit_jump *j_len1, *j_len2, *j_len4;
				struct sljit_jump *j_s8, *j_s1, *j_s2;
				struct sljit_label *l_store;

				j_len1 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 1);
				j_len2 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 2);
				j_len4 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 4);

				/* attlen == 8 */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
				j_s8 = sljit_emit_jump(C, SLJIT_JUMP);

				sljit_set_label(j_len1, sljit_emit_label(C));
				sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
				j_s1 = sljit_emit_jump(C, SLJIT_JUMP);

				sljit_set_label(j_len2, sljit_emit_label(C));
				sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
				j_s2 = sljit_emit_jump(C, SLJIT_JUMP);

				sljit_set_label(j_len4, sljit_emit_label(C));
				sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);

				/* Store: tts_values[attnum] = R0 */
				l_store = sljit_emit_label(C);
				sljit_set_label(j_s8, l_store);
				sljit_set_label(j_s1, l_store);
				sljit_set_label(j_s2, l_store);

				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
							   SLJIT_R3, 0, SLJIT_IMM, 3);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
							   SLJIT_R0, 0);

				j_byval_done = sljit_emit_jump(C, SLJIT_JUMP);
			}

			/* NOT BYVAL: tts_values[attnum] = base + offset */
			sljit_set_label(j_not_byval, sljit_emit_label(C));
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S3, 0, SLJIT_S4, 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_R3, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);

			sljit_set_label(j_byval_done, sljit_emit_label(C));
		}

		/* ---- ADVANCE OFFSET ---- */
		{
			struct sljit_jump *j_fixedlen, *j_varlena;
			struct sljit_label *l_next;

			/* R2 = desc_ptr, R1 = attlen */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
			sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_R2), offsetof(DeformColDesc, attlen));

			j_fixedlen = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
										SLJIT_R1, 0, SLJIT_IMM, 0);
			j_varlena = sljit_emit_cmp(C, SLJIT_EQUAL,
									   SLJIT_R1, 0, SLJIT_IMM, -1);

			/* CSTRING: S4 += strlen(base + S4) + 1 */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S3, 0, SLJIT_S4, 0);
			sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
							 SLJIT_IMM, (sljit_sw) strlen);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_R0, 0);
			{
				struct sljit_jump *j_cstr_done = sljit_emit_jump(C, SLJIT_JUMP);

				/* FIXED: S4 += attlen */
				sljit_set_label(j_fixedlen, sljit_emit_label(C));
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R1, 0);
				{
					struct sljit_jump *j_fixed_done = sljit_emit_jump(C, SLJIT_JUMP);

					/* VARLENA: S4 += varsize_any(base + S4) */
					sljit_set_label(j_varlena, sljit_emit_label(C));
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
								   SLJIT_S3, 0, SLJIT_S4, 0);
					sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
									 SLJIT_IMM, (sljit_sw) varsize_any);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
								   SLJIT_S4, 0, SLJIT_R0, 0);

					l_next = sljit_emit_label(C);
					sljit_set_label(j_cstr_done, l_next);
					sljit_set_label(j_fixed_done, l_next);
				}
			}
		}

		/* Advance attnum and desc_ptr, then loop */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
					   SLJIT_R0, 0, SLJIT_IMM, 1);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_ATTNUM,
					   SLJIT_R0, 0);

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
					   SLJIT_R0, 0, SLJIT_IMM, (sljit_sw) sizeof(DeformColDesc));
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_DESCPTR,
					   SLJIT_R0, 0);

		sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

		l_epilogue = sljit_emit_label(C);
		sljit_set_label(j_done, l_epilogue);
		sljit_set_label(j_avail_out, l_epilogue);
	}

	/* ============================================================
	 * EPILOGUE (shared by all paths)
	 * ============================================================ */

	/* tts_nvalid = natts */
	sljit_emit_op1(C, SLJIT_MOV_S16,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_nvalid),
				   SLJIT_IMM, natts);

	/* slot->off = S4 (offset) */
	sljit_emit_op1(C, SLJIT_MOV_U32,
				   SLJIT_MEM1(SLJIT_S0), slot_off,
				   SLJIT_S4, 0);

	/* tts_flags |= TTS_FLAG_SLOW */
	sljit_emit_op1(C, SLJIT_MOV_U16, SLJIT_R1, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_flags));
	sljit_emit_op2(C, SLJIT_OR, SLJIT_R1, 0,
				   SLJIT_R1, 0, SLJIT_IMM, TTS_FLAG_SLOW);
	sljit_emit_op1(C, SLJIT_MOV_U16,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_flags),
				   SLJIT_R1, 0);

	sljit_emit_return_void(C);

	/* Generate native code */
	if (gen_buf)
	{
		/* Generate into caller-provided buffer (for shared deform) */
		code = sljit_generate_code(C, SLJIT_GENERATE_CODE_BUFFER, gen_buf);
	}
	else
	{
		code = sljit_generate_code(C, 0, NULL);
	}

	if (code && code_size_out)
		*code_size_out = sljit_get_generated_code_size(C);

	sljit_free_compiler(C);

	return code;
}


/* ================================================================
 * pg_jitter_compiled_deform_dispatch — dylib-resident deform wrapper
 *
 * Called from JIT-compiled expression code.  Being a dylib function,
 * its address is within +-512KB of pg_jitter_fallback_step and
 * therefore properly relocated between leader and worker processes.
 *
 * On first call for a given (table-type, natts, slot-ops) combination,
 * JIT-compiles the deform function (loop-based for wide tables, unrolled
 * for narrow) and caches it for subsequent calls within the same backend
 * process.
 * ================================================================ */

#define DEFORM_DISPATCH_CACHE_SIZE 32

typedef struct DeformDispatchEntry
{
	Oid			tdtypeid;
	int32		tdtypmod;
	int			natts;
	const TupleTableSlotOps *ops;
	void	   *fn;
} DeformDispatchEntry;

static DeformDispatchEntry deform_dispatch_cache[DEFORM_DISPATCH_CACHE_SIZE];
static int n_deform_dispatch = 0;

/* ================================================================
 * Shared deform state — set by leader (compile) or worker (attach),
 * used as fast-path in dispatch.
 * ================================================================ */
static void *shared_deform_fn = NULL;
static int   shared_deform_natts = 0;
static void *shared_deform_page = NULL;		/* for munmap on cleanup */
static Size  shared_deform_page_size = 0;

void
pg_jitter_compiled_deform_dispatch(TupleTableSlot *slot, int natts)
{
	TupleDesc desc = slot->tts_tupleDescriptor;
	const TupleTableSlotOps *ops = slot->tts_ops;
	void *fn = NULL;
	int i;

	/* Shared deform fast path: same VA across all parallel workers */
	if (shared_deform_fn && natts == shared_deform_natts)
	{
		((void (*)(TupleTableSlot *)) shared_deform_fn)(slot);
		return;
	}

	/* Fast cache lookup */
	for (i = 0; i < n_deform_dispatch; i++)
	{
		DeformDispatchEntry *e = &deform_dispatch_cache[i];
		if (e->natts == natts && e->ops == ops &&
			e->tdtypeid == desc->tdtypeid && e->tdtypmod == desc->tdtypmod)
		{
			((void (*)(TupleTableSlot *)) e->fn)(slot);
			return;
		}
	}

	/* Compile: loop for wide tables, unrolled for narrow */
	if (natts > pg_jitter_deform_threshold())
		fn = pg_jitter_compile_deform_loop(desc, ops, natts, NULL, NULL, NULL);
	else
		fn = pg_jitter_compile_deform(desc, ops, natts);

	if (fn)
	{
		/* Cache for future calls (code persists for backend lifetime) */
		if (n_deform_dispatch < DEFORM_DISPATCH_CACHE_SIZE)
		{
			DeformDispatchEntry *e = &deform_dispatch_cache[n_deform_dispatch++];
			e->tdtypeid = desc->tdtypeid;
			e->tdtypmod = desc->tdtypmod;
			e->natts = natts;
			e->ops = ops;
			e->fn = fn;
		}
		((void (*)(TupleTableSlot *)) fn)(slot);
	}
	else
	{
		/* Final fallback to interpreter */
		slot_getsomeattrs_int(slot, natts);
	}
}


/* ================================================================
 * Platform helpers for shared deform memory management.
 *
 * shared_deform_alloc_fixed:  Allocate RW memory at an exact VA.
 *   Linux:  MAP_FIXED_NOREPLACE — fails safely if VA is occupied.
 *   macOS:  vm_allocate(VM_FLAGS_FIXED) — same semantics via Mach VM.
 *
 * shared_deform_free:  Release memory from either alloc path.
 * ================================================================ */

static void *
shared_deform_alloc_fixed(void *target_addr, Size size)
{
#ifdef __linux__
	void *page = mmap(target_addr, size,
					  PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE,
					  -1, 0);

	if (page == MAP_FAILED || page != target_addr)
	{
		if (page != MAP_FAILED && page != target_addr)
			munmap(page, size);
		return NULL;
	}
	return page;

#elif defined(__APPLE__)
	vm_address_t addr = (vm_address_t) target_addr;
	kern_return_t kr;

	kr = vm_allocate(mach_task_self(), &addr, (vm_size_t) size, VM_FLAGS_FIXED);
	if (kr != KERN_SUCCESS)
		return NULL;

	return (void *) addr;

#else
	return NULL;
#endif
}

static void
shared_deform_free(void *addr, Size size)
{
#ifdef __APPLE__
	vm_deallocate(mach_task_self(), (vm_address_t) addr, (vm_size_t) size);
#else
	munmap(addr, size);
#endif
}


/* ================================================================
 * pg_jitter_compile_shared_deform — leader-side shared deform setup
 *
 * Compiles a deform function and places code + descriptor array into
 * a single mmap'd page at a chosen virtual address.  The page bytes
 * are copied into the DSM so workers can mmap at the same VA.
 *
 * All absolute addresses embedded in the generated code (libc, PG
 * binary, pg_jitter .so functions) are identical across forked workers,
 * so no relocation is needed.
 * ================================================================ */
bool
pg_jitter_compile_shared_deform(SharedJitCompiledCode *sjc,
								TupleDesc desc,
								const TupleTableSlotOps *ops,
								int natts)
{
	Size		code_area_size;
	Size		desc_offset;
	Size		total_size;
	long		page_size;
	void	   *page_addr;
	void	   *compiled;
	Size		code_size = 0;
	DeformColDesc *descs;
	char	   *dsm_dest;

	if (!sjc || sjc->deform_addr != 0)
		return false;		/* already set */

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = 4096;

	/*
	 * Layout: [code (up to code_area_size)] [descriptors (8 bytes × natts)]
	 *
	 * Loop-based deform is ~530-800 bytes.  We reserve 2048 bytes.
	 */
	code_area_size = 2048;
	desc_offset = MAXALIGN(code_area_size);
	total_size = desc_offset + sizeof(DeformColDesc) * natts;
	total_size = (total_size + page_size - 1) & ~(page_size - 1);

	/* Check DSM has room for the page bytes at the end */
	if (sjc->used + total_size > sjc->capacity)
	{
		elog(DEBUG1, "pg_jitter: shared deform skipped, DSM too small "
			 "(%zu used + %zu needed > %zu capacity)",
			 sjc->used, total_size, sjc->capacity);
		return false;
	}

	/*
	 * Allocate RW page at an address that will also be free in parallel
	 * workers.  All workers are forked from the same postmaster, so the
	 * inherited address space layout is identical.  The only post-fork
	 * allocations that could conflict are sljit executable memory blocks.
	 *
	 * Strategy: use mmap with a high hint address (above typical mmap
	 * region) to avoid the area where sljit's exec allocator works.
	 * Then verify with MAP_FIXED_NOREPLACE / vm_allocate(VM_FLAGS_FIXED)
	 * to ensure we get the exact address.
	 *
	 * We probe starting from a high address derived from the .so text
	 * segment, stepping in 2MB increments.  Both leader and workers
	 * share the same .so load address (same postmaster fork).
	 */
	{
		uintptr_t so_base = (uintptr_t) pg_jitter_compile_shared_deform;
		uintptr_t hint_base;
		int attempt;

		/*
		 * Start 64MB above the .so text segment.  This avoids the
		 * mmap region where sljit allocates executable memory (which
		 * grows downward from the top on Linux).
		 */
		hint_base = (so_base + 0x4000000) & ~(uintptr_t)(page_size - 1);

		page_addr = NULL;
		for (attempt = 0; attempt < 32; attempt++)
		{
			void *try_addr = (void *)(hint_base + attempt * (uintptr_t) total_size);

			page_addr = shared_deform_alloc_fixed(try_addr, total_size);
			if (page_addr)
				break;
		}

		if (!page_addr)
		{
			/* Final fallback: let the kernel choose */
			page_addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
							 MAP_PRIVATE | MAP_ANON, -1, 0);
			if (page_addr == MAP_FAILED)
			{
				elog(DEBUG1, "pg_jitter: shared deform mmap failed: %m");
				return false;
			}
		}
	}

	/* Descriptor array lives at page_addr + desc_offset */
	descs = (DeformColDesc *) ((char *) page_addr + desc_offset);

	/*
	 * Compile the deform loop directly into our page using
	 * SLJIT_GENERATE_CODE_BUFFER.  target_descs points INTO the page,
	 * so the generated code embeds page_addr + desc_offset as the
	 * descriptor pointer — same VA in all workers.
	 *
	 * This is critical because x86-64 uses PC-relative call instructions
	 * and ARM64 uses page-relative ADRP sequences; generating at sljit's
	 * chosen address and then memcpy-ing would break those relocations.
	 */
	{
		struct sljit_generate_code_buffer buf;
		buf.buffer = page_addr;
		buf.size = code_area_size;
		buf.executable_offset = 0;

		compiled = pg_jitter_compile_deform_loop(desc, ops, natts,
												 descs, &code_size, &buf);
	}

	if (!compiled)
	{
		shared_deform_free(page_addr, total_size);
		elog(DEBUG1, "pg_jitter: shared deform compilation failed");
		return false;
	}

	/* Make the page executable */
	if (mprotect(page_addr, total_size, PROT_READ | PROT_EXEC) != 0)
	{
		shared_deform_free(page_addr, total_size);
		elog(DEBUG1, "pg_jitter: shared deform mprotect failed: %m");
		return false;
	}

	/* Store metadata in DSM header */
	sjc->deform_addr = (uint64)(uintptr_t) page_addr;
	sjc->deform_page_size = total_size;
	sjc->deform_code_size = code_size;
	sjc->deform_desc_offset = desc_offset;
	sjc->deform_natts = natts;

	/* Copy page bytes to end of DSM (page is PROT_READ so this works) */
	dsm_dest = (char *) sjc + sjc->capacity - total_size;
	memcpy(dsm_dest, page_addr, total_size);

	/* Set local fast-path */
	shared_deform_fn = page_addr;
	shared_deform_natts = natts;
	shared_deform_page = page_addr;
	shared_deform_page_size = total_size;

	elog(DEBUG1, "pg_jitter: compiled shared deform at %p, "
		 "%zu code + %d×%zu desc = %zu bytes (%zu pages)",
		 page_addr, code_size,
		 natts, sizeof(DeformColDesc), total_size,
		 total_size / page_size);

	return true;
}


/* ================================================================
 * pg_jitter_attach_shared_deform — worker-side shared deform attach
 *
 * Reads deform metadata from DSM, allocates memory at the same virtual
 * address as the leader (using shared_deform_alloc_fixed), copies page
 * bytes from DSM, and makes it executable.
 * ================================================================ */
bool
pg_jitter_attach_shared_deform(SharedJitCompiledCode *sjc)
{
	void   *target_addr;
	void   *page;
	char   *dsm_src;

	if (!sjc || sjc->deform_addr == 0)
		return false;

	/* Already attached? */
	if (shared_deform_fn)
		return true;

	target_addr = (void *)(uintptr_t) sjc->deform_addr;

	/* Try to allocate at the exact same virtual address */
	page = shared_deform_alloc_fixed(target_addr, sjc->deform_page_size);
	if (!page)
	{
		elog(DEBUG1, "pg_jitter: worker shared deform alloc at %p failed, "
			 "falling back to per-process compile", target_addr);
		return false;
	}

	/* Copy page bytes from DSM */
	dsm_src = (char *) sjc + sjc->capacity - sjc->deform_page_size;
	memcpy(page, dsm_src, sjc->deform_page_size);

	/* Make executable */
	if (mprotect(page, sjc->deform_page_size, PROT_READ | PROT_EXEC) != 0)
	{
		shared_deform_free(page, sjc->deform_page_size);
		elog(DEBUG1, "pg_jitter: worker shared deform mprotect failed: %m");
		return false;
	}

	shared_deform_fn = page;
	shared_deform_natts = sjc->deform_natts;
	shared_deform_page = page;
	shared_deform_page_size = sjc->deform_page_size;

	elog(DEBUG1, "pg_jitter: worker attached shared deform at %p "
		 "(%zu bytes, %d cols)",
		 page, sjc->deform_page_size, sjc->deform_natts);

	return true;
}


/* ================================================================
 * pg_jitter_reset_shared_deform — cleanup on context release
 *
 * Unmaps the shared deform page and clears the fast-path pointer.
 * Called from pg_jitter_cleanup_shared_dsm().
 * ================================================================ */
void
pg_jitter_reset_shared_deform(void)
{
	if (shared_deform_page)
	{
		shared_deform_free(shared_deform_page, shared_deform_page_size);
		shared_deform_page = NULL;
		shared_deform_page_size = 0;
	}
	shared_deform_fn = NULL;
	shared_deform_natts = 0;
}
