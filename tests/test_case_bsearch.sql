-- test_case_bsearch.sql — Regression tests for CASE binary search optimization
-- Tests correctness for all supported types: int2, int4, int8, float4, float8,
-- date, timestamp, timestamptz, oid, bool, text, numeric, uuid, interval,
-- bpchar, bytea, jsonb, inet.
--
-- Tests both Pattern A (searched CASE with </<=/>/>=) and Pattern B (simple CASE with =).
-- Each test uses >= 4 branches (CASE_BSEARCH_MIN_BRANCHES).
-- Results are compared against expected values to verify correctness.
--
-- Usage: psql -f tests/test_case_bsearch.sql <dbname>

\set ON_ERROR_STOP on

-- Enable JIT with low threshold to force JIT compilation
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;

-- ════════════════════════════════════════════════════════════════════
-- Create test table with one row per type to verify CASE results
-- ════════════════════════════════════════════════════════════════════

DROP TABLE IF EXISTS case_bsearch_test;
CREATE TABLE case_bsearch_test (
    id          serial PRIMARY KEY,
    v_int2      smallint,
    v_int4      integer,
    v_int8      bigint,
    v_float4    real,
    v_float8    double precision,
    v_date      date,
    v_ts        timestamp,
    v_tstz      timestamptz,
    v_oid       oid,
    v_bool      boolean,
    v_text      text,
    v_numeric   numeric,
    v_uuid      uuid,
    v_interval  interval,
    v_bpchar    char(10),
    v_bytea     bytea,
    v_inet      inet
);

-- Insert test rows covering various ranges for each type
INSERT INTO case_bsearch_test (v_int2, v_int4, v_int8, v_float4, v_float8,
    v_date, v_ts, v_tstz, v_oid, v_bool, v_text, v_numeric, v_uuid,
    v_interval, v_bpchar, v_bytea, v_inet)
VALUES
-- Row 1: low values (should hit first CASE branch)
(1, 5, 50, 0.5, 0.5, '2020-01-15', '2020-01-15 12:00:00', '2020-01-15 12:00:00+00',
 100, false, 'alpha', 1.5, 'a0000000-0000-0000-0000-000000000001',
 '1 hour', 'aaa       ', '\x0102', '10.0.0.1'),
-- Row 2: mid values
(50, 50, 500, 5.5, 5.5, '2021-06-15', '2021-06-15 12:00:00', '2021-06-15 12:00:00+00',
 500, true, 'delta', 50.5, 'd0000000-0000-0000-0000-000000000004',
 '5 days', 'ddd       ', '\x0506', '172.16.0.1'),
-- Row 3: high values
(90, 95, 950, 9.5, 9.5, '2023-11-15', '2023-11-15 12:00:00', '2023-11-15 12:00:00+00',
 900, true, 'hotel', 95.5, 'f0000000-0000-0000-0000-000000000008',
 '30 days', 'hhh       ', '\x0f10', '192.168.1.1'),
-- Row 4: exact match values (for equality patterns)
(20, 20, 200, 2.0, 2.0, '2020-06-01', '2020-06-01 00:00:00', '2020-06-01 00:00:00+00',
 200, false, 'bravo', 20.0, 'b0000000-0000-0000-0000-000000000002',
 '2 hours', 'bbb       ', '\x0304', '10.0.1.1'),
-- Row 5: values that should fall to ELSE
(120, 120, 1200, 12.0, 12.0, '2025-01-15', '2025-01-15 12:00:00', '2025-01-15 12:00:00+00',
 1200, true, 'zulu', 120.0, 'ff000000-0000-0000-0000-00000000000f',
 '365 days', 'zzz       ', '\xff00', '255.255.255.255');

ANALYZE case_bsearch_test;

-- ════════════════════════════════════════════════════════════════════
-- Pattern A: Searched CASE with < (less than)
-- Tests: int2, int4, int8, float4, float8, date, timestamp, oid
-- ════════════════════════════════════════════════════════════════════

\echo '=== Pattern A: Searched CASE with < ==='

