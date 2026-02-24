/*
 * mir_jit_funcs.c — Simplified jit functions for c2mir compilation
 *
 * c2mir cannot parse macOS math.h (type-generic macros) or PG's
 * __builtin_add_overflow with pointer-to-int args. This file provides
 * equivalent implementations using only basic C that c2mir handles.
 *
 * Compiled by mir_precompile at build time → MIR IR blobs.
 * At runtime, MIR_gen() produces native code equivalent to pg_jit_funcs.c.
 */

#include <stdint.h>
#include <limits.h>

/* Types matching PG conventions */
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint32_t uint32;

/* Error handlers — imported from pg_jit_funcs.c at link time */
extern void jit_error_int4_overflow(void);
extern void jit_error_int8_overflow(void);
extern void jit_error_int2_overflow(void);
extern void jit_error_division_by_zero(void);

/* hash_bytes_uint32 from PG — imported at runtime */
extern uint32 hash_bytes_uint32(uint32 k);

/* ================================================================
 * int32 overflow-checked arithmetic
 * ================================================================ */

int32 jit_int4pl(int32 a, int32 b)
{
	int64 r = (int64)a + (int64)b;
	if (r > INT32_MAX || r < INT32_MIN)
		jit_error_int4_overflow();
	return (int32)r;
}

int32 jit_int4mi(int32 a, int32 b)
{
	int64 r = (int64)a - (int64)b;
	if (r > INT32_MAX || r < INT32_MIN)
		jit_error_int4_overflow();
	return (int32)r;
}

int32 jit_int4mul(int32 a, int32 b)
{
	int64 r = (int64)a * (int64)b;
	if (r > INT32_MAX || r < INT32_MIN)
		jit_error_int4_overflow();
	return (int32)r;
}

int32 jit_int4div(int32 a, int32 b)
{
	if (b == 0)
		jit_error_division_by_zero();
	if (a == INT32_MIN && b == -1)
		jit_error_int4_overflow();
	return a / b;
}

int32 jit_int4mod(int32 a, int32 b)
{
	if (b == 0)
		jit_error_division_by_zero();
	if (a == INT32_MIN && b == -1)
		return 0;
	return a % b;
}

int32 jit_int4abs(int32 a)
{
	if (a == INT32_MIN)
		jit_error_int4_overflow();
	return (a < 0) ? -a : a;
}

int32 jit_int4inc(int32 a)
{
	if (a == INT32_MAX)
		jit_error_int4_overflow();
	return a + 1;
}

/* ================================================================
 * int64 overflow-checked arithmetic
 * Uses unsigned overflow detection to avoid needing __builtin_*_overflow
 * ================================================================ */

int64 jit_int8pl(int64 a, int64 b)
{
	int64 r = (int64)((uint64_t)a + (uint64_t)b);
	/* Overflow: signs of a and b match, but result differs */
	if (((a ^ r) & (b ^ r)) < 0)
		jit_error_int8_overflow();
	return r;
}

int64 jit_int8mi(int64 a, int64 b)
{
	int64 r = (int64)((uint64_t)a - (uint64_t)b);
	/* Overflow: signs of a and -b (i.e. a and ~b) match, but result differs */
	if (((a ^ b) & (a ^ r)) < 0)
		jit_error_int8_overflow();
	return r;
}

int64 jit_int8mul(int64 a, int64 b)
{
	int64 r = (int64)((uint64_t)a * (uint64_t)b);
	/* Check: if a != 0, r/a should == b */
	if (a != 0 && (a == -1 ? -r : r / a) != b)
		jit_error_int8_overflow();
	/* Special case: INT64_MIN * -1 */
	if (a == -1 && b == INT64_MIN)
		jit_error_int8_overflow();
	return r;
}

int64 jit_int8div(int64 a, int64 b)
{
	if (b == 0)
		jit_error_division_by_zero();
	if (a == INT64_MIN && b == -1)
		jit_error_int8_overflow();
	return a / b;
}

