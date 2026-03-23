-- test_parallel_shared.sql — Correctness test for parallel shared JIT code
--
-- Tests that leader-compiled JIT code shared with workers via DSM produces
-- correct results for all expression types, including tricky cases:
--   - byref CONST datums (text literals, embedded pointers)
--   - fn_addr relocation (different ASLR base in workers)
--   - deform dispatch (shared vs local deform functions)
--   - text IN-list hash tables (process-local tables)
--   - PCRE2 regex (process-local compiled patterns)
--   - CASE binary search (pointer to arrays in step data)
--   - aggregate transitions (strict/non-strict, byval/byref)
--   - hash joins (HASHDATUM opcodes)
--   - IOCOERCE (type casts like int→text)
--
-- Each test compares parallel result against a serial (p=0) baseline.
-- All tests are run under pg_jitter.parallel_mode = 'shared'.
--
-- Usage:
--   psql -p PORT -d postgres -f tests/test_parallel_shared.sql
--   Exit code 0 = all passed, non-zero = failures

\set ON_ERROR_STOP on
\pset pager off
\timing off

-- ============================================================
-- Setup
-- ============================================================
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET pg_jitter.parallel_mode = 'shared';

-- Force parallel execution
SET parallel_tuple_cost = 0;
SET parallel_setup_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;

