/*
 * pg_jit_funcs.c — Unwrapped implementations of hot PG built-in functions
 *
 * These bypass the V1 fmgr calling convention (fcinfo): args are passed
 * as native C types in registers, and results are returned directly.
 * The JIT backends emit direct calls to these instead of fn_addr(fcinfo).
 *
 * TIER 1: Pass-by-value types with trivial bodies (int, float, bool, date,
 *         timestamp, oid). Full native implementation.
 * TIER 2: Pass-by-reference operators (text, numeric, jsonb, uuid, array,
 *         interval, bytea, bpchar, inet). Listed in lookup table with
 *         jit_fn=NULL (deferred to LLVM IR inlining in Part 2).
 * TIER 3: Complex mutation functions. Listed as comments only.
 */

#include "postgres.h"
#include "pg_jitter_compat.h"
#include "common/int.h"
#include "common/int128.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"   /* timestamp_cmp_internal, Interval */
#include "utils/date.h"        /* DateADT, date constants */
#include "datatype/timestamp.h" /* USECS_PER_DAY, INTERVAL_NOT_FINITE */
#include "utils/float.h"       /* get_float8_nan */
#if PG_VERSION_NUM >= 160000
#include "varatt.h"            /* VARDATA_ANY, VARSIZE_ANY_EXHDR */
#endif
#include "utils/varlena.h"     /* varstr_cmp */
#include "utils/pg_locale.h"   /* pg_newlocale_from_collation, pg_locale_t */
#include "access/detoast.h"    /* toast_raw_datum_size */
#include "utils/array.h"       /* ARR_OVERHEAD_NONULLS, ArrayType */
#include "mb/pg_wchar.h"       /* pg_mbstrlen_with_len */

#include "pg_jit_funcs.h"
#include "pg_jit_tier2_wrappers.h"
#include "pg_jitter_simd.h"

/* ================================================================
 * Error handlers — cold path, never inlined
 *
 * On Windows x64, these call ereport(ERROR) which does longjmp through
 * JIT-generated stack frames.  This is safe because we register proper
 * SEH unwind metadata via RtlInstallFunctionTableCallback.
 * ================================================================ */

pg_noinline void jit_error_int2_overflow(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("smallint out of range")));
}

pg_noinline void jit_error_int4_overflow(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("integer out of range")));
}

pg_noinline void jit_error_int8_overflow(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("bigint out of range")));
}

pg_noinline void jit_error_division_by_zero(void) {
  ereport(ERROR,
          (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
}

pg_noinline void jit_error_float_overflow(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("value out of range: overflow")));
}

pg_noinline void jit_error_float_underflow(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("value out of range: underflow")));
}

/* ================================================================
 * TIER 1: int32 arithmetic (6 + 1 functions)
 * ================================================================ */

int32 jit_int4pl(int32 a, int32 b) {
  int32 r;
  if (unlikely(pg_add_s32_overflow(a, b, &r)))
    jit_error_int4_overflow();
  return r;
}

int32 jit_int4mi(int32 a, int32 b) {
  int32 r;
  if (unlikely(pg_sub_s32_overflow(a, b, &r)))
    jit_error_int4_overflow();
  return r;
}

int32 jit_int4mul(int32 a, int32 b) {
  int32 r;
  if (unlikely(pg_mul_s32_overflow(a, b, &r)))
    jit_error_int4_overflow();
  return r;
}

int32 jit_int4div(int32 a, int32 b) {
  if (unlikely(b == 0))
    jit_error_division_by_zero();
  if (unlikely(a == PG_INT32_MIN && b == -1))
    jit_error_int4_overflow();
  return a / b;
}

int32 jit_int4mod(int32 a, int32 b) {
  if (unlikely(b == 0))
    jit_error_division_by_zero();
  if (unlikely(a == PG_INT32_MIN && b == -1))
    return 0; /* PG returns 0, not overflow */
  return a % b;
}

int32 jit_int4abs(int32 a) {
  int32 r;
  if (unlikely(a == PG_INT32_MIN))
    jit_error_int4_overflow();
  r = (a < 0) ? -a : a;
  return r;
}

int32 jit_int4inc(int32 a) {
  int32 r;
  if (unlikely(pg_add_s32_overflow(a, 1, &r)))
    jit_error_int4_overflow();
  return r;
}

/* ================================================================
 * TIER 1: int64 arithmetic (7 functions)
 * ================================================================ */

int64 jit_int8pl(int64 a, int64 b) {
  int64 r;
  if (unlikely(pg_add_s64_overflow(a, b, &r)))
    jit_error_int8_overflow();
  return r;
}

int64 jit_int8mi(int64 a, int64 b) {
  int64 r;
  if (unlikely(pg_sub_s64_overflow(a, b, &r)))
    jit_error_int8_overflow();
  return r;
}

int64 jit_int8mul(int64 a, int64 b) {
  int64 r;
  if (unlikely(pg_mul_s64_overflow(a, b, &r)))
    jit_error_int8_overflow();
  return r;
}

int64 jit_int8div(int64 a, int64 b) {
  if (unlikely(b == 0))
    jit_error_division_by_zero();
  if (unlikely(a == PG_INT64_MIN && b == -1))
    jit_error_int8_overflow();
  return a / b;
}

int64 jit_int8mod(int64 a, int64 b) {
  if (unlikely(b == 0))
    jit_error_division_by_zero();
  if (unlikely(a == PG_INT64_MIN && b == -1))
    return 0;
  return a % b;
}

int64 jit_int8abs(int64 a) {
  if (unlikely(a == PG_INT64_MIN))
    jit_error_int8_overflow();
  return (a < 0) ? -a : a;
}

int64 jit_int8inc(int64 a) {
  int64 r;
  if (unlikely(pg_add_s64_overflow(a, 1, &r)))
    jit_error_int8_overflow();
  return r;
}

int64 jit_int8dec(int64 a) {
  int64 r;
  if (unlikely(pg_sub_s64_overflow(a, 1, &r)))
    jit_error_int8_overflow();
  return r;
}

/* ================================================================
 * TIER 1: int16 arithmetic (6 functions)
 * ================================================================ */

int32 jit_int2pl(int32 a32, int32 b32) {
  int16 a = (int16)a32, b = (int16)b32;
  int32 r = (int32)a + (int32)b;
  if (unlikely(r < PG_INT16_MIN || r > PG_INT16_MAX))
    jit_error_int2_overflow();
  return (int32)(int16)r;
}

int32 jit_int2mi(int32 a32, int32 b32) {
  int16 a = (int16)a32, b = (int16)b32;
  int32 r = (int32)a - (int32)b;
  if (unlikely(r < PG_INT16_MIN || r > PG_INT16_MAX))
    jit_error_int2_overflow();
  return (int32)(int16)r;
}

int32 jit_int2mul(int32 a32, int32 b32) {
  int16 a = (int16)a32, b = (int16)b32;
  int32 r = (int32)a * (int32)b;
  if (unlikely(r < PG_INT16_MIN || r > PG_INT16_MAX))
    jit_error_int2_overflow();
  return (int32)(int16)r;
}

int32 jit_int2div(int32 a32, int32 b32) {
  int16 a = (int16)a32, b = (int16)b32;
  if (unlikely(b == 0))
    jit_error_division_by_zero();
  if (unlikely(a == PG_INT16_MIN && b == -1))
    jit_error_int2_overflow();
  return (int32)(a / b);
}

int32 jit_int2mod(int32 a32, int32 b32) {
  int16 a = (int16)a32, b = (int16)b32;
  if (unlikely(b == 0))
    jit_error_division_by_zero();
  if (unlikely(a == PG_INT16_MIN && b == -1))
    return 0;
  return (int32)(a % b);
}

int32 jit_int2abs(int32 a32) {
  int16 a = (int16)a32;
  if (unlikely(a == PG_INT16_MIN))
    jit_error_int2_overflow();
  return (int32)((a < 0) ? -a : a);
}

/* ================================================================
 * TIER 1: int32 comparison (6 functions)
 * ================================================================ */

#define DEF_CMP(name, type, op)                                                \
  int32 jit_##name(type a, type b) { return (a op b) ? 1 : 0; }

DEF_CMP(int4eq, int32, ==)
DEF_CMP(int4ne, int32, !=)
DEF_CMP(int4lt, int32, <)
DEF_CMP(int4le, int32, <=)
DEF_CMP(int4gt, int32, >)
DEF_CMP(int4ge, int32, >=)

/* ================================================================
 * TIER 1: int64 comparison (6 functions)
 * ================================================================ */

DEF_CMP(int8eq, int64, ==)
DEF_CMP(int8ne, int64, !=)
DEF_CMP(int8lt, int64, <)
DEF_CMP(int8le, int64, <=)
DEF_CMP(int8gt, int64, >)
DEF_CMP(int8ge, int64, >=)

/* ================================================================
 * TIER 1: int16 comparison (6 functions)
 * These take int32 args (lower 16 bits is the int16 value as stored in Datum)
 * ================================================================ */

int32 jit_int2eq(int32 a, int32 b) { return ((int16)a == (int16)b) ? 1 : 0; }
int32 jit_int2ne(int32 a, int32 b) { return ((int16)a != (int16)b) ? 1 : 0; }
int32 jit_int2lt(int32 a, int32 b) { return ((int16)a < (int16)b) ? 1 : 0; }
int32 jit_int2le(int32 a, int32 b) { return ((int16)a <= (int16)b) ? 1 : 0; }
int32 jit_int2gt(int32 a, int32 b) { return ((int16)a > (int16)b) ? 1 : 0; }
int32 jit_int2ge(int32 a, int32 b) { return ((int16)a >= (int16)b) ? 1 : 0; }

/* ================================================================
 * TIER 1: Cross-type int comparison (30 functions)
 * ================================================================ */

/* int4 vs int8 (widen int4 to int8) */
int32 jit_int48eq(int32 a, int64 b) { return ((int64)a == b) ? 1 : 0; }
int32 jit_int48ne(int32 a, int64 b) { return ((int64)a != b) ? 1 : 0; }
int32 jit_int48lt(int32 a, int64 b) { return ((int64)a < b) ? 1 : 0; }
int32 jit_int48le(int32 a, int64 b) { return ((int64)a <= b) ? 1 : 0; }
int32 jit_int48gt(int32 a, int64 b) { return ((int64)a > b) ? 1 : 0; }
int32 jit_int48ge(int32 a, int64 b) { return ((int64)a >= b) ? 1 : 0; }

/* int8 vs int4 */
int32 jit_int84eq(int64 a, int32 b) { return (a == (int64)b) ? 1 : 0; }
int32 jit_int84ne(int64 a, int32 b) { return (a != (int64)b) ? 1 : 0; }
int32 jit_int84lt(int64 a, int32 b) { return (a < (int64)b) ? 1 : 0; }
int32 jit_int84le(int64 a, int32 b) { return (a <= (int64)b) ? 1 : 0; }
int32 jit_int84gt(int64 a, int32 b) { return (a > (int64)b) ? 1 : 0; }
int32 jit_int84ge(int64 a, int32 b) { return (a >= (int64)b) ? 1 : 0; }

/* int2 vs int4 (widen int2 to int4) — args passed as int32 */
/* int2 vs int4: a is int16 (in lower 16 bits of int32 Datum), b is int32 */
int32 jit_int24eq(int32 a, int32 b) { return ((int32)(int16)a == b) ? 1 : 0; }
int32 jit_int24ne(int32 a, int32 b) { return ((int32)(int16)a != b) ? 1 : 0; }
int32 jit_int24lt(int32 a, int32 b) { return ((int32)(int16)a < b) ? 1 : 0; }
int32 jit_int24le(int32 a, int32 b) { return ((int32)(int16)a <= b) ? 1 : 0; }
int32 jit_int24gt(int32 a, int32 b) { return ((int32)(int16)a > b) ? 1 : 0; }
int32 jit_int24ge(int32 a, int32 b) { return ((int32)(int16)a >= b) ? 1 : 0; }

