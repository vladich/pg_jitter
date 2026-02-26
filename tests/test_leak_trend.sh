#!/bin/bash
# test_leak_trend.sh — Take RSS snapshots at intervals to detect growth trend
#
# Usage: ./tests/test_leak_trend.sh [port] [backend]
set -euo pipefail

PORT=${1:-5447}
BACKEND=${2:-sljit}
PSQL="psql -p $PORT -d postgres -X -t -A"
TOTAL=10000

# Snapshot points
CHECKPOINTS=(0 500 1000 2000 3000 5000 7000 10000)

echo "=== Leak trend: $BACKEND (port $PORT, $TOTAL queries) ==="

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
    for i in $(seq 1 30); do
        echo "SELECT sum(val + id * $i) FROM leak_test WHERE grp < 50;"
    done

    # Snapshot at 0
    echo "SELECT 'SNAPSHOT 0 ' || pg_backend_rss_kb();"

    cp_idx=1
    next_cp=${CHECKPOINTS[$cp_idx]}

    for i in $(seq 1 $TOTAL); do
        case $((i % 6)) in
            0) echo "SELECT sum(val + id * $((i % 7 + 1))) FROM leak_test WHERE grp < $((i % 80 + 10));" ;;
            1) echo "SELECT count(*) FROM leak_test WHERE val > $((i % 5000)) AND id < $((i % 8000 + 1000));" ;;
            2) echo "SELECT sum(CASE WHEN val > $((i % 3000)) THEN val ELSE 0 END) FROM leak_test WHERE grp = $((i % 100));" ;;
            3) echo "SELECT max(val) - min(val) + count(*) FROM leak_test WHERE grp BETWEEN $((i % 50)) AND $((i % 50 + 20));" ;;
            4) echo "SELECT sum(val * id + val - id) FROM leak_test WHERE val < $((i % 9000 + 500));" ;;
            5) echo "SELECT count(*) + sum(val) FROM leak_test WHERE grp = $((i % 100));" ;;
        esac

        if [ "$i" -eq "$next_cp" ]; then
            echo "SELECT 'SNAPSHOT $i ' || pg_backend_rss_kb();"
            cp_idx=$((cp_idx + 1))
            if [ $cp_idx -lt ${#CHECKPOINTS[@]} ]; then
                next_cp=${CHECKPOINTS[$cp_idx]}
            else
                next_cp=$((TOTAL + 1))
            fi
        fi
    done

    cat <<'SQL'
DROP TABLE IF EXISTS leak_test;
DROP FUNCTION IF EXISTS pg_backend_rss_kb();
SQL
} | $PSQL -q 2>/dev/null | grep "^SNAPSHOT" | while read -r _ n rss; do
    printf "  %6s queries: %s kB\n" "$n" "$rss"
done
