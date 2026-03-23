#!/bin/bash
# bench_case_bsearch.sh — Benchmark CASE binary search optimization for all types
#
# Measures CASE expression performance with varying branch counts across
# all supported types: int4, int8, int2, float4, float8, date, timestamp,
# oid, text, numeric, uuid, bytea.
#
# Usage:
#   ./tests/bench_case_bsearch.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config (default: $PG_CONFIG or pg_config)
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: $PGDB or postgres)
#   --backends LIST    Space-separated backend list (default: "interp sljit asmjit mir")
#   --branches N       Number of CASE branches (default: 50)
#   --types LIST       Space-separated type list (default: all types)
#   --rows N           Rows in test table (default: 100000)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-postgres}"
BACKENDS="interp sljit asmjit mir"
NUM_BRANCHES=50
TEST_TYPES="int4 int8 int2 float4 float8 date timestamp oid text numeric uuid bytea"
NUM_ROWS=100000

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --backends)  BACKENDS="$2";  shift 2;;
        --branches)  NUM_BRANCHES="$2"; shift 2;;
        --types)     TEST_TYPES="$2"; shift 2;;
        --rows)      NUM_ROWS="$2";   shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PG_VERSION="$("$PG_CONFIG" --version)"

psql_cmd() {
    "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"
}

echo "============================================"
echo "  CASE Binary Search Benchmark"
echo "  PostgreSQL: $PG_VERSION"
echo "  Backends: $BACKENDS"
echo "  Branches: $NUM_BRANCHES"
echo "  Rows: $NUM_ROWS"
echo "  Types: $TEST_TYPES"
echo "============================================"
echo ""

# Check provider
JIT_PROVIDER=$(psql_cmd -t -A -c "SHOW jit_provider;" 2>/dev/null || echo "")
META_MODE=0
PGDATA=""
if [ "$JIT_PROVIDER" = "pg_jitter" ]; then
    META_MODE=1
    PGDATA=$(psql_cmd -t -A -c "SHOW data_directory;" 2>/dev/null)
fi

# Create test table
echo "Creating test table (case_bsearch_bench, $NUM_ROWS rows)..."
psql_cmd -c "DROP TABLE IF EXISTS case_bsearch_bench;" >/dev/null 2>&1
psql_cmd -c "
CREATE TABLE case_bsearch_bench AS
SELECT i,
    (i % 1000)::smallint AS v_int2,
    (i % 1000)::integer AS v_int4,
    (i % 1000)::bigint AS v_int8,
    (i % 1000)::real AS v_float4,
    (i % 1000)::double precision AS v_float8,
    ('2020-01-01'::date + (i % 1000))::date AS v_date,
    ('2020-01-01'::timestamp + (i % 1000) * interval '1 day')::timestamp AS v_timestamp,
    (i % 1000)::oid AS v_oid,
    ('val_' || lpad((i % 1000)::text, 4, '0'))::text AS v_text,
    (i % 1000)::numeric AS v_numeric,
    (lpad(to_hex((i % 1000) % 256), 8, '0') || '-0000-0000-0000-000000000000')::uuid AS v_uuid,
    decode(lpad(to_hex(i % 256), 4, '0'), 'hex')::bytea AS v_bytea
FROM generate_series(1, $NUM_ROWS) i;
" >/dev/null 2>&1
psql_cmd -c "ANALYZE case_bsearch_bench;" >/dev/null 2>&1

