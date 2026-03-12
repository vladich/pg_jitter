#!/bin/bash
# bench_ecommerce_sustained.sh — Sustained e-commerce workload with CPU/memory monitoring
#
# Runs pgbench for 5 minutes per backend while sampling CPU and RSS of all
# postgres processes every second.  Produces a per-backend summary with
# TPS, avg/peak CPU%, avg/peak RSS, and a per-second CSV log.
#
# Usage:
#   ./tests/bench_ecommerce_sustained.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: bench_ecommerce)
#   --scale N          Data scale factor (default: 10)
#   --clients N        Concurrent pgbench clients (default: 8)
#   --threads N        pgbench threads (default: 4)
#   --duration N       Seconds per backend (default: 300 = 5 min)
#   --warmup N         Warmup seconds (default: 30)
#   --backends LIST    Backends to test (default: "nojit sljit asmjit mir auto")
#   --skip-load        Skip data loading (reuse existing bench_ecommerce DB)
#   --outdir DIR       Output directory (default: tests/sustained_<timestamp>)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-bench_ecommerce}"
SCALE=10
CLIENTS=8
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
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PGCTL="$PGBIN/pg_ctl"

PGDATA="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"
if [ -z "$PGDATA" ] || [ ! -d "$PGDATA" ]; then
    echo "ERROR: Cannot determine PGDATA. Is PostgreSQL running on port $PGPORT?"
    exit 1
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
if [ -z "$OUTDIR" ]; then
    OUTDIR="$SCRIPT_DIR/sustained_${TIMESTAMP}"
fi
mkdir -p "$OUTDIR"

TMPDIR=$(mktemp -d)

# Save and restore original provider
ORIG_PROVIDER="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW jit_provider;" 2>/dev/null)"

