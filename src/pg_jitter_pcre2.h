/*
 * pg_jitter_pcre2.h — PCRE2 integration for regex and LIKE matching
 * with JIT compilation.
 *
 * Provides compiled-pattern caching and a fast match wrapper that
 * JIT-compiled expression code can call instead of PG's character-level
 * RE_compile_and_execute / MatchText.
 *
 * PCRE2 flags are selected from the database encoding and collation.  UTF8
 * LIKE uses PCRE2_UTF where the collation semantics are compatible.  Fixed-width
 * single-byte encodings use byte-mode PCRE2 only where byte semantics match
 * PostgreSQL semantics.
 */
#ifndef PG_JITTER_PCRE2_H
#define PG_JITTER_PCRE2_H

#include "postgres.h"
#include "pg_jitter_common.h"

#ifdef PG_JITTER_HAVE_PCRE2

#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

#if defined(PCRE2_JIT_FAST_API) && defined(PG_JITTER_HAVE_PCRE2_JIT_FAST_CONTEXT_FREE)
#define PG_JITTER_USE_PCRE2_JIT_FAST_API 1
#endif

/* ================================================================
 * Pattern cache entry
 * ================================================================ */
typedef struct Pcre2CacheEntry {
	char              *pattern;      /* regex string (palloc'd) */
	int                pattern_len;  /* regex string length */
	uint32             flags;        /* PCRE2 compile flags */
	pcre2_code        *code;         /* compiled + JIT-compiled code */
	pcre2_match_data  *match_data;   /* reusable match_data */
	pcre2_match_context *match_context; /* resource limits + public JIT stack */
	pcre2_jit_stack   *jit_stack;    /* reusable public PCRE2 JIT stack */
	int                refcount;     /* generated-code references */
	bool               in_cache;     /* present in the process-local LRU */
	uint64             lru_counter;  /* cache recency stamp */

	/*
	 * Opaque PCRE2 fast-JIT state.  Available with the bundled patched PCRE2
	 * API only when it also exposes a matching destructor.  Other PCRE2 builds
	 * use match_context + jit_stack above.
	 */
#ifdef PG_JITTER_USE_PCRE2_JIT_FAST_API
	pcre2_jit_fast_context *jit_fast;
#endif
} Pcre2CacheEntry;

/*
 * Compile and cache a PCRE2 pattern with JIT.
 *
 * For LIKE patterns, set is_like=true and the function converts SQL LIKE
 * syntax (%, _) to regex.  For raw regex patterns, set is_like=false; the
 * implementation converts the PostgreSQL ARE subset it can prove equivalent to
 * PCRE2 and returns NULL for PostgreSQL fallback otherwise.
 *
 * Returns a Pcre2CacheEntry pointer pinned to the owning JIT context, or NULL
 * if compilation/JIT fails.  Cache eviction removes entries from future lookup,
 * but pinned entries remain valid until that JIT context is released.
 */
extern bool pg_jitter_pcre2_is_eligible(Oid collid, bool is_like,
                                         bool case_insensitive);

extern Pcre2CacheEntry *pg_jitter_pcre2_compile(PgJitterContext *ctx,
                                                 const char *pattern, int patlen,
                                                 bool is_like,
                                                 bool case_insensitive,
                                                 Oid collid);

/*
 * Match a text value against a precompiled PCRE2 pattern.
 * Returns true on match, false otherwise.
 */
extern bool pg_jitter_pcre2_match(Pcre2CacheEntry *entry,
                                   const char *data, int len);

/*
 * JIT-callable wrapper for already-detoasted text data.
 * Signature: int32(int64 entry_ptr, int64 data_ptr, int32 len).
 */
extern int32 pg_jitter_pcre2_match_raw(int64 entry_ptr, int64 data_ptr,
                                       int32 len);

/*
 * JIT-callable wrapper: takes text Datum and Pcre2CacheEntry pointer,
 * returns int32 (1 = match, 0 = no match).
 * Handles detoasting internally.
 * Signature: int32(int64 datum, int64 entry_ptr).
 */
extern int32 pg_jitter_pcre2_match_text(int64 datum, int64 entry_ptr);

#endif /* PG_JITTER_HAVE_PCRE2 */

#endif /* PG_JITTER_PCRE2_H */