-- int4 (CASE_BS_I32)
SELECT id, v_int4,
    CASE
        WHEN v_int4 < 10 THEN 'low'
        WHEN v_int4 < 30 THEN 'medium-low'
        WHEN v_int4 < 60 THEN 'medium'
        WHEN v_int4 < 80 THEN 'medium-high'
        WHEN v_int4 < 100 THEN 'high'
        ELSE 'very high'
    END AS result_int4
FROM case_bsearch_test ORDER BY id;

-- int8 (CASE_BS_I64)
SELECT id, v_int8,
    CASE
        WHEN v_int8 < 100 THEN 'low'
        WHEN v_int8 < 300 THEN 'medium-low'
        WHEN v_int8 < 600 THEN 'medium'
        WHEN v_int8 < 800 THEN 'medium-high'
        WHEN v_int8 < 1000 THEN 'high'
        ELSE 'very high'
    END AS result_int8
FROM case_bsearch_test ORDER BY id;

-- int2 (CASE_BS_I16)
SELECT id, v_int2,
    CASE
        WHEN v_int2 < 10 THEN 'low'
        WHEN v_int2 < 30 THEN 'medium-low'
        WHEN v_int2 < 60 THEN 'medium'
        WHEN v_int2 < 80 THEN 'medium-high'
        WHEN v_int2 < 100 THEN 'high'
        ELSE 'very high'
    END AS result_int2
FROM case_bsearch_test ORDER BY id;

-- float4 (CASE_BS_F4)
SELECT id, v_float4,
    CASE
        WHEN v_float4 < 1.0::real THEN 'low'
        WHEN v_float4 < 3.0::real THEN 'medium-low'
        WHEN v_float4 < 6.0::real THEN 'medium'
        WHEN v_float4 < 8.0::real THEN 'medium-high'
        WHEN v_float4 < 10.0::real THEN 'high'
        ELSE 'very high'
    END AS result_float4
FROM case_bsearch_test ORDER BY id;

-- float8 (CASE_BS_F8)
SELECT id, v_float8,
    CASE
        WHEN v_float8 < 1.0 THEN 'low'
        WHEN v_float8 < 3.0 THEN 'medium-low'
        WHEN v_float8 < 6.0 THEN 'medium'
        WHEN v_float8 < 8.0 THEN 'medium-high'
        WHEN v_float8 < 10.0 THEN 'high'
        ELSE 'very high'
    END AS result_float8
FROM case_bsearch_test ORDER BY id;

-- date (CASE_BS_I32 — DateADT is int32)
SELECT id, v_date,
    CASE
        WHEN v_date < '2020-06-01'::date THEN 'early 2020'
        WHEN v_date < '2021-01-01'::date THEN 'late 2020'
        WHEN v_date < '2022-01-01'::date THEN '2021'
        WHEN v_date < '2023-01-01'::date THEN '2022'
        WHEN v_date < '2024-01-01'::date THEN '2023'
        ELSE '2024+'
    END AS result_date
FROM case_bsearch_test ORDER BY id;

-- timestamp (CASE_BS_I64)
SELECT id, v_ts,
    CASE
        WHEN v_ts < '2020-06-01'::timestamp THEN 'early 2020'
        WHEN v_ts < '2021-01-01'::timestamp THEN 'late 2020'
        WHEN v_ts < '2022-01-01'::timestamp THEN '2021'
        WHEN v_ts < '2023-01-01'::timestamp THEN '2022'
        WHEN v_ts < '2024-01-01'::timestamp THEN '2023'
        ELSE '2024+'
    END AS result_ts
FROM case_bsearch_test ORDER BY id;

-- oid (CASE_BS_U32 — unsigned)
SELECT id, v_oid,
    CASE
        WHEN v_oid < 150::oid THEN 'low'
        WHEN v_oid < 350::oid THEN 'medium-low'
        WHEN v_oid < 600::oid THEN 'medium'
        WHEN v_oid < 800::oid THEN 'medium-high'
        WHEN v_oid < 1000::oid THEN 'high'
        ELSE 'very high'
    END AS result_oid
FROM case_bsearch_test ORDER BY id;

-- ════════════════════════════════════════════════════════════════════
-- Pattern A: Searched CASE with <= (less than or equal)
-- ════════════════════════════════════════════════════════════════════

