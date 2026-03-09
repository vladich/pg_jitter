/*
 * pg_jitter_vectorscan.h — Vectorscan (Hyperscan fork) integration for
 * SIMD-accelerated LIKE and regex matching.
 *
 * Provides compiled-pattern caching and a fast match wrapper that
 * JIT-compiled expression code can call instead of PG's character-level
 * MatchText / RE_compile_and_execute.
 */
#ifndef PG_JITTER_VECTORSCAN_H
#define PG_JITTER_VECTORSCAN_H

#include "postgres.h"

#ifdef PG_JITTER_HAVE_VECTORSCAN

#include "hs.h"

/* ================================================================
 * Pattern cache entry
 * ================================================================ */
typedef struct VsCacheEntry {
	char           *pattern;     /* original LIKE/regex string (palloc'd) */
	uint32          flags;       /* HS_FLAG_CASELESS, HS_FLAG_UTF8, etc. */
	hs_database_t  *db;          /* compiled database */
	hs_scratch_t   *scratch;     /* per-backend-process scratch space */
} VsCacheEntry;

/*
 * Compile and cache a Vectorscan pattern.
 *
 * For LIKE patterns, set is_like=true and the function converts SQL LIKE
 * syntax (%, _) to regex.  For raw regex patterns, set is_like=false.
 *
 * Returns a cached VsCacheEntry pointer valid for the backend's lifetime,
 * or NULL if compilation fails (pattern too complex, etc.).
 */
extern VsCacheEntry *pg_jitter_vs_compile(const char *pattern, int patlen,
                                           bool is_like, bool case_insensitive,
                                           bool is_utf8);

/*
 * Match a text value against a precompiled Vectorscan pattern.
 * Returns true on match, false otherwise.
 */
extern bool pg_jitter_vs_match(VsCacheEntry *entry,
                                const char *data, int len);

/*
 * JIT-callable wrapper: takes text Datum and VsCacheEntry pointer,
 * returns int32 (1 = match, 0 = no match).
 * Handles detoasting internally.
 */
extern int32 pg_jitter_vs_match_text(int64 datum, int64 entry_ptr);

#endif /* PG_JITTER_HAVE_VECTORSCAN */

#endif /* PG_JITTER_VECTORSCAN_H */
