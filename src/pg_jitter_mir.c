/*
 * pg_jitter_mir.c — MIR-based PostgreSQL JIT provider
 *
 * Uses MIR's medium-level IR with optimization pipeline.
 * Hot-path opcodes (~30) get native code; everything else falls back to
 * pg_jitter_fallback_step().
 */
#include "postgres.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "executor/execExpr.h"
#include "executor/tuptable.h"
#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "utils/expandeddatum.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "access/htup_details.h"
#include "utils/fmgrprotos.h"
#include "common/hashfn.h"
#include "pg_jitter_common.h"
#include "pg_jit_funcs.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <sys/mman.h>
#include <unistd.h>
#endif

/*
 * Mirror of Int8TransTypeData from numeric.c.
 */
typedef struct
{
	int64		count;
	int64		sum;
} MirInt8TransTypeData;
#include "mir.h"
#include "mir-gen.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_jitter_mir",
);

/* Forward declarations */
static bool mir_compile_expr(ExprState *state);
static void mir_reset_after_error(void);

/* Counter for unique register/proto names within a single expression */
static int mir_name_counter = 0;

/*
 * When true, use steps-relative addressing for all step field accesses,
 * making generated code position-independent and shareable via DSM.
 */
static bool mir_shared_code_mode = false;

/*
 * Sentinel address table for MIR shared code mode.
 *
 * MIR's code generator optimizes 64-bit immediate loads by skipping zero
 * halfwords — e.g., an address 0x0000AAAA0000BBBB gets only 2 instructions
 * (MOVZ+MOVK) instead of the full MOVZ+3×MOVK.  The relocation scanner
 * requires exactly 4 instructions to detect and patch addresses.
 *
 * Fix: when mir_shared_code_mode is true, register external functions with
 * sentinel addresses that have all 4 halfwords non-zero.  This forces MIR
 * to emit 4-instruction sequences.  After MIR_gen(), we patch the sentinels
 * back to real addresses in the leader's code.  Workers copy from DSM and
 * only need ASLR delta relocation (which the scanner handles correctly since
 * all sequences are 4 instructions).
 */
typedef struct
{
	void	   *real_addr;		/* actual function pointer */
	uint64		sentinel;		/* all-nonzero-halfword fake address */
} MirSentinelEntry;

#define MIR_MAX_SENTINELS 128

static MirSentinelEntry mir_sentinels[MIR_MAX_SENTINELS];
static int mir_n_sentinels = 0;

/*
 * Generate a sentinel address with all 4 halfwords non-zero.
 * Pattern: 0xCAFE_<idx>001_BABE_<idx>001 where idx varies.
 */
static uint64
mir_alloc_sentinel(void *real_addr)
{
	int idx = mir_n_sentinels;
	uint64 s;

	Assert(idx < MIR_MAX_SENTINELS);

	/*
	 * Construct sentinel with all 4 halfwords non-zero.
	 * Use idx+1 in low bits to make each one unique.
	 */
	s = 0xCAFE000000000000ULL
		| ((uint64)((idx + 1) & 0xFFFF) << 32)
		| 0x00000000BABE0000ULL
		| ((uint64)((idx + 1) & 0xFFFF));

	mir_sentinels[idx].real_addr = real_addr;
	mir_sentinels[idx].sentinel = s;
	mir_n_sentinels++;

	return s;
}

/*
 * Get the address to pass to MIR_load_external: sentinel in shared mode,
 * real address otherwise.
 */
static void *
mir_extern_addr(void *real_addr)
{
	if (!mir_shared_code_mode)
		return real_addr;
	return (void *)(uintptr_t) mir_alloc_sentinel(real_addr);
}

/*
 * After MIR_gen(), scan the generated code and replace sentinel addresses
 * with real addresses.  Only needed for the leader's code — workers get
 * already-patched code from DSM.
 *
 * Uses the same MOVZ+3×MOVK scanning as pg_jitter_relocate_dylib_addrs,
 * but matches against our sentinel table instead of a range check.
 */
static int
mir_patch_sentinels(void *code, Size code_size)
{
#if defined(__aarch64__)
	uint32_t   *insns = (uint32_t *) code;
	int			ninsns = code_size / 4;
	int			patched = 0;

	for (int i = 0; i + 3 < ninsns; i++)
	{
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

		/* Reconstruct the 64-bit value */
		uint64 val;
		val  = (uint64)((w0 >> 5) & 0xFFFF);
		val |= (uint64)((w1 >> 5) & 0xFFFF) << 16;
		val |= (uint64)((w2 >> 5) & 0xFFFF) << 32;
		val |= (uint64)((w3 >> 5) & 0xFFFF) << 48;

		/* Check if it matches any sentinel */
		for (int s = 0; s < mir_n_sentinels; s++)
		{
			if (val == mir_sentinels[s].sentinel)
			{
				uint64 real = (uint64)(uintptr_t) mir_sentinels[s].real_addr;

				insns[i]     = (w0 & 0xFFE0001F) | (((real >>  0) & 0xFFFF) << 5);
				insns[i + 1] = (w1 & 0xFFE0001F) | (((real >> 16) & 0xFFFF) << 5);
				insns[i + 2] = (w2 & 0xFFE0001F) | (((real >> 32) & 0xFFFF) << 5);
				insns[i + 3] = (w3 & 0xFFE0001F) | (((real >> 48) & 0xFFFF) << 5);

				patched++;
				i += 3;
				break;
			}
		}
	}

	return patched;
#else
	/* x86_64: scan for 8-byte sentinel values in const pool */
	int patched = 0;
	uint8_t *bytes = (uint8_t *) code;

	for (Size off = 0; off + 7 < code_size; off++)
	{
		uint64 val;
		memcpy(&val, bytes + off, 8);

		for (int s = 0; s < mir_n_sentinels; s++)
		{
			if (val == mir_sentinels[s].sentinel)
			{
				uint64 real = (uint64)(uintptr_t) mir_sentinels[s].real_addr;
				memcpy(bytes + off, &real, 8);
				patched++;
				off += 7;
				break;
			}
		}
	}

	return patched;
#endif
}

/*
 * Pre-compiled MIR blob support — shared infrastructure from pg_jit_mir_blobs.h.
 * Provides mir_find_precompiled_fn() and mir_load_precompiled_blobs().
 */
#include "pg_jit_mir_blobs.h"

/*
 * Per-query MIR state.
 *
 * Each PgJitterContext (= each query) gets its own MIR context.  When the
 * JitContext is released (ResourceOwner cleanup), we destroy the MIR context,
 * freeing all generated code and IR.  This prevents unbounded code memory
 * growth in long-running backends.
 *
 * Cost: MIR_init() + MIR_gen_init() ≈ 1.4 ms, paid once per query (not per
 * expression).  A query typically has 1–5 expressions sharing the same
 * JitContext.
 */
typedef struct MirPerQueryState
{
	MIR_context_t	ctx;
	int				module_counter;
} MirPerQueryState;

/* Single-threaded PG: cache the current per-query state */
static MirPerQueryState *mir_current_state = NULL;
static PgJitterContext  *mir_current_jctx = NULL;

/*
 * Free the per-query MIR context when its JitContext is released.
 */
static void
mir_ctx_free(void *data)
{
	MirPerQueryState *st = (MirPerQueryState *) data;

	if (st)
	{
		if (mir_current_state == st)
		{
			mir_current_state = NULL;
			mir_current_jctx = NULL;
		}
		MIR_gen_finish(st->ctx);
		MIR_finish(st->ctx);
		pfree(st);
	}
}

/*
 * Get or create a MIR context for the given JitContext.
 */
static MIR_context_t
mir_get_or_create_ctx(PgJitterContext *jctx)
{
	if (mir_current_jctx == jctx)
		return mir_current_state->ctx;

	/* New JitContext — create fresh MIR context */
	{
		MirPerQueryState *st = MemoryContextAllocZero(TopMemoryContext,
													  sizeof(MirPerQueryState));
		st->ctx = MIR_init();
		MIR_gen_init(st->ctx);
		MIR_gen_set_optimize_level(st->ctx, 1);
		st->module_counter = 0;

		/* Register for cleanup when this JitContext is released */
		pg_jitter_register_compiled(jctx, mir_ctx_free, st);

		mir_current_state = st;
		mir_current_jctx = jctx;

#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
		/* Precompiled blobs use their own mmap'd memory, load once globally */
		mir_load_precompiled_blobs(NULL);
#endif

		return st->ctx;
	}
}

/*
 * Provider entry point.
 */
void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = mir_reset_after_error;
	cb->release_context = pg_jitter_release_context;
	cb->compile_expr = mir_compile_expr;
}

/*
 * Error reset — intentionally a no-op.
 *
 * Per-query MIR contexts are freed via mir_ctx_free() called from
 * pg_jitter_release_context() through the ResourceOwner machinery.
 * A cursor surviving a ROLLBACK TO SAVEPOINT keeps its ResourceOwner
 * alive, so its JitContext (and MIR context) won't be freed until the
 * cursor is closed.
 */
static void
mir_reset_after_error(void)
{
	/* nothing to do */
}

/*
 * Helper: get econtext slot offset for a given opcode.
 */
static int64_t
mir_slot_offset(ExprEvalOp opcode)
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
#ifdef HAVE_EEOP_OLD_NEW
		case EEOP_OLD_FETCHSOME:
		case EEOP_OLD_VAR:
		case EEOP_ASSIGN_OLD_VAR:
			return offsetof(ExprContext, ecxt_oldtuple);
		case EEOP_NEW_FETCHSOME:
		case EEOP_NEW_VAR:
		case EEOP_ASSIGN_NEW_VAR:
			return offsetof(ExprContext, ecxt_newtuple);
#endif
		default:
			return offsetof(ExprContext, ecxt_scantuple);
	}
}

/*
 * Helper: create a uniquely-named MIR register.
 */
static MIR_reg_t
mir_new_reg(MIR_context_t ctx, MIR_func_t f, MIR_type_t type, const char *prefix)
{
	char	name[64];

	snprintf(name, sizeof(name), "%s_%d", prefix, mir_name_counter++);
	return MIR_new_func_reg(ctx, f, type, name);
}

/*
 * Check whether PG's hand-optimized fast-path evalfuncs handle this
 * expression better than JIT-compiled code.  Tiny 2-5 step expressions
 * that match these patterns should be left alone.
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

#ifdef HAVE_EEOP_HASHDATUM
		/* INNER_FETCHSOME + HASHDATUM_SET_INITVAL + INNER_VAR + HASHDATUM_NEXT32 + DONE */
		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_HASHDATUM_SET_INITVAL &&
			step2 == EEOP_INNER_VAR &&
			step3 == EEOP_HASHDATUM_NEXT32)
			return true;
#endif
	}
	else if (nsteps == 4)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);

#ifdef HAVE_EEOP_HASHDATUM
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
#endif
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
 * mir_emit_deform_inline — emit inline tuple deform code into the MIR
 * expression function body.
 *
 * This replaces the slot_getsomeattrs_int() call with an unrolled
 * per-column extraction loop, keeping all code in the same MIR function
 * (contiguous I-cache, zero call overhead).
 *
 * Returns true if inline deform was emitted, false to fall back to the
 * C function call.
 */
