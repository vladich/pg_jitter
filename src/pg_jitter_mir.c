/*
 * pg_jitter_mir.c — MIR-based PostgreSQL JIT provider
 *
 * Uses MIR's medium-level IR with optimization pipeline.
 * Hot-path opcodes (~30) get native code; everything else falls back to
 * pg_jitter_fallback_step().
 */
#include "postgres.h"
#include "executor/execExpr.h"
#include "executor/nodeAgg.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "nodes/execnodes.h"
#include "utils/array.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "common/hashfn.h"
#include "pg_jit_funcs.h"
#include "pg_jitter_common.h"
#include "pg_jitter_simd.h"
#include "pg_jitter_simdjson.h"
#include "utils/fmgrprotos.h"
#include "pg_jitter_vectorscan.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "mb/pg_wchar.h"
#include "catalog/pg_collation_d.h"
#include "utils/date.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM >= 160000
#include "nodes/miscnodes.h"
#endif
#include "commands/sequence.h"
#include "funcapi.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif


/*
 * Mirror of Int8TransTypeData from numeric.c.
 */
typedef struct {
  int64 count;
  int64 sum;
} MirInt8TransTypeData;
#include "mir-gen.h"
#include "mir.h"

PG_MODULE_MAGIC_EXT(.name = "pg_jitter_mir", );

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
typedef struct {
  void *real_addr; /* actual function pointer */
  uint64 sentinel; /* all-nonzero-halfword fake address */
} MirSentinelEntry;

#define MIR_MAX_SENTINELS 128

static MirSentinelEntry mir_sentinels[MIR_MAX_SENTINELS];
static int mir_n_sentinels = 0;

/*
 * Generate a sentinel address with all 4 halfwords non-zero.
 * Pattern: 0xCAFE_<idx>001_BABE_<idx>001 where idx varies.
 */
static uint64 mir_alloc_sentinel(void *real_addr) {
  int idx = mir_n_sentinels;
  uint64 s;

  Assert(idx < MIR_MAX_SENTINELS);

  /*
   * Construct sentinel with all 4 halfwords non-zero.
   * Use idx+1 in low bits to make each one unique.
   */
  s = 0xCAFE000000000000ULL | ((uint64)((idx + 1) & 0xFFFF) << 32) |
      0x00000000BABE0000ULL | ((uint64)((idx + 1) & 0xFFFF));

  mir_sentinels[idx].real_addr = real_addr;
  mir_sentinels[idx].sentinel = s;
  mir_n_sentinels++;

  return s;
}

/*
 * Get the address to pass to MIR_load_external: sentinel in shared mode,
 * real address otherwise.
 */
static void *mir_extern_addr(void *real_addr) {
  if (!mir_shared_code_mode)
    return real_addr;
  return (void *)(uintptr_t)mir_alloc_sentinel(real_addr);
}

/*
 * After MIR_gen(), scan the generated code and replace sentinel addresses
 * with real addresses.  Only needed for the leader's code — workers get
 * already-patched code from DSM.
 *
 * Uses the same MOVZ+3×MOVK scanning as pg_jitter_relocate_dylib_addrs,
 * but matches against our sentinel table instead of a range check.
 */
static int mir_patch_sentinels(void *code, Size code_size) {
#if defined(__aarch64__)
  uint32_t *insns = (uint32_t *)code;
  int ninsns = code_size / 4;
  int patched = 0;

  for (int i = 0; i + 3 < ninsns; i++) {
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
    val = (uint64)((w0 >> 5) & 0xFFFF);
    val |= (uint64)((w1 >> 5) & 0xFFFF) << 16;
    val |= (uint64)((w2 >> 5) & 0xFFFF) << 32;
    val |= (uint64)((w3 >> 5) & 0xFFFF) << 48;

    /* Check if it matches any sentinel */
    for (int s = 0; s < mir_n_sentinels; s++) {
      if (val == mir_sentinels[s].sentinel) {
        uint64 real = (uint64)(uintptr_t)mir_sentinels[s].real_addr;

        insns[i] = (w0 & 0xFFE0001F) | (((real >> 0) & 0xFFFF) << 5);
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
  uint8_t *bytes = (uint8_t *)code;

  for (Size off = 0; off + 7 < code_size; off++) {
    uint64 val;
    memcpy(&val, bytes + off, 8);

    for (int s = 0; s < mir_n_sentinels; s++) {
      if (val == mir_sentinels[s].sentinel) {
        uint64 real = (uint64)(uintptr_t)mir_sentinels[s].real_addr;
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
 * Pre-compiled MIR blob support — shared infrastructure from
 * pg_jit_mir_blobs.h. Provides mir_find_precompiled_fn() and
 * mir_load_precompiled_blobs().
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
typedef struct MirPerQueryState {
  MIR_context_t ctx;
  int module_counter;
#ifdef _WIN64
  void **unwind_codes;  /* machine_code pointers with registered SEH unwind */
  int n_unwind;
  int unwind_capacity;
#endif
} MirPerQueryState;

/* Single-threaded PG: cache the current per-query state */
static MirPerQueryState *mir_current_state = NULL;
static PgJitterContext *mir_current_jctx = NULL;

/*
 * Free the per-query MIR context when its JitContext is released.
 */
static void mir_ctx_free(void *data) {
  MirPerQueryState *st = (MirPerQueryState *)data;

  if (st) {
    if (mir_current_state == st) {
      mir_current_state = NULL;
      mir_current_jctx = NULL;
    }
#ifdef _WIN64
    /* Deregister SEH unwind info before MIR frees code pages */
    for (int i = 0; i < st->n_unwind; i++)
      pg_jitter_win64_deregister_unwind(st->unwind_codes[i]);
    if (st->unwind_codes)
      pfree(st->unwind_codes);
#endif
    MIR_gen_finish(st->ctx);
    MIR_finish(st->ctx);
    pfree(st);
  }
}

/*
 * Get or create a MIR context for the given JitContext.
 */
static MIR_context_t mir_get_or_create_ctx(PgJitterContext *jctx) {
  if (mir_current_jctx == jctx)
    return mir_current_state->ctx;

  /* New JitContext — create fresh MIR context */
  {
    MirPerQueryState *st =
        MemoryContextAllocZero(TopMemoryContext, sizeof(MirPerQueryState));
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
 * Debug signal handler to dump backtrace on SIGBUS/SIGSEGV.
 */
/*
 * Provider entry point.
 */
#if defined(_MSC_VER) && PG_VERSION_NUM < 160000
#pragma comment(linker, "/EXPORT:_PG_jit_provider_init")
#endif
void _PG_jit_provider_init(JitProviderCallbacks *cb) {
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
static void mir_reset_after_error(void) { /* nothing to do */ }

/*
 * Helper: get econtext slot offset for a given opcode.
 */
static int64_t mir_slot_offset(ExprEvalOp opcode) {
  switch (opcode) {
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
static MIR_reg_t mir_new_reg(MIR_context_t ctx, MIR_func_t f, MIR_type_t type,
                             const char *prefix) {
  char name[64];

  snprintf(name, sizeof(name), "%s_%d", prefix, mir_name_counter++);
  return MIR_new_func_reg(ctx, f, type, name);
}

/*
 * Check whether PG's hand-optimized fast-path evalfuncs handle this
 * expression better than JIT-compiled code.  Tiny 2-5 step expressions
 * that match these patterns should be left alone.
 */
static bool expr_has_fast_path(ExprState *state) {
  int nsteps = state->steps_len;
  ExprEvalOp step0, step1, step2, step3;

  /* Fast-paths only exist for 2-5 step expressions */
  if (nsteps < 2 || nsteps > 5)
    return false;

  step0 = ExecEvalStepOp(state, &state->steps[0]);

  if (nsteps == 5) {
    step1 = ExecEvalStepOp(state, &state->steps[1]);
    step2 = ExecEvalStepOp(state, &state->steps[2]);
    step3 = ExecEvalStepOp(state, &state->steps[3]);

#ifdef HAVE_EEOP_HASHDATUM
    /* INNER_FETCHSOME + HASHDATUM_SET_INITVAL + INNER_VAR + HASHDATUM_NEXT32 +
     * DONE */
    if (step0 == EEOP_INNER_FETCHSOME && step1 == EEOP_HASHDATUM_SET_INITVAL &&
        step2 == EEOP_INNER_VAR && step3 == EEOP_HASHDATUM_NEXT32)
      return true;
#endif
  } else if (nsteps == 4) {
    step1 = ExecEvalStepOp(state, &state->steps[1]);
    step2 = ExecEvalStepOp(state, &state->steps[2]);

#ifdef HAVE_EEOP_HASHDATUM
    /* (INNER|OUTER)_FETCHSOME + (INNER|OUTER)_VAR + HASHDATUM_FIRST(_STRICT) +
     * DONE */
    if (step0 == EEOP_OUTER_FETCHSOME && step1 == EEOP_OUTER_VAR &&
        step2 == EEOP_HASHDATUM_FIRST)
      return true;
    if (step0 == EEOP_INNER_FETCHSOME && step1 == EEOP_INNER_VAR &&
        step2 == EEOP_HASHDATUM_FIRST)
      return true;
    if (step0 == EEOP_OUTER_FETCHSOME && step1 == EEOP_OUTER_VAR &&
        step2 == EEOP_HASHDATUM_FIRST_STRICT)
      return true;
#endif
  } else if (nsteps == 3) {
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
         || step1 == EEOP_FUNCEXPR_STRICT_1 || step1 == EEOP_FUNCEXPR_STRICT_2
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
  } else if (nsteps == 2) {
    /* CONST, VAR (inner/outer/scan), ASSIGN_VAR (inner/outer/scan) */
    if (step0 == EEOP_CONST || step0 == EEOP_INNER_VAR ||
        step0 == EEOP_OUTER_VAR || step0 == EEOP_SCAN_VAR ||
        step0 == EEOP_ASSIGN_INNER_VAR || step0 == EEOP_ASSIGN_OUTER_VAR ||
        step0 == EEOP_ASSIGN_SCAN_VAR)
      return true;
  }

  return false;
}

/* mir_emit_deform_inline removed — MIR now always uses
 * pg_jitter_compiled_deform_dispatch() which calls SLJIT-compiled
 * deform functions. This is faster than MIR inline deform, especially
 * for wide tables (100+ cols: 37% faster). */

/*
 * Helper: emit MIR instruction to load the address of a step field into
 * a register.  In PIC mode (mir_shared_code_mode), computes the address
 * at runtime from r_steps; otherwise embeds the absolute address.
 *
 * 'ptr' is the absolute address (e.g., op->resvalue).
 * 'opno' is the step index, 'field' is the offsetof within ExprEvalStep.
 */
static void mir_emit_step_addr(MIR_context_t ctx, MIR_item_t func_item,
                               MIR_reg_t dst, MIR_reg_t r_steps, int opno,
                               int64_t field_offset, uint64_t abs_addr) {
  if (mir_shared_code_mode) {
    int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) + field_offset;
    MIR_append_insn(ctx, func_item,
                    MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, dst),
                                 MIR_new_reg_op(ctx, r_steps),
                                 MIR_new_int_op(ctx, off)));
  } else {
    MIR_append_insn(ctx, func_item,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst),
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
static void mir_emit_step_load_ptr(MIR_context_t ctx, MIR_item_t func_item,
                                   MIR_reg_t dst, MIR_reg_t r_steps, int opno,
                                   int64_t field_offset, uint64_t abs_addr) {
  if (mir_shared_code_mode) {
    int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) + field_offset;
    MIR_append_insn(
        ctx, func_item,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst),
                     MIR_new_mem_op(ctx, MIR_T_P, off, r_steps, 0, 1)));
  } else {
    MIR_append_insn(ctx, func_item,
                    MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, dst),
                                 MIR_new_uint_op(ctx, abs_addr)));
  }
}

/*
 * Macros for common step-field address patterns.
 * MIR_STEP_ADDR: load address of steps[opno].field into dst
 * MIR_STEP_LOAD: load value of steps[opno].field (pointer-typed) into dst
 */
#define MIR_STEP_ADDR(dst, opno, field)                                        \
  mir_emit_step_addr(ctx, func_item, dst, r_steps, opno,                       \
                     offsetof(ExprEvalStep, field), (uint64_t)(op->field))

#define MIR_STEP_ADDR_RAW(dst, opno, field_offset, abs_addr)                   \
  mir_emit_step_addr(ctx, func_item, dst, r_steps, opno, field_offset, abs_addr)

#define MIR_STEP_LOAD(dst, opno, field)                                        \
  mir_emit_step_load_ptr(ctx, func_item, dst, r_steps, opno,                   \
                         offsetof(ExprEvalStep, field), (uint64_t)(op->field))

/*
 * Compile an expression using MIR.
 *
 * Generated function signature:
 *   Datum fn(ExprState *state, ExprContext *econtext, bool *isNull)
 */
static bool mir_compile_expr(ExprState *state) {
  PgJitterContext *jctx;
  MIR_context_t ctx;
  MIR_module_t m;
  MIR_item_t func_item;
  MIR_func_t f;
  ExprEvalStep *steps;
  int steps_len;
  instr_time starttime, endtime;
  char modname[32];
  MIR_reg_t r_state, r_econtext, r_isnullp;
  MIR_reg_t r_resvaluep, r_resnullp;
  MIR_reg_t r_resultvals, r_resultnulls;
  MIR_reg_t r_tmp1, r_tmp2, r_tmp3, r_slot;
  MIR_reg_t r_steps;

  MIR_insn_t *step_labels;

  /* Prototypes and imports for function calls */
  MIR_item_t proto_fallback, import_fallback;
  MIR_item_t proto_getsomeattrs, import_getsomeattrs;

  MIR_item_t proto_v1func, import_v1func; /* per-step, see below */
  MIR_item_t proto_makero, import_makero;
  MIR_type_t res_type;
  int shared_node_id = 0;
  int shared_expr_idx = 0;

  if (!state->parent)
    return false;

  /* Skip JIT in parallel workers when mode is OFF */
  if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_OFF && IsParallelWorker())
    return false;

  /* Let PG's hand-optimized fast-path evalfuncs handle tiny expressions */
  if (expr_has_fast_path(state)) {
    /* elog(LOG, "MIR skip fast_path expr %d steps", state->steps_len); */
    return false;
  }

  /* Skip JIT for expressions below the minimum step threshold */
  {
    int min_steps = pg_jitter_get_min_expr_steps();
    if (min_steps > 0 && state->steps_len < min_steps)
      return false;
  }

  jctx = pg_jitter_get_context(state);

  /*
   * Compute expression identity for shared code.
   *
   * Leader: creates DSM on first compile, writes code directly.
   * Worker: attaches to DSM via GUC, looks up pre-compiled code.
   */
  if (state->parent->state->es_jit_flags & PGJIT_EXPR) {
    pg_jitter_get_expr_identity(jctx, state, &shared_node_id, &shared_expr_idx);

    mir_shared_code_mode =
        (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED) &&
        state->parent->state->es_plannedstmt->parallelModeNeeded;

#if defined(__x86_64__) || defined(_M_X64)
    /*
     * x86_64: MIR's code patterns aren't reliably handled by the
     * relocation scanner yet (it expects MOVABS but MIR may use
     * RIP-relative addressing). Disable shared code mode so
     * workers compile locally instead of reusing leader code.
     */
    mir_shared_code_mode = false;
#endif

    elog(DEBUG1,
         "pg_jitter[mir]: compile_expr node=%d expr=%d is_worker=%d "
         "shared_mode=%d share_init=%d",
         shared_node_id, shared_expr_idx, IsParallelWorker(),
         mir_shared_code_mode, jctx->share_state.initialized);

    /* Leader: create DSM on first compile */
    if (mir_shared_code_mode && !IsParallelWorker() &&
        !jctx->share_state.initialized)
      pg_jitter_init_shared_dsm(jctx);
  }

  /*
   * Parallel worker: try to use pre-compiled code from the leader.
   */
  if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED &&
      IsParallelWorker()) {
    const void *code_bytes;
    Size code_size;
    uint64 leader_dylib_ref;

    if (!jctx->share_state.initialized)
      pg_jitter_attach_shared_dsm(jctx);

    if (jctx->share_state.sjc &&
        pg_jitter_find_shared_code(jctx->share_state.sjc, shared_node_id,
                                   shared_expr_idx, &code_bytes, &code_size,
                                   &leader_dylib_ref)) {
      void *handle;
      void *code_ptr;

      handle = pg_jitter_copy_to_executable(code_bytes, code_size);
      if (handle) {
        uint64 worker_ref = (uint64)(uintptr_t)pg_jitter_fallback_step;
        int npatched;

        npatched = pg_jitter_relocate_dylib_addrs(handle, code_size,
                                                  leader_dylib_ref, worker_ref);

        code_ptr = pg_jitter_exec_code_ptr(handle);

        elog(DEBUG1,
             "pg_jitter[mir]: worker reused shared code "
             "node=%d expr=%d (%zu bytes, patched=%d) code_ptr=%p "
             "leader_ref=%lx worker_ref=%lx",
             shared_node_id, shared_expr_idx, code_size, npatched, code_ptr,
             (unsigned long)leader_dylib_ref, (unsigned long)worker_ref);

        pg_jitter_register_compiled(jctx, pg_jitter_exec_free, handle);

        /* Worker: set up binary search arrays for CASE optimization */
        pg_jitter_setup_case_bsearch_arrays(state, state->steps,
                                             state->steps_len);

        pg_jitter_install_expr(state, (ExprStateEvalFunc)code_ptr);

        mir_shared_code_mode = false;

        jctx->base.instr.created_functions++;
        return true;
      }

      elog(WARNING,
           "pg_jitter[mir]: failed to allocate executable memory "
           "for shared code node=%d expr=%d, compiling locally",
           shared_node_id, shared_expr_idx);
    } else
      elog(DEBUG1,
           "pg_jitter[mir]: worker did not find shared code "
           "node=%d expr=%d, compiling locally",
           shared_node_id, shared_expr_idx);
  }

  INSTR_TIME_SET_CURRENT(starttime);

  steps = state->steps;
  steps_len = state->steps_len;

  mir_name_counter = 0;

  ctx = mir_get_or_create_ctx(jctx);
  if (!ctx) {
    mir_shared_code_mode = false;
    return false;
  }

  snprintf(modname, sizeof(modname), "m_%d",
           mir_current_state->module_counter++);
  m = MIR_new_module(ctx, modname);

  /*
   * Create prototypes and imports at module level (before the function).
   */

  /* Proto for pg_jitter_fallback_step(state, op, econtext) -> int */
  {
    MIR_type_t res_type = MIR_T_I64;
    proto_fallback = MIR_new_proto(ctx, "p_fallback", 1, &res_type, 3, MIR_T_P,
                                   "s", MIR_T_P, "o", MIR_T_P, "e");
  }
  import_fallback = MIR_new_import(ctx, "fallback_step");

  /* Proto for slot_getsomeattrs_int(slot, last_var) -> void */
  proto_getsomeattrs = MIR_new_proto(ctx, "p_getsomeattrs", 0, NULL, 2, MIR_T_P,
                                     "sl", MIR_T_I32, "n");
  import_getsomeattrs = MIR_new_import(ctx, "getsomeattrs");

  /* Proto for V1 function: fn(FunctionCallInfo) -> Datum */
  res_type = MIR_T_I64;
  proto_v1func =
      MIR_new_proto(ctx, "p_v1func", 1, &res_type, 1, MIR_T_P, "fci");

  /* Proto for MakeExpandedObjectReadOnlyInternal(Datum) -> Datum */
  proto_makero =
      MIR_new_proto(ctx, "p_makero", 1, &res_type, 1, MIR_T_I64, "d");
  import_makero = MIR_new_import(ctx, "make_ro");

  /* Proto for CASE binary search helpers: (val, desc_ptr) -> Datum
   * Two variants: I32 val and I64 val. */
  MIR_item_t proto_bsearch_i32, proto_bsearch_i64;
  {
    MIR_type_t ret_datum = MIR_T_I64;
    proto_bsearch_i32 = MIR_new_proto(ctx, "p_bsearch_i32", 1, &ret_datum, 2,
                                       MIR_T_I32, "val", MIR_T_P, "desc");
    proto_bsearch_i64 = MIR_new_proto(ctx, "p_bsearch_i64", 1, &ret_datum, 2,
                                       MIR_T_I64, "val", MIR_T_P, "desc");
  }

  /* Protos for direct native calls: [nargs-1][ret_type][arg0_type]
   * where ret_type/arg0_type index 0 = I32, 1 = I64 (matches JIT_TYPE_*) */
  MIR_item_t proto_direct[2][2][2];
  {
    MIR_type_t rt32 = MIR_T_I32, rt64 = MIR_T_I64;
    /* 1-arg protos */
    proto_direct[0][0][0] =
        MIR_new_proto(ctx, "p_d1_32_32", 1, &rt32, 1, MIR_T_I32, "a0");
    proto_direct[0][0][1] =
        MIR_new_proto(ctx, "p_d1_32_64", 1, &rt32, 1, MIR_T_I64, "a0");
    proto_direct[0][1][0] =
        MIR_new_proto(ctx, "p_d1_64_32", 1, &rt64, 1, MIR_T_I32, "a0");
    proto_direct[0][1][1] =
        MIR_new_proto(ctx, "p_d1_64_64", 1, &rt64, 1, MIR_T_I64, "a0");
    /* 2-arg protos (arg0 == arg1 type for all non-deferred entries) */
    proto_direct[1][0][0] = MIR_new_proto(ctx, "p_d2_32_32", 1, &rt32, 2,
                                          MIR_T_I32, "a0", MIR_T_I32, "a1");
    proto_direct[1][0][1] = MIR_new_proto(ctx, "p_d2_32_64", 1, &rt32, 2,
                                          MIR_T_I64, "a0", MIR_T_I64, "a1");
    proto_direct[1][1][0] = MIR_new_proto(ctx, "p_d2_64_32", 1, &rt64, 2,
                                          MIR_T_I32, "a0", MIR_T_I32, "a1");
    proto_direct[1][1][1] = MIR_new_proto(ctx, "p_d2_64_64", 1, &rt64, 2,
                                          MIR_T_I64, "a0", MIR_T_I64, "a1");
  }

  /* 2-arg + collation protos: (arg0, arg1, I32 collid) -> ret
   * Used for collation-aware direct-call functions (texteq, text_lt, etc.) */
  MIR_item_t proto_direct_coll[2][2]; /* [ret_type][arg0_type] */
  {
    MIR_type_t rt32 = MIR_T_I32, rt64 = MIR_T_I64;
    proto_direct_coll[0][0] = MIR_new_proto(ctx, "p_dc_32_32", 1, &rt32, 3,
                                            MIR_T_I32, "a0", MIR_T_I32, "a1",
                                            MIR_T_I32, "c");
    proto_direct_coll[0][1] = MIR_new_proto(ctx, "p_dc_32_64", 1, &rt32, 3,
                                            MIR_T_I64, "a0", MIR_T_I64, "a1",
                                            MIR_T_I32, "c");
    proto_direct_coll[1][0] = MIR_new_proto(ctx, "p_dc_64_32", 1, &rt64, 3,
                                            MIR_T_I32, "a0", MIR_T_I32, "a1",
                                            MIR_T_I32, "c");
    proto_direct_coll[1][1] = MIR_new_proto(ctx, "p_dc_64_64", 1, &rt64, 3,
                                            MIR_T_I64, "a0", MIR_T_I64, "a1",
                                            MIR_T_I32, "c");
  }

  /* Proto for agg_trans helpers: (ExprState*, ExprEvalStep*) -> void */
  MIR_item_t proto_agg_helper;
  proto_agg_helper =
      MIR_new_proto(ctx, "p_aggh", 0, NULL, 2, MIR_T_P, "s", MIR_T_P, "o");

  /* Proto for SVF: int32 fn(void) — for GetSQLCurrentDate */
  MIR_item_t proto_svf_i32_void;
  {
    MIR_type_t rt_i32 = MIR_T_I32;
    proto_svf_i32_void =
        MIR_new_proto(ctx, "p_svfi32v", 1, &rt_i32, 0);
  }

  /* Proto for SVF: int64 fn(int32) — for GetSQLCurrentTime/Timestamp etc. */
  MIR_item_t proto_svf_i64_i32;
  {
    MIR_type_t rt_i64 = MIR_T_I64;
    proto_svf_i64_i32 =
        MIR_new_proto(ctx, "p_svfi64i32", 1, &rt_i64, 1, MIR_T_I32, "tm");
  }

  /* Proto for palloc: void* fn(Size) — for ARRAYEXPR inline */
  MIR_item_t proto_palloc;
  {
    MIR_type_t rt_p = MIR_T_P;
    proto_palloc =
        MIR_new_proto(ctx, "p_palloc", 1, &rt_p, 1, MIR_T_I64, "sz");
  }
  MIR_item_t import_palloc = MIR_new_import(ctx, "palloc_fn");

  /* Proto for nextval_internal: int64 fn(int32 seqid, int32 check_perms) */
  MIR_item_t proto_nextval;
  {
    MIR_type_t rt_i64 = MIR_T_I64;
    proto_nextval =
        MIR_new_proto(ctx, "p_nextval", 1, &rt_i64, 2, MIR_T_I32, "sid", MIR_T_I32, "cp");
  }

  /* Proto for heap_form_tuple: void* fn(void* tupdesc, void* vals, void* nulls) */
  MIR_item_t proto_hft;
  {
    MIR_type_t rt_p = MIR_T_P;
    proto_hft =
        MIR_new_proto(ctx, "p_hft", 1, &rt_p, 3, MIR_T_P, "td", MIR_T_P, "v", MIR_T_P, "n");
  }

  /* Proto for HeapTupleHeaderGetDatum: Datum fn(void* tuple_header) */
  MIR_item_t proto_htgd;
  {
    MIR_type_t rt_p = MIR_T_P;
    proto_htgd =
        MIR_new_proto(ctx, "p_htgd", 1, &rt_p, 1, MIR_T_P, "th");
  }

  MIR_item_t import_nextval = MIR_new_import(ctx, "nextval_fn");
  MIR_item_t import_hft = MIR_new_import(ctx, "hft_fn");
  MIR_item_t import_htgd = MIR_new_import(ctx, "htgd_fn");

#ifdef PG_JITTER_HAVE_SIMDJSON
  /* Proto for simdjson IS_JSON: (Datum, int32) -> int32 */
  MIR_item_t proto_sj_is_json;
  {
    MIR_type_t rt32 = MIR_T_I32;
    proto_sj_is_json = MIR_new_proto(ctx, "p_sjisj", 1, &rt32, 2,
                                      MIR_T_I64, "datum", MIR_T_I32, "itype");
  }

  /* Proto for simdjson json_in/jsonb_in: (Datum, FunctionCallInfo) -> Datum */
  MIR_item_t proto_sj_json_in;
  {
    MIR_type_t rt64 = MIR_T_I64;
    proto_sj_json_in = MIR_new_proto(ctx, "p_sjjin", 1, &rt64, 2,
                                      MIR_T_I64, "cstr", MIR_T_P, "fci");
  }
#endif

#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
  /* Proto + imports for presorted distinct: fn(AggState*, AggStatePerTrans) ->
   * bool */
  MIR_item_t proto_presorted_distinct;
  MIR_item_t import_presorted_single, import_presorted_multi;
  {
    MIR_type_t rt = MIR_T_I64;
    proto_presorted_distinct =
        MIR_new_proto(ctx, "p_pdist", 1, &rt, 2, MIR_T_P, "a", MIR_T_P, "p");
  }
  import_presorted_single = MIR_new_import(ctx, "pdist_single");
  import_presorted_multi = MIR_new_import(ctx, "pdist_multi");
#endif

#ifdef PG_JITTER_HAVE_SIMDJSON
  MIR_item_t import_sj_is_json = MIR_new_import(ctx, "sj_is_json");
  MIR_item_t import_sj_json_in = MIR_new_import(ctx, "sj_json_in");
  MIR_item_t import_sj_jsonb_in = MIR_new_import(ctx, "sj_jsonb_in");
#endif

  /* Proto for 3-arg void calls: fn(state, op, econtext) -> void */
  MIR_item_t proto_3arg_void;
  proto_3arg_void = MIR_new_proto(ctx, "p_3v", 0, NULL, 3, MIR_T_P, "s",
                                  MIR_T_P, "o", MIR_T_P, "e");

  /* Proto for 4-arg void calls: fn(state, op, econtext, slot) -> void */
  MIR_item_t proto_4arg_void;
  proto_4arg_void = MIR_new_proto(ctx, "p_4v", 0, NULL, 4, MIR_T_P, "a",
                                  MIR_T_P, "b", MIR_T_P, "c", MIR_T_P, "d");

  /* Proto + imports for inline error handlers: void -> void (noreturn) */
  MIR_item_t proto_err_void;
  proto_err_void = MIR_new_proto(ctx, "p_err", 0, NULL, 0);
  MIR_item_t import_err_int4_overflow = MIR_new_import(ctx, "err_i4ov");
  MIR_item_t import_err_int8_overflow = MIR_new_import(ctx, "err_i8ov");
  MIR_item_t import_err_div_by_zero = MIR_new_import(ctx, "err_divz");

  /* Proto + imports for SCALARARRAYOP inline:
   * pg_detoast_datum(varlena*) -> varlena*
   * pg_jitter_scalararrayop_loop(op, arr, scalar_value, scalar_null) -> void
   */
  MIR_item_t proto_detoast_arr;
  {
    MIR_type_t rt = MIR_T_P;
    proto_detoast_arr =
        MIR_new_proto(ctx, "p_dta", 1, &rt, 1, MIR_T_I64, "d");
  }
  MIR_item_t import_detoast_arr = MIR_new_import(ctx, "detoast_arr");

  MIR_item_t proto_saop_loop;
  proto_saop_loop = MIR_new_proto(ctx, "p_saop", 0, NULL, 4, MIR_T_P, "op",
                                  MIR_T_P, "arr", MIR_T_I64, "sv", MIR_T_I32,
                                  "sn");
  MIR_item_t import_saop_loop = MIR_new_import(ctx, "saop_loop");

  /* Proto + import for bms_is_member (GROUPING_FUNC inline):
   * bool bms_is_member(int x, const Bitmapset *a)
   */
  MIR_item_t proto_bms;
  {
    MIR_type_t bms_ret_type = MIR_T_I32;
    proto_bms = MIR_new_proto(ctx, "p_bms", 1, &bms_ret_type,
                              2, MIR_T_I32, "x", MIR_T_P, "a");
  }
  MIR_item_t import_bms = MIR_new_import(ctx, "bms_is_member");

  /* Proto + import for compiled LIKE fast path:
   * int32 simd_like_match_compiled(int64 datum, int64 compiled_ptr) */
  MIR_item_t proto_szcomp;
  MIR_item_t import_szcomp;
  {
    MIR_type_t szc_ret = MIR_T_I32;
    proto_szcomp = MIR_new_proto(ctx, "p_szcomp", 1, &szc_ret,
                                 2, MIR_T_I64, "d", MIR_T_I64, "c");
  }
  import_szcomp = MIR_new_import(ctx, "simd_like_match_compiled");

#ifdef PG_JITTER_HAVE_VECTORSCAN
  /* Proto + import for Vectorscan LIKE/regex fast path:
   * int32 pg_jitter_vs_match_text(int64 datum, int64 entry_ptr) */
  MIR_item_t proto_vs_match;
  MIR_item_t import_vs_match;
  {
    MIR_type_t vs_ret = MIR_T_I32;
    proto_vs_match = MIR_new_proto(ctx, "p_vsmatch", 1, &vs_ret,
                                   2, MIR_T_I64, "d", MIR_T_I64, "e");
  }
  import_vs_match = MIR_new_import(ctx, "vs_match_text");
#endif /* PG_JITTER_HAVE_VECTORSCAN */

  /*
   * Per-step imports for V1 function addresses.
   * We need unique import names for each distinct fn_addr.
   */
  MIR_item_t *step_fn_imports = palloc0(sizeof(MIR_item_t) * steps_len);
  MIR_item_t *step_direct_imports = palloc0(sizeof(MIR_item_t) * steps_len);
  uint8 *step_direct_ret_types = palloc0(steps_len * sizeof(uint8));
  uint8 *step_direct_arg0_types = palloc0(steps_len * sizeof(uint8));
  MIR_item_t *ioc_in_imports = palloc0(sizeof(MIR_item_t) * steps_len);
  for (int i = 0; i < steps_len; i++) {
    ExprEvalStep *op = &steps[i];
    ExprEvalOp opcode = ExecEvalStepOp(state, op);

    if (opcode == EEOP_FUNCEXPR || opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
        || opcode == EEOP_FUNCEXPR_STRICT_1 || opcode == EEOP_FUNCEXPR_STRICT_2
#endif
    ) {
      const JitDirectFn *dfn = jit_find_direct_fn(op->d.func.fn_addr);
      if (dfn && dfn->jit_fn) {
        /* Create import for the direct native function */
        char name[32];
        snprintf(name, sizeof(name), "dfn_%d", i);
        step_direct_imports[i] = MIR_new_import(ctx, name);
        step_direct_ret_types[i] = dfn->ret_type;
        step_direct_arg0_types[i] = dfn->arg_types[0];
      }
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
      else if (!mir_shared_code_mode && dfn && dfn->jit_fn_name &&
               mir_find_precompiled_fn(dfn->jit_fn_name)) {
        /* Use MIR-precompiled function pointer (skip in shared mode:
         * blob addresses are in separate mmap, outside relocation range) */
        char name[32];
        snprintf(name, sizeof(name), "dfn_%d", i);
        step_direct_imports[i] = MIR_new_import(ctx, name);
        step_direct_ret_types[i] = dfn->ret_type;
        step_direct_arg0_types[i] = dfn->arg_types[0];
      }
#endif
      else {
        char name[32];
        snprintf(name, sizeof(name), "fn_%d", i);
        step_fn_imports[i] = MIR_new_import(ctx, name);
      }
    }
#ifdef HAVE_EEOP_HASHDATUM
    else if (opcode == EEOP_HASHDATUM_FIRST ||
             opcode == EEOP_HASHDATUM_FIRST_STRICT ||
             opcode == EEOP_HASHDATUM_NEXT32 ||
             opcode == EEOP_HASHDATUM_NEXT32_STRICT) {
      PGFunction hash_fn = op->d.hashdatum.fn_addr;
      const JitDirectFn *dfn = jit_find_direct_fn(hash_fn);
      if (dfn && dfn->jit_fn) {
        char name[32];
        snprintf(name, sizeof(name), "dhfn_%d", i);
        step_direct_imports[i] = MIR_new_import(ctx, name);
        step_direct_ret_types[i] = dfn->ret_type;
        step_direct_arg0_types[i] = dfn->arg_types[0];
      }
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
      else if (!mir_shared_code_mode && dfn && dfn->jit_fn_name &&
               mir_find_precompiled_fn(dfn->jit_fn_name)) {
        char name[32];
        snprintf(name, sizeof(name), "dhfn_%d", i);
        step_direct_imports[i] = MIR_new_import(ctx, name);
        step_direct_ret_types[i] = dfn->ret_type;
        step_direct_arg0_types[i] = dfn->arg_types[0];
      }
#endif
      else {
        char name[32];
        snprintf(name, sizeof(name), "fn_%d", i);
        step_fn_imports[i] = MIR_new_import(ctx, name);
      }
    }
#endif /* HAVE_EEOP_HASHDATUM */
    else if (opcode == EEOP_HASHED_SCALARARRAYOP) {
      /* Import for fallback ExecEvalHashedScalarArrayOp */
      char name[32];
      snprintf(name, sizeof(name), "saop_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
               opcode <= EEOP_AGG_PLAIN_TRANS_BYREF) {
      /* Pre-create import for agg_trans helper */
      char name[32];
      snprintf(name, sizeof(name), "ah_%d", i);
      step_direct_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_AGG_STRICT_DESERIALIZE ||
               opcode == EEOP_AGG_DESERIALIZE) {
      /* Import for deserialize function: fcinfo->flinfo->fn_addr */
      char name[32];
      snprintf(name, sizeof(name), "dser_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_DISTINCT || opcode == EEOP_NOT_DISTINCT) {
      /* Import for equality function in DISTINCT/NOT_DISTINCT */
      char name[32];
      snprintf(name, sizeof(name), "fn_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_NULLIF) {
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
             opcode == EEOP_FIELDSTORE_DEFORM ||
             opcode == EEOP_FIELDSTORE_FORM || opcode == EEOP_CONVERT_ROWTYPE ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
             opcode == EEOP_JSON_CONSTRUCTOR ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
             opcode == EEOP_JSONEXPR_COERCION ||
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
             opcode == EEOP_MERGE_SUPPORT_FUNC ||
#endif
             opcode == EEOP_SUBPLAN || opcode == EEOP_WHOLEROW ||
             opcode == EEOP_AGG_ORDERED_TRANS_DATUM ||
             opcode == EEOP_AGG_ORDERED_TRANS_TUPLE ||
             /* SCALARARRAYOP uses dedicated inline with separate imports */
             opcode == EEOP_CURRENTOFEXPR || opcode == EEOP_NEXTVALUEEXPR ||
             opcode == EEOP_ROW ||
             opcode == EEOP_MINMAX || opcode == EEOP_DOMAIN_NOTNULL ||
             opcode == EEOP_DOMAIN_CHECK || opcode == EEOP_XMLEXPR ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
             opcode == EEOP_IS_JSON ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
             opcode == EEOP_JSONEXPR_COERCION_FINISH ||
#endif
             opcode == EEOP_GROUPING_FUNC || opcode == EEOP_PARAM_CALLBACK ||
             opcode == EEOP_PARAM_EXEC || opcode == EEOP_PARAM_EXTERN) {
      char name[32];
      snprintf(name, sizeof(name), "dc_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    }
#ifdef HAVE_EEOP_IOCOERCE_SAFE
    else if (opcode == EEOP_IOCOERCE_SAFE) {
      /* Two separate imports for output and input functions (like IOCOERCE) */
      char name[32];
      snprintf(name, sizeof(name), "ioc_safe_out_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
      snprintf(name, sizeof(name), "ioc_safe_in_%d", i);
      ioc_in_imports[i] = MIR_new_import(ctx, name);
    }
#endif
    else if (opcode == EEOP_SQLVALUEFUNCTION) {
      /* Import for SVF getter or fallback ExecEvalSQLValueFunction */
      SQLValueFunction *svf = op->d.sqlvaluefunction.svf;
      char name[32];
      switch (svf->op) {
      case SVFOP_CURRENT_DATE:
      case SVFOP_CURRENT_TIME: case SVFOP_CURRENT_TIME_N:
      case SVFOP_CURRENT_TIMESTAMP: case SVFOP_CURRENT_TIMESTAMP_N:
      case SVFOP_LOCALTIME: case SVFOP_LOCALTIME_N:
      case SVFOP_LOCALTIMESTAMP: case SVFOP_LOCALTIMESTAMP_N:
        snprintf(name, sizeof(name), "svf_%d", i);
        step_fn_imports[i] = MIR_new_import(ctx, name);
        break;
      default:
        snprintf(name, sizeof(name), "dc_%d", i);
        step_fn_imports[i] = MIR_new_import(ctx, name);
        break;
      }
    }
    else if (opcode == EEOP_ARRAYEXPR || opcode == EEOP_FIELDSELECT) {
      /* Fallback import for ARRAYEXPR/FIELDSELECT */
      char name[32];
      snprintf(name, sizeof(name), "dc_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    }
    else if (opcode == EEOP_INNER_SYSVAR || opcode == EEOP_OUTER_SYSVAR ||
               opcode == EEOP_SCAN_SYSVAR
#ifdef HAVE_EEOP_OLD_NEW
               || opcode == EEOP_OLD_SYSVAR || opcode == EEOP_NEW_SYSVAR
#endif
    ) {
      char name[32];
      snprintf(name, sizeof(name), "sysvar_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_SBSREF_SUBSCRIPTS) {
      char name[32];
      snprintf(name, sizeof(name), "sbss_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_SBSREF_OLD || opcode == EEOP_SBSREF_ASSIGN ||
               opcode == EEOP_SBSREF_FETCH) {
      char name[32];
      snprintf(name, sizeof(name), "sbsf_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_IOCOERCE) {
      char name[32];
      snprintf(name, sizeof(name), "ioc_out_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
      snprintf(name, sizeof(name), "ioc_in_%d", i);
      ioc_in_imports[i] = MIR_new_import(ctx, name);
    } else if (opcode == EEOP_ROWCOMPARE_STEP) {
      char name[32];
      snprintf(name, sizeof(name), "rcmp_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    }
#ifdef HAVE_EEOP_JSONEXPR
    else if (opcode == EEOP_JSONEXPR_PATH) {
      char name[32];
      snprintf(name, sizeof(name), "jpath_%d", i);
      step_fn_imports[i] = MIR_new_import(ctx, name);
    }
#endif
  }

  /*
   * Create the JIT function.
   */
  func_item = MIR_new_func(ctx, "jit_eval", 1, &res_type, 3, MIR_T_P, "state",
                           MIR_T_P, "econtext", MIR_T_P, "isNull");
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
  MIR_append_insn(
      ctx, func_item,
      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_steps),
                   MIR_new_mem_op(ctx, MIR_T_P, offsetof(ExprState, steps),
                                  r_state, 0, 1)));

  /* r_resvaluep = &state->resvalue */
  MIR_append_insn(
      ctx, func_item,
      MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_resvaluep),
                   MIR_new_reg_op(ctx, r_state),
                   MIR_new_int_op(ctx, offsetof(ExprState, resvalue))));

  /* r_resnullp = &state->resnull */
  MIR_append_insn(
      ctx, func_item,
      MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_resnullp),
                   MIR_new_reg_op(ctx, r_state),
                   MIR_new_int_op(ctx, offsetof(ExprState, resnull))));

  /* r_slot = state->resultslot (may be NULL for non-projecting exprs) */
  MIR_append_insn(
      ctx, func_item,
      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_slot),
                   MIR_new_mem_op(ctx, MIR_T_P, offsetof(ExprState, resultslot),
                                  r_state, 0, 1)));

  /*
   * Initialize rvals/rnulls to 0 before the resultslot NULL check.
   * This ensures MIR's register allocator doesn't leave them with
   * uninitialized garbage on the NULL path (important at opt level 0
   * where MIR may reuse registers from a previous compilation).
   */
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_resultvals),
                               MIR_new_int_op(ctx, 0)));
  MIR_append_insn(ctx, func_item,
                  MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_resultnulls),
                               MIR_new_int_op(ctx, 0)));

  {
    MIR_label_t skip_rs_label = MIR_new_label(ctx);

    /* if (r_slot == 0) goto skip_rs_label */
    MIR_append_insn(
        ctx, func_item,
        MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, skip_rs_label),
                     MIR_new_reg_op(ctx, r_slot), MIR_new_int_op(ctx, 0)));

    /* r_resultvals = resultslot->tts_values */
    MIR_append_insn(
        ctx, func_item,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_resultvals),
                     MIR_new_mem_op(ctx, MIR_T_P,
                                    offsetof(TupleTableSlot, tts_values),
                                    r_slot, 0, 1)));

    /* r_resultnulls = resultslot->tts_isnull */
    MIR_append_insn(
        ctx, func_item,
        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_resultnulls),
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
   * Build jump-target bitmap: is_jump_target[i] is true if any other
   * step can jump to step i.  Used to prevent batch VAR/ASSIGN_VAR
   * from merging steps that are branch targets — a jump into the
   * middle of a batch would skip the batch's load code.
   */
  bool *is_jump_target = palloc0(sizeof(bool) * steps_len);
  for (int i = 0; i < steps_len; i++) {
    ExprEvalStep *s = &steps[i];
    ExprEvalOp sop = ExecEvalStepOp(state, s);
    int tgt = -1;

    switch (sop) {
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
   * CASE binary search pre-scan.
   */
  CaseBSearchInfo case_bsearch[16];
  int num_case_bsearch = 0;
  int next_bsearch_idx = 0;

  num_case_bsearch = pg_jitter_detect_case_bsearch(
      state, steps, steps_len, case_bsearch, 16);

  if (num_case_bsearch > 0) {
    pg_jitter_setup_case_bsearch_arrays(state, steps, steps_len);
    elog(DEBUG1, "pg_jitter[mir]: detected %d CASE bsearch pattern(s)",
         num_case_bsearch);
  }

  /*
   * Emit code for each step.
   */
  for (int opno = 0; opno < steps_len; opno++) {
    ExprEvalStep *op = &steps[opno];
    ExprEvalOp opcode = ExecEvalStepOp(state, op);

    MIR_append_insn(ctx, func_item, step_labels[opno]);

    /*
     * CASE binary search: inject binary search call after the VAR step
     * and skip remaining pattern steps.
     */
    if (next_bsearch_idx < num_case_bsearch &&
        opno == case_bsearch[next_bsearch_idx].start_opno + 1) {
      CaseBSearchInfo *cbi = &case_bsearch[next_bsearch_idx];
      int var_opno = cbi->start_opno;
      ExprEvalStep *var_op = &steps[var_opno];

      /* Load resnull from the VAR step → null check.
       * Note: can't use MIR_STEP_LOAD macro here because 'op' points to
       * the current step (start_opno+1), not the VAR step (start_opno). */
      MIR_reg_t r_null_chk = mir_new_reg(ctx, f, MIR_T_I64, "bsn");
      mir_emit_step_load_ptr(ctx, func_item, r_tmp3, r_steps, var_opno,
                             offsetof(ExprEvalStep, resnull),
                             (uint64_t)(var_op->resnull));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_null_chk),
                                   MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      MIR_insn_t null_label = MIR_new_label(ctx);
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, null_label),
                                   MIR_new_reg_op(ctx, r_null_chk),
                                   MIR_new_int_op(ctx, 0)));

      /* Load resvalue from VAR step */
      MIR_reg_t r_val = mir_new_reg(ctx, f, MIR_T_I64, "bsv");
      mir_emit_step_load_ptr(ctx, func_item, r_tmp3, r_steps, var_opno,
                             offsetof(ExprEvalStep, resvalue),
                             (uint64_t)(var_op->resvalue));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_val),
                                   MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

      /* Load desc pointer from step data (start_opno + 2, dead JINT step) */
      MIR_reg_t r_desc = mir_new_reg(ctx, f, MIR_T_I64, "bsd");
      {
        int64_t off = (int64_t)(cbi->start_opno + 2) * (int64_t)sizeof(ExprEvalStep) +
                      offsetof(ExprEvalStep, d.constval.value);
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_desc),
                                     MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0, 1)));
      }

      /* Select proto + import name for the binary search helper */
      MIR_item_t bs_proto = (cbi->var_type == JIT_TYPE_32) ? proto_bsearch_i32
                                                           : proto_bsearch_i64;

      /* Build a unique import name for this helper */
      char bs_import_name[32];
      snprintf(bs_import_name, sizeof(bs_import_name), "bsearch_%d", opno);

      /* Select the right helper function */
      void *helper_fn = pg_jitter_select_bsearch_helper(cbi);

      /* Create import for the helper */
      MIR_item_t bs_import = MIR_new_import(ctx, bs_import_name);

      /* Call helper: (val, desc) -> Datum */
      MIR_reg_t r_result = mir_new_reg(ctx, f, MIR_T_I64, "bsr");
      MIR_append_insn(ctx, func_item,
                      MIR_new_call_insn(ctx, 5,
                                         MIR_new_ref_op(ctx, bs_proto),
                                         MIR_new_ref_op(ctx, bs_import),
                                         MIR_new_reg_op(ctx, r_result),
                                         MIR_new_reg_op(ctx, r_val),
                                         MIR_new_reg_op(ctx, r_desc)));

      /* Store result to ELSE step's resvalue, set resnull = false */
      {
        ExprEvalStep *else_op = &steps[cbi->end_opno];
        MIR_reg_t r_rv = mir_new_reg(ctx, f, MIR_T_I64, "bsrv");
        mir_emit_step_load_ptr(ctx, func_item, r_rv, r_steps, cbi->end_opno,
                               offsetof(ExprEvalStep, resvalue),
                               (uint64_t)(else_op->resvalue));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_I64, 0, r_rv, 0, 1),
                                     MIR_new_reg_op(ctx, r_result)));

        MIR_reg_t r_rn = mir_new_reg(ctx, f, MIR_T_I64, "bsrn");
        mir_emit_step_load_ptr(ctx, func_item, r_rn, r_steps, cbi->end_opno,
                               offsetof(ExprEvalStep, resnull),
                               (uint64_t)(else_op->resnull));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_U8, 0, r_rn, 0, 1),
                                     MIR_new_int_op(ctx, 0)));
      }

      /* Jump to else_end_opno */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_JMP,
                                   MIR_new_label_op(ctx, step_labels[cbi->else_end_opno])));

      /* Null path */
      MIR_append_insn(ctx, func_item, null_label);
      {
        ExprEvalStep *else_op = &steps[cbi->end_opno];
        MIR_reg_t r_rv2 = mir_new_reg(ctx, f, MIR_T_I64, "bsnrv");
        mir_emit_step_load_ptr(ctx, func_item, r_rv2, r_steps, cbi->end_opno,
                               offsetof(ExprEvalStep, resvalue),
                               (uint64_t)(else_op->resvalue));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_I64, 0, r_rv2, 0, 1),
                                     MIR_new_int_op(ctx, (int64_t)cbi->default_result)));

        MIR_reg_t r_rn2 = mir_new_reg(ctx, f, MIR_T_I64, "bsnrn");
        mir_emit_step_load_ptr(ctx, func_item, r_rn2, r_steps, cbi->end_opno,
                               offsetof(ExprEvalStep, resnull),
                               (uint64_t)(else_op->resnull));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_U8, 0, r_rn2, 0, 1),
                                     MIR_new_int_op(ctx, cbi->default_is_null ? 1 : 0)));
      }
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_JMP,
                                   MIR_new_label_op(ctx, step_labels[cbi->else_end_opno])));

      /* Bind labels for skipped steps */
      for (int skip = opno + 1; skip < cbi->else_end_opno && skip < steps_len; skip++)
        MIR_append_insn(ctx, func_item, step_labels[skip]);

      opno = cbi->else_end_opno - 1;
      next_bsearch_idx++;
      continue;
    }

    switch (opcode) {
    /*
     * ---- DONE ----
     */
    case EEOP_DONE_RETURN: {
      /* r_tmp1 = state->resvalue */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_mem_op(ctx, MIR_T_I64,
                                                  offsetof(ExprState, resvalue),
                                                  r_state, 0, 1)));
      /* r_tmp2 = state->resnull (byte) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_mem_op(ctx, MIR_T_U8,
                                                  offsetof(ExprState, resnull),
                                                  r_state, 0, 1)));
      /* *isNull = resnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_isnullp, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));
      /* return resvalue */
      MIR_append_insn(ctx, func_item,
                      MIR_new_ret_insn(ctx, 1, MIR_new_reg_op(ctx, r_tmp1)));
      break;
    }

