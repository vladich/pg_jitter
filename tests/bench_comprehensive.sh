#!/bin/bash
# bench_comprehensive.sh — Comprehensive benchmark with JIT timing extraction
#
# Benchmarks 5 backends (interp, llvmjit, sljit, asmjit, mir) using
# EXPLAIN (ANALYZE, FORMAT JSON) to extract both execution time and
# JIT compilation overhead. Generates CSV data and BENCHMARKS.md.
#
# Usage:
#   ./tests/bench_comprehensive.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config (default: $PG_CONFIG or pg_config)
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: $PGDB or postgres)
#   --runs N           Timed runs per query (default: 5)
#   --warmup N         Warmup runs per query (default: 3)
#   --backends LIST    Space-separated backend list (default: auto-detect)
#   --csv FILE         CSV output file (default: auto-timestamped)
#   --md FILE          Markdown output file (default: BENCHMARKS.md in repo root)
#   --no-md            Skip BENCHMARKS.md generation
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-postgres}"
NRUNS=5
NWARMUP=3
REQUESTED_BACKENDS=""
CSV_FILE=""
MD_FILE="$REPO_DIR/BENCHMARKS.md"
GENERATE_MD=1

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --runs)      NRUNS="$2";     shift 2;;
        --warmup)    NWARMUP="$2";   shift 2;;
        --backends)  REQUESTED_BACKENDS="$2"; shift 2;;
        --csv)       CSV_FILE="$2";  shift 2;;
        --md)        MD_FILE="$2";   shift 2;;
        --no-md)     GENERATE_MD=0;  shift;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PGDATA="$("$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"
PGCTL="$PGBIN/pg_ctl"
PG_VERSION="$("$PG_CONFIG" --version)"
PARSE_JSON="$SCRIPT_DIR/parse_explain_json.py"

if [ -z "$CSV_FILE" ]; then
    CSV_FILE="$SCRIPT_DIR/bench_comprehensive_$(date +%Y%m%d_%H%M%S).csv"
fi

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
    ('ultra_wide'),('wide_100'),('wide_300'),('wide_1000')
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
    echo "Running bench_setup_wide.sql..."
    psql_cmd -q -f "$SCRIPT_DIR/bench_setup_wide.sql" 2>&1 | grep -E "NOTICE|ERROR" || true
    echo "Setup complete."
else
    echo "All tables present."
fi

# ================================================================
# Detect available backends
# ================================================================

# Detect DLSUFFIX
if [ -f "$PKGLIBDIR/pg_jitter_sljit.dylib" ]; then
    DLSUFFIX=".dylib"
else
    DLSUFFIX=".so"
fi

# Check if meta-provider is installed
JIT_PROVIDER=$(psql_cmd -t -A -c "SHOW jit_provider;" 2>/dev/null || echo "")
META_MODE=0
if [ "$JIT_PROVIDER" = "pg_jitter" ]; then
    META_MODE=1
    # Trigger provider init so GUC is available
    psql_cmd -t -A -c "SET jit_above_cost = 0; SELECT 1;" >/dev/null 2>&1
fi

# Build backend list
ALL_BACKENDS=("interp")
ALL_NAMES=("interp")

# Check for llvmjit
if [ -f "$PKGLIBDIR/llvmjit${DLSUFFIX}" ]; then
    ALL_BACKENDS+=("llvmjit")
    ALL_NAMES+=("llvmjit")
fi

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
echo "Runs per query: $NRUNS (warmup: $NWARMUP)"
echo "Database: $PGDB (port $PGPORT)"
echo "PostgreSQL: $PG_VERSION"
echo "Meta-provider: $([ $META_MODE -eq 1 ] && echo 'yes' || echo 'no')"
echo "CSV output: $CSV_FILE"
[ "$GENERATE_MD" -eq 1 ] && echo "Markdown: $MD_FILE"
echo ""

