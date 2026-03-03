#!/bin/bash
# bench_blob_compare.sh — Run benchmark for current build across all 3 backends
# Usage: ./bench_blob_compare.sh <label> [outdir]
set -e

LABEL="${1:?Usage: $0 <label> [outdir]}"
OUTDIR="${2:-/tmp/blob_bench}"
mkdir -p "$OUTDIR"

PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBIN="$("$PG_CONFIG" --bindir)"
PGDATA="${PGDATA:-$("$PGBIN/psql" -p "${PGPORT:-5432}" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null)}"
PGPORT="${PGPORT:-5432}"
NRUNS=3

psql_cmd() {
    "$PGBIN/psql" -p "$PGPORT" -d postgres -X -t -A "$@"
}

get_exec_time() {
    local query="$1" backend="$2"
    psql_cmd -c "
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET max_parallel_workers_per_gather = 0;
SET pg_jitter.backend = '$backend';
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*: //' | sed 's/ ms//'
}

get_exec_time_nojit() {
    local query="$1"
    psql_cmd -c "
SET jit = off;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*: //' | sed 's/ ms//'
}

median3() {
    printf '%s\n' "$@" | sort -g | head -2 | tail -1
}

run_query() {
    local label="$1" query="$2" backend="$3"
    local func="get_exec_time"
    [ "$backend" = "nojit" ] && func="get_exec_time_nojit"

    # Warmup
    if [ "$backend" = "nojit" ]; then
        $func "$query" >/dev/null 2>&1
    else
        $func "$query" "$backend" >/dev/null 2>&1
    fi

    local t1 t2 t3
    if [ "$backend" = "nojit" ]; then
        t1=$($func "$query")
        t2=$($func "$query")
        t3=$($func "$query")
    else
        t1=$($func "$query" "$backend")
        t2=$($func "$query" "$backend")
        t3=$($func "$query" "$backend")
    fi
    median3 "$t1" "$t2" "$t3"
}

# Ensure pg_jitter meta-provider is loaded and detect backends
ensure_pg() {
    "$PGBIN/pg_isready" -p "$PGPORT" -q 2>/dev/null || {
        "$PGBIN/pg_ctl" -D "$PGDATA" start -l "$PGDATA/logfile" -w 2>/dev/null
        sleep 1
    }
    # Verify pg_jitter is loaded
    local prov=$(psql_cmd -c "SHOW jit_provider;" 2>/dev/null)
    if [ "$prov" != "pg_jitter" ]; then
        psql_cmd -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" 2>/dev/null
        "$PGBIN/pg_ctl" -D "$PGDATA" restart -l "$PGDATA/logfile" -w 2>/dev/null
        sleep 1
    fi
    # Init the provider
    psql_cmd -c "SET jit_above_cost = 0; SELECT 1;" >/dev/null 2>&1 || true
}

ensure_pg

# Queries - representative mix covering deform, expressions, aggregation, joins
LABELS=(
    "SUM_int"
    "COUNT_star"
    "GroupBy_5agg"
    "GroupBy_100K"
    "HashJoin_single"
    "HashJoin_filter"
    "CASE_4way"
    "Arith_expr"
    "IN_list_20"
    "Wide_10col"
    "Wide_20col"
    "Wide_grpby"
    "Date_extract"
    "Timestamp_diff"
    "Text_EQ_filter"
    "Text_length"
    "Numeric_agg"
    "Numeric_arith"
)
QUERIES=(
    "SELECT SUM(val1) FROM bench_data"
    "SELECT COUNT(*) FROM bench_data"
    "SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp"
    "SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1"
    "SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1"
    "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000"
    "SELECT SUM(CASE WHEN val < 1000 THEN 1 WHEN val < 5000 THEN 2 WHEN val < 9000 THEN 3 ELSE 4 END) FROM join_left"
    "SELECT SUM(val + key1 * 3 - key2) FROM join_left"
    "SELECT COUNT(*) FROM bench_data WHERE grp IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"
    "SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10) FROM ultra_wide"
    "SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ultra_wide"
    "SELECT grp, SUM(c01*c02 + c03 - c04), AVG(c10+c20) FROM ultra_wide GROUP BY grp"
    "SELECT EXTRACT(year FROM d) AS yr, COUNT(*), SUM(val) FROM date_data GROUP BY yr"
    "SELECT SUM(EXTRACT(epoch FROM ts - '2000-01-01'::timestamp)) FROM date_data WHERE id <= 500000"
    "SELECT COUNT(*) FROM text_data WHERE grp_text = 'prefix_42'"
    "SELECT SUM(length(varlen_text) + length(word)) FROM text_data"
    "SELECT grp_num, SUM(val1), AVG(val2) FROM numeric_data GROUP BY grp_num"
    "SELECT SUM(val1 * val2 + val1 - val2) FROM numeric_data"
)

BACKENDS=(nojit sljit asmjit mir)
NQ=${#LABELS[@]}

echo "=== Benchmark: $LABEL ==="
echo "Queries: $NQ × ${#BACKENDS[@]} backends × $NRUNS runs"
echo ""

# Warmup buffer cache
echo -n "Warming cache..."
psql_cmd -c "
SET max_parallel_workers_per_gather = 0;
SELECT COUNT(*) FROM bench_data; SELECT COUNT(*) FROM join_left;
SELECT COUNT(*) FROM join_right; SELECT COUNT(*) FROM date_data;
SELECT COUNT(*) FROM text_data; SELECT COUNT(*) FROM numeric_data;
SELECT COUNT(*) FROM ultra_wide;
" > /dev/null 2>&1
echo " done."

OUTFILE="$OUTDIR/${LABEL}.csv"
echo "query,nojit,sljit,asmjit,mir" > "$OUTFILE"

for qi in $(seq 0 $((NQ - 1))); do
    qlabel="${LABELS[$qi]}"
    query="${QUERIES[$qi]}"
    printf "  %-20s" "$qlabel"

    line="$qlabel"
    for backend in "${BACKENDS[@]}"; do
        t=$(run_query "$qlabel" "$query" "$backend")
        printf "%10s" "${t:-ERR}"
        line="$line,$t"
    done
    echo "$line" >> "$OUTFILE"
    echo ""
done

echo ""
echo "Results saved to: $OUTFILE"
