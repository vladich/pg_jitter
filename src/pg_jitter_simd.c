/*
 * pg_jitter_simd.c — SIMD-accelerated helpers for pg_jitter
 *
 * Uses StringZilla for SIMD text comparison (NEON/SSE4.2) and
 * NEON intrinsics for integer array search and null bitmap checks.
 */
#include "postgres.h"
#include "pg_jitter_compat.h"
#include "fmgr.h"
#include "utils/varlena.h"
#include "access/detoast.h"
#include "catalog/pg_collation_d.h"
#include "mb/pg_wchar.h"
#include "common/hashfn.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#include "pg_jitter_common.h"
#include "pg_jitter_simd.h"

/*
 * StringZilla configuration: we only need compare + hash.
 * Define SZ_DYNAMIC_DISPATCH=0 to use compile-time NEON detection.
 */
#define SZ_DYNAMIC_DISPATCH 0
#include "stringzilla/stringzilla.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PG_JITTER_HAVE_NEON 1
#endif

/* ================================================================
 * Internal: detoast + extract text data
 * ================================================================ */
#define JIT_FREE_IF_COPY(t, d)                                                 \
  do {                                                                         \
    if ((Pointer)(t) != DatumGetPointer(d))                                    \
      pfree((t));                                                              \
  } while (0)

#include "utils/pg_locale.h"

/*
 * Check if we can use byte-level SIMD comparison for this collation.
 * Returns true for C collation and deterministic collations that are
 * byte-level comparable (collate_is_c flag on locale struct).
 */
static inline bool
collation_is_c_or_posix(Oid collid)
{
	return pg_jitter_collation_is_c(collid);
}

/*
 * SIMD text comparison using StringZilla for C/POSIX collation.
 * Falls back to varstr_cmp for non-C collations.
 * Returns memcmp-style result: <0, 0, >0.
 */
static int
simd_text_cmp_internal(Datum a, Datum b, Oid collid)
{
	text *t1 = DatumGetTextPP(a);
	text *t2 = DatumGetTextPP(b);
	int len1 = VARSIZE_ANY_EXHDR(t1);
	int len2 = VARSIZE_ANY_EXHDR(t2);
	int result;

	if (collation_is_c_or_posix(collid)) {
		result = (int)sz_order(VARDATA_ANY(t1), len1, VARDATA_ANY(t2), len2);
	} else {
		result = varstr_cmp(VARDATA_ANY(t1), len1, VARDATA_ANY(t2), len2,
		                    collid);
	}

	JIT_FREE_IF_COPY(t1, a);
	JIT_FREE_IF_COPY(t2, b);
	return result;
}

/* ================================================================
 * SIMD text comparison functions
 * ================================================================ */
int32
simd_texteq(int64 a, int64 b, int32 collid)
{
	Datum da = (Datum)a, db = (Datum)b;

	/*
	 * For any deterministic collation, equality = byte equality.
	 * Use StringZilla sz_equal (SIMD-accelerated) for the comparison.
	 * Only non-deterministic collations need locale-aware comparison.
	 */
	if (pg_jitter_collation_is_deterministic((Oid)collid)) {
		if (toast_raw_datum_size(da) != toast_raw_datum_size(db))
			return 0;

		text *t1 = DatumGetTextPP(da);
		text *t2 = DatumGetTextPP(db);
		int len1 = VARSIZE_ANY_EXHDR(t1);
		int result = sz_equal(VARDATA_ANY(t1), VARDATA_ANY(t2), len1)
		                 ? 1
		                 : 0;
		JIT_FREE_IF_COPY(t1, da);
		JIT_FREE_IF_COPY(t2, db);
		return result;
	}

	return simd_text_cmp_internal(da, db, (Oid)collid) == 0 ? 1 : 0;
}

int32
simd_textne(int64 a, int64 b, int32 collid)
{
	Datum da = (Datum)a, db = (Datum)b;

	if (pg_jitter_collation_is_deterministic((Oid)collid)) {
		if (toast_raw_datum_size(da) != toast_raw_datum_size(db))
			return 1;

		text *t1 = DatumGetTextPP(da);
		text *t2 = DatumGetTextPP(db);
		int len1 = VARSIZE_ANY_EXHDR(t1);
		int result = sz_equal(VARDATA_ANY(t1), VARDATA_ANY(t2), len1)
		                 ? 0
		                 : 1;
		JIT_FREE_IF_COPY(t1, da);
		JIT_FREE_IF_COPY(t2, db);
		return result;
	}

	return simd_text_cmp_internal(da, db, (Oid)collid) != 0 ? 1 : 0;
}

