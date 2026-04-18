#!/usr/bin/env bash
# run_pg_regress.sh - Run PostgreSQL pg_regress for pg_jitter backends.
#
# Typical local use with an existing server:
#   ./tests/run_pg_regress.sh \
#     --pg-config ~/postgres_install/v17/bin/pg_config \
#     --pg-src ~/postgres_versions/v17 \
#     --port 5433 \
#     --backends "sljit asmjit mir auto"
#
# Safer isolated use: initialize a fresh temporary cluster per backend.
#   ./tests/run_pg_regress.sh \
#     --pg-config ~/postgres_install/v17/bin/pg_config \
#     --pg-src ~/postgres_versions/v17 \
#     --port 5433 \
#     --fresh-cluster
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGHOST="${PGHOST:-/tmp}"
PG_SRC="${PG_SRC:-}"
BACKENDS="${BACKENDS:-}"
FRESH_CLUSTER=0
KEEP_CLUSTER=0
PGDATA_OVERRIDE="${PGDATA:-}"
OUTPUT_DIR=""
MAX_CONCURRENT_TESTS=20
MAX_CONNECTIONS=1
SCHEDULE="parallel_schedule"

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --pg-config PATH       Path to pg_config (default: PG_CONFIG or pg_config)
  --pg-src DIR           PostgreSQL source tree with src/test/regress
  --port PORT            PostgreSQL port (default: PGPORT or 5433)
  --host HOST            pg_regress/psql host (default: PGHOST or /tmp)
  --backends LIST        Space-separated backend list
                         (default: direct backends, plus auto on PG17+)
  --fresh-cluster        Reinitialize a temporary cluster for each backend
  --datadir DIR          Data directory for --fresh-cluster
                         (default: /tmp/pg_jitter_regress_pgMAJOR_PORT)
  --keep-cluster         Do not stop/remove the fresh cluster on exit
  --output-dir DIR       Directory for per-backend pg_regress artifacts
  --schedule FILE        Schedule filename/path (default: parallel_schedule)
  --max-concurrent-tests N
  --max-connections N
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2 ;;
        --pg-src) PG_SRC="$2"; shift 2 ;;
        --port) PGPORT="$2"; shift 2 ;;
        --host) PGHOST="$2"; shift 2 ;;
        --backends) BACKENDS="$2"; shift 2 ;;
        --fresh-cluster) FRESH_CLUSTER=1; shift ;;
        --datadir) PGDATA_OVERRIDE="$2"; shift 2 ;;
        --keep-cluster) KEEP_CLUSTER=1; shift ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --schedule) SCHEDULE="$2"; shift 2 ;;
        --max-concurrent-tests) MAX_CONCURRENT_TESTS="$2"; shift 2 ;;
        --max-connections) MAX_CONNECTIONS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [ -x "$PG_CONFIG" ]; then
    PG_CONFIG="$(cd "$(dirname "$PG_CONFIG")" && pwd)/$(basename "$PG_CONFIG")"
elif command -v "$PG_CONFIG" >/dev/null 2>&1; then
    PG_CONFIG="$(command -v "$PG_CONFIG")"
else
    echo "ERROR: pg_config not found: $PG_CONFIG" >&2
    exit 1
fi

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PG_LIBDIR="$("$PG_CONFIG" --libdir)"
PKGLIB_CANDIDATES=("$PKGLIBDIR")
if [ -d "$PG_LIBDIR/postgresql" ] && [ "$PG_LIBDIR/postgresql" != "$PKGLIBDIR" ]; then
    PKGLIB_CANDIDATES+=("$PG_LIBDIR/postgresql")
fi
PSQL="$PGBIN/psql"
PGCTL="$PGBIN/pg_ctl"
INITDB="$PGBIN/initdb"
PG_MAJOR="$("$PG_CONFIG" --version | sed -E 's/PostgreSQL ([0-9]+).*/\1/')"

if [ -z "$BACKENDS" ]; then
    case "$(uname -m)" in
        x86_64|amd64|AMD64|aarch64|arm64)
            BACKENDS="sljit asmjit mir"
            ;;
        *)
            BACKENDS="sljit mir"
            ;;
    esac
    if [ "$PG_MAJOR" -ge 17 ]; then
        BACKENDS="$BACKENDS auto"
    fi
