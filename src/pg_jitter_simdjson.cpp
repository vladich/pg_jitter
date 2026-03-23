/*
 * pg_jitter_simdjson.cpp — simdjson SIMD-accelerated JSON parsing
 *
 * Uses the simdjson library (https://simdjson.org) for:
 * 1. IS JSON validation (DOM parser)
 * 2. text→json validation (DOM parser)
 * 3. text→jsonb conversion (on-demand parser + pushJsonbValue)
 *
 * All exported functions are extern "C" for use from JIT-compiled code.
 */

/* simdjson must be included before PG headers to avoid macro conflicts */
#include "simdjson.h"

extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "common/jsonapi.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/jsonb.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif
}

#include "pg_jitter_simdjson.h"

#ifdef PG_JITTER_HAVE_SIMDJSON

using namespace simdjson;

/* ================================================================
 * Process-local state
 * ================================================================ */

/* DOM parser — used for validation-only paths (IS_JSON, json_in) */
static dom::parser *sj_dom = nullptr;

/* On-demand parser — used for jsonb conversion (walks tree lazily) */
static ondemand::parser *sj_ondemand = nullptr;

/* Reusable padded buffer — grown on demand, never shrunk */
static char *sj_padded_buf = nullptr;
static size_t sj_padded_buf_size = 0;

/* Cached database encoding check */
static bool sj_encoding_checked = false;
static bool sj_is_utf8 = false;

/* ================================================================
 * Helpers
 * ================================================================ */

static void
sj_check_encoding(void)
{
	if (!sj_encoding_checked)
	{
		sj_is_utf8 = (GetDatabaseEncoding() == PG_UTF8);
		sj_encoding_checked = true;
	}
}

static void
sj_ensure_dom(void)
{
	if (sj_dom == nullptr)
	{
		/* Allocate parsers using C++ new — they live for the backend lifetime */
		sj_dom = new dom::parser();
	}
}

static void
sj_ensure_ondemand(void)
{
	if (sj_ondemand == nullptr)
	{
		sj_ondemand = new ondemand::parser();
	}
}

/*
 * Ensure the padded buffer is large enough for `len` bytes + SIMDJSON_PADDING.
 * Copies data into the buffer and zeroes the padding.
 *
 * For non-UTF8 databases, converts to UTF-8 first.
 * Returns a padded_string_view over the buffer.
 */
static padded_string_view
sj_ensure_padded(const char *data, size_t len)
{
	const char *src = data;
	size_t src_len = len;

	sj_check_encoding();

	/* Convert to UTF-8 if needed */
	if (!sj_is_utf8)
	{
		int converted_len;
		char *converted = (char *) pg_server_to_any(data, (int) len, PG_UTF8);
		if (converted != data)
		{
			src = converted;
			src_len = strlen(converted);
		}
	}

	size_t needed = src_len + SIMDJSON_PADDING;
	if (sj_padded_buf == nullptr || needed > sj_padded_buf_size)
	{
		/* Grow with doubling, minimum 4KB */
		size_t new_size = sj_padded_buf_size ? sj_padded_buf_size : 4096;
		while (new_size < needed)
			new_size *= 2;

		if (sj_padded_buf)
			pfree(sj_padded_buf);
		sj_padded_buf = (char *) MemoryContextAlloc(TopMemoryContext, new_size);
		sj_padded_buf_size = new_size;
	}

	memcpy(sj_padded_buf, src, src_len);
	memset(sj_padded_buf + src_len, 0, SIMDJSON_PADDING);

	return padded_string_view(sj_padded_buf, src_len,
							  src_len + SIMDJSON_PADDING);
}

/* ================================================================
 * jsonb tree walker (on-demand parser → pushJsonbValue)
 * ================================================================ */

static void sj_walk_value(ondemand::value val, JsonbParseState **pstate,
						  JsonbIteratorToken tok);

static JsonbValue *
sj_walk_object(ondemand::object obj, JsonbParseState **pstate)
{
	JsonbValue	jbv;

	pushJsonbValue(pstate, WJB_BEGIN_OBJECT, NULL);

	for (auto field : obj)
	{
		/* Key */
		std::string_view key = field.unescaped_key();

		jbv.type = jbvString;
		jbv.val.string.val = (char *) key.data();
		jbv.val.string.len = (int) key.length();

		/* For non-UTF8 databases, convert key back to server encoding */
		if (!sj_is_utf8)
		{
			char *converted = (char *) pg_any_to_server(
				jbv.val.string.val, jbv.val.string.len, PG_UTF8);
			if (converted != jbv.val.string.val)
			{
				jbv.val.string.val = converted;
				jbv.val.string.len = (int) strlen(converted);
			}
		}

		pushJsonbValue(pstate, WJB_KEY, &jbv);

		/* Value — use WJB_VALUE for object context */
		sj_walk_value(field.value(), pstate, WJB_VALUE);
	}

	return pushJsonbValue(pstate, WJB_END_OBJECT, NULL);
}