static bool
mir_emit_deform_inline(MIR_context_t ctx, MIR_item_t func_item,
					   MIR_func_t f,
					   TupleDesc desc, const TupleTableSlotOps *ops,
					   int natts, ExprEvalOp fetch_opcode,
					   MIR_reg_t r_econtext, MIR_reg_t r_slot,
					   MIR_item_t proto_varsize, MIR_item_t import_varsize,
					   MIR_item_t proto_strlen, MIR_item_t import_strlen)
{
	int			attnum;
	int			known_alignment = 0;
	bool		attguaranteedalign = true;
	int			guaranteed_column_number = -1;
	int64_t		tuple_off, slot_off;

	MIR_reg_t	r_tupdata, r_values, r_nulls, r_off;
	MIR_reg_t	r_hasnulls, r_maxatt, r_tbits, r_dtmp;
	MIR_reg_t	r_val, r_callret;

	MIR_insn_t *att_labels;
	MIR_insn_t	deform_done;

	/* --- Guards --- */
	if (ops == &TTSOpsVirtual)
		return false;
	if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple)
		return false;
	if (natts <= 0 || natts > desc->natts)
		return false;
	/* Wide tables: fall back to slot_getsomeattrs_int */
	if (natts > pg_jitter_deform_threshold())
		return false;

	/* Determine slot-type-specific field offsets */
	tuple_off = offsetof(HeapTupleTableSlot, tuple);
	slot_off = offsetof(HeapTupleTableSlot, off);

	/* --- Pre-scan: reject atthasmissing, find guaranteed_column_number --- */
	for (attnum = 0; attnum < natts; attnum++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, attnum);

		if (att->atthasmissing)
			return false;

		if (JITTER_ATT_IS_NOTNULL(att) && !att->attisdropped)
			guaranteed_column_number = attnum;
	}

	/* --- Allocate MIR registers (all upfront to minimize spill pressure) --- */
	r_tupdata = mir_new_reg(ctx, f, MIR_T_I64, "df_td");
	r_values  = mir_new_reg(ctx, f, MIR_T_I64, "df_vals");
	r_nulls   = mir_new_reg(ctx, f, MIR_T_I64, "df_nuls");
	r_off     = mir_new_reg(ctx, f, MIR_T_I64, "df_off");
	r_hasnulls = mir_new_reg(ctx, f, MIR_T_I64, "df_hn");
	r_maxatt  = mir_new_reg(ctx, f, MIR_T_I64, "df_mx");
	r_tbits   = mir_new_reg(ctx, f, MIR_T_I64, "df_tb");
	r_dtmp    = mir_new_reg(ctx, f, MIR_T_I64, "df_dt");
	r_val     = mir_new_reg(ctx, f, MIR_T_I64, "df_v");
	r_callret = mir_new_reg(ctx, f, MIR_T_I64, "df_cr");

	/* --- Labels --- */
	att_labels = palloc0(sizeof(MIR_insn_t) * natts);
	for (attnum = 0; attnum < natts; attnum++)
		att_labels[attnum] = MIR_new_label(ctx);
	deform_done = MIR_new_label(ctx);

	/* ============================================================
	 * PROLOGUE: load tuple header fields
	 * ============================================================ */

	/* r_values = slot->tts_values */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_values),
			MIR_new_mem_op(ctx, MIR_T_P,
				offsetof(TupleTableSlot, tts_values),
				r_slot, 0, 1)));

	/* r_nulls = slot->tts_isnull */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_nulls),
			MIR_new_mem_op(ctx, MIR_T_P,
				offsetof(TupleTableSlot, tts_isnull),
				r_slot, 0, 1)));

	/* r_dtmp = slot->tuple (HeapTuple pointer) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_mem_op(ctx, MIR_T_P, tuple_off,
				r_slot, 0, 1)));

	/* r_dtmp = tuple->t_data (HeapTupleHeader) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_mem_op(ctx, MIR_T_P,
				offsetof(HeapTupleData, t_data),
				r_dtmp, 0, 1)));

	/* r_hasnulls = *(uint16)(r_dtmp + t_infomask) & HEAP_HASNULL */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_hasnulls),
			MIR_new_mem_op(ctx, MIR_T_U16,
				offsetof(HeapTupleHeaderData, t_infomask),
				r_dtmp, 0, 1)));
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_AND,
			MIR_new_reg_op(ctx, r_hasnulls),
			MIR_new_reg_op(ctx, r_hasnulls),
			MIR_new_int_op(ctx, HEAP_HASNULL)));

	/* r_maxatt = *(uint16)(r_dtmp + t_infomask2) & HEAP_NATTS_MASK */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_maxatt),
			MIR_new_mem_op(ctx, MIR_T_U16,
				offsetof(HeapTupleHeaderData, t_infomask2),
				r_dtmp, 0, 1)));
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_AND,
			MIR_new_reg_op(ctx, r_maxatt),
			MIR_new_reg_op(ctx, r_maxatt),
			MIR_new_int_op(ctx, HEAP_NATTS_MASK)));

	/* r_tbits = r_dtmp + offsetof(HeapTupleHeaderData, t_bits) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_ADD,
			MIR_new_reg_op(ctx, r_tbits),
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_int_op(ctx, offsetof(HeapTupleHeaderData, t_bits))));

	/* r_tupdata = r_dtmp + *(uint8)(r_dtmp + t_hoff) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_tupdata),
			MIR_new_mem_op(ctx, MIR_T_U8,
				offsetof(HeapTupleHeaderData, t_hoff),
				r_dtmp, 0, 1)));
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_ADD,
			MIR_new_reg_op(ctx, r_tupdata),
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_reg_op(ctx, r_tupdata)));

	/* r_off = *(uint32)(r_slot + slot_off) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_off),
			MIR_new_mem_op(ctx, MIR_T_U32, slot_off,
				r_slot, 0, 1)));

	/* ============================================================
	 * NVALID DISPATCH: comparison chain
	 * ============================================================ */
	/* r_dtmp = slot->tts_nvalid (reuse r_dtmp, no longer needed for header) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_mem_op(ctx, MIR_T_I16,
				offsetof(TupleTableSlot, tts_nvalid),
				r_slot, 0, 1)));

	for (attnum = 0; attnum < natts; attnum++)
	{
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_BEQ,
				MIR_new_label_op(ctx, att_labels[attnum]),
				MIR_new_reg_op(ctx, r_dtmp),
				MIR_new_int_op(ctx, attnum)));
	}
	/* Default: already fully deformed → skip */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_JMP,
			MIR_new_label_op(ctx, deform_done)));

	/* ============================================================
	 * PER-ATTRIBUTE CODE EMISSION (unrolled)
	 * ============================================================ */
	for (attnum = 0; attnum < natts; attnum++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, attnum);
		int		alignto = JITTER_ATTALIGNBY(att);
		MIR_insn_t next_label = (attnum < natts - 1)
								? att_labels[attnum + 1]
								: deform_done;

		/* ---- Emit label ---- */
		MIR_append_insn(ctx, func_item, att_labels[attnum]);

		/* If attnum == 0: reset offset to 0 */
		if (attnum == 0)
		{
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_reg_op(ctx, r_off),
					MIR_new_int_op(ctx, 0)));
		}

		/* ---- Availability check ---- */
		if (attnum > guaranteed_column_number)
		{
			/* if attnum >= maxatt → deform_done */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_BGE,
					MIR_new_label_op(ctx, deform_done),
					MIR_new_int_op(ctx, attnum),
					MIR_new_reg_op(ctx, r_maxatt)));
		}

		/* ---- Null check ---- */
		if (!JITTER_ATT_IS_NOTNULL(att))
		{
			MIR_insn_t notnull_label = MIR_new_label(ctx);

			/* if (!hasnulls) skip to not-null path */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_BEQ,
					MIR_new_label_op(ctx, notnull_label),
					MIR_new_reg_op(ctx, r_hasnulls),
					MIR_new_int_op(ctx, 0)));

			/* byte = t_bits[attnum >> 3] */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_reg_op(ctx, r_dtmp),
					MIR_new_mem_op(ctx, MIR_T_U8,
						attnum >> 3,
						r_tbits, 0, 1)));

			/* test bit (1 << (attnum & 7)) */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_AND,
					MIR_new_reg_op(ctx, r_dtmp),
					MIR_new_reg_op(ctx, r_dtmp),
					MIR_new_int_op(ctx, 1 << (attnum & 0x07))));

			/* if bit set → not null */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_BNE,
					MIR_new_label_op(ctx, notnull_label),
					MIR_new_reg_op(ctx, r_dtmp),
					MIR_new_int_op(ctx, 0)));

			/* ---- Column IS NULL ---- */
			/* tts_values[attnum] = 0 */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_mem_op(ctx, MIR_T_I64,
						attnum * (int64_t) sizeof(Datum),
						r_values, 0, 1),
					MIR_new_int_op(ctx, 0)));

			/* tts_isnull[attnum] = true */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_mem_op(ctx, MIR_T_U8,
						attnum, r_nulls, 0, 1),
					MIR_new_int_op(ctx, 1)));

			/* jump to next attribute (or deform_done for last) */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_JMP,
					MIR_new_label_op(ctx, next_label)));

			/* ---- NOT NULL path ---- */
			MIR_append_insn(ctx, func_item, notnull_label);

			attguaranteedalign = false;
		}

		/* ---- Alignment ---- */
		if (alignto > 1 &&
			(known_alignment < 0 ||
			 known_alignment != TYPEALIGN(alignto, known_alignment)))
		{
			if (att->attlen == -1)
			{
				MIR_insn_t is_short_label = MIR_new_label(ctx);

				attguaranteedalign = false;

				/* Peek first byte: if nonzero → short varlena, skip align */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ADD,
						MIR_new_reg_op(ctx, r_dtmp),
						MIR_new_reg_op(ctx, r_tupdata),
						MIR_new_reg_op(ctx, r_off)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_dtmp),
						MIR_new_mem_op(ctx, MIR_T_U8, 0,
							r_dtmp, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, is_short_label),
						MIR_new_reg_op(ctx, r_dtmp),
						MIR_new_int_op(ctx, 0)));

				/* TYPEALIGN: r_off = (r_off + alignto-1) & ~(alignto-1) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ADD,
						MIR_new_reg_op(ctx, r_off),
						MIR_new_reg_op(ctx, r_off),
						MIR_new_int_op(ctx, alignto - 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_AND,
						MIR_new_reg_op(ctx, r_off),
						MIR_new_reg_op(ctx, r_off),
						MIR_new_int_op(ctx, ~((int64_t)(alignto - 1)))));

				MIR_append_insn(ctx, func_item, is_short_label);
			}
			else
			{
				/* TYPEALIGN: r_off = (r_off + alignto-1) & ~(alignto-1) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ADD,
						MIR_new_reg_op(ctx, r_off),
						MIR_new_reg_op(ctx, r_off),
						MIR_new_int_op(ctx, alignto - 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_AND,
						MIR_new_reg_op(ctx, r_off),
						MIR_new_reg_op(ctx, r_off),
						MIR_new_int_op(ctx, ~((int64_t)(alignto - 1)))));
			}

			if (known_alignment >= 0)
				known_alignment = TYPEALIGN(alignto, known_alignment);
		}

		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_reg_op(ctx, r_off),
					MIR_new_int_op(ctx, known_alignment)));
		}

		/* ---- Value extraction ---- */
		/* r_dtmp = tupdata + off (attdatap) */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_ADD,
				MIR_new_reg_op(ctx, r_dtmp),
				MIR_new_reg_op(ctx, r_tupdata),
				MIR_new_reg_op(ctx, r_off)));

		/* tts_isnull[attnum] = false */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_mem_op(ctx, MIR_T_U8,
					attnum, r_nulls, 0, 1),
				MIR_new_int_op(ctx, 0)));

		if (att->attbyval)
		{
			MIR_type_t load_type;

			switch (att->attlen)
			{
				case 1: load_type = MIR_T_I8; break;
				case 2: load_type = MIR_T_I16; break;
				case 4: load_type = MIR_T_I32; break;
				case 8: load_type = MIR_T_I64; break;
				default:
					pfree(att_labels);
					return false;
			}

			/* r_val = *(load_type *)(r_dtmp) — sign-extend to 64-bit */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_reg_op(ctx, r_val),
					MIR_new_mem_op(ctx, load_type, 0,
						r_dtmp, 0, 1)));

			/* tts_values[attnum] = r_val */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_mem_op(ctx, MIR_T_I64,
						attnum * (int64_t) sizeof(Datum),
						r_values, 0, 1),
					MIR_new_reg_op(ctx, r_val)));
		}
		else
		{
			/* byref: tts_values[attnum] = pointer to data (r_dtmp) */
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_MOV,
					MIR_new_mem_op(ctx, MIR_T_I64,
						attnum * (int64_t) sizeof(Datum),
						r_values, 0, 1),
					MIR_new_reg_op(ctx, r_dtmp)));
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
				/* Will be set by constant at top of next column */
			}
			else
			{
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ADD,
						MIR_new_reg_op(ctx, r_off),
						MIR_new_reg_op(ctx, r_off),
						MIR_new_int_op(ctx, att->attlen)));
			}
		}
		else if (att->attlen == -1)
		{
			/* Varlena: off += varsize_any(attdatap) */
			MIR_append_insn(ctx, func_item,
				MIR_new_call_insn(ctx, 4,
					MIR_new_ref_op(ctx, proto_varsize),
					MIR_new_ref_op(ctx, import_varsize),
					MIR_new_reg_op(ctx, r_callret),
					MIR_new_reg_op(ctx, r_dtmp)));
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_ADD,
					MIR_new_reg_op(ctx, r_off),
					MIR_new_reg_op(ctx, r_off),
					MIR_new_reg_op(ctx, r_callret)));
		}
		else if (att->attlen == -2)
		{
			/* CString: off += strlen(attdatap) + 1 */
			MIR_append_insn(ctx, func_item,
				MIR_new_call_insn(ctx, 4,
					MIR_new_ref_op(ctx, proto_strlen),
					MIR_new_ref_op(ctx, import_strlen),
					MIR_new_reg_op(ctx, r_callret),
					MIR_new_reg_op(ctx, r_dtmp)));
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_ADD,
					MIR_new_reg_op(ctx, r_callret),
					MIR_new_reg_op(ctx, r_callret),
					MIR_new_int_op(ctx, 1)));
			MIR_append_insn(ctx, func_item,
				MIR_new_insn(ctx, MIR_ADD,
					MIR_new_reg_op(ctx, r_off),
					MIR_new_reg_op(ctx, r_off),
					MIR_new_reg_op(ctx, r_callret)));
		}
	}

	/* ============================================================
	 * EPILOGUE: store tts_nvalid, off, flags
	 * ============================================================ */
	MIR_append_insn(ctx, func_item, deform_done);

	/* tts_nvalid = natts (int16 store) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_mem_op(ctx, MIR_T_I16,
				offsetof(TupleTableSlot, tts_nvalid),
				r_slot, 0, 1),
			MIR_new_int_op(ctx, natts)));

	/* slot->off = (uint32) r_off */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_mem_op(ctx, MIR_T_U32, slot_off,
				r_slot, 0, 1),
			MIR_new_reg_op(ctx, r_off)));

	/* tts_flags |= TTS_FLAG_SLOW */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_mem_op(ctx, MIR_T_U16,
				offsetof(TupleTableSlot, tts_flags),
				r_slot, 0, 1)));
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_OR,
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_reg_op(ctx, r_dtmp),
			MIR_new_int_op(ctx, TTS_FLAG_SLOW)));
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_mem_op(ctx, MIR_T_U16,
				offsetof(TupleTableSlot, tts_flags),
				r_slot, 0, 1),
			MIR_new_reg_op(ctx, r_dtmp)));

	pfree(att_labels);
	return true;
}

/*
 * Helper: emit MIR instruction to load the address of a step field into
 * a register.  In PIC mode (mir_shared_code_mode), computes the address
 * at runtime from r_steps; otherwise embeds the absolute address.
 *
 * 'ptr' is the absolute address (e.g., op->resvalue).
 * 'opno' is the step index, 'field' is the offsetof within ExprEvalStep.
 */
static void
mir_emit_step_addr(MIR_context_t ctx, MIR_item_t func_item,
				   MIR_reg_t dst, MIR_reg_t r_steps,
				   int opno, int64_t field_offset,
				   uint64_t abs_addr)
{
	if (mir_shared_code_mode)
	{
		int64_t	off = (int64_t) opno * (int64_t) sizeof(ExprEvalStep) + field_offset;
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_ADD,
				MIR_new_reg_op(ctx, dst),
				MIR_new_reg_op(ctx, r_steps),
				MIR_new_int_op(ctx, off)));
	}
	else
	{
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, dst),
				MIR_new_uint_op(ctx, abs_addr)));
	}
}

/*
 * Helper: emit MIR instruction to load a pointer field from a step into
 * a register.  In PIC mode, loads via r_steps + offset; otherwise embeds
 * the absolute address and loads through it.
 *
 * Used for pointer-valued fields like op->d.func.fcinfo_data.
 */
static void
mir_emit_step_load_ptr(MIR_context_t ctx, MIR_item_t func_item,
					   MIR_reg_t dst, MIR_reg_t r_steps,
					   int opno, int64_t field_offset,
					   uint64_t abs_addr)
{
	if (mir_shared_code_mode)
	{
		int64_t	off = (int64_t) opno * (int64_t) sizeof(ExprEvalStep) + field_offset;
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, dst),
				MIR_new_mem_op(ctx, MIR_T_P, off, r_steps, 0, 1)));
	}
	else
	{
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, dst),
				MIR_new_uint_op(ctx, abs_addr)));
	}
}

/*
 * Macros for common step-field address patterns.
 * MIR_STEP_ADDR: load address of steps[opno].field into dst
 * MIR_STEP_LOAD: load value of steps[opno].field (pointer-typed) into dst
 */
#define MIR_STEP_ADDR(dst, opno, field) \
	mir_emit_step_addr(ctx, func_item, dst, r_steps, opno, \
					   offsetof(ExprEvalStep, field), \
					   (uint64_t)(op->field))

#define MIR_STEP_ADDR_RAW(dst, opno, field_offset, abs_addr) \
	mir_emit_step_addr(ctx, func_item, dst, r_steps, opno, \
					   field_offset, abs_addr)

#define MIR_STEP_LOAD(dst, opno, field) \
	mir_emit_step_load_ptr(ctx, func_item, dst, r_steps, opno, \
						   offsetof(ExprEvalStep, field), \
						   (uint64_t)(op->field))

/*
 * Compile an expression using MIR.
 *
 * Generated function signature:
 *   Datum fn(ExprState *state, ExprContext *econtext, bool *isNull)
 */
static bool
mir_compile_expr(ExprState *state)
{
	PgJitterContext *jctx;
	MIR_context_t	ctx;
	MIR_module_t	m;
	MIR_item_t		func_item;
	MIR_func_t		f;
	ExprEvalStep   *steps;
	int				steps_len;
	instr_time		starttime, endtime;
	char			modname[32];
	MIR_reg_t		r_state, r_econtext, r_isnullp;
	MIR_reg_t		r_resvaluep, r_resnullp;
	MIR_reg_t		r_resultvals, r_resultnulls;
	MIR_reg_t		r_tmp1, r_tmp2, r_tmp3, r_slot;
	MIR_reg_t		r_steps;

	MIR_insn_t	   *step_labels;

	/* Prototypes and imports for function calls */
	MIR_item_t		proto_fallback, import_fallback;
	MIR_item_t		proto_getsomeattrs, import_getsomeattrs;
	MIR_item_t		import_deform_dispatch;
	MIR_item_t		proto_v1func, import_v1func;	/* per-step, see below */
	MIR_item_t		proto_makero, import_makero;
	MIR_type_t		res_type;
	int				shared_node_id = 0;
	int				shared_expr_idx = 0;

	if (!state->parent)
		return false;

	/* Skip JIT in parallel workers when mode is OFF */
	if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_OFF && IsParallelWorker())
		return false;

	/* Let PG's hand-optimized fast-path evalfuncs handle tiny expressions */
	if (expr_has_fast_path(state))
		return false;

	jctx = pg_jitter_get_context(state);

	/*
	 * Compute expression identity for shared code.
	 *
	 * Leader: creates DSM on first compile, writes code directly.
	 * Worker: attaches to DSM via GUC, looks up pre-compiled code.
	 */
	if (state->parent->state->es_jit_flags & PGJIT_EXPR)
	{
		pg_jitter_get_expr_identity(jctx, state,
									&shared_node_id, &shared_expr_idx);

		mir_shared_code_mode = (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED)
							&& state->parent->state->es_plannedstmt->parallelModeNeeded;

#if defined(__x86_64__) || defined(_M_X64)
		/*
		 * x86_64: MIR's code patterns aren't reliably handled by the
		 * relocation scanner yet (it expects MOVABS but MIR may use
		 * RIP-relative addressing). Disable shared code mode so
		 * workers compile locally instead of reusing leader code.
		 */
		mir_shared_code_mode = false;
#endif

		elog(DEBUG1, "pg_jitter[mir]: compile_expr node=%d expr=%d is_worker=%d "
			 "shared_mode=%d share_init=%d",
			 shared_node_id, shared_expr_idx, IsParallelWorker(),
			 mir_shared_code_mode,
			 jctx->share_state.initialized);

		/* Leader: create DSM on first compile */
		if (mir_shared_code_mode && !IsParallelWorker() &&
			!jctx->share_state.initialized)
			pg_jitter_init_shared_dsm(jctx);
	}

	/*
	 * Parallel worker: try to use pre-compiled code from the leader.
	 */
	if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED &&
		IsParallelWorker())
	{
		const void *code_bytes;
		Size		code_size;
		uint64		leader_dylib_ref;

		if (!jctx->share_state.initialized)
			pg_jitter_attach_shared_dsm(jctx);

		if (jctx->share_state.sjc &&
			pg_jitter_find_shared_code(jctx->share_state.sjc,
									   shared_node_id, shared_expr_idx,
									   &code_bytes, &code_size,
									   &leader_dylib_ref))
		{
			void   *handle;
			void   *code_ptr;

			handle = pg_jitter_copy_to_executable(code_bytes, code_size);
			if (handle)
			{
				uint64	worker_ref = (uint64)(uintptr_t) pg_jitter_fallback_step;
				int		npatched;

				npatched = pg_jitter_relocate_dylib_addrs(handle, code_size,
														  leader_dylib_ref,
														  worker_ref);

				code_ptr = pg_jitter_exec_code_ptr(handle);

				elog(DEBUG1, "pg_jitter[mir]: worker reused shared code "
					 "node=%d expr=%d (%zu bytes, patched=%d) code_ptr=%p "
					 "leader_ref=%lx worker_ref=%lx",
					 shared_node_id, shared_expr_idx, code_size,
					 npatched, code_ptr,
					 (unsigned long) leader_dylib_ref,
					 (unsigned long) worker_ref);

				pg_jitter_register_compiled(jctx, pg_jitter_exec_free, handle);
				pg_jitter_install_expr(state,
									  (ExprStateEvalFunc) code_ptr);

				mir_shared_code_mode = false;

				jctx->base.instr.created_functions++;
				return true;
			}

			elog(WARNING, "pg_jitter[mir]: failed to allocate executable memory "
				 "for shared code node=%d expr=%d, compiling locally",
				 shared_node_id, shared_expr_idx);
		}
		else
			elog(DEBUG1, "pg_jitter[mir]: worker did not find shared code "
				 "node=%d expr=%d, compiling locally",
				 shared_node_id, shared_expr_idx);
	}

	INSTR_TIME_SET_CURRENT(starttime);

	steps = state->steps;
	steps_len = state->steps_len;

	mir_name_counter = 0;

	ctx = mir_get_or_create_ctx(jctx);
	if (!ctx)
	{
		mir_shared_code_mode = false;
		return false;
	}

	snprintf(modname, sizeof(modname), "m_%d", mir_current_state->module_counter++);
	m = MIR_new_module(ctx, modname);

	/*
	 * Create prototypes and imports at module level (before the function).
	 */

	/* Proto for pg_jitter_fallback_step(state, op, econtext) -> int */
	{
		MIR_type_t res_type = MIR_T_I64;
		proto_fallback = MIR_new_proto(ctx, "p_fallback",
									   1, &res_type,
									   3, MIR_T_P, "s", MIR_T_P, "o", MIR_T_P, "e");
	}
	import_fallback = MIR_new_import(ctx, "fallback_step");

	/* Proto for slot_getsomeattrs_int(slot, last_var) -> void */
	proto_getsomeattrs = MIR_new_proto(ctx, "p_getsomeattrs",
										0, NULL,
										2, MIR_T_P, "sl", MIR_T_I32, "n");
	import_getsomeattrs = MIR_new_import(ctx, "getsomeattrs");

	/* Import for deform dispatch (same proto as getsomeattrs: void(P, I32)) */
	import_deform_dispatch = MIR_new_import(ctx, "deform_dispatch");

	/* Proto for V1 function: fn(FunctionCallInfo) -> Datum */
	res_type = MIR_T_I64;
	proto_v1func = MIR_new_proto(ctx, "p_v1func",
								  1, &res_type,
								  1, MIR_T_P, "fci");

	/* Proto for MakeExpandedObjectReadOnlyInternal(Datum) -> Datum */
	proto_makero = MIR_new_proto(ctx, "p_makero",
								 1, &res_type,
								 1, MIR_T_I64, "d");
	import_makero = MIR_new_import(ctx, "make_ro");

	/* Protos for direct native calls (1-arg and 2-arg, int64 args/ret) */
	MIR_item_t proto_direct1, proto_direct2;
	{
		MIR_type_t rt = MIR_T_I64;
		proto_direct1 = MIR_new_proto(ctx, "p_d1", 1, &rt, 1, MIR_T_I64, "a0");
		proto_direct2 = MIR_new_proto(ctx, "p_d2", 1, &rt, 2, MIR_T_I64, "a0", MIR_T_I64, "a1");
	}

	/* Proto for agg_trans helpers: (ExprState*, ExprEvalStep*) -> void */
	MIR_item_t proto_agg_helper;
	proto_agg_helper = MIR_new_proto(ctx, "p_aggh", 0, NULL,
									  2, MIR_T_P, "s", MIR_T_P, "o");

