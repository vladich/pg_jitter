#!/bin/bash
# test_leak_toplevel.sh — Leak test using separate top-level queries
#
# Each SELECT is an independent statement in the same psql session.
# Each gets its own Portal/ResourceOwner, so JIT code is compiled
# and freed 3000 times. Tests the full compile→release→free cycle.
#
# Usage: ./tests/test_leak_toplevel.sh [port] [backend]
set -euo pipefail

PORT=${1:-5447}
BACKEND=${2:-sljit}
PSQL="psql -p $PORT -d postgres -X -t -A"
NQUERIES=3000

echo "=== Leak test (top-level): $BACKEND (port $PORT, $NQUERIES queries) ==="

# Generate all SQL: setup + warmup + measure + 3000 queries + measure + cleanup
# All piped to ONE psql session so RSS is measured in the same backend process.
{
    cat <<'SQL'
CREATE OR REPLACE FUNCTION pg_backend_rss_kb() RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE contents text;
BEGIN
    SELECT pg_read_file('/proc/' || pg_backend_pid() || '/status') INTO contents;
    RETURN (regexp_match(contents, 'VmRSS:\s+(\d+)'))[1]::bigint;
END;
$$;

SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;

DROP TABLE IF EXISTS leak_test;
CREATE TABLE leak_test AS
    SELECT i AS id, i % 100 AS grp, (random() * 10000)::int AS val
    FROM generate_series(1, 10000) i;
ANALYZE leak_test;
SQL

    # Warmup
    for i in $(seq 1 20); do
        echo "SELECT sum(val + id * $i) FROM leak_test WHERE grp < 50;"
    done

    # Snapshot RSS before
    echo "CREATE TEMP TABLE _rss_snap AS SELECT pg_backend_rss_kb() AS before_kb;"

    # 3000 top-level queries
    for i in $(seq 1 $NQUERIES); do
        case $((i % 6)) in
            0) echo "SELECT sum(val + id * $((i % 7 + 1))) FROM leak_test WHERE grp < $((i % 80 + 10));" ;;
            1) echo "SELECT count(*) FROM leak_test WHERE val > $((i % 5000)) AND id < $((i % 8000 + 1000));" ;;
            2) echo "SELECT sum(CASE WHEN val > $((i % 3000)) THEN val ELSE 0 END) FROM leak_test WHERE grp = $((i % 100));" ;;
            3) echo "SELECT max(val) - min(val) + count(*) FROM leak_test WHERE grp BETWEEN $((i % 50)) AND $((i % 50 + 20));" ;;
            4) echo "SELECT sum(val * id + val - id) FROM leak_test WHERE val < $((i % 9000 + 500));" ;;
            5) echo "SELECT count(*) + sum(val) FROM leak_test WHERE grp = $((i % 100));" ;;
        esac
    done

    # Report
    cat <<'SQL'
SELECT 'RSS before: ' || before_kb || ' kB' FROM _rss_snap;
SELECT 'RSS after:  ' || pg_backend_rss_kb() || ' kB';
SELECT 'Delta:      ' || (pg_backend_rss_kb() - before_kb) || ' kB' FROM _rss_snap;
SELECT CASE
    WHEN pg_backend_rss_kb() - before_kb > 5120
    THEN 'VERDICT:    LEAK SUSPECTED (>5 MB growth)'
    ELSE 'VERDICT:    OK (delta within tolerance)'
END FROM _rss_snap;

DROP TABLE IF EXISTS leak_test;
DROP TABLE IF EXISTS _rss_snap;
DROP FUNCTION IF EXISTS pg_backend_rss_kb();
SQL
} | $PSQL -q 2>/dev/null | grep -E "^(RSS|Delta|VERDICT)"
