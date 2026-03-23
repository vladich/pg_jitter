#!/bin/bash
# bench_parallel_modes.sh — Parallel execution benchmark across sharing modes
#
# Compares 3 parallel modes (off, per_worker, shared) at varying
# parallelism levels (0, 2, 4 workers) for the sljit backend.
# Also runs the parallel shared code correctness test.
#
# Usage:
#   ./tests/bench_parallel_modes.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config (default: $PG_CONFIG or pg_config)
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: $PGDB or postgres)
#   --runs N           Timed runs per query (default: 3)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-postgres}"
NRUNS=3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --runs)      NRUNS="$2";     shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PSQL="$PGBIN/psql -p $PGPORT -d $PGDB -X -t -A"

echo "=== Parallel Execution Benchmark ==="
echo "  Port: $PGPORT  |  Runs: $NRUNS"
echo ""

# Ensure tables exist
TABLE_CHECK=$($PSQL -c "SELECT count(*) FROM pg_class WHERE relname = 'join_left'" 2>/dev/null || echo 0)
if [ "$TABLE_CHECK" != "1" ]; then
    echo "ERROR: benchmark tables not found. Run bench_comprehensive.sh first."
    exit 1
fi

# Queries
declare -a LABELS QUERIES
nq=0
add_q() { LABELS[$nq]="$1"; QUERIES[$nq]="$2"; nq=$((nq + 1)); }

add_q "SeqScan_SUM"       "SELECT SUM(val) FROM join_left"
add_q "SeqScan_Filter"    "SELECT COUNT(*) FROM join_left WHERE val > 5000 AND key1 < 50000"
add_q "HashJoin_1key"     "SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1"
add_q "HashJoin_Filter"   "SELECT COUNT(*) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000"
add_q "GroupBy_5Agg"      "SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp"
add_q "IN_int_20"         "SELECT COUNT(*) FROM bench_data WHERE val1+0 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"
add_q "IN_text_5"         "SELECT COUNT(*) FROM text_data WHERE grp_text IN ('prefix_1','prefix_10','prefix_20','prefix_50','prefix_99')"
add_q "REGEX_filter"      "SELECT COUNT(*) FROM text_data WHERE grp_text ~ '^prefix_[1-3][0-9]$'"
add_q "CASE_20way"        "SELECT SUM(CASE WHEN val1<500 THEN 1 WHEN val1<1000 THEN 2 WHEN val1<1500 THEN 3 WHEN val1<2000 THEN 4 WHEN val1<2500 THEN 5 WHEN val1<3000 THEN 6 WHEN val1<3500 THEN 7 WHEN val1<4000 THEN 8 WHEN val1<4500 THEN 9 WHEN val1<5000 THEN 10 WHEN val1<5500 THEN 11 WHEN val1<6000 THEN 12 WHEN val1<6500 THEN 13 WHEN val1<7000 THEN 14 WHEN val1<7500 THEN 15 WHEN val1<8000 THEN 16 WHEN val1<8500 THEN 17 WHEN val1<9000 THEN 18 WHEN val1<9500 THEN 19 ELSE 20 END) FROM bench_data"

# Prewarm
echo "  Prewarming..."
for qi in $(seq 0 $((nq - 1))); do
    $PSQL -c "SET jit = off; ${QUERIES[$qi]}" > /dev/null 2>&1
done
echo ""

# Run a query N times, return median execution time (ms)
run_median() {
    local query="$1"
    local settings="$2"
    local times=()

    for r in $(seq 1 $NRUNS); do
        local t=$($PSQL -c "
            $settings
            EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
        " 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//')
        [ -n "$t" ] && times+=("$t")
    done

    if [ ${#times[@]} -eq 0 ]; then echo "N/A"; return; fi
    IFS=$'\n' sorted=($(printf '%s\n' "${times[@]}" | sort -g)); unset IFS
    echo "${sorted[$(( ${#sorted[@]} / 2 ))]}"
}

OUTFILE="$SCRIPT_DIR/bench_parallel_modes_$(date +%Y%m%d_%H%M%S).csv"
echo "query,mode,workers,exec_ms" > "$OUTFILE"

# Modes × workers combinations
MODES="off per_worker shared"
WORKERS="0 2 4"

# Print header
printf "%-18s" "Query"
for mode in $MODES; do
    for pw in $WORKERS; do
        printf "%12s" "${mode:0:5}_p${pw}"
    done
done
printf "\n"
printf "%-18s" "$(printf -- '-%.0s' {1..18})"
for mode in $MODES; do
    for pw in $WORKERS; do
        printf "%12s" "--------"
    done
done
printf "\n"

# Benchmark loop
for qi in $(seq 0 $((nq - 1))); do
    label="${LABELS[$qi]}"
    query="${QUERIES[$qi]}"
    printf "%-18s" "$label"

    for mode in $MODES; do
        for pw in $WORKERS; do
            settings="
                SET jit = on;
                SET jit_above_cost = 0;
                SET jit_inline_above_cost = 0;
                SET jit_optimize_above_cost = 0;
                SET enable_mergejoin = off;
                SET enable_nestloop = off;
                SET pg_jitter.backend = 'sljit';
                SET pg_jitter.parallel_mode = '$mode';
                SET max_parallel_workers_per_gather = $pw;
                SET parallel_tuple_cost = 0;
                SET parallel_setup_cost = 0;
                SET min_parallel_table_scan_size = 0;
            "
            t=$(run_median "$query" "$settings")
            printf "%12s" "$t"
            echo "$label,$mode,$pw,$t" >> "$OUTFILE"
        done
    done
    printf "\n"
done

echo ""
echo "All times in ms (median of $NRUNS). Lower is better."
echo "CSV saved to: $OUTFILE"
echo ""

# ============================================================
# Correctness test
# ============================================================
echo "=== Running Parallel Shared Code Correctness Test ==="
echo ""
CORR_OUTPUT=$($PGBIN/psql -p $PGPORT -d $PGDB -X -f "$SCRIPT_DIR/test_parallel_shared.sql" 2>&1)
PASS_COUNT=$(echo "$CORR_OUTPUT" | grep -c "PASS \[" || true)
FAIL_COUNT=$(echo "$CORR_OUTPUT" | grep -c "FAIL \[" || true)

echo "$CORR_OUTPUT" | grep -E "PASS \[|FAIL \[|=== ALL" || true
echo ""

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "CORRECTNESS: $PASS_COUNT passed, $FAIL_COUNT FAILED"
    exit 1
else
    echo "CORRECTNESS: $PASS_COUNT passed, 0 failed"
fi

echo ""
echo "=== Done ==="