int64 jit_int8mod(int64 a, int64 b)
{
	if (b == 0)
		jit_error_division_by_zero();
	if (a == INT64_MIN && b == -1)
		return 0;
	return a % b;
}

int64 jit_int8abs(int64 a)
{
	if (a == INT64_MIN)
		jit_error_int8_overflow();
	return (a < 0) ? -a : a;
}

int64 jit_int8inc(int64 a)
{
	int64 r = (int64)((uint64_t)a + 1);
	if (((a ^ r) & ((int64)1 ^ r)) < 0)
		jit_error_int8_overflow();
	return r;
}

int64 jit_int8dec(int64 a)
{
	int64 r = (int64)((uint64_t)a - 1);
	if (((a ^ (int64)1) & (a ^ r)) < 0)
		jit_error_int8_overflow();
	return r;
}

/* ================================================================
 * int16 arithmetic (with overflow checks in int32 range)
 * ================================================================ */

int32 jit_int2pl(int32 a32, int32 b32)
{
	int16 a = (int16)a32, b = (int16)b32;
	int32 r = (int32)a + (int32)b;
	if (r < -32768 || r > 32767)
		jit_error_int2_overflow();
	return (int32)(int16)r;
}

int32 jit_int2mi(int32 a32, int32 b32)
{
	int16 a = (int16)a32, b = (int16)b32;
	int32 r = (int32)a - (int32)b;
	if (r < -32768 || r > 32767)
		jit_error_int2_overflow();
	return (int32)(int16)r;
}

int32 jit_int2mul(int32 a32, int32 b32)
{
	int16 a = (int16)a32, b = (int16)b32;
	int32 r = (int32)a * (int32)b;
	if (r < -32768 || r > 32767)
		jit_error_int2_overflow();
	return (int32)(int16)r;
}

int32 jit_int2div(int32 a32, int32 b32)
{
	int16 a = (int16)a32, b = (int16)b32;
	if (b == 0)
		jit_error_division_by_zero();
	if (a == -32768 && b == -1)
		jit_error_int2_overflow();
	return (int32)(a / b);
}

int32 jit_int2mod(int32 a32, int32 b32)
{
	int16 a = (int16)a32, b = (int16)b32;
	if (b == 0)
		jit_error_division_by_zero();
	if (a == -32768 && b == -1)
		return 0;
	return (int32)(a % b);
}

int32 jit_int2abs(int32 a32)
{
	int16 a = (int16)a32;
	if (a == -32768)
		jit_error_int2_overflow();
	return (int32)((a < 0) ? -a : a);
}

/* ================================================================
 * int32 comparison
 * ================================================================ */

int32 jit_int4eq(int32 a, int32 b) { return (a == b) ? 1 : 0; }
int32 jit_int4ne(int32 a, int32 b) { return (a != b) ? 1 : 0; }
int32 jit_int4lt(int32 a, int32 b) { return (a < b) ? 1 : 0; }
int32 jit_int4le(int32 a, int32 b) { return (a <= b) ? 1 : 0; }
int32 jit_int4gt(int32 a, int32 b) { return (a > b) ? 1 : 0; }
int32 jit_int4ge(int32 a, int32 b) { return (a >= b) ? 1 : 0; }

/* ================================================================
 * int64 comparison
 * ================================================================ */

int32 jit_int8eq(int64 a, int64 b) { return (a == b) ? 1 : 0; }
int32 jit_int8ne(int64 a, int64 b) { return (a != b) ? 1 : 0; }
int32 jit_int8lt(int64 a, int64 b) { return (a < b) ? 1 : 0; }
int32 jit_int8le(int64 a, int64 b) { return (a <= b) ? 1 : 0; }
int32 jit_int8gt(int64 a, int64 b) { return (a > b) ? 1 : 0; }
int32 jit_int8ge(int64 a, int64 b) { return (a >= b) ? 1 : 0; }

/* ================================================================
 * int16 comparison
 * ================================================================ */