#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
	/* Proto + imports for presorted distinct: fn(AggState*, AggStatePerTrans) -> bool */
	MIR_item_t proto_presorted_distinct;
	MIR_item_t import_presorted_single, import_presorted_multi;
	{
		MIR_type_t rt = MIR_T_I64;
		proto_presorted_distinct = MIR_new_proto(ctx, "p_pdist", 1, &rt,
												  2, MIR_T_P, "a", MIR_T_P, "p");
	}
	import_presorted_single = MIR_new_import(ctx, "pdist_single");
	import_presorted_multi = MIR_new_import(ctx, "pdist_multi");
#endif

	/* Proto for 3-arg void calls: fn(state, op, econtext) -> void */
	MIR_item_t proto_3arg_void;
	proto_3arg_void = MIR_new_proto(ctx, "p_3v", 0, NULL,
									3, MIR_T_P, "s", MIR_T_P, "o", MIR_T_P, "e");

	/* Proto for 4-arg void calls: fn(state, op, econtext, slot) -> void */
	MIR_item_t proto_4arg_void;
	proto_4arg_void = MIR_new_proto(ctx, "p_4v", 0, NULL,
									4, MIR_T_P, "a", MIR_T_P, "b", MIR_T_P, "c", MIR_T_P, "d");

	/* Proto + imports for inline error handlers: void -> void (noreturn) */
	MIR_item_t proto_err_void;
	proto_err_void = MIR_new_proto(ctx, "p_err", 0, NULL, 0);
	MIR_item_t import_err_int4_overflow = MIR_new_import(ctx, "err_i4ov");
	MIR_item_t import_err_int8_overflow = MIR_new_import(ctx, "err_i8ov");
	MIR_item_t import_err_div_by_zero = MIR_new_import(ctx, "err_divz");

	/* Proto + imports for inline deform helpers */
	MIR_item_t proto_deform_varsize, import_deform_varsize;
	MIR_item_t proto_deform_strlen, import_deform_strlen;
	{
		MIR_type_t rt = MIR_T_I64;
		proto_deform_varsize = MIR_new_proto(ctx, "p_varsize",
			1, &rt, 1, MIR_T_P, "p");
		import_deform_varsize = MIR_new_import(ctx, "varsize_any");
		proto_deform_strlen = MIR_new_proto(ctx, "p_strlen",
			1, &rt, 1, MIR_T_P, "s");
		import_deform_strlen = MIR_new_import(ctx, "strlen");
	}

	/*
	 * Per-step imports for V1 function addresses.
	 * We need unique import names for each distinct fn_addr.
	 */
	MIR_item_t *step_fn_imports = palloc0(sizeof(MIR_item_t) * steps_len);
	MIR_item_t *step_direct_imports = palloc0(sizeof(MIR_item_t) * steps_len);
	MIR_item_t *ioc_in_imports = palloc0(sizeof(MIR_item_t) * steps_len);
	for (int i = 0; i < steps_len; i++)
	{
		ExprEvalStep *op = &steps[i];
		ExprEvalOp opcode = ExecEvalStepOp(state, op);

		if (opcode == EEOP_FUNCEXPR ||
			opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			|| opcode == EEOP_FUNCEXPR_STRICT_1
			|| opcode == EEOP_FUNCEXPR_STRICT_2
#endif
			)
		{
			const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);
			if (dfn && dfn->jit_fn)
			{
				/* Create import for the direct native function */
				char name[32];
				snprintf(name, sizeof(name), "dfn_%d", i);
				step_direct_imports[i] = MIR_new_import(ctx, name);
			}
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
			else if (!mir_shared_code_mode &&
					 dfn && dfn->jit_fn_name &&
					 mir_find_precompiled_fn(dfn->jit_fn_name))
			{
				/* Use MIR-precompiled function pointer (skip in shared mode:
				 * blob addresses are in separate mmap, outside relocation range) */
				char name[32];
				snprintf(name, sizeof(name), "dfn_%d", i);
				step_direct_imports[i] = MIR_new_import(ctx, name);
			}
#endif
			else
			{
				char name[32];
				snprintf(name, sizeof(name), "fn_%d", i);
				step_fn_imports[i] = MIR_new_import(ctx, name);
			}
		}
#ifdef HAVE_EEOP_HASHDATUM
		else if (opcode == EEOP_HASHDATUM_FIRST ||
				 opcode == EEOP_HASHDATUM_FIRST_STRICT ||
				 opcode == EEOP_HASHDATUM_NEXT32 ||
				 opcode == EEOP_HASHDATUM_NEXT32_STRICT)
		{
			PGFunction hash_fn = op->d.hashdatum.fn_addr;
			const JitDirectFn *dfn = jit_find_direct_fn(hash_fn);
			if (dfn && dfn->jit_fn)
			{
				char name[32];
				snprintf(name, sizeof(name), "dhfn_%d", i);
				step_direct_imports[i] = MIR_new_import(ctx, name);
			}
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
			else if (!mir_shared_code_mode &&
					 dfn && dfn->jit_fn_name &&
					 mir_find_precompiled_fn(dfn->jit_fn_name))
			{
				char name[32];
				snprintf(name, sizeof(name), "dhfn_%d", i);
				step_direct_imports[i] = MIR_new_import(ctx, name);
			}
#endif
			else
			{
				char name[32];
				snprintf(name, sizeof(name), "fn_%d", i);
				step_fn_imports[i] = MIR_new_import(ctx, name);
			}
		}
