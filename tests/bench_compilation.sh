#!/bin/bash
# bench_compilation.sh — Compilation stress test with ~200KB SQL query
#
# Tests JIT compilation overhead on extremely large queries.
# Uses EXPLAIN ANALYZE to measure planning and execution time separately.
# This is a separate test because each run takes 90+ seconds.
#
# Usage:
#   ./tests/bench_compilation.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config (default: $PG_CONFIG or pg_config)
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: $PGDB or postgres)
#   --backends LIST    Space-separated backend list (default: "interp sljit asmjit mir")
#   --branches N       Number of CASE branches (default: 3500 for multi, 500 for single)
#   --mode MODE        "multi" (default): many 1-branch CASEs (stress compilation)
#                      "single": one CASE with N WHEN branches (stress branch evaluation;
#                                benefits from binary search optimization)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-postgres}"
BACKENDS="interp sljit asmjit mir"
NUM_BRANCHES=""
MODE="multi"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --backends)  BACKENDS="$2";  shift 2;;
        --branches)  NUM_BRANCHES="$2"; shift 2;;
        --mode)      MODE="$2";      shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

# Default branch count depends on mode
if [ -z "$NUM_BRANCHES" ]; then
    if [ "$MODE" = "single" ]; then
        NUM_BRANCHES=500
    else
        NUM_BRANCHES=3500
    fi
fi

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PG_VERSION="$("$PG_CONFIG" --version)"

psql_cmd() {
    "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"
}

echo "============================================"
echo "  Compilation Stress Test"
echo "  PostgreSQL: $PG_VERSION"
echo "  Backends: $BACKENDS"
echo "  Mode: $MODE"
echo "  CASE branches: $NUM_BRANCHES"
echo "============================================"
echo ""

# Check provider
JIT_PROVIDER=$(psql_cmd -t -A -c "SHOW jit_provider;" 2>/dev/null || echo "")
META_MODE=0
PGDATA=""
if [ "$JIT_PROVIDER" = "pg_jitter" ]; then
    META_MODE=1
    PGDATA=$(psql_cmd -t -A -c "SHOW data_directory;" 2>/dev/null)
    psql_cmd -t -A -c "SET jit_above_cost = 0; SELECT 1;" >/dev/null 2>&1
fi

# Ensure ultra_wide exists
ROW_COUNT=$(psql_cmd -t -A -c "SELECT COUNT(*) FROM ultra_wide;" 2>/dev/null || echo "0")
if [ "$ROW_COUNT" -eq 0 ]; then
    echo "ERROR: ultra_wide table is empty or missing. Run bench_setup_extra.sql first."
    exit 1
fi
echo "ultra_wide rows: $ROW_COUNT"

# Generate the query
if [ "$MODE" = "single" ]; then
    # Single CASE with N WHEN branches — stress branch evaluation.
    # Benefits from binary search optimization (O(log N) vs O(N)).
    echo "Generating single CASE with $NUM_BRANCHES WHEN branches..."
    BRANCHES=""
    for i in $(seq 1 "$NUM_BRANCHES"); do
        BRANCHES="${BRANCHES} WHEN c01 < $i THEN $((i-1))"
    done
    FULL_QUERY="SELECT SUM(CASE${BRANCHES} ELSE $NUM_BRANCHES END) FROM ultra_wide"
else
    # Many independent 1-branch CASEs — stress compilation (default).
    echo "Generating $NUM_BRANCHES independent CASE expressions..."
    MEGA=""
    for i in $(seq 1 "$NUM_BRANCHES"); do
        c1=$(printf "c%02d" $(( (i % 20) + 1 )))
        c2=$(printf "c%02d" $(( ((i+3) % 20) + 1 )))
        c3=$(printf "c%02d" $(( ((i+7) % 20) + 1 )))
        c4=$(printf "c%02d" $(( ((i+11) % 20) + 1 )))
        c5=$(printf "c%02d" $(( ((i+15) % 20) + 1 )))
        if [ -n "$MEGA" ]; then MEGA="$MEGA + "; fi
        MEGA="${MEGA}CASE WHEN $c1>$((i%500)) THEN ($c2+$c3)%100 ELSE ($c4-$c5)%100 END"
    done
    FULL_QUERY="SELECT SUM($MEGA) FROM ultra_wide"
fi
QUERY_SIZE=$(echo -n "$FULL_QUERY" | wc -c | tr -d ' ')
echo "Query size: $QUERY_SIZE bytes ($((QUERY_SIZE/1024))KB)"
echo ""

# Save query to temp file (too large for command line in some shells)
TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"' EXIT

# Run for each backend
printf "%-12s %12s %12s\n" "Backend" "Planning" "Execution"
printf "%-12s %12s %12s\n" "-------" "--------" "---------"

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
        # If we previously switched to llvmjit, switch back
        if [ "$SWITCHED_AWAY" -eq 1 ]; then
            psql_cmd -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" >/dev/null 2>&1
            "$PGBIN/pg_ctl" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 2
            SWITCHED_AWAY=0
        fi
    fi

    # Write query to temp file
    cat > "$TMPFILE" <<EOSQL
$JIT_PREFIX
SET max_parallel_workers_per_gather = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $FULL_QUERY;
EOSQL

    # Run and extract Planning Time + Execution Time from TEXT output
    OUTPUT=$(psql_cmd -f "$TMPFILE" 2>/dev/null)

    PLANNING=$(echo "$OUTPUT" | grep "Planning Time" | sed 's/.*Planning Time: //' | sed 's/ ms//')
    EXEC=$(echo "$OUTPUT" | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//')

    printf "%-12s %10s ms %10s ms\n" "$backend" "${PLANNING:-?}" "${EXEC:-?}"
done

# Restore pg_jitter if we switched away
if [ "$SWITCHED_AWAY" -eq 1 ] && [ "$META_MODE" -eq 1 ]; then
    psql_cmd -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" >/dev/null 2>&1
    "$PGBIN/pg_ctl" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
fi

echo ""
echo "Notes:"
echo "  - Planning: SQL parsing + query planning time"
echo "  - Execution: Total execution time (includes JIT compilation)"
echo "  - Mode: $MODE ($NUM_BRANCHES branches = ~$((QUERY_SIZE/1024))KB of SQL)"
if [ "$MODE" = "single" ]; then
echo "  - Single CASE: benefits from binary search (CASE_BSEARCH_MIN_BRANCHES=16)"
fi
echo "  - ultra_wide table: $ROW_COUNT rows, 22 columns"
echo ""
echo "Done."
