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
#include "pg_jitter_simd.h"
#include "sljitLir.h"

#include <sys/mman.h>
#include <unistd.h>            /* sysconf, _SC_PAGESIZE */

#include "port/pg_crc32c.h"
#ifdef _WIN64
#include "pg_crc32c_compat.h"
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef __linux__
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
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
 * Register allocation:
 *   S0 = slot (input arg)
 *   S1 = tts_values
 *   S2 = tts_isnull
 *   S3 = tupdata_base
 *   S4 = byte offset into tuple data
 *   S5 = attnum (column counter)
 *   R0-R3 = scratch
 *
 * ARM64 (10 callee-saved GPRs available, S6/S7 are free):
 *   S6 = dispatch array base pointer
 *   S7 = loop_limit (= min(natts, maxatt))
 *   8 saved regs, smaller stack (no DLOFF_DISPATCH/DLOFF_JUMPTBL)
 *
 * x86-64 (6 callee-saved GPRs: rbx,rbp,r12-r15):
 *   dispatch base + loop_limit on stack (SLJIT would spill S6/S7 anyway)
 *   6 saved regs, full stack layout
 * ================================================================ */

/*
 * Platform-conditional register allocation.
 * ARM64 has 10 callee-saved GPRs (x19-x28) so S6/S7 are real registers.
 * x86-64 has only 6 (rbx,rbp,r12-r15) so S6/S7 would be spilled to stack
 * by SLJIT — we skip them and use explicit stack slots instead.
 */
#if defined(__aarch64__) || defined(_M_ARM64)
#define DEFORM_USE_REG_DISPATCH  1
#define DEFORM_NSAVED            8
#else
#define DEFORM_USE_REG_DISPATCH  0
#define DEFORM_NSAVED            6
#endif

/* Stack layout for loop-based deform */
#define DLOFF_HASNULLS   0
#define DLOFF_MAXATT     8
#define DLOFF_TBITS      16
#if DEFORM_USE_REG_DISPATCH
/* ARM64: dispatch base in S6, loop_limit in S7 — no stack slots needed */
#define DLOFF_TOTAL      24
#else
/* x86-64: dispatch base + jump table on stack */
#define DLOFF_DISPATCH   24		/* dispatch byte array base */
#define DLOFF_JUMPTBL    32		/* jump table base */
#define DLOFF_TOTAL      40
#endif

/* Dispatch handler indices (bits 0-6 of dispatch byte) */
#define DHANDLER_BOOL     0		/* attlen=1, byval, align=1 */
#define DHANDLER_INT2     1		/* attlen=2, byval, align=2 */
#define DHANDLER_INT4     2		/* attlen=4, byval, align=4 */
#define DHANDLER_INT8     3		/* attlen=8, byval, align=8 */
#define DHANDLER_VARLENA  4		/* attlen=-1, byref, align=4 */
#define DHANDLER_CSTRING  5		/* attlen=-2, byref, align=1 */
#define DHANDLER_GENERIC  6		/* fixed byref, variable align */
#define DHANDLER_COUNT    7
#define DHANDLER_NULLABLE_BIT 0x80	/* bit 7: column is nullable */

/*
 * Dispatch byte layout (local dispatch loop):
 *   bits 0-6: handler index (0-6)
 *   bit 7:    nullable flag
 */

/* ================================================================
 * deform_sparse_dispatch — C helper for wide nullable tables
 *
 * Processes the null bitmap byte-by-byte, eliminating per-column
 * dispatch overhead for null columns. All-null bytes (0x00) are
 * handled in ~5 instructions for 8 columns. Mixed bytes process
 * individual bits. Non-null columns use descriptor-driven extraction.
 *
 * Called from JIT code via sljit_emit_icall for tables with:
 *   - nullable columns (!all_notnull)
 *   - local mode (!gen_buf)
 *   - wide tables (natts >= 64)
 * ================================================================ */