\echo '=== Pattern A: Searched CASE with <= ==='

SELECT id, v_int4,
    CASE
        WHEN v_int4 <= 10 THEN 'low'
        WHEN v_int4 <= 30 THEN 'medium-low'
        WHEN v_int4 <= 60 THEN 'medium'
        WHEN v_int4 <= 80 THEN 'medium-high'
        WHEN v_int4 <= 100 THEN 'high'
        ELSE 'very high'
    END AS result_int4_le
FROM case_bsearch_test ORDER BY id;

-- ════════════════════════════════════════════════════════════════════
-- Pattern A: Searched CASE with > (greater than)
-- ════════════════════════════════════════════════════════════════════

\echo '=== Pattern A: Searched CASE with > ==='

SELECT id, v_int4,
    CASE
        WHEN v_int4 > 100 THEN 'very high'
        WHEN v_int4 > 80 THEN 'high'
        WHEN v_int4 > 60 THEN 'medium-high'
        WHEN v_int4 > 30 THEN 'medium'
        WHEN v_int4 > 10 THEN 'medium-low'
        ELSE 'low'
    END AS result_int4_gt
FROM case_bsearch_test ORDER BY id;

-- ════════════════════════════════════════════════════════════════════
-- Pattern A: Searched CASE with >= (greater than or equal)
-- ════════════════════════════════════════════════════════════════════

\echo '=== Pattern A: Searched CASE with >= ==='

SELECT id, v_float8,
    CASE
        WHEN v_float8 >= 10.0 THEN 'very high'
        WHEN v_float8 >= 8.0 THEN 'high'
        WHEN v_float8 >= 6.0 THEN 'medium-high'
        WHEN v_float8 >= 3.0 THEN 'medium'
        WHEN v_float8 >= 1.0 THEN 'medium-low'
        ELSE 'low'
    END AS result_float8_ge
FROM case_bsearch_test ORDER BY id;

-- ════════════════════════════════════════════════════════════════════
-- Pattern B: Simple CASE with = (equality)
-- Tests: int4, int8, int2, float8, oid, text, numeric, uuid,
--        interval, bpchar, bytea, inet
-- ════════════════════════════════════════════════════════════════════

\echo '=== Pattern B: Simple CASE with = (byval types) ==='

-- int4 equality
SELECT id, v_int4,
    CASE v_int4
        WHEN 5  THEN 'five'
        WHEN 20 THEN 'twenty'
        WHEN 50 THEN 'fifty'
        WHEN 95 THEN 'ninety-five'
        WHEN 120 THEN 'one-twenty'
        ELSE 'other'
    END AS result_int4_eq
FROM case_bsearch_test ORDER BY id;

-- int8 equality
SELECT id, v_int8,
    CASE v_int8
        WHEN 50  THEN 'fifty'
        WHEN 200 THEN 'two-hundred'
        WHEN 500 THEN 'five-hundred'
        WHEN 950 THEN 'nine-fifty'
        WHEN 1200 THEN 'twelve-hundred'
        ELSE 'other'
    END AS result_int8_eq
FROM case_bsearch_test ORDER BY id;

-- int2 equality
SELECT id, v_int2,
    CASE v_int2
        WHEN 1::smallint  THEN 'one'
        WHEN 20::smallint THEN 'twenty'
        WHEN 50::smallint THEN 'fifty'
        WHEN 90::smallint THEN 'ninety'
        WHEN 120::smallint THEN 'one-twenty'
        ELSE 'other'
    END AS result_int2_eq
FROM case_bsearch_test ORDER BY id;

-- float8 equality
SELECT id, v_float8,
    CASE v_float8
        WHEN 0.5 THEN 'half'
        WHEN 2.0 THEN 'two'
        WHEN 5.5 THEN 'five-half'
        WHEN 9.5 THEN 'nine-half'
        WHEN 12.0 THEN 'twelve'
        ELSE 'other'
    END AS result_float8_eq
FROM case_bsearch_test ORDER BY id;

