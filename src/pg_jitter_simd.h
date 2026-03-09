/*
 * pg_jitter_simd.h — SIMD-accelerated helpers for pg_jitter
 *
 * Provides hardware-accelerated text comparison, integer array search,
 * and null bitmap checking using StringZilla and NEON intrinsics.
 */
#ifndef PG_JITTER_SIMD_H
#define PG_JITTER_SIMD_H

#include "postgres.h"
#include "fmgr.h"

/* ================================================================
 * SIMD text ops (C-collation / deterministic fast path)
 * ================================================================ */
extern int32 simd_texteq(int64 a, int64 b, int32 collid);
extern int32 simd_textne(int64 a, int64 b, int32 collid);
extern int32 simd_text_lt(int64 a, int64 b, int32 collid);
extern int32 simd_text_le(int64 a, int64 b, int32 collid);
extern int32 simd_text_gt(int64 a, int64 b, int32 collid);
extern int32 simd_text_ge(int64 a, int64 b, int32 collid);
extern int32 simd_bttextcmp(int64 a, int64 b, int32 collid);
extern int32 simd_hashtext(int64 a);

/* ================================================================
 * StringZilla fast-path LIKE matching
 * ================================================================ */
#define LIKE_MATCH_EXACT    0
#define LIKE_MATCH_PREFIX   1
#define LIKE_MATCH_SUFFIX   2
#define LIKE_MATCH_INTERIOR 3

extern int32 simd_like_match_text(int64 datum, int64 pattern_ptr,
                                  int32 pattern_len, int32 match_type);
extern int simd_like_classify(const char *pattern, int patlen,
                              const char **literal_out, int *literal_len_out);

/* ================================================================
 * Compiled LIKE matching (handles ALL case-sensitive patterns)
 * ================================================================ */
#define SZ_LIKE_MAX_SEGMENTS 16
#define SZ_LIKE_MAX_PIECES   8

typedef struct { uint8 offset; uint8 len; const char *data; } SzPiece;
typedef struct { uint8 width; uint8 num_pieces; uint8 longest_idx; SzPiece pieces[SZ_LIKE_MAX_PIECES]; } SzSegment;
typedef struct {
	int16 min_len;
	uint8 num_segments;
	bool anchored_start;
	bool anchored_end;
	SzSegment segments[SZ_LIKE_MAX_SEGMENTS];
} SzLikeCompiled;

/*
 * Sentinel: pattern has floating segments with underscores.
 * Backend should skip both compiled LIKE and Vectorscan (use V1).
 */
#define SIMD_LIKE_USE_V1 ((SzLikeCompiled *)(intptr_t)1)

extern SzLikeCompiled *simd_like_compile(const char *pattern, int patlen);
extern int32 simd_like_match_compiled(int64 datum, int64 compiled_ptr);

/* ================================================================
 * SIMD integer array search (for SCALARARRAYOP)
 * ================================================================ */
extern int32 simd_int4_array_eq(int32 val, const int32 *data, int nitems);
extern int32 simd_int8_array_eq(int64 val, const int64 *data, int nitems);

/* ================================================================
 * SIMD null bitmap bulk check (for deform)
 * ================================================================ */
extern bool simd_nullbitmap_all_notnull(const uint8 *bits, int ncols);

/* ================================================================
 * SIMD batch value extraction (for deform uniform int32 columns)
 * ================================================================ */
extern void simd_extract_int32_values(const char *tupdata, Datum *values,
                                      int count);

/* ================================================================
 * SCALARARRAYOP helper (partially inlined from JIT, handles element loop)
 * ================================================================ */
struct ExprEvalStep;
extern void pg_jitter_scalararrayop_loop(struct ExprEvalStep *op,
                                         void *arr, Datum scalar_value,
                                         bool scalar_null);

#endif /* PG_JITTER_SIMD_H */
