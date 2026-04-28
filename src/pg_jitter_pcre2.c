/*
 * pg_jitter_pcre2.c — PCRE2 pattern cache and match wrappers
 *
 * Compiled patterns are JIT-compiled via PCRE2's built-in SLJIT JIT and
 * cached in a per-backend LRU cache for reuse across rows and queries.
 */
#include "postgres.h"

#ifdef PG_JITTER_HAVE_PCRE2

#include "pg_jitter_pcre2.h"
#include "pg_jitter_common.h"
#include "pg_jitter_compat.h"

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "access/detoast.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

/* Pre-allocated stack size for JIT backtracking (matches PCRE2's MACHINE_STACK_SIZE) */
#define PJ_STACK_SIZE   32768

/* ================================================================
 * Pattern cache
 * ================================================================ */
#define PCRE2_CACHE_SIZE 64
#define PCRE2_MAX_PATTERN_BYTES 65536

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

bool
pg_jitter_pcre2_is_eligible(Oid collid, bool is_like, bool case_insensitive)
{
	int			encoding;

	if (!pg_jitter_collation_is_deterministic(collid))
		return false;

	encoding = GetDatabaseEncoding();

	/*
	 * UTF8 LIKE can be represented natively by PCRE2_UTF for deterministic
	 * collations because translated LIKE patterns only use literals, '.', and
	 * '.*'.  Case-insensitive matching needs PostgreSQL-compatible folding;
	 * PCRE2_UTF caseless matching folds non-ASCII characters even in C/POSIX
	 * collations, so leave those operations on PostgreSQL's matcher.  Regex
	 * character classes still need C/POSIX-compatible semantics.
	 */
	if (encoding == PG_UTF8)
	{
		if (case_insensitive)
			return false;
		if (is_like)
			return true;
		return pg_jitter_collation_is_c(collid);
	}

	/*
	 * Non-UTF multibyte encodings are not PCRE2 encodings.  In byte mode, PCRE2
	 * would let . and translated LIKE _ match one byte instead of one database
	 * character, so keep them on PostgreSQL's regex/LIKE engine.
	 */
	if (pg_database_encoding_max_length() != 1)
		return false;

	/*
	 * In fixed-width single-byte encodings, case-sensitive LIKE is bytewise and
	 * the translated _ wildcard still spans one character.  Regex and
	 * case-insensitive matching need locale-aware character classes/folding; the
	 * byte-mode PCRE2 default is only equivalent for C/POSIX.
	 */
	if (is_like && !case_insensitive)
		return true;

	return pg_jitter_collation_is_c(collid);
}

static uint32
pg_jitter_pcre2_compile_flags(bool case_insensitive)
{
	uint32		flags = PCRE2_DOTALL | PCRE2_DOLLAR_ENDONLY;

	if (GetDatabaseEncoding() == PG_UTF8)
		flags |= PCRE2_UTF;

	if (case_insensitive)
		flags |= PCRE2_CASELESS;

	return flags;
}

/* ================================================================
 * LIKE → regex conversion
 * ================================================================ */
