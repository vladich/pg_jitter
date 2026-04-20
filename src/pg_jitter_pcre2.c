/*
 * pg_jitter_pcre2.c — PCRE2 pattern cache and match wrappers
 *
 * Uses PCRE2 with PCRE2_UTF + PCRE2_UCP for Unicode-aware regex matching.
 * POSIX character classes ([:alpha:], [:upper:], etc.) correctly match
 * Unicode code points, making this safe for non-C/POSIX collations.
 *
 * Compiled patterns are JIT-compiled via PCRE2's built-in SLJIT JIT and
 * cached in a per-backend LRU cache for reuse across rows and queries.
 */
#include "postgres.h"

#ifdef PG_JITTER_HAVE_PCRE2

#include "pg_jitter_pcre2.h"
#include "pg_jitter_compat.h"

#include "fmgr.h"
#include "access/detoast.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#include "port/pg_crc32c.h"
#ifdef _WIN64
#include "pg_crc32c_compat.h"
#endif

/* Pre-allocated stack size for JIT backtracking (matches PCRE2's MACHINE_STACK_SIZE) */
#define PJ_STACK_SIZE   32768

/* ================================================================
 * Pattern cache
 * ================================================================ */
#define PCRE2_CACHE_SIZE 64

typedef struct Pcre2CacheKey {
	char    pattern[256]; /* truncated pattern for hash key */
	int     patlen;       /* full pattern length */
	uint32  flags;        /* PCRE2 compile flags */
} Pcre2CacheKey;

typedef struct Pcre2CacheSlot {
	Pcre2CacheKey     key;
	Pcre2CacheEntry   entry;
	int               lru_counter;
} Pcre2CacheSlot;

static Pcre2CacheSlot pcre2_cache[PCRE2_CACHE_SIZE];
static int pcre2_cache_used = 0;
static int pcre2_lru_clock = 0;

static uint32
pcre2_hash_key(const char *pattern, int patlen, uint32 flags)
{
	pg_crc32c crc;
	int len = patlen < 256 ? patlen : 256;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, pattern, len);
	COMP_CRC32C(crc, &flags, sizeof(flags));
	COMP_CRC32C(crc, &patlen, sizeof(patlen));
	FIN_CRC32C(crc);
	return (uint32) crc;
}

/* ================================================================
 * LIKE → regex conversion
 * ================================================================ */
static char *
like_to_regex(const char *like_pat, int like_len, int *regex_len_out)
{
	/* Worst case: each char becomes \x (2 bytes) + anchors ^...$ */
	int max_len = like_len * 2 + 3;
	char *buf = MemoryContextAlloc(TopMemoryContext, max_len);
	int pos = 0;

	/*
	 * Optimization: strip leading/trailing % and omit ^/$ anchors
	 * accordingly. This avoids PCRE2 generating a greedy ^.* that
	 * scans to end-of-string and backtracks. Instead, PCRE2 uses
	 * its built-in first-char optimization (memchr) to find the
	 * first literal, then scans forward.
	 *
	 * LIKE 'abc'     → ^abc$     (exact match)
	 * LIKE '%abc'    → abc$      (suffix match, no leading .*)
	 * LIKE 'abc%'    → ^abc      (prefix match, no trailing .*)
	 * LIKE '%abc%'   → abc       (interior, unanchored)
	 * LIKE '%a%b%'   → a.*b      (no leading/trailing .*)
	 */
	bool anchored_start = true;
	bool anchored_end = true;
	int start = 0;
	int end = like_len;

	/* Strip leading % */
	while (start < end && like_pat[start] == '%') {
		anchored_start = false;
		start++;
	}
	/* Strip trailing % */
	while (end > start && like_pat[end - 1] == '%') {
		anchored_end = false;
		end--;
	}

	if (anchored_start)
		buf[pos++] = '^';

	for (int i = start; i < end; i++)
	{
		char c = like_pat[i];

		if (c == '%')
		{
			buf[pos++] = '.';
			buf[pos++] = '*';
		}
		else if (c == '_')
		{
			buf[pos++] = '.';
		}
		else if (c == '\\' && i + 1 < end)
		{
			i++;
			c = like_pat[i];
			if (c == '.' || c == '+' || c == '*' || c == '?' ||
				c == '[' || c == ']' || c == '(' || c == ')' ||
				c == '{' || c == '}' || c == '^' || c == '$' ||
				c == '|' || c == '\\')
				buf[pos++] = '\\';
			buf[pos++] = c;
		}
		else
		{
			if (c == '.' || c == '+' || c == '*' || c == '?' ||
				c == '[' || c == ']' || c == '(' || c == ')' ||
				c == '{' || c == '}' || c == '^' || c == '$' ||
				c == '|' || c == '\\')
				buf[pos++] = '\\';
			buf[pos++] = c;
		}
	}

	if (anchored_end)
		buf[pos++] = '$';
	buf[pos] = '\0';

	*regex_len_out = pos;
	return buf;
}

