/*
 * pg_jitter_yyjson.c -- yyjson-backed JSON parsing
 */
#include "postgres.h"

#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#include <stdint.h>

#include "yyjson.h"
#include "pg_jitter_yyjson.h"

#ifdef PG_JITTER_HAVE_YYJSON

#define PG_JITTER_YYJSON_MAX_DEPTH 6400

typedef struct YjInput
{
	const char *data;
	size_t		len;
	char	   *converted;
} YjInput;

typedef struct YjConvertedStrings
{
	char	  **items;
	int			count;
	int			capacity;
} YjConvertedStrings;

static bool yj_encoding_checked = false;
static bool yj_is_utf8 = false;
static bool yj_is_sql_ascii = false;

#if PG_VERSION_NUM < 160000
static bool
yj_unicode_escape_error_is_validation_failure(int sqlerrcode)
{
	switch (sqlerrcode)
	{
		case ERRCODE_UNTRANSLATABLE_CHARACTER:
		case ERRCODE_CHARACTER_NOT_IN_REPERTOIRE:
		case ERRCODE_FEATURE_NOT_SUPPORTED:
		case ERRCODE_SYNTAX_ERROR:
			return true;
		default:
			return false;
	}
}
#endif

static void
yj_check_encoding(void)
{
	if (!yj_encoding_checked)
	{
		int			encoding = GetDatabaseEncoding();

		yj_is_utf8 = (encoding == PG_UTF8);
		yj_is_sql_ascii = (encoding == PG_SQL_ASCII);
		yj_encoding_checked = true;
	}
}

static bool
yj_validate_unicode_escape(void *ctx, uint32_t codepoint)
{
	unsigned char buf[MAX_UNICODE_EQUIVALENT_STRING + 1];

	(void) ctx;

#if PG_VERSION_NUM >= 160000
	return pg_unicode_to_server_noerror((pg_wchar) codepoint, buf);
#else
	MemoryContext oldcontext = CurrentMemoryContext;
	bool		ok = true;

	PG_TRY();
	{
		pg_unicode_to_server((pg_wchar) codepoint, buf);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		if (yj_unicode_escape_error_is_validation_failure(edata->sqlerrcode))
		{
			FreeErrorData(edata);
			ok = false;
		}
		else
			ReThrowError(edata);
	}
	PG_END_TRY();

	return ok;
#endif
}

static bool
yj_contains_unicode_escape(const char *data, size_t len)
{
	if (len < 2)
		return false;

	for (size_t i = 0; i + 1 < len; i++)
	{
		if (data[i] == '\\' && data[i + 1] == 'u')
			return true;
	}

	return false;
}

static void *
yj_pg_malloc(void *ctx, size_t size)
{
	MemoryContext mctx = ctx ? (MemoryContext) ctx : CurrentMemoryContext;
	Size		alloc_size;

	if (size > MaxAllocSize)
		return NULL;
	alloc_size = size == 0 ? 1 : (Size) size;
	return MemoryContextAllocExtended(mctx, alloc_size, MCXT_ALLOC_NO_OOM);
}

static void *
yj_pg_realloc(void *ctx, void *ptr, size_t old_size, size_t size)
{
	MemoryContext mctx = ctx ? (MemoryContext) ctx : CurrentMemoryContext;
	void	   *newptr;
	Size		copy_size;

	if (size > MaxAllocSize)
		return NULL;
	if (ptr == NULL)
		return yj_pg_malloc(ctx, size);
	if (size == 0)
	{
		pfree(ptr);
		return NULL;
	}

	newptr = MemoryContextAllocExtended(mctx, (Size) size, MCXT_ALLOC_NO_OOM);
	if (newptr == NULL)
		return NULL;

	copy_size = old_size < size ? (Size) old_size : (Size) size;
	if (copy_size > 0)
		memcpy(newptr, ptr, copy_size);
	pfree(ptr);
	return newptr;
}

static void
yj_pg_free(void *ctx, void *ptr)
{
	(void) ctx;

	if (ptr != NULL)
		pfree(ptr);
}

