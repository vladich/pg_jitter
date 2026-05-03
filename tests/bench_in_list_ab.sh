#!/bin/bash
set -e
PG_CONFIG="${PG_CONFIG:-$HOME/postgres_install/v18/bin/pg_config}"
PORT="${1:-5099}"
SIZES="${2:-300 500 1000 2000 4000 8000 16000}"
RUNS=8
DATADIR="/tmp/pg_jitter_bench_inlist"
PGBIN="$($PG_CONFIG --bindir)"

if ! "$PGBIN/pg_isready" -p "$PORT" -q 2>/dev/null; then
    rm -rf "$DATADIR"
    "$PGBIN/initdb" -D "$DATADIR" --locale=en_US.UTF-8 > /dev/null 2>&1
    cat >> "$DATADIR/postgresql.conf" << EOF
port = $PORT
jit_above_cost = 0
shared_preload_libraries = 'pg_jitter'
jit_provider = 'pg_jitter'
log_min_messages = warning
work_mem = '64MB'
max_parallel_workers_per_gather = 0
EOF
    "$PGBIN/pg_ctl" -D "$DATADIR" start -l "$DATADIR/logfile" -w > /dev/null 2>&1
fi

PSQL="$PGBIN/psql -p $PORT -d postgres"

$PSQL -q -c "
CREATE TABLE IF NOT EXISTS bench_inlist AS
  SELECT i AS id, (random()*10000)::int AS val2
  FROM generate_series(1, 1000000) i;
ANALYZE bench_inlist;" 2>/dev/null

get_exec_time() {
    local settings="$1"
    local query="$2"
    $PSQL -t -A -c "$settings EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON) $query" 2>/dev/null \
      | grep -v '^SET' \
      | python3 -c "import sys,json;d=json.load(sys.stdin);print(f'{d[0][\"Execution Time\"]:.3f}')" 2>/dev/null
}

get_median() {
    printf '%s\n' "$@" | sort -n | sed -n "$((${#@}/2))p"
}

echo "IN_size  CRC32_ms  Native_ms  Interp_ms  CRC32_spd  Native_spd"
echo "-------  --------  ----------  ---------  ---------  ---------"

for N in $SIZES; do
    IN_LIST=$(python3 -c "print(','.join(str(i) for i in range(1,$((N+1)))))")
    Q="SELECT COUNT(*) FROM bench_inlist WHERE val2 IN ($IN_LIST)"

    CRC="SET jit_above_cost=0; SET pg_jitter.backend='sljit'; SET pg_jitter.in_hash='crc32';"
    NAT="SET jit_above_cost=0; SET pg_jitter.backend='sljit'; SET pg_jitter.in_hash='pg';"
    INT="SET jit_above_cost=1000000;"

    # Warmup all 3
    get_exec_time "$CRC" "$Q" > /dev/null
    get_exec_time "$NAT" "$Q" > /dev/null
    get_exec_time "$INT" "$Q" > /dev/null

    ct=(); nt=(); it=()
    for r in $(seq 1 $RUNS); do
        t=$(get_exec_time "$CRC" "$Q"); [ -n "$t" ] && ct+=($t)
        t=$(get_exec_time "$NAT" "$Q"); [ -n "$t" ] && nt+=($t)
        t=$(get_exec_time "$INT" "$Q"); [ -n "$t" ] && it+=($t)
    done

    cm=$(printf '%s\n' "${ct[@]}" | sort -n | sed -n "$((RUNS/2))p")
    nm=$(printf '%s\n' "${nt[@]}" | sort -n | sed -n "$((RUNS/2))p")
    im=$(printf '%s\n' "${it[@]}" | sort -n | sed -n "$((RUNS/2))p")

    if [ -n "$cm" ] && [ -n "$nm" ] && [ -n "$im" ]; then
        cs=$(python3 -c "print(f'{float(\"$im\")/float(\"$cm\"):.2f}')")
        ns=$(python3 -c "print(f'{float(\"$im\")/float(\"$nm\"):.2f}')")
        printf "%-7s  %8s  %9s  %9s  %8sx  %9sx\n" "$N" "$cm" "$nm" "$im" "$cs" "$ns"
    else
        printf "%-7s  FAILED\n" "$N"
    fi
done