/* int4 vs int2: a is int32, b is int16 */
int32 jit_int42eq(int32 a, int32 b) { return (a == (int32)(int16)b) ? 1 : 0; }
int32 jit_int42ne(int32 a, int32 b) { return (a != (int32)(int16)b) ? 1 : 0; }
int32 jit_int42lt(int32 a, int32 b) { return (a < (int32)(int16)b) ? 1 : 0; }
int32 jit_int42le(int32 a, int32 b) { return (a <= (int32)(int16)b) ? 1 : 0; }
int32 jit_int42gt(int32 a, int32 b) { return (a > (int32)(int16)b) ? 1 : 0; }
int32 jit_int42ge(int32 a, int32 b) { return (a >= (int32)(int16)b) ? 1 : 0; }

/* int2 vs int8: a is int16 (in Datum/int64), b is int64 */
int32 jit_int28eq(int64 a, int64 b) { return ((int64)(int16)a == b) ? 1 : 0; }
int32 jit_int28ne(int64 a, int64 b) { return ((int64)(int16)a != b) ? 1 : 0; }
int32 jit_int28lt(int64 a, int64 b) { return ((int64)(int16)a < b) ? 1 : 0; }
int32 jit_int28le(int64 a, int64 b) { return ((int64)(int16)a <= b) ? 1 : 0; }
int32 jit_int28gt(int64 a, int64 b) { return ((int64)(int16)a > b) ? 1 : 0; }
int32 jit_int28ge(int64 a, int64 b) { return ((int64)(int16)a >= b) ? 1 : 0; }

/* int8 vs int2: a is int64, b is int16 (in Datum/int64) */
int32 jit_int82eq(int64 a, int64 b) { return (a == (int64)(int16)b) ? 1 : 0; }
int32 jit_int82ne(int64 a, int64 b) { return (a != (int64)(int16)b) ? 1 : 0; }
int32 jit_int82lt(int64 a, int64 b) { return (a < (int64)(int16)b) ? 1 : 0; }
int32 jit_int82le(int64 a, int64 b) { return (a <= (int64)(int16)b) ? 1 : 0; }
int32 jit_int82gt(int64 a, int64 b) { return (a > (int64)(int16)b) ? 1 : 0; }
int32 jit_int82ge(int64 a, int64 b) { return (a >= (int64)(int16)b) ? 1 : 0; }

/* ================================================================
 * TIER 1: Cross-type int arithmetic (24 functions)
 * ================================================================ */

/* int2 + int4 → int4 */
int32 jit_int24pl(int32 a, int32 b) { return jit_int4pl((int32)(int16)a, b); }
int32 jit_int24mi(int32 a, int32 b) { return jit_int4mi((int32)(int16)a, b); }
int32 jit_int24mul(int32 a, int32 b) { return jit_int4mul((int32)(int16)a, b); }
int32 jit_int24div(int32 a, int32 b) { return jit_int4div((int32)(int16)a, b); }

/* int4 + int2 → int4 */
int32 jit_int42pl(int32 a, int32 b) { return jit_int4pl(a, (int32)(int16)b); }
int32 jit_int42mi(int32 a, int32 b) { return jit_int4mi(a, (int32)(int16)b); }
int32 jit_int42mul(int32 a, int32 b) { return jit_int4mul(a, (int32)(int16)b); }
int32 jit_int42div(int32 a, int32 b) { return jit_int4div(a, (int32)(int16)b); }

/* int4 + int8 → int8 */
int64 jit_int48pl(int64 a, int64 b) { return jit_int8pl((int64)(int32)a, b); }
int64 jit_int48mi(int64 a, int64 b) { return jit_int8mi((int64)(int32)a, b); }
int64 jit_int48mul(int64 a, int64 b) { return jit_int8mul((int64)(int32)a, b); }
int64 jit_int48div(int64 a, int64 b) { return jit_int8div((int64)(int32)a, b); }

/* int8 + int4 → int8 */
int64 jit_int84pl(int64 a, int64 b) { return jit_int8pl(a, (int64)(int32)b); }
int64 jit_int84mi(int64 a, int64 b) { return jit_int8mi(a, (int64)(int32)b); }
int64 jit_int84mul(int64 a, int64 b) { return jit_int8mul(a, (int64)(int32)b); }
int64 jit_int84div(int64 a, int64 b) { return jit_int8div(a, (int64)(int32)b); }

/* int2 + int8 → int8 */
int64 jit_int28pl(int64 a, int64 b) { return jit_int8pl((int64)(int16)a, b); }
int64 jit_int28mi(int64 a, int64 b) { return jit_int8mi((int64)(int16)a, b); }
int64 jit_int28mul(int64 a, int64 b) { return jit_int8mul((int64)(int16)a, b); }
int64 jit_int28div(int64 a, int64 b) { return jit_int8div((int64)(int16)a, b); }

/* int8 + int2 → int8 */
int64 jit_int82pl(int64 a, int64 b) { return jit_int8pl(a, (int64)(int16)b); }
int64 jit_int82mi(int64 a, int64 b) { return jit_int8mi(a, (int64)(int16)b); }
int64 jit_int82mul(int64 a, int64 b) { return jit_int8mul(a, (int64)(int16)b); }
int64 jit_int82div(int64 a, int64 b) { return jit_int8div(a, (int64)(int16)b); }

/* ================================================================
 * TIER 1: int min/max (6 functions)
 * ================================================================ */

int32 jit_int2larger(int32 a, int32 b) {
  return ((int16)a >= (int16)b) ? a : b;
}
int32 jit_int2smaller(int32 a, int32 b) {
  return ((int16)a <= (int16)b) ? a : b;
}
int32 jit_int4larger(int32 a, int32 b) { return (a >= b) ? a : b; }
int32 jit_int4smaller(int32 a, int32 b) { return (a <= b) ? a : b; }
int64 jit_int8larger(int64 a, int64 b) { return (a >= b) ? a : b; }
int64 jit_int8smaller(int64 a, int64 b) { return (a <= b) ? a : b; }

/* ================================================================
 * TIER 1: int bitwise (18 functions)
 * ================================================================ */

int32 jit_int2and(int32 a, int32 b) { return (int32)((int16)a & (int16)b); }
int32 jit_int2or(int32 a, int32 b) { return (int32)((int16)a | (int16)b); }
int32 jit_int2xor(int32 a, int32 b) { return (int32)((int16)a ^ (int16)b); }
int32 jit_int2not(int32 a) { return (int32)(~(int16)a); }
int32 jit_int2shl(int32 a, int32 b) { return (int32)((int16)a << b); }
int32 jit_int2shr(int32 a, int32 b) { return (int32)((int16)a >> b); }

int32 jit_int4and(int32 a, int32 b) { return a & b; }
int32 jit_int4or(int32 a, int32 b) { return a | b; }
int32 jit_int4xor(int32 a, int32 b) { return a ^ b; }
int32 jit_int4not(int32 a) { return ~a; }
int32 jit_int4shl(int32 a, int32 b) { return a << b; }
int32 jit_int4shr(int32 a, int32 b) { return a >> b; }

int64 jit_int8and(int64 a, int64 b) { return a & b; }
int64 jit_int8or(int64 a, int64 b) { return a | b; }
int64 jit_int8xor(int64 a, int64 b) { return a ^ b; }
int64 jit_int8not(int64 a) { return ~a; }
int64 jit_int8shl(int64 a, int64 b) { return a << (int)b; }
int64 jit_int8shr(int64 a, int64 b) { return a >> (int)b; }

/* ================================================================
 * TIER 1: float8 arithmetic (4 functions)
 * Pass float8 as Datum (int64 bits), cast inside.
 * ================================================================ */

/* Helper: reinterpret Datum bits as float8 and back */
static inline float8 datum_to_float8(int64 d) {
  union {
    int64 i;
    float8 f;
  } u;
  u.i = d;
  return u.f;
}

static inline int64 float8_to_datum(float8 f) {
  union {
    int64 i;
    float8 v;
  } u;
  u.v = f;
  return u.i;
}

static inline float4 datum_to_float4(int32 d) {
  union {
    int32 i;
    float4 f;
  } u;
  u.i = d;
  return u.f;
}

static inline int32 float4_to_datum(float4 f) {
  union {
    int32 i;
    float4 v;
  } u;
  u.v = f;
  return u.i;
}

int64 jit_float8pl(int64 a, int64 b) {
  float8 r = datum_to_float8(a) + datum_to_float8(b);
  if (unlikely(isinf(r)) && !isinf(datum_to_float8(a)) &&
      !isinf(datum_to_float8(b)))
    jit_error_float_overflow();
  return float8_to_datum(r);
}

int64 jit_float8mi(int64 a, int64 b) {
  float8 r = datum_to_float8(a) - datum_to_float8(b);
  if (unlikely(isinf(r)) && !isinf(datum_to_float8(a)) &&
      !isinf(datum_to_float8(b)))
    jit_error_float_overflow();
  return float8_to_datum(r);
}

int64 jit_float8mul(int64 a, int64 b) {
  float8 r = datum_to_float8(a) * datum_to_float8(b);
  if (unlikely(isinf(r)) && !isinf(datum_to_float8(a)) &&
      !isinf(datum_to_float8(b)))
    jit_error_float_overflow();
  if (unlikely(r == 0.0) && datum_to_float8(a) != 0.0 &&
      datum_to_float8(b) != 0.0)
    jit_error_float_underflow();
  return float8_to_datum(r);
}

int64 jit_float8div(int64 a, int64 b) {
  float8 fa = datum_to_float8(a);
  float8 fb = datum_to_float8(b);
  if (unlikely(fb == 0.0))
    jit_error_division_by_zero();
  float8 r = fa / fb;
  if (unlikely(isinf(r)) && !isinf(fa))
    jit_error_float_overflow();
  if (unlikely(r == 0.0) && fa != 0.0)
    jit_error_float_underflow();
  return float8_to_datum(r);
}

int64 jit_float8abs(int64 a) {
  return float8_to_datum(fabs(datum_to_float8(a)));
}

int64 jit_float8um(int64 a) { return float8_to_datum(-datum_to_float8(a)); }

/* ================================================================
 * TIER 1: float4 arithmetic (4 + 2 functions)
 * float4 stored in lower 32 bits of Datum. We pass as int64 (Datum).
 * ================================================================ */

int64 jit_float4pl(int64 a, int64 b) {
  float4 r = DatumGetFloat4((Datum)a) + DatumGetFloat4((Datum)b);
  if (unlikely(isinf(r)) && !isinf(DatumGetFloat4((Datum)a)) &&
      !isinf(DatumGetFloat4((Datum)b)))
    jit_error_float_overflow();
  return (int64)Float4GetDatum(r);
}

int64 jit_float4mi(int64 a, int64 b) {
  float4 r = DatumGetFloat4((Datum)a) - DatumGetFloat4((Datum)b);
  if (unlikely(isinf(r)) && !isinf(DatumGetFloat4((Datum)a)) &&
      !isinf(DatumGetFloat4((Datum)b)))
    jit_error_float_overflow();
  return (int64)Float4GetDatum(r);
}

int64 jit_float4mul(int64 a, int64 b) {
  float4 r = DatumGetFloat4((Datum)a) * DatumGetFloat4((Datum)b);
  if (unlikely(isinf(r)) && !isinf(DatumGetFloat4((Datum)a)) &&
      !isinf(DatumGetFloat4((Datum)b)))
    jit_error_float_overflow();
  return (int64)Float4GetDatum(r);
}

int64 jit_float4div(int64 a, int64 b) {
  float4 fa = DatumGetFloat4((Datum)a);
  float4 fb = DatumGetFloat4((Datum)b);
  if (unlikely(fb == 0.0f))
    jit_error_division_by_zero();
  float4 r = fa / fb;
  if (unlikely(isinf(r)) && !isinf(fa))
    jit_error_float_overflow();
  return (int64)Float4GetDatum(r);
}

int64 jit_float4abs(int64 a) {
  return (int64)Float4GetDatum(fabsf(DatumGetFloat4((Datum)a)));
}

int64 jit_float4um(int64 a) {
  return (int64)Float4GetDatum(-DatumGetFloat4((Datum)a));
}

/* ================================================================
 * TIER 1: float comparison (12 functions)
 * Args passed as Datum (int64).
 * ================================================================ */

/*
 * PG float comparison: NaN is considered equal to NaN, and greater
 * than any non-NaN value. This differs from IEEE 754 where NaN != NaN.
 * We must replicate PG's float8_cmp_internal() semantics.
 */