int32
simd_text_lt(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) < 0 ? 1 : 0;
}

int32
simd_text_le(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) <= 0 ? 1 : 0;
}

int32
simd_text_gt(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) > 0 ? 1 : 0;
}

int32
simd_text_ge(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) >= 0 ? 1 : 0;
}

int32
simd_bttextcmp(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid);
}

int32
simd_hashtext(int64 a)
{
	Datum da = (Datum)a;
	text *t = DatumGetTextPP(da);
	int len = VARSIZE_ANY_EXHDR(t);
	/*
	 * Must use PG's hash_any (Jenkins lookup3) — NOT sz_hash.
	 * Hash values must match PG's built-in hashtext exactly for
	 * hash join and hash aggregate correctness.
	 */
	Datum result = hash_any((const unsigned char *)VARDATA_ANY(t), len);
	JIT_FREE_IF_COPY(t, da);
	return DatumGetInt32(result);
}

/* ================================================================
 * StringZilla fast-path LIKE matching
 *
 * For simple LIKE patterns, StringZilla substring search is faster
 * than PCRE2 regex compilation.  The JIT compiler classifies
 * the pattern at compile time and passes the match_type:
 *   0 = exact match (no wildcards)
 *   1 = prefix match (pattern%)
 *   2 = suffix match (%pattern)
 *   3 = interior substring (%pattern%)
 * ================================================================ */

/* Match types for simd_like_match_text */
#define LIKE_MATCH_EXACT    0
#define LIKE_MATCH_PREFIX   1
#define LIKE_MATCH_SUFFIX   2
#define LIKE_MATCH_INTERIOR 3

int32
simd_like_match_text(int64 datum, int64 pattern_ptr, int32 pattern_len,
                     int32 match_type)
{
	text *t = DatumGetTextPP((Datum)datum);
	const char *data = VARDATA_ANY(t);
	int data_len = VARSIZE_ANY_EXHDR(t);
	const char *pat = (const char *)(uintptr_t)pattern_ptr;
	int32 result;

	switch (match_type)
	{
		case LIKE_MATCH_EXACT:
			result = (data_len == pattern_len &&
			          sz_equal(data, pat, data_len)) ? 1 : 0;
			break;

		case LIKE_MATCH_PREFIX:
			result = (data_len >= pattern_len &&
			          memcmp(data, pat, pattern_len) == 0) ? 1 : 0;
			break;

		case LIKE_MATCH_SUFFIX:
			result = (data_len >= pattern_len &&
			          memcmp(data + data_len - pattern_len, pat,
			                 pattern_len) == 0) ? 1 : 0;
			break;

		case LIKE_MATCH_INTERIOR:
		{
			sz_cptr_t found = sz_find(data, data_len, pat, pattern_len);
			result = (found != NULL) ? 1 : 0;
			break;
		}

		default:
			result = 0;
			break;
	}

	JIT_FREE_IF_COPY(t, (Datum)datum);
	return result;
}

/*
 * Classify a LIKE pattern at JIT compile time.
 * Returns match_type (0-3) and extracts the literal substring.
 * Returns -1 if the pattern is too complex for StringZilla fast path.
 */
int
simd_like_classify(const char *pattern, int patlen,
                   const char **literal_out, int *literal_len_out)
{
	bool has_leading_pct = false;
	bool has_trailing_pct = false;
	int start = 0, end = patlen;

	/* Strip leading % */
	if (patlen > 0 && pattern[0] == '%') {
		has_leading_pct = true;
		start = 1;
	}

	/* Strip trailing % */
	if (end > start && pattern[end - 1] == '%') {
		has_trailing_pct = true;
		end--;
	}

	/* Check remaining literal for wildcards or escapes */
	for (int i = start; i < end; i++) {
		if (pattern[i] == '%' || pattern[i] == '_' || pattern[i] == '\\')
			return -1; /* too complex */
	}

	*literal_out = pattern + start;
	*literal_len_out = end - start;

	if (*literal_len_out == 0) {
		/* Pattern is just % or %% — matches everything */
		*literal_out = NULL;
		return LIKE_MATCH_EXACT; /* not really, but result is always true */
	}

	if (!has_leading_pct && !has_trailing_pct)
		return LIKE_MATCH_EXACT;
	else if (!has_leading_pct && has_trailing_pct)
		return LIKE_MATCH_PREFIX;
	else if (has_leading_pct && !has_trailing_pct)
		return LIKE_MATCH_SUFFIX;
	else
		return LIKE_MATCH_INTERIOR; /* %literal% — sz_find SIMD substring search */
}