#ifdef HAVE_EEOP_DONE_SPLIT
    case EEOP_DONE_NO_RETURN: {
      MIR_append_insn(ctx, func_item,
                      MIR_new_ret_insn(ctx, 1, MIR_new_int_op(ctx, 0)));
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
      MIR_insn_t skip_label = MIR_new_label(ctx);
      int64_t soff = mir_slot_offset(opcode);

      /* r_slot = econtext->ecxt_*tuple */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_slot),
                       MIR_new_mem_op(ctx, MIR_T_P, soff, r_econtext, 0, 1)));

      /* r_tmp1 = slot->tts_nvalid (int16) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I16,
                                      offsetof(TupleTableSlot, tts_nvalid),
                                      r_slot, 0, 1)));

      /* if tts_nvalid >= last_var, skip */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_BGE,
                                   MIR_new_label_op(ctx, skip_label),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_int_op(ctx, op->d.fetch.last_var)));

      /*
       * Call pg_jitter_compiled_deform_dispatch(slot, natts).
       * This dispatches to SLJIT-compiled deform functions which are
       * faster than MIR's inline deform, especially for wide tables.
       */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_getsomeattrs),
                            MIR_new_ref_op(ctx, import_getsomeattrs),
                            MIR_new_reg_op(ctx, r_slot),
                            MIR_new_int_op(ctx, op->d.fetch.last_var)));

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
      int64_t soff = mir_slot_offset(opcode);

      /*
       * Look ahead: count consecutive VARs from the same slot.
       * Stop before any step that is a jump target.
       */
      int batch_end = opno;
      while (batch_end + 1 < steps_len) {
        if (is_jump_target[batch_end + 1])
          break;
        ExprEvalOp next_opc = ExecEvalStepOp(state, &steps[batch_end + 1]);
        if (next_opc != opcode)
          break;
        batch_end++;
      }
      int batch_count = batch_end - opno + 1;

      /* r_slot = econtext->ecxt_*tuple */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_slot),
                       MIR_new_mem_op(ctx, MIR_T_P, soff, r_econtext, 0,
                                      1)));

      /* Phase 1: load all values from tts_values */
      /* r_tmp1 = slot->tts_values */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(TupleTableSlot, tts_values),
                                      r_slot, 0, 1)));
      for (int bi = 0; bi < batch_count; bi++) {
        ExprEvalStep *cur = &steps[opno + bi];
        int attnum = cur->d.var.attnum;
        /* r_tmp2 = tts_values[attnum] */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64,
                                        attnum * (int64_t)sizeof(Datum),
                                        r_tmp1, 0, 1)));
        /* *cur->resvalue = r_tmp2 */
        mir_emit_step_load_ptr(ctx, func_item, r_tmp3, r_steps, opno + bi,
                               offsetof(ExprEvalStep, resvalue),
                               (uint64_t)cur->resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
      }

      /* Phase 2: load all isnulls from tts_isnull */
      /* r_tmp1 = slot->tts_isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(TupleTableSlot, tts_isnull),
                                      r_slot, 0, 1)));
      for (int bi = 0; bi < batch_count; bi++) {
        ExprEvalStep *cur = &steps[opno + bi];
        int attnum = cur->d.var.attnum;
        /* r_tmp2 = tts_isnull[attnum] */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        attnum * (int64_t)sizeof(bool),
                                        r_tmp1, 0, 1)));
        /* *cur->resnull = r_tmp2 */
        mir_emit_step_load_ptr(ctx, func_item, r_tmp3, r_steps, opno + bi,
                               offsetof(ExprEvalStep, resnull),
                               (uint64_t)cur->resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
      }

      /* Emit labels for skipped steps so jump targets work */
      for (int bi = 1; bi < batch_count; bi++)
        MIR_append_insn(ctx, func_item, step_labels[opno + bi]);

      opno = batch_end; /* advance past batch */
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
      int64_t soff = mir_slot_offset(opcode);

      /*
       * Look ahead: count consecutive ASSIGN_VARs from the same slot.
       * Stop before any step that is a jump target.
       */
      int batch_end = opno;
      while (batch_end + 1 < steps_len) {
        if (is_jump_target[batch_end + 1])
          break;
        ExprEvalOp next_opc = ExecEvalStepOp(state, &steps[batch_end + 1]);
        if (next_opc != opcode)
          break;
        batch_end++;
      }
      int batch_count = batch_end - opno + 1;

      /* r_slot = source slot */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_slot),
                       MIR_new_mem_op(ctx, MIR_T_P, soff, r_econtext, 0,
                                      1)));

      /* Phase 1: copy all values from tts_values to resultvals */
      /* r_tmp1 = slot->tts_values */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(TupleTableSlot, tts_values),
                                      r_slot, 0, 1)));
      for (int bi = 0; bi < batch_count; bi++) {
        ExprEvalStep *cur = &steps[opno + bi];
        int attnum = cur->d.assign_var.attnum;
        int resultnum = cur->d.assign_var.resultnum;
        /* r_tmp2 = tts_values[attnum] */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64,
                                        attnum * (int64_t)sizeof(Datum),
                                        r_tmp1, 0, 1)));
        /* resultvals[resultnum] = r_tmp2 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64,
                                        resultnum * (int64_t)sizeof(Datum),
                                        r_resultvals, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
      }

      /* Phase 2: copy all isnulls from tts_isnull to resultnulls */
      /* r_tmp1 = slot->tts_isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(TupleTableSlot, tts_isnull),
                                      r_slot, 0, 1)));
      for (int bi = 0; bi < batch_count; bi++) {
        ExprEvalStep *cur = &steps[opno + bi];
        int attnum = cur->d.assign_var.attnum;
        int resultnum = cur->d.assign_var.resultnum;
        /* r_tmp2 = tts_isnull[attnum] */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        attnum * (int64_t)sizeof(bool),
                                        r_tmp1, 0, 1)));
        /* resultnulls[resultnum] = r_tmp2 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        resultnum * (int64_t)sizeof(bool),
                                        r_resultnulls, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
      }

      /* Emit labels for skipped steps so jump targets work */
      for (int bi = 1; bi < batch_count; bi++)
        MIR_append_insn(ctx, func_item, step_labels[opno + bi]);

      opno = batch_end; /* advance past batch */
      break;
    }

    /*
     * ---- ASSIGN_TMP / ASSIGN_TMP_MAKE_RO ----
     */
    case EEOP_ASSIGN_TMP:
    case EEOP_ASSIGN_TMP_MAKE_RO: {
      int resultnum = op->d.assign_tmp.resultnum;

      /* r_tmp1 = state->resvalue */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_mem_op(ctx, MIR_T_I64,
                                                  offsetof(ExprState, resvalue),
                                                  r_state, 0, 1)));
      /* r_tmp2 = state->resnull */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_mem_op(ctx, MIR_T_U8,
                                                  offsetof(ExprState, resnull),
                                                  r_state, 0, 1)));

      /* Store null first */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8,
                                      resultnum * (int64_t)sizeof(bool),
                                      r_resultnulls, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      if (opcode == EEOP_ASSIGN_TMP_MAKE_RO) {
        MIR_insn_t skip_ro = MIR_new_label(ctx);
        MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "mro");

        /* If null, skip MakeReadOnly */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, skip_ro),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

        /* r_tmp1 = MakeExpandedObjectReadOnlyInternal(r_tmp1) */
        MIR_append_insn(ctx, func_item,
                        MIR_new_call_insn(ctx, 4,
                                          MIR_new_ref_op(ctx, proto_makero),
                                          MIR_new_ref_op(ctx, import_makero),
                                          MIR_new_reg_op(ctx, r_ret),
                                          MIR_new_reg_op(ctx, r_tmp1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                     MIR_new_reg_op(ctx, r_ret)));

        MIR_append_insn(ctx, func_item, skip_ro);
      }

      /* Store value */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64,
                                      resultnum * (int64_t)sizeof(Datum),
                                      r_resultvals, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp1)));
      break;
    }

    /*
     * ---- CONST ----
     */
    case EEOP_CONST: {
      /* *op->resvalue = constval.value */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      if (mir_shared_code_mode) {
        /*
         * In shared mode, load constval.value from step data at runtime.
         * Pass-by-reference Datums (text, bytea, etc.) are pointers into
         * process-local heap memory that differ between leader and worker.
         */
        MIR_reg_t r_val = mir_new_reg(ctx, f, MIR_T_I64, "cval");
        int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                      offsetof(ExprEvalStep, d.constval.value);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_val),
                         MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_reg_op(ctx, r_val)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, (int64_t)op->d.constval.value)));
      }

      /* *op->resnull = constval.isnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
      int nargs = op->d.func.nargs;
      MIR_insn_t done_label = MIR_new_label(ctx);
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "fret");
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "fci");

      if (opcode == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
          || opcode == EEOP_FUNCEXPR_STRICT_1 ||
          opcode == EEOP_FUNCEXPR_STRICT_2
#endif
      ) {
        /* Set resnull = true */
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 1)));

        /*
         * Batched null check: OR all isnull flags, single branch.
         */
        MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
        if (nargs > 1 && nargs <= 4) {
          int64_t null_off_0 =
              (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_tmp1),
                                       MIR_new_mem_op(ctx, MIR_T_U8, null_off_0,
                                                      r_fci, 0, 1)));
          for (int argno = 1; argno < nargs; argno++) {
            int64_t null_off =
                (int64_t)((char *)&fcinfo->args[argno].isnull - (char *)fcinfo);
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
        } else {
          for (int argno = 0; argno < nargs; argno++) {
            int64_t null_off =
                (int64_t)((char *)&fcinfo->args[argno].isnull - (char *)fcinfo);
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

        if (dfn && (dfn->inline_op == JIT_INLINE_TEXT_EQ ||
                    dfn->inline_op == JIT_INLINE_TEXT_NE) &&
            pg_jitter_collation_is_deterministic(fcinfo->fncollation)) {
          /*
           * TIER 0a — FULLY INLINE TEXT EQ/NE.
           * Short varlena (≤ 7 data bytes): zero function calls.
           * Longer short varlena: memcmp call only.
           * Non-short (toast/compressed): call jit_text_datum_eq/ne.
           * Only for deterministic collations; non-deterministic falls
           * through to V1.
           */
          {
            bool is_eq = (dfn->inline_op == JIT_INLINE_TEXT_EQ);

            MIR_insn_t lbl_result_eq = MIR_new_label(ctx);
            MIR_insn_t lbl_result_ne = MIR_new_label(ctx);
            MIR_insn_t lbl_slow = MIR_new_label(ctx);
            MIR_insn_t lbl_memcmp = MIR_new_label(ctx);
            MIR_insn_t lbl_store = MIR_new_label(ctx);

            char rn[32];
            snprintf(rn, sizeof(rn), "ta0_%d", opno);
            MIR_reg_t ta0 = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "ta1_%d", opno);
            MIR_reg_t ta1 = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "thdra_%d", opno);
            MIR_reg_t thdra = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "thdrb_%d", opno);
            MIR_reg_t thdrb = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "tlen_%d", opno);
            MIR_reg_t tlen = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "ttmp_%d", opno);
            MIR_reg_t ttmp = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "twa_%d", opno);
            MIR_reg_t twa = mir_new_reg(ctx, f, MIR_T_I64, rn);
            snprintf(rn, sizeof(rn), "twb_%d", opno);
            MIR_reg_t twb = mir_new_reg(ctx, f, MIR_T_I64, rn);

            int64_t val_off_0 =
                (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
            int64_t val_off_1 =
                (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

            /* Load fcinfo, then args */
            MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ta0),
                                         MIR_new_mem_op(ctx, MIR_T_I64, val_off_0,
                                                        r_fci, 0, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ta1),
                                         MIR_new_mem_op(ctx, MIR_T_I64, val_off_1,
                                                        r_fci, 0, 1)));

            /* 1. Pointer equality → result_eq */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_result_eq),
                                         MIR_new_reg_op(ctx, ta0),
                                         MIR_new_reg_op(ctx, ta1)));

            /* 2. Load uint8 headers */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, thdra),
                                         MIR_new_mem_op(ctx, MIR_T_U8, 0,
                                                        ta0, 0, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, thdrb),
                                         MIR_new_mem_op(ctx, MIR_T_U8, 0,
                                                        ta1, 0, 1)));

            /* 3. Check hdr_a short: (hdr_a & 1)==0 → slow */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_ANDS,
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_reg_op(ctx, thdra),
                                         MIR_new_int_op(ctx, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_slow),
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_int_op(ctx, 0)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_slow),
                                         MIR_new_reg_op(ctx, thdra),
                                         MIR_new_int_op(ctx, 1)));

            /* Check hdr_b short */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_ANDS,
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_reg_op(ctx, thdrb),
                                         MIR_new_int_op(ctx, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_slow),
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_int_op(ctx, 0)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_slow),
                                         MIR_new_reg_op(ctx, thdrb),
                                         MIR_new_int_op(ctx, 1)));

            /* 4. Headers equal? (same length) */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BNE,
                                         MIR_new_label_op(ctx, lbl_result_ne),
                                         MIR_new_reg_op(ctx, thdra),
                                         MIR_new_reg_op(ctx, thdrb)));

            /* 5. data_len = (hdr >> 1) - 1 */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_RSHS,
                                         MIR_new_reg_op(ctx, tlen),
                                         MIR_new_reg_op(ctx, thdra),
                                         MIR_new_int_op(ctx, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_SUBS,
                                         MIR_new_reg_op(ctx, tlen),
                                         MIR_new_reg_op(ctx, tlen),
                                         MIR_new_int_op(ctx, 1)));

            /* data_len == 0 → result_eq */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_result_eq),
                                         MIR_new_reg_op(ctx, tlen),
                                         MIR_new_int_op(ctx, 0)));

            /* 6. data_len > 7 → memcmp */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BGTS,
                                         MIR_new_label_op(ctx, lbl_memcmp),
                                         MIR_new_reg_op(ctx, tlen),
                                         MIR_new_int_op(ctx, 7)));

            /* 7. Inline word comparison (data_len 1-7):
             *    Load 8 bytes, shift left by (7-data_len)*8, compare */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, twa),
                                         MIR_new_mem_op(ctx, MIR_T_I64, 0,
                                                        ta0, 0, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, twb),
                                         MIR_new_mem_op(ctx, MIR_T_I64, 0,
                                                        ta1, 0, 1)));
            /* shift = (7 - data_len) * 8 */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_SUB,
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_int_op(ctx, 7),
                                         MIR_new_reg_op(ctx, tlen)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_LSH,
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_reg_op(ctx, ttmp),
                                         MIR_new_int_op(ctx, 3)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_LSH,
                                         MIR_new_reg_op(ctx, twa),
                                         MIR_new_reg_op(ctx, twa),
                                         MIR_new_reg_op(ctx, ttmp)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_LSH,
                                         MIR_new_reg_op(ctx, twb),
                                         MIR_new_reg_op(ctx, twb),
                                         MIR_new_reg_op(ctx, ttmp)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_result_eq),
                                         MIR_new_reg_op(ctx, twa),
                                         MIR_new_reg_op(ctx, twb)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_JMP,
                                         MIR_new_label_op(ctx, lbl_result_ne)));

            /* 8. memcmp path (data_len > 7) */
            MIR_append_insn(ctx, func_item, lbl_memcmp);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_ADD,
                                         MIR_new_reg_op(ctx, ta0),
                                         MIR_new_reg_op(ctx, ta0),
                                         MIR_new_int_op(ctx, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_ADD,
                                         MIR_new_reg_op(ctx, ta1),
                                         MIR_new_reg_op(ctx, ta1),
                                         MIR_new_int_op(ctx, 1)));
            {
              char pn[32], in_name[32];
              snprintf(pn, sizeof(pn), "p_memcmp_%d", opno);
              snprintf(in_name, sizeof(in_name), "memcmp_%d", opno);
              MIR_type_t mc_ret = MIR_T_I32;
              MIR_item_t proto_mc = MIR_new_proto(
                  ctx, pn, 1, &mc_ret, 3,
                  MIR_T_I64, "s1", MIR_T_I64, "s2", MIR_T_I64, "n");
              MIR_item_t import_mc = MIR_new_import(ctx, in_name);
              MIR_load_external(ctx, in_name,
                                mir_extern_addr((void *)memcmp));
              MIR_append_insn(ctx, func_item,
                              MIR_new_call_insn(
                                  ctx, 6, MIR_new_ref_op(ctx, proto_mc),
                                  MIR_new_ref_op(ctx, import_mc),
                                  MIR_new_reg_op(ctx, r_ret),
                                  MIR_new_reg_op(ctx, ta0),
                                  MIR_new_reg_op(ctx, ta1),
                                  MIR_new_reg_op(ctx, tlen)));
            }
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_BEQ,
                                         MIR_new_label_op(ctx, lbl_result_eq),
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_int_op(ctx, 0)));
            /* memcmp != 0 → fall through to result_ne */

            /* result_ne */
            MIR_append_insn(ctx, func_item, lbl_result_ne);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_int_op(ctx, is_eq ? 0 : 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_JMP,
                                         MIR_new_label_op(ctx, lbl_store)));

            /* result_eq */
            MIR_append_insn(ctx, func_item, lbl_result_eq);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_int_op(ctx, is_eq ? 1 : 0)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_JMP,
                                         MIR_new_label_op(ctx, lbl_store)));

            /* slow path: call jit_text_datum_eq/ne */
            MIR_append_insn(ctx, func_item, lbl_slow);
            MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ta0),
                                         MIR_new_mem_op(ctx, MIR_T_I64, val_off_0,
                                                        r_fci, 0, 1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, ta1),
                                         MIR_new_mem_op(ctx, MIR_T_I64, val_off_1,
                                                        r_fci, 0, 1)));
            {
              char pn[32], in_name[32];
              snprintf(pn, sizeof(pn), "p_txdeq_%d", opno);
              snprintf(in_name, sizeof(in_name), "txdeq_%d", opno);
              MIR_type_t tc_ret = MIR_T_I32;
              MIR_item_t proto_tc = MIR_new_proto(
                  ctx, pn, 1, &tc_ret, 2,
                  MIR_T_I64, "a", MIR_T_I64, "b");
              MIR_item_t import_tc = MIR_new_import(ctx, in_name);
              MIR_load_external(ctx, in_name,
                                mir_extern_addr(is_eq ? (void *)jit_text_datum_eq
                                                      : (void *)jit_text_datum_ne));
              MIR_append_insn(ctx, func_item,
                              MIR_new_call_insn(
                                  ctx, 5, MIR_new_ref_op(ctx, proto_tc),
                                  MIR_new_ref_op(ctx, import_tc),
                                  MIR_new_reg_op(ctx, r_ret),
                                  MIR_new_reg_op(ctx, ta0),
                                  MIR_new_reg_op(ctx, ta1)));
            }
            /* fall through to store */

            /* store_result */
            MIR_append_insn(ctx, func_item, lbl_store);

            /* *op->resvalue = r_ret */
            MIR_STEP_LOAD(r_tmp3, opno, resvalue);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV,
                             MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                             MIR_new_reg_op(ctx, r_ret)));

            /* *op->resnull = false */
            MIR_STEP_LOAD(r_tmp3, opno, resnull);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV,
                             MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                             MIR_new_int_op(ctx, 0)));
          }
        } else if (dfn && dfn->inline_op >= JIT_INLINE_INT4_ADD &&
            dfn->inline_op <= JIT_INLINE_INT8_GE) {
          /*
           * TIER 0 — INLINE: emit the operation as MIR
           * instructions for int32/int64 ops.
           * Float8 inline ops are not handled here — they
           * fall through to the direct-call path below.
           */
          MIR_reg_t a0 = mir_new_reg(ctx, f, MIR_T_I64, "ia0");
          MIR_reg_t a1 = mir_new_reg(ctx, f, MIR_T_I64, "ia1");
          int64_t val_off_0 =
              (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
          int64_t val_off_1 =
              (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);
          MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, a0),
                                       MIR_new_mem_op(ctx, MIR_T_I64, val_off_0,
                                                      r_fci, 0, 1)));
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, a1),
                                       MIR_new_mem_op(ctx, MIR_T_I64, val_off_1,
                                                      r_fci, 0, 1)));

          switch ((JitInlineOp)dfn->inline_op) {
          /* ---- int32 arithmetic (overflow-checked) ---- */
          case JIT_INLINE_INT4_ADD: {
            MIR_insn_t ok = MIR_new_label(ctx);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_ADDOS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int4_overflow)));
            MIR_append_insn(ctx, func_item, ok);
            /* Sign-extend 32→64 */
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_EXT32,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_reg_op(ctx, r_ret)));
            break;
          }
          case JIT_INLINE_INT4_SUB: {
            MIR_insn_t ok = MIR_new_label(ctx);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_SUBOS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int4_overflow)));
            MIR_append_insn(ctx, func_item, ok);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_EXT32,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_reg_op(ctx, r_ret)));
            break;
          }
          case JIT_INLINE_INT4_MUL: {
            MIR_insn_t ok = MIR_new_label(ctx);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MULOS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int4_overflow)));
            MIR_append_insn(ctx, func_item, ok);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_EXT32,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_reg_op(ctx, r_ret)));
            break;
          }
          case JIT_INLINE_INT4_DIV: {
            MIR_insn_t not_zero = MIR_new_label(ctx);
            MIR_insn_t not_minmax = MIR_new_label(ctx);
            MIR_insn_t not_neg1 = MIR_new_label(ctx);
            /* Check divisor == 0 */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNES, MIR_new_label_op(ctx, not_zero),
                             MIR_new_reg_op(ctx, a1), MIR_new_int_op(ctx, 0)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_call_insn(ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                  MIR_new_ref_op(ctx, import_err_div_by_zero)));
            MIR_append_insn(ctx, func_item, not_zero);
            /* Check INT32_MIN / -1 */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNES, MIR_new_label_op(ctx, not_minmax),
                             MIR_new_reg_op(ctx, a0),
                             MIR_new_int_op(ctx, (int32_t)PG_INT32_MIN)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNES, MIR_new_label_op(ctx, not_neg1),
                             MIR_new_reg_op(ctx, a1), MIR_new_int_op(ctx, -1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int4_overflow)));
            MIR_append_insn(ctx, func_item, not_neg1);
            MIR_append_insn(ctx, func_item, not_minmax);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_DIVS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_EXT32,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_reg_op(ctx, r_ret)));
            break;
          }
          case JIT_INLINE_INT4_MOD: {
            MIR_insn_t not_zero = MIR_new_label(ctx);
            MIR_insn_t not_minmax = MIR_new_label(ctx);
            MIR_insn_t zero_result = MIR_new_label(ctx);
            MIR_insn_t after = MIR_new_label(ctx);
            /* Check divisor == 0 */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNES, MIR_new_label_op(ctx, not_zero),
                             MIR_new_reg_op(ctx, a1), MIR_new_int_op(ctx, 0)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_call_insn(ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                  MIR_new_ref_op(ctx, import_err_div_by_zero)));
            MIR_append_insn(ctx, func_item, not_zero);
            /* Check INT32_MIN % -1 → 0 */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNES, MIR_new_label_op(ctx, not_minmax),
                             MIR_new_reg_op(ctx, a0),
                             MIR_new_int_op(ctx, (int32_t)PG_INT32_MIN)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BEQS, MIR_new_label_op(ctx, zero_result),
                             MIR_new_reg_op(ctx, a1), MIR_new_int_op(ctx, -1)));
            MIR_append_insn(ctx, func_item, not_minmax);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MODS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_EXT32,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_reg_op(ctx, r_ret)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, after)));
            MIR_append_insn(ctx, func_item, zero_result);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV,
                                         MIR_new_reg_op(ctx, r_ret),
                                         MIR_new_int_op(ctx, 0)));
            MIR_append_insn(ctx, func_item, after);
            break;
          }
          /* ---- int64 arithmetic (overflow-checked) ---- */
          case JIT_INLINE_INT8_ADD: {
            MIR_insn_t ok = MIR_new_label(ctx);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_ADDO, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int8_overflow)));
            MIR_append_insn(ctx, func_item, ok);
            break;
          }
          case JIT_INLINE_INT8_SUB: {
            MIR_insn_t ok = MIR_new_label(ctx);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_SUBO, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int8_overflow)));
            MIR_append_insn(ctx, func_item, ok);
            break;
          }
          case JIT_INLINE_INT8_MUL: {
            MIR_insn_t ok = MIR_new_label(ctx);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MULO, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                                MIR_new_ref_op(ctx, import_err_int8_overflow)));
            MIR_append_insn(ctx, func_item, ok);
            break;
          }
          /* ---- int32 comparison ---- */
          case JIT_INLINE_INT4_EQ:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_EQS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT4_NE:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_NES, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT4_LT:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_LTS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT4_LE:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_LES, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT4_GT:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_GTS, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT4_GE:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_GES, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          /* ---- int64 comparison ---- */
          case JIT_INLINE_INT8_EQ:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT8_NE:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_NE, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT8_LT:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_LT, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT8_LE:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_LE, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT8_GT:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_GT, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          case JIT_INLINE_INT8_GE:
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_GE, MIR_new_reg_op(ctx, r_ret),
                             MIR_new_reg_op(ctx, a0), MIR_new_reg_op(ctx, a1)));
            break;
          default:
            Assert(false);
            break;
          }

          /* *op->resvalue = r_ret */
          MIR_STEP_LOAD(r_tmp3, opno, resvalue);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                           MIR_new_reg_op(ctx, r_ret)));

          /* *op->resnull = false */
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, 0)));
        } else if (dfn && step_direct_imports[opno]) {
          /*
           * Direct native call — either from dfn->jit_fn or
           * MIR-precompiled function pointer.  Match arg types
           * to the actual C signature to avoid ABI mismatch on ARM64.
           */
          /* Load fcinfo once, then load all args from offsets */
          MIR_reg_t r_args[4];
          MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
          for (int i = 0; i < dfn->nargs; i++) {
            char aname[16];
            snprintf(aname, sizeof(aname), "da%d_%d", i, opno);
            /*
             * Always use I64 registers — MIR_MOV only supports I64.
             * Datum values in fcinfo->args[].value are always 8 bytes.
             * The function call proto handles the I64→I32 narrowing.
             */
            r_args[i] = mir_new_reg(ctx, f, MIR_T_I64, aname);
            int64_t val_off =
                (int64_t)((char *)&fcinfo->args[i].value - (char *)fcinfo);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV,
                                         MIR_new_reg_op(ctx, r_args[i]),
                                         MIR_new_mem_op(ctx, MIR_T_I64, val_off,
                                                        r_fci, 0, 1)));
          }

          /* Direct call — select proto matching ret + arg0 types */
          bool has_coll = (dfn->flags & JIT_FN_FLAG_COLLATION) != 0;
          if (has_coll && dfn->nargs == 2) {
            /* Collation-aware 2-arg call: load fncollation as 3rd arg */
            char cname[16];
            snprintf(cname, sizeof(cname), "dcoll_%d", opno);
            MIR_reg_t r_coll = mir_new_reg(ctx, f, MIR_T_I64, cname);
            MIR_append_insn(ctx, func_item,
                            MIR_new_insn(ctx, MIR_MOV,
                                         MIR_new_reg_op(ctx, r_coll),
                                         MIR_new_mem_op(ctx, MIR_T_I32,
                                             offsetof(FunctionCallInfoBaseData,
                                                      fncollation),
                                             r_fci, 0, 1)));
            MIR_item_t d_proto =
                proto_direct_coll[step_direct_ret_types[opno]]
                                 [step_direct_arg0_types[opno]];
            MIR_append_insn(ctx, func_item,
                            MIR_new_call_insn(
                                ctx, 6, MIR_new_ref_op(ctx, d_proto),
                                MIR_new_ref_op(ctx, step_direct_imports[opno]),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_args[0]),
                                MIR_new_reg_op(ctx, r_args[1]),
                                MIR_new_reg_op(ctx, r_coll)));
          } else {
            int nargs_idx = (dfn->nargs >= 2) ? 1 : 0;
            MIR_item_t d_proto =
                proto_direct[nargs_idx][step_direct_ret_types[opno]]
                            [step_direct_arg0_types[opno]];
            if (dfn->nargs == 1) {
              MIR_append_insn(ctx, func_item,
                              MIR_new_call_insn(
                                  ctx, 4, MIR_new_ref_op(ctx, d_proto),
                                  MIR_new_ref_op(ctx, step_direct_imports[opno]),
                                  MIR_new_reg_op(ctx, r_ret),
                                  MIR_new_reg_op(ctx, r_args[0])));
            } else {
              MIR_append_insn(ctx, func_item,
                              MIR_new_call_insn(
                                  ctx, 5, MIR_new_ref_op(ctx, d_proto),
                                  MIR_new_ref_op(ctx, step_direct_imports[opno]),
                                  MIR_new_reg_op(ctx, r_ret),
                                  MIR_new_reg_op(ctx, r_args[0]),
                                  MIR_new_reg_op(ctx, r_args[1])));
            }
          }

          /* *op->resvalue = result */
          MIR_STEP_LOAD(r_tmp3, opno, resvalue);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                           MIR_new_reg_op(ctx, r_ret)));

          /* *op->resnull = false */
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, 0)));
        } else {
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
          if (!mir_shared_code_mode) {
            PGFunction fn = op->d.func.fn_addr;
            bool is_like_fn = (fn == textlike || fn == textnlike);
            bool is_ilike_fn = (fn == texticlike || fn == texticnlike);
            bool is_regex_fn = (fn == textregexeq || fn == textregexne);
            bool is_iregex_fn = (fn == texticregexeq || fn == texticregexne);
            bool vs_negate = (fn == textnlike || fn == texticnlike ||
                              fn == textregexne || fn == texticregexne);

            if ((is_like_fn || is_ilike_fn || is_regex_fn || is_iregex_fn) &&
                fcinfo->nargs == 2) {
              /*
               * Check if args[1] (the pattern) is a compile-time constant.
               * PG's ExecInitFunc directly assigns Const values into
               * fcinfo->args[argno] without generating an EEOP_CONST step.
               * So if no step writes to args[1], it was pre-initialized
               * from a Const node — treat it as constant.
               */
              bool pat_const = true;
              for (int j = opno - 1; j >= 0; j--) {
                if (steps[j].resvalue == &fcinfo->args[1].value) {
                  pat_const = (steps[j].opcode == EEOP_CONST);
                  break;
                }
              }

              /*
               * LIKE/regex: Vectorscan/StringZilla use POSIX character
               * classes, not ICU.  Only safe for C/POSIX collation.
               */
              if (pat_const && !fcinfo->args[1].isnull &&
                  pg_jitter_collation_is_c(fcinfo->fncollation)) {
                text *pat_text = DatumGetTextPP(fcinfo->args[1].value);
                char *pat_str = VARDATA_ANY(pat_text);
                int pat_len = VARSIZE_ANY_EXHDR(pat_text);

                /*
                 * StringZilla fast path for simple LIKE patterns.
                 * Only for LIKE/NOT LIKE (not ILIKE/regex).
                 */
                if (is_like_fn) {
                  const char *literal;
                  int literal_len;
                  int match_type = simd_like_classify(
                      pat_str, pat_len, &literal, &literal_len);

                  if (match_type >= 0 && literal != NULL) {
                    char sld_name[32], sll_name[32], slm_name[32];
                    snprintf(sld_name, sizeof(sld_name), "sld_%d", opno);
                    snprintf(sll_name, sizeof(sll_name), "sll_%d", opno);
                    snprintf(slm_name, sizeof(slm_name), "slm_%d", opno);
                    MIR_reg_t r_datum = mir_new_reg(ctx, f, MIR_T_I64, sld_name);
                    MIR_reg_t r_litptr = mir_new_reg(ctx, f, MIR_T_I64, sll_name);
                    MIR_reg_t r_litlen = mir_new_reg(ctx, f, MIR_T_I64, slm_name);

                    /* Load args[0].value */
                    MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
                    int64_t off0 =
                        (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_datum),
                                     MIR_new_mem_op(ctx, MIR_T_I64, off0,
                                                    r_fci, 0, 1)));

                    /* Load literal pointer + length as immediates */
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_litptr),
                                     MIR_new_int_op(ctx, (int64_t)(uintptr_t)literal)));
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_litlen),
                                     MIR_new_int_op(ctx, (int64_t)literal_len)));

                    /* Declare proto/import for simd_like_match_text */
                    char pn[64], in_name[64];
                    snprintf(pn, sizeof(pn), "proto_szlike_%d", opno);
                    snprintf(in_name, sizeof(in_name), "import_szlike_%d", opno);
                    MIR_type_t szlike_res = MIR_T_I32;
                    MIR_item_t proto_szlike = MIR_new_proto(
                        ctx, pn, 1, &szlike_res, 4,
                        MIR_T_I64, "d", MIR_T_I64, "p",
                        MIR_T_I32, "l", MIR_T_I32, "t");
                    MIR_item_t import_szlike = MIR_new_import(ctx, in_name);
                    MIR_load_external(ctx, in_name,
                                      mir_extern_addr((void *)simd_like_match_text));

                    /* Call simd_like_match_text(datum, literal, len, type) */
                    char slmt_name[32];
                    snprintf(slmt_name, sizeof(slmt_name), "slmt_%d", opno);
                    MIR_reg_t r_mt = mir_new_reg(ctx, f, MIR_T_I64, slmt_name);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_mt),
                                     MIR_new_int_op(ctx, (int64_t)match_type)));
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_call_insn(ctx, 7,
                                          MIR_new_ref_op(ctx, proto_szlike),
                                          MIR_new_ref_op(ctx, import_szlike),
                                          MIR_new_reg_op(ctx, r_ret),
                                          MIR_new_reg_op(ctx, r_datum),
                                          MIR_new_reg_op(ctx, r_litptr),
                                          MIR_new_reg_op(ctx, r_litlen),
                                          MIR_new_reg_op(ctx, r_mt)));

                    /* Sign-extend I32 result to I64 */
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32,
                                     MIR_new_reg_op(ctx, r_ret),
                                     MIR_new_reg_op(ctx, r_ret)));

                    if (vs_negate)
                      MIR_append_insn(
                          ctx, func_item,
                          MIR_new_insn(ctx, MIR_XOR,
                                       MIR_new_reg_op(ctx, r_ret),
                                       MIR_new_reg_op(ctx, r_ret),
                                       MIR_new_int_op(ctx, 1)));

                    MIR_STEP_LOAD(r_tmp3, opno, resvalue);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                                     MIR_new_reg_op(ctx, r_ret)));

                    MIR_STEP_LOAD(r_tmp3, opno, resnull);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                                     MIR_new_int_op(ctx, 0)));

                    vs_handled = true;
                  }
                }

                /* Compiled LIKE path for anchored patterns */
                bool skip_vectorscan = false;
                if (!vs_handled && is_like_fn) {
                  SzLikeCompiled *compiled =
                      simd_like_compile(pat_str, pat_len);
                  if (compiled == SIMD_LIKE_USE_V1) {
                    skip_vectorscan = true;
                  } else if (compiled) {
                    char szcd_name[32], szce_name[32];
                    snprintf(szcd_name, sizeof(szcd_name), "szcd_%d", opno);
                    snprintf(szce_name, sizeof(szce_name), "szce_%d", opno);
                    MIR_reg_t r_datum = mir_new_reg(ctx, f, MIR_T_I64, szcd_name);
                    MIR_reg_t r_comp = mir_new_reg(ctx, f, MIR_T_I64, szce_name);

                    MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
                    int64_t off0 =
                        (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_datum),
                                     MIR_new_mem_op(ctx, MIR_T_I64, off0,
                                                    r_fci, 0, 1)));

                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_comp),
                                     MIR_new_int_op(ctx, (int64_t)(uintptr_t)compiled)));

                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_call_insn(ctx, 5,
                                          MIR_new_ref_op(ctx, proto_szcomp),
                                          MIR_new_ref_op(ctx, import_szcomp),
                                          MIR_new_reg_op(ctx, r_ret),
                                          MIR_new_reg_op(ctx, r_datum),
                                          MIR_new_reg_op(ctx, r_comp)));

                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32,
                                     MIR_new_reg_op(ctx, r_ret),
                                     MIR_new_reg_op(ctx, r_ret)));

                    if (vs_negate)
                      MIR_append_insn(
                          ctx, func_item,
                          MIR_new_insn(ctx, MIR_XOR,
                                       MIR_new_reg_op(ctx, r_ret),
                                       MIR_new_reg_op(ctx, r_ret),
                                       MIR_new_int_op(ctx, 1)));

                    MIR_STEP_LOAD(r_tmp3, opno, resvalue);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                                     MIR_new_reg_op(ctx, r_ret)));

                    MIR_STEP_LOAD(r_tmp3, opno, resnull);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                                     MIR_new_int_op(ctx, 0)));

                    vs_handled = true;
                  }
                }

