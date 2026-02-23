/*-------------------------------------------------------------------------
 *
 * pg_jit_deform_templates.h
 *		Pre-compiled deform functions for common fixed-width column patterns.
 *
 * These functions live in the shared library's .text section, so all
 * parallel workers share the same virtual address — eliminating the
 * L1 I-cache coldness that occurs when each worker independently
 * compiles deform into separate sljit code buffers.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_JIT_DEFORM_TEMPLATES_H
#define PG_JIT_DEFORM_TEMPLATES_H

#include "postgres.h"
#include "executor/tuptable.h"

/*
 * Signature encoding: pack (natts, attlen[0..natts-1]) into a uint16.
 *
 * Bits [2:0] = natts - 1  (0..4 → natts 1..5)
 * Bits [4:3] = col0 encoding
 * Bits [6:5] = col1 encoding
 * ...
 * Bits [2*i+4 : 2*i+3] = col_i encoding
 *
 * attlen encoding: 1→0, 2→1, 4→2, 8→3
 *
 * Max natts=5 uses bits [0..12] → fits in uint16.
 */
static inline uint16
deform_signature(int natts, const int16 *attlens)
{
	uint16	sig = (uint16)(natts - 1);	/* bits [2:0] */

	for (int i = 0; i < natts; i++)
	{
		uint16	enc;

		switch (attlens[i])
		{
			case 1: enc = 0; break;
			case 2: enc = 1; break;
			case 4: enc = 2; break;
			case 8: enc = 3; break;
			default: return 0xFFFF;	/* unsupported */
		}
		sig |= enc << (3 + i * 2);
	}
	return sig;
}

typedef void (*deform_template_fn)(TupleTableSlot *slot);

typedef struct DeformTemplate
{
	uint16				signature;
	deform_template_fn	fn;
} DeformTemplate;

/* Generated sorted array + count */
extern const DeformTemplate jit_deform_templates[];
extern const int jit_deform_templates_count;

/*
 * Look up a pre-compiled deform template by signature.
 * Returns function pointer or NULL if no match.
 * Uses binary search on the sorted template array.
 */
extern deform_template_fn jit_deform_find_template(uint16 sig);

#endif /* PG_JIT_DEFORM_TEMPLATES_H */