/* ================================================================
 * Compiled LIKE matching — handles ALL case-sensitive patterns
 *
 * Parses LIKE pattern into segments (separated by %) each containing
 * literal pieces at fixed offsets (separated by _).  Uses sz_find
 * for SIMD-accelerated substring search on the longest piece.
 * ================================================================ */

SzLikeCompiled *
simd_like_compile(const char *pattern, int patlen)
{
	/*
	 * Decode pattern: resolve backslash escapes and classify each
	 * logical character as LITERAL (0), PERCENT (1), or UNDERSCORE (2).
	 */
#define PC_LITERAL    0
#define PC_PERCENT    1
#define PC_UNDERSCORE 2

	uint8 *kinds = palloc(patlen);  /* at most patlen logical chars */
	char *chars = palloc(patlen);
	int nchars = 0;

	for (int i = 0; i < patlen; i++) {
		if (pattern[i] == '\\') {
			if (i + 1 >= patlen) {
				/* Dangling escape at end of pattern */
				pfree(kinds); pfree(chars);
				return NULL;
			}
			i++;
			kinds[nchars] = PC_LITERAL;
			chars[nchars] = pattern[i];
			nchars++;
		} else if (pattern[i] == '%') {
			kinds[nchars] = PC_PERCENT;
			chars[nchars] = '%';
			nchars++;
		} else if (pattern[i] == '_') {
			kinds[nchars] = PC_UNDERSCORE;
			chars[nchars] = '_';
			nchars++;
		} else {
			kinds[nchars] = PC_LITERAL;
			chars[nchars] = pattern[i];
			nchars++;
		}
	}

	/* Strip leading/trailing unescaped % */
	bool anchored_start = true;
	bool anchored_end = true;
	int start = 0;
	int end = nchars;

	if (nchars > 0 && kinds[0] == PC_PERCENT) {
		anchored_start = false;
		start = 1;
	}
	if (end > start && kinds[end - 1] == PC_PERCENT) {
		anchored_end = false;
		end--;
	}

	/* Count segments (separated by %) and total literal bytes */
	int num_segments = 0;
	int total_literal = 0;
	bool has_underscore = false;
	int seg_start = start;

	for (int i = start; i <= end; i++) {
		if (i == end || kinds[i] == PC_PERCENT) {
			int seg_len = i - seg_start;
			if (seg_len > 0) {
				num_segments++;
				if (num_segments > SZ_LIKE_MAX_SEGMENTS) {
					pfree(kinds); pfree(chars);
					return NULL;
				}
				for (int j = seg_start; j < i; j++) {
					if (kinds[j] == PC_LITERAL)
						total_literal++;
					else if (kinds[j] == PC_UNDERSCORE)
						has_underscore = true;
				}
			}
			seg_start = i + 1;
		}
	}

	/* Pattern like %%% — matches everything */
	if (num_segments == 0) {
		pfree(kinds); pfree(chars);
		SzLikeCompiled *c = (SzLikeCompiled *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(SzLikeCompiled));
		c->min_len = 0;
		c->num_segments = 0;
		c->anchored_start = false;
		c->anchored_end = false;
		return c;
	}

	/*
	 * Route patterns based on what's fastest:
	 * - Single-segment interior, no underscore (%literal%): PCRE2's
	 *   literal matcher is fastest (85% on short, 29% on long strings).
	 * - Everything else: V1 (PG's MatchText) — its tight inline loop
	 *   beats compiled LIKE's function call + struct access overhead
	 *   on short strings.  Compiled LIKE's memchr advantage only
	 *   shows on strings >100 bytes, but those are rare in practice.
	 */
	{
		int anchored_count = 0;
		if (anchored_start) anchored_count++;
		if (anchored_end && num_segments > anchored_count) anchored_count++;
		int num_floating = num_segments - anchored_count;

		if (num_floating > 0 && num_segments == 1 && !has_underscore) {
			/* Single-segment interior: PCRE2 literal matcher */
			pfree(kinds); pfree(chars);
			return NULL;
		}
		/* All other patterns: V1 is faster for short strings */
		pfree(kinds); pfree(chars);
		return SIMD_LIKE_USE_V1;
	}

	/* Allocate struct + trailing literal data */
	Size alloc_size = sizeof(SzLikeCompiled) + total_literal;
	SzLikeCompiled *c = (SzLikeCompiled *)
		MemoryContextAllocZero(TopMemoryContext, alloc_size);
	char *lit_buf = (char *)c + sizeof(SzLikeCompiled);
	char *lit_pos = lit_buf;

	c->anchored_start = anchored_start;
	c->anchored_end = anchored_end;
	c->num_segments = num_segments;
	c->min_len = 0;

	/* Second pass: populate segments and pieces */
	int seg_idx = 0;
	seg_start = start;

	for (int i = start; i <= end; i++) {
		if (i == end || kinds[i] == PC_PERCENT) {
			int seg_len = i - seg_start;
			if (seg_len > 0) {
				SzSegment *seg = &c->segments[seg_idx];
				seg->width = seg_len;
				c->min_len += seg_len;

				/* Extract pieces (runs of LITERAL characters) */
				int num_pieces = 0;
				int piece_start = -1;
				int longest_len = 0;

				for (int j = 0; j <= seg_len; j++) {
					int pos = seg_start + j;
					bool is_wild = (j == seg_len) ||
					               (kinds[pos] == PC_UNDERSCORE);

					if (!is_wild && piece_start < 0)
						piece_start = j;
					else if (is_wild && piece_start >= 0) {
						int plen = j - piece_start;
						if (num_pieces >= SZ_LIKE_MAX_PIECES) {
							pfree(c); pfree(kinds); pfree(chars);
							return NULL;
						}
						SzPiece *p = &seg->pieces[num_pieces];
						p->offset = piece_start;
						p->len = plen;
						/* Copy decoded literal data */
						for (int k = 0; k < plen; k++)
							lit_pos[k] = chars[seg_start + piece_start + k];
						p->data = lit_pos;
						lit_pos += plen;

						if (plen > longest_len) {
							longest_len = plen;
							seg->longest_idx = num_pieces;
						}
						num_pieces++;
						piece_start = -1;
					}
				}
				seg->num_pieces = num_pieces;
				seg_idx++;
			}
			seg_start = i + 1;
		}
	}

	pfree(kinds);
	pfree(chars);
	return c;
}