#ifdef PG_JITTER_HAVE_VECTORSCAN
                /* Vectorscan path for complex LIKE or regex patterns */
                if (!vs_handled && !skip_vectorscan) {
                  bool is_utf8 = (GetDatabaseEncoding() == PG_UTF8);
                  VsCacheEntry *entry = pg_jitter_vs_compile(
                      pat_str, pat_len,
                      is_like_fn || is_ilike_fn, /* is_like */
                      is_ilike_fn || is_iregex_fn, /* case_insensitive */
                      is_utf8);

                  if (entry) {
                    char vsd_name[32], vse_name[32];
                    snprintf(vsd_name, sizeof(vsd_name), "vsd_%d", opno);
                    snprintf(vse_name, sizeof(vse_name), "vse_%d", opno);
                    MIR_reg_t r_datum = mir_new_reg(ctx, f, MIR_T_I64, vsd_name);
                    MIR_reg_t r_entry = mir_new_reg(ctx, f, MIR_T_I64, vse_name);

                    MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
                    int64_t off0 =
                        (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_datum),
                                     MIR_new_mem_op(ctx, MIR_T_I64, off0,
                                                    r_fci, 0, 1)));

                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_reg_op(ctx, r_entry),
                                     MIR_new_int_op(ctx, (int64_t)(uintptr_t)entry)));

                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_call_insn(ctx, 5,
                                          MIR_new_ref_op(ctx, proto_vs_match),
                                          MIR_new_ref_op(ctx, import_vs_match),
                                          MIR_new_reg_op(ctx, r_ret),
                                          MIR_new_reg_op(ctx, r_datum),
                                          MIR_new_reg_op(ctx, r_entry)));

                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32,
                                     MIR_new_reg_op(ctx, r_ret),
                                     MIR_new_reg_op(ctx, r_ret)));

                    if (vs_negate)
                      MIR_append_insn(
                          ctx, func_item,
                          MIR_new_insn(ctx, MIR_XOR,
                                       MIR_new_reg_op(ctx, r_ret),
                                       MIR_new_reg_op(ctx, r_ret),
                                       MIR_new_int_op(ctx, 1)));

                    MIR_STEP_LOAD(r_tmp3, opno, resvalue);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                                     MIR_new_reg_op(ctx, r_ret)));

                    MIR_STEP_LOAD(r_tmp3, opno, resnull);
                    MIR_append_insn(
                        ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV,
                                     MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                                     MIR_new_int_op(ctx, 0)));

                    vs_handled = true;
                  }
                }
