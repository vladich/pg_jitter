#!/bin/bash
# test_meta_provider.sh — Tests for the pg_jitter meta-provider
#
# Tests:
#   1. Runtime switching between all installed backends
#   2. Warning when switching to a non-installed backend
#   3. Default backend selected by priority (sljit > asmjit > mir)
#   4. CMake error when building asmjit on unsupported platform
#
# Usage:
#   ./tests/test_meta_provider.sh [--pg-config /path/to/pg_config]
#
# Requires: jit_provider = 'pg_jitter' and server running.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PSQL="$PGBIN/psql"
PGCTL="$PGBIN/pg_ctl"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-postgres}"

PASS=0
FAIL=0
SKIP=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

psql_quiet() {
    "$PSQL" -p "$PGPORT" -d "$PGDB" -X -t -A "$@" 2>&1
}

# Detect PG data directory for restart tests
PGDATA="${PGDATA:-$(psql_quiet -c "SHOW data_directory;" 2>/dev/null || echo "")}"
if [ -z "$PGDATA" ]; then
    echo "WARNING: Cannot detect PGDATA; default-detection tests will restart manually"
fi

restart_pg() {
    "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    sleep 1
}

# Ensure the function exists
psql_quiet -c "
CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
LANGUAGE C STABLE;
" >/dev/null 2>&1

# ================================================================
echo "=== Test 1: Runtime switching between backends ==="
# ================================================================

# Trigger provider init
psql_quiet -c "SET jit_above_cost = 0; SELECT 1;" >/dev/null