static void
deform_sparse_dispatch(TupleTableSlot *slot, int natts,
                       const DeformColDesc *descs, int is_minimal)
{
    HeapTupleData *tuple;
    HeapTupleHeader tup;
    bits8 *t_bits;
    char *tupdata;
    uint32 off;
    Datum *values = slot->tts_values;
    bool *isnull = slot->tts_isnull;
    int attnum = slot->tts_nvalid;

    /* Get tuple pointer based on slot type */
    if (is_minimal)
        tuple = ((MinimalTupleTableSlot *)slot)->tuple;
    else
        tuple = ((HeapTupleTableSlot *)slot)->tuple;

    tup = tuple->t_data;

    /* Check for missing attributes */
    int maxatt = HeapTupleHeaderGetNatts(tup);
    if (maxatt < natts)
        slot_getmissingattrs(slot, maxatt, natts);

    bool hasnulls = (tup->t_infomask & HEAP_HASNULL) != 0;
    t_bits = hasnulls ? tup->t_bits : NULL;
    tupdata = (char *)tup + tup->t_hoff;

    /* Initialize offset */
    if (attnum == 0)
        off = 0;
    else if (is_minimal)
        off = ((MinimalTupleTableSlot *)slot)->off;
    else
        off = ((HeapTupleTableSlot *)slot)->off;

    int limit = Min(natts, maxatt);

    if (!hasnulls)
    {
        /* No nulls in this tuple: extract all columns sequentially */
        for (; attnum < limit; attnum++)
        {
            const DeformColDesc *d = &descs[attnum];

            /* Alignment */
            if (d->attlen == -1)
                off = att_align_pointer(off, d->attalign, -1, tupdata + off);
            else if (d->attalign > 1)
                off = att_align_nominal(off, d->attalign);

            isnull[attnum] = false;
            values[attnum] = fetch_att(tupdata + off, d->attbyval, d->attlen);
            off = att_addlength_pointer(off, d->attlen, tupdata + off);
        }
    }
    else
    {
        /*
         * Has null bitmap: byte-level NEON-zero + CTZ extraction.
         *
         * For each bitmap byte:
         *   1. Unconditionally NEON-zero 8 values[] + set 8 isnull[]
         *      (assumes all null — 5 NEON instructions for 8 columns)
         *   2. If any bits set: CTZ-scan to extract only non-null columns
         *      (overwrites the zeros for present columns)
         *
         * This eliminates ALL branching for null columns. The CTZ loop
         * runs only for non-null columns (~10% for 90% null tables).
         * Total stores: 126 NEON stores + ~100 scalar stores ≈ 830
         * vs interpreter's ~2004 individual stores.
         */
        int start_byte = attnum >> 3;
        int start_bit_off = attnum & 7;
        int nbytes = (limit + 7) >> 3;

        /* Handle partial first byte if attnum not byte-aligned */
        if (start_bit_off > 0)
        {
            uint8 bm = t_bits[start_byte];
            int base = start_byte << 3;
            int end_col = Min(base + 8, limit);

            for (int col = attnum; col < end_col; col++)
            {
                if (!(bm & (1 << (col & 7))))
                {
                    values[col] = (Datum)0;
                    isnull[col] = true;
                }
                else
                {
                    const DeformColDesc *d = &descs[col];
                    if (d->attlen == -1)
                        off = att_align_pointer(off, d->attalign, -1, tupdata + off);
                    else if (d->attalign > 1)
                        off = att_align_nominal(off, d->attalign);
                    isnull[col] = false;
                    values[col] = fetch_att(tupdata + off, d->attbyval, d->attlen);
                    off = att_addlength_pointer(off, d->attlen, tupdata + off);
                }
            }
            attnum = end_col;
            start_byte++;
        }

        /* Main loop: full bytes, NEON-zero + CTZ extract */
        for (int bi = start_byte; bi < nbytes; bi++)
        {
            int base = bi << 3;

            if (base + 8 > limit)
            {
                /* Partial last byte: per-bit processing */
                uint8 bm = t_bits[bi];
                for (int col = base; col < limit; col++)
                {
                    if (!(bm & (1 << (col & 7))))
                    {
                        values[col] = (Datum)0;
                        isnull[col] = true;
                    }
                    else
                    {
                        const DeformColDesc *d = &descs[col];
                        if (d->attlen == -1)
                            off = att_align_pointer(off, d->attalign, -1, tupdata + off);
                        else if (d->attalign > 1)
                            off = att_align_nominal(off, d->attalign);
                        isnull[col] = false;
                        values[col] = fetch_att(tupdata + off, d->attbyval, d->attlen);
                        off = att_addlength_pointer(off, d->attlen, tupdata + off);
                    }
                }
                attnum = limit;
                break;
            }

            /*
             * Set 8 isnull = true + zero 8 values (unconditional).
             *
             * NEON on ARM64: 1 store for isnull (8 bytes) +
             * 4 stores for values (64 bytes) = 5 stores for 8 columns.
             * The subsequent CTZ loop overwrites non-null entries.
             *
             * For bitmap bytes that are 0x00 (all null, ~43% at 90% null),
             * no CTZ work follows — pure NEON, 0.625 instructions per column.
             */
#if defined(__aarch64__) || defined(_M_ARM64)
            {
                uint8x8_t vones = vdup_n_u8(1);
                vst1_u8((uint8_t *)(isnull + base), vones);
                uint64x2_t vzero = vdupq_n_u64(0);
                vst1q_u64((uint64_t *)(values + base), vzero);
                vst1q_u64((uint64_t *)(values + base + 2), vzero);
                vst1q_u64((uint64_t *)(values + base + 4), vzero);
                vst1q_u64((uint64_t *)(values + base + 6), vzero);
            }
#else
            memset(isnull + base, 1, 8);
            memset(values + base, 0, 8 * sizeof(Datum));
#endif

            /* CTZ-extract only non-null columns (overwrite zeros) */
            uint8 bm = t_bits[bi];
            while (bm)
            {
#ifdef _MSC_VER
                unsigned long _bsf_idx;
                _BitScanForward(&_bsf_idx, bm);
                int bit = (int)_bsf_idx;
#else
                int bit = __builtin_ctz(bm);
#endif
                int col = base + bit;
                const DeformColDesc *d = &descs[col];

                if (d->attlen == -1)
                    off = att_align_pointer(off, d->attalign, -1, tupdata + off);
                else if (d->attalign > 1)
                    off = att_align_nominal(off, d->attalign);

                isnull[col] = false;
                values[col] = fetch_att(tupdata + off, d->attbyval, d->attlen);
                off = att_addlength_pointer(off, d->attlen, tupdata + off);

                bm &= bm - 1;
            }
            attnum = base + 8;
        }
    }

    /* Epilogue: update slot metadata */
    slot->tts_nvalid = natts;
    if (is_minimal)
        ((MinimalTupleTableSlot *)slot)->off = off;
    else
        ((HeapTupleTableSlot *)slot)->off = off;
    slot->tts_flags |= TTS_FLAG_SLOW;
}

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

	/* Dispatch table state (local mode only, patched after code gen) */
	struct sljit_label *dispatch_handler_labels[DHANDLER_COUNT];
	sljit_uw *dispatch_jump_table = NULL;
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
					 4 | (sljit_has_cpu_feature(SLJIT_HAS_SIMD) ? SLJIT_ENTER_VECTOR(2) : 0),
					 DEFORM_NSAVED, DLOFF_TOTAL);

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
	/* S5 = tts_nvalid (attnum kept in register) */
	sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_S5, 0,
				   SLJIT_MEM1(SLJIT_S0),
				   offsetof(TupleTableSlot, tts_nvalid));

	/* S4 = offset. if attnum == 0: 0, else slot->off */
	{
		struct sljit_jump *j_nonzero;

		j_nonzero = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
								   SLJIT_S5, 0, SLJIT_IMM, 0);

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_S4, 0, SLJIT_IMM, 0);

		{
			struct sljit_jump *j_skip = sljit_emit_jump(C, SLJIT_JUMP);
			sljit_set_label(j_nonzero, sljit_emit_label(C));

			sljit_emit_op1(C, SLJIT_MOV_U32, SLJIT_S4, 0,
						   SLJIT_MEM1(SLJIT_S0), slot_off);

			sljit_set_label(j_skip, sljit_emit_label(C));
		}
	}

	/* ============================================================
	 * SPARSE DISPATCH: wide nullable tables (C helper)
	 *
	 * For tables with 64-500 nullable columns in local mode,
	 * delegate to a C helper that processes the null bitmap
	 * byte-by-byte with NEON bulk-zero + CTZ bit scanning.
	 *
	 * For >500 columns, return NULL to let the dispatch function
	 * fall back to PG's slot_getsomeattrs_int() — at that width,
	 * deform is memory-bandwidth limited and the interpreter's
	 * hand-tuned code matches our C helper.
	 * ============================================================ */
	if (!all_notnull && !gen_buf && natts > 500)
	{
		/* Too wide: interpreter beats any JIT path */
		sljit_free_compiler(C);
		if (!target_descs)
			pfree(descriptors);
		return NULL;
	}

	if (!all_notnull && !gen_buf && natts >= 64)
	{
		int is_minimal_val = (ops == &TTSOpsMinimalTuple) ? 1 : 0;

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, natts);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
					   SLJIT_IMM, (sljit_sw)descriptors);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
					   SLJIT_IMM, is_minimal_val);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS4V(P, 32, P, 32),
						 SLJIT_IMM, (sljit_sw)deform_sparse_dispatch);
		sljit_emit_return_void(C);

		/* Skip all loop code and shared epilogue — C helper handles everything */
		goto deform_code_gen;
	}

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
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
					   SLJIT_S2, 0, SLJIT_S5, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
					   SLJIT_IMM, natts, SLJIT_S5, 0);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
						 SLJIT_IMM, (sljit_sw) memset);

		if (attlen == 4)
		{
			/*
			 * SIMD batch extraction for uniform int32 columns.
			 * Processes 4 columns at a time using NEON: load 4×int32,
			 * sign-extend to 4×Datum (int64), store.
			 */
			/* R2 = count = min(natts, maxatt) - S5 */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
			sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
						   SLJIT_R2, 0, SLJIT_S5, 0);
			j_fast_done = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
										 SLJIT_R2, 0, SLJIT_IMM, 0);

			if (sljit_has_cpu_feature(SLJIT_HAS_SIMD)) {
				/*
				 * Inline SLJIT SIMD: load 4x int32, sign-extend to
				 * 2x int64, store as Datums. Eliminates C function
				 * call overhead (~100 cycles per invocation).
				 */
				/* R0 = tupdata + offset, R1 = values + attnum*8 */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_S1, 0, SLJIT_R1, 0);
				/* R2 = count */
				/* R3 = end pointer (values + (attnum+count)*8) */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R3, 0,
							   SLJIT_R2, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
							   SLJIT_R1, 0, SLJIT_R3, 0);

				/* SIMD loop: 4 int32 per iteration */
				struct sljit_label *l_simd_top;
				struct sljit_jump *j_simd_done;

				j_simd_done = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL,
											  SLJIT_R1, 0, SLJIT_R3, 0);
				l_simd_top = sljit_emit_label(C);

				/* VR0 = load 4x int32 from tupdata */
				sljit_emit_simd_mov(C,
					SLJIT_SIMD_LOAD | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_32,
					SLJIT_VR0, SLJIT_MEM1(SLJIT_R0), 0);

				/* VR1 = sign-extend lower 2x int32 → 2x int64 */
				sljit_emit_simd_extend(C,
					SLJIT_SIMD_REG_128 | SLJIT_SIMD_EXTEND_32 | SLJIT_SIMD_EXTEND_SIGNED,
					SLJIT_VR1, SLJIT_VR0, 0);

				/* Store lower 2 Datums */
				sljit_emit_simd_mov(C,
					SLJIT_SIMD_STORE | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
					SLJIT_VR1, SLJIT_MEM1(SLJIT_R1), 0);

				/* For upper 2: extract upper half and extend.
				 * simd_extend with offset gets upper elements. */
				/* Move upper 64 bits to lower: lane_replicate lane 1 */
				sljit_emit_simd_lane_replicate(C,
					SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
					SLJIT_VR0, SLJIT_VR0, 1);
				/* Now VR0 has upper 2 int32s in the lower 64 bits */
				sljit_emit_simd_extend(C,
					SLJIT_SIMD_REG_128 | SLJIT_SIMD_EXTEND_32 | SLJIT_SIMD_EXTEND_SIGNED,
					SLJIT_VR1, SLJIT_VR0, 0);
				sljit_emit_simd_mov(C,
					SLJIT_SIMD_STORE | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
					SLJIT_VR1, SLJIT_MEM1(SLJIT_R1), 16);

				/* Advance: R0 += 16 (4 int32), R1 += 32 (4 Datums) */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 16);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_R1, 0, SLJIT_IMM, 32);

				sljit_set_label(
					sljit_emit_cmp(C, SLJIT_LESS, SLJIT_R1, 0, SLJIT_R3, 0),
					l_simd_top);
				sljit_set_label(j_simd_done, sljit_emit_label(C));
			} else {
				/* Fallback: call C function */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_S1, 0, SLJIT_R1, 0);
				sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32),
								 SLJIT_IMM,
								 (sljit_sw) simd_extract_int32_values);
			}

			/* Update S4 (offset): S4 += count * 4 */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
			sljit_emit_op2(C, SLJIT_SUB, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_S5, 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_IMM, 2);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_R0, 0);

			sljit_set_label(j_fast_done, sljit_emit_label(C));
		}
		else
		{
			/* Tight inner loop — pointer-based do-while (non-int32 attlen) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S5, 0);
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

			sljit_set_label(j_fast_done, sljit_emit_label(C));
		}
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

		/*
		 * Check hasnulls — two-stage fast path:
		 * 1) HEAP_HASNULL not set → fast path (no null bitmap at all)
		 * 2) HEAP_HASNULL set → SIMD bulk-check bitmap; if all non-null
		 *    for the columns we need, still take the fast path
		 * 3) Otherwise → fall to general loop
		 */
		struct sljit_jump *j_no_hasnulls, *j_simd_all_notnull;

		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
		j_no_hasnulls = sljit_emit_cmp(C, SLJIT_EQUAL,
									   SLJIT_R0, 0, SLJIT_IMM, 0);

		/* HEAP_HASNULL is set — call simd_nullbitmap_all_notnull(t_bits, natts) */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, natts);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2(32, P, 32),
						 SLJIT_IMM, (sljit_sw) simd_nullbitmap_all_notnull);
		j_simd_all_notnull = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);
		/* Bitmap has actual nulls — fall to general loop */
		j_has_nulls_general = sljit_emit_jump(C, SLJIT_JUMP);

		/* Fast path target: either no HEAP_HASNULL or SIMD confirmed all non-null */
		{
			struct sljit_label *l_fast = sljit_emit_label(C);
			sljit_set_label(j_no_hasnulls, l_fast);
			sljit_set_label(j_simd_all_notnull, l_fast);
		}

		/* All columns non-null for this row — bulk-clear isnull, then tight loop */

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
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
					   SLJIT_S2, 0, SLJIT_S5, 0);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
		sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
					   SLJIT_IMM, natts, SLJIT_S5, 0);
		sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
						 SLJIT_IMM, (sljit_sw) memset);

		if (attlen == 4)
		{
			/* SIMD batch extraction for uniform int32 columns */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
			sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
						   SLJIT_R2, 0, SLJIT_S5, 0);
			j_semi_done = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
										 SLJIT_R2, 0, SLJIT_IMM, 0);

			if (sljit_has_cpu_feature(SLJIT_HAS_SIMD)) {
				/* Inline SIMD: same as all_notnull path */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_S1, 0, SLJIT_R1, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R3, 0,
							   SLJIT_R2, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
							   SLJIT_R1, 0, SLJIT_R3, 0);

				struct sljit_label *l_st;
				struct sljit_jump *j_sd;
				j_sd = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL,
									   SLJIT_R1, 0, SLJIT_R3, 0);
				l_st = sljit_emit_label(C);

				sljit_emit_simd_mov(C,
					SLJIT_SIMD_LOAD | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_32,
					SLJIT_VR0, SLJIT_MEM1(SLJIT_R0), 0);
				sljit_emit_simd_extend(C,
					SLJIT_SIMD_REG_128 | SLJIT_SIMD_EXTEND_32 | SLJIT_SIMD_EXTEND_SIGNED,
					SLJIT_VR1, SLJIT_VR0, 0);
				sljit_emit_simd_mov(C,
					SLJIT_SIMD_STORE | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
					SLJIT_VR1, SLJIT_MEM1(SLJIT_R1), 0);
				sljit_emit_simd_lane_replicate(C,
					SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
					SLJIT_VR0, SLJIT_VR0, 1);
				sljit_emit_simd_extend(C,
					SLJIT_SIMD_REG_128 | SLJIT_SIMD_EXTEND_32 | SLJIT_SIMD_EXTEND_SIGNED,
					SLJIT_VR1, SLJIT_VR0, 0);
				sljit_emit_simd_mov(C,
					SLJIT_SIMD_STORE | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
					SLJIT_VR1, SLJIT_MEM1(SLJIT_R1), 16);

				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 16);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_R1, 0, SLJIT_IMM, 32);
				sljit_set_label(
					sljit_emit_cmp(C, SLJIT_LESS, SLJIT_R1, 0, SLJIT_R3, 0),
					l_st);
				sljit_set_label(j_sd, sljit_emit_label(C));
			} else {
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_S1, 0, SLJIT_R1, 0);
				sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32),
								 SLJIT_IMM,
								 (sljit_sw) simd_extract_int32_values);
			}

			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
			sljit_emit_op2(C, SLJIT_SUB, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_S5, 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_IMM, 2);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_R0, 0);

			sljit_set_label(j_semi_done, sljit_emit_label(C));
		}
		else
		{
			/* Pointer-based do-while loop (non-int32 attlen) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0, SLJIT_S5, 0);
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
		}

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

				/* S5 = attnum (in register) */
				j_gn_done = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
										   SLJIT_S5, 0, SLJIT_IMM, natts);
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
				j_gn_avail = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
											SLJIT_S5, 0, SLJIT_R0, 0);

				/* NULL CHECK: bitmap check (hasnulls is known true here) */
				{
					struct sljit_jump *j_bit_set;

					/* byte = t_bits[attnum >> 3] */
					sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0,
								   SLJIT_S5, 0, SLJIT_IMM, 3);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
								   SLJIT_MEM2(SLJIT_R1, SLJIT_R0), 0);

					/* bit = 1 << (attnum & 7) */
					sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
								   SLJIT_S5, 0, SLJIT_IMM, 7);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 1);
					sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
								   SLJIT_R2, 0, SLJIT_R1, 0);

					sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
								   SLJIT_R0, 0, SLJIT_R2, 0);
					j_bit_set = sljit_emit_jump(C, SLJIT_NOT_ZERO);

					/* IS NULL */
					sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
								   SLJIT_S5, 0, SLJIT_IMM, 3);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM2(SLJIT_S1, SLJIT_R0), 0,
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV_U8,
								   SLJIT_MEM2(SLJIT_S2, SLJIT_S5), 0,
								   SLJIT_IMM, 1);

					/* Advance attnum and loop */
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
								   SLJIT_S5, 0, SLJIT_IMM, 1);
					sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_gn_top);

					sljit_set_label(j_bit_set, sljit_emit_label(C));
				}

				/* NOT NULL: tts_isnull[attnum] = false, extract int value */
				sljit_emit_op1(C, SLJIT_MOV_U8,
							   SLJIT_MEM2(SLJIT_S2, SLJIT_S5), 0,
							   SLJIT_IMM, 0);

				/* Load value: since uniform attlen, use fixed mov_op */
				sljit_emit_op1(C, mov_op, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);

				/* Store: tts_values[attnum] = R0 */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
							   SLJIT_R0, 0);

				/* Advance offset by fixed attlen */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_IMM, attlen);

				/* Advance attnum and loop */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 1);
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
	else if (gen_buf)
	{
		/* ============================================================
		 * GENERAL LOOP — shared mode (descriptor-based)
		 *
		 * In shared mode, dispatch/jump_table arrays are process-local
		 * and can't be embedded as immediates.  Use the existing
		 * descriptor-based loop which accesses the shared descriptors
		 * array via embedded pointer (valid across workers in DSM).
		 * ============================================================ */
		struct sljit_label *l_loop_top;
		struct sljit_label *l_epilogue;
		struct sljit_jump *j_done, *j_avail_out;

		l_loop_top = sljit_emit_label(C);

		j_done = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
								SLJIT_S5, 0, SLJIT_IMM, natts);
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		j_avail_out = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
									 SLJIT_S5, 0, SLJIT_R0, 0);

		/* R3 = desc_ptr = &descriptors[S5] */
		sljit_emit_op2(C, SLJIT_SHL, SLJIT_R3, 0,
					   SLJIT_S5, 0, SLJIT_IMM, 3);
		sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
					   SLJIT_R3, 0, SLJIT_IMM, (sljit_sw) descriptors);

		if (!all_notnull)
		{
			struct sljit_jump *j_notnull_attr, *j_no_tuple_nulls;
			struct sljit_jump *j_bit_set;
			struct sljit_label *l_not_null;

			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attnotnull));
			j_notnull_attr = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
			j_no_tuple_nulls = sljit_emit_cmp(C, SLJIT_EQUAL,
											  SLJIT_R0, 0, SLJIT_IMM, 0);

			sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
			sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_R1, SLJIT_R0), 0);

			sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 7);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_R2, 0, SLJIT_R1, 0);

			sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
						   SLJIT_R0, 0, SLJIT_R2, 0);
			j_bit_set = sljit_emit_jump(C, SLJIT_NOT_ZERO);

			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R0), 0,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV_U8,
						   SLJIT_MEM2(SLJIT_S2, SLJIT_S5), 0,
						   SLJIT_IMM, 1);

			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

			l_not_null = sljit_emit_label(C);
			sljit_set_label(j_notnull_attr, l_not_null);
			sljit_set_label(j_no_tuple_nulls, l_not_null);
			sljit_set_label(j_bit_set, l_not_null);
		}

		/* ALIGNMENT */
		{
			struct sljit_jump *j_no_align;

			sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attlen));
			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attalign));

			j_no_align = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 1);

			{
				struct sljit_jump *j_not_varlena, *j_is_short;

				j_not_varlena = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											   SLJIT_R1, 0, SLJIT_IMM, -1);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
							   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
				j_is_short = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R2, 0, SLJIT_IMM, 0);
				sljit_set_label(j_not_varlena, sljit_emit_label(C));

				sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R2, 0);
				sljit_emit_op2(C, SLJIT_XOR, SLJIT_R2, 0,
							   SLJIT_R2, 0, SLJIT_IMM, -1);
				sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R2, 0);

				{
					struct sljit_label *l_aligned = sljit_emit_label(C);
					sljit_set_label(j_no_align, l_aligned);
					sljit_set_label(j_is_short, l_aligned);
				}
			}
		}

		/* EXTRACT VALUE */
		{
			struct sljit_jump *j_not_byval, *j_byval_done;

			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attbyval));
			sljit_emit_op1(C, SLJIT_MOV_U8,
						   SLJIT_MEM2(SLJIT_S2, SLJIT_S5), 0,
						   SLJIT_IMM, 0);

			j_not_byval = sljit_emit_cmp(C, SLJIT_EQUAL,
										 SLJIT_R0, 0, SLJIT_IMM, 0);

			{
				struct sljit_jump *j_len1, *j_len2, *j_len4;
				struct sljit_jump *j_s8, *j_s1, *j_s2;
				struct sljit_label *l_store;

				j_len1 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 1);
				j_len2 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 2);
				j_len4 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 4);

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

				l_store = sljit_emit_label(C);
				sljit_set_label(j_s8, l_store);
				sljit_set_label(j_s1, l_store);
				sljit_set_label(j_s2, l_store);

				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
							   SLJIT_R0, 0);

				j_byval_done = sljit_emit_jump(C, SLJIT_JUMP);
			}

			sljit_set_label(j_not_byval, sljit_emit_label(C));
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S3, 0, SLJIT_S4, 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);

			sljit_set_label(j_byval_done, sljit_emit_label(C));
		}

		/* ADVANCE OFFSET */
		{
			struct sljit_jump *j_fixedlen, *j_varlena;
			struct sljit_label *l_next;

			j_fixedlen = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
										SLJIT_R1, 0, SLJIT_IMM, 0);
			j_varlena = sljit_emit_cmp(C, SLJIT_EQUAL,
									   SLJIT_R1, 0, SLJIT_IMM, -1);

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

				sljit_set_label(j_fixedlen, sljit_emit_label(C));
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R1, 0);
				{
					struct sljit_jump *j_fixed_done = sljit_emit_jump(C, SLJIT_JUMP);

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

		sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
					   SLJIT_S5, 0, SLJIT_IMM, 1);
		sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

		l_epilogue = sljit_emit_label(C);
		sljit_set_label(j_done, l_epilogue);
		sljit_set_label(j_avail_out, l_epilogue);
	}
	else
	{
		/* ============================================================
		 * DISPATCH-TABLE GENERAL LOOP — local mode
		 *
		 * Each column maps to one of 7 specialized handlers compiled
		 * with ALL properties as immediates.  Zero per-column metadata
		 * reads at runtime — alignment masks, load widths, and offset
		 * advances are baked into each handler's machine code.
		 *
		 * Dispatch byte per column: bits 0-6 = handler index,
		 *                           bit 7    = nullable flag.
		 *
		 * ~14 instructions per not-null column vs ~30 in the
		 * descriptor-based loop.
		 * ============================================================ */
		struct sljit_label *l_loop_top;
		struct sljit_label *l_epilogue;
		struct sljit_jump *j_done;
		struct sljit_jump *j_is_int4 = NULL, *j_is_int8 = NULL;
		uint8	*dispatch;
		uint8	*batch_count = NULL;
		bool	has_int4_batch = false;
		bool	has_int8_batch = false;
		MemoryContext old;

		/* Allocate dispatch and jump_table in TopMemoryContext */
		old = MemoryContextSwitchTo(TopMemoryContext);
		dispatch = palloc(natts);
		dispatch_jump_table = palloc(DHANDLER_COUNT * sizeof(sljit_uw));
		MemoryContextSwitchTo(old);

		/* Build dispatch array: 1 byte per column */
		for (int i = 0; i < natts; i++)
		{
			CompactAttribute *att = TupleDescCompactAttr(desc, i);
			uint8 d;

			if (att->attbyval)
			{
				switch (att->attlen)
				{
					case 1: d = DHANDLER_BOOL; break;
					case 2: d = DHANDLER_INT2; break;
					case 4: d = DHANDLER_INT4; break;
					case 8: d = DHANDLER_INT8; break;
					default: d = DHANDLER_GENERIC; break;
				}
			}
			else if (att->attlen == -1)
				d = DHANDLER_VARLENA;
			else if (att->attlen == -2)
				d = DHANDLER_CSTRING;
			else
				d = DHANDLER_GENERIC;

			if (!JITTER_ATT_IS_NOTNULL(att))
				d |= DHANDLER_NULLABLE_BIT;

			dispatch[i] = d;
		}

		/* === Batch detection for run-length optimization (#1 + #3) ===
		 *
		 * Scan dispatch array for runs of >= 4 consecutive non-nullable
		 * INT4 or INT8 columns.  For each column within a qualifying run,
		 * batch_count[i] = remaining columns in the run from position i.
		 * The INT4/INT8 handlers check this at runtime and process the
		 * entire remaining run with a single SIMD/memcpy call instead
		 * of per-column dispatch.
		 */
		{
			uint8 *bc;
			int run_start = 0;

			old = MemoryContextSwitchTo(TopMemoryContext);
			bc = palloc0(natts);
			MemoryContextSwitchTo(old);

			for (int i = 1; i <= natts; i++)
			{
				bool continues = (i < natts &&
								  dispatch[i] == dispatch[run_start] &&
								  !(dispatch[run_start] & DHANDLER_NULLABLE_BIT));
				if (!continues)
				{
					int run_len = i - run_start;
					uint8 handler = dispatch[run_start] & 0x7F;

					if (handler == DHANDLER_INT4 && run_len >= 4 &&
						!(dispatch[run_start] & DHANDLER_NULLABLE_BIT))
					{
						for (int j = run_start; j < run_start + run_len; j++)
						{
							int remaining = run_start + run_len - j;
							if (remaining >= 4)
								bc[j] = (remaining > 255) ? 255 : remaining;
						}
						has_int4_batch = true;
					}
					else if (handler == DHANDLER_INT8 && run_len >= 4 &&
							 !(dispatch[run_start] & DHANDLER_NULLABLE_BIT))
					{
						for (int j = run_start; j < run_start + run_len; j++)
						{
							int remaining = run_start + run_len - j;
							if (remaining >= 4)
								bc[j] = (remaining > 255) ? 255 : remaining;
						}
						has_int8_batch = true;
					}

					run_start = i;
				}
			}

			if (has_int4_batch || has_int8_batch)
				batch_count = bc;
			else
				pfree(bc);
		}

#if DEFORM_USE_REG_DISPATCH
		/* ARM64: dispatch base in S6, loop_limit in S7 */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_S6, 0,
					   SLJIT_IMM, (sljit_sw) dispatch);

		/* S7 = min(maxatt, natts) — single loop limit register */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_S7, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
		{
			struct sljit_jump *j_maxatt_ok;
			j_maxatt_ok = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
										 SLJIT_S7, 0, SLJIT_IMM, natts);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_S7, 0, SLJIT_IMM, natts);
			sljit_set_label(j_maxatt_ok, sljit_emit_label(C));
		}