#endif /* PG_JITTER_HAVE_VECTORSCAN */
              }
            }
          }

          if (!vs_handled) {
          /* Fallback: generic fcinfo path */

          /* fcinfo->isnull = false (caller must clear before V1 call) */
          MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV,
                  MIR_new_mem_op(ctx, MIR_T_U8,
                                 offsetof(FunctionCallInfoBaseData, isnull),
                                 r_fci, 0, 1),
                  MIR_new_int_op(ctx, 0)));

          /* Call fn_addr(fcinfo) → result */
          if (mir_shared_code_mode) {
            /*
             * In shared mode, fn_addr may point into a dynamically-loaded
             * library (e.g. plpgsql.dylib) whose load address differs
             * between leader and worker.  Load from step data at runtime.
             */
            MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
            int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                          offsetof(ExprEvalStep, d.func.fn_addr);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                             MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0,
                                            1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                  MIR_new_reg_op(ctx, r_fn),
                                  MIR_new_reg_op(ctx, r_ret),
                                  MIR_new_reg_op(ctx, r_fci)));
          } else {
            MIR_append_insn(
                ctx, func_item,
                MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                  MIR_new_ref_op(ctx, step_fn_imports[opno]),
                                  MIR_new_reg_op(ctx, r_ret),
                                  MIR_new_reg_op(ctx, r_fci)));
          }

          /* *op->resvalue = result */
          MIR_STEP_LOAD(r_tmp3, opno, resvalue);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                           MIR_new_reg_op(ctx, r_ret)));

          /* *op->resnull = fcinfo->isnull */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                  MIR_new_mem_op(ctx, MIR_T_U8,
                                 offsetof(FunctionCallInfoBaseData, isnull),
                                 r_fci, 0, 1)));
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                           MIR_new_reg_op(ctx, r_tmp1)));
          } /* end if (!vs_handled) */
        } /* end V1 fallback / vectorscan */
      } /* end direct-call dispatch */

      MIR_append_insn(ctx, func_item, done_label);
      break;
    }

    /*
     * ---- BOOL_AND_STEP ----
     */
    case EEOP_BOOL_AND_STEP_FIRST:
    case EEOP_BOOL_AND_STEP:
    case EEOP_BOOL_AND_STEP_LAST: {
      MIR_insn_t null_handler = MIR_new_label(ctx);
      MIR_insn_t cont = MIR_new_label(ctx);

      if (opcode == EEOP_BOOL_AND_STEP_FIRST) {
        /* *anynull = false */
        MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 0)));
      }

      /* Load resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      /* If null, go to null_handler */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, null_handler),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Not null: check if false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

      /* If false (value == 0), short-circuit */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_BEQ,
              MIR_new_label_op(ctx, step_labels[op->d.boolexpr.jumpdone]),
              MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Jump over null handler */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, cont)));

      /* Null handler: set *anynull = true */
      MIR_append_insn(ctx, func_item, null_handler);
      MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));

      MIR_append_insn(ctx, func_item, cont);

      /* On last step: if anynull, set result to NULL */
      if (opcode == EEOP_BOOL_AND_STEP_LAST) {
        MIR_insn_t no_anynull = MIR_new_label(ctx);

        MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, no_anynull),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

        /* Set resnull = true, resvalue = 0 */
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 1)));
        MIR_STEP_LOAD(r_tmp3, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
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
    case EEOP_BOOL_OR_STEP_LAST: {
      MIR_insn_t null_handler = MIR_new_label(ctx);
      MIR_insn_t cont = MIR_new_label(ctx);

      if (opcode == EEOP_BOOL_OR_STEP_FIRST) {
        MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 0)));
      }

      /* Load resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, null_handler),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Not null: check if true */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));

      /* If true (value != 0), short-circuit */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_BNE,
              MIR_new_label_op(ctx, step_labels[op->d.boolexpr.jumpdone]),
              MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, cont)));

      /* Null handler */
      MIR_append_insn(ctx, func_item, null_handler);
      MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));

      MIR_append_insn(ctx, func_item, cont);

      if (opcode == EEOP_BOOL_OR_STEP_LAST) {
        MIR_insn_t no_anynull = MIR_new_label(ctx);

        MIR_STEP_LOAD(r_tmp3, opno, d.boolexpr.anynull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, no_anynull),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 1)));
        MIR_STEP_LOAD(r_tmp3, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
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
    case EEOP_BOOL_NOT_STEP: {
      /* Load resvalue, negate, store back */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
      /* r_tmp1 = (r_tmp1 == 0) ? 1 : 0 */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp1)));
      break;
    }

    /*
     * ---- QUAL ----
     */
    case EEOP_QUAL: {
      MIR_insn_t qualfail = MIR_new_label(ctx);
      MIR_insn_t cont = MIR_new_label(ctx);

      /* Check null */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, qualfail),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Check false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, qualfail),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Pass: continue */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, cont)));

      /* Qual fail: set resvalue=0, resnull=false, jump to done */
      MIR_append_insn(ctx, func_item, qualfail);
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_JMP,
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
                                   MIR_new_label_op(
                                       ctx, step_labels[op->d.jump.jumpdone])));
      break;

    case EEOP_JUMP_IF_NULL:
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE,
                       MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      break;

    case EEOP_JUMP_IF_NOT_NULL:
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ,
                       MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      break;

    case EEOP_JUMP_IF_NOT_TRUE: {
      /* Jump if null OR false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      /* If null, jump */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE,
                       MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      /* If false, jump */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ,
                       MIR_new_label_op(ctx, step_labels[op->d.jump.jumpdone]),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- NULLTEST ----
     */
    case EEOP_NULLTEST_ISNULL: {
      /* resvalue = resnull ? 1 : 0; resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      /* Store as Datum */
      MIR_STEP_LOAD(r_tmp2, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp1)));
      /* resnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

    case EEOP_NULLTEST_ISNOTNULL: {
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      /* r_tmp1 = !r_tmp1 */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_int_op(ctx, 0)));
      /* Store as Datum */
      MIR_STEP_LOAD(r_tmp2, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp1)));
      /* resnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

#ifdef HAVE_EEOP_HASHDATUM
    /*
     * ---- HASHDATUM_SET_INITVAL ----
     */
    case EEOP_HASHDATUM_SET_INITVAL: {
      /* *op->resvalue = op->d.hashdatum_initvalue.init_value */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV, MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
              MIR_new_int_op(ctx,
                             (int64_t)op->d.hashdatum_initvalue.init_value)));

      /* *op->resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- HASHDATUM_FIRST (non-strict) ----
     */
    case EEOP_HASHDATUM_FIRST: {
      FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
      int64_t isnull_off =
          (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
      MIR_insn_t store_zero = MIR_new_label(ctx);
      MIR_insn_t store_result = MIR_new_label(ctx);
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");

      /* r_fci = fcinfo */
      MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

      /* r_tmp1 = fcinfo->args[0].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci, 0, 1)));

      /* if isnull, jump to store_zero */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, store_zero),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Call hash function (direct or fcinfo) */
      if (step_direct_imports[opno]) {
        /* Direct: load Datum arg from fcinfo->args[0].value.
         * Always use I64 register — MIR_MOV only supports I64.
         * Proto handles I64→I32 narrowing at call boundary. */
        int64_t val_off =
            (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
        MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_harg),
                         MIR_new_mem_op(ctx, MIR_T_I64, val_off, r_fci, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(
                ctx, 4,
                MIR_new_ref_op(ctx, proto_direct[0][step_direct_ret_types[opno]]
                                                [step_direct_arg0_types[opno]]),
                MIR_new_ref_op(ctx, step_direct_imports[opno]),
                MIR_new_reg_op(ctx, r_ret), MIR_new_reg_op(ctx, r_harg)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(FunctionCallInfoBaseData, isnull),
                               r_fci, 0, 1),
                MIR_new_int_op(ctx, 0)));
        if (mir_shared_code_mode) {
          MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
          int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                        offsetof(ExprEvalStep, d.hashdatum.fn_addr);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                           MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0,
                                          1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_reg_op(ctx, r_fn),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        } else {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_ref_op(ctx, step_fn_imports[opno]),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        }
      }

      /* r_tmp1 = r_ret */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_ret)));

      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, store_result)));

      /* store_zero: r_tmp1 = 0 */
      MIR_append_insn(ctx, func_item, store_zero);
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_int_op(ctx, 0)));

      /* store_result: *op->resvalue = r_tmp1, *op->resnull = false */
      MIR_append_insn(ctx, func_item, store_result);
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp1)));

      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- HASHDATUM_FIRST_STRICT ----
     */
    case EEOP_HASHDATUM_FIRST_STRICT: {
      FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
      int64_t isnull_off =
          (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
      MIR_insn_t null_path = MIR_new_label(ctx);
      MIR_insn_t after_null = MIR_new_label(ctx);
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");

      /* r_fci = fcinfo */
      MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

      /* r_tmp1 = fcinfo->args[0].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci, 0, 1)));

      /* if isnull, jump to null_path */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, null_path),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      if (step_direct_imports[opno]) {
        int64_t val_off =
            (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
        MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_harg),
                         MIR_new_mem_op(ctx, MIR_T_I64, val_off, r_fci, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(
                ctx, 4,
                MIR_new_ref_op(ctx, proto_direct[0][step_direct_ret_types[opno]]
                                                [step_direct_arg0_types[opno]]),
                MIR_new_ref_op(ctx, step_direct_imports[opno]),
                MIR_new_reg_op(ctx, r_ret), MIR_new_reg_op(ctx, r_harg)));
      } else {
        /* fcinfo->isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(FunctionCallInfoBaseData, isnull),
                               r_fci, 0, 1),
                MIR_new_int_op(ctx, 0)));

        /* r_ret = call fn_addr(fcinfo) */
        if (mir_shared_code_mode) {
          MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
          int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                        offsetof(ExprEvalStep, d.hashdatum.fn_addr);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                           MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0,
                                          1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_reg_op(ctx, r_fn),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        } else {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_ref_op(ctx, step_fn_imports[opno]),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        }
      }

      /* *op->resvalue = r_ret */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_ret)));

      /* *op->resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));

      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, after_null)));

      /* null_path: set resnull=1, resvalue=0, jump to jumpdone */
      MIR_append_insn(ctx, func_item, null_path);
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_JMP,
              MIR_new_label_op(ctx, step_labels[op->d.hashdatum.jumpdone])));

      MIR_append_insn(ctx, func_item, after_null);
      break;
    }

    /*
     * ---- HASHDATUM_NEXT32 (non-strict, rotate + XOR) ----
     */
    case EEOP_HASHDATUM_NEXT32: {
      FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
      NullableDatum *iresult = op->d.hashdatum.iresult;
      int64_t isnull_off =
          (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
      MIR_insn_t skip_hash = MIR_new_label(ctx);
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");
      MIR_reg_t r_hash = mir_new_reg(ctx, f, MIR_T_I64, "hash");

      /* Load existing hash: r_hash = iresult->value (as U32) */
      MIR_STEP_LOAD(r_tmp1, opno, d.hashdatum.iresult);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_hash),
                       MIR_new_mem_op(ctx, MIR_T_U32,
                                      offsetof(NullableDatum, value), r_tmp1, 0,
                                      1)));

      /* Rotate left 1: r_hash = (r_hash << 1) | (r_hash >> 31) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_LSHS, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_int_op(ctx, 1)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_URSHS, MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_int_op(ctx, 31)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_ORS, MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp2)));

      /* r_fci = fcinfo */
      MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

      /* r_tmp1 = fcinfo->args[0].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci, 0, 1)));

      /* if isnull, skip hash call */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, skip_hash),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      if (step_direct_imports[opno]) {
        int64_t val_off =
            (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
        MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_harg),
                         MIR_new_mem_op(ctx, MIR_T_I64, val_off, r_fci, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(
                ctx, 4,
                MIR_new_ref_op(ctx, proto_direct[0][step_direct_ret_types[opno]]
                                                [step_direct_arg0_types[opno]]),
                MIR_new_ref_op(ctx, step_direct_imports[opno]),
                MIR_new_reg_op(ctx, r_ret), MIR_new_reg_op(ctx, r_harg)));
      } else {
        /* fcinfo->isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(FunctionCallInfoBaseData, isnull),
                               r_fci, 0, 1),
                MIR_new_int_op(ctx, 0)));

        /* r_ret = call fn_addr(fcinfo) */
        if (mir_shared_code_mode) {
          MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
          int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                        offsetof(ExprEvalStep, d.hashdatum.fn_addr);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                           MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0,
                                          1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_reg_op(ctx, r_fn),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        } else {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_ref_op(ctx, step_fn_imports[opno]),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        }
      }

      /* r_hash = r_hash XOR r_ret (32-bit) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_XORS, MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_reg_op(ctx, r_ret)));

      /* skip_hash: store result */
      MIR_append_insn(ctx, func_item, skip_hash);

      /* *op->resvalue = r_hash (already zero-extended from 32-bit ops) */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_hash)));

      /* *op->resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- HASHDATUM_NEXT32_STRICT ----
     */
    case EEOP_HASHDATUM_NEXT32_STRICT: {
      FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
      NullableDatum *iresult = op->d.hashdatum.iresult;
      int64_t isnull_off =
          (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
      MIR_insn_t null_path = MIR_new_label(ctx);
      MIR_insn_t after_null = MIR_new_label(ctx);
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "hret");
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "hfci");
      MIR_reg_t r_hash = mir_new_reg(ctx, f, MIR_T_I64, "hash");

      /* r_fci = fcinfo */
      MIR_STEP_LOAD(r_fci, opno, d.hashdatum.fcinfo_data);

      /* r_tmp1 = fcinfo->args[0].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci, 0, 1)));

      /* if isnull, jump to null_path */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, null_path),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Load existing hash: r_hash = iresult->value (as U32) */
      MIR_STEP_LOAD(r_tmp1, opno, d.hashdatum.iresult);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_hash),
                       MIR_new_mem_op(ctx, MIR_T_U32,
                                      offsetof(NullableDatum, value), r_tmp1, 0,
                                      1)));

      /* Rotate left 1: r_hash = (r_hash << 1) | (r_hash >> 31) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_LSHS, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_int_op(ctx, 1)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_URSHS, MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_int_op(ctx, 31)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_ORS, MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp2)));

      if (step_direct_imports[opno]) {
        int64_t val_off =
            (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
        MIR_reg_t r_harg = mir_new_reg(ctx, f, MIR_T_I64, "harg");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_harg),
                         MIR_new_mem_op(ctx, MIR_T_I64, val_off, r_fci, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(
                ctx, 4,
                MIR_new_ref_op(ctx, proto_direct[0][step_direct_ret_types[opno]]
                                                [step_direct_arg0_types[opno]]),
                MIR_new_ref_op(ctx, step_direct_imports[opno]),
                MIR_new_reg_op(ctx, r_ret), MIR_new_reg_op(ctx, r_harg)));
      } else {
        /* fcinfo->isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(FunctionCallInfoBaseData, isnull),
                               r_fci, 0, 1),
                MIR_new_int_op(ctx, 0)));

        /* r_ret = call fn_addr(fcinfo) */
        if (mir_shared_code_mode) {
          MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
          int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                        offsetof(ExprEvalStep, d.hashdatum.fn_addr);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                           MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0,
                                          1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_reg_op(ctx, r_fn),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        } else {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_ref_op(ctx, step_fn_imports[opno]),
                                MIR_new_reg_op(ctx, r_ret),
                                MIR_new_reg_op(ctx, r_fci)));
        }
      }

      /* r_hash = r_hash XOR r_ret (32-bit) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_XORS, MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_reg_op(ctx, r_hash),
                                   MIR_new_reg_op(ctx, r_ret)));

      /* *op->resvalue = r_hash */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_hash)));

      /* *op->resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));

      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, after_null)));

      /* null_path: set resnull=1, resvalue=0, jump to jumpdone */
      MIR_append_insn(ctx, func_item, null_path);
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_JMP,
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
    case EEOP_AGG_PLAIN_TRANS_BYREF: {
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

      MIR_insn_t end_label = MIR_new_label(ctx);
      MIR_reg_t r_pergroup = mir_new_reg(ctx, f, MIR_T_I64, "pg");
      MIR_reg_t r_aggst = mir_new_reg(ctx, f, MIR_T_I64, "agst");

      /* Compute pergroup at runtime: aggstate->all_pergroups[setoff] + transno
       */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_aggst),
                       MIR_new_mem_op(ctx, MIR_T_P, offsetof(ExprState, parent),
                                      r_state, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_pergroup),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(AggState, all_pergroups),
                                      r_aggst, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV, MIR_new_reg_op(ctx, r_pergroup),
              MIR_new_mem_op(ctx, MIR_T_P,
                             setoff * (int64_t)sizeof(AggStatePerGroup),
                             r_pergroup, 0, 1)));
      if (transno != 0)
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_ADD, MIR_new_reg_op(ctx, r_pergroup),
                MIR_new_reg_op(ctx, r_pergroup),
                MIR_new_int_op(
                    ctx, transno * (int64_t)sizeof(AggStatePerGroupData))));

      /* INIT check */
      if (is_init) {
        MIR_insn_t no_init = MIR_new_label(ctx);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, noTransValue),
                               r_pergroup, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, no_init),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
        /* noTransValue=true: call helper(state, op) which handles
         * ExecAggInitGroup internally. */
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                              MIR_new_ref_op(ctx, step_direct_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));
        MIR_append_insn(ctx, func_item, no_init);
      }

      /* STRICT check: skip if transValueIsNull */
      if (is_strict) {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, transValueIsNull),
                               r_pergroup, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, end_label),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      }

      /*
       * Transition function dispatch — inline hot aggs.
       */
      if (!is_byref && (fn_addr == int8inc || fn_addr == int8inc_any)) {
        /* COUNT: transValue += 1 (int64, overflow-checked) */
        MIR_insn_t ok = MIR_new_label(ctx);
        MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tv),
                MIR_new_mem_op(ctx, MIR_T_I64,
                               offsetof(AggStatePerGroupData, transValue),
                               r_pergroup, 0, 1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_ADDO, MIR_new_reg_op(ctx, r_tv),
                                     MIR_new_reg_op(ctx, r_tv),
                                     MIR_new_int_op(ctx, 1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_BNO, MIR_new_label_op(ctx, ok)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 2, MIR_new_ref_op(ctx, proto_err_void),
                              MIR_new_ref_op(ctx, import_err_int8_overflow)));
        MIR_append_insn(ctx, func_item, ok);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                               offsetof(AggStatePerGroupData, transValue),
                               r_pergroup, 0, 1),
                MIR_new_reg_op(ctx, r_tv)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, transValueIsNull),
                               r_pergroup, 0, 1),
                MIR_new_int_op(ctx, 0)));
      } else if (!is_byref && fn_addr == int4_sum) {
        /* SUM(int4): transValue += (int64)arg1 */
        MIR_insn_t arg_not_null = MIR_new_label(ctx);
        MIR_insn_t trans_not_null = MIR_new_label(ctx);
        MIR_insn_t after_sum = MIR_new_label(ctx);
        MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "sfci");
        MIR_reg_t r_arg1 = mir_new_reg(ctx, f, MIR_T_I64, "arg1");
        int64_t isnull1_off =
            (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
        int64_t val1_off =
            (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

        /* Load fcinfo = pertrans->transfn_fcinfo (runtime) */
        if (mir_shared_code_mode) {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fci),
                  MIR_new_mem_op(
                      ctx, MIR_T_P,
                      (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                          (int64_t)offsetof(ExprEvalStep, d.agg_trans.pertrans),
                      r_steps, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fci),
                  MIR_new_mem_op(ctx, MIR_T_P,
                                 offsetof(AggStatePerTransData, transfn_fcinfo),
                                 r_fci, 0, 1)));
        } else
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fci),
                                       MIR_new_uint_op(ctx, (uint64_t)fcinfo)));
        /* Check arg1 isnull */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                     MIR_new_mem_op(ctx, MIR_T_U8, isnull1_off,
                                                    r_fci, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, arg_not_null),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));

        MIR_append_insn(ctx, func_item, arg_not_null);
        /* Load arg1, sign-extend int32→int64 */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_arg1),
                                     MIR_new_mem_op(ctx, MIR_T_I64, val1_off,
                                                    r_fci, 0, 1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32,
                                     MIR_new_reg_op(ctx, r_arg1),
                                     MIR_new_reg_op(ctx, r_arg1)));

        /* Check transValueIsNull */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, transValueIsNull),
                               r_pergroup, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, trans_not_null),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

        /* First non-null: transValue = (int64)arg1 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                               offsetof(AggStatePerGroupData, transValue),
                               r_pergroup, 0, 1),
                MIR_new_reg_op(ctx, r_arg1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, transValueIsNull),
                               r_pergroup, 0, 1),
                MIR_new_int_op(ctx, 0)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, after_sum)));

        /* Normal: transValue += (int64)arg1 */
        MIR_append_insn(ctx, func_item, trans_not_null);
        {
          MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tv),
                  MIR_new_mem_op(ctx, MIR_T_I64,
                                 offsetof(AggStatePerGroupData, transValue),
                                 r_pergroup, 0, 1)));
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_tv),
                                       MIR_new_reg_op(ctx, r_tv),
                                       MIR_new_reg_op(ctx, r_arg1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV,
                  MIR_new_mem_op(ctx, MIR_T_I64,
                                 offsetof(AggStatePerGroupData, transValue),
                                 r_pergroup, 0, 1),
                  MIR_new_reg_op(ctx, r_tv)));
        }
        MIR_append_insn(ctx, func_item, after_sum);
      } else if (!is_byref &&
                 (fn_addr == int4smaller || fn_addr == int4larger)) {
        /* MIN/MAX(int4) */
        bool is_min = (fn_addr == int4smaller);
        MIR_insn_t skip_update = MIR_new_label(ctx);
        MIR_reg_t r_tv = mir_new_reg(ctx, f, MIR_T_I64, "tv");
        MIR_reg_t r_nv = mir_new_reg(ctx, f, MIR_T_I64, "nv");
        int64_t val1_off =
            (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tv),
                MIR_new_mem_op(ctx, MIR_T_I64,
                               offsetof(AggStatePerGroupData, transValue),
                               r_pergroup, 0, 1)));
        /* Load fcinfo = pertrans->transfn_fcinfo (runtime) */
        if (mir_shared_code_mode) {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                  MIR_new_mem_op(
                      ctx, MIR_T_P,
                      (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                          (int64_t)offsetof(ExprEvalStep, d.agg_trans.pertrans),
                      r_steps, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                  MIR_new_mem_op(ctx, MIR_T_P,
                                 offsetof(AggStatePerTransData, transfn_fcinfo),
                                 r_tmp1, 0, 1)));
        } else
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_tmp1),
                                       MIR_new_uint_op(ctx, (uint64_t)fcinfo)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_nv),
                                     MIR_new_mem_op(ctx, MIR_T_I64, val1_off,
                                                    r_tmp1, 0, 1)));
        /* Sign-extend both */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32, MIR_new_reg_op(ctx, r_tv),
                                     MIR_new_reg_op(ctx, r_tv)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32, MIR_new_reg_op(ctx, r_nv),
                                     MIR_new_reg_op(ctx, r_nv)));
        /* MIN: skip if tv <= nv; MAX: skip if tv >= nv */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, is_min ? MIR_BLE : MIR_BGE,
                                     MIR_new_label_op(ctx, skip_update),
                                     MIR_new_reg_op(ctx, r_tv),
                                     MIR_new_reg_op(ctx, r_nv)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                               offsetof(AggStatePerGroupData, transValue),
                               r_pergroup, 0, 1),
                MIR_new_reg_op(ctx, r_nv)));
        MIR_append_insn(ctx, func_item, skip_update);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, transValueIsNull),
                               r_pergroup, 0, 1),
                MIR_new_int_op(ctx, 0)));
      } else if (is_byref &&
                 (fn_addr == int4_avg_accum || fn_addr == int2_avg_accum)) {
/* AVG(int4/int2): in-place update of {count, sum} */
#define MIR_INT8_TRANS_OFFSET 24
        MIR_reg_t r_td = mir_new_reg(ctx, f, MIR_T_I64, "td");
        MIR_reg_t r_cnt = mir_new_reg(ctx, f, MIR_T_I64, "cnt");
        MIR_reg_t r_sum = mir_new_reg(ctx, f, MIR_T_I64, "sum");
        MIR_reg_t r_a1 = mir_new_reg(ctx, f, MIR_T_I64, "a1v");
        int64_t val1_off =
            (int64_t)((char *)&fcinfo->args[1].value - (char *)fcinfo);

        /* r_td = transValue + 24 = pointer to Int8TransTypeData */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_td),
                MIR_new_mem_op(ctx, MIR_T_I64,
                               offsetof(AggStatePerGroupData, transValue),
                               r_pergroup, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_td),
                         MIR_new_reg_op(ctx, r_td),
                         MIR_new_int_op(ctx, MIR_INT8_TRANS_OFFSET)));

        /* count++ */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_cnt),
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_td, 0, 1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_cnt),
                                     MIR_new_reg_op(ctx, r_cnt),
                                     MIR_new_int_op(ctx, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_td, 0, 1),
                         MIR_new_reg_op(ctx, r_cnt)));

        /* Load arg1, sign-extend */
        /* Load fcinfo = pertrans->transfn_fcinfo (runtime) */
        if (mir_shared_code_mode) {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                  MIR_new_mem_op(
                      ctx, MIR_T_P,
                      (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                          (int64_t)offsetof(ExprEvalStep, d.agg_trans.pertrans),
                      r_steps, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                  MIR_new_mem_op(ctx, MIR_T_P,
                                 offsetof(AggStatePerTransData, transfn_fcinfo),
                                 r_tmp1, 0, 1)));
        } else
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_MOV,
                                       MIR_new_reg_op(ctx, r_tmp1),
                                       MIR_new_uint_op(ctx, (uint64_t)fcinfo)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_a1),
                                     MIR_new_mem_op(ctx, MIR_T_I64, val1_off,
                                                    r_tmp1, 0, 1)));
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
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_sum),
                MIR_new_mem_op(ctx, MIR_T_I64,
                               (int64_t)offsetof(MirInt8TransTypeData, sum),
                               r_td, 0, 1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_sum),
                                     MIR_new_reg_op(ctx, r_sum),
                                     MIR_new_reg_op(ctx, r_a1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64,
                               (int64_t)offsetof(MirInt8TransTypeData, sum),
                               r_td, 0, 1),
                MIR_new_reg_op(ctx, r_sum)));

        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8,
                               offsetof(AggStatePerGroupData, transValueIsNull),
                               r_pergroup, 0, 1),
                MIR_new_int_op(ctx, 0)));