# Generate CASE expressions for each type
generate_case_lt() {
    local col_type=$1
    local num=$2
    local col="v_${col_type}"
    local branches=""
    local cast=""

    case $col_type in
        int4)      cast=""; for i in $(seq 1 $num); do branches="$branches WHEN $col < $((i * (1000 / (num + 1)))) THEN $((i-1))"; done;;
        int8)      cast="::bigint"; for i in $(seq 1 $num); do branches="$branches WHEN $col < $((i * (1000 / (num + 1))))${cast} THEN $((i-1))"; done;;
        int2)      cast="::smallint"; for i in $(seq 1 $num); do branches="$branches WHEN $col < $((i * (1000 / (num + 1))))${cast} THEN $((i-1))"; done;;
        float4)    cast="::real"; for i in $(seq 1 $num); do branches="$branches WHEN $col < $(echo "scale=1; $i * 1000 / ($num + 1)" | bc)${cast} THEN $((i-1))"; done;;
        float8)    cast=""; for i in $(seq 1 $num); do branches="$branches WHEN $col < $(echo "scale=1; $i * 1000 / ($num + 1)" | bc) THEN $((i-1))"; done;;
        date)      for i in $(seq 1 $num); do branches="$branches WHEN $col < ('2020-01-01'::date + $((i * (1000 / (num + 1))))) THEN $((i-1))"; done;;
        timestamp) for i in $(seq 1 $num); do branches="$branches WHEN $col < ('2020-01-01'::timestamp + $((i * (1000 / (num + 1)))) * interval '1 day') THEN $((i-1))"; done;;
        oid)       cast="::oid"; for i in $(seq 1 $num); do branches="$branches WHEN $col < $((i * (1000 / (num + 1))))${cast} THEN $((i-1))"; done;;
        text)      for i in $(seq 1 $num); do branches="$branches WHEN $col < 'val_$(printf '%04d' $((i * (1000 / (num + 1)))))' THEN $((i-1))"; done;;
        numeric)   for i in $(seq 1 $num); do branches="$branches WHEN $col < $(echo "scale=1; $i * 1000 / ($num + 1)" | bc)::numeric THEN $((i-1))"; done;;
        *)         return 1;;
    esac

    echo "SELECT SUM(CASE${branches} ELSE $num END) FROM case_bsearch_bench"
}

generate_case_eq() {
    local col_type=$1
    local num=$2
    local col="v_${col_type}"
    local branches=""
    local cast=""

    case $col_type in
        int4)      for i in $(seq 1 $num); do branches="$branches WHEN $((i * (1000 / (num + 1)))) THEN $((i-1))"; done;;
        int8)      cast="::bigint"; for i in $(seq 1 $num); do branches="$branches WHEN $((i * (1000 / (num + 1))))${cast} THEN $((i-1))"; done;;
        int2)      cast="::smallint"; for i in $(seq 1 $num); do branches="$branches WHEN $((i * (1000 / (num + 1))))${cast} THEN $((i-1))"; done;;
        float4)    cast="::real"; for i in $(seq 1 $num); do branches="$branches WHEN $(echo "scale=1; $i * 1000 / ($num + 1)" | bc)${cast} THEN $((i-1))"; done;;
        float8)    for i in $(seq 1 $num); do branches="$branches WHEN $(echo "scale=1; $i * 1000 / ($num + 1)" | bc) THEN $((i-1))"; done;;
        oid)       cast="::oid"; for i in $(seq 1 $num); do branches="$branches WHEN $((i * (1000 / (num + 1))))${cast} THEN $((i-1))"; done;;
        text)      for i in $(seq 1 $num); do branches="$branches WHEN 'val_$(printf '%04d' $((i * (1000 / (num + 1)))))' THEN $((i-1))"; done;;
        numeric)   for i in $(seq 1 $num); do branches="$branches WHEN $(echo "scale=1; $i * 1000 / ($num + 1)" | bc)::numeric THEN $((i-1))"; done;;
        uuid)      for i in $(seq 1 $num); do
                       local hex=$(printf '%08x' $((i * (256 / (num + 1)) % 256)))
                       branches="$branches WHEN '${hex}-0000-0000-0000-000000000000'::uuid THEN $((i-1))"
                   done;;
        bytea)     for i in $(seq 1 $num); do
                       local hex=$(printf '%04x' $((i % 256)))
                       branches="$branches WHEN '\\x${hex}'::bytea THEN $((i-1))"
                   done;;
        *)         return 1;;
    esac

    echo "SELECT SUM(CASE $col${branches} ELSE $num END) FROM case_bsearch_bench"
}

TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"; psql_cmd -c "DROP TABLE IF EXISTS case_bsearch_bench;" >/dev/null 2>&1' EXIT

# Print header
printf "%-12s %-10s %-8s %12s %12s\n" "Backend" "Type" "Pattern" "Planning" "Execution"
printf "%-12s %-10s %-8s %12s %12s\n" "-------" "----" "-------" "--------" "---------"

