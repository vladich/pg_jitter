#!/bin/bash
# bench_olap_sustained.sh — Sustained OLAP workload with CPU/memory monitoring
#
# Star-schema data warehouse with heavy analytical queries: full scans,
# hash joins, aggregations, window functions, complex expressions, wide tables.
# These are the workloads where JIT delivers the biggest wins.
#
# Usage:
#   ./tests/bench_olap_sustained.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: bench_olap)
#   --scale N          Data scale factor (default: 10)
#   --clients N        Concurrent pgbench clients (default: 4)
#   --threads N        pgbench threads (default: 4)
#   --duration N       Seconds per backend (default: 300 = 5 min)
#   --warmup N         Warmup seconds (default: 30)
#   --backends LIST    Backends to test (default: "nojit sljit asmjit mir auto")
#   --skip-load        Skip data loading (reuse existing DB)
#   --outdir DIR       Output directory
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-bench_olap}"
SCALE=10
CLIENTS=4
THREADS=4
DURATION=300
WARMUP=30
BACKENDS="nojit sljit asmjit mir auto"
SKIP_LOAD=0
OUTDIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --db)        PGDB="$2";      shift 2;;
        --scale)     SCALE="$2";     shift 2;;
        --clients)   CLIENTS="$2";   shift 2;;
        --threads)   THREADS="$2";   shift 2;;
        --duration)  DURATION="$2";  shift 2;;
        --warmup)    WARMUP="$2";    shift 2;;
        --backends)  BACKENDS="$2";  shift 2;;
        --skip-load) SKIP_LOAD=1;    shift;;
        --outdir)    OUTDIR="$2";    shift 2;;
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PSQL="$PGBIN/psql"
PGBENCH="$PGBIN/pgbench"
PGCTL="$PGBIN/pg_ctl"

PGDATA="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"
if [ -z "$PGDATA" ] || [ ! -d "$PGDATA" ]; then
    echo "ERROR: Cannot determine PGDATA. Is PostgreSQL running on port $PGPORT?"
    exit 1
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ -z "$OUTDIR" ]; then
    OUTDIR="$SCRIPT_DIR/olap_${TIMESTAMP}"
fi
mkdir -p "$OUTDIR"

TMPDIR=$(mktemp -d)

ORIG_PROVIDER="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW jit_provider;" 2>/dev/null)"
ORIG_SHARED_BUFFERS="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW shared_buffers;" 2>/dev/null)"

# Ensure shared_buffers is large enough to hold the dataset in memory.
# Scale 10 ≈ 2.5GB, scale 5 ≈ 1.3GB, scale 3 ≈ 0.8GB.
DESIRED_GB=$(( (SCALE + 2) / 3 + 1 ))  # ~1GB for scale 3, ~3GB for scale 10
SHARED_BUFFERS="${DESIRED_GB}GB"
CURRENT_SB="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c \
    "SELECT setting::bigint * pg_size_bytes(unit) FROM pg_settings WHERE name='shared_buffers';" 2>/dev/null)"
DESIRED_SB=$(( DESIRED_GB * 1024 * 1024 * 1024 ))
if [ -n "$CURRENT_SB" ] && [ "$CURRENT_SB" -lt "$DESIRED_SB" ]; then
    echo "=== Raising shared_buffers to $SHARED_BUFFERS (was $ORIG_SHARED_BUFFERS) ==="
    "$PSQL" -p "$PGPORT" -d postgres -q -c "ALTER SYSTEM SET shared_buffers = '$SHARED_BUFFERS';"
    "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
    sleep 2
fi

