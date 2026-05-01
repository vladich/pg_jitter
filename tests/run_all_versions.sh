#!/usr/bin/env bash
# run_all_versions.sh - Build, install, regress, test, and benchmark PG14-PG18.
#
# For each PG version:
#   1. Build all backends
#   2. Initialize data directory if needed
#   3. Install backends + meta provider
#   4. Run full PostgreSQL pg_regress through tests/run_pg_regress.sh
#   5. Run pg_jitter correctness tests per backend
#   6. Run benchmark suite (bench_all_backends.sh adapted for 3 backends)
#
# Usage:
#   ./tests/run_all_versions.sh [--skip-regress] [--skip-bench] [--versions "17 18"]
#
# Default local layout:
#   PG binaries: ~/postgres_install/v17/bin/pg_config
#   PG source:   ~/postgres_versions/v17
#   PG data:     /tmp/pg_jitter_all_versions/v17
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_INSTALL_BASE="${PG_INSTALL_BASE:-${PGBASE:-$HOME/postgres_install}}"
PG_SRC_BASE="${PG_SRC_BASE:-$HOME/postgres_versions}"
PGDATA_BASE="${PGDATA_BASE:-/tmp/pg_jitter_all_versions}"
RESULTS_DIR="$PROJECT_DIR/tests/results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

# Defaults
SKIP_REGRESS=0
SKIP_BENCH=0
VERSIONS=(14 15 16 17 18)

case "$(uname -m)" in
    x86_64|amd64|AMD64|aarch64|arm64)
        BACKENDS=(sljit asmjit mir)
        ;;
    *)
        BACKENDS=(sljit mir)
        ;;
esac
REGRESS_BACKENDS_OVERRIDE="${REGRESS_BACKENDS:-}"

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --versions "17 18"       Space-separated PostgreSQL major versions
  --pg-install-base DIR    Base for installed binaries (default: $PG_INSTALL_BASE)
  --pg-src-base DIR        Base for PostgreSQL source trees (default: $PG_SRC_BASE)
  --pgdata-base DIR        Base for local test clusters (default: $PGDATA_BASE)
  --regress-backends LIST  Backends for full pg_regress
                           (default: direct backends, plus auto on PG17+)
  --skip-regress           Skip full PostgreSQL pg_regress
  --skip-bench             Skip benchmark section
  -h, --help               Show this help
EOF
}

# Parse args
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-regress) SKIP_REGRESS=1; shift ;;
        --skip-bench) SKIP_BENCH=1; shift ;;
        --versions) IFS=' ' read -ra VERSIONS <<< "$2"; shift 2 ;;
        --pg-install-base) PG_INSTALL_BASE="$2"; shift 2 ;;
        --pg-src-base) PG_SRC_BASE="$2"; shift 2 ;;
        --pgdata-base) PGDATA_BASE="$2"; shift 2 ;;
        --regress-backends) REGRESS_BACKENDS_OVERRIDE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

NRUNS=3
TOTAL_FAILURES=0

log() { echo "[$(date +%H:%M:%S)] $*"; }
logfile() { echo "[$(date +%H:%M:%S)] $*" >> "$RESULTS_DIR/run.log"; }

# ---- Helper functions ----

pg_bin()  { echo "$PG_INSTALL_BASE/v$1/bin"; }
pg_src()  { echo "$PG_SRC_BASE/v$1"; }
pg_data() { echo "$PGDATA_BASE/v$1"; }
pg_port() { echo $((5430 + $1)); }

pg_env() {
    local ver=$1
    shift
    local pg_config="$(pg_bin "$ver")/pg_config"
    local libdir dynlib_var path_sep dynlib_path current

    libdir="$("$pg_config" --libdir)"
    case "$(uname -s)" in
        Darwin) dynlib_var=DYLD_LIBRARY_PATH; path_sep=: ;;
        MINGW*|MSYS*|CYGWIN*) dynlib_var=PATH; path_sep=';' ;;
        *) dynlib_var=LD_LIBRARY_PATH; path_sep=: ;;
    esac

    dynlib_path="$libdir"
    if [ -d "$libdir/postgresql" ] && [ "$libdir/postgresql" != "$libdir" ]; then
        dynlib_path="$libdir/postgresql$path_sep$dynlib_path"
    fi

    current="${!dynlib_var:-}"
    if [ -n "$current" ]; then
        env "$dynlib_var=$dynlib_path$path_sep$current" "$@"
    else
        env "$dynlib_var=$dynlib_path" "$@"
    fi
}

