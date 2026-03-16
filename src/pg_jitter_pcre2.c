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

/* ================================================================
 * Mirror of PCRE2 internal structures for direct JIT function calls.
 *
 * Copied from PCRE2 10.48-DEV:
 *   sljit_stack        — sljit_src/sljitLir.h
 *   jit_arguments      — pcre2_jit_compile.c:179
 *   executable_functions — pcre2_jit_compile.c:199
 *   pcre2_real_code    — pcre2_intmodedep.h:660
 *
 * These MUST match the PCRE2 version we build against.
 * ================================================================ */

/* sljit_stack: 4 pointers controlling the backtracking stack */
typedef struct pj_sljit_stack {
	uint8_t *min_start;
	uint8_t *start;
	uint8_t *end;
	uint8_t *top;
} pj_sljit_stack;

/*
 * jit_arguments: the struct passed to PCRE2's JIT-compiled function.
 * Fields before 'str' are constant per-pattern; str/begin/end change per row.
 */
typedef struct pj_jit_args {
	pj_sljit_stack *stack;           /* backtracking stack */
	const uint8_t  *str;             /* current position (= begin + start_offset) */
	const uint8_t  *begin;           /* subject start */
	const uint8_t  *end;             /* subject end */
	void           *match_data;      /* pcre2_match_data* for ovector writes */
	const uint8_t  *startchar_ptr;   /* output: start of matched text */
	uint8_t        *mark_ptr;        /* output: (*MARK) name */
	void           *callout;         /* callout function pointer */
	void           *callout_data;    /* callout user data */
	size_t          offset_limit;    /* use-offset-limit value */
	uint32_t        limit_match;     /* match limit */
	uint32_t        oveccount;       /* number of ovector slots (pairs × 2) */
	uint32_t        options;         /* match options */
} pj_jit_args;

/* JIT function signature: int func(jit_arguments *args) */
typedef int (*pj_jit_func_t)(pj_jit_args *);

/*
 * Offset of executable_jit in pcre2_real_code:
 *   pcre2_memctl memctl   = { malloc(8), free(8), data(8) } = 24 bytes
 *   const uint8_t *tables = 8 bytes
 *   void *executable_jit  → at offset 32
 */
#define PJ_EXECUTABLE_JIT_OFFSET  32

/* PCRE2 default match limit (from pcre2_internal.h) */
#define PJ_MATCH_LIMIT  10000000

/* Pre-allocated stack size for JIT backtracking (matches PCRE2's MACHINE_STACK_SIZE) */
#define PJ_STACK_SIZE   32768