static inline int float8_cmp_jit(float8 a, float8 b) {
  if (isnan(a))
    return isnan(b) ? 0 : 1;
  if (isnan(b))
    return -1;
  return (a > b) ? 1 : ((a < b) ? -1 : 0);
}

static inline int float4_cmp_jit(float4 a, float4 b) {
  if (isnan(a))
    return isnan(b) ? 0 : 1;
  if (isnan(b))
    return -1;
  return (a > b) ? 1 : ((a < b) ? -1 : 0);
}

int32 jit_float4eq(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) == 0
             ? 1
             : 0;
}
int32 jit_float4ne(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) != 0
             ? 1
             : 0;
}
int32 jit_float4lt(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) < 0
             ? 1
             : 0;
}
int32 jit_float4le(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) <= 0
             ? 1
             : 0;
}
int32 jit_float4gt(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) > 0
             ? 1
             : 0;
}
int32 jit_float4ge(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) >= 0
             ? 1
             : 0;
}

int32 jit_float8eq(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) == 0 ? 1 : 0;
}
int32 jit_float8ne(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) != 0 ? 1 : 0;
}
int32 jit_float8lt(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) < 0 ? 1 : 0;
}
int32 jit_float8le(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) <= 0 ? 1 : 0;
}
int32 jit_float8gt(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) > 0 ? 1 : 0;
}
int32 jit_float8ge(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) >= 0 ? 1 : 0;
}

/* ================================================================
 * TIER 1: float min/max (4 functions)
 * ================================================================ */

int64 jit_float4larger(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) >= 0
             ? a
             : b;
}
int64 jit_float4smaller(int64 a, int64 b) {
  return float4_cmp_jit(DatumGetFloat4((Datum)a), DatumGetFloat4((Datum)b)) <= 0
             ? a
             : b;
}
int64 jit_float8larger(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) >= 0 ? a : b;
}
int64 jit_float8smaller(int64 a, int64 b) {
  return float8_cmp_jit(datum_to_float8(a), datum_to_float8(b)) <= 0 ? a : b;
}

/* ================================================================
 * TIER 1: Type cast functions (14 functions)
 * Bypass fmgr for trivial type conversions.
 * ================================================================ */

/* int widening (sign-extend, cannot overflow) */
int64 jit_int48_cast(int32 a)  { return (int64)a; }
int64 jit_int28_cast(int32 a)  { return (int64)(int16)a; }
int32 jit_int24_cast(int32 a)  { return (int32)(int16)a; }

/* int narrowing (range-checked) */
int32 jit_int84_cast(int64 a)  {
  if (unlikely(a < PG_INT32_MIN || a > PG_INT32_MAX))
    jit_error_int4_overflow();
  return (int32)a;
}
int32 jit_int82_cast(int64 a)  {
  if (unlikely(a < PG_INT16_MIN || a > PG_INT16_MAX))
    jit_error_int2_overflow();
  return (int32)(int16)a;
}
int32 jit_int42_cast(int32 a)  {
  if (unlikely(a < PG_INT16_MIN || a > PG_INT16_MAX))
    jit_error_int2_overflow();
  return (int32)(int16)a;
}

/* float<->float */
int64 jit_ftod(int64 a) {
  return float8_to_datum((float8)DatumGetFloat4((Datum)a));
}
int64 jit_dtof(int64 a) {
  float8 v = datum_to_float8(a);
  if (unlikely(isinf((float4)v) && !isinf(v)))
    jit_error_float_overflow();
  return (int64)Float4GetDatum((float4)v);
}

/* int->float */
int64 jit_i4tod(int32 a) { return float8_to_datum((float8)a); }
int64 jit_i4tof(int32 a) { return (int64)Float4GetDatum((float4)a); }
int64 jit_i8tod(int64 a) { return float8_to_datum((float8)a); }
int64 jit_i2tod(int32 a) { return float8_to_datum((float8)(int16)a); }
int64 jit_i2tof(int32 a) { return (int64)Float4GetDatum((float4)(int16)a); }

/* float->int (range-checked, NaN-checked, uses rint for banker's rounding) */
int32 jit_dtoi4(int64 a) {
  float8 v = datum_to_float8(a);
  if (unlikely(isnan(v) || !FLOAT8_FITS_IN_INT32(v)))
    jit_error_int4_overflow();
  return (int32)rint(v);
}
int32 jit_ftoi4(int64 a) {
  float4 v = DatumGetFloat4((Datum)a);
  if (unlikely(isnan(v) || !FLOAT4_FITS_IN_INT32(v)))
    jit_error_int4_overflow();
  return (int32)rint(v);
}
int64 jit_dtoi8(int64 a) {
  float8 v = datum_to_float8(a);
  if (unlikely(isnan(v) || !FLOAT8_FITS_IN_INT64(v)))
    jit_error_int8_overflow();
  return (int64)rint(v);
}
int32 jit_ftoi2(int64 a) {
  float4 v = DatumGetFloat4((Datum)a);
  if (unlikely(isnan(v) || !FLOAT4_FITS_IN_INT16(v)))
    jit_error_int2_overflow();
  return (int32)(int16)rint(v);
}
int32 jit_dtoi2(int64 a) {
  float8 v = datum_to_float8(a);
  if (unlikely(isnan(v) || !FLOAT8_FITS_IN_INT16(v)))
    jit_error_int2_overflow();
  return (int32)(int16)rint(v);
}

/* ================================================================
 * TIER 1: Cross-type float comparison (12 functions)
 * Promote float4 to float8, then compare.
 * ================================================================ */

#define DEF_FLOAT48_CMP(name, op) \
int32 jit_##name(int64 a, int64 b) { \
  float8 fa = (float8)DatumGetFloat4((Datum)a); \
  float8 fb = datum_to_float8(b); \
  return float8_cmp_jit(fa, fb) op ? 1 : 0; \
}
DEF_FLOAT48_CMP(float48eq, == 0)
DEF_FLOAT48_CMP(float48ne, != 0)
DEF_FLOAT48_CMP(float48lt, < 0)
DEF_FLOAT48_CMP(float48le, <= 0)
DEF_FLOAT48_CMP(float48gt, > 0)
DEF_FLOAT48_CMP(float48ge, >= 0)

#define DEF_FLOAT84_CMP(name, op) \
int32 jit_##name(int64 a, int64 b) { \
  float8 fa = datum_to_float8(a); \
  float8 fb = (float8)DatumGetFloat4((Datum)b); \
  return float8_cmp_jit(fa, fb) op ? 1 : 0; \
}
DEF_FLOAT84_CMP(float84eq, == 0)
DEF_FLOAT84_CMP(float84ne, != 0)
DEF_FLOAT84_CMP(float84lt, < 0)
DEF_FLOAT84_CMP(float84le, <= 0)
DEF_FLOAT84_CMP(float84gt, > 0)
DEF_FLOAT84_CMP(float84ge, >= 0)

/* ================================================================
 * TIER 1: Cross-type float arithmetic (8 functions)
 * Promote float4 to float8, operate, check overflow.
 * ================================================================ */

/* Add/sub: zero result from non-zero inputs is valid (e.g., -5.0 + 5 = 0) */
#define DEF_FLOAT48_ADDSUB(name, op) \
int64 jit_##name(int64 a, int64 b) { \
  float8 fa = (float8)DatumGetFloat4((Datum)a); \
  float8 fb = datum_to_float8(b); \
  float8 r = fa op fb; \
  if (unlikely(isinf(r)) && !isinf(fa) && !isinf(fb)) \
    jit_error_float_overflow(); \
  return float8_to_datum(r); \
}
DEF_FLOAT48_ADDSUB(float48pl, +)
DEF_FLOAT48_ADDSUB(float48mi, -)

/* Mul: zero from non-zero inputs IS underflow (precision loss) */
int64 jit_float48mul(int64 a, int64 b) {
  float8 fa = (float8)DatumGetFloat4((Datum)a);
  float8 fb = datum_to_float8(b);
  float8 r = fa * fb;
  if (unlikely(isinf(r)) && !isinf(fa) && !isinf(fb))
    jit_error_float_overflow();
  if (unlikely(r == 0.0 && fa != 0.0 && fb != 0.0))
    jit_error_float_underflow();
  return float8_to_datum(r);
}

int64 jit_float48div(int64 a, int64 b) {
  float8 fa = (float8)DatumGetFloat4((Datum)a);
  float8 fb = datum_to_float8(b);
  if (unlikely(fb == 0.0))
    jit_error_division_by_zero();
  float8 r = fa / fb;
  if (unlikely(isinf(r)) && !isinf(fa))
    jit_error_float_overflow();
  if (unlikely(r == 0.0 && fa != 0.0))
    jit_error_float_underflow();
  return float8_to_datum(r);
}

#define DEF_FLOAT84_ADDSUB(name, op) \
int64 jit_##name(int64 a, int64 b) { \
  float8 fa = datum_to_float8(a); \
  float8 fb = (float8)DatumGetFloat4((Datum)b); \
  float8 r = fa op fb; \
  if (unlikely(isinf(r)) && !isinf(fa) && !isinf(fb)) \
    jit_error_float_overflow(); \
  return float8_to_datum(r); \
}
DEF_FLOAT84_ADDSUB(float84pl, +)
DEF_FLOAT84_ADDSUB(float84mi, -)

int64 jit_float84mul(int64 a, int64 b) {
  float8 fa = datum_to_float8(a);
  float8 fb = (float8)DatumGetFloat4((Datum)b);
  float8 r = fa * fb;
  if (unlikely(isinf(r)) && !isinf(fa) && !isinf(fb))
    jit_error_float_overflow();
  if (unlikely(r == 0.0 && fa != 0.0 && fb != 0.0))
    jit_error_float_underflow();
  return float8_to_datum(r);
}

int64 jit_float84div(int64 a, int64 b) {
  float8 fa = datum_to_float8(a);
  float8 fb = (float8)DatumGetFloat4((Datum)b);
  if (unlikely(fb == 0.0))
    jit_error_division_by_zero();
  float8 r = fa / fb;
  if (unlikely(isinf(r)) && !isinf(fa))
    jit_error_float_overflow();
  if (unlikely(r == 0.0 && fa != 0.0))
    jit_error_float_underflow();
  return float8_to_datum(r);
}

/* Extended hash functions use DEFERRED — call PG's V1 functions directly.
 * Getting the exact hash algorithm right is critical for hash partitioning
 * correctness, so we don't re-implement. */

/* ================================================================
 * TIER 1: bool comparison + aggregates (8 functions)
 * bool is stored as Datum: 0 or 1.
 * ================================================================ */

int32 jit_booleq(int64 a, int64 b) { return (a == b) ? 1 : 0; }
int32 jit_boolne(int64 a, int64 b) { return (a != b) ? 1 : 0; }
int32 jit_boollt(int64 a, int64 b) { return (a < b) ? 1 : 0; }
int32 jit_boolgt(int64 a, int64 b) { return (a > b) ? 1 : 0; }
int32 jit_boolle(int64 a, int64 b) { return (a <= b) ? 1 : 0; }
int32 jit_boolge(int64 a, int64 b) { return (a >= b) ? 1 : 0; }
int64 jit_booland_statefunc(int64 a, int64 b) {
  return (a != 0 && b != 0) ? 1 : 0;
}
int64 jit_boolor_statefunc(int64 a, int64 b) {
  return (a != 0 || b != 0) ? 1 : 0;
}

/* ================================================================
 * TIER 1: timestamp/timestamptz comparison (14 functions)
 * Timestamp = int64 microseconds since 2000-01-01.
 * timestamp_cmp_internal is just int64 comparison.
 * ================================================================ */

