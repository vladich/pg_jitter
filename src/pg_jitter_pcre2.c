/*
 * pg_jitter_pcre2.c — PCRE2 pattern cache and match wrappers
 *
 * Compiled patterns are JIT-compiled via PCRE2's built-in SLJIT JIT and
 * cached in a backend-local LRU directory for reuse across rows and queries.
 * Generated code pins returned entries, so eviction removes only cache lookup
 * ownership; live generated-code pointers remain valid until context release.
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
#define PG_JITTER_PCRE2_MATCH_LIMIT 10000000U
#define PG_JITTER_PCRE2_DEPTH_LIMIT 10000000U
#define PG_JITTER_PCRE2_HEAP_LIMIT_KIB 65536U

/* ================================================================
 * Pattern cache
 * ================================================================ */
#define PCRE2_CACHE_SIZE 64
#define PCRE2_MAX_PATTERN_BYTES 65536

static Pcre2CacheEntry *pcre2_cache[PCRE2_CACHE_SIZE];
static uint64 pcre2_lru_clock = 0;

static void
pg_jitter_pcre2_free_entry(Pcre2CacheEntry *entry)
{
	if (entry == NULL)
		return;

	Assert(entry->refcount == 0);
	Assert(!entry->in_cache);

#ifdef PG_JITTER_USE_PCRE2_JIT_FAST_API
	if (entry->jit_fast)
		pcre2_jit_fast_context_free(entry->jit_fast);
#endif
	if (entry->match_context)
		pcre2_match_context_free(entry->match_context);
	if (entry->jit_stack)
		pcre2_jit_stack_free(entry->jit_stack);
	if (entry->match_data)
		pcre2_match_data_free(entry->match_data);
	if (entry->code)
		pcre2_code_free(entry->code);
	if (entry->pattern)
		pfree(entry->pattern);
	pfree(entry);
}

static void
pg_jitter_pcre2_unpin_entry(void *data)
{
	Pcre2CacheEntry *entry = (Pcre2CacheEntry *) data;

	if (entry == NULL)
		return;

	Assert(entry->refcount > 0);
	entry->refcount--;
	if (entry->refcount == 0 && !entry->in_cache)
		pg_jitter_pcre2_free_entry(entry);
}

static bool
pg_jitter_pcre2_pin_entry(PgJitterContext *ctx, Pcre2CacheEntry *entry)
{
	if (ctx == NULL || entry == NULL)
		return false;

	pg_jitter_register_compiled(ctx, pg_jitter_pcre2_unpin_entry, entry);
	entry->refcount++;
	return true;
}

