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
 * TIER 1: Numeric fast-path operations
 *
 * PG's numeric type is arbitrary-precision. But DECIMAL(15,2) — the most
 * common analytical type (TPC-H, financial) — fits in int64 as "scaled
 * integer" (value × 10^dscale). We detect short-format numerics and
 * convert to int64 for comparison, avoiding the full cmp_var_common().
 *
 * NumericShort layout: [varlena_hdr(4)] [n_header(2)] [digits(2×ndigits)]
 *   n_header bits: 1=short_flag, 1=sign, 6=dscale, 1=weight_sign, 6=weight
 *   digits: base-10000, big-endian order, leading zeros stripped
 *
 * For numeric(15,2): weight ≤ 3, ndigits ≤ 4, fits in int64 easily.
 * ================================================================ */

/*
 * Minimal numeric internal definitions (from numeric.c).
 * We only need enough to detect short-format and extract digits.
 */
typedef int16 JitNumericDigit;

struct JitNumericShort {
  uint16 n_header;
  JitNumericDigit n_data[FLEXIBLE_ARRAY_MEMBER];
};

struct JitNumericLong {
  uint16 n_sign_dscale;
  int16 n_weight;
  JitNumericDigit n_data[FLEXIBLE_ARRAY_MEMBER];
};

struct JitNumericData {
  int32 vl_len_;
  union {
    uint16 n_header;
    struct JitNumericLong n_long;
    struct JitNumericShort n_short;
  } choice;
};

typedef struct JitNumericData *JitNumeric;

/* Detoast only for truly external/compressed — avoids palloc for 1-byte headers */
#define JIT_NUMERIC(datum) ((JitNumeric)PG_DETOAST_DATUM(datum))

#define NBASE_JIT 10000
#define NUMERIC_SHORT_JIT 0x8000
#define NUMERIC_SPECIAL_JIT 0xC000
#define NUMERIC_SIGN_MASK_JIT 0xC000

/*
 * Parse a numeric Datum (any varlena header form) into components.
 * Handles 1-byte and 4-byte varlena headers WITHOUT allocation.
 * Returns false if external/compressed (needs real detoast) or not short-format.
 */
static inline bool
numeric_parse_datum(int64 datum, uint16 *phdr, JitNumericDigit **pdigits,
                    int *pndigits) {
  struct varlena *v = (struct varlena *)DatumGetPointer(datum);
  if (unlikely(VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v)))
    return false;
  char *data = VARDATA_ANY(v);
  int data_len = VARSIZE_ANY_EXHDR(v);
  if (unlikely(data_len < (int)sizeof(uint16)))
    return false;
  uint16 hdr;
  memcpy(&hdr, data, sizeof(uint16));
  if (unlikely((hdr & NUMERIC_SIGN_MASK_JIT) != NUMERIC_SHORT_JIT))
    return false;
  *phdr = hdr;
  *pndigits = (data_len - sizeof(uint16)) / sizeof(JitNumericDigit);
  *pdigits = (JitNumericDigit *)(data + sizeof(uint16));
  return true;
}

/* Extract sign, weight from a parsed header */
#define NUMERIC_HDR_NEG(hdr) (((hdr) & 0x2000) != 0)
#define NUMERIC_HDR_WEIGHT(hdr) \
  (((hdr) & 0x0040) ? (~0x003F | ((hdr) & 0x003F)) : ((hdr) & 0x003F))
#define NUMERIC_HDR_DSCALE(hdr) (((hdr) & 0x1F80) >> 7)

/*
 * Inline numeric comparison from Datum (no detoast needed).
 * Compares sign → weight → digit-by-digit, matching PG's cmp_numerics.
 * Returns -1, 0, 1 on success; -2 if fallback needed.
 */
static inline int
numeric_cmp_datum(int64 datum_a, int64 datum_b)
{
  uint16 hdr1, hdr2;
  JitNumericDigit *d1, *d2;
  int nd1, nd2;

  if (!numeric_parse_datum(datum_a, &hdr1, &d1, &nd1) ||
      !numeric_parse_datum(datum_b, &hdr2, &d2, &nd2))
    return -2;

  bool neg1 = NUMERIC_HDR_NEG(hdr1);
  bool neg2 = NUMERIC_HDR_NEG(hdr2);

  if (neg1 != neg2) {
    if (nd1 == 0 && nd2 == 0) return 0;
    return neg1 ? -1 : 1;
  }

  int16 w1 = NUMERIC_HDR_WEIGHT(hdr1);
  int16 w2 = NUMERIC_HDR_WEIGHT(hdr2);

  if (w1 != w2) {
    int cmp = (w1 > w2) ? 1 : -1;
    return neg1 ? -cmp : cmp;
  }

  int nmin = (nd1 < nd2) ? nd1 : nd2;
  for (int i = 0; i < nmin; i++) {
    if (d1[i] != d2[i]) {
      int cmp = (d1[i] > d2[i]) ? 1 : -1;
      return neg1 ? -cmp : cmp;
    }
  }

  if (nd1 > nd2) {
    for (int i = nmin; i < nd1; i++)
      if (d1[i] != 0) return neg1 ? -1 : 1;
  } else if (nd2 > nd1) {
    for (int i = nmin; i < nd2; i++)
      if (d2[i] != 0) return neg1 ? 1 : -1;
  }

  return 0;
}

/* Legacy wrapper */
static inline int
numeric_cmp_short(JitNumeric num1, JitNumeric num2)
{
  return numeric_cmp_datum(PointerGetDatum(num1), PointerGetDatum(num2));
}

int32 jit_numeric_cmp(int64 a, int64 b) {
  int r = numeric_cmp_datum(a, b);
  if (likely(r != -2)) return r;
  return (int32)DirectFunctionCall2(numeric_cmp, (Datum)a, (Datum)b);
}

#define DEF_NUMERIC_CMP(name, cond) \
int32 jit_##name(int64 a, int64 b) { \
  int r = numeric_cmp_datum(a, b); \
  if (likely(r != -2)) return (cond) ? 1 : 0; \
  return (int32)DirectFunctionCall2(name, (Datum)a, (Datum)b); \
}

DEF_NUMERIC_CMP(numeric_eq, r == 0)
DEF_NUMERIC_CMP(numeric_ne, r != 0)
DEF_NUMERIC_CMP(numeric_lt, r < 0)
DEF_NUMERIC_CMP(numeric_le, r <= 0)
DEF_NUMERIC_CMP(numeric_gt, r > 0)
DEF_NUMERIC_CMP(numeric_ge, r >= 0)

int64 jit_numeric_larger(int64 a, int64 b) {
  int r = numeric_cmp_datum(a, b);
  if (likely(r != -2)) return (r >= 0) ? a : b;
  return (int64)DirectFunctionCall2(numeric_larger, (Datum)a, (Datum)b);
}

int64 jit_numeric_smaller(int64 a, int64 b) {
  int r = numeric_cmp_datum(a, b);
  if (likely(r != -2)) return (r <= 0) ? a : b;
  return (int64)DirectFunctionCall2(numeric_smaller, (Datum)a, (Datum)b);
}

/* ================================================================
 * TIER 1: Numeric arithmetic (fast-path for short-format numerics)
 *
 * For numerics with ≤4 base-10000 digits (covers DECIMAL(15,S) for any S),
 * convert to int64 scaled value, do native arithmetic, convert back.
 *
 * The scaled value = sign * (digit[0]*10000^weight + digit[1]*10000^(weight-1) + ...)
 * normalized so that the result represents value * 10000^(max_weight+1).
 * ================================================================ */

/*
 * Convert short numeric to int64 with a known power-of-10000 scale.
 * Returns the value as int64 and sets *pweight to the weight.
 * The int64 value = digits interpreted as a big-endian base-10000 number.
 * To compare or add two numerics, they must be aligned to the same weight.
 */
/*
 * Convert a numeric Datum to int64 parts WITHOUT detoasting.
 * Handles both 1-byte and 4-byte varlena headers.
 */
static inline bool
numeric_datum_to_parts(int64 datum, int64 *pval, int *pweight, int *pndigits, bool *pneg)
{
  uint16 hdr;
  JitNumericDigit *digits;
  int ndigits;
  if (!numeric_parse_datum(datum, &hdr, &digits, &ndigits))
    return false;
  if (ndigits > 4)
    return false;

  *pneg = NUMERIC_HDR_NEG(hdr);
  *pweight = NUMERIC_HDR_WEIGHT(hdr);
  *pndigits = ndigits;

  int64 val = 0;
  for (int i = 0; i < ndigits; i++)
    val = val * NBASE_JIT + digits[i];
  *pval = val;
  return true;
}