int32 jit_int2eq(int32 a, int32 b) { return ((int16)a == (int16)b) ? 1 : 0; }
int32 jit_int2ne(int32 a, int32 b) { return ((int16)a != (int16)b) ? 1 : 0; }
int32 jit_int2lt(int32 a, int32 b) { return ((int16)a < (int16)b) ? 1 : 0; }
int32 jit_int2le(int32 a, int32 b) { return ((int16)a <= (int16)b) ? 1 : 0; }
int32 jit_int2gt(int32 a, int32 b) { return ((int16)a > (int16)b) ? 1 : 0; }
int32 jit_int2ge(int32 a, int32 b) { return ((int16)a >= (int16)b) ? 1 : 0; }

/* ================================================================
 * Cross-type int comparison
 * ================================================================ */

int32 jit_int48eq(int32 a, int64 b) { return ((int64)a == b) ? 1 : 0; }
int32 jit_int48ne(int32 a, int64 b) { return ((int64)a != b) ? 1 : 0; }
int32 jit_int48lt(int32 a, int64 b) { return ((int64)a < b) ? 1 : 0; }
int32 jit_int48le(int32 a, int64 b) { return ((int64)a <= b) ? 1 : 0; }
int32 jit_int48gt(int32 a, int64 b) { return ((int64)a > b) ? 1 : 0; }
int32 jit_int48ge(int32 a, int64 b) { return ((int64)a >= b) ? 1 : 0; }

int32 jit_int84eq(int64 a, int32 b) { return (a == (int64)b) ? 1 : 0; }
int32 jit_int84ne(int64 a, int32 b) { return (a != (int64)b) ? 1 : 0; }
int32 jit_int84lt(int64 a, int32 b) { return (a < (int64)b) ? 1 : 0; }
int32 jit_int84le(int64 a, int32 b) { return (a <= (int64)b) ? 1 : 0; }
int32 jit_int84gt(int64 a, int32 b) { return (a > (int64)b) ? 1 : 0; }
int32 jit_int84ge(int64 a, int32 b) { return (a >= (int64)b) ? 1 : 0; }

int32 jit_int24eq(int32 a, int32 b) { return ((int32)(int16)a == b) ? 1 : 0; }
int32 jit_int24ne(int32 a, int32 b) { return ((int32)(int16)a != b) ? 1 : 0; }
int32 jit_int24lt(int32 a, int32 b) { return ((int32)(int16)a < b) ? 1 : 0; }
int32 jit_int24le(int32 a, int32 b) { return ((int32)(int16)a <= b) ? 1 : 0; }
int32 jit_int24gt(int32 a, int32 b) { return ((int32)(int16)a > b) ? 1 : 0; }
int32 jit_int24ge(int32 a, int32 b) { return ((int32)(int16)a >= b) ? 1 : 0; }

int32 jit_int42eq(int32 a, int32 b) { return (a == (int32)(int16)b) ? 1 : 0; }
int32 jit_int42ne(int32 a, int32 b) { return (a != (int32)(int16)b) ? 1 : 0; }
int32 jit_int42lt(int32 a, int32 b) { return (a < (int32)(int16)b) ? 1 : 0; }
int32 jit_int42le(int32 a, int32 b) { return (a <= (int32)(int16)b) ? 1 : 0; }
int32 jit_int42gt(int32 a, int32 b) { return (a > (int32)(int16)b) ? 1 : 0; }
int32 jit_int42ge(int32 a, int32 b) { return (a >= (int32)(int16)b) ? 1 : 0; }

int32 jit_int28eq(int64 a, int64 b) { return ((int64)(int16)a == b) ? 1 : 0; }
int32 jit_int28ne(int64 a, int64 b) { return ((int64)(int16)a != b) ? 1 : 0; }
int32 jit_int28lt(int64 a, int64 b) { return ((int64)(int16)a < b) ? 1 : 0; }
int32 jit_int28le(int64 a, int64 b) { return ((int64)(int16)a <= b) ? 1 : 0; }
int32 jit_int28gt(int64 a, int64 b) { return ((int64)(int16)a > b) ? 1 : 0; }
int32 jit_int28ge(int64 a, int64 b) { return ((int64)(int16)a >= b) ? 1 : 0; }

