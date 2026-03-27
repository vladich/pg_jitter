#!/bin/bash
# bench_tpch.sh — TPC-H benchmark comparing pg_jitter backends
#
# Runs all 22 TPC-H queries at SF=1 against each backend, reports
# median execution time and speedup vs interpreter.
#
# Usage:
#   ./tests/bench_tpch.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config
#   --port PORT        PostgreSQL port (default: $PGPORT or 5528)
#   --db DB            Database name (default: tpch)
#   --scale N          Scale factor (default: 1)
#   --data-dir DIR     Directory with .tbl files (default: /tmp/tpch-data)
#   --runs N           Timed runs per query (default: 3)
#   --warmup N         Warmup runs per query (default: 1)
#   --backends LIST    Space-separated backend list (default: auto-detect)
#   --skip-load        Skip data loading (reuse existing DB)
#   --csv FILE         CSV output file
#   --md FILE          Markdown output file (default: TPCH_BENCHMARKS.md)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5528}"
PGDB="tpch"
SCALE=1
DATA_DIR="/tmp/tpch-data"
NRUNS=3
NWARMUP=1
REQUESTED_BACKENDS=""
CSV_FILE=""
MD_FILE="$REPO_DIR/TPCH_BENCHMARKS.md"
SKIP_LOAD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --scale)     SCALE="$2";     shift 2;;
        --data-dir)  DATA_DIR="$2";  shift 2;;
        --runs)      NRUNS="$2";     shift 2;;
        --warmup)    NWARMUP="$2";   shift 2;;
        --backends)  REQUESTED_BACKENDS="$2"; shift 2;;
        --skip-load) SKIP_LOAD=1;    shift;;
        --csv)       CSV_FILE="$2";  shift 2;;
        --md)        MD_FILE="$2";   shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PGCTL="$PGBIN/pg_ctl"
DLSUFFIX=".dylib"
[ "$(uname)" = "Linux" ] && DLSUFFIX=".so"

psql_cmd() {
    "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"
}

psql_maint() {
    "$PGBIN/psql" -p "$PGPORT" -d postgres -X "$@"
}

PGDATA="$(psql_maint -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"

if [ -z "$CSV_FILE" ]; then
    CSV_FILE="$SCRIPT_DIR/bench_tpch_$(date +%Y%m%d_%H%M%S).csv"
fi

# ================================================================
# Ensure meta-provider
# ================================================================
JIT_PROVIDER=$(psql_maint -t -A -c "SHOW jit_provider;" 2>/dev/null || echo "")
META_MODE=0
if [ -f "$PKGLIBDIR/pg_jitter${DLSUFFIX}" ]; then
    if [ "$JIT_PROVIDER" != "pg_jitter" ]; then
        echo "Switching jit_provider to pg_jitter..."
        psql_maint -q -c "ALTER SYSTEM SET jit_provider = 'pg_jitter';"
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
        sleep 1
    fi
    META_MODE=1
    psql_maint -t -A -c "SET jit_above_cost = 0; SELECT 1;" >/dev/null 2>&1
fi

# ================================================================
# Backend detection
# ================================================================
ALL_BACKENDS=("interp")
ALL_NAMES=("interp")

for b in sljit asmjit mir; do
    if [ -f "$PKGLIBDIR/pg_jitter_${b}${DLSUFFIX}" ]; then
        ALL_BACKENDS+=("$b")
        ALL_NAMES+=("$b")
    fi
done

if [ "$META_MODE" -eq 1 ]; then
    ALL_BACKENDS+=("auto")
    ALL_NAMES+=("auto")
fi

if [ -n "$REQUESTED_BACKENDS" ]; then
    BACKENDS=()
    NAMES=()
    for req in $REQUESTED_BACKENDS; do
        for i in "${!ALL_BACKENDS[@]}"; do
            if [ "${ALL_BACKENDS[$i]}" = "$req" ] || [ "${ALL_NAMES[$i]}" = "$req" ]; then
                BACKENDS+=("${ALL_BACKENDS[$i]}")
                NAMES+=("${ALL_NAMES[$i]}")
            fi
        done
    done