for backend in sljit asmjit mir; do
    if [ ! -f "$PKGLIBDIR/pg_jitter_${backend}.dylib" ] && \
       [ ! -f "$PKGLIBDIR/pg_jitter_${backend}.so" ]; then
        skip "switch to $backend (not installed)"
        continue
    fi

    result=$(psql_quiet -c "
        SET jit_above_cost = 0;
        SELECT 1;
        SET pg_jitter.backend = '$backend';
        SELECT pg_jitter_current_backend();
    " | tail -1)

    if [ "$result" = "$backend" ]; then
        pass "switch to $backend"
    else
        fail "switch to $backend (expected '$backend', got '$result')"
    fi
done

# ================================================================
echo "=== Test 2: JIT produces correct results after switching ==="
# ================================================================

for backend in sljit asmjit mir; do
    if [ ! -f "$PKGLIBDIR/pg_jitter_${backend}.dylib" ] && \
       [ ! -f "$PKGLIBDIR/pg_jitter_${backend}.so" ]; then
        skip "compute via $backend (not installed)"
        continue
    fi

    active=$(psql_quiet -c "
        SET jit_above_cost = 0;
        SELECT 1;
        SET pg_jitter.backend = '$backend';
        SELECT pg_jitter_current_backend();
    " | tail -1)
    result=$(psql_quiet -c "
        SET jit_above_cost = 0;
        SET pg_jitter.backend = '$backend';
        SELECT SUM(x) FROM generate_series(1,100) x;
    " | tail -1)

    if [ "$active" != "$backend" ]; then
        fail "compute via $backend (active backend is '$active')"
    elif [ "$result" = "5050" ]; then
        pass "compute via $backend"
    else
        fail "compute via $backend (expected '5050', got '$result')"
    fi
done

# ================================================================
echo "=== Test 3: SET LOCAL reverts after COMMIT ==="
# ================================================================

# Set to asmjit, then SET LOCAL to mir inside a transaction.
# After COMMIT it must revert to asmjit, not to the boot default.
result=$(psql_quiet -c "
    SET jit_above_cost = 0;
    SELECT 1;
    SET pg_jitter.backend = 'asmjit';
    BEGIN;
    SET LOCAL pg_jitter.backend = 'mir';
    COMMIT;
    SELECT pg_jitter_current_backend();
" | tail -1)

if [ "$result" = "asmjit" ]; then
    pass "SET LOCAL reverts after COMMIT"
else
    fail "SET LOCAL reverts after COMMIT (expected 'asmjit', got '$result')"
fi

# ================================================================
echo "=== Test 4: Warning on non-installed backend ==="
# ================================================================

# Temporarily hide a backend to test the warning
# Pick one that exists, rename it
TEST_BACKEND=""
for b in mir asmjit sljit; do
    if [ -f "$PKGLIBDIR/pg_jitter_${b}.dylib" ]; then
        TEST_BACKEND="$b"
        TEST_FILE="$PKGLIBDIR/pg_jitter_${b}.dylib"
        break
    elif [ -f "$PKGLIBDIR/pg_jitter_${b}.so" ]; then
        TEST_BACKEND="$b"
        TEST_FILE="$PKGLIBDIR/pg_jitter_${b}.so"
        break
    fi
done

if [ -n "$TEST_BACKEND" ] && [ -n "$PGDATA" ]; then
    # Hide the backend, restart so the cached state is cleared
    mv "$TEST_FILE" "${TEST_FILE}.bak"
    restart_pg

    # Ensure function exists after restart
    psql_quiet -c "
    CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
    RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
    LANGUAGE C STABLE;
    " >/dev/null 2>&1

    output=$(psql_quiet -c "
        SET jit_above_cost = 0;
        SELECT 1;
        SET pg_jitter.backend = '$TEST_BACKEND';
    " 2>&1)

    if echo "$output" | grep -q "WARNING.*not installed.*fall back"; then
        pass "warning on missing backend ($TEST_BACKEND)"
    else
        fail "warning on missing backend ($TEST_BACKEND) — output: $output"
    fi

    # Restore
    mv "${TEST_FILE}.bak" "$TEST_FILE"
    restart_pg

    # Re-create function after restart
    psql_quiet -c "
    CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
    RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
    LANGUAGE C STABLE;
    " >/dev/null 2>&1
else
    skip "warning on missing backend (cannot rename files or no PGDATA)"
fi

# ================================================================
echo "=== Test 5: Default backend selected by priority ==="
# ================================================================

if [ -z "$PGDATA" ]; then
    skip "default priority (no PGDATA for restart)"
else
    # --- 5a: All installed → sljit default
    if [ -f "$PKGLIBDIR/pg_jitter_sljit.dylib" ] || [ -f "$PKGLIBDIR/pg_jitter_sljit.so" ]; then
        restart_pg

        psql_quiet -c "
        CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
        RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
        LANGUAGE C STABLE;
        " >/dev/null 2>&1

        result=$(psql_quiet -c "
            SET jit_above_cost = 0;
            SELECT 1;
            SELECT pg_jitter_current_backend();
        " | tail -1)

        if [ "$result" = "sljit" ]; then
            pass "default is sljit when all installed"
        else
            fail "default is sljit when all installed (got '$result')"
        fi
    fi

    # --- 5b: Hide sljit → asmjit default (if asmjit exists)
    SLJIT_FILE=""
    for ext in dylib so; do
        [ -f "$PKGLIBDIR/pg_jitter_sljit.$ext" ] && SLJIT_FILE="$PKGLIBDIR/pg_jitter_sljit.$ext"
    done
    ASMJIT_EXISTS=0
    [ -f "$PKGLIBDIR/pg_jitter_asmjit.dylib" ] || [ -f "$PKGLIBDIR/pg_jitter_asmjit.so" ] && ASMJIT_EXISTS=1

    if [ -n "$SLJIT_FILE" ] && [ "$ASMJIT_EXISTS" = "1" ]; then
        mv "$SLJIT_FILE" "${SLJIT_FILE}.bak"
        restart_pg

        psql_quiet -c "
        CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
        RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
        LANGUAGE C STABLE;
        " >/dev/null 2>&1

        result=$(psql_quiet -c "
            SET jit_above_cost = 0;
            SELECT 1;
            SELECT pg_jitter_current_backend();
        " | tail -1)

        if [ "$result" = "asmjit" ]; then
            pass "default is asmjit when sljit missing"
        else
            fail "default is asmjit when sljit missing (got '$result')"
        fi

        mv "${SLJIT_FILE}.bak" "$SLJIT_FILE"
    else
        skip "default is asmjit when sljit missing (sljit or asmjit not installed)"
    fi

    # --- 5c: Hide sljit + asmjit → mir default
    ASMJIT_FILE=""
    for ext in dylib so; do
        [ -f "$PKGLIBDIR/pg_jitter_asmjit.$ext" ] && ASMJIT_FILE="$PKGLIBDIR/pg_jitter_asmjit.$ext"
    done
    MIR_EXISTS=0
    [ -f "$PKGLIBDIR/pg_jitter_mir.dylib" ] || [ -f "$PKGLIBDIR/pg_jitter_mir.so" ] && MIR_EXISTS=1

    if [ -n "$SLJIT_FILE" ] && [ -n "$ASMJIT_FILE" ] && [ "$MIR_EXISTS" = "1" ]; then
        mv "$SLJIT_FILE" "${SLJIT_FILE}.bak"
        mv "$ASMJIT_FILE" "${ASMJIT_FILE}.bak"
        restart_pg

        psql_quiet -c "
        CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
        RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
        LANGUAGE C STABLE;
        " >/dev/null 2>&1

        result=$(psql_quiet -c "
            SET jit_above_cost = 0;
            SELECT 1;
            SELECT pg_jitter_current_backend();
        " | tail -1)

        if [ "$result" = "mir" ]; then
            pass "default is mir when sljit+asmjit missing"
        else
            fail "default is mir when sljit+asmjit missing (got '$result')"
        fi

        mv "${SLJIT_FILE}.bak" "$SLJIT_FILE"
        mv "${ASMJIT_FILE}.bak" "$ASMJIT_FILE"
    else
        skip "default is mir when sljit+asmjit missing (not all backends installed)"
    fi

    # Restore normal state
    restart_pg
    psql_quiet -c "
    CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
    RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
    LANGUAGE C STABLE;
    " >/dev/null 2>&1
fi

# ================================================================
echo "=== Test 6: CMake rejects asmjit on unsupported platform ==="
# ================================================================

# Use a toolchain file to override CMAKE_SYSTEM_PROCESSOR —
# a plain -D flag gets overwritten by CMake's platform detection.
FAKE_TOOLCHAIN=$(mktemp /tmp/ppc64le_toolchain.XXXXXX.cmake)
cat > "$FAKE_TOOLCHAIN" <<'TC'
set(CMAKE_SYSTEM_NAME "Linux")
set(CMAKE_SYSTEM_PROCESSOR "ppc64le")
TC

TMPBUILD=$(mktemp -d)
output=$(cmake -S "$PROJECT_DIR" -B "$TMPBUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$FAKE_TOOLCHAIN" \
    -DPG_CONFIG="$PG_CONFIG" \
    -DPG_JITTER_BACKENDS="sljit;asmjit;mir" \
    2>&1 || true)
rm -rf "$TMPBUILD"

if echo "$output" | grep -q "AsmJIT requires arm64 or x86_64"; then
    pass "CMake rejects asmjit on ppc64le"
else
    fail "CMake rejects asmjit on ppc64le — output: $(echo "$output" | grep -E 'FATAL|asmjit|backends')"
fi

# --- Default excludes asmjit on unsupported platform
TMPBUILD=$(mktemp -d)
output=$(cmake -S "$PROJECT_DIR" -B "$TMPBUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$FAKE_TOOLCHAIN" \
    -DPG_CONFIG="$PG_CONFIG" \
    2>&1 || true)

if echo "$output" | grep -q "asmjit: *OFF"; then
    pass "CMake default excludes asmjit on ppc64le"
else
    fail "CMake default excludes asmjit on ppc64le — output: $(echo "$output" | grep -E 'asmjit|backends')"
fi
rm -rf "$TMPBUILD" "$FAKE_TOOLCHAIN"

# ================================================================
echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
# ================================================================

[ "$FAIL" -eq 0 ] || exit 1
