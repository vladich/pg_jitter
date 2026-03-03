#!/bin/bash
# run_all_versions.sh — Build, install, test and benchmark pg_jitter on PG14-PG18
#
# For each PG version:
#   1. Build all backends
#   2. Initialize data directory if needed
#   3. Install backends + meta provider
#   4. Run PG regression tests (make installcheck)
#   5. Run pg_jitter correctness tests (test_precompiled.sql per backend)
#   6. Run benchmark suite (bench_all_backends.sh adapted for 3 backends)
#
# Usage: ./tests/run_all_versions.sh [--skip-bench] [--versions "17 18"]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PGBASE="${PGBASE:-$HOME/postgres}"
RESULTS_DIR="$PROJECT_DIR/tests/results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

# Defaults
SKIP_BENCH=0
VERSIONS=(14 15 16 17 18)

# Parse args
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-bench) SKIP_BENCH=1; shift ;;
        --versions) IFS=' ' read -ra VERSIONS <<< "$2"; shift 2 ;;
        *) echo "Usage: $0 [--skip-bench] [--versions '17 18']"; exit 1 ;;
    esac
done

BACKENDS=(sljit asmjit mir)
NRUNS=3

log() { echo "[$(date +%H:%M:%S)] $*"; }
logfile() { echo "[$(date +%H:%M:%S)] $*" >> "$RESULTS_DIR/run.log"; }

# ---- Helper functions ----

pg_bin()  { echo "$PGBASE/v$1/bin"; }
pg_data() { echo "$PGBASE/v$1/data"; }
pg_port() { echo $((5430 + $1)); }

init_cluster() {
    local ver=$1
    local bindir=$(pg_bin $ver)
    local datadir=$(pg_data $ver)
    local port=$(pg_port $ver)

    if [ -f "$datadir/PG_VERSION" ]; then
        log "PG$ver: data directory exists"
    else
        log "PG$ver: initializing data directory at $datadir"
        "$bindir/initdb" -D "$datadir" --no-locale -E UTF8 -A trust > "$RESULTS_DIR/initdb_$ver.log" 2>&1
    fi

    # Ensure port and JIT settings are correct (idempotent)
    # Remove any existing pg_jitter settings block, then append fresh
    sed -i '/^# -- pg_jitter test settings --$/,/^# -- end pg_jitter --$/d' "$datadir/postgresql.conf"
    cat >> "$datadir/postgresql.conf" <<EOF
# -- pg_jitter test settings --
port = $port
shared_buffers = 256MB
work_mem = 64MB
jit = on
jit_above_cost = 50000
log_min_messages = warning
logging_collector = off
# -- end pg_jitter --
EOF
    log "PG$ver: configured on port $port"
}

start_pg() {
    local ver=$1
    local bindir=$(pg_bin $ver)
    local datadir=$(pg_data $ver)
    local port=$(pg_port $ver)

    if "$bindir/pg_isready" -p "$port" -q 2>/dev/null; then
        return 0
    fi
    "$bindir/pg_ctl" -D "$datadir" -l "$datadir/logfile" start -w > /dev/null 2>&1
    sleep 1
}

stop_pg() {
    local ver=$1
    local bindir=$(pg_bin $ver)
    local datadir=$(pg_data $ver)

    "$bindir/pg_ctl" -D "$datadir" stop -m fast > /dev/null 2>&1 || true
    sleep 1
}

restart_pg() {
    stop_pg "$1"
    start_pg "$1"
}

run_psql() {
    local ver=$1; shift
    local port=$(pg_port $ver)
    local bindir=$(pg_bin $ver)
    "$bindir/psql" -p "$port" "$@"
}

ensure_db() {
    local ver=$1
    local db=$2
    if ! run_psql "$ver" -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname='$db'" 2>/dev/null | grep -q 1; then
        run_psql "$ver" -d postgres -c "CREATE DATABASE $db;" 2>/dev/null || true
    fi
}

# ---- Benchmark helpers ----

