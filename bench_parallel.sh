#!/bin/bash
#
# Parallel JIT benchmark — 4 JIT modes × 3 providers = 12 configs
#
# Modes:
#   1. jit=off                                    (baseline, everyone interprets)
#   2. jit=on, parallel_mode='per_worker'         (each worker compiles independently)
#   3. jit=on, parallel_mode='off'                (leader JITs, workers interpret)
#   4. jit=on, parallel_mode='shared'             (leader JITs, workers reuse via DSM)
#

set -euo pipefail

PG_INSTALL="$HOME/PgCypher/pg_install"
PG_DATA="$HOME/PgCypher/pg_data"
PSQL="$PG_INSTALL/bin/psql -p 5433 -d postgres -tAX"
PGCTL="$PG_INSTALL/bin/pg_ctl"

RUNS=7
WORKERS=4

switch_provider() {
    local prov=$1
    $PSQL -c "ALTER SYSTEM SET jit_provider = '$prov';" >/dev/null 2>&1
    $PGCTL -D "$PG_DATA" restart -l /dev/null >/dev/null 2>&1
    sleep 1
}

run_once() {
    local settings="$1"
    local sql="$2"
    $PSQL -c "
        $settings
        EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON, FORMAT TEXT)
        $sql;
    " 2>/dev/null | grep "Execution Time" | sed 's/.*: //' | sed 's/ ms//' | tr -d ' '
}

run_bench() {
    local settings="$1"
    local sql="$2"
    local times=""
    local count=0

    for r in $(seq 1 $RUNS); do
        local ms
        ms=$(run_once "$settings" "$sql")
        if [ -n "$ms" ]; then
            times="$times $ms"
            count=$((count + 1))
        fi
    done

    if [ $count -lt 3 ]; then
        echo "ERR"
        return
    fi

    # Sort, drop min+max, average the rest
    sorted=$(echo $times | tr ' ' '\n' | sort -g)
    n=$(echo "$sorted" | wc -l | tr -d ' ')
    middle=$(echo "$sorted" | tail -n $((n - 1)) | head -n $((n - 2)))
    sum=0
    cnt=0
    for v in $middle; do
        sum=$(echo "$sum + $v" | bc -l)
        cnt=$((cnt + 1))
    done
    if [ $cnt -gt 0 ]; then
        printf "%.1f" $(echo "$sum / $cnt" | bc -l)
    else
        echo "ERR"
    fi
}

jit_on="SET jit = on; SET jit_above_cost = 0; SET jit_inline_above_cost = 0; SET jit_optimize_above_cost = 0; SET max_parallel_workers_per_gather = $WORKERS;"
jit_off="SET jit = off; SET max_parallel_workers_per_gather = $WORKERS;"

echo "=================================================================="
echo "Parallel JIT Benchmark — $(date)"
echo "Workers: $WORKERS   Runs: $RUNS (trimmed mean: drop min+max)"
echo "=================================================================="
echo ""

# --- Run benchmark per query ---

run_query_all_modes() {
    local qname="$1"
    local sql="$2"

    echo "=== $qname ==="
    printf "%-18s  %10s  %10s  %10s\n" "Mode" "sljit" "asmjit" "mir"
    printf "%-18s  %10s  %10s  %10s\n" "----" "-----" "------" "---"

    # Mode 1: JIT OFF
    local r_sljit r_asmjit r_mir

    switch_provider "pg_jitter_sljit"
    r_sljit=$(run_bench "$jit_off" "$sql")
    switch_provider "pg_jitter_asmjit"
    r_asmjit=$(run_bench "$jit_off" "$sql")
    switch_provider "pg_jitter_mir"
    r_mir=$(run_bench "$jit_off" "$sql")
    printf "%-18s  %9sms  %9sms  %9sms\n" "JIT OFF" "$r_sljit" "$r_asmjit" "$r_mir"

    # Mode 2: per_worker
    switch_provider "pg_jitter_sljit"
    r_sljit=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'per_worker';" "$sql")
    switch_provider "pg_jitter_asmjit"
    r_asmjit=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'per_worker';" "$sql")
    switch_provider "pg_jitter_mir"
    r_mir=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'per_worker';" "$sql")
    printf "%-18s  %9sms  %9sms  %9sms\n" "per_worker" "$r_sljit" "$r_asmjit" "$r_mir"

    # Mode 3: workers interpret (leader-only JIT, workers use PG interpreter)
    switch_provider "pg_jitter_sljit"
    r_sljit=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'off';" "$sql")
    switch_provider "pg_jitter_asmjit"
    r_asmjit=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'off';" "$sql")
    switch_provider "pg_jitter_mir"
    r_mir=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'off';" "$sql")
    printf "%-18s  %9sms  %9sms  %9sms\n" "workers interp" "$r_sljit" "$r_asmjit" "$r_mir"

    # Mode 4: shared (leader JITs, workers reuse via DSM)
    switch_provider "pg_jitter_sljit"
    r_sljit=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'shared';" "$sql")
    switch_provider "pg_jitter_asmjit"
    r_asmjit=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'shared';" "$sql")
    switch_provider "pg_jitter_mir"
    r_mir=$(run_bench "$jit_on SET pg_jitter.parallel_mode = 'shared';" "$sql")
    printf "%-18s  %9sms  %9sms  %9sms\n" "shared (DSM)" "$r_sljit" "$r_asmjit" "$r_mir"

    echo ""
}

run_query_all_modes "Q1: Agg 5 funcs (ultra_wide)" \
    "SELECT COUNT(*), SUM(c01), MIN(c02), MAX(c03), AVG(c04) FROM ultra_wide"

run_query_all_modes "Q2: Agg 10 funcs (ultra_wide)" \
    "SELECT SUM(c01), AVG(c02), MIN(c03), MAX(c04), COUNT(c05), SUM(c06), AVG(c07), MIN(c08), MAX(c09), COUNT(c10) FROM ultra_wide"

run_query_all_modes "Q3: Expr + agg (ultra_wide)" \
    "SELECT SUM(c01+c02+c03+c04+c05), SUM(c06*c07-c08+c09*c10) FROM ultra_wide"

run_query_all_modes "Q4: Filter IN (bench_data)" \
    "SELECT SUM(val1), SUM(val2) FROM bench_data WHERE grp IN (1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39)"

run_query_all_modes "Q5: HashJoin + Agg (bench_data)" \
    "SELECT a.grp, SUM(a.val1 + b.val2), COUNT(*) FROM bench_data a JOIN bench_data b ON a.grp = b.grp AND a.id = b.id WHERE a.val1 > 0 GROUP BY a.grp"

echo "=================================================================="
echo "Done — $(date)"