int32
simd_like_match_compiled(int64 datum, int64 compiled_ptr)
{
	SzLikeCompiled *c = (SzLikeCompiled *)(uintptr_t)compiled_ptr;
	text *t = DatumGetTextPP((Datum)datum);
	const char *data = VARDATA_ANY(t);
	int data_len = VARSIZE_ANY_EXHDR(t);
	int32 result = 0;

	/* Quick reject on minimum length */
	if (data_len < c->min_len)
		goto done;

	/* 0 segments means match-all (pattern was just %) */
	if (c->num_segments == 0) {
		result = 1;
		goto done;
	}

	int pos = 0;           /* current position in data */
	int remaining = data_len;
	int first_seg = 0;
	int last_seg = c->num_segments;

	/* Anchored start: first segment must match at position 0 */
	if (c->anchored_start) {
		SzSegment *seg = &c->segments[0];
		if (remaining < seg->width)
			goto done;
		/* Verify all pieces at their fixed offsets (inline) */
		for (int p = 0; p < seg->num_pieces; p++) {
			SzPiece *pc = &seg->pieces[p];
			const char *s = data + pos + pc->offset;
			for (int b = 0; b < pc->len; b++) {
				if (s[b] != pc->data[b])
					goto done;
			}
		}
		pos += seg->width;
		remaining -= seg->width;
		first_seg = 1;
	}

	/* Anchored end: last segment must match at end */
	if (c->anchored_end && last_seg > first_seg) {
		SzSegment *seg = &c->segments[last_seg - 1];
		if (remaining < seg->width)
			goto done;
		int end_pos = data_len - seg->width;
		/* Check no overlap with anchored start */
		if (end_pos < pos)
			goto done;
		for (int p = 0; p < seg->num_pieces; p++) {
			SzPiece *pc = &seg->pieces[p];
			const char *s = data + end_pos + pc->offset;
			for (int b = 0; b < pc->len; b++) {
				if (s[b] != pc->data[b])
					goto done;
			}
		}
		remaining -= seg->width;
		last_seg--;
	}

	/* Floating segments: memchr for SIMD-accelerated first-byte search,
	 * then inline byte comparison to avoid memcmp function call overhead. */
	for (int s = first_seg; s < last_seg; s++) {
		SzSegment *seg = &c->segments[s];
		if (remaining < seg->width)
			goto done;

		if (seg->num_pieces == 0) {
			/* Segment is all underscores — just consume width bytes */
			pos += seg->width;
			remaining -= seg->width;
			continue;
		}

		/* Use longest piece as search anchor */
		SzPiece *anchor = &seg->pieces[seg->longest_idx];
		int anchor_off = anchor->offset;
		int anchor_len = anchor->len;
		const char *anchor_data = anchor->data;
		int max_pos = pos + remaining - seg->width;
		bool found = false;
		const char *scan = data + pos + anchor_off;
		const char *scan_end = data + max_pos + anchor_off;

		while (scan <= scan_end) {
			const char *hit = (const char *)memchr(
			    scan, (unsigned char)anchor_data[0],
			    scan_end - scan + 1);
			if (!hit)
				break;

			int seg_pos = (int)(hit - data) - anchor_off;

			/* Verify anchor piece (inline byte comparison) */
			if (anchor_len > 1) {
				const char *a = hit + 1;
				const char *b = anchor_data + 1;
				bool ok = true;
				for (int i = 0; i < anchor_len - 1; i++) {
					if (a[i] != b[i]) { ok = false; break; }
				}
				if (!ok) { scan = hit + 1; continue; }
			}

			/* Verify other pieces (inline byte comparison) */
			bool pieces_ok = true;
			for (int p = 0; p < seg->num_pieces; p++) {
				if (p == seg->longest_idx)
					continue;
				SzPiece *pc = &seg->pieces[p];
				const char *a = data + seg_pos + pc->offset;
				const char *b = pc->data;
				for (int i = 0; i < pc->len; i++) {
					if (a[i] != b[i]) {
						pieces_ok = false;
						break;
					}
				}
				if (!pieces_ok)
					break;
			}

			if (pieces_ok) {
				pos = seg_pos + seg->width;
				remaining = data_len - pos;
				found = true;
				break;
			}
			scan = hit + 1;
		}

		if (!found)
			goto done;
	}

	/* If anchored_end and the end segment was NOT handled separately
	 * (single segment with both anchors), verify exact length match.
	 * When last_seg < num_segments, the end was already verified from
	 * the right side, so any gap is absorbed by the % between segments. */
	if (c->anchored_end && last_seg == c->num_segments && pos != data_len)
		goto done;

	/* If not anchored_end, any trailing data is fine (implicit %) */
	result = 1;

done:
	JIT_FREE_IF_COPY(t, (Datum)datum);
	return result;
}

