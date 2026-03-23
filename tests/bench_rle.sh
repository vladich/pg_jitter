#!/bin/bash
# bench_rle.sh — Benchmark RLE optimization: group-typed vs random-typed tables
#
# Tests deform performance on:
#   rle_grp_NNN: all-int4 columns (long RLE runs, ideal case)
#   rle_rnd_NNN: cycling 6 types (no RLE runs, worst case)
#
# Usage:
#   ./tests/bench_rle.sh [--pg-config /path] [--port 5018] [--runs 5] [--backends "interp sljit"]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5018}"
PGDB="${PGDB:-postgres}"
NRUNS=5
REQUESTED_BACKENDS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --runs)      NRUNS="$2";     shift 2;;
        --backends)  REQUESTED_BACKENDS="$2"; shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
OUTFILE="$SCRIPT_DIR/bench_rle_results_$(date +%Y%m%d_%H%M%S).txt"

psql_cmd() {
    "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"
}

# Detect available backends
BACKENDS=""
if [ -n "$REQUESTED_BACKENDS" ]; then
    BACKENDS="$REQUESTED_BACKENDS"
else
    BACKENDS="interp sljit"
    [ -f "$PKGLIBDIR/pg_jitter_asmjit.dylib" ] && BACKENDS="$BACKENDS asmjit"
    [ -f "$PKGLIBDIR/pg_jitter_mir.dylib" ] && BACKENDS="$BACKENDS mir"
fi

echo "=== RLE Benchmark ==="
echo "Port: $PGPORT, DB: $PGDB, Runs: $NRUNS"
echo "Backends: $BACKENDS"
echo "Output: $OUTFILE"
echo ""

# Setup tables
echo "Setting up RLE benchmark tables..."
psql_cmd -q -f "$SCRIPT_DIR/bench_setup_rle.sql" 2>&1 | grep -E "NOTICE|ERROR" || true
echo "Setup complete."
echo ""

# Verify tables exist
for tbl in rle_grp_100 rle_grp_300 rle_grp_1000 rle_rnd_100 rle_rnd_300 rle_rnd_1000; do
    cnt=$(psql_cmd -t -A -c "SELECT COUNT(*) FROM $tbl" 2>/dev/null || echo "MISSING")
    echo "  $tbl: $cnt rows"
done
echo ""

# Define queries
declare -a Q_NAMES Q_SQLS

add_query() {
    Q_NAMES+=("$1")
    Q_SQLS+=("$2")
}

# Group-typed (all-int4): RLE should help
# "last" = read the LAST data column to force full deform of all 100/300/1000 columns
add_query "Grp100_last"     "SELECT SUM(c0100) FROM rle_grp_100"
add_query "Grp100_span10"   "SELECT SUM(c0007+c0018+c0028+c0038+c0048+c0058+c0068+c0078+c0088+c0098) FROM rle_grp_100"
add_query "Grp100_grpby"    "SELECT grp, COUNT(*), SUM(c0007+c0048+c0098) FROM rle_grp_100 GROUP BY grp"
add_query "Grp300_last"     "SELECT SUM(c0300) FROM rle_grp_300"
add_query "Grp300_span10"   "SELECT SUM(c0007+c0058+c0108+c0158+c0208+c0258+c0298) FROM rle_grp_300"
add_query "Grp1000_last"    "SELECT SUM(c1000) FROM rle_grp_1000"
add_query "Grp1000_span10"  "SELECT SUM(c0007+c0108+c0208+c0308+c0408+c0508+c0608+c0708+c0808+c0908) FROM rle_grp_1000"
add_query "Grp1000_grpby"   "SELECT grp, COUNT(*), SUM(c0007+c0508+c0998) FROM rle_grp_1000 GROUP BY grp"