static const yyjson_alc yj_pg_allocator = {
	yj_pg_malloc,
	yj_pg_realloc,
	yj_pg_free,
	NULL
};

static void
yj_prepare_input(YjInput *input, const char *data, size_t len)
{
	input->data = data;
	input->len = len;
	input->converted = NULL;

	yj_check_encoding();

	if (!yj_is_utf8 && !yj_is_sql_ascii)
	{
		char	   *maybe_converted;

		if (len > INT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("JSON input is too large to convert")));

		maybe_converted = (char *) pg_server_to_any(data, (int) len, PG_UTF8);
		if (maybe_converted != data)
		{
			input->converted = maybe_converted;
			input->data = maybe_converted;
			input->len = strlen(maybe_converted);
		}
	}
}

static void
yj_release_input(YjInput *input)
{
	if (input->converted != NULL)
	{
		pfree(input->converted);
		input->converted = NULL;
	}
}

static void
yj_track_converted_string(YjConvertedStrings *strings, char *str)
{
	if (str == NULL)
		return;

	if (strings->count == strings->capacity)
	{
		Size		max_capacity = MaxAllocSize / sizeof(char *);
		Size		new_capacity;

		if (strings->capacity < 0 ||
			(Size) strings->capacity >= max_capacity ||
			(strings->capacity != 0 &&
			 (Size) strings->capacity > (Size) INT_MAX / 2))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("JSON input contains too many strings")));

		new_capacity = strings->capacity == 0 ? 16 :
			(Size) strings->capacity * 2;
		if (new_capacity > max_capacity || new_capacity > (Size) INT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("JSON input contains too many strings")));

		if (strings->items == NULL)
			strings->items = palloc(new_capacity * sizeof(char *));
		else
			strings->items = repalloc(strings->items,
									  new_capacity * sizeof(char *));
		strings->capacity = (int) new_capacity;
	}

	strings->items[strings->count++] = str;
}

static void
yj_release_converted_strings(YjConvertedStrings *strings)
{
	for (int i = 0; i < strings->count; i++)
		pfree(strings->items[i]);
	if (strings->items != NULL)
		pfree(strings->items);

	strings->items = NULL;
	strings->count = 0;
	strings->capacity = 0;
}

static yyjson_doc *
yj_parse_document(const char *data, size_t len, YjInput *input,
				  yyjson_read_err *err)
{
	yyjson_read_flag flags = YYJSON_READ_NUMBER_AS_RAW;

	yj_prepare_input(input, data, len);
	memset(err, 0, sizeof(*err));
	if (yj_is_sql_ascii)
		flags |= YYJSON_READ_ALLOW_INVALID_UNICODE;
	return yyjson_read_opts((char *) input->data, input->len,
							flags, &yj_pg_allocator, err);
}

static void
yj_invalid_input_error(const char *type_name, const yyjson_read_err *err)
{
	if (err != NULL && err->code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory while parsing JSON input for type %s",
						type_name)));

	if (err != NULL && err->msg != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", type_name),
				 errdetail("The input string is not valid JSON: %s at byte %zu.",
						   err->msg, err->pos)));

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s", type_name),
			 errdetail("The input string is not valid JSON.")));
}

static bool
yj_is_scalar(const yyjson_val *val)
{
	return val != NULL && !yyjson_is_obj(val) && !yyjson_is_arr(val);
}

static void
yj_check_container_depth(int depth)
{
	check_stack_depth();
	if (depth > PG_JITTER_YYJSON_MAX_DEPTH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "jsonb"),
				 errdetail("JSON nested too deep, maximum permitted depth is %d.",
						   PG_JITTER_YYJSON_MAX_DEPTH)));
}

static void
yj_check_string_len(size_t len)
{
	if (len > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("JSON string is too large")));
}

