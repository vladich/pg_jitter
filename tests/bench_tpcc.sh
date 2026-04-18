#!/bin/bash
# bench_tpcc.sh — TPC-C benchmark comparing pg_jitter backends
#
# Measures server-side execution time via EXPLAIN ANALYZE for each
# TPC-C transaction type, eliminating client/network variability.
#
# Usage:
#   ./tests/bench_tpcc.sh [options]
set +e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5528}"
PGDB="bench_tpcc"
WAREHOUSES=10
NRUNS=5
NWARMUP=2
BACKENDS=""
SKIP_LOAD=0
JIT_ABOVE_COST="${JIT_ABOVE_COST:-0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config)   PG_CONFIG="$2"; shift 2;;
        --port)        PGPORT="$2";    shift 2;;
        --db)          PGDB="$2";      shift 2;;
        --warehouses)  WAREHOUSES="$2"; shift 2;;
        --runs)        NRUNS="$2";     shift 2;;
        --warmup)      NWARMUP="$2";   shift 2;;
        --backends)    BACKENDS="$2";  shift 2;;
        --skip-load)   SKIP_LOAD=1;    shift;;
        --jit-above-cost) JIT_ABOVE_COST="$2"; shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
DLSUFFIX=".dylib"
[ "$(uname)" = "Linux" ] && DLSUFFIX=".so"

psql_cmd() { "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"; }
psql_maint() { "$PGBIN/psql" -p "$PGPORT" -d postgres -X "$@"; }

# Auto-detect backends
if [ -z "$BACKENDS" ]; then
    BACKENDS="interp"
    for b in sljit asmjit mir; do
        [ -f "$PKGLIBDIR/pg_jitter_${b}${DLSUFFIX}" ] && BACKENDS="$BACKENDS $b"
    done
    [ -f "$PKGLIBDIR/pg_jitter${DLSUFFIX}" ] && BACKENDS="$BACKENDS auto"
fi

echo "=== TPC-C Benchmark ($WAREHOUSES warehouses, jit_above_cost=$JIT_ABOVE_COST) ==="
echo "  Port:     $PGPORT"
echo "  Backends: $BACKENDS"
echo "  Runs:     $NRUNS (warmup: $NWARMUP)"
echo ""

# ================================================================
# Data loading
# ================================================================
if [ "$SKIP_LOAD" -eq 0 ]; then
    echo "Creating database $PGDB..."
    psql_maint -q -c "DROP DATABASE IF EXISTS $PGDB;" 2>/dev/null
    psql_maint -q -c "CREATE DATABASE $PGDB;"
    echo "Creating schema..."
    psql_cmd -q -f "$SCRIPT_DIR/bench_tpcc_setup.sql"
    echo "Loading data ($WAREHOUSES warehouses)..."
    psql_cmd -v warehouses="$WAREHOUSES" -f "$SCRIPT_DIR/bench_tpcc_load.sql"
    echo "Creating indexes & analyzing..."
    psql_cmd -q -c "ANALYZE;"
    psql_cmd -q -c "
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
SELECT pg_prewarm(c.oid::regclass, 'buffer')
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'public' AND c.relkind IN ('r','i');
" > /dev/null 2>&1
    echo ""
fi

# ================================================================
# TPC-C Queries — individual statements from each transaction type
# ================================================================
declare -a LABELS QUERIES

add_q() { LABELS+=("$1"); QUERIES+=("$2"); }

# Pre-generate enough random parameters for the static query templates.
# Some templates intentionally use different slots to avoid reusing the same
# warehouse/district/customer values; the highest slot currently used is 6.
TOTAL_RUNS=$((NWARMUP + NRUNS))
if [ "$TOTAL_RUNS" -lt 7 ]; then
    TOTAL_RUNS=7
fi
gen_rand() {
    local lo="$1" hi="$2" range=$(($2 - $1 + 1))
    for i in $(seq 1 "$TOTAL_RUNS"); do
        echo $(( (RANDOM * 32768 + RANDOM) % range + lo ))
    done | tr '\n' ' '
}
R_WID=($(gen_rand 1 "$WAREHOUSES"))
R_DID=($(gen_rand 1 10))
R_CID=($(gen_rand 1 3000))
R_IID=($(gen_rand 1 100000))
R_IID2=($(gen_rand 1 100000))
R_OID=($(gen_rand 2000 2999))
R_QTY=($(gen_rand 1 10))
R_THR=($(gen_rand 10 20))

# --- Point lookups (single-row, PK index) ---
add_q "PK_warehouse" \
    "SELECT w_tax FROM warehouse WHERE w_id = ${R_WID[0]}"

add_q "PK_district" \
    "SELECT d_tax, d_next_o_id FROM district WHERE d_w_id = ${R_WID[1]} AND d_id = ${R_DID[0]}"

add_q "PK_customer" \
    "SELECT c_discount, c_last, c_credit FROM customer WHERE c_w_id = ${R_WID[2]} AND c_d_id = ${R_DID[1]} AND c_id = ${R_CID[0]}"