#undef MIR_INT8_TRANS_OFFSET
      } else {
        /* Generic fallback: call helper function */
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
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
    case EEOP_HASHED_SCALARARRAYOP: {
#if PG_VERSION_NUM >= 150000
      FunctionCallInfo fcinfo = op->d.hashedscalararrayop.fcinfo_data;
      bool inclause = op->d.hashedscalararrayop.inclause;
      ScalarArrayOpExpr *saop = op->d.hashedscalararrayop.saop;

      /*
       * Detect byval type at compile time.
       */
      const JitDirectFn *eq_dfn =
          jit_find_direct_fn(op->d.hashedscalararrayop.finfo->fn_addr);

      /*
       * Try to extract constant array values at compile time
       * for inline binary search.
       */
      Datum *sorted_vals = NULL;
      int nvals = 0;
      bool array_has_nulls = false;

      if (eq_dfn && eq_dfn->jit_fn) {
        /*
         * Check if the array argument is a Const node.
         * saop->args = list of (scalar, array).
         */
        Expr *arrayarg = (Expr *)lsecond(saop->args);

        if (IsA(arrayarg, Const)) {
          Const *arrayconst = (Const *)arrayarg;

          if (!arrayconst->constisnull) {
            ArrayType *arr = DatumGetArrayTypeP(arrayconst->constvalue);
            int16 typlen;
            bool typbyval;
            char typalign;
            int nitems;

            nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
            get_typlenbyvalalign(ARR_ELEMTYPE(arr), &typlen, &typbyval,
                                 &typalign);

            if (typbyval && nitems > 0 && nitems <= 64) {
              /*
               * Extract all values. Check for NULLs.
               */
              bits8 *bitmap = ARR_NULLBITMAP(arr);
              char *s = (char *)ARR_DATA_PTR(arr);
              int bitmask = 1;

              sorted_vals = (Datum *)palloc(nitems * sizeof(Datum));
              nvals = 0;

              for (int k = 0; k < nitems; k++) {
                if (bitmap && (*bitmap & bitmask) == 0) {
                  array_has_nulls = true;
                } else {
                  Datum d = fetch_att(s, true, typlen);
                  sorted_vals[nvals++] = d;
                  s = att_addlength_pointer(s, typlen, s);
                  s = (char *)att_align_nominal(s, typalign);
                }

                if (bitmap) {
                  bitmask <<= 1;
                  if (bitmask == 0x100) {
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
              for (int a = 1; a < nvals; a++) {
                Datum tmp = sorted_vals[a];
                int b = a - 1;
                while (b >= 0 && (int64)sorted_vals[b] > (int64)tmp) {
                  sorted_vals[b + 1] = sorted_vals[b];
                  b--;
                }
                sorted_vals[b + 1] = tmp;
              }
            }
          }
        }
      }

      if (sorted_vals && nvals > 0) {
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
            (int64_t)((char *)&fcinfo->args[0].value - (char *)fcinfo);
        int64_t off_arg0_isnull =
            (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);

        MIR_reg_t r_scalar = mir_new_reg(ctx, f, MIR_T_I64, "bscalar");

        /*
         * Step 1: Check scalar not NULL (strict function).
         */
        MIR_STEP_LOAD(r_tmp1, opno, d.hashedscalararrayop.fcinfo_data);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                MIR_new_mem_op(ctx, MIR_T_U8, off_arg0_isnull, r_tmp1, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, lbl_null_result),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

        /*
         * Step 2: Load scalar value into r_scalar.
         * (r_tmp1 still = fcinfo)
         */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_scalar),
                MIR_new_mem_op(ctx, MIR_T_I64, off_arg0_value, r_tmp1, 0, 1)));

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

          while (work_top > 0) {
            int lo, hi, mid;
            MIR_insn_t lbl_entry;
            MIR_insn_t lbl_left;

            work_top--;
            lo = work[work_top].lo;
            hi = work[work_top].hi;
            lbl_entry = work[work_top].entry_label;

            /* Place entry label for this node */
            if (lbl_entry)
              MIR_append_insn(ctx, func_item, lbl_entry);

            if (lo > hi) {
              /* Empty range -> not found */
              MIR_append_insn(
                  ctx, func_item,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, lbl_not_found)));
              continue;
            }

            if (lo == hi) {
              /* Single element: BEQ + JMP */
              MIR_append_insn(
                  ctx, func_item,
                  MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, lbl_found),
                               MIR_new_reg_op(ctx, r_scalar),
                               MIR_new_int_op(ctx, (int64_t)sorted_vals[lo])));
              MIR_append_insn(
                  ctx, func_item,
                  MIR_new_insn(ctx, MIR_JMP,
                               MIR_new_label_op(ctx, lbl_not_found)));
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

            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, lbl_found),
                             MIR_new_reg_op(ctx, r_scalar),
                             MIR_new_int_op(ctx, (int64_t)sorted_vals[mid])));

            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_BLTS, MIR_new_label_op(ctx, lbl_left),
                             MIR_new_reg_op(ctx, r_scalar),
                             MIR_new_int_op(ctx, (int64_t)sorted_vals[mid])));

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
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, inclause ? 1 : 0)));
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 0)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_done)));

        /* ---- Not found ---- */
        MIR_append_insn(ctx, func_item, lbl_not_found);
        if (array_has_nulls) {
          /*
           * Array had NULLs -- result is NULL
           * (indeterminate for strict equality).
           */
          MIR_STEP_LOAD(r_tmp3, opno, resvalue);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, 0)));
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, 1)));
        } else {
          /* Definitive not-found result */
          MIR_STEP_LOAD(r_tmp3, opno, resvalue);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, inclause ? 0 : 1)));
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, 0)));
        }
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, lbl_done)));

        /* ---- Null scalar ---- */
        MIR_append_insn(ctx, func_item, lbl_null_result);
        MIR_STEP_LOAD(r_tmp3, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 0)));
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 1)));

        /* ---- Done ---- */
        MIR_append_insn(ctx, func_item, lbl_done);

        pfree(sorted_vals);
      } else {
        /*
         * Non-constant array or byref type -- fall back
         * to C function call.
         */
        if (sorted_vals)
          pfree(sorted_vals);

        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1),
                              MIR_new_reg_op(ctx, r_econtext)));
      }
#else  /* PG14: no inclause/saop -- always use fallback */
      {
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
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
    case EEOP_AGG_PRESORTED_DISTINCT_MULTI: {
      int jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;
      MIR_item_t fn_import = (opcode == EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
                                 ? import_presorted_single
                                 : import_presorted_multi;

      MIR_reg_t r_aggst = mir_new_reg(ctx, f, MIR_T_I64, "pdst");

      /* aggstate = state->parent */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_aggst),
                       MIR_new_mem_op(ctx, MIR_T_P, offsetof(ExprState, parent),
                                      r_state, 0, 1)));

      /* pertrans = op->d.agg_presorted_distinctcheck.pertrans */
      MIR_STEP_LOAD(r_tmp1, opno, d.agg_presorted_distinctcheck.pertrans);

      /* r_tmp2 = fn(aggstate, pertrans) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(
              ctx, 5, MIR_new_ref_op(ctx, proto_presorted_distinct),
              MIR_new_ref_op(ctx, fn_import), MIR_new_reg_op(ctx, r_tmp2),
              MIR_new_reg_op(ctx, r_aggst), MIR_new_reg_op(ctx, r_tmp1)));

      /*
       * Zero-extend bool return: the function returns bool (1 byte in AL).
       * On Windows x64 the ABI does not guarantee upper bytes of RAX are
       * cleared.  Mask to 8 bits before comparing.
       */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_int_op(ctx, 0xFF)));

      /* If result == 0 (not distinct), jump to jumpdistinct */
      if (jumpdistinct >= 0 && jumpdistinct < steps_len) {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ,
                         MIR_new_label_op(ctx, step_labels[jumpdistinct]),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));
      }
      break;
    }
#endif /* HAVE_EEOP_AGG_PRESORTED_DISTINCT */

    /*
     * ---- BOOLTEST_IS_TRUE ----
     * If null: resvalue=false, resnull=false. Else keep.
     */
    case EEOP_BOOLTEST_IS_TRUE: {
      MIR_insn_t not_null_label = MIR_new_label(ctx);

      /* r_tmp1 = *op->resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      /* if (!resnull) goto not_null */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_null_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* null path: *resvalue = 0 (false), *resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_BOOLTEST_IS_NOT_TRUE: {
      MIR_insn_t not_null_label = MIR_new_label(ctx);
      MIR_insn_t done_label = MIR_new_label(ctx);

      /* r_tmp1 = *op->resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      /* if (!resnull) goto not_null */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_null_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* null path: *resvalue = 1 (true), *resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done_label)));

      /* not_null: *resvalue = !*resvalue */
      MIR_append_insn(ctx, func_item, not_null_label);
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_BOOLTEST_IS_FALSE: {
      MIR_insn_t not_null_label = MIR_new_label(ctx);
      MIR_insn_t done_label = MIR_new_label(ctx);

      /* r_tmp1 = *op->resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      /* if (!resnull) goto not_null */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_null_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* null path: *resvalue = 0 (false), *resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done_label)));

      /* not_null: *resvalue = !*resvalue */
      MIR_append_insn(ctx, func_item, not_null_label);
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_BOOLTEST_IS_NOT_FALSE: {
      MIR_insn_t not_null_label = MIR_new_label(ctx);

      /* r_tmp1 = *op->resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));

      /* if (!resnull) goto not_null */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_null_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* null path: *resvalue = 1 (true), *resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_AGGREF: {
      int aggno = op->d.aggref.aggno;

      /* r_tmp1 = econtext->ecxt_aggvalues */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(ExprContext, ecxt_aggvalues),
                                      r_econtext, 0, 1)));
      /* r_tmp2 = aggvalues[aggno] */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64,
                                      aggno * (int64_t)sizeof(Datum), r_tmp1, 0,
                                      1)));
      /* *op->resvalue = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* r_tmp1 = econtext->ecxt_aggnulls */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(ExprContext, ecxt_aggnulls),
                                      r_econtext, 0, 1)));
      /* r_tmp2 = aggnulls[aggno] */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_mem_op(ctx, MIR_T_U8,
                                                  aggno * (int64_t)sizeof(bool),
                                                  r_tmp1, 0, 1)));
      /* *op->resnull = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_WINDOW_FUNC: {
      MIR_reg_t r_wfno = mir_new_reg(ctx, f, MIR_T_I64, "wfno");

      /* r_tmp1 = op->d.window_func.wfstate */
      MIR_STEP_LOAD(r_tmp1, opno, d.window_func.wfstate);
      /* r_wfno = wfstate->wfuncno (int, sign-extend) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_wfno),
                       MIR_new_mem_op(ctx, MIR_T_I32,
                                      offsetof(WindowFuncExprState, wfuncno),
                                      r_tmp1, 0, 1)));

      /* r_tmp1 = econtext->ecxt_aggvalues */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(ExprContext, ecxt_aggvalues),
                                      r_econtext, 0, 1)));
      /* r_tmp2 = wfuncno * sizeof(Datum) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_LSH, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_reg_op(ctx, r_wfno),
                       MIR_new_int_op(ctx, 3))); /* *8 for sizeof(Datum) */
      /* r_tmp1 = aggvalues + offset */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp2)));
      /* r_tmp2 = *r_tmp1 (Datum) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
      /* *op->resvalue = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* r_tmp1 = econtext->ecxt_aggnulls */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(ExprContext, ecxt_aggnulls),
                                      r_econtext, 0, 1)));
      /* r_tmp1 = aggnulls + wfuncno (bool is 1 byte, no shift) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_reg_op(ctx, r_wfno)));
      /* r_tmp2 = *r_tmp1 (bool) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      /* *op->resnull = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_CASE_TESTVAL: {
      /*
       * If d.casetest.value is non-NULL, dereference it.
       * Otherwise read from econtext->caseValue_datum/isNull.
       * Matches interpreter NULL check for JSON_TABLE etc.
       */
      if (op->d.casetest.value) {
        MIR_STEP_LOAD(r_tmp1, opno, d.casetest.value);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64,
                                        offsetof(ExprContext, caseValue_datum),
                                        r_econtext, 0, 1)));
      }
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      if (op->d.casetest.isnull) {
        MIR_STEP_LOAD(r_tmp1, opno, d.casetest.isnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        offsetof(ExprContext, caseValue_isNull),
                                        r_econtext, 0, 1)));
      }
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_CASE_TESTVAL_EXT: {
      /* r_tmp2 = econtext->caseValue_datum */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64,
                                      offsetof(ExprContext, caseValue_datum),
                                      r_econtext, 0, 1)));
      /* *op->resvalue = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* r_tmp2 = econtext->caseValue_isNull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8,
                                      offsetof(ExprContext, caseValue_isNull),
                                      r_econtext, 0, 1)));
      /* *op->resnull = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_DOMAIN_TESTVAL: {
      /*
       * If d.casetest.value is non-NULL, dereference it.
       * Otherwise read from econtext->domainValue_datum/isNull.
       */
      if (op->d.casetest.value) {
        MIR_STEP_LOAD(r_tmp1, opno, d.casetest.value);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64,
                                        offsetof(ExprContext, domainValue_datum),
                                        r_econtext, 0, 1)));
      }
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      if (op->d.casetest.isnull) {
        MIR_STEP_LOAD(r_tmp1, opno, d.casetest.isnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        offsetof(ExprContext, domainValue_isNull),
                                        r_econtext, 0, 1)));
      }
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_DOMAIN_TESTVAL_EXT: {
      /* r_tmp2 = econtext->domainValue_datum */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64,
                                      offsetof(ExprContext, domainValue_datum),
                                      r_econtext, 0, 1)));
      /* *op->resvalue = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* r_tmp2 = econtext->domainValue_isNull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8,
                                      offsetof(ExprContext, domainValue_isNull),
                                      r_econtext, 0, 1)));
      /* *op->resnull = r_tmp2 */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_MAKE_READONLY: {
      MIR_insn_t skip_label = MIR_new_label(ctx);
      MIR_reg_t r_val = mir_new_reg(ctx, f, MIR_T_I64, "mro_v");
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "mro_r");

      /* r_tmp1 = op->d.make_readonly.isnull (pointer) */
      MIR_STEP_LOAD(r_tmp1, opno, d.make_readonly.isnull);
      /* r_tmp2 = *isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));

      /* *op->resnull = isnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* if isnull, skip MakeReadOnly call */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, skip_label),
                       MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

      /* Not null: r_val = *op->d.make_readonly.value */
      MIR_STEP_LOAD(r_tmp1, opno, d.make_readonly.value);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_val),
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
      MIR_append_insn(
          ctx, func_item,
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
      int64_t isnull_off0 = (int64_t)offsetof(NullableDatum, isnull);
      int64_t nd_size = (int64_t)sizeof(NullableDatum);

      /* r_tmp1 = op->d.agg_strict_input_check.args */
      MIR_STEP_LOAD(r_tmp1, opno, d.agg_strict_input_check.args);

      for (int argno = 0; argno < nargs; argno++) {
        /* r_tmp2 = args[argno].isnull */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        argno * nd_size + isnull_off0, r_tmp1,
                                        0, 1)));
        /* if isnull, jump to jumpnull */
        if (jumpnull >= 0 && jumpnull < steps_len) {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_BNE, MIR_new_label_op(ctx, step_labels[jumpnull]),
                  MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));
        }
      }
      break;
    }

    /*
     * ---- AGG_STRICT_INPUT_CHECK_NULLS ----
     * Check bool nulls[i] for i=0..nargs-1,
     * jump to jumpnull if any null.
     */
    case EEOP_AGG_STRICT_INPUT_CHECK_NULLS: {
      int nargs = op->d.agg_strict_input_check.nargs;
      int jumpnull = op->d.agg_strict_input_check.jumpnull;

      /* r_tmp1 = op->d.agg_strict_input_check.nulls */
      MIR_STEP_LOAD(r_tmp1, opno, d.agg_strict_input_check.nulls);

      for (int argno = 0; argno < nargs; argno++) {
        /* r_tmp2 = nulls[argno] */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        argno * (int64_t)sizeof(bool), r_tmp1,
                                        0, 1)));
        /* if null, jump to jumpnull */
        if (jumpnull >= 0 && jumpnull < steps_len) {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_BNE, MIR_new_label_op(ctx, step_labels[jumpnull]),
                  MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));
        }
      }
      break;
    }

    /*
     * ---- AGG_PLAIN_PERGROUP_NULLCHECK ----
     * Load all_pergroups[setoff], jump to jumpnull if NULL.
     */
    case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK: {
      int setoff = op->d.agg_plain_pergroup_nullcheck.setoff;
      int jumpnull = op->d.agg_plain_pergroup_nullcheck.jumpnull;

      /* r_tmp1 = state->parent (AggState*) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P, offsetof(ExprState, parent),
                                      r_state, 0, 1)));
      /* r_tmp1 = aggstate->all_pergroups */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(AggState, all_pergroups), r_tmp1,
                                      0, 1)));
      /* r_tmp1 = all_pergroups[setoff] */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
              MIR_new_mem_op(ctx, MIR_T_P,
                             setoff * (int64_t)sizeof(AggStatePerGroup), r_tmp1,
                             0, 1)));
      /* if NULL, jump to jumpnull */
      if (jumpnull >= 0 && jumpnull < steps_len) {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ,
                         MIR_new_label_op(ctx, step_labels[jumpnull]),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      }
      break;
    }

    /*
     * ---- AGG_STRICT_DESERIALIZE ----
     * If args[0].isnull, jump to jumpnull.
     * Then call fcinfo->flinfo->fn_addr(fcinfo), store result.
     */
    case EEOP_AGG_STRICT_DESERIALIZE: {
      FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
      int jumpnull = op->d.agg_deserialize.jumpnull;
      int64_t null0_off =
          (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "dsfci");
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "dsret");

      /* r_fci = op->d.agg_deserialize.fcinfo_data */
      MIR_STEP_LOAD(r_fci, opno, d.agg_deserialize.fcinfo_data);

      /* r_tmp1 = fcinfo->args[0].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, null0_off, r_fci, 0, 1)));

      /* if null, jump to jumpnull */
      if (jumpnull >= 0 && jumpnull < steps_len) {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BNE,
                         MIR_new_label_op(ctx, step_labels[jumpnull]),
                         MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));
      }

      /* fcinfo->isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1),
              MIR_new_int_op(ctx, 0)));

      /* r_ret = fn_addr(fcinfo) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_ret),
                            MIR_new_reg_op(ctx, r_fci)));

      /* *op->resvalue = r_ret */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_ret)));

      /* *op->resnull = fcinfo->isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp1)));
      break;
    }

    /*
     * ---- AGG_DESERIALIZE ----
     * Call fcinfo->flinfo->fn_addr(fcinfo), store result. No jump.
     */
    case EEOP_AGG_DESERIALIZE: {
      FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "adfci");
      MIR_reg_t r_ret = mir_new_reg(ctx, f, MIR_T_I64, "adret");

      /* r_fci = op->d.agg_deserialize.fcinfo_data */
      MIR_STEP_LOAD(r_fci, opno, d.agg_deserialize.fcinfo_data);

      /* fcinfo->isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1),
              MIR_new_int_op(ctx, 0)));

      /* r_ret = fn_addr(fcinfo) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_ret),
                            MIR_new_reg_op(ctx, r_fci)));

      /* *op->resvalue = r_ret */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_ret)));

      /* *op->resnull = fcinfo->isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_NOT_DISTINCT: {
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
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_n0),
                       MIR_new_mem_op(ctx, MIR_T_U8, null0_off, r_fci, 0, 1)));
      /* r_n1 = args[1].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_n1),
                       MIR_new_mem_op(ctx, MIR_T_U8, null1_off, r_fci, 0, 1)));

      /* if (n0 != n1) goto one_null */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, one_null_label),
                       MIR_new_reg_op(ctx, r_n0), MIR_new_reg_op(ctx, r_n1)));

      /* n0 == n1: if (n0 != 0) goto both_null */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, both_null_label),
                       MIR_new_reg_op(ctx, r_n0), MIR_new_int_op(ctx, 0)));

      /* Neither null: call equality fn */
      /* fcinfo->isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1),
              MIR_new_int_op(ctx, 0)));
      /* r_ret = fn_addr(fcinfo) */
      if (mir_shared_code_mode) {
        MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
        int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                      offsetof(ExprEvalStep, d.func.fn_addr);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                         MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_reg_op(ctx, r_fn),
                              MIR_new_reg_op(ctx, r_ret),
                              MIR_new_reg_op(ctx, r_fci)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_ret),
                              MIR_new_reg_op(ctx, r_fci)));
      }

      /* For DISTINCT: invert result (resvalue = !result) */
      if (opcode == EEOP_DISTINCT) {
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_EQ, MIR_new_reg_op(ctx, r_ret),
                                     MIR_new_reg_op(ctx, r_ret),
                                     MIR_new_int_op(ctx, 0)));
      }

      /* *op->resvalue = r_ret, *op->resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_ret)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));

      /* one_null: nulls differ → DISTINCT=true, NOT_DISTINCT=false */
      MIR_append_insn(ctx, func_item, one_null_label);
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, (opcode == EEOP_DISTINCT) ? 1 : 0)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));

      /* both_null: both null → DISTINCT=false, NOT_DISTINCT=true */
      MIR_append_insn(ctx, func_item, both_null_label);
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, (opcode == EEOP_DISTINCT) ? 0 : 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_NULLIF: {
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
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, null0_off, r_fci, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, a_null_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Check if arg1 is null → skip to return_a */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, null1_off, r_fci, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, b_null_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Both non-null: call equality function */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV,
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1),
              MIR_new_int_op(ctx, 0)));
      if (mir_shared_code_mode) {
        MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
        int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                      offsetof(ExprEvalStep, d.func.fn_addr);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                         MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_reg_op(ctx, r_fn),
                              MIR_new_reg_op(ctx, r_ret),
                              MIR_new_reg_op(ctx, r_fci)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_ret),
                              MIR_new_reg_op(ctx, r_fci)));
      }

      /* If fcinfo->isnull, treat as not equal */
      MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(
              ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
              MIR_new_mem_op(ctx, MIR_T_U8,
                             offsetof(FunctionCallInfoBaseData, isnull), r_fci,
                             0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, not_equal_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* If result == false (0), not equal */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_equal_label),
                       MIR_new_reg_op(ctx, r_ret), MIR_new_int_op(ctx, 0)));

      /* Equal: return null — *op->resnull = true */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));

      /* not_equal / b_null: return a's value with resnull=false */
      MIR_append_insn(ctx, func_item, not_equal_label);
      MIR_append_insn(ctx, func_item, b_null_label);
      MIR_STEP_LOAD(r_fci, opno, d.func.fcinfo_data);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64, val0_off, r_fci, 0, 1)));
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));

      /* a_null: *op->resnull = true */
      MIR_append_insn(ctx, func_item, a_null_label);
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 1)));

      /* end: all paths converge */
      MIR_append_insn(ctx, func_item, end_label);
      break;
    }

    /*
     * ---- NEXTVALUEEXPR (inlined) ----
     * Call nextval_internal with seqid as immediate.
     * Compile-time switch on seqtypid.
     */
    case EEOP_NEXTVALUEEXPR: {
      Oid seqid = op->d.nextvalueexpr.seqid;
      Oid seqtypid = op->d.nextvalueexpr.seqtypid;

      char nv_reg_name[32];
      snprintf(nv_reg_name, sizeof(nv_reg_name), "nv_result_%d", opno);
      MIR_reg_t r_nv = MIR_new_func_reg(ctx, func_item->u.func, MIR_T_I64,
                                          nv_reg_name);

      /* Call nextval_internal(seqid, false) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_nextval),
                            MIR_new_ref_op(ctx, import_nextval),
                            MIR_new_reg_op(ctx, r_nv),
                            MIR_new_int_op(ctx, (int64_t)seqid),
                            MIR_new_int_op(ctx, 0)));

      /* Compile-time cast */
      switch (seqtypid) {
      case INT2OID:
        /* Sign-extend 16-bit: (int16)(int64) val */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT16,
                                     MIR_new_reg_op(ctx, r_nv),
                                     MIR_new_reg_op(ctx, r_nv)));
        break;
      case INT4OID:
        /* Sign-extend 32-bit: (int32)(int64) val */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_EXT32,
                                     MIR_new_reg_op(ctx, r_nv),
                                     MIR_new_reg_op(ctx, r_nv)));
        break;
      case INT8OID:
        break; /* identity */
      }

      /* *resvalue = r_nv */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_nv)));
      /* *resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- ROW (inlined) ----
     * Direct call to heap_form_tuple + inline HeapTupleGetDatum.
     */
    case EEOP_ROW: {
      /* Load args from step data */
      MIR_STEP_LOAD(r_tmp1, opno, d.row.tupdesc);
      MIR_STEP_LOAD(r_tmp2, opno, d.row.elemvalues);

      char row_reg_name[32];
      snprintf(row_reg_name, sizeof(row_reg_name), "row_nulls_%d", opno);
      MIR_reg_t r_rnulls = MIR_new_func_reg(ctx, func_item->u.func,
                                              MIR_T_I64, row_reg_name);
      MIR_STEP_LOAD(r_rnulls, opno, d.row.elemnulls);

      /* heap_form_tuple(tupdesc, values, nulls) -> HeapTuple */
      snprintf(row_reg_name, sizeof(row_reg_name), "row_tuple_%d", opno);
      MIR_reg_t r_tuple = MIR_new_func_reg(ctx, func_item->u.func,
                                             MIR_T_I64, row_reg_name);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 6, MIR_new_ref_op(ctx, proto_hft),
                            MIR_new_ref_op(ctx, import_hft),
                            MIR_new_reg_op(ctx, r_tuple),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_tmp2),
                            MIR_new_reg_op(ctx, r_rnulls)));

      /* HeapTupleGetDatum(tuple) = HeapTupleHeaderGetDatum(tuple->t_data)
       * Must call HeapTupleHeaderGetDatum to flatten external TOAST ptrs */
      snprintf(row_reg_name, sizeof(row_reg_name), "row_tdata_%d", opno);
      MIR_reg_t r_tdata = MIR_new_func_reg(ctx, func_item->u.func,
                                             MIR_T_I64, row_reg_name);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tdata),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      (int)offsetof(HeapTupleData, t_data),
                                      r_tuple, 0, 1)));

      snprintf(row_reg_name, sizeof(row_reg_name), "row_datum_%d", opno);
      MIR_reg_t r_datum = MIR_new_func_reg(ctx, func_item->u.func,
                                             MIR_T_I64, row_reg_name);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_htgd),
                            MIR_new_ref_op(ctx, import_htgd),
                            MIR_new_reg_op(ctx, r_datum),
                            MIR_new_reg_op(ctx, r_tdata)));

      /* *resvalue = datum */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_datum)));
      /* *resnull = false */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- AGG_ORDERED_TRANS_DATUM ----
     * Direct call (AggStatePerTransData is opaque).
     */
    case EEOP_AGG_ORDERED_TRANS_DATUM: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }

    /*
     * ---- AGG_ORDERED_TRANS_TUPLE ----
     * Direct call (AggStatePerTransData is opaque).
     */
    case EEOP_AGG_ORDERED_TRANS_TUPLE: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }

    /*
     * ---- WHOLEROW ----
     * Direct call to ExecEvalWholeRowVar.
     */
    case EEOP_WHOLEROW: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }

    /*
     * ---- CONVERT_ROWTYPE (partially inlined) ----
     * Null check: if *op->resnull, skip C call.
     */
    case EEOP_CONVERT_ROWTYPE: {
      MIR_insn_t cvt_done = MIR_new_label(ctx);

      /* Check *op->resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      /* if (resnull) skip */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, cvt_done),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Non-null: call ExecEvalConvertRowtype */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));

      MIR_append_insn(ctx, func_item, cvt_done);
      break;
    }

