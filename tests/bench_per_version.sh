#!/bin/bash
# bench_per_version.sh — Run bench_comprehensive.sh for each PG version,
# generating BENCHMARK_PG{V}.md files.
#
# Usage:
#   ./tests/bench_per_version.sh [VERSIONS...]
#   ./tests/bench_per_version.sh 14 15 16 17 18
#   RUNS=3 WARMUP=2 ./tests/bench_per_version.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNS=${RUNS:-5}
WARMUP=${WARMUP:-3}
BACKENDS="${BACKENDS:-interp sljit asmjit mir}"

if [ $# -gt 0 ]; then
    VERSIONS="$@"
else
    VERSIONS="14 15 16 17 18"
fi

echo "============================================================"
echo "  pg_jitter Per-Version Benchmark Suite"
echo "============================================================"
echo "  Versions:  $VERSIONS"
echo "  Backends:  $BACKENDS"
echo "  Runs:      $RUNS (warmup: $WARMUP)"
echo "  Output:    BENCHMARK_PG{V}.md in repo root"
echo "============================================================"
echo ""

for v in $VERSIONS; do
    PORT="55${v}"
    PGBIN=~/postgres_install/v${v}/bin
    PGCONFIG="$PGBIN/pg_config"
    PGDATA="/tmp/pg_jitter_bench_v${v}"

    if [ ! -x "$PGCONFIG" ]; then
        echo "SKIP: PG$v not installed at $PGBIN"
        continue
    fi

    echo ""
    echo "########################################################"
    echo "  PostgreSQL $v (port $PORT)"
    echo "########################################################"
    echo ""

    # --- Create cluster if needed ---
    CLUSTER_CREATED=0
    if ! $PGBIN/pg_isready -p "$PORT" -q 2>/dev/null; then
        if [ -d "$PGDATA" ]; then
            rm -rf "$PGDATA"
        fi
        echo "  Creating cluster at $PGDATA..."
        $PGBIN/initdb -D "$PGDATA" --locale=en_US.UTF-8 --no-instructions >/dev/null 2>&1
        cat >> "$PGDATA/postgresql.conf" <<EOF
port = $PORT
shared_buffers = 1GB
work_mem = 128MB
jit = on
jit_above_cost = 0
jit_provider = 'pg_jitter'
max_connections = 50
log_min_messages = warning
EOF
        $PGBIN/pg_ctl -D "$PGDATA" start -l "$PGDATA/logfile" -w >/dev/null 2>&1
        CLUSTER_CREATED=1
        echo "  Cluster started on port $PORT"
    else
        echo "  Using existing cluster on port $PORT"
    fi

    # Verify
    VER=$($PGBIN/psql -p "$PORT" -d postgres -t -A -c "SELECT version();" 2>/dev/null)
    echo "  $VER"
    echo ""

    # --- Run comprehensive benchmark ---
    MD_FILE="$REPO_DIR/BENCHMARK_PG${v}.md"
    CSV_FILE="$SCRIPT_DIR/bench_pg${v}_$(date +%Y%m%d_%H%M%S).csv"

    echo "  Running comprehensive benchmark -> $MD_FILE"
    bash "$SCRIPT_DIR/bench_comprehensive.sh" \
        --pg-config "$PGCONFIG" \
        --port "$PORT" \
        --runs "$RUNS" \
        --warmup "$WARMUP" \
        --backends "$BACKENDS" \
        --csv "$CSV_FILE" \
        --md "$MD_FILE" \
        2>&1

    echo ""
    echo "  PG${v} benchmark complete."
    echo "  CSV: $CSV_FILE"
    echo "  MD:  $MD_FILE"

    # --- Tear down temporary cluster ---
    if [ "$CLUSTER_CREATED" -eq 1 ]; then
        echo "  Stopping temporary cluster..."
        $PGBIN/pg_ctl -D "$PGDATA" stop -m fast -w >/dev/null 2>&1 || true
        rm -rf "$PGDATA"
        echo "  Cluster removed."
    fi

    echo ""
done

echo "============================================================"
echo "  All versions complete."
echo "  Generated files:"
for v in $VERSIONS; do
    f="$REPO_DIR/BENCHMARK_PG${v}.md"
    [ -f "$f" ] && echo "    $f"
done
echo "============================================================"