#endif /* HAVE_EEOP_HASHDATUM */
		else if (opcode == EEOP_HASHED_SCALARARRAYOP)
		{
			/* Import for fallback ExecEvalHashedScalarArrayOp */
			char name[32];
			snprintf(name, sizeof(name), "saop_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
				 opcode <= EEOP_AGG_PLAIN_TRANS_BYREF)
		{
			/* Pre-create import for agg_trans helper */
			char name[32];
			snprintf(name, sizeof(name), "ah_%d", i);
			step_direct_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_AGG_STRICT_DESERIALIZE ||
				 opcode == EEOP_AGG_DESERIALIZE)
		{
			/* Import for deserialize function: fcinfo->flinfo->fn_addr */
			char name[32];
			snprintf(name, sizeof(name), "dser_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_DISTINCT ||
				 opcode == EEOP_NOT_DISTINCT)
		{
			/* Import for equality function in DISTINCT/NOT_DISTINCT */
			char name[32];
			snprintf(name, sizeof(name), "fn_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_NULLIF)
		{
			/* Import for equality function in NULLIF */
			char name[32];
			snprintf(name, sizeof(name), "fn_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		/* Direct-call opcodes: 3-arg void, 2-arg void, PARAM_CALLBACK */
		else if (opcode == EEOP_FUNCEXPR_FUSAGE ||
				 opcode == EEOP_FUNCEXPR_STRICT_FUSAGE ||
				 opcode == EEOP_NULLTEST_ROWISNULL ||
				 opcode == EEOP_NULLTEST_ROWISNOTNULL ||
#ifdef HAVE_EEOP_PARAM_SET
				 opcode == EEOP_PARAM_SET ||
#endif
				 opcode == EEOP_ARRAYCOERCE ||
				 opcode == EEOP_FIELDSELECT ||
				 opcode == EEOP_FIELDSTORE_DEFORM ||
				 opcode == EEOP_FIELDSTORE_FORM ||
				 opcode == EEOP_CONVERT_ROWTYPE ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
				 opcode == EEOP_JSON_CONSTRUCTOR ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
				 opcode == EEOP_JSONEXPR_COERCION ||
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
				 opcode == EEOP_MERGE_SUPPORT_FUNC ||
#endif
				 opcode == EEOP_SUBPLAN ||
				 opcode == EEOP_WHOLEROW ||
				 opcode == EEOP_AGG_ORDERED_TRANS_DATUM ||
				 opcode == EEOP_AGG_ORDERED_TRANS_TUPLE ||
#ifdef HAVE_EEOP_IOCOERCE_SAFE
				 opcode == EEOP_IOCOERCE_SAFE ||
#endif
				 opcode == EEOP_SCALARARRAYOP ||
				 opcode == EEOP_SQLVALUEFUNCTION ||
				 opcode == EEOP_CURRENTOFEXPR ||
				 opcode == EEOP_NEXTVALUEEXPR ||
				 opcode == EEOP_ARRAYEXPR ||
				 opcode == EEOP_ROW ||
				 opcode == EEOP_MINMAX ||
				 opcode == EEOP_DOMAIN_NOTNULL ||
				 opcode == EEOP_DOMAIN_CHECK ||
				 opcode == EEOP_XMLEXPR ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
				 opcode == EEOP_IS_JSON ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
				 opcode == EEOP_JSONEXPR_COERCION_FINISH ||
#endif
				 opcode == EEOP_GROUPING_FUNC ||
				 opcode == EEOP_PARAM_CALLBACK ||
				 opcode == EEOP_PARAM_EXEC ||
				 opcode == EEOP_PARAM_EXTERN)
		{
			char name[32];
			snprintf(name, sizeof(name), "dc_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_INNER_SYSVAR ||
				 opcode == EEOP_OUTER_SYSVAR ||
				 opcode == EEOP_SCAN_SYSVAR
#ifdef HAVE_EEOP_OLD_NEW
				 || opcode == EEOP_OLD_SYSVAR
				 || opcode == EEOP_NEW_SYSVAR
#endif
				 )
		{
			char name[32];
			snprintf(name, sizeof(name), "sysvar_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_SBSREF_SUBSCRIPTS)
		{
			char name[32];
			snprintf(name, sizeof(name), "sbss_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_SBSREF_OLD ||
				 opcode == EEOP_SBSREF_ASSIGN ||
				 opcode == EEOP_SBSREF_FETCH)
		{
			char name[32];
			snprintf(name, sizeof(name), "sbsf_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_IOCOERCE)
		{
			char name[32];
			snprintf(name, sizeof(name), "ioc_out_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
			snprintf(name, sizeof(name), "ioc_in_%d", i);
			ioc_in_imports[i] = MIR_new_import(ctx, name);
		}
		else if (opcode == EEOP_ROWCOMPARE_STEP)
		{
			char name[32];
			snprintf(name, sizeof(name), "rcmp_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
#ifdef HAVE_EEOP_JSONEXPR
		else if (opcode == EEOP_JSONEXPR_PATH)
		{
			char name[32];
			snprintf(name, sizeof(name), "jpath_%d", i);
			step_fn_imports[i] = MIR_new_import(ctx, name);
		}
#endif
	}

	/*
	 * Create the JIT function.
	 */
	func_item = MIR_new_func(ctx, "jit_eval",
							  1, &res_type,
							  3, MIR_T_P, "state", MIR_T_P, "econtext", MIR_T_P, "isNull");
	f = func_item->u.func;

	/* Get parameter registers */
	r_state = MIR_reg(ctx, "state", f);
	r_econtext = MIR_reg(ctx, "econtext", f);
	r_isnullp = MIR_reg(ctx, "isNull", f);

	/* Create local registers */
	r_resvaluep = mir_new_reg(ctx, f, MIR_T_I64, "rvp");
	r_resnullp = mir_new_reg(ctx, f, MIR_T_I64, "rnp");
	r_resultvals = mir_new_reg(ctx, f, MIR_T_I64, "rvals");
	r_resultnulls = mir_new_reg(ctx, f, MIR_T_I64, "rnulls");
	r_tmp1 = mir_new_reg(ctx, f, MIR_T_I64, "t1");
	r_tmp2 = mir_new_reg(ctx, f, MIR_T_I64, "t2");
	r_tmp3 = mir_new_reg(ctx, f, MIR_T_I64, "t3");
	r_slot = mir_new_reg(ctx, f, MIR_T_I64, "sl");
	r_steps = mir_new_reg(ctx, f, MIR_T_I64, "stp");

	/*
	 * Prologue: cache frequently-used pointers.
	 */

	/* r_steps = state->steps (for steps-relative addressing in PIC mode) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_steps),
			MIR_new_mem_op(ctx, MIR_T_P,
				offsetof(ExprState, steps),
				r_state, 0, 1)));

	/* r_resvaluep = &state->resvalue */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_ADD,
			MIR_new_reg_op(ctx, r_resvaluep),
			MIR_new_reg_op(ctx, r_state),
			MIR_new_int_op(ctx, offsetof(ExprState, resvalue))));

	/* r_resnullp = &state->resnull */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_ADD,
			MIR_new_reg_op(ctx, r_resnullp),
			MIR_new_reg_op(ctx, r_state),
			MIR_new_int_op(ctx, offsetof(ExprState, resnull))));

	/* r_slot = state->resultslot (may be NULL for non-projecting exprs) */
	MIR_append_insn(ctx, func_item,
		MIR_new_insn(ctx, MIR_MOV,
			MIR_new_reg_op(ctx, r_slot),
			MIR_new_mem_op(ctx, MIR_T_P,
				offsetof(ExprState, resultslot),
				r_state, 0, 1)));

	{
		MIR_label_t skip_rs_label = MIR_new_label(ctx);

		/* if (r_slot == 0) goto skip_rs_label */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, skip_rs_label),
				MIR_new_reg_op(ctx, r_slot), MIR_new_int_op(ctx, 0)));

		/* r_resultvals = resultslot->tts_values */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, r_resultvals),
				MIR_new_mem_op(ctx, MIR_T_P,
					offsetof(TupleTableSlot, tts_values),
					r_slot, 0, 1)));

		/* r_resultnulls = resultslot->tts_isnull */
		MIR_append_insn(ctx, func_item,
			MIR_new_insn(ctx, MIR_MOV,
				MIR_new_reg_op(ctx, r_resultnulls),
				MIR_new_mem_op(ctx, MIR_T_P,
					offsetof(TupleTableSlot, tts_isnull),
					r_slot, 0, 1)));

		MIR_append_insn(ctx, func_item, skip_rs_label);
	}

	/*
	 * Create labels for each step.
	 */
	step_labels = palloc(sizeof(MIR_insn_t) * steps_len);
	for (int i = 0; i < steps_len; i++)
		step_labels[i] = MIR_new_label(ctx);

	/*
	 * Emit code for each step.
	 */
	for (int opno = 0; opno < steps_len; opno++)
	{
		ExprEvalStep   *op = &steps[opno];
		ExprEvalOp		opcode = ExecEvalStepOp(state, op);

		MIR_append_insn(ctx, func_item, step_labels[opno]);

		switch (opcode)
		{
			/*
			 * ---- DONE ----
			 */
			case EEOP_DONE_RETURN:
			{
				/* r_tmp1 = state->resvalue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64,
							offsetof(ExprState, resvalue),
							r_state, 0, 1)));
				/* r_tmp2 = state->resnull (byte) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(ExprState, resnull),
							r_state, 0, 1)));
				/* *isNull = resnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_isnullp, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				/* return resvalue */
				MIR_append_insn(ctx, func_item,
					MIR_new_ret_insn(ctx, 1,
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

#ifdef HAVE_EEOP_DONE_SPLIT
			case EEOP_DONE_NO_RETURN:
			{
				MIR_append_insn(ctx, func_item,
					MIR_new_ret_insn(ctx, 1,
						MIR_new_int_op(ctx, 0)));
				break;
			}
#endif

			/*
			 * ---- FETCHSOME ----
			 */
			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_FETCHSOME:
			case EEOP_NEW_FETCHSOME:
#endif
			{
				MIR_insn_t	skip_label = MIR_new_label(ctx);
				int64_t		soff = mir_slot_offset(opcode);
				bool		deform_emitted = false;

				/* r_slot = econtext->ecxt_*tuple */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_mem_op(ctx, MIR_T_P, soff,
							r_econtext, 0, 1)));

				/* r_tmp1 = slot->tts_nvalid (int16) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I16,
							offsetof(TupleTableSlot, tts_nvalid),
							r_slot, 0, 1)));

				/* if tts_nvalid >= last_var, skip */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BGE,
						MIR_new_label_op(ctx, skip_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, op->d.fetch.last_var)));

				/* Try inline deform */
				if (op->d.fetch.fixed && op->d.fetch.known_desc &&
					(jctx->base.flags & PGJIT_DEFORM))
				{
					deform_emitted = mir_emit_deform_inline(
						ctx, func_item, f,
						op->d.fetch.known_desc,
						op->d.fetch.kind,
						op->d.fetch.last_var,
						opcode,
						r_econtext, r_slot,
						proto_deform_varsize,
						import_deform_varsize,
						proto_deform_strlen,
						import_deform_strlen);
				}

				if (!deform_emitted)
				{
					/*
					 * Call deform dispatch: JIT-compiles deform via sljit
					 * (loop-based for wide, unrolled for narrow) and caches.
					 */
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_getsomeattrs),
							MIR_new_ref_op(ctx, import_deform_dispatch),
							MIR_new_reg_op(ctx, r_slot),
							MIR_new_int_op(ctx, op->d.fetch.last_var)));
				}

				MIR_append_insn(ctx, func_item, skip_label);
				break;
			}

			/*
			 * ---- VAR ----
			 */
			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_VAR:
			case EEOP_NEW_VAR:
#endif
			{
				int			attnum = op->d.var.attnum;
				int64_t		soff = mir_slot_offset(opcode);

				/* r_slot = econtext->ecxt_*tuple */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_mem_op(ctx, MIR_T_P, soff,
							r_econtext, 0, 1)));

				/* r_tmp1 = slot->tts_values (pointer) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_values),
							r_slot, 0, 1)));
				/* r_tmp2 = values[attnum] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							attnum * (int64_t) sizeof(Datum),
							r_tmp1, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp1 = slot->tts_isnull (pointer) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_isnull),
							r_slot, 0, 1)));
				/* r_tmp2 = isnull[attnum] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							attnum * (int64_t) sizeof(bool),
							r_tmp1, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- ASSIGN_*_VAR ----
			 */
			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_ASSIGN_OLD_VAR:
			case EEOP_ASSIGN_NEW_VAR:
#endif
			{
				int			attnum = op->d.assign_var.attnum;
				int			resultnum = op->d.assign_var.resultnum;
				int64_t		soff = mir_slot_offset(opcode);

				/* r_slot = source slot */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_slot),
						MIR_new_mem_op(ctx, MIR_T_P, soff,
							r_econtext, 0, 1)));

				/* Load value from source */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_values),
							r_slot, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							attnum * (int64_t) sizeof(Datum),
							r_tmp1, 0, 1)));
				/* Store to result values */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64,
							resultnum * (int64_t) sizeof(Datum),
							r_resultvals, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* Load null from source */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(TupleTableSlot, tts_isnull),
							r_slot, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							attnum * (int64_t) sizeof(bool),
							r_tmp1, 0, 1)));
				/* Store to result nulls */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							resultnum * (int64_t) sizeof(bool),
							r_resultnulls, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- ASSIGN_TMP / ASSIGN_TMP_MAKE_RO ----
			 */
			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
			{
				int			resultnum = op->d.assign_tmp.resultnum;

				/* r_tmp1 = state->resvalue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64,
							offsetof(ExprState, resvalue),
							r_state, 0, 1)));
				/* r_tmp2 = state->resnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(ExprState, resnull),
							r_state, 0, 1)));

				/* Store null first */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							resultnum * (int64_t) sizeof(bool),
							r_resultnulls, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
				{
					MIR_insn_t	skip_ro = MIR_new_label(ctx);
					MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "mro");

					/* If null, skip MakeReadOnly */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNE,
							MIR_new_label_op(ctx, skip_ro),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));

					/* r_tmp1 = MakeExpandedObjectReadOnlyInternal(r_tmp1) */
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_makero),
							MIR_new_ref_op(ctx, import_makero),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_tmp1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_reg_op(ctx, r_ret)));

					MIR_append_insn(ctx, func_item, skip_ro);
				}

				/* Store value */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64,
							resultnum * (int64_t) sizeof(Datum),
							r_resultvals, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- CONST ----
			 */
			case EEOP_CONST:
			{
				/* *op->resvalue = constval.value */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, (int64_t) op->d.constval.value)));

				/* *op->resnull = constval.isnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, op->d.constval.isnull ? 1 : 0)));
				break;
			}

			/*
			 * ---- FUNCEXPR / FUNCEXPR_STRICT ----
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
				MIR_insn_t	done_label = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "fret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "fci");

				if (opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
					|| opcode == EEOP_FUNCEXPR_STRICT_1
					|| opcode == EEOP_FUNCEXPR_STRICT_2
#endif
					)
				{
					/* Set resnull = true */
					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));

					/*
					 * Batched null check: OR all isnull flags, single branch.
					 */
					MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
					if (nargs > 1 && nargs <= 4)
					{
						int64_t null_off_0 =
							(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_mem_op(ctx, MIR_T_U8, null_off_0,
									r_fci, 0, 1)));
						for (int argno = 1; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull -
										  (char *)fcinfo);
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_MOV,
									MIR_new_reg_op(ctx, r_tmp2),
									MIR_new_mem_op(ctx, MIR_T_U8, null_off,
										r_fci, 0, 1)));
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_OR,
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_reg_op(ctx, r_tmp2)));
						}
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNE,
								MIR_new_label_op(ctx, done_label),
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_int_op(ctx, 0)));
					}
					else
					{
						for (int argno = 0; argno < nargs; argno++)
						{
							int64_t null_off =
								(int64_t)((char *)&fcinfo->args[argno].isnull -
										  (char *)fcinfo);
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_MOV,
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_mem_op(ctx, MIR_T_U8, null_off,
										r_fci, 0, 1)));
							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_BNE,
									MIR_new_label_op(ctx, done_label),
									MIR_new_reg_op(ctx, r_tmp1),
									MIR_new_int_op(ctx, 0)));
						}
					}
				}

				/*
				 * Try direct native call — bypasses fcinfo entirely.
				 * Dispatch order: inline → direct call → fcinfo fallback.
				 */
				{
				const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);

				if (dfn && dfn->inline_op != JIT_INLINE_NONE)
				{
					/*
					 * TIER 0 — INLINE: emit the operation as MIR
					 * instructions, no function call at all.
					 */
					MIR_reg_t a0 = mir_new_reg(ctx, f, MIR_T_I64, "ia0");
					MIR_reg_t a1 = mir_new_reg(ctx, f, MIR_T_I64, "ia1");
					int64_t val_off_0 =
						(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
					int64_t val_off_1 =
						(int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
					MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, a0),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off_0,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, a1),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off_1,
								r_fci, 0, 1)));

					switch ((JitInlineOp) dfn->inline_op)
					{
					/* ---- int32 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT4_ADD:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_ADDOS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						/* Sign-extend 32→64 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_SUB:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_SUBOS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_MUL:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MULOS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_DIV:
					{
						MIR_insn_t not_zero = MIR_new_label(ctx);
						MIR_insn_t not_minmax = MIR_new_label(ctx);
						MIR_insn_t not_neg1 = MIR_new_label(ctx);
						/* Check divisor == 0 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_zero),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, 0)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_div_by_zero)));
						MIR_append_insn(ctx, func_item, not_zero);
						/* Check INT32_MIN / -1 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_minmax),
								MIR_new_reg_op(ctx, a0),
								MIR_new_int_op(ctx, (int32_t) PG_INT32_MIN)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_neg1),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, -1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int4_overflow)));
						MIR_append_insn(ctx, func_item, not_neg1);
						MIR_append_insn(ctx, func_item, not_minmax);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_DIVS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						break;
					}
					case JIT_INLINE_INT4_MOD:
					{
						MIR_insn_t not_zero = MIR_new_label(ctx);
						MIR_insn_t not_minmax = MIR_new_label(ctx);
						MIR_insn_t zero_result = MIR_new_label(ctx);
						MIR_insn_t after = MIR_new_label(ctx);
						/* Check divisor == 0 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_zero),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, 0)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_div_by_zero)));
						MIR_append_insn(ctx, func_item, not_zero);
						/* Check INT32_MIN % -1 → 0 */
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNES,
								MIR_new_label_op(ctx, not_minmax),
								MIR_new_reg_op(ctx, a0),
								MIR_new_int_op(ctx, (int32_t) PG_INT32_MIN)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BEQS,
								MIR_new_label_op(ctx, zero_result),
								MIR_new_reg_op(ctx, a1),
								MIR_new_int_op(ctx, -1)));
						MIR_append_insn(ctx, func_item, not_minmax);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MODS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_ret)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_JMP,
								MIR_new_label_op(ctx, after)));
						MIR_append_insn(ctx, func_item, zero_result);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_int_op(ctx, 0)));
						MIR_append_insn(ctx, func_item, after);
						break;
					}
					/* ---- int64 arithmetic (overflow-checked) ---- */
					case JIT_INLINE_INT8_ADD:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_ADDO,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int8_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						break;
					}
					case JIT_INLINE_INT8_SUB:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_SUBO,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int8_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						break;
					}
					case JIT_INLINE_INT8_MUL:
					{
						MIR_insn_t ok = MIR_new_label(ctx);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MULO,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNO,
								MIR_new_label_op(ctx, ok)));
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 2,
								MIR_new_ref_op(ctx, proto_err_void),
								MIR_new_ref_op(ctx, import_err_int8_overflow)));
						MIR_append_insn(ctx, func_item, ok);
						break;
					}
					/* ---- int32 comparison ---- */
					case JIT_INLINE_INT4_EQ:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EQS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_NE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_NES,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_LT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LTS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_LE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LES,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_GT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GTS,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT4_GE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GES,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					/* ---- int64 comparison ---- */
					case JIT_INLINE_INT8_EQ:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EQ,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_NE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_NE,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_LT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LT,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_LE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_LE,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_GT:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GT,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					case JIT_INLINE_INT8_GE:
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_GE,
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, a0),
								MIR_new_reg_op(ctx, a1)));
						break;
					default:
						Assert(false);
						break;
					}

					/* *op->resvalue = r_ret */
					MIR_STEP_LOAD(r_tmp3, opno, resvalue);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_reg_op(ctx, r_ret)));

					/* *op->resnull = false */
					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else if (dfn && step_direct_imports[opno])
				{
					/*
					 * Direct native call — either from dfn->jit_fn or
					 * MIR-precompiled function pointer. Both have the
					 * same calling convention (int32/int64 args → int64 ret).
					 */
					/* Load fcinfo once, then load all args from offsets */
					MIR_reg_t r_args[4];
					MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
					for (int i = 0; i < dfn->nargs; i++)
					{
						char aname[16];
						snprintf(aname, sizeof(aname), "da%d_%d", i, opno);
						r_args[i] = mir_new_reg(ctx, f, MIR_T_I64, aname);
						int64_t val_off = (int64_t)((char *)&fcinfo->args[i].value -
													(char *)fcinfo);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_args[i]),
								MIR_new_mem_op(ctx, MIR_T_I64, val_off,
									r_fci, 0, 1)));
					}

					/* Direct call */
					MIR_item_t d_proto = (dfn->nargs == 1)
						? proto_direct1 : proto_direct2;
					if (dfn->nargs == 1)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 4,
								MIR_new_ref_op(ctx, d_proto),
								MIR_new_ref_op(ctx, step_direct_imports[opno]),
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_args[0])));
					}
					else
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_call_insn(ctx, 5,
								MIR_new_ref_op(ctx, d_proto),
								MIR_new_ref_op(ctx, step_direct_imports[opno]),
								MIR_new_reg_op(ctx, r_ret),
								MIR_new_reg_op(ctx, r_args[0]),
								MIR_new_reg_op(ctx, r_args[1])));
					}

					/* *op->resvalue = result */
					MIR_STEP_LOAD(r_tmp3, opno, resvalue);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_reg_op(ctx, r_ret)));

					/* *op->resnull = false */
					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else
				{
				/* Fallback: generic fcinfo path */

				/* fcinfo->isnull = false (caller must clear before V1 call) */
				MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* Call fn_addr(fcinfo) → result */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));

				/* *op->resvalue = result */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resnull = fcinfo->isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				} /* end else fallback */
				} /* end direct-call dispatch */

				MIR_append_insn(ctx, func_item, done_label);
				break;
			}

			/*
			 * ---- BOOL_AND_STEP ----
			 */
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_LAST:
			{
				MIR_insn_t	null_handler = MIR_new_label(ctx);
				MIR_insn_t	cont = MIR_new_label(ctx);

				if (opcode == EEOP_BOOL_AND_STEP_FIRST)
				{
					/* *anynull = false */
					MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}

				/* Load resnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				/* If null, go to null_handler */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_handler),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Not null: check if false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

				/* If false (value == 0), short-circuit */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, step_labels[op->d.boolexpr.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Jump over null handler */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, cont)));

				/* Null handler: set *anynull = true */
				MIR_append_insn(ctx, func_item, null_handler);
				MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));

				MIR_append_insn(ctx, func_item, cont);

				/* On last step: if anynull, set result to NULL */
				if (opcode == EEOP_BOOL_AND_STEP_LAST)
				{
					MIR_insn_t	no_anynull = MIR_new_label(ctx);

					MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, no_anynull),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));

					/* Set resnull = true, resvalue = 0 */
					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));
					MIR_STEP_LOAD(r_tmp3, opno, resvalue);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));

					MIR_append_insn(ctx, func_item, no_anynull);
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
				MIR_insn_t	null_handler = MIR_new_label(ctx);
				MIR_insn_t	cont = MIR_new_label(ctx);

				if (opcode == EEOP_BOOL_OR_STEP_FIRST)
				{
					MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}

				/* Load resnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_handler),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Not null: check if true */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

				/* If true (value != 0), short-circuit */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[op->d.boolexpr.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, cont)));

				/* Null handler */
				MIR_append_insn(ctx, func_item, null_handler);
				MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));

				MIR_append_insn(ctx, func_item, cont);

				if (opcode == EEOP_BOOL_OR_STEP_LAST)
				{
					MIR_insn_t	no_anynull = MIR_new_label(ctx);

					MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, no_anynull),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));

					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));
					MIR_STEP_LOAD(r_tmp3, opno, resvalue);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));

					MIR_append_insn(ctx, func_item, no_anynull);
				}
				break;
			}

			/*
			 * ---- BOOL_NOT_STEP ----
			 */
			case EEOP_BOOL_NOT_STEP:
			{
				/* Load resvalue, negate, store back */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				/* r_tmp1 = (r_tmp1 == 0) ? 1 : 0 */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_EQ,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- QUAL ----
			 */
			case EEOP_QUAL:
			{
				MIR_insn_t	qualfail = MIR_new_label(ctx);
				MIR_insn_t	cont = MIR_new_label(ctx);

				/* Check null */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, qualfail),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Check false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, qualfail),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Pass: continue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, cont)));

				/* Qual fail: set resvalue=0, resnull=false, jump to done */
				MIR_append_insn(ctx, func_item, qualfail);
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.qualexpr.jumpdone])));

				MIR_append_insn(ctx, func_item, cont);
				break;
			}

			/*
			 * ---- JUMP variants ----
			 */
			case EEOP_JUMP:
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone])));
				break;

			case EEOP_JUMP_IF_NULL:
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				break;

			case EEOP_JUMP_IF_NOT_NULL:
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				break;

			case EEOP_JUMP_IF_NOT_TRUE:
			{
				/* Jump if null OR false */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				/* If null, jump */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				/* If false, jump */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- NULLTEST ----
			 */
			case EEOP_NULLTEST_ISNULL:
			{
				/* resvalue = resnull ? 1 : 0; resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				/* Store as Datum */
				MIR_STEP_LOAD(r_tmp2, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				/* resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			case EEOP_NULLTEST_ISNOTNULL:
			{
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				/* r_tmp1 = !r_tmp1 */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_EQ,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				/* Store as Datum */
				MIR_STEP_LOAD(r_tmp2, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				/* resnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

#ifdef HAVE_EEOP_HASHDATUM
			/*
			 * ---- HASHDATUM_SET_INITVAL ----
			 */
			case EEOP_HASHDATUM_SET_INITVAL:
			{
				/* *op->resvalue = op->d.hashdatum_initvalue.init_value */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, (int64_t) op->d.hashdatum_initvalue.init_value)));

				/* *op->resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_FIRST (non-strict) ----
			 */
			case EEOP_HASHDATUM_FIRST:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	store_zero = MIR_new_label(ctx);
				MIR_insn_t	store_result = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");

				/* r_fci = fcinfo */
				MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, jump to store_zero */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, store_zero),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Call hash function (direct or fcinfo) */
				if (step_direct_imports[opno])
				{
					/* Direct: load arg from fcinfo->args[0].value */
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(FunctionCallInfoBaseData, isnull),
								r_fci, 0, 1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_v1func),
							MIR_new_ref_op(ctx, step_fn_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_fci)));
				}

				/* r_tmp1 = r_ret */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_ret)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, store_result)));

				/* store_zero: r_tmp1 = 0 */
				MIR_append_insn(ctx, func_item, store_zero);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* store_result: *op->resvalue = r_tmp1, *op->resnull = false */
				MIR_append_insn(ctx, func_item, store_result);
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));

				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_FIRST_STRICT ----
			 */
			case EEOP_HASHDATUM_FIRST_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	null_path = MIR_new_label(ctx);
				MIR_insn_t	after_null = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");

				/* r_fci = fcinfo */
				MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, jump to null_path */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_path),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				if (step_direct_imports[opno])
				{
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = call fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));
				}

				/* *op->resvalue = r_ret */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, after_null)));

				/* null_path: set resnull=1, resvalue=0, jump to jumpdone */
				MIR_append_insn(ctx, func_item, null_path);
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.hashdatum.jumpdone])));

				MIR_append_insn(ctx, func_item, after_null);
				break;
			}

			/*
			 * ---- HASHDATUM_NEXT32 (non-strict, rotate + XOR) ----
			 */
			case EEOP_HASHDATUM_NEXT32:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				NullableDatum *iresult = op->d.hashdatum.iresult;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	skip_hash = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");
				MIR_reg_t	r_hash = mir_new_reg(ctx, f, MIR_T_I64, "hash");

				/* Load existing hash: r_hash = iresult->value (as U32) */
				MIR_STEP_LOAD(r_tmp1, opno, d.hashdatum.iresult);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_mem_op(ctx, MIR_T_U32,
							offsetof(NullableDatum, value),
							r_tmp1, 0, 1)));

				/* Rotate left 1: r_hash = (r_hash << 1) | (r_hash >> 31) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_LSHS,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_URSHS,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 31)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_fci = fcinfo */
				MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, skip hash call */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, skip_hash),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				if (step_direct_imports[opno])
				{
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = call fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));
				}

				/* r_hash = r_hash XOR r_ret (32-bit) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_XORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_ret)));

				/* skip_hash: store result */
				MIR_append_insn(ctx, func_item, skip_hash);

				/* *op->resvalue = r_hash (already zero-extended from 32-bit ops) */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_hash)));

				/* *op->resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- HASHDATUM_NEXT32_STRICT ----
			 */
			case EEOP_HASHDATUM_NEXT32_STRICT:
			{
				FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
				NullableDatum *iresult = op->d.hashdatum.iresult;
				int64_t isnull_off = (int64_t)((char *)&fcinfo->args[0].isnull -
											   (char *)fcinfo);
				MIR_insn_t	null_path = MIR_new_label(ctx);
				MIR_insn_t	after_null = MIR_new_label(ctx);
				MIR_reg_t	r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
				MIR_reg_t	r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");
				MIR_reg_t	r_hash = mir_new_reg(ctx, f, MIR_T_I64, "hash");

				/* r_fci = fcinfo */
				MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, isnull_off,
							r_fci, 0, 1)));

				/* if isnull, jump to null_path */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, null_path),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Load existing hash: r_hash = iresult->value (as U32) */
				MIR_STEP_LOAD(r_tmp1, opno, d.hashdatum.iresult);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_mem_op(ctx, MIR_T_U32,
							offsetof(NullableDatum, value),
							r_tmp1, 0, 1)));

				/* Rotate left 1: r_hash = (r_hash << 1) | (r_hash >> 31) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_LSHS,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_URSHS,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_int_op(ctx, 31)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp2)));

				if (step_direct_imports[opno])
				{
					int64_t val_off = (int64_t)((char *)&fcinfo->args[0].value -
												(char *)fcinfo);
					MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_harg),
							MIR_new_mem_op(ctx, MIR_T_I64, val_off,
								r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_direct1),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_harg)));
				}
				else
				{
					/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = call fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));
				}

				/* r_hash = r_hash XOR r_ret (32-bit) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_XORS,
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_hash),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resvalue = r_hash */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_hash)));

				/* *op->resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, after_null)));

				/* null_path: set resnull=1, resvalue=0, jump to jumpdone */
				MIR_append_insn(ctx, func_item, null_path);
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.hashdatum.jumpdone])));

				MIR_append_insn(ctx, func_item, after_null);
				break;
			}