#ifdef HAVE_EEOP_JSONEXPR
    /*
     * ---- JSONEXPR_COERCION_FINISH (partially inlined, PG17+) ----
     * Happy path: if escontext.error_occurred is false, no-op.
     */
    case EEOP_JSONEXPR_COERCION_FINISH: {
      MIR_insn_t jcf_done = MIR_new_label(ctx);

      /* Compute struct offset at JIT compile time */
      JsonExprState *jsestate = op->d.jsonexpr.jsestate;
      int esctx_err_off = (int)(
          (char *)&jsestate->escontext.error_occurred - (char *)jsestate);

      /* Load jsestate from step data */
      MIR_STEP_LOAD(r_tmp2, opno, d.jsonexpr.jsestate);

      /* Check error_occurred */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, esctx_err_off,
                                      r_tmp2, 0, 1)));
      /* if (!error_occurred) skip */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, jcf_done),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Error: call ExecEvalJsonCoercionFinish(state, op) */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1)));

      MIR_append_insn(ctx, func_item, jcf_done);
      break;
    }
#endif /* HAVE_EEOP_JSONEXPR */

#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
    /*
     * ---- JSON_CONSTRUCTOR (PG16+) ----
     * Direct call to ExecEvalJsonConstructor.
     */
    case EEOP_JSON_CONSTRUCTOR: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }
#endif /* HAVE_EEOP_JSON_CONSTRUCTOR */

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
    case EEOP_FIELDSTORE_DEFORM:
    case EEOP_FIELDSTORE_FORM:
#ifdef HAVE_EEOP_JSONEXPR
    case EEOP_JSONEXPR_COERCION:
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
    case EEOP_MERGE_SUPPORT_FUNC:
#endif
    case EEOP_SUBPLAN: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }

    /*
     * ---- DOMAIN_NOTNULL (inlined) ----
     * Fast path: if *op->resnull is false, done.
     * Slow path: call ExecEvalConstraintNotNull(state, op).
     */
    case EEOP_DOMAIN_NOTNULL: {
      MIR_insn_t ok_label = MIR_new_label(ctx);

      /* r_tmp1 = *op->resnull */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      /* if (!resnull) goto ok */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, ok_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Slow path: call ExecEvalConstraintNotNull(state, op) */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1)));

      MIR_append_insn(ctx, func_item, ok_label);
      break;
    }

    /*
     * ---- DOMAIN_CHECK (inlined) ----
     * Fast path: if *checknull → done. Else if DatumGetBool(*checkvalue) → done.
     * Slow path: call ExecEvalConstraintCheck(state, op).
     */
    case EEOP_DOMAIN_CHECK: {
      MIR_insn_t done_label = MIR_new_label(ctx);

      /* r_tmp1 = *op->d.domaincheck.checknull */
      MIR_STEP_LOAD(r_tmp3, opno, d.domaincheck.checknull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
      /* if (checknull) goto done */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* r_tmp1 = DatumGetBool(*op->d.domaincheck.checkvalue) */
      MIR_STEP_LOAD(r_tmp3, opno, d.domaincheck.checkvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1)));
      /* if (checkvalue != 0) goto done */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, done_label),
                       MIR_new_reg_op(ctx, r_tmp1), MIR_new_int_op(ctx, 0)));

      /* Slow path: call ExecEvalConstraintCheck(state, op) */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1)));

      MIR_append_insn(ctx, func_item, done_label);
      break;
    }

    /*
     * ---- SCALARARRAYOP (partially inlined) ----
     * Inline the NULL-array check, then call helper with SIMD dispatch.
     */
    case EEOP_SCALARARRAYOP: {
      MIR_insn_t done_label = MIR_new_label(ctx);

      /* 1. Check *op->resnull — if true, array is NULL → skip */
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_BNE,
                                   MIR_new_label_op(ctx, done_label),
                                   MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_int_op(ctx, 0)));

      /* 2. Load *op->resvalue → call DatumGetArrayTypeP */
      MIR_STEP_LOAD(r_tmp1, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));
      char saop_reg_name[32];
      snprintf(saop_reg_name, sizeof(saop_reg_name), "saop_arr_%d", opno);
      MIR_reg_t r_arr = MIR_new_func_reg(ctx, func_item->u.func, MIR_T_I64,
                                          saop_reg_name);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_detoast_arr),
                            MIR_new_ref_op(ctx, import_detoast_arr),
                            MIR_new_reg_op(ctx, r_arr),
                            MIR_new_reg_op(ctx, r_tmp1)));
      /* r_arr now holds the result from pg_detoast_datum */

      /* 3. Load scalar value and null from fcinfo->args[0] */
      snprintf(saop_reg_name, sizeof(saop_reg_name), "saop_sv_%d", opno);
      MIR_reg_t r_scalar_val = MIR_new_func_reg(ctx, func_item->u.func,
                                                  MIR_T_I64, saop_reg_name);
      snprintf(saop_reg_name, sizeof(saop_reg_name), "saop_sn_%d", opno);
      MIR_reg_t r_scalar_null = MIR_new_func_reg(ctx, func_item->u.func,
                                                   MIR_T_I64, saop_reg_name);
      MIR_STEP_LOAD(r_tmp1, opno, d.scalararrayop.fcinfo_data);
      /* scalar_value = fcinfo->args[0].value */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_scalar_val),
                       MIR_new_mem_op(ctx, MIR_T_I64,
                                      offsetof(FunctionCallInfoBaseData,
                                               args[0].value),
                                      r_tmp1, 0, 1)));
      /* scalar_null = fcinfo->args[0].isnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_scalar_null),
                       MIR_new_mem_op(ctx, MIR_T_U8,
                                      offsetof(FunctionCallInfoBaseData,
                                               args[0].isnull),
                                      r_tmp1, 0, 1)));

      /* 4. Call pg_jitter_scalararrayop_loop(op, arr, scalar_value,
       * scalar_null) */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 6, MIR_new_ref_op(ctx, proto_saop_loop),
                            MIR_new_ref_op(ctx, import_saop_loop),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_arr),
                            MIR_new_reg_op(ctx, r_scalar_val),
                            MIR_new_reg_op(ctx, r_scalar_null)));

      MIR_append_insn(ctx, func_item, done_label);
      break;
    }

    /*
     * ---- IS_JSON (simdjson-accelerated for text input) ----
     */
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
    case EEOP_IS_JSON: {
#ifdef PG_JITTER_HAVE_SIMDJSON
      JsonIsPredicate *pred = op->d.is_json.pred;
      if (exprType(pred->expr) == TEXTOID && !pred->unique_keys) {
        /* simdjson fast path */
        MIR_label_t skip_null = MIR_new_label(ctx);
        MIR_label_t done_label = MIR_new_label(ctx);

        /* Load *op->resnull */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
        /* If resnull != 0, skip to null path */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, skip_null),
                         MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_int_op(ctx, 0)));

        /* Load *op->resvalue (text Datum) */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));

        /* Call pg_jitter_sj_is_json_datum(datum, item_type) → r_tmp2 */
        MIR_reg_t r_item_type =
            mir_new_reg(ctx, f, MIR_T_I64, "itype");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_item_type),
                         MIR_new_int_op(ctx, (int64_t)pred->item_type)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_sj_is_json),
                              MIR_new_ref_op(ctx, import_sj_is_json),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_item_type)));

        /* *op->resvalue = BoolGetDatum(r_tmp2) — zero-extend I32 to I64 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_UEXT32, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_reg_op(ctx, r_tmp2)));
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));

        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, done_label)));

        /* Null path: *op->resvalue = BoolGetDatum(false) = 0 */
        MIR_append_insn(ctx, func_item, skip_null);
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));

        MIR_append_insn(ctx, func_item, done_label);
        break;
      }
#endif /* PG_JITTER_HAVE_SIMDJSON */
      /* Fallback: generic 2-arg call */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1)));
      break;
    }
#endif

    /*
     * ---- DIRECT CALL: 2-arg void ----
     * fn(ExprState *state, ExprEvalStep *op)
     */
    case EEOP_CURRENTOFEXPR:
    case EEOP_XMLEXPR:
    {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1)));
      break;
    }

    /*
     * ---- GROUPING_FUNC (inlined) ----
     * Unroll the clause loop at compile time (cap at 8).
     */
    case EEOP_GROUPING_FUNC: {
      List *clauses = op->d.grouping_func.clauses;
      int nclauses = list_length(clauses);

      if (nclauses > 8) {
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1)));
      } else {
        char grp_reg_name[32];
        snprintf(grp_reg_name, sizeof(grp_reg_name), "grp_result_%d", opno);
        MIR_reg_t r_result = MIR_new_func_reg(ctx, func_item->u.func, MIR_T_I64,
                                               grp_reg_name);
        snprintf(grp_reg_name, sizeof(grp_reg_name), "grouped_cols_%d", opno);
        MIR_reg_t r_gcols = MIR_new_func_reg(ctx, func_item->u.func, MIR_T_I64,
                                              grp_reg_name);

        /* r_gcols = state->parent->grouped_cols */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_gcols),
                         MIR_new_mem_op(ctx, MIR_T_P,
                                        offsetof(ExprState, parent),
                                        r_state, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_gcols),
                         MIR_new_mem_op(ctx, MIR_T_P,
                                        offsetof(AggState, grouped_cols),
                                        r_gcols, 0, 1)));
        /* result = 0 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_result),
                         MIR_new_int_op(ctx, 0)));

        /* bms_is_member proto/import declared globally */

        ListCell *lc;
        foreach (lc, clauses) {
          int attnum = lfirst_int(lc);
          MIR_insn_t is_member_label = MIR_new_label(ctx);

          /* result <<= 1 */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_LSH, MIR_new_reg_op(ctx, r_result),
                           MIR_new_reg_op(ctx, r_result),
                           MIR_new_int_op(ctx, 1)));

          /* r_tmp1 = bms_is_member(attnum, grouped_cols) */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(
                  ctx, 5,
                  MIR_new_ref_op(ctx, proto_bms),
                  MIR_new_ref_op(ctx, import_bms),
                  MIR_new_reg_op(ctx, r_tmp1),
                  MIR_new_int_op(ctx, attnum),
                  MIR_new_reg_op(ctx, r_gcols)));
#ifdef _WIN64
          /* bms_is_member returns bool (1 byte in AL). Windows x64 ABI
           * does not guarantee upper bytes of RAX are cleared. */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_int_op(ctx, 0xFF)));
#endif

          /* if (bms_is_member) skip OR */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_BNE,
                           MIR_new_label_op(ctx, is_member_label),
                           MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_int_op(ctx, 0)));
          /* result |= 1 */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, r_result),
                           MIR_new_reg_op(ctx, r_result),
                           MIR_new_int_op(ctx, 1)));
          MIR_append_insn(ctx, func_item, is_member_label);
        }

        /* *resvalue = Int32GetDatum(result) */
        MIR_STEP_LOAD(r_tmp3, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_reg_op(ctx, r_result)));
        /* *resnull = false */
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 0)));
      }
      break;
    }

    /*
     * ---- MINMAX (GREATEST/LEAST, partially inlined) ----
     * For nelems <= 4: unroll. For nelems > 4: C call.
     */
    case EEOP_MINMAX: {
      int nelems = op->d.minmax.nelems;

      if (nelems > 4) {
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_agg_helper),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1)));
      } else {
        /* Set *resnull = true */
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 1)));

        char mm_reg_name[32];
        snprintf(mm_reg_name, sizeof(mm_reg_name), "mm_newval_%d", opno);
        MIR_reg_t r_newval = MIR_new_func_reg(ctx, func_item->u.func,
                                               MIR_T_I64, mm_reg_name);

        for (int i = 0; i < nelems; i++) {
          MIR_insn_t null_label = MIR_new_label(ctx);
          MIR_insn_t skip_cmp_label = MIR_new_label(ctx);
          MIR_insn_t not_first_label = MIR_new_label(ctx);

          /* r_tmp1 = nulls[i] */
          MIR_STEP_LOAD(r_tmp3, opno, d.minmax.nulls);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_mem_op(ctx, MIR_T_U8,
                                          i * (int64_t)sizeof(bool),
                                          r_tmp3, 0, 1)));
          /* if null, skip */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_BNE,
                           MIR_new_label_op(ctx, null_label),
                           MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_int_op(ctx, 0)));

          /* r_newval = values[i] */
          MIR_STEP_LOAD(r_tmp3, opno, d.minmax.values);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_newval),
                           MIR_new_mem_op(ctx, MIR_T_I64,
                                          i * (int64_t)sizeof(Datum),
                                          r_tmp3, 0, 1)));

          /* Check *resnull */
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1)));
          /* if (!resnull) → not first */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_BEQ,
                           MIR_new_label_op(ctx, not_first_label),
                           MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_int_op(ctx, 0)));

          /* First non-null: adopt value */
          MIR_STEP_LOAD(r_tmp3, opno, resvalue);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                           MIR_new_reg_op(ctx, r_newval)));
          MIR_STEP_LOAD(r_tmp3, opno, resnull);
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                           MIR_new_int_op(ctx, 0)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_JMP,
                           MIR_new_label_op(ctx, skip_cmp_label)));

          /* Not first: compare */
          MIR_append_insn(ctx, func_item, not_first_label);
          {
            /* Load fcinfo from step data */
            MIR_STEP_LOAD(r_tmp3, opno, d.minmax.fcinfo_data);

            /* fcinfo->args[0].value = *resvalue */
            MIR_STEP_LOAD(r_tmp2, opno, resvalue);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                             MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp2, 0, 1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(
                    ctx, MIR_MOV,
                    MIR_new_mem_op(
                        ctx, MIR_T_I64,
                        offsetof(FunctionCallInfoBaseData, args[0].value),
                        r_tmp3, 0, 1),
                    MIR_new_reg_op(ctx, r_tmp1)));
            /* fcinfo->args[1].value = newval */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(
                    ctx, MIR_MOV,
                    MIR_new_mem_op(
                        ctx, MIR_T_I64,
                        offsetof(FunctionCallInfoBaseData, args[1].value),
                        r_tmp3, 0, 1),
                    MIR_new_reg_op(ctx, r_newval)));
            /* fcinfo->isnull = false */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(
                    ctx, MIR_MOV,
                    MIR_new_mem_op(
                        ctx, MIR_T_U8,
                        offsetof(FunctionCallInfoBaseData, isnull),
                        r_tmp3, 0, 1),
                    MIR_new_int_op(ctx, 0)));

            /* r_tmp1 = fcinfo->flinfo->fn_addr */
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                             MIR_new_mem_op(
                                 ctx, MIR_T_P,
                                 offsetof(FunctionCallInfoBaseData, flinfo),
                                 r_tmp3, 0, 1)));
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                             MIR_new_mem_op(
                                 ctx, MIR_T_P,
                                 offsetof(FmgrInfo, fn_addr),
                                 r_tmp1, 0, 1)));

            /* Call fn_addr(fcinfo) → r_tmp2 (cmpresult as Datum) */
            char mm_proto_name[32];
            snprintf(mm_proto_name, sizeof(mm_proto_name), "p_mmcmp_%d_%d", opno, i);
            MIR_type_t cmp_ret_type = MIR_T_I64;
            MIR_item_t cmp_proto =
                MIR_new_proto(ctx, mm_proto_name, 1, &cmp_ret_type,
                              1, MIR_T_P, "f");
            MIR_append_insn(
                ctx, func_item,
                MIR_new_call_insn(
                    ctx, 4,
                    MIR_new_ref_op(ctx, cmp_proto),
                    MIR_new_reg_op(ctx, r_tmp1),
                    MIR_new_reg_op(ctx, r_tmp2),
                    MIR_new_reg_op(ctx, r_tmp3)));

            /* Check cmpresult and update if needed */
            MIR_insn_t no_update_label = MIR_new_label(ctx);
            if (op->d.minmax.op == IS_LEAST) {
              /* LEAST: update if cmpresult > 0 */
              MIR_append_insn(
                  ctx, func_item,
                  MIR_new_insn(ctx, MIR_BLE,
                               MIR_new_label_op(ctx, no_update_label),
                               MIR_new_reg_op(ctx, r_tmp2),
                               MIR_new_int_op(ctx, 0)));
            } else {
              /* GREATEST: update if cmpresult < 0 */
              MIR_append_insn(
                  ctx, func_item,
                  MIR_new_insn(ctx, MIR_BGE,
                               MIR_new_label_op(ctx, no_update_label),
                               MIR_new_reg_op(ctx, r_tmp2),
                               MIR_new_int_op(ctx, 0)));
            }

            MIR_STEP_LOAD(r_tmp3, opno, resvalue);
            MIR_append_insn(
                ctx, func_item,
                MIR_new_insn(ctx, MIR_MOV,
                             MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                             MIR_new_reg_op(ctx, r_newval)));
            MIR_append_insn(ctx, func_item, no_update_label);
          }

          MIR_append_insn(ctx, func_item, skip_cmp_label);
          MIR_append_insn(ctx, func_item, null_label);
        }
      }
      break;
    }

    /*
     * ---- PARAM_CALLBACK: indirect fn pointer ----
     * op->d.cparam.paramfunc(state, op, econtext)
     */
    case EEOP_PARAM_CALLBACK: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }

    /*
     * ---- PARAM_EXEC (inlined) ----
     * Fast path: load value/isnull from econtext->ecxt_param_exec_vals[paramid].
     * Slow path (execPlan != NULL, i.e. lazy InitPlan): call ExecEvalParamExec.
     */
    case EEOP_PARAM_EXEC: {
      int paramid = op->d.param.paramid;
      MIR_insn_t slow_label = MIR_new_label(ctx);
      MIR_insn_t done_label = MIR_new_label(ctx);

      /* r_tmp1 = econtext->ecxt_param_exec_vals */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(ExprContext, ecxt_param_exec_vals),
                                      r_econtext, 0, 1)));
      /* r_tmp1 = &ecxt_param_exec_vals[paramid] */
      if (paramid != 0)
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_tmp1),
                         MIR_new_reg_op(ctx, r_tmp1),
                         MIR_new_int_op(ctx,
                                        paramid * (int64_t)sizeof(ParamExecData))));

      /* r_tmp2 = prm->execPlan */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_P,
                                      offsetof(ParamExecData, execPlan),
                                      r_tmp1, 0, 1)));
      /* if (execPlan != 0) goto slow_label */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_BNE,
                                   MIR_new_label_op(ctx, slow_label),
                                   MIR_new_reg_op(ctx, r_tmp2),
                                   MIR_new_int_op(ctx, 0)));

      /* Fast path: *resvalue = prm->value; *resnull = prm->isnull */
      /* r_tmp2 = prm->value */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64,
                                      offsetof(ParamExecData, value),
                                      r_tmp1, 0, 1)));
      /* Load resvalue pointer and store */
      MIR_STEP_LOAD(r_tmp3, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));
      /* r_tmp2 = prm->isnull (u8) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8,
                                      offsetof(ParamExecData, isnull),
                                      r_tmp1, 0, 1)));
      /* Load resnull pointer and store */
      MIR_STEP_LOAD(r_tmp3, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));
      /* Jump to done */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_JMP,
                                   MIR_new_label_op(ctx, done_label)));

      /* Slow path: call ExecEvalParamExec(state, op, econtext) */
      MIR_append_insn(ctx, func_item, slow_label);
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));

      MIR_append_insn(ctx, func_item, done_label);
      break;
    }

    /*
     * ---- PARAM_EXTERN ----
     * Direct call: fn(state, op, econtext) -> void
     */
    case EEOP_PARAM_EXTERN: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));
      break;
    }

    /*
     * ---- SYSVAR (partially inlined) ----
     * For tableoid (-6) and ctid (-1): inline the load.
     * For OLD/NEW and other attnums: fall back to ExecEvalSysVar.
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
      int attnum = op->d.var.attnum;

      switch (opcode) {
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

      bool can_inline = (attnum == TableOidAttributeNumber ||
                         attnum == SelfItemPointerAttributeNumber);
#ifdef HAVE_EEOP_OLD_NEW
      if (opcode == EEOP_OLD_SYSVAR || opcode == EEOP_NEW_SYSVAR)
        can_inline = false;
#endif

      if (!can_inline) {
        /* Fall back to C call */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_P, slot_offset,
                                        r_econtext, 0, 1)));
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(
                ctx, 6, MIR_new_ref_op(ctx, proto_4arg_void),
                MIR_new_ref_op(ctx, step_fn_imports[opno]),
                MIR_new_reg_op(ctx, r_state), MIR_new_reg_op(ctx, r_tmp1),
                MIR_new_reg_op(ctx, r_econtext),
                MIR_new_reg_op(ctx, r_tmp2)));
      } else {
        /* r_tmp1 = slot = econtext->ecxt_*tuple */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                         MIR_new_mem_op(ctx, MIR_T_P, slot_offset,
                                        r_econtext, 0, 1)));

        if (attnum == TableOidAttributeNumber) {
          /* r_tmp2 = (Datum)slot->tts_tableOid (U32 → I64) */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                           MIR_new_mem_op(ctx, MIR_T_U32,
                                          offsetof(TupleTableSlot, tts_tableOid),
                                          r_tmp1, 0, 1)));
        } else {
          /* ctid: r_tmp2 = &slot->tts_tid */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_ADD, MIR_new_reg_op(ctx, r_tmp2),
                           MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_int_op(ctx,
                                          offsetof(TupleTableSlot, tts_tid))));
        }

        /* *resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp3, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp3, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
        /* *resnull = false */
        MIR_STEP_LOAD(r_tmp3, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp3, 0, 1),
                         MIR_new_int_op(ctx, 0)));
      }
      break;
    }

    /*
     * ---- SBSREF_OLD / SBSREF_ASSIGN / SBSREF_FETCH ----
     * Indirect call: op->d.sbsref.subscriptfunc(state, op, econtext)
     */
    case EEOP_SBSREF_OLD:
    case EEOP_SBSREF_ASSIGN:
    case EEOP_SBSREF_FETCH: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_3arg_void),
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
    case EEOP_SBSREF_SUBSCRIPTS: {
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 6, MIR_new_ref_op(ctx, proto_fallback),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_tmp2), /* return value */
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));

      /* Zero-extend bool return (same Windows x64 ABI issue) */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_int_op(ctx, 0xFF)));

      /* If false (0), jump to jumpdone */
      {
        int jumpdone = op->d.sbsref_subscript.jumpdone;
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ,
                         MIR_new_label_op(ctx, step_labels[jumpdone]),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));
      }
      break;
    }

    /*
     * ---- IOCOERCE ----
     * Two function calls: output fn -> cstring -> input fn.
     * If *resnull, skip entirely.
     */
    case EEOP_IOCOERCE: {
      FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
      FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

      /*
       * Byte offsets within FunctionCallInfoBaseData for field access.
       * These are position-independent — valid in any process.
       */
      int64_t arg0_val_off =
          (int64_t)((char *)&fcinfo_out->args[0].value - (char *)fcinfo_out);
      int64_t arg0_null_off =
          (int64_t)((char *)&fcinfo_out->args[0].isnull - (char *)fcinfo_out);
      int64_t isnull_off = offsetof(FunctionCallInfoBaseData, isnull);

      MIR_label_t skip_null = MIR_new_label(ctx);

      /* Load *op->resnull (pointer to bool) */
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      /* If resnull != 0, skip */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, skip_null),
                       MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

      /* Load *op->resvalue */
      MIR_STEP_LOAD(r_tmp1, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));

      /*
       * Load fcinfo_out from step data.  In shared mode the fcinfo pointer
       * is process-local so we must always load it at runtime; the old code
       * embedded the leader's pointer via MIR_STEP_ADDR_RAW which gave
       * &steps[opno] (wrong) in shared mode.
       */
      MIR_reg_t r_fci_out = mir_new_reg(ctx, f, MIR_T_I64, "fco");
      MIR_STEP_LOAD(r_fci_out, opno, d.iocoerce.fcinfo_data_out);

      /* fcinfo_out->args[0].value = resvalue */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, arg0_val_off, r_fci_out,
                                      0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* fcinfo_out->args[0].isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, arg0_null_off, r_fci_out,
                                      0, 1),
                       MIR_new_int_op(ctx, 0)));

      /* fcinfo_out->isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci_out, 0,
                                      1),
                       MIR_new_int_op(ctx, 0)));

      /* Call output fn: ret = fn_addr_out(fcinfo_out) */
      if (mir_shared_code_mode) {
        MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
        int64_t fn_off =
            (int64_t)((char *)&fcinfo_out->flinfo->fn_addr - (char *)fcinfo_out)
            - (int64_t)((char *)fcinfo_out->flinfo - (char *)fcinfo_out);
        /* Load flinfo pointer first, then fn_addr from it */
        MIR_reg_t r_flinfo = mir_new_reg(ctx, f, MIR_T_I64, "fli");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_flinfo),
                MIR_new_mem_op(ctx, MIR_T_P,
                               offsetof(FunctionCallInfoBaseData, flinfo),
                               r_fci_out, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                         MIR_new_mem_op(ctx, MIR_T_P,
                                        offsetof(FmgrInfo, fn_addr), r_flinfo,
                                        0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_reg_op(ctx, r_fn),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fci_out)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fci_out)));
      }

      /* r_tmp2 now holds cstring result (Datum) */

      /* Load fcinfo_in from step data */
      MIR_reg_t r_fci_in = mir_new_reg(ctx, f, MIR_T_I64, "fci");
      MIR_STEP_LOAD(r_fci_in, opno, d.iocoerce.fcinfo_data_in);

