/*
 * pg_jitter_vectorscan.c — Vectorscan pattern cache and match wrappers
 *
 * Uses Vectorscan (community fork of Hyperscan, BSD-3 license) for
 * hardware-accelerated LIKE and regex matching.  Compiled pattern databases
 * are cached in a per-backend hash table for reuse across rows and queries.
 */
#include "postgres.h"

#ifdef PG_JITTER_HAVE_VECTORSCAN

#include "pg_jitter_vectorscan.h"
#include "pg_jitter_compat.h"

#include "fmgr.h"
#include "access/detoast.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

/* pg_crc32c.h included via pg_jitter_compat.h (with Windows CRC32C override) */
#include "hs.h"

/* ================================================================
 * Pattern cache
 * ================================================================ */
#define VS_CACHE_SIZE 64

typedef struct VsCacheKey {
	char    pattern[256]; /* truncated pattern for hash key */
	int     patlen;       /* full pattern length */
	uint32  flags;        /* HS_FLAG_* */
} VsCacheKey;

typedef struct VsCacheSlot {
	VsCacheKey     key;
	VsCacheEntry   entry;
	int            lru_counter;
} VsCacheSlot;

static VsCacheSlot vs_cache[VS_CACHE_SIZE];
static int vs_cache_used = 0;
static int vs_lru_clock = 0;

static uint32
vs_hash_key(const char *pattern, int patlen, uint32 flags)
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

	buf[pos++] = '^';

	for (int i = 0; i < like_len; i++)
	{
		char c = like_pat[i];

		if (c == '%')
		{
			/* SQL % → regex .* */
			buf[pos++] = '.';
			buf[pos++] = '*';
		}
		else if (c == '_')
		{
			/* SQL _ → regex . */
			buf[pos++] = '.';
		}
		else if (c == '\\' && i + 1 < like_len)
		{
			/* Escaped character: emit literal */
			i++;
			c = like_pat[i];
			/* Escape regex metacharacters */
			if (c == '.' || c == '+' || c == '*' || c == '?' ||
				c == '[' || c == ']' || c == '(' || c == ')' ||
				c == '{' || c == '}' || c == '^' || c == '$' ||
				c == '|' || c == '\\')
				buf[pos++] = '\\';
			buf[pos++] = c;
		}
		else
		{
			/* Escape regex metacharacters in literal text */
			if (c == '.' || c == '+' || c == '*' || c == '?' ||
				c == '[' || c == ']' || c == '(' || c == ')' ||
				c == '{' || c == '}' || c == '^' || c == '$' ||
				c == '|' || c == '\\')
				buf[pos++] = '\\';
			buf[pos++] = c;
		}
	}

	buf[pos++] = '$';
	buf[pos] = '\0';

	*regex_len_out = pos;
	return buf;
}

/* ================================================================
 * Vectorscan callback — halt on first match
 * ================================================================ */
static int
vs_match_callback(unsigned int id, unsigned long long from,
                  unsigned long long to, unsigned int flags, void *ctx)
{
	*(bool *)ctx = true;
	return 1; /* halt scanning */
}

/* ================================================================
 * Compile and cache a pattern
 * ================================================================ */