static JsonbValue *
sj_walk_array(ondemand::array arr, JsonbParseState **pstate)
{
	pushJsonbValue(pstate, WJB_BEGIN_ARRAY, NULL);

	for (auto elem : arr)
	{
		sj_walk_value(elem.value(), pstate, WJB_ELEM);
	}

	return pushJsonbValue(pstate, WJB_END_ARRAY, NULL);
}

static void
sj_walk_value(ondemand::value val, JsonbParseState **pstate,
			  JsonbIteratorToken tok)
{
	JsonbValue	jbv;

	switch (val.type())
	{
		case ondemand::json_type::object:
			sj_walk_object(val.get_object(), pstate);
			return;

		case ondemand::json_type::array:
			sj_walk_array(val.get_array(), pstate);
			return;

		case ondemand::json_type::string:
		{
			std::string_view sv = val.get_string();

			jbv.type = jbvString;
			jbv.val.string.val = (char *) sv.data();
			jbv.val.string.len = (int) sv.length();

			if (!sj_is_utf8)
			{
				char *converted = (char *) pg_any_to_server(
					jbv.val.string.val, jbv.val.string.len, PG_UTF8);
				if (converted != jbv.val.string.val)
				{
					jbv.val.string.val = converted;
					jbv.val.string.len = (int) strlen(converted);
				}
			}

			pushJsonbValue(pstate, tok, &jbv);
			return;
		}

		case ondemand::json_type::number:
		{
			/*
			 * Use raw_json_token() + PG's numeric_in() to preserve exact
			 * decimal representation — never go through double.
			 */
			std::string_view raw = val.raw_json_token();
			char *num_str = pnstrdup(raw.data(), raw.length());

			jbv.type = jbvNumeric;
			jbv.val.numeric = DatumGetNumeric(
				DirectFunctionCall3(numeric_in,
								   CStringGetDatum(num_str),
								   ObjectIdGetDatum(InvalidOid),
								   Int32GetDatum(-1)));

			pushJsonbValue(pstate, tok, &jbv);
			pfree(num_str);
			return;
		}

		case ondemand::json_type::boolean:
		{
			jbv.type = jbvBool;
			jbv.val.boolean = val.get_bool();
			pushJsonbValue(pstate, tok, &jbv);
			return;
		}

		case ondemand::json_type::null:
		{
			jbv.type = jbvNull;
			pushJsonbValue(pstate, tok, &jbv);
			return;
		}
	}
}

/* ================================================================
 * Exported functions (extern "C")
 * ================================================================ */

extern "C" {

#if PG_VERSION_NUM >= 160000
/*
 * IS JSON validation for text Datum.
 * Returns 1 (valid) or 0 (invalid).
 */
int32
pg_jitter_sj_is_json_datum(Datum datum, int32 item_type)
{
	text	   *json = DatumGetTextPP(datum);
	char	   *data = VARDATA_ANY(json);
	int			len = VARSIZE_ANY_EXHDR(json);

	/* Short inputs: fall back to PG's token-based check */
	if (len < SIMDJSON_MIN_INPUT_LEN)
	{
		/*
		 * For short strings we still need to validate and check type.
		 * Use the same logic as ExecEvalJsonIsPredicate for TEXTOID.
		 */
		bool		valid;

		valid = json_validate(json, false, false);
		if (!valid)
			return 0;

		if (item_type == 0 /* JS_TYPE_ANY */)
			return 1;

		/* Determine what kind of JSON value it is */
		JsonTokenType tok = json_get_first_token(json, false);
		switch (tok)
		{
			case JSON_TOKEN_OBJECT_START:
				return (item_type == 1 /* JS_TYPE_OBJECT */) ? 1 : 0;
			case JSON_TOKEN_ARRAY_START:
				return (item_type == 2 /* JS_TYPE_ARRAY */) ? 1 : 0;
			case JSON_TOKEN_STRING:
			case JSON_TOKEN_NUMBER:
			case JSON_TOKEN_TRUE:
			case JSON_TOKEN_FALSE:
			case JSON_TOKEN_NULL:
				return (item_type == 3 /* JS_TYPE_SCALAR */) ? 1 : 0;
			default:
				return 0;
		}
	}

	/* Use simdjson for validation */
	sj_ensure_dom();

	padded_string_view padded = sj_ensure_padded(data, (size_t) len);
	dom::element doc;
	auto error = sj_dom->parse(padded).get(doc);

	if (error)
		return 0;  /* invalid JSON */

	/* Check item_type if not JS_TYPE_ANY */
	if (item_type != 0 /* JS_TYPE_ANY */)
	{
		switch (doc.type())
		{
			case dom::element_type::OBJECT:
				return (item_type == 1 /* JS_TYPE_OBJECT */) ? 1 : 0;
			case dom::element_type::ARRAY:
				return (item_type == 2 /* JS_TYPE_ARRAY */) ? 1 : 0;
			default:
				/* scalars: string, number, bool, null */
				return (item_type == 3 /* JS_TYPE_SCALAR */) ? 1 : 0;
		}
	}

	return 1;  /* valid JSON of any type */
}
#endif /* PG_VERSION_NUM >= 160000 */

/*
 * text→json: validate cstring with simdjson, return text Datum.
 */
Datum
pg_jitter_sj_json_in(Datum cstring_datum, FunctionCallInfo fcinfo)
{
	char	   *str = DatumGetCString(cstring_datum);
	size_t		len = strlen(str);

	/* Short inputs: fall back to PG's json_in */
	if (len < SIMDJSON_MIN_INPUT_LEN)
	{
		fcinfo->args[0].value = cstring_datum;
		fcinfo->args[0].isnull = false;
		fcinfo->isnull = false;
		return fcinfo->flinfo->fn_addr(fcinfo);
	}

	/* Validate with simdjson */
	sj_ensure_dom();

	padded_string_view padded = sj_ensure_padded(str, len);
	auto error = sj_dom->parse(padded).error();

	if (error)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail("The input string is not valid JSON.")));
	}

	/* json_in just validates and returns cstring_to_text */
	PG_RETURN_TEXT_P(cstring_to_text_with_len(str, (int) len));
}