/* Legacy wrapper for code that already has a JitNumeric pointer */
static inline bool
numeric_to_parts(JitNumeric num, int64 *pval, int *pweight, int *pndigits, bool *pneg)
{
  return numeric_datum_to_parts(PointerGetDatum(num), pval, pweight, pndigits, pneg);
}

/*
 * Construct a short-format numeric from parts.
 * val = absolute value as base-10000 digit sequence
 * weight = weight of the first digit
 * dscale = display scale (number of decimal digits after point)
 */
static Datum
numeric_from_int64(int64 val, int weight, int dscale, bool neg)
{
  /* Handle zero */
  if (val == 0) {
    int sz = VARHDRSZ + sizeof(uint16);
    JitNumeric num = (JitNumeric)palloc0(sz);
    SET_VARSIZE(num, sz);
    num->choice.n_header = NUMERIC_SHORT_JIT | ((dscale & 0x3F) << 7);
    return PointerGetDatum(num);
  }

  /* Extract base-10000 digits */
  JitNumericDigit digits[8];
  int ndigits = 0;
  int64 v = val;
  while (v > 0) {
    digits[ndigits++] = (JitNumericDigit)(v % NBASE_JIT);
    v /= NBASE_JIT;
  }

  /* Reverse to big-endian order */
  for (int i = 0; i < ndigits / 2; i++) {
    JitNumericDigit tmp = digits[i];
    digits[i] = digits[ndigits - 1 - i];
    digits[ndigits - 1 - i] = tmp;
  }

  /* Strip leading zeros */
  int start = 0;
  while (start < ndigits && digits[start] == 0) {
    start++;
    weight--;
  }

  /* Strip trailing zeros */
  int end = ndigits;
  while (end > start && digits[end - 1] == 0)
    end--;

  int final_ndigits = end - start;

  /* Build the numeric — palloc in per-tuple context (cheap bump allocator) */
  int sz = VARHDRSZ + sizeof(uint16) + final_ndigits * sizeof(JitNumericDigit);
  JitNumeric num = (JitNumeric)palloc(sz);
  SET_VARSIZE(num, sz);

  uint16 hdr = NUMERIC_SHORT_JIT;
  if (neg) hdr |= 0x2000;
  hdr |= ((dscale & 0x3F) << 7);
  if (weight < 0) hdr |= 0x0040 | (weight & 0x003F);
  else hdr |= (weight & 0x003F);
  num->choice.n_header = hdr;

  memcpy(num->choice.n_short.n_data, &digits[start],
         final_ndigits * sizeof(JitNumericDigit));
  return PointerGetDatum(num);
}

/*
 * Fast numeric add: align by weight, add digit arrays as int64, construct result.
 */
/*
 * Helper: get dscale from a numeric Datum (no detoast).
 */
static inline int numeric_datum_dscale(int64 datum) {
  uint16 hdr;
  JitNumericDigit *digits;
  int ndigits;
  if (numeric_parse_datum(datum, &hdr, &digits, &ndigits))
    return NUMERIC_HDR_DSCALE(hdr);
  return -1;
}

int64 jit_numeric_add(int64 a, int64 b) {
  int64 va, vb;
  int wa, wb, nda, ndb;
  bool nega, negb;

  if (unlikely(!numeric_datum_to_parts(a, &va, &wa, &nda, &nega) ||
               !numeric_datum_to_parts(b, &vb, &wb, &ndb, &negb)))
    return (int64)DirectFunctionCall2(numeric_add, (Datum)a, (Datum)b);

  int max_w = (wa > wb) ? wa : wb;
  while (wa < max_w) { va *= NBASE_JIT; wa++; nda++; }
  while (wb < max_w) { vb *= NBASE_JIT; wb++; ndb++; }
  int max_nd = (nda > ndb) ? nda : ndb;
  while (nda < max_nd) { va *= NBASE_JIT; nda++; }
  while (ndb < max_nd) { vb *= NBASE_JIT; ndb++; }

  if (va > PG_INT64_MAX / NBASE_JIT || vb > PG_INT64_MAX / NBASE_JIT)
    return (int64)DirectFunctionCall2(numeric_add, (Datum)a, (Datum)b);

  int64 sa = nega ? -va : va;
  int64 sb = negb ? -vb : vb;
  int64 result = sa + sb;

  int dscale_a = numeric_datum_dscale(a);
  int dscale_b = numeric_datum_dscale(b);
  int dscale = (dscale_a > dscale_b) ? dscale_a : dscale_b;

  bool result_neg = (result < 0);
  int64 result_abs = result_neg ? -result : result;
  return numeric_from_int64(result_abs, max_w, dscale, result_neg);
}

int64 jit_numeric_sub(int64 a, int64 b) {
  int64 va, vb;
  int wa, wb, nda, ndb;
  bool nega, negb;

  if (unlikely(!numeric_datum_to_parts(a, &va, &wa, &nda, &nega) ||
               !numeric_datum_to_parts(b, &vb, &wb, &ndb, &negb)))
    return (int64)DirectFunctionCall2(numeric_sub, (Datum)a, (Datum)b);

  int max_w = (wa > wb) ? wa : wb;
  while (wa < max_w) { va *= NBASE_JIT; wa++; nda++; }
  while (wb < max_w) { vb *= NBASE_JIT; wb++; ndb++; }
  int max_nd = (nda > ndb) ? nda : ndb;
  while (nda < max_nd) { va *= NBASE_JIT; nda++; }
  while (ndb < max_nd) { vb *= NBASE_JIT; ndb++; }

  if (va > PG_INT64_MAX / NBASE_JIT || vb > PG_INT64_MAX / NBASE_JIT)
    return (int64)DirectFunctionCall2(numeric_sub, (Datum)a, (Datum)b);

  int64 sa = nega ? -va : va;
  int64 sb = negb ? -vb : vb;
  int64 result = sa - sb;

  int dscale_a = numeric_datum_dscale(a);
  int dscale_b = numeric_datum_dscale(b);
  int dscale = (dscale_a > dscale_b) ? dscale_a : dscale_b;

  bool result_neg = (result < 0);
  int64 result_abs = result_neg ? -result : result;
  return numeric_from_int64(result_abs, max_w, dscale, result_neg);
}

int64 jit_numeric_mul(int64 a, int64 b) {
  int64 va, vb;
  int wa, wb, nda, ndb;
  bool nega, negb;

  if (unlikely(!numeric_datum_to_parts(a, &va, &wa, &nda, &nega) ||
               !numeric_datum_to_parts(b, &vb, &wb, &ndb, &negb)))
    return (int64)DirectFunctionCall2(numeric_mul, (Datum)a, (Datum)b);

  int result_weight = wa + wb + 1;

  if (va > 3000000000LL || vb > 3000000000LL)
    return (int64)DirectFunctionCall2(numeric_mul, (Datum)a, (Datum)b);

  int64 product = va * vb;
  bool result_neg = (nega != negb);

  int dscale_a = numeric_datum_dscale(a);
  int dscale_b = numeric_datum_dscale(b);
  int dscale = dscale_a + dscale_b;

  return numeric_from_int64(product, result_weight, dscale, result_neg);
}

/* ================================================================
 * TIER 1: Fast numeric aggregate accumulation
 *
 * PG's numeric_accum/numeric_avg_accum call init_var (decompose numeric
 * into NumericVar) + accum_sum_add for every row. For short-format
 * numerics, we skip init_var and directly add the base-10000 digits
 * to the accumulator's pos/neg digit arrays.
 *
 * This requires duplicating PG's private NumericSumAccum and
 * NumericAggState structs — fragile across PG versions.
 * ================================================================ */

/* Duplicated from numeric.c — MUST match the installed PG version */
typedef struct JitNumericSumAccum {
  int ndigits;
  int weight;
  int dscale;
  int num_uncarried;
  bool have_carry_space;
  int32 *pos_digits;
  int32 *neg_digits;
} JitNumericSumAccum;

typedef struct JitNumericAggState {
  bool calcSumX2;
  MemoryContext agg_context;
  int64 N;
  JitNumericSumAccum sumX;
  JitNumericSumAccum sumX2;
  int maxScale;
  int64 maxScaleCount;
  int64 NaNcount;
  int64 pInfcount;
  int64 nInfcount;
} JitNumericAggState;