int32 jit_int82eq(int64 a, int64 b) { return (a == (int64)(int16)b) ? 1 : 0; }
int32 jit_int82ne(int64 a, int64 b) { return (a != (int64)(int16)b) ? 1 : 0; }
int32 jit_int82lt(int64 a, int64 b) { return (a < (int64)(int16)b) ? 1 : 0; }
int32 jit_int82le(int64 a, int64 b) { return (a <= (int64)(int16)b) ? 1 : 0; }
int32 jit_int82gt(int64 a, int64 b) { return (a > (int64)(int16)b) ? 1 : 0; }
int32 jit_int82ge(int64 a, int64 b) { return (a >= (int64)(int16)b) ? 1 : 0; }

/* ================================================================
 * Cross-type int arithmetic
 * ================================================================ */

int32 jit_int24pl(int32 a, int32 b)  { return jit_int4pl((int32)(int16)a, b); }
int32 jit_int24mi(int32 a, int32 b)  { return jit_int4mi((int32)(int16)a, b); }
int32 jit_int24mul(int32 a, int32 b) { return jit_int4mul((int32)(int16)a, b); }
int32 jit_int24div(int32 a, int32 b) { return jit_int4div((int32)(int16)a, b); }
int32 jit_int42pl(int32 a, int32 b)  { return jit_int4pl(a, (int32)(int16)b); }
int32 jit_int42mi(int32 a, int32 b)  { return jit_int4mi(a, (int32)(int16)b); }
int32 jit_int42mul(int32 a, int32 b) { return jit_int4mul(a, (int32)(int16)b); }
int32 jit_int42div(int32 a, int32 b) { return jit_int4div(a, (int32)(int16)b); }

int64 jit_int48pl(int64 a, int64 b)  { return jit_int8pl((int64)(int32)a, b); }
int64 jit_int48mi(int64 a, int64 b)  { return jit_int8mi((int64)(int32)a, b); }
int64 jit_int48mul(int64 a, int64 b) { return jit_int8mul((int64)(int32)a, b); }
int64 jit_int48div(int64 a, int64 b) { return jit_int8div((int64)(int32)a, b); }
int64 jit_int84pl(int64 a, int64 b)  { return jit_int8pl(a, (int64)(int32)b); }
int64 jit_int84mi(int64 a, int64 b)  { return jit_int8mi(a, (int64)(int32)b); }
int64 jit_int84mul(int64 a, int64 b) { return jit_int8mul(a, (int64)(int32)b); }
int64 jit_int84div(int64 a, int64 b) { return jit_int8div(a, (int64)(int32)b); }

int64 jit_int28pl(int64 a, int64 b)  { return jit_int8pl((int64)(int16)a, b); }
int64 jit_int28mi(int64 a, int64 b)  { return jit_int8mi((int64)(int16)a, b); }
int64 jit_int28mul(int64 a, int64 b) { return jit_int8mul((int64)(int16)a, b); }
int64 jit_int28div(int64 a, int64 b) { return jit_int8div((int64)(int16)a, b); }
int64 jit_int82pl(int64 a, int64 b)  { return jit_int8pl(a, (int64)(int16)b); }
int64 jit_int82mi(int64 a, int64 b)  { return jit_int8mi(a, (int64)(int16)b); }
int64 jit_int82mul(int64 a, int64 b) { return jit_int8mul(a, (int64)(int16)b); }
int64 jit_int82div(int64 a, int64 b) { return jit_int8div(a, (int64)(int16)b); }

/* ================================================================
 * int min/max
 * ================================================================ */

int32 jit_int2larger(int32 a, int32 b)  { return ((int16)a >= (int16)b) ? a : b; }
int32 jit_int2smaller(int32 a, int32 b) { return ((int16)a <= (int16)b) ? a : b; }
int32 jit_int4larger(int32 a, int32 b)  { return (a >= b) ? a : b; }
int32 jit_int4smaller(int32 a, int32 b) { return (a <= b) ? a : b; }
int64 jit_int8larger(int64 a, int64 b)  { return (a >= b) ? a : b; }
int64 jit_int8smaller(int64 a, int64 b) { return (a <= b) ? a : b; }