else
    BACKENDS=("${ALL_BACKENDS[@]}")
    NAMES=("${ALL_NAMES[@]}")
fi

echo "=== TPC-H Benchmark (SF=$SCALE) ==="
echo "  Port:     $PGPORT"
echo "  Database: $PGDB"
echo "  Data dir: $DATA_DIR"
echo "  Backends: ${NAMES[*]}"
echo "  Runs:     $NRUNS (warmup: $NWARMUP)"
echo ""

# ================================================================
# Database setup
# ================================================================
if [ "$SKIP_LOAD" -eq 0 ]; then
    echo "Creating database $PGDB..."
    psql_maint -q -c "DROP DATABASE IF EXISTS $PGDB;" 2>/dev/null || true
    psql_maint -q -c "CREATE DATABASE $PGDB;"

    echo "Creating schema..."
    psql_cmd -q -c "
CREATE TABLE nation  (n_nationkey INTEGER NOT NULL, n_name CHAR(25) NOT NULL,
                      n_regionkey INTEGER NOT NULL, n_comment VARCHAR(152));
CREATE TABLE region  (r_regionkey INTEGER NOT NULL, r_name CHAR(25) NOT NULL,
                      r_comment VARCHAR(152));
CREATE TABLE part    (p_partkey INTEGER NOT NULL, p_name VARCHAR(55) NOT NULL,
                      p_mfgr CHAR(25) NOT NULL, p_brand CHAR(10) NOT NULL,
                      p_type VARCHAR(25) NOT NULL, p_size INTEGER NOT NULL,
                      p_container CHAR(10) NOT NULL, p_retailprice DECIMAL(15,2) NOT NULL,
                      p_comment VARCHAR(23) NOT NULL);
CREATE TABLE supplier(s_suppkey INTEGER NOT NULL, s_name CHAR(25) NOT NULL,
                      s_address VARCHAR(40) NOT NULL, s_nationkey INTEGER NOT NULL,
                      s_phone CHAR(15) NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL,
                      s_comment VARCHAR(101) NOT NULL);
CREATE TABLE partsupp(ps_partkey INTEGER NOT NULL, ps_suppkey INTEGER NOT NULL,
                      ps_availqty INTEGER NOT NULL, ps_supplycost DECIMAL(15,2) NOT NULL,
                      ps_comment VARCHAR(199) NOT NULL);
CREATE TABLE customer(c_custkey INTEGER NOT NULL, c_name VARCHAR(25) NOT NULL,
                      c_address VARCHAR(40) NOT NULL, c_nationkey INTEGER NOT NULL,
                      c_phone CHAR(15) NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL,
                      c_mktsegment CHAR(10) NOT NULL, c_comment VARCHAR(117) NOT NULL);
CREATE TABLE orders  (o_orderkey INTEGER NOT NULL, o_custkey INTEGER NOT NULL,
                      o_orderstatus CHAR(1) NOT NULL, o_totalprice DECIMAL(15,2) NOT NULL,
                      o_orderdate DATE NOT NULL, o_orderpriority CHAR(15) NOT NULL,
                      o_clerk CHAR(15) NOT NULL, o_shippriority INTEGER NOT NULL,
                      o_comment VARCHAR(79) NOT NULL);
CREATE TABLE lineitem(l_orderkey INTEGER NOT NULL, l_partkey INTEGER NOT NULL,
                      l_suppkey INTEGER NOT NULL, l_linenumber INTEGER NOT NULL,
                      l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL,
                      l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL,
                      l_returnflag CHAR(1) NOT NULL, l_linestatus CHAR(1) NOT NULL,
                      l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL,
                      l_receiptdate DATE NOT NULL, l_shipinstruct CHAR(25) NOT NULL,
                      l_shipmode CHAR(10) NOT NULL, l_comment VARCHAR(44) NOT NULL);