int32 jit_timestamp_eq(int64 a, int64 b) { return (a == b) ? 1 : 0; }
int32 jit_timestamp_ne(int64 a, int64 b) { return (a != b) ? 1 : 0; }
int32 jit_timestamp_lt(int64 a, int64 b) { return (a < b) ? 1 : 0; }
int32 jit_timestamp_le(int64 a, int64 b) { return (a <= b) ? 1 : 0; }
int32 jit_timestamp_gt(int64 a, int64 b) { return (a > b) ? 1 : 0; }
int32 jit_timestamp_ge(int64 a, int64 b) { return (a >= b) ? 1 : 0; }
int32 jit_timestamp_cmp(int64 a, int64 b) {
  return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

/* timestamptz is same representation */
#define jit_timestamptz_eq jit_timestamp_eq
#define jit_timestamptz_ne jit_timestamp_ne
#define jit_timestamptz_lt jit_timestamp_lt
#define jit_timestamptz_le jit_timestamp_le
#define jit_timestamptz_gt jit_timestamp_gt
#define jit_timestamptz_ge jit_timestamp_ge
#define jit_timestamptz_cmp jit_timestamp_cmp

/* timestamp min/max */
int64 jit_timestamp_larger(int64 a, int64 b) { return (a >= b) ? a : b; }
int64 jit_timestamp_smaller(int64 a, int64 b) { return (a <= b) ? a : b; }
#define jit_timestamptz_larger jit_timestamp_larger
#define jit_timestamptz_smaller jit_timestamp_smaller

/* timestamp hash = hashint8 */
/* (use the same jit_hashint8 defined below) */

/* ================================================================
 * TIER 1: date comparison + arithmetic (12 functions)
 * DateADT = int32 (days since 2000-01-01).
 * ================================================================ */

DEF_CMP(date_eq, int32, ==)
DEF_CMP(date_ne, int32, !=)
DEF_CMP(date_lt, int32, <)
DEF_CMP(date_le, int32, <=)
DEF_CMP(date_gt, int32, >)
DEF_CMP(date_ge, int32, >=)

int32 jit_date_larger(int32 a, int32 b) { return (a >= b) ? a : b; }
int32 jit_date_smaller(int32 a, int32 b) { return (a <= b) ? a : b; }

/* date + int4 = date, date - int4 = date, date - date = int4 */
int32 jit_date_pli(int32 date, int32 days) {
  int32 r;
  if (unlikely(pg_add_s32_overflow(date, days, &r)))
    jit_error_int4_overflow();
  return r;
}

int32 jit_date_mii(int32 date, int32 days) {
  int32 r;
  if (unlikely(pg_sub_s32_overflow(date, days, &r)))
    jit_error_int4_overflow();
  return r;
}

int32 jit_date_mi(int32 a, int32 b) {
  int32 r;
  if (unlikely(pg_sub_s32_overflow(a, b, &r)))
    jit_error_int4_overflow();
  return r;
}

/* ================================================================
 * TIER 1: OID comparison (8 functions)
 * OID = uint32, stored as Datum.
 * ================================================================ */

int32 jit_oideq(int32 a, int32 b) { return ((uint32)a == (uint32)b) ? 1 : 0; }
int32 jit_oidne(int32 a, int32 b) { return ((uint32)a != (uint32)b) ? 1 : 0; }
int32 jit_oidlt(int32 a, int32 b) { return ((uint32)a < (uint32)b) ? 1 : 0; }
int32 jit_oidle(int32 a, int32 b) { return ((uint32)a <= (uint32)b) ? 1 : 0; }
int32 jit_oidgt(int32 a, int32 b) { return ((uint32)a > (uint32)b) ? 1 : 0; }
int32 jit_oidge(int32 a, int32 b) { return ((uint32)a >= (uint32)b) ? 1 : 0; }
int32 jit_oidlarger(int32 a, int32 b) {
  return ((uint32)a >= (uint32)b) ? a : b;
}
int32 jit_oidsmaller(int32 a, int32 b) {
  return ((uint32)a <= (uint32)b) ? a : b;
}

/* ================================================================
 * TIER 1: Hash functions (10 functions)
 * All return uint32 (stored as Datum).
 * ================================================================ */

int32 jit_hashint2(int64 a) {
  return (int32)hash_bytes_uint32((uint32)(int16)a);
}

int32 jit_hashint4(int32 a) { return (int32)hash_bytes_uint32((uint32)a); }

int64 jit_hashint8(int64 val) {
  uint32 lohalf = (uint32)val;
  uint32 hihalf = (uint32)(val >> 32);
  lohalf ^= (val >= 0) ? hihalf : ~hihalf;
  return (int64)(uint32)hash_bytes_uint32(lohalf);
}

int32 jit_hashoid(int32 a) { return (int32)hash_bytes_uint32((uint32)a); }

int32 jit_hashbool(int64 a) {
  return (int32)hash_bytes_uint32((uint32)(a != 0));
}

int32 jit_hashdate(int32 a) { return (int32)hash_bytes_uint32((uint32)a); }

/* timestamp_hash and timestamptz_hash just call hashint8 */
#define jit_timestamp_hash jit_hashint8
#define jit_timestamptz_hash jit_hashint8

/* ================================================================
 * TIER 1: Aggregate COUNT/SUM helpers (7 functions)
 * ================================================================ */

/* int8inc: used for COUNT(*) — arg is current count (int64) */
int64 jit_int8inc_any(int64 a, int64 dummy) {
  (void)dummy;
  return jit_int8inc(a);
}

int64 jit_int8dec_any(int64 a, int64 dummy) {
  (void)dummy;
  return jit_int8dec(a);
}

/* int2_sum: accumulates into int64 */
int64 jit_int2_sum(int64 oldsum, int64 newval) {
  int64 r;
  /* newval is actually int16 stored as Datum */
  if (unlikely(pg_add_s64_overflow(oldsum, (int64)(int16)newval, &r)))
    jit_error_int8_overflow();
  return r;
}

/* int4_sum: accumulates int32 values into int64 */
int64 jit_int4_sum(int64 oldsum, int64 newval) {
  int64 r;
  /* newval is actually int32 stored as Datum */
  if (unlikely(pg_add_s64_overflow(oldsum, (int64)(int32)newval, &r)))
    jit_error_int8_overflow();
  return r;
}

/*
 * int4_avg_accum: transition function for avg(int4).
 * Trans state is ArrayType* containing Int8TransTypeData = {count, sum}.
 * In aggregate context (always true here), modifies array in-place.
 * Args: trans_datum (pointer to ArrayType), newval (int32).
 * Returns: same trans_datum (modified in-place).
 */
int64 jit_int4_avg_accum(int64 trans_datum, int32 newval) {
  /* ArrayType header: vl_len, ndim, dataoffset, elemtype, ...
   * Data starts at ARR_DATA_PTR = (char*)array + ARR_OVERHEAD_NONULLS(1)
   * ARR_OVERHEAD_NONULLS(1) = sizeof(ArrayType) + sizeof(int) (one dim)
   * = 12 + 4 = 16 on 32-bit, 16 on 64-bit (packed struct) */
  char *arr = (char *)(uintptr_t)trans_datum;
  /* Int8TransTypeData starts at offset ARR_OVERHEAD_NONULLS(1) */
  int64 *count_ptr = (int64 *)(arr + ARR_OVERHEAD_NONULLS(1));
  int64 *sum_ptr = count_ptr + 1;
  (*count_ptr)++;
  *sum_ptr += (int64)newval;
  return trans_datum;  /* same pointer, modified in-place */
}

/*
 * int8_avg_accum: transition function for avg(int8).
 * Same layout as int4_avg_accum but input is int64.
 */
int64 jit_int8_avg_accum(int64 trans_datum, int64 newval) {
  char *arr = (char *)(uintptr_t)trans_datum;
  int64 *count_ptr = (int64 *)(arr + ARR_OVERHEAD_NONULLS(1));
  int64 *sum_ptr = count_ptr + 1;
  (*count_ptr)++;
  *sum_ptr += newval;
  return trans_datum;
}

/* int8_sum: accumulates int64 values into numeric — too complex, skip */

/* ================================================================
 * TIER 1: Interval comparison (8 functions)
 * Interval = { TimeOffset time, int32 day, int32 month }
 * Comparison uses INT128 arithmetic: span = time + days*USECS_PER_DAY
 * where days = month*30 + day.
 * ================================================================ */

static inline int jit_interval_cmp_internal(int64 a_ptr, int64 b_ptr) {
  const Interval *a = (const Interval *)a_ptr;
  const Interval *b = (const Interval *)b_ptr;
  INT128 span1, span2;
  int64 days1, days2;

  days1 = (int64)a->month * INT64CONST(30) + a->day;
  span1 = int64_to_int128(a->time);
  int128_add_int64_mul_int64(&span1, days1, USECS_PER_DAY);

  days2 = (int64)b->month * INT64CONST(30) + b->day;
  span2 = int64_to_int128(b->time);
  int128_add_int64_mul_int64(&span2, days2, USECS_PER_DAY);

  return int128_compare(span1, span2);
}

int32 jit_interval_eq(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) == 0) ? 1 : 0;
}
int32 jit_interval_ne(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) != 0) ? 1 : 0;
}
int32 jit_interval_lt(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) < 0) ? 1 : 0;
}
int32 jit_interval_le(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) <= 0) ? 1 : 0;
}
int32 jit_interval_gt(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) > 0) ? 1 : 0;
}
int32 jit_interval_ge(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) >= 0) ? 1 : 0;
}
int32 jit_interval_cmp(int64 a, int64 b) {
  return jit_interval_cmp_internal(a, b);
}

/* interval min/max: return the pointer to the smaller/larger interval */
int64 jit_interval_smaller(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) <= 0) ? a : b;
}
int64 jit_interval_larger(int64 a, int64 b) {
  return (jit_interval_cmp_internal(a, b) >= 0) ? a : b;
}

/* interval_hash: compute INT128 span, take low 64 bits, hash as int8 */
int32 jit_interval_hash(int64 a_ptr) {
  const Interval *a = (const Interval *)a_ptr;
  INT128 span;
  int64 days, span64;

  days = (int64)a->month * INT64CONST(30) + a->day;
  span = int64_to_int128(a->time);
  int128_add_int64_mul_int64(&span, days, USECS_PER_DAY);
  span64 = int128_to_int64(span);

  return (int32)jit_hashint8(span64);
}

/* ================================================================
 * TIER 1: Interval / timestamp arithmetic (4 functions)
 *
 * For intervals with month=0, arithmetic is just:
 *   result = timestamp + (day * USECS_PER_DAY + time)
 * We inline this fast path. For month != 0, delegate to PG's
 * complex calendar arithmetic (handles variable month lengths).
 * ================================================================ */

#define USECS_PER_DAY_CONST INT64CONST(86400000000)

int64 jit_interval_pl(int64 a, int64 b) {
  return (int64)DirectFunctionCall2(interval_pl, (Datum)a, (Datum)b);
}

int64 jit_interval_mi(int64 a, int64 b) {
  return (int64)DirectFunctionCall2(interval_mi, (Datum)a, (Datum)b);
}

int64 jit_timestamp_pl_interval(int64 ts, int64 span_ptr) {
  const Interval *span = (const Interval *)DatumGetPointer((Datum)span_ptr);

  /* Fast path: no month component — pure arithmetic */
  if (span->month == 0) {
    int64 result = ts;
    result += (int64)span->day * USECS_PER_DAY_CONST;
    result += span->time;
    return result;
  }

  return (int64)DirectFunctionCall2(timestamp_pl_interval, (Datum)ts,
                                    (Datum)span_ptr);
}

int64 jit_timestamp_mi_interval(int64 ts, int64 span_ptr) {
  const Interval *span = (const Interval *)DatumGetPointer((Datum)span_ptr);

  /* Fast path: no month component — pure arithmetic */
  if (span->month == 0) {
    int64 result = ts;
    result -= (int64)span->day * USECS_PER_DAY_CONST;
    result -= span->time;
    return result;
  }

  return (int64)DirectFunctionCall2(timestamp_mi_interval, (Datum)ts,
                                    (Datum)span_ptr);
}

/* ================================================================
 * TIER 1: hashfloat4 / hashfloat8 (2 functions)
 * Must handle -0.0 → 0 hash and NaN normalization.
 * ================================================================ */

int32 jit_hashfloat4(int64 datum) {
  union {
    int32 i;
    float f;
  } u;
  float8 key8;

  u.i = (int32)datum; /* float4 stored as int32 in Datum */
  if (u.f == (float4)0)
    return 0;
  key8 = (float8)u.f;
  if (isnan(key8))
    key8 = get_float8_nan();
  return (int32)hash_any((unsigned char *)&key8, sizeof(key8));
}

