/*
 * pg_jitter_pcre2.h — PCRE2 integration for Unicode-aware
 * regex and LIKE matching with JIT compilation.
 *
 * Provides compiled-pattern caching and a fast match wrapper that
 * JIT-compiled expression code can call instead of PG's character-level
 * RE_compile_and_execute / MatchText.
 *
 * PCRE2 with PCRE2_UTF + PCRE2_UCP correctly handles Unicode POSIX
 * character classes ([:alpha:], [:upper:], etc.), making it safe for
 * any deterministic collation including ICU and non-C locales.
 */
#ifndef PG_JITTER_PCRE2_H
#define PG_JITTER_PCRE2_H

#include "postgres.h"

#ifdef PG_JITTER_HAVE_PCRE2

#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

/* ================================================================
 * Pattern cache entry
 * ================================================================ */
typedef struct Pcre2CacheEntry {
	char              *pattern;      /* regex string (palloc'd) */
	uint32             flags;        /* PCRE2 compile flags */
	pcre2_code        *code;         /* compiled + JIT-compiled code */
	pcre2_match_data  *match_data;   /* reusable match_data (oveccount=1) */

	/*
	 * Direct JIT call state — bypasses pcre2_jit_match() overhead.
	 * At runtime, only str/begin/end are updated per row; everything
	 * else is pre-filled at compile time.  Saves ~35ns/row by avoiding:
	 * - pcre2_jit_match argument setup (14 fields)
	 * - 32KB stack allocation in jit_machine_stack_exec
	 * - result writeback to match_data
	 */
	void              *jit_func;     /* PCRE2 JIT function pointer, or NULL */
	void              *jit_direct;   /* pre-allocated Pcre2DirectState */
} Pcre2CacheEntry;

/*
 * Compile and cache a PCRE2 pattern with JIT.
 *
 * For LIKE patterns, set is_like=true and the function converts SQL LIKE
 * syntax (%, _) to regex.  For raw regex patterns, set is_like=false.
 *
 * Returns a cached Pcre2CacheEntry pointer valid for the backend's lifetime,
 * or NULL if compilation/JIT fails.
 */
extern Pcre2CacheEntry *pg_jitter_pcre2_compile(const char *pattern, int patlen,
                                                 bool is_like,
                                                 bool case_insensitive);

/*
 * Match a text value against a precompiled PCRE2 pattern.
 * Returns true on match, false otherwise.
 */
extern bool pg_jitter_pcre2_match(Pcre2CacheEntry *entry,
                                   const char *data, int len);

/*
 * JIT-callable wrapper: takes text Datum and Pcre2CacheEntry pointer,
 * returns int32 (1 = match, 0 = no match).
 * Handles detoasting internally.
 * Signature: int32(int64 datum, int64 entry_ptr).
 */
extern int32 pg_jitter_pcre2_match_text(int64 datum, int64 entry_ptr);

#endif /* PG_JITTER_HAVE_PCRE2 */

#endif /* PG_JITTER_PCRE2_H */