-- Create test tables if they don't exist
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'par_test') THEN
    CREATE TABLE par_test AS
      SELECT i AS id,
             i % 100 AS grp,
             i AS val,
             ('text_' || i)::text AS txt,
             CASE WHEN i % 10 = 0 THEN NULL ELSE i END AS nullable_val,
             CASE WHEN i % 5 = 0 THEN NULL ELSE ('word_' || (i % 1000))::text END AS nullable_txt,
             (i::float8 / 7.0) AS fval,
             ('2020-01-01'::date + (i % 365)) AS dt,
             (i % 2 = 0) AS flag
      FROM generate_series(1, 500000) i;
    ANALYZE par_test;
  END IF;

  IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'par_right') THEN
    CREATE TABLE par_right AS
      SELECT i AS id,
             i % 100 AS grp,
             i * 2 AS rval,
             ('right_' || i)::text AS rtxt
      FROM generate_series(1, 100000) i;
    ANALYZE par_right;
  END IF;

  -- Wide nullable table for deform testing (100 cols, ~90% NULL)
  IF NOT EXISTS (SELECT 1 FROM pg_class WHERE relname = 'par_wide') THEN
    EXECUTE (
      SELECT 'CREATE TABLE par_wide AS SELECT i AS id, i % 10 AS grp, ' ||
             string_agg(
               format('CASE WHEN random() > 0.9 THEN %s END AS c%s',
                      CASE WHEN x % 3 = 0 THEN 'i::int'
                           WHEN x % 3 = 1 THEN '''val_'' || i'
                           ELSE 'i::float8' END,
                      lpad(x::text, 3, '0')),
               ', ') ||
             ' FROM generate_series(1, 50000) i'
      FROM generate_series(1, 80) x
    );
    ANALYZE par_wide;
  END IF;
END $$;

-- ============================================================
-- Helper: compare serial (p=0) vs parallel (p=4) results
-- ============================================================
CREATE OR REPLACE FUNCTION par_check(test_name text, query text)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
  serial_result text;
  parallel_result text;
BEGIN
  -- Serial (no parallel workers)
  EXECUTE 'SET max_parallel_workers_per_gather = 0';
  EXECUTE 'SELECT (' || query || ')::text' INTO serial_result;

  -- Parallel (4 workers, shared code)
  EXECUTE 'SET max_parallel_workers_per_gather = 4';
  EXECUTE 'SELECT (' || query || ')::text' INTO parallel_result;

  IF serial_result IS DISTINCT FROM parallel_result THEN
    RAISE EXCEPTION 'FAIL [%]: serial=% parallel=%', test_name, serial_result, parallel_result;
  ELSE
    RAISE NOTICE 'PASS [%]: %', test_name, serial_result;
  END IF;
END $$;

\echo ''
\echo '======================================================='
\echo '=== Parallel Shared Code Correctness Tests          ==='
\echo '======================================================='
\echo ''

-- ============================================================
-- 1. Basic expressions
-- ============================================================
\echo '--- Basic Expressions ---'
SELECT par_check('COUNT_star',
  $$SELECT count(*) FROM par_test$$);
SELECT par_check('SUM_int',
  $$SELECT sum(val) FROM par_test$$);
SELECT par_check('SUM_float',
  $$SELECT round(sum(fval)::numeric, 2) FROM par_test$$);
SELECT par_check('AVG_nullable',
  $$SELECT round(avg(nullable_val)::numeric, 2) FROM par_test$$);
SELECT par_check('COUNT_nullable',
  $$SELECT count(nullable_val) FROM par_test$$);
SELECT par_check('BOOL_expr',
  $$SELECT count(*) FROM par_test WHERE (val > 1000 AND val < 9000) OR (grp > 50 AND flag)$$);
SELECT par_check('ARITH_expr',
  $$SELECT sum(val + grp * 3 - id) FROM par_test WHERE val < 100000$$);

-- ============================================================
-- 2. Text operations (byref CONST, IOCOERCE)
-- ============================================================
\echo ''
\echo '--- Text / Byref Constants ---'
SELECT par_check('TEXT_EQ_const',
  $$SELECT count(*) FROM par_test WHERE txt = 'text_42'$$);
SELECT par_check('TEXT_LIKE',
  $$SELECT count(*) FROM par_test WHERE txt LIKE 'text_1%'$$);
SELECT par_check('TEXT_CONCAT',
  $$SELECT count(*) FROM par_test WHERE txt || '_suffix' = 'text_100_suffix'$$);
SELECT par_check('COALESCE_text',
  $$SELECT count(*) FROM par_test WHERE coalesce(nullable_txt, 'default') = 'default'$$);
SELECT par_check('CAST_int_text',
  $$SELECT count(*) FROM par_test WHERE val::text = '12345'$$);
SELECT par_check('NULLIF_text',
  $$SELECT count(nullif(nullable_txt, 'word_0')) FROM par_test$$);

-- ============================================================
-- 3. IN-list (integer and text hash tables)
-- ============================================================
\echo ''
\echo '--- IN-list (Hash Tables) ---'
SELECT par_check('IN_int_20',
  $$SELECT count(*) FROM par_test WHERE val IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)$$);
SELECT par_check('IN_text_5',
  $$SELECT count(*) FROM par_test WHERE txt IN ('text_1','text_50','text_100','text_500','text_1000')$$);
SELECT par_check('IN_text_20',
  $$SELECT count(*) FROM par_test WHERE nullable_txt IN ('word_1','word_5','word_10','word_15','word_20','word_25','word_30','word_35','word_40','word_45','word_50','word_55','word_60','word_65','word_70','word_75','word_80','word_85','word_90','word_99')$$);
SELECT par_check('NOT_IN_text',
  $$SELECT count(*) FROM par_test WHERE nullable_txt NOT IN ('word_1','word_2','word_3')$$);
SELECT par_check('IN_text_NULL',
  $$SELECT count(*) FROM par_test WHERE nullable_txt IN ('word_1', NULL)$$);

-- ============================================================
-- 4. LIKE / ILIKE / Regex (PCRE2)
-- ============================================================
\echo ''
\echo '--- LIKE / Regex ---'
SELECT par_check('LIKE_prefix',
  $$SELECT count(*) FROM par_test WHERE txt LIKE 'text_42%'$$);
SELECT par_check('LIKE_suffix',
  $$SELECT count(*) FROM par_test WHERE txt LIKE '%_42'$$);
SELECT par_check('LIKE_interior',
  $$SELECT count(*) FROM par_test WHERE txt LIKE '%_42%'$$);
SELECT par_check('ILIKE_prefix',
  $$SELECT count(*) FROM par_test WHERE txt ILIKE 'TEXT_1%'$$);
SELECT par_check('REGEX_simple',
  $$SELECT count(*) FROM par_test WHERE txt ~ '^text_[0-9]{3}$'$$);
SELECT par_check('REGEX_icase',
  $$SELECT count(*) FROM par_test WHERE txt ~* 'TEXT_42'$$);
SELECT par_check('REGEX_on_funcresult',
  $$SELECT count(*) FROM par_test WHERE lower(txt) ~ '^text_42$'$$);

-- ============================================================
-- 5. CASE expressions (binary search)
-- ============================================================
\echo ''
\echo '--- CASE Expressions ---'
SELECT par_check('CASE_simple',
  $$SELECT sum(CASE WHEN val < 1000 THEN 1 WHEN val < 5000 THEN 2 WHEN val < 9000 THEN 3 ELSE 4 END) FROM par_test$$);
SELECT par_check('CASE_text_result',
  $$SELECT count(*) FROM par_test WHERE CASE WHEN grp < 10 THEN 'low' WHEN grp < 50 THEN 'mid' ELSE 'high' END = 'mid'$$);
SELECT par_check('CASE_20way',
  $$SELECT sum(CASE grp WHEN 0 THEN 1 WHEN 1 THEN 2 WHEN 2 THEN 3 WHEN 3 THEN 4 WHEN 4 THEN 5 WHEN 5 THEN 6 WHEN 6 THEN 7 WHEN 7 THEN 8 WHEN 8 THEN 9 WHEN 9 THEN 10 WHEN 10 THEN 11 WHEN 11 THEN 12 WHEN 12 THEN 13 WHEN 13 THEN 14 WHEN 14 THEN 15 WHEN 15 THEN 16 WHEN 16 THEN 17 WHEN 17 THEN 18 WHEN 18 THEN 19 WHEN 19 THEN 20 ELSE 0 END) FROM par_test$$);

-- ============================================================
-- 6. Hash joins (HASHDATUM, deform)
-- ============================================================
\echo ''
\echo '--- Hash Joins ---'
SELECT par_check('HASHJOIN_int',
  $$SELECT count(*) FROM par_test t JOIN par_right r ON t.grp = r.grp$$);
SELECT par_check('HASHJOIN_sum',
  $$SELECT sum(t.val + r.rval) FROM par_test t JOIN par_right r ON t.id = r.id$$);
SELECT par_check('HASHJOIN_filter',
  $$SELECT count(*) FROM par_test t JOIN par_right r ON t.grp = r.grp WHERE t.val > 5000 AND r.rval < 100000$$);
SELECT par_check('HASHJOIN_text',
  $$SELECT count(*) FROM par_test t JOIN par_right r ON t.txt = r.rtxt$$);

-- ============================================================
-- 7. Aggregates (strict/non-strict, byval/byref)
-- ============================================================
\echo ''
\echo '--- Aggregates ---'
SELECT par_check('AGG_count_distinct',
  $$SELECT count(distinct grp) FROM par_test$$);
SELECT par_check('AGG_string_agg',
  $$SELECT length(string_agg(txt, ',')) FROM par_test WHERE id <= 100$$);
SELECT par_check('AGG_groupby_5',
  $$SELECT sum(c) FROM (SELECT grp, count(*) c, sum(val) s, avg(val) a, min(val) m, max(val) x FROM par_test GROUP BY grp) t$$);
SELECT par_check('AGG_nullable',
  $$SELECT count(nullable_val) || ',' || sum(nullable_val) || ',' || avg(nullable_val)::int FROM par_test$$);

-- ============================================================
-- 8. Subqueries and LATERAL
-- ============================================================
\echo ''
\echo '--- Subqueries ---'
SELECT par_check('EXISTS_semi',
  $$SELECT count(*) FROM par_test t WHERE EXISTS (SELECT 1 FROM par_right r WHERE r.grp = t.grp AND r.rval > 50000)$$);
SELECT par_check('NOT_EXISTS_anti',
  $$SELECT count(*) FROM par_test t WHERE NOT EXISTS (SELECT 1 FROM par_right r WHERE r.id = t.id)$$);
SELECT par_check('IN_subquery',
  $$SELECT count(*) FROM par_test WHERE grp IN (SELECT grp FROM par_right WHERE rval > 100000)$$);
SELECT par_check('SCALAR_subq',
  $$SELECT sum(val) FROM par_test WHERE grp < (SELECT avg(grp) FROM par_right)$$);

-- ============================================================
-- 9. Wide table deform (sparse dispatch)
-- ============================================================
\echo ''
\echo '--- Wide Table Deform ---'
SELECT par_check('WIDE_count',
  $$SELECT count(*) FROM par_wide$$);
SELECT par_check('WIDE_count_col',
  $$SELECT count(c080) FROM par_wide$$);
SELECT par_check('WIDE_sum_col',
  $$SELECT sum(c078::numeric)::bigint FROM par_wide$$);
SELECT par_check('WIDE_filter',
  $$SELECT count(*) FROM par_wide WHERE c001 IS NOT NULL$$);
SELECT par_check('WIDE_groupby',
  $$SELECT sum(c) FROM (SELECT grp, count(*) c FROM par_wide GROUP BY grp) t$$);

-- ============================================================
-- 10. Date/timestamp expressions
-- ============================================================
\echo ''
\echo '--- Date/Timestamp ---'
SELECT par_check('DATE_extract',
  $$SELECT sum(extract(month from dt)) FROM par_test$$);
SELECT par_check('DATE_compare',
  $$SELECT count(*) FROM par_test WHERE dt > '2020-06-01'::date AND dt < '2020-09-01'::date$$);
SELECT par_check('INTERVAL_arith',
  $$SELECT count(*) FROM par_test WHERE dt + interval '30 days' > '2020-12-01'::date$$);

-- ============================================================
-- Cleanup helper
-- ============================================================
DROP FUNCTION par_check(text, text);

\echo ''
\echo '======================================================='
\echo '=== ALL PARALLEL SHARED CODE TESTS PASSED           ==='
\echo '======================================================='