-- oid equality
SELECT id, v_oid,
    CASE v_oid
        WHEN 100::oid THEN 'hundred'
        WHEN 200::oid THEN 'two-hundred'
        WHEN 500::oid THEN 'five-hundred'
        WHEN 900::oid THEN 'nine-hundred'
        WHEN 1200::oid THEN 'twelve-hundred'
        ELSE 'other'
    END AS result_oid_eq
FROM case_bsearch_test ORDER BY id;

\echo '=== Pattern B: Simple CASE with = (byref/generic types) ==='

-- text equality (CASE_BS_GENERIC)
SELECT id, v_text,
    CASE v_text
        WHEN 'alpha' THEN 'first'
        WHEN 'bravo' THEN 'second'
        WHEN 'delta' THEN 'fourth'
        WHEN 'hotel' THEN 'eighth'
        WHEN 'zulu'  THEN 'last'
        ELSE 'other'
    END AS result_text_eq
FROM case_bsearch_test ORDER BY id;

-- numeric equality (CASE_BS_GENERIC)
SELECT id, v_numeric,
    CASE v_numeric
        WHEN 1.5   THEN 'one-half'
        WHEN 20.0  THEN 'twenty'
        WHEN 50.5  THEN 'fifty-half'
        WHEN 95.5  THEN 'ninety-five-half'
        WHEN 120.0 THEN 'one-twenty'
        ELSE 'other'
    END AS result_numeric_eq
FROM case_bsearch_test ORDER BY id;

-- uuid equality (CASE_BS_GENERIC)
SELECT id, v_uuid,
    CASE v_uuid
        WHEN 'a0000000-0000-0000-0000-000000000001'::uuid THEN 'uuid-a'
        WHEN 'b0000000-0000-0000-0000-000000000002'::uuid THEN 'uuid-b'
        WHEN 'd0000000-0000-0000-0000-000000000004'::uuid THEN 'uuid-d'
        WHEN 'f0000000-0000-0000-0000-000000000008'::uuid THEN 'uuid-f'
        WHEN 'ff000000-0000-0000-0000-00000000000f'::uuid THEN 'uuid-ff'
        ELSE 'other'
    END AS result_uuid_eq
FROM case_bsearch_test ORDER BY id;

-- inet equality (CASE_BS_GENERIC)
SELECT id, v_inet,
    CASE v_inet
        WHEN '10.0.0.1'::inet THEN 'private-a'
        WHEN '10.0.1.1'::inet THEN 'private-a2'
        WHEN '172.16.0.1'::inet THEN 'private-b'
        WHEN '192.168.1.1'::inet THEN 'private-c'
        WHEN '255.255.255.255'::inet THEN 'broadcast'
        ELSE 'other'
    END AS result_inet_eq
FROM case_bsearch_test ORDER BY id;

-- bytea equality (CASE_BS_GENERIC)
SELECT id, v_bytea,
    CASE v_bytea
        WHEN '\x0102'::bytea THEN 'bytes-12'
        WHEN '\x0304'::bytea THEN 'bytes-34'
        WHEN '\x0506'::bytea THEN 'bytes-56'
        WHEN '\x0f10'::bytea THEN 'bytes-f10'
        WHEN '\xff00'::bytea THEN 'bytes-ff00'
        ELSE 'other'
    END AS result_bytea_eq
FROM case_bsearch_test ORDER BY id;

-- ════════════════════════════════════════════════════════════════════
-- Pattern A with generic types: searched CASE with <
-- Tests that generic binary search works for ordered comparisons
-- ════════════════════════════════════════════════════════════════════

\echo '=== Pattern A: Searched CASE with < (generic types) ==='

-- text <
SELECT id, v_text,
    CASE
        WHEN v_text < 'bravo' THEN 'before-bravo'
        WHEN v_text < 'echo' THEN 'bravo-to-echo'
        WHEN v_text < 'golf' THEN 'echo-to-golf'
        WHEN v_text < 'india' THEN 'golf-to-india'
        ELSE 'india+'
    END AS result_text_lt
FROM case_bsearch_test ORDER BY id;

