/*
 * pg_jit_funcs.h — Unwrapped PG function declarations for direct JIT calls
 *
 * Maps hot PG built-in functions to thin native implementations that bypass
 * the V1 fmgr calling convention (fcinfo). The JIT backends use the lookup
 * table to dispatch direct calls at JIT compile time.
 */
#ifndef PG_JIT_FUNCS_H
#define PG_JIT_FUNCS_H

#include "postgres.h"
#include "fmgr.h"

/*
 * Argument/return type classification for JIT calling convention.
 * Maps to sljit (32 / W), asmjit (int32_t / int64_t), MIR (I32 / I64).
 */
#define JIT_TYPE_32   0   /* 32-bit: int32, uint32, int16, bool */
#define JIT_TYPE_64   1   /* 64-bit: int64, Datum, pointer */

/*
 * Flags for JitDirectFn entries.
 */
#define JIT_FN_FLAG_NONE       0x00
#define JIT_FN_FLAG_COLLATION  0x01  /* pass fcinfo->fncollation as last native arg */

/*
 * Inline operation codes for emit_inline_funcexpr().
 * When set on a JitDirectFn entry, the sljit backend emits the operation
 * as inline instructions instead of calling jit_fn.
 */
typedef enum JitInlineOp
{
	JIT_INLINE_NONE = 0,
	/* int32 arithmetic (overflow-checked) */
	JIT_INLINE_INT4_ADD, JIT_INLINE_INT4_SUB, JIT_INLINE_INT4_MUL,
	JIT_INLINE_INT4_DIV, JIT_INLINE_INT4_MOD,
	/* int64 arithmetic (overflow-checked) */
	JIT_INLINE_INT8_ADD, JIT_INLINE_INT8_SUB, JIT_INLINE_INT8_MUL,
	/* int32 comparison */
	JIT_INLINE_INT4_EQ, JIT_INLINE_INT4_NE,
	JIT_INLINE_INT4_LT, JIT_INLINE_INT4_LE,
	JIT_INLINE_INT4_GT, JIT_INLINE_INT4_GE,
	/* int64 comparison */
	JIT_INLINE_INT8_EQ, JIT_INLINE_INT8_NE,
	JIT_INLINE_INT8_LT, JIT_INLINE_INT8_LE,
	JIT_INLINE_INT8_GT, JIT_INLINE_INT8_GE,
	/* float64 arithmetic (IEEE 754, check isinf after) */
	JIT_INLINE_FLOAT8_ADD, JIT_INLINE_FLOAT8_SUB,
	JIT_INLINE_FLOAT8_MUL, JIT_INLINE_FLOAT8_DIV,
	/* float64 comparison */
	JIT_INLINE_FLOAT8_EQ, JIT_INLINE_FLOAT8_NE,
	JIT_INLINE_FLOAT8_LT, JIT_INLINE_FLOAT8_LE,
	JIT_INLINE_FLOAT8_GT, JIT_INLINE_FLOAT8_GE,
	/* hash functions (1-arg, inline hash_bytes_uint32) */
	JIT_INLINE_HASHINT4,	/* hash_bytes_uint32((uint32) arg) */
	JIT_INLINE_HASHINT8,	/* fold int64 → uint32, then hash_bytes_uint32 */
} JitInlineOp;

/*
 * Lookup entry: maps PG fn_addr → direct-call function + metadata.
 * Searched once per expression step at JIT compile time, not per row.
 */
typedef struct JitDirectFn
{
	PGFunction   pg_fn;        /* PG's V1 function address (e.g., int4pl) */
	void        *jit_fn;       /* our unwrapped native version, or NULL */
	uint8        nargs;        /* number of PG args (1-4) */
	uint8        ret_type;     /* JIT_TYPE_32 or JIT_TYPE_64 */
	uint8        arg_types[4]; /* type of each arg */
	uint8        inline_op;    /* JitInlineOp, or 0 for none */
	uint8        flags;        /* JIT_FN_FLAG_* */
	const char  *jit_fn_name;  /* name of jit_fn for precompiled blob lookup */
} JitDirectFn;

/*
 * Total number of native args including implicit collation arg.
 * Used by backends to allocate registers and select calling convention.
 */