static char *
like_to_regex(const char *like_pat, int like_len, int *regex_len_out)
{
	typedef enum LikeTokenKind
	{
		LIKE_TOKEN_LITERAL,
		LIKE_TOKEN_PERCENT,
		LIKE_TOKEN_UNDERSCORE
	} LikeTokenKind;
	typedef struct LikeToken
	{
		unsigned char ch;
		LikeTokenKind kind;
	} LikeToken;

	Size max_len;
	Size ntokens_alloc;
	char *buf;
	LikeToken *tokens;
	int ntokens = 0;
	int pos = 0;
	bool anchored_start = true;
	bool anchored_end = true;
	int start_token;
	int end_token;

	if (like_len < 0)
		return NULL;

	/* Worst case: each char becomes \x (2 bytes) + anchors ^...\z */
	if ((Size) like_len > (MaxAllocSize - 4) / 2)
		return NULL;
	max_len = (Size) like_len * 2 + 4;

	ntokens_alloc = like_len > 0 ? (Size) like_len : 1;
	if (ntokens_alloc > MaxAllocSize / sizeof(*tokens))
		return NULL;

	buf = MemoryContextAlloc(TopMemoryContext, max_len);
	tokens = MemoryContextAlloc(TopMemoryContext,
	                            ntokens_alloc * sizeof(*tokens));

	for (int i = 0; i < like_len; i++)
	{
		unsigned char c = (unsigned char) like_pat[i];

		if (c == '%')
		{
			tokens[ntokens].ch = c;
			tokens[ntokens].kind = LIKE_TOKEN_PERCENT;
		}
		else if (c == '_')
		{
			tokens[ntokens].ch = c;
			tokens[ntokens].kind = LIKE_TOKEN_UNDERSCORE;
		}
		else if (c == '\\')
		{
			if (i + 1 >= like_len)
			{
				pfree(tokens);
				pfree(buf);
				return NULL;
			}
			i++;
			tokens[ntokens].ch = (unsigned char) like_pat[i];
			tokens[ntokens].kind = LIKE_TOKEN_LITERAL;
		}
		else
		{
			tokens[ntokens].ch = c;
			tokens[ntokens].kind = LIKE_TOKEN_LITERAL;
		}
		ntokens++;
	}

	/*
	 * Optimization: strip only unescaped leading/trailing % tokens and omit
	 * ^/\z anchors accordingly.  Escape parsing must happen first so a literal
	 * trailing "\%" remains part of the regex.
	 *
	 * LIKE 'abc'     -> ^abc\z    (exact match)
	 * LIKE '%abc'    -> abc\z     (suffix match, no leading .*)
	 * LIKE 'abc%'    -> ^abc      (prefix match, no trailing .*)
	 * LIKE '%abc%'   -> abc       (interior, unanchored)
	 * LIKE '%a%b%'   -> a.*b      (no leading/trailing .*)
	 */
	start_token = 0;
	end_token = ntokens;
	while (start_token < end_token &&
	       tokens[start_token].kind == LIKE_TOKEN_PERCENT)
	{
		anchored_start = false;
		start_token++;
	}
	while (end_token > start_token &&
	       tokens[end_token - 1].kind == LIKE_TOKEN_PERCENT)
	{
		anchored_end = false;
		end_token--;
	}

	if (anchored_start)
		buf[pos++] = '^';

	for (int i = start_token; i < end_token; i++)
	{
		if (tokens[i].kind == LIKE_TOKEN_PERCENT)
		{
			buf[pos++] = '.';
			buf[pos++] = '*';
		}
		else if (tokens[i].kind == LIKE_TOKEN_UNDERSCORE)
		{
			buf[pos++] = '.';
		}
		else
		{
			unsigned char c = tokens[i].ch;

			if (c == '.' || c == '+' || c == '*' || c == '?' ||
				c == '[' || c == ']' || c == '(' || c == ')' ||
				c == '{' || c == '}' || c == '^' || c == '$' ||
				c == '|' || c == '\\')
				buf[pos++] = '\\';
			buf[pos++] = c;
		}
	}

	if (anchored_end)
	{
		buf[pos++] = '\\';
		buf[pos++] = 'z';
	}
	buf[pos] = '\0';

	pfree(tokens);
	*regex_len_out = pos;
	return buf;
}

/* ================================================================
 * PostgreSQL ARE regex -> PCRE2 conversion
 * ================================================================ */
static bool
pcre2_regex_append(char *buf, Size cap, Size *pos, const char *str)
{
	Size		len = strlen(str);

	if (*pos > cap || len > cap - *pos)
		return false;
	memcpy(buf + *pos, str, len);
	*pos += len;
	buf[*pos] = '\0';
	return true;
}

static bool
pcre2_regex_append_byte(char *buf, Size cap, Size *pos, unsigned char c)
{
	if (*pos >= cap)
		return false;
	buf[(*pos)++] = (char) c;
	buf[*pos] = '\0';
	return true;
}

static bool
raw_regex_dollar_is_constraint(const char *pattern, int patlen, int pos)
{
	int			next = pos + 1;

	return next >= patlen || pattern[next] == '|' || pattern[next] == ')';
}

static bool
raw_regex_class_start_is_unsupported(const char *pattern, int patlen, int pos)
{
	if (pos + 2 >= patlen || pattern[pos + 1] != '[')
		return false;

	if (pattern[pos + 2] == '.' || pattern[pos + 2] == '=')
		return true;

	if (pos + 4 < patlen && pattern[pos + 2] == ':' &&
		(pattern[pos + 3] == '<' || pattern[pos + 3] == '>') &&
		pattern[pos + 4] == ':')
		return true;

	return false;
}

