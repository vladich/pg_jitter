#!/bin/bash
# run_pg_regress.sh — Run PostgreSQL regression tests for each JIT backend
#
# Runs the standard pg_regress installcheck suite once per backend,
# switching jit_provider via ALTER SYSTEM + restart between runs.
# Restores the original jit_provider when done.
#
# Usage:
#   ./tests/run_pg_regress.sh [options]
#
# Options:
#   --pg-config PATH     Path to pg_config (default: $PG_CONFIG or pg_config)
#   --port PORT          PostgreSQL port (default: $PGPORT or 5433)
#   --pg-src DIR         PostgreSQL source tree (default: $PG_SRC or auto-detect)
#   --backends LIST      Space-separated backend list (default: "sljit asmjit mir")
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PG_SRC="${PG_SRC:-}"
BACKENDS="sljit asmjit mir"

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --pg-src)    PG_SRC="$2";    shift 2;;
        --backends)  BACKENDS="$2";  shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

# Resolve pg_config
if ! command -v "$PG_CONFIG" > /dev/null 2>&1 && [ ! -x "$PG_CONFIG" ]; then
    echo "ERROR: pg_config not found: $PG_CONFIG"
    echo "  Use: --pg-config /path/to/pg_config  or  export PG_CONFIG=..."
    exit 1
fi

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PSQL="$PGBIN/psql"
PGCTL="$PGBIN/pg_ctl"
PGDATA="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"

if [ -z "$PGDATA" ] || [ ! -d "$PGDATA" ]; then
    echo "ERROR: Cannot determine PGDATA. Is PostgreSQL running on port $PGPORT?"
    exit 1
fi

# Auto-detect PostgreSQL source tree
if [ -z "$PG_SRC" ]; then
    # Walk up from pg_config bindir to find src/test/regress
    PG_INSTALL_PREFIX="$("$PG_CONFIG" --prefix 2>/dev/null || echo "")"
    for candidate in \
        "$REPO_DIR/../postgres-jit" \
        "$REPO_DIR/../postgresql" \
        "$REPO_DIR/../postgres" \
        "$PG_INSTALL_PREFIX" \
    ; do
        if [ -n "$candidate" ] && [ -f "$candidate/src/test/regress/pg_regress" ]; then
            PG_SRC="$candidate"
            break
        fi
    done
fi

if [ -z "$PG_SRC" ] || [ ! -f "$PG_SRC/src/test/regress/pg_regress" ]; then
    echo "ERROR: PostgreSQL source tree not found."
    echo "  Use: --pg-src /path/to/postgres  or  export PG_SRC=..."
    exit 1
fi

REGRESS_DIR="$PG_SRC/src/test/regress"

# Verify all backend dylibs exist
for backend in $BACKENDS; do
    if [ ! -f "$PKGLIBDIR/pg_jitter_$backend.dylib" ] && \
       [ ! -f "$PKGLIBDIR/pg_jitter_$backend.so" ]; then
        echo "ERROR: pg_jitter_$backend not found in $PKGLIBDIR"
        echo "  Run: ./install all"
        exit 1
    fi
done

# Save original provider
ORIG_PROVIDER="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW jit_provider;" 2>/dev/null)"

ensure_pg_running() {
    if ! "$PGBIN/pg_isready" -p "$PGPORT" -q 2>/dev/null; then
        if [ -n "$PGDATA" ]; then
            "$PGCTL" -D "$PGDATA" stop -m immediate 2>/dev/null || true
            sleep 1
            "$PGCTL" -D "$PGDATA" start -l "$PGDATA/logfile" -w 2>/dev/null || true
            sleep 1
        else
            echo "ERROR: Cannot restart — PGDATA unknown" >&2
            exit 1
        fi
    fi
}

restore_provider() {
    if [ -n "$ORIG_PROVIDER" ]; then
        ensure_pg_running
        "$PSQL" -p "$PGPORT" -d postgres -q -c \
            "ALTER SYSTEM SET jit_provider = '$ORIG_PROVIDER';" 2>/dev/null
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    fi
}
trap restore_provider EXIT

echo "=== PostgreSQL Regression Tests — pg_jitter backends ==="
echo ""
echo "  Port:       $PGPORT"
echo "  PGDATA:     $PGDATA"
echo "  PG source:  $PG_SRC"
echo "  pkglibdir:  $PKGLIBDIR"
echo "  Backends:   $BACKENDS"
echo ""

PASSED=0
FAILED=0
RESULTS=""

for backend in $BACKENDS; do
    echo "============================================"
    echo "  $backend"
    echo "============================================"

    # Switch provider
    ensure_pg_running
    "$PSQL" -p "$PGPORT" -d postgres -q -c \
        "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null
    "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    sleep 1

    # Verify provider is active
    ACTIVE="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW jit_provider;" 2>/dev/null)"
    if [ "$ACTIVE" != "pg_jitter_$backend" ]; then
        echo "  WARNING: expected pg_jitter_$backend, got $ACTIVE"
    fi

    # Run installcheck, capture output
    LOGFILE="$REGRESS_DIR/regression_${backend}.log"
    set +e
    (cd "$PG_SRC" && make installcheck PGPORT="$PGPORT" 2>&1) | tee "$LOGFILE" | tail -3
    RC=${PIPESTATUS[0]}
    set -e

    if [ $RC -eq 0 ]; then
        echo "  => $backend: PASSED"
        RESULTS="$RESULTS  $backend: PASSED (all tests)\n"
        PASSED=$((PASSED + 1))
    else
        # Extract failure count from log
        FAIL_LINE=$(grep -E '# [0-9]+ of [0-9]+ tests failed' "$LOGFILE" | tail -1)
        if [ -n "$FAIL_LINE" ]; then
            echo "  => $backend: FAILED — $FAIL_LINE"
            RESULTS="$RESULTS  $backend: FAILED — $FAIL_LINE\n"
        else
            echo "  => $backend: FAILED (exit code $RC)"
            RESULTS="$RESULTS  $backend: FAILED (exit code $RC)\n"
        fi
        FAILED=$((FAILED + 1))

        # Copy diffs for inspection
        DIFFS="$REGRESS_DIR/regression.diffs"
        if [ -f "$DIFFS" ]; then
            cp "$DIFFS" "$REGRESS_DIR/regression_${backend}.diffs"
            echo "     Diffs saved: $REGRESS_DIR/regression_${backend}.diffs"
        fi
    fi

    echo ""
done

# Summary
echo "============================================"
echo "  SUMMARY"
echo "============================================"
printf "$RESULTS"
echo ""
echo "  $PASSED passed, $FAILED failed out of $(echo $BACKENDS | wc -w | tr -d ' ') backends"
echo "============================================"

exit $FAILED