int32 jit_hashfloat8(int64 datum) {
  union {
    int64 i;
    float8 f;
  } u;

  u.i = datum;
  if (u.f == (float8)0)
    return 0;
  if (isnan(u.f))
    u.f = get_float8_nan();
  return (int32)hash_any((unsigned char *)&u.f, sizeof(u.f));
}

/* ================================================================
 * TIER 1: Text comparison wrappers (collation-aware)
 *
 * These take fcinfo->fncollation as an explicit last argument,
 * bypassing PG_GET_COLLATION() which requires fcinfo.
 * ================================================================ */

/* Free detoasted copy if different from original datum */
#define JIT_FREE_IF_COPY(ptr, datum)                                           \
  do {                                                                         \
    if ((Pointer)(ptr) != DatumGetPointer(datum))                              \
      pfree(ptr);                                                              \
  } while (0)

static inline int jit_text_cmp_internal(Datum a, Datum b, Oid collid) {
  text *t1 = DatumGetTextPP(a);
  text *t2 = DatumGetTextPP(b);
  int result = varstr_cmp(VARDATA_ANY(t1), VARSIZE_ANY_EXHDR(t1),
                          VARDATA_ANY(t2), VARSIZE_ANY_EXHDR(t2), collid);
  JIT_FREE_IF_COPY(t1, a);
  JIT_FREE_IF_COPY(t2, b);
  return result;
}

int32 jit_texteq(int64 a, int64 b, int32 collid) {
  Datum da = (Datum)a, db = (Datum)b;
  Size len1, len2;

  /*
   * For deterministic collations (including C, en_US.UTF-8, etc.), equality
   * can be determined by raw byte comparison — no need for the expensive
   * locale-aware varstr_cmp / pg_strncoll.  This mirrors PG's texteq().
   */
  len1 = toast_raw_datum_size(da);
  len2 = toast_raw_datum_size(db);
  if (len1 != len2)
    return 0;

  {
    pg_locale_t locale = pg_newlocale_from_collation((Oid)collid);
    if (locale->deterministic) {
      text *t1 = DatumGetTextPP(da);
      text *t2 = DatumGetTextPP(db);
      int result = (memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2),
                           len1 - VARHDRSZ) == 0) ? 1 : 0;
      JIT_FREE_IF_COPY(t1, da);
      JIT_FREE_IF_COPY(t2, db);
      return result;
    }
  }
  return jit_text_cmp_internal(da, db, (Oid)collid) == 0 ? 1 : 0;
}

int32 jit_textne(int64 a, int64 b, int32 collid) {
  Datum da = (Datum)a, db = (Datum)b;
  Size len1, len2;

  len1 = toast_raw_datum_size(da);
  len2 = toast_raw_datum_size(db);
  if (len1 != len2)
    return 1;

  {
    pg_locale_t locale = pg_newlocale_from_collation((Oid)collid);
    if (locale->deterministic) {
      text *t1 = DatumGetTextPP(da);
      text *t2 = DatumGetTextPP(db);
      int result = (memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2),
                           len1 - VARHDRSZ) != 0) ? 1 : 0;
      JIT_FREE_IF_COPY(t1, da);
      JIT_FREE_IF_COPY(t2, db);
      return result;
    }
  }
  return jit_text_cmp_internal(da, db, (Oid)collid) != 0 ? 1 : 0;
}

/*
 * jit_text_datum_eq / jit_text_datum_ne — lean 2-arg text equality.
 *
 * Called from JIT inline path when collation is known-deterministic at
 * compile time.  Skips pg_newlocale_from_collation (the biggest overhead
 * in jit_texteq for short strings) and takes only 2 args (no collation).
 * Caller already checked pointer inequality (a != b).
 */
int32 jit_text_datum_eq(int64 a, int64 b) {
  uint8 ha = *(uint8 *)a, hb = *(uint8 *)b;

  /* Fast path: both short 1-byte header */
  if ((ha & 1) && ha > 1 && (hb & 1) && hb > 1) {
    uint32 len;
    if (ha != hb)
      return 0; /* different lengths */
    len = (ha >> 1) - 1;
    return len == 0 || memcmp((char *)a + 1, (char *)b + 1, len) == 0;
  }

  /* Slow path: 4-byte header, toasted, or compressed.
   * Deterministic collation → raw byte comparison is correct. */
  {
    Size len1 = toast_raw_datum_size((Datum)a);
    Size len2 = toast_raw_datum_size((Datum)b);
    if (len1 != len2)
      return 0;
    {
      text *t1 = DatumGetTextPP((Datum)a);
      text *t2 = DatumGetTextPP((Datum)b);
      int32 result =
          memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2), len1 - VARHDRSZ) == 0;
      JIT_FREE_IF_COPY(t1, (Datum)a);
      JIT_FREE_IF_COPY(t2, (Datum)b);
      return result;
    }
  }
}

int32 jit_text_datum_ne(int64 a, int64 b) {
  return !jit_text_datum_eq(a, b);
}

int32 jit_text_lt(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) < 0 ? 1 : 0;
}
int32 jit_text_le(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) <= 0 ? 1 : 0;
}
int32 jit_text_gt(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) > 0 ? 1 : 0;
}
int32 jit_text_ge(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) >= 0 ? 1 : 0;
}
int32 jit_bttextcmp(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid);
}
int64 jit_text_larger(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) >= 0 ? a : b;
}
int64 jit_text_smaller(int64 a, int64 b, int32 collid) {
  return jit_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) <= 0 ? a : b;
}

/* ================================================================
 * TIER 1: Text functions (no collation needed)
 * ================================================================ */

int32 jit_textlen(int64 a) {
  text *t = DatumGetTextPP((Datum)a);
  int32 result = pg_mbstrlen_with_len(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
  JIT_FREE_IF_COPY(t, (Datum)a);
  return result;
}

int32 jit_textoctetlen(int64 a) {
  /* toast_raw_datum_size returns on-disk size including VARHDRSZ */
  return (int32)(toast_raw_datum_size((Datum)a) - VARHDRSZ);
}

/* text_pattern_* comparison: raw byte-wise (no collation) */
static inline int jit_text_pattern_cmp_internal(Datum a, Datum b) {
  text *t1 = DatumGetTextPP(a);
  text *t2 = DatumGetTextPP(b);
  int len1 = VARSIZE_ANY_EXHDR(t1);
  int len2 = VARSIZE_ANY_EXHDR(t2);
  int result = memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2), Min(len1, len2));
  if (result == 0)
    result = (len1 < len2) ? -1 : ((len1 > len2) ? 1 : 0);
  JIT_FREE_IF_COPY(t1, a);
  JIT_FREE_IF_COPY(t2, b);
  return result;
}

int32 jit_text_pattern_lt(int64 a, int64 b) {
  return jit_text_pattern_cmp_internal((Datum)a, (Datum)b) < 0 ? 1 : 0;
}
int32 jit_text_pattern_le(int64 a, int64 b) {
  return jit_text_pattern_cmp_internal((Datum)a, (Datum)b) <= 0 ? 1 : 0;
}
int32 jit_text_pattern_ge(int64 a, int64 b) {
  return jit_text_pattern_cmp_internal((Datum)a, (Datum)b) >= 0 ? 1 : 0;
}
int32 jit_text_pattern_gt(int64 a, int64 b) {
  return jit_text_pattern_cmp_internal((Datum)a, (Datum)b) > 0 ? 1 : 0;
}
int32 jit_bttext_pattern_cmp(int64 a, int64 b) {
  return jit_text_pattern_cmp_internal((Datum)a, (Datum)b);
}

/* ================================================================
 * LOOKUP TABLE
 *
 * Maps PG function address → direct-call entry.
 * Tier 2 entries have jit_fn = NULL (deferred to LLVM IR inlining).
 * Tier 3 entries are listed as comments.
 * ================================================================ */

#define T32 JIT_TYPE_32
#define T64 JIT_TYPE_64

/* Shorthand: E<nargs>(pg_fn, jit_fn, ret_type, arg_types...)
 * The stringified jit_fn name is stored for precompiled blob lookup. */