static char *
raw_regex_to_pcre2(const char *pattern, int patlen, int *regex_len_out)
{
	char	   *buf;
	Size		cap;
	Size		pos = 0;
	bool		in_class = false;

	if (patlen < 0 || patlen > PCRE2_MAX_PATTERN_BYTES)
		return NULL;

	cap = (Size) patlen * 32 + 1;
	if (cap > PCRE2_MAX_PATTERN_BYTES)
		cap = PCRE2_MAX_PATTERN_BYTES;
	buf = MemoryContextAlloc(TopMemoryContext, cap + 1);
	buf[0] = '\0';

	for (int i = 0; i < patlen; i++)
	{
		unsigned char c = (unsigned char) pattern[i];

		if (in_class)
		{
			if (c == '\\')
			{
				if (i + 1 >= patlen ||
					!pcre2_regex_append_byte(buf, cap, &pos, c))
					goto fallback;
				i++;
				c = (unsigned char) pattern[i];
			}
			else if (c == '[' && raw_regex_class_start_is_unsupported(pattern,
																	   patlen,
																	   i))
				goto fallback;
			else if (c == ']')
				in_class = false;

			if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;
			continue;
		}

		if (c == '[')
		{
			if (raw_regex_class_start_is_unsupported(pattern, patlen, i))
				goto fallback;
			in_class = true;
			if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;
			continue;
		}

		if (c == '$')
		{
			if (!raw_regex_dollar_is_constraint(pattern, patlen, i))
				goto fallback;
			if (!pcre2_regex_append(buf, cap, &pos, "\\z"))
				goto fallback;
			continue;
		}

		if (c == '\\')
		{
			unsigned char e;

			if (i + 1 >= patlen)
				goto fallback;
			e = (unsigned char) pattern[++i];
			switch (e)
			{
				case 'A':
					if (!pcre2_regex_append(buf, cap, &pos, "\\A"))
						goto fallback;
					break;
				case 'Z':
					if (!pcre2_regex_append(buf, cap, &pos, "\\z"))
						goto fallback;
					break;
				case 'y':
					if (!pcre2_regex_append(buf, cap, &pos, "\\b"))
						goto fallback;
					break;
				case 'Y':
					if (!pcre2_regex_append(buf, cap, &pos, "\\B"))
						goto fallback;
					break;
				case 'm':
					if (!pcre2_regex_append(buf, cap, &pos,
											"(?<![[:alnum:]_])(?=[[:alnum:]_])"))
						goto fallback;
					break;
				case 'M':
					if (!pcre2_regex_append(buf, cap, &pos,
											"(?<=[[:alnum:]_])(?![[:alnum:]_])"))
						goto fallback;
					break;
				case 'd':
				case 'D':
				case 's':
				case 'S':
				case 'w':
				case 'W':
				case 'n':
				case 'r':
				case 't':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					if (e >= '1' && e <= '9' && i + 1 < patlen &&
						pattern[i + 1] >= '0' && pattern[i + 1] <= '9')
						goto fallback;
					if (!pcre2_regex_append_byte(buf, cap, &pos, '\\') ||
						!pcre2_regex_append_byte(buf, cap, &pos, e))
						goto fallback;
					break;
				default:
					if ((e >= 'A' && e <= 'Z') ||
						(e >= 'a' && e <= 'z') ||
						(e >= '0' && e <= '9'))
						goto fallback;
					if (!pcre2_regex_append_byte(buf, cap, &pos, '\\') ||
						!pcre2_regex_append_byte(buf, cap, &pos, e))
						goto fallback;
					break;
			}
			continue;
		}

		if (c == '(' && i + 1 < patlen &&
			(pattern[i + 1] == '?' || pattern[i + 1] == '*'))
			goto fallback;
		if ((c == '*' || c == '+' || c == '?' || c == '}') &&
			i + 1 < patlen && pattern[i + 1] == '+')
			goto fallback;
		if (!pcre2_regex_append_byte(buf, cap, &pos, c))
			goto fallback;
	}

	if (in_class || pos > PCRE2_MAX_PATTERN_BYTES)
		goto fallback;

	*regex_len_out = (int) pos;
	return buf;

fallback:
	pfree(buf);
	return NULL;
}

static bool
pcre2_match_result_is_match(int rc)
{
	if (rc >= 0)
		return true;
	if (rc == PCRE2_ERROR_NOMATCH)
		return false;

	elog(ERROR, "PCRE2 JIT match failed with error code %d", rc);
	return false;
}

/* ================================================================
 * Compile and cache a pattern
 * ================================================================ */
