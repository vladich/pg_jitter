/*
 * pg_jitter_compat.h — PostgreSQL version compatibility (PG14–PG18)
 *
 * Included early by pg_jitter_common.h.  Provides type/macro aliases so
 * PG18-style code compiles unchanged on older versions, plus feature-test
 * macros for opcodes that only exist in certain PG releases.
 */
#ifndef PG_JITTER_COMPAT_H
#define PG_JITTER_COMPAT_H

#include "postgres.h"

/* ----------------------------------------------------------------
 * PG_MODULE_MAGIC_EXT — PG18+ extended module magic
 * On older versions fall back to plain PG_MODULE_MAGIC.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM < 180000
#ifdef PG_MODULE_MAGIC_EXT
#undef PG_MODULE_MAGIC_EXT
#endif
#define PG_MODULE_MAGIC_EXT(...) PG_MODULE_MAGIC
#endif

/* ----------------------------------------------------------------
 * CompactAttribute / TupleDescCompactAttr — PG18 compact tuple attrs
 * Older versions use FormData_pg_attribute / TupleDescAttr.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM < 180000
#define CompactAttribute			FormData_pg_attribute
#define TupleDescCompactAttr(desc, i)	TupleDescAttr((desc), (i))
#endif

/* ----------------------------------------------------------------
 * JITTER_ATT_IS_NOTNULL — PG18 has attnullability on CompactAttribute;
 * older versions don't, so we conservatively assume nullable.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 180000
#define JITTER_ATT_IS_NOTNULL(att) \
	((att)->attnullability == ATTNULLABLE_VALID)
#else
#define JITTER_ATT_IS_NOTNULL(att) false
#endif

/* ----------------------------------------------------------------
 * JITTER_ATTALIGNBY — PG18 stores alignment as int (attalignby),
 * older versions store char (attalign).  This macro gives the int.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 180000
#define JITTER_ATTALIGNBY(att)	((att)->attalignby)
#else
static inline int
jitter_attalign_to_int(char align)
{
	switch (align)
	{
		case 'c': return 1;
		case 's': return 2;
		case 'i': return 4;
		case 'd': return 8;
		default:  return 4;
	}
}
#define JITTER_ATTALIGNBY(att)	jitter_attalign_to_int((att)->attalign)
#endif

/* ----------------------------------------------------------------
 * CompareType / cmptype — PG18 renamed RowCompareType → CompareType
 * and field rctype → cmptype.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 180000
#include "access/cmptype.h"
#else
#define CompareType			RowCompareType
#define COMPARE_LT			ROWCOMPARE_LT
#define COMPARE_LE			ROWCOMPARE_LE
#define COMPARE_GE			ROWCOMPARE_GE
#define COMPARE_GT			ROWCOMPARE_GT
#define COMPARE_EQ			ROWCOMPARE_EQ
#define COMPARE_NE			ROWCOMPARE_NE
#endif

/* Field name for row-compare-final changed in PG18 */
#if PG_VERSION_NUM < 180000
#define cmptype				rctype
#endif

/* ----------------------------------------------------------------
 * EEOP_DONE split — PG18 split EEOP_DONE into DONE_RETURN / DONE_NO_RETURN.
 * On older versions, only EEOP_DONE_RETURN is defined (maps to EEOP_DONE).
 * Code that handles EEOP_DONE_NO_RETURN must be guarded with
 * #ifdef HAVE_EEOP_DONE_SPLIT to avoid duplicate case values.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 180000
#define HAVE_EEOP_DONE_SPLIT
#else
#define EEOP_DONE_RETURN		EEOP_DONE
/* EEOP_DONE_NO_RETURN not defined — guard with #ifdef HAVE_EEOP_DONE_SPLIT */
#endif

/* ----------------------------------------------------------------
 * ExecAggCopyTransValue — renamed from ExecAggTransReparent in PG16
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM < 160000
#define ExecAggCopyTransValue	ExecAggTransReparent
#endif

/* ----------------------------------------------------------------
 * MarkGUCPrefixReserved — added in PG15, previously EmitWarningsOnPlaceholders
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM < 150000
#define MarkGUCPrefixReserved(prefix)	EmitWarningsOnPlaceholders(prefix)
#endif

/* ----------------------------------------------------------------
 * JitInstrumentation.deform_counter — added in PG17
 * On older PG, skip deform timing instrumentation.
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 170000
#define JITTER_INSTR_DEFORM_ACCUM(instr, end, start) \
	INSTR_TIME_ACCUM_DIFF((instr).deform_counter, (end), (start))
#else
#define JITTER_INSTR_DEFORM_ACCUM(instr, end, start) ((void) 0)
#endif

/* ================================================================
 * Feature-test macros — for #ifdef guards in backend code
 *
 * Each macro indicates an opcode (or group) exists in this PG version.
 * Backend files wrap the relevant code with #ifdef HAVE_EEOP_xxx.
 * ================================================================ */

/* --- PG18+ opcodes --- */
#if PG_VERSION_NUM >= 180000
#define HAVE_EEOP_OLD_NEW						/* OLD/NEW FETCHSOME/VAR/SYSVAR, ASSIGN_OLD/NEW_VAR */
#define HAVE_EEOP_RETURNINGEXPR				/* EEOP_RETURNINGEXPR + d.returningexpr */
#define HAVE_EEOP_PARAM_SET					/* EEOP_PARAM_SET */
#define HAVE_EEOP_FUNCEXPR_STRICT_12			/* EEOP_FUNCEXPR_STRICT_1/2 */
#define HAVE_EEOP_HASHDATUM					/* EEOP_HASHDATUM_* (5 opcodes) */
#define HAVE_EEOP_TESTVAL_EXT					/* EEOP_CASE_TESTVAL_EXT / EEOP_DOMAIN_TESTVAL_EXT */
#define HAVE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1	/* dedicated 1-arg variant */
#endif

/* --- PG17+ opcodes --- */
#if PG_VERSION_NUM >= 170000
#define HAVE_EEOP_IOCOERCE_SAFE				/* EEOP_IOCOERCE_SAFE */
#define HAVE_EEOP_JSONEXPR						/* EEOP_JSONEXPR_PATH/COERCION/COERCION_FINISH */
#define HAVE_EEOP_MERGE_SUPPORT_FUNC			/* EEOP_MERGE_SUPPORT_FUNC */
#endif

/* --- PG16+ opcodes --- */
#if PG_VERSION_NUM >= 160000
#define HAVE_EEOP_JSON_CONSTRUCTOR				/* EEOP_JSON_CONSTRUCTOR / EEOP_IS_JSON */
#define HAVE_EEOP_AGG_PRESORTED_DISTINCT		/* EEOP_AGG_PRESORTED_DISTINCT_SINGLE/MULTI */
#endif

#endif /* PG_JITTER_COMPAT_H */