static inline int
jit_native_nargs(const JitDirectFn *dfn)
{
	return dfn->nargs + ((dfn->flags & JIT_FN_FLAG_COLLATION) ? 1 : 0);
}

extern const JitDirectFn jit_direct_fns[];
extern const int jit_direct_fns_count;

/*
 * Find the direct-call entry for a PG function address.
 * Returns NULL if no direct call is available.
 * Linear scan over ~350 entries — CPU caches it after first lookup.
 */
const JitDirectFn *jit_find_direct_fn(PGFunction pg_fn);

/*
 * Build an sljit call-type bitmask from a JitDirectFn.
 * Encoding: ret_type in bits 0-3, arg[i] type in bits 4*(i+1)..4*(i+1)+3
 * Type values: 1 = W (64-bit), 2 = 32-bit
 */
static inline uint32
jit_sljit_call_type(const JitDirectFn *dfn)
{
	/* Map: JIT_TYPE_32 → 2 (SLJIT 32-bit), JIT_TYPE_64 → 1 (SLJIT W) */
	static const uint32 type_map[] = { 2, 1 };
	uint32 result = type_map[dfn->ret_type];
	for (int i = 0; i < dfn->nargs; i++)
		result |= type_map[dfn->arg_types[i]] << ((i + 1) * 4);
	/* Collation arg (Oid = uint32) goes after PG args as 32-bit */
	if (dfn->flags & JIT_FN_FLAG_COLLATION)
		result |= 2 << ((dfn->nargs + 1) * 4);
	return result;
}

/*
 * Error handlers — cold path, callable from JIT code.
 * These never return (ereport), so no register save needed at call site.
 */
extern pg_noinline void jit_error_int2_overflow(void);
extern pg_noinline void jit_error_int4_overflow(void);
extern pg_noinline void jit_error_int8_overflow(void);
extern pg_noinline void jit_error_division_by_zero(void);
extern pg_noinline void jit_error_float_overflow(void);
extern pg_noinline void jit_error_float_underflow(void);

/*
 * Hash function declarations — used by HASHED_SCALARARRAYOP native path
 * to identify hash type at compile time via function pointer comparison.
 */
extern int32 jit_hashint2(int64 a);
extern int32 jit_hashint4(int32 a);
extern int64 jit_hashint8(int64 val);
extern int32 jit_hashoid(int32 a);
extern int32 jit_hashbool(int64 a);
extern int32 jit_hashdate(int32 a);
extern int32 jit_hashfloat4(int64 datum);
extern int32 jit_hashfloat8(int64 datum);

/*
 * Interval comparison — native implementations using INT128.
 * Args are Interval* passed as int64 (pointer).
 */
extern int32 jit_interval_eq(int64 a, int64 b);
extern int32 jit_interval_ne(int64 a, int64 b);
extern int32 jit_interval_lt(int64 a, int64 b);
extern int32 jit_interval_le(int64 a, int64 b);
extern int32 jit_interval_gt(int64 a, int64 b);
extern int32 jit_interval_ge(int64 a, int64 b);
extern int32 jit_interval_cmp(int64 a, int64 b);
extern int64 jit_interval_smaller(int64 a, int64 b);
extern int64 jit_interval_larger(int64 a, int64 b);
extern int32 jit_interval_hash(int64 a);

/*
 * Interval / timestamp arithmetic wrappers.
 * Pass-through to PG's complex interval arithmetic via DirectFunctionCall2.
 */
extern int64 jit_interval_pl(int64 a, int64 b);
extern int64 jit_interval_mi(int64 a, int64 b);
extern int64 jit_timestamp_pl_interval(int64 ts, int64 span);
extern int64 jit_timestamp_mi_interval(int64 ts, int64 span);

/*
 * Text comparison wrappers (collation-aware).
 * Collation Oid is passed as int32 last arg (JIT_FN_FLAG_COLLATION).
 */