/* ================================================================
 * int bitwise
 * ================================================================ */

int32 jit_int2and(int32 a, int32 b) { return (int32)((int16)a & (int16)b); }
int32 jit_int2or(int32 a, int32 b)  { return (int32)((int16)a | (int16)b); }
int32 jit_int2xor(int32 a, int32 b) { return (int32)((int16)a ^ (int16)b); }
int32 jit_int2not(int32 a)          { return (int32)(~(int16)a); }
int32 jit_int2shl(int32 a, int32 b) { return (int32)((int16)a << b); }
int32 jit_int2shr(int32 a, int32 b) { return (int32)((int16)a >> b); }

int32 jit_int4and(int32 a, int32 b) { return a & b; }
int32 jit_int4or(int32 a, int32 b)  { return a | b; }
int32 jit_int4xor(int32 a, int32 b) { return a ^ b; }
int32 jit_int4not(int32 a)          { return ~a; }
int32 jit_int4shl(int32 a, int32 b) { return a << b; }
int32 jit_int4shr(int32 a, int32 b) { return a >> b; }

int64 jit_int8and(int64 a, int64 b) { return a & b; }
int64 jit_int8or(int64 a, int64 b)  { return a | b; }
int64 jit_int8xor(int64 a, int64 b) { return a ^ b; }
int64 jit_int8not(int64 a)          { return ~a; }
int64 jit_int8shl(int64 a, int64 b) { return a << (int)b; }
int64 jit_int8shr(int64 a, int64 b) { return a >> (int)b; }

/* ================================================================
 * bool comparison + aggregates
 * ================================================================ */

int32 jit_booleq(int64 a, int64 b) { return (a == b) ? 1 : 0; }
int32 jit_boolne(int64 a, int64 b) { return (a != b) ? 1 : 0; }
int32 jit_boollt(int64 a, int64 b) { return (a < b) ? 1 : 0; }
int32 jit_boolgt(int64 a, int64 b) { return (a > b) ? 1 : 0; }
int32 jit_boolle(int64 a, int64 b) { return (a <= b) ? 1 : 0; }
int32 jit_boolge(int64 a, int64 b) { return (a >= b) ? 1 : 0; }
int64 jit_booland_statefunc(int64 a, int64 b) { return (a != 0 && b != 0) ? 1 : 0; }
int64 jit_boolor_statefunc(int64 a, int64 b)  { return (a != 0 || b != 0) ? 1 : 0; }

/* ================================================================
 * timestamp comparison (int64 microseconds)
 * ================================================================ */

int32 jit_timestamp_eq(int64 a, int64 b) { return (a == b) ? 1 : 0; }
int32 jit_timestamp_ne(int64 a, int64 b) { return (a != b) ? 1 : 0; }
int32 jit_timestamp_lt(int64 a, int64 b) { return (a < b) ? 1 : 0; }
int32 jit_timestamp_le(int64 a, int64 b) { return (a <= b) ? 1 : 0; }
int32 jit_timestamp_gt(int64 a, int64 b) { return (a > b) ? 1 : 0; }
int32 jit_timestamp_ge(int64 a, int64 b) { return (a >= b) ? 1 : 0; }
int32 jit_timestamp_cmp(int64 a, int64 b) { return (a < b) ? -1 : ((a > b) ? 1 : 0); }
int64 jit_timestamp_larger(int64 a, int64 b)  { return (a >= b) ? a : b; }
int64 jit_timestamp_smaller(int64 a, int64 b) { return (a <= b) ? a : b; }

/* ================================================================
 * date comparison (int32 days)
 * ================================================================ */