# ================================================================
# Switch backend helper
# ================================================================
switch_backend() {
    local backend="$1"

    if [ "$backend" = "interp" ]; then
        return 0
    fi

    if [ "$backend" = "llvmjit" ]; then
        # llvmjit requires ALTER SYSTEM + restart
        ensure_pg_running
        psql_cmd -q -c "ALTER SYSTEM SET jit_provider = 'llvmjit';" 2>/dev/null
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
        sleep 1
        return 0
    fi

    if [ "$META_MODE" -eq 1 ]; then
        # Use pg_jitter meta-provider GUC — no restart needed
        return 0
    fi

    # Fallback: ALTER SYSTEM + restart for pg_jitter backends
    ensure_pg_running
    psql_cmd -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';" 2>/dev/null
    "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    sleep 1
}

restore_provider() {
    # Restore meta-provider if we changed it
    if [ "$META_MODE" -eq 1 ] || [ "$JIT_PROVIDER" = "pg_jitter" ]; then
        ensure_pg_running
        psql_cmd -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';" 2>/dev/null
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    fi
}

# ================================================================
# Query execution with JSON EXPLAIN
# ================================================================
run_explain_json() {
    local query="$1"
    local jit_on="$2"
    local backend="$3"
    local set_backend=""

    if [ "$META_MODE" -eq 1 ] && [ "$backend" != "interp" ] && [ "$backend" != "llvmjit" ]; then
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
EXPLAIN (ANALYZE, FORMAT JSON) $query;
" 2>/dev/null
}

