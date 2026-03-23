#!/bin/bash
# test_encoding_jit.sh — Reproduce flaky encoding/substring failures
#
# Runs failing SQL patterns in a tight loop with each JIT backend,
# checking for errors.  Uses psql with jit_above_cost=0 to force JIT.
#
# Usage:
#   ./tests/test_encoding_jit.sh [--pg-config /path/to/pg_config] [--port 5432]
#                                [--iterations 200] [--backend mir|sljit|asmjit|all]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5432}"
ITERATIONS=200
BACKEND="all"

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)     PGPORT="$2"; shift 2;;
        --iterations) ITERATIONS="$2"; shift 2;;
        --backend)  BACKEND="$2"; shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PSQL="$PGBIN/psql"
PGCTL="$PGBIN/pg_ctl"
PGDB="${PGDB:-postgres}"

TOTAL_PASS=0
TOTAL_FAIL=0

psql_quiet() {
    "$PSQL" -p "$PGPORT" -d "$PGDB" -X -t -A "$@" 2>&1
}

# Detect PGDATA for restarts
PGDATA="${PGDATA:-$(psql_quiet -c "SHOW data_directory;" 2>/dev/null || echo "")}"

# Determine which backends to test
if [ "$BACKEND" = "all" ]; then
    BACKENDS=()
    for b in sljit asmjit mir; do
        if [ -f "$PKGLIBDIR/pg_jitter_${b}.dylib" ] || [ -f "$PKGLIBDIR/pg_jitter_${b}.so" ]; then
            BACKENDS+=("$b")
        fi
    done
    if [ ${#BACKENDS[@]} -eq 0 ]; then
        echo "ERROR: No pg_jitter backends found in $PKGLIBDIR"
        exit 1
    fi
else
    BACKENDS=("$BACKEND")
fi

echo "=== Encoding/substring JIT stress test ==="
echo "Backends: ${BACKENDS[*]}"
echo "Iterations: $ITERATIONS"
echo "Port: $PGPORT"
echo ""

# Save original jit_provider
ORIG_PROVIDER="$(psql_quiet -c "SHOW jit_provider;" 2>/dev/null || echo "pg_jitter")"

cleanup() {
    if [ -n "$PGDATA" ] && [ -n "$ORIG_PROVIDER" ]; then
        psql_quiet -c "ALTER SYSTEM SET jit_provider = '$ORIG_PROVIDER';" >/dev/null 2>&1 || true
        "$PGCTL" -D "$PGDATA" -l /dev/null restart -w >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# Common JIT settings prefix
JIT_ON="SET jit = on; SET jit_above_cost = 0; SET jit_inline_above_cost = 0; SET jit_optimize_above_cost = 0;"

# --- SQL test blocks ---

# Test 1: 2-byte UTF-8 chars (U+00E9 = C3 A9) in non-TOAST column, substring
TEST1_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t1;
CREATE TEMP TABLE _enc_t1(c text);
INSERT INTO _enc_t1 VALUES (repeat(U&'\\00e9', 200));
SELECT length(substring(c, 1, 50)), length(substring(c, 100, 50)) FROM _enc_t1;
DROP TABLE _enc_t1;"

# Test 2: TOAST-compressed 3-byte UTF-8 (U+2026 = E2 80 A6)
TEST2_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t2;
CREATE TEMP TABLE _enc_t2(c text);
INSERT INTO _enc_t2 VALUES (repeat(U&'\\2026', 4000));
SELECT length(SUBSTRING(c FROM 1 FOR 1)), length(SUBSTRING(c FROM 2000 FOR 100)) FROM _enc_t2;
DROP TABLE _enc_t2;"

# Test 3: TOAST + appended bytes, substring from end
TEST3_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t3;
CREATE TEMP TABLE _enc_t3(c text);
INSERT INTO _enc_t3 VALUES (repeat(U&'\\2026', 4000) || 'XYZ');
SELECT SUBSTRING(c FROM 4000 FOR 1), SUBSTRING(c FROM 4001 FOR 3) FROM _enc_t3;
DROP TABLE _enc_t3;"

# Test 4: 4-byte UTF-8 chars (emoji U+1F600 = F0 9F 98 80)
TEST4_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t4;
CREATE TEMP TABLE _enc_t4(c text);
INSERT INTO _enc_t4 VALUES (repeat(U&'\\+01F600', 2000));
SELECT length(SUBSTRING(c FROM 1 FOR 10)), length(SUBSTRING(c FROM 1000 FOR 500)) FROM _enc_t4;
DROP TABLE _enc_t4;"

# Test 5: Multi-column table with mixed types + text substring
# (exercises deform across multiple columns including varlena)
TEST5_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t5;
CREATE TEMP TABLE _enc_t5(id int, pad1 int, pad2 float8, c text, pad3 bool);
INSERT INTO _enc_t5
  SELECT i, i*2, i*1.5, repeat(U&'\\2603', 800 + (i % 200)), (i % 2 = 0)
  FROM generate_series(1, 100) i;
SELECT count(*), sum(length(substring(c, 1, 50))) FROM _enc_t5;
DROP TABLE _enc_t5;"

# Test 6: Join with text columns (exercises inner/outer tuple deform)
TEST6_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t6a;
DROP TABLE IF EXISTS _enc_t6b;
CREATE TEMP TABLE _enc_t6a(id int, c text);
CREATE TEMP TABLE _enc_t6b(id int, c text);
INSERT INTO _enc_t6a SELECT i, repeat(U&'\\00e9', 100 + i) FROM generate_series(1, 50) i;
INSERT INTO _enc_t6b SELECT i, repeat(U&'\\2026', 100 + i) FROM generate_series(1, 50) i;
SELECT count(*), sum(length(a.c) + length(b.c))
  FROM _enc_t6a a JOIN _enc_t6b b ON a.id = b.id;
DROP TABLE _enc_t6a;
DROP TABLE _enc_t6b;"

# Test 7: Aggregate + GROUP BY on text with multi-byte chars
TEST7_SQL="${JIT_ON}
DROP TABLE IF EXISTS _enc_t7;
CREATE TEMP TABLE _enc_t7(grp int, c text);
INSERT INTO _enc_t7
  SELECT i % 10, repeat(U&'\\2603', 500 + i)
  FROM generate_series(1, 200) i;
SELECT grp, count(*), sum(length(substring(c, 1, 100)))
  FROM _enc_t7 GROUP BY grp ORDER BY grp;
DROP TABLE _enc_t7;"

# Test 8: Parallel query with TOAST text
TEST8_SQL="${JIT_ON}
SET max_parallel_workers_per_gather = 2;
SET parallel_tuple_cost = 0;
SET parallel_setup_cost = 0;
SET min_parallel_table_scan_size = 0;
DROP TABLE IF EXISTS _enc_t8;
CREATE TABLE _enc_t8(id int, c text);
INSERT INTO _enc_t8
  SELECT i, repeat(U&'\\2026', 500 + (i % 500))
  FROM generate_series(1, 1000) i;
ANALYZE _enc_t8;
SELECT count(*), sum(length(substring(c, 1, 50))) FROM _enc_t8;
DROP TABLE _enc_t8;"

TESTS=(
    "2byte_utf8:TEST1_SQL"
    "toast_3byte:TEST2_SQL"
    "toast_end:TEST3_SQL"
    "4byte_emoji:TEST4_SQL"
    "multicol:TEST5_SQL"
    "join_text:TEST6_SQL"
    "agg_group:TEST7_SQL"
    "parallel:TEST8_SQL"
)

run_test() {
    local test_name="$1"
    local sql="$2"
    local backend="$3"
    local pass=0
    local fail=0

    for i in $(seq 1 "$ITERATIONS"); do
        # Capture stdout and stderr separately
        output=$(echo "$sql" | "$PSQL" -p "$PGPORT" -d "$PGDB" -X -t -A 2>&1)
        rc=$?
        if [ $rc -ne 0 ] || echo "$output" | grep -qi "ERROR\|invalid byte sequence\|PANIC\|FATAL\|server closed"; then
            fail=$((fail + 1))
            if [ $fail -le 3 ]; then
                # Show the actual error lines, not SET lines
                errlines=$(echo "$output" | grep -i "ERROR\|invalid\|PANIC\|FATAL\|server closed" | head -2)
                echo "    FAIL iter $i: $errlines"
            fi
        else
            pass=$((pass + 1))
        fi
    done

    if [ $fail -gt 0 ]; then
        echo "  $test_name: FAIL ($fail/$ITERATIONS failures)"
        TOTAL_FAIL=$((TOTAL_FAIL + fail))
    else
        echo "  $test_name: PASS ($pass/$ITERATIONS)"
        TOTAL_PASS=$((TOTAL_PASS + pass))
    fi
}

for backend in "${BACKENDS[@]}"; do
    echo "--- Backend: $backend ---"

    # Switch backend
    psql_quiet -c "ALTER SYSTEM SET jit_provider = 'pg_jitter_${backend}';" >/dev/null 2>&1
    if [ -n "$PGDATA" ]; then
        "$PGCTL" -D "$PGDATA" -l /dev/null restart -w >/dev/null 2>&1
        sleep 1
    fi

    # Verify
    active="$(psql_quiet -c "SHOW jit_provider;" 2>/dev/null || echo "")"
    echo "  Active provider: $active"

    for entry in "${TESTS[@]}"; do
        test_name="${entry%%:*}"
        var_name="${entry##*:}"
        run_test "$test_name" "${!var_name}" "$backend"
    done
    echo ""
done

echo "=== Summary ==="
echo "Total pass: $TOTAL_PASS"
echo "Total fail: $TOTAL_FAIL"

if [ $TOTAL_FAIL -gt 0 ]; then
    echo "VERDICT: FAIL"
    exit 1
else
    echo "VERDICT: PASS"
    exit 0
fi