int32 jit_date_eq(int32 a, int32 b) { return (a == b) ? 1 : 0; }
int32 jit_date_ne(int32 a, int32 b) { return (a != b) ? 1 : 0; }
int32 jit_date_lt(int32 a, int32 b) { return (a < b) ? 1 : 0; }
int32 jit_date_le(int32 a, int32 b) { return (a <= b) ? 1 : 0; }
int32 jit_date_gt(int32 a, int32 b) { return (a > b) ? 1 : 0; }
int32 jit_date_ge(int32 a, int32 b) { return (a >= b) ? 1 : 0; }
int32 jit_date_larger(int32 a, int32 b)  { return (a >= b) ? a : b; }
int32 jit_date_smaller(int32 a, int32 b) { return (a <= b) ? a : b; }

int32 jit_date_pli(int32 date, int32 days)
{
	int64 r = (int64)date + (int64)days;
	if (r > INT32_MAX || r < INT32_MIN)
		jit_error_int4_overflow();
	return (int32)r;
}

int32 jit_date_mii(int32 date, int32 days)
{
	int64 r = (int64)date - (int64)days;
	if (r > INT32_MAX || r < INT32_MIN)
		jit_error_int4_overflow();
	return (int32)r;
}

int32 jit_date_mi(int32 a, int32 b)
{
	int64 r = (int64)a - (int64)b;
	if (r > INT32_MAX || r < INT32_MIN)
		jit_error_int4_overflow();
	return (int32)r;
}

/* ================================================================
 * OID comparison (uint32)
 * ================================================================ */

int32 jit_oideq(int32 a, int32 b) { return ((uint32)a == (uint32)b) ? 1 : 0; }
int32 jit_oidne(int32 a, int32 b) { return ((uint32)a != (uint32)b) ? 1 : 0; }
int32 jit_oidlt(int32 a, int32 b) { return ((uint32)a < (uint32)b) ? 1 : 0; }
int32 jit_oidle(int32 a, int32 b) { return ((uint32)a <= (uint32)b) ? 1 : 0; }
int32 jit_oidgt(int32 a, int32 b) { return ((uint32)a > (uint32)b) ? 1 : 0; }
int32 jit_oidge(int32 a, int32 b) { return ((uint32)a >= (uint32)b) ? 1 : 0; }
int32 jit_oidlarger(int32 a, int32 b)  { return ((uint32)a >= (uint32)b) ? a : b; }
int32 jit_oidsmaller(int32 a, int32 b) { return ((uint32)a <= (uint32)b) ? a : b; }

/* ================================================================
 * Hash functions
 * ================================================================ */

int32 jit_hashint2(int64 a)
{
	return (int32)hash_bytes_uint32((uint32)(int16)a);
}

int32 jit_hashint4(int32 a)
{
	return (int32)hash_bytes_uint32((uint32)a);
}

int64 jit_hashint8(int64 val)
{
	uint32 lohalf = (uint32)val;
	uint32 hihalf = (uint32)(val >> 32);
	lohalf ^= (val >= 0) ? hihalf : ~hihalf;
	return (int64)(uint32)hash_bytes_uint32(lohalf);
}

int32 jit_hashoid(int32 a)
{
	return (int32)hash_bytes_uint32((uint32)a);
}

int32 jit_hashbool(int64 a)
{
	return (int32)hash_bytes_uint32((uint32)(a != 0));
}

int32 jit_hashdate(int32 a)
{
	return (int32)hash_bytes_uint32((uint32)a);
}

/* ================================================================
 * Aggregate helpers
 * ================================================================ */

int64 jit_int8inc_any(int64 a, int64 dummy)
{
	(void)dummy;
	return jit_int8inc(a);
}

int64 jit_int8dec_any(int64 a, int64 dummy)
{
	(void)dummy;
	return jit_int8dec(a);
}

int64 jit_int2_sum(int64 oldsum, int64 newval)
{
	return jit_int8pl(oldsum, (int64)(int16)newval);
}

int64 jit_int4_sum(int64 oldsum, int64 newval)
{
	return jit_int8pl(oldsum, (int64)(int32)newval);
}