#ifdef PG_JITTER_HAVE_SIMDJSON
      if (fcinfo_in->flinfo->fn_oid == 321 /* F_JSON_IN */ ||
          fcinfo_in->flinfo->fn_oid == 3806 /* F_JSONB_IN */) {
        /* simdjson fast path: sj_fn(cstring, fcinfo_in) → Datum */
        MIR_item_t sj_import = (fcinfo_in->flinfo->fn_oid == 321)
                                    ? import_sj_json_in
                                    : import_sj_jsonb_in;
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_sj_json_in),
                              MIR_new_ref_op(ctx, sj_import),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fci_in)));

        /* *op->resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));

        /* *op->resnull = false */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));
      } else
#endif /* PG_JITTER_HAVE_SIMDJSON */
      {
        /* Standard input function call */

        /* fcinfo_in->args[0].value = r_tmp2 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64, arg0_val_off, r_fci_in, 0, 1),
                MIR_new_reg_op(ctx, r_tmp2)));

        /* fcinfo_in->args[0].isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8, arg0_null_off, r_fci_in, 0, 1),
                MIR_new_int_op(ctx, 0)));

        /* fcinfo_in->isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci_in, 0,
                                        1),
                         MIR_new_int_op(ctx, 0)));

        /* Call input fn: ret = fn_addr_in(fcinfo_in) */
        if (mir_shared_code_mode) {
          MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
          MIR_reg_t r_flinfo = mir_new_reg(ctx, f, MIR_T_I64, "fli");
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_flinfo),
                  MIR_new_mem_op(ctx, MIR_T_P,
                                 offsetof(FunctionCallInfoBaseData, flinfo),
                                 r_fci_in, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                           MIR_new_mem_op(ctx, MIR_T_P,
                                          offsetof(FmgrInfo, fn_addr),
                                          r_flinfo, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_reg_op(ctx, r_fn),
                                MIR_new_reg_op(ctx, r_tmp2),
                                MIR_new_reg_op(ctx, r_fci_in)));
        } else {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_ref_op(ctx, ioc_in_imports[opno]),
                                MIR_new_reg_op(ctx, r_tmp2),
                                MIR_new_reg_op(ctx, r_fci_in)));
        }

        /* *op->resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));

        /* *op->resnull = fcinfo_in->isnull */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci_in, 0,
                                        1)));
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
      }

      MIR_append_insn(ctx, func_item, skip_null);
      break;
    }

    /*
     * ---- IOCOERCE_SAFE ----
     * Same as IOCOERCE but with soft-error handling after input function.
     * If ErrorSaveContext.error_occurred is true after input function,
     * store *resnull=true, *resvalue=0 instead of the normal result.
     */
#ifdef HAVE_EEOP_IOCOERCE_SAFE
    case EEOP_IOCOERCE_SAFE: {
      FunctionCallInfo fcinfo_out = op->d.iocoerce.fcinfo_data_out;
      FunctionCallInfo fcinfo_in = op->d.iocoerce.fcinfo_data_in;

      int64_t arg0_val_off =
          (int64_t)((char *)&fcinfo_out->args[0].value - (char *)fcinfo_out);
      int64_t arg0_null_off =
          (int64_t)((char *)&fcinfo_out->args[0].isnull - (char *)fcinfo_out);
      int64_t isnull_off = offsetof(FunctionCallInfoBaseData, isnull);

      MIR_label_t skip_null_safe = MIR_new_label(ctx);

      /* Load *op->resnull */
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      /* If resnull != 0, skip */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, skip_null_safe),
                       MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

      /* Load *op->resvalue */
      MIR_STEP_LOAD(r_tmp1, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1)));

      /* Load fcinfo_out from step data */
      MIR_reg_t r_fco_safe = mir_new_reg(ctx, f, MIR_T_I64, "fcos");
      MIR_STEP_LOAD(r_fco_safe, opno, d.iocoerce.fcinfo_data_out);

      /* fcinfo_out->args[0].value = resvalue */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, arg0_val_off,
                                      r_fco_safe, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* fcinfo_out->args[0].isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, arg0_null_off,
                                      r_fco_safe, 0, 1),
                       MIR_new_int_op(ctx, 0)));

      /* fcinfo_out->isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fco_safe,
                                      0, 1),
                       MIR_new_int_op(ctx, 0)));

      /* Call output fn: ret = fn_addr_out(fcinfo_out) */
      if (mir_shared_code_mode) {
        MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
        MIR_reg_t r_flinfo = mir_new_reg(ctx, f, MIR_T_I64, "fli");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV, MIR_new_reg_op(ctx, r_flinfo),
                MIR_new_mem_op(ctx, MIR_T_P,
                               offsetof(FunctionCallInfoBaseData, flinfo),
                               r_fco_safe, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                         MIR_new_mem_op(ctx, MIR_T_P,
                                        offsetof(FmgrInfo, fn_addr), r_flinfo,
                                        0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_reg_op(ctx, r_fn),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fco_safe)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fco_safe)));
      }

      /* r_tmp2 now holds cstring result (Datum) */

      /* Load fcinfo_in from step data */
      MIR_reg_t r_fci_safe = mir_new_reg(ctx, f, MIR_T_I64, "fcis");
      MIR_STEP_LOAD(r_fci_safe, opno, d.iocoerce.fcinfo_data_in);

#ifdef PG_JITTER_HAVE_SIMDJSON
      if (fcinfo_in->flinfo->fn_oid == 321 /* F_JSON_IN */ ||
          fcinfo_in->flinfo->fn_oid == 3806 /* F_JSONB_IN */) {
        /* simdjson fast path: sj_fn(cstring, fcinfo_in) -> Datum */
        MIR_item_t sj_import = (fcinfo_in->flinfo->fn_oid == 321)
                                    ? import_sj_json_in
                                    : import_sj_jsonb_in;
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 5, MIR_new_ref_op(ctx, proto_sj_json_in),
                              MIR_new_ref_op(ctx, sj_import),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fci_safe)));

        /* *op->resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));

        /* *op->resnull = false (simdjson ereport's on error, skip soft check) */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));
      } else
#endif /* PG_JITTER_HAVE_SIMDJSON */
      {
        /* Standard input function call */

        /* fcinfo_in->args[0].value = r_tmp2 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_I64, arg0_val_off, r_fci_safe, 0, 1),
                MIR_new_reg_op(ctx, r_tmp2)));

        /* fcinfo_in->args[0].isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(
                ctx, MIR_MOV,
                MIR_new_mem_op(ctx, MIR_T_U8, arg0_null_off, r_fci_safe, 0, 1),
                MIR_new_int_op(ctx, 0)));

        /* fcinfo_in->isnull = false */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci_safe,
                                        0, 1),
                         MIR_new_int_op(ctx, 0)));

        /* Call input fn: ret = fn_addr_in(fcinfo_in) */
        if (mir_shared_code_mode) {
          MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
          MIR_reg_t r_flinfo = mir_new_reg(ctx, f, MIR_T_I64, "fli");
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(
                  ctx, MIR_MOV, MIR_new_reg_op(ctx, r_flinfo),
                  MIR_new_mem_op(ctx, MIR_T_P,
                                 offsetof(FunctionCallInfoBaseData, flinfo),
                                 r_fci_safe, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                           MIR_new_mem_op(ctx, MIR_T_P,
                                          offsetof(FmgrInfo, fn_addr),
                                          r_flinfo, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_reg_op(ctx, r_fn),
                                MIR_new_reg_op(ctx, r_tmp2),
                                MIR_new_reg_op(ctx, r_fci_safe)));
        } else {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                                MIR_new_ref_op(ctx, ioc_in_imports[opno]),
                                MIR_new_reg_op(ctx, r_tmp2),
                                MIR_new_reg_op(ctx, r_fci_safe)));
        }

        /* Check SOFT_ERROR_OCCURRED(fcinfo_in->context) */
        MIR_label_t soft_error_label = MIR_new_label(ctx);
        MIR_label_t done_safe_label = MIR_new_label(ctx);

        /* Load fcinfo_in->context */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp3),
                         MIR_new_mem_op(ctx, MIR_T_P,
                                        offsetof(FunctionCallInfoBaseData,
                                                 context),
                                        r_fci_safe, 0, 1)));

        /* Load error_occurred bool */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp3),
                         MIR_new_mem_op(ctx, MIR_T_U8,
                                        offsetof(ErrorSaveContext,
                                                 error_occurred),
                                        r_tmp3, 0, 1)));

        /* If error_occurred != 0, jump to soft error path */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BNE,
                         MIR_new_label_op(ctx, soft_error_label),
                         MIR_new_reg_op(ctx, r_tmp3),
                         MIR_new_int_op(ctx, 0)));

        /* No error: *op->resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));

        /* *op->resnull = fcinfo_in->isnull */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci_safe,
                                        0, 1)));
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));

        /* Jump past soft error path */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP,
                         MIR_new_label_op(ctx, done_safe_label)));

        /* Soft error path: *op->resnull = true, *op->resvalue = 0 */
        MIR_append_insn(ctx, func_item, soft_error_label);
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 1)));
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));

        MIR_append_insn(ctx, func_item, done_safe_label);
      }

      MIR_append_insn(ctx, func_item, skip_null_safe);
      break;
    }
#endif /* HAVE_EEOP_IOCOERCE_SAFE */

    /*
     * ---- SQLVALUEFUNCTION (inlined) ----
     * Resolve svf->op at compile time, emit direct call to getter.
     * Time functions: direct call with typmod immediate.
     * Identity functions: fall back to ExecEvalSQLValueFunction.
     */
    case EEOP_SQLVALUEFUNCTION: {
      SQLValueFunction *svf = op->d.sqlvaluefunction.svf;

      switch (svf->op) {
      case SVFOP_CURRENT_DATE: {
        /* *op->resnull = false */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));

        /* r_tmp2 = GetSQLCurrentDate() */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 3,
                              MIR_new_ref_op(ctx, proto_svf_i32_void),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_tmp2)));

        /* Zero-extend I32 to I64 (DateADT is int32) */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_UEXT32, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_reg_op(ctx, r_tmp2)));

        /* *op->resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
        break;
      }

      case SVFOP_CURRENT_TIME:
      case SVFOP_CURRENT_TIME_N:
      case SVFOP_CURRENT_TIMESTAMP:
      case SVFOP_CURRENT_TIMESTAMP_N:
      case SVFOP_LOCALTIME:
      case SVFOP_LOCALTIME_N:
      case SVFOP_LOCALTIMESTAMP:
      case SVFOP_LOCALTIMESTAMP_N: {
        /* *op->resnull = false */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));

        /* r_tmp3 = typmod (I64 reg, proto narrows to I32 at call boundary) */
        MIR_reg_t r_typmod = mir_new_reg(ctx, f, MIR_T_I64, "tmod");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_typmod),
                         MIR_new_int_op(ctx, svf->typmod)));

        /* r_tmp2 = fn(typmod) */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4,
                              MIR_new_ref_op(ctx, proto_svf_i64_i32),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_typmod)));

        /* *op->resvalue = r_tmp2 */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp2)));
        break;
      }

      default:
        /* Identity functions (CURRENT_USER etc): fall back to C helper */
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4,
                              MIR_new_ref_op(ctx, proto_agg_helper),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1)));
        break;
      }
      break;
    }

    /*
     * ---- ARRAYEXPR (inlined for fixed-width by-value) ----
     * For simple 1-D arrays of fixed-width by-value elements with no nulls,
     * inline: palloc(known_size) + header init + element copy.
     * All other cases fall back to ExecEvalArrayExpr.
     */
    case EEOP_ARRAYEXPR: {
      int nelems = op->d.arrayexpr.nelems;
      int16 elemlength = op->d.arrayexpr.elemlength;
      bool elembyval = op->d.arrayexpr.elembyval;
      bool multidims = op->d.arrayexpr.multidims;

      if (!multidims && elemlength > 0 && elembyval && nelems > 0 &&
          nelems <= 64) {
        /*
         * Fast path: fixed-width by-value, 1-D, small array.
         * Pre-compute total size at compile time.
         */
        int dataoff = ARR_OVERHEAD_NONULLS(1);
        int data_size = nelems * elemlength;
        int total_size = dataoff + data_size;

        MIR_label_t has_null_label = MIR_new_label(ctx);
        MIR_label_t arr_done_label = MIR_new_label(ctx);

        /* Check for any nulls in elemnulls[] array */
        MIR_reg_t r_enulls = mir_new_reg(ctx, f, MIR_T_I64, "enulls");
        MIR_STEP_LOAD(r_enulls, opno, d.arrayexpr.elemnulls);

        for (int k = 0; k < nelems; k++) {
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_mem_op(ctx, MIR_T_U8, k * sizeof(bool),
                                          r_enulls, 0, 1)));
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_BNE,
                           MIR_new_label_op(ctx, has_null_label),
                           MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_int_op(ctx, 0)));
        }

        /* No nulls: palloc(total_size) */
        MIR_reg_t r_arr_sz = mir_new_reg(ctx, f, MIR_T_I64, "arsz");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_arr_sz),
                         MIR_new_int_op(ctx, total_size)));

        MIR_reg_t r_arr = mir_new_reg(ctx, f, MIR_T_I64, "arr");
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4,
                              MIR_new_ref_op(ctx, proto_palloc),
                              MIR_new_ref_op(ctx, import_palloc),
                              MIR_new_reg_op(ctx, r_arr),
                              MIR_new_reg_op(ctx, r_arr_sz)));

        /* SET_VARSIZE(result, total_size): store (total_size << 2) in first 4 bytes */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I32, 0, r_arr, 0, 1),
                         MIR_new_int_op(ctx, total_size << 2)));

        /* ndim = 1 */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I32,
                                        offsetof(ArrayType, ndim),
                                        r_arr, 0, 1),
                         MIR_new_int_op(ctx, 1)));

        /* dataoffset = 0 (no null bitmap) */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I32,
                                        offsetof(ArrayType, dataoffset),
                                        r_arr, 0, 1),
                         MIR_new_int_op(ctx, 0)));

        /* elemtype */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I32,
                                        offsetof(ArrayType, elemtype),
                                        r_arr, 0, 1),
                         MIR_new_int_op(ctx, op->d.arrayexpr.elemtype)));

        /* dims[0] = nelems (offset = sizeof(ArrayType)) */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I32,
                                        (int64_t)sizeof(ArrayType),
                                        r_arr, 0, 1),
                         MIR_new_int_op(ctx, nelems)));

        /* lbound[0] = 1 (offset = sizeof(ArrayType) + sizeof(int)) */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I32,
                                        (int64_t)(sizeof(ArrayType) +
                                                  sizeof(int)),
                                        r_arr, 0, 1),
                         MIR_new_int_op(ctx, 1)));

        /* Copy elements from elemvalues[] into data area */
        MIR_reg_t r_evals = mir_new_reg(ctx, f, MIR_T_I64, "evals");
        MIR_STEP_LOAD(r_evals, opno, d.arrayexpr.elemvalues);

        for (int k = 0; k < nelems; k++) {
          /* Load Datum from elemvalues[k] */
          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                           MIR_new_mem_op(ctx, MIR_T_I64,
                                          k * (int64_t)sizeof(Datum),
                                          r_evals, 0, 1)));

          /* Store into data area with correct element size */
          MIR_type_t store_type;
          if (elemlength == (int16)sizeof(Datum))
            store_type = MIR_T_I64;
          else if (elemlength == 4)
            store_type = MIR_T_I32;
          else if (elemlength == 2)
            store_type = MIR_T_U16;
          else
            store_type = MIR_T_U8;

          MIR_append_insn(
              ctx, func_item,
              MIR_new_insn(ctx, MIR_MOV,
                           MIR_new_mem_op(ctx, store_type,
                                          dataoff + k * elemlength,
                                          r_arr, 0, 1),
                           MIR_new_reg_op(ctx, r_tmp1)));
        }

        /* *op->resvalue = PointerGetDatum(result) */
        MIR_STEP_LOAD(r_tmp1, opno, resvalue);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_arr)));

        /* *op->resnull = false */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 0)));

        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP,
                         MIR_new_label_op(ctx, arr_done_label)));

        /* Fallback: has nulls or complex case -> call ExecEvalArrayExpr */
        MIR_append_insn(ctx, func_item, has_null_label);
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4,
                              MIR_new_ref_op(ctx, proto_agg_helper),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1)));

        MIR_append_insn(ctx, func_item, arr_done_label);
      } else {
        /* Fallback to C function for varlena/multidims/complex cases */
        MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4,
                              MIR_new_ref_op(ctx, proto_agg_helper),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_state),
                              MIR_new_reg_op(ctx, r_tmp1)));
      }
      break;
    }

    /*
     * ---- FIELDSELECT (inlined null check) ----
     * If input is null, result is null (already set, skip).
     * Otherwise call ExecEvalFieldSelect(state, op, econtext).
     */
    case EEOP_FIELDSELECT: {
      MIR_label_t fsel_null_label = MIR_new_label(ctx);

      /* If *op->resnull, skip (input null -> output null) */
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE,
                       MIR_new_label_op(ctx, fsel_null_label),
                       MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_int_op(ctx, 0)));

      /* Non-null: call ExecEvalFieldSelect(state, op, econtext) */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 5,
                            MIR_new_ref_op(ctx, proto_3arg_void),
                            MIR_new_ref_op(ctx, step_fn_imports[opno]),
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));

      MIR_append_insn(ctx, func_item, fsel_null_label);
      break;
    }

    /*
     * ---- ROWCOMPARE_STEP ----
     * Call comparison fn via fcinfo; jump to jumpnull on NULL
     * result, jumpdone on non-zero result.
     */
    case EEOP_ROWCOMPARE_STEP: {
      FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
      int jnull = op->d.rowcompare_step.jumpnull;
      int jdone = op->d.rowcompare_step.jumpdone;

      /*
       * Load fcinfo from step data.  The old code used MIR_STEP_ADDR_RAW
       * with fcinfo field addresses, which in shared mode wrongly resolved
       * to &steps[opno] instead of the actual fcinfo address.
       */
      MIR_reg_t r_fci = mir_new_reg(ctx, f, MIR_T_I64, "rcfci");
      MIR_STEP_LOAD(r_fci, opno, d.rowcompare_step.fcinfo_data);

      int64_t arg0_null_off =
          (int64_t)((char *)&fcinfo->args[0].isnull - (char *)fcinfo);
      int64_t arg1_null_off =
          (int64_t)((char *)&fcinfo->args[1].isnull - (char *)fcinfo);
      int64_t isnull_off = offsetof(FunctionCallInfoBaseData, isnull);

      if (op->d.rowcompare_step.finfo->fn_strict) {
        /* Check args[0].isnull || args[1].isnull */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                         MIR_new_mem_op(ctx, MIR_T_U8, arg0_null_off, r_fci, 0,
                                        1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp3),
                         MIR_new_mem_op(ctx, MIR_T_U8, arg1_null_off, r_fci, 0,
                                        1)));
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_OR, MIR_new_reg_op(ctx, r_tmp2),
                                     MIR_new_reg_op(ctx, r_tmp2),
                                     MIR_new_reg_op(ctx, r_tmp3)));

        MIR_label_t not_null = MIR_new_label(ctx);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_null),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

        /* Null path: *resnull = true, jump to jumpnull */
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP,
                         MIR_new_label_op(ctx, step_labels[jnull])));

        MIR_append_insn(ctx, func_item, not_null);
      }

      /* fcinfo->isnull = false */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci, 0, 1),
                       MIR_new_int_op(ctx, 0)));

      /* Call fn_addr(fcinfo) -> Datum */
      if (mir_shared_code_mode) {
        MIR_reg_t r_fn = mir_new_reg(ctx, f, MIR_T_I64, "fn_rt");
        int64_t off = (int64_t)opno * (int64_t)sizeof(ExprEvalStep) +
                      offsetof(ExprEvalStep, d.rowcompare_step.fn_addr);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_fn),
                         MIR_new_mem_op(ctx, MIR_T_I64, off, r_steps, 0, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_reg_op(ctx, r_fn),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fci)));
      } else {
        MIR_append_insn(
            ctx, func_item,
            MIR_new_call_insn(ctx, 4, MIR_new_ref_op(ctx, proto_v1func),
                              MIR_new_ref_op(ctx, step_fn_imports[opno]),
                              MIR_new_reg_op(ctx, r_tmp2),
                              MIR_new_reg_op(ctx, r_fci)));
      }

      /* *op->resvalue = r_tmp2 */
      MIR_STEP_LOAD(r_tmp1, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                       MIR_new_reg_op(ctx, r_tmp2)));

      /* If fcinfo->isnull, *resnull = true, jump to jumpnull */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp3),
                       MIR_new_mem_op(ctx, MIR_T_U8, isnull_off, r_fci, 0,
                                      1)));
      {
        MIR_label_t not_null2 = MIR_new_label(ctx);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, not_null2),
                         MIR_new_reg_op(ctx, r_tmp3), MIR_new_int_op(ctx, 0)));
        MIR_STEP_LOAD(r_tmp1, opno, resnull);
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                         MIR_new_int_op(ctx, 1)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP,
                         MIR_new_label_op(ctx, step_labels[jnull])));
        MIR_append_insn(ctx, func_item, not_null2);
      }

      /* *resnull = false */
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                       MIR_new_int_op(ctx, 0)));

      /* If int32(r_tmp2) != 0, jump to jumpdone */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BNE, MIR_new_label_op(ctx, step_labels[jdone]),
                       MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));
      break;
    }

    /*
     * ---- ROWCOMPARE_FINAL ----
     * Read int32 result from *op->resvalue, apply comparison,
     * store boolean result.
     */
    case EEOP_ROWCOMPARE_FINAL: {
      CompareType cmptype = op->d.rowcompare_final.cmptype;

      /* Load int32 cmpresult from *op->resvalue */
      MIR_STEP_LOAD(r_tmp1, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_mem_op(ctx, MIR_T_I32, 0, r_tmp1, 0, 1)));

      /* result = (cmpresult <cmp> 0) ? 1 : 0 */
      {
        MIR_label_t true_label = MIR_new_label(ctx);
        MIR_label_t end_label = MIR_new_label(ctx);

        MIR_insn_code_t cmp_code;
        switch (cmptype) {
        case COMPARE_LT:
          cmp_code = MIR_BLT;
          break;
        case COMPARE_LE:
          cmp_code = MIR_BLE;
          break;
        case COMPARE_GE:
          cmp_code = MIR_BGE;
          break;
        case COMPARE_GT:
          cmp_code = MIR_BGT;
          break;
        default:
          cmp_code = MIR_BLT;
          break;
        }
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, cmp_code, MIR_new_label_op(ctx, true_label),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

        /* false path: result = 0 */
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp3),
                                     MIR_new_int_op(ctx, 0)));
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_JMP, MIR_new_label_op(ctx, end_label)));

        /* true path: result = 1 */
        MIR_append_insn(ctx, func_item, true_label);
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp3),
                                     MIR_new_int_op(ctx, 1)));

        MIR_append_insn(ctx, func_item, end_label);

        /* *op->resvalue = result */
        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_MOV,
                         MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                         MIR_new_reg_op(ctx, r_tmp3)));
      }

      /* *op->resnull = false */
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
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
    case EEOP_JSONEXPR_PATH: {
      JsonExprState *jsestate = op->d.jsonexpr.jsestate;
      int targets[4];
      int ntargets = 0;

      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(
              ctx, 6, MIR_new_ref_op(ctx, proto_fallback),
              MIR_new_ref_op(ctx, step_fn_imports[opno]),
              MIR_new_reg_op(ctx, r_tmp2), MIR_new_reg_op(ctx, r_state),
              MIR_new_reg_op(ctx, r_tmp1), MIR_new_reg_op(ctx, r_econtext)));

      /*
       * ExecEvalJsonExprPath returns int (32-bit).  proto_fallback
       * declares I64 return, so upper 32 bits of r_tmp2 may be
       * garbage on ARM64.  Sign-extend to 64 bits for correct
       * comparison against target step indices below.
       */
      MIR_append_insn(ctx, func_item,
          MIR_new_insn(ctx, MIR_EXT32,
                       MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_reg_op(ctx, r_tmp2)));

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

      for (int t = 0; t < ntargets; t++) {
        if (targets[t] >= 0 && targets[t] < steps_len) {
          MIR_append_insn(
              ctx, func_item,
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
    case EEOP_RETURNINGEXPR: {
      MIR_label_t cont_label = MIR_new_label(ctx);

      /* Load state->flags (int32) */
      MIR_append_insn(ctx, func_item,
                      MIR_new_insn(ctx, MIR_MOV, MIR_new_reg_op(ctx, r_tmp1),
                                   MIR_new_mem_op(ctx, MIR_T_I32,
                                                  offsetof(ExprState, flags),
                                                  r_state, 0, 1)));

      /* Test: flags & nullflag */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_AND, MIR_new_reg_op(ctx, r_tmp2),
                       MIR_new_reg_op(ctx, r_tmp1),
                       MIR_new_int_op(ctx, op->d.returningexpr.nullflag)));

      /* If zero (flag not set), continue */
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_BEQ, MIR_new_label_op(ctx, cont_label),
                       MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

      /* Flag set: *resvalue = 0, *resnull = true, jump */
      MIR_STEP_LOAD(r_tmp1, opno, resvalue);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_I64, 0, r_tmp1, 0, 1),
                       MIR_new_int_op(ctx, 0)));
      MIR_STEP_LOAD(r_tmp1, opno, resnull);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_MOV,
                       MIR_new_mem_op(ctx, MIR_T_U8, 0, r_tmp1, 0, 1),
                       MIR_new_int_op(ctx, 1)));
      MIR_append_insn(
          ctx, func_item,
          MIR_new_insn(ctx, MIR_JMP,
                       MIR_new_label_op(
                           ctx, step_labels[op->d.returningexpr.jumpdone])));

      MIR_append_insn(ctx, func_item, cont_label);
      break;
    }