cleanup() {
    kill $MONITOR_PID 2>/dev/null || true
    if [ -n "$ORIG_PROVIDER" ]; then
        "$PSQL" -p "$PGPORT" -d postgres -q -c \
            "ALTER SYSTEM SET jit_provider = '$ORIG_PROVIDER';" 2>/dev/null || true
    fi
    "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1 || true
    "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
        ALTER DATABASE $PGDB RESET jit;
        ALTER DATABASE $PGDB RESET jit_above_cost;
        ALTER DATABASE $PGDB RESET work_mem;
    " 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT
MONITOR_PID=""

# ================================================================
# Star-schema data warehouse
# ================================================================
load_data() {
    echo "=== Creating database $PGDB ==="
    "$PSQL" -p "$PGPORT" -d postgres -q -c "DROP DATABASE IF EXISTS $PGDB;" 2>/dev/null
    "$PSQL" -p "$PGPORT" -d postgres -q -c "CREATE DATABASE $PGDB;"

    local N_PRODUCTS=$((50000 * SCALE / 10))     # 50K at scale 10
    local N_CUSTOMERS=$((500000 * SCALE / 10))    # 500K at scale 10
    local N_STORES=$((1000 * SCALE / 10))         # 1K at scale 10
    local N_SALES=$((10000000 * SCALE / 10))      # 10M at scale 10
    local N_DATES=3652                             # 10 years

    echo "=== Loading star schema (scale=$SCALE) ==="
    echo "    fact_sales: $N_SALES rows"
    echo "    dim_product: $N_PRODUCTS | dim_customer: $N_CUSTOMERS | dim_store: $N_STORES"

    "$PSQL" -p "$PGPORT" -d "$PGDB" -q <<SQL

-- Date dimension (10 years: 2015-01-01 to 2024-12-31)
CREATE TABLE dim_date (
    date_key     int PRIMARY KEY,
    full_date    date NOT NULL,
    year         smallint NOT NULL,
    quarter      smallint NOT NULL,
    month        smallint NOT NULL,
    month_name   text NOT NULL,
    week         smallint NOT NULL,
    day_of_week  smallint NOT NULL,
    day_name     text NOT NULL,
    is_weekend   boolean NOT NULL,
    is_holiday   boolean NOT NULL,
    fiscal_year  smallint NOT NULL,
    fiscal_qtr   smallint NOT NULL
);

INSERT INTO dim_date
SELECT
    d::date - '2015-01-01'::date AS date_key,
    d::date AS full_date,
    extract(year FROM d)::smallint,
    extract(quarter FROM d)::smallint,
    extract(month FROM d)::smallint,
    to_char(d, 'Month'),
    extract(week FROM d)::smallint,
    extract(isodow FROM d)::smallint,
    to_char(d, 'Day'),
    extract(isodow FROM d) IN (6,7),
    extract(month FROM d) = 12 AND extract(day FROM d) = 25,  -- just Christmas
    CASE WHEN extract(month FROM d) >= 7
         THEN extract(year FROM d)::smallint
         ELSE (extract(year FROM d) - 1)::smallint END,
    CASE WHEN extract(month FROM d) >= 7
         THEN ((extract(month FROM d) - 7) / 3 + 1)::smallint
         ELSE ((extract(month FROM d) + 5) / 3 + 1)::smallint END
FROM generate_series('2015-01-01'::date, '2024-12-31'::date, '1 day'::interval) d;

-- Product dimension (wide table — 20+ columns, good for deform JIT)
CREATE TABLE dim_product (
    product_key  serial PRIMARY KEY,
    sku          text NOT NULL,
    name         text NOT NULL,
    category     text NOT NULL,
    subcategory  text NOT NULL,
    brand        text NOT NULL,
    supplier     text NOT NULL,
    unit_cost    numeric(10,2) NOT NULL,
    list_price   numeric(10,2) NOT NULL,
    weight_kg    numeric(8,3),
    width_cm     numeric(6,1),
    height_cm    numeric(6,1),
    depth_cm     numeric(6,1),
    color        text,
    material     text,
    country_of_origin text,
    warranty_months smallint,
    is_active    boolean DEFAULT true,
    launch_date  date,
    discontinue_date date,
    margin_pct   numeric(5,2)
);

INSERT INTO dim_product (sku, name, category, subcategory, brand, supplier,
    unit_cost, list_price, weight_kg, width_cm, height_cm, depth_cm,
    color, material, country_of_origin, warranty_months, launch_date, margin_pct)
SELECT
    'P-' || lpad(g::text, 7, '0'),
    'Product ' || g || ' ' || (ARRAY['Alpha','Beta','Gamma','Delta','Epsilon',
        'Zeta','Eta','Theta','Iota','Kappa'])[1 + g % 10],
    (ARRAY['Electronics','Clothing','Home','Sports','Books','Food','Beauty',
        'Automotive','Garden','Toys'])[1 + g % 10],
    (ARRAY['Phones','Shirts','Kitchen','Running','Fiction','Snacks','Skincare',
        'Parts','Tools','Games','Tablets','Pants','Bath','Cycling','NonFiction',
        'Beverages','Makeup','Tires','Seeds','Puzzles'])[1 + g % 20],
    (ARRAY['BrandA','BrandB','BrandC','BrandD','BrandE','BrandF','BrandG',
        'BrandH','BrandI','BrandJ','BrandK','BrandL'])[1 + g % 12],
    (ARRAY['SupplierX','SupplierY','SupplierZ','SupplierW','SupplierV'])[1 + g % 5],
    round((5 + random() * 200)::numeric, 2),
    round((15 + random() * 500)::numeric, 2),
    round((0.05 + random() * 30)::numeric, 3),
    round((1 + random() * 100)::numeric, 1),
    round((1 + random() * 50)::numeric, 1),
    round((1 + random() * 40)::numeric, 1),
    (ARRAY['Red','Blue','Green','Black','White','Silver','Gold','Navy'])[1 + g % 8],
    (ARRAY['Plastic','Metal','Wood','Cotton','Leather','Glass','Ceramic'])[1 + g % 7],
    (ARRAY['CN','US','DE','JP','KR','TW','VN','MX','IN','IT'])[1 + g % 10],
    (ARRAY[3,6,12,24,36])[1 + g % 5],
    '2015-01-01'::date + (g % $N_DATES) * interval '1 day',
    round((10 + random() * 60)::numeric, 2)
FROM generate_series(1, $N_PRODUCTS) g;

-- Customer dimension
CREATE TABLE dim_customer (
    customer_key serial PRIMARY KEY,
    email        text NOT NULL,
    first_name   text NOT NULL,
    last_name    text NOT NULL,
    gender       char(1),
    birth_year   smallint,
    city         text,
    state        text,
    country      text DEFAULT 'US',
    zip          text,
    segment      text NOT NULL,
    acq_channel  text,
    acq_date     date,
    lifetime_value numeric(12,2) DEFAULT 0
);

INSERT INTO dim_customer (email, first_name, last_name, gender, birth_year,
    city, state, zip, segment, acq_channel, acq_date)
SELECT
    'c' || g || '@example.com',
    (ARRAY['James','Mary','John','Patricia','Robert','Jennifer','Michael',
        'Linda','David','Elizabeth'])[1 + g % 10],
    (ARRAY['Smith','Johnson','Williams','Brown','Jones','Garcia','Miller',
        'Davis','Rodriguez','Martinez'])[1 + (g/10) % 10],
    (ARRAY['M','F','M','F','M','F','M','F'])[1 + g % 8],
    1950 + (g % 55),
    (ARRAY['New York','Los Angeles','Chicago','Houston','Phoenix','Philadelphia',
        'San Antonio','San Diego','Dallas','San Jose'])[1 + g % 10],
    (ARRAY['NY','CA','IL','TX','AZ','PA','TX','CA','TX','CA'])[1 + g % 10],
    lpad((10000 + g % 90000)::text, 5, '0'),
    (ARRAY['Premium','Standard','Standard','Standard','Budget','Budget',
        'Premium','Standard'])[1 + g % 8],
    (ARRAY['Organic','Paid Search','Social','Email','Referral','Direct'])[1 + g % 6],
    '2015-01-01'::date + (g % $N_DATES) * interval '1 day'
FROM generate_series(1, $N_CUSTOMERS) g;

-- Store dimension
CREATE TABLE dim_store (
    store_key    serial PRIMARY KEY,
    store_name   text NOT NULL,
    store_type   text NOT NULL,
    region       text NOT NULL,
    district     text NOT NULL,
    city         text NOT NULL,
    state        text NOT NULL,
    open_date    date,
    sqft         int,
    employees    smallint
);

INSERT INTO dim_store (store_name, store_type, region, district, city, state,
    open_date, sqft, employees)
SELECT
    'Store #' || g,
    (ARRAY['Flagship','Standard','Outlet','Express','Online'])[1 + g % 5],
    (ARRAY['Northeast','Southeast','Midwest','Southwest','West','Northwest'])[1 + g % 6],
    'District ' || (1 + g % 30),
    (ARRAY['New York','Miami','Chicago','Dallas','Seattle','Denver','Boston',
        'Atlanta','Portland','Nashville'])[1 + g % 10],
    (ARRAY['NY','FL','IL','TX','WA','CO','MA','GA','OR','TN'])[1 + g % 10],
    '2010-01-01'::date + (g % 3650) * interval '1 day',
    2000 + (g % 10) * 1000,
    10 + g % 50
FROM generate_series(1, $N_STORES) g;

-- Fact table: sales transactions
CREATE TABLE fact_sales (
    sale_id      bigserial,
    date_key     int NOT NULL,
    product_key  int NOT NULL,
    customer_key int NOT NULL,
    store_key    int NOT NULL,
    quantity     smallint NOT NULL,
    unit_price   numeric(10,2) NOT NULL,
    discount_pct numeric(5,2) NOT NULL DEFAULT 0,
    tax_rate     numeric(5,4) NOT NULL,
    line_total   numeric(12,2) NOT NULL,
    cost_total   numeric(12,2) NOT NULL,
    profit       numeric(12,2) NOT NULL,
    payment_type text NOT NULL,
    is_returned  boolean DEFAULT false,
    promo_code   text
) PARTITION BY RANGE (date_key);

-- Create yearly partitions (2015-2024)
SQL

    for yr in $(seq 2015 2024); do
        local start_key=$(( (yr - 2015) * 365 + (yr - 2015) / 4 ))
        local end_key=$(( (yr - 2014) * 365 + (yr - 2014) / 4 ))
        "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
            CREATE TABLE fact_sales_${yr} PARTITION OF fact_sales
            FOR VALUES FROM ($start_key) TO ($end_key);"
    done

    "$PSQL" -p "$PGPORT" -d "$PGDB" -q <<SQL
-- Populate fact_sales
INSERT INTO fact_sales (date_key, product_key, customer_key, store_key,
    quantity, unit_price, discount_pct, tax_rate, line_total, cost_total,
    profit, payment_type, is_returned, promo_code)
SELECT
    (g * 97) % $N_DATES AS date_key,
    1 + (g * 31) % $N_PRODUCTS,
    1 + (g * 17) % $N_CUSTOMERS,
    1 + (g * 13) % $N_STORES,
    1 + (g % 10) AS qty,
    round((10 + (g * 7 % 500))::numeric, 2) AS unit_price,
    CASE WHEN g % 5 = 0 THEN round((5 + g % 25)::numeric, 2) ELSE 0 END AS discount_pct,
    (ARRAY[0.0000, 0.0600, 0.0725, 0.0800, 0.0825, 0.0900, 0.1000])[1 + g % 7] AS tax_rate,
    round(((1 + g % 10) * (10 + (g * 7 % 500)) *
        (1 - CASE WHEN g % 5 = 0 THEN (5 + g % 25)::numeric/100 ELSE 0 END))::numeric, 2),
    round(((1 + g % 10) * (5 + (g * 3 % 200)))::numeric, 2),
    round(((1 + g % 10) * (10 + (g * 7 % 500)) *
        (1 - CASE WHEN g % 5 = 0 THEN (5 + g % 25)::numeric/100 ELSE 0 END) -
        (1 + g % 10) * (5 + (g * 3 % 200)))::numeric, 2),
    (ARRAY['Cash','Credit','Debit','Mobile','Gift Card'])[1 + g % 5],
    g % 20 = 0,
    CASE WHEN g % 10 = 0 THEN 'PROMO' || (g % 50) ELSE NULL END
FROM generate_series(1, $N_SALES) g;

-- Indexes for dimension lookups (fact table uses partition pruning)
CREATE INDEX idx_fact_date ON fact_sales (date_key);
CREATE INDEX idx_fact_product ON fact_sales (product_key);
CREATE INDEX idx_fact_customer ON fact_sales (customer_key);
CREATE INDEX idx_fact_store ON fact_sales (store_key);
CREATE INDEX idx_product_category ON dim_product (category, subcategory);
CREATE INDEX idx_customer_segment ON dim_customer (segment);
CREATE INDEX idx_store_region ON dim_store (region);
CREATE INDEX idx_date_year_month ON dim_date (year, month);

ANALYZE;
SQL

    "$PSQL" -p "$PGPORT" -d "$PGDB" -t -A -c "
        SELECT 'fact_sales=' || (SELECT count(*) FROM fact_sales) ||
               ' dim_product=' || (SELECT count(*) FROM dim_product) ||
               ' dim_customer=' || (SELECT count(*) FROM dim_customer) ||
               ' dim_store=' || (SELECT count(*) FROM dim_store) ||
               ' dim_date=' || (SELECT count(*) FROM dim_date);"
}

# ================================================================
# OLAP query scripts — TPC-H/DS inspired
# ================================================================
write_pgbench_scripts() {
    local NP=$((50000 * SCALE / 10))
    local NC=$((500000 * SCALE / 10))
    local NS=$((1000 * SCALE / 10))

    # Q1: Revenue by quarter and category — full scan + hash join + group by + expressions
    # (TPC-H Q1 inspired: scans all of fact, computes derived columns)
    cat > "$TMPDIR/q1.sql" <<'EOSQL'
\set yr random(2018, 2023)
SELECT d.year, d.quarter, p.category,
       count(*) AS num_sales,
       sum(f.quantity) AS total_units,
       sum(f.line_total) AS gross_revenue,
       sum(f.cost_total) AS total_cost,
       sum(f.profit) AS total_profit,
       round(avg(f.discount_pct), 2) AS avg_discount,
       round(sum(f.profit) / NULLIF(sum(f.line_total), 0) * 100, 1) AS margin_pct,
       sum(CASE WHEN f.is_returned THEN f.line_total ELSE 0 END) AS returned_revenue,
       count(CASE WHEN f.is_returned THEN 1 END) AS num_returns
FROM fact_sales f
JOIN dim_date d ON d.date_key = f.date_key
JOIN dim_product p ON p.product_key = f.product_key
WHERE d.year = :yr
GROUP BY d.year, d.quarter, p.category
ORDER BY d.quarter, gross_revenue DESC;
EOSQL

    # Q2: Top products with ranking — window functions + hash join + complex expressions
    cat > "$TMPDIR/q2.sql" <<'EOSQL'
\set yr random(2018, 2023)
\set cat_idx random(0, 9)
SELECT * FROM (
    SELECT p.product_key, p.name, p.brand, p.subcategory,
           sum(f.line_total) AS revenue,
           sum(f.profit) AS profit,
           sum(f.quantity) AS units,
           count(DISTINCT f.customer_key) AS unique_buyers,
           round(sum(f.profit) / NULLIF(sum(f.line_total), 0) * 100, 1) AS margin,
           rank() OVER (ORDER BY sum(f.line_total) DESC) AS revenue_rank,
           rank() OVER (ORDER BY sum(f.profit) DESC) AS profit_rank,
           ntile(10) OVER (ORDER BY sum(f.line_total) DESC) AS revenue_decile
    FROM fact_sales f
    JOIN dim_product p ON p.product_key = f.product_key
    JOIN dim_date d ON d.date_key = f.date_key
    WHERE d.year = :yr
      AND p.category = (ARRAY['Electronics','Clothing','Home','Sports','Books',
          'Food','Beauty','Automotive','Garden','Toys'])[1 + :cat_idx]
    GROUP BY p.product_key, p.name, p.brand, p.subcategory
) ranked
WHERE revenue_rank <= 50;
EOSQL

    # Q3: Customer segmentation analysis — CASE expressions, aggregates, hash joins
    cat > "$TMPDIR/q3.sql" <<'EOSQL'
\set yr random(2019, 2023)
SELECT c.segment, c.state,
       count(DISTINCT c.customer_key) AS customers,
       count(*) AS transactions,
       sum(f.line_total) AS total_spend,
       round(avg(f.line_total), 2) AS avg_transaction,
       round(sum(f.line_total) / NULLIF(count(DISTINCT c.customer_key), 0), 2) AS spend_per_customer,
       sum(f.quantity) AS total_units,
       count(CASE WHEN f.discount_pct > 0 THEN 1 END) AS discounted_txns,
       round(count(CASE WHEN f.discount_pct > 0 THEN 1 END)::numeric /
             NULLIF(count(*), 0) * 100, 1) AS pct_discounted,
       count(CASE WHEN f.is_returned THEN 1 END) AS returns,
       CASE
           WHEN sum(f.line_total) / NULLIF(count(DISTINCT c.customer_key), 0) > 5000 THEN 'High Value'
           WHEN sum(f.line_total) / NULLIF(count(DISTINCT c.customer_key), 0) > 1000 THEN 'Medium Value'
           ELSE 'Low Value'
       END AS value_tier
FROM fact_sales f
JOIN dim_customer c ON c.customer_key = f.customer_key
JOIN dim_date d ON d.date_key = f.date_key
WHERE d.year = :yr
GROUP BY c.segment, c.state
ORDER BY total_spend DESC;
EOSQL

    # Q4: Year-over-year comparison — self-join pattern, heavy expressions
    cat > "$TMPDIR/q4.sql" <<'EOSQL'
\set yr random(2019, 2023)
SELECT cur.region, cur.month,
       cur.revenue AS cur_revenue,
       prev.revenue AS prev_revenue,
       cur.revenue - prev.revenue AS revenue_change,
       round((cur.revenue / NULLIF(prev.revenue, 0) - 1) * 100, 1) AS yoy_pct,
       cur.orders AS cur_orders,
       prev.orders AS prev_orders,
       cur.avg_order AS cur_avg_order,
       prev.avg_order AS prev_avg_order
FROM (
    SELECT s.region, d.month,
           sum(f.line_total) AS revenue,
           count(*) AS orders,
           round(avg(f.line_total), 2) AS avg_order
    FROM fact_sales f
    JOIN dim_date d ON d.date_key = f.date_key
    JOIN dim_store s ON s.store_key = f.store_key
    WHERE d.year = :yr
    GROUP BY s.region, d.month
) cur
JOIN (
    SELECT s.region, d.month,
           sum(f.line_total) AS revenue,
           count(*) AS orders,
           round(avg(f.line_total), 2) AS avg_order
    FROM fact_sales f
    JOIN dim_date d ON d.date_key = f.date_key
    JOIN dim_store s ON s.store_key = f.store_key
    WHERE d.year = :yr - 1
    GROUP BY s.region, d.month
) prev ON cur.region = prev.region AND cur.month = prev.month
ORDER BY cur.region, cur.month;
EOSQL

    # Q5: Monthly rolling averages — window functions over large result sets
    cat > "$TMPDIR/q5.sql" <<'EOSQL'
\set yr random(2018, 2023)
SELECT region, full_date, daily_revenue,
       round(avg(daily_revenue) OVER w, 2) AS rolling_7d_avg,
       round(avg(daily_revenue) OVER w2, 2) AS rolling_30d_avg,
       round(sum(daily_revenue) OVER (PARTITION BY region ORDER BY full_date
           ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW), 2) AS cumulative_revenue,
       round(daily_revenue / NULLIF(avg(daily_revenue) OVER w2, 0) * 100, 1) AS pct_of_30d_avg
FROM (
    SELECT s.region, d.full_date,
           sum(f.line_total) AS daily_revenue
    FROM fact_sales f
    JOIN dim_date d ON d.date_key = f.date_key
    JOIN dim_store s ON s.store_key = f.store_key
    WHERE d.year = :yr
    GROUP BY s.region, d.full_date
) daily
WINDOW w AS (PARTITION BY region ORDER BY full_date ROWS BETWEEN 6 PRECEDING AND CURRENT ROW),
       w2 AS (PARTITION BY region ORDER BY full_date ROWS BETWEEN 29 PRECEDING AND CURRENT ROW)
ORDER BY region, full_date;
EOSQL

    # Q6: Wide-table deform stress — reads all 20+ product columns + expressions
    cat > "$TMPDIR/q6.sql" <<'EOSQL'
\set yr random(2019, 2023)
SELECT p.product_key, p.sku, p.name, p.category, p.subcategory, p.brand,
       p.supplier, p.unit_cost, p.list_price, p.weight_kg, p.width_cm,
       p.height_cm, p.depth_cm, p.color, p.material, p.country_of_origin,
       p.warranty_months, p.launch_date, p.margin_pct,
       sum(f.quantity) AS units_sold,
       sum(f.line_total) AS revenue,
       sum(f.profit) AS profit,
       round(sum(f.profit) / NULLIF(sum(f.line_total), 0) * 100, 1) AS actual_margin,
       round(avg(f.discount_pct), 2) AS avg_discount,
       count(DISTINCT f.store_key) AS num_stores,
       count(DISTINCT f.customer_key) AS num_customers,
       CASE
           WHEN p.list_price < 50 THEN 'Budget'
           WHEN p.list_price < 150 THEN 'Mid-Range'
           WHEN p.list_price < 350 THEN 'Premium'
           ELSE 'Luxury'
       END AS price_tier,
       CASE
           WHEN p.weight_kg < 1 THEN 'Light'
           WHEN p.weight_kg < 5 THEN 'Medium'
           WHEN p.weight_kg < 15 THEN 'Heavy'
           ELSE 'Very Heavy'
       END AS weight_class,
       round(p.width_cm * p.height_cm * p.depth_cm / 1000000, 3) AS volume_m3
FROM dim_product p
JOIN fact_sales f ON f.product_key = p.product_key
JOIN dim_date d ON d.date_key = f.date_key
WHERE d.year = :yr
GROUP BY p.product_key, p.sku, p.name, p.category, p.subcategory, p.brand,
         p.supplier, p.unit_cost, p.list_price, p.weight_kg, p.width_cm,
         p.height_cm, p.depth_cm, p.color, p.material, p.country_of_origin,
         p.warranty_months, p.launch_date, p.margin_pct
HAVING sum(f.quantity) > 10
ORDER BY revenue DESC
LIMIT 200;
EOSQL

    # Q7: Store performance with complex scoring — heavy CASE + arithmetic expressions
    cat > "$TMPDIR/q7.sql" <<'EOSQL'
\set yr random(2019, 2023)
SELECT s.store_key, s.store_name, s.store_type, s.region, s.district,
       count(*) AS transactions,
       sum(f.line_total) AS revenue,
       sum(f.profit) AS profit,
       round(avg(f.line_total), 2) AS avg_basket,
       count(DISTINCT f.customer_key) AS unique_customers,
       count(DISTINCT f.product_key) AS product_variety,
       round(sum(f.profit) / NULLIF(sum(f.line_total), 0) * 100, 1) AS margin_pct,
       round(count(CASE WHEN f.is_returned THEN 1 END)::numeric /
             NULLIF(count(*), 0) * 100, 2) AS return_rate,
       -- Composite performance score (0-100)
       round((
           (sum(f.line_total) / NULLIF(max(s.sqft), 0)) * 0.3 +
           (count(DISTINCT f.customer_key)::numeric / NULLIF(max(s.employees), 0)) * 20 +
           (sum(f.profit) / NULLIF(sum(f.line_total), 0)) * 100 * 0.3 -
           (count(CASE WHEN f.is_returned THEN 1 END)::numeric /
            NULLIF(count(*), 0)) * 100 * 0.1
       )::numeric, 1) AS perf_score
FROM fact_sales f
JOIN dim_store s ON s.store_key = f.store_key
JOIN dim_date d ON d.date_key = f.date_key
WHERE d.year = :yr
GROUP BY s.store_key, s.store_name, s.store_type, s.region, s.district
ORDER BY perf_score DESC;
EOSQL

    # Q8: Revenue concentration (Pareto/80-20) — CTE + window + complex math
    cat > "$TMPDIR/q8.sql" <<'EOSQL'
\set yr random(2018, 2023)
WITH customer_revenue AS (
    SELECT c.customer_key, c.segment, c.state,
           sum(f.line_total) AS total_spend,
           sum(f.profit) AS total_profit,
           count(*) AS num_txns,
           count(DISTINCT f.product_key) AS num_products
    FROM fact_sales f
    JOIN dim_customer c ON c.customer_key = f.customer_key
    JOIN dim_date d ON d.date_key = f.date_key
    WHERE d.year = :yr
    GROUP BY c.customer_key, c.segment, c.state
),
ranked AS (
    SELECT *,
           row_number() OVER (ORDER BY total_spend DESC) AS spend_rank,
           sum(total_spend) OVER (ORDER BY total_spend DESC
               ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS cumul_spend,
           sum(total_spend) OVER () AS grand_total,
           ntile(100) OVER (ORDER BY total_spend DESC) AS percentile
    FROM customer_revenue
)
SELECT percentile,
       count(*) AS num_customers,
       round(sum(total_spend), 2) AS group_spend,
       round(sum(total_profit), 2) AS group_profit,
       round(min(cumul_spend) / NULLIF(max(grand_total), 0) * 100, 2) AS cumul_pct_start,
       round(max(cumul_spend) / NULLIF(max(grand_total), 0) * 100, 2) AS cumul_pct_end,
       round(avg(total_spend), 2) AS avg_spend,
       round(avg(num_txns), 1) AS avg_txns,
       round(avg(num_products), 1) AS avg_products
FROM ranked
GROUP BY percentile
ORDER BY percentile;
EOSQL
}

# ================================================================
# CPU/Memory monitor
# ================================================================
start_monitor() {
    local csv_file="$1"
    echo "elapsed_s,total_cpu_pct,max_proc_cpu_pct,total_rss_mb,max_proc_rss_mb,n_active" > "$csv_file"

    (
        local start_time=$(date +%s)
        while true; do
            local now=$(date +%s)
            local elapsed=$((now - start_time))

            local stats
            stats=$(ps -eo pid,ucomm,%cpu,rss 2>/dev/null | \
                    grep "[p]ostgres" | \
                    awk '
                    {
                        cpu = $3; rss_kb = $4
                        total_cpu += cpu
                        total_rss += rss_kb
                        if (cpu > max_cpu) max_cpu = cpu
                        if (rss_kb > max_rss) max_rss = rss_kb
                        if (cpu > 0.1) n_active++
                        n++
                    }
                    END {
                        if (n > 0)
                            printf "%.1f,%.1f,%.1f,%.1f,%d\n", total_cpu, max_cpu, total_rss/1024, max_rss/1024, n_active
                        else
                            printf "0,0,0,0,0\n"
                    }')

            echo "${elapsed},${stats}" >> "$csv_file"
            sleep 1
        done
    ) &
    MONITOR_PID=$!
}

stop_monitor() {
    if [ -n "$MONITOR_PID" ]; then
        kill $MONITOR_PID 2>/dev/null || true
        wait $MONITOR_PID 2>/dev/null || true
        MONITOR_PID=""
    fi
}

summarize_monitor() {
    local csv_file="$1"
    awk -F, '
    NR <= 1 { next }
    NR <= 6 { next }
    {
        tcpu = $2; mcpu = $3; trss = $4; mrss = $5; nact = $6
        sum_tcpu += tcpu; sum_trss += trss; sum_nact += nact
        if (tcpu > peak_tcpu) peak_tcpu = tcpu
        if (mcpu > peak_mcpu) peak_mcpu = mcpu
        if (trss > peak_trss) peak_trss = trss
        if (mrss > peak_mrss) peak_mrss = mrss
        count++
    }
    END {
        if (count > 0) {
            printf "  CPU total: avg=%.1f%%  peak=%.1f%%\n", sum_tcpu/count, peak_tcpu
            printf "  CPU/proc:  peak=%.1f%%\n", peak_mcpu
            printf "  RSS total: avg=%.0f MB  peak=%.0f MB\n", sum_trss/count, peak_trss
            printf "  RSS/proc:  peak=%.0f MB\n", peak_mrss
            printf "  Active PG: avg=%.0f\n", sum_nact/count
        }
    }' "$csv_file"
}

# ================================================================
# Apply JIT settings
# ================================================================
apply_jit_settings() {
    local backend="$1"
    case "$backend" in
        nojit)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = off;
                ALTER DATABASE $PGDB SET work_mem = '256MB';"
            ;;
        sljit|asmjit|mir)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = on;
                ALTER DATABASE $PGDB SET jit_above_cost = 0;
                ALTER DATABASE $PGDB SET work_mem = '256MB';"
            "$PSQL" -p "$PGPORT" -d postgres -q -c \
                "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';"
            "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 2
            ;;
        auto)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = on;
                ALTER DATABASE $PGDB SET jit_above_cost = 0;
                ALTER DATABASE $PGDB SET work_mem = '256MB';"
            "$PSQL" -p "$PGPORT" -d postgres -q -c \
                "ALTER SYSTEM SET jit_provider = 'pg_jitter';"
            "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 2
            ;;
    esac
}

