/*
 * pg_jitter_asmjit.cpp — AsmJIT-based PostgreSQL JIT provider
 *
 * Uses AsmJIT's Compiler for virtual register allocation.
 * Architecture-specific code generation is in .inc files.
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
#include "pg_jitter_simd.h"
#include "pg_jitter_yyjson.h"
#include "pg_jitter_pcre2.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "mb/pg_wchar.h"
#include "catalog/pg_collation_d.h"
#include "pg_jit_funcs.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tupdesc_details.h"
#include "access/parallel.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"
#include "common/hashfn.h"
#include "utils/date.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM >= 160000
#include "nodes/miscnodes.h"
#endif
#include "commands/sequence.h"
#include "funcapi.h"
#include "utils/fmgroids.h"

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

	}

/*
 * On Windows, PG's win32_port.h redefines socket functions as macros
 * (e.g. #define bind(s, addr, addrlen) pgwin32_bind(s, addr, addrlen)).
 * These collide with AsmJIT C++ method names like cc.bind(label).
 * Undefine them before including AsmJIT headers.
 */
#ifdef _WIN32
#undef bind
#undef accept
#undef connect
#undef select
#undef socket
#undef listen
#undef recv
#undef send
#endif

#include <new>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <asmjit/a64.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <asmjit/x86.h>
#else
#error "Unsupported architecture for pg_jitter_asmjit"
#endif

using namespace asmjit;

/*
 * Try to match a tuple descriptor against pre-compiled deform templates.
 * Returns a function pointer to a shared-text deform function, or NULL.
 */

/* GUC enum options for pg_jitter.parallel_mode */
static const struct config_enum_entry parallel_jit_options[] = {
	{"off", PARALLEL_JIT_OFF, false},
	{"per_worker", PARALLEL_JIT_PER_WORKER, false},
	{"shared", PARALLEL_JIT_SHARED, false},
	{NULL, 0, false}
};


/* Per-expression compiled code handle */
struct AsmjitCode {
	JitRuntime	rt;
	void	   *func = nullptr;
};

/* Forward declarations */
static bool asmjit_compile_expr(ExprState *state);
static bool asmjit_emit_all(CodeHolder &code, ExprState *state,
                            PgJitterContext *ctx,
                            ExprEvalStep *steps, int steps_len);
static void asmjit_reset_after_error(void);

static void
asmjit_code_free(void *data)
{
	AsmjitCode *ac = (AsmjitCode *) data;
	if (ac)
	{
		if (ac->func)
		{
			pg_jitter_win64_deregister_unwind(ac->func);
			ac->rt.release(ac->func);
		}
		delete ac;
	}
}