/* Pre-allocated state for direct JIT calls (one per cache entry) */
typedef struct Pcre2DirectState {
	pj_jit_args       args;
	pcre2_jit_stack   *jit_stack;   /* PCRE2 API-allocated stack */
} Pcre2DirectState;

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

	buf[pos++] = '^';

	for (int i = 0; i < like_len; i++)
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
		else if (c == '\\' && i + 1 < like_len)
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

	/* Create reusable match_data (only need 1 ovector pair for yes/no) */
	match_data = pcre2_match_data_create(1, NULL);
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
		if (pcre2_cache[slot_idx].entry.jit_direct) {
			Pcre2DirectState *old_ds =
				(Pcre2DirectState *)pcre2_cache[slot_idx].entry.jit_direct;
			if (old_ds->jit_stack)
				pcre2_jit_stack_free(old_ds->jit_stack);
			pfree(old_ds);
		}
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
	slot->entry.jit_func = NULL;
	slot->entry.jit_direct = NULL;

	/*
	 * Extract JIT function pointer for direct calls.
	 *
	 * pcre2_real_code.executable_jit points to an executable_functions
	 * struct whose first field is executable_funcs[3] — an array of
	 * function pointers for full/partial-soft/partial-hard matching.
	 * Index 0 = PCRE2_JIT_COMPLETE (full matching), which is what we use.
	 */
	{
		void **ejit_ptr = (void **)((char *)code + PJ_EXECUTABLE_JIT_OFFSET);
		void *ejit = *ejit_ptr;

		if (ejit)
		{
			void **funcs = (void **)ejit;
			pj_jit_func_t jit_fn = (pj_jit_func_t)funcs[0];

			if (jit_fn)
			{
				Pcre2DirectState *ds = MemoryContextAllocZero(
					TopMemoryContext, sizeof(Pcre2DirectState));

				/*
				 * Use PCRE2 API to create a proper JIT stack.
				 * pcre2_jit_stack wraps an sljit_stack with correct
				 * alignment and memory management.
				 *
				 * pcre2_real_jit_stack layout:
				 *   pcre2_memctl memctl;    // 24 bytes
				 *   struct sljit_stack *stack; // at offset 24
				 *
				 * But we can just use the internal pointer directly.
				 */
				ds->jit_stack = pcre2_jit_stack_create(
					PJ_STACK_SIZE, PJ_STACK_SIZE * 4, NULL);

				/* Extract sljit_stack from pcre2_jit_stack */
				/* pcre2_real_jit_stack: memctl(24) then stack pointer */
				pj_sljit_stack *stack_ptr =
					*(pj_sljit_stack **)((char *)ds->jit_stack + 24);

				/* Pre-fill constant jit_arguments fields */
				ds->args.stack = stack_ptr;
				ds->args.match_data = match_data;
				ds->args.mark_ptr = NULL;
				ds->args.callout = NULL;
				ds->args.callout_data = NULL;
				ds->args.offset_limit = (size_t)-1; /* PCRE2_UNSET */
				ds->args.options = 0;

				/* Match limit: min(MATCH_LIMIT, pattern's limit) */
				{
					uint32_t pat_limit;
					ds->args.limit_match = PJ_MATCH_LIMIT;
					if (pcre2_pattern_info(code, PCRE2_INFO_MATCHLIMIT,
					                       &pat_limit) == 0 &&
					    pat_limit < (uint32_t)PJ_MATCH_LIMIT)
						ds->args.limit_match = pat_limit;
				}

				/*
				 * oveccount: must match match_data capacity AND the pattern's
				 * top_bracket.  PCRE2 JIT returns PCRE2_ERROR_JIT_STACKLIMIT
				 * or similar errors if oveccount is insufficient.
				 *
				 * executable_functions layout (pcre2_jit_compile.c:199):
				 *   void *executable_funcs[3]       = 24 bytes
				 *   void *read_only_data_heads[3]   = 24 bytes
				 *   sljit_uw executable_sizes[3]     = 24 bytes
				 *   sljit_u32 top_bracket            = at offset 72
				 *
				 * top_bracket = re->top_bracket + 1 (includes group 0).
				 * We need match_data with at least top_bracket pairs.
				 */
				{
					uint32_t top_bracket = *(uint32_t *)((char *)ejit + 72);

					/* Reallocate match_data if needed */
					if (top_bracket > 1) {
						pcre2_match_data_free(match_data);
						match_data = pcre2_match_data_create(top_bracket, NULL);
						slot->entry.match_data = match_data;
					}
					ds->args.match_data = match_data;
					ds->args.oveccount = top_bracket << 1;
				}

				slot->entry.jit_func = (void *)jit_fn;
				slot->entry.jit_direct = ds;
			}
		}
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
	                     NULL);    /* match_context */

	/*
	 * rc > 0: match found (number of ovector pairs filled)
	 * rc == 0: match found but ovector too small (still a match)
	 * rc == PCRE2_ERROR_NOMATCH (-1): no match
	 * rc < -1: some other error
	 */
	return (rc >= 0);
}

/* ================================================================
 * JIT-callable wrapper: text Datum + Pcre2CacheEntry* → int32
 *
 * When jit_func is available, calls the PCRE2 JIT function directly
 * with a pre-allocated jit_arguments struct and stack, bypassing
 * the pcre2_jit_match() wrapper overhead (~35ns/call savings).
 * Falls back to pcre2_jit_match() if direct state is unavailable.
 * ================================================================ */
int32
pg_jitter_pcre2_match_text(int64 datum, int64 entry_ptr)
{
	Pcre2CacheEntry *entry = (Pcre2CacheEntry *)entry_ptr;
	Datum d = (Datum)datum;
	text *t = DatumGetTextPP(d);
	int len = VARSIZE_ANY_EXHDR(t);
	const uint8_t *data = (const uint8_t *)VARDATA_ANY(t);
	int rc;

	if (entry->jit_func)
	{
		/*
		 * Direct JIT call — update only the per-row fields
		 * (str, begin, end, startchar_ptr) and invoke the
		 * JIT function with our pre-allocated arguments.
		 */
		Pcre2DirectState *ds = (Pcre2DirectState *)entry->jit_direct;
		pj_jit_func_t func = (pj_jit_func_t)entry->jit_func;

		ds->args.str = data;
		ds->args.begin = data;
		ds->args.end = data + len;
		ds->args.startchar_ptr = data;

		rc = func(&ds->args);
	}
	else
	{
		/* Fallback: standard pcre2_jit_match path */
		rc = pcre2_jit_match(entry->code,
		                     (PCRE2_SPTR)data, len,
		                     0, 0,
		                     entry->match_data,
		                     NULL);
	}

	if ((Pointer)t != DatumGetPointer(d))
		pfree(t);

	return (rc >= 0) ? 1 : 0;
}

#endif /* PG_JITTER_HAVE_PCRE2 */