#else
		/* x86-64: dispatch base + jump table on stack */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_DISPATCH,
					   SLJIT_IMM, (sljit_sw) dispatch);

		/* Clamp maxatt on stack: DLOFF_MAXATT = min(maxatt, natts) */
		{
			struct sljit_jump *j_maxatt_ok;
			j_maxatt_ok = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
										 SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT,
										 SLJIT_IMM, natts);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT,
						   SLJIT_IMM, natts);
			sljit_set_label(j_maxatt_ok, sljit_emit_label(C));
		}

		/* Store jump table base on stack */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_JUMPTBL,
					   SLJIT_IMM, (sljit_sw) dispatch_jump_table);
#endif

		/* Bulk-clear isnull for ALL tables (not just all_notnull).
		 * This allows skipping per-handler isnull[S5] = false stores
		 * and enables the early null check optimization below. */
		{
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S2, 0, SLJIT_S5, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
			sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
						   SLJIT_IMM, natts, SLJIT_S5, 0);
			sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
							 SLJIT_IMM, (sljit_sw) memset);
		}

		/* === OPTIMIZATION #2: Bulk null bitmap check ===
		 *
		 * For nullable tables, check the entire null bitmap at once
		 * using SIMD (NEON: 128 columns per op).  If all columns are
		 * non-null for THIS tuple, clear the hasnulls flag on stack.
		 * This makes every per-column null check in the dispatch loop
		 * skip via the existing "hasnulls == 0" fast path (2 instructions
		 * instead of ~8 for the bitmap lookup).
		 */
		if (!all_notnull)
		{
			struct sljit_jump *j_no_hasnulls_pre, *j_has_actual_nulls;

			/* Skip if HEAP_HASNULL not set */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
			j_no_hasnulls_pre = sljit_emit_cmp(C, SLJIT_EQUAL,
											   SLJIT_R0, 0, SLJIT_IMM, 0);

			/* Call simd_nullbitmap_all_notnull(t_bits, natts) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, natts);
			sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2(32, P, 32),
							 SLJIT_IMM,
							 (sljit_sw) simd_nullbitmap_all_notnull);

			/* If returns false (has actual nulls), keep hasnulls flag */
			j_has_actual_nulls = sljit_emit_cmp(C, SLJIT_EQUAL,
												SLJIT_R0, 0, SLJIT_IMM, 0);

			/* All non-null: clear hasnulls → per-column checks skip */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS,
						   SLJIT_IMM, 0);

			{
				struct sljit_label *l_bitmap_done = sljit_emit_label(C);
				sljit_set_label(j_no_hasnulls_pre, l_bitmap_done);
				sljit_set_label(j_has_actual_nulls, l_bitmap_done);
			}
		}

		/* === LOOP TOP === */
		l_loop_top = sljit_emit_label(C);

