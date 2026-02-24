/*
 * pg_jit_tier2_wrappers.c — C wrapper functions for Tier 2 pass-by-ref PG operations
 *
 * Each wrapper calls the corresponding PG built-in function via
 * DirectFunctionCall, converting between native arg/return types and
 * the V1 fcinfo calling convention.
 *
 * These are compiled directly into each backend's shared library,
 * requiring no external dependencies (no LLVM, no PG bitcode).
 * All three JIT backends call these as regular C functions.
 *
 * Performance note: These have ~10ns overhead vs ~3ns for LLVM-inlined
 * Tier 2 wrappers, but are always available on any platform.
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"

#include "pg_jit_tier2_wrappers.h"

/* ================================================================
 * numeric comparison (7 functions)
 * All take Datum (numeric pointer), return bool as int32.
 * ================================================================ */

int32
jit_numeric_eq_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_eq, (Datum) a, (Datum) b));
}

int32
jit_numeric_ne_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_ne, (Datum) a, (Datum) b));
}

int32
jit_numeric_lt_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_lt, (Datum) a, (Datum) b));
}

int32
jit_numeric_le_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_le, (Datum) a, (Datum) b));
}

int32
jit_numeric_gt_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_gt, (Datum) a, (Datum) b));
}

int32
jit_numeric_ge_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_ge, (Datum) a, (Datum) b));
}

int32
jit_numeric_cmp_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(numeric_cmp, (Datum) a, (Datum) b));
}

/* ================================================================
 * numeric arithmetic (3 functions)
 * Return Datum (pointer to palloc'd Numeric).
 * ================================================================ */

int64
jit_numeric_add_wrapper(int64 a, int64 b)
{
	return (int64) DirectFunctionCall2(numeric_add, (Datum) a, (Datum) b);
}

int64
jit_numeric_sub_wrapper(int64 a, int64 b)
{
	return (int64) DirectFunctionCall2(numeric_sub, (Datum) a, (Datum) b);
}

int64
jit_numeric_mul_wrapper(int64 a, int64 b)
{
	return (int64) DirectFunctionCall2(numeric_mul, (Datum) a, (Datum) b);
}

/* ================================================================
 * numeric hash (1 function)
 * ================================================================ */

int32
jit_hash_numeric_wrapper(int64 a)
{
	return DatumGetInt32(DirectFunctionCall1(hash_numeric, (Datum) a));
}

/*
 * Text comparison/hash wrappers removed: these functions require collation
 * (PG_GET_COLLATION) which DirectFunctionCall cannot provide. Text ops must
 * go through the fcinfo path with fncollation set by ExecInitFunc.
 */

/* ================================================================
 * interval comparison (3 functions)
 *
 * Note: interval comparison is already natively implemented in
 * pg_jit_funcs.c as Tier 1 (using INT128). These wrappers are
 * provided for consistency with the LLVM Tier 2 pipeline but
 * should rarely be reached since the lookup table already has
 * native jit_fn pointers for interval_eq/lt/cmp.
 * ================================================================ */

int32
jit_interval_eq_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(interval_eq, (Datum) a, (Datum) b));
}

int32
jit_interval_lt_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(interval_lt, (Datum) a, (Datum) b));
}

int32
jit_interval_cmp_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(interval_cmp, (Datum) a, (Datum) b));
}

/* ================================================================
 * uuid comparison + hash (4 functions)
 * ================================================================ */

int32
jit_uuid_eq_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(uuid_eq, (Datum) a, (Datum) b));
}

int32
jit_uuid_lt_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(uuid_lt, (Datum) a, (Datum) b));
}

int32
jit_uuid_cmp_wrapper(int64 a, int64 b)
{
	return DatumGetInt32(DirectFunctionCall2(uuid_cmp, (Datum) a, (Datum) b));
}

int32
jit_uuid_hash_wrapper(int64 a)
{
	return DatumGetInt32(DirectFunctionCall1(uuid_hash, (Datum) a));
}