/*
 * Fast-path numeric aggregate transition.
 * Called from JIT AGG_TRANS handler BEFORE the generic V1 path.
 *
 * Args:
 *   state_ptr — pergroup->transValue (pointer to NumericAggState)
 *   numeric_datum — fcinfo->args[1].value (numeric Datum)
 *
 * Returns 1 if fast path succeeded, 0 if generic V1 fallback needed.
 *
 * On success, updates the accumulator in-place (N++, sumX digits added,
 * maxScale/maxScaleCount updated). The caller only needs to set
 * pergroup->transValueIsNull = false.
 *
 * Falls back (returns 0) when:
 * - Value is toasted/compressed (would need memory allocation)
 * - Value is long-format, NaN, or Inf
 * - Accumulator needs carry propagation (num_uncarried == NBASE-1)
 * - Value's weight/digit range exceeds accumulator bounds (needs rescaling)
 *
 * No memory allocation on the fast path → no MemoryContextSwitchTo needed.
 */
int32 jit_numeric_agg_trans_fast(int64 state_ptr, int64 numeric_datum) {
  /* NULL state or NULL datum — fallback to V1 */
  if (unlikely(state_ptr == 0 || numeric_datum == 0))
    return 0;

  struct varlena *v = (struct varlena *)DatumGetPointer(numeric_datum);

  /* Reject external toast (on-disk or indirect) — would need I/O */
  if (unlikely(VARATT_IS_EXTERNAL(v)))
    return 0;

  /* Also reject compressed varlena — would need decompression + palloc */
  if (unlikely(VARATT_IS_COMPRESSED(v)))
    return 0;

  /*
   * Use VARDATA_ANY/VARSIZE_ANY_EXHDR — handles both 1-byte (short)
   * and 4-byte varlena headers without allocation.
   * In-tuple numerics commonly use 1-byte headers.
   */
  char *data = VARDATA_ANY(v);
  int data_len = VARSIZE_ANY_EXHDR(v);

  /* Read n_header from start of numeric data */
  if (unlikely(data_len < (int)sizeof(uint16)))
    return 0;

  uint16 hdr;
  memcpy(&hdr, data, sizeof(uint16));

  /* Only handle short-format, non-special */
  if (unlikely((hdr & NUMERIC_SIGN_MASK_JIT) != NUMERIC_SHORT_JIT))
    return 0;

  int ndigits = (data_len - sizeof(uint16)) / sizeof(JitNumericDigit);
  JitNumericAggState *state = (JitNumericAggState *)state_ptr;

  if (ndigits == 0) {
    /* Zero value — nothing to add, just update metadata */
    state->N++;
    int dscale = (hdr & 0x1F80) >> 7;
    if (dscale > state->maxScale) {
      state->maxScale = dscale;
      state->maxScaleCount = 1;
    } else if (dscale == state->maxScale) {
      state->maxScaleCount++;
    }
    return 1;
  }

  bool neg = (hdr & 0x2000) != 0;
  int16 weight = (hdr & 0x0040) ? (~0x003F | (hdr & 0x003F)) : (hdr & 0x003F);
  int dscale = (hdr & 0x1F80) >> 7;
  JitNumericDigit *digits = (JitNumericDigit *)(data + sizeof(uint16));

  /* Check carry propagation limit */
  if (state->sumX.num_uncarried >= NBASE_JIT - 1)
    return 0;

  /* Ensure accumulator has space for this weight/digit range */
  JitNumericSumAccum *accum = &state->sumX;
  if (accum->ndigits == 0 || weight > accum->weight)
    return 0;

  int idx = accum->weight - weight;
  /* Check that ALL digits fit within the accumulator array */
  if (idx + ndigits > accum->ndigits)
    return 0;

  /* Add digits directly to accumulator */
  int32 *accum_digits = neg ? accum->neg_digits : accum->pos_digits;

  for (int vi = 0; vi < ndigits; vi++) {
    accum_digits[idx] += (int32)digits[vi];
    idx++;
  }
  accum->num_uncarried++;

  if (dscale > accum->dscale)
    accum->dscale = dscale;

  /* Update aggregate state */
  state->N++;
  if (dscale > state->maxScale) {
    state->maxScale = dscale;
    state->maxScaleCount = 1;
  } else if (dscale == state->maxScale) {
    state->maxScaleCount++;
  }
  return 1;
}

/*
 * Fast-path for int2_accum/int4_accum/int8_accum:
 * STDDEV/VARIANCE on integer types.
 *
 * These use NumericAggState (NOT Int128AggState). PG converts int→numeric
 * via int64_to_numericvar + do_numeric_accum. We do it inline:
 *   1. Convert int64 → base-10000 digits on stack (max 5 digits, no alloc)
 *   2. Add digits to sumX accumulator
 *   3. If calcSumX2: compute val² via int128, convert to digits, add to sumX2
 *   4. Update N, maxScale(=0), maxScaleCount
 *
 * Returns 1 on success, 0 if V1 fallback needed.
 */
int32 jit_int_numeric_accum_fast(int64 state_ptr, int64 intval) {
  if (unlikely(state_ptr == 0))
    return 0;

  JitNumericAggState *state = (JitNumericAggState *)state_ptr;

  /* Convert int64 → base-10000 digits (max 5 digits for int64) */
  bool neg = (intval < 0);
  uint64 uval = neg ? (uint64)(-(intval + 1)) + 1 : (uint64)intval;

  if (uval == 0) {
    /* Zero: just increment N, dscale=0 */
    state->N++;
    if (0 == state->maxScale)
      state->maxScaleCount++;
    return 1;
  }

  JitNumericDigit digits[5]; /* max 5 base-10000 digits for int64 */
  int ndigits = 0;
  {
    uint64 tmp = uval;
    while (tmp > 0) {
      digits[ndigits++] = (JitNumericDigit)(tmp % NBASE_JIT);
      tmp /= NBASE_JIT;
    }
    /* Reverse to big-endian order (PG keeps trailing zeros) */
    for (int i = 0; i < ndigits / 2; i++) {
      JitNumericDigit t = digits[i];
      digits[i] = digits[ndigits - 1 - i];
      digits[ndigits - 1 - i] = t;
    }
  }
  int weight = ndigits - 1;

  /* Pre-compute val² digits if calcSumX2, so we can check bounds
   * for BOTH accumulators before modifying either. */
  JitNumericDigit sq_digits[10];
  int sq_ndigits = 0;
  int sq_weight = 0;

  if (state->calcSumX2) {
#ifdef PG_INT128_TYPE
    int128 sq = (int128)intval * (int128)intval;
    uint128 usq = (uint128)sq;
    while (usq > 0) {
      sq_digits[sq_ndigits++] = (JitNumericDigit)(usq % NBASE_JIT);
      usq /= NBASE_JIT;
    }
    for (int i = 0; i < sq_ndigits / 2; i++) {
      JitNumericDigit t = sq_digits[i];
      sq_digits[i] = sq_digits[sq_ndigits - 1 - i];
      sq_digits[sq_ndigits - 1 - i] = t;
    }
    sq_weight = sq_ndigits > 0 ? sq_ndigits - 1 : 0;
#else
    return 0;
#endif
  }

  /* Check bounds for sumX */
  JitNumericSumAccum *accumX = &state->sumX;
  if (accumX->num_uncarried >= NBASE_JIT - 1 ||
      accumX->ndigits == 0 || weight > accumX->weight)
    return 0;
  int idxX = accumX->weight - weight;
  if (idxX + ndigits > accumX->ndigits)
    return 0;

  /* Check bounds for sumX2 if needed */
  if (state->calcSumX2 && sq_ndigits > 0) {
    JitNumericSumAccum *accumX2 = &state->sumX2;
    if (accumX2->num_uncarried >= NBASE_JIT - 1 ||
        accumX2->ndigits == 0 || sq_weight > accumX2->weight)
      return 0;
    int idxX2 = accumX2->weight - sq_weight;
    if (idxX2 + sq_ndigits > accumX2->ndigits)
      return 0;
  }

  /* Both checks passed — safe to modify both accumulators */
  {
    int32 *ad = neg ? accumX->neg_digits : accumX->pos_digits;
    for (int i = 0; i < ndigits; i++)
      ad[idxX + i] += (int32)digits[i];
    accumX->num_uncarried++;
  }

  if (state->calcSumX2 && sq_ndigits > 0) {
    JitNumericSumAccum *accumX2 = &state->sumX2;
    int idxX2 = accumX2->weight - sq_weight;
    int32 *ad = accumX2->pos_digits; /* val² always positive */
    for (int i = 0; i < sq_ndigits; i++)
      ad[idxX2 + i] += (int32)sq_digits[i];
    accumX2->num_uncarried++;
  }

  state->N++;
  if (0 > state->maxScale) {
    state->maxScale = 0;
    state->maxScaleCount = 1;
  } else if (0 == state->maxScale) {
    state->maxScaleCount++;
  }
  return 1;
}