/*
 * text→jsonb: parse cstring with simdjson, build binary jsonb via
 * pushJsonbValue(), return Datum.
 */
Datum
pg_jitter_sj_jsonb_in(Datum cstring_datum, FunctionCallInfo fcinfo)
{
	char	   *str = DatumGetCString(cstring_datum);
	size_t		len = strlen(str);

	/* Short inputs: fall back to PG's jsonb_in */
	if (len < SIMDJSON_MIN_INPUT_LEN)
	{
		fcinfo->args[0].value = cstring_datum;
		fcinfo->args[0].isnull = false;
		fcinfo->isnull = false;
		return fcinfo->flinfo->fn_addr(fcinfo);
	}

	sj_check_encoding();
	sj_ensure_ondemand();

	padded_string_view padded = sj_ensure_padded(str, len);
	ondemand::document doc;
	auto error = sj_ondemand->iterate(padded).get(doc);

	if (error)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "jsonb"),
				 errdetail("The input string is not valid JSON.")));
	}

	/* Walk the document tree and build jsonb via pushJsonbValue */
	JsonbParseState *pstate = NULL;
	JsonbValue	jbv;
	JsonbValue *result;

	switch (doc.type())
	{
		case ondemand::json_type::object:
			result = sj_walk_object(doc.get_object(), &pstate);
			goto done;

		case ondemand::json_type::array:
			result = sj_walk_array(doc.get_array(), &pstate);
			goto done;

		case ondemand::json_type::string:
		{
			std::string_view sv = doc.get_string();

			/* Scalar JSON: wrap in array for jsonb representation */
			pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, NULL);
			jbv.type = jbvString;
			jbv.val.string.val = (char *) sv.data();
			jbv.val.string.len = (int) sv.length();

			if (!sj_is_utf8)
			{
				char *converted = (char *) pg_any_to_server(
					jbv.val.string.val, jbv.val.string.len, PG_UTF8);
				if (converted != jbv.val.string.val)
				{
					jbv.val.string.val = converted;
					jbv.val.string.len = (int) strlen(converted);
				}
			}

			pushJsonbValue(&pstate, WJB_ELEM, &jbv);
			result = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);
			result->val.array.rawScalar = true;
			goto done;
		}

		case ondemand::json_type::number:
		{
			std::string_view raw = doc.raw_json_token();
			char *num_str = pnstrdup(raw.data(), raw.length());

			pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, NULL);
			jbv.type = jbvNumeric;
			jbv.val.numeric = DatumGetNumeric(
				DirectFunctionCall3(numeric_in,
								   CStringGetDatum(num_str),
								   ObjectIdGetDatum(InvalidOid),
								   Int32GetDatum(-1)));
			pushJsonbValue(&pstate, WJB_ELEM, &jbv);
			result = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);
			result->val.array.rawScalar = true;
			pfree(num_str);
			goto done;
		}

		case ondemand::json_type::boolean:
		{
			pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, NULL);
			jbv.type = jbvBool;
			jbv.val.boolean = doc.get_bool();
			pushJsonbValue(&pstate, WJB_ELEM, &jbv);
			result = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);
			result->val.array.rawScalar = true;
			goto done;
		}

		case ondemand::json_type::null:
		{
			pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, NULL);
			jbv.type = jbvNull;
			pushJsonbValue(&pstate, WJB_ELEM, &jbv);
			result = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);
			result->val.array.rawScalar = true;
			goto done;
		}
	}

done:
	if (result == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "jsonb")));
	}

	PG_RETURN_POINTER(JsonbValueToJsonb(result));
}

} /* extern "C" */

#endif /* PG_JITTER_HAVE_SIMDJSON */
