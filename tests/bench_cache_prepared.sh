#!/bin/bash
# bench_cache_prepared.sh — Measure cache benefit on prepared statements
# Uses clock_timestamp() to measure wall time of N EXECUTE cycles
set -e

PG_INSTALL="$HOME/PgCypher/pg_install"
PGBIN="$PG_INSTALL/bin"
PGCTL="$PGBIN/pg_ctl"
PG_DATA="$HOME/PgCypher/pg_data"
LOGFILE="$PG_DATA/logfile"
OUTFILE="/Users/vladimir/PgCypher/pg_jitter/tests/bench_prepared_$(date +%Y%m%d_%H%M%S).txt"

NEXEC=50

restart_pg() {
    "$PGCTL" -D "$PG_DATA" stop -m fast 2>/dev/null || true
    sleep 1
    "$PGCTL" -D "$PG_DATA" start -l "$LOGFILE" -w 2>/dev/null
    sleep 1
}

# Measure total wall time for N executes of a prepared query
# Returns milliseconds
measure_prepared() {
    local prepare_sql="$1"
    local execute_sql="$2"
    local cache_setting="$3"
    local n="$NEXEC"

    # Build a DO block that loops N times
    "$PGBIN/psql" -p 5433 -d regression -X -t -A -c "
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET max_parallel_workers_per_gather = 0;
SET pg_jitter.code_cache = $cache_setting;
$prepare_sql
-- warmup
$execute_sql
DO \$\$
DECLARE
    t0 timestamptz;
    t1 timestamptz;
    dummy record;
BEGIN
    t0 := clock_timestamp();
    FOR i IN 1..$n LOOP
        EXECUTE 'EXECUTE the_query';
    END LOOP;
    t1 := clock_timestamp();
    RAISE NOTICE 'elapsed_ms: %', extract(epoch from (t1 - t0)) * 1000;
END
\$\$;
DEALLOCATE the_query;
" 2>&1 | grep 'elapsed_ms' | sed 's/.*elapsed_ms: //'
}

