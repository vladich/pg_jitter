-- bench_parallel_jit.sql — Measure I-cache impact of per-worker JIT
-- Usage: psql -p 5433 -d postgres -f bench_parallel_jit.sql
--
-- Runs each query 3 times at p0, p2, p4 for both parallel_jit=on and off.
-- Extracts "Execution Time" from EXPLAIN ANALYZE output.

\pset pager off
\timing off

-- Ensure JIT is on and forced for all queries
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;

-- Force hash joins
SET enable_mergejoin = off;
SET enable_nestloop = off;

-- ============================================================
-- Helper: warm buffer cache
-- ============================================================
\echo '>>> Warming buffer cache...'
SELECT COUNT(*) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT COUNT(*) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

-- ============================================================
\echo ''
\echo '======================================================='
\echo '=== A: parallel_jit = ON (workers compile JIT)      ==='
\echo '======================================================='
SET pg_jitter.parallel_jit = on;

-- ---- p0 ----
\echo ''
\echo '--- parallel_jit=ON  p0 ---'
SET max_parallel_workers_per_gather = 0;

\echo 'HashJoin_1key p0 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p0 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p0 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo 'HashJoin_2key p0 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p0 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p0 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo 'HashJoin_Filter p0 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p0 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p0 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo 'GroupBy_5Agg p0 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p0 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p0 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

-- ---- p2 ----
\echo ''
\echo '--- parallel_jit=ON  p2 ---'
SET max_parallel_workers_per_gather = 2;

\echo 'HashJoin_1key p2 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p2 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p2 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo 'HashJoin_2key p2 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p2 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p2 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo 'HashJoin_Filter p2 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p2 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p2 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo 'GroupBy_5Agg p2 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p2 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p2 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

-- ---- p4 ----
\echo ''
\echo '--- parallel_jit=ON  p4 ---'
SET max_parallel_workers_per_gather = 4;

\echo 'HashJoin_1key p4 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p4 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p4 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo 'HashJoin_2key p4 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p4 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p4 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo 'HashJoin_Filter p4 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p4 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p4 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo 'GroupBy_5Agg p4 jit_on (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p4 jit_on (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p4 jit_on (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

-- ============================================================
\echo ''
\echo '======================================================='
\echo '=== B: parallel_jit = OFF (workers use interpreter) ==='
\echo '======================================================='
SET pg_jitter.parallel_jit = off;

-- ---- p0 ----
\echo ''
\echo '--- parallel_jit=OFF p0 ---'
SET max_parallel_workers_per_gather = 0;

\echo 'HashJoin_1key p0 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p0 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p0 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo 'HashJoin_2key p0 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p0 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p0 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo 'HashJoin_Filter p0 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p0 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p0 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo 'GroupBy_5Agg p0 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p0 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p0 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

-- ---- p2 ----
\echo ''
\echo '--- parallel_jit=OFF p2 ---'
SET max_parallel_workers_per_gather = 2;

\echo 'HashJoin_1key p2 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p2 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p2 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo 'HashJoin_2key p2 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p2 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p2 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo 'HashJoin_Filter p2 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p2 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p2 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo 'GroupBy_5Agg p2 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p2 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p2 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

-- ---- p4 ----
\echo ''
\echo '--- parallel_jit=OFF p4 ---'
SET max_parallel_workers_per_gather = 4;

\echo 'HashJoin_1key p4 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p4 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
\echo 'HashJoin_1key p4 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo 'HashJoin_2key p4 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p4 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
\echo 'HashJoin_2key p4 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo 'HashJoin_Filter p4 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p4 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;
\echo 'HashJoin_Filter p4 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
  WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo 'GroupBy_5Agg p4 jit_off (run1)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p4 jit_off (run2)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
\echo 'GroupBy_5Agg p4 jit_off (run3)'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val), MIN(r.val)
  FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

RESET enable_mergejoin;
RESET enable_nestloop;

\echo ''
\echo '======================================================='
\echo '=== BENCHMARK COMPLETE ==='
\echo '======================================================='