#endif /* HAVE_EEOP_HASHDATUM */

			/*
			 * ---- AGG_PLAIN_TRANS (all 6 variants) ----
			 * Call the specific agg_trans helper directly instead of
			 * going through fallback_step's switch dispatch.
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

				AggState   *aggstate = castNode(AggState, state->parent);
				AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
				int			setoff = op->d.agg_trans.setoff;
				int			transno = op->d.agg_trans.transno;
				FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
				PGFunction	fn_addr = fcinfo->flinfo->fn_addr;
				ExprContext *aggcontext = op->d.agg_trans.aggcontext;

				MIR_insn_t	end_label = MIR_new_label(ctx);
				MIR_reg_t	r_pergroup = mir_new_reg(ctx, f, MIR_T_I64, "pg");
				MIR_reg_t	r_aggst = mir_new_reg(ctx, f, MIR_T_I64, "agst");

				/* Compute pergroup at runtime: aggstate->all_pergroups[setoff] + transno */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_aggst),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprState, parent),
							r_state, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_pergroup),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(AggState, all_pergroups),
							r_aggst, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_pergroup),
						MIR_new_mem_op(ctx, MIR_T_P,
							setoff * (int64_t)sizeof(AggStatePerGroup),
							r_pergroup, 0, 1)));
				if (transno != 0)
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_pergroup),
							MIR_new_reg_op(ctx, r_pergroup),
							MIR_new_int_op(ctx,
								transno * (int64_t)sizeof(AggStatePerGroupData))));

				/* INIT check */
				if (is_init)
				{
					MIR_insn_t no_init = MIR_new_label(ctx);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, noTransValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, no_init),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
					/* noTransValue=true: call helper(state, op) which handles
					 * ExecAggInitGroup internally. */
					MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_agg_helper),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_state),
							MIR_new_reg_op(ctx, r_tmp1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, end_label)));
					MIR_append_insn(ctx, func_item, no_init);
				}

				/* STRICT check: skip if transValueIsNull */
				if (is_strict)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNE,
							MIR_new_label_op(ctx, end_label),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
				}

				/*
				 * Transition function dispatch — inline hot aggs.
				 */
				if (!is_byref &&
					(fn_addr == int8inc || fn_addr == int8inc_any))
				{
					/* COUNT: transValue += 1 (int64, overflow-checked) */
					MIR_insn_t ok = MIR_new_label(ctx);
					MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADDO,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNO,
							MIR_new_label_op(ctx, ok)));
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 2,
							MIR_new_ref_op(ctx, proto_err_void),
							MIR_new_ref_op(ctx, import_err_int8_overflow)));
					MIR_append_insn(ctx, func_item, ok);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1),
							MIR_new_reg_op(ctx, r_tv)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else if (!is_byref && fn_addr == int4_sum)
				{
					/* SUM(int4): transValue += (int64)arg1 */
					MIR_insn_t arg_not_null = MIR_new_label(ctx);
					MIR_insn_t trans_not_null = MIR_new_label(ctx);
					MIR_insn_t after_sum = MIR_new_label(ctx);
					MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "sfci");
					MIR_reg_t r_arg1 = mir_new_reg(ctx, f, MIR_T_I64, "arg1");
					int64_t isnull1_off = (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					/* Load fcinfo = pertrans->transfn_fcinfo (runtime) */
					if (mir_shared_code_mode)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_fci),
								MIR_new_mem_op(ctx, MIR_T_P,
									(int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
									(int64_t)offsetof(ExprEvalStep, d.agg_trans.pertrans),
									r_steps, 0, 1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_fci),
								MIR_new_mem_op(ctx, MIR_T_P,
									offsetof(AggStatePerTransData, transfn_fcinfo),
									r_fci, 0, 1)));
					}
					else
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_fci),
								MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					/* Check arg1 isnull */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8, isnull1_off, r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, arg_not_null),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, end_label)));

					MIR_append_insn(ctx, func_item, arg_not_null);
					/* Load arg1, sign-extend int32→int64 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_arg1),
							MIR_new_mem_op(ctx, MIR_T_I64, val1_off, r_fci, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EXT32,
							MIR_new_reg_op(ctx, r_arg1),
							MIR_new_reg_op(ctx, r_arg1)));

					/* Check transValueIsNull */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, trans_not_null),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));

					/* First non-null: transValue = (int64)arg1 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1),
							MIR_new_reg_op(ctx, r_arg1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, after_sum)));

					/* Normal: transValue += (int64)arg1 */
					MIR_append_insn(ctx, func_item, trans_not_null);
					{
						MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tv),
								MIR_new_mem_op(ctx, MIR_T_I64,
									offsetof(AggStatePerGroupData, transValue),
									r_pergroup, 0, 1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_ADD,
								MIR_new_reg_op(ctx, r_tv),
								MIR_new_reg_op(ctx, r_tv),
								MIR_new_reg_op(ctx, r_arg1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_mem_op(ctx, MIR_T_I64,
									offsetof(AggStatePerGroupData, transValue),
									r_pergroup, 0, 1),
								MIR_new_reg_op(ctx, r_tv)));
					}
					MIR_append_insn(ctx, func_item, after_sum);
				}
				else if (!is_byref &&
						 (fn_addr == int4smaller || fn_addr == int4larger))
				{
					/* MIN/MAX(int4) */
					bool is_min = (fn_addr == int4smaller);
					MIR_insn_t skip_update = MIR_new_label(ctx);
					MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
					MIR_reg_t r_nv = mir_new_reg(ctx, f, MIR_T_I64, "nv");
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1)));
					/* Load fcinfo = pertrans->transfn_fcinfo (runtime) */
					if (mir_shared_code_mode)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_mem_op(ctx, MIR_T_P,
									(int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
									(int64_t)offsetof(ExprEvalStep, d.agg_trans.pertrans),
									r_steps, 0, 1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_mem_op(ctx, MIR_T_P,
									offsetof(AggStatePerTransData, transfn_fcinfo),
									r_tmp1, 0, 1)));
					}
					else
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_nv),
							MIR_new_mem_op(ctx, MIR_T_I64, val1_off, r_tmp1, 0, 1)));
					/* Sign-extend both */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EXT32,
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_reg_op(ctx, r_tv)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EXT32,
							MIR_new_reg_op(ctx, r_nv),
							MIR_new_reg_op(ctx, r_nv)));
					/* MIN: skip if tv <= nv; MAX: skip if tv >= nv */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, is_min ? MIR_BLE : MIR_BGE,
							MIR_new_label_op(ctx, skip_update),
							MIR_new_reg_op(ctx, r_tv),
							MIR_new_reg_op(ctx, r_nv)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1),
							MIR_new_reg_op(ctx, r_nv)));
					MIR_append_insn(ctx, func_item, skip_update);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
				}
				else if (is_byref &&
						 (fn_addr == int4_avg_accum || fn_addr == int2_avg_accum))
				{
					/* AVG(int4/int2): in-place update of {count, sum} */
					#define MIR_INT8_TRANS_OFFSET 24
					MIR_reg_t r_td = mir_new_reg(ctx, f, MIR_T_I64, "td");
					MIR_reg_t r_cnt = mir_new_reg(ctx, f, MIR_T_I64, "cnt");
					MIR_reg_t r_sum = mir_new_reg(ctx, f, MIR_T_I64, "sum");
					MIR_reg_t r_a1 = mir_new_reg(ctx, f, MIR_T_I64, "a1v");
					int64_t val1_off = (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

					/* r_td = transValue + 24 = pointer to Int8TransTypeData */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_td),
							MIR_new_mem_op(ctx, MIR_T_I64,
								offsetof(AggStatePerGroupData, transValue),
								r_pergroup, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_td),
							MIR_new_reg_op(ctx, r_td),
							MIR_new_int_op(ctx, MIR_INT8_TRANS_OFFSET)));

					/* count++ */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_cnt),
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_td, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_cnt),
							MIR_new_reg_op(ctx, r_cnt),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_td, 0, 1),
							MIR_new_reg_op(ctx, r_cnt)));

					/* Load arg1, sign-extend */
					/* Load fcinfo = pertrans->transfn_fcinfo (runtime) */
					if (mir_shared_code_mode)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_mem_op(ctx, MIR_T_P,
									(int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
									(int64_t)offsetof(ExprEvalStep, d.agg_trans.pertrans),
									r_steps, 0, 1)));
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_mem_op(ctx, MIR_T_P,
									offsetof(AggStatePerTransData, transfn_fcinfo),
									r_tmp1, 0, 1)));
					}
					else
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_reg_op(ctx, r_tmp1),
								MIR_new_uint_op(ctx, (uint64_t) fcinfo)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_a1),
							MIR_new_mem_op(ctx, MIR_T_I64, val1_off, r_tmp1, 0, 1)));
					if (fn_addr == int4_avg_accum)
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT32,
								MIR_new_reg_op(ctx, r_a1),
								MIR_new_reg_op(ctx, r_a1)));
					else
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_EXT16,
								MIR_new_reg_op(ctx, r_a1),
								MIR_new_reg_op(ctx, r_a1)));

					/* sum += arg1 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_sum),
							MIR_new_mem_op(ctx, MIR_T_I64,
								(int64_t) offsetof(MirInt8TransTypeData, sum),
								r_td, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_ADD,
							MIR_new_reg_op(ctx, r_sum),
							MIR_new_reg_op(ctx, r_sum),
							MIR_new_reg_op(ctx, r_a1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								(int64_t) offsetof(MirInt8TransTypeData, sum),
								r_td, 0, 1),
							MIR_new_reg_op(ctx, r_sum)));

					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								offsetof(AggStatePerGroupData, transValueIsNull),
								r_pergroup, 0, 1),
							MIR_new_int_op(ctx, 0)));
					#undef MIR_INT8_TRANS_OFFSET
				}
				else
				{
					/* Generic fallback: call helper function */
					MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 4,
							MIR_new_ref_op(ctx, proto_agg_helper),
							MIR_new_ref_op(ctx, step_direct_imports[opno]),
							MIR_new_reg_op(ctx, r_state),
							MIR_new_reg_op(ctx, r_tmp1)));
				}

				MIR_append_insn(ctx, func_item, end_label);
				break;
			}

			/*
			 * ---- HASHED_SCALARARRAYOP (compile-time binary search) ----
			 *
			 * For constant byval IN lists: extract values at compile time,
			 * sort them, and emit an inline binary search tree (~5 CMP+branch
			 * levels for 20 elements).
			 * For non-constant or byref types: fall back to C function call.
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
								 * Simple insertion sort -- at most 64
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
					 *
					 * Strategy: iterative using a work stack. At each
					 * node, compare scalar against the median value:
					 *   - equal  -> found  (BEQ to lbl_found)
					 *   - less   -> left subtree (BLTS to left label)
					 *   - greater -> right subtree (fall-through)
					 *   - leaf miss -> JMP to lbl_not_found
					 */
					MIR_insn_t lbl_found = MIR_new_label(ctx);
					MIR_insn_t lbl_not_found = MIR_new_label(ctx);
					MIR_insn_t lbl_null_result = MIR_new_label(ctx);
					MIR_insn_t lbl_done = MIR_new_label(ctx);

					int64_t off_arg0_value =
						(int64_t)((char *)&fcinfo->args[0].value -
								  (char *)fcinfo);
					int64_t off_arg0_isnull =
						(int64_t)((char *)&fcinfo->args[0].isnull -
								  (char *)fcinfo);

					MIR_reg_t r_scalar =
						mir_new_reg(ctx, f, MIR_T_I64, "bscalar");

					/*
					 * Step 1: Check scalar not NULL (strict function).
					 */
					MIR_STEP_LOAD(r_tmp1, opno, d.hashedscalararrayop.fcinfo_data);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_mem_op(ctx, MIR_T_U8,
								off_arg0_isnull, r_tmp1, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNE,
							MIR_new_label_op(ctx, lbl_null_result),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));

					/*
					 * Step 2: Load scalar value into r_scalar.
					 * (r_tmp1 still = fcinfo)
					 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_scalar),
							MIR_new_mem_op(ctx, MIR_T_I64,
								off_arg0_value, r_tmp1, 0, 1)));

					/*
					 * Step 3: Emit binary search tree.
					 *
					 * Work stack approach: each item is a [lo,hi]
					 * range. For each range pick median, emit:
					 *   BEQ lbl_found  (if scalar == median)
					 *   BLTS lbl_left  (if scalar < median)
					 *   fall-through to right subtree
					 *
					 * Push left first (processed last = emitted
					 * later = BLTS target), then right (processed
					 * next = fall-through).
					 *
					 * Empty range: JMP lbl_not_found
					 * Single element: BEQ lbl_found + JMP lbl_not_found
					 */
					{
						struct {
							int lo, hi;
							MIR_insn_t entry_label;
						} work[128];
						int work_top = 0;

						/* Push initial range */
						work[work_top].lo = 0;
						work[work_top].hi = nvals - 1;
						work[work_top].entry_label = NULL;
						work_top++;

						while (work_top > 0)
						{
							int lo, hi, mid;
							MIR_insn_t lbl_entry;
							MIR_insn_t lbl_left;

							work_top--;
							lo = work[work_top].lo;
							hi = work[work_top].hi;
							lbl_entry = work[work_top].entry_label;

							/* Place entry label for this node */
							if (lbl_entry)
								MIR_append_insn(ctx, func_item,
									lbl_entry);

							if (lo > hi)
							{
								/* Empty range -> not found */
								MIR_append_insn(ctx, func_item,
									MIR_new_insn(ctx, MIR_JMP,
										MIR_new_label_op(ctx,
											lbl_not_found)));
								continue;
							}

							if (lo == hi)
							{
								/* Single element: BEQ + JMP */
								MIR_append_insn(ctx, func_item,
									MIR_new_insn(ctx, MIR_BEQ,
										MIR_new_label_op(ctx,
											lbl_found),
										MIR_new_reg_op(ctx,
											r_scalar),
										MIR_new_int_op(ctx,
											(int64_t)
											sorted_vals[lo])));
								MIR_append_insn(ctx, func_item,
									MIR_new_insn(ctx, MIR_JMP,
										MIR_new_label_op(ctx,
											lbl_not_found)));
								continue;
							}

							/* Pick median */
							mid = lo + (hi - lo) / 2;

							/*
							 * BEQ lbl_found  (== median)
							 * BLTS lbl_left  (< median)
							 * fall-through   (> median -> right)
							 */
							lbl_left = MIR_new_label(ctx);

							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_BEQ,
									MIR_new_label_op(ctx,
										lbl_found),
									MIR_new_reg_op(ctx,
										r_scalar),
									MIR_new_int_op(ctx,
										(int64_t)
										sorted_vals[mid])));

							MIR_append_insn(ctx, func_item,
								MIR_new_insn(ctx, MIR_BLTS,
									MIR_new_label_op(ctx,
										lbl_left),
									MIR_new_reg_op(ctx,
										r_scalar),
									MIR_new_int_op(ctx,
										(int64_t)
										sorted_vals[mid])));

							/*
							 * Push left first (processed last),
							 * then right (processed next =
							 * fall-through).
							 */
							work[work_top].lo = lo;
							work[work_top].hi = mid - 1;
							work[work_top].entry_label = lbl_left;
							work_top++;

							work[work_top].lo = mid + 1;
							work[work_top].hi = hi;
							work[work_top].entry_label = NULL;
							work_top++;
						}
					}

					/* ---- Found ---- */
					MIR_append_insn(ctx, func_item, lbl_found);
					MIR_STEP_LOAD(r_tmp3, opno, resvalue);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx,
								inclause ? 1 : 0)));
					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, lbl_done)));

					/* ---- Not found ---- */
					MIR_append_insn(ctx, func_item, lbl_not_found);
					if (array_has_nulls)
					{
						/*
						 * Array had NULLs -- result is NULL
						 * (indeterminate for strict equality).
						 */
						MIR_STEP_LOAD(r_tmp3, opno, resvalue);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_mem_op(ctx, MIR_T_I64,
									0, r_tmp3, 0, 1),
								MIR_new_int_op(ctx, 0)));
						MIR_STEP_LOAD(r_tmp3, opno, resnull);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_mem_op(ctx, MIR_T_U8,
									0, r_tmp3, 0, 1),
								MIR_new_int_op(ctx, 1)));
					}
					else
					{
						/* Definitive not-found result */
						MIR_STEP_LOAD(r_tmp3, opno, resvalue);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_mem_op(ctx, MIR_T_I64,
									0, r_tmp3, 0, 1),
								MIR_new_int_op(ctx,
									inclause ? 0 : 1)));
						MIR_STEP_LOAD(r_tmp3, opno, resnull);
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_MOV,
								MIR_new_mem_op(ctx, MIR_T_U8,
									0, r_tmp3, 0, 1),
								MIR_new_int_op(ctx, 0)));
					}
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, lbl_done)));

					/* ---- Null scalar ---- */
					MIR_append_insn(ctx, func_item, lbl_null_result);
					MIR_STEP_LOAD(r_tmp3, opno, resvalue);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64,
								0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 0)));
					MIR_STEP_LOAD(r_tmp3, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8,
								0, r_tmp3, 0, 1),
							MIR_new_int_op(ctx, 1)));

					/* ---- Done ---- */
					MIR_append_insn(ctx, func_item, lbl_done);

					pfree(sorted_vals);
				}
				else
				{
					/*
					 * Non-constant array or byref type -- fall back
					 * to C function call.
					 */
					if (sorted_vals)
						pfree(sorted_vals);

					MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 5,
							MIR_new_ref_op(ctx, proto_3arg_void),
							MIR_new_ref_op(ctx, step_fn_imports[opno]),
							MIR_new_reg_op(ctx, r_state),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_reg_op(ctx, r_econtext)));
				}
