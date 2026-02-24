/*
 * pg_jit_tier2_wrappers.h — C wrapper functions for Tier 2 pass-by-ref PG operations
 *
 * These wrappers call PG built-in functions via DirectFunctionCall, bypassing
 * the need for LLVM bitcode linking. All three JIT backends (sljit, asmjit, mir)
 * can call these directly as regular C functions.
 *
 * When LLVM Tier 2 is available (PG_JITTER_HAVE_TIER2), the LLVM-optimized
 * versions override these for better performance (PG function bodies inlined).
 *
 * Args/returns use the same convention as pg_jit_funcs.h:
 *   - Pass-by-ref args passed as int64 (Datum / pointer)
 *   - bool/int32 results returned as int32
 *   - Datum/pointer results returned as int64
 */
#ifndef PG_JIT_TIER2_WRAPPERS_H
#define PG_JIT_TIER2_WRAPPERS_H

#include "postgres.h"

/* ---- numeric comparison (7) ---- */
extern int32 jit_numeric_eq_wrapper(int64 a, int64 b);
extern int32 jit_numeric_ne_wrapper(int64 a, int64 b);
extern int32 jit_numeric_lt_wrapper(int64 a, int64 b);
extern int32 jit_numeric_le_wrapper(int64 a, int64 b);
extern int32 jit_numeric_gt_wrapper(int64 a, int64 b);
extern int32 jit_numeric_ge_wrapper(int64 a, int64 b);
extern int32 jit_numeric_cmp_wrapper(int64 a, int64 b);

/* ---- numeric arithmetic (3) ---- */
extern int64 jit_numeric_add_wrapper(int64 a, int64 b);
extern int64 jit_numeric_sub_wrapper(int64 a, int64 b);
extern int64 jit_numeric_mul_wrapper(int64 a, int64 b);

/* ---- numeric hash (1) ---- */
extern int32 jit_hash_numeric_wrapper(int64 a);

/*
 * Text comparison/hash wrappers removed: these functions require collation
 * (PG_GET_COLLATION) which DirectFunctionCall cannot provide. Text ops must
 * go through the fcinfo path with fncollation set by ExecInitFunc.
 */

/* ---- interval (3) ---- */
extern int32 jit_interval_eq_wrapper(int64 a, int64 b);
extern int32 jit_interval_lt_wrapper(int64 a, int64 b);
extern int32 jit_interval_cmp_wrapper(int64 a, int64 b);

/* ---- uuid (4) ---- */
extern int32 jit_uuid_eq_wrapper(int64 a, int64 b);
extern int32 jit_uuid_lt_wrapper(int64 a, int64 b);
extern int32 jit_uuid_cmp_wrapper(int64 a, int64 b);
extern int32 jit_uuid_hash_wrapper(int64 a);

#endif /* PG_JIT_TIER2_WRAPPERS_H */