#if DEFORM_USE_REG_DISPATCH
		/* ARM64: bounds check against S7 register, dispatch from S6 */
		j_done = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
								SLJIT_S5, 0, SLJIT_S7, 0);
#else
		/* x86-64: bounds check against stack, dispatch from stack */
		j_done = sljit_emit_cmp(C, SLJIT_SIG_GREATER_EQUAL,
								SLJIT_S5, 0,
								SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
#endif

		/* === 8-COLUMN NULL GROUP SKIP ===
		 *
		 * For sparse tables (many NULLs), the per-column null bitmap
		 * check dominates: ~15 instructions per NULL column.
		 * At 8-column boundaries, check the entire bitmap byte.
		 * If byte == 0x00 (all 8 columns NULL): bulk-zero values[]
		 * and set isnull[] in ~12 instructions instead of 8×15=120.
		 *
		 * For 90% NULL tables with 1000 columns, this skips ~112
		 * groups of 8, saving ~10,000 instructions per row.
		 */
		if (!all_notnull && natts >= 16)
		{
			struct sljit_jump *j_not_boundary, *j_no_hasnulls_grp;
			struct sljit_jump *j_not_all_null, *j_grp_overflow;

			/* Only at 8-column boundaries: test S5 & 7 */
			sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
						   SLJIT_S5, 0, SLJIT_IMM, 7);
			j_not_boundary = sljit_emit_jump(C, SLJIT_NOT_ZERO);

			/* Skip if no hasnulls flag */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
			j_no_hasnulls_grp = sljit_emit_cmp(C, SLJIT_EQUAL,
											   SLJIT_R0, 0, SLJIT_IMM, 0);

			/* Bounds check: S5 + 8 <= limit */
#if DEFORM_USE_REG_DISPATCH
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 8);
			j_grp_overflow = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
											SLJIT_R0, 0, SLJIT_S7, 0);
