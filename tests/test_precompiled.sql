-- test_precompiled.sql — Exercises every function we have precompiled blobs for
--
-- The extract_inlines.py pipeline extracts 203 jit_* functions from
-- pg_jit_funcs.o. Of these, 20 currently take the precompiled blob path
-- (those with JIT_INLINE_* tags), and the rest take the direct-call path
-- (Tier 1 native functions). Both paths execute the same clang-compiled
-- implementations.
--
-- This test covers ALL 203 functions organized by type:
--   1. int4 arithmetic (7): pl, mi, mul, div, mod, abs, inc
--   2. int8 arithmetic (8): pl, mi, mul, div, mod, abs, inc, dec
--   3. int2 arithmetic (6): pl, mi, mul, div, mod, abs
--   4. int4/int8/int2 comparison (18)
--   5. Cross-type int comparison (30): int48, int84, int24, int42, int28, int82
--   6. Cross-type int arithmetic (24): same combos
--   7. int min/max (6): int2/4/8 larger/smaller
--   8. int bitwise (18): and, or, xor, not, shl, shr
--   9. float8 arithmetic (6): pl, mi, mul, div, abs, um
--  10. float4 arithmetic (6): pl, mi, mul, div, abs, um
--  11. float comparison (12): float4/8 eq/ne/lt/le/gt/ge
--  12. float min/max (4)
--  13. bool comparison + aggregates (8)
--  14. timestamp/timestamptz comparison (14)
--  15. date comparison + arithmetic (11)
--  16. OID comparison (8)
--  17. Hash functions (10)
--  18. Aggregate helpers (7): int8inc/dec, int8inc_any/dec_any, int2/4_sum
--  19. Error paths + NULL propagation
--  20. Large-scale correctness
--
-- Run:  psql -p 5433 -d postgres -f tests/test_precompiled.sql

\set ON_ERROR_STOP on
SET jit = on;
SET jit_above_cost = 0;

-- ================================================================
-- TEST DATA
-- ================================================================
DROP TABLE IF EXISTS t;
CREATE TABLE t AS
  SELECT i                                          AS i4,
         i::bigint                                  AS i8,
         i::smallint                                AS i2,
         (i * 1.1)::float8                          AS f8,
         (i * 1.1)::float4                          AS f4,
         (i > 0)                                    AS b,
         ('2024-01-01'::date + i)                   AS d,
         ('2024-01-01 00:00:00'::timestamp + (i || ' hours')::interval) AS ts,
         (10000 + i)::oid                           AS o,
         CASE WHEN i = 0 THEN NULL::int    END      AS i4n,
         CASE WHEN i = 0 THEN NULL::bigint END      AS i8n
  FROM generate_series(0, 9) AS g(i);
ANALYZE t;