/* ================================================================
 * SIMD integer array search (for SCALARARRAYOP int IN())
 * ================================================================ */
int32
simd_int4_array_eq(int32 val, const int32 *data, int nitems)
{
#ifdef PG_JITTER_HAVE_NEON
	int32x4_t vval = vdupq_n_s32(val);
	int i = 0;
	for (; i + 4 <= nitems; i += 4) {
		int32x4_t vdata = vld1q_s32(data + i);
		uint32x4_t cmp = vceqq_s32(vdata, vval);
		if (vmaxvq_u32(cmp) != 0)
			return 1;
	}
	for (; i < nitems; i++)
		if (data[i] == val)
			return 1;
	return 0;
#else
	for (int i = 0; i < nitems; i++)
		if (data[i] == val)
			return 1;
	return 0;
#endif
}

int32
simd_int8_array_eq(int64 val, const int64 *data, int nitems)
{
#ifdef PG_JITTER_HAVE_NEON
	int64x2_t vval = vdupq_n_s64(val);
	int i = 0;
	for (; i + 2 <= nitems; i += 2) {
		int64x2_t vdata = vld1q_s64(data + i);
		uint64x2_t cmp = vceqq_s64(vdata, vval);
		/* Check if any lane matched */
		if (vgetq_lane_u64(cmp, 0) | vgetq_lane_u64(cmp, 1))
			return 1;
	}
	for (; i < nitems; i++)
		if (data[i] == val)
			return 1;
	return 0;
#else
	for (int i = 0; i < nitems; i++)
		if (data[i] == val)
			return 1;
	return 0;
#endif
}