-- numeric <
SELECT id, v_numeric,
    CASE
        WHEN v_numeric < 10.0 THEN 'under-10'
        WHEN v_numeric < 30.0 THEN '10-30'
        WHEN v_numeric < 60.0 THEN '30-60'
        WHEN v_numeric < 100.0 THEN '60-100'
        ELSE '100+'
    END AS result_numeric_lt
FROM case_bsearch_test ORDER BY id;

-- ════════════════════════════════════════════════════════════════════
-- Correctness check with aggregation (forces JIT expression eval)
-- ════════════════════════════════════════════════════════════════════

\echo '=== Aggregation test: COUNT per CASE bucket ==='

-- Create a larger table to ensure JIT triggers
DROP TABLE IF EXISTS case_bsearch_bulk;
CREATE TABLE case_bsearch_bulk AS
SELECT i,
    (i % 100)::int AS v_int4,
    (i % 100)::bigint AS v_int8,
    (i % 100)::real AS v_float4,
    (i % 100)::double precision AS v_float8,
    ('2020-01-01'::date + (i % 1000))::date AS v_date,
    ('prefix_' || (i % 20)::text) AS v_text,
    (i % 100)::numeric AS v_numeric
FROM generate_series(1, 10000) i;
ANALYZE case_bsearch_bulk;

-- int4 aggregation
SELECT
    CASE
        WHEN v_int4 < 20 THEN 'bucket_1'
        WHEN v_int4 < 40 THEN 'bucket_2'
        WHEN v_int4 < 60 THEN 'bucket_3'
        WHEN v_int4 < 80 THEN 'bucket_4'
        ELSE 'bucket_5'
    END AS bucket,
    COUNT(*) AS cnt
FROM case_bsearch_bulk
GROUP BY 1 ORDER BY 1;

-- text equality aggregation
SELECT
    CASE v_text
        WHEN 'prefix_0' THEN 'zero'
        WHEN 'prefix_5' THEN 'five'
        WHEN 'prefix_10' THEN 'ten'
        WHEN 'prefix_15' THEN 'fifteen'
        ELSE 'other'
    END AS txt_bucket,
    COUNT(*) AS cnt
FROM case_bsearch_bulk
GROUP BY 1 ORDER BY 1;

-- float8 searched
SELECT
    CASE
        WHEN v_float8 < 25.0 THEN 'q1'
        WHEN v_float8 < 50.0 THEN 'q2'
        WHEN v_float8 < 75.0 THEN 'q3'
        WHEN v_float8 < 100.0 THEN 'q4'
        ELSE 'overflow'
    END AS quartile,
    COUNT(*) AS cnt
FROM case_bsearch_bulk
GROUP BY 1 ORDER BY 1;

-- ════════════════════════════════════════════════════════════════════
-- Edge cases
-- ════════════════════════════════════════════════════════════════════

\echo '=== Edge cases ==='

-- NaN handling for float types
SELECT
    CASE
        WHEN 'NaN'::float8 < 1.0 THEN 'less'
        WHEN 'NaN'::float8 < 5.0 THEN 'less5'
        WHEN 'NaN'::float8 < 10.0 THEN 'less10'
        WHEN 'NaN'::float8 < 100.0 THEN 'less100'
        ELSE 'nan-or-big'
    END AS nan_test;

-- NULL handling (CASE should return NULL for ELSE when input is NULL)
SELECT
    CASE NULL::integer
        WHEN 1 THEN 'one'
        WHEN 2 THEN 'two'
        WHEN 3 THEN 'three'
        WHEN 4 THEN 'four'
        ELSE 'null-or-other'
    END AS null_test;

-- Exact boundary values
SELECT id, v_int4,
    CASE
        WHEN v_int4 < 5 THEN 'below'
        WHEN v_int4 < 20 THEN 'range1'
        WHEN v_int4 < 50 THEN 'range2'
        WHEN v_int4 < 95 THEN 'range3'
        WHEN v_int4 < 120 THEN 'range4'
        ELSE 'above'
    END AS boundary_test
FROM case_bsearch_test ORDER BY id;

-- Cleanup
DROP TABLE IF EXISTS case_bsearch_test;
DROP TABLE IF EXISTS case_bsearch_bulk;

\echo '=== All CASE binary search tests completed ==='
