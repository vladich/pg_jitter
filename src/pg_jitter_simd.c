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

#include "utils/formatting.h"  /* str_toupper, str_tolower */
#include "pg_jitter_common.h"
#include "pg_jitter_simd.h"

#include <limits.h>

#include "pg_jitter_stringzilla_config.h"
#include "stringzilla/find.h"
#include "stringzilla/hash.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PG_JITTER_HAVE_NEON 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
/* MSVC x64 always has SSE2; SSE4.2 intrinsics available via intrin.h */
#include <intrin.h>
#include <nmmintrin.h>
#define PG_JITTER_HAVE_SSE2 1
#else
#ifdef __SSE2__
#include <emmintrin.h>
#define PG_JITTER_HAVE_SSE2 1
#endif
#ifdef __SSE4_1__
#include <smmintrin.h>
#define PG_JITTER_HAVE_SSE41 1
#endif
#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif
#endif /* _MSC_VER */
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
		/*
		 * C/POSIX collation: unsigned byte comparison (same as memcmp).
		 * Cannot use sz_order here — it does SIGNED byte comparison
		 * which gives wrong results for bytes >= 0x80 (e.g., UTF-8
		 * multi-byte chars like ä = 0xC3A4 would sort before 'b' = 0x62).
		 */
		int cmplen = (len1 < len2) ? len1 : len2;
		result = memcmp(VARDATA_ANY(t1), VARDATA_ANY(t2), cmplen);
		if (result == 0)
			result = (len1 > len2) ? 1 : (len1 < len2) ? -1 : 0;
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

int64
simd_text_larger(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) >= 0 ? a : b;
}

int64
simd_text_smaller(int64 a, int64 b, int32 collid)
{
	return simd_text_cmp_internal((Datum)a, (Datum)b, (Oid)collid) <= 0 ? a : b;
}

/*
 * SIMD upper/lower for ASCII text in C/POSIX collation.
 * For non-ASCII or non-C collation, falls back to PG's str_toupper/tolower.
 */
