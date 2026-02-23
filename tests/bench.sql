-- JIT Benchmark Suite
-- Runs each query 3 times, reports timing via \timing

\timing on

-- ============================================================
-- 1. GROUP BY + 5 aggregates (previously tested)
-- ============================================================
\echo '=== Test 1: GROUP BY + 5 aggregates ==='
\echo '--- jit=off ---'
SET jit = off;
SELECT grp, COUNT(*), SUM(val1), AVG(val2), MIN(val3), MAX(val4) FROM bench_data GROUP BY grp;
SELECT grp, COUNT(*), SUM(val1), AVG(val2), MIN(val3), MAX(val4) FROM bench_data GROUP BY grp;
SELECT grp, COUNT(*), SUM(val1), AVG(val2), MIN(val3), MAX(val4) FROM bench_data GROUP BY grp;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT grp, COUNT(*), SUM(val1), AVG(val2), MIN(val3), MAX(val4) FROM bench_data GROUP BY grp;
SELECT grp, COUNT(*), SUM(val1), AVG(val2), MIN(val3), MAX(val4) FROM bench_data GROUP BY grp;
SELECT grp, COUNT(*), SUM(val1), AVG(val2), MIN(val3), MAX(val4) FROM bench_data GROUP BY grp;

-- ============================================================
-- 2. SUM-only (previously tested)
-- ============================================================
\echo '=== Test 2: SUM only ==='
\echo '--- jit=off ---'
SET jit = off;
SELECT SUM(val1) FROM bench_data;
SELECT SUM(val1) FROM bench_data;
SELECT SUM(val1) FROM bench_data;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT SUM(val1) FROM bench_data;
SELECT SUM(val1) FROM bench_data;
SELECT SUM(val1) FROM bench_data;

-- ============================================================
-- 3. Hash Join — single key, large
-- ============================================================
\echo '=== Test 3: Hash Join single key (1M x 500K) ==='
\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

-- ============================================================
-- 4. Hash Join — composite key (two columns)
-- ============================================================
\echo '=== Test 4: Hash Join composite key (key1, key2) ==='
\echo '--- jit=off ---'
SET jit = off;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

-- ============================================================
-- 5. Hash Join with complex JIT-ed condition
-- ============================================================
\echo '=== Test 5: Hash Join with expression filter ==='
\echo '--- jit=off ---'
SET jit = off;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;

-- ============================================================
-- 6. INTERSECT / EXCEPT (multi-column hash)
-- ============================================================
\echo '=== Test 6: INTERSECT (hash setop) ==='
\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = on;
SET enable_nestloop = on;
SELECT COUNT(*) FROM (SELECT grp, val1 FROM bench_data WHERE id <= 500000 INTERSECT SELECT grp, val1 FROM bench_data WHERE id > 250000) t;
SELECT COUNT(*) FROM (SELECT grp, val1 FROM bench_data WHERE id <= 500000 INTERSECT SELECT grp, val1 FROM bench_data WHERE id > 250000) t;
SELECT COUNT(*) FROM (SELECT grp, val1 FROM bench_data WHERE id <= 500000 INTERSECT SELECT grp, val1 FROM bench_data WHERE id > 250000) t;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT COUNT(*) FROM (SELECT grp, val1 FROM bench_data WHERE id <= 500000 INTERSECT SELECT grp, val1 FROM bench_data WHERE id > 250000) t;
SELECT COUNT(*) FROM (SELECT grp, val1 FROM bench_data WHERE id <= 500000 INTERSECT SELECT grp, val1 FROM bench_data WHERE id > 250000) t;
SELECT COUNT(*) FROM (SELECT grp, val1 FROM bench_data WHERE id <= 500000 INTERSECT SELECT grp, val1 FROM bench_data WHERE id > 250000) t;

-- ============================================================
-- 7. Hash aggregate with many groups + expression
-- ============================================================
\echo '=== Test 7: Hash Agg many groups + expression ==='
\echo '--- jit=off ---'
SET jit = off;
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1;
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1;
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1;
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1;
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1;

-- ============================================================
-- 8. Hash Join + Group By + Agg (combined)
-- ============================================================
\echo '=== Test 8: Hash Join + GroupBy + Agg ==='
\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000;

\timing off
\echo '=== DONE ==='
