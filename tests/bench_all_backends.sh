#!/bin/bash
# bench_all_backends.sh — Comprehensive benchmark across all JIT backends
#
# Uses the pg_jitter meta-provider for zero-restart backend switching.
# Automatically creates benchmark tables if missing.
#
# Usage:
#   ./tests/bench_all_backends.sh [--pg-config /path] [--port 5433] [--db postgres]
#                                 [--runs 3] [--backends "interp sljit asmjit mir"]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-postgres}"
NRUNS=3
REQUESTED_BACKENDS=""

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --runs)      NRUNS="$2";     shift 2;;
        --backends)  REQUESTED_BACKENDS="$2"; shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PGDATA="$("$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"
PGCTL="$PGBIN/pg_ctl"
OUTFILE="$SCRIPT_DIR/bench_results_$(date +%Y%m%d_%H%M%S).txt"
TMPDIR=$(mktemp -d)

trap 'rm -rf "$TMPDIR"' EXIT

psql_cmd() {
    "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"
}

ensure_pg_running() {
    if ! "$PGBIN/pg_isready" -p "$PGPORT" -q 2>/dev/null; then
        echo " (server down, restarting)"
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

# ================================================================
# Setup: create benchmark tables if missing
# ================================================================
echo "Checking benchmark tables..."

TABLES_NEEDED=$(psql_cmd -t -A -c "
SELECT string_agg(t, ',') FROM (VALUES
    ('bench_data'),('join_left'),('join_right'),('date_data'),
    ('text_data'),('numeric_data'),('jsonb_data'),('array_data'),
    ('ultra_wide')
) AS v(t)
WHERE NOT EXISTS (
    SELECT 1 FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE c.relname = v.t AND n.nspname = 'public'
      AND c.relkind IN ('r','p')
);")

if [ -n "$TABLES_NEEDED" ]; then
    echo "Creating missing tables: $TABLES_NEEDED"
    echo "Running bench_setup.sql..."
    psql_cmd -q -f "$SCRIPT_DIR/bench_setup.sql" 2>&1 | grep -E "NOTICE|ERROR" || true
    echo "Running bench_setup_extra.sql..."
    psql_cmd -q -f "$SCRIPT_DIR/bench_setup_extra.sql" 2>&1 | grep -E "NOTICE|ERROR" || true
    echo "Setup complete."
else
    echo "All tables present."
fi

# Ensure the SQL function exists
psql_cmd -q -c "
CREATE OR REPLACE FUNCTION pg_jitter_current_backend()
RETURNS text AS 'pg_jitter', 'pg_jitter_current_backend'
LANGUAGE C STABLE;
" 2>/dev/null || true

# ================================================================
# Detect jit_provider and available backends
# ================================================================
JIT_PROVIDER=$(psql_cmd -t -A -c "SHOW jit_provider;" 2>/dev/null || echo "")
META_MODE=0
if [ "$JIT_PROVIDER" = "pg_jitter" ]; then
    META_MODE=1
    # Trigger provider init so GUC is available
    psql_cmd -t -A -c "SET jit_above_cost = 0; SELECT 1;" >/dev/null 2>&1
fi

# Determine DLSUFFIX
if [ -f "$PKGLIBDIR/pg_jitter_sljit.dylib" ]; then
    DLSUFFIX=".dylib"
else
    DLSUFFIX=".so"
fi

# Build backend list
ALL_BACKENDS=("interp")
ALL_NAMES=("interp")

for b in sljit asmjit mir; do
    if [ -f "$PKGLIBDIR/pg_jitter_${b}${DLSUFFIX}" ]; then
        ALL_BACKENDS+=("$b")
        ALL_NAMES+=("$b")
    fi
done

# Filter to requested backends if specified
if [ -n "$REQUESTED_BACKENDS" ]; then
    BACKENDS=()
    NAMES=()
    for rb in $REQUESTED_BACKENDS; do
        for i in "${!ALL_NAMES[@]}"; do
            if [ "${ALL_NAMES[$i]}" = "$rb" ]; then
                BACKENDS+=("${ALL_BACKENDS[$i]}")
                NAMES+=("${ALL_NAMES[$i]}")
            fi
        done
    done
else
    BACKENDS=("${ALL_BACKENDS[@]}")
    NAMES=("${ALL_NAMES[@]}")
fi

echo ""
echo "Backends: ${NAMES[*]}"
echo "Runs per query: $NRUNS"
echo "Database: $PGDB (port $PGPORT)"
echo "Meta-provider: $([ $META_MODE -eq 1 ] && echo 'yes (zero-restart switching)' || echo 'no (will use ALTER SYSTEM + restart)')"
echo ""

# ================================================================
# Query execution helpers
# ================================================================
get_exec_time() {
    local query="$1"
    local jit_on="$2"
    local backend="$3"
    local set_backend=""

    if [ "$META_MODE" -eq 1 ] && [ "$backend" != "interp" ]; then
        set_backend="SET pg_jitter.backend = '$backend';"
    fi

    psql_cmd -t -A -c "
SET jit = $jit_on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET max_parallel_workers_per_gather = 0;
$set_backend
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

median() {
    # Read N values, sort, return middle
    local vals=("$@")
    local n=${#vals[@]}
    local sorted=($(printf '%s\n' "${vals[@]}" | sort -g))
    echo "${sorted[$((n / 2))]}"
}

# ================================================================
# Query definitions
# ================================================================
LABELS=()
QUERIES=()
SECTIONS=()

add_section() { SECTIONS+=("${#LABELS[@]}:$1"); }
add_query() { LABELS+=("$1"); QUERIES+=("$2"); }

# --- Basic Aggregation ---
add_section "Basic Aggregation"
add_query "SUM_int"             "SELECT SUM(val1) FROM bench_data"
add_query "COUNT_star"          "SELECT COUNT(*) FROM bench_data"
add_query "GroupBy_5agg"        "SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp"
add_query "GroupBy_100K_grp"    "SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1"
add_query "COUNT_DISTINCT"      "SELECT COUNT(DISTINCT key1) FROM join_left"

# --- Hash Joins ---
add_section "Hash Joins"
add_query "HashJoin_single"     "SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1"
add_query "HashJoin_composite"  "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2"
add_query "HashJoin_filter"     "SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000"
add_query "HashJoin_GroupBy"    "SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000"

# --- Outer Joins ---
add_section "Outer Joins"
add_query "LeftJoin"            "SELECT COUNT(*), SUM(COALESCE(r.val, 0)) FROM join_left l LEFT JOIN join_right r ON l.key1 = r.key1 WHERE l.id <= 200000"
add_query "RightJoin"           "SELECT COUNT(*), SUM(COALESCE(l.val, 0)) FROM (SELECT * FROM join_left WHERE id <= 200000) l RIGHT JOIN join_right r ON l.key1 = r.key1"
add_query "FullOuterJoin"       "SELECT COUNT(*), SUM(COALESCE(l.val, 0) + COALESCE(r.val, 0)) FROM (SELECT * FROM join_left WHERE id <= 100000) l FULL OUTER JOIN (SELECT * FROM join_right WHERE id <= 100000) r ON l.key1 = r.key1"

# --- Semi/Anti Joins ---
add_section "Semi/Anti Joins"
add_query "EXISTS_semi"         "SELECT COUNT(*) FROM join_left l WHERE EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1 AND r.val > 5000)"
add_query "NOT_EXISTS_anti"     "SELECT COUNT(*) FROM join_left l WHERE NOT EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1)"
add_query "IN_subquery"         "SELECT COUNT(*) FROM join_left WHERE key1 IN (SELECT key1 FROM join_right WHERE val > 8000)"

# --- Set Operations ---
add_section "Set Operations"
add_query "INTERSECT"           "SELECT key1 FROM join_left INTERSECT SELECT key1 FROM join_right"
add_query "EXCEPT"              "SELECT key1 FROM join_left EXCEPT SELECT key1 FROM join_right"
add_query "UNION_ALL_agg"       "SELECT SUM(val) FROM (SELECT val FROM join_left UNION ALL SELECT val FROM join_right) t"

# --- Expressions & Filters ---
add_section "Expressions"
add_query "CASE_simple"         "SELECT SUM(CASE WHEN val > 5000 THEN val ELSE 0 END) FROM join_left"
add_query "CASE_searched_4way"  "SELECT SUM(CASE WHEN val < 1000 THEN 1 WHEN val < 5000 THEN 2 WHEN val < 9000 THEN 3 ELSE 4 END) FROM join_left"
add_query "COALESCE_NULLIF"     "SELECT SUM(COALESCE(NULLIF(val, 0), -1)) FROM join_left"
add_query "Bool_AND_OR"         "SELECT COUNT(*) FROM join_left WHERE (val > 1000 AND val < 9000) OR (key1 > 100 AND key2 < 400)"
add_query "Arith_expr"          "SELECT SUM(val + key1 * 3 - key2) FROM join_left"
add_query "IN_list_20"          "SELECT COUNT(*) FROM bench_data WHERE grp IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)"

# --- Subqueries & Lateral ---
add_section "Subqueries"
add_query "Scalar_subq"         "SELECT SUM(val) FROM join_left WHERE key1 < (SELECT AVG(key1) FROM join_right)"
add_query "Correlated_subq"     "SELECT COUNT(*) FROM (SELECT DISTINCT key1 FROM join_left) d WHERE (SELECT SUM(val) FROM join_right r WHERE r.key1 = d.key1) > 25000"
add_query "LATERAL_top3"        "SELECT d.grp, lat.top_val FROM (SELECT DISTINCT grp FROM bench_data) d, LATERAL (SELECT val1 AS top_val FROM bench_data b WHERE b.grp = d.grp ORDER BY val1 DESC LIMIT 3) lat"

# --- Date/Timestamp ---
add_section "Date/Timestamp"
add_query "Date_extract"        "SELECT EXTRACT(year FROM d) AS yr, COUNT(*), SUM(val) FROM date_data GROUP BY yr"
add_query "Timestamp_trunc"     "SELECT date_trunc('month', ts) AS mo, COUNT(*) FROM date_data GROUP BY mo"
add_query "Interval_arith"      "SELECT COUNT(*) FROM date_data WHERE ts + interval '30 days' > '2000-06-01'::timestamp AND ts - interval '10 days' < '2000-03-01'::timestamp"
add_query "Timestamp_diff"      "SELECT SUM(EXTRACT(epoch FROM ts - '2000-01-01'::timestamp)) FROM date_data WHERE id <= 500000"

# --- Text/String ---
add_section "Text/String"
add_query "Text_EQ_filter"      "SELECT COUNT(*) FROM text_data WHERE grp_text = 'prefix_42'"
add_query "Text_LIKE"           "SELECT COUNT(*) FROM text_data WHERE word LIKE 'word_1%'"
add_query "Text_concat_agg"     "SELECT grp_text, string_agg(word, ',') FROM text_data WHERE id <= 10000 GROUP BY grp_text"
add_query "Text_length_expr"    "SELECT SUM(length(varlen_text) + length(word)) FROM text_data"

# --- Numeric ---
add_section "Numeric"
add_query "Numeric_agg"         "SELECT grp_num, SUM(val1), AVG(val2) FROM numeric_data GROUP BY grp_num"
add_query "Numeric_arith"       "SELECT SUM(val1 * val2 + val1 - val2) FROM numeric_data"

# --- JSONB ---
add_section "JSONB"
add_query "JSONB_extract"       "SELECT SUM((doc->>'a')::int) FROM jsonb_data"
add_query "JSONB_contains"      "SELECT COUNT(*) FROM jsonb_data WHERE doc @> '{\"a\": 42}'"
add_query "JSONB_agg"           "SELECT grp_jsonb, COUNT(*), SUM((doc->>'a')::int) FROM jsonb_data GROUP BY grp_jsonb"

# --- Arrays ---
add_section "Arrays"
add_query "Array_overlap"       "SELECT COUNT(*) FROM array_data WHERE tags && ARRAY[1,2,3,4,5]"
add_query "Array_contains"      "SELECT COUNT(*) FROM array_data WHERE tags @> ARRAY[10,20]"
add_query "Unnest_agg"          "SELECT u, COUNT(*) FROM array_data, unnest(tags) u WHERE id <= 100000 GROUP BY u"

# --- Wide Row / Deform ---
add_section "Wide Row / Deform"
add_query "Wide_10col_sum"      "SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10) FROM ultra_wide"
add_query "Wide_20col_sum"      "SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ultra_wide"
add_query "Wide_GroupBy_expr"   "SELECT grp, SUM(c01*c02 + c03 - c04), AVG(c10+c20) FROM ultra_wide GROUP BY grp"

# --- Partitioned Table ---
add_section "Partitioned"
add_query "PartScan_filter"     "SELECT COUNT(*), SUM(val) FROM part_data WHERE grp BETWEEN 10 AND 30"
add_query "PartScan_agg_all"    "SELECT grp, COUNT(*), SUM(val) FROM part_data GROUP BY grp"

NQUERIES=${#LABELS[@]}
echo "$NQUERIES queries defined."
echo ""

# ================================================================
# Warmup buffer cache (once)
# ================================================================
echo "Warming up buffer cache..."
psql_cmd -q -c "
SET max_parallel_workers_per_gather = 0;
SELECT COUNT(*) FROM bench_data; SELECT COUNT(*) FROM join_left;
SELECT COUNT(*) FROM join_right; SELECT COUNT(*) FROM date_data;
SELECT COUNT(*) FROM text_data; SELECT COUNT(*) FROM numeric_data;
SELECT COUNT(*) FROM array_data; SELECT COUNT(*) FROM ultra_wide;
SELECT COUNT(*) FROM jsonb_data; SELECT COUNT(*) FROM part_data;
" > /dev/null 2>&1
echo ""

# ================================================================
# Run all queries per backend
# ================================================================
for bi in "${!BACKENDS[@]}"; do
    backend="${BACKENDS[$bi]}"
    bname="${NAMES[$bi]}"

    if [ "$backend" = "interp" ]; then
        echo "Running $bname (jit=off)..."
        jit_on="off"
    elif [ "$META_MODE" -eq 1 ]; then
        echo "Running $bname (SET pg_jitter.backend)..."
        jit_on="on"
    else
        # Fallback: ALTER SYSTEM + restart (when not using meta-provider)
        echo "Switching to $bname (ALTER SYSTEM + restart)..."
        ensure_pg_running
        psql_cmd -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
        sleep 1
        jit_on="on"
    fi

    crash_count=0
    for qi in $(seq 0 $((NQUERIES - 1))); do
        if [ "$crash_count" -ge 3 ]; then
            echo ""
            echo "  Backend $bname crashed 3+ times, skipping remaining queries."
            for rqi in $(seq "$qi" $((NQUERIES - 1))); do
                echo "CRASH" >> "$TMPDIR/${bname}_${rqi}.txt"
            done
            break
        fi

        query="${QUERIES[$qi]}"
        ensure_pg_running

        # Warmup run
        get_exec_time "$query" "$jit_on" "$backend" > /dev/null 2>&1
        ensure_pg_running

        # Timed runs
        times=()
        for r in $(seq 1 "$NRUNS"); do
            t=$(get_exec_time "$query" "$jit_on" "$backend")
            times+=("$t")
        done

        # Check for crash (all empty)
        all_empty=1
        for t in "${times[@]}"; do
            [ -n "$t" ] && all_empty=0
        done

        if [ "$all_empty" -eq 1 ]; then
            m="CRASH"
            crash_count=$((crash_count + 1))
            ensure_pg_running
        else
            m=$(median "${times[@]}")
        fi

        echo "$m" >> "$TMPDIR/${bname}_${qi}.txt"
        printf "."
    done
    echo " done."
done

echo ""
echo "Formatting results..."
echo ""

# ================================================================
# Format output table
# ================================================================
{
    # Header
    header_fmt="%-30s"
    header_args=("Query")
    sep_args=("")
    for bname in "${NAMES[@]}"; do
        header_fmt+="%10s"
        header_args+=("$bname")
        sep_args+=("--------")
    done
    printf "$header_fmt\n" "${header_args[@]}"
    printf "$header_fmt\n" "${sep_args[@]}"

    for qi in $(seq 0 $((NQUERIES - 1))); do
        # Section headers
        for sec in "${SECTIONS[@]}"; do
            sec_idx="${sec%%:*}"
            sec_name="${sec#*:}"
            if [ "$sec_idx" -eq "$qi" ]; then
                echo ""
                echo "--- $sec_name ---"
            fi
        done

        label="${LABELS[$qi]}"
        row_fmt="%-30s"
        row_args=("$label")
        for bname in "${NAMES[@]}"; do
            v=$(cat "$TMPDIR/${bname}_${qi}.txt" 2>/dev/null || echo "N/A")
            row_fmt+="%10s"
            row_args+=("$v")
        done
        printf "$row_fmt\n" "${row_args[@]}"
    done

    echo ""
    echo "All times in ms (median of $NRUNS). Lower is better."
} | tee "$OUTFILE"

echo ""
echo "Results saved to: $OUTFILE"

# Restore default if we used ALTER SYSTEM
if [ "$META_MODE" -eq 0 ] && [ -n "$PGDATA" ]; then
    ensure_pg_running
    psql_cmd -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" 2>/dev/null
    "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    echo "Restored jit_provider = pg_jitter"
fi