VsCacheEntry *
pg_jitter_vs_compile(const char *pattern, int patlen,
                      bool is_like, bool case_insensitive,
                      bool is_utf8)
{
	uint32 hs_flags = HS_FLAG_SINGLEMATCH | HS_FLAG_DOTALL;
	char *regex_str;
	int regex_len;
	uint32 hash;
	int slot_idx;
	hs_database_t *db = NULL;
	hs_compile_error_t *compile_err = NULL;

	if (case_insensitive)
		hs_flags |= HS_FLAG_CASELESS;
	if (is_utf8)
		hs_flags |= HS_FLAG_UTF8;

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
	hash = vs_hash_key(regex_str, regex_len, hs_flags);

	for (int i = 0; i < vs_cache_used; i++)
	{
		VsCacheSlot *s = &vs_cache[i];
		int key_len = s->key.patlen < 256 ? s->key.patlen : 256;
		int cmp_len = regex_len < 256 ? regex_len : 256;

		if (s->key.flags == hs_flags &&
			s->key.patlen == regex_len &&
			key_len == cmp_len &&
			memcmp(s->key.pattern, regex_str, cmp_len) == 0)
		{
			/* Cache hit */
			s->lru_counter = ++vs_lru_clock;
			pfree(regex_str);
			return &s->entry;
		}
	}

	/* Compile pattern */
	if (hs_compile(regex_str, hs_flags, HS_MODE_BLOCK,
	               NULL, &db, &compile_err) != HS_SUCCESS)
	{
		/* Compilation failed — pattern too complex or unsupported */
		if (compile_err)
			hs_free_compile_error(compile_err);
		pfree(regex_str);
		return NULL;
	}

	/* Find or evict a cache slot */
	if (vs_cache_used < VS_CACHE_SIZE)
	{
		slot_idx = vs_cache_used++;
	}
	else
	{
		/* LRU eviction */
		int min_lru = vs_cache[0].lru_counter;
		slot_idx = 0;
		for (int i = 1; i < VS_CACHE_SIZE; i++)
		{
			if (vs_cache[i].lru_counter < min_lru)
			{
				min_lru = vs_cache[i].lru_counter;
				slot_idx = i;
			}
		}
		/* Free old entry */
		if (vs_cache[slot_idx].entry.db)
			hs_free_database(vs_cache[slot_idx].entry.db);
		if (vs_cache[slot_idx].entry.scratch)
			hs_free_scratch(vs_cache[slot_idx].entry.scratch);
		if (vs_cache[slot_idx].entry.pattern)
			pfree(vs_cache[slot_idx].entry.pattern);
	}

	/* Allocate scratch space */
	hs_scratch_t *scratch = NULL;
	if (hs_alloc_scratch(db, &scratch) != HS_SUCCESS)
	{
		hs_free_database(db);
		pfree(regex_str);
		return NULL;
	}

	/* Fill cache slot */
	VsCacheSlot *slot = &vs_cache[slot_idx];
	int key_copy = regex_len < 256 ? regex_len : 256;
	memcpy(slot->key.pattern, regex_str, key_copy);
	if (key_copy < 256)
		slot->key.pattern[key_copy] = '\0';
	slot->key.patlen = regex_len;
	slot->key.flags = hs_flags;

	slot->entry.pattern = regex_str; /* transfer ownership */
	slot->entry.flags = hs_flags;
	slot->entry.db = db;
	slot->entry.scratch = scratch;
	slot->lru_counter = ++vs_lru_clock;

	return &slot->entry;
}

/* ================================================================
 * Match a text buffer against a compiled pattern
 * ================================================================ */
bool
pg_jitter_vs_match(VsCacheEntry *entry, const char *data, int len)
{
	bool matched = false;
	hs_error_t err;

	err = hs_scan(entry->db, data, len, 0, entry->scratch,
	              vs_match_callback, &matched);

	/*
	 * hs_scan returns HS_SCAN_TERMINATED when the callback halts scanning
	 * (i.e., a match was found and the callback returned non-zero).
	 * HS_SUCCESS means scanning completed without the callback halting.
	 * Both are valid outcomes — only treat other errors as failure.
	 */
	if (err != HS_SUCCESS && err != HS_SCAN_TERMINATED)
		return false;

	return matched;
}

/* ================================================================
 * JIT-callable wrapper: text Datum + VsCacheEntry* → int32
 * ================================================================ */
int32
pg_jitter_vs_match_text(int64 datum, int64 entry_ptr)
{
	VsCacheEntry *entry = (VsCacheEntry *)entry_ptr;
	Datum d = (Datum)datum;
	text *t = DatumGetTextPP(d);
	int len = VARSIZE_ANY_EXHDR(t);
	const char *data = VARDATA_ANY(t);
	bool result;

	result = pg_jitter_vs_match(entry, data, len);

	if ((Pointer)t != DatumGetPointer(d))
		pfree(t);

	return result ? 1 : 0;
}

#endif /* PG_JITTER_HAVE_VECTORSCAN */