/*
 * Inline float8_accum / float4_accum helper.
 * Implements the Youngs-Cramer algorithm matching PG's float8_accum exactly.
 * Array layout: float8[3] at offset 24 from ArrayType pointer.
 *   [0] = N, [1] = Sx, [2] = Sxx
 *
 * Youngs-Cramer: N_new=N+1; Sx_new=Sx+val;
 *   if (N>0) tmp=val*N_new-Sx_new; Sxx += tmp*tmp/(N_new*N)
 *   Overflow: if Sx or Sxx becomes inf from finite inputs → NaN Sxx
 */
void jit_float8_accum_fast(int64 array_ptr, int64 newval_datum,
                           int32 is_float4) {
  double *transvalues = (double *)((char *)array_ptr + 24);
  double newval;

  if (is_float4) {
    /* float4 stored in low 32 bits of Datum */
    union { int32 i; float f; } u;
    u.i = (int32)newval_datum;
    newval = (double)u.f;
  } else {
    union { int64 i; double d; } u;
    u.i = newval_datum;
    newval = u.d;
  }

  double N = transvalues[0];
  double Sx = transvalues[1];
  double Sxx = transvalues[2];
  double oldN = N;

  N += 1.0;
  Sx += newval;
  if (oldN > 0.0) {
    double tmp = newval * N - Sx;
    Sxx += tmp * tmp / (N * oldN);

    if (isinf(Sx) || isinf(Sxx)) {
      if (!isinf(transvalues[1]) && !isinf(newval))
        float_overflow_error();
      Sxx = get_float8_nan();
    }
  } else {
    /* First input: Inf/NaN must force Sxx to NaN */
    if (isnan(newval) || isinf(newval))
      Sxx = get_float8_nan();
  }

  transvalues[0] = N;
  transvalues[1] = Sx;
  transvalues[2] = Sxx;
}

/* ================================================================
 * TIER 1: Int128 aggregate helpers (SUM/AVG/STDDEV/VARIANCE on integers)
 *
 * On 64-bit platforms with HAVE_INT128, PG uses Int128AggState for
 * integer aggregates (PolyNumAggState = Int128AggState).
 * Layout: { bool calcSumX2, int64 N, int128 sumX, int128 sumX2 }
 *
 * These helpers are called from the SLJIT AGG_TRANS handler.
 * ================================================================ */

#ifdef PG_INT128_TYPE

typedef struct JitInt128AggState {
  bool calcSumX2;
  int64 N;
  int128 sumX;
  int128 sumX2;
} JitInt128AggState;

/*
 * int8_avg_accum fast path: SUM(bigint), AVG(bigint).
 * calcSumX2 = false. Just add int64 to int128 sum.
 * Returns 1 on success, 0 if V1 fallback needed (first call, state NULL).
 */
int32 jit_int128_accum_fast(int64 state_ptr, int64 newval) {
  if (unlikely(state_ptr == 0))
    return 0;
  JitInt128AggState *state = (JitInt128AggState *)state_ptr;
  state->sumX += (int128)newval;
  state->N++;
  return 1;
}

/*
 * int2_accum/int4_accum/int8_accum fast path:
 * STDDEV/VARIANCE on integers. calcSumX2 = true.
 * Returns 1 on success, 0 if V1 fallback needed.
 */
int32 jit_int128_accum_x2_fast(int64 state_ptr, int64 newval) {
  if (unlikely(state_ptr == 0))
    return 0;
  JitInt128AggState *state = (JitInt128AggState *)state_ptr;
  int128 v = (int128)newval;

  state->sumX += v;
  state->sumX2 += v * v;
  state->N++;
  return 1;
}

#endif /* PG_INT128_TYPE */

/* jit_numeric_accum_x2_fast removed — integer STDDEV/VARIANCE now uses
 * jit_int_numeric_accum_fast which converts int64→digits inline.
 * Numeric STDDEV/VARIANCE falls through to V1. */

/*
 * numeric_larger / numeric_smaller fast path for MIN/MAX(numeric).
 * Compares two short-format numerics and returns:
 *   1 if new value should replace current (new is larger/smaller)
 *   0 if current should be kept or fallback needed
 *
 * Args: current_datum, new_datum, is_min (1=smaller, 0=larger)
 */
int32 jit_numeric_minmax_fast(int64 current_datum, int64 new_datum,
                              int32 is_min) {
  struct varlena *vc = (struct varlena *)DatumGetPointer(current_datum);
  struct varlena *vn = (struct varlena *)DatumGetPointer(new_datum);

  /* Reject external/compressed */
  if (unlikely(VARATT_IS_EXTERNAL(vc) || VARATT_IS_EXTERNAL(vn) ||
               VARATT_IS_COMPRESSED(vc) || VARATT_IS_COMPRESSED(vn)))
    return -1; /* fallback */

  char *data_c = VARDATA_ANY(vc);
  char *data_n = VARDATA_ANY(vn);
  int len_c = VARSIZE_ANY_EXHDR(vc);
  int len_n = VARSIZE_ANY_EXHDR(vn);

  if (unlikely(len_c < (int)sizeof(uint16) || len_n < (int)sizeof(uint16)))
    return -1;

  uint16 hdr_c, hdr_n;
  memcpy(&hdr_c, data_c, sizeof(uint16));
  memcpy(&hdr_n, data_n, sizeof(uint16));

  /* Both must be short-format */
  if (unlikely((hdr_c & NUMERIC_SIGN_MASK_JIT) != NUMERIC_SHORT_JIT ||
               (hdr_n & NUMERIC_SIGN_MASK_JIT) != NUMERIC_SHORT_JIT))
    return -1;

  /* Extract sign, weight, ndigits for both */
  bool neg_c = (hdr_c & 0x2000) != 0;
  bool neg_n = (hdr_n & 0x2000) != 0;

  /* Different signs: positive > negative */
  if (neg_c != neg_n) {
    bool new_is_less = neg_n; /* negative is less */
    return (is_min ? new_is_less : !new_is_less) ? 1 : 0;
  }

  int16 weight_c = (hdr_c & 0x0040) ? (~0x003F | (hdr_c & 0x003F))
                                     : (hdr_c & 0x003F);
  int16 weight_n = (hdr_n & 0x0040) ? (~0x003F | (hdr_n & 0x003F))
                                     : (hdr_n & 0x003F);

  int ndigits_c = (len_c - sizeof(uint16)) / sizeof(JitNumericDigit);
  int ndigits_n = (len_n - sizeof(uint16)) / sizeof(JitNumericDigit);

  JitNumericDigit *digits_c = (JitNumericDigit *)(data_c + sizeof(uint16));
  JitNumericDigit *digits_n = (JitNumericDigit *)(data_n + sizeof(uint16));

  /* Compare magnitude: weight first, then digit-by-digit */
  int cmp = 0;
  if (weight_c != weight_n) {
    cmp = (weight_c > weight_n) ? 1 : -1;
  } else {
    int common = (ndigits_c < ndigits_n) ? ndigits_c : ndigits_n;
    for (int i = 0; i < common; i++) {
      if (digits_c[i] != digits_n[i]) {
        cmp = (digits_c[i] > digits_n[i]) ? 1 : -1;
        break;
      }
    }
    if (cmp == 0) {
      if (ndigits_c != ndigits_n)
        cmp = (ndigits_c > ndigits_n) ? 1 : -1;
    }
  }

  /* If negative, reverse the comparison */
  if (neg_c)
    cmp = -cmp;

  /* cmp > 0 means current > new. For MIN, replace if new < current (cmp > 0).
   * For MAX, replace if new > current (cmp < 0). */
  if (is_min)
    return (cmp > 0) ? 1 : 0;
  else
    return (cmp < 0) ? 1 : 0;
}

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

/* ================================================================
 * TIER 1: Compile-time extract/date_part optimization
 *
 * When the unit string (e.g., 'year', 'month') is a compile-time constant,
 * we resolve it to an enum at JIT compile time and emit a direct call
 * to this helper, skipping the string resolution at runtime.
 *
 * PG's timestamp_part_common does:
 *   1. downcase_truncate_identifier (string alloc)
 *   2. DecodeUnits (binary search)
 *   3. timestamp2tm (decompose to y/m/d/h/m/s)
 *   4. switch on val to extract field
 *
 * We skip steps 1-2 and inline step 4.
 * ================================================================ */
#include "utils/datetime.h"    /* timestamp2tm, DTK_* constants */
#include "utils/numeric.h"     /* int64_to_numeric */
#include "utils/builtins.h"    /* cstring_to_text */
#include "parser/scansup.h"    /* downcase_truncate_identifier */

/*
 * Resolve unit string (e.g., 'year', 'month') to DTK_* enum value.
 * Called at JIT compile time. Returns -1 if unrecognized.
 */
