/*
 * pg_jitter_simdjson.h — simdjson SIMD-accelerated JSON parsing integration
 *
 * Provides fast validation (IS JSON), json_in, and jsonb_in replacements
 * using the simdjson library for inputs >= 64 bytes.  Shorter inputs fall
 * back to PG's native byte-by-byte parser.
 */
#ifndef PG_JITTER_SIMDJSON_H
#define PG_JITTER_SIMDJSON_H

#include "postgres.h"

#ifdef PG_JITTER_HAVE_SIMDJSON

#include "fmgr.h"

#define SIMDJSON_MIN_INPUT_LEN 64

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IS JSON validation: text datum + item_type (JS_TYPE_ANY/OBJECT/ARRAY/SCALAR)
 * Returns 1 (true) or 0 (false).
 *
 * Caller must check for NULL before calling — this function assumes the
 * datum is a valid non-NULL text value.
 */
#if PG_VERSION_NUM >= 160000
extern int32 pg_jitter_sj_is_json_datum(Datum datum, int32 item_type);
#endif

/*
 * text→json: validate cstring with simdjson, return text Datum.
 * On invalid JSON, ereport(ERROR) matching json_in() behavior.
 * For short inputs (< 64 bytes), delegates to the original json_in via fcinfo.
 */
extern Datum pg_jitter_sj_json_in(Datum cstring_datum,
                                   FunctionCallInfo fcinfo);

/*
 * text→jsonb: parse cstring with simdjson on-demand parser, build binary
 * jsonb via pushJsonbValue(), return Datum.
 * On invalid JSON, ereport(ERROR) matching jsonb_in() behavior.
 * For short inputs (< 64 bytes), delegates to the original jsonb_in via fcinfo.
 */
extern Datum pg_jitter_sj_jsonb_in(Datum cstring_datum,
                                    FunctionCallInfo fcinfo);

#ifdef __cplusplus
}
#endif

#endif /* PG_JITTER_HAVE_SIMDJSON */

#endif /* PG_JITTER_SIMDJSON_H */