static bool
pg_jitter_pcre2_publish_and_pin_entry(PgJitterContext *ctx, int slot,
									  Pcre2CacheEntry *entry)
{
	bool pinned = false;

	Assert(slot >= 0 && slot < PCRE2_CACHE_SIZE);
	Assert(entry != NULL);

	entry->in_cache = true;
	entry->lru_counter = ++pcre2_lru_clock;
	pcre2_cache[slot] = entry;

	PG_TRY();
	{
		pinned = pg_jitter_pcre2_pin_entry(ctx, entry);
	}
	PG_CATCH();
	{
		if (pcre2_cache[slot] == entry)
			pcre2_cache[slot] = NULL;
		entry->in_cache = false;
		pg_jitter_pcre2_free_entry(entry);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (pinned)
		return true;

	if (pcre2_cache[slot] == entry)
		pcre2_cache[slot] = NULL;
	entry->in_cache = false;
	pg_jitter_pcre2_free_entry(entry);
	return false;
}

static int
pg_jitter_pcre2_choose_slot(void)
{
	int unpinned_slot = -1;
	int pinned_slot = -1;
	uint64 unpinned_oldest = 0;
	uint64 pinned_oldest = 0;

	for (int i = 0; i < PCRE2_CACHE_SIZE; i++)
	{
		Pcre2CacheEntry *entry = pcre2_cache[i];

		if (entry == NULL)
			return i;

		if (entry->refcount == 0)
		{
			if (unpinned_slot < 0 || entry->lru_counter < unpinned_oldest)
			{
				unpinned_slot = i;
				unpinned_oldest = entry->lru_counter;
			}
		}
		else if (pinned_slot < 0 || entry->lru_counter < pinned_oldest)
		{
			pinned_slot = i;
			pinned_oldest = entry->lru_counter;
		}
	}

	return unpinned_slot >= 0 ? unpinned_slot : pinned_slot;
}

static void
pg_jitter_pcre2_evict_slot(int slot)
{
	Pcre2CacheEntry *entry;

	Assert(slot >= 0 && slot < PCRE2_CACHE_SIZE);

	entry = pcre2_cache[slot];
	if (entry == NULL)
		return;

	entry->in_cache = false;
	pcre2_cache[slot] = NULL;

	if (entry->refcount == 0)
		pg_jitter_pcre2_free_entry(entry);
}

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

	buf = MemoryContextAllocExtended(TopMemoryContext, max_len,
	                                 MCXT_ALLOC_NO_OOM);
	if (buf == NULL)
		return NULL;
	tokens = MemoryContextAllocExtended(TopMemoryContext,
	                                    ntokens_alloc * sizeof(*tokens),
	                                    MCXT_ALLOC_NO_OOM);
	if (tokens == NULL)
	{
		pfree(buf);
		return NULL;
	}

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
raw_regex_ascii_is_alnum(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ||
		(c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9');
}

static bool
raw_regex_ascii_is_digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

static bool
raw_regex_parse_bound_number(const char *pattern, int patlen, int *idx,
							 int *value)
{
	int			n = 0;

	if (*idx >= patlen ||
		!raw_regex_ascii_is_digit((unsigned char) pattern[*idx]))
		return false;

	do
	{
		n = n * 10 + (pattern[*idx] - '0');
		if (n > 255)
			return false;
		(*idx)++;
	} while (*idx < patlen &&
			 raw_regex_ascii_is_digit((unsigned char) pattern[*idx]));

	*value = n;
	return true;
}

static bool
raw_regex_bound_is_supported(const char *pattern, int patlen, int pos)
{
	int			idx = pos + 1;
	int			lower;
	int			upper;

	if (idx >= patlen ||
		!raw_regex_ascii_is_digit((unsigned char) pattern[idx]))
		return true;

	if (!raw_regex_parse_bound_number(pattern, patlen, &idx, &lower))
		return false;

	if (idx < patlen && pattern[idx] == ',')
	{
		idx++;
		if (idx < patlen &&
			raw_regex_ascii_is_digit((unsigned char) pattern[idx]))
		{
			if (!raw_regex_parse_bound_number(pattern, patlen, &idx, &upper))
				return false;
			if (lower > upper)
				return false;
		}
	}

	return idx < patlen && pattern[idx] == '}';
}

static bool
raw_regex_starts_quantifier(const char *pattern, int patlen, int pos)
{
	unsigned char c;

	if (pos >= patlen)
		return false;

	c = (unsigned char) pattern[pos];
	if (c == '*' || c == '+' || c == '?')
		return true;
	if (c == '{' && pos + 1 < patlen &&
		raw_regex_ascii_is_digit((unsigned char) pattern[pos + 1]))
		return true;
	return false;
}

static int
raw_regex_hex_value(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool
raw_regex_append_codepoint(char *buf, Size cap, Size *pos, uint32 codepoint)
{
	char		tmp[16];
	int			len;

	if (codepoint > 0x10FFFF)
		return false;

	len = snprintf(tmp, sizeof(tmp), "\\x{%X}", codepoint);
	if (len <= 0 || len >= (int) sizeof(tmp))
		return false;
	return pcre2_regex_append(buf, cap, pos, tmp);
}

static bool
raw_regex_parse_fixed_hex_escape(const char *pattern, int patlen, int *idx,
								 int digits, uint32 *codepoint)
{
	uint32		value = 0;

	if (*idx + digits >= patlen)
		return false;

	for (int j = 1; j <= digits; j++)
	{
		int			hex = raw_regex_hex_value((unsigned char) pattern[*idx + j]);

		if (hex < 0)
			return false;
		value = (value << 4) | (uint32) hex;
	}

	if (value > 0x10FFFF)
		return false;

	*idx += digits;
	*codepoint = value;
	return true;
}

static bool
raw_regex_parse_variable_hex_escape(const char *pattern, int patlen, int *idx,
									uint32 *codepoint)
{
	uint32		value = 0;
	int			digits = 0;
	int			pos = *idx + 1;

	while (pos < patlen && digits < 255)
	{
		int			hex = raw_regex_hex_value((unsigned char) pattern[pos]);

		if (hex < 0)
			break;
		if (value > (0x10FFFFU - (uint32) hex) / 16U)
			return false;
		value = value * 16U + (uint32) hex;
		pos++;
		digits++;
	}

	if (digits == 0 || value > 0x10FFFF)
		return false;

	*idx = pos - 1;
	*codepoint = value;
	return true;
}

static bool
raw_regex_parse_octal_escape(const char *pattern, int patlen, int *idx,
							 uint32 *codepoint)
{
	uint32		value = 0;
	int			digits = 0;
	int			pos = *idx;

	while (pos < patlen && digits < 3 &&
		   pattern[pos] >= '0' && pattern[pos] <= '7')
	{
		value = (value << 3) + (uint32) (pattern[pos] - '0');
		pos++;
		digits++;
	}

	if (digits == 0)
		return false;

	if (value > 0xFF && digits == 3)
	{
		value >>= 3;
		pos--;
		digits--;
	}

	if (value > 0xFF || digits == 0)
		return false;

	*idx = pos - 1;
	*codepoint = value;
	return true;
}

static bool
raw_regex_append_plain_escape(char *buf, Size cap, Size *pos,
							  const char *pattern, int patlen, int *idx,
							  unsigned char escape)
{
	uint32		codepoint;

	switch (escape)
	{
		case 'a':
			return raw_regex_append_codepoint(buf, cap, pos, '\007');
		case 'b':
			return raw_regex_append_codepoint(buf, cap, pos, '\b');
		case 'B':
			return raw_regex_append_codepoint(buf, cap, pos, '\\');
		case 'c':
			if (*idx + 1 >= patlen)
				return false;
			(*idx)++;
			return raw_regex_append_codepoint(buf, cap, pos,
											  (unsigned char) pattern[*idx] & 037);
		case 'e':
			return raw_regex_append_codepoint(buf, cap, pos, '\033');
		case 'f':
			return raw_regex_append_codepoint(buf, cap, pos, '\f');
		case 'n':
			return raw_regex_append_codepoint(buf, cap, pos, '\n');
		case 'r':
			return raw_regex_append_codepoint(buf, cap, pos, '\r');
		case 't':
			return raw_regex_append_codepoint(buf, cap, pos, '\t');
		case 'u':
			if (!raw_regex_parse_fixed_hex_escape(pattern, patlen, idx, 4,
												  &codepoint))
				return false;
			return raw_regex_append_codepoint(buf, cap, pos, codepoint);
		case 'U':
			if (!raw_regex_parse_fixed_hex_escape(pattern, patlen, idx, 8,
												  &codepoint))
				return false;
			return raw_regex_append_codepoint(buf, cap, pos, codepoint);
		case 'v':
			return raw_regex_append_codepoint(buf, cap, pos, '\v');
		case 'x':
			if (!raw_regex_parse_variable_hex_escape(pattern, patlen, idx,
													 &codepoint))
				return false;
			return raw_regex_append_codepoint(buf, cap, pos, codepoint);
		case '0':
			if (!raw_regex_parse_octal_escape(pattern, patlen, idx,
											  &codepoint))
				return false;
			return raw_regex_append_codepoint(buf, cap, pos, codepoint);
		default:
			return false;
	}
}

static bool
raw_regex_supported_group_intro(const char *pattern, int patlen, int pos,
								int *intro_len, bool *is_lookaround)
{
	if (pos + 2 >= patlen || pattern[pos] != '(' || pattern[pos + 1] != '?')
		return false;

	*is_lookaround = false;
	switch (pattern[pos + 2])
	{
		case ':':
			*intro_len = 3;
			return true;
		case '=':
		case '!':
			*intro_len = 3;
			*is_lookaround = true;
			return true;
		case '<':
			if (pos + 3 < patlen &&
				(pattern[pos + 3] == '=' || pattern[pos + 3] == '!'))
			{
				*intro_len = 4;
				*is_lookaround = true;
				return true;
			}
			return false;
		default:
			return false;
	}
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

static bool
raw_regex_word_constraint(const char *pattern, int patlen, int pos,
						  unsigned char *kind)
{
	if (pos + 6 >= patlen)
		return false;
	if (pattern[pos] != '[' || pattern[pos + 1] != '[' ||
		pattern[pos + 2] != ':' ||
		(pattern[pos + 3] != '<' && pattern[pos + 3] != '>') ||
		pattern[pos + 4] != ':' ||
		pattern[pos + 5] != ']' || pattern[pos + 6] != ']')
		return false;

	*kind = (unsigned char) pattern[pos + 3];
	return true;
}

static char *
raw_regex_to_pcre2(const char *pattern, int patlen, int *regex_len_out)
{
	char	   *buf;
	bool	   *group_lookaround;
	Size		cap;
	Size		pos = 0;
	bool		in_class = false;
	bool		class_first = false;
	bool		class_after_leading_caret = false;
	bool		last_atom_was_constraint = false;
	int			group_depth = 0;
	int			lookaround_depth = 0;

	if (patlen < 0 || patlen > PCRE2_MAX_PATTERN_BYTES)
		return NULL;

	cap = (Size) patlen * 32 + 1;
	if (cap > PCRE2_MAX_PATTERN_BYTES)
		cap = PCRE2_MAX_PATTERN_BYTES;
	buf = MemoryContextAllocExtended(TopMemoryContext, cap + 1,
	                                 MCXT_ALLOC_NO_OOM);
	if (buf == NULL)
		return NULL;
	buf[0] = '\0';
	group_lookaround = MemoryContextAllocExtended(TopMemoryContext,
												  (patlen > 0 ? (Size) patlen : 1) *
												  sizeof(*group_lookaround),
												  MCXT_ALLOC_NO_OOM);
	if (group_lookaround == NULL)
	{
		pfree(buf);
		return NULL;
	}

	for (int i = 0; i < patlen; i++)
	{
		unsigned char c = (unsigned char) pattern[i];

		if (in_class)
		{
			bool		escaped = false;

			if (c == '\\')
			{
				unsigned char e;

				if (i + 1 >= patlen)
					goto fallback;
				e = (unsigned char) pattern[++i];
				escaped = true;
				switch (e)
				{
					case 'd':
					case 'D':
					case 's':
					case 'S':
					case 'w':
					case 'W':
						if (!pcre2_regex_append_byte(buf, cap, &pos, '\\') ||
							!pcre2_regex_append_byte(buf, cap, &pos, e))
							goto fallback;
						break;
					case 'a':
					case 'b':
					case 'B':
					case 'c':
					case 'e':
					case 'f':
					case 'n':
					case 'r':
					case 't':
					case 'u':
					case 'U':
					case 'v':
					case 'x':
					case '0':
						if (!raw_regex_append_plain_escape(buf, cap, &pos,
														   pattern, patlen, &i,
														   e))
							goto fallback;
						break;
					default:
						if (raw_regex_ascii_is_alnum(e))
							goto fallback;
						if (!raw_regex_append_codepoint(buf, cap, &pos, e))
							goto fallback;
						break;
				}
				c = e;
			}
			else if (c == '[' && raw_regex_class_start_is_unsupported(pattern,
																	   patlen,
																	   i))
				goto fallback;
			else if (c == ']' && !class_first)
			{
				in_class = false;
				if (!pcre2_regex_append_byte(buf, cap, &pos, c))
					goto fallback;
			}
			else if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;

			if (!in_class)
			{
				class_first = false;
				class_after_leading_caret = false;
				last_atom_was_constraint = false;
			}
			else if (class_first)
			{
				if (!escaped && c == '^' && !class_after_leading_caret)
					class_after_leading_caret = true;
				else
					class_first = false;
			}
			continue;
		}

		if (last_atom_was_constraint &&
			raw_regex_starts_quantifier(pattern, patlen, i))
			/*
			 * PostgreSQL constraints are legal branch items, but cannot be
			 * followed by a quantifier.  PCRE2 accepts some quantified
			 * assertions, so reject before handing the pattern to PCRE2.
			 */
			goto fallback;

		if (c == '[')
		{
			unsigned char kind;

			if (raw_regex_word_constraint(pattern, patlen, i, &kind))
			{
				if (!pcre2_regex_append(buf, cap, &pos,
											kind == '<' ?
											"(?<![[:alnum:]_])(?=[[:alnum:]_])" :
											"(?<=[[:alnum:]_])(?![[:alnum:]_])"))
					goto fallback;
				last_atom_was_constraint = true;
				i += 6;
				continue;
			}
			if (raw_regex_class_start_is_unsupported(pattern, patlen, i))
				goto fallback;
			in_class = true;
			class_first = true;
			class_after_leading_caret = false;
			if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;
			last_atom_was_constraint = false;
			continue;
		}

		if (c == '$')
		{
			if (!pcre2_regex_append(buf, cap, &pos, "\\z"))
				goto fallback;
			last_atom_was_constraint = true;
			continue;
		}

		if (c == '^')
		{
			if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;
			last_atom_was_constraint = true;
			continue;
		}

		if (c == '{' && !raw_regex_bound_is_supported(pattern, patlen, i))
			goto fallback;

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
						last_atom_was_constraint = true;
						break;
					case 'Z':
						if (!pcre2_regex_append(buf, cap, &pos, "\\z"))
							goto fallback;
						last_atom_was_constraint = true;
						break;
					case 'y':
						if (!pcre2_regex_append(buf, cap, &pos, "\\b"))
							goto fallback;
						last_atom_was_constraint = true;
						break;
					case 'Y':
						if (!pcre2_regex_append(buf, cap, &pos, "\\B"))
							goto fallback;
						last_atom_was_constraint = true;
						break;
					case 'm':
						if (!pcre2_regex_append(buf, cap, &pos,
												"(?<![[:alnum:]_])(?=[[:alnum:]_])"))
							goto fallback;
						last_atom_was_constraint = true;
						break;
					case 'M':
						if (!pcre2_regex_append(buf, cap, &pos,
												"(?<=[[:alnum:]_])(?![[:alnum:]_])"))
							goto fallback;
						last_atom_was_constraint = true;
						break;
					case 'a':
					case 'b':
				case 'B':
				case 'c':
				case 'e':
				case 'f':
				case 'u':
				case 'U':
				case 'v':
				case 'x':
				case '0':
						if (!raw_regex_append_plain_escape(buf, cap, &pos,
														   pattern, patlen, &i,
														   e))
							goto fallback;
						last_atom_was_constraint = false;
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
					/*
					 * PostgreSQL's lookaround constraints do not allow back
					 * references inside the constraint expression.
					 */
					if (e >= '1' && e <= '9' && lookaround_depth > 0)
						goto fallback;
					if (e >= '1' && e <= '9' && i + 1 < patlen &&
						pattern[i + 1] >= '0' && pattern[i + 1] <= '9')
						goto fallback;
						if (!pcre2_regex_append_byte(buf, cap, &pos, '\\') ||
							!pcre2_regex_append_byte(buf, cap, &pos, e))
							goto fallback;
						last_atom_was_constraint = false;
						break;
					default:
						if (raw_regex_ascii_is_alnum(e))
							goto fallback;
						if (!pcre2_regex_append_byte(buf, cap, &pos, '\\') ||
							!pcre2_regex_append_byte(buf, cap, &pos, e))
							goto fallback;
						last_atom_was_constraint = false;
						break;
				}
				continue;
		}

		if (c == '(')
		{
			int			intro_len;
			bool		is_lookaround;

			if (group_depth >= patlen)
				goto fallback;
			if (i + 1 < patlen && pattern[i + 1] == '*')
				goto fallback;
			if (i + 1 < patlen && pattern[i + 1] == '?')
			{
				if (!raw_regex_supported_group_intro(pattern, patlen, i,
													 &intro_len,
													 &is_lookaround))
					goto fallback;
				for (int j = 0; j < intro_len; j++)
				{
					if (!pcre2_regex_append_byte(buf, cap, &pos,
												 (unsigned char) pattern[i + j]))
						goto fallback;
				}
					group_lookaround[group_depth++] = is_lookaround;
					if (is_lookaround)
						lookaround_depth++;
					i += intro_len - 1;
					last_atom_was_constraint = false;
					continue;
				}

			group_lookaround[group_depth++] = false;
			if (lookaround_depth > 0)
			{
				/*
				 * PostgreSQL treats all parentheses inside lookaround
				 * constraints as non-capturing, so keep external capture
				 * numbering aligned before handing the pattern to PCRE2.
				 */
				if (!pcre2_regex_append(buf, cap, &pos, "(?:"))
					goto fallback;
			}
				else if (!pcre2_regex_append_byte(buf, cap, &pos, c))
					goto fallback;
				last_atom_was_constraint = false;
				continue;
			}

		if (c == ')')
		{
			bool		closing_lookaround = false;

			if (group_depth > 0)
			{
				closing_lookaround = group_lookaround[--group_depth];
				if (closing_lookaround && lookaround_depth > 0)
					lookaround_depth--;
			}
			if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;
			last_atom_was_constraint = closing_lookaround;
			continue;
		}
			if ((c == '*' || c == '+' || c == '?' || c == '}') &&
				i + 1 < patlen && pattern[i + 1] == '+')
				goto fallback;
			if (!pcre2_regex_append_byte(buf, cap, &pos, c))
				goto fallback;
			last_atom_was_constraint = false;
		}

	if (in_class || pos > PCRE2_MAX_PATTERN_BYTES)
		goto fallback;

	*regex_len_out = (int) pos;
	pfree(group_lookaround);
	return buf;

fallback:
	pfree(group_lookaround);
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

	elog(ERROR, "PCRE2 match failed with error code %d", rc);
	return false;
}

static bool
pg_jitter_pcre2_should_retry_without_jit(int rc)
{
#ifdef PCRE2_ERROR_JIT_STACKLIMIT
	if (rc == PCRE2_ERROR_JIT_STACKLIMIT)
		return true;
#endif
#ifdef PCRE2_ERROR_MATCHLIMIT
	if (rc == PCRE2_ERROR_MATCHLIMIT)
		return true;
#endif
#ifdef PCRE2_ERROR_DEPTHLIMIT
	if (rc == PCRE2_ERROR_DEPTHLIMIT)
		return true;
#endif
#ifdef PCRE2_ERROR_HEAPLIMIT
	if (rc == PCRE2_ERROR_HEAPLIMIT)
		return true;
#endif

	return false;
}

static uint32
pg_jitter_pcre2_match_options(Pcre2CacheEntry *entry)
{
	uint32		options = 0;

	if ((entry->flags & PCRE2_UTF) != 0)
		options |= PCRE2_NO_UTF_CHECK;

	return options;
}

static pcre2_match_context *
pg_jitter_pcre2_create_match_context(pcre2_jit_stack *jit_stack)
{
	pcre2_match_context *match_context;

	match_context = pcre2_match_context_create(NULL);
	if (match_context == NULL)
		return NULL;

	if (pcre2_set_match_limit(match_context, PG_JITTER_PCRE2_MATCH_LIMIT) != 0 ||
		pcre2_set_depth_limit(match_context, PG_JITTER_PCRE2_DEPTH_LIMIT) != 0 ||
		pcre2_set_heap_limit(match_context, PG_JITTER_PCRE2_HEAP_LIMIT_KIB) != 0)
	{
		pcre2_match_context_free(match_context);
		return NULL;
	}

	if (jit_stack != NULL)
		pcre2_jit_stack_assign(match_context, NULL, jit_stack);

	return match_context;
}

static int
pg_jitter_pcre2_interpreter_match(Pcre2CacheEntry *entry,
								  const uint8_t *data, int len)
{
	uint32		options = pg_jitter_pcre2_match_options(entry);

#ifdef PCRE2_NO_JIT
	options |= PCRE2_NO_JIT;
#endif

	return pcre2_match(entry->code,
					   (PCRE2_SPTR) data, len,
					   0, options,
					   entry->match_data,
					   entry->match_context);
}

/* ================================================================
 * Compile and cache a pattern
 * ================================================================ */
Pcre2CacheEntry *
pg_jitter_pcre2_compile(PgJitterContext *ctx,
                         const char *pattern, int patlen,
                         bool is_like, bool case_insensitive, Oid collid)
{
	uint32 pcre2_flags;
	char *regex_str;
	int regex_len;
	int errcode;
	PCRE2_SIZE erroffset;
	Pcre2CacheEntry *entry;
	pcre2_code *code = NULL;
	pcre2_match_data *match_data = NULL;

	if (ctx == NULL)
		return NULL;
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

	for (int i = 0; i < PCRE2_CACHE_SIZE; i++)
	{
		Pcre2CacheEntry *entry = pcre2_cache[i];

		if (entry != NULL &&
			entry->flags == pcre2_flags &&
			entry->pattern_len == regex_len &&
			entry->pattern != NULL &&
			memcmp(entry->pattern, regex_str, regex_len) == 0)
		{
			entry->lru_counter = ++pcre2_lru_clock;
			pfree(regex_str);
			if (!pg_jitter_pcre2_pin_entry(ctx, entry))
				return NULL;
			return entry;
		}
	}

	entry = MemoryContextAllocExtended(TopMemoryContext, sizeof(Pcre2CacheEntry),
	                                   MCXT_ALLOC_ZERO | MCXT_ALLOC_NO_OOM);
	if (entry == NULL)
	{
		pfree(regex_str);
		return NULL;
	}

	/* Compile pattern */
	code = pcre2_compile((PCRE2_SPTR)regex_str, regex_len,
	                     pcre2_flags, &errcode, &erroffset, NULL);
	if (!code)
	{
		pfree(entry);
		pfree(regex_str);
		return NULL;
	}

	/* JIT compile — if it fails, fall back to V1 */
	if (pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) != 0)
	{
		pcre2_code_free(code);
		pfree(entry);
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
		pfree(entry);
		pfree(regex_str);
		return NULL;
	}

	int slot = pg_jitter_pcre2_choose_slot();

	if (slot < 0)
	{
		pcre2_match_data_free(match_data);
		pcre2_code_free(code);
		pfree(entry);
		pfree(regex_str);
		return NULL;
	}

	pg_jitter_pcre2_evict_slot(slot);

	entry->pattern = regex_str; /* transfer ownership */
	entry->pattern_len = regex_len;
	entry->flags = pcre2_flags;
	entry->code = code;
	entry->match_data = match_data;
	entry->match_context = NULL;
	entry->jit_stack = pcre2_jit_stack_create(PJ_STACK_SIZE,
	                                          PJ_STACK_SIZE * 4, NULL);
#ifdef PG_JITTER_USE_PCRE2_JIT_FAST_API
	entry->jit_fast = NULL;
#endif

	entry->match_context =
		pg_jitter_pcre2_create_match_context(entry->jit_stack);
	if (entry->match_context == NULL)
	{
		if (entry->jit_stack)
		{
			pcre2_jit_stack_free(entry->jit_stack);
			entry->jit_stack = NULL;
		}
		pcre2_match_data_free(match_data);
		pcre2_code_free(code);
		pfree(entry);
		pfree(regex_str);
		return NULL;
	}

#ifdef PG_JITTER_USE_PCRE2_JIT_FAST_API
	if (entry->jit_stack)
		entry->jit_fast =
			pcre2_jit_fast_context_create(code, PCRE2_JIT_COMPLETE,
		                                  match_data, entry->jit_stack);
#endif

	if (!pg_jitter_pcre2_publish_and_pin_entry(ctx, slot, entry))
		return NULL;

	return entry;
}

/* ================================================================
 * Match a text buffer against a compiled pattern
 * ================================================================ */
bool
pg_jitter_pcre2_match(Pcre2CacheEntry *entry, const char *data, int len)
{
	int rc;
	uint32		match_options;

	if (entry == NULL || entry->code == NULL || entry->match_data == NULL ||
	    len < 0 ||
	    (data == NULL && len > 0))
		return false;

	match_options = pg_jitter_pcre2_match_options(entry);
	rc = pcre2_jit_match(entry->code,
	                     (PCRE2_SPTR)data, len,
	                     0,        /* start_offset */
	                     match_options,
	                     entry->match_data,
	                     entry->match_context);

	if (pg_jitter_pcre2_should_retry_without_jit(rc))
		rc = pg_jitter_pcre2_interpreter_match(entry,
											   (const uint8_t *) data, len);

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
	uint32		match_options;

	if (entry == NULL || entry->code == NULL || entry->match_data == NULL ||
	    len < 0 ||
	    (data == NULL && len > 0))
		return 0;

	match_options = pg_jitter_pcre2_match_options(entry);
#ifdef PG_JITTER_USE_PCRE2_JIT_FAST_API
	if (entry->jit_fast)
		rc = pcre2_jit_fast_match(entry->jit_fast, (PCRE2_SPTR)data,
		                          len, 0, match_options);
	else
#endif
		rc = pcre2_jit_match(entry->code,
		                     (PCRE2_SPTR)data, len,
		                     0, match_options,
		                     entry->match_data,
		                     entry->match_context);

	if (pg_jitter_pcre2_should_retry_without_jit(rc))
		rc = pg_jitter_pcre2_interpreter_match(entry, data, len);

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
	int32 result;

	PG_TRY();
	{
		result = pg_jitter_pcre2_match_raw(entry_ptr, (int64)(uintptr_t)data,
										   len);
	}
	PG_CATCH();
	{
		if ((Pointer)t != DatumGetPointer(d))
			pfree(t);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if ((Pointer)t != DatumGetPointer(d))
		pfree(t);

	return result;
}

#endif /* PG_JITTER_HAVE_PCRE2 */