int32 jit_resolve_timestamp_field(int64 unit_datum) {
  text *unit_text = DatumGetTextPP((Datum)unit_datum);
  char *lowunits = downcase_truncate_identifier(
      VARDATA_ANY(unit_text), VARSIZE_ANY_EXHDR(unit_text), false);
  int type, val;
  type = DecodeUnits(0, lowunits, &val);
  if (type == UNKNOWN_FIELD)
    type = DecodeSpecial(0, lowunits, &val);
  pfree(lowunits);
  if (type == UNITS || type == RESERV)
    return val;
  return -1;
}

/*
 * Fast timestamp field extraction. Called with pre-resolved field enum.
 * Returns int64 result for integer fields, or Datum for numeric fields.
 * field_type: DTK_YEAR, DTK_MONTH, DTK_DAY, DTK_HOUR, DTK_MINUTE,
 *             DTK_QUARTER, DTK_DOW, DTK_ISODOW, DTK_DOY, DTK_WEEK,
 *             DTK_MICROSEC, DTK_SECOND, DTK_EPOCH, etc.
 *
 * For extract() (retnumeric=true): returns a Numeric Datum.
 * For date_part() (retnumeric=false): returns float8 bits as int64.
 */
int64 jit_extract_timestamp(int64 ts, int32 field_val) {
  Timestamp timestamp = (Timestamp)ts;
  int64 intresult;

  if (unlikely(TIMESTAMP_NOT_FINITE(timestamp)))
    return (int64)DirectFunctionCall2(extract_timestamp,
                                     PointerGetDatum(cstring_to_text("year")),
                                     Int64GetDatum(ts));

  /*
   * Fast path for HOUR/MINUTE/SECOND/MICROSEC: pure arithmetic
   * on microseconds since midnight, no timestamp2tm needed.
   */
  if (field_val == DTK_HOUR || field_val == DTK_MINUTE ||
      field_val == DTK_MICROSEC) {
    /* time within day = ts mod USECS_PER_DAY (handle negative) */
    int64 time_part = ts % USECS_PER_DAY;
    if (time_part < 0) time_part += USECS_PER_DAY;
    switch (field_val) {
      case DTK_HOUR:     intresult = time_part / INT64CONST(3600000000); break;
      case DTK_MINUTE:   intresult = (time_part / INT64CONST(60000000)) % 60; break;
      case DTK_MICROSEC: intresult = time_part % INT64CONST(60000000); break;
      default: pg_unreachable();
    }
    return NumericGetDatum(int64_to_numeric(intresult));
  }

  /* For date fields, need full decomposition */
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  if (unlikely(timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  switch (field_val) {
    case DTK_YEAR:     intresult = tm->tm_year; break;
    case DTK_MONTH:    intresult = tm->tm_mon; break;
    case DTK_DAY:      intresult = tm->tm_mday; break;
    case DTK_QUARTER:  intresult = (tm->tm_mon - 1) / 3 + 1; break;
    case DTK_DOW:      intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)); break;
    case DTK_DOY:
      intresult = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
                - date2j(tm->tm_year, 1, 1) + 1;
      break;
    default:
      return (int64)DirectFunctionCall2(extract_timestamp,
                                       PointerGetDatum(cstring_to_text("epoch")),
                                       Int64GetDatum(ts));
  }

  return NumericGetDatum(int64_to_numeric(intresult));
}

/*
 * Same for date_part() which returns float8.
 */
int64 jit_datepart_timestamp(int64 ts, int32 field_val) {
  Timestamp timestamp = (Timestamp)ts;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    return (int64)DirectFunctionCall2(timestamp_part,
                                     PointerGetDatum(cstring_to_text("year")),
                                     Int64GetDatum(ts));

  if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  double result;
  switch (field_val) {
    case DTK_YEAR:     result = tm->tm_year; break;
    case DTK_MONTH:    result = tm->tm_mon; break;
    case DTK_DAY:      result = tm->tm_mday; break;
    case DTK_HOUR:     result = tm->tm_hour; break;
    case DTK_MINUTE:   result = tm->tm_min; break;
    case DTK_SECOND:   result = tm->tm_sec + fsec / 1000000.0; break;
    case DTK_QUARTER:  result = (tm->tm_mon - 1) / 3 + 1; break;
    case DTK_DOW:      result = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)); break;
    case DTK_DOY:
      result = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
             - date2j(tm->tm_year, 1, 1) + 1;
      break;
    case DTK_MICROSEC: result = tm->tm_sec * 1000000.0 + fsec; break;
    case DTK_MILLISEC: result = tm->tm_sec * 1000.0 + fsec / 1000.0; break;
    default:
      return (int64)DirectFunctionCall2(timestamp_part,
                                       PointerGetDatum(cstring_to_text("epoch")),
                                       Int64GetDatum(ts));
  }
  int64 r; memcpy(&r, &result, 8); return r;
}

/* ================================================================
 * TIER 1: Cross-type temporal comparisons
 *
 * date_cmp_timestamp: date * USECS_PER_DAY → timestamp, then int64 compare
 * timestamp_cmp_timestamptz: on same-offset systems, just int64 compare
 *   (PG compiles ts→tstz as ts + session_timezone offset)
 * time: int64 microseconds since midnight, simple compare
 * ================================================================ */

/* date → timestamp conversion (non-special dates only) */
static inline int64 jit_date_to_timestamp(int32 dateVal) {
  return (int64)dateVal * USECS_PER_DAY;
}

/* date vs timestamp comparison */
static inline int
jit_date_cmp_ts(int32 dateVal, int64 ts) {
  /* Special dates: fall back */
  if (unlikely(dateVal == (int32)0x7FFFFFFF || dateVal == (int32)0x80000000))
    return -2; /* sentinel: needs fallback */
  int64 dt = jit_date_to_timestamp(dateVal);
  return (dt < ts) ? -1 : (dt > ts) ? 1 : 0;
}

#define DEF_DATE_CMP_TS(name, cond) \
int32 jit_##name(int32 a, int64 b) { \
  int r = jit_date_cmp_ts(a, b); \
  if (likely(r != -2)) return (cond) ? 1 : 0; \
  return (int32)DirectFunctionCall2(name, Int32GetDatum(a), Int64GetDatum(b)); \
}
DEF_DATE_CMP_TS(date_eq_timestamp, r == 0)
DEF_DATE_CMP_TS(date_ne_timestamp, r != 0)
DEF_DATE_CMP_TS(date_lt_timestamp, r < 0)
DEF_DATE_CMP_TS(date_le_timestamp, r <= 0)
DEF_DATE_CMP_TS(date_gt_timestamp, r > 0)
DEF_DATE_CMP_TS(date_ge_timestamp, r >= 0)
int32 jit_date_cmp_timestamp(int32 a, int64 b) {
  int r = jit_date_cmp_ts(a, b);
  if (likely(r != -2)) return r;
  return (int32)DirectFunctionCall2(date_cmp_timestamp, Int32GetDatum(a), Int64GetDatum(b));
}

/* timestamp vs date (reversed) */
#define DEF_TS_CMP_DATE(name, cond) \
int32 jit_##name(int64 a, int32 b) { \
  int r = jit_date_cmp_ts(b, a); \
  if (likely(r != -2)) { r = -r; return (cond) ? 1 : 0; } \
  return (int32)DirectFunctionCall2(name, Int64GetDatum(a), Int32GetDatum(b)); \
}
DEF_TS_CMP_DATE(timestamp_eq_date, r == 0)
DEF_TS_CMP_DATE(timestamp_ne_date, r != 0)
DEF_TS_CMP_DATE(timestamp_lt_date, r < 0)
DEF_TS_CMP_DATE(timestamp_le_date, r <= 0)
DEF_TS_CMP_DATE(timestamp_gt_date, r > 0)
DEF_TS_CMP_DATE(timestamp_ge_date, r >= 0)
int32 jit_timestamp_cmp_date(int64 a, int32 b) {
  int r = jit_date_cmp_ts(b, a);
  if (likely(r != -2)) return -r;
  return (int32)DirectFunctionCall2(timestamp_cmp_date, Int64GetDatum(a), Int32GetDatum(b));
}

/* time comparisons — int64 microseconds since midnight */
int32 jit_time_eq(int64 a, int64 b) { return a == b; }
int32 jit_time_ne(int64 a, int64 b) { return a != b; }
int32 jit_time_lt(int64 a, int64 b) { return a < b; }
int32 jit_time_le(int64 a, int64 b) { return a <= b; }
int32 jit_time_gt(int64 a, int64 b) { return a > b; }
int32 jit_time_ge(int64 a, int64 b) { return a >= b; }
int32 jit_time_cmp(int64 a, int64 b) { return (a < b) ? -1 : (a > b) ? 1 : 0; }

