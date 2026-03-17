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
 * Backend should skip both compiled LIKE and PCRE2 (use V1).
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
 * CRC32 open-addressing hash table for large IN lists (> 128 elements)
 *
 * Built at JIT compile time from constant array values.
 * At runtime: crc32(val) & mask → linear probe.
 * ~3 cycles per lookup vs PG's ~30 cycles (Jenkins + chained buckets).
 * ================================================================ */
typedef struct Crc32HashTable {
	int32    mask;         /* table_size - 1 (power of 2) */
	int32    nitems;       /* number of values stored */
	bool     has_nulls;    /* array contained NULLs */
	int32    table[];      /* open-addressing slots, INT32_MIN = empty */
} Crc32HashTable;

/* Build hash table from sorted int32 values (called at JIT compile time) */
extern Crc32HashTable *crc32_hash_build_int4(const int32 *vals, int nvals,
                                              bool has_nulls);

/* Probe: returns 1 if found, 0 if not (JIT-callable) */
extern int32 crc32_hash_probe_int4(int32 val, int64 table_ptr);

/* ================================================================
 * Runtime binary search for large IN lists (> 256 elements)
 *
 * Sorted int32 array built at JIT compile time, searched at runtime.
 * O(log n) comparisons, ~15 instructions, zero code bloat.
 * ================================================================ */
typedef struct SortedInt32Array {
	int32    nvals;
	bool     has_nulls;
	int32    vals[];      /* sorted values */
} SortedInt32Array;

/* Build sorted array (called at JIT compile time) */
extern SortedInt32Array *sorted_array_build_int4(const int32 *vals, int nvals,
                                                  bool has_nulls);

/* Binary search probe: returns 1 if found, 0 if not (JIT-callable) */
extern int32 sorted_array_probe_int4(int32 val, int64 array_ptr);


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

/* ================================================================
 * JSONB fast-path extraction (bypasses FunctionCallInfo overhead)
 *
 * JIT-callable wrapper for doc->>'key' with compile-time constant key.
 * Calls getKeyJsonValueFromContainer directly, skipping PG_GETARG_TEXT_PP
 * for the key argument and FunctionCallInfo setup entirely.
 *
 * Returns: text Datum on success, 0 with *isnull=true on NULL/not-found.
 * ================================================================ */
extern int64 jit_jsonb_object_field_text(int64 jb_datum, int64 key_ptr,
                                          int32 key_len, int64 isnull_ptr);

/* ================================================================
 * Text IN-list hash table (for HASHED_SCALARARRAYOP with text)
 *
 * Open-addressing hash table built at JIT compile time from constant
 * text array elements. At runtime: hybrid CRC32/sz_hash probe.
 * ================================================================ */
typedef struct TextHashEntry {
    uint32   hash;      /* hybrid hash value, 0 = empty slot */
    uint32   len;       /* text data length (excl varlena header) */
    const char *data;   /* pointer to text data (TopMemoryContext copy) */
} TextHashEntry;

typedef struct TextHashTable {
    int32    mask;       /* table_size - 1 (power of 2) */
    int32    nitems;
    bool     has_nulls;
    TextHashEntry entries[];  /* open-addressing, hash=0 = empty */
} TextHashTable;

/* Build text hash table from constant text Datum array (JIT compile time) */
extern TextHashTable *text_hash_build(Datum *text_datums, int nvals,
                                       bool has_nulls);

/* Runtime probe: returns 1 if found, 0 if not (JIT-callable, 2 args) */
extern int32 text_hash_probe(int64 datum, int64 table_ptr);

#endif /* PG_JITTER_SIMD_H */