#else
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 8);
			j_grp_overflow = sljit_emit_cmp(C, SLJIT_SIG_GREATER,
											SLJIT_R0, 0,
											SLJIT_MEM1(SLJIT_SP), DLOFF_MAXATT);
#endif

			/* Load bitmap byte: R0 = t_bits[S5 >> 3] */
			sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
			sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_R1, SLJIT_R0), 0);

			/* If byte != 0 (some columns not null), fall through */
			j_not_all_null = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

			/* ALL 8 COLUMNS ARE NULL: bulk-zero values + set isnull */

			/* R0 = &values[S5] */
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S1, 0, SLJIT_R0, 0);
			/* Zero 8 Datum values (64 bytes) */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 0,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 8,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 16,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 24,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 32,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 40,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 48,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 56,
						   SLJIT_IMM, 0);

			/* Set 8 isnull bytes to true (0x0101010101010101) */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S2, 0, SLJIT_S5, 0);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
						   SLJIT_IMM, (sljit_sw) 0x0101010101010101LL);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 0,
						   SLJIT_R1, 0);

			/* Advance S5 by 8 and loop */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 8);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

			/* Normal per-column dispatch for non-boundary or non-all-null */
			{
				struct sljit_label *l_normal_dispatch = sljit_emit_label(C);
				sljit_set_label(j_not_boundary, l_normal_dispatch);
				sljit_set_label(j_no_hasnulls_grp, l_normal_dispatch);
				sljit_set_label(j_not_all_null, l_normal_dispatch);
				sljit_set_label(j_grp_overflow, l_normal_dispatch);
			}
		}

		/* === EARLY NULL CHECK ===
		 *
		 * Check null bitmap BEFORE loading dispatch byte.  For sparse
		 * tables (90% NULL), this skips dispatch byte load + handler
		 * extraction + nullable bit check for null columns — saving
		 * ~5 instructions per null column.
		 *
		 * Since isnull[] is bulk-cleared to false above, we only need
		 * to set isnull[S5] = true and values[S5] = 0 for null columns.
		 * Non-null columns jump directly to dispatch byte load.
		 */
		if (!all_notnull)
		{
			struct sljit_jump *j_no_hasnulls_early, *j_bit_set_early;

			/* Fast path when hasnulls==0 (Opt#2 cleared it or
			 * HEAP_HASNULL not set): skip bitmap check entirely */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_HASNULLS);
			j_no_hasnulls_early = sljit_emit_cmp(C, SLJIT_EQUAL,
												 SLJIT_R0, 0, SLJIT_IMM, 0);

			/* Bitmap check: byte = t_bits[S5 >> 3] */
			sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_SP), DLOFF_TBITS);
			sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_R1, SLJIT_R0), 0);

			/* bit = 1 << (S5 & 7) */
			sljit_emit_op2(C, SLJIT_AND, SLJIT_R1, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 7);
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_R2, 0, SLJIT_R1, 0);

			/* if (byte & bit) → not null, skip to dispatch */
			sljit_emit_op2u(C, SLJIT_AND | SLJIT_SET_Z,
						   SLJIT_R0, 0, SLJIT_R2, 0);
			j_bit_set_early = sljit_emit_jump(C, SLJIT_NOT_ZERO);

			/* IS NULL: values[S5] = 0, isnull[S5] = true
			 * (isnull was bulk-cleared to false, override for nulls) */
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R0), 0,
						   SLJIT_IMM, 0);
			sljit_emit_op1(C, SLJIT_MOV_U8,
						   SLJIT_MEM2(SLJIT_S2, SLJIT_S5), 0,
						   SLJIT_IMM, 1);

			/* Advance attnum and loop (skip dispatch entirely) */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

			/* Not null: fall through to dispatch byte load */
			{
				struct sljit_label *l_load_dispatch = sljit_emit_label(C);
				sljit_set_label(j_no_hasnulls_early, l_load_dispatch);
				sljit_set_label(j_bit_set_early, l_load_dispatch);
			}
		}

		/* Load dispatch byte */