init_cluster() {
    local ver=$1
    local bindir=$(pg_bin $ver)
    local datadir=$(pg_data $ver)
    local port=$(pg_port $ver)

    if [ -f "$datadir/PG_VERSION" ]; then
        log "PG$ver: data directory exists"
    else
        log "PG$ver: initializing data directory at $datadir"
        mkdir -p "$(dirname "$datadir")"
        pg_env "$ver" "$bindir/initdb" -D "$datadir" --no-locale -E UTF8 -A trust > "$RESULTS_DIR/initdb_$ver.log" 2>&1
    fi

    # Ensure port and JIT settings are correct (idempotent)
    # Remove any existing pg_jitter settings block, then append fresh
    sed -i.bak '/^# -- pg_jitter test settings --$/,/^# -- end pg_jitter --$/d' "$datadir/postgresql.conf"
    rm -f "$datadir/postgresql.conf.bak"
    cat >> "$datadir/postgresql.conf" <<EOF
# -- pg_jitter test settings --
port = $port
unix_socket_directories = '/tmp'
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

    if pg_env "$ver" "$bindir/pg_isready" -p "$port" -q 2>/dev/null; then
        return 0
    fi
    pg_env "$ver" "$bindir/pg_ctl" -D "$datadir" -l "$datadir/logfile" start -w > /dev/null 2>&1
    sleep 1
}

stop_pg() {
    local ver=$1
    local bindir=$(pg_bin $ver)
    local datadir=$(pg_data $ver)

    pg_env "$ver" "$bindir/pg_ctl" -D "$datadir" stop -m fast -w -t 30 > /dev/null 2>&1 ||
        pg_env "$ver" "$bindir/pg_ctl" -D "$datadir" stop -m immediate -w -t 30 > /dev/null 2>&1 ||
        true
    sleep 1
}

restart_pg() {
    stop_pg "$1"
    start_pg "$1"
}

set_jit_backend() {
    local ver=$1
    local backend=$2

    if [ "$ver" -ge 17 ]; then
        run_psql "$ver" -d postgres -X -q -c \
            "ALTER SYSTEM SET jit_provider = 'pg_jitter';" 2>/dev/null || true
        run_psql "$ver" -d postgres -X -q -c \
            "ALTER SYSTEM SET pg_jitter.backend = '$backend';" 2>/dev/null || true
    else
        run_psql "$ver" -d postgres -X -q -c \
            "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null || true
    fi
}

run_psql() {
    local ver=$1; shift
    local port=$(pg_port $ver)
    local bindir=$(pg_bin $ver)
    pg_env "$ver" "$bindir/psql" -p "$port" "$@"
}

provider_modes_for_version() {
    local ver=$1
    printf '%s\n' "${BACKENDS[@]}"
    if [ "$ver" -ge 17 ]; then
        printf '%s\n' auto
    fi
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

make_in_list() {
    python3 - "$1" <<'PY'
import sys
n = int(sys.argv[1])
print(",".join(str(i) for i in range(1, n + 1)))
PY
}

IN_LIST_128="$(make_in_list 128)"
IN_LIST_4096="$(make_in_list 4096)"
IN_LIST_4097="$(make_in_list 4097)"
IN_LIST_5000="$(make_in_list 5000)"
IN_LIST_5000_NULL="NULL,$IN_LIST_5000"

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
add_query "IN_expr_20"          "SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"
add_query "IN_expr_128"         "SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN ($IN_LIST_128)"
add_query "IN_expr_4096"        "SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN ($IN_LIST_4096)"
add_query "IN_expr_4097"        "SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN ($IN_LIST_4097)"
add_query "IN_expr_5000_null"   "SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN ($IN_LIST_5000_NULL)"
add_query "IN_index_20"         "SELECT COUNT(*) FROM bench_data WHERE grp IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"

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
    echo "PG install base: $PG_INSTALL_BASE"
    echo "PG source base:  $PG_SRC_BASE"
    echo "PG data base:    $PGDATA_BASE"
    echo "Backends:        ${BACKENDS[*]}"
    echo "Regress override:${REGRESS_BACKENDS_OVERRIDE:-<version default>}"
    echo ""
} > "$SUMMARY_FILE"