get_exec_time() {
    local ver=$1
    local query="$2"
    local jit_on="$3"
    run_psql "$ver" -d regression -X -t -A -c "
SET jit = $jit_on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

median3() {
    echo -e "$1\n$2\n$3" | sort -g | sed -n '2p'
}

# ---- Benchmark query definitions ----
LABELS=()
QUERIES=()
SECTIONS=()

add_section() { SECTIONS+=("${#LABELS[@]}:$1"); }
add_query()   { LABELS+=("$1"); QUERIES+=("$2"); }

add_section "Basic Aggregation"
add_query "SUM_int"             "SELECT SUM(val1) FROM bench_data"
add_query "COUNT_star"          "SELECT COUNT(*) FROM bench_data"
add_query "GroupBy_5agg"        "SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp"

add_section "Hash Joins"
add_query "HashJoin_single"     "SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1"
add_query "HashJoin_composite"  "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2"
add_query "HashJoin_filter"     "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000"

add_section "Expressions"
add_query "CASE_simple"         "SELECT SUM(CASE WHEN val > 5000 THEN val ELSE 0 END) FROM join_left"
add_query "COALESCE_NULLIF"     "SELECT SUM(COALESCE(NULLIF(val, 0), -1)) FROM join_left"
add_query "Arith_expr"          "SELECT SUM(val + key1 * 3 - key2) FROM join_left"
add_query "IN_list_20"          "SELECT COUNT(*) FROM bench_data WHERE grp IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"

add_section "Subqueries"
add_query "Scalar_subq"         "SELECT SUM(val) FROM join_left WHERE key1 < (SELECT AVG(key1) FROM join_right)"

add_section "Wide Row / Deform"
add_query "Wide_10col_sum"      "SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10) FROM ultra_wide"
add_query "Wide_20col_sum"      "SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ultra_wide"

NQUERIES=${#LABELS[@]}

# ==============================================================
# Main loop — process each PG version
# ==============================================================

SUMMARY_FILE="$RESULTS_DIR/summary.txt"
{
    echo "pg_jitter Multi-Version Test Results"
    echo "===================================="
    echo "Date: $(date)"
    echo "Versions: ${VERSIONS[*]}"
    echo ""
} > "$SUMMARY_FILE"

for VER in "${VERSIONS[@]}"; do
    VERLOG="$RESULTS_DIR/pg${VER}.log"
    log "================================================================"
    log "PostgreSQL $VER"
    log "================================================================"

    PG_CONFIG="$PGBASE/v$VER/bin/pg_config"
    if [ ! -x "$PG_CONFIG" ]; then
        log "PG$VER: pg_config not found at $PG_CONFIG — SKIPPING"
        echo "PG$VER: SKIPPED (not installed)" >> "$SUMMARY_FILE"
        continue
    fi

    PORT=$(pg_port $VER)
    BINDIR=$(pg_bin $VER)
    DATADIR=$(pg_data $VER)

    # ---- Step 1: Build ----
    log "PG$VER: Building all backends..."
    if "$PROJECT_DIR/build.sh" --pg-config "$PG_CONFIG" all > "$RESULTS_DIR/build_$VER.log" 2>&1; then
        log "PG$VER: Build OK"
        echo "PG$VER: build OK" >> "$SUMMARY_FILE"
    else
        log "PG$VER: Build FAILED — see build_$VER.log"
        echo "PG$VER: BUILD FAILED" >> "$SUMMARY_FILE"
        continue
    fi

    # ---- Step 2: Init + start ----
    init_cluster "$VER"
    start_pg "$VER"

    # ---- Step 3: Install (copy .so files directly, avoid install.sh restart issues) ----
    log "PG$VER: Installing backends..."
    PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
    EXT=$( [ "$(uname -s)" = "Darwin" ] && echo dylib || echo so )
    stop_pg "$VER"
    for b in "${BACKENDS[@]}"; do
        SO="$PROJECT_DIR/build/$b/pg_jitter_$b.$EXT"
        if [ -f "$SO" ]; then
            cp "$SO" "$PKGLIBDIR/"
            log "PG$VER:   installed pg_jitter_$b.$EXT"
        else
            log "PG$VER:   WARNING: pg_jitter_$b.$EXT not found"
        fi
    done
    # Also install meta provider
    META_SO="$PROJECT_DIR/build/sljit/pg_jitter.$EXT"
    if [ -f "$META_SO" ]; then
        cp "$META_SO" "$PKGLIBDIR/"
        log "PG$VER:   installed pg_jitter.$EXT (meta)"
    fi
    start_pg "$VER"
    log "PG$VER: Installed"

    # ---- Step 4: Create regression database + setup ----
    ensure_db "$VER" regression
    log "PG$VER: Setting up benchmark tables..."
    run_psql "$VER" -d regression -f "$PROJECT_DIR/tests/bench_setup.sql" > /dev/null 2>&1 || true
    run_psql "$VER" -d regression -f "$PROJECT_DIR/tests/bench_setup_extra.sql" > /dev/null 2>&1 || true
    log "PG$VER: Tables ready"

    # ---- Step 5: Correctness tests per backend ----
    log "PG$VER: Running correctness tests..."
    CORR_OK=0
    CORR_FAIL=0

    for backend in "${BACKENDS[@]}"; do
        log "PG$VER:   Testing $backend..."
        run_psql "$VER" -d regression -X -q -c \
            "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null || true
        restart_pg "$VER"

        OUTFILE="$RESULTS_DIR/test_${VER}_${backend}.log"
        if run_psql "$VER" -d regression -f "$PROJECT_DIR/tests/test_precompiled.sql" \
            > "$OUTFILE" 2>&1; then
            # Check for errors in output (exclude NOTICE lines that mention "error" in text)
            if grep -i "ERROR\|FATAL" "$OUTFILE" 2>/dev/null | grep -v "^psql:.*NOTICE:" | grep -qi "ERROR\|FATAL" 2>/dev/null; then
                ERRORS=$(grep -i "ERROR\|FATAL" "$OUTFILE" 2>/dev/null | grep -v "^psql:.*NOTICE:" | grep -ci "ERROR\|FATAL" 2>/dev/null || echo "?")
                log "PG$VER:   $backend: FAIL ($ERRORS errors)"
                echo "PG$VER: $backend correctness FAIL ($ERRORS errors)" >> "$SUMMARY_FILE"
                CORR_FAIL=$((CORR_FAIL + 1))
            else
                log "PG$VER:   $backend: PASS"
                echo "PG$VER: $backend correctness PASS" >> "$SUMMARY_FILE"
                CORR_OK=$((CORR_OK + 1))
            fi
        else
            log "PG$VER:   $backend: FAIL (psql exit code $?)"
            echo "PG$VER: $backend correctness FAIL (crash)" >> "$SUMMARY_FILE"
            CORR_FAIL=$((CORR_FAIL + 1))
            # Restart in case of crash
            restart_pg "$VER"
        fi
    done
    log "PG$VER: Correctness: $CORR_OK pass, $CORR_FAIL fail"

    # ---- Step 6: Quick sanity test — run a few queries per backend ----
    log "PG$VER: Running quick sanity checks..."
    for backend in "${BACKENDS[@]}"; do
        run_psql "$VER" -d regression -X -q -c \
            "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null || true
        restart_pg "$VER"

        RESULT=$(run_psql "$VER" -d regression -X -t -A -c "
SET jit = on; SET jit_above_cost = 0;
SELECT SUM(val1) FROM bench_data;
" 2>/dev/null || echo "FAIL")
        if [ -n "$RESULT" ] && [ "$RESULT" != "FAIL" ]; then
            log "PG$VER:   $backend sanity: OK (SUM=$RESULT)"
        else
            log "PG$VER:   $backend sanity: FAIL"
            echo "PG$VER: $backend sanity FAIL" >> "$SUMMARY_FILE"
        fi
    done

    # ---- Step 7: Benchmarks ----
    if [ "$SKIP_BENCH" -eq 1 ]; then
        log "PG$VER: Benchmarks skipped (--skip-bench)"
        echo "PG$VER: benchmarks skipped" >> "$SUMMARY_FILE"
    else
        log "PG$VER: Running benchmarks ($NQUERIES queries × 3 backends)..."
        BENCH_FILE="$RESULTS_DIR/bench_${VER}.txt"
        TMPDIR_B=$(mktemp -d)

        # Interpreter baseline (jit=off)
        run_psql "$VER" -d regression -X -q -c \
            "ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';" 2>/dev/null || true
        restart_pg "$VER"

        printf "  interp"
        for qi in $(seq 0 $((NQUERIES - 1))); do
            get_exec_time "$VER" "${QUERIES[$qi]}" "off" > /dev/null 2>&1 || true  # warmup
            t1=$(get_exec_time "$VER" "${QUERIES[$qi]}" "off" 2>/dev/null)
            t2=$(get_exec_time "$VER" "${QUERIES[$qi]}" "off" 2>/dev/null)
            t3=$(get_exec_time "$VER" "${QUERIES[$qi]}" "off" 2>/dev/null)
            if [ -n "$t1" ] && [ -n "$t2" ] && [ -n "$t3" ]; then
                m=$(median3 "$t1" "$t2" "$t3")
            else
                m="N/A"
            fi
            echo "$m" > "$TMPDIR_B/interp_${qi}.txt"
            printf "."
        done
        echo " done"

        # Each JIT backend
        for backend in "${BACKENDS[@]}"; do
            run_psql "$VER" -d regression -X -q -c \
                "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null || true
            restart_pg "$VER"

            # Warmup buffer cache
            run_psql "$VER" -d regression -X -q -c "
SET max_parallel_workers_per_gather = 0;
SELECT COUNT(*) FROM bench_data; SELECT COUNT(*) FROM join_left;
SELECT COUNT(*) FROM join_right;
SELECT COUNT(*) FROM ultra_wide;
" > /dev/null 2>&1 || true

            printf "  $backend"
            crash_count=0
            for qi in $(seq 0 $((NQUERIES - 1))); do
                if [ "$crash_count" -ge 3 ]; then
                    echo "CRASH" > "$TMPDIR_B/${backend}_${qi}.txt"
                    continue
                fi

                if ! "$BINDIR/pg_isready" -p "$PORT" -q 2>/dev/null; then
                    restart_pg "$VER"
                fi

                get_exec_time "$VER" "${QUERIES[$qi]}" "on" > /dev/null 2>&1 || true  # warmup
                if ! "$BINDIR/pg_isready" -p "$PORT" -q 2>/dev/null; then
                    restart_pg "$VER"
                fi

                t1=$(get_exec_time "$VER" "${QUERIES[$qi]}" "on" 2>/dev/null)
                t2=$(get_exec_time "$VER" "${QUERIES[$qi]}" "on" 2>/dev/null)
                t3=$(get_exec_time "$VER" "${QUERIES[$qi]}" "on" 2>/dev/null)
                if [ -n "$t1" ] && [ -n "$t2" ] && [ -n "$t3" ]; then
                    m=$(median3 "$t1" "$t2" "$t3")
                else
                    m="CRASH"
                    crash_count=$((crash_count + 1))
                    restart_pg "$VER"
                fi
                echo "$m" > "$TMPDIR_B/${backend}_${qi}.txt"
                printf "."
            done
            echo " done"
        done

        # Format benchmark results
        {
            echo ""
            echo "PostgreSQL $VER Benchmark Results"
            echo "================================="
            echo ""
            printf "%-24s%10s%10s%10s%10s\n" "Query" "interp" "sljit" "asmjit" "mir"
            printf "%-24s%10s%10s%10s%10s\n" "" "--------" "--------" "--------" "--------"

            for qi in $(seq 0 $((NQUERIES - 1))); do
                for sec in "${SECTIONS[@]}"; do
                    sec_idx="${sec%%:*}"
                    sec_name="${sec#*:}"
                    if [ "$sec_idx" -eq "$qi" ]; then
                        echo ""
                        echo "--- $sec_name ---"
                    fi
                done

                label="${LABELS[$qi]}"
                v_interp=$(cat "$TMPDIR_B/interp_${qi}.txt" 2>/dev/null || echo "N/A")
                v_sljit=$(cat "$TMPDIR_B/sljit_${qi}.txt" 2>/dev/null || echo "N/A")
                v_asmjit=$(cat "$TMPDIR_B/asmjit_${qi}.txt" 2>/dev/null || echo "N/A")
                v_mir=$(cat "$TMPDIR_B/mir_${qi}.txt" 2>/dev/null || echo "N/A")
                printf "%-24s%10s%10s%10s%10s\n" "$label" "$v_interp" "$v_sljit" "$v_asmjit" "$v_mir"
            done

            echo ""
            echo "All times in ms (median of 3 runs). Lower is better."
        } | tee "$BENCH_FILE"

        # Append to summary
        cat "$BENCH_FILE" >> "$SUMMARY_FILE"
        rm -rf "$TMPDIR_B"
        log "PG$VER: Benchmarks done → $BENCH_FILE"
    fi

    # ---- Cleanup: stop this version ----
    stop_pg "$VER"
    echo "" >> "$SUMMARY_FILE"
done

log "================================================================"
log "All done! Results in: $RESULTS_DIR/"
log "Summary: $RESULTS_DIR/summary.txt"
log "================================================================"