#if DEFORM_USE_REG_DISPATCH
		/* ARM64: R0 = S6[S5] (single indexed load) */
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
					   SLJIT_MEM2(SLJIT_S6, SLJIT_S5), 0);
#else
		/* x86-64: load base from stack, then index */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_DISPATCH);
		sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
					   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);
#endif

		/* R3 = handler index (bits 0-6) */
		sljit_emit_op2(C, SLJIT_AND, SLJIT_R3, 0,
					   SLJIT_R0, 0, SLJIT_IMM, 0x7F);

		/* === HANDLER DISPATCH: direct branches for common types ===
		 * INT4 and INT8 cover ~40% of analytics columns.  Direct
		 * conditional branches are well-predicted by the CPU; rare
		 * types fall through to the jump table. */
		j_is_int4 = sljit_emit_cmp(C, SLJIT_EQUAL,
								   SLJIT_R3, 0, SLJIT_IMM, DHANDLER_INT4);
		j_is_int8 = sljit_emit_cmp(C, SLJIT_EQUAL,
								   SLJIT_R3, 0, SLJIT_IMM, DHANDLER_INT8);

		/* Fallback: jump table for other types */
#if DEFORM_USE_REG_DISPATCH
		/* ARM64: jump table address as immediate (no stack slot needed) */
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_IMM, (sljit_sw) dispatch_jump_table);
#else
		sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
					   SLJIT_MEM1(SLJIT_SP), DLOFF_JUMPTBL);
