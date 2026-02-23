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
 * Lookup entry: maps PG fn_addr → direct-call function + metadata.
 * Searched once per expression step at JIT compile time, not per row.
 */
typedef struct JitDirectFn
{
	PGFunction   pg_fn;        /* PG's V1 function address (e.g., int4pl) */
	void        *jit_fn;       /* our unwrapped native version, or NULL */
	uint8        nargs;        /* number of native args (0-4) */
	uint8        ret_type;     /* JIT_TYPE_32 or JIT_TYPE_64 */
	uint8        arg_types[4]; /* type of each arg */
} JitDirectFn;

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
	return result;
}

#endif /* PG_JIT_FUNCS_H */