/* ================================================================
 * Compile and cache a pattern
 * ================================================================ */
Pcre2CacheEntry *
pg_jitter_pcre2_compile(const char *pattern, int patlen,
                         bool is_like, bool case_insensitive)
{
	uint32 pcre2_flags = PCRE2_UTF | PCRE2_UCP | PCRE2_DOTALL;
	char *regex_str;
	int regex_len;
	int errcode;
	PCRE2_SIZE erroffset;
	pcre2_code *code = NULL;
	pcre2_match_data *match_data = NULL;

	if (case_insensitive)
		pcre2_flags |= PCRE2_CASELESS;

	/* Convert LIKE to regex if needed */
	if (is_like)
	{
		regex_str = like_to_regex(pattern, patlen, &regex_len);
	}
	else
	{
		regex_str = MemoryContextAlloc(TopMemoryContext, patlen + 1);
		memcpy(regex_str, pattern, patlen);
		regex_str[patlen] = '\0';
		regex_len = patlen;
	}

	/* Check cache */
	(void) pcre2_hash_key(regex_str, regex_len, pcre2_flags);

	for (int i = 0; i < pcre2_cache_used; i++)
	{
		Pcre2CacheSlot *s = &pcre2_cache[i];
		int key_len = s->key.patlen < 256 ? s->key.patlen : 256;
		int cmp_len = regex_len < 256 ? regex_len : 256;

		if (s->key.flags == pcre2_flags &&
			s->key.patlen == regex_len &&
			key_len == cmp_len &&
			memcmp(s->key.pattern, regex_str, cmp_len) == 0)
		{
			/* Cache hit */
			s->lru_counter = ++pcre2_lru_clock;
			pfree(regex_str);
			return &s->entry;
		}
	}

	/* Compile pattern */
	code = pcre2_compile((PCRE2_SPTR)regex_str, regex_len,
	                     pcre2_flags, &errcode, &erroffset, NULL);
	if (!code)
	{
		pfree(regex_str);
		return NULL;
	}

	/* JIT compile — if it fails, fall back to V1 */
	if (pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) != 0)
	{
		pcre2_code_free(code);
		pfree(regex_str);
		return NULL;
	}

	/*
	 * Create reusable match_data with the pattern's capture capacity.  This uses
	 * PCRE2's public API instead of reading executable_jit->top_bracket.
	 */
	match_data = pcre2_match_data_create_from_pattern(code, NULL);
	if (!match_data)
	{
		pcre2_code_free(code);
		pfree(regex_str);
		return NULL;
	}

	/* Find or evict a cache slot */
	int slot_idx;
	if (pcre2_cache_used < PCRE2_CACHE_SIZE)
	{
		slot_idx = pcre2_cache_used++;
	}
	else
	{
		/* LRU eviction */
		int min_lru = pcre2_cache[0].lru_counter;
		slot_idx = 0;
		for (int i = 1; i < PCRE2_CACHE_SIZE; i++)
		{
			if (pcre2_cache[i].lru_counter < min_lru)
			{
				min_lru = pcre2_cache[i].lru_counter;
				slot_idx = i;
			}
		}
		/* Free old entry */
#ifdef PCRE2_JIT_FAST_API
		if (pcre2_cache[slot_idx].entry.jit_fast)
			pcre2_jit_fast_context_free(pcre2_cache[slot_idx].entry.jit_fast);
#endif
		if (pcre2_cache[slot_idx].entry.match_context)
			pcre2_match_context_free(pcre2_cache[slot_idx].entry.match_context);
		if (pcre2_cache[slot_idx].entry.jit_stack)
			pcre2_jit_stack_free(pcre2_cache[slot_idx].entry.jit_stack);
		if (pcre2_cache[slot_idx].entry.code)
			pcre2_code_free(pcre2_cache[slot_idx].entry.code);
		if (pcre2_cache[slot_idx].entry.match_data)
			pcre2_match_data_free(pcre2_cache[slot_idx].entry.match_data);
		if (pcre2_cache[slot_idx].entry.pattern)
			pfree(pcre2_cache[slot_idx].entry.pattern);
	}

	/* Fill cache slot */
	Pcre2CacheSlot *slot = &pcre2_cache[slot_idx];
	int key_copy = regex_len < 256 ? regex_len : 256;
	memcpy(slot->key.pattern, regex_str, key_copy);
	if (key_copy < 256)
		slot->key.pattern[key_copy] = '\0';
	slot->key.patlen = regex_len;
	slot->key.flags = pcre2_flags;

	slot->entry.pattern = regex_str; /* transfer ownership */
	slot->entry.flags = pcre2_flags;
	slot->entry.code = code;
	slot->entry.match_data = match_data;
	slot->entry.match_context = NULL;
	slot->entry.jit_stack = pcre2_jit_stack_create(PJ_STACK_SIZE,
	                                               PJ_STACK_SIZE * 4, NULL);