#else /* PG14: no inclause/saop -- always use fallback */
				{
					MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
					MIR_append_insn(ctx, func_item,
						MIR_new_call_insn(ctx, 5,
							MIR_new_ref_op(ctx, proto_3arg_void),
							MIR_new_ref_op(ctx, step_fn_imports[opno]),
							MIR_new_reg_op(ctx, r_state),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_reg_op(ctx, r_econtext)));
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
				MIR_item_t fn_import =
					(opcode == EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
					? import_presorted_single
					: import_presorted_multi;

				MIR_reg_t r_aggst = mir_new_reg(ctx, f, MIR_T_I64, "pdst");

				/* aggstate = state->parent */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_aggst),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprState, parent),
							r_state, 0, 1)));

				/* pertrans = op->d.agg_presorted_distinctcheck.pertrans */
				MIR_STEP_LOAD(r_tmp1, opno,
					d.agg_presorted_distinctcheck.pertrans);

				/* r_tmp2 = fn(aggstate, pertrans) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 5,
						MIR_new_ref_op(ctx, proto_presorted_distinct),
						MIR_new_ref_op(ctx, fn_import),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_aggst),
						MIR_new_reg_op(ctx, r_tmp1)));

				/* If result == 0 (not distinct), jump to jumpdistinct */
				if (jumpdistinct >= 0 && jumpdistinct < steps_len)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, step_labels[jumpdistinct]),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));
				}
				break;
			}
#endif /* HAVE_EEOP_AGG_PRESORTED_DISTINCT */

			/*
			 * ---- BOOLTEST_IS_TRUE ----
			 * If null: resvalue=false, resnull=false. Else keep.
			 */
			case EEOP_BOOLTEST_IS_TRUE:
			{
				MIR_insn_t not_null_label = MIR_new_label(ctx);

				/* r_tmp1 = *op->resnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				/* if (!resnull) goto not_null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, not_null_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* null path: *resvalue = 0 (false), *resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* not_null: keep resvalue as-is */
				MIR_append_insn(ctx, func_item, not_null_label);
				break;
			}

			/*
			 * ---- BOOLTEST_IS_NOT_TRUE ----
			 * If null: resvalue=true, resnull=false. Else invert resvalue.
			 */
			case EEOP_BOOLTEST_IS_NOT_TRUE:
			{
				MIR_insn_t not_null_label = MIR_new_label(ctx);
				MIR_insn_t done_label = MIR_new_label(ctx);

				/* r_tmp1 = *op->resnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				/* if (!resnull) goto not_null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, not_null_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* null path: *resvalue = 1 (true), *resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, done_label)));

				/* not_null: *resvalue = !*resvalue */
				MIR_append_insn(ctx, func_item, not_null_label);
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_EQ,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));

				MIR_append_insn(ctx, func_item, done_label);
				break;
			}

			/*
			 * ---- BOOLTEST_IS_FALSE ----
			 * If null: resvalue=false, resnull=false. Else invert resvalue.
			 */
			case EEOP_BOOLTEST_IS_FALSE:
			{
				MIR_insn_t not_null_label = MIR_new_label(ctx);
				MIR_insn_t done_label = MIR_new_label(ctx);

				/* r_tmp1 = *op->resnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				/* if (!resnull) goto not_null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, not_null_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* null path: *resvalue = 0 (false), *resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, done_label)));

				/* not_null: *resvalue = !*resvalue */
				MIR_append_insn(ctx, func_item, not_null_label);
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_EQ,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));

				MIR_append_insn(ctx, func_item, done_label);
				break;
			}

			/*
			 * ---- BOOLTEST_IS_NOT_FALSE ----
			 * If null: resvalue=true, resnull=false. Else keep.
			 */
			case EEOP_BOOLTEST_IS_NOT_FALSE:
			{
				MIR_insn_t not_null_label = MIR_new_label(ctx);

				/* r_tmp1 = *op->resnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

				/* if (!resnull) goto not_null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, not_null_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* null path: *resvalue = 1 (true), *resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* not_null: keep resvalue as-is */
				MIR_append_insn(ctx, func_item, not_null_label);
				break;
			}

			/*
			 * ---- AGGREF ----
			 * resvalue = econtext->ecxt_aggvalues[aggno]
			 * resnull  = econtext->ecxt_aggnulls[aggno]
			 */
			case EEOP_AGGREF:
			{
				int aggno = op->d.aggref.aggno;

				/* r_tmp1 = econtext->ecxt_aggvalues */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprContext, ecxt_aggvalues),
							r_econtext, 0, 1)));
				/* r_tmp2 = aggvalues[aggno] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							aggno * (int64_t) sizeof(Datum),
							r_tmp1, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp1 = econtext->ecxt_aggnulls */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprContext, ecxt_aggnulls),
							r_econtext, 0, 1)));
				/* r_tmp2 = aggnulls[aggno] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							aggno * (int64_t) sizeof(bool),
							r_tmp1, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- WINDOW_FUNC ----
			 * wfuncno read at RUNTIME from op->d.window_func.wfstate->wfuncno
			 * (assigned after expression compilation).
			 */
			case EEOP_WINDOW_FUNC:
			{
				MIR_reg_t r_wfno = mir_new_reg(ctx, f, MIR_T_I64, "wfno");

				/* r_tmp1 = op->d.window_func.wfstate */
				MIR_STEP_LOAD(r_tmp1, opno, d.window_func.wfstate);
				/* r_wfno = wfstate->wfuncno (int, sign-extend) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_wfno),
						MIR_new_mem_op(ctx, MIR_T_I32,
							offsetof(WindowFuncExprState, wfuncno),
							r_tmp1, 0, 1)));

				/* r_tmp1 = econtext->ecxt_aggvalues */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprContext, ecxt_aggvalues),
							r_econtext, 0, 1)));
				/* r_tmp2 = wfuncno * sizeof(Datum) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_LSH,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_wfno),
						MIR_new_int_op(ctx, 3)));  /* *8 for sizeof(Datum) */
				/* r_tmp1 = aggvalues + offset */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ADD,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp2)));
				/* r_tmp2 = *r_tmp1 (Datum) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp1 = econtext->ecxt_aggnulls */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprContext, ecxt_aggnulls),
							r_econtext, 0, 1)));
				/* r_tmp1 = aggnulls + wfuncno (bool is 1 byte, no shift) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_ADD,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_wfno)));
				/* r_tmp2 = *r_tmp1 (bool) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- CASE_TESTVAL ----
			 * resvalue = *op->d.casetest.value
			 * resnull  = *op->d.casetest.isnull
			 */
			case EEOP_CASE_TESTVAL:
			{
				/* r_tmp1 = op->d.casetest.value (pointer) */
				MIR_STEP_LOAD(r_tmp1, opno, d.casetest.value);
				/* r_tmp2 = *r_tmp1 (Datum) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp1 = op->d.casetest.isnull (pointer) */
				MIR_STEP_LOAD(r_tmp1, opno, d.casetest.isnull);
				/* r_tmp2 = *r_tmp1 (bool) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

#ifdef HAVE_EEOP_TESTVAL_EXT
			/*
			 * ---- CASE_TESTVAL_EXT ----
			 * resvalue = econtext->caseValue_datum
			 * resnull  = econtext->caseValue_isNull
			 */
			case EEOP_CASE_TESTVAL_EXT:
			{
				/* r_tmp2 = econtext->caseValue_datum */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							offsetof(ExprContext, caseValue_datum),
							r_econtext, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp2 = econtext->caseValue_isNull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(ExprContext, caseValue_isNull),
							r_econtext, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}
#endif /* HAVE_EEOP_TESTVAL_EXT */

			/*
			 * ---- DOMAIN_TESTVAL ----
			 * Same as CASE_TESTVAL (uses same union d.casetest).
			 */
			case EEOP_DOMAIN_TESTVAL:
			{
				/* r_tmp1 = op->d.casetest.value (pointer) */
				MIR_STEP_LOAD(r_tmp1, opno, d.casetest.value);
				/* r_tmp2 = *r_tmp1 (Datum) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp1 = op->d.casetest.isnull (pointer) */
				MIR_STEP_LOAD(r_tmp1, opno, d.casetest.isnull);
				/* r_tmp2 = *r_tmp1 (bool) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

#ifdef HAVE_EEOP_TESTVAL_EXT
			/*
			 * ---- DOMAIN_TESTVAL_EXT ----
			 * resvalue = econtext->domainValue_datum
			 * resnull  = econtext->domainValue_isNull
			 */
			case EEOP_DOMAIN_TESTVAL_EXT:
			{
				/* r_tmp2 = econtext->domainValue_datum */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64,
							offsetof(ExprContext, domainValue_datum),
							r_econtext, 0, 1)));
				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* r_tmp2 = econtext->domainValue_isNull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(ExprContext, domainValue_isNull),
							r_econtext, 0, 1)));
				/* *op->resnull = r_tmp2 */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}