# Single-shot EXPLAIN ANALYZE for comparison
measure_single() {
    local query="$1"
    local cache_setting="$2"
    "$PGBIN/psql" -p 5433 -d regression -X -t -A -c "
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET max_parallel_workers_per_gather = 0;
SET pg_jitter.code_cache = $cache_setting;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

median3() {
    echo -e "$1\n$2\n$3" | sort -g | sed -n '2p'
}

echo "================================================================="
echo "  sljit Code Cache Benchmark — $(date)"
echo "  PostgreSQL 18.2, macOS ARM64, no parallel"
echo "  Part 1: Single execution (median of 3)"
echo "  Part 2: $NEXEC prepared-statement EXECUTEs"
echo "================================================================="
echo ""

restart_pg

# Warmup
echo "Warming up..."
"$PGBIN/psql" -p 5433 -d regression -X -q -c "
SET enable_mergejoin = off; SET enable_nestloop = off; SET max_parallel_workers_per_gather = 0;
SELECT SUM(val1) FROM bench_data;
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1;
SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp;
SELECT key1 FROM join_left INTERSECT SELECT key1 FROM join_right;
" > /dev/null 2>&1
echo "Done."
echo ""

# Define queries as arrays
labels=(
    "SUM_only"
    "COUNT_star"
    "GroupBy_5agg"
    "HashJoin_single"
    "HashJoin_composite"
    "HashJoin_filter"
    "HashJoin_multi_expr"
    "CASE_simple"
    "CASE_searched"
    "COALESCE_NULLIF"
    "Bool_AND_OR"
    "Arith_expr"
    "IN_list_20"
    "INTERSECT"
    "EXCEPT"
    "COUNT_DISTINCT"
    "EXISTS_subq"
    "Scalar_subq"
)

queries=(
    "SELECT SUM(val1) FROM bench_data"
    "SELECT COUNT(*) FROM bench_data"
    "SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp"
    "SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1"
    "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2"
    "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000"
    "SELECT COUNT(*), SUM(l.val * 2 + r.val), AVG(l.key1 + r.key1) FROM join_left l JOIN join_right r ON l.key1 = r.key1"
    "SELECT SUM(CASE WHEN val > 5000 THEN val ELSE 0 END) FROM join_left"
    "SELECT SUM(CASE WHEN val < 1000 THEN 1 WHEN val < 5000 THEN 2 WHEN val < 9000 THEN 3 ELSE 4 END) FROM join_left"
    "SELECT SUM(COALESCE(NULLIF(val, 0), -1)) FROM join_left"
    "SELECT COUNT(*) FROM join_left WHERE (val > 1000 AND val < 9000) OR (key1 > 100 AND key2 < 400)"
    "SELECT SUM(val + key1 * 3 - key2) FROM join_left"
    "SELECT COUNT(*) FROM join_left WHERE key1 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"
    "SELECT key1 FROM join_left INTERSECT SELECT key1 FROM join_right"
    "SELECT key1 FROM join_left EXCEPT SELECT key1 FROM join_right"
    "SELECT COUNT(DISTINCT key1) FROM join_left"
    "SELECT COUNT(*) FROM join_left l WHERE EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1)"
    "SELECT SUM(val) FROM join_left WHERE key1 < 250"
)

# ==========================================
# PART 1: Single execution
# ==========================================
echo "=== Part 1: Single Execution (ms, median of 3) ===" | tee "$OUTFILE"
hdr=$(printf "%-24s%12s%12s%10s" "Query" "cache_off" "cache_on" "ratio")
sep=$(printf "%-24s%12s%12s%10s" "" "--------" "--------" "------")
echo "$hdr" | tee -a "$OUTFILE"
echo "$sep" | tee -a "$OUTFILE"

for i in "${!labels[@]}"; do
    label="${labels[$i]}"
    query="${queries[$i]}"
    printf "  %-22s" "$label"
    printf "%-24s" "$label" >> "$OUTFILE"

    # cache OFF
    measure_single "$query" "off" > /dev/null 2>&1
    t1=$(measure_single "$query" "off")
    t2=$(measure_single "$query" "off")
    t3=$(measure_single "$query" "off")
    m_off=$(median3 "$t1" "$t2" "$t3")
    printf "%12s" "$m_off" | tee -a "$OUTFILE"

    # cache ON
    measure_single "$query" "on" > /dev/null 2>&1
    t1=$(measure_single "$query" "on")
    t2=$(measure_single "$query" "on")
    t3=$(measure_single "$query" "on")
    m_on=$(median3 "$t1" "$t2" "$t3")
    printf "%12s" "$m_on" | tee -a "$OUTFILE"

    if [ -n "$m_off" ] && [ -n "$m_on" ]; then
        spd=$(echo "scale=2; $m_off / $m_on" | bc 2>/dev/null || echo "N/A")
        printf "%10sx" "$spd" | tee -a "$OUTFILE"
    fi
    echo "" | tee -a "$OUTFILE"
done

echo "" | tee -a "$OUTFILE"

# ==========================================
# PART 2: Prepared statement repeated execution
# ==========================================
echo "=== Part 2: $NEXEC Prepared EXECUTEs (total ms) ===" | tee -a "$OUTFILE"
hdr=$(printf "%-24s%12s%12s%10s" "Query" "cache_off" "cache_on" "ratio")
sep=$(printf "%-24s%12s%12s%10s" "" "--------" "--------" "------")
echo "$hdr" | tee -a "$OUTFILE"
echo "$sep" | tee -a "$OUTFILE"

for i in "${!labels[@]}"; do
    label="${labels[$i]}"
    query="${queries[$i]}"
    printf "  %-22s" "$label"
    printf "%-24s" "$label" >> "$OUTFILE"

    # cache OFF
    m_off=$(measure_prepared "PREPARE the_query AS $query;" "EXECUTE the_query;" "off")
    printf "%12s" "${m_off:0:10}" | tee -a "$OUTFILE"

    # cache ON
    m_on=$(measure_prepared "PREPARE the_query AS $query;" "EXECUTE the_query;" "on")
    printf "%12s" "${m_on:0:10}" | tee -a "$OUTFILE"

    if [ -n "$m_off" ] && [ -n "$m_on" ]; then
        spd=$(echo "scale=2; $m_off / $m_on" | bc 2>/dev/null || echo "N/A")
        printf "%10sx" "$spd" | tee -a "$OUTFILE"
    fi
    echo "" | tee -a "$OUTFILE"
done

echo "" | tee -a "$OUTFILE"
echo "Lower is better. Ratio = cache_off / cache_on (>1 = cache helps)." | tee -a "$OUTFILE"
echo "Results saved to: $OUTFILE"