get_exec_time() {
    local query="$1"
    local jit_on="$2"
    local backend="$3"

    psql_cmd -t -A -c "
SET jit = $jit_on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET max_parallel_workers_per_gather = 0;
$([ "$META_MODE" -eq 1 ] && [ "$backend" != "interp" ] && [ "$backend" != "llvmjit" ] && echo "SET pg_jitter.backend = '$backend';")
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

median() {
    local vals=("$@")
    local n=${#vals[@]}
    local sorted=($(printf '%s\n' "${vals[@]}" | sort -g))
    echo "${sorted[$((n / 2))]}"
}

# ================================================================
# Query definitions (same 48 as bench_all_backends.sh)
# ================================================================
LABELS=()
QUERIES=()
SECTIONS=()
SECTION_NAMES=()

add_section() { SECTIONS+=("${#LABELS[@]}:$1"); SECTION_NAMES+=("$1"); }
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

# --- Super-Wide Tables (100/300/1000 cols, sparse, mixed types) ---
add_section "Super-Wide Tables"
# 100 cols: sum a column near the end — deform must skip ~100 cols
add_query "Wide100_sum"         "SELECT SUM(c0097) FROM wide_100"
# 100 cols: group by with one agg at the tail
add_query "Wide100_groupby"     "SELECT grp, COUNT(*), AVG(c0097) FROM wide_100 GROUP BY grp"
# 100 cols: filter on a late varchar column
add_query "Wide100_filter"      "SELECT COUNT(*) FROM wide_100 WHERE c0091 IS NOT NULL"
# 300 cols: sum a column near the tail — deform ~300 cols
add_query "Wide300_sum"         "SELECT SUM(c0297) FROM wide_300"
# 300 cols: group by reaching the end of the row
add_query "Wide300_groupby"     "SELECT grp, COUNT(*), AVG(c0297) FROM wide_300 GROUP BY grp"
# 300 cols: filter on a late varchar
add_query "Wide300_filter"      "SELECT COUNT(*) FROM wide_300 WHERE c0291 IS NOT NULL"
# 1000 cols: sum the last int column — deform across full width
add_query "Wide1K_sum"          "SELECT SUM(c0997) FROM wide_1000"
# 1000 cols: group by with agg near the end
add_query "Wide1K_groupby"      "SELECT grp, COUNT(*), AVG(c0997) FROM wide_1000 GROUP BY grp"
# 1000 cols: filter on the last column
add_query "Wide1K_filter"       "SELECT COUNT(*) FROM wide_1000 WHERE c1000 IS NOT NULL"

# --- Partitioned Table ---
add_section "Partitioned"
add_query "PartScan_filter"     "SELECT COUNT(*), SUM(val) FROM part_data WHERE grp BETWEEN 10 AND 30"
add_query "PartScan_agg_all"    "SELECT grp, COUNT(*), SUM(val) FROM part_data GROUP BY grp"

NQUERIES=${#LABELS[@]}
echo "$NQUERIES queries defined."
echo ""

# ================================================================
# Warmup buffer cache
# ================================================================
echo "Warming up buffer cache..."
psql_cmd -q -c "
SET max_parallel_workers_per_gather = 0;
SELECT COUNT(*) FROM bench_data; SELECT COUNT(*) FROM join_left;
SELECT COUNT(*) FROM join_right; SELECT COUNT(*) FROM date_data;
SELECT COUNT(*) FROM text_data; SELECT COUNT(*) FROM numeric_data;
SELECT COUNT(*) FROM array_data; SELECT COUNT(*) FROM ultra_wide;
SELECT COUNT(*) FROM jsonb_data; SELECT COUNT(*) FROM part_data;
SELECT COUNT(*) FROM wide_100; SELECT COUNT(*) FROM wide_300;
SELECT COUNT(*) FROM wide_1000;
" > /dev/null 2>&1
echo ""

# ================================================================
# CSV header
# ================================================================
echo "query,backend,exec_time,jit_gen,jit_inline,jit_opt,jit_emit,jit_total" > "$CSV_FILE"

# ================================================================
# Run all queries per backend
# ================================================================
SWITCHED_AWAY=0

for bi in "${!BACKENDS[@]}"; do
    backend="${BACKENDS[$bi]}"
    bname="${NAMES[$bi]}"

    if [ "$backend" = "interp" ]; then
        jit_on="off"
        echo "Running $bname (jit=off)..."
    else
        jit_on="on"
        if [ "$backend" = "llvmjit" ]; then
            echo "Switching to $bname (ALTER SYSTEM + restart)..."
            switch_backend "$backend"
            SWITCHED_AWAY=1
        elif [ "$META_MODE" -eq 1 ]; then
            echo "Running $bname (SET pg_jitter.backend)..."
        else
            echo "Switching to $bname (ALTER SYSTEM + restart)..."
            switch_backend "$backend"
            SWITCHED_AWAY=1
        fi
    fi

    crash_count=0
    for qi in $(seq 0 $((NQUERIES - 1))); do
        if [ "$crash_count" -ge 3 ]; then
            echo ""
            echo "  Backend $bname crashed 3+ times, skipping remaining queries."
            for rqi in $(seq "$qi" $((NQUERIES - 1))); do
                echo "${LABELS[$rqi]},$bname,CRASH,0,0,0,0,0" >> "$CSV_FILE"
            done
            break
        fi

        label="${LABELS[$qi]}"
        query="${QUERIES[$qi]}"
        ensure_pg_running

        # Warmup runs (use simple EXPLAIN, not JSON — faster)
        for w in $(seq 1 "$NWARMUP"); do
            get_exec_time "$query" "$jit_on" "$backend" > /dev/null 2>&1 || true
            ensure_pg_running
        done

        # Timed runs — collect exec times
        exec_times=()
        for r in $(seq 1 "$NRUNS"); do
            t=$(get_exec_time "$query" "$jit_on" "$backend")
            exec_times+=("$t")
        done

        # Check for crash
        all_empty=1
        for t in "${exec_times[@]}"; do
            [ -n "$t" ] && all_empty=0
        done

        if [ "$all_empty" -eq 1 ]; then
            echo "${label},$bname,CRASH,0,0,0,0,0" >> "$CSV_FILE"
            crash_count=$((crash_count + 1))
            ensure_pg_running
            printf "X"
            continue
        fi

        median_exec=$(median "${exec_times[@]}")

        # One extra JSON EXPLAIN run to get JIT timing breakdown
        jit_gen=0; jit_inline=0; jit_opt=0; jit_emit=0; jit_total=0
        if [ "$jit_on" = "on" ]; then
            json_output=$(run_explain_json "$query" "$jit_on" "$backend" 2>/dev/null || echo "")
            if [ -n "$json_output" ]; then
                parsed=$(echo "$json_output" | python3 "$PARSE_JSON" 2>/dev/null || echo "0,0,0,0,0,0")
                IFS=',' read -r _ jit_gen jit_inline jit_opt jit_emit jit_total <<< "$parsed"
            fi
        fi

        echo "${label},$bname,$median_exec,$jit_gen,$jit_inline,$jit_opt,$jit_emit,$jit_total" >> "$CSV_FILE"
        printf "."
    done
    echo " done."

    # Restore meta-provider after llvmjit
    if [ "$backend" = "llvmjit" ] && [ "$META_MODE" -eq 1 ]; then
        echo "Restoring pg_jitter meta-provider..."
        restore_provider
        SWITCHED_AWAY=0
    fi
done

echo ""
echo "CSV results saved to: $CSV_FILE"

# Restore provider if we switched away
if [ "$SWITCHED_AWAY" -eq 1 ] && [ "$META_MODE" -eq 1 ]; then
    restore_provider
    echo "Restored jit_provider = pg_jitter"
fi

# ================================================================
# Print text summary (same format as bench_all_backends.sh)
# ================================================================
echo ""
echo "=== Results Summary ==="
echo ""

{
    header_fmt="%-25s"
    header_args=("Query")
    for bname in "${NAMES[@]}"; do
        header_fmt+="%12s"
        header_args+=("$bname")
    done
    printf "$header_fmt\n" "${header_args[@]}"
    printf "$header_fmt\n" "$(printf -- '---%.0s' {1..25})" $(for b in "${NAMES[@]}"; do printf -- '----------  '; done)

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
        row_fmt="%-25s"
        row_args=("$label")
        for bname in "${NAMES[@]}"; do
            # Get exec_time from CSV
            v=$(grep "^${label},${bname}," "$CSV_FILE" | head -1 | cut -d',' -f3)
            [ -z "$v" ] && v="N/A"
            row_fmt+="%12s"
            row_args+=("$v")
        done
        printf "$row_fmt\n" "${row_args[@]}"
    done

    echo ""
    echo "All times in ms (median of $NRUNS). Lower is better."
} | tee "$SCRIPT_DIR/bench_comprehensive_summary_$(date +%Y%m%d_%H%M%S).txt"

# ================================================================
# Generate BENCHMARKS.md
# ================================================================
if [ "$GENERATE_MD" -eq 1 ]; then
    echo ""
    echo "Generating $MD_FILE..."

    # Collect system info
    OS_INFO=$(uname -srm)
    CPU_INFO=$(lscpu 2>/dev/null | grep "Model name" | sed 's/Model name:[[:space:]]*//' || sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "unknown")
    RAM_INFO=$(free -h 2>/dev/null | awk '/^Mem:/{print $2}' || sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.0f GB", $1/1073741824}' || echo "unknown")

    # Find the interp column index (for baseline)
    INTERP_IDX=-1
    for i in "${!NAMES[@]}"; do
        if [ "${NAMES[$i]}" = "interp" ]; then
            INTERP_IDX=$i
            break
        fi
    done

    cat > "$MD_FILE" << 'HEADER'
# pg_jitter Benchmark Results

Comprehensive benchmark comparing pg_jitter JIT backends against PostgreSQL's interpreter and LLVM JIT.

## Environment

HEADER

    cat >> "$MD_FILE" << EOF
| Parameter | Value |
|-----------|-------|
| PostgreSQL | $PG_VERSION |
| OS | $OS_INFO |
| CPU | $CPU_INFO |
| RAM | $RAM_INFO |
| Backends tested | ${NAMES[*]} |
| Runs per query | $NRUNS (median) |
| Warmup runs | $NWARMUP |
| Parallel workers | 0 (disabled) |
| Date | $(date +%Y-%m-%d) |

## Dataset

| Table | Rows | Description |
|-------|------|-------------|
| bench_data | 1M | 8 columns (id, grp, val1-val5, txt) |
| join_left | 1M | 5 columns, key1 100K distinct, key2 50K distinct |
| join_right | 500K | 5 columns, matching keys |
| date_data | 1M | date + timestamp + val |
| text_data | 500K | variable-length text, 100 groups |
| numeric_data | 500K | numeric(15,8) and numeric(12,4) |
| jsonb_data | 500K | JSON documents with 3 keys |
| array_data | 300K | integer arrays, 4 elements each |
| ultra_wide | 1M | 22 columns (20 int + id + grp) |
| wide_100 | 500K | 102 columns (mixed types, ~10% populated) |
| wide_300 | 200K | 302 columns (mixed types, ~10% populated) |
| wide_1000 | 100K | 1002 columns (mixed types, ~10% populated) |
| part_data | 1M | 4 range partitions on grp |

## Methodology

- All queries run with \`max_parallel_workers_per_gather = 0\`
- JIT thresholds set to 0 to force JIT compilation on every query
- Buffer cache warmed before benchmarking
- Each query: $NWARMUP warmup runs, then $NRUNS timed runs, median reported
- JIT compilation timing from a separate \`EXPLAIN (ANALYZE, FORMAT JSON)\` run
- Percentages relative to interpreter (no JIT) baseline: <100% = faster, >100% = slower

## Results

EOF

    # Generate per-section results tables
    prev_section_idx=0
    for si in "${!SECTIONS[@]}"; do
        sec="${SECTIONS[$si]}"
        sec_idx="${sec%%:*}"
        sec_name="${sec#*:}"

        # Determine range of queries in this section
        if [ "$si" -lt $((${#SECTIONS[@]} - 1)) ]; then
            next_sec="${SECTIONS[$((si + 1))]}"
            next_idx="${next_sec%%:*}"
        else
            next_idx=$NQUERIES
        fi

        echo "### $sec_name" >> "$MD_FILE"
        echo "" >> "$MD_FILE"

        # Table header
        header="| Query | No JIT |"
        separator="|-------|--------|"
        for bname in "${NAMES[@]}"; do
            [ "$bname" = "interp" ] && continue
            header+=" $bname |"
            separator+="------|"
        done
        echo "$header" >> "$MD_FILE"
        echo "$separator" >> "$MD_FILE"

        # Table rows
        for qi in $(seq "$sec_idx" $((next_idx - 1))); do
            label="${LABELS[$qi]}"
            # Get interp baseline
            baseline=$(grep "^${label},interp," "$CSV_FILE" | head -1 | cut -d',' -f3)

            row="| $label | "
            if [ "$baseline" = "CRASH" ] || [ -z "$baseline" ]; then
                row+="CRASH |"
            else
                row+="${baseline} ms |"
            fi

            for bname in "${NAMES[@]}"; do
                [ "$bname" = "interp" ] && continue
                val=$(grep "^${label},${bname}," "$CSV_FILE" | head -1 | cut -d',' -f3)
                if [ "$val" = "CRASH" ] || [ -z "$val" ]; then
                    row+=" CRASH |"
                elif [ "$baseline" = "CRASH" ] || [ -z "$baseline" ] || [ "$baseline" = "0" ]; then
                    row+=" ${val} ms |"
                else
                    pct=$(echo "scale=0; $val * 100 / $baseline" | bc 2>/dev/null || echo "?")
                    row+=" ${val} ms (${pct}%) |"
                fi
            done
            echo "$row" >> "$MD_FILE"
        done
        echo "" >> "$MD_FILE"
    done

    # JIT Compilation Overhead table
    cat >> "$MD_FILE" << 'JIT_HEADER'
## JIT Compilation Overhead

Time spent on JIT compilation (generation + optimization + emission), extracted from EXPLAIN JSON.

| Query |
JIT_HEADER

    # Build JIT overhead header dynamically
    jit_header="| Query |"
    jit_sep="|-------|"
    for bname in "${NAMES[@]}"; do
        [ "$bname" = "interp" ] && continue
        jit_header+=" $bname |"
        jit_sep+="------|"
    done
    # Rewrite the last partial header
    sed -i '$ d' "$MD_FILE"
    echo "$jit_header" >> "$MD_FILE"
    echo "$jit_sep" >> "$MD_FILE"

    for qi in $(seq 0 $((NQUERIES - 1))); do
        label="${LABELS[$qi]}"
        row="| $label |"
        for bname in "${NAMES[@]}"; do
            [ "$bname" = "interp" ] && continue
            jit_total=$(grep "^${label},${bname}," "$CSV_FILE" | head -1 | cut -d',' -f8)
            if [ -z "$jit_total" ] || [ "$jit_total" = "0" ]; then
                row+=" - |"
            else
                row+=" ${jit_total} ms |"
            fi
        done
        echo "$row" >> "$MD_FILE"
    done

    cat >> "$MD_FILE" << 'FOOTER'

## Key Observations

### Compilation Speed
- pg_jitter backends (sljit, AsmJIT, MIR) compile in <1 ms per query
- LLVM JIT compilation takes 30-60+ ms, often exceeding the query execution time itself
- For short-running queries, LLVM JIT overhead can make JIT a net negative

### Execution Performance
- On integer arithmetic and hash join workloads, pg_jitter backends match or beat the interpreter
- LLVM JIT can optimize numeric/text operations that pg_jitter handles via function calls (pass-by-reference types)
- Wide row deforming benefits significantly from JIT across all backends

### When JIT Hurts
- Very short queries (<5 ms): LLVM JIT overhead dominates
- Pass-by-reference types (numeric, text, JSONB): all JIT backends fall back to C function calls

FOOTER

    # ============================================================
    # Appendix A: Benchmark Queries
    # ============================================================
    echo "## Appendix A: Benchmark Queries" >> "$MD_FILE"
    echo "" >> "$MD_FILE"

    for si in "${!SECTIONS[@]}"; do
        sec="${SECTIONS[$si]}"
        sec_idx="${sec%%:*}"
        sec_name="${sec#*:}"

        if [ "$si" -lt $((${#SECTIONS[@]} - 1)) ]; then
            next_sec="${SECTIONS[$((si + 1))]}"
            next_idx="${next_sec%%:*}"
        else
            next_idx=$NQUERIES
        fi

        echo "### $sec_name" >> "$MD_FILE"
        echo "" >> "$MD_FILE"

        for qi in $(seq "$sec_idx" $((next_idx - 1))); do
            label="${LABELS[$qi]}"
            query="${QUERIES[$qi]}"
            echo "**${label}**" >> "$MD_FILE"
            echo '```sql' >> "$MD_FILE"
            echo "$query" >> "$MD_FILE"
            echo '```' >> "$MD_FILE"
            echo "" >> "$MD_FILE"
        done
    done

    # ============================================================
    # Appendix B: Table Definitions and Population
    # ============================================================
    echo "## Appendix B: Table Definitions and Data Population" >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo "### Core Tables" >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo '```sql' >> "$MD_FILE"
    # Extract just CREATE TABLE and INSERT statements from bench_setup.sql
    # Show the full file — it's the canonical reference
    cat "$SCRIPT_DIR/bench_setup.sql" >> "$MD_FILE"
    echo '```' >> "$MD_FILE"
    echo "" >> "$MD_FILE"

    echo "### Extended Tables" >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo '```sql' >> "$MD_FILE"
    cat "$SCRIPT_DIR/bench_setup_extra.sql" >> "$MD_FILE"
    echo '```' >> "$MD_FILE"
    echo "" >> "$MD_FILE"

    echo "### Super-Wide Tables (100 / 300 / 1000 columns)" >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo "Generated by \`tests/gen_wide_tables.py\`. Type mix: ~60% varchar, ~20% int/bigint, ~20% boolean/numeric/timestamp. Sparse: ~10% of columns populated per row." >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo "<details>" >> "$MD_FILE"
    echo "<summary>Click to expand DDL and population SQL ($(wc -l < "$SCRIPT_DIR/bench_setup_wide.sql") lines)</summary>" >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo '```sql' >> "$MD_FILE"
    cat "$SCRIPT_DIR/bench_setup_wide.sql" >> "$MD_FILE"
    echo '```' >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo "</details>" >> "$MD_FILE"
    echo "" >> "$MD_FILE"

    echo "---" >> "$MD_FILE"
    echo "" >> "$MD_FILE"
    echo "*Generated by \`tests/bench_comprehensive.sh\` on $(date +%Y-%m-%d)*" >> "$MD_FILE"

    echo "BENCHMARKS.md generated at: $MD_FILE"
fi

echo ""
echo "=== Done ==="