/* ================================================================
 * TIER 1: Extended hash functions
 * Used by parallel hash joins and hash aggregation.
 * These take (value, seed) and return int64.
 * ================================================================ */
int64 jit_hashint2extended(int64 val, int64 seed) {
  return hash_bytes_uint32_extended((int32)(int16)val, (uint64)seed);
}
int64 jit_hashint4extended(int64 val, int64 seed) {
  return hash_bytes_uint32_extended((uint32)(int32)val, (uint64)seed);
}
int64 jit_hashint8extended(int64 val, int64 seed) {
  uint32 lohalf = (uint32)val;
  uint32 hihalf = (uint32)((uint64)val >> 32);
  lohalf ^= (val >= 0) ? hihalf : ~hihalf;
  return hash_bytes_uint32_extended(lohalf, (uint64)seed);
}
int64 jit_hashoidextended(int64 val, int64 seed) {
  return hash_bytes_uint32_extended((uint32)val, (uint64)seed);
}
int64 jit_hashfloat4extended(int64 val, int64 seed) {
  /* float4 stored in low 32 bits of Datum */
  float4 f;
  memcpy(&f, &val, sizeof(float4));
  if (f == 0.0f) f = 0.0f; /* -0 → +0 */
  uint32 k;
  memcpy(&k, &f, sizeof(uint32));
  return hash_bytes_uint32_extended(k, (uint64)seed);
}
int64 jit_hashfloat8extended(int64 val, int64 seed) {
  float8 f;
  memcpy(&f, &val, sizeof(float8));
  if (f == 0.0) f = 0.0; /* -0 → +0 */
  uint32 lohalf, hihalf;
  memcpy(&lohalf, &f, sizeof(uint32));
  memcpy(&hihalf, ((char*)&f) + 4, sizeof(uint32));
  lohalf ^= hihalf;
  return hash_bytes_uint32_extended(lohalf, (uint64)seed);
}
int64 jit_hashdateextended(int64 val, int64 seed) {
  return hash_bytes_uint32_extended((uint32)(int32)val, (uint64)seed);
}

/* ================================================================
 * TIER 2: numeric_abs / numeric_uminus
 * Just flip/clear the sign bit in the numeric header.
 * Must palloc a copy since the original is in the tuple.
 * ================================================================ */
int64 jit_numeric_abs(int64 datum) {
  uint16 hdr;
  JitNumericDigit *digits;
  int ndigits;
  if (!numeric_parse_datum(datum, &hdr, &digits, &ndigits))
    return (int64)DirectFunctionCall1(numeric_abs, (Datum)datum);
  /* Short-format: clear sign bit (0x2000) */
  uint16 new_hdr = hdr & ~0x2000;
  if (new_hdr == hdr)
    return datum; /* already positive */
  /* Allocate copy with cleared sign */
  struct varlena *v = (struct varlena *)DatumGetPointer(datum);
  int total_len = VARSIZE_ANY(v);
  struct varlena *copy = (struct varlena *)palloc(total_len);
  memcpy(copy, v, total_len);
  /* Patch the header in the copy */
  char *copy_data = VARDATA_ANY(copy);
  memcpy(copy_data, &new_hdr, sizeof(uint16));
  return PointerGetDatum(copy);
}

int64 jit_numeric_uminus(int64 datum) {
  uint16 hdr;
  JitNumericDigit *digits;
  int ndigits;
  if (!numeric_parse_datum(datum, &hdr, &digits, &ndigits))
    return (int64)DirectFunctionCall1(numeric_uminus, (Datum)datum);
  /* Zero: no sign flip needed */
  if (ndigits == 0)
    return datum;
  /* Short-format: toggle sign bit (0x2000) */
  uint16 new_hdr = hdr ^ 0x2000;
  struct varlena *v = (struct varlena *)DatumGetPointer(datum);
  int total_len = VARSIZE_ANY(v);
  struct varlena *copy = (struct varlena *)palloc(total_len);
  memcpy(copy, v, total_len);
  char *copy_data = VARDATA_ANY(copy);
  memcpy(copy_data, &new_hdr, sizeof(uint16));
  return PointerGetDatum(copy);
}

/* ================================================================
 * TIER 2: Type cast functions (trivial fixed-type conversions)
 * ================================================================ */

/* int widening (safe, no overflow) */
int64 jit_int8_from_int4(int32 v) { return (int64)v; }
int64 jit_int8_from_int2(int32 v) { return (int64)(int16)v; }
int64 jit_int8_from_oid(int32 v) { return (int64)(uint32)v; }

/* float from int */
int64 jit_float8_from_int4(int32 v) {
  double d = (double)v;
  int64 r; memcpy(&r, &d, 8); return r;
}
int64 jit_float8_from_int2(int32 v) {
  double d = (double)(int16)v;
  int64 r; memcpy(&r, &d, 8); return r;
}
int64 jit_float8_from_int8(int64 v) {
  double d = (double)v;
  int64 r; memcpy(&r, &d, 8); return r;
}
int64 jit_float4_from_int4(int32 v) {
  float f = (float)v;
  int32 r; memcpy(&r, &f, 4); return (int64)(uint32)r;
}
int64 jit_float4_from_int2(int32 v) {
  float f = (float)(int16)v;
  int32 r; memcpy(&r, &f, 4); return (int64)(uint32)r;
}

/* ================================================================
 * TIER 3: Btree comparison functions (return -1, 0, 1)
 * ================================================================ */
int32 jit_btint4cmp(int32 a, int32 b) { return (a > b) - (a < b); }
int32 jit_btint8cmp(int64 a, int64 b) { return (a > b) - (a < b); }
int32 jit_btint42cmp(int32 a, int32 b) { int64 la = (int64)a, lb = (int64)(int16)b; return (la > lb) - (la < lb); }
int32 jit_btint48cmp(int32 a, int64 b) { int64 la = (int64)a; return (la > b) - (la < b); }
int32 jit_btint82cmp(int64 a, int32 b) { int64 lb = (int64)(int16)b; return (a > lb) - (a < lb); }
int32 jit_btint84cmp(int64 a, int32 b) { int64 lb = (int64)b; return (a > lb) - (a < lb); }
int32 jit_btfloat4cmp(int64 a, int64 b) {
  float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
  return (fa > fb) ? 1 : (fa < fb) ? -1 : 0;
}
int32 jit_btfloat8cmp(int64 a, int64 b) {
  double da, db; memcpy(&da, &a, 8); memcpy(&db, &b, 8);
  return (da > db) ? 1 : (da < db) ? -1 : 0;
}
int32 jit_btfloat48cmp(int64 a, int64 b) {
  float fa; double db; memcpy(&fa, &a, 4); memcpy(&db, &b, 8);
  double da = (double)fa;
  return (da > db) ? 1 : (da < db) ? -1 : 0;
}
int32 jit_btfloat84cmp(int64 a, int64 b) {
  double da; float fb; memcpy(&da, &a, 8); memcpy(&fb, &b, 4);
  double db = (double)fb;
  return (da > db) ? 1 : (da < db) ? -1 : 0;
}
int32 jit_date_cmp(int32 a, int32 b) { return (a > b) - (a < b); }

/* ================================================================
 * TIER 5: Text operations
 * ================================================================ */
int32 jit_textlen(int64 datum) {
  struct varlena *v = (struct varlena *)DatumGetPointer(datum);
  if (unlikely(VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v)))
    return (int32)DirectFunctionCall1(textlen, (Datum)datum);
  return pg_mbstrlen_with_len(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
}
int32 jit_textoctetlen(int64 datum) {
  struct varlena *v = (struct varlena *)DatumGetPointer(datum);
  if (unlikely(VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v)))
    return (int32)toast_raw_datum_size((Datum)datum) - VARHDRSZ;
  return VARSIZE_ANY_EXHDR(v);
}
int32 jit_text_starts_with(int64 a, int64 b) {
  struct varlena *va = (struct varlena *)DatumGetPointer(a);
  struct varlena *vb = (struct varlena *)DatumGetPointer(b);
  if (unlikely(VARATT_IS_EXTERNAL(va) || VARATT_IS_COMPRESSED(va) ||
               VARATT_IS_EXTERNAL(vb) || VARATT_IS_COMPRESSED(vb)))
    return (int32)DirectFunctionCall2(text_starts_with, (Datum)a, (Datum)b);
  int len1 = VARSIZE_ANY_EXHDR(va);
  int len2 = VARSIZE_ANY_EXHDR(vb);
  return (len1 >= len2 && memcmp(VARDATA_ANY(va), VARDATA_ANY(vb), len2) == 0) ? 1 : 0;
}