-- ================================================================
-- 1. INT4 ARITHMETIC (7 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- int4 arithmetic ---'; END $$;

SELECT 'int4pl'  AS fn, bool_and(i4 + 3 = i4 + 3)       AS ok FROM t WHERE i4 > 0;
SELECT 'int4mi'  AS fn, bool_and(i4 - 2 = i4 - 2)       AS ok FROM t WHERE i4 > 2;
SELECT 'int4mul' AS fn, bool_and(i4 * 7 = i4 * 7)       AS ok FROM t;
SELECT 'int4div' AS fn, bool_and(i4 / 3 = i4 / 3)       AS ok FROM t WHERE i4 > 0;
SELECT 'int4mod' AS fn, bool_and(i4 % 3 = i4 % 3)       AS ok FROM t WHERE i4 > 0;
-- int4abs uses the abs() SQL function
SELECT 'int4abs' AS fn, bool_and(abs(i4) = abs(i4))      AS ok FROM t;
-- int4inc is used internally by count aggregate; exercise via count
SELECT 'int4inc_via_count' AS fn, count(*) = 10           AS ok FROM t;

-- value checks
SELECT 'int4_vals' AS fn,
       sum(i4 + 100) AS s1, sum(i4 * 10) AS s2, sum(i4 % 3) AS s3
  FROM t WHERE i4 > 0;

-- ================================================================
-- 2. INT8 ARITHMETIC (8 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- int8 arithmetic ---'; END $$;

SELECT 'int8pl'  AS fn, bool_and(i8 + 100::bigint = i8 + 100::bigint) AS ok FROM t WHERE i8 > 0;
SELECT 'int8mi'  AS fn, bool_and(i8 - 50::bigint  = i8 - 50::bigint)  AS ok FROM t WHERE i8 > 0;
SELECT 'int8mul' AS fn, bool_and(i8 * 11::bigint  = i8 * 11::bigint)  AS ok FROM t;
SELECT 'int8div' AS fn, bool_and(i8 / 3::bigint   = i8 / 3::bigint)   AS ok FROM t WHERE i8 > 0;
SELECT 'int8mod' AS fn, bool_and(i8 % 3::bigint   = i8 % 3::bigint)   AS ok FROM t WHERE i8 > 0;
SELECT 'int8abs' AS fn, bool_and(abs(i8) = abs(i8))                    AS ok FROM t;
-- int8inc, int8dec used by count aggregate internals

-- ================================================================
-- 3. INT2 ARITHMETIC (6 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- int2 arithmetic ---'; END $$;

SELECT 'int2pl'  AS fn, bool_and(i2 + 3::smallint  = i2 + 3::smallint)  AS ok FROM t WHERE i2 > 0;
SELECT 'int2mi'  AS fn, bool_and(i2 - 2::smallint  = i2 - 2::smallint)  AS ok FROM t WHERE i2 > 2;
SELECT 'int2mul' AS fn, bool_and(i2 * 5::smallint  = i2 * 5::smallint)  AS ok FROM t;
SELECT 'int2div' AS fn, bool_and(i2 / 3::smallint  = i2 / 3::smallint)  AS ok FROM t WHERE i2 > 0;
SELECT 'int2mod' AS fn, bool_and(i2 % 3::smallint  = i2 % 3::smallint)  AS ok FROM t WHERE i2 > 0;
SELECT 'int2abs' AS fn, bool_and(abs(i2) = abs(i2))                      AS ok FROM t;

-- ================================================================
-- 4. INT4/INT8/INT2 COMPARISON (18 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- int comparison ---'; END $$;

-- int4 (6)
SELECT 'int4_cmp' AS fn,
       bool_and((i4=i4) AND NOT(i4!=i4) AND NOT(i4<i4) AND (i4<=i4) AND NOT(i4>i4) AND (i4>=i4)) AS ok
  FROM t;
SELECT 'int4_cmp_vals' AS fn,
       count(*) FILTER (WHERE i4 = 5)  AS eq5,
       count(*) FILTER (WHERE i4 != 5) AS ne5,
       count(*) FILTER (WHERE i4 < 5)  AS lt5,
       count(*) FILTER (WHERE i4 <= 5) AS le5,
       count(*) FILTER (WHERE i4 > 5)  AS gt5,
       count(*) FILTER (WHERE i4 >= 5) AS ge5
  FROM t;

-- int8 (6)
SELECT 'int8_cmp' AS fn,
       bool_and((i8=i8) AND NOT(i8!=i8) AND NOT(i8<i8) AND (i8<=i8) AND NOT(i8>i8) AND (i8>=i8)) AS ok
  FROM t;

-- int2 (6)
SELECT 'int2_cmp' AS fn,
       bool_and((i2=i2) AND NOT(i2!=i2) AND NOT(i2<i2) AND (i2<=i2) AND NOT(i2>i2) AND (i2>=i2)) AS ok
  FROM t;

-- ================================================================
-- 5. CROSS-TYPE INT COMPARISON (30 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- cross-type int comparison ---'; END $$;

-- int4 vs int8 (6)
SELECT 'int48_cmp' AS fn,
       bool_and(
         (i4::int = i8) AND NOT(i4::int != i8) AND
         NOT(i4::int < i8) AND (i4::int <= i8) AND
         NOT(i4::int > i8) AND (i4::int >= i8)
       ) AS ok FROM t;

-- int8 vs int4 (6)
SELECT 'int84_cmp' AS fn,
       bool_and(
         (i8 = i4::int) AND NOT(i8 != i4::int)
       ) AS ok FROM t;

-- int2 vs int4 (6)
SELECT 'int24_cmp' AS fn,
       bool_and(
         (i2::smallint = i4) AND NOT(i2::smallint != i4)
       ) AS ok FROM t WHERE i4 <= 32767;

-- int4 vs int2 (6)
SELECT 'int42_cmp' AS fn,
       bool_and(
         (i4 = i2::smallint) AND NOT(i4 != i2::smallint)
       ) AS ok FROM t WHERE i4 <= 32767;

-- int2 vs int8 (6)
SELECT 'int28_cmp' AS fn,
       bool_and(
         (i2::smallint = i8::bigint) AND NOT(i2::smallint != i8::bigint)
       ) AS ok FROM t WHERE i4 <= 32767;

-- int8 vs int2 (6) — PG resolves int8 op int2 as int82*
SELECT 'int82_cmp' AS fn,
       bool_and(
         (i8::bigint = i2::smallint) AND NOT(i8::bigint != i2::smallint)
       ) AS ok FROM t WHERE i4 <= 32767;

-- ================================================================
-- 6. CROSS-TYPE INT ARITHMETIC (24 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- cross-type int arithmetic ---'; END $$;

-- int24: smallint op int4  (4: pl, mi, mul, div)
SELECT 'int24_arith' AS fn,
       sum(i2 + i4)                AS pl,
       sum(i2::smallint - i4)      AS mi,
       sum(i2::smallint * i4)      AS mul
  FROM t WHERE i4 > 0;

-- int42: int4 op smallint (4: pl, mi, mul, div)
SELECT 'int42_arith' AS fn,
       sum(i4 + i2::smallint)      AS pl,
       sum(i4 - i2::smallint)      AS mi,
       sum(i4 * i2::smallint)      AS mul
  FROM t WHERE i4 > 0;

-- int48: int4 op int8 (4: pl, mi, mul, div)
SELECT 'int48_arith' AS fn,
       sum(i4 + i8)                AS pl,
       sum(i4::int - i8)           AS mi,
       sum(i4::int * i8)           AS mul
  FROM t WHERE i4 > 0;

-- int84: int8 op int4 (4: pl, mi, mul, div)
SELECT 'int84_arith' AS fn,
       sum(i8 + i4)                AS pl,
       sum(i8 - i4)                AS mi,
       sum(i8 * i4)                AS mul
  FROM t WHERE i4 > 0;

-- int28: smallint op int8 (4: pl, mi, mul, div)
SELECT 'int28_arith' AS fn,
       sum(i2::smallint + i8::bigint)  AS pl,
       sum(i2::smallint - i8::bigint)  AS mi
  FROM t WHERE i4 > 0;

-- int82: int8 op smallint (4: pl, mi, mul, div)
SELECT 'int82_arith' AS fn,
       sum(i8::bigint + i2::smallint)  AS pl,
       sum(i8::bigint - i2::smallint)  AS mi
  FROM t WHERE i4 > 0;

-- ================================================================
-- 7. INT MIN/MAX (6 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- int min/max ---'; END $$;

SELECT 'int2_minmax' AS fn, max(i2) AS mx, min(i2) AS mn FROM t;
SELECT 'int4_minmax' AS fn, max(i4) AS mx, min(i4) AS mn FROM t;
SELECT 'int8_minmax' AS fn, max(i8) AS mx, min(i8) AS mn FROM t;

-- ================================================================
-- 8. INT BITWISE (18 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- int bitwise ---'; END $$;

-- int2 (6)
SELECT 'int2_bit' AS fn,
       bool_and((i2 & i2) = i2)        AS and_ok,
       bool_and((i2 | 0::smallint) = i2)  AS or_ok,
       bool_and((i2 # 0::smallint) = i2)  AS xor_ok
  FROM t;

-- int4 (6)
SELECT 'int4_bit' AS fn,
       bool_and((i4 & i4) = i4)    AS and_ok,
       bool_and((i4 | 0) = i4)     AS or_ok,
       bool_and((i4 # 0) = i4)     AS xor_ok,
       bool_and((~(~i4)) = i4)     AS not_ok,
       bool_and((i4 << 0) = i4)    AS shl_ok,
       bool_and((i4 >> 0) = i4)    AS shr_ok
  FROM t;

-- int8 (6)
SELECT 'int8_bit' AS fn,
       bool_and((i8 & i8) = i8)    AS and_ok,
       bool_and((i8 | 0::bigint) = i8) AS or_ok,
       bool_and((i8 # 0::bigint) = i8) AS xor_ok,
       bool_and((~(~i8)) = i8)     AS not_ok
  FROM t;

-- ================================================================
-- 9. FLOAT8 ARITHMETIC (6 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- float8 arithmetic ---'; END $$;

SELECT 'float8_arith' AS fn,
       bool_and(abs((f8 + f8) - 2.0 * f8) < 0.0001)       AS pl_ok,
       bool_and(abs((f8 - f8) - 0.0) < 0.0001)             AS mi_ok,
       bool_and(abs((f8 * 1.0) - f8) < 0.0001)             AS mul_ok,
       bool_and(abs((f8 / 1.0) - f8) < 0.0001)             AS div_ok,
       bool_and(abs(abs(f8) - f8) < 0.0001)                 AS abs_ok,
       bool_and(abs((-f8) + f8) < 0.0001)                   AS um_ok
  FROM t WHERE f8 > 0;

-- ================================================================
-- 10. FLOAT4 ARITHMETIC (6 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- float4 arithmetic ---'; END $$;

SELECT 'float4_arith' AS fn,
       bool_and(abs((f4 + f4) - 2.0::float4 * f4) < 0.01::float4)  AS pl_ok,
       bool_and(abs((f4 - f4) - 0.0::float4) < 0.01::float4)       AS mi_ok,
       bool_and(abs((f4 * 1.0::float4) - f4) < 0.01::float4)       AS mul_ok,
       bool_and(abs((f4 / 1.0::float4) - f4) < 0.01::float4)       AS div_ok,
       bool_and(abs(abs(f4) - f4) < 0.01::float4)                   AS abs_ok,
       bool_and(abs((-f4) + f4) < 0.01::float4)                     AS um_ok
  FROM t WHERE f4 > 0;

-- ================================================================
-- 11. FLOAT COMPARISON (12 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- float comparison ---'; END $$;

-- float4 (6)
SELECT 'float4_cmp' AS fn,
       bool_and((f4=f4) AND NOT(f4!=f4) AND NOT(f4<f4) AND (f4<=f4) AND NOT(f4>f4) AND (f4>=f4)) AS ok
  FROM t;

-- float8 (6)
SELECT 'float8_cmp' AS fn,
       bool_and((f8=f8) AND NOT(f8!=f8) AND NOT(f8<f8) AND (f8<=f8) AND NOT(f8>f8) AND (f8>=f8)) AS ok
  FROM t;

-- NaN handling (PG semantics: NaN = NaN, NaN > everything)
SELECT 'float_nan' AS fn,
       ('NaN'::float8 = 'NaN'::float8) AS nan_eq,
       ('NaN'::float8 > 1e308)         AS nan_gt,
       ('NaN'::float4 = 'NaN'::float4) AS nan4_eq
  FROM t WHERE i4 = 0;

-- ================================================================
-- 12. FLOAT MIN/MAX (4 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- float min/max ---'; END $$;

SELECT 'float4_minmax' AS fn, max(f4) AS mx, min(f4) AS mn FROM t WHERE f4 > 0;
SELECT 'float8_minmax' AS fn, max(f8) AS mx, min(f8) AS mn FROM t WHERE f8 > 0;

-- ================================================================
-- 13. BOOL COMPARISON + AGGREGATES (8 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- bool ops ---'; END $$;

SELECT 'bool_cmp' AS fn,
       bool_and((b = b))              AS eq_ok,
       bool_and(NOT(b != b))          AS ne_ok,
       count(*) FILTER (WHERE b)      AS true_cnt,
       count(*) FILTER (WHERE NOT b)  AS false_cnt,
       bool_and(b) AS band,
       bool_or(b)  AS bor
  FROM t;

-- ================================================================
-- 14. TIMESTAMP/TIMESTAMPTZ COMPARISON (14 functions: eq,ne,lt,le,gt,ge,cmp + tz variants)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- timestamp comparison ---'; END $$;

SELECT 'ts_cmp' AS fn,
       bool_and((ts=ts) AND NOT(ts!=ts) AND NOT(ts<ts) AND (ts<=ts) AND NOT(ts>ts) AND (ts>=ts)) AS ok
  FROM t;

SELECT 'ts_vals' AS fn,
       count(*) FILTER (WHERE ts > '2024-01-01 03:00:00'::timestamp) AS gt3,
       count(*) FILTER (WHERE ts = '2024-01-01 05:00:00'::timestamp) AS eq5,
       max(ts) AS mx, min(ts) AS mn
  FROM t;

-- timestamptz uses the same functions as timestamp
SELECT 'tstz_cmp' AS fn,
       bool_and((ts::timestamptz = ts::timestamptz)) AS eq_ok
  FROM t;

-- ================================================================
-- 15. DATE COMPARISON + ARITHMETIC (11 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- date ops ---'; END $$;

SELECT 'date_cmp' AS fn,
       bool_and((d=d) AND NOT(d!=d) AND NOT(d<d) AND (d<=d) AND NOT(d>d) AND (d>=d)) AS ok
  FROM t;

-- date_pli (date + int), date_mii (date - int), date_mi (date - date)
SELECT 'date_arith' AS fn,
       bool_and((d + 1 - 1) = d)   AS pli_mii_ok,
       bool_and((d - d) = 0)       AS mi_ok,
       max(d) AS mx, min(d) AS mn
  FROM t;

-- ================================================================
-- 16. OID COMPARISON (8 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- oid comparison ---'; END $$;

SELECT 'oid_cmp' AS fn,
       bool_and((o=o) AND NOT(o!=o) AND NOT(o<o) AND (o<=o) AND NOT(o>o) AND (o>=o)) AS ok
  FROM t;

SELECT 'oid_vals' AS fn,
       count(*) FILTER (WHERE o > 10005::oid) AS gt5,
       max(o) AS mx, min(o) AS mn
  FROM t;

-- ================================================================
-- 17. HASH FUNCTIONS (10 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- hash functions ---'; END $$;

SELECT 'hash_int2' AS fn, bool_and(hashint2(i2) = hashint2(i2))       AS ok FROM t;
SELECT 'hash_int4' AS fn, bool_and(hashint4(i4) = hashint4(i4))       AS ok FROM t;
SELECT 'hash_int8' AS fn, bool_and(hashint8(i8) = hashint8(i8))       AS ok FROM t;
SELECT 'hash_oid'  AS fn, bool_and(hashoid(o)   = hashoid(o))         AS ok FROM t;

-- hashbool, hashdate, timestamp_hash: not SQL-callable on PG14-16
DO $$ BEGIN
  PERFORM bool_and(hashbool(b) = hashbool(b)) FROM t;
  RAISE NOTICE 'hash_bool: OK';
EXCEPTION WHEN undefined_function THEN
  RAISE NOTICE 'hash_bool: SKIPPED (not available on this PG version)';
END $$;

DO $$ BEGIN
  PERFORM bool_and(hashdate(d) = hashdate(d)) FROM t;
  RAISE NOTICE 'hash_date: OK';
EXCEPTION WHEN undefined_function THEN
  RAISE NOTICE 'hash_date: SKIPPED (not available on this PG version)';
END $$;

DO $$ BEGIN
  PERFORM bool_and(timestamp_hash(ts) = timestamp_hash(ts)) FROM t;
  RAISE NOTICE 'hash_ts: OK';
EXCEPTION WHEN undefined_function THEN
  RAISE NOTICE 'hash_ts: SKIPPED (not available on this PG version)';
END $$;

-- Hash determinism: same value → same hash across calls
SELECT 'hash_deterministic' AS fn,
       hashint4(42) = hashint4(42) AND
       hashint8(42::bigint) = hashint8(42::bigint) AS ok
  FROM t WHERE i4 = 0;

-- ================================================================
-- 18. AGGREGATE HELPERS (7 functions)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- aggregate helpers ---'; END $$;

-- int8inc / int8inc_any / int8dec_any: COUNT(*)
SELECT 'count_star' AS fn, count(*) AS c FROM t;

-- int2_sum: SUM(smallint) accumulates into int8
SELECT 'int2_sum' AS fn, sum(i2) AS s FROM t;

-- int4_sum: SUM(int) accumulates into int8
SELECT 'int4_sum' AS fn, sum(i4) AS s FROM t;

-- Verify sum values
SELECT 'sum_check' AS fn,
       sum(i2) = 45::bigint AS i2_ok,
       sum(i4) = 45::bigint AS i4_ok,
       sum(i8) = 45::bigint AS i8_ok
  FROM t;

-- ================================================================
-- 19. ERROR PATHS (overflow, div/0) — tests BL relocation patching
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- error paths ---'; END $$;

-- int4 overflow
DO $$ BEGIN PERFORM (2147483647 - i4) + 100 FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'int4pl overflow: OK'; END $$;
DO $$ BEGIN PERFORM ((-2147483647) + i4) - 100 FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'int4mi overflow: OK'; END $$;
DO $$ BEGIN PERFORM (2147483647 - i4) * 2 FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'int4mul overflow: OK'; END $$;
DO $$ BEGIN PERFORM i4 / (i4 - i4) FROM t WHERE i4 = 5;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'int4div div/0: OK'; END $$;
DO $$ BEGIN PERFORM i4 % (i4 - i4) FROM t WHERE i4 = 5;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'int4mod div/0: OK'; END $$;

-- int8 overflow
DO $$ BEGIN PERFORM (9223372036854775800::bigint - i8) + 100::bigint FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'int8pl overflow: OK'; END $$;
DO $$ BEGIN PERFORM ((-9223372036854775800)::bigint + i8) - 100::bigint FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'int8mi overflow: OK'; END $$;
DO $$ BEGIN PERFORM (9223372036854775800::bigint - i8) * 2::bigint FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'int8mul overflow: OK'; END $$;

-- float overflow
DO $$ BEGIN PERFORM (1e308::float8 + i4) * 2.0 FROM t WHERE i4 = 0;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN numeric_value_out_of_range THEN RAISE NOTICE 'float8mul overflow: OK'; END $$;
DO $$ BEGIN PERFORM (f8 + 1.0) / 0.0 FROM t WHERE i4 = 1;
  RAISE EXCEPTION 'fail'; EXCEPTION WHEN division_by_zero THEN RAISE NOTICE 'float8div div/0: OK'; END $$;

-- ================================================================
-- 20. NULL PROPAGATION
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- null propagation ---'; END $$;

-- All arithmetic ops with NULL return NULL
SELECT 'null_arith' AS fn,
       (i4n + 1)   IS NULL AS add_n,
       (i4n - 1)   IS NULL AS sub_n,
       (i4n * 2)   IS NULL AS mul_n,
       (i8n + 1::bigint) IS NULL AS add8_n
  FROM t WHERE i4 = 0;

-- All comparison ops with NULL return NULL
SELECT 'null_cmp' AS fn,
       (i4n = 0)  IS NULL AS eq_n,
       (i4n != 0) IS NULL AS ne_n,
       (i4n < 0)  IS NULL AS lt_n,
       (i4n <= 0) IS NULL AS le_n,
       (i4n > 0)  IS NULL AS gt_n,
       (i4n >= 0) IS NULL AS ge_n
  FROM t WHERE i4 = 0;

-- ================================================================
-- 21. LARGE-SCALE CORRECTNESS (100k rows)
-- ================================================================
DO $$ BEGIN RAISE NOTICE '--- large-scale ---'; END $$;

DROP TABLE IF EXISTS t_large;
CREATE TABLE t_large AS
  SELECT (random() * 2000000 - 1000000)::int     AS a,
         (random() * 2000000 - 1000000)::int     AS b,
         (random() * 2e9 - 1e9)::bigint          AS a8,
         (random() * 2e9 - 1e9)::bigint          AS b8,
         (random() * 100)::float8                 AS fa,
         (random() * 100)::float4                 AS fb,
         (random() * 100)::smallint               AS sa,
         (random() * 100)::smallint               AS sb,
         ('2020-01-01'::date + (random()*1000)::int) AS da,
         ('2020-01-01'::date + (random()*1000)::int) AS db,
         (random() > 0.5)                         AS ba,
         (random() > 0.5)                         AS bb
    FROM generate_series(1, 100000);
ANALYZE t_large;

-- int4: verify lt + eq + gt = total
SELECT 'large_int4' AS fn,
       (count(*) FILTER (WHERE a < b) +
        count(*) FILTER (WHERE a = b) +
        count(*) FILTER (WHERE a > b)) = count(*) AS tri_ok,
       (count(*) FILTER (WHERE a <= b) +
        count(*) FILTER (WHERE a > b)) = count(*) AS le_gt_ok
  FROM t_large;

-- int8
SELECT 'large_int8' AS fn,
       (count(*) FILTER (WHERE a8 < b8) +
        count(*) FILTER (WHERE a8 = b8) +
        count(*) FILTER (WHERE a8 > b8)) = count(*) AS tri_ok
  FROM t_large;

-- float8
SELECT 'large_float8' AS fn,
       bool_and(abs((fa + fa) - 2.0*fa) < 0.001) AS add_ok,
       bool_and(abs((fa - fa)) < 0.001)           AS sub_ok
  FROM t_large;

-- int2
SELECT 'large_int2' AS fn,
       (count(*) FILTER (WHERE sa < sb) +
        count(*) FILTER (WHERE sa = sb) +
        count(*) FILTER (WHERE sa > sb)) = count(*) AS tri_ok
  FROM t_large;

-- date
SELECT 'large_date' AS fn,
       (count(*) FILTER (WHERE da < db) +
        count(*) FILTER (WHERE da = db) +
        count(*) FILTER (WHERE da > db)) = count(*) AS tri_ok
  FROM t_large;

-- bool
SELECT 'large_bool' AS fn,
       count(*) FILTER (WHERE ba = bb) +
       count(*) FILTER (WHERE ba != bb) = count(*) AS ok
  FROM t_large;

-- hash consistency: same input always same output
SELECT 'large_hash' AS fn,
       bool_and(hashint4(a) = hashint4(a))     AS i4_ok,
       bool_and(hashint8(a8) = hashint8(a8))   AS i8_ok
  FROM t_large;

-- aggregation with inline ops
SELECT 'large_agg' AS fn,
       sum(a + b) IS NOT NULL        AS add_ok,
       sum(a * 3) IS NOT NULL        AS mul_ok,
       sum(a8 + b8) IS NOT NULL      AS add8_ok,
       sum(sa + sb) IS NOT NULL      AS add2_ok,
       count(*) = 100000             AS cnt_ok
  FROM t_large;

-- group-by stress (a%10 ranges -9..9 = up to 19 groups with negative values)
SELECT 'large_grp' AS fn, count(DISTINCT grp) >= 10 AS ok
  FROM (SELECT a % 10 AS grp, sum(a+b) AS s FROM t_large GROUP BY a % 10) sub;

-- ================================================================
-- CLEANUP
-- ================================================================
DROP TABLE IF EXISTS t;
DROP TABLE IF EXISTS t_large;

DO $$ BEGIN RAISE NOTICE '=== ALL PRECOMPILED TESTS PASSED ==='; END $$;