add_q "PK_item" \
    "SELECT i_price, i_name, i_data FROM item WHERE i_id = ${R_IID[0]}"

# --- Single-row updates ---
add_q "UPD_warehouse" \
    "UPDATE warehouse SET w_ytd = w_ytd + 1234.56 WHERE w_id = ${R_WID[4]}"

add_q "UPD_customer" \
    "UPDATE customer SET c_balance = c_balance - 1234.56, c_ytd_payment = c_ytd_payment + 1234.56, c_payment_cnt = c_payment_cnt + 1 WHERE c_w_id = ${R_WID[6]} AND c_d_id = ${R_DID[3]} AND c_id = ${R_CID[1]}"

# --- Multi-row scans & aggregations (expression-heavy) ---
add_q "SCAN_cust_name" \
    "SELECT c_id, c_first, c_last, c_balance FROM customer WHERE c_w_id = ${R_WID[2]} AND c_d_id = ${R_DID[1]} AND c_last LIKE 'OUGHT%' ORDER BY c_first"

add_q "SCAN_orders_range" \
    "SELECT o_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt FROM oorder WHERE o_w_id = ${R_WID[0]} AND o_d_id = ${R_DID[0]} AND o_id BETWEEN 1000 AND 2000"

add_q "SCAN_orderlines" \
    "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_w_id = ${R_WID[0]} AND ol_d_id = ${R_DID[0]} AND ol_o_id BETWEEN 1000 AND 1100"

add_q "AGG_district_rev" \
    "SELECT o.o_d_id, count(*), sum(ol_amount), avg(ol_amount) FROM order_line ol JOIN oorder o ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id WHERE o.o_w_id = ${R_WID[0]} AND o.o_id BETWEEN 1 AND 1000 GROUP BY o.o_d_id"

add_q "AGG_cust_stats" \
    "SELECT c_credit, count(*), sum(c_balance), avg(c_discount), min(c_ytd_payment), max(c_ytd_payment) FROM customer WHERE c_w_id = ${R_WID[0]} AND c_d_id = ${R_DID[0]} GROUP BY c_credit"

add_q "JOIN_order_detail" \
    "SELECT o.o_id, c.c_last, c.c_credit, o.o_entry_d, o.o_ol_cnt, sum(ol.ol_amount) AS total FROM oorder o JOIN customer c ON c.c_w_id = o.o_w_id AND c.c_d_id = o.o_d_id AND c.c_id = o.o_c_id JOIN order_line ol ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id WHERE o.o_w_id = ${R_WID[0]} AND o.o_d_id = ${R_DID[0]} AND o.o_id BETWEEN 2500 AND 2600 GROUP BY o.o_id, c.c_last, c.c_credit, o.o_entry_d, o.o_ol_cnt ORDER BY total DESC"

# --- Stock Level (TPC-C's most complex query) ---
add_q "SL_stock_check" \
    "SELECT count(DISTINCT s_i_id) FROM order_line JOIN stock ON s_i_id = ol_i_id AND s_w_id = ol_w_id WHERE ol_w_id = 1 AND ol_d_id = 1 AND ol_o_id BETWEEN (SELECT d_next_o_id - 20 FROM district WHERE d_w_id = 1 AND d_id = 1) AND (SELECT d_next_o_id - 1 FROM district WHERE d_w_id = 1 AND d_id = 1) AND s_quantity < ${R_THR[0]}"

# --- CASE/expression-heavy analytics on TPC-C data ---
add_q "EXPR_order_prio" \
    "SELECT CASE WHEN o_carrier_id IS NOT NULL THEN 'delivered' WHEN o_carrier_id IS NULL AND o_entry_d > now() - interval '30 day' THEN 'pending' ELSE 'old_pending' END AS status, count(*), avg(o_ol_cnt) FROM oorder WHERE o_w_id = ${R_WID[0]} GROUP BY 1"

add_q "EXPR_cust_tier" \
    "SELECT CASE WHEN c_balance > 1000 THEN 'high' WHEN c_balance > 0 THEN 'medium' WHEN c_balance > -100 THEN 'low' ELSE 'negative' END AS tier, c_credit, count(*), sum(c_balance), avg(c_discount) FROM customer WHERE c_w_id = ${R_WID[0]} GROUP BY 1, 2 ORDER BY 4 DESC"

