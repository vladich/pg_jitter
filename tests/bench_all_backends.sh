#!/bin/bash
# bench_all_backends.sh — Comprehensive benchmark across all 5 JIT backends
# Runs ALL queries per backend before switching (only 4 restarts total).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBIN="$("$PG_CONFIG" --bindir)"
PG_DATA="${PGDATA:-$("$PGBIN/psql" -p "${PGPORT:-5432}" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || echo "$HOME/pgdata")}"
PGCTL="$PGBIN/pg_ctl"
LOGFILE="$PG_DATA/logfile"
OUTFILE="$SCRIPT_DIR/bench_results_$(date +%Y%m%d_%H%M%S).txt"
NRUNS=3
TMPDIR=$(mktemp -d)

restart_pg() {
    "$PGCTL" -D "$PG_DATA" stop -m fast 2>/dev/null || true
    sleep 1
    "$PGCTL" -D "$PG_DATA" start -l "$LOGFILE" -w 2>/dev/null
    sleep 1
}

ensure_pg_running() {
    if ! "$PGBIN/pg_isready" -p "${PGPORT:-5432}" -q 2>/dev/null; then
        echo " (server down, restarting)"
        "$PGCTL" -D "$PG_DATA" stop -m immediate 2>/dev/null || true
        sleep 1
        "$PGCTL" -D "$PG_DATA" start -l "$LOGFILE" -w 2>/dev/null || true
        sleep 1
    fi
}

get_exec_time() {
    local query="$1"
    local jit_on="$2"
    "$PGBIN/psql" -p "${PGPORT:-5432}" -d regression -X -t -A -c "
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

# ---- Query definitions ----
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

# ---- Run all queries per backend ----
BACKENDS=("interp" "llvmjit" "pg_jitter_sljit" "pg_jitter_asmjit" "pg_jitter_mir")
BACKEND_NAMES=("interp" "llvm" "sljit" "asmjit" "mir")

for bi in "${!BACKENDS[@]}"; do
    backend="${BACKENDS[$bi]}"
    bname="${BACKEND_NAMES[$bi]}"

    if [ "$backend" = "interp" ]; then
        echo "Running $bname (jit=off)..."
        jit_on="off"
    else
        echo "Switching to $bname..."
        ensure_pg_running
        "$PGBIN/psql" -p "${PGPORT:-5432}" -d regression -X -q -c "ALTER SYSTEM SET jit_provider = '$backend';" 2>/dev/null
        restart_pg
        jit_on="on"
    fi

    # Warmup buffer cache
    "$PGBIN/psql" -p "${PGPORT:-5432}" -d regression -X -q -c "
SET max_parallel_workers_per_gather = 0;
SELECT COUNT(*) FROM bench_data; SELECT COUNT(*) FROM join_left;
SELECT COUNT(*) FROM join_right; SELECT COUNT(*) FROM date_data;
SELECT COUNT(*) FROM wide_data; SELECT COUNT(*) FROM text_data;
SELECT COUNT(*) FROM numeric_data; SELECT COUNT(*) FROM array_data;
SELECT COUNT(*) FROM ultra_wide; SELECT COUNT(*) FROM jsonb_data;
SELECT COUNT(*) FROM part_data;
" > /dev/null 2>&1

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
        # warmup
        get_exec_time "$query" "$jit_on" > /dev/null 2>&1
        ensure_pg_running
        t1=$(get_exec_time "$query" "$jit_on")
        t2=$(get_exec_time "$query" "$jit_on")
        t3=$(get_exec_time "$query" "$jit_on")
        if [ -z "$t1" ] && [ -z "$t2" ] && [ -z "$t3" ]; then
            m="CRASH"
            crash_count=$((crash_count + 1))
            ensure_pg_running
        else
            m=$(median3 "$t1" "$t2" "$t3")
        fi
        echo "$m" >> "$TMPDIR/${bname}_${qi}.txt"
        printf "."
    done
    echo " done."
done

echo ""
echo "Formatting results..."
echo ""

# ---- Format output ----
{
    printf "%-30s%10s%10s%10s%10s%10s\n" "Query" "interp" "llvm" "sljit" "asmjit" "mir"
    printf "%-30s%10s%10s%10s%10s%10s\n" "" "--------" "--------" "--------" "--------" "--------"

    for qi in $(seq 0 $((NQUERIES - 1))); do
        # Check if this index starts a section
        for sec in "${SECTIONS[@]}"; do
            sec_idx="${sec%%:*}"
            sec_name="${sec#*:}"
            if [ "$sec_idx" -eq "$qi" ]; then
                echo ""
                echo "--- $sec_name ---"
            fi
        done

        label="${LABELS[$qi]}"
        v_interp=$(cat "$TMPDIR/interp_${qi}.txt" 2>/dev/null || echo "N/A")
        v_llvm=$(cat "$TMPDIR/llvm_${qi}.txt" 2>/dev/null || echo "N/A")
        v_sljit=$(cat "$TMPDIR/sljit_${qi}.txt" 2>/dev/null || echo "N/A")
        v_asmjit=$(cat "$TMPDIR/asmjit_${qi}.txt" 2>/dev/null || echo "N/A")
        v_mir=$(cat "$TMPDIR/mir_${qi}.txt" 2>/dev/null || echo "N/A")
        printf "%-30s%10s%10s%10s%10s%10s\n" "$label" "$v_interp" "$v_llvm" "$v_sljit" "$v_asmjit" "$v_mir"
    done

    echo ""
    echo "All times in ms. Lower is better."
} | tee "$OUTFILE"

echo "Results saved to: $OUTFILE"

# Cleanup
rm -rf "$TMPDIR"

# Restore sljit as default
ensure_pg_running
"$PGBIN/psql" -p "${PGPORT:-5432}" -d regression -X -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';" 2>/dev/null
restart_pg
echo "Restored jit_provider = pg_jitter_sljit"