int64
simd_upper(int64 a, int32 collid)
{
	Datum da = (Datum)a;
	struct varlena *v = (struct varlena *)DatumGetPointer(a);
	text *detoasted = NULL;
	char *data;
	int len;
	char *out_str = NULL;
	text *result = NULL;

	if (unlikely(VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v))) {
		detoasted = DatumGetTextPP(da);
		data = VARDATA_ANY(detoasted);
		len = VARSIZE_ANY_EXHDR(detoasted);
	} else {
		data = VARDATA_ANY(v);
		len = VARSIZE_ANY_EXHDR(v);
	}

	PG_TRY();
	{
		/* Non-C collation: use PG's locale-aware str_toupper */
		if (!pg_jitter_collation_is_c((Oid)collid)) {
			int rlen;

			out_str = str_toupper(data, len, (Oid)collid);
			rlen = strlen(out_str);
			result = (text *)palloc(VARHDRSZ + rlen);
			SET_VARSIZE(result, VARHDRSZ + rlen);
			memcpy(VARDATA(result), out_str, rlen);
			pfree(out_str);
			out_str = NULL;
		} else {
			/* C/POSIX: ASCII fast-path, convert a-z to A-Z */
			char *out;

			result = (text *)palloc(VARHDRSZ + len);
			SET_VARSIZE(result, VARHDRSZ + len);
			out = VARDATA(result);
			for (int i = 0; i < len; i++) {
				unsigned char c = (unsigned char)data[i];
				out[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
			}
		}
	}
	PG_CATCH();
	{
		if (out_str != NULL)
			pfree(out_str);
		if (detoasted != NULL)
			JIT_FREE_IF_COPY(detoasted, da);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (detoasted != NULL)
		JIT_FREE_IF_COPY(detoasted, da);
	return PointerGetDatum(result);
}

int64
simd_lower(int64 a, int32 collid)
{
	Datum da = (Datum)a;
	struct varlena *v = (struct varlena *)DatumGetPointer(a);
	text *detoasted = NULL;
	char *data;
	int len;
	char *out_str = NULL;
	text *result = NULL;

	if (unlikely(VARATT_IS_EXTERNAL(v) || VARATT_IS_COMPRESSED(v))) {
		detoasted = DatumGetTextPP(da);
		data = VARDATA_ANY(detoasted);
		len = VARSIZE_ANY_EXHDR(detoasted);
	} else {
		data = VARDATA_ANY(v);
		len = VARSIZE_ANY_EXHDR(v);
	}

	PG_TRY();
	{
		if (!pg_jitter_collation_is_c((Oid)collid)) {
			int rlen;

			out_str = str_tolower(data, len, (Oid)collid);
			rlen = strlen(out_str);
			result = (text *)palloc(VARHDRSZ + rlen);
			SET_VARSIZE(result, VARHDRSZ + rlen);
			memcpy(VARDATA(result), out_str, rlen);
			pfree(out_str);
			out_str = NULL;
		} else {
			char *out;

			result = (text *)palloc(VARHDRSZ + len);
			SET_VARSIZE(result, VARHDRSZ + len);
			out = VARDATA(result);
			for (int i = 0; i < len; i++) {
				unsigned char c = (unsigned char)data[i];
				out[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
			}
		}
	}
	PG_CATCH();
	{
		if (out_str != NULL)
			pfree(out_str);
		if (detoasted != NULL)
			JIT_FREE_IF_COPY(detoasted, da);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (detoasted != NULL)
		JIT_FREE_IF_COPY(detoasted, da);
	return PointerGetDatum(result);
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

bool
simd_like_byte_search_is_eligible(Oid collid)
{
	if (!pg_jitter_collation_is_deterministic(collid))
		return false;

	if (pg_database_encoding_max_length() == 1)
		return true;

	return GetDatabaseEncoding() == PG_UTF8;
}

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

	if (pg_database_encoding_max_length() != 1 &&
		GetDatabaseEncoding() != PG_UTF8)
		return -1;

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

	if (*literal_len_out == 0 && (has_leading_pct || has_trailing_pct)) {
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
		return LIKE_MATCH_INTERIOR; /* %literal% — StringZilla sz_find */
}

const char *
simd_like_copy_literal(const char *literal, int literal_len, PgJitterContext *ctx)
{
	char *copy;

	if (literal == NULL)
		return NULL;

	if (literal_len <= 0)
		return pg_jitter_context_alloc(ctx, 1);

	copy = pg_jitter_context_alloc(ctx, literal_len);
	memcpy(copy, literal, literal_len);
	return copy;
}

/* ================================================================
 * Compiled LIKE matching — handles byte-width case-sensitive patterns
 *
 * Parses LIKE pattern into segments (separated by %) each containing
 * literal pieces at fixed offsets (separated by _).  Uses sz_find
 * for SIMD-accelerated substring search on the longest piece.
 * ================================================================ */

SzLikeCompiled *
simd_like_compile(const char *pattern, int patlen, PgJitterContext *ctx)
{
	/*
	 * Decode pattern: resolve backslash escapes and classify each
	 * logical character as LITERAL (0), PERCENT (1), or UNDERSCORE (2).
	 */
#define PC_LITERAL    0
#define PC_PERCENT    1
#define PC_UNDERSCORE 2

	if (patlen < 0)
		return NULL;

	if (pg_database_encoding_max_length() != 1 &&
		GetDatabaseEncoding() != PG_UTF8)
		return NULL;

	Size scratch_size = patlen > 0 ? (Size)patlen : 1;
	uint8 *kinds = palloc(scratch_size);  /* at most patlen logical chars */
	char *chars = palloc(scratch_size);
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

	/*
	 * The compiled matcher advances "_" by one byte.  PostgreSQL LIKE "_"
	 * matches one character, so multibyte encodings need PCRE2 or V1.
	 */
	if (has_underscore && pg_database_encoding_max_length() != 1) {
		pfree(kinds); pfree(chars);
		return NULL;
	}

	/* Pattern like %%% matches everything; empty pattern matches only empty. */
	if (num_segments == 0) {
		bool match_all = (nchars > 0);

		pfree(kinds); pfree(chars);
		SzLikeCompiled *c = (SzLikeCompiled *)
			pg_jitter_context_alloc_zero(ctx, sizeof(SzLikeCompiled));
		c->min_len = 0;
		c->num_segments = 0;
		c->anchored_start = !match_all;
		c->anchored_end = !match_all;
		return c;
	}

	if ((Size) total_literal > MaxAllocSize - sizeof(SzLikeCompiled)) {
		pfree(kinds); pfree(chars);
		return NULL;
	}

	{
		int anchored_count = 0;
		if (anchored_start) anchored_count++;
		if (anchored_end && num_segments > anchored_count) anchored_count++;
		int num_floating = num_segments - anchored_count;

		if (num_floating > 0 && num_segments == 1 && !has_underscore) {
			pfree(kinds); pfree(chars);
			if (total_literal < 5)
				return SIMD_LIKE_USE_V1; /* V1 beats PCRE2 on short needles */
			return NULL; /* PCRE2 handles it */
		}
	}

	/* Allocate struct + trailing literal data */
	Size alloc_size = sizeof(SzLikeCompiled) + total_literal;
	SzLikeCompiled *c = (SzLikeCompiled *)
		pg_jitter_context_alloc_zero(ctx, alloc_size);
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

	/* 0 segments means either match-all (%) or exact empty pattern. */
	if (c->num_segments == 0) {
		result = (!c->anchored_start && !c->anchored_end) || data_len == 0;
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
 * CRC32 open-addressing hash table for large IN lists
 * ================================================================ */
static inline uint32
pg_jitter_crc32c_sw_raw(uint32 crc, const void *data, Size len)
{
	const uint8 *p = (const uint8 *) data;

	while (len-- > 0)
	{
		crc ^= *p++;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
	}
	return crc;
}

static inline uint32
pg_jitter_crc32c_raw(uint32 crc, const void *data, Size len)
{
#if (defined(__x86_64__) && defined(__SSE4_2__)) || defined(_M_X64)
	if (pg_jitter_cpu_has_sse42())
	{
		const uint8 *p = (const uint8 *) data;

#if defined(_M_X64)
		for (; len >= 8; p += 8, len -= 8)
		{
			uint64 word64;

			memcpy(&word64, p, sizeof(word64));
			crc = (uint32) _mm_crc32_u64(crc, word64);
		}
#endif
		for (; len >= 4; p += 4, len -= 4)
		{
			uint32 word32;

			memcpy(&word32, p, sizeof(word32));
			crc = _mm_crc32_u32(crc, word32);
		}
		for (; len > 0; p++, len--)
			crc = _mm_crc32_u8(crc, *p);
		return crc;
	}
#endif

	return pg_jitter_crc32c_sw_raw(crc, data, len);
}

static inline uint32
crc32_hash_int4(int32 val)
{
	/* Keep the raw pre-FIN value. The JIT emits raw CRC32 instructions for
	 * this hash table, so build and probe must use the same convention. */
	uint32 word = (uint32) val;

	return pg_jitter_crc32c_raw(0xFFFFFFFFU, &word, sizeof(word));
}

Crc32HashTable *
crc32_hash_build_int4(const int32 *vals, int nvals, bool has_nulls,
					  PgJitterContext *ctx)
{
	/* Table size = next power of 2, at least 2x nvals for low collision rate */
	Size min_slots;
	Size table_size = 16;
	Size header_size = offsetof(Crc32HashTable, table);
	Size alloc;

	if (nvals <= 0 || (Size)nvals > (Size)INT_MAX / 2)
		elog(ERROR, "invalid int4 hash table size: %d values", nvals);

	min_slots = (Size)nvals * 2;
	while (table_size < min_slots)
	{
		if (table_size > (Size)INT_MAX / 2)
			elog(ERROR, "int4 hash table size overflow: %d values", nvals);
		table_size <<= 1;
	}

	if (header_size > MaxAllocSize ||
		table_size > (MaxAllocSize - header_size) / sizeof(Crc32HashSlot))
		elog(ERROR, "int4 hash table allocation too large: %d values", nvals);

	alloc = header_size + table_size * sizeof(Crc32HashSlot);
	Crc32HashTable *ht = pg_jitter_context_alloc_zero(ctx, alloc);
	ht->mask = (int32)table_size - 1;
	ht->nitems = nvals;
	ht->has_nulls = has_nulls;

	/* Insert values with linear probing */
	for (int i = 0; i < nvals; i++) {
		uint32 h = crc32_hash_int4(vals[i]) & ht->mask;
		while (ht->table[h].occupied)
			h = (h + 1) & ht->mask;
		ht->table[h].value = vals[i];
		ht->table[h].occupied = true;
	}

	return ht;
}

int32
crc32_hash_probe_int4(int32 val, int64 table_ptr)
{
	Crc32HashTable *ht = (Crc32HashTable *)table_ptr;
	uint32 h = crc32_hash_int4(val) & ht->mask;

	for (;;) {
		if (!ht->table[h].occupied)
			return 0;  /* not found */
		if (ht->table[h].value == val)
			return 1;  /* found */
		h = (h + 1) & ht->mask;
	}
}

/* ================================================================
 * Runtime binary search for large IN lists
 * ================================================================ */
SortedInt32Array *
sorted_array_build_int4(const int32 *vals, int nvals, bool has_nulls,
						PgJitterContext *ctx)
{
	Size alloc = offsetof(SortedInt32Array, vals) + nvals * sizeof(int32);
	SortedInt32Array *sa = pg_jitter_context_alloc(ctx, alloc);
	sa->nvals = nvals;
	sa->has_nulls = has_nulls;
	memcpy(sa->vals, vals, nvals * sizeof(int32));
	/* vals are already sorted from the extraction step */
	return sa;
}

int32
sorted_array_probe_int4(int32 val, int64 array_ptr)
{
	SortedInt32Array *sa = (SortedInt32Array *)array_ptr;
	const int32 *base = sa->vals;
	int n = sa->nvals;

	/*
	 * Branchless binary search (Knuth/Morin style).
	 *
	 * Key: don't check equality during the loop. Narrow to one element,
	 * then check. The conditional advance compiles to CSEL (ARM64) or
	 * CMOV (x86) — no branch misprediction.
	 *
	 * 1.7-2.3x faster than branchy binary search in microbenchmarks.
	 */
	while (n > 1) {
		int half = n >> 1;
#ifdef _MSC_VER
		_mm_prefetch((const char *)(base + (n >> 2)), _MM_HINT_T0);
#else
		__builtin_prefetch(base + (n >> 2));
#endif
		base += (base[half - 1] < val) * half;
		n -= half;
	}
	return (n > 0 && *base == val) ? 1 : 0;
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
		int64x2_t vdata = vld1q_s64((const long long *)(const void *)(data + i));
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
	int full_bytes = ncols >> 3;
	int trailing = ncols & 7;

#ifdef PG_JITTER_HAVE_NEON
	int i = 0;
	for (; i + 16 <= full_bytes; i += 16) {
		uint8x16_t v = vld1q_u8(bits + i);
		if (vminvq_u8(v) != 0xFF)
			return false;
	}
	for (; i < full_bytes; i++)
		if (bits[i] != 0xFF)
			return false;
#else
	for (int i = 0; i < full_bytes; i++)
		if (bits[i] != 0xFF)
			return false;
#endif

	if (trailing != 0) {
		uint8 mask = (1 << trailing) - 1;
		if ((bits[full_bytes] & mask) != mask)
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
		vst1q_s64((long long *)(void *)(values + i), lo);
		vst1q_s64((long long *)(void *)(values + i + 2), hi);
	}
	for (; i < count; i++)
		values[i] = Int32GetDatum(*(int32 *)(tupdata + i * 4));
#elif defined(PG_JITTER_HAVE_SSE41)
	/* SSE4.1: _mm_cvtepi32_epi64 sign-extends 2×int32 → 2×int64 */
	int i = 0;
	for (; i + 4 <= count; i += 4) {
		__m128i v = _mm_loadu_si128((const __m128i *)(tupdata + i * 4));
		__m128i lo = _mm_cvtepi32_epi64(v);
		_mm_storeu_si128((__m128i *)(values + i), lo);
		__m128i v_hi = _mm_srli_si128(v, 8);
		__m128i hi = _mm_cvtepi32_epi64(v_hi);
		_mm_storeu_si128((__m128i *)(values + i + 2), hi);
	}
	for (; i < count; i++)
		values[i] = Int32GetDatum(*(int32 *)(tupdata + i * 4));
#elif defined(PG_JITTER_HAVE_SSE2)
	/* SSE2: manual sign extension int32 → int64, 2 at a time */
	int i = 0;
	for (; i + 2 <= count; i += 2) {
		__m128i v = _mm_loadl_epi64((const __m128i *)(tupdata + i * 4));
		__m128i sign = _mm_srai_epi32(v, 31);
		__m128i ext = _mm_unpacklo_epi32(v, sign);
		_mm_storeu_si128((__m128i *)(values + i), ext);
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
 * Text IN-list hash table
 *
 * Hybrid hash: CRC32C for strings <= 16 bytes, StringZilla sz_hash
 * for longer strings. Open-addressing with linear probing.
 * Built at JIT compile time, probed at runtime.
 * ================================================================ */

static inline uint32
text_hybrid_hash(const char *data, int len)
{
    uint32 h;

    if (len <= 16)
    {
        /* Short strings: runtime-gated CRC32C, with a software fallback. */
        h = pg_jitter_crc32c_raw(0xFFFFFFFFU, data, (Size) len);
    }
    else
    {
        /* Long strings: StringZilla hash */
        h = (uint32)sz_hash(data, len, 0);
    }

    /* Ensure hash is never 0 (reserved for empty slots) */
    return h | 1;
}

TextHashTable *
text_hash_build(Datum *text_datums, int nvals, bool has_nulls,
				 PgJitterContext *ctx)
{
    /* Table size = next power of 2, at least 2x nvals */
    Size min_slots;
    Size table_size = 16;
    Size header_size = offsetof(TextHashTable, entries);
    Size alloc;

    if (nvals <= 0 || (Size)nvals > (Size)INT_MAX / 2)
        elog(ERROR, "invalid text hash table size: %d values", nvals);

    min_slots = (Size)nvals * 2;
    while (table_size < min_slots)
    {
        if (table_size > (Size)INT_MAX / 2)
            elog(ERROR, "text hash table size overflow: %d values", nvals);
        table_size <<= 1;
    }

    if (header_size > MaxAllocSize ||
        table_size > (MaxAllocSize - header_size) / sizeof(TextHashEntry))
        elog(ERROR, "text hash table allocation too large: %d values", nvals);

    alloc = header_size + table_size * sizeof(TextHashEntry);
    TextHashTable *ht = pg_jitter_context_alloc_zero(ctx, alloc);
    ht->mask = (int32)table_size - 1;
    ht->nitems = nvals;
    ht->has_nulls = has_nulls;
    /* entries[] already zeroed (hash=0 = empty) */

    for (int i = 0; i < nvals; i++)
    {
        Datum datum = text_datums[i];
        text *t = NULL;

        PG_TRY();
        {
            int len;
            const char *src;
            uint32 h;
            uint32 idx;
            char *datacopy;

            t = DatumGetTextPP(datum);
            len = VARSIZE_ANY_EXHDR(t);
            src = VARDATA_ANY(t);

            h = text_hybrid_hash(src, len);
            idx = h & ht->mask;

            /* Linear probe for empty slot */
            while (ht->entries[idx].hash != 0)
                idx = (idx + 1) & ht->mask;

            /* Copy text data into the JIT context for generated-code lifetime. */
            datacopy = pg_jitter_context_alloc(ctx, len);
            memcpy(datacopy, src, len);

            ht->entries[idx].hash = h;
            ht->entries[idx].len = len;
            ht->entries[idx].data = datacopy;

            JIT_FREE_IF_COPY(t, datum);
            t = NULL;
        }
        PG_CATCH();
        {
            if (t != NULL)
                JIT_FREE_IF_COPY(t, datum);
            PG_RE_THROW();
        }
        PG_END_TRY();
    }

    return ht;
}

TextHashTable *
text_hash_build_from_array(Datum array_datum, ExprEvalStep *op,
                           FunctionCallInfo fcinfo, bool *has_nulls_out,
                           PgJitterContext *ctx)
{
    ArrayType *arr = NULL;
    Datum *text_vals = NULL;
    TextHashTable *ht = NULL;
    bool has_nulls = false;

    if (has_nulls_out != NULL)
        *has_nulls_out = false;

    if (!pg_jitter_text_hash_saop_eligible(op, fcinfo))
        return NULL;

    PG_TRY();
    {
        int nitems;
        int16 typlen;
        bool typbyval;
        char typalign;

        arr = DatumGetArrayTypeP(array_datum);
        nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
        get_typlenbyvalalign(ARR_ELEMTYPE(arr), &typlen, &typbyval, &typalign);

        if (!typbyval && ARR_ELEMTYPE(arr) == TEXTOID &&
            nitems > 0 && nitems <= 16384)
        {
            bits8 *bitmap = ARR_NULLBITMAP(arr);
            char *s = (char *)ARR_DATA_PTR(arr);
            int bitmask = 1;
            int text_nvals = 0;

            text_vals = palloc(nitems * sizeof(Datum));

            for (int k = 0; k < nitems; k++)
            {
                if (bitmap && (*bitmap & bitmask) == 0)
                {
                    has_nulls = true;
                }
                else
                {
                    text_vals[text_nvals++] = fetch_att(s, false, typlen);
                    s = att_addlength_pointer(s, typlen, s);
                    s = (char *)att_align_nominal(s, typalign);
                }

                if (bitmap)
                {
                    bitmask <<= 1;
                    if (bitmask == 0x100)
                    {
                        bitmap++;
                        bitmask = 1;
                    }
                }
            }

            if (text_nvals > 0)
                ht = text_hash_build(text_vals, text_nvals, has_nulls, ctx);
        }
    }
    PG_CATCH();
    {
        if (text_vals != NULL)
            pfree(text_vals);
        if (arr != NULL && (Pointer)arr != DatumGetPointer(array_datum))
            pfree(arr);
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (text_vals != NULL)
        pfree(text_vals);
    if (arr != NULL && (Pointer)arr != DatumGetPointer(array_datum))
        pfree(arr);

    if (has_nulls_out != NULL)
        *has_nulls_out = has_nulls;

    return ht;
}

int32
text_hash_probe(int64 datum, int64 table_ptr)
{
    TextHashTable *ht = (TextHashTable *)(uintptr_t)table_ptr;
    text *t = DatumGetTextPP((Datum)datum);
    int len = VARSIZE_ANY_EXHDR(t);
    const char *data = VARDATA_ANY(t);

    uint32 h = text_hybrid_hash(data, len);
    uint32 idx = h & ht->mask;

    for (;;)
    {
        TextHashEntry *e = &ht->entries[idx];

        if (e->hash == 0)
        {
            /* Empty slot — not found */
            JIT_FREE_IF_COPY(t, (Datum)datum);
            return 0;
        }

        if (e->hash == h && e->len == (uint32)len &&
            sz_equal(data, e->data, len))
        {
            /* Found */
            JIT_FREE_IF_COPY(t, (Datum)datum);
            return 1;
        }

        idx = (idx + 1) & ht->mask;
    }
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
	Datum jb_input = (Datum)jb_datum;
	bool *isnull = (bool *)(uintptr_t)isnull_ptr;
	Jsonb *jb = DatumGetJsonbP(jb_input);
	const char *key = (const char *)(uintptr_t)key_ptr;
	JsonbValue vbuf;
	JsonbValue *v;
	int64 result = 0;
	char *cstr = NULL;
	StringInfoData jtext;
	bool jtext_valid = false;

	memset(&jtext, 0, sizeof(jtext));

	PG_TRY();
	{
		if (!JB_ROOT_IS_OBJECT(jb))
		{
			*isnull = true;
		}
		else
		{
			v = getKeyJsonValueFromContainer(&jb->root, key, key_len, &vbuf);

			if (v == NULL || v->type == jbvNull)
			{
				*isnull = true;
			}
			else
			{
				*isnull = false;

				switch (v->type)
				{
					case jbvString:
						result = (int64)(Datum)PointerGetDatum(
							cstring_to_text_with_len(v->val.string.val,
													 v->val.string.len));
						break;

					case jbvNumeric:
						cstr = DatumGetCString(
							DirectFunctionCall1(numeric_out,
												PointerGetDatum(v->val.numeric)));
						result = (int64)(Datum)PointerGetDatum(
							cstring_to_text(cstr));
						pfree(cstr);
						cstr = NULL;
						break;

					case jbvBool:
						result = v->val.boolean
							? (int64)(Datum)PointerGetDatum(
								cstring_to_text_with_len("true", 4))
							: (int64)(Datum)PointerGetDatum(
								cstring_to_text_with_len("false", 5));
						break;

					case jbvBinary:
						initStringInfo(&jtext);
						jtext_valid = true;
						JsonbToCString(&jtext, v->val.binary.data,
									   v->val.binary.len);
						result = (int64)(Datum)PointerGetDatum(
							cstring_to_text_with_len(jtext.data, jtext.len));
						pfree(jtext.data);
						jtext.data = NULL;
						jtext_valid = false;
						break;

					default:
						*isnull = true;
						break;
				}
			}
		}
	}
	PG_CATCH();
	{
		if (cstr != NULL)
			pfree(cstr);
		if (jtext_valid && jtext.data != NULL)
			pfree(jtext.data);
		JIT_FREE_IF_COPY(jb, jb_input);
		PG_RE_THROW();
	}
	PG_END_TRY();

	JIT_FREE_IF_COPY(jb, jb_input);
	return result;
}