NQUERIES=${#LABELS[@]}

# ================================================================
# Helpers
# ================================================================
median() {
    local vals=("$@")
    local n=${#vals[@]}
    local sorted=($(printf '%s\n' "${vals[@]}" | sort -g))
    echo "${sorted[$((n / 2))]}"
}

get_exec_times() {
    # Run warmup + timed runs in a SINGLE session to avoid provider reload cost.
    # Returns one execution time per line for the timed runs.
    local query="$1" jit_on="$2" backend="$3"
    local nwarm="$NWARMUP" nrun="$NRUNS"

    local set_backend=""
    [ "$backend" != "interp" ] && set_backend="SET pg_jitter.backend = '$backend';"

    # Build SQL: settings, warmup runs (discard), timed runs (measure)
    local sql="SET jit = $jit_on;
SET jit_above_cost = $JIT_ABOVE_COST;
SET jit_inline_above_cost = $JIT_ABOVE_COST;
SET jit_optimize_above_cost = $JIT_ABOVE_COST;
$set_backend
"
    # Warmup: plain execution (no EXPLAIN)
    for w in $(seq 1 "$nwarm"); do
        sql="$sql
$query;
"
    done

    # Timed: EXPLAIN ANALYZE
    for r in $(seq 1 "$nrun"); do
        sql="$sql
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
"
    done

    psql_cmd -t -A -c "$sql" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

ensure_pg_running() {
    if ! "$PGBIN/pg_isready" -p "$PGPORT" -q 2>/dev/null; then
        local PGDATA
        PGDATA=$(psql_maint -t -A -c "SHOW data_directory;" 2>/dev/null)
        "$PGBIN/pg_ctl" -D "$PGDATA" start -l "$PGDATA/logfile" -w 2>/dev/null
        sleep 1
    fi
}

# ================================================================
# Run benchmarks — interleaved per query
# ================================================================
CSV="$SCRIPT_DIR/bench_tpcc_$(date +%Y%m%d_%H%M%S).csv"
echo "query,backend,exec_time_ms" > "$CSV"

# Prewarm all tables and indexes into shared_buffers
echo -n "Prewarming buffer cache..."
psql_cmd -q -c "
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
SELECT pg_prewarm(c.oid::regclass, 'buffer')
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'public' AND c.relkind IN ('r', 'i');
" > /dev/null 2>&1
echo " done."
echo ""

echo "Running $NQUERIES queries × ${#BACKENDS[@]} backends (interleaved)..."
echo ""

BACKEND_ARR=($BACKENDS)

for qi in $(seq 0 $((NQUERIES - 1))); do
    label="${LABELS[$qi]}"
    query="${QUERIES[$qi]}"
    printf "  %-18s " "$label"

    # Prewarm buffer cache before each query to eliminate I/O variance
    psql_cmd -q -c "
    CREATE EXTENSION IF NOT EXISTS pg_prewarm;
    SELECT pg_prewarm(c.oid::regclass, 'buffer')
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public' AND c.relkind IN ('r', 'i');
    " > /dev/null 2>&1 || true

    for backend in "${BACKEND_ARR[@]}"; do
        jit_on="on"
        [ "$backend" = "interp" ] && jit_on="off"

        ensure_pg_running

        # All warmup + timed runs in a single session
        exec_times=()
        while IFS= read -r t; do
            [ -n "$t" ] && exec_times+=("$t")
        done < <(get_exec_times "$query" "$jit_on" "$backend" 2>/dev/null)

        if [ ${#exec_times[@]} -eq 0 ]; then
            echo "${label},$backend,CRASH" >> "$CSV"
            printf "X"
        else
            med=$(median "${exec_times[@]}")
            echo "${label},$backend,$med" >> "$CSV"
            printf "."
        fi
    done
    echo ""
done

echo ""
echo "CSV: $CSV"
echo ""

# ================================================================
# Summary
# ================================================================
echo "=== TPC-C Results (jit_above_cost=$JIT_ABOVE_COST) ==="
echo ""
printf "%-18s %10s" "Query" "No JIT"
for b in "${BACKEND_ARR[@]}"; do
    [ "$b" = "interp" ] && continue
    printf " %14s" "$b"
done
echo ""
printf "%-18s %10s" "-----" "------"
for b in "${BACKEND_ARR[@]}"; do
    [ "$b" = "interp" ] && continue
    printf " %14s" "------"
done
echo ""

for qi in $(seq 0 $((NQUERIES - 1))); do
    label="${LABELS[$qi]}"
    base=$(grep "^${label},interp," "$CSV" | head -1 | cut -d',' -f3)
    printf "%-18s" "$label"

    if [ -z "$base" ] || [ "$base" = "CRASH" ]; then
        printf " %10s" "CRASH"
    else
        printf " %9.3f" "$base"
    fi

    for b in "${BACKEND_ARR[@]}"; do
        [ "$b" = "interp" ] && continue
        val=$(grep "^${label},${b}," "$CSV" | head -1 | cut -d',' -f3)
        if [ -z "$val" ] || [ "$val" = "CRASH" ]; then
            printf " %14s" "CRASH"
        elif [ -n "$base" ] && [ "$base" != "CRASH" ] && [ "$base" != "0" ]; then
            spd=$(echo "scale=2; $base / $val" | bc 2>/dev/null)
            printf " %8.3f (%sx)" "$val" "$spd"
        else
            printf " %14.3f" "$val"
        fi
    done
    echo ""
done
echo ""