#define E0(pg, jf, rt) {(PGFunction)(pg), (void *)(jf), 0, rt, {0}, 0, 0, #jf}
#define E1(pg, jf, rt, a0)                                                     \
  {(PGFunction)(pg), (void *)(jf), 1, rt, {a0}, 0, 0, #jf}
#define E2(pg, jf, rt, a0, a1)                                                 \
  {(PGFunction)(pg), (void *)(jf), 2, rt, {a0, a1}, 0, 0, #jf}
/* EI1: 1-arg entry with inline_op tag for sljit inlining */
#define EI1(pg, jf, rt, a0, iop)                                               \
  {(PGFunction)(pg), (void *)(jf), 1, rt, {a0}, iop, 0, #jf}
/* EI2: 2-arg entry with inline_op tag for sljit inlining */
#define EI2(pg, jf, rt, a0, a1, iop)                                           \
  {(PGFunction)(pg), (void *)(jf), 2, rt, {a0, a1}, iop, 0, #jf}
/* EC<nargs>: collation-aware entry — passes fcinfo->fncollation as last arg */
#define EC1(pg, jf, rt, a0)                                                    \
  {(PGFunction)(pg), (void *)(jf), 1, rt, {a0}, 0, JIT_FN_FLAG_COLLATION, #jf}
#define EC2(pg, jf, rt, a0, a1)                                                \
  {(PGFunction)(pg),      (void *)(jf), 2, rt, {a0, a1}, 0,                    \
   JIT_FN_FLAG_COLLATION, #jf}
/* EIC2: 2-arg collation-aware entry with inline_op tag (jit_fn = DEFERRED) */
#define EIC2(pg, rt, a0, a1, iop)                                              \
  {(PGFunction)(pg), DEFERRED, 2, rt, {a0, a1}, iop,                           \
   JIT_FN_FLAG_COLLATION, "DEFERRED"}

/* NULL means no native implementation yet — fall through to fcinfo path */
#define DEFERRED NULL

const JitDirectFn jit_direct_fns[] = {

    /* ---- int4 arithmetic ---- */
    EI2(int4pl, jit_int4pl, T32, T32, T32, JIT_INLINE_INT4_ADD),
    EI2(int4mi, jit_int4mi, T32, T32, T32, JIT_INLINE_INT4_SUB),
    EI2(int4mul, jit_int4mul, T32, T32, T32, JIT_INLINE_INT4_MUL),
    EI2(int4div, jit_int4div, T32, T32, T32, JIT_INLINE_INT4_DIV),
    EI2(int4mod, jit_int4mod, T32, T32, T32, JIT_INLINE_INT4_MOD),
    E1(int4abs, jit_int4abs, T32, T32),
    E1(int4inc, jit_int4inc, T32, T32),

    /* ---- int8 arithmetic ---- */
    EI2(int8pl, jit_int8pl, T64, T64, T64, JIT_INLINE_INT8_ADD),
    EI2(int8mi, jit_int8mi, T64, T64, T64, JIT_INLINE_INT8_SUB),
    EI2(int8mul, jit_int8mul, T64, T64, T64, JIT_INLINE_INT8_MUL),
    E2(int8div, jit_int8div, T64, T64, T64),
    E2(int8mod, jit_int8mod, T64, T64, T64),
    E1(int8abs, jit_int8abs, T64, T64),
    E1(int8inc, jit_int8inc, T64, T64),
    E1(int8dec, jit_int8dec, T64, T64),

    /* ---- int2 arithmetic ---- */
    E2(int2pl, jit_int2pl, T32, T32, T32),
    E2(int2mi, jit_int2mi, T32, T32, T32),
    E2(int2mul, jit_int2mul, T32, T32, T32),
    E2(int2div, jit_int2div, T32, T32, T32),
    E2(int2mod, jit_int2mod, T32, T32, T32),
    E1(int2abs, jit_int2abs, T32, T32),

    /* ---- int4 comparison ---- */
    EI2(int4eq, jit_int4eq, T32, T32, T32, JIT_INLINE_INT4_EQ),
    EI2(int4ne, jit_int4ne, T32, T32, T32, JIT_INLINE_INT4_NE),
    EI2(int4lt, jit_int4lt, T32, T32, T32, JIT_INLINE_INT4_LT),
    EI2(int4le, jit_int4le, T32, T32, T32, JIT_INLINE_INT4_LE),
    EI2(int4gt, jit_int4gt, T32, T32, T32, JIT_INLINE_INT4_GT),
    EI2(int4ge, jit_int4ge, T32, T32, T32, JIT_INLINE_INT4_GE),

    /* ---- int8 comparison ---- */
    EI2(int8eq, jit_int8eq, T32, T64, T64, JIT_INLINE_INT8_EQ),
    EI2(int8ne, jit_int8ne, T32, T64, T64, JIT_INLINE_INT8_NE),
    EI2(int8lt, jit_int8lt, T32, T64, T64, JIT_INLINE_INT8_LT),
    EI2(int8le, jit_int8le, T32, T64, T64, JIT_INLINE_INT8_LE),
    EI2(int8gt, jit_int8gt, T32, T64, T64, JIT_INLINE_INT8_GT),
    EI2(int8ge, jit_int8ge, T32, T64, T64, JIT_INLINE_INT8_GE),

    /* ---- int2 comparison ---- */
    E2(int2eq, jit_int2eq, T32, T32, T32),
    E2(int2ne, jit_int2ne, T32, T32, T32),
    E2(int2lt, jit_int2lt, T32, T32, T32),
    E2(int2le, jit_int2le, T32, T32, T32),
    E2(int2gt, jit_int2gt, T32, T32, T32),
    E2(int2ge, jit_int2ge, T32, T32, T32),

    /* ---- int48 comparison ---- */
    E2(int48eq, jit_int48eq, T32, T64, T64),
    E2(int48ne, jit_int48ne, T32, T64, T64),
    E2(int48lt, jit_int48lt, T32, T64, T64),
    E2(int48le, jit_int48le, T32, T64, T64),
    E2(int48gt, jit_int48gt, T32, T64, T64),
    E2(int48ge, jit_int48ge, T32, T64, T64),

    /* ---- int84 comparison ---- */
    E2(int84eq, jit_int84eq, T32, T64, T64),
    E2(int84ne, jit_int84ne, T32, T64, T64),
    E2(int84lt, jit_int84lt, T32, T64, T64),
    E2(int84le, jit_int84le, T32, T64, T64),
    E2(int84gt, jit_int84gt, T32, T64, T64),
    E2(int84ge, jit_int84ge, T32, T64, T64),

    /* ---- int24 comparison ---- */
    E2(int24eq, jit_int24eq, T32, T32, T32),
    E2(int24ne, jit_int24ne, T32, T32, T32),
    E2(int24lt, jit_int24lt, T32, T32, T32),
    E2(int24le, jit_int24le, T32, T32, T32),
    E2(int24gt, jit_int24gt, T32, T32, T32),
    E2(int24ge, jit_int24ge, T32, T32, T32),

    /* ---- int42 comparison ---- */
    E2(int42eq, jit_int42eq, T32, T32, T32),
    E2(int42ne, jit_int42ne, T32, T32, T32),
    E2(int42lt, jit_int42lt, T32, T32, T32),
    E2(int42le, jit_int42le, T32, T32, T32),
    E2(int42gt, jit_int42gt, T32, T32, T32),
    E2(int42ge, jit_int42ge, T32, T32, T32),

    /* ---- int28 comparison ---- */
    E2(int28eq, jit_int28eq, T32, T64, T64),
    E2(int28ne, jit_int28ne, T32, T64, T64),
    E2(int28lt, jit_int28lt, T32, T64, T64),
    E2(int28le, jit_int28le, T32, T64, T64),
    E2(int28gt, jit_int28gt, T32, T64, T64),
    E2(int28ge, jit_int28ge, T32, T64, T64),

    /* ---- int82 comparison ---- */
    E2(int82eq, jit_int82eq, T32, T64, T64),
    E2(int82ne, jit_int82ne, T32, T64, T64),
    E2(int82lt, jit_int82lt, T32, T64, T64),
    E2(int82le, jit_int82le, T32, T64, T64),
    E2(int82gt, jit_int82gt, T32, T64, T64),
    E2(int82ge, jit_int82ge, T32, T64, T64),

    /* ---- cross-type int arithmetic ---- */
    E2(int24pl, jit_int24pl, T32, T32, T32),
    E2(int24mi, jit_int24mi, T32, T32, T32),
    E2(int24mul, jit_int24mul, T32, T32, T32),
    E2(int24div, jit_int24div, T32, T32, T32),
    E2(int42pl, jit_int42pl, T32, T32, T32),
    E2(int42mi, jit_int42mi, T32, T32, T32),
    E2(int42mul, jit_int42mul, T32, T32, T32),
    E2(int42div, jit_int42div, T32, T32, T32),
    E2(int48pl, jit_int48pl, T64, T64, T64),
    E2(int48mi, jit_int48mi, T64, T64, T64),
    E2(int48mul, jit_int48mul, T64, T64, T64),
    E2(int48div, jit_int48div, T64, T64, T64),
    E2(int84pl, jit_int84pl, T64, T64, T64),
    E2(int84mi, jit_int84mi, T64, T64, T64),
    E2(int84mul, jit_int84mul, T64, T64, T64),
    E2(int84div, jit_int84div, T64, T64, T64),
    E2(int28pl, jit_int28pl, T64, T64, T64),
    E2(int28mi, jit_int28mi, T64, T64, T64),
    E2(int28mul, jit_int28mul, T64, T64, T64),
    E2(int28div, jit_int28div, T64, T64, T64),
    E2(int82pl, jit_int82pl, T64, T64, T64),
    E2(int82mi, jit_int82mi, T64, T64, T64),
    E2(int82mul, jit_int82mul, T64, T64, T64),
    E2(int82div, jit_int82div, T64, T64, T64),

    /* ---- int min/max ---- */
    E2(int2larger, jit_int2larger, T32, T32, T32),
    E2(int2smaller, jit_int2smaller, T32, T32, T32),
    E2(int4larger, jit_int4larger, T32, T32, T32),
    E2(int4smaller, jit_int4smaller, T32, T32, T32),
    E2(int8larger, jit_int8larger, T64, T64, T64),
    E2(int8smaller, jit_int8smaller, T64, T64, T64),

    /* ---- int bitwise ---- */
    E2(int2and, jit_int2and, T32, T32, T32),
    E2(int2or, jit_int2or, T32, T32, T32),
    E2(int2xor, jit_int2xor, T32, T32, T32),
    E1(int2not, jit_int2not, T32, T32),
    E2(int2shl, jit_int2shl, T32, T32, T32),
    E2(int2shr, jit_int2shr, T32, T32, T32),
    E2(int4and, jit_int4and, T32, T32, T32),
    E2(int4or, jit_int4or, T32, T32, T32),
    E2(int4xor, jit_int4xor, T32, T32, T32),
    E1(int4not, jit_int4not, T32, T32),
    E2(int4shl, jit_int4shl, T32, T32, T32),
    E2(int4shr, jit_int4shr, T32, T32, T32),
    E2(int8and, jit_int8and, T64, T64, T64),
    E2(int8or, jit_int8or, T64, T64, T64),
    E2(int8xor, jit_int8xor, T64, T64, T64),
    E1(int8not, jit_int8not, T64, T64),
    E2(int8shl, jit_int8shl, T64, T64, T64),
    E2(int8shr, jit_int8shr, T64, T64, T64),

    /* ---- float8 arithmetic ---- */
    EI2(float8pl, jit_float8pl, T64, T64, T64, JIT_INLINE_FLOAT8_ADD),
    EI2(float8mi, jit_float8mi, T64, T64, T64, JIT_INLINE_FLOAT8_SUB),
    EI2(float8mul, jit_float8mul, T64, T64, T64, JIT_INLINE_FLOAT8_MUL),
    EI2(float8div, jit_float8div, T64, T64, T64, JIT_INLINE_FLOAT8_DIV),
    E1(float8abs, jit_float8abs, T64, T64),
    E1(float8um, jit_float8um, T64, T64),

    /* ---- float4 arithmetic ---- */
    E2(float4pl, jit_float4pl, T64, T64, T64),
    E2(float4mi, jit_float4mi, T64, T64, T64),
    E2(float4mul, jit_float4mul, T64, T64, T64),
    E2(float4div, jit_float4div, T64, T64, T64),
    E1(float4abs, jit_float4abs, T64, T64),
    E1(float4um, jit_float4um, T64, T64),

    /* ---- float comparison ---- */
    E2(float4eq, jit_float4eq, T32, T64, T64),
    E2(float4ne, jit_float4ne, T32, T64, T64),
    E2(float4lt, jit_float4lt, T32, T64, T64),
    E2(float4le, jit_float4le, T32, T64, T64),
    E2(float4gt, jit_float4gt, T32, T64, T64),
    E2(float4ge, jit_float4ge, T32, T64, T64),
    EI2(float8eq, jit_float8eq, T32, T64, T64, JIT_INLINE_FLOAT8_EQ),
    EI2(float8ne, jit_float8ne, T32, T64, T64, JIT_INLINE_FLOAT8_NE),
    EI2(float8lt, jit_float8lt, T32, T64, T64, JIT_INLINE_FLOAT8_LT),
    EI2(float8le, jit_float8le, T32, T64, T64, JIT_INLINE_FLOAT8_LE),
    EI2(float8gt, jit_float8gt, T32, T64, T64, JIT_INLINE_FLOAT8_GT),
    EI2(float8ge, jit_float8ge, T32, T64, T64, JIT_INLINE_FLOAT8_GE),

    /* ---- float min/max ---- */
    E2(float4larger, jit_float4larger, T64, T64, T64),
    E2(float4smaller, jit_float4smaller, T64, T64, T64),
    E2(float8larger, jit_float8larger, T64, T64, T64),
    E2(float8smaller, jit_float8smaller, T64, T64, T64),

    /* ---- cross-type float comparison ---- */
    E2(float48eq, jit_float48eq, T32, T64, T64),
    E2(float48ne, jit_float48ne, T32, T64, T64),
    E2(float48lt, jit_float48lt, T32, T64, T64),
    E2(float48le, jit_float48le, T32, T64, T64),
    E2(float48gt, jit_float48gt, T32, T64, T64),
    E2(float48ge, jit_float48ge, T32, T64, T64),
    E2(float84eq, jit_float84eq, T32, T64, T64),
    E2(float84ne, jit_float84ne, T32, T64, T64),
    E2(float84lt, jit_float84lt, T32, T64, T64),
    E2(float84le, jit_float84le, T32, T64, T64),
    E2(float84gt, jit_float84gt, T32, T64, T64),
    E2(float84ge, jit_float84ge, T32, T64, T64),

    /* ---- cross-type float arithmetic ---- */
    E2(float48pl, jit_float48pl, T64, T64, T64),
    E2(float48mi, jit_float48mi, T64, T64, T64),
    E2(float48mul, jit_float48mul, T64, T64, T64),
    E2(float48div, jit_float48div, T64, T64, T64),
    E2(float84pl, jit_float84pl, T64, T64, T64),
    E2(float84mi, jit_float84mi, T64, T64, T64),
    E2(float84mul, jit_float84mul, T64, T64, T64),
    E2(float84div, jit_float84div, T64, T64, T64),

    /* ---- type cast functions (inlineable casts use EI1) ---- */
    EI1(int48, jit_int48_cast, T64, T32, JIT_INLINE_INT4_TO_INT8),
    EI1(int84, jit_int84_cast, T32, T64, JIT_INLINE_INT8_TO_INT4),
    EI1(i2toi4, jit_int24_cast, T32, T32, JIT_INLINE_INT2_TO_INT4),
    EI1(i4toi2, jit_int42_cast, T32, T32, JIT_INLINE_INT4_TO_INT2),
    EI1(int28, jit_int28_cast, T64, T32, JIT_INLINE_INT2_TO_INT8),
    EI1(int82, jit_int82_cast, T32, T64, JIT_INLINE_INT8_TO_INT2),
    EI1(ftod, jit_ftod, T64, T64, JIT_INLINE_FLOAT4_TO_FLOAT8),
    EI1(dtof, jit_dtof, T64, T64, JIT_INLINE_FLOAT8_TO_FLOAT4),
    EI1(i4tod, jit_i4tod, T64, T32, JIT_INLINE_INT4_TO_FLOAT8),
    E1(dtoi4, jit_dtoi4, T32, T64),
    E1(i4tof, jit_i4tof, T64, T32),
    E1(ftoi4, jit_ftoi4, T32, T64),
    EI1(i8tod, jit_i8tod, T64, T64, JIT_INLINE_INT8_TO_FLOAT8),
    E1(dtoi8, jit_dtoi8, T64, T64),
    E1(i2tod, jit_i2tod, T64, T32),
    E1(dtoi2, jit_dtoi2, T32, T64),
    E1(i2tof, jit_i2tof, T64, T32),
    E1(ftoi2, jit_ftoi2, T32, T64),

    /* ---- extended hash functions (DEFERRED — V1 call for correctness) ---- */
    E2(hashint4extended, DEFERRED, T64, T32, T64),
    E2(hashint8extended, DEFERRED, T64, T64, T64),
    E2(hashint2extended, DEFERRED, T64, T64, T64),
    E2(hashoidextended, DEFERRED, T64, T32, T64),
    E2(hashfloat4extended, DEFERRED, T64, T64, T64),
    E2(hashfloat8extended, DEFERRED, T64, T64, T64),

    /* ---- bool comparison + aggregates ---- */
    E2(booleq, jit_booleq, T32, T64, T64),
    E2(boolne, jit_boolne, T32, T64, T64),
    E2(boollt, jit_boollt, T32, T64, T64),
    E2(boolgt, jit_boolgt, T32, T64, T64),
    E2(boolle, jit_boolle, T32, T64, T64),
    E2(boolge, jit_boolge, T32, T64, T64),
    E2(booland_statefunc, jit_booland_statefunc, T64, T64, T64),
    E2(boolor_statefunc, jit_boolor_statefunc, T64, T64, T64),

    /* ---- timestamp comparison (same repr as int8) ---- */
    EI2(timestamp_eq, jit_timestamp_eq, T32, T64, T64, JIT_INLINE_INT8_EQ),
    EI2(timestamp_ne, jit_timestamp_ne, T32, T64, T64, JIT_INLINE_INT8_NE),
    EI2(timestamp_lt, jit_timestamp_lt, T32, T64, T64, JIT_INLINE_INT8_LT),
    EI2(timestamp_le, jit_timestamp_le, T32, T64, T64, JIT_INLINE_INT8_LE),
    EI2(timestamp_gt, jit_timestamp_gt, T32, T64, T64, JIT_INLINE_INT8_GT),
    EI2(timestamp_ge, jit_timestamp_ge, T32, T64, T64, JIT_INLINE_INT8_GE),
    E2(timestamp_cmp, jit_timestamp_cmp, T32, T64, T64),

    /*
     * timestamptz comparison reuses timestamp_* PG functions (same
     * representation), so no separate entries needed. The JIT will
     * match on fn_addr which is the same function pointer.
     */

    /* ---- timestamp min/max ---- */
    E2(timestamp_larger, jit_timestamp_larger, T64, T64, T64),
    E2(timestamp_smaller, jit_timestamp_smaller, T64, T64, T64),

    /* ---- timestamp/tz hash ---- */
    EI1(timestamp_hash, jit_timestamp_hash, T64, T64, JIT_INLINE_HASHINT8),
#if PG_VERSION_NUM >= 180000
    EI1(timestamptz_hash, jit_timestamptz_hash, T64, T64, JIT_INLINE_HASHINT8),
#endif

    /* ---- timestamp/interval arithmetic (fast path for month=0) ---- */
    E2(timestamp_pl_interval, jit_timestamp_pl_interval, T64, T64, T64),
    E2(timestamp_mi_interval, jit_timestamp_mi_interval, T64, T64, T64),

    /* ---- date comparison (same repr as int4) ---- */
    EI2(date_eq, jit_date_eq, T32, T32, T32, JIT_INLINE_INT4_EQ),
    EI2(date_ne, jit_date_ne, T32, T32, T32, JIT_INLINE_INT4_NE),
    EI2(date_lt, jit_date_lt, T32, T32, T32, JIT_INLINE_INT4_LT),
    EI2(date_le, jit_date_le, T32, T32, T32, JIT_INLINE_INT4_LE),
    EI2(date_gt, jit_date_gt, T32, T32, T32, JIT_INLINE_INT4_GT),
    EI2(date_ge, jit_date_ge, T32, T32, T32, JIT_INLINE_INT4_GE),
    E2(date_larger, jit_date_larger, T32, T32, T32),
    E2(date_smaller, jit_date_smaller, T32, T32, T32),
    E2(date_pli, jit_date_pli, T32, T32, T32),
    E2(date_mii, jit_date_mii, T32, T32, T32),
    E2(date_mi, jit_date_mi, T32, T32, T32),

    /* ---- OID comparison ---- */
    E2(oideq, jit_oideq, T32, T32, T32),
    E2(oidne, jit_oidne, T32, T32, T32),
    E2(oidlt, jit_oidlt, T32, T32, T32),
    E2(oidle, jit_oidle, T32, T32, T32),
    E2(oidgt, jit_oidgt, T32, T32, T32),
    E2(oidge, jit_oidge, T32, T32, T32),
    E2(oidlarger, jit_oidlarger, T32, T32, T32),
    E2(oidsmaller, jit_oidsmaller, T32, T32, T32),

    /* ---- hash functions ---- */
    E1(hashint2, jit_hashint2, T32, T64),
    EI1(hashint4, jit_hashint4, T32, T32, JIT_INLINE_HASHINT4),
    EI1(hashint8, jit_hashint8, T64, T64, JIT_INLINE_HASHINT8),
    EI1(hashoid, jit_hashoid, T32, T32, JIT_INLINE_HASHINT4),
#if PG_VERSION_NUM >= 180000
    E1(hashbool, jit_hashbool, T32, T64),
    EI1(hashdate, jit_hashdate, T32, T32, JIT_INLINE_HASHINT4),
#endif

    /* ---- aggregate COUNT/SUM ---- */
    E1(int8inc, jit_int8inc, T64, T64),
    E1(int8dec, jit_int8dec, T64, T64),
    E2(int8inc_any, jit_int8inc_any, T64, T64, T64),
    E2(int8dec_any, jit_int8dec_any, T64, T64, T64),
    E2(int2_sum, jit_int2_sum, T64, T64, T64),
    E2(int4_sum, jit_int4_sum, T64, T64, T64),
    E2(int4_avg_accum, jit_int4_avg_accum, T64, T64, T32),
    E2(int8_avg_accum, jit_int8_avg_accum, T64, T64, T64),

    /* ================================================================
     * TIER 2: Pass-by-reference operators — deferred (jit_fn = NULL)
     *
     * These are listed for the lookup infrastructure. When Part 2
     * (LLVM IR codegen) is implemented, native implementations will
     * be auto-generated for these entries.
     * ================================================================ */

    /* ---- interval comparison ---- */
    E2(interval_eq, jit_interval_eq, T32, T64, T64),
    E2(interval_ne, jit_interval_ne, T32, T64, T64),
    E2(interval_lt, jit_interval_lt, T32, T64, T64),
    E2(interval_le, jit_interval_le, T32, T64, T64),
    E2(interval_gt, jit_interval_gt, T32, T64, T64),
    E2(interval_ge, jit_interval_ge, T32, T64, T64),
    E2(interval_cmp, jit_interval_cmp, T32, T64, T64),
    E2(interval_smaller, jit_interval_smaller, T64, T64, T64),
    E2(interval_larger, jit_interval_larger, T64, T64, T64),
    E1(interval_hash, jit_interval_hash, T32, T64),
    E2(interval_pl, jit_interval_pl, T64, T64, T64),
    E2(interval_mi, jit_interval_mi, T64, T64, T64),
    E1(interval_um, DEFERRED, T64, T64),

    /*
     * Text/varchar comparison — SIMD-accelerated via StringZilla.
     * Uses sz_order/sz_equal for C/POSIX collation, falls back to
     * varstr_cmp for non-C/non-deterministic collations.
     * hashtext uses PG's hash_any (Jenkins lookup3) for hash join
     * correctness — must match PG's built-in hash exactly.
     */
    /*
     * texteq/textne: inline fast path for short 1-byte-header varlena
     * (pointer eq → header check → length compare → memcmp).
     * Slow path (toasted/compressed/long header) calls jit_texteq/jit_textne.
     * Non-deterministic collations: inline_op is ignored, falls to V1.
     */
    EIC2(texteq, T32, T64, T64, JIT_INLINE_TEXT_EQ),
    EIC2(textne, T32, T64, T64, JIT_INLINE_TEXT_NE),
    EC2(text_lt, DEFERRED, T32, T64, T64),
    EC2(text_le, DEFERRED, T32, T64, T64),
    EC2(text_gt, DEFERRED, T32, T64, T64),
    EC2(text_ge, DEFERRED, T32, T64, T64),
    EC2(bttextcmp, DEFERRED, T32, T64, T64),
    EC2(text_larger, DEFERRED, T64, T64, T64),
    EC2(text_smaller, DEFERRED, T64, T64, T64),
#ifdef PG_JITTER_HAVE_SIMD
    E1(hashtext, simd_hashtext, T32, T64),
#else
    E1(hashtext, DEFERRED, T32, T64),
#endif
    /* text_pattern_*: raw byte comparison, no collation needed */
    E2(text_pattern_lt, jit_text_pattern_lt, T32, T64, T64),
    E2(text_pattern_le, jit_text_pattern_le, T32, T64, T64),
    E2(text_pattern_ge, jit_text_pattern_ge, T32, T64, T64),
    E2(text_pattern_gt, jit_text_pattern_gt, T32, T64, T64),
    E2(bttext_pattern_cmp, jit_bttext_pattern_cmp, T32, T64, T64),
    E2(text_starts_with, DEFERRED, T32, T64,
       T64), /* keep DEFERRED: complex logic */
    E1(textlen, DEFERRED, T32, T64),
    E1(textoctetlen, DEFERRED, T32, T64),
    E2(nameeqtext, DEFERRED, T32, T64,
       T64), /* keep DEFERRED: name→text conversion */
    E2(texteqname, DEFERRED, T32, T64, T64),
    E2(namenetext, DEFERRED, T32, T64, T64),
    E2(textnename, DEFERRED, T32, T64, T64),

/* ---- numeric comparison + arithmetic ---- */
#ifdef PG_JITTER_HAVE_TIER2
    E2(numeric_eq, jit_numeric_eq_precompiled, T32, T64, T64),
    E2(numeric_ne, jit_numeric_ne_precompiled, T32, T64, T64),
    E2(numeric_lt, jit_numeric_lt_precompiled, T32, T64, T64),
    E2(numeric_le, jit_numeric_le_precompiled, T32, T64, T64),
    E2(numeric_gt, jit_numeric_gt_precompiled, T32, T64, T64),
    E2(numeric_ge, jit_numeric_ge_precompiled, T32, T64, T64),
    E2(numeric_cmp, jit_numeric_cmp_precompiled, T32, T64, T64),
#else
    E2(numeric_eq, DEFERRED, T32, T64, T64),
    E2(numeric_ne, DEFERRED, T32, T64, T64),
    E2(numeric_lt, DEFERRED, T32, T64, T64),
    E2(numeric_le, DEFERRED, T32, T64, T64),
    E2(numeric_gt, DEFERRED, T32, T64, T64),
    E2(numeric_ge, DEFERRED, T32, T64, T64),
    E2(numeric_cmp, DEFERRED, T32, T64, T64),
#endif
    E2(numeric_larger, DEFERRED, T64, T64, T64),
    E2(numeric_smaller, DEFERRED, T64, T64, T64),
#ifdef PG_JITTER_HAVE_TIER2
    E1(hash_numeric, jit_hash_numeric_precompiled, T32, T64),
    E2(numeric_add, jit_numeric_add_precompiled, T64, T64, T64),
    E2(numeric_sub, jit_numeric_sub_precompiled, T64, T64, T64),
    E2(numeric_mul, jit_numeric_mul_precompiled, T64, T64, T64),
#else
    E1(hash_numeric, DEFERRED, T32, T64),
    E2(numeric_add, DEFERRED, T64, T64, T64),
    E2(numeric_sub, DEFERRED, T64, T64, T64),
    E2(numeric_mul, DEFERRED, T64, T64, T64),
#endif
    E2(numeric_div, DEFERRED, T64, T64, T64),
    E2(numeric_mod, DEFERRED, T64, T64, T64),
    E1(numeric_abs, DEFERRED, T64, T64),
    E1(numeric_uminus, DEFERRED, T64, T64),
    E1(int4_numeric, DEFERRED, T64, T32),
    E1(int8_numeric, DEFERRED, T64, T64),
    E1(numeric_int4, DEFERRED, T32, T64),
    E1(numeric_int8, DEFERRED, T64, T64),
    E1(float8_numeric, DEFERRED, T64, T64),
    E1(numeric_float8, DEFERRED, T64, T64),

/* ---- uuid comparison ---- */
#ifdef PG_JITTER_HAVE_TIER2
    E2(uuid_eq, jit_uuid_eq_precompiled, T32, T64, T64),
#else
    E2(uuid_eq, DEFERRED, T32, T64, T64),
#endif
    E2(uuid_ne, DEFERRED, T32, T64, T64),
#ifdef PG_JITTER_HAVE_TIER2
    E2(uuid_lt, jit_uuid_lt_precompiled, T32, T64, T64),
#else
    E2(uuid_lt, DEFERRED, T32, T64, T64),
#endif
    E2(uuid_le, DEFERRED, T32, T64, T64),
    E2(uuid_gt, DEFERRED, T32, T64, T64),
    E2(uuid_ge, DEFERRED, T32, T64, T64),
#ifdef PG_JITTER_HAVE_TIER2
    E2(uuid_cmp, jit_uuid_cmp_precompiled, T32, T64, T64),
    E1(uuid_hash, jit_uuid_hash_precompiled, T32, T64),
#else
    E2(uuid_cmp, DEFERRED, T32, T64, T64),
    E1(uuid_hash, DEFERRED, T32, T64),
#endif

    /* ---- jsonb comparison + operators ---- */
    E2(jsonb_eq, DEFERRED, T32, T64, T64),
    E2(jsonb_ne, DEFERRED, T32, T64, T64),
    E2(jsonb_lt, DEFERRED, T32, T64, T64),
    E2(jsonb_le, DEFERRED, T32, T64, T64),
    E2(jsonb_gt, DEFERRED, T32, T64, T64),
    E2(jsonb_ge, DEFERRED, T32, T64, T64),
    E2(jsonb_cmp, DEFERRED, T32, T64, T64),
    E1(jsonb_hash, DEFERRED, T32, T64),
    E2(jsonb_exists, DEFERRED, T32, T64, T64),
    E2(jsonb_contains, DEFERRED, T32, T64, T64),
    E2(jsonb_contained, DEFERRED, T32, T64, T64),

    /* ---- jsonb accessors (non-mutating) ---- */
    E2(jsonb_object_field, DEFERRED, T64, T64, T64),
    E2(jsonb_object_field_text, DEFERRED, T64, T64, T64),
    E2(jsonb_array_element, DEFERRED, T64, T64, T32),
    E2(jsonb_array_element_text, DEFERRED, T64, T64, T32),
    E1(jsonb_array_length, DEFERRED, T32, T64),
    E1(jsonb_typeof, DEFERRED, T64, T64),

    /* ---- bytea comparison ---- */
    E2(byteaeq, DEFERRED, T32, T64, T64),
    E2(byteane, DEFERRED, T32, T64, T64),
    E2(bytealt, DEFERRED, T32, T64, T64),
    E2(byteale, DEFERRED, T32, T64, T64),
    E2(byteagt, DEFERRED, T32, T64, T64),
    E2(byteage, DEFERRED, T32, T64, T64),
    E2(byteacmp, DEFERRED, T32, T64, T64),
#if PG_VERSION_NUM >= 180000
    E2(bytea_larger, DEFERRED, T64, T64, T64),
    E2(bytea_smaller, DEFERRED, T64, T64, T64),
#endif

    /* ---- bpchar comparison ---- */
    E2(bpchareq, DEFERRED, T32, T64, T64),
    E2(bpcharne, DEFERRED, T32, T64, T64),
    E2(bpcharlt, DEFERRED, T32, T64, T64),
    E2(bpcharle, DEFERRED, T32, T64, T64),
    E2(bpchargt, DEFERRED, T32, T64, T64),
    E2(bpcharge, DEFERRED, T32, T64, T64),
    E2(bpcharcmp, DEFERRED, T32, T64, T64),
    E1(hashbpchar, DEFERRED, T32, T64),

    /* ---- array comparison + non-mutating ---- */
    E2(array_eq, DEFERRED, T32, T64, T64),
    E2(array_ne, DEFERRED, T32, T64, T64),
    E2(array_lt, DEFERRED, T32, T64, T64),
    E2(array_le, DEFERRED, T32, T64, T64),
    E2(array_gt, DEFERRED, T32, T64, T64),
    E2(array_ge, DEFERRED, T32, T64, T64),
    E2(btarraycmp, DEFERRED, T32, T64, T64),
    E1(hash_array, DEFERRED, T32, T64),
    E2(arraycontains, DEFERRED, T32, T64, T64),
    E2(arraycontained, DEFERRED, T32, T64, T64),
    E2(arrayoverlap, DEFERRED, T32, T64, T64),

    /* ---- network (inet/cidr) comparison ---- */
    E2(network_eq, DEFERRED, T32, T64, T64),
    E2(network_ne, DEFERRED, T32, T64, T64),
    E2(network_lt, DEFERRED, T32, T64, T64),
    E2(network_le, DEFERRED, T32, T64, T64),
    E2(network_gt, DEFERRED, T32, T64, T64),
    E2(network_ge, DEFERRED, T32, T64, T64),
    E2(network_cmp, DEFERRED, T32, T64, T64),
    E1(hashinet, DEFERRED, T32, T64),

    /* ---- float hash ---- */
    E1(hashfloat4, jit_hashfloat4, T32, T64),
    E1(hashfloat8, jit_hashfloat8, T32, T64),

    /* ---- numeric aggregates ---- */
    E2(numeric_accum, DEFERRED, T64, T64, T64),
    E2(numeric_avg_accum, DEFERRED, T64, T64, T64),
    E2(int2_avg_accum, DEFERRED, T64, T64, T64),
    E1(numeric_avg, DEFERRED, T64, T64),
    E1(numeric_sum, DEFERRED, T64, T64),

    /* ---- float aggregates ---- */
    E2(float8_accum, DEFERRED, T64, T64, T64),
    E2(float4_accum, DEFERRED, T64, T64, T64),
    E2(float8_combine, DEFERRED, T64, T64, T64),

    /* ---- text/string functions ---- */
    EC2(textcat, DEFERRED, T64, T64, T64),
    E1(textlen, DEFERRED, T32, T64),
    EC2(text_substr, DEFERRED, T64, T64, T32),
    EC2(text_substr_no_len, DEFERRED, T64, T64, T32),
    E1(text_reverse, DEFERRED, T64, T64),
    E1(ascii, DEFERRED, T32, T64),
    E1(chr, DEFERRED, T64, T32),
    EC2(upper, DEFERRED, T64, T64, T64),
    EC2(lower, DEFERRED, T64, T64, T64),
    EC2(initcap, DEFERRED, T64, T64, T64),
    EC2(btrim1, DEFERRED, T64, T64, T64),
    EC2(ltrim1, DEFERRED, T64, T64, T64),
    EC2(rtrim1, DEFERRED, T64, T64, T64),
    EC2(btrim, DEFERRED, T64, T64, T64),
    EC2(ltrim, DEFERRED, T64, T64, T64),
    EC2(rtrim, DEFERRED, T64, T64, T64),
    E2(text_left, DEFERRED, T64, T64, T32),
    E2(text_right, DEFERRED, T64, T64, T32),
    E2(textpos, DEFERRED, T32, T64, T64),
    E2(text_to_array, DEFERRED, T64, T64, T64),

    /* ---- array functions ---- */
    E2(array_length, DEFERRED, T32, T64, T32),
    E2(array_append, DEFERRED, T64, T64, T64),
    E2(array_prepend, DEFERRED, T64, T64, T64),
    E2(array_cat, DEFERRED, T64, T64, T64),
    E2(array_position, DEFERRED, T32, T64, T64),
    E2(array_remove, DEFERRED, T64, T64, T64),
    E1(array_ndims, DEFERRED, T32, T64),
    E1(array_dims, DEFERRED, T64, T64),
    E2(array_lower, DEFERRED, T32, T64, T32),
    E2(array_upper, DEFERRED, T32, T64, T32),
    E1(array_cardinality, DEFERRED, T32, T64),

    /* ---- date/time functions ---- */
    E2(timestamp_part, DEFERRED, T64, T64, T64),
    E2(timestamptz_part, DEFERRED, T64, T64, T64),
    E2(timestamp_trunc, DEFERRED, T64, T64, T64),
    E2(timestamptz_trunc, DEFERRED, T64, T64, T64),
    E2(timestamp_age, DEFERRED, T64, T64, T64),
    E1(date_timestamp, DEFERRED, T64, T32),
    E1(timestamptz_timestamp, DEFERRED, T64, T64),
    E1(timestamp_timestamptz, DEFERRED, T64, T64),
    E2(interval_pl, DEFERRED, T64, T64, T64),
    E2(interval_mi, DEFERRED, T64, T64, T64),
    E1(interval_um, DEFERRED, T64, T64),

    /* ---- aggregate transition functions ---- */
    E2(int8_sum, DEFERRED, T64, T64, T64),
    E2(numeric_avg_accum, DEFERRED, T64, T64, T64),
    E2(numeric_accum, DEFERRED, T64, T64, T64),

    /* ---- formatting/conversion ---- */
    E1(pg_typeof, DEFERRED, T64, T64),
    E2(timestamp_to_char, DEFERRED, T64, T64, T64),
    E2(numeric_to_number, DEFERRED, T64, T64, T64),
    E2(to_date, DEFERRED, T64, T64, T64),
    E2(to_timestamp, DEFERRED, T64, T64, T64),

    /* ---- misc string ---- */
    E2(regexp_match, DEFERRED, T64, T64, T64),
    E1(md5_text, DEFERRED, T64, T64),
    E2(binary_encode, DEFERRED, T64, T64, T64),
    E2(binary_decode, DEFERRED, T64, T64, T64),

    /* ---- math ---- */
    E1(numeric_abs, DEFERRED, T64, T64),
    E1(numeric_ceil, DEFERRED, T64, T64),
    E1(numeric_floor, DEFERRED, T64, T64),
    E2(numeric_round, DEFERRED, T64, T64, T32),
    E2(numeric_trunc, DEFERRED, T64, T64, T32),
    E2(numeric_power, DEFERRED, T64, T64, T64),
    E1(numeric_sqrt, DEFERRED, T64, T64),
    E1(numeric_exp, DEFERRED, T64, T64),
    E1(numeric_ln, DEFERRED, T64, T64),
    E2(numeric_log, DEFERRED, T64, T64, T64),
};

const int jit_direct_fns_count = lengthof(jit_direct_fns);

/*
 * TIER 3: Mutation-only functions (not in lookup table, listed for reference)
 *
 * JSONB mutation: jsonb_concat, jsonb_delete, jsonb_delete_path,
 *   jsonb_set, jsonb_insert, jsonb_strip_nulls, jsonb_pretty,
 *   jsonb_build_object, jsonb_build_array
 *
 * Text mutation: textcat, replace_text, split_part, text_to_array, text_substr
 *
 * Array mutation: array_append, array_prepend, array_cat
 */

/* ================================================================
 * Lookup function — linear scan, ~350 entries.
 * Called once per expression step at JIT compile time.
 * ================================================================ */

const JitDirectFn *jit_find_direct_fn(PGFunction pg_fn) {
#ifdef JIT_DISABLE_DIRECT_CALLS
  return NULL;
#endif
  for (int i = 0; i < jit_direct_fns_count; i++) {
    if (jit_direct_fns[i].pg_fn == pg_fn)
      return &jit_direct_fns[i];
  }
  return NULL;
}

#undef T32
#undef T64
#undef E0
#undef E1
#undef E2
#undef EI2
#undef DEFERRED
#undef DEF_CMP