/* ================================================================
 * SIMD null bitmap bulk check (for deform)
 * ================================================================ */
bool
simd_nullbitmap_all_notnull(const uint8 *bits, int ncols)
{
	int nbytes = (ncols + 7) / 8;

#ifdef PG_JITTER_HAVE_NEON
	int i = 0;
	for (; i + 16 <= nbytes; i += 16) {
		uint8x16_t v = vld1q_u8(bits + i);
		if (vminvq_u8(v) != 0xFF)
			return false;
	}
	for (; i < nbytes; i++)
		if (bits[i] != 0xFF)
			return false;
#else
	for (int i = 0; i < nbytes; i++)
		if (bits[i] != 0xFF)
			return false;
#endif
	/* Mask out trailing bits in the last byte */
	int trailing = ncols % 8;
	if (trailing != 0) {
		uint8 mask = (1 << trailing) - 1;
		if ((bits[nbytes - 1] & mask) != mask)
			return false;
	}
	return true;
}

/* ================================================================
 * SIMD batch value extraction (for deform — uniform int32 columns)
 * ================================================================ */
void
simd_extract_int32_values(const char *tupdata, Datum *values, int count)
{
#ifdef PG_JITTER_HAVE_NEON
	int i = 0;
	for (; i + 4 <= count; i += 4) {
		int32x4_t v = vld1q_s32((const int32 *)(tupdata + i * 4));
		int64x2_t lo = vmovl_s32(vget_low_s32(v));
		int64x2_t hi = vmovl_s32(vget_high_s32(v));
		vst1q_s64((int64 *)(values + i), lo);
		vst1q_s64((int64 *)(values + i + 2), hi);
	}
	for (; i < count; i++)
		values[i] = Int32GetDatum(*(int32 *)(tupdata + i * 4));
#else
	for (int i = 0; i < count; i++)
		values[i] = Int32GetDatum(*(int32 *)(tupdata + i * 4));
#endif
}

/* ================================================================
 * SCALARARRAYOP helper — element iteration with SIMD dispatch
 * ================================================================ */
#include "executor/execExpr.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/fmgrprotos.h"