# ================================================================
# Run one backend test
# ================================================================
run_backend() {
    local backend="$1"
    local csv_file="$OUTDIR/${backend}_monitor.csv"
    local pgbench_log="$OUTDIR/${backend}_pgbench.log"

    echo ""
    echo "================================================================"
    echo "  $backend  —  ${DURATION}s sustained OLAP load, $CLIENTS clients"
    echo "================================================================"

    apply_jit_settings "$backend"

    # Warmup
    if [ "$WARMUP" -gt 0 ]; then
        echo "  Warming up (${WARMUP}s)..."
        "$PGBENCH" -p "$PGPORT" -d "$PGDB" \
            -f "$TMPDIR/q1.sql@20" -f "$TMPDIR/q2.sql@15" \
            -f "$TMPDIR/q3.sql@15" -f "$TMPDIR/q4.sql@15" \
            -f "$TMPDIR/q5.sql@10" -f "$TMPDIR/q6.sql@10" \
            -f "$TMPDIR/q7.sql@10" -f "$TMPDIR/q8.sql@5" \
            -c "$CLIENTS" -j "$THREADS" -T "$WARMUP" \
            --no-vacuum -n > /dev/null 2>&1 || true
    fi

    # Start monitoring
    start_monitor "$csv_file"

    echo "  Running pgbench (${DURATION}s)..."
    "$PGBENCH" -p "$PGPORT" -d "$PGDB" \
        -f "$TMPDIR/q1.sql@20" -f "$TMPDIR/q2.sql@15" \
        -f "$TMPDIR/q3.sql@15" -f "$TMPDIR/q4.sql@15" \
        -f "$TMPDIR/q5.sql@10" -f "$TMPDIR/q6.sql@10" \
        -f "$TMPDIR/q7.sql@10" -f "$TMPDIR/q8.sql@5" \
        -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
        --no-vacuum -n -P 30 \
        2>&1 | tee "$pgbench_log"

    stop_monitor

    # Extract TPS and latency
    local tps lat ntx
    tps=$(grep "^tps" "$pgbench_log" | head -1 | awk '{print $3}')
    lat=$(grep "^latency average" "$pgbench_log" | head -1 | awk '{print $4}')
    ntx=$(grep "^number of transactions actually processed" "$pgbench_log" | head -1 | awk '{print $NF}')

    echo ""
    echo "  --- $backend results ---"
    echo "  TPS: $tps  |  Avg latency: ${lat}ms  |  Total txns: $ntx"
    summarize_monitor "$csv_file"
    echo ""

    RESULTS+=("$backend|$tps|$lat|$ntx")

    MON_SUMMARIES+=("$backend|$(awk -F, '
    NR<=1{next} NR<=6{next}
    {tcpu=$2; trss=$4; if(tcpu>mc)mc=tcpu; if(trss>mr)mr=trss; sc+=tcpu; sr+=trss; c++}
    END{if(c>0) printf "%.1f|%.1f|%.0f|%.0f", sc/c, mc, sr/c, mr; else print "0|0|0|0"}
    ' "$csv_file")")
}

# ================================================================
# Main
# ================================================================
echo "============================================"
echo "  OLAP Sustained Load Benchmark"
echo "============================================"
echo ""
echo "  Port:       $PGPORT"
echo "  Database:   $PGDB"
echo "  Scale:      $SCALE (~$((10000000 * SCALE / 10)) fact rows)"
echo "  Clients:    $CLIENTS"
echo "  Threads:    $THREADS"
echo "  Duration:   ${DURATION}s per backend ($(( DURATION / 60 ))m$(( DURATION % 60 ))s)"
echo "  Warmup:     ${WARMUP}s"
echo "  Backends:   $BACKENDS"
echo "  Output:     $OUTDIR"
echo ""

if [ "$SKIP_LOAD" -eq 0 ]; then
    echo "$(load_data)"
fi

write_pgbench_scripts

declare -a RESULTS
declare -a MON_SUMMARIES

for backend in $BACKENDS; do
    run_backend "$backend"
done

# ================================================================
# Final summary
# ================================================================
SUMMARY_FILE="$OUTDIR/summary.txt"

{
echo ""
echo "================================================================"
echo "  OLAP SUSTAINED LOAD BENCHMARK SUMMARY"
echo "  $(date)"
echo "  Duration: ${DURATION}s per backend | Clients: $CLIENTS | Scale: $SCALE"
echo "================================================================"
echo ""
printf "%-10s %8s %10s %10s  %8s %8s  %8s %8s\n" \
    "Backend" "TPS" "Lat(ms)" "Txns" "CPU avg" "CPU peak" "RSS avg" "RSS peak"
printf "%-10s %8s %10s %10s  %8s %8s  %8s %8s\n" \
    "-------" "------" "--------" "--------" "-------" "--------" "-------" "--------"

NOJIT_TPS=""
for i in "${!RESULTS[@]}"; do
    IFS='|' read -r label tps lat ntx <<< "${RESULTS[$i]}"
    IFS='|' read -r _lbl avg_cpu peak_cpu avg_rss peak_rss <<< "${MON_SUMMARIES[$i]}"

    pct_str=""
    if [ "$label" = "nojit" ]; then
        NOJIT_TPS="$tps"
        pct_str="(baseline)"
    elif [ -n "$NOJIT_TPS" ] && [ -n "$tps" ]; then
        pct=$(echo "scale=4; ($tps / $NOJIT_TPS - 1) * 100" | bc 2>/dev/null || echo "?")
        if [ "$pct" != "?" ]; then
            if echo "$pct" | grep -q "^-"; then
                pct_str="(${pct}%)"
            else
                pct_str="(+${pct}%)"
            fi
        fi
    fi

    printf "%-10s %8s %10s %10s  %7s%% %7s%%  %6sMB %6sMB  %s\n" \
        "$label" "$tps" "$lat" "$ntx" "$avg_cpu" "$peak_cpu" "$avg_rss" "$peak_rss" "$pct_str"
done

echo ""
echo "Query mix: quarterly_revenue(20%) top_products(15%) customer_segments(15%)"
echo "           yoy_comparison(15%) rolling_averages(10%) wide_table_deform(10%)"
echo "           store_scoring(10%) pareto_analysis(5%)"
echo ""
echo "Monitor CSVs: $OUTDIR/<backend>_monitor.csv"
echo "pgbench logs: $OUTDIR/<backend>_pgbench.log"
} | tee "$SUMMARY_FILE"

echo ""
echo "Full summary saved to: $SUMMARY_FILE"
