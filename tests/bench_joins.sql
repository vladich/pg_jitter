\pset pager off
\timing on

-- ============================================================
-- Test 1: Hash Join — single key
-- ============================================================
\echo '==========================================='
\echo '=== Test 1: Hash Join — single key ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 2: Hash Join — composite key
-- ============================================================
\echo '==========================================='
\echo '=== Test 2: Hash Join — composite key ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 3: Merge Join — single key
-- ============================================================
\echo '==========================================='
\echo '=== Test 3: Merge Join — single key ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_hashjoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;

RESET enable_hashjoin;
RESET enable_nestloop;

-- ============================================================
-- Test 4: Nested Loop Join (small right side)
-- ============================================================
\echo '==========================================='
\echo '=== Test 4: Nested Loop Join (indexed) ==='
\echo '==========================================='

-- Create index for NL join
CREATE INDEX IF NOT EXISTS idx_join_right_key1 ON join_right(key1);
ANALYZE join_right;

\echo '--- jit=off ---'
SET jit = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.id <= 10000;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.id <= 10000;

RESET enable_hashjoin;
RESET enable_mergejoin;

-- ============================================================
-- Test 5: Hash Join + Filter expression
-- ============================================================
\echo '==========================================='
\echo '=== Test 5: Hash Join + complex filter ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
WHERE l.val + r.val > 10000 AND l.val * 2 < 15000 AND r.val % 3 = 0;

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 6: Hash Join + Group By + Agg
-- ============================================================
\echo '==========================================='
\echo '=== Test 6: Hash Join + GroupBy + Agg ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val)
FROM join_left l JOIN join_right r ON l.key1 = r.key1
GROUP BY l.key1 % 1000;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val)
FROM join_left l JOIN join_right r ON l.key1 = r.key1
GROUP BY l.key1 % 1000;

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 7: Hash Anti Join (NOT EXISTS)
-- ============================================================
\echo '==========================================='
\echo '=== Test 7: Hash Anti Join (NOT EXISTS) ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*) FROM join_left l WHERE NOT EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1);

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*) FROM join_left l WHERE NOT EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1);

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 8: Hash Semi Join (EXISTS)
-- ============================================================
\echo '==========================================='
\echo '=== Test 8: Hash Semi Join (EXISTS) ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*) FROM join_left l WHERE EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1 AND r.val > 5000);

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*) FROM join_left l WHERE EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1 AND r.val > 5000);

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 9: INTERSECT (multi-column hash setop)
-- ============================================================
\echo '==========================================='
\echo '=== Test 9: INTERSECT ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*) FROM (
  SELECT grp, val1 FROM bench_data WHERE id <= 500000
  INTERSECT
  SELECT grp, val1 FROM bench_data WHERE id > 250000
) t;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*) FROM (
  SELECT grp, val1 FROM bench_data WHERE id <= 500000
  INTERSECT
  SELECT grp, val1 FROM bench_data WHERE id > 250000
) t;

-- ============================================================
-- Test 10: Hash Right Join
-- ============================================================
\echo '==========================================='
\echo '=== Test 10: Hash Right Join ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(COALESCE(l.val, 0))
FROM (SELECT * FROM join_left WHERE id <= 200000) l
RIGHT JOIN join_right r ON l.key1 = r.key1;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(COALESCE(l.val, 0))
FROM (SELECT * FROM join_left WHERE id <= 200000) l
RIGHT JOIN join_right r ON l.key1 = r.key1;

RESET enable_mergejoin;
RESET enable_nestloop;

-- ============================================================
-- Test 11: Hash Full Outer Join
-- ============================================================
\echo '==========================================='
\echo '=== Test 11: Hash Full Outer Join ==='
\echo '==========================================='

\echo '--- jit=off ---'
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(COALESCE(l.val, 0) + COALESCE(r.val, 0))
FROM (SELECT * FROM join_left WHERE id <= 100000) l
FULL OUTER JOIN (SELECT * FROM join_right WHERE id <= 100000) r ON l.key1 = r.key1;

\echo '--- jit=on ---'
SET jit = on;
SET jit_above_cost = 0;
EXPLAIN (ANALYZE, BUFFERS)
SELECT COUNT(*), SUM(COALESCE(l.val, 0) + COALESCE(r.val, 0))
FROM (SELECT * FROM join_left WHERE id <= 100000) l
FULL OUTER JOIN (SELECT * FROM join_right WHERE id <= 100000) r ON l.key1 = r.key1;

RESET enable_mergejoin;
RESET enable_nestloop;

\timing off
\echo '==========================================='
\echo '=== ALL TESTS DONE ==='
\echo '==========================================='