/* ================================================================
 * TIER 7: Money (cash) type — int64 internally
 * ================================================================ */
int32 jit_cash_eq(int64 a, int64 b) { return a == b; }
int32 jit_cash_ne(int64 a, int64 b) { return a != b; }
int32 jit_cash_lt(int64 a, int64 b) { return a < b; }
int32 jit_cash_le(int64 a, int64 b) { return a <= b; }
int32 jit_cash_gt(int64 a, int64 b) { return a > b; }
int32 jit_cash_ge(int64 a, int64 b) { return a >= b; }
int32 jit_cash_cmp(int64 a, int64 b) { return (a > b) - (a < b); }
int64 jit_cash_pl(int64 a, int64 b) { return a + b; }
int64 jit_cash_mi(int64 a, int64 b) { return a - b; }
int64 jit_cashlarger(int64 a, int64 b) { return (a >= b) ? a : b; }
int64 jit_cashsmaller(int64 a, int64 b) { return (a <= b) ? a : b; }

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

  /* Fast path: no month component, finite timestamp — pure arithmetic */
  if (span->month == 0 && !TIMESTAMP_NOT_FINITE(ts) &&
      likely(span->time != PG_INT64_MIN && span->time != PG_INT64_MAX)) {
    int64 result = ts;
    result += (int64)span->day * USECS_PER_DAY_CONST;
    result += span->time;
    if (unlikely(!IS_VALID_TIMESTAMP(result)))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    return result;
  }

  return (int64)DirectFunctionCall2(timestamp_pl_interval, (Datum)ts,
                                    (Datum)span_ptr);
}

int64 jit_timestamp_mi_interval(int64 ts, int64 span_ptr) {
  const Interval *span = (const Interval *)DatumGetPointer((Datum)span_ptr);

  if (span->month == 0 && !TIMESTAMP_NOT_FINITE(ts) &&
      likely(span->time != PG_INT64_MIN && span->time != PG_INT64_MAX)) {
    int64 result = ts;
    result -= (int64)span->day * USECS_PER_DAY_CONST;
    result -= span->time;
    if (unlikely(!IS_VALID_TIMESTAMP(result)))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    return result;
  }

  return (int64)DirectFunctionCall2(timestamp_mi_interval, (Datum)ts,
                                    (Datum)span_ptr);
}

/*
 * timestamptz_pl/mi_interval: same as timestamp (PG uses same internal
 * representation), but month-based intervals need timezone adjustment.
 * For month=0 fast path, timestamptz arithmetic is identical to timestamp.
 */
int64 jit_timestamptz_pl_interval(int64 ts, int64 span_ptr) {
  const Interval *span = (const Interval *)DatumGetPointer((Datum)span_ptr);
  /* Fast path ONLY for pure time intervals (no day/month component).
   * Day intervals on timestamptz must go through PG's full path because
   * "add 1 day" means same wall-clock time across DST boundaries,
   * NOT add 86400 seconds. */
  if (span->month == 0 && span->day == 0 && !TIMESTAMP_NOT_FINITE(ts)) {
    int64 result;
    if (unlikely(pg_add_s64_overflow(ts, span->time, &result)))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    if (unlikely(!IS_VALID_TIMESTAMP(result)))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    return result;
  }
  return (int64)DirectFunctionCall2(timestamptz_pl_interval, (Datum)ts,
                                    (Datum)span_ptr);
}

int64 jit_timestamptz_mi_interval(int64 ts, int64 span_ptr) {
  const Interval *span = (const Interval *)DatumGetPointer((Datum)span_ptr);
  if (span->month == 0 && span->day == 0 && !TIMESTAMP_NOT_FINITE(ts)) {
    int64 result;
    if (unlikely(pg_sub_s64_overflow(ts, span->time, &result)))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    if (unlikely(!IS_VALID_TIMESTAMP(result)))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    return result;
  }
  return (int64)DirectFunctionCall2(timestamptz_mi_interval, (Datum)ts,
                                    (Datum)span_ptr);
}

/* timestamp_mi: subtract two timestamps → interval.
 * PG splits the difference into days + time-of-day. */
int64 jit_timestamp_mi(int64 ts1, int64 ts2) {
  int64 diff = ts1 - ts2;
  Interval *result = (Interval *)palloc(sizeof(Interval));
  result->month = 0;
  result->day = (int32)(diff / USECS_PER_DAY);
  result->time = diff - (int64)result->day * USECS_PER_DAY;
  return PointerGetDatum(result);
}