#endif /* HAVE_EEOP_TESTVAL_EXT */

			/*
			 * ---- MAKE_READONLY ----
			 * If *op->d.make_readonly.isnull: resnull=true, copy *value.
			 * Else: resvalue = MakeExpandedObjectReadOnlyInternal(*value),
			 *       resnull = false.
			 */
			case EEOP_MAKE_READONLY:
			{
				MIR_insn_t skip_label = MIR_new_label(ctx);
				MIR_reg_t r_val = mir_new_reg(ctx, f, MIR_T_I64, "mro_v");
				MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "mro_r");

				/* r_tmp1 = op->d.make_readonly.isnull (pointer) */
				MIR_STEP_LOAD(r_tmp1, opno, d.make_readonly.isnull);
				/* r_tmp2 = *isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));

				/* *op->resnull = isnull */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* if isnull, skip MakeReadOnly call */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, skip_label),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_int_op(ctx, 0)));

				/* Not null: r_val = *op->d.make_readonly.value */
				MIR_STEP_LOAD(r_tmp1, opno, d.make_readonly.value);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_val),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));

				/* r_ret = MakeExpandedObjectReadOnlyInternal(r_val) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_makero),
						MIR_new_ref_op(ctx, import_makero),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_val)));

				/* *op->resvalue = r_ret */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				MIR_append_insn(ctx, func_item, skip_label);
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
				int nargs = op->d.agg_strict_input_check.nargs;
				int jumpnull = op->d.agg_strict_input_check.jumpnull;
				int64_t isnull_off0 = (int64_t) offsetof(NullableDatum, isnull);
				int64_t nd_size = (int64_t) sizeof(NullableDatum);

				/* r_tmp1 = op->d.agg_strict_input_check.args */
				MIR_STEP_LOAD(r_tmp1, opno, d.agg_strict_input_check.args);

				for (int argno = 0; argno < nargs; argno++)
				{
					/* r_tmp2 = args[argno].isnull */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_mem_op(ctx, MIR_T_U8,
								argno * nd_size + isnull_off0,
								r_tmp1, 0, 1)));
					/* if isnull, jump to jumpnull */
					if (jumpnull >= 0 && jumpnull < steps_len)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNE,
								MIR_new_label_op(ctx, step_labels[jumpnull]),
								MIR_new_reg_op(ctx, r_tmp2),
								MIR_new_int_op(ctx, 0)));
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
				int nargs = op->d.agg_strict_input_check.nargs;
				int jumpnull = op->d.agg_strict_input_check.jumpnull;

				/* r_tmp1 = op->d.agg_strict_input_check.nulls */
				MIR_STEP_LOAD(r_tmp1, opno, d.agg_strict_input_check.nulls);

				for (int argno = 0; argno < nargs; argno++)
				{
					/* r_tmp2 = nulls[argno] */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_mem_op(ctx, MIR_T_U8,
								argno * (int64_t) sizeof(bool),
								r_tmp1, 0, 1)));
					/* if null, jump to jumpnull */
					if (jumpnull >= 0 && jumpnull < steps_len)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BNE,
								MIR_new_label_op(ctx, step_labels[jumpnull]),
								MIR_new_reg_op(ctx, r_tmp2),
								MIR_new_int_op(ctx, 0)));
					}
				}
				break;
			}

			/*
			 * ---- AGG_PLAIN_PERGROUP_NULLCHECK ----
			 * Load all_pergroups[setoff], jump to jumpnull if NULL.
			 */
			case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
			{
				int setoff = op->d.agg_plain_pergroup_nullcheck.setoff;
				int jumpnull = op->d.agg_plain_pergroup_nullcheck.jumpnull;

				/* r_tmp1 = state->parent (AggState*) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(ExprState, parent),
							r_state, 0, 1)));
				/* r_tmp1 = aggstate->all_pergroups */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							offsetof(AggState, all_pergroups),
							r_tmp1, 0, 1)));
				/* r_tmp1 = all_pergroups[setoff] */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_P,
							setoff * (int64_t) sizeof(AggStatePerGroup),
							r_tmp1, 0, 1)));
				/* if NULL, jump to jumpnull */
				if (jumpnull >= 0 && jumpnull < steps_len)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, step_labels[jumpnull]),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
				}
				break;
			}

			/*
			 * ---- AGG_STRICT_DESERIALIZE ----
			 * If args[0].isnull, jump to jumpnull.
			 * Then call fcinfo->flinfo->fn_addr(fcinfo), store result.
			 */
			case EEOP_AGG_STRICT_DESERIALIZE:
			{
				FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
				int jumpnull = op->d.agg_deserialize.jumpnull;
				int64_t null0_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "dsfci");
				MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "dsret");

				/* r_fci = op->d.agg_deserialize.fcinfo_data */
				MIR_STEP_LOAD(r_fci, opno, d.agg_deserialize.fcinfo_data);

				/* r_tmp1 = fcinfo->args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, null0_off,
							r_fci, 0, 1)));

				/* if null, jump to jumpnull */
				if (jumpnull >= 0 && jumpnull < steps_len)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BNE,
							MIR_new_label_op(ctx, step_labels[jumpnull]),
							MIR_new_reg_op(ctx, r_tmp1),
							MIR_new_int_op(ctx, 0)));
				}

				/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));

				/* *op->resvalue = r_ret */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resnull = fcinfo->isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- AGG_DESERIALIZE ----
			 * Call fcinfo->flinfo->fn_addr(fcinfo), store result. No jump.
			 */
			case EEOP_AGG_DESERIALIZE:
			{
				FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
				MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "adfci");
				MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "adret");

				/* r_fci = op->d.agg_deserialize.fcinfo_data */
				MIR_STEP_LOAD(r_fci, opno, d.agg_deserialize.fcinfo_data);

				/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* r_ret = fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));

				/* *op->resvalue = r_ret */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));

				/* *op->resnull = fcinfo->isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- DISTINCT / NOT_DISTINCT ----
			 * 3-way null logic + equality function call.
			 * If null flags differ → DISTINCT=true, NOT_DISTINCT=false
			 * If both null       → DISTINCT=false, NOT_DISTINCT=true
			 * If neither null    → call equality fn, invert for DISTINCT
			 */
			case EEOP_DISTINCT:
			case EEOP_NOT_DISTINCT:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int64_t null0_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				int64_t null1_off =
					(int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
				MIR_insn_t one_null_label = MIR_new_label(ctx);
				MIR_insn_t both_null_label = MIR_new_label(ctx);
				MIR_insn_t end_label = MIR_new_label(ctx);
				MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "dfci");
				MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "dret");
				MIR_reg_t r_n0 = mir_new_reg(ctx, f, MIR_T_I64, "dn0");
				MIR_reg_t r_n1 = mir_new_reg(ctx, f, MIR_T_I64, "dn1");

				/* r_fci = op->d.func.fcinfo_data */
				MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);

				/* r_n0 = args[0].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_n0),
						MIR_new_mem_op(ctx, MIR_T_U8, null0_off,
							r_fci, 0, 1)));
				/* r_n1 = args[1].isnull */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_n1),
						MIR_new_mem_op(ctx, MIR_T_U8, null1_off,
							r_fci, 0, 1)));

				/* if (n0 != n1) goto one_null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, one_null_label),
						MIR_new_reg_op(ctx, r_n0),
						MIR_new_reg_op(ctx, r_n1)));

				/* n0 == n1: if (n0 != 0) goto both_null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, both_null_label),
						MIR_new_reg_op(ctx, r_n0),
						MIR_new_int_op(ctx, 0)));

				/* Neither null: call equality fn */
				/* fcinfo->isnull = false */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));
				/* r_ret = fn_addr(fcinfo) */
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));

				/* For DISTINCT: invert result (resvalue = !result) */
				if (opcode == EEOP_DISTINCT)
				{
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_EQ,
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_reg_op(ctx, r_ret),
							MIR_new_int_op(ctx, 0)));
				}

				/* *op->resvalue = r_ret, *op->resnull = false */
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_ret)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, end_label)));

				/* one_null: nulls differ → DISTINCT=true, NOT_DISTINCT=false */
				MIR_append_insn(ctx, func_item, one_null_label);
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx,
							(opcode == EEOP_DISTINCT) ? 1 : 0)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, end_label)));

				/* both_null: both null → DISTINCT=false, NOT_DISTINCT=true */
				MIR_append_insn(ctx, func_item, both_null_label);
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx,
							(opcode == EEOP_DISTINCT) ? 0 : 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* end: all paths converge */
				MIR_append_insn(ctx, func_item, end_label);
				break;
			}

			/*
			 * ---- NULLIF ----
			 * NULLIF(a,b): if a is null → return null.
			 * If b is null → return a's value (not null).
			 * Both non-null → call equality fn.
			 *   If !fcinfo->isnull && result is true → return null.
			 *   Else → return a's value.
			 */
			case EEOP_NULLIF:
			{
				FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
				int64_t null0_off =
					(int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
				int64_t null1_off =
					(int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
				int64_t val0_off =
					(int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
				MIR_insn_t a_null_label = MIR_new_label(ctx);
				MIR_insn_t b_null_label = MIR_new_label(ctx);
				MIR_insn_t not_equal_label = MIR_new_label(ctx);
				MIR_insn_t end_label = MIR_new_label(ctx);
				MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "nfci");
				MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "nret");

				/* r_fci = op->d.func.fcinfo_data */
				MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);

				/* Check if arg0 is null */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, null0_off,
							r_fci, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, a_null_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Check if arg1 is null → skip to return_a */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8, null1_off,
							r_fci, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, b_null_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* Both non-null: call equality function */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_reg_op(ctx, r_fci)));

				/* If fcinfo->isnull, treat as not equal */
				MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_U8,
							offsetof(FunctionCallInfoBaseData, isnull),
							r_fci, 0, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, not_equal_label),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, 0)));

				/* If result == false (0), not equal */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, not_equal_label),
						MIR_new_reg_op(ctx, r_ret),
						MIR_new_int_op(ctx, 0)));

				/* Equal: return null — *op->resnull = true */
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, end_label)));

				/* not_equal / b_null: return a's value with resnull=false */
				MIR_append_insn(ctx, func_item, not_equal_label);
				MIR_append_insn(ctx, func_item, b_null_label);
				MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64, val0_off,
							r_fci, 0, 1)));
				MIR_STEP_LOAD(r_tmp3, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, end_label)));

				/* a_null: *op->resnull = true */
				MIR_append_insn(ctx, func_item, a_null_label);
				MIR_STEP_LOAD(r_tmp3, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 1)));

				/* end: all paths converge */
				MIR_append_insn(ctx, func_item, end_label);
				break;
			}

			/*
			 * ---- DIRECT CALL: 3-arg void ----
			 * fn(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
			 */
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
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 5,
						MIR_new_ref_op(ctx, proto_3arg_void),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));
				break;
			}

			/*
			 * ---- DIRECT CALL: 2-arg void ----
			 * fn(ExprState *state, ExprEvalStep *op)
			 */
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
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_agg_helper),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1)));
				break;
			}

			/*
			 * ---- PARAM_CALLBACK: indirect fn pointer ----
			 * op->d.cparam.paramfunc(state, op, econtext)
			 */
			case EEOP_PARAM_CALLBACK:
			{
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 5,
						MIR_new_ref_op(ctx, proto_3arg_void),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));
				break;
			}

			/*
			 * ---- PARAM_EXEC / PARAM_EXTERN ----
			 * Direct call: fn(state, op, econtext) -> void
			 */
			case EEOP_PARAM_EXEC:
			case EEOP_PARAM_EXTERN:
			{
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 5,
						MIR_new_ref_op(ctx, proto_3arg_void),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));
				break;
			}

			/*
			 * ---- SYSVAR ----
			 * Direct call to ExecEvalSysVar(state, op, econtext, slot).
			 * 4-arg call; slot loaded from econtext at compile-time offset.
			 */
			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
#ifdef HAVE_EEOP_OLD_NEW
			case EEOP_OLD_SYSVAR:
			case EEOP_NEW_SYSVAR:
#endif
			{
				int slot_offset;

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

				/* r_tmp2 = econtext->ecxt_*tuple (slot) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_P, slot_offset,
							r_econtext, 0, 1)));

				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 6,
						MIR_new_ref_op(ctx, proto_4arg_void),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext),
						MIR_new_reg_op(ctx, r_tmp2)));
				break;
			}

			/*
			 * ---- SBSREF_OLD / SBSREF_ASSIGN / SBSREF_FETCH ----
			 * Indirect call: op->d.sbsref.subscriptfunc(state, op, econtext)
			 */
			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
			{
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 5,
						MIR_new_ref_op(ctx, proto_3arg_void),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));
				break;
			}

			/*
			 * ---- SBSREF_SUBSCRIPTS ----
			 * Call subscriptfunc(state, op, econtext) -> bool.
			 * If false (0), jump to jumpdone.
			 */
			case EEOP_SBSREF_SUBSCRIPTS:
			{
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 6,
						MIR_new_ref_op(ctx, proto_fallback),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_tmp2),  /* return value */
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));

				/* If false (0), jump to jumpdone */
				{
					int jumpdone = op->d.sbsref_subscript.jumpdone;
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, step_labels[jumpdone]),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));
				}
				break;
			}

			/*
			 * ---- IOCOERCE ----
			 * Two function calls: output fn -> cstring -> input fn.
			 * If *resnull, skip entirely.
			 */
			case EEOP_IOCOERCE:
			{
				FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

				MIR_label_t skip_null = MIR_new_label(ctx);

				/* Load *op->resnull (pointer to bool) */
				MIR_STEP_LOAD(r_tmp1, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
				/* If resnull != 0, skip */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, skip_null),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_int_op(ctx, 0)));

				/* Load *op->resvalue */
				MIR_STEP_LOAD(r_tmp1, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));

				/* fcinfo_out->args[0].value = resvalue */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_out->args[0].value);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* fcinfo_out->args[0].isnull = false */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_out->args[0].isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* fcinfo_out->isnull = false */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_out->isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* Call output fn: ret = fn_addr_out(fcinfo_out) */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) fcinfo_out);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_tmp3)));

				/* r_tmp2 now holds cstring result (Datum) */
				/* fcinfo_in->args[0].value = r_tmp2 */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_in->args[0].value);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* fcinfo_in->args[0].isnull = false */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_in->args[0].isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* fcinfo_in->isnull = false */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_in->isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* Call input fn: ret = fn_addr_in(fcinfo_in) */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) fcinfo_in);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, ioc_in_imports[opno]),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_tmp3)));

				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp1, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* *op->resnull = fcinfo_in->isnull */
				MIR_STEP_ADDR_RAW(r_tmp3, opno, 0, (uint64_t) &fcinfo_in->isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
				MIR_STEP_LOAD(r_tmp1, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				MIR_append_insn(ctx, func_item, skip_null);
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

				if (op->d.rowcompare_step.finfo->fn_strict)
				{
					/* Check args[0].isnull || args[1].isnull */
					MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) &fcinfo->args[0].isnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
					MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) &fcinfo->args[1].isnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_OR,
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_reg_op(ctx, r_tmp3)));

					MIR_label_t not_null = MIR_new_label(ctx);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, not_null),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));

					/* Null path: *resnull = true, jump to jumpnull */
					MIR_STEP_LOAD(r_tmp1, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, step_labels[jnull])));

					MIR_append_insn(ctx, func_item, not_null);
				}

				/* fcinfo->isnull = false */
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) &fcinfo->isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* Call fn_addr(fcinfo) -> Datum */
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) fcinfo);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 4,
						MIR_new_ref_op(ctx, proto_v1func),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_tmp1)));

				/* *op->resvalue = r_tmp2 */
				MIR_STEP_LOAD(r_tmp1, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
						MIR_new_reg_op(ctx, r_tmp2)));

				/* If fcinfo->isnull, *resnull = true, jump to jumpnull */
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) &fcinfo->isnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp3),
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
				{
					MIR_label_t not_null2 = MIR_new_label(ctx);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BEQ,
							MIR_new_label_op(ctx, not_null2),
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_int_op(ctx, 0)));
					MIR_STEP_LOAD(r_tmp1, opno, resnull);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
							MIR_new_int_op(ctx, 1)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, step_labels[jnull])));
					MIR_append_insn(ctx, func_item, not_null2);
				}

				/* *resnull = false */
				MIR_STEP_LOAD(r_tmp1, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
						MIR_new_int_op(ctx, 0)));

				/* If int32(r_tmp2) != 0, jump to jumpdone */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BNE,
						MIR_new_label_op(ctx, step_labels[jdone]),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_int_op(ctx, 0)));
				break;
			}

			/*
			 * ---- ROWCOMPARE_FINAL ----
			 * Read int32 result from *op->resvalue, apply comparison,
			 * store boolean result.
			 */
			case EEOP_ROWCOMPARE_FINAL:
			{
				CompareType cmptype = op->d.rowcompare_final.cmptype;

				/* Load int32 cmpresult from *op->resvalue */
				MIR_STEP_LOAD(r_tmp1, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_mem_op(ctx, MIR_T_I32, 0, r_tmp1, 0, 1)));

				/* result = (cmpresult <cmp> 0) ? 1 : 0 */
				{
					MIR_label_t true_label = MIR_new_label(ctx);
					MIR_label_t end_label = MIR_new_label(ctx);

					MIR_insn_code_t cmp_code;
					switch (cmptype)
					{
						case COMPARE_LT: cmp_code = MIR_BLT; break;
						case COMPARE_LE: cmp_code = MIR_BLE; break;
						case COMPARE_GE: cmp_code = MIR_BGE; break;
						case COMPARE_GT: cmp_code = MIR_BGT; break;
						default: cmp_code = MIR_BLT; break;
					}
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, cmp_code,
							MIR_new_label_op(ctx, true_label),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));

					/* false path: result = 0 */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_int_op(ctx, 0)));
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_JMP,
							MIR_new_label_op(ctx, end_label)));

					/* true path: result = 1 */
					MIR_append_insn(ctx, func_item, true_label);
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_reg_op(ctx, r_tmp3),
							MIR_new_int_op(ctx, 1)));

					MIR_append_insn(ctx, func_item, end_label);

					/* *op->resvalue = result */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_MOV,
							MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
							MIR_new_reg_op(ctx, r_tmp3)));
				}

				/* *op->resnull = false */
				MIR_STEP_LOAD(r_tmp1, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
						MIR_new_int_op(ctx, 0)));
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

				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 6,
						MIR_new_ref_op(ctx, proto_fallback),
						MIR_new_ref_op(ctx, step_fn_imports[opno]),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));

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

				for (int t = 0; t < ntargets; t++)
				{
					if (targets[t] >= 0 && targets[t] < steps_len)
					{
						MIR_append_insn(ctx, func_item,
							MIR_new_insn(ctx, MIR_BEQ,
								MIR_new_label_op(ctx, step_labels[targets[t]]),
								MIR_new_reg_op(ctx, r_tmp2),
								MIR_new_int_op(ctx, targets[t])));
					}
				}
				break;
			}
#endif /* HAVE_EEOP_JSONEXPR */