for VER in "${VERSIONS[@]}"; do
    VERLOG="$RESULTS_DIR/pg${VER}.log"
    log "================================================================"
    log "PostgreSQL $VER"
    log "================================================================"

    PG_CONFIG="$(pg_bin "$VER")/pg_config"
    if [ ! -x "$PG_CONFIG" ]; then
        log "PG$VER: pg_config not found at $PG_CONFIG — SKIPPING"
        echo "PG$VER: SKIPPED (not installed)" >> "$SUMMARY_FILE"
        continue
    fi
    PG_SRC="$(pg_src "$VER")"
    if [ ! -d "$PG_SRC/src/test/regress" ]; then
        log "PG$VER: PostgreSQL source not found at $PG_SRC — SKIPPING"
        echo "PG$VER: SKIPPED (source not found)" >> "$SUMMARY_FILE"
        TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        continue
    fi

    PORT=$(pg_port $VER)
    BINDIR=$(pg_bin $VER)
    DATADIR=$(pg_data $VER)
    if [ -n "$REGRESS_BACKENDS_OVERRIDE" ]; then
        REGRESS_BACKENDS_FOR_VER="$REGRESS_BACKENDS_OVERRIDE"
    elif [ "$VER" -ge 17 ]; then
        REGRESS_BACKENDS_FOR_VER="${BACKENDS[*]} auto"
    else
        REGRESS_BACKENDS_FOR_VER="${BACKENDS[*]}"
    fi

    # ---- Step 1: Build ----
    log "PG$VER: Building all backends..."
    BACKENDS_CMAKE="$(IFS=';'; echo "${BACKENDS[*]}")"
    if "$PROJECT_DIR/build.sh" --pg-config "$PG_CONFIG" all \
        "-DPG_JITTER_BACKENDS=$BACKENDS_CMAKE" \
        > "$RESULTS_DIR/build_$VER.log" 2>&1; then
        log "PG$VER: Build OK"
        echo "PG$VER: build OK" >> "$SUMMARY_FILE"
    else
        log "PG$VER: Build FAILED — see build_$VER.log"
        echo "PG$VER: BUILD FAILED" >> "$SUMMARY_FILE"
        TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        continue
    fi

    # ---- Step 2: Init + start ----
    init_cluster "$VER"
    start_pg "$VER"

    # ---- Step 3: Install (copy .so files directly, avoid install.sh restart issues) ----
    log "PG$VER: Installing backends..."
    PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
    PG_LIBDIR="$("$PG_CONFIG" --libdir)"
    INSTALL_DIRS=("$PKGLIBDIR")
    if [ -d "$PG_LIBDIR/postgresql" ] && [ "$PG_LIBDIR/postgresql" != "$PKGLIBDIR" ]; then
        INSTALL_DIRS+=("$PG_LIBDIR/postgresql")
    fi
    EXT=$( [ "$(uname -s)" = "Darwin" ] && echo dylib || echo so )
    stop_pg "$VER"
    BUILD_DIR="$PROJECT_DIR/build/pg$VER"
    shopt -s nullglob
    BUILT_LIBS=("$BUILD_DIR"/pg_jitter*."$EXT")
    shopt -u nullglob
    if [ "${#BUILT_LIBS[@]}" -eq 0 ]; then
        log "PG$VER: Install FAILED — no pg_jitter libraries in $BUILD_DIR"
        echo "PG$VER: INSTALL FAILED" >> "$SUMMARY_FILE"
        TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        continue
    fi
    for SO in "${BUILT_LIBS[@]}"; do
        for INSTALL_DIR in "${INSTALL_DIRS[@]}"; do
            cp "$SO" "$INSTALL_DIR/"
        done
        log "PG$VER:   installed $(basename "$SO") to ${INSTALL_DIRS[*]}"
    done
    start_pg "$VER"
    log "PG$VER: Installed"

    # ---- Step 4: Full PostgreSQL regression tests ----
    if [ "$SKIP_REGRESS" -eq 1 ]; then
        log "PG$VER: Full pg_regress skipped (--skip-regress)"
        echo "PG$VER: pg_regress skipped" >> "$SUMMARY_FILE"
    else
        REGRESS_OUT="$RESULTS_DIR/pg${VER}_pg_regress"
        REGRESS_LOG="$RESULTS_DIR/pg_regress_$VER.log"
        log "PG$VER: Running full pg_regress for backends: $REGRESS_BACKENDS_FOR_VER"
        stop_pg "$VER"
        if pg_env "$VER" "$PROJECT_DIR/tests/run_pg_regress.sh" \
            --pg-config "$PG_CONFIG" \
            --pg-src "$PG_SRC" \
            --port "$PORT" \
            --fresh-cluster \
            --backends "$REGRESS_BACKENDS_FOR_VER" \
            --output-dir "$REGRESS_OUT" \
            > "$REGRESS_LOG" 2>&1; then
            log "PG$VER: pg_regress PASS"
            echo "PG$VER: pg_regress PASS" >> "$SUMMARY_FILE"
        else
            log "PG$VER: pg_regress FAIL — see $REGRESS_LOG"
            echo "PG$VER: pg_regress FAIL" >> "$SUMMARY_FILE"
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        fi
        start_pg "$VER"
    fi

    # ---- Step 5: Create regression database + optional benchmark setup ----
    ensure_db "$VER" regression
    if [ "$SKIP_BENCH" -eq 0 ]; then
        log "PG$VER: Setting up benchmark tables..."
        run_psql "$VER" -d regression -f "$PROJECT_DIR/tests/bench_setup.sql" > /dev/null 2>&1 || true
        run_psql "$VER" -d regression -f "$PROJECT_DIR/tests/bench_setup_extra.sql" > /dev/null 2>&1 || true
        log "PG$VER: Tables ready"
    else
        log "PG$VER: Benchmark table setup skipped (--skip-bench)"
    fi

    # ---- Step 6: Correctness tests per backend ----
    log "PG$VER: Running correctness tests..."
    CORR_OK=0
    CORR_FAIL=0
    PROVIDER_MODES=()
    while IFS= read -r provider_mode; do
        PROVIDER_MODES+=("$provider_mode")
    done < <(provider_modes_for_version "$VER")

    for backend in "${PROVIDER_MODES[@]}"; do
        log "PG$VER:   Testing $backend..."
        set_jit_backend "$VER" "$backend"
        if ! restart_pg "$VER"; then
            log "PG$VER:   $backend: FAIL (restart failed before correctness)"
            echo "PG$VER: $backend correctness FAIL (restart)" >> "$SUMMARY_FILE"
            CORR_FAIL=$((CORR_FAIL + 1))
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
            continue
        fi

        BUG_OUTFILE="$RESULTS_DIR/test_${VER}_${backend}_provider_regressions.log"
        if ! pg_env "$VER" "$PROJECT_DIR/tests/test_provider_regressions.sh" \
            --pg-config "$(pg_bin "$VER")/pg_config" \
            --host /tmp \
            --port "$PORT" \
            --db regression \
            --backend "$backend" \
            > "$BUG_OUTFILE" 2>&1; then
            log "PG$VER:   $backend: FAIL (targeted provider regressions)"
            echo "PG$VER: $backend provider regressions FAIL" >> "$SUMMARY_FILE"
            CORR_FAIL=$((CORR_FAIL + 1))
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
            restart_pg "$VER" || true
            continue
        fi

        SURFACE_OUTFILE="$RESULTS_DIR/test_${VER}_${backend}_function_surface.log"
        if ! pg_env "$VER" "$PROJECT_DIR/tests/test_function_surface.sh" \
            --pg-config "$(pg_bin "$VER")/pg_config" \
            --host /tmp \
            --port "$PORT" \
            --db regression \
            --backend "$backend" \
            > "$SURFACE_OUTFILE" 2>&1; then
            log "PG$VER:   $backend: FAIL (function surface regressions)"
            echo "PG$VER: $backend function surface FAIL" >> "$SUMMARY_FILE"
            CORR_FAIL=$((CORR_FAIL + 1))
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
            restart_pg "$VER" || true
            continue
        fi

        log "PG$VER:   $backend: PASS"
        echo "PG$VER: $backend correctness PASS" >> "$SUMMARY_FILE"
        CORR_OK=$((CORR_OK + 1))
    done
    log "PG$VER: Correctness: $CORR_OK pass, $CORR_FAIL fail"

    # ---- Step 7: Quick sanity test — run a self-contained query per backend ----
    log "PG$VER: Running quick sanity checks..."
    for backend in "${PROVIDER_MODES[@]}"; do
        set_jit_backend "$VER" "$backend"
        if ! restart_pg "$VER"; then
            log "PG$VER:   $backend sanity: FAIL (restart)"
            echo "PG$VER: $backend sanity FAIL (restart)" >> "$SUMMARY_FILE"
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
            continue
        fi

        RESULT=$(run_psql "$VER" -d regression -X -q -t -A -c "
	SET jit = on; SET jit_above_cost = 0;
	SELECT SUM(i) FROM generate_series(1,1000) AS g(i) WHERE i > 10;
	" 2>/dev/null | tail -n 1 || echo "FAIL")
        if [ -n "$RESULT" ] && [ "$RESULT" != "FAIL" ]; then
            log "PG$VER:   $backend sanity: OK (SUM=$RESULT)"
        else
            log "PG$VER:   $backend sanity: FAIL"
            echo "PG$VER: $backend sanity FAIL" >> "$SUMMARY_FILE"
            TOTAL_FAILURES=$((TOTAL_FAILURES + 1))
        fi
    done

    # ---- Step 8: Benchmarks ----
    if [ "$SKIP_BENCH" -eq 1 ]; then
        log "PG$VER: Benchmarks skipped (--skip-bench)"
        echo "PG$VER: benchmarks skipped" >> "$SUMMARY_FILE"
    else
        log "PG$VER: Running benchmarks ($NQUERIES queries × 3 backends)..."
        BENCH_FILE="$RESULTS_DIR/bench_${VER}.txt"
        TMPDIR_B=$(mktemp -d)

        # Interpreter baseline (jit=off)
        set_jit_backend "$VER" "sljit"
        restart_pg "$VER" || true

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
            set_jit_backend "$VER" "$backend"
            restart_pg "$VER" || true

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

                if ! pg_env "$VER" "$BINDIR/pg_isready" -p "$PORT" -q 2>/dev/null; then
                    restart_pg "$VER" || true
                fi

                get_exec_time "$VER" "${QUERIES[$qi]}" "on" > /dev/null 2>&1 || true  # warmup
                if ! pg_env "$VER" "$BINDIR/pg_isready" -p "$PORT" -q 2>/dev/null; then
                    restart_pg "$VER" || true
                fi

                t1=$(get_exec_time "$VER" "${QUERIES[$qi]}" "on" 2>/dev/null)
                t2=$(get_exec_time "$VER" "${QUERIES[$qi]}" "on" 2>/dev/null)
                t3=$(get_exec_time "$VER" "${QUERIES[$qi]}" "on" 2>/dev/null)
                if [ -n "$t1" ] && [ -n "$t2" ] && [ -n "$t3" ]; then
                    m=$(median3 "$t1" "$t2" "$t3")
                else
                    m="CRASH"
                    crash_count=$((crash_count + 1))
                    restart_pg "$VER" || true
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

exit "$TOTAL_FAILURES"