SWITCHED_AWAY=0

for backend in $BACKENDS; do
    # Set JIT parameters
    if [ "$backend" = "interp" ]; then
        JIT_PREFIX="SET jit = off;"
    elif [ "$backend" = "llvmjit" ]; then
        JIT_PREFIX="SET jit = on; SET jit_above_cost = 0; SET jit_inline_above_cost = 0; SET jit_optimize_above_cost = 0;"
        if [ "$META_MODE" -eq 1 ]; then
            psql_cmd -c "ALTER SYSTEM SET jit_provider = 'llvmjit';" >/dev/null 2>&1
            "$PGBIN/pg_ctl" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 2
            SWITCHED_AWAY=1
        fi
    else
        JIT_PREFIX="SET jit = on; SET jit_above_cost = 0; SET jit_inline_above_cost = 0; SET jit_optimize_above_cost = 0;"
        if [ "$META_MODE" -eq 1 ]; then
            JIT_PREFIX="$JIT_PREFIX SET pg_jitter.backend = '$backend';"
        fi
        if [ "$SWITCHED_AWAY" -eq 1 ]; then
            psql_cmd -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" >/dev/null 2>&1
            "$PGBIN/pg_ctl" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 2
            SWITCHED_AWAY=0
        fi
    fi

    for col_type in $TEST_TYPES; do
        # Pattern A: searched CASE with <
        # (only for types that support <: skip uuid, bytea for < pattern)
        case $col_type in
            uuid|bytea)
                ;;
            *)
                QUERY=$(generate_case_lt "$col_type" "$NUM_BRANCHES")
                if [ -n "$QUERY" ]; then
                    cat > "$TMPFILE" <<EOSQL
$JIT_PREFIX
SET max_parallel_workers_per_gather = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $QUERY;
EOSQL
                    OUTPUT=$(psql_cmd -f "$TMPFILE" 2>/dev/null)
                    PLANNING=$(echo "$OUTPUT" | grep "Planning Time" | sed 's/.*Planning Time: //' | sed 's/ ms//')
                    EXEC=$(echo "$OUTPUT" | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//')
                    printf "%-12s %-10s %-8s %10s ms %10s ms\n" "$backend" "$col_type" "LT" "${PLANNING:-?}" "${EXEC:-?}"
                fi
                ;;
        esac

        # Pattern B: simple CASE with =
        # (only for types that support = pattern: skip date, timestamp for = since
        #  simple CASE patterns with these types are unusual)
        case $col_type in
            date|timestamp)
                ;;
            *)
                QUERY=$(generate_case_eq "$col_type" "$NUM_BRANCHES")
                if [ -n "$QUERY" ]; then
                    cat > "$TMPFILE" <<EOSQL
$JIT_PREFIX
SET max_parallel_workers_per_gather = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $QUERY;
EOSQL
                    OUTPUT=$(psql_cmd -f "$TMPFILE" 2>/dev/null)
                    PLANNING=$(echo "$OUTPUT" | grep "Planning Time" | sed 's/.*Planning Time: //' | sed 's/ ms//')
                    EXEC=$(echo "$OUTPUT" | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//')
                    printf "%-12s %-10s %-8s %10s ms %10s ms\n" "$backend" "$col_type" "EQ" "${PLANNING:-?}" "${EXEC:-?}"
                fi
                ;;
        esac
    done
    echo ""
done

# Restore
if [ "$SWITCHED_AWAY" -eq 1 ] && [ "$META_MODE" -eq 1 ]; then
    psql_cmd -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" >/dev/null 2>&1
    "$PGBIN/pg_ctl" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
fi

echo "Notes:"
echo "  - Planning includes JIT compilation time"
echo "  - Execution is query execution time"
echo "  - LT = searched CASE (WHEN col < val THEN ...)"
echo "  - EQ = simple CASE (CASE col WHEN val THEN ...)"
echo "  - $NUM_BRANCHES branches, $NUM_ROWS rows"
echo "  - Binary search: O(log N) per row vs O(N) per row without optimization"
echo ""
echo "Done."