#ifdef HAVE_EEOP_RETURNINGEXPR
			/*
			 * ---- RETURNINGEXPR ----
			 * If state->flags & nullflag: set NULL result, jump to jumpdone.
			 * Otherwise continue.
			 */
			case EEOP_RETURNINGEXPR:
			{
				MIR_label_t cont_label = MIR_new_label(ctx);

				/* Load state->flags (int32) */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_mem_op(ctx, MIR_T_I32,
							offsetof(ExprState, flags), r_state, 0, 1)));

				/* Test: flags & nullflag */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_AND,
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_int_op(ctx, op->d.returningexpr.nullflag)));

				/* If zero (flag not set), continue */
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_BEQ,
						MIR_new_label_op(ctx, cont_label),
						MIR_new_reg_op(ctx, r_tmp2),
						MIR_new_int_op(ctx, 0)));

				/* Flag set: *resvalue = 0, *resnull = true, jump */
				MIR_STEP_LOAD(r_tmp1, opno, resvalue);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
						MIR_new_int_op(ctx, 0)));
				MIR_STEP_LOAD(r_tmp1, opno, resnull);
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_MOV,
						MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
						MIR_new_int_op(ctx, 1)));
				MIR_append_insn(ctx, func_item,
					MIR_new_insn(ctx, MIR_JMP,
						MIR_new_label_op(ctx, step_labels[op->d.returningexpr.jumpdone])));

				MIR_append_insn(ctx, func_item, cont_label);
				break;
			}
#endif /* HAVE_EEOP_RETURNINGEXPR */

			/*
			 * ---- DEFAULT: fallback ----
			 */
			default:
			{
				int fb_jump_target = -1;

				/* Call pg_jitter_fallback_step(state, op, econtext) -> int */
				MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t) op);
				MIR_append_insn(ctx, func_item,
					MIR_new_call_insn(ctx, 6,
						MIR_new_ref_op(ctx, proto_fallback),
						MIR_new_ref_op(ctx, import_fallback),
						MIR_new_reg_op(ctx, r_tmp2),   /* return value */
						MIR_new_reg_op(ctx, r_state),
						MIR_new_reg_op(ctx, r_tmp1),
						MIR_new_reg_op(ctx, r_econtext)));

				/* Check if this opcode could jump */
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
					/* If r_tmp2 >= 0, jump to target */
					MIR_append_insn(ctx, func_item,
						MIR_new_insn(ctx, MIR_BGE,
							MIR_new_label_op(ctx, step_labels[fb_jump_target]),
							MIR_new_reg_op(ctx, r_tmp2),
							MIR_new_int_op(ctx, 0)));
				}
				break;
			}
		}
	}

	/*
	 * Finalize the function and module.
	 */
	MIR_finish_func(ctx);
	MIR_finish_module(ctx);

	/*
	 * Load, link, and generate code.
	 */
	{
		void *code;

		MIR_load_module(ctx, m);

		/* Reset sentinel table for this compilation */
		mir_n_sentinels = 0;

		/* Load external symbols (sentinels in shared mode) */
		MIR_load_external(ctx, "fallback_step", mir_extern_addr((void *) pg_jitter_fallback_step));
		MIR_load_external(ctx, "getsomeattrs", mir_extern_addr((void *) slot_getsomeattrs_int));
		MIR_load_external(ctx, "deform_dispatch", mir_extern_addr((void *) pg_jitter_compiled_deform_dispatch));
		MIR_load_external(ctx, "make_ro", mir_extern_addr((void *) MakeExpandedObjectReadOnlyInternal));
		/* Inline error handlers */
		MIR_load_external(ctx, "err_i4ov", mir_extern_addr((void *) jit_error_int4_overflow));
		MIR_load_external(ctx, "err_i8ov", mir_extern_addr((void *) jit_error_int8_overflow));
		MIR_load_external(ctx, "err_divz", mir_extern_addr((void *) jit_error_division_by_zero));
		/* Inline deform helpers */
		MIR_load_external(ctx, "varsize_any", mir_extern_addr((void *) varsize_any));
		MIR_load_external(ctx, "strlen", mir_extern_addr((void *) strlen));
#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
		/* Presorted distinct helpers */
		MIR_load_external(ctx, "pdist_single", mir_extern_addr((void *) ExecEvalPreOrderedDistinctSingle));
		MIR_load_external(ctx, "pdist_multi", mir_extern_addr((void *) ExecEvalPreOrderedDistinctMulti));
#endif

	for (int i = 0; i < steps_len; i++)
	{
		if (step_fn_imports[i])
		{
			ExprEvalOp op = ExecEvalStepOp(state, &steps[i]);
			void *addr;
			char name[32];

			if (op == EEOP_HASHED_SCALARARRAYOP)
			{
				snprintf(name, sizeof(name), "saop_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) ExecEvalHashedScalarArrayOp));
				continue;
			}
			else if (op == EEOP_AGG_STRICT_DESERIALIZE ||
					 op == EEOP_AGG_DESERIALIZE)
			{
				FunctionCallInfo fcinfo = steps[i].d.agg_deserialize.fcinfo_data;
				snprintf(name, sizeof(name), "dser_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) fcinfo->flinfo->fn_addr));
				continue;
			}
			else if (op == EEOP_INNER_SYSVAR ||
					 op == EEOP_OUTER_SYSVAR ||
					 op == EEOP_SCAN_SYSVAR
#ifdef HAVE_EEOP_OLD_NEW
					 || op == EEOP_OLD_SYSVAR
					 || op == EEOP_NEW_SYSVAR
#endif
					 )
			{
				snprintf(name, sizeof(name), "sysvar_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) ExecEvalSysVar));
				continue;
			}
			else if (op == EEOP_SBSREF_SUBSCRIPTS)
			{
				snprintf(name, sizeof(name), "sbss_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) steps[i].d.sbsref_subscript.subscriptfunc));
				continue;
			}
			else if (op == EEOP_SBSREF_OLD ||
					 op == EEOP_SBSREF_ASSIGN ||
					 op == EEOP_SBSREF_FETCH)
			{
				snprintf(name, sizeof(name), "sbsf_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) steps[i].d.sbsref.subscriptfunc));
				continue;
			}
			else if (op == EEOP_IOCOERCE)
			{
				FunctionCallInfo fcinfo_out = steps[i].d.iocoerce.fcinfo_data_out;
				FunctionCallInfo fcinfo_in = steps[i].d.iocoerce.fcinfo_data_in;
				snprintf(name, sizeof(name), "ioc_out_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) fcinfo_out->flinfo->fn_addr));
				snprintf(name, sizeof(name), "ioc_in_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) fcinfo_in->flinfo->fn_addr));
				continue;
			}
			else if (op == EEOP_ROWCOMPARE_STEP)
			{
				snprintf(name, sizeof(name), "rcmp_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) steps[i].d.rowcompare_step.fn_addr));
				continue;
			}
#ifdef HAVE_EEOP_JSONEXPR
			else if (op == EEOP_JSONEXPR_PATH)
			{
				snprintf(name, sizeof(name), "jpath_%d", i);
				MIR_load_external(ctx, name,
								  mir_extern_addr((void *) ExecEvalJsonExprPath));
				continue;
			}
#endif
			/* Direct-call opcodes: resolve to PG-exported C function */
			else if (op == EEOP_FUNCEXPR_FUSAGE ||
					 op == EEOP_FUNCEXPR_STRICT_FUSAGE ||
					 op == EEOP_NULLTEST_ROWISNULL ||
					 op == EEOP_NULLTEST_ROWISNOTNULL ||
#ifdef HAVE_EEOP_PARAM_SET
					 op == EEOP_PARAM_SET ||
#endif
					 op == EEOP_ARRAYCOERCE ||
					 op == EEOP_FIELDSELECT ||
					 op == EEOP_FIELDSTORE_DEFORM ||
					 op == EEOP_FIELDSTORE_FORM ||
					 op == EEOP_CONVERT_ROWTYPE ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
					 op == EEOP_JSON_CONSTRUCTOR ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
					 op == EEOP_JSONEXPR_COERCION ||
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
					 op == EEOP_MERGE_SUPPORT_FUNC ||
#endif
					 op == EEOP_SUBPLAN ||
					 op == EEOP_WHOLEROW ||
					 op == EEOP_AGG_ORDERED_TRANS_DATUM ||
					 op == EEOP_AGG_ORDERED_TRANS_TUPLE ||
#ifdef HAVE_EEOP_IOCOERCE_SAFE
					 op == EEOP_IOCOERCE_SAFE ||
#endif
					 op == EEOP_SCALARARRAYOP ||
					 op == EEOP_SQLVALUEFUNCTION ||
					 op == EEOP_CURRENTOFEXPR ||
					 op == EEOP_NEXTVALUEEXPR ||
					 op == EEOP_ARRAYEXPR ||
					 op == EEOP_ROW ||
					 op == EEOP_MINMAX ||
					 op == EEOP_DOMAIN_NOTNULL ||
					 op == EEOP_DOMAIN_CHECK ||
					 op == EEOP_XMLEXPR ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
					 op == EEOP_IS_JSON ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
					 op == EEOP_JSONEXPR_COERCION_FINISH ||
#endif
					 op == EEOP_GROUPING_FUNC ||
					 op == EEOP_PARAM_CALLBACK ||
					 op == EEOP_PARAM_EXEC ||
					 op == EEOP_PARAM_EXTERN)
			{
				void *fn = NULL;
				switch (op)
				{
					/* 3-arg void calls */
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
					/* 2-arg void calls */
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
					/* PARAM_CALLBACK: indirect fn pointer */
					case EEOP_PARAM_CALLBACK:
						fn = (void *) steps[i].d.cparam.paramfunc; break;
					case EEOP_PARAM_EXEC:
						fn = (void *) ExecEvalParamExec; break;
					case EEOP_PARAM_EXTERN:
						fn = (void *) ExecEvalParamExtern; break;
					default:
						break;
				}
				snprintf(name, sizeof(name), "dc_%d", i);
				if (fn)
					MIR_load_external(ctx, name, mir_extern_addr(fn));
				continue;
			}
#ifdef HAVE_EEOP_HASHDATUM
			else if (op == EEOP_HASHDATUM_FIRST ||
				op == EEOP_HASHDATUM_FIRST_STRICT ||
				op == EEOP_HASHDATUM_NEXT32 ||
				op == EEOP_HASHDATUM_NEXT32_STRICT)
				addr = (void *) steps[i].d.hashdatum.fn_addr;
#endif
			else
				addr = (void *) steps[i].d.func.fn_addr;

			snprintf(name, sizeof(name), "fn_%d", i);
			MIR_load_external(ctx, name, mir_extern_addr(addr));
		}
		if (step_direct_imports[i])
		{
			ExprEvalOp opc = ExecEvalStepOp(state, &steps[i]);
			char name[32];

#ifdef HAVE_EEOP_HASHDATUM
			if (opc == EEOP_HASHDATUM_FIRST ||
				opc == EEOP_HASHDATUM_FIRST_STRICT ||
				opc == EEOP_HASHDATUM_NEXT32 ||
				opc == EEOP_HASHDATUM_NEXT32_STRICT)
			{
				const JitDirectFn *dfn = jit_find_direct_fn(steps[i].d.hashdatum.fn_addr);
				snprintf(name, sizeof(name), "dhfn_%d", i);
				if (dfn && dfn->jit_fn)
					MIR_load_external(ctx, name, mir_extern_addr(dfn->jit_fn));
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
				else if (!mir_shared_code_mode && dfn && dfn->jit_fn_name)
				{
					void *fn = mir_find_precompiled_fn(dfn->jit_fn_name);
					if (fn)
						MIR_load_external(ctx, name, fn);
				}
#endif
			}
			else
#endif /* HAVE_EEOP_HASHDATUM */
			if (opc >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
					 opc <= EEOP_AGG_PLAIN_TRANS_BYREF)
			{
				/* Resolve agg_trans helper address */
				void *helper_fn = NULL;
				switch (opc) {
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
				default: break;
				}
				snprintf(name, sizeof(name), "ah_%d", i);
				if (helper_fn)
					MIR_load_external(ctx, name, mir_extern_addr(helper_fn));
			}
			else
			{
				const JitDirectFn *dfn = jit_find_direct_fn(steps[i].d.func.fn_addr);
				snprintf(name, sizeof(name), "dfn_%d", i);
				if (dfn && dfn->jit_fn)
					MIR_load_external(ctx, name, mir_extern_addr(dfn->jit_fn));
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
				else if (!mir_shared_code_mode && dfn && dfn->jit_fn_name)
				{
					void *fn = mir_find_precompiled_fn(dfn->jit_fn_name);
					if (fn)
						MIR_load_external(ctx, name, fn);
				}
#endif
			}
		}
	}

		MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);

		code = MIR_gen(ctx, func_item);
		if (!code)
		{
			pfree(step_labels);
			pfree(step_fn_imports);
			pfree(step_direct_imports);
			pfree(ioc_in_imports);
			mir_shared_code_mode = false;
			return false;
		}

		/*
		 * MIR_gen() returns a thunk address, not the actual machine code.
		 * For local execution, the thunk works fine (it jumps to machine_code).
		 * But for sharing via DSM, we need the actual machine_code pointer and
		 * its length.
		 */

		/*
		 * In shared code mode, MIR compiled with sentinel addresses.
		 * Patch them to real addresses in the actual machine code, before
		 * executing or storing.
		 *
		 * MIR uses MAP_JIT on macOS ARM64, so after MIR_gen() the code
		 * memory is in execute mode.  Toggle W^X for patching.
		 */
		if (mir_shared_code_mode && mir_n_sentinels > 0)
		{
			void   *mcode = f->machine_code;
			Size	mcode_size = f->machine_code_len;
			int		npatched;

#if defined(__APPLE__) && defined(__aarch64__)
			pthread_jit_write_protect_np(0);	/* write mode */
#elif defined(__x86_64__) || defined(_M_X64)
			/*
			 * MIR sets code pages to PROT_READ|PROT_EXEC after MIR_gen().
			 * We need write access for sentinel patching.
			 */
			{
				long		pgsz = sysconf(_SC_PAGESIZE);
				uintptr_t	page_start = (uintptr_t) mcode & ~(pgsz - 1);
				Size		page_len = ((uintptr_t) mcode + mcode_size) - page_start;

				page_len = (page_len + pgsz - 1) & ~(pgsz - 1);
				if (mprotect((void *) page_start, page_len,
							 PROT_READ | PROT_WRITE) != 0)
					elog(WARNING, "pg_jitter[mir]: mprotect(RW) for sentinel patching failed: %m");
			}
#endif

			npatched = mir_patch_sentinels(mcode, mcode_size);

			elog(DEBUG1, "pg_jitter[mir]: patched %d/%d sentinel addresses "
				 "in %zu bytes of machine code (thunk=%p, mcode=%p)",
				 npatched, mir_n_sentinels, mcode_size, code, mcode);

#if defined(__APPLE__) && defined(__aarch64__)
			sys_icache_invalidate(mcode, mcode_size);
			pthread_jit_write_protect_np(1);	/* exec mode */
#elif defined(__x86_64__) || defined(_M_X64)
			{
				long		pgsz = sysconf(_SC_PAGESIZE);
				uintptr_t	page_start = (uintptr_t) mcode & ~(pgsz - 1);
				Size		page_len = ((uintptr_t) mcode + mcode_size) - page_start;

				page_len = (page_len + pgsz - 1) & ~(pgsz - 1);
				if (mprotect((void *) page_start, page_len,
							 PROT_READ | PROT_EXEC) != 0)
					elog(WARNING, "pg_jitter[mir]: mprotect(RX) for sentinel patching failed: %m");
			}
#endif
		}

		/* Set the eval function (with validation wrapper on first call) */
		pg_jitter_install_expr(state, (ExprStateEvalFunc) code);

		/*
		 * Leader: store compiled machine code directly in DSM.
		 * Use f->machine_code (actual code), not `code` (thunk).
		 */
		if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED &&
			!IsParallelWorker() &&
			(state->parent->state->es_jit_flags & PGJIT_EXPR) &&
			jctx->share_state.sjc)
		{
			void   *mcode = f->machine_code;
			Size	mcode_size = f->machine_code_len;

			elog(DEBUG1, "pg_jitter[mir]: leader storing code "
				 "node=%d expr=%d (%zu bytes) mcode=%p fallback=%p",
				 shared_node_id, shared_expr_idx,
				 mcode_size, mcode, (void *) pg_jitter_fallback_step);

			pg_jitter_store_shared_code(jctx->share_state.sjc,
										mcode, mcode_size,
										shared_node_id, shared_expr_idx,
										(uint64)(uintptr_t) pg_jitter_fallback_step);
		}
	}

	/*
	 * No per-expression registration needed — the per-query MIR context
	 * (registered via mir_ctx_free in mir_get_or_create_ctx) handles cleanup
	 * of all generated code when the JitContext is released.
	 */

	pfree(step_labels);
	pfree(step_fn_imports);
	pfree(step_direct_imports);
	pfree(ioc_in_imports);

	/* Reset shared code mode for next compilation */
	mir_shared_code_mode = false;

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(jctx->base.instr.generation_counter,
						  endtime, starttime);
	jctx->base.instr.created_functions++;

	return true;
}