#endif /* HAVE_EEOP_RETURNINGEXPR */

    /*
     * ---- DEFAULT: fallback ----
     */
    default: {
      /* Call pg_jitter_fallback_step(state, op, econtext) -> int */
      MIR_STEP_ADDR_RAW(r_tmp1, opno, 0, (uint64_t)op);
      MIR_append_insn(
          ctx, func_item,
          MIR_new_call_insn(ctx, 6, MIR_new_ref_op(ctx, proto_fallback),
                            MIR_new_ref_op(ctx, import_fallback),
                            MIR_new_reg_op(ctx, r_tmp2), /* return value */
                            MIR_new_reg_op(ctx, r_state),
                            MIR_new_reg_op(ctx, r_tmp1),
                            MIR_new_reg_op(ctx, r_econtext)));

      /*
       * Generic jump dispatch: fallback_step returns >= 0 when a
       * jump is needed (the return value is the target step index).
       * Emit comparisons for all reachable step labels.
       */
      {
        MIR_label_t no_jump = MIR_new_label(ctx);

        MIR_append_insn(
            ctx, func_item,
            MIR_new_insn(ctx, MIR_BLT, MIR_new_label_op(ctx, no_jump),
                         MIR_new_reg_op(ctx, r_tmp2), MIR_new_int_op(ctx, 0)));

        for (int t = 0; t < steps_len; t++) {
          MIR_append_insn(ctx, func_item,
                          MIR_new_insn(ctx, MIR_BEQ,
                                       MIR_new_label_op(ctx, step_labels[t]),
                                       MIR_new_reg_op(ctx, r_tmp2),
                                       MIR_new_int_op(ctx, t)));
        }

        MIR_append_insn(ctx, func_item, no_jump);
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
    MIR_load_external(ctx, "fallback_step",
                      mir_extern_addr((void *)pg_jitter_fallback_step));
    MIR_load_external(ctx, "getsomeattrs",
                      mir_extern_addr((void *)pg_jitter_compiled_deform_dispatch));

    MIR_load_external(
        ctx, "make_ro",
        mir_extern_addr((void *)MakeExpandedObjectReadOnlyInternal));
    /* Inline error handlers */
    MIR_load_external(ctx, "err_i4ov",
                      mir_extern_addr((void *)jit_error_int4_overflow));
    MIR_load_external(ctx, "err_i8ov",
                      mir_extern_addr((void *)jit_error_int8_overflow));
    MIR_load_external(ctx, "err_divz",
                      mir_extern_addr((void *)jit_error_division_by_zero));
    /* SCALARARRAYOP helpers */
    MIR_load_external(ctx, "detoast_arr",
                      mir_extern_addr((void *)pg_detoast_datum));
    MIR_load_external(ctx, "saop_loop",
                      mir_extern_addr((void *)pg_jitter_scalararrayop_loop));
    /* GROUPING_FUNC helper */
    MIR_load_external(ctx, "bms_is_member",
                      mir_extern_addr((void *)bms_is_member));
    /* ARRAYEXPR inline palloc helper */
    MIR_load_external(ctx, "palloc_fn",
                      mir_extern_addr((void *)palloc));
    MIR_load_external(ctx, "nextval_fn",
                      mir_extern_addr((void *)nextval_internal));
    MIR_load_external(ctx, "hft_fn",
                      mir_extern_addr((void *)heap_form_tuple));
    MIR_load_external(ctx, "htgd_fn",
                      mir_extern_addr((void *)HeapTupleHeaderGetDatum));
    /* Compiled LIKE match helper */
    MIR_load_external(ctx, "simd_like_match_compiled",
                      mir_extern_addr((void *)simd_like_match_compiled));
#ifdef PG_JITTER_HAVE_VECTORSCAN
    /* Vectorscan LIKE/regex match helper */
    MIR_load_external(ctx, "vs_match_text",
                      mir_extern_addr((void *)pg_jitter_vs_match_text));
#endif
    /* Inline deform helpers */
#ifdef HAVE_EEOP_AGG_PRESORTED_DISTINCT
    /* Presorted distinct helpers */
    MIR_load_external(
        ctx, "pdist_single",
        mir_extern_addr((void *)ExecEvalPreOrderedDistinctSingle));
    MIR_load_external(ctx, "pdist_multi",
                      mir_extern_addr((void *)ExecEvalPreOrderedDistinctMulti));
#endif

    /* CASE binary search helpers */
    for (int bi = 0; bi < num_case_bsearch; bi++) {
      CaseBSearchInfo *cbi = &case_bsearch[bi];
      char bs_name[32];
      snprintf(bs_name, sizeof(bs_name), "bsearch_%d", cbi->start_opno + 1);

      void *helper_fn = pg_jitter_select_bsearch_helper(cbi);

      MIR_load_external(ctx, bs_name, mir_extern_addr(helper_fn));
    }

    for (int i = 0; i < steps_len; i++) {
      if (step_fn_imports[i]) {
        ExprEvalOp op = ExecEvalStepOp(state, &steps[i]);
        void *addr;
        char name[32];

        if (op == EEOP_HASHED_SCALARARRAYOP) {
          snprintf(name, sizeof(name), "saop_%d", i);
          MIR_load_external(
              ctx, name, mir_extern_addr((void *)ExecEvalHashedScalarArrayOp));
          continue;
        } else if (op == EEOP_AGG_STRICT_DESERIALIZE ||
                   op == EEOP_AGG_DESERIALIZE) {
          FunctionCallInfo fcinfo = steps[i].d.agg_deserialize.fcinfo_data;
          snprintf(name, sizeof(name), "dser_%d", i);
          MIR_load_external(ctx, name,
                            mir_extern_addr((void *)fcinfo->flinfo->fn_addr));
          continue;
        } else if (op == EEOP_INNER_SYSVAR || op == EEOP_OUTER_SYSVAR ||
                   op == EEOP_SCAN_SYSVAR
#ifdef HAVE_EEOP_OLD_NEW
                   || op == EEOP_OLD_SYSVAR || op == EEOP_NEW_SYSVAR
#endif
        ) {
          snprintf(name, sizeof(name), "sysvar_%d", i);
          MIR_load_external(ctx, name, mir_extern_addr((void *)ExecEvalSysVar));
          continue;
        } else if (op == EEOP_SBSREF_SUBSCRIPTS) {
          snprintf(name, sizeof(name), "sbss_%d", i);
          MIR_load_external(
              ctx, name,
              mir_extern_addr(
                  (void *)steps[i].d.sbsref_subscript.subscriptfunc));
          continue;
        } else if (op == EEOP_SBSREF_OLD || op == EEOP_SBSREF_ASSIGN ||
                   op == EEOP_SBSREF_FETCH) {
          snprintf(name, sizeof(name), "sbsf_%d", i);
          MIR_load_external(
              ctx, name,
              mir_extern_addr((void *)steps[i].d.sbsref.subscriptfunc));
          continue;
        } else if (op == EEOP_IOCOERCE) {
          FunctionCallInfo fcinfo_out = steps[i].d.iocoerce.fcinfo_data_out;
          FunctionCallInfo fcinfo_in = steps[i].d.iocoerce.fcinfo_data_in;
          snprintf(name, sizeof(name), "ioc_out_%d", i);
          MIR_load_external(
              ctx, name, mir_extern_addr((void *)fcinfo_out->flinfo->fn_addr));
          snprintf(name, sizeof(name), "ioc_in_%d", i);
          MIR_load_external(
              ctx, name, mir_extern_addr((void *)fcinfo_in->flinfo->fn_addr));
          continue;
        }
#ifdef HAVE_EEOP_IOCOERCE_SAFE
        else if (op == EEOP_IOCOERCE_SAFE) {
          FunctionCallInfo fcinfo_out = steps[i].d.iocoerce.fcinfo_data_out;
          FunctionCallInfo fcinfo_in = steps[i].d.iocoerce.fcinfo_data_in;
          snprintf(name, sizeof(name), "ioc_safe_out_%d", i);
          MIR_load_external(
              ctx, name, mir_extern_addr((void *)fcinfo_out->flinfo->fn_addr));
          snprintf(name, sizeof(name), "ioc_safe_in_%d", i);
          MIR_load_external(
              ctx, name, mir_extern_addr((void *)fcinfo_in->flinfo->fn_addr));
          continue;
        }
#endif
        else if (op == EEOP_SQLVALUEFUNCTION) {
          SQLValueFunction *svf = steps[i].d.sqlvaluefunction.svf;
          void *fn = NULL;
          switch (svf->op) {
          case SVFOP_CURRENT_DATE:
            fn = (void *)GetSQLCurrentDate;
            break;
          case SVFOP_CURRENT_TIME:
          case SVFOP_CURRENT_TIME_N:
            fn = (void *)GetSQLCurrentTime;
            break;
          case SVFOP_CURRENT_TIMESTAMP:
          case SVFOP_CURRENT_TIMESTAMP_N:
            fn = (void *)GetSQLCurrentTimestamp;
            break;
          case SVFOP_LOCALTIME:
          case SVFOP_LOCALTIME_N:
            fn = (void *)GetSQLLocalTime;
            break;
          case SVFOP_LOCALTIMESTAMP:
          case SVFOP_LOCALTIMESTAMP_N:
            fn = (void *)GetSQLLocalTimestamp;
            break;
          default:
            fn = (void *)ExecEvalSQLValueFunction;
            break;
          }
          if (svf->op <= SVFOP_LOCALTIMESTAMP_N)
            snprintf(name, sizeof(name), "svf_%d", i);
          else
            snprintf(name, sizeof(name), "dc_%d", i);
          MIR_load_external(ctx, name, mir_extern_addr(fn));
          continue;
        }
        else if (op == EEOP_ROWCOMPARE_STEP) {
          snprintf(name, sizeof(name), "rcmp_%d", i);
          MIR_load_external(
              ctx, name,
              mir_extern_addr((void *)steps[i].d.rowcompare_step.fn_addr));
          continue;
        }
#ifdef HAVE_EEOP_JSONEXPR
        else if (op == EEOP_JSONEXPR_PATH) {
          snprintf(name, sizeof(name), "jpath_%d", i);
          MIR_load_external(ctx, name,
                            mir_extern_addr((void *)ExecEvalJsonExprPath));
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
                 op == EEOP_ARRAYCOERCE || op == EEOP_FIELDSELECT ||
                 op == EEOP_FIELDSTORE_DEFORM || op == EEOP_FIELDSTORE_FORM ||
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
                 op == EEOP_SUBPLAN || op == EEOP_WHOLEROW ||
                 op == EEOP_AGG_ORDERED_TRANS_DATUM ||
                 op == EEOP_AGG_ORDERED_TRANS_TUPLE ||
                 /* SCALARARRAYOP uses dedicated inline with separate imports */
                 op == EEOP_CURRENTOFEXPR || op == EEOP_NEXTVALUEEXPR ||
                 op == EEOP_ARRAYEXPR || op == EEOP_ROW || op == EEOP_MINMAX ||
                 op == EEOP_DOMAIN_NOTNULL || op == EEOP_DOMAIN_CHECK ||
                 op == EEOP_XMLEXPR ||
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
                 op == EEOP_IS_JSON ||
#endif
#ifdef HAVE_EEOP_JSONEXPR
                 op == EEOP_JSONEXPR_COERCION_FINISH ||
#endif
                 op == EEOP_GROUPING_FUNC || op == EEOP_PARAM_CALLBACK ||
                 op == EEOP_PARAM_EXEC || op == EEOP_PARAM_EXTERN) {
          void *fn = NULL;
          switch (op) {
          /* 3-arg void calls */
          case EEOP_FUNCEXPR_FUSAGE:
            fn = (void *)ExecEvalFuncExprFusage;
            break;
          case EEOP_FUNCEXPR_STRICT_FUSAGE:
            fn = (void *)ExecEvalFuncExprStrictFusage;
            break;
          case EEOP_NULLTEST_ROWISNULL:
            fn = (void *)ExecEvalRowNull;
            break;
          case EEOP_NULLTEST_ROWISNOTNULL:
            fn = (void *)ExecEvalRowNotNull;
            break;
#ifdef HAVE_EEOP_PARAM_SET
          case EEOP_PARAM_SET:
            fn = (void *)ExecEvalParamSet;
            break;
#endif
          case EEOP_ARRAYCOERCE:
            fn = (void *)ExecEvalArrayCoerce;
            break;
          case EEOP_FIELDSELECT:
            fn = (void *)ExecEvalFieldSelect;
            break;
          case EEOP_FIELDSTORE_DEFORM:
            fn = (void *)ExecEvalFieldStoreDeForm;
            break;
          case EEOP_FIELDSTORE_FORM:
            fn = (void *)ExecEvalFieldStoreForm;
            break;
          case EEOP_CONVERT_ROWTYPE:
            fn = (void *)ExecEvalConvertRowtype;
            break;
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
          case EEOP_JSON_CONSTRUCTOR:
            fn = (void *)ExecEvalJsonConstructor;
            break;
#endif
#ifdef HAVE_EEOP_JSONEXPR
          case EEOP_JSONEXPR_COERCION:
            fn = (void *)ExecEvalJsonCoercion;
            break;
#endif
#ifdef HAVE_EEOP_MERGE_SUPPORT_FUNC
          case EEOP_MERGE_SUPPORT_FUNC:
            fn = (void *)ExecEvalMergeSupportFunc;
            break;
#endif
          case EEOP_SUBPLAN:
            fn = (void *)ExecEvalSubPlan;
            break;
          case EEOP_WHOLEROW:
            fn = (void *)ExecEvalWholeRowVar;
            break;
          case EEOP_AGG_ORDERED_TRANS_DATUM:
            fn = (void *)ExecEvalAggOrderedTransDatum;
            break;
          case EEOP_AGG_ORDERED_TRANS_TUPLE:
            fn = (void *)ExecEvalAggOrderedTransTuple;
            break;
            /* 2-arg void calls */
          case EEOP_CURRENTOFEXPR:
            fn = (void *)ExecEvalCurrentOfExpr;
            break;
          case EEOP_NEXTVALUEEXPR:
            fn = (void *)ExecEvalNextValueExpr;
            break;
          case EEOP_ARRAYEXPR:
            fn = (void *)ExecEvalArrayExpr;
            break;
          case EEOP_ROW:
            fn = (void *)ExecEvalRow;
            break;
          case EEOP_MINMAX:
            fn = (void *)ExecEvalMinMax;
            break;
          case EEOP_DOMAIN_NOTNULL:
            fn = (void *)ExecEvalConstraintNotNull;
            break;
          case EEOP_DOMAIN_CHECK:
            fn = (void *)ExecEvalConstraintCheck;
            break;
          case EEOP_XMLEXPR:
            fn = (void *)ExecEvalXmlExpr;
            break;
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
          case EEOP_IS_JSON:
            fn = (void *)ExecEvalJsonIsPredicate;
            break;
#endif
#ifdef HAVE_EEOP_JSONEXPR
          case EEOP_JSONEXPR_COERCION_FINISH:
            fn = (void *)ExecEvalJsonCoercionFinish;
            break;
#endif
          case EEOP_GROUPING_FUNC:
            fn = (void *)ExecEvalGroupingFunc;
            break;
          /* PARAM_CALLBACK: indirect fn pointer */
          case EEOP_PARAM_CALLBACK:
            fn = (void *)steps[i].d.cparam.paramfunc;
            break;
          case EEOP_PARAM_EXEC:
            fn = (void *)ExecEvalParamExec;
            break;
          case EEOP_PARAM_EXTERN:
            fn = (void *)ExecEvalParamExtern;
            break;
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
          addr = (void *)steps[i].d.hashdatum.fn_addr;
#endif
        else
          addr = (void *)steps[i].d.func.fn_addr;

        snprintf(name, sizeof(name), "fn_%d", i);
        MIR_load_external(ctx, name, mir_extern_addr(addr));
      }
      if (step_direct_imports[i]) {
        ExprEvalOp opc = ExecEvalStepOp(state, &steps[i]);
        char name[32];

#ifdef HAVE_EEOP_HASHDATUM
        if (opc == EEOP_HASHDATUM_FIRST || opc == EEOP_HASHDATUM_FIRST_STRICT ||
            opc == EEOP_HASHDATUM_NEXT32 ||
            opc == EEOP_HASHDATUM_NEXT32_STRICT) {
          const JitDirectFn *dfn =
              jit_find_direct_fn(steps[i].d.hashdatum.fn_addr);
          snprintf(name, sizeof(name), "dhfn_%d", i);
          if (dfn && dfn->jit_fn)
            MIR_load_external(ctx, name, mir_extern_addr(dfn->jit_fn));
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
          else if (!mir_shared_code_mode && dfn && dfn->jit_fn_name) {
            void *fn = mir_find_precompiled_fn(dfn->jit_fn_name);
            if (fn)
              MIR_load_external(ctx, name, fn);
          }
#endif
        } else
#endif /* HAVE_EEOP_HASHDATUM */
          if (opc >= EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL &&
              opc <= EEOP_AGG_PLAIN_TRANS_BYREF) {
            /* Resolve agg_trans helper address */
            void *helper_fn = NULL;
            switch (opc) {
            case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
              helper_fn = (void *)pg_jitter_agg_trans_init_strict_byval;
              break;
            case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
              helper_fn = (void *)pg_jitter_agg_trans_strict_byval;
              break;
            case EEOP_AGG_PLAIN_TRANS_BYVAL:
              helper_fn = (void *)pg_jitter_agg_trans_byval;
              break;
            case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
              helper_fn = (void *)pg_jitter_agg_trans_init_strict_byref;
              break;
            case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
              helper_fn = (void *)pg_jitter_agg_trans_strict_byref;
              break;
            case EEOP_AGG_PLAIN_TRANS_BYREF:
              helper_fn = (void *)pg_jitter_agg_trans_byref;
              break;
            default:
              break;
            }
            snprintf(name, sizeof(name), "ah_%d", i);
            if (helper_fn)
              MIR_load_external(ctx, name, mir_extern_addr(helper_fn));
          } else {
            const JitDirectFn *dfn =
                jit_find_direct_fn(steps[i].d.func.fn_addr);
            snprintf(name, sizeof(name), "dfn_%d", i);
            if (dfn && dfn->jit_fn)
              MIR_load_external(ctx, name, mir_extern_addr(dfn->jit_fn));
#ifdef PG_JITTER_HAVE_MIR_PRECOMPILED
            else if (!mir_shared_code_mode && dfn && dfn->jit_fn_name) {
              void *fn = mir_find_precompiled_fn(dfn->jit_fn_name);
              if (fn)
                MIR_load_external(ctx, name, fn);
            }
#endif
          }
      }
    }

#ifdef PG_JITTER_HAVE_SIMDJSON
    MIR_load_external(ctx, "sj_is_json",
                      mir_extern_addr((void *)pg_jitter_sj_is_json_datum));
    MIR_load_external(ctx, "sj_json_in",
                      mir_extern_addr((void *)pg_jitter_sj_json_in));
    MIR_load_external(ctx, "sj_jsonb_in",
                      mir_extern_addr((void *)pg_jitter_sj_jsonb_in));
#endif

    MIR_link(ctx, MIR_set_lazy_gen_interface, NULL);

    code = MIR_gen(ctx, func_item);
    if (!code) {
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
    if (mir_shared_code_mode && mir_n_sentinels > 0) {
      void *mcode = f->machine_code;
      Size mcode_size = f->machine_code_len;
      int npatched;

#if defined(__APPLE__) && defined(__aarch64__)
      pthread_jit_write_protect_np(0); /* write mode */
#else
      /*
       * MIR sets code pages to PROT_READ|PROT_EXEC after MIR_gen().
       * We need write access for sentinel patching.
       */
      {
        long pgsz = sysconf(_SC_PAGESIZE);
        uintptr_t page_start = (uintptr_t)mcode & ~(pgsz - 1);
        Size page_len = ((uintptr_t)mcode + mcode_size) - page_start;

        page_len = (page_len + pgsz - 1) & ~(pgsz - 1);
        if (mprotect((void *)page_start, page_len, PROT_READ | PROT_WRITE) != 0)
          elog(WARNING,
               "pg_jitter[mir]: mprotect(RW) for sentinel patching failed: %m");
      }
#endif

      npatched = mir_patch_sentinels(mcode, mcode_size);

      elog(DEBUG1,
           "pg_jitter[mir]: patched %d/%d sentinel addresses "
           "in %zu bytes of machine code (thunk=%p, mcode=%p)",
           npatched, mir_n_sentinels, mcode_size, code, mcode);

#if defined(__APPLE__) && defined(__aarch64__)
      sys_icache_invalidate(mcode, mcode_size);
      pthread_jit_write_protect_np(1); /* exec mode */
#elif defined(__aarch64__) || defined(_M_ARM64)
      /* ARM64 has non-coherent I/D caches: flush before executing */
      __builtin___clear_cache((char *)mcode, (char *)mcode + mcode_size);
      {
        long pgsz = sysconf(_SC_PAGESIZE);
        uintptr_t page_start = (uintptr_t)mcode & ~(pgsz - 1);
        Size page_len = ((uintptr_t)mcode + mcode_size) - page_start;

        page_len = (page_len + pgsz - 1) & ~(pgsz - 1);
        if (mprotect((void *)page_start, page_len, PROT_READ | PROT_EXEC) != 0)
          elog(WARNING,
               "pg_jitter[mir]: mprotect(RX) for sentinel patching failed: %m");
      }
#else
      /* x86_64: coherent I/D caches, no flush needed */
      {
        long pgsz = sysconf(_SC_PAGESIZE);
        uintptr_t page_start = (uintptr_t)mcode & ~(pgsz - 1);
        Size page_len = ((uintptr_t)mcode + mcode_size) - page_start;

        page_len = (page_len + pgsz - 1) & ~(pgsz - 1);
        if (mprotect((void *)page_start, page_len, PROT_READ | PROT_EXEC) != 0)
          elog(WARNING,
               "pg_jitter[mir]: mprotect(RX) for sentinel patching failed: %m");
      }
#endif
    }

#ifdef _WIN64
    /* Register Windows x64 SEH unwind metadata for longjmp safety */
    {
      void *mcode = f->machine_code;
      Size mcode_size = f->machine_code_len;

      pg_jitter_win64_register_unwind(mcode, mcode_size);

      /* Track for deregistration when MIR context is freed */
      if (mir_current_state) {
        MirPerQueryState *st = mir_current_state;
        if (st->n_unwind >= st->unwind_capacity) {
          int new_cap = st->unwind_capacity ? st->unwind_capacity * 2 : 8;
          if (st->unwind_codes)
            st->unwind_codes = repalloc(st->unwind_codes, new_cap * sizeof(void *));
          else
            st->unwind_codes = MemoryContextAlloc(TopMemoryContext,
                                                   new_cap * sizeof(void *));
          st->unwind_capacity = new_cap;
        }
        st->unwind_codes[st->n_unwind++] = mcode;
      }
    }
#endif

    /* Set the eval function (with validation wrapper on first call) */
    pg_jitter_install_expr(state, (ExprStateEvalFunc)code);

    /*
     * Leader: store compiled machine code directly in DSM.
     * Use f->machine_code (actual code), not `code` (thunk).
     */
    if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED &&
        !IsParallelWorker() &&
        (state->parent->state->es_jit_flags & PGJIT_EXPR) &&
        jctx->share_state.sjc) {
      void *mcode = f->machine_code;
      Size mcode_size = f->machine_code_len;

      elog(DEBUG1,
           "pg_jitter[mir]: leader storing code "
           "node=%d expr=%d (%zu bytes) mcode=%p fallback=%p",
           shared_node_id, shared_expr_idx, mcode_size, mcode,
           (void *)pg_jitter_fallback_step);

      pg_jitter_store_shared_code(jctx->share_state.sjc, mcode, mcode_size,
                                  shared_node_id, shared_expr_idx,
                                  (uint64)(uintptr_t)pg_jitter_fallback_step);
    }
  }

  /*
   * Unload the module from the MIR context to free IR memory and remove
   * items from the global hash table.  The generated machine code survives
   * independently (it was copied to executable memory by MIR_gen).
   */
  MIR_unload_module(ctx, m);

  pfree(step_labels);
  pfree(step_fn_imports);
  pfree(step_direct_imports);
  pfree(ioc_in_imports);

  /* Reset shared code mode for next compilation */
  mir_shared_code_mode = false;

  INSTR_TIME_SET_CURRENT(endtime);
  INSTR_TIME_ACCUM_DIFF(jctx->base.instr.generation_counter, endtime,
                        starttime);
  jctx->base.instr.created_functions++;

  return true;
}