"

    echo "Loading data from $DATA_DIR..."
    for tbl in nation region part supplier partsupp customer orders lineitem; do
        f="$DATA_DIR/${tbl}.tbl"
        if [ ! -f "$f" ]; then
            echo "ERROR: $f not found" >&2
            exit 1
        fi
        echo -n "  $tbl..."
        psql_cmd -q -c "\\COPY $tbl FROM '$f' WITH (FORMAT csv, DELIMITER '|');"
        echo " done"
    done

    echo "Creating primary keys..."
    psql_cmd -q -c "
ALTER TABLE region   ADD PRIMARY KEY (r_regionkey);
ALTER TABLE nation   ADD PRIMARY KEY (n_nationkey);
ALTER TABLE part     ADD PRIMARY KEY (p_partkey);
ALTER TABLE supplier ADD PRIMARY KEY (s_suppkey);
ALTER TABLE partsupp ADD PRIMARY KEY (ps_partkey, ps_suppkey);
ALTER TABLE customer ADD PRIMARY KEY (c_custkey);
ALTER TABLE orders   ADD PRIMARY KEY (o_orderkey);
ALTER TABLE lineitem ADD PRIMARY KEY (l_orderkey, l_linenumber);
"

    echo "Creating indexes..."
    psql_cmd -q -c "
CREATE INDEX idx_nation_regionkey ON nation (n_regionkey);
CREATE INDEX idx_supplier_nationkey ON supplier (s_nationkey);
CREATE INDEX idx_partsupp_partkey ON partsupp (ps_partkey);
CREATE INDEX idx_partsupp_suppkey ON partsupp (ps_suppkey);
CREATE INDEX idx_customer_nationkey ON customer (c_nationkey);
CREATE INDEX idx_orders_custkey ON orders (o_custkey);
CREATE INDEX idx_orders_orderdate ON orders (o_orderdate);
CREATE INDEX idx_lineitem_orderkey ON lineitem (l_orderkey);
CREATE INDEX idx_lineitem_partsupp ON lineitem (l_partkey, l_suppkey);
CREATE INDEX idx_lineitem_shipdate ON lineitem (l_shipdate, l_discount, l_quantity);
"

    echo "Analyzing..."
    psql_cmd -q -c "ANALYZE;"

    echo "Prewarming..."
    psql_cmd -q -c "
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
SELECT pg_prewarm(c.oid::regclass, 'buffer')
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'public' AND c.relkind IN ('r', 'i');
" > /dev/null 2>&1

    echo "Data loaded."
    echo ""
fi

# Row counts
echo "Table sizes:"
psql_cmd -t -A -c "
SELECT 'lineitem', count(*) FROM lineitem
UNION ALL SELECT 'orders', count(*) FROM orders
UNION ALL SELECT 'partsupp', count(*) FROM partsupp
UNION ALL SELECT 'customer', count(*) FROM customer
UNION ALL SELECT 'part', count(*) FROM part
UNION ALL SELECT 'supplier', count(*) FROM supplier
UNION ALL SELECT 'nation', count(*) FROM nation
UNION ALL SELECT 'region', count(*) FROM region
ORDER BY 2 DESC;
"
echo ""

# ================================================================
# TPC-H Queries (standard parameters)
# ================================================================
declare -a Q_LABELS Q_SQL

add_q() { Q_LABELS+=("$1"); Q_SQL+=("$2"); }

add_q "Q1" "SELECT l_returnflag, l_linestatus, sum(l_quantity) as sum_qty, sum(l_extendedprice) as sum_base_price, sum(l_extendedprice*(1-l_discount)) as sum_disc_price, sum(l_extendedprice*(1-l_discount)*(1+l_tax)) as sum_charge, avg(l_quantity) as avg_qty, avg(l_extendedprice) as avg_price, avg(l_discount) as avg_disc, count(*) as count_order FROM lineitem WHERE l_shipdate <= date '1998-12-01' - interval '90 day' GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus"

