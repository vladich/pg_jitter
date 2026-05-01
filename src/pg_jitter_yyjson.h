/*
 * pg_jitter_yyjson.h -- yyjson-backed JSON parsing integration
 */
#ifndef PG_JITTER_YYJSON_H
#define PG_JITTER_YYJSON_H

#include "postgres.h"

#ifdef PG_JITTER_HAVE_YYJSON

#include "fmgr.h"

#define PG_JITTER_YYJSON_MIN_INPUT_LEN 64

#if PG_VERSION_NUM >= 160000
extern int32 pg_jitter_yj_is_json_datum(Datum datum, int32 item_type,
										int32 unique_keys);
#endif

extern Datum pg_jitter_yj_jsonb_in(Datum cstring_datum,
								   FunctionCallInfo fcinfo);

extern uint64 pg_jitter_yyjson_is_json_counter;
extern uint64 pg_jitter_yyjson_jsonb_in_counter;

#endif /* PG_JITTER_HAVE_YYJSON */

#endif /* PG_JITTER_YYJSON_H */