static void
yj_store_jsonb_string(JsonbValue *jbv, const char *str, size_t len,
					  YjConvertedStrings *converted_strings)
{
	yj_check_string_len(len);

	if (memchr(str, '\0', len) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
				 errmsg("unsupported Unicode escape sequence"),
				 errdetail("\\u0000 cannot be converted to text.")));

	jbv->type = jbvString;
	jbv->val.string.val = (char *) str;
	jbv->val.string.len = (int) len;

	if (!yj_is_utf8 && !yj_is_sql_ascii)
	{
		char	   *server_str;

		server_str = (char *) pg_any_to_server(jbv->val.string.val,
											   jbv->val.string.len,
											   PG_UTF8);
		if (server_str != jbv->val.string.val)
		{
			jbv->val.string.val = server_str;
			jbv->val.string.len = (int) strlen(server_str);
			yj_track_converted_string(converted_strings, server_str);
		}
	}
}

static void yj_walk_value(yyjson_val *val, JsonbParseState **pstate,
						  JsonbIteratorToken tok, int depth,
						  YjConvertedStrings *converted_strings);

static JsonbValue *
yj_walk_object(yyjson_val *obj, JsonbParseState **pstate, int depth,
			   YjConvertedStrings *converted_strings)
{
	yyjson_obj_iter iter;
	yyjson_val *key;
	JsonbValue	jbv;

	yj_check_container_depth(depth);
	pushJsonbValue(pstate, WJB_BEGIN_OBJECT, NULL);

	iter = yyjson_obj_iter_with(obj);
	while ((key = yyjson_obj_iter_next(&iter)) != NULL)
	{
		yyjson_val *val = yyjson_obj_iter_get_val(key);
		const char *key_str = yyjson_get_str(key);
		size_t		key_len = yyjson_get_len(key);

		yj_store_jsonb_string(&jbv, key_str, key_len, converted_strings);
		pushJsonbValue(pstate, WJB_KEY, &jbv);

		yj_walk_value(val, pstate, WJB_VALUE, depth + 1, converted_strings);
	}

	return pushJsonbValue(pstate, WJB_END_OBJECT, NULL);
}

static JsonbValue *
yj_walk_array(yyjson_val *arr, JsonbParseState **pstate, int depth,
			  YjConvertedStrings *converted_strings)
{
	size_t		idx;
	size_t		max;
	yyjson_val *elem;

	yj_check_container_depth(depth);
	pushJsonbValue(pstate, WJB_BEGIN_ARRAY, NULL);

	yyjson_arr_foreach(arr, idx, max, elem)
	{
		yj_walk_value(elem, pstate, WJB_ELEM, depth + 1, converted_strings);
	}

	return pushJsonbValue(pstate, WJB_END_ARRAY, NULL);
}

static void
yj_store_jsonb_number(yyjson_val *val, JsonbValue *jbv)
{
	const char *raw = yyjson_get_raw(val);
	size_t		raw_len = yyjson_get_len(val);
	char	   *num_str;
	Numeric		num = NULL;

	if (raw == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "jsonb"),
				 errdetail("JSON number was not available as an exact token.")));

	if (raw_len > MaxAllocSize - 1)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("JSON number is too large")));

	num_str = pnstrdup(raw, raw_len);
	PG_TRY();
	{
		num = DatumGetNumeric(
			DirectFunctionCall3(numeric_in,
								CStringGetDatum(num_str),
								ObjectIdGetDatum(InvalidOid),
								Int32GetDatum(-1)));
	}
	PG_CATCH();
	{
		pfree(num_str);
		PG_RE_THROW();
	}
	PG_END_TRY();
	pfree(num_str);

	jbv->type = jbvNumeric;
	jbv->val.numeric = num;
}

