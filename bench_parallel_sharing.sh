#!/bin/bash
#
# Benchmark: parallel JIT code sharing — sljit vs asmjit
#
# 4 JIT modes × 2 backends = 8 configurations per query
# (JIT-off is backend-independent, so 7 distinct + 1 shared baseline)
# Each configuration runs 7 times, reports median.
# Workers fixed at 4.
#

PSQL="$HOME/PgCypher/pg_install/bin/psql -p 5433 -d postgres -tA"
WORKERS=24
RUNS=7
BACKENDS=(sljit asmjit)
OUTFILE="bench_parallel_sharing_$(date +%Y%m%d_%H%M%S).txt"

# Verify jit_provider is pg_jitter (meta provider)
provider=$($PSQL -c "SHOW jit_provider;" 2>/dev/null)
if [ "$provider" != "pg_jitter" ]; then
    echo "ERROR: jit_provider is '$provider', expected 'pg_jitter'"
    echo "Run: ALTER SYSTEM SET jit_provider = 'pg_jitter'; then restart."
    exit 1
fi

run_median() {
    local jit="$1"
    local backend="$2"
    local mode="$3"
    local query="$4"

    # Warmup run
    $PSQL -c "
        SET jit = $jit;
        SET jit_above_cost = 0;
        SET pg_jitter.backend = '$backend';
        SET pg_jitter.parallel_mode = '$mode';
        SET max_parallel_workers_per_gather = $WORKERS;
        EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, FORMAT TEXT)
        $query;
    " >/dev/null 2>&1

    local times=()
    for ((r=1; r<=RUNS; r++)); do
        result=$($PSQL -c "
            SET jit = $jit;
            SET jit_above_cost = 0;
            SET pg_jitter.backend = '$backend';
            SET pg_jitter.parallel_mode = '$mode';
            SET max_parallel_workers_per_gather = $WORKERS;
            EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, FORMAT TEXT)
            $query;
        " 2>/dev/null | grep "Execution Time" | sed 's/.*: //' | sed 's/ ms//')

        if [ -z "$result" ]; then
            echo "ERR"
            return
        fi
        times+=("$result")
    done

    local sorted=($(printf '%s\n' "${times[@]}" | sort -n))
    local mid=$((RUNS / 2))
    echo "${sorted[$mid]}"
}

run_query_suite() {
    local qname="$1"
    local query="$2"

    echo ""
    echo "=== $qname ==="
    echo "    $query"
    echo ""
    printf "  %-32s  %10s  %10s\n" "Mode" "sljit" "asmjit"
    printf "  %-32s  %10s  %10s\n" "----" "-----" "------"

    # Mode 1: JIT off (backend-independent — run once, show same value)
    local jitoff_ms
    jitoff_ms=$(run_median "off" "sljit" "shared" "$query")
    printf "  %-32s  %7s ms  %7s ms\n" "jit=off" "$jitoff_ms" "$jitoff_ms"

    # Modes 2-4 per backend
    local mode_labels=("per_worker" "leader_only (workers interp)" "shared")
    local mode_gucs=("per_worker" "off" "shared")

    local sljit_results=()
    local asmjit_results=()

    for i in 0 1 2; do
        local guc="${mode_gucs[$i]}"
        sljit_results[$i]=$(run_median "on" "sljit" "$guc" "$query")
        asmjit_results[$i]=$(run_median "on" "asmjit" "$guc" "$query")
    done

    for i in 0 1 2; do
        printf "  %-32s  %7s ms  %7s ms\n" \
            "${mode_labels[$i]}" "${sljit_results[$i]}" "${asmjit_results[$i]}"
    done
}

{
    echo "=================================================================="
    echo "Parallel JIT Sharing Benchmark — $(date)"
    echo "Table: ultra_wide (1M rows, 20 int4 columns)"
    echo "Workers: $WORKERS, Runs per config: $RUNS (median reported)"
    echo "=================================================================="

    run_query_suite "Q1: Simple aggregate" \
        "SELECT SUM(c01) FROM ultra_wide"

    run_query_suite "Q2: Multi-aggregate (5 funcs)" \
        "SELECT COUNT(*), SUM(c01), MIN(c02), MAX(c03), AVG(c04) FROM ultra_wide"

    run_query_suite "Q3: Expression + aggregate" \
        "SELECT SUM(c01+c02+c03+c04+c05) FROM ultra_wide"

    run_query_suite "Q4: Wide aggregate (10 funcs)" \
        "SELECT SUM(c01), AVG(c02), MIN(c03), MAX(c04), COUNT(c05), SUM(c06), AVG(c07), MIN(c08), MAX(c09), COUNT(c10) FROM ultra_wide"

    echo ""
    echo "=================================================================="
    echo "Done — $(date)"
    echo "=================================================================="
} 2>&1 | tee "$OUTFILE"

echo ""
echo "Results saved to: $OUTFILE"
