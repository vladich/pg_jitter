#!/bin/bash
# run_all_benchmarks.sh — Run bench_comprehensive.sh for PG14-18 sequentially
#
# Usage:
#   PG_BASE=/path/to/postgres_installs ./tests/run_all_benchmarks.sh
#   ./tests/run_all_benchmarks.sh --pg-base /path/to/postgres_installs
#
# PG_BASE should contain v14/, v15/, ..., v18/ subdirectories,
# each with bin/pg_config, bin/initdb, etc.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Parse --pg-base if present
if [ "${1:-}" = "--pg-base" ]; then
    [ -z "${2:-}" ] && { echo "ERROR: --pg-base requires a path argument"; exit 1; }
    PG_BASE="$2"
    shift 2
fi

if [ -z "${PG_BASE:-}" ]; then
    echo "ERROR: PG_BASE is not set."
    echo "  Use: PG_BASE=/path/to/postgres_installs $0"
    echo "   or: $0 --pg-base /path/to/postgres_installs"
    echo ""
    echo "  PG_BASE should contain v14/, v15/, ..., v18/ subdirectories."
    exit 1
fi

for v in 14 15 16 17 18; do
    PG="$PG_BASE/v$v"
    PORT=$((5500 + v))
    CLUSTER="/tmp/pg_jitter_bench_v$v"

    echo ""
    echo "============================================================"
    echo "  PG$v — port $PORT"
    echo "============================================================"

    # Clean up any previous cluster
    "$PG/bin/pg_ctl" -D "$CLUSTER" stop -m immediate 2>/dev/null || true
    sleep 1
    rm -rf "$CLUSTER"

    # Init fresh cluster
    "$PG/bin/initdb" -D "$CLUSTER" --no-locale 2>&1 | tail -1
    printf "jit_provider = 'pg_jitter'\n" >> "$CLUSTER/postgresql.conf"
    printf "port = %d\n" "$PORT" >> "$CLUSTER/postgresql.conf"

    # Start
    "$PG/bin/pg_ctl" -D "$CLUSTER" start -l "$CLUSTER/logfile" -w 2>&1
    sleep 2

    # Verify
    "$PG/bin/psql" -p "$PORT" -d postgres -t -A -c "SHOW jit_provider;" 2>&1

    # Run benchmark
    bash "$SCRIPT_DIR/bench_comprehensive.sh" \
        --pg-config "$PG/bin/pg_config" \
        --port "$PORT" \
        --db postgres \
        --runs 5 \
        --warmup 3 \
        --md "$REPO_DIR/BENCHMARK_PG$v.md" \
    || echo "WARNING: PG$v benchmark exited with error (non-fatal)"

    # Stop cluster
    "$PG/bin/pg_ctl" -D "$CLUSTER" stop -m fast 2>/dev/null || true
    sleep 1
    rm -rf "$CLUSTER"

    echo "PG$v benchmark complete."
done

echo ""
echo "All benchmarks done."