#endif
		sljit_emit_ijump(C, SLJIT_JUMP,
						 SLJIT_MEM2(SLJIT_R0, SLJIT_R3), SLJIT_WORD_SHIFT);

		/* === HANDLER: BOOL (attlen=1, byval, align=1) === */
		dispatch_handler_labels[DHANDLER_BOOL] = sljit_emit_label(C);
		{
			/* No alignment needed (align=1) */
			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);
			/* isnull[S5] already false from bulk-clear */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
		}

		/* === HANDLER: INT2 (attlen=2, byval, align=2) === */
		dispatch_handler_labels[DHANDLER_INT2] = sljit_emit_label(C);
		{
			/* Align to 2: S4 = (S4 + 1) & ~1 */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, (sljit_sw) ~(sljit_uw)1);
			sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);
			/* isnull[S5] already false from bulk-clear */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 2);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
		}

		/* === HANDLER: INT4 (attlen=4, byval, align=4) === */
		dispatch_handler_labels[DHANDLER_INT4] = sljit_emit_label(C);
		if (j_is_int4)
			sljit_set_label(j_is_int4, dispatch_handler_labels[DHANDLER_INT4]);
		{
			/* Align to 4: S4 = (S4 + 3) & ~3 */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 3);
			sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, (sljit_sw) ~(sljit_uw)3);

			/* === OPTIMIZATION #1+#3: Batch INT4 extraction ===
			 *
			 * Check if this column starts a run of >= 4 consecutive
			 * non-nullable INT4 columns.  If so, process the entire
			 * run with a single simd_extract_int32_values() call
			 * (NEON/SSE vectorized) instead of per-column dispatch.
			 */
			if (has_int4_batch)
			{
				struct sljit_jump *j_no_batch;

				/* R0 = batch_count[S5] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) batch_count);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);
				j_no_batch = sljit_emit_cmp(C, SLJIT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

				/* --- BATCH PATH --- */
				if (!all_notnull)
				{
					/* memset(&isnull[S5], 0, count) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
								   SLJIT_R0, 0);		/* save count in R3 */
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
								   SLJIT_S2, 0, SLJIT_S5, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_R3, 0);
					sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
									 SLJIT_IMM, (sljit_sw) memset);
					/* Reload count after call clobbers R0-R3 */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) batch_count);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
								   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_R0, 0);
				}

				/* Extract int32→int64: inline SIMD or C fallback.
				 * R2 = count (set above) */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_S1, 0, SLJIT_R1, 0);
				if (sljit_has_cpu_feature(SLJIT_HAS_SIMD)) {
					sljit_emit_op2(C, SLJIT_SHL, SLJIT_R3, 0,
								   SLJIT_R2, 0, SLJIT_IMM, 3);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
								   SLJIT_R1, 0, SLJIT_R3, 0);
					struct sljit_label *l_bt;
					struct sljit_jump *j_bd;
					j_bd = sljit_emit_cmp(C, SLJIT_GREATER_EQUAL,
										   SLJIT_R1, 0, SLJIT_R3, 0);
					l_bt = sljit_emit_label(C);
					sljit_emit_simd_mov(C,
						SLJIT_SIMD_LOAD | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_32,
						SLJIT_VR0, SLJIT_MEM1(SLJIT_R0), 0);
					sljit_emit_simd_extend(C,
						SLJIT_SIMD_REG_128 | SLJIT_SIMD_EXTEND_32 | SLJIT_SIMD_EXTEND_SIGNED,
						SLJIT_VR1, SLJIT_VR0, 0);
					sljit_emit_simd_mov(C,
						SLJIT_SIMD_STORE | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
						SLJIT_VR1, SLJIT_MEM1(SLJIT_R1), 0);
					sljit_emit_simd_lane_replicate(C,
						SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
						SLJIT_VR0, SLJIT_VR0, 1);
					sljit_emit_simd_extend(C,
						SLJIT_SIMD_REG_128 | SLJIT_SIMD_EXTEND_32 | SLJIT_SIMD_EXTEND_SIGNED,
						SLJIT_VR1, SLJIT_VR0, 0);
					sljit_emit_simd_mov(C,
						SLJIT_SIMD_STORE | SLJIT_SIMD_REG_128 | SLJIT_SIMD_ELEM_64,
						SLJIT_VR1, SLJIT_MEM1(SLJIT_R1), 16);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
								   SLJIT_R0, 0, SLJIT_IMM, 16);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
								   SLJIT_R1, 0, SLJIT_IMM, 32);
					sljit_set_label(
						sljit_emit_cmp(C, SLJIT_LESS, SLJIT_R1, 0, SLJIT_R3, 0),
						l_bt);
					sljit_set_label(j_bd, sljit_emit_label(C));
				} else {
					sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3V(P, P, 32),
									 SLJIT_IMM,
									 (sljit_sw) simd_extract_int32_values);
				}

				/* Reload count for offset/attnum advance */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) batch_count);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);

				/* S4 += count * 4 */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 2);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R1, 0);
				/* S5 += count */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
							   SLJIT_S5, 0, SLJIT_R0, 0);

				sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

				/* --- SINGLE-COLUMN PATH --- */
				sljit_set_label(j_no_batch, sljit_emit_label(C));
			}

			/* Single INT4 column: load, store, advance */
			sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);
			/* isnull[S5] already false from bulk-clear */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 4);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
		}

		/* === HANDLER: INT8 (attlen=8, byval, align=8) === */
		dispatch_handler_labels[DHANDLER_INT8] = sljit_emit_label(C);
		if (j_is_int8)
			sljit_set_label(j_is_int8, dispatch_handler_labels[DHANDLER_INT8]);
		{
			/* Align to 8: S4 = (S4 + 7) & ~7 */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 7);
			sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, (sljit_sw) ~(sljit_uw)7);

			/* === OPTIMIZATION #3: Batch INT8 extraction ===
			 *
			 * For runs of >= 4 consecutive non-nullable INT8 columns,
			 * use memcpy instead of per-column dispatch.  INT8 Datum
			 * is the same as int64 (8 bytes), so the copy is identity.
			 */
			if (has_int8_batch)
			{
				struct sljit_jump *j_no_batch;

				/* R0 = batch_count[S5] */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) batch_count);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);
				j_no_batch = sljit_emit_cmp(C, SLJIT_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 0);

				/* --- BATCH PATH --- */
				if (!all_notnull)
				{
					/* memset(&isnull[S5], 0, count) */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R3, 0,
								   SLJIT_R0, 0);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
								   SLJIT_S2, 0, SLJIT_S5, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0,
								   SLJIT_IMM, 0);
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_R3, 0);
					sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, 32, W),
									 SLJIT_IMM, (sljit_sw) memset);
					/* Reload count */
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_IMM, (sljit_sw) batch_count);
					sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R2, 0,
								   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);
				}
				else
				{
					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0,
								   SLJIT_R0, 0);
				}

				/* memcpy(&values[S5], tupdata + S4, count * 8)
				 * R2 = count (set above) */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R0, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S1, 0, SLJIT_R0, 0);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R1, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
							   SLJIT_R2, 0, SLJIT_IMM, 3);  /* count * 8 */
				sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS3(P, P, P, W),
								 SLJIT_IMM, (sljit_sw) memcpy);

				/* Reload count for advance */
				sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
							   SLJIT_IMM, (sljit_sw) batch_count);
				sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
							   SLJIT_MEM2(SLJIT_R0, SLJIT_S5), 0);

				/* S4 += count * 8 */
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R1, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 3);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R1, 0);
				/* S5 += count */
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
							   SLJIT_S5, 0, SLJIT_R0, 0);

				sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);

				/* --- SINGLE-COLUMN PATH --- */
				sljit_set_label(j_no_batch, sljit_emit_label(C));
			}

			/* Single INT8 column: load, store, advance */
			sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);
			/* isnull[S5] already false from bulk-clear */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 8);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
		}

		/* === HANDLER: VARLENA (attlen=-1, byref, align=4) === */
		dispatch_handler_labels[DHANDLER_VARLENA] = sljit_emit_label(C);
		{
			struct sljit_jump *j_is_short;

			/* Short varlena check: if *(base+offset) != 0 → skip align */
			sljit_emit_op1(C, SLJIT_MOV_U8, SLJIT_R0, 0,
						   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
			j_is_short = sljit_emit_cmp(C, SLJIT_NOT_EQUAL,
										SLJIT_R0, 0, SLJIT_IMM, 0);

			/* Full varlena: align to 4 */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, 3);
			sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_IMM, (sljit_sw) ~(sljit_uw)3);

			sljit_set_label(j_is_short, sljit_emit_label(C));

			/* Store pointer: values[attnum] = base + offset */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S3, 0, SLJIT_S4, 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);
			/* isnull[S5] already false from bulk-clear */

			/* Advance: S4 += varsize_any(base + offset)
			 * R0 still = S3 + S4 from pointer store above */
			sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
							 SLJIT_IMM, (sljit_sw) varsize_any);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_R0, 0);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
		}

		/* === HANDLER: CSTRING (attlen=-2, byref, align=1) === */
		dispatch_handler_labels[DHANDLER_CSTRING] = sljit_emit_label(C);
		{
			/* No alignment needed (align=1) */
			/* Store pointer: values[attnum] = base + offset */
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_S3, 0, SLJIT_S4, 0);
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op1(C, SLJIT_MOV,
						   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
						   SLJIT_R0, 0);
			/* isnull[S5] already false from bulk-clear */

			/* Advance: S4 += strlen(base + offset) + 1
			 * R0 still = S3 + S4 */
			sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(W, P),
							 SLJIT_IMM, (sljit_sw) strlen);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
						   SLJIT_R0, 0, SLJIT_IMM, 1);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
						   SLJIT_S4, 0, SLJIT_R0, 0);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 1);
			sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
		}

		/* === HANDLER: GENERIC (fixed byref, variable attlen/attalign) ===
		 * Falls back to descriptor array for rare column types. */
		dispatch_handler_labels[DHANDLER_GENERIC] = sljit_emit_label(C);
		{
			/* R3 = &descriptors[S5] */
			sljit_emit_op2(C, SLJIT_SHL, SLJIT_R3, 0,
						   SLJIT_S5, 0, SLJIT_IMM, 3);
			sljit_emit_op2(C, SLJIT_ADD, SLJIT_R3, 0,
						   SLJIT_R3, 0, SLJIT_IMM, (sljit_sw) descriptors);

			/* R1 = attlen, R0 = attalign */
			sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R1, 0,
						   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attlen));
			sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
						   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attalign));

			/* Alignment: S4 = (S4 + (align-1)) & ~(align-1)
			 * Skip if align <= 1 */
			{
				struct sljit_jump *j_no_align;

				j_no_align = sljit_emit_cmp(C, SLJIT_SIG_LESS_EQUAL,
											SLJIT_R0, 0, SLJIT_IMM, 1);

				sljit_emit_op2(C, SLJIT_SUB, SLJIT_R2, 0,
							   SLJIT_R0, 0, SLJIT_IMM, 1);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R2, 0);
				sljit_emit_op2(C, SLJIT_XOR, SLJIT_R2, 0,
							   SLJIT_R2, 0, SLJIT_IMM, -1);
				sljit_emit_op2(C, SLJIT_AND, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R2, 0);

				sljit_set_label(j_no_align, sljit_emit_label(C));
			}

			/* Check byval for load type */
			{
				struct sljit_jump *j_generic_byref;

				sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
							   SLJIT_MEM1(SLJIT_R3), offsetof(DeformColDesc, attbyval));
				j_generic_byref = sljit_emit_cmp(C, SLJIT_EQUAL,
												 SLJIT_R0, 0, SLJIT_IMM, 0);

				/* Byval: load by attlen (R1 still live) */
				{
					struct sljit_jump *j_gl1, *j_gl2, *j_gl4;
					struct sljit_jump *j_gs8, *j_gs1, *j_gs2;
					struct sljit_label *l_gstore;

					j_gl1 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 1);
					j_gl2 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 2);
					j_gl4 = sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 4);

					sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0,
								   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
					j_gs8 = sljit_emit_jump(C, SLJIT_JUMP);

					sljit_set_label(j_gl1, sljit_emit_label(C));
					sljit_emit_op1(C, SLJIT_MOV_S8, SLJIT_R0, 0,
								   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
					j_gs1 = sljit_emit_jump(C, SLJIT_JUMP);

					sljit_set_label(j_gl2, sljit_emit_label(C));
					sljit_emit_op1(C, SLJIT_MOV_S16, SLJIT_R0, 0,
								   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);
					j_gs2 = sljit_emit_jump(C, SLJIT_JUMP);

					sljit_set_label(j_gl4, sljit_emit_label(C));
					sljit_emit_op1(C, SLJIT_MOV_S32, SLJIT_R0, 0,
								   SLJIT_MEM2(SLJIT_S3, SLJIT_S4), 0);

					l_gstore = sljit_emit_label(C);
					sljit_set_label(j_gs8, l_gstore);
					sljit_set_label(j_gs1, l_gstore);
					sljit_set_label(j_gs2, l_gstore);

					sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
								   SLJIT_S5, 0, SLJIT_IMM, 3);
					sljit_emit_op1(C, SLJIT_MOV,
								   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
								   SLJIT_R0, 0);
					/* isnull[S5] already false from bulk-clear */

					/* Advance offset by attlen */
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
								   SLJIT_S4, 0, SLJIT_R1, 0);
					sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
								   SLJIT_S5, 0, SLJIT_IMM, 1);
					sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
				}

				/* Byref: store pointer, advance by attlen */
				sljit_set_label(j_generic_byref, sljit_emit_label(C));
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0,
							   SLJIT_S3, 0, SLJIT_S4, 0);
				sljit_emit_op2(C, SLJIT_SHL, SLJIT_R2, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 3);
				sljit_emit_op1(C, SLJIT_MOV,
							   SLJIT_MEM2(SLJIT_S1, SLJIT_R2), 0,
							   SLJIT_R0, 0);
				/* isnull[S5] already false from bulk-clear */

				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S4, 0,
							   SLJIT_S4, 0, SLJIT_R1, 0);
				sljit_emit_op2(C, SLJIT_ADD, SLJIT_S5, 0,
							   SLJIT_S5, 0, SLJIT_IMM, 1);
				sljit_set_label(sljit_emit_jump(C, SLJIT_JUMP), l_loop_top);
			}
		}

		/* === EPILOGUE === */
		l_epilogue = sljit_emit_label(C);
		sljit_set_label(j_done, l_epilogue);
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