fi

if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="/tmp/pg_jitter_regress_results_pg${PG_MAJOR}_${PGPORT}_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
SUMMARY_FILE="$OUTPUT_DIR/summary.txt"
: > "$SUMMARY_FILE"

psql_postgres() {
    "$PSQL" -h "$PGHOST" -p "$PGPORT" -d postgres -X "$@"
}

pg_ready() {
    "$PGBIN/pg_isready" -h "$PGHOST" -p "$PGPORT" -q >/dev/null 2>&1
}

log_summary() {
    printf '%s\n' "$*" | tee -a "$SUMMARY_FILE"
}

# Auto-detect PostgreSQL source tree.
if [ -z "$PG_SRC" ]; then
    PG_INSTALL_PREFIX="$("$PG_CONFIG" --prefix 2>/dev/null || echo "")"
    for candidate in \
        "$HOME/postgres_versions/v${PG_MAJOR}" \
        "$HOME/postgres_versions/${PG_MAJOR}" \
        "$REPO_DIR/../postgres_versions/v${PG_MAJOR}" \
        "$REPO_DIR/../postgres-jit" \
        "$REPO_DIR/../postgresql" \
        "$REPO_DIR/../postgres" \
        "$PG_INSTALL_PREFIX"
    do
        if [ -n "$candidate" ] && [ -f "$candidate/src/test/regress/pg_regress" ]; then
            PG_SRC="$candidate"
            break
        fi
    done
fi

if [ -z "$PG_SRC" ] || [ ! -d "$PG_SRC/src/test/regress" ]; then
    echo "ERROR: PostgreSQL source tree not found." >&2
    echo "Use --pg-src /path/to/postgres or export PG_SRC." >&2
    exit 1
fi

REGRESS_DIR_ORIG="$PG_SRC/src/test/regress"
REGRESS_DIR="/tmp/pg_jitter_regress_work_pg${PG_MAJOR}_${PGPORT}"

refresh_regress_workdir() {
    local needs_copy=0

    if [ ! -d "$REGRESS_DIR" ]; then
        needs_copy=1
    elif [ "$REGRESS_DIR_ORIG/pg_regress" -nt "$REGRESS_DIR/pg_regress" ]; then
        needs_copy=1
    elif [ "$REGRESS_DIR_ORIG/parallel_schedule" -nt "$REGRESS_DIR/parallel_schedule" ]; then
        needs_copy=1
    fi

    if [ "$needs_copy" -eq 1 ]; then
        rm -rf "$REGRESS_DIR"
        cp -a "$REGRESS_DIR_ORIG" "$REGRESS_DIR"
    fi

    mkdir -p "$REGRESS_DIR/testtablespace"
}

