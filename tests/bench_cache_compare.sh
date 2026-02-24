#!/bin/bash
# bench_cache_compare.sh — Compare sljit with code cache ON vs OFF
# Uses PREPARE/EXECUTE to show caching benefits on repeated execution
set -e

PG_INSTALL="$HOME/PgCypher/pg_install"
PG_DATA="$HOME/PgCypher/pg_data"
PGBIN="$PG_INSTALL/bin"
PGCTL="$PGBIN/pg_ctl"
LOGFILE="$PG_DATA/logfile"
OUTFILE="/Users/vladimir/PgCypher/pg_jitter/tests/bench_cache_$(date +%Y%m%d_%H%M%S).txt"
NRUNS=3

restart_pg() {
    "$PGCTL" -D "$PG_DATA" stop -m fast 2>/dev/null || true
    sleep 1
    "$PGCTL" -D "$PG_DATA" start -l "$LOGFILE" -w 2>/dev/null
    sleep 1
}

get_exec_time() {
    local query="$1"
    local cache_on="$2"
    "$PGBIN/psql" -p 5433 -d regression -X -t -A -c "
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET pg_jitter.code_cache = $cache_on;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

median3() {
    echo -e "$1\n$2\n$3" | sort -g | sed -n '2p'
}

echo "================================================================="
echo "  sljit Code Cache Benchmark — ON vs OFF — $(date)"
echo "  PostgreSQL 18.2, macOS ARM64"
echo "  $NRUNS runs per query (+ 1 warmup), median time in ms"
echo "================================================================="
echo ""

restart_pg

# Warmup buffer cache
echo "Warming up buffer cache..."
"$PGBIN/psql" -p 5433 -d regression -X -q -c "
SET enable_mergejoin = off; SET enable_nestloop = off;
SELECT SUM(val1) FROM bench_data;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2;
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000;
SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp;
SELECT key1 FROM join_left INTERSECT SELECT key1 FROM join_right;
SELECT key1 FROM join_left EXCEPT SELECT key1 FROM join_right;
SELECT COUNT(DISTINCT key1) FROM join_left;
SELECT SUM(CASE WHEN val > 5000 THEN val ELSE 0 END) FROM join_left;
SELECT COUNT(*) FROM join_left WHERE key1 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20);
SELECT COALESCE(NULLIF(val, 0), -1) FROM join_left LIMIT 1;
" > /dev/null 2>&1
echo "Warmup done."
echo ""

# Header
hdr=$(printf "%-28s%12s%12s%10s" "Query" "cache_off" "cache_on" "speedup")
sep=$(printf "%-28s%12s%12s%10s" "" "--------" "--------" "-------")
echo "$hdr" | tee "$OUTFILE"
echo "$sep" | tee -a "$OUTFILE"

run_one_bench() {
    local label="$1"
    local query="$2"
    printf "  %-26s" "$label"
    printf "%-28s" "$label" >> "$OUTFILE"

    # --- cache OFF ---
    get_exec_time "$query" "off" > /dev/null 2>&1
    t1=$(get_exec_time "$query" "off")
    t2=$(get_exec_time "$query" "off")
    t3=$(get_exec_time "$query" "off")
    m_off=$(median3 "$t1" "$t2" "$t3")
    printf "%12s" "$m_off" | tee -a "$OUTFILE"

    # --- cache ON ---
    get_exec_time "$query" "on" > /dev/null 2>&1
    t1=$(get_exec_time "$query" "on")
    t2=$(get_exec_time "$query" "on")
    t3=$(get_exec_time "$query" "on")
    m_on=$(median3 "$t1" "$t2" "$t3")
    printf "%12s" "$m_on" | tee -a "$OUTFILE"

    # Speedup
    if [ -n "$m_off" ] && [ -n "$m_on" ]; then
        spd=$(echo "scale=2; $m_off / $m_on" | bc 2>/dev/null || echo "N/A")
        printf "%10sx" "$spd" | tee -a "$OUTFILE"
    fi

    echo "" | tee -a "$OUTFILE"
}

# ---- Aggregation queries ----
echo "--- Aggregation ---" | tee -a "$OUTFILE"

run_one_bench "SUM_only" \
    "SELECT SUM(val1) FROM bench_data"

run_one_bench "COUNT_star" \
    "SELECT COUNT(*) FROM bench_data"

run_one_bench "GroupBy_5agg" \
    "SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp"

run_one_bench "COUNT_DISTINCT" \
    "SELECT COUNT(DISTINCT key1) FROM join_left"

# ---- Join queries ----
echo "--- Joins ---" | tee -a "$OUTFILE"

run_one_bench "HashJoin_single" \
    "SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1"

run_one_bench "HashJoin_composite" \
    "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2"

run_one_bench "HashJoin_filter" \
    "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000"

run_one_bench "HashJoin_multi_expr" \
    "SELECT COUNT(*), SUM(l.val * 2 + r.val), AVG(l.key1 + r.key1) FROM join_left l JOIN join_right r ON l.key1 = r.key1"

# ---- Set operations ----
echo "--- Set Operations ---" | tee -a "$OUTFILE"

run_one_bench "INTERSECT" \
    "SELECT key1 FROM join_left INTERSECT SELECT key1 FROM join_right"

run_one_bench "EXCEPT" \
    "SELECT key1 FROM join_left EXCEPT SELECT key1 FROM join_right"

# ---- Expressions & Filters ----
echo "--- Expressions ---" | tee -a "$OUTFILE"

run_one_bench "CASE_simple" \
    "SELECT SUM(CASE WHEN val > 5000 THEN val ELSE 0 END) FROM join_left"

run_one_bench "CASE_searched" \
    "SELECT SUM(CASE WHEN val < 1000 THEN 1 WHEN val < 5000 THEN 2 WHEN val < 9000 THEN 3 ELSE 4 END) FROM join_left"

run_one_bench "COALESCE_NULLIF" \
    "SELECT SUM(COALESCE(NULLIF(val, 0), -1)) FROM join_left"

run_one_bench "Bool_AND_OR" \
    "SELECT COUNT(*) FROM join_left WHERE (val > 1000 AND val < 9000) OR (key1 > 100 AND key2 < 400)"

run_one_bench "Arith_complex" \
    "SELECT SUM(val * val + key1 * 3 - key2) FROM join_left"

run_one_bench "IN_list_20" \
    "SELECT COUNT(*) FROM join_left WHERE key1 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"

# ---- Subqueries ----
echo "--- Subqueries ---" | tee -a "$OUTFILE"

run_one_bench "EXISTS_subq" \
    "SELECT COUNT(*) FROM join_left l WHERE EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1)"

run_one_bench "Scalar_subq" \
    "SELECT SUM(val) FROM join_left WHERE key1 < (SELECT AVG(key1) FROM join_right)"

echo "" | tee -a "$OUTFILE"
echo "All times in ms. Lower is better. Speedup = cache_off / cache_on." | tee -a "$OUTFILE"
echo "Results saved to: $OUTFILE"