static void
yj_walk_value(yyjson_val *val, JsonbParseState **pstate,
			  JsonbIteratorToken tok, int depth,
			  YjConvertedStrings *converted_strings)
{
	JsonbValue	jbv;

	check_stack_depth();

	if (yyjson_is_obj(val))
	{
		yj_walk_object(val, pstate, depth, converted_strings);
		return;
	}
	if (yyjson_is_arr(val))
	{
		yj_walk_array(val, pstate, depth, converted_strings);
		return;
	}
	if (yyjson_is_str(val))
	{
		const char *str = yyjson_get_str(val);
		size_t		len = yyjson_get_len(val);

		yj_store_jsonb_string(&jbv, str, len, converted_strings);
		pushJsonbValue(pstate, tok, &jbv);
		return;
	}
	if (yyjson_is_raw(val))
	{
		yj_store_jsonb_number(val, &jbv);
		pushJsonbValue(pstate, tok, &jbv);
		return;
	}
	if (yyjson_is_bool(val))
	{
		jbv.type = jbvBool;
		jbv.val.boolean = yyjson_get_bool(val);
		pushJsonbValue(pstate, tok, &jbv);
		return;
	}
	if (yyjson_is_null(val))
	{
		jbv.type = jbvNull;
		pushJsonbValue(pstate, tok, &jbv);
		return;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s", "jsonb")));
}

static JsonbValue *
yj_walk_scalar(yyjson_val *val, JsonbParseState **pstate,
			   YjConvertedStrings *converted_strings)
{
	JsonbValue *result;

	check_stack_depth();
	pushJsonbValue(pstate, WJB_BEGIN_ARRAY, NULL);
	yj_walk_value(val, pstate, WJB_ELEM, 0, converted_strings);
	result = pushJsonbValue(pstate, WJB_END_ARRAY, NULL);
	result->val.array.rawScalar = true;
	return result;
}

#if PG_VERSION_NUM >= 160000
static int32
yj_is_json_item_type_from_yyjson(yyjson_val *root, int32 item_type)
{
	if (root == NULL)
		return 0;
	if (item_type == 0 /* JS_TYPE_ANY */)
		return 1;
	if (yyjson_is_obj(root))
		return item_type == 1 /* JS_TYPE_OBJECT */;
	if (yyjson_is_arr(root))
		return item_type == 2 /* JS_TYPE_ARRAY */;
	return item_type == 3 /* JS_TYPE_SCALAR */;
}

static int32
yj_is_json_item_type_from_pg_text(yyjson_pg_text_type type, int32 item_type)
{
	if (type == YYJSON_PG_TEXT_INVALID)
		return 0;
	if (item_type == 0 /* JS_TYPE_ANY */)
		return 1;

	switch (type)
	{
		case YYJSON_PG_TEXT_OBJECT:
			return item_type == 1 /* JS_TYPE_OBJECT */;
		case YYJSON_PG_TEXT_ARRAY:
			return item_type == 2 /* JS_TYPE_ARRAY */;
		case YYJSON_PG_TEXT_SCALAR:
			return item_type == 3 /* JS_TYPE_SCALAR */;
		default:
			return 0;
	}
}

static int32
yj_is_json_pg_text_syntax(const char *data, size_t len, int32 item_type,
						  int32 unique_keys)
{
	yyjson_read_err err;
	yyjson_pg_text_type type;

	memset(&err, 0, sizeof(err));
	/*
	 * Plain json text validation is syntax-only in PostgreSQL: it checks
	 * \uXXXX shape but does not reject decoded \u0000 or lone surrogates.
	 * Unique-key validation decodes strings, so it must also apply the server
	 * encoding validator just like PostgreSQL's need_escapes path.
	 */
	type = yyjson_read_pg_text_type_opts_err_ex(
		data, len, unique_keys != 0,
		unique_keys ? yj_validate_unicode_escape : NULL,
		NULL, &yj_pg_allocator, &err);
	if (type == YYJSON_PG_TEXT_INVALID &&
		err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory while validating JSON text")));

	return yj_is_json_item_type_from_pg_text(type, item_type);
}

int32
pg_jitter_yj_is_json_datum(Datum datum, int32 item_type, int32 unique_keys)
{
	text	   *json = DatumGetTextPP(datum);
	char	   *data = VARDATA_ANY(json);
	int			len = VARSIZE_ANY_EXHDR(json);
	int32		result = 0;
	yyjson_read_err err;
	YjInput		input;
	yyjson_doc *doc = NULL;

	input.data = NULL;
	input.len = 0;
	input.converted = NULL;

	PG_TRY();
	{
		/*
		 * The regular yyjson reader is the fast path, but it is stricter than
		 * PostgreSQL json text syntax for some Unicode escapes.  On parse
		 * rejection, retry with yyjson's PostgreSQL syntax classifier.  Unique
		 * key checks go straight to that classifier because they need decoded
		 * string comparison and Unicode validation.
		 */
		if (unique_keys || len < PG_JITTER_YYJSON_MIN_INPUT_LEN)
		{
			yj_prepare_input(&input, data, (size_t) len);
			result = yj_is_json_pg_text_syntax(input.data, input.len,
											  item_type, unique_keys);
		}
		else
		{
			doc = yj_parse_document(data, (size_t) len, &input, &err);
			if (doc != NULL)
			{
				result = yj_is_json_item_type_from_yyjson(yyjson_doc_get_root(doc),
														 item_type);
				yyjson_doc_free(doc);
				doc = NULL;
			}
			else
			{
				if (err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
					ereport(ERROR,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory while validating JSON text")));
				result = yj_is_json_pg_text_syntax(input.data, input.len,
												  item_type, unique_keys);
			}
		}
	}
	PG_CATCH();
	{
		if (doc != NULL)
			yyjson_doc_free(doc);
		yj_release_input(&input);
		if ((Pointer) json != DatumGetPointer(datum))
			pfree(json);
		PG_RE_THROW();
	}
	PG_END_TRY();

	yj_release_input(&input);
	if ((Pointer) json != DatumGetPointer(datum))
		pfree(json);

	return result;
}
#endif /* PG_VERSION_NUM >= 160000 */

Datum
pg_jitter_yj_jsonb_in(Datum cstring_datum, FunctionCallInfo fcinfo)
{
	char	   *str = DatumGetCString(cstring_datum);
	size_t		len = strlen(str);
	yyjson_read_err err;
	YjInput		input;
	yyjson_doc *doc;
	yyjson_val *root;
	JsonbParseState *pstate = NULL;
	JsonbValue *result = NULL;
	Jsonb	   *jsonb = NULL;
	YjConvertedStrings converted_strings = {0};

	if (len > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("JSON input is too large")));

	doc = NULL;
	input.data = NULL;
	input.len = 0;
	input.converted = NULL;

	PG_TRY();
	{
		yj_check_encoding();
		if (yj_is_sql_ascii && yj_contains_unicode_escape(str, len))
		{
			yyjson_read_err pg_err;
			yyjson_pg_text_type type;

			memset(&pg_err, 0, sizeof(pg_err));
			type = yyjson_read_pg_text_type_opts_err_ex(
				str, len, false, yj_validate_unicode_escape, NULL,
				&yj_pg_allocator, &pg_err);
			if (type == YYJSON_PG_TEXT_INVALID)
				yj_invalid_input_error("jsonb", &pg_err);
		}

		doc = yj_parse_document(str, len, &input, &err);
		if (doc == NULL)
			yj_invalid_input_error("jsonb", &err);

		root = yyjson_doc_get_root(doc);
		if (root == NULL)
			yj_invalid_input_error("jsonb", &err);

		if (yyjson_is_obj(root))
			result = yj_walk_object(root, &pstate, 1, &converted_strings);
		else if (yyjson_is_arr(root))
			result = yj_walk_array(root, &pstate, 1, &converted_strings);
		else if (yj_is_scalar(root))
			result = yj_walk_scalar(root, &pstate, &converted_strings);
		else
			yj_invalid_input_error("jsonb", &err);

		jsonb = JsonbValueToJsonb(result);
		yj_release_converted_strings(&converted_strings);
	}
	PG_CATCH();
	{
		yj_release_converted_strings(&converted_strings);
		if (doc != NULL)
			yyjson_doc_free(doc);
		yj_release_input(&input);
		PG_RE_THROW();
	}
	PG_END_TRY();

	yyjson_doc_free(doc);
	yj_release_input(&input);
	PG_RETURN_POINTER(jsonb);
}

#endif /* PG_JITTER_HAVE_YYJSON */