find_pg_regress() {
    local pgxs candidate

    pgxs="$("$PG_CONFIG" --pgxs 2>/dev/null || true)"
    for candidate in \
        "${pgxs:+$(printf '%s' "$pgxs" | sed 's|/pgxs/.*|/pgxs/src/test/regress/pg_regress|')}" \
        "$REGRESS_DIR_ORIG/pg_regress" \
        "$PGBIN/pg_regress" \
        "$PGBIN/../lib/pgxs/src/test/regress/pg_regress" \
        "$PKGLIBDIR/pgxs/src/test/regress/pg_regress"
    do
        if [ -n "$candidate" ] && [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

refresh_regress_workdir
PG_REGRESS="$(find_pg_regress)" || {
    echo "ERROR: pg_regress not found." >&2
    exit 1
}

schedule_path() {
    if [[ "$SCHEDULE" = /* ]]; then
        printf '%s\n' "$SCHEDULE"
    else
        printf '%s/%s\n' "$REGRESS_DIR" "$SCHEDULE"
    fi
}

SCHEDULE_PATH="$(schedule_path)"
if [ ! -f "$SCHEDULE_PATH" ]; then
    echo "ERROR: regression schedule not found: $SCHEDULE_PATH" >&2
    exit 1
fi

verify_backend_libraries() {
    local backend

    provider_lib_exists() {
        local name="$1"
        local dir

        for dir in "${PKGLIB_CANDIDATES[@]}"; do
            if [ -f "$dir/$name.dylib" ] ||
               [ -f "$dir/$name.so" ] ||
               [ -f "$dir/$name.dll" ]; then
                return 0
            fi
        done
        return 1
    }

    for backend in $BACKENDS; do
        if [ "$backend" = "auto" ]; then
            if ! provider_lib_exists "pg_jitter"; then
                echo "ERROR: pg_jitter meta provider not found in ${PKGLIB_CANDIDATES[*]}" >&2
                exit 1
            fi
            continue
        fi

        if ! provider_lib_exists "pg_jitter_${backend}"; then
            echo "ERROR: pg_jitter_${backend} not found in ${PKGLIB_CANDIDATES[*]}" >&2
            echo "Run: ./install.sh --pg-config $PG_CONFIG all" >&2
            exit 1
        fi
    done
}

verify_backend_libraries

if [ "$FRESH_CLUSTER" -eq 1 ]; then
    PGDATA_TEST="${PGDATA_OVERRIDE:-/tmp/pg_jitter_regress_pg${PG_MAJOR}_${PGPORT}}"
    if [[ "$PGDATA_TEST" != /tmp/pg_jitter_regress_* ]]; then
        echo "ERROR: --fresh-cluster refuses to manage non-temporary datadir: $PGDATA_TEST" >&2
        echo "Use a /tmp/pg_jitter_regress_* path with --datadir." >&2
        exit 1
    fi
else
    PGDATA_TEST="$(psql_postgres -t -A -c "SHOW data_directory;" 2>/dev/null || true)"
    if [ -z "$PGDATA_TEST" ] || [ ! -d "$PGDATA_TEST" ]; then
        echo "ERROR: Cannot determine PGDATA. Is PostgreSQL running on $PGHOST:$PGPORT?" >&2
        echo "Use --fresh-cluster to let this script initialize a temporary cluster." >&2
        exit 1
    fi
fi

ORIG_PROVIDER=""
ORIG_JIT_ABOVE=""
ORIG_JIT_INLINE=""
ORIG_JIT_OPTIMIZE=""
if [ "$FRESH_CLUSTER" -eq 0 ]; then
    ORIG_PROVIDER="$(psql_postgres -t -A -c "SHOW jit_provider;" 2>/dev/null || true)"
    ORIG_JIT_ABOVE="$(psql_postgres -t -A -c "SHOW jit_above_cost;" 2>/dev/null || true)"
    ORIG_JIT_INLINE="$(psql_postgres -t -A -c "SHOW jit_inline_above_cost;" 2>/dev/null || true)"
    ORIG_JIT_OPTIMIZE="$(psql_postgres -t -A -c "SHOW jit_optimize_above_cost;" 2>/dev/null || true)"
fi

stop_fresh_cluster() {
    if [ "$FRESH_CLUSTER" -eq 1 ] && [ "$KEEP_CLUSTER" -eq 0 ] && [ -n "${PGDATA_TEST:-}" ]; then
        "$PGCTL" -D "$PGDATA_TEST" stop -m immediate -w >/dev/null 2>&1 || true
    fi
}

restore_existing_cluster() {
    if [ "$FRESH_CLUSTER" -eq 0 ] && [ -n "$ORIG_PROVIDER" ]; then
        if ! pg_ready; then
            "$PGCTL" -D "$PGDATA_TEST" stop -m immediate >/dev/null 2>&1 || true
            "$PGCTL" -D "$PGDATA_TEST" start -l "$PGDATA_TEST/logfile" -w >/dev/null 2>&1 || true
        fi

        psql_postgres -q -c "ALTER SYSTEM SET jit_provider = '$ORIG_PROVIDER';" >/dev/null 2>&1 || true
        [ -n "$ORIG_JIT_ABOVE" ] &&
            psql_postgres -q -c "ALTER SYSTEM SET jit_above_cost = '$ORIG_JIT_ABOVE';" >/dev/null 2>&1 || true
        [ -n "$ORIG_JIT_INLINE" ] &&
            psql_postgres -q -c "ALTER SYSTEM SET jit_inline_above_cost = '$ORIG_JIT_INLINE';" >/dev/null 2>&1 || true
        [ -n "$ORIG_JIT_OPTIMIZE" ] &&
            psql_postgres -q -c "ALTER SYSTEM SET jit_optimize_above_cost = '$ORIG_JIT_OPTIMIZE';" >/dev/null 2>&1 || true
        psql_postgres -q -c "ALTER SYSTEM RESET pg_jitter.backend;" >/dev/null 2>&1 || true
        "$PGCTL" -D "$PGDATA_TEST" restart -l "$PGDATA_TEST/logfile" -w >/dev/null 2>&1 || true
    fi
}

cleanup_on_exit() {
    restore_existing_cluster
    stop_fresh_cluster
}
trap cleanup_on_exit EXIT

start_fresh_cluster() {
    local backend="$1"
    local log_file="$OUTPUT_DIR/postgresql_${backend}.log"

    "$PGCTL" -D "$PGDATA_TEST" stop -m immediate -w >/dev/null 2>&1 || true
    rm -rf "$PGDATA_TEST"

    "$INITDB" -D "$PGDATA_TEST" --no-locale -E UTF8 -A trust --no-instructions \
        > "$OUTPUT_DIR/initdb_${backend}.log" 2>&1

    {
        echo "port = $PGPORT"
        echo "listen_addresses = 'localhost'"
        if [[ "$PGHOST" = /* ]]; then
            mkdir -p "$PGHOST"
            echo "unix_socket_directories = '$PGHOST'"
        fi
        echo "max_connections = 100"
        echo "shared_buffers = '128MB'"
        echo "log_min_messages = warning"
    } >> "$PGDATA_TEST/postgresql.conf"

    "$PGCTL" -D "$PGDATA_TEST" -l "$log_file" start -w >/dev/null
}

ensure_pg_running() {
    if ! pg_ready; then
        "$PGCTL" -D "$PGDATA_TEST" stop -m immediate >/dev/null 2>&1 || true
        "$PGCTL" -D "$PGDATA_TEST" start -l "$PGDATA_TEST/logfile" -w >/dev/null 2>&1 || true
        sleep 1
    fi
}

drop_regression_database() {
    psql_postgres -q -c "DROP DATABASE IF EXISTS regression WITH (FORCE);" >/dev/null 2>&1 || true
}

cleanup_database() {
    psql_postgres -q -v ON_ERROR_STOP=0 <<'CLEANUP_SQL' >/dev/null || true
DO $$ DECLARE r RECORD; BEGIN
    FOR r IN SELECT evtname FROM pg_event_trigger LOOP
        EXECUTE format('DROP EVENT TRIGGER IF EXISTS %I CASCADE', r.evtname);
    END LOOP;
END $$;

DO $$ DECLARE r RECORD; BEGIN
    FOR r IN SELECT pubname FROM pg_publication LOOP
        EXECUTE format('DROP PUBLICATION IF EXISTS %I CASCADE', r.pubname);
    END LOOP;
END $$;

DO $$ DECLARE r RECORD; BEGIN
    FOR r IN SELECT subname FROM pg_subscription LOOP
        BEGIN
            EXECUTE format('ALTER SUBSCRIPTION %I DISABLE', r.subname);
            EXECUTE format('ALTER SUBSCRIPTION %I SET (slot_name = NONE)', r.subname);
            EXECUTE format('DROP SUBSCRIPTION IF EXISTS %I', r.subname);
        EXCEPTION WHEN OTHERS THEN NULL;
        END;
    END LOOP;
END $$;

DO $$ DECLARE r RECORD; BEGIN
    FOR r IN SELECT srvname FROM pg_foreign_server LOOP
        EXECUTE format('DROP SERVER IF EXISTS %I CASCADE', r.srvname);
    END LOOP;
END $$;

DO $$ DECLARE r RECORD; BEGIN
    FOR r IN SELECT nspname FROM pg_namespace
             WHERE nspname NOT IN ('pg_catalog','information_schema','public')
               AND nspname NOT LIKE 'pg_temp%'
               AND nspname NOT LIKE 'pg_toast%' LOOP
        EXECUTE format('DROP SCHEMA IF EXISTS %I CASCADE', r.nspname);
    END LOOP;
END $$;

SELECT format('DROP TABLESPACE IF EXISTS %I;', spcname)
FROM pg_tablespace
WHERE spcname NOT IN ('pg_default','pg_global')
\gexec

DO $$ DECLARE r RECORD; BEGIN
    FOR r IN SELECT rolname FROM pg_roles
             WHERE rolname LIKE 'regress_%'
               AND rolname NOT IN (SELECT usename FROM pg_stat_activity) LOOP
        BEGIN
            EXECUTE format('REASSIGN OWNED BY %I TO CURRENT_USER', r.rolname);
            EXECUTE format('DROP OWNED BY %I', r.rolname);
            EXECUTE format('DROP ROLE IF EXISTS %I', r.rolname);
        EXCEPTION WHEN OTHERS THEN NULL;
        END;
    END LOOP;
END $$;
CLEANUP_SQL
}

set_backend() {
    local backend="$1"
    local expected_provider

    psql_postgres -q -c "ALTER SYSTEM SET jit_above_cost = 0;" >/dev/null
    psql_postgres -q -c "ALTER SYSTEM SET jit_inline_above_cost = 0;" >/dev/null
    psql_postgres -q -c "ALTER SYSTEM SET jit_optimize_above_cost = 0;" >/dev/null

    if [ "$PG_MAJOR" -ge 17 ]; then
        expected_provider="pg_jitter"
        psql_postgres -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" >/dev/null
        psql_postgres -q -c "ALTER SYSTEM SET pg_jitter.backend = '$backend';" >/dev/null
    elif [ "$backend" = "auto" ]; then
        expected_provider="pg_jitter"
        psql_postgres -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" >/dev/null
    else
        expected_provider="pg_jitter_$backend"
        psql_postgres -q -c "ALTER SYSTEM SET jit_provider = '$expected_provider';" >/dev/null
    fi

    "$PGCTL" -D "$PGDATA_TEST" restart -l "$OUTPUT_DIR/postgresql_${backend}.log" -w >/dev/null 2>&1
    sleep 1

    local active_provider
    active_provider="$(psql_postgres -t -A -c "SHOW jit_provider;" 2>/dev/null || true)"
    if [ "$active_provider" != "$expected_provider" ]; then
        echo "expected jit_provider=$expected_provider, got $active_provider"
        return 1
    fi

    if [ "$expected_provider" = "pg_jitter" ] &&
       { [ "$PG_MAJOR" -ge 17 ] || [ "$backend" != "auto" ]; }; then
        local backend_output active_backend
        backend_output="$(psql_postgres -t -A -c "SHOW pg_jitter.backend;" 2>&1 || true)"
        active_backend="$(printf '%s\n' "$backend_output" | tail -1)"
        if [ "$active_backend" != "$backend" ]; then
            printf 'expected pg_jitter.backend=%s, got output:\n%s\n' "$backend" "$backend_output"
            return 1
        fi
    fi

    # SHOW pg_jitter.backend can still succeed when PostgreSQL accepted an
    # unknown custom GUC into postgresql.auto.conf before loading pg_jitter.
    # Execute a cheap forced-JIT query and reject startup-time GUC warnings
    # before running the full suite.
    local smoke_output
    smoke_output="$(psql_postgres -q -t -A -v ON_ERROR_STOP=1 -c "
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SELECT SUM(i) FROM generate_series(1,1000) AS g(i) WHERE i > 10;
$(if [ "$expected_provider" = "pg_jitter" ]; then printf 'SHOW pg_jitter.backend;'; fi)
" 2>&1)" || {
        printf 'forced-JIT smoke query failed:\n%s\n' "$smoke_output"
        return 1
    }

    if printf '%s\n' "$smoke_output" |
       grep -q 'invalid value for parameter "pg_jitter.backend"'; then
        printf 'forced-JIT smoke query reported invalid backend:\n%s\n' "$smoke_output"
        return 1
    fi

    if [ "$expected_provider" = "pg_jitter" ]; then
        local active_backend
        active_backend="$(printf '%s\n' "$smoke_output" | tail -1)"
        if [ "$active_backend" != "$backend" ]; then
            printf 'expected pg_jitter.backend=%s after forced-JIT load, got output:\n%s\n' \
                "$backend" "$smoke_output"
            return 1
        fi
    fi

    return 0
}

run_backend() {
    local backend="$1"
    local outdir="$OUTPUT_DIR/$backend"
    local logfile="$outdir/pg_regress.log"
    local rc fail_line

    mkdir -p "$outdir/testtablespace"
    log_summary "===== run: $backend ====="

    if [ "$FRESH_CLUSTER" -eq 1 ]; then
        start_fresh_cluster "$backend"
    else
        ensure_pg_running
    fi

    drop_regression_database
    cleanup_database

    if ! set_backend "$backend" > "$outdir/backend_check.log" 2>&1; then
        log_summary "backend=$backend rc=1"
        log_summary "backend activation failed; see $outdir/backend_check.log"
        log_summary ""
        [ "$FRESH_CLUSTER" -eq 1 ] && stop_fresh_cluster
        return 1
    fi

    set +e
    (
        cd "$REGRESS_DIR"
        PGPORT="$PGPORT" "$PG_REGRESS" \
            --inputdir="$REGRESS_DIR" \
            --outputdir="$outdir" \
            --bindir="$PGBIN" \
            --dlpath="$REGRESS_DIR" \
            --max-concurrent-tests="$MAX_CONCURRENT_TESTS" \
            --max-connections="$MAX_CONNECTIONS" \
            --schedule="$SCHEDULE_PATH" \
            --host="$PGHOST" \
            --port="$PGPORT" \
            --dbname=regression \
            2>&1
    ) | tee "$logfile" | tail -3
    rc=${PIPESTATUS[0]}
    set -e

    cp -f "$logfile" "$REGRESS_DIR/regression_${backend}.log" 2>/dev/null || true
    cp -f "$outdir/regression.diffs" "$REGRESS_DIR/regression_${backend}.diffs" 2>/dev/null || true
    cp -f "$outdir/regression.out" "$REGRESS_DIR/regression_${backend}.out" 2>/dev/null || true

    log_summary "backend=$backend rc=$rc"
    if [ "$rc" -eq 0 ]; then
        log_summary "All tests passed."
    else
        fail_line="$(grep -E '# [0-9]+ of [0-9]+ tests failed|[0-9]+ of [0-9]+ tests failed' "$logfile" | tail -1 || true)"
        if [ -n "$fail_line" ]; then
            log_summary "$fail_line"
        else
            log_summary "pg_regress failed; see $logfile"
        fi
    fi
    log_summary "artifacts=$outdir"
    log_summary ""

    [ "$FRESH_CLUSTER" -eq 1 ] && stop_fresh_cluster
    return "$rc"
}

echo "=== PostgreSQL Regression Tests - pg_jitter backends ==="
echo "  PG version:  $("$PG_CONFIG" --version)"
echo "  Port:        $PGPORT"
echo "  Host:        $PGHOST"
echo "  PGDATA:      $PGDATA_TEST"
echo "  PG source:   $PG_SRC"
echo "  pg_regress:  $PG_REGRESS"
echo "  pkglibdirs:  ${PKGLIB_CANDIDATES[*]}"
echo "  Backends:    $BACKENDS"
echo "  Output dir:  $OUTPUT_DIR"
echo "  Fresh mode:  $FRESH_CLUSTER"
echo ""

PASSED=0
FAILED=0
RESULTS=""

for backend in $BACKENDS; do
    echo "============================================"
    echo "  $backend"
    echo "============================================"

    if run_backend "$backend"; then
        echo "  => $backend: PASSED"
        RESULTS="${RESULTS}  ${backend}: PASSED\n"
        PASSED=$((PASSED + 1))
    else
        echo "  => $backend: FAILED"
        RESULTS="${RESULTS}  ${backend}: FAILED\n"
        FAILED=$((FAILED + 1))
    fi
    echo ""
done

echo "============================================"
echo "  SUMMARY"
echo "============================================"
printf '%b' "$RESULTS"
echo ""
echo "  $PASSED passed, $FAILED failed out of $(echo "$BACKENDS" | wc -w | tr -d ' ') backends"
echo "  Artifacts: $OUTPUT_DIR"
echo "  Summary:   $SUMMARY_FILE"
echo "============================================"

exit "$FAILED"