Pcre2CacheEntry *
pg_jitter_pcre2_compile(const char *pattern, int patlen,
                         bool is_like, bool case_insensitive, Oid collid)
{
	uint32 pcre2_flags;
	char *regex_str;
	int regex_len;
	int errcode;
	PCRE2_SIZE erroffset;
	pcre2_code *code = NULL;
	pcre2_match_data *match_data = NULL;

	if (!pg_jitter_pcre2_is_eligible(collid, is_like, case_insensitive))
		return NULL;
	if (patlen < 0 || patlen > PCRE2_MAX_PATTERN_BYTES)
		return NULL;

	pcre2_flags = pg_jitter_pcre2_compile_flags(case_insensitive);

	/* Convert LIKE to regex if needed */
	if (is_like)
	{
		regex_str = like_to_regex(pattern, patlen, &regex_len);
		if (regex_str == NULL)
			return NULL;
	}
	else
	{
		regex_str = raw_regex_to_pcre2(pattern, patlen, &regex_len);
		if (regex_str == NULL)
			return NULL;
	}

	if (regex_len > PCRE2_MAX_PATTERN_BYTES)
	{
		pfree(regex_str);
		return NULL;
	}

	for (int i = 0; i < pcre2_cache_used; i++)
	{
		Pcre2CacheSlot *s = &pcre2_cache[i];

		if (s->key.flags == pcre2_flags &&
			s->key.patlen == regex_len &&
			s->entry.pattern != NULL &&
			memcmp(s->entry.pattern, regex_str, regex_len) == 0)
		{
			/* Cache hit */
			s->lru_counter = ++pcre2_lru_clock;
			pfree(regex_str);
			return &s->entry;
		}
	}

	/*
	 * Generated code may embed Pcre2CacheEntry pointers, so entries in the fixed
	 * cache cannot be evicted safely.  Once the cache is full, leave this pattern
	 * on the regular PostgreSQL matcher instead of allocating backend-lifetime
	 * overflow entries.
	 */
	if (pcre2_cache_used >= PCRE2_CACHE_SIZE)
	{
		pfree(regex_str);
		return NULL;
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

	Pcre2CacheEntry *entry;
	Pcre2CacheSlot *slot = &pcre2_cache[pcre2_cache_used++];

	entry = &slot->entry;

	int key_copy = regex_len < 256 ? regex_len : 256;
	memcpy(slot->key.pattern, regex_str, key_copy);
	if (key_copy < 256)
		slot->key.pattern[key_copy] = '\0';
	slot->key.patlen = regex_len;
	slot->key.flags = pcre2_flags;

	entry->pattern = regex_str; /* transfer ownership */
	entry->flags = pcre2_flags;
	entry->code = code;
	entry->match_data = match_data;
	entry->match_context = NULL;
	entry->jit_stack = pcre2_jit_stack_create(PJ_STACK_SIZE,
	                                          PJ_STACK_SIZE * 4, NULL);
#ifdef PCRE2_JIT_FAST_API
	entry->jit_fast = NULL;
#endif

	if (entry->jit_stack)
	{
#ifdef PCRE2_JIT_FAST_API
		entry->jit_fast =
			pcre2_jit_fast_context_create(code, PCRE2_JIT_COMPLETE,
			                              match_data, entry->jit_stack);
		if (!entry->jit_fast)
		{
			entry->match_context = pcre2_match_context_create(NULL);
			if (entry->match_context)
				pcre2_jit_stack_assign(entry->match_context, NULL,
				                       entry->jit_stack);
			else
			{
				pcre2_jit_stack_free(entry->jit_stack);
				entry->jit_stack = NULL;
			}
		}
#else
		entry->match_context = pcre2_match_context_create(NULL);
		if (entry->match_context)
			pcre2_jit_stack_assign(entry->match_context, NULL,
			                       entry->jit_stack);
		else
		{
			pcre2_jit_stack_free(entry->jit_stack);
			entry->jit_stack = NULL;
		}
#endif
	}

	slot->lru_counter = ++pcre2_lru_clock;

	return entry;
}

/* ================================================================
 * Match a text buffer against a compiled pattern
 * ================================================================ */
bool
pg_jitter_pcre2_match(Pcre2CacheEntry *entry, const char *data, int len)
{
	int rc;

	if (entry == NULL || entry->code == NULL || len < 0 ||
	    (data == NULL && len > 0))
		return false;

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
	return pcre2_match_result_is_match(rc);
}

int32
pg_jitter_pcre2_match_raw(int64 entry_ptr, int64 data_ptr, int32 len)
{
	Pcre2CacheEntry *entry = (Pcre2CacheEntry *)entry_ptr;
	const uint8_t *data = (const uint8_t *)(uintptr_t)data_ptr;
	int rc;

	if (entry == NULL || entry->code == NULL || len < 0 ||
	    (data == NULL && len > 0))
		return 0;

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

	return pcre2_match_result_is_match(rc) ? 1 : 0;
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