/* date → timestamp cast: simple multiply by USECS_PER_DAY */
int64 jit_date_timestamp(int32 dateVal) {
  /* Special dates: fall back */
  if (unlikely(dateVal == (int32)0x7FFFFFFF || dateVal == (int32)0x80000000))
    return (int64)DirectFunctionCall1(date_timestamp, Int32GetDatum(dateVal));
  return jit_date_to_timestamp(dateVal);
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

    /* Extended hash functions moved to implemented section below */

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
    E2(timestamptz_pl_interval, jit_timestamptz_pl_interval, T64, T64, T64),
    E2(timestamptz_mi_interval, jit_timestamptz_mi_interval, T64, T64, T64),
    E2(timestamp_mi, jit_timestamp_mi, T64, T64, T64),

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

    /* ---- cross-type: date vs timestamp ---- */
    E2(date_eq_timestamp, jit_date_eq_timestamp, T32, T32, T64),
    E2(date_ne_timestamp, jit_date_ne_timestamp, T32, T32, T64),
    E2(date_lt_timestamp, jit_date_lt_timestamp, T32, T32, T64),
    E2(date_le_timestamp, jit_date_le_timestamp, T32, T32, T64),
    E2(date_gt_timestamp, jit_date_gt_timestamp, T32, T32, T64),
    E2(date_ge_timestamp, jit_date_ge_timestamp, T32, T32, T64),
    E2(date_cmp_timestamp, jit_date_cmp_timestamp, T32, T32, T64),

    /* ---- cross-type: timestamp vs date ---- */
    E2(timestamp_eq_date, jit_timestamp_eq_date, T32, T64, T32),
    E2(timestamp_ne_date, jit_timestamp_ne_date, T32, T64, T32),
    E2(timestamp_lt_date, jit_timestamp_lt_date, T32, T64, T32),
    E2(timestamp_le_date, jit_timestamp_le_date, T32, T64, T32),
    E2(timestamp_gt_date, jit_timestamp_gt_date, T32, T64, T32),
    E2(timestamp_ge_date, jit_timestamp_ge_date, T32, T64, T32),
    E2(timestamp_cmp_date, jit_timestamp_cmp_date, T32, T64, T32),

    /* ---- time comparison (int64 microseconds) ---- */
    EI2(time_eq, jit_time_eq, T32, T64, T64, JIT_INLINE_INT8_EQ),
    EI2(time_ne, jit_time_ne, T32, T64, T64, JIT_INLINE_INT8_NE),
    EI2(time_lt, jit_time_lt, T32, T64, T64, JIT_INLINE_INT8_LT),
    EI2(time_le, jit_time_le, T32, T64, T64, JIT_INLINE_INT8_LE),
    EI2(time_gt, jit_time_gt, T32, T64, T64, JIT_INLINE_INT8_GT),
    EI2(time_ge, jit_time_ge, T32, T64, T64, JIT_INLINE_INT8_GE),
    E2(time_cmp, jit_time_cmp, T32, T64, T64),

    /* ---- cross-type timestamptz comparisons (DEFERRED — need TZ conversion) ---- */

    /* ---- btree comparison functions ---- */
    E2(btint4cmp, jit_btint4cmp, T32, T32, T32),
    E2(btint8cmp, jit_btint8cmp, T32, T64, T64),
    E2(btint42cmp, jit_btint42cmp, T32, T32, T32),
    E2(btint48cmp, jit_btint48cmp, T32, T32, T64),
    E2(btint82cmp, jit_btint82cmp, T32, T64, T32),
    E2(btint84cmp, jit_btint84cmp, T32, T64, T32),
    E2(btfloat4cmp, jit_btfloat4cmp, T32, T64, T64),
    E2(btfloat8cmp, jit_btfloat8cmp, T32, T64, T64),
    E2(btfloat48cmp, jit_btfloat48cmp, T32, T64, T64),
    E2(btfloat84cmp, jit_btfloat84cmp, T32, T64, T64),
    E2(date_cmp, jit_date_cmp, T32, T32, T32),

    /* Type casts are handled via EI1 entries with JIT_INLINE_* ops above.
     * Additional cast functions (int8/float8/float4 by name) share names
     * with PG type macros — they're matched by fn_addr via the existing
     * cross-type entries (int48, i4tod, i4tof, i8tod, ftod, dtof). */

    /* ---- money (cash) type — int64 internally ---- */
    EI2(cash_eq, jit_cash_eq, T32, T64, T64, JIT_INLINE_INT8_EQ),
    EI2(cash_ne, jit_cash_ne, T32, T64, T64, JIT_INLINE_INT8_NE),
    EI2(cash_lt, jit_cash_lt, T32, T64, T64, JIT_INLINE_INT8_LT),
    EI2(cash_le, jit_cash_le, T32, T64, T64, JIT_INLINE_INT8_LE),
    EI2(cash_gt, jit_cash_gt, T32, T64, T64, JIT_INLINE_INT8_GT),
    EI2(cash_ge, jit_cash_ge, T32, T64, T64, JIT_INLINE_INT8_GE),
    E2(cash_cmp, jit_cash_cmp, T32, T64, T64),
    E2(cash_pl, jit_cash_pl, T64, T64, T64),
    E2(cash_mi, jit_cash_mi, T64, T64, T64),
    E2(cashlarger, jit_cashlarger, T64, T64, T64),
    E2(cashsmaller, jit_cashsmaller, T64, T64, T64),

    /* ---- text operations ---- */
    E1(textlen, jit_textlen, T32, T64),
    E1(textoctetlen, jit_textoctetlen, T32, T64),
    E2(text_starts_with, jit_text_starts_with, T32, T64, T64),
    /* upper/lower disabled: pg_jitter_collation_is_c returns true for
     * default collation in en_US.UTF-8 databases, causing ASCII fast-path
     * to incorrectly handle multi-byte characters. Needs fix in collation
     * detection before re-enabling. */

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

    /* ---- extended hash functions (parallel hash join/agg) ---- */
    E2(hashint2extended, jit_hashint2extended, T64, T64, T64),
    E2(hashint4extended, jit_hashint4extended, T64, T64, T64),
    E2(hashint8extended, jit_hashint8extended, T64, T64, T64),
    E2(hashoidextended, jit_hashoidextended, T64, T64, T64),
    /* hashfloat4/8extended removed: PG uses hash_any_extended on float8 bytes,
     * not hash_bytes_uint32_extended. Our implementation was wrong. */
#if PG_VERSION_NUM >= 180000
    E2(hashdateextended, jit_hashdateextended, T64, T64, T64),
#endif

    /* ---- aggregate COUNT/SUM ---- */
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
    EC2(texteq, simd_texteq, T32, T64, T64),
    EC2(textne, simd_textne, T32, T64, T64),
    EC2(text_lt, simd_text_lt, T32, T64, T64),
    EC2(text_le, simd_text_le, T32, T64, T64),
    EC2(text_gt, simd_text_gt, T32, T64, T64),
    EC2(text_ge, simd_text_ge, T32, T64, T64),
    EC2(bttextcmp, simd_bttextcmp, T32, T64, T64),
    EC2(text_larger, simd_text_larger, T64, T64, T64),
    EC2(text_smaller, simd_text_smaller, T64, T64, T64),
#ifdef PG_JITTER_HAVE_SIMD
    E1(hashtext, simd_hashtext, T32, T64),
#else
#endif
    /* text_pattern_*: raw byte comparison, no collation needed */
    E2(text_pattern_lt, jit_text_pattern_lt, T32, T64, T64),
    E2(text_pattern_le, jit_text_pattern_le, T32, T64, T64),
    E2(text_pattern_ge, jit_text_pattern_ge, T32, T64, T64),
    E2(text_pattern_gt, jit_text_pattern_gt, T32, T64, T64),
    E2(bttext_pattern_cmp, jit_bttext_pattern_cmp, T32, T64, T64),
    /* textlen, textoctetlen, text_starts_with moved to implemented section above */

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
    E2(numeric_eq, jit_numeric_eq, T32, T64, T64),
    E2(numeric_ne, jit_numeric_ne, T32, T64, T64),
    E2(numeric_lt, jit_numeric_lt, T32, T64, T64),
    E2(numeric_le, jit_numeric_le, T32, T64, T64),
    E2(numeric_gt, jit_numeric_gt, T32, T64, T64),
    E2(numeric_ge, jit_numeric_ge, T32, T64, T64),
    E2(numeric_cmp, jit_numeric_cmp, T32, T64, T64),
#endif
    E2(numeric_larger, jit_numeric_larger, T64, T64, T64),
    E2(numeric_smaller, jit_numeric_smaller, T64, T64, T64),
#ifdef PG_JITTER_HAVE_TIER2
    E1(hash_numeric, jit_hash_numeric_precompiled, T32, T64),
    E1(numeric_abs, jit_numeric_abs, T64, T64),
    E1(numeric_uminus, jit_numeric_uminus, T64, T64),
    E2(numeric_add, jit_numeric_add, T64, T64, T64),
    E2(numeric_sub, jit_numeric_sub, T64, T64, T64),
    E2(numeric_mul, jit_numeric_mul, T64, T64, T64),
#else
    /* numeric_add/sub/mul now have native implementations above */
#endif

/* ---- uuid comparison ---- */
#ifdef PG_JITTER_HAVE_TIER2
    E2(uuid_eq, jit_uuid_eq_precompiled, T32, T64, T64),
#else
#endif
#ifdef PG_JITTER_HAVE_TIER2
    E2(uuid_lt, jit_uuid_lt_precompiled, T32, T64, T64),
#else
#endif
#ifdef PG_JITTER_HAVE_TIER2
    E2(uuid_cmp, jit_uuid_cmp_precompiled, T32, T64, T64),
    E1(uuid_hash, jit_uuid_hash_precompiled, T32, T64),
#else
#endif

    /* ---- jsonb comparison + operators ---- */

    /* ---- jsonb accessors (non-mutating) ---- */

    /* ---- bytea comparison ---- */
#if PG_VERSION_NUM >= 180000
#endif

    /* ---- bpchar comparison ---- */

    /* ---- array comparison + non-mutating ---- */

    /* ---- network (inet/cidr) comparison ---- */

    /* ---- float hash ---- */
    E1(hashfloat4, jit_hashfloat4, T32, T64),
    E1(hashfloat8, jit_hashfloat8, T32, T64),

    /* ---- numeric aggregates ---- */

    /* ---- float aggregates ---- */

    /* ---- text/string functions ---- */

    /* ---- array functions ---- */

    /* ---- date/time functions ---- */
    E1(date_timestamp, jit_date_timestamp, T64, T32),

    /* ---- aggregate transition functions ---- */

    /* ---- formatting/conversion ---- */

    /* ---- misc string ---- */

    /* ---- math ---- */
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
 * Lookup: O(1) hash table by PGFunction pointer.
 * Built on first call, lives for backend lifetime.
 * Open addressing, Fibonacci hash, ~40% load factor.
 * ================================================================ */
static const JitDirectFn **jit_fn_ht = NULL;
static int jit_fn_ht_mask = 0;

static inline uint32
jit_fn_hash(PGFunction fn) {
  uintptr_t h = (uintptr_t)fn;
  /* Fibonacci hash — spreads pointer-aligned values well */
  h = (h >> 4) * (uintptr_t)11400714819323198485ULL;
  return (uint32)(h >> 32);
}

static void
jit_fn_ht_init(void) {
  int sz = 1024; /* power of 2, >= 2.5× entry count */
  while (sz < jit_direct_fns_count * 3)
    sz <<= 1;
  jit_fn_ht_mask = sz - 1;
  jit_fn_ht = (const JitDirectFn **)calloc(sz, sizeof(void *));

  for (int i = 0; i < jit_direct_fns_count; i++) {
    uint32 idx = jit_fn_hash(jit_direct_fns[i].pg_fn) & jit_fn_ht_mask;
    while (jit_fn_ht[idx])
      idx = (idx + 1) & jit_fn_ht_mask;
    jit_fn_ht[idx] = &jit_direct_fns[i];
  }
}

const JitDirectFn *jit_find_direct_fn(PGFunction pg_fn) {
#ifdef JIT_DISABLE_DIRECT_CALLS
  return NULL;
#endif
  if (unlikely(!jit_fn_ht))
    jit_fn_ht_init();

  uint32 idx = jit_fn_hash(pg_fn) & jit_fn_ht_mask;
  while (jit_fn_ht[idx]) {
    if (jit_fn_ht[idx]->pg_fn == pg_fn)
      return jit_fn_ht[idx];
    idx = (idx + 1) & jit_fn_ht_mask;
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