extern int32 jit_texteq(int64 a, int64 b, int32 collid);
extern int32 jit_textne(int64 a, int64 b, int32 collid);
extern int32 jit_text_lt(int64 a, int64 b, int32 collid);
extern int32 jit_text_le(int64 a, int64 b, int32 collid);
extern int32 jit_text_gt(int64 a, int64 b, int32 collid);
extern int32 jit_text_ge(int64 a, int64 b, int32 collid);
extern int32 jit_bttextcmp(int64 a, int64 b, int32 collid);
extern int64 jit_text_larger(int64 a, int64 b, int32 collid);
extern int64 jit_text_smaller(int64 a, int64 b, int32 collid);

/* Text functions (no collation needed) */
extern int32 jit_textlen(int64 a);
extern int32 jit_textoctetlen(int64 a);
extern int32 jit_text_pattern_lt(int64 a, int64 b);
extern int32 jit_text_pattern_le(int64 a, int64 b);
extern int32 jit_text_pattern_ge(int64 a, int64 b);
extern int32 jit_text_pattern_gt(int64 a, int64 b);
extern int32 jit_bttext_pattern_cmp(int64 a, int64 b);

/*
 * Tier 2 C wrapper declarations — always available, no external deps.
 */
#include "pg_jit_tier2_wrappers.h"

/*
 * Pre-compiled native blob support (c2mir pipeline).
 *
 * When PG_JITTER_HAVE_MIR_PRECOMPILED is defined, all backends can use
 * pre-compiled native code blobs generated by c2mir + MIR_gen at build time.
 * The blobs are loaded by pg_jit_mir_blobs.h infrastructure.
 */

/*
 * Pre-compiled inline blob support.
 *
 * When PG_JITTER_HAVE_PRECOMPILED is defined, the sljit backend can emit
 * clang-optimized native code for Tier 1 functions instead of hand-written
 * sljit instruction sequences.
 */
#ifdef PG_JITTER_HAVE_PRECOMPILED
#include "pg_jit_precompiled.h"

/*
 * Find the pre-compiled inline blob for a jit_* function name.
 * Returns NULL if no blob is available.
 */
static inline const PrecompiledInline *
jit_find_precompiled(const char *fn_name)
{
	for (int i = 0; i < precompiled_inlines_count; i++)
	{
		if (strcmp(precompiled_inlines[i].name, fn_name) == 0)
			return precompiled_inlines[i].blob;
	}
	return NULL;
}
#endif /* PG_JITTER_HAVE_PRECOMPILED */

/*
 * Pre-compiled Tier 2 wrapper declarations.
 * These are auto-generated by gen_tier2_wrappers.py and compiled from
 * PG bitcode with LLVM optimizations (function bodies inlined).
 */
#ifdef PG_JITTER_HAVE_TIER2
extern int32 jit_numeric_eq_precompiled(int64 a, int64 b);
extern int32 jit_numeric_ne_precompiled(int64 a, int64 b);
extern int32 jit_numeric_lt_precompiled(int64 a, int64 b);
extern int32 jit_numeric_le_precompiled(int64 a, int64 b);
extern int32 jit_numeric_gt_precompiled(int64 a, int64 b);
extern int32 jit_numeric_ge_precompiled(int64 a, int64 b);
extern int32 jit_numeric_cmp_precompiled(int64 a, int64 b);
extern int64 jit_numeric_add_precompiled(int64 a, int64 b);
extern int64 jit_numeric_sub_precompiled(int64 a, int64 b);
extern int64 jit_numeric_mul_precompiled(int64 a, int64 b);
extern int32 jit_hash_numeric_precompiled(int64 a);
/* Text precompiled functions removed: require collation (PG_GET_COLLATION) */
extern int32 jit_interval_eq_precompiled(int64 a, int64 b);
extern int32 jit_interval_lt_precompiled(int64 a, int64 b);
extern int32 jit_interval_cmp_precompiled(int64 a, int64 b);
extern int32 jit_uuid_eq_precompiled(int64 a, int64 b);
extern int32 jit_uuid_lt_precompiled(int64 a, int64 b);
extern int32 jit_uuid_cmp_precompiled(int64 a, int64 b);
extern int32 jit_uuid_hash_precompiled(int64 a);
#endif /* PG_JITTER_HAVE_TIER2 */

#endif /* PG_JIT_FUNCS_H */