add_q "Q2" "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment FROM part, supplier, partsupp, nation, region WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15 AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = 'EUROPE' AND ps_supplycost = (SELECT min(ps_supplycost) FROM partsupp, supplier, nation, region WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = 'EUROPE') ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100"

add_q "Q3" "SELECT l_orderkey, sum(l_extendedprice*(1-l_discount)) as revenue, o_orderdate, o_shippriority FROM customer, orders, lineitem WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate < date '1995-03-15' AND l_shipdate > date '1995-03-15' GROUP BY l_orderkey, o_orderdate, o_shippriority ORDER BY revenue DESC, o_orderdate LIMIT 10"

add_q "Q4" "SELECT o_orderpriority, count(*) as order_count FROM orders WHERE o_orderdate >= date '1993-07-01' AND o_orderdate < date '1993-07-01' + interval '3 month' AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate) GROUP BY o_orderpriority ORDER BY o_orderpriority"

add_q "Q5" "SELECT n_name, sum(l_extendedprice*(1-l_discount)) as revenue FROM customer, orders, lineitem, supplier, nation, region WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = 'ASIA' AND o_orderdate >= date '1994-01-01' AND o_orderdate < date '1994-01-01' + interval '1 year' GROUP BY n_name ORDER BY revenue DESC"

add_q "Q6" "SELECT sum(l_extendedprice*l_discount) as revenue FROM lineitem WHERE l_shipdate >= date '1994-01-01' AND l_shipdate < date '1994-01-01' + interval '1 year' AND l_discount BETWEEN 0.06 - 0.01 AND 0.06 + 0.01 AND l_quantity < 24"

add_q "Q7" "SELECT supp_nation, cust_nation, l_year, sum(volume) as revenue FROM (SELECT n1.n_name as supp_nation, n2.n_name as cust_nation, extract(year from l_shipdate) as l_year, l_extendedprice*(1-l_discount) as volume FROM supplier, lineitem, orders, customer, nation n1, nation n2 WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey AND c_nationkey = n2.n_nationkey AND ((n1.n_name = 'FRANCE' AND n2.n_name = 'GERMANY') OR (n1.n_name = 'GERMANY' AND n2.n_name = 'FRANCE')) AND l_shipdate BETWEEN date '1995-01-01' AND date '1996-12-31') as shipping GROUP BY supp_nation, cust_nation, l_year ORDER BY supp_nation, cust_nation, l_year"

add_q "Q8" "SELECT o_year, sum(CASE WHEN nation = 'BRAZIL' THEN volume ELSE 0 END)/sum(volume) as mkt_share FROM (SELECT extract(year from o_orderdate) as o_year, l_extendedprice*(1-l_discount) as volume, n2.n_name as nation FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey AND o_custkey = c_custkey AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey AND r_name = 'AMERICA' AND s_nationkey = n2.n_nationkey AND o_orderdate BETWEEN date '1995-01-01' AND date '1996-12-31' AND p_type = 'ECONOMY ANODIZED STEEL') as all_nations GROUP BY o_year ORDER BY o_year"

add_q "Q9" "SELECT nation, o_year, sum(amount) as sum_profit FROM (SELECT n_name as nation, extract(year from o_orderdate) as o_year, l_extendedprice*(1-l_discount) - ps_supplycost*l_quantity as amount FROM part, supplier, lineitem, partsupp, orders, nation WHERE s_suppkey = l_suppkey AND ps_suppkey = l_suppkey AND ps_partkey = l_partkey AND p_partkey = l_partkey AND o_orderkey = l_orderkey AND s_nationkey = n_nationkey AND p_name LIKE '%green%') as profit GROUP BY nation, o_year ORDER BY nation, o_year DESC"