cleanup() {
    # Kill any lingering monitor
    kill $MONITOR_PID 2>/dev/null || true
    # Restore provider
    if [ -n "$ORIG_PROVIDER" ]; then
        "$PSQL" -p "$PGPORT" -d postgres -q -c \
            "ALTER SYSTEM SET jit_provider = '$ORIG_PROVIDER';" 2>/dev/null || true
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1 || true
    fi
    "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
        ALTER DATABASE $PGDB RESET jit;
        ALTER DATABASE $PGDB RESET jit_above_cost;
    " 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT
MONITOR_PID=""

# ================================================================
# Schema + data loading (same as bench_ecommerce.sh)
# ================================================================
load_data() {
    echo "=== Creating database $PGDB ==="
    "$PSQL" -p "$PGPORT" -d postgres -q -c "DROP DATABASE IF EXISTS $PGDB;" 2>/dev/null
    "$PSQL" -p "$PGPORT" -d postgres -q -c "CREATE DATABASE $PGDB;"

    echo "=== Loading schema (scale=$SCALE) ==="
    local N_CATEGORIES=200
    local N_PRODUCTS=$((5000 * SCALE))
    local N_CUSTOMERS=$((10000 * SCALE))
    local N_ORDERS=$((100000 * SCALE))
    local N_REVIEWS=$((50000 * SCALE))

    "$PSQL" -p "$PGPORT" -d "$PGDB" -q <<SQL
CREATE TABLE categories (
    category_id serial PRIMARY KEY, parent_id int REFERENCES categories(category_id),
    name text NOT NULL, slug text NOT NULL, description text,
    is_active boolean DEFAULT true, sort_order int DEFAULT 0);
CREATE TABLE products (
    product_id serial PRIMARY KEY, sku text NOT NULL UNIQUE, name text NOT NULL,
    description text, category_id int NOT NULL REFERENCES categories(category_id),
    brand text, price numeric(10,2) NOT NULL, cost numeric(10,2),
    weight_kg numeric(6,3), is_active boolean DEFAULT true,
    created_at timestamptz DEFAULT now(), updated_at timestamptz DEFAULT now(),
    attrs jsonb DEFAULT '{}');
CREATE TABLE inventory (
    product_id int PRIMARY KEY REFERENCES products(product_id),
    qty_available int NOT NULL DEFAULT 0, qty_reserved int NOT NULL DEFAULT 0,
    warehouse text DEFAULT 'main', last_restock timestamptz);
CREATE TABLE customers (
    customer_id serial PRIMARY KEY, email text NOT NULL UNIQUE,
    first_name text NOT NULL, last_name text NOT NULL, phone text,
    address_line1 text, address_line2 text, city text, state text,
    zip text, country text DEFAULT 'US', created_at timestamptz DEFAULT now(),
    tier text DEFAULT 'standard');
CREATE TABLE orders (
    order_id serial PRIMARY KEY,
    customer_id int NOT NULL REFERENCES customers(customer_id),
    status text NOT NULL DEFAULT 'pending',
    order_date timestamptz NOT NULL DEFAULT now(), ship_date timestamptz,
    subtotal numeric(12,2) NOT NULL DEFAULT 0, tax numeric(10,2) NOT NULL DEFAULT 0,
    shipping numeric(10,2) NOT NULL DEFAULT 0, total numeric(12,2) NOT NULL DEFAULT 0,
    payment_method text, shipping_method text);
CREATE TABLE order_items (
    item_id serial PRIMARY KEY,
    order_id int NOT NULL REFERENCES orders(order_id),
    product_id int NOT NULL REFERENCES products(product_id),
    quantity int NOT NULL DEFAULT 1, unit_price numeric(10,2) NOT NULL,
    discount_pct numeric(5,2) DEFAULT 0, line_total numeric(12,2) NOT NULL);
CREATE TABLE reviews (
    review_id serial PRIMARY KEY,
    product_id int NOT NULL REFERENCES products(product_id),
    customer_id int NOT NULL REFERENCES customers(customer_id),
    rating smallint NOT NULL CHECK (rating BETWEEN 1 AND 5),
    title text, body text, helpful_votes int DEFAULT 0,
    verified boolean DEFAULT false, created_at timestamptz DEFAULT now());

INSERT INTO categories (name, slug, description, sort_order)
SELECT 'Category ' || g, 'cat-' || g, 'Top-level category ' || g, g
FROM generate_series(1, 20) g;
INSERT INTO categories (parent_id, name, slug, description, sort_order)
SELECT (g % 20) + 1, 'Subcategory ' || g, 'subcat-' || g, 'Child category ' || g, g
FROM generate_series(1, $N_CATEGORIES - 20) g;

INSERT INTO products (sku, name, description, category_id, brand, price, cost, weight_kg, attrs)
SELECT 'SKU-' || lpad(g::text, 8, '0'),
       'Product ' || g || ' ' || (ARRAY['Widget','Gadget','Tool','Device','Component','Module','Kit','Pack','Set','Bundle'])[1 + g % 10],
       'Description for product ' || g,
       1 + (g % $N_CATEGORIES),
       (ARRAY['Acme','TechCo','ProBuild','QuickShip','ValueMax','PremiumPlus','EcoLine','MegaCorp'])[1 + g % 8],
       round((10 + random() * 990)::numeric, 2),
       round((5 + random() * 400)::numeric, 2),
       round((0.1 + random() * 25)::numeric, 3),
       jsonb_build_object('color', (ARRAY['red','blue','green','black','white','silver'])[1 + (g % 6)],
                          'size', (ARRAY['S','M','L','XL','XXL'])[1 + (g % 5)])
FROM generate_series(1, $N_PRODUCTS) g;

INSERT INTO inventory (product_id, qty_available, qty_reserved, last_restock)
SELECT product_id, (random()*500)::int, (random()*20)::int, now()-(random()*interval '90 days')
FROM products;

INSERT INTO customers (email, first_name, last_name, phone, city, state, zip, tier)
SELECT 'user' || g || '@example.com',
       (ARRAY['John','Jane','Bob','Alice','Charlie','Diana','Eve','Frank','Grace','Henry'])[1 + g % 10],
       (ARRAY['Smith','Johnson','Williams','Brown','Jones','Garcia','Miller','Davis','Wilson','Taylor'])[1 + (g/10) % 10],
       '+1-555-' || lpad((g % 10000)::text, 4, '0'),
       (ARRAY['New York','Los Angeles','Chicago','Houston','Phoenix'])[1 + g % 5],
       (ARRAY['NY','CA','IL','TX','AZ'])[1 + g % 5],
       lpad((10000 + g % 90000)::text, 5, '0'),
       (ARRAY['standard','standard','standard','silver','silver','gold','platinum'])[1 + g % 7]
FROM generate_series(1, $N_CUSTOMERS) g;

INSERT INTO orders (customer_id, status, order_date, payment_method, shipping_method)
SELECT 1 + (g % $N_CUSTOMERS),
       (ARRAY['completed','completed','completed','completed','shipped','processing','pending','cancelled'])[1 + g % 8],
       now() - (random() * interval '730 days'),
       (ARRAY['credit_card','debit_card','paypal','apple_pay','bank_transfer'])[1 + g % 5],
       (ARRAY['standard','express','overnight','pickup'])[1 + g % 4]
FROM generate_series(1, $N_ORDERS) g;

INSERT INTO order_items (order_id, product_id, quantity, unit_price, discount_pct, line_total)
SELECT o.order_id, 1 + ((o.order_id * 7 + item_num) % $N_PRODUCTS),
       1 + (o.order_id + item_num) % 5, p.price,
       CASE WHEN random() < 0.2 THEN round((random() * 20)::numeric, 2) ELSE 0 END,
       round(p.price * (1 + (o.order_id + item_num) % 5), 2)
FROM orders o
CROSS JOIN generate_series(1, 1 + (o.order_id % 5)) AS item_num
JOIN products p ON p.product_id = 1 + ((o.order_id * 7 + item_num) % $N_PRODUCTS);

UPDATE orders SET subtotal = oi.s, tax = round(oi.s * 0.08, 2),
       shipping = CASE WHEN oi.s > 100 THEN 0 ELSE 9.99 END,
       total = oi.s + round(oi.s * 0.08, 2) + CASE WHEN oi.s > 100 THEN 0 ELSE 9.99 END
FROM (SELECT order_id, sum(line_total) AS s FROM order_items GROUP BY order_id) oi
WHERE orders.order_id = oi.order_id;
UPDATE orders SET ship_date = order_date + interval '2 days' + random() * interval '5 days'
WHERE status IN ('shipped', 'completed');

INSERT INTO reviews (product_id, customer_id, rating, title, body, helpful_votes, verified, created_at)
SELECT 1 + (g % $N_PRODUCTS), 1 + (g % $N_CUSTOMERS), 1 + (g % 5),
       (ARRAY['Great!','Not bad','Excellent','Disappointing','Average'])[1 + g % 5],
       'Review text for product ' || g, (random()*50)::int, random() < 0.7,
       now() - (random() * interval '365 days')
FROM generate_series(1, $N_REVIEWS) g;

CREATE INDEX idx_products_category ON products(category_id) WHERE is_active;
CREATE INDEX idx_products_brand ON products(brand);
CREATE INDEX idx_products_price ON products(price);
CREATE INDEX idx_products_attrs ON products USING gin(attrs);
CREATE INDEX idx_orders_customer ON orders(customer_id);
CREATE INDEX idx_orders_date ON orders(order_date DESC);
CREATE INDEX idx_orders_status ON orders(status);
CREATE INDEX idx_order_items_order ON order_items(order_id);
CREATE INDEX idx_order_items_product ON order_items(product_id);
CREATE INDEX idx_reviews_product ON reviews(product_id);
CREATE INDEX idx_reviews_rating ON reviews(product_id, rating);
CREATE INDEX idx_inventory_qty ON inventory(qty_available) WHERE qty_available > 0;
CREATE INDEX idx_customers_tier ON customers(tier);
CREATE INDEX idx_categories_parent ON categories(parent_id);
ANALYZE;
SQL

    "$PSQL" -p "$PGPORT" -d "$PGDB" -t -A -c "
        SELECT 'products=' || (SELECT count(*) FROM products) ||
               ' customers=' || (SELECT count(*) FROM customers) ||
               ' orders=' || (SELECT count(*) FROM orders) ||
               ' items=' || (SELECT count(*) FROM order_items) ||
               ' reviews=' || (SELECT count(*) FROM reviews);"
}

# ================================================================
# pgbench query scripts
# ================================================================
write_pgbench_scripts() {
    local NP=$((5000 * SCALE))
    local NC=$((10000 * SCALE))
    local NO=$((100000 * SCALE))
    local NCAT=200

    cat > "$TMPDIR/q1.sql" <<EOSQL
\set pid random(1, $NP)
SELECT p.product_id, p.name, p.description, p.brand, p.price, p.weight_kg,
       p.attrs, c.name AS category_name, i.qty_available,
       COALESCE(rs.avg_rating, 0) AS avg_rating, COALESCE(rs.review_count, 0) AS review_count
FROM products p
JOIN categories c ON c.category_id = p.category_id
LEFT JOIN inventory i ON i.product_id = p.product_id
LEFT JOIN LATERAL (
    SELECT round(avg(rating)::numeric, 1) AS avg_rating, count(*) AS review_count
    FROM reviews r WHERE r.product_id = p.product_id
) rs ON true
WHERE p.product_id = :pid;
EOSQL

    cat > "$TMPDIR/q2.sql" <<EOSQL
\set cid random(1, $NCAT)
SELECT p.product_id, p.name, p.brand, p.price,
       COALESCE(rs.avg_rating, 0) AS avg_rating, COALESCE(rs.cnt, 0) AS review_count,
       i.qty_available > 0 AS in_stock
FROM products p
LEFT JOIN LATERAL (
    SELECT round(avg(rating)::numeric,1) AS avg_rating, count(*) AS cnt
    FROM reviews r WHERE r.product_id = p.product_id
) rs ON true
LEFT JOIN inventory i ON i.product_id = p.product_id
WHERE p.category_id = :cid AND p.is_active
ORDER BY rs.cnt DESC NULLS LAST, p.price ASC
LIMIT 20;
EOSQL

    cat > "$TMPDIR/q3.sql" <<EOSQL
\set min_price random(10, 200)
\set max_price random(300, 1000)
\set brand_idx random(0, 7)
\set pg random(0, 5)
SELECT p.product_id, p.name, p.brand, p.price, c.name AS cat, i.qty_available > 0 AS in_stock
FROM products p
JOIN categories c ON c.category_id = p.category_id
LEFT JOIN inventory i ON i.product_id = p.product_id
WHERE p.is_active AND p.price BETWEEN :min_price AND :max_price
  AND p.brand = (ARRAY['Acme','TechCo','ProBuild','QuickShip','ValueMax','PremiumPlus','EcoLine','MegaCorp'])[1 + :brand_idx]
ORDER BY p.price ASC LIMIT 20 OFFSET :pg * 20;
EOSQL

    cat > "$TMPDIR/q4.sql" <<EOSQL
\set cust_id random(1, $NC)
SELECT o.order_id, o.status, o.order_date, o.total, o.payment_method,
       count(oi.item_id) AS item_count, sum(oi.quantity) AS total_items
FROM orders o
JOIN order_items oi ON oi.order_id = o.order_id
WHERE o.customer_id = :cust_id
GROUP BY o.order_id, o.status, o.order_date, o.total, o.payment_method
ORDER BY o.order_date DESC LIMIT 10;
EOSQL

    cat > "$TMPDIR/q5.sql" <<EOSQL
\set oid random(1, $NO)
SELECT o.order_id, o.status, o.order_date, o.ship_date,
       o.subtotal, o.tax, o.shipping, o.total,
       cust.first_name || ' ' || cust.last_name AS customer_name, cust.email,
       oi.quantity, oi.unit_price, oi.discount_pct, oi.line_total,
       p.name AS product_name, p.sku
FROM orders o
JOIN customers cust ON cust.customer_id = o.customer_id
JOIN order_items oi ON oi.order_id = o.order_id
JOIN products p ON p.product_id = oi.product_id
WHERE o.order_id = :oid;
EOSQL

    cat > "$TMPDIR/q6.sql" <<EOSQL
\set pid random(1, $NP)
SELECT r.rating, r.title, r.body, r.helpful_votes, r.verified, r.created_at,
       c.first_name || ' ' || substr(c.last_name,1,1) || '.' AS reviewer
FROM reviews r JOIN customers c ON c.customer_id = r.customer_id
WHERE r.product_id = :pid
ORDER BY r.helpful_votes DESC, r.created_at DESC LIMIT 10;
EOSQL

    cat > "$TMPDIR/q7.sql" <<EOSQL
\set days_back random(7, 90)
SELECT date_trunc('day', o.order_date) AS day,
       count(DISTINCT o.order_id) AS orders, count(DISTINCT o.customer_id) AS customers,
       sum(o.total) AS revenue, avg(o.total) AS avg_order, sum(oi.quantity) AS items_sold
FROM orders o JOIN order_items oi ON oi.order_id = o.order_id
WHERE o.order_date >= now() - make_interval(days => :days_back)
  AND o.status IN ('completed','shipped')
GROUP BY date_trunc('day', o.order_date) ORDER BY day DESC;
EOSQL

    cat > "$TMPDIR/q8.sql" <<EOSQL
\set days_back random(30, 180)
SELECT p.product_id, p.name, p.brand, p.price, c.name AS category,
       count(DISTINCT oi.order_id) AS orders, sum(oi.quantity) AS units,
       sum(oi.line_total) AS revenue,
       COALESCE(rv.avg_rating, 0) AS avg_rating
FROM order_items oi
JOIN orders o ON o.order_id = oi.order_id
JOIN products p ON p.product_id = oi.product_id
JOIN categories c ON c.category_id = p.category_id
LEFT JOIN LATERAL (
    SELECT round(avg(rating)::numeric,1) AS avg_rating FROM reviews r WHERE r.product_id = p.product_id
) rv ON true
WHERE o.order_date >= now() - make_interval(days => :days_back) AND o.status IN ('completed','shipped')
GROUP BY p.product_id, p.name, p.brand, p.price, c.name, rv.avg_rating
ORDER BY revenue DESC LIMIT 20;
EOSQL
}

# ================================================================
# CPU/Memory monitor — samples all postgres processes every second
# ================================================================
start_monitor() {
    local csv_file="$1"
    echo "elapsed_s,total_cpu_pct,max_proc_cpu_pct,total_rss_mb,max_proc_rss_mb,n_active" > "$csv_file"

    (
        local start_time=$(date +%s)
        while true; do
            local now=$(date +%s)
            local elapsed=$((now - start_time))

            # Sample all postgres processes; track total CPU, max single-process CPU,
            # total RSS, max single-process RSS, and count of processes using >0.1% CPU
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

# ================================================================
# Summarize monitor CSV
# ================================================================
summarize_monitor() {
    local csv_file="$1"
    # CSV: elapsed_s,total_cpu_pct,max_proc_cpu_pct,total_rss_mb,max_proc_rss_mb,n_active
    # Skip header + first 5 samples (warmup settling)
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
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "ALTER DATABASE $PGDB SET jit = off;"
            ;;
        sljit|asmjit|mir)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = on;
                ALTER DATABASE $PGDB SET jit_above_cost = 0;"
            "$PSQL" -p "$PGPORT" -d postgres -q -c \
                "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';"
            "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 2
            ;;
        auto)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = on;
                ALTER DATABASE $PGDB SET jit_above_cost = 0;"
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
    echo "  $backend  —  ${DURATION}s sustained load, $CLIENTS clients"
    echo "================================================================"

    apply_jit_settings "$backend"

    # Warmup (no monitoring)
    if [ "$WARMUP" -gt 0 ]; then
        echo "  Warming up (${WARMUP}s)..."
        "$PGBENCH" -p "$PGPORT" -d "$PGDB" \
            -f "$TMPDIR/q1.sql@30" -f "$TMPDIR/q2.sql@20" \
            -f "$TMPDIR/q3.sql@15" -f "$TMPDIR/q4.sql@15" \
            -f "$TMPDIR/q5.sql@10" -f "$TMPDIR/q6.sql@5" \
            -f "$TMPDIR/q7.sql@3"  -f "$TMPDIR/q8.sql@2" \
            -c "$CLIENTS" -j "$THREADS" -T "$WARMUP" \
            --no-vacuum -n > /dev/null 2>&1 || true
    fi

    # Start monitoring
    start_monitor "$csv_file"
    local mon_start=$(date +%s)

    echo "  Running pgbench (${DURATION}s)..."
    "$PGBENCH" -p "$PGPORT" -d "$PGDB" \
        -f "$TMPDIR/q1.sql@30" -f "$TMPDIR/q2.sql@20" \
        -f "$TMPDIR/q3.sql@15" -f "$TMPDIR/q4.sql@15" \
        -f "$TMPDIR/q5.sql@10" -f "$TMPDIR/q6.sql@5" \
        -f "$TMPDIR/q7.sql@3"  -f "$TMPDIR/q8.sql@2" \
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

    # Store for summary
    RESULTS+=("$backend|$tps|$lat|$ntx")

    # Store monitor summary: avg_cpu|peak_cpu|avg_rss|peak_rss
    # CSV: elapsed_s,total_cpu_pct,max_proc_cpu_pct,total_rss_mb,max_proc_rss_mb,n_active
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
echo "  E-Commerce Sustained Load Benchmark"
echo "============================================"
echo ""
echo "  Port:       $PGPORT"
echo "  Database:   $PGDB"
echo "  Scale:      $SCALE (~$((100000 * SCALE)) orders)"
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
echo "  SUSTAINED LOAD BENCHMARK SUMMARY"
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
echo "Query mix: product_detail(30%) category_browse(20%) search_filter(15%)"
echo "           order_history(15%) order_detail(10%) reviews(5%)"
echo "           sales_dashboard(3%) top_products(2%)"
echo ""
echo "Monitor CSVs: $OUTDIR/<backend>_monitor.csv"
echo "pgbench logs: $OUTDIR/<backend>_pgbench.log"
} | tee "$SUMMARY_FILE"

echo ""
echo "Full summary saved to: $SUMMARY_FILE"