deform_code_gen:
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

	/* Patch dispatch jump table with actual handler addresses */
	if (code && dispatch_jump_table)
	{
		int i;
		for (i = 0; i < DHANDLER_COUNT; i++)
			dispatch_jump_table[i] = sljit_get_label_addr(dispatch_handler_labels[i]);
	}

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
#define DEFORM_MAX_CACHED_ATTRS 16  /* max columns stored for validation */

typedef struct DeformDispatchEntry
{
	uint32		attrs_hash;	/* hash of CompactAttribute array for natts columns */
	int			natts;
	const TupleTableSlotOps *ops;
	void	   *fn;
	CompactAttribute attrs[DEFORM_MAX_CACHED_ATTRS]; /* copy for collision check */
} DeformDispatchEntry;

/*
 * Compute a hash over the CompactAttribute entries relevant to deform code
 * generation.  Two descriptors produce identical deform code iff they agree
 * on every column's attlen, attalign, attbyval, attnotnull, attisdropped,
 * and atthasmissing.
 *
 * IMPORTANT: skip attcacheoff (first field).  It starts as -1 and is lazily
 * set during first deform, so hashing it causes false matches between
 * tables with different physical layouts but identical column types.
 */
static uint32
deform_attrs_hash(TupleDesc desc, int natts)
{
	pg_crc32c crc;

	INIT_CRC32C(crc);
	for (int i = 0; i < natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);
		/* Skip attcacheoff (first 4 bytes) — hash from attlen onward */
		COMP_CRC32C(crc,
					(const char *)att + offsetof(CompactAttribute, attlen),
					sizeof(CompactAttribute) - offsetof(CompactAttribute, attlen));
	}
	FIN_CRC32C(crc);
	return (uint32) crc;
}

static DeformDispatchEntry deform_dispatch_cache[DEFORM_DISPATCH_CACHE_SIZE];
static int n_deform_dispatch = 0;

/*
 * Multi-entry fast-path cache for deform dispatch.  Avoids recomputing the
 * O(natts) CRC32C hash on every row.
 *
 * Uses (TupleDesc pointer, natts) as the cache key.  Within a single query,
 * a given slot always has the same TupleDesc pointer, so this is safe for
 * the hot inner loop.  The attrs_hash is stored alongside to verify
 * correctness across query boundaries (where palloc can reuse TupleDesc
 * addresses with different layouts).
 *
 * 4 entries handles hash joins (inner + outer slots) and complex queries
 * with up to 4 distinct table types.
 */
#define DISPATCH_FASTPATH_SIZE 4

typedef struct DispatchFastEntry
{
	TupleDesc	desc;		/* TupleDesc pointer — fast match key */
	const TupleTableSlotOps *ops;  /* slot type — must match exactly */
	int			natts;
	uint32		attrs_hash;	/* CRC32C hash for cross-query validation */
	Oid			tdtypeid;	/* type OID — changes on table recreate */
	int32		tdtypmod;	/* type modifier */
	void	   *fn;
} DispatchFastEntry;

static DispatchFastEntry dispatch_fast[DISPATCH_FASTPATH_SIZE];
static int n_dispatch_fast = 0;

/* ================================================================
 * Shared deform state — set by leader (compile) or worker (attach),
 * used as fast-path in dispatch.
 * ================================================================ */
static void *shared_deform_fn = NULL;
static int   shared_deform_natts = 0;
static void *shared_deform_page = NULL;		/* for munmap on cleanup */
static Size  shared_deform_page_size = 0;

void
pg_jitter_deform_dispatch_reset_fastpath(void)
{
	n_dispatch_fast = 0;
}


void
pg_jitter_compiled_deform_dispatch(TupleTableSlot *slot, int natts)
{
	TupleDesc desc = slot->tts_tupleDescriptor;
	const TupleTableSlotOps *ops = slot->tts_ops;
	void *fn = NULL;
	uint32 ahash = 0;
	int i;

#ifdef _WIN64
	/*
	 * Verify slot integrity before deform.  If the slot pointer or its
	 * tts_values/tts_isnull are corrupt, bail to the safe PG deform.
	 */
	if (unlikely(!slot || !slot->tts_values || !slot->tts_isnull ||
				 !desc || natts <= 0))
	{
		slot_getsomeattrs_int(slot, natts);
		return;
	}
#endif

	if (natts > pg_jitter_wide_deform_limit())
	{
		slot_getsomeattrs_int(slot, natts);
		return;
	}

	/* Shared deform fast path: same VA across all parallel workers */
	if (shared_deform_fn && natts == shared_deform_natts)
	{
		((void (*)(TupleTableSlot *)) shared_deform_fn)(slot);
		return;
	}

	if (pg_jitter_deform_cache)
	{
		/* TupleDesc-pointer fast path — O(1) for repeated calls within a query.
		 * Pointer is stable within a query; cache is cleared at query end
		 * (pg_jitter_release_context) to prevent stale pointer matches.
		 *
		 * Additional safety: validate the attrs_hash matches.  DDL statements
		 * (CREATE/DROP TABLE) may not create a JitContext, so the fast-path
		 * can persist across DDL that reuses a TupleDesc address for a
		 * different table layout. */
		for (i = 0; i < n_dispatch_fast; i++)
		{
			DispatchFastEntry *fe = &dispatch_fast[i];
			if (fe->desc == desc && fe->ops == ops && fe->natts == natts &&
				fe->tdtypeid == desc->tdtypeid && fe->tdtypmod == desc->tdtypmod)
			{
				/* Move to front (MRU) for hot-path O(1) */
				if (i > 0)
				{
					DispatchFastEntry tmp = *fe;
					memmove(&dispatch_fast[1], &dispatch_fast[0],
							i * sizeof(DispatchFastEntry));
					dispatch_fast[0] = tmp;
				}
				((void (*)(TupleTableSlot *)) dispatch_fast[0].fn)(slot);
				return;
			}
		}

		/* Full lookup with CRC32C hash + full attribute validation */
		ahash = deform_attrs_hash(desc, natts);
		for (i = 0; i < n_deform_dispatch; i++)
		{
			DeformDispatchEntry *e = &deform_dispatch_cache[i];
			if (e->natts == natts && e->ops == ops && e->attrs_hash == ahash)
			{
				/* Verify full attribute equality to prevent hash collisions */
				bool match = true;
				for (int a = 0; a < natts; a++)
				{
					CompactAttribute *ca = TupleDescCompactAttr(desc, a);
					if (memcmp((char *)ca + offsetof(CompactAttribute, attlen),
							   (char *)&e->attrs[a] + offsetof(CompactAttribute, attlen),
							   sizeof(CompactAttribute) - offsetof(CompactAttribute, attlen)) != 0)
					{
						match = false;
						break;
					}
				}
				if (match)
				{
					fn = e->fn;
					goto found;
				}
			}
		}
	}

	/* Compile: loop for wide tables, unrolled for narrow */
	if (natts > pg_jitter_deform_threshold())
		fn = pg_jitter_compile_deform_loop(desc, ops, natts, NULL, NULL, NULL);
	else
		fn = pg_jitter_compile_deform(desc, ops, natts);

	if (fn)
	{
		if (pg_jitter_deform_cache)
		{
			/* Cache for future calls (code persists for backend lifetime) */
			if (n_deform_dispatch < DEFORM_DISPATCH_CACHE_SIZE &&
				natts <= DEFORM_MAX_CACHED_ATTRS)
			{
				DeformDispatchEntry *e = &deform_dispatch_cache[n_deform_dispatch++];
				e->attrs_hash = ahash;
				e->natts = natts;
				e->ops = ops;
				e->fn = fn;
				/* Store attribute metadata for collision validation */
				for (int a = 0; a < natts; a++)
					e->attrs[a] = *TupleDescCompactAttr(desc, a);
			}
found:
			/* Add to TupleDesc-pointer fast-path cache (MRU front insertion) */
			{
				int slot_idx = (n_dispatch_fast < DISPATCH_FASTPATH_SIZE)
					? n_dispatch_fast++ : DISPATCH_FASTPATH_SIZE - 1;
				if (slot_idx > 0)
					memmove(&dispatch_fast[1], &dispatch_fast[0],
							slot_idx * sizeof(DispatchFastEntry));
				dispatch_fast[0].desc = desc;
				dispatch_fast[0].ops = ops;
				dispatch_fast[0].natts = natts;
				dispatch_fast[0].attrs_hash = ahash;
				dispatch_fast[0].tdtypeid = desc->tdtypeid;
				dispatch_fast[0].tdtypmod = desc->tdtypmod;
				dispatch_fast[0].fn = fn;
			}
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