add_q "Q10" "SELECT c_custkey, c_name, sum(l_extendedprice*(1-l_discount)) as revenue, c_acctbal, n_name, c_address, c_phone, c_comment FROM customer, orders, lineitem, nation WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate >= date '1993-10-01' AND o_orderdate < date '1993-10-01' + interval '3 month' AND l_returnflag = 'R' AND c_nationkey = n_nationkey GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment ORDER BY revenue DESC LIMIT 20"

add_q "Q11" "SELECT ps_partkey, sum(ps_supplycost*ps_availqty) as value FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'GERMANY' GROUP BY ps_partkey HAVING sum(ps_supplycost*ps_availqty) > (SELECT sum(ps_supplycost*ps_availqty)*0.0001 FROM partsupp, supplier, nation WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'GERMANY') ORDER BY value DESC"

add_q "Q12" "SELECT l_shipmode, sum(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) as high_line_count, sum(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) as low_line_count FROM orders, lineitem WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL','SHIP') AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate AND l_receiptdate >= date '1994-01-01' AND l_receiptdate < date '1994-01-01' + interval '1 year' GROUP BY l_shipmode ORDER BY l_shipmode"

add_q "Q13" "SELECT c_count, count(*) as custdist FROM (SELECT c_custkey, count(o_orderkey) FROM customer LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE '%special%requests%' GROUP BY c_custkey) as c_orders (c_custkey, c_count) GROUP BY c_count ORDER BY custdist DESC, c_count DESC"

add_q "Q14" "SELECT 100.00*sum(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice*(1-l_discount) ELSE 0 END)/sum(l_extendedprice*(1-l_discount)) as promo_revenue FROM lineitem, part WHERE l_partkey = p_partkey AND l_shipdate >= date '1995-09-01' AND l_shipdate < date '1995-09-01' + interval '1 month'"

# Q15 uses a CTE instead of temp view (temp view can't be used with EXPLAIN ANALYZE)
add_q "Q15" "WITH revenue0 AS (SELECT l_suppkey AS supplier_no, sum(l_extendedprice*(1-l_discount)) AS total_revenue FROM lineitem WHERE l_shipdate >= date '1996-01-01' AND l_shipdate < date '1996-01-01' + interval '3 month' GROUP BY l_suppkey) SELECT s_suppkey, s_name, s_address, s_phone, total_revenue FROM supplier, revenue0 WHERE s_suppkey = supplier_no AND total_revenue = (SELECT max(total_revenue) FROM revenue0) ORDER BY s_suppkey"

add_q "Q16" "SELECT p_brand, p_type, p_size, count(DISTINCT ps_suppkey) as supplier_cnt FROM partsupp, part WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#45' AND p_type NOT LIKE 'MEDIUM POLISHED%' AND p_size IN (49,14,23,45,19,3,36,9) AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') GROUP BY p_brand, p_type, p_size ORDER BY supplier_cnt DESC, p_brand, p_type, p_size"

add_q "Q17" "SELECT sum(l_extendedprice)/7.0 as avg_yearly FROM lineitem, part, (SELECT l_partkey AS agg_partkey, 0.2*avg(l_quantity) AS avg_quantity FROM lineitem GROUP BY l_partkey) part_agg WHERE p_partkey = l_partkey AND agg_partkey = l_partkey AND p_brand = 'Brand#23' AND p_container = 'MED BOX' AND l_quantity < avg_quantity"

add_q "Q18" "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, sum(l_quantity) FROM customer, orders, lineitem WHERE o_orderkey IN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING sum(l_quantity) > 300) AND c_custkey = o_custkey AND o_orderkey = l_orderkey GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice ORDER BY o_totalprice DESC, o_orderdate LIMIT 100"