#ifdef PCRE2_JIT_FAST_API
	slot->entry.jit_fast = NULL;
#endif

	if (slot->entry.jit_stack)
	{
#ifdef PCRE2_JIT_FAST_API
		slot->entry.jit_fast =
			pcre2_jit_fast_context_create(code, PCRE2_JIT_COMPLETE,
			                              match_data, slot->entry.jit_stack);
		if (!slot->entry.jit_fast)
		{
			slot->entry.match_context = pcre2_match_context_create(NULL);
			if (slot->entry.match_context)
				pcre2_jit_stack_assign(slot->entry.match_context, NULL,
				                       slot->entry.jit_stack);
			else
			{
				pcre2_jit_stack_free(slot->entry.jit_stack);
				slot->entry.jit_stack = NULL;
			}
		}
#else
		slot->entry.match_context = pcre2_match_context_create(NULL);
		if (slot->entry.match_context)
			pcre2_jit_stack_assign(slot->entry.match_context, NULL,
			                       slot->entry.jit_stack);
		else
		{
			pcre2_jit_stack_free(slot->entry.jit_stack);
			slot->entry.jit_stack = NULL;
		}
#endif
	}

	slot->lru_counter = ++pcre2_lru_clock;

	return &slot->entry;
}

/* ================================================================
 * Match a text buffer against a compiled pattern
 * ================================================================ */
bool
pg_jitter_pcre2_match(Pcre2CacheEntry *entry, const char *data, int len)
{
	int rc;

	rc = pcre2_jit_match(entry->code,
	                     (PCRE2_SPTR)data, len,
	                     0,        /* start_offset */
	                     0,        /* options */
	                     entry->match_data,
	                     entry->match_context);

	/*
	 * rc > 0: match found (number of ovector pairs filled)
	 * rc == 0: match found but ovector too small (still a match)
	 * rc == PCRE2_ERROR_NOMATCH (-1): no match
	 * rc < -1: some other error
	 */
	return (rc >= 0);
}

int32
pg_jitter_pcre2_match_raw(int64 entry_ptr, int64 data_ptr, int32 len)
{
	Pcre2CacheEntry *entry = (Pcre2CacheEntry *)entry_ptr;
	const uint8_t *data = (const uint8_t *)(uintptr_t)data_ptr;
	int rc;

#ifdef PCRE2_JIT_FAST_API
	if (entry->jit_fast)
		rc = pcre2_jit_fast_match(entry->jit_fast, (PCRE2_SPTR)data,
		                          len, 0, 0);
	else
#endif
		rc = pcre2_jit_match(entry->code,
		                     (PCRE2_SPTR)data, len,
		                     0, 0,
		                     entry->match_data,
		                     entry->match_context);

	return (rc >= 0) ? 1 : 0;
}

/* ================================================================
 * JIT-callable wrapper: text Datum + Pcre2CacheEntry* → int32
 *
 * With the bundled patched PCRE2, uses an opaque fast-JIT context that keeps
 * PCRE2's private argument state inside PCRE2.  System PCRE2 builds use the
 * public pcre2_jit_match() API with an assigned reusable JIT stack.
 * ================================================================ */
int32
pg_jitter_pcre2_match_text(int64 datum, int64 entry_ptr)
{
	Pcre2CacheEntry *entry = (Pcre2CacheEntry *)entry_ptr;
	Datum d = (Datum)datum;
	text *t = DatumGetTextPP(d);
	int len = VARSIZE_ANY_EXHDR(t);
	const uint8_t *data = (const uint8_t *)VARDATA_ANY(t);
	int32 result = pg_jitter_pcre2_match_raw(entry_ptr, (int64)(uintptr_t)data,
	                                         len);

	if ((Pointer)t != DatumGetPointer(d))
		pfree(t);

	return result;
}

#endif /* PG_JITTER_HAVE_PCRE2 */
