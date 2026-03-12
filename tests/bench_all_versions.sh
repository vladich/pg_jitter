#!/bin/bash
# bench_all_versions.sh — Run OLTP + OLAP benchmarks on all PG versions
#
# Creates a temporary cluster per version, runs both benchmarks, then
# destroys the cluster before moving to the next version.
# This keeps disk usage manageable (~2GB peak).
#
# Usage:
#   DURATION=60 WARMUP=15 ./tests/bench_all_versions.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DURATION=${DURATION:-60}
WARMUP=${WARMUP:-15}
SCALE=${SCALE:-3}
VERSIONS="${VERSIONS:-14 15 16 17 18}"
BACKENDS="nojit sljit asmjit mir auto"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTDIR="$SCRIPT_DIR/allver_${TIMESTAMP}"
mkdir -p "$OUTDIR"

echo "============================================================"
echo "  pg_jitter Multi-Version Benchmark Suite"
echo "============================================================"
echo "  Versions:  $VERSIONS"
echo "  Backends:  $BACKENDS"
echo "  Duration:  ${DURATION}s per backend"
echo "  Warmup:    ${WARMUP}s"
echo "  Scale:     ${SCALE}"
echo "  Output:    $OUTDIR"
echo "============================================================"
echo ""

for v in $VERSIONS; do
    PORT="50${v}"
    PGBIN=~/postgres_install/v${v}/bin
    PGCONFIG="$PGBIN/pg_config"
    PGDATA="/tmp/pg_jitter_bench_v${v}"

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
        PGDATA=$($PGBIN/psql -p "$PORT" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null)
        echo "  Using existing cluster at $PGDATA (port $PORT)"
    fi

    # Verify connectivity
    VER=$($PGBIN/psql -p "$PORT" -d postgres -t -A -c "SELECT version();" 2>/dev/null)
    echo "  $VER"
    echo ""

    # --- Run OLTP ---
    echo ">>> OLTP benchmark for PG${v}..."
    OLTP_DIR="$OUTDIR/pg${v}_oltp"
    bash "$SCRIPT_DIR/bench_ecommerce_sustained.sh" \
        --pg-config "$PGCONFIG" \
        --port "$PORT" \
        --duration "$DURATION" \
        --warmup "$WARMUP" \
        --backends "$BACKENDS" \
        --outdir "$OLTP_DIR" \
        --scale "$SCALE" \
        --clients 8 \
        --threads 4 \
        2>&1 | tee "$OUTDIR/pg${v}_oltp.log"

    # Drop OLTP database to save space before OLAP
    $PGBIN/psql -p "$PORT" -d postgres -q -c "DROP DATABASE IF EXISTS bench_ecommerce;" 2>/dev/null || true

    # --- Run OLAP ---
    echo ""
    echo ">>> OLAP benchmark for PG${v}..."
    OLAP_DIR="$OUTDIR/pg${v}_olap"
    bash "$SCRIPT_DIR/bench_olap_sustained.sh" \
        --pg-config "$PGCONFIG" \
        --port "$PORT" \
        --duration "$DURATION" \
        --warmup "$WARMUP" \
        --backends "$BACKENDS" \
        --outdir "$OLAP_DIR" \
        --scale "$SCALE" \
        --clients 4 \
        --threads 4 \
        2>&1 | tee "$OUTDIR/pg${v}_olap.log"

    # Drop OLAP database to save space
    $PGBIN/psql -p "$PORT" -d postgres -q -c "DROP DATABASE IF EXISTS bench_olap;" 2>/dev/null || true

    # --- Tear down temporary cluster ---
    if [ "$CLUSTER_CREATED" -eq 1 ]; then
        echo "  Stopping temporary cluster..."
        $PGBIN/pg_ctl -D "$PGDATA" stop -m fast -w >/dev/null 2>&1 || true
        rm -rf "$PGDATA"
        echo "  Cluster removed."
    fi

    echo ""
    echo "  PG${v} done. Free disk: $(df -h / | awk 'NR==2{print $4}')"
    echo ""
done

# ================================================================
# Aggregate summary
# ================================================================
SUMMARY="$OUTDIR/summary_all.txt"

{
echo ""
echo "================================================================"
echo "  pg_jitter Multi-Version Benchmark Summary"
echo "  $(date)"
echo "  Duration: ${DURATION}s | Scale: ${SCALE} | Versions: $VERSIONS"
echo "================================================================"

for bench in oltp olap; do
    echo ""
    if [ "$bench" = "oltp" ]; then
        echo "  OLTP (E-Commerce Mixed Workload)"
    else
        echo "  OLAP (Star Schema Analytical)"
    fi
    echo "  ──────────────────────────────────────────────────────────────────────────────"
    echo ""
    printf "%-6s  " "PG"
    for b in $BACKENDS; do
        printf "%10s" "$b"
    done
    printf "  %10s\n" "best%"
    printf "%-6s  " "----"
    for b in $BACKENDS; do
        printf "%10s" "--------"
    done
    printf "  %10s\n" "--------"

    for v in $VERSIONS; do
        dir="$OUTDIR/pg${v}_${bench}"
        printf "PG%-4s  " "$v"

        nojit_tps=""
        for b in $BACKENDS; do
            logf="$dir/${b}_pgbench.log"
            if [ -f "$logf" ]; then
                tps=$(grep "^tps" "$logf" 2>/dev/null | head -1 | awk '{printf "%.1f", $3}')
                if [ "$b" = "nojit" ]; then
                    nojit_tps="$tps"
                fi
                printf "%10s" "$tps"
            else
                printf "%10s" "n/a"
            fi
        done

        # Best JIT improvement
        if [ -n "$nojit_tps" ] && [ "$nojit_tps" != "0" ] && [ "$nojit_tps" != "0.0" ]; then
            best=""
            for b in sljit asmjit mir auto; do
                logf="$dir/${b}_pgbench.log"
                if [ -f "$logf" ]; then
                    t=$(grep "^tps" "$logf" 2>/dev/null | head -1 | awk '{print $3}')
                    if [ -n "$t" ]; then
                        pct=$(echo "scale=1; ($t / $nojit_tps - 1) * 100" | bc 2>/dev/null)
                        if [ -z "$best" ] || [ "$(echo "$pct > $best" | bc 2>/dev/null)" = "1" ]; then
                            best="$pct"
                        fi
                    fi
                fi
            done
            [ -n "$best" ] && printf "  %+9s%%" "$best"
        fi
        echo ""
    done
done

echo ""
echo "TPS values. Higher = better."
echo "best% = best JIT backend improvement over nojit baseline."
echo ""
echo "Output: $OUTDIR"
} | tee "$SUMMARY"

echo ""
echo "Full summary: $SUMMARY"