add_q "Q19" "SELECT sum(l_extendedprice*(1-l_discount)) as revenue FROM lineitem, part WHERE (p_partkey = l_partkey AND p_brand = 'Brand#12' AND p_container IN ('SM CASE','SM BOX','SM PACK','SM PKG') AND l_quantity >= 1 AND l_quantity <= 1+10 AND p_size BETWEEN 1 AND 5 AND l_shipmode IN ('AIR','AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') OR (p_partkey = l_partkey AND p_brand = 'Brand#23' AND p_container IN ('MED BAG','MED BOX','MED PKG','MED PACK') AND l_quantity >= 10 AND l_quantity <= 10+10 AND p_size BETWEEN 1 AND 10 AND l_shipmode IN ('AIR','AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') OR (p_partkey = l_partkey AND p_brand = 'Brand#34' AND p_container IN ('LG CASE','LG BOX','LG PACK','LG PKG') AND l_quantity >= 20 AND l_quantity <= 20+10 AND p_size BETWEEN 1 AND 15 AND l_shipmode IN ('AIR','AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON')"

add_q "Q20" "SELECT s_name, s_address FROM supplier, nation WHERE s_suppkey IN (SELECT ps_suppkey FROM partsupp, (SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5*sum(l_quantity) AS agg_quantity FROM lineitem WHERE l_shipdate >= date '1994-01-01' AND l_shipdate < date '1994-01-01' + interval '1 year' GROUP BY l_partkey, l_suppkey) agg_lineitem WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'forest%') AND ps_availqty > agg_quantity) AND s_nationkey = n_nationkey AND n_name = 'CANADA' ORDER BY s_name"

add_q "Q21" "SELECT s_name, count(*) as numwait FROM supplier, lineitem l1, orders, nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100"

add_q "Q22" "SELECT cntrycode, count(*) as numcust, sum(c_acctbal) as totacctbal FROM (SELECT substring(c_phone from 1 for 2) as cntrycode, c_acctbal FROM customer WHERE substring(c_phone from 1 for 2) IN ('13','31','23','29','30','18','17') AND c_acctbal > (SELECT avg(c_acctbal) FROM customer WHERE c_acctbal > 0.00 AND substring(c_phone from 1 for 2) IN ('13','31','23','29','30','18','17')) AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) as custsale GROUP BY cntrycode ORDER BY cntrycode"