# Random-typed (cycling 6 types): RLE should not help (no runs >= 4)
# "last" = last data column (forces full deform). Use int columns for SUM.
add_query "Rnd100_last"     "SELECT SUM(c0096) FROM rle_rnd_100"
add_query "Rnd100_span_int" "SELECT SUM(c0006+c0012+c0018+c0024+c0030+c0042+c0048+c0054+c0060+c0066) FROM rle_rnd_100"
add_query "Rnd100_grpby"    "SELECT grp, COUNT(*), SUM(c0006+c0048+c0096) FROM rle_rnd_100 GROUP BY grp"
add_query "Rnd300_last"     "SELECT SUM(c0300) FROM rle_rnd_300"
add_query "Rnd300_span_int" "SELECT SUM(c0006+c0060+c0120+c0180+c0240+c0300) FROM rle_rnd_300"
add_query "Rnd1000_last"    "SELECT SUM(c0996) FROM rle_rnd_1000"
add_query "Rnd1000_span_int" "SELECT SUM(c0006+c0120+c0240+c0360+c0480+c0600+c0720+c0840+c0960) FROM rle_rnd_1000"
add_query "Rnd1000_grpby"   "SELECT grp, COUNT(*), SUM(c0006+c0480+c0960) FROM rle_rnd_1000 GROUP BY grp"

nq=${#Q_NAMES[@]}

# GUC prefix per backend — prepended to every query so settings survive new sessions
guc_prefix() {
    local be="$1"
    if [ "$be" = "interp" ]; then
        echo "SET jit = off;"
    else
        echo "SET jit = on; SET jit_above_cost = 0; SET jit_inline_above_cost = 0; SET jit_optimize_above_cost = 0; SET pg_jitter.backend = '$be';"
    fi
}

run_query_timed() {
    local sql="$1"
    local gucs="$2"
    # Warmup
    psql_cmd -t -A -c "$gucs $sql" >/dev/null 2>&1 || true
    # Timed runs: extract execution time from EXPLAIN ANALYZE
    local total=0
    local count=0
    for ((r=1; r<=NRUNS; r++)); do
        local ms
        ms=$(psql_cmd -t -A -c "$gucs EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $sql" 2>/dev/null \
            | grep -i "execution time" | sed 's/.*: *\([0-9.]*\).*/\1/')
        if [ -n "$ms" ]; then
            total=$(echo "$total + $ms" | bc)
            count=$((count + 1))
        fi
    done
    if [ "$count" -gt 0 ]; then
        echo "scale=2; $total / $count" | bc
    else
        echo "ERR"
    fi
}

# Header
{
    echo "=== RLE Benchmark Results ==="
    echo "Date: $(date)"
    echo "Backends: $BACKENDS"
    echo "Runs per query: $NRUNS"
    echo ""
    printf "%-22s" "Query"
    for be in $BACKENDS; do
        printf "%12s" "$be"
    done
    echo ""
    printf "%-22s" "-----"
    for be in $BACKENDS; do
        printf "%12s" "------"
    done
    echo ""
} | tee "$OUTFILE"

# Run benchmarks
for ((qi=0; qi<nq; qi++)); do
    name="${Q_NAMES[$qi]}"
    sql="${Q_SQLS[$qi]}"
    printf "%-22s" "$name" | tee -a "$OUTFILE"

    for be in $BACKENDS; do
        gucs=$(guc_prefix "$be")
        ms=$(run_query_timed "$sql" "$gucs")
        printf "%10s ms" "$ms" | tee -a "$OUTFILE"
    done
    echo "" | tee -a "$OUTFILE"

    # Separator between sections
    if [[ "$name" == *"grpby"* && "$name" == *"Grp"* ]] || \
       [[ "$name" == *"grpby"* && "$name" == *"Rnd"* ]] || \
       [[ "$name" == "Wide1000_span10" ]]; then
        echo "" | tee -a "$OUTFILE"
    fi
done

echo "" | tee -a "$OUTFILE"
echo "Results saved to: $OUTFILE"