#if defined(_MSC_VER) && PG_VERSION_NUM < 160000
#pragma comment(linker, "/EXPORT:_PG_jit_provider_init")
#endif
extern "C" PG_JITTER_EXPORT void
_PG_jit_provider_init(JitProviderCallbacks *cb)
{
	cb->reset_after_error = asmjit_reset_after_error;
	cb->release_context = pg_jitter_release_context;
	cb->compile_expr = asmjit_compile_expr;

	/*
	 * Define GUCs only if not already registered (avoids conflict when
	 * loaded via the meta module after another backend already defined them).
	 */
	if (!GetConfigOption("pg_jitter.shared_code_max", true, false))
	{
		DefineCustomIntVariable(
			"pg_jitter.shared_code_max",
			"Maximum shared JIT code DSM size in KB.",
			NULL,
			&pg_jitter_shared_code_max_kb,
			4096,		/* 4 MB default */
			64,			/* 64 KB minimum */
			1048576,	/* 1 GB maximum */
			PGC_USERSET,
			GUC_UNIT_KB | GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.parallel_mode", true, false))
	{
		DefineCustomEnumVariable(
			"pg_jitter.parallel_mode",
			"Controls JIT behavior in parallel workers: "
			"off (workers use interpreter), "
			"per_worker (each worker compiles independently), "
			"shared (leader shares compiled code via DSM)",
			NULL,
			&pg_jitter_parallel_mode,
			PARALLEL_JIT_PER_WORKER,
			parallel_jit_options,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.deform_avx512", true, false))
	{
		DefineCustomBoolVariable(
			"pg_jitter.deform_avx512",
			"Use AVX-512F for supported tuple deform batches when the CPU and OS "
			"allow executing AVX-512 instructions.",
			NULL,
			&pg_jitter_deform_avx512,
			true,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.deform_avx512_min_cols", true, false))
	{
		DefineCustomIntVariable(
			"pg_jitter.deform_avx512_min_cols",
			"Minimum uniform int4 deform batch width required before using "
			"AVX-512. 0 allows AVX-512 for every eligible batch.",
			NULL,
			&pg_jitter_deform_avx512_min_cols,
			DEFORM_AVX512_MIN_COLS_DEFAULT,
			0,
			MaxTupleAttributeNumber,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.min_expr_steps", true, false))
	{
		DefineCustomIntVariable(
			"pg_jitter.min_expr_steps",
			"Minimum expression step count for JIT compilation. "
			"Expressions with fewer steps use the interpreter.",
			NULL,
			&pg_jitter_min_expr_steps,
			4,			/* skip JIT for expressions with fewer than 4 steps */
			0,			/* minimum */
			1000,		/* maximum */
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
				NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.in_hash", true, false))
	{
		static const struct config_enum_entry in_hash_options[] = {
			{"pg", IN_HASH_PG, false},
			{"crc32", IN_HASH_CRC32, false},
			{NULL, 0, false}};
		DefineCustomEnumVariable(
			"pg_jitter.in_hash",
			"SLJIT strategy hint for integer IN lists larger than "
			"pg_jitter.in_bsearch_max: pg (PostgreSQL hashed scalar-array op), "
			"crc32 (CRC32C open-addressing; default on x86_64)",
			NULL,
			&pg_jitter_in_hash_strategy,
			IN_HASH_DEFAULT,
			in_hash_options,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.in_bsearch_max", true, false))
	{
		DefineCustomIntVariable(
			"pg_jitter.in_bsearch_max",
			"Max IN list elements for inline binary search tree. "
			"Larger lists use pg_jitter.in_hash. 0 disables inline bsearch.",
			NULL,
			&pg_jitter_in_bsearch_max,
			IN_BSEARCH_MAX_DEFAULT,
			0,
			IN_BSEARCH_MAX_DEFAULT,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

	if (!GetConfigOption("pg_jitter.in_text_hash", true, false))
	{
		DefineCustomBoolVariable(
			"pg_jitter.in_text_hash",
			"Use pg_jitter's experimental text IN-list hash table. "
			"When off, text HASHED_SCALARARRAYOP uses PostgreSQL's native path.",
			NULL,
			&pg_jitter_in_text_hash,
			false,
			PGC_USERSET,
			GUC_ALLOW_IN_PARALLEL,
			NULL, NULL, NULL);
	}

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

#ifdef HAVE_EEOP_HASHDATUM
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
	else
#endif
	if (nsteps == 4)
	{
		step1 = ExecEvalStepOp(state, &state->steps[1]);
		step2 = ExecEvalStepOp(state, &state->steps[2]);

#ifdef HAVE_EEOP_HASHDATUM
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
			(step1 == EEOP_FUNCEXPR_STRICT
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			 || step1 == EEOP_FUNCEXPR_STRICT_1
			 || step1 == EEOP_FUNCEXPR_STRICT_2
#endif
			))
			return true;

#ifdef HAVE_EEOP_HASHDATUM
		if (step0 == EEOP_INNER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
		if (step0 == EEOP_OUTER_VAR && step1 == EEOP_HASHDATUM_FIRST)
			return true;
#endif
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

static bool
asmjit_expr_allows_shared_code(ExprState *state)
{
#if defined(__x86_64__) || defined(_M_X64)
	/*
	 * The x86 AsmJIT emitter materializes many ExprEvalStep, FunctionCallInfo,
	 * resvalue, and resnull addresses as immediates.  Those pointers are
	 * process-local, so parallel workers must compile their own code.
	 */
	(void) state;
	return false;
#else
	for (int i = 0; i < state->steps_len; i++)
	{
		ExprEvalStep *step = &state->steps[i];

		switch (ExecEvalStepOp(state, step))
		{
			/*
			 * These ARM64 emitters still materialize process-local ExprEvalStep
			 * or FunctionCallInfo addresses as immediates.  Shared code may be
			 * executed in a worker with different expression storage, so keep
			 * them on the local-compile path until their emitters use
			 * steps-relative loads throughout.
			 */
#ifdef HAVE_EEOP_JSON_CONSTRUCTOR
			case EEOP_IS_JSON:
#endif
#ifdef HAVE_EEOP_HASHDATUM
			case EEOP_HASHDATUM_SET_INITVAL:
			case EEOP_HASHDATUM_FIRST:
			case EEOP_HASHDATUM_FIRST_STRICT:
			case EEOP_HASHDATUM_NEXT32:
			case EEOP_HASHDATUM_NEXT32_STRICT:
#endif
			case EEOP_PARAM_CALLBACK:
			case EEOP_SBSREF_SUBSCRIPTS:
			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
			case EEOP_IOCOERCE:
#ifdef HAVE_EEOP_IOCOERCE_SAFE
				case EEOP_IOCOERCE_SAFE:
	#endif
				case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
				case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
				case EEOP_AGG_PLAIN_TRANS_BYVAL:
				case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
				case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
				case EEOP_AGG_PLAIN_TRANS_BYREF:
				case EEOP_AGG_DESERIALIZE:
				case EEOP_AGG_STRICT_DESERIALIZE:
			case EEOP_ROWCOMPARE_STEP:
			case EEOP_ROWCOMPARE_FINAL:
#ifdef HAVE_EEOP_RETURNINGEXPR
			case EEOP_RETURNINGEXPR:
#endif
				return false;
			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
#ifdef HAVE_EEOP_FUNCEXPR_STRICT_12
			case EEOP_FUNCEXPR_STRICT_1:
			case EEOP_FUNCEXPR_STRICT_2:
#endif
			{
				FunctionCallInfo fcinfo;
				Datum pat_datum;
				bool pat_isnull;
				Oid fn_oid = InvalidOid;

				fcinfo = step->d.func.fcinfo_data;
				if (fcinfo == NULL || fcinfo->nargs != 2)
					break;
				if (fcinfo->flinfo != NULL)
					fn_oid = fcinfo->flinfo->fn_oid;
				if (!pg_jitter_classify_text_pattern_fn(step->d.func.fn_addr,
														fn_oid, NULL, NULL,
														NULL, NULL, NULL))
					break;
				if (pg_jitter_resolve_constant_func_arg(state, i, fcinfo, 1,
														&pat_datum,
														&pat_isnull) &&
					!pat_isnull)
					return false;
				break;
			}
			default:
				break;
		}
	}

	return true;
#endif
}

/* Include architecture-specific code generation */
#if defined(__aarch64__) || defined(_M_ARM64)
#include "pg_jitter_asmjit_arm64.inc"
#elif defined(__x86_64__) || defined(_M_X64)
#include "pg_jitter_asmjit_x86.inc"
#endif

static void
asmjit_reset_after_error(void)
{
	asmjit_shared_code_mode = false;
}

static bool
asmjit_compile_expr(ExprState *state)
{
	PgJitterContext *ctx;
	AsmjitCode	   *ac;
	ExprEvalStep   *steps;
	int				steps_len;
	instr_time		starttime, endtime;
	int				shared_node_id = 0;
	int				shared_expr_idx = 0;
	uint64			shared_expr_fingerprint = 0;
	bool			shared_code_eligible = true;

	if (!state->parent)
		return false;

	/* Skip JIT in parallel workers when mode is OFF */
	if (pg_jitter_get_parallel_mode() == PARALLEL_JIT_OFF && IsParallelWorker())
		return false;

	if (expr_has_fast_path(state))
		return false;

	/* Skip JIT for expressions below the minimum step threshold */
	{
		int min_steps = pg_jitter_get_min_expr_steps();
		if (min_steps > 0 && state->steps_len < min_steps)
			return false;
	}

	ctx = pg_jitter_get_context(state);
	shared_code_eligible = asmjit_expr_allows_shared_code(state);

	/*
	 * Compute expression identity for shared code.
	 *
	 * Leader: creates DSM on first compile, writes code directly.
	 * Worker: attaches to DSM via GUC, looks up pre-compiled code.
	 */
	if (state->parent->state->es_jit_flags & PGJIT_EXPR)
	{
		pg_jitter_get_expr_identity(ctx, state,
									&shared_node_id, &shared_expr_idx);
		shared_expr_fingerprint = pg_jitter_expr_fingerprint(state);

		/*
		 * Shared code mode is only supported on ARM64 where the generated
		 * code uses steps-relative addressing.  On x86_64, the AsmJIT
		 * backend embeds absolute ExprEvalStep pointer addresses, which
		 * belong to the leader and are invalid in the worker.
		 */
#if defined(__aarch64__) || defined(_M_ARM64)
		asmjit_shared_code_mode =
			shared_code_eligible &&
			(pg_jitter_get_parallel_mode() == PARALLEL_JIT_SHARED) &&
			state->parent->state->es_plannedstmt->parallelModeNeeded;
#else
		asmjit_shared_code_mode = false;
#endif

		elog(DEBUG1, "pg_jitter[asmjit]: compile_expr node=%d expr=%d is_worker=%d "
			 "shared_mode=%d share_init=%d",
			 shared_node_id, shared_expr_idx, IsParallelWorker(),
			 asmjit_shared_code_mode,
			 ctx->share_state.initialized);

		/* Leader: create DSM on first compile */
		if (asmjit_shared_code_mode && !IsParallelWorker() &&
			!ctx->share_state.initialized)
			pg_jitter_init_shared_dsm(ctx);
	}

	/*
	 * Parallel worker: try to use pre-compiled code from the leader.
	 * If found in DSM, copy to local executable memory and skip compilation.
	 */
	if (asmjit_shared_code_mode && IsParallelWorker())
	{
		const void *code_bytes;
		Size		code_size;
		uint64		leader_dylib_ref;

		if (!ctx->share_state.initialized)
			pg_jitter_attach_shared_dsm(ctx);

		if (ctx->share_state.sjc &&
			pg_jitter_find_shared_code(ctx->share_state.sjc,
									   shared_node_id, shared_expr_idx,
									   shared_expr_fingerprint,
									   &code_bytes, &code_size,
									   &leader_dylib_ref))
		{
			void   *handle;
			void   *code_ptr;

			handle = pg_jitter_copy_to_executable(code_bytes, code_size);
			if (handle)
			{
				/* Relocate dylib addresses (ASLR differs between processes) */
				uint64	worker_ref = (uint64)(uintptr_t) pg_jitter_fallback_step;
				int		npatched;

				npatched = pg_jitter_relocate_dylib_addrs(handle, code_size,
														  leader_dylib_ref,
														  worker_ref);

				if (npatched < 0)
				{
					pg_jitter_exec_free(handle);
					elog(WARNING, "pg_jitter[asmjit]: shared code relocation failed "
						 "node=%d expr=%d, compiling locally",
						 shared_node_id, shared_expr_idx);
					goto no_shared_code_reuse;
				}

				code_ptr = pg_jitter_exec_code_ptr(handle);

				elog(DEBUG1, "pg_jitter[asmjit]: worker reused shared code "
					 "node=%d expr=%d (%zu bytes, patched=%d)",
					 shared_node_id, shared_expr_idx, code_size,
					 npatched);

				pg_jitter_register_exec_handle(ctx, handle);

				/* Register Windows x64 unwind metadata for SEH-safe longjmp */
				pg_jitter_win64_register_unwind(code_ptr, code_size);

				/* Worker: set up process-local data structures.
				 * Leader stored pointers in step data; worker needs its own. */
				pg_jitter_setup_case_bsearch_arrays(state, state->steps,
													 state->steps_len);
				pg_jitter_setup_shared_in_hash(state, state->steps,
											   state->steps_len);

				pg_jitter_install_expr(state,
									  (ExprStateEvalFunc) code_ptr);

				asmjit_shared_code_mode = false;

				ctx->base.instr.created_functions++;
				return true;
			}

		elog(WARNING, "pg_jitter[asmjit]: failed to allocate executable memory "
			 "for shared code node=%d expr=%d, compiling locally",
			 shared_node_id, shared_expr_idx);
		/* Fall through to normal compilation */
no_shared_code_reuse:
		;
		}
		else
			elog(DEBUG1, "pg_jitter[asmjit]: worker did not find shared code "
				 "node=%d expr=%d, compiling locally",
				 shared_node_id, shared_expr_idx);
		/* Fall through to normal compilation */
	}

	INSTR_TIME_SET_CURRENT(starttime);

	steps = state->steps;
	steps_len = state->steps_len;

	ac = new (std::nothrow) AsmjitCode();
	if (ac == nullptr)
	{
		asmjit_shared_code_mode = false;
		return false;
	}

	CodeHolder code;
	Error err = code.init(ac->rt.environment());
	if (err != kErrorOk)
	{
		delete ac;
		asmjit_shared_code_mode = false;
		return false;
	}

	if (!asmjit_emit_all(code, state, ctx, steps, steps_len))
	{
		elog(DEBUG1, "pg_jitter[asmjit]: emit_all failed");
		delete ac;
		asmjit_shared_code_mode = false;
		return false;
	}

	err = ac->rt.add(&ac->func, &code);
	if (err != kErrorOk)
	{
		elog(DEBUG1, "pg_jitter[asmjit]: runtime add failed: %u", (unsigned) err);
		delete ac;
		asmjit_shared_code_mode = false;
		return false;
	}

	/*
	 * Leader: store compiled code directly in DSM.
	 */
	if (asmjit_shared_code_mode &&
		!IsParallelWorker() &&
		(state->parent->state->es_jit_flags & PGJIT_EXPR) &&
		ctx->share_state.sjc)
	{
		Size	gen_code_size = code.code_size();

		elog(DEBUG1, "pg_jitter[asmjit]: leader storing code "
			 "node=%d expr=%d (%zu bytes) at %p fallback=%p",
			 shared_node_id, shared_expr_idx,
			 gen_code_size, ac->func, (void *) pg_jitter_fallback_step);

		pg_jitter_store_shared_code(ctx->share_state.sjc,
									ac->func, gen_code_size,
									shared_node_id, shared_expr_idx,
									shared_expr_fingerprint,
									(uint64)(uintptr_t) pg_jitter_fallback_step);
	}

	/*
	 * From this point onward the file-scoped compile mode is no longer needed.
	 * Clear it before ownership transfer so a registration ERROR cannot leave
	 * the next compilation in shared-code mode.
	 */
	asmjit_shared_code_mode = false;

	/* Register for cleanup */
	pg_jitter_register_compiled_or_free(ctx, asmjit_code_free, ac);

	/* Register Windows x64 unwind metadata for SEH-safe longjmp */
	pg_jitter_win64_register_unwind(ac->func, code.code_size());

	/* Set the eval function (with validation wrapper on first call) */
	pg_jitter_install_expr(state, (ExprStateEvalFunc) ac->func);

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(ctx->base.instr.generation_counter,
						  endtime, starttime);
	ctx->base.instr.created_functions++;

	return true;
}