NQUERIES=${#Q_LABELS[@]}
echo "Loaded $NQUERIES TPC-H queries."
echo ""

# ================================================================
# Helpers
# ================================================================
median() {
    local vals=("$@")
    local n=${#vals[@]}
    local sorted=($(printf '%s\n' "${vals[@]}" | sort -g))
    echo "${sorted[$((n / 2))]}"
}

ensure_pg_running() {
    if ! "$PGBIN/pg_isready" -p "$PGPORT" -q 2>/dev/null; then
        echo " (server down, restarting)"
        "$PGCTL" -D "$PGDATA" start -l "$PGDATA/logfile" -w 2>/dev/null || true
        sleep 1
    fi
}

get_exec_time() {
    local query="$1" jit_on="$2" backend="$3"
    local inline_cost=500000 opt_cost=500000

    psql_cmd -t -A -c "
SET jit = $jit_on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = $inline_cost;
SET jit_optimize_above_cost = $opt_cost;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '256MB';
$([ "$META_MODE" -eq 1 ] && [ "$backend" != "interp" ] && echo "SET pg_jitter.backend = '$backend';")
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sed 's/.*Execution Time: //' | sed 's/ ms//'
}

# ================================================================
# CSV header
# ================================================================
echo "query,backend,exec_time_ms" > "$CSV_FILE"

# ================================================================
# Prewarm
# ================================================================
echo -n "Prewarming buffer cache..."
psql_cmd -q -c "
CREATE EXTENSION IF NOT EXISTS pg_prewarm;
SELECT pg_prewarm(c.oid::regclass, 'buffer')
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'public' AND c.relkind IN ('r', 'i');
" > /dev/null 2>&1
echo " done."
echo ""

# ================================================================
# Run benchmarks — interleaved per query
# ================================================================
echo "Running TPC-H queries (${#BACKENDS[@]} backends, interleaved)..."
echo ""

crash_counts=()
for bi in "${!BACKENDS[@]}"; do crash_counts[$bi]=0; done

for qi in $(seq 0 $((NQUERIES - 1))); do
    label="${Q_LABELS[$qi]}"
    query="${Q_SQL[$qi]}"
    printf "  %-4s " "$label"

    # Prewarm buffer cache before each query to eliminate I/O variance
    psql_cmd -q -c "
    SELECT pg_prewarm(c.oid::regclass, 'buffer')
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public' AND c.relkind IN ('r', 'i');
    " > /dev/null 2>&1 || true

    for bi in "${!BACKENDS[@]}"; do
        backend="${BACKENDS[$bi]}"
        bname="${NAMES[$bi]}"

        crash_count=${crash_counts[$bi]}
        if [ "$crash_count" -ge 2 ]; then
            echo "${label},$bname,CRASH" >> "$CSV_FILE"
            printf "X"
            continue
        fi

        jit_on="on"
        [ "$backend" = "interp" ] && jit_on="off"

        ensure_pg_running

        # Warmup
        for w in $(seq 1 "$NWARMUP"); do
            get_exec_time "$query" "$jit_on" "$backend" > /dev/null 2>&1 || true
            ensure_pg_running
        done

        # Timed runs
        exec_times=()
        for r in $(seq 1 "$NRUNS"); do
            t=$(get_exec_time "$query" "$jit_on" "$backend" 2>/dev/null)
            exec_times+=("$t")
        done

        # Check crash
        all_empty=1
        for t in "${exec_times[@]}"; do [ -n "$t" ] && all_empty=0; done

        if [ "$all_empty" -eq 1 ]; then
            echo "${label},$bname,CRASH" >> "$CSV_FILE"
            crash_counts[$bi]=$((crash_count + 1))
            ensure_pg_running
            printf "X"
            continue
        fi

        med=$(median "${exec_times[@]}")
        echo "${label},$bname,$med" >> "$CSV_FILE"
        printf "."
    done
    echo ""
done

echo ""
echo "CSV saved to: $CSV_FILE"
echo ""

# ================================================================
# Generate Markdown report
# ================================================================
echo "Generating $MD_FILE..."

{
    echo "# TPC-H Benchmark Results (SF=$SCALE)"
    echo ""
    echo "Standard TPC-H decision support benchmark comparing pg_jitter backends."
    echo ""
    echo "## Environment"
    echo ""
    echo "| Parameter | Value |"
    echo "|-----------|-------|"
    echo "| PostgreSQL | $("$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -t -A -c "SELECT version();" 2>/dev/null | head -1) |"
    echo "| OS | $(uname -s) $(uname -r) $(uname -m) |"
    echo "| CPU | $(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep 'model name' /proc/cpuinfo 2>/dev/null | head -1 | sed 's/.*: //') |"
    echo "| RAM | $(($(sysctl -n hw.memsize 2>/dev/null || grep MemTotal /proc/meminfo 2>/dev/null | awk '{print $2*1024}') / 1073741824)) GB |"
    echo "| Scale Factor | $SCALE (~$((SCALE))GB) |"
    echo "| Backends | ${NAMES[*]} |"
    echo "| Runs per query | $NRUNS (median) |"
    echo "| Warmup runs | $NWARMUP |"
    echo "| Parallel workers | 0 (disabled) |"
    echo "| Date | $(date +%Y-%m-%d) |"
    echo ""
    echo "## Results"
    echo ""

    # Build header
    hdr="| Query | No JIT"
    for bi in "${!NAMES[@]}"; do
        [ "${NAMES[$bi]}" = "interp" ] && continue
        hdr="$hdr | ${NAMES[$bi]}"
    done
    hdr="$hdr |"
    echo "$hdr"

    sep="|-------|--------"
    for bi in "${!NAMES[@]}"; do
        [ "${NAMES[$bi]}" = "interp" ] && continue
        sep="$sep|------"
    done
    sep="$sep|"
    echo "$sep"

    # Collect geomean data
    declare -A GEOM_PRODUCTS GEOM_COUNTS

    for qi in $(seq 0 $((NQUERIES - 1))); do
        label="${Q_LABELS[$qi]}"
        baseline=$(grep "^${label},interp," "$CSV_FILE" | head -1 | cut -d',' -f3)

        row="| $label | "
        if [ -z "$baseline" ] || [ "$baseline" = "CRASH" ]; then
            row="${row}CRASH"
        else
            row="${row}$(printf "%.1f ms" "$baseline")"
        fi

        for bi in "${!NAMES[@]}"; do
            bname="${NAMES[$bi]}"
            [ "$bname" = "interp" ] && continue

            val=$(grep "^${label},${bname}," "$CSV_FILE" | head -1 | cut -d',' -f3)
            if [ -z "$val" ] || [ "$val" = "CRASH" ]; then
                row="$row | CRASH"
            else
                if [ -n "$baseline" ] && [ "$baseline" != "CRASH" ] && [ "$baseline" != "0" ]; then
                    speedup=$(echo "scale=2; $baseline / $val" | bc 2>/dev/null)
                    row="$row | $(printf "%.1f ms (${speedup}x)" "$val")"
                    # Accumulate for geomean
                    prev="${GEOM_PRODUCTS[$bname]:-1}"
                    GEOM_PRODUCTS[$bname]=$(echo "scale=10; $prev * $speedup" | bc 2>/dev/null)
                    GEOM_COUNTS[$bname]=$(( ${GEOM_COUNTS[$bname]:-0} + 1 ))
                else
                    row="$row | $(printf "%.1f ms" "$val")"
                fi
            fi
        done
        echo "$row |"
    done

    echo ""
    echo "## Summary"
    echo ""
    echo "| Backend | Geomean Speedup |"
    echo "|---------|----------------|"
    for bi in "${!NAMES[@]}"; do
        bname="${NAMES[$bi]}"
        [ "$bname" = "interp" ] && continue
        cnt="${GEOM_COUNTS[$bname]:-0}"
        if [ "$cnt" -gt 0 ]; then
            prod="${GEOM_PRODUCTS[$bname]}"
            geomean=$(echo "scale=4; e(l($prod)/$cnt)" | bc -l 2>/dev/null)
            echo "| $bname | ${geomean}x |"
        else
            echo "| $bname | N/A |"
        fi
    done

    echo ""
    echo "Speedup = No JIT time / JIT time. >1x = faster with JIT."
    echo ""
    echo "All times in ms (median of $NRUNS runs). Parallel workers disabled."
} > "$MD_FILE"

echo "Results saved to: $MD_FILE"
echo ""

# Print quick summary to stdout
echo "=== Quick Summary ==="
echo ""
printf "%-5s" ""
for bi in "${!NAMES[@]}"; do
    printf "  %10s" "${NAMES[$bi]}"
done
echo ""

for qi in $(seq 0 $((NQUERIES - 1))); do
    label="${Q_LABELS[$qi]}"
    printf "%-5s" "$label"
    for bi in "${!NAMES[@]}"; do
        bname="${NAMES[$bi]}"
        val=$(grep "^${label},${bname}," "$CSV_FILE" | head -1 | cut -d',' -f3)
        if [ -z "$val" ] || [ "$val" = "CRASH" ]; then
            printf "  %10s" "CRASH"
        else
            printf "  %9.1f" "$val"
        fi
    done
    echo ""
done

echo ""
echo "Done."