void
pg_jitter_scalararrayop_loop(ExprEvalStep *op, void *arr_ptr,
                             Datum scalar_value, bool scalar_null)
{
	ArrayType *arr = (ArrayType *)arr_ptr;
	FunctionCallInfo fcinfo = op->d.scalararrayop.fcinfo_data;
	bool useOr = op->d.scalararrayop.useOr;
	bool strictfunc = op->d.scalararrayop.finfo->fn_strict;
	int nitems;
	int16 typlen;
	bool typbyval;
	char typalign;
	char *s;
	bits8 *bitmap;
	int bitmask;
	Datum result;
	bool resultnull;

	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0) {
		*op->resvalue = BoolGetDatum(!useOr);
		*op->resnull = false;
		return;
	}

	if (scalar_null && strictfunc) {
		*op->resnull = true;
		return;
	}

	/* Cache element type info */
	if (op->d.scalararrayop.element_type != ARR_ELEMTYPE(arr)) {
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
		                     &op->d.scalararrayop.typlen,
		                     &op->d.scalararrayop.typbyval,
		                     &op->d.scalararrayop.typalign);
		op->d.scalararrayop.element_type = ARR_ELEMTYPE(arr);
	}
	typlen = op->d.scalararrayop.typlen;
	typbyval = op->d.scalararrayop.typbyval;
	typalign = op->d.scalararrayop.typalign;

	/*
	 * SIMD fast path for int4eq/int8eq with no-null arrays.
	 * Check if fn_addr matches int4eq or int8eq.
	 */
	bitmap = ARR_NULLBITMAP(arr);
	if (bitmap == NULL && typbyval && useOr) {
		PGFunction fn = op->d.scalararrayop.fn_addr;
		if (fn == int4eq && typlen == 4) {
			bool found = simd_int4_array_eq(
			    DatumGetInt32(scalar_value),
			    (const int32 *)ARR_DATA_PTR(arr), nitems);
			*op->resvalue = BoolGetDatum(found);
			*op->resnull = false;
			return;
		}
		if (fn == int8eq && typlen == 8) {
			bool found = simd_int8_array_eq(
			    DatumGetInt64(scalar_value),
			    (const int64 *)ARR_DATA_PTR(arr), nitems);
			*op->resvalue = BoolGetDatum(found);
			*op->resnull = false;
			return;
		}
	}

	/* Generic per-element loop */
	fcinfo->args[0].value = scalar_value;
	fcinfo->args[0].isnull = scalar_null;

	result = BoolGetDatum(!useOr);
	resultnull = false;

	s = (char *)ARR_DATA_PTR(arr);
	bitmask = 1;

	for (int i = 0; i < nitems; i++) {
		Datum elt;
		Datum thisresult;

		if (bitmap && (*bitmap & bitmask) == 0) {
			fcinfo->args[1].value = (Datum)0;
			fcinfo->args[1].isnull = true;
		} else {
			elt = fetch_att(s, typbyval, typlen);
			s = att_addlength_pointer(s, typlen, s);
			s = (char *)att_align_nominal(s, typalign);
			fcinfo->args[1].value = elt;
			fcinfo->args[1].isnull = false;
		}

		if (fcinfo->args[1].isnull && strictfunc) {
			fcinfo->isnull = true;
			thisresult = (Datum)0;
		} else {
			fcinfo->isnull = false;
			thisresult = op->d.scalararrayop.fn_addr(fcinfo);
		}

		if (fcinfo->isnull)
			resultnull = true;
		else if (useOr) {
			if (DatumGetBool(thisresult)) {
				result = BoolGetDatum(true);
				resultnull = false;
				break;
			}
		} else {
			if (!DatumGetBool(thisresult)) {
				result = BoolGetDatum(false);
				resultnull = false;
				break;
			}
		}

		if (bitmap) {
			bitmask <<= 1;
			if (bitmask == 0x100) {
				bitmap++;
				bitmask = 1;
			}
		}
	}

	*op->resvalue = result;
	*op->resnull = resultnull;
}

/* ================================================================
 * JSONB fast-path extraction: doc->>'key' with constant key
 *
 * Bypasses FunctionCallInfo overhead by calling
 * getKeyJsonValueFromContainer directly with pre-extracted key.
 *
 * Returns text Datum, or 0 with *isnull=true for NULL/missing.
 * ================================================================ */
#include "utils/jsonb.h"
#include "utils/builtins.h"
#include "utils/numeric.h"

int64
jit_jsonb_object_field_text(int64 jb_datum, int64 key_ptr,
                             int32 key_len, int64 isnull_ptr)
{
	bool *isnull = (bool *)(uintptr_t)isnull_ptr;
	Jsonb *jb = DatumGetJsonbP((Datum)jb_datum);
	const char *key = (const char *)(uintptr_t)key_ptr;
	JsonbValue vbuf;
	JsonbValue *v;

	if (!JB_ROOT_IS_OBJECT(jb))
	{
		*isnull = true;
		return (int64)0;
	}

	v = getKeyJsonValueFromContainer(&jb->root, key, key_len, &vbuf);

	if (v == NULL || v->type == jbvNull)
	{
		*isnull = true;
		return (int64)0;
	}

	*isnull = false;

	switch (v->type)
	{
		case jbvString:
			return (int64)(Datum)PointerGetDatum(
				cstring_to_text_with_len(v->val.string.val,
				                         v->val.string.len));

		case jbvNumeric:
		{
			char *cstr = DatumGetCString(
				DirectFunctionCall1(numeric_out,
				                    PointerGetDatum(v->val.numeric)));
			return (int64)(Datum)PointerGetDatum(cstring_to_text(cstr));
		}

		case jbvBool:
			return v->val.boolean
				? (int64)(Datum)PointerGetDatum(
					cstring_to_text_with_len("true", 4))
				: (int64)(Datum)PointerGetDatum(
					cstring_to_text_with_len("false", 5));

		default:
			/* jbvBinary (nested object/array) — fall back to NULL
			 * rather than pulling in JsonbToCString dependency */
			*isnull = true;
			return (int64)0;
	}
}
