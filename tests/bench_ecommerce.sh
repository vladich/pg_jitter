#!/bin/bash
# bench_ecommerce.sh — Sustained e-commerce read workload via pgbench
#
# Creates a realistic e-commerce schema (products, categories, customers,
# orders, order_items, reviews, inventory) and hammers it with randomized
# read queries typical of an online store.  Compares throughput (TPS) with
# JIT off vs. each pg_jitter backend.
#
# Usage:
#   ./tests/bench_ecommerce.sh [options]
#
# Options:
#   --pg-config PATH   Path to pg_config (default: $PG_CONFIG or pg_config)
#   --port PORT        PostgreSQL port (default: $PGPORT or 5433)
#   --db DB            Database name (default: bench_ecommerce)
#   --scale N          Data scale factor (default: 10, ~1M orders)
#   --clients N        Concurrent pgbench clients (default: 8)
#   --threads N        pgbench threads (default: 4)
#   --duration N       Seconds per test run (default: 60)
#   --warmup N         Warmup seconds (default: 10)
#   --backends LIST    Backends to test (default: "nojit sljit asmjit mir auto")
#   --skip-load        Skip schema creation / data loading
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5433}"
PGDB="${PGDB:-bench_ecommerce}"
SCALE=10
CLIENTS=8
THREADS=4
DURATION=60
WARMUP=10
BACKENDS="nojit sljit asmjit mir auto"
SKIP_LOAD=0

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
        *) echo "Unknown option: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PSQL="$PGBIN/psql"
PGBENCH="$PGBIN/pgbench"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PGCTL="$PGBIN/pg_ctl"

# Detect PGDATA from running server
PGDATA="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || echo "")"
if [ -z "$PGDATA" ] || [ ! -d "$PGDATA" ]; then
    echo "ERROR: Cannot determine PGDATA. Is PostgreSQL running on port $PGPORT?"
    exit 1
fi

# ================================================================
# Temp directory for pgbench scripts
# ================================================================
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# ================================================================
# Schema and data loading
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

-- Categories (hierarchical)
CREATE TABLE categories (
    category_id   serial PRIMARY KEY,
    parent_id     int REFERENCES categories(category_id),
    name          text NOT NULL,
    slug          text NOT NULL,
    description   text,
    is_active     boolean DEFAULT true,
    sort_order    int DEFAULT 0
);

-- Products
CREATE TABLE products (
    product_id    serial PRIMARY KEY,
    sku           text NOT NULL UNIQUE,
    name          text NOT NULL,
    description   text,
    category_id   int NOT NULL REFERENCES categories(category_id),
    brand         text,
    price         numeric(10,2) NOT NULL,
    cost          numeric(10,2),
    weight_kg     numeric(6,3),
    is_active     boolean DEFAULT true,
    created_at    timestamptz DEFAULT now(),
    updated_at    timestamptz DEFAULT now(),
    attrs         jsonb DEFAULT '{}'
);

-- Inventory
CREATE TABLE inventory (
    product_id    int PRIMARY KEY REFERENCES products(product_id),
    qty_available int NOT NULL DEFAULT 0,
    qty_reserved  int NOT NULL DEFAULT 0,
    warehouse     text DEFAULT 'main',
    last_restock  timestamptz
);

-- Customers
CREATE TABLE customers (
    customer_id   serial PRIMARY KEY,
    email         text NOT NULL UNIQUE,
    first_name    text NOT NULL,
    last_name     text NOT NULL,
    phone         text,
    address_line1 text,
    address_line2 text,
    city          text,
    state         text,
    zip           text,
    country       text DEFAULT 'US',
    created_at    timestamptz DEFAULT now(),
    tier          text DEFAULT 'standard'  -- standard, silver, gold, platinum
);

-- Orders
CREATE TABLE orders (
    order_id      serial PRIMARY KEY,
    customer_id   int NOT NULL REFERENCES customers(customer_id),
    status        text NOT NULL DEFAULT 'pending',
    order_date    timestamptz NOT NULL DEFAULT now(),
    ship_date     timestamptz,
    subtotal      numeric(12,2) NOT NULL DEFAULT 0,
    tax           numeric(10,2) NOT NULL DEFAULT 0,
    shipping      numeric(10,2) NOT NULL DEFAULT 0,
    total         numeric(12,2) NOT NULL DEFAULT 0,
    payment_method text,
    shipping_method text
);

-- Order items
CREATE TABLE order_items (
    item_id       serial PRIMARY KEY,
    order_id      int NOT NULL REFERENCES orders(order_id),
    product_id    int NOT NULL REFERENCES products(product_id),
    quantity      int NOT NULL DEFAULT 1,
    unit_price    numeric(10,2) NOT NULL,
    discount_pct  numeric(5,2) DEFAULT 0,
    line_total    numeric(12,2) NOT NULL
);

-- Reviews
CREATE TABLE reviews (
    review_id     serial PRIMARY KEY,
    product_id    int NOT NULL REFERENCES products(product_id),
    customer_id   int NOT NULL REFERENCES customers(customer_id),
    rating        smallint NOT NULL CHECK (rating BETWEEN 1 AND 5),
    title         text,
    body          text,
    helpful_votes int DEFAULT 0,
    verified      boolean DEFAULT false,
    created_at    timestamptz DEFAULT now()
);

-- ================================================================
-- Generate data
-- ================================================================

-- Categories: 20 top-level, 180 children
INSERT INTO categories (name, slug, description, sort_order)
SELECT 'Category ' || g,
       'cat-' || g,
       'Top-level category number ' || g,
       g
FROM generate_series(1, 20) g;

INSERT INTO categories (parent_id, name, slug, description, sort_order)
SELECT (g % 20) + 1,
       'Subcategory ' || g,
       'subcat-' || g,
       'Child category number ' || g,
       g
FROM generate_series(1, $N_CATEGORIES - 20) g;

-- Products
INSERT INTO products (sku, name, description, category_id, brand, price, cost, weight_kg, attrs)
SELECT 'SKU-' || lpad(g::text, 8, '0'),
       'Product ' || g || ' ' || (ARRAY['Widget','Gadget','Tool','Device','Component','Module','Kit','Pack','Set','Bundle'])[1 + g % 10],
       'Detailed description for product ' || g || '. High quality ' ||
         (ARRAY['aluminum','steel','plastic','carbon fiber','titanium'])[1 + g % 5] || ' construction.',
       1 + (g % $N_CATEGORIES),
       (ARRAY['Acme','TechCo','ProBuild','QuickShip','ValueMax','PremiumPlus','EcoLine','MegaCorp'])[1 + g % 8],
       round((10 + random() * 990)::numeric, 2),
       round((5 + random() * 400)::numeric, 2),
       round((0.1 + random() * 25)::numeric, 3),
       jsonb_build_object(
           'color', (ARRAY['red','blue','green','black','white','silver'])[1 + (g % 6)],
           'size', (ARRAY['S','M','L','XL','XXL'])[1 + (g % 5)],
           'material', (ARRAY['aluminum','steel','plastic','carbon fiber','titanium'])[1 + (g % 5)]
       )
FROM generate_series(1, $N_PRODUCTS) g;

-- Inventory
INSERT INTO inventory (product_id, qty_available, qty_reserved, last_restock)
SELECT product_id,
       (random() * 500)::int,
       (random() * 20)::int,
       now() - (random() * interval '90 days')
FROM products;

-- Customers
INSERT INTO customers (email, first_name, last_name, phone, city, state, zip, tier)
SELECT 'user' || g || '@example.com',
       (ARRAY['John','Jane','Bob','Alice','Charlie','Diana','Eve','Frank','Grace','Henry'])[1 + g % 10],
       (ARRAY['Smith','Johnson','Williams','Brown','Jones','Garcia','Miller','Davis','Wilson','Taylor'])[1 + (g/10) % 10],
       '+1-555-' || lpad((g % 10000)::text, 4, '0'),
       (ARRAY['New York','Los Angeles','Chicago','Houston','Phoenix','Philadelphia','San Antonio','San Diego','Dallas','Austin'])[1 + g % 10],
       (ARRAY['NY','CA','IL','TX','AZ','PA','TX','CA','TX','TX'])[1 + g % 10],
       lpad((10000 + g % 90000)::text, 5, '0'),
       (ARRAY['standard','standard','standard','silver','silver','gold','platinum'])[1 + g % 7]
FROM generate_series(1, $N_CUSTOMERS) g;

-- Orders (spread over 2 years)
INSERT INTO orders (customer_id, status, order_date, subtotal, tax, shipping, total, payment_method, shipping_method)
SELECT 1 + (g % $N_CUSTOMERS),
       (ARRAY['completed','completed','completed','completed','shipped','processing','pending','cancelled'])[1 + g % 8],
       now() - (random() * interval '730 days'),
       0, 0, 0, 0,  -- will be updated
       (ARRAY['credit_card','debit_card','paypal','apple_pay','bank_transfer'])[1 + g % 5],
       (ARRAY['standard','express','overnight','pickup'])[1 + g % 4]
FROM generate_series(1, $N_ORDERS) g;

-- Order items (1-5 items per order)
INSERT INTO order_items (order_id, product_id, quantity, unit_price, discount_pct, line_total)
SELECT o.order_id,
       1 + ((o.order_id * 7 + item_num) % $N_PRODUCTS),
       1 + (o.order_id + item_num) % 5,
       p.price,
       CASE WHEN random() < 0.2 THEN round((random() * 20)::numeric, 2) ELSE 0 END,
       round(p.price * (1 + (o.order_id + item_num) % 5) * (1 - CASE WHEN random() < 0.2 THEN round((random() * 0.2)::numeric, 4) ELSE 0 END), 2)
FROM orders o
CROSS JOIN generate_series(1, 1 + (o.order_id % 5)) AS item_num
JOIN products p ON p.product_id = 1 + ((o.order_id * 7 + item_num) % $N_PRODUCTS);

-- Update order totals
UPDATE orders SET
    subtotal = oi.s,
    tax = round(oi.s * 0.08, 2),
    shipping = CASE WHEN oi.s > 100 THEN 0 ELSE 9.99 END,
    total = oi.s + round(oi.s * 0.08, 2) + CASE WHEN oi.s > 100 THEN 0 ELSE 9.99 END
FROM (
    SELECT order_id, sum(line_total) AS s
    FROM order_items
    GROUP BY order_id
) oi
WHERE orders.order_id = oi.order_id;

-- Set ship_date for shipped/completed orders
UPDATE orders SET ship_date = order_date + interval '2 days' + random() * interval '5 days'
WHERE status IN ('shipped', 'completed');

-- Reviews
INSERT INTO reviews (product_id, customer_id, rating, title, body, helpful_votes, verified, created_at)
SELECT 1 + (g % $N_PRODUCTS),
       1 + (g % $N_CUSTOMERS),
       1 + (g % 5),
       (ARRAY['Great product!','Not bad','Excellent quality','Disappointing','Average','Love it!','Terrible','Good value','Amazing!','Meh'])[1 + g % 10],
       'Review body text for product. ' ||
         CASE WHEN g % 5 = 0 THEN 'Really exceeded my expectations. Would recommend to anyone.'
              WHEN g % 5 = 1 THEN 'Decent product for the price. Could be better.'
              WHEN g % 5 = 2 THEN 'Perfect in every way. Five stars!'
              WHEN g % 5 = 3 THEN 'Did not meet expectations. Returned it.'
              ELSE 'Works as described. Nothing special but gets the job done.'
         END,
       (random() * 50)::int,
       random() < 0.7,
       now() - (random() * interval '365 days')
FROM generate_series(1, $N_REVIEWS) g;

-- ================================================================
-- Indexes (realistic for e-commerce read workload)
-- ================================================================
CREATE INDEX idx_products_category ON products(category_id) WHERE is_active;
CREATE INDEX idx_products_brand ON products(brand);
CREATE INDEX idx_products_price ON products(price);
CREATE INDEX idx_products_attrs ON products USING gin(attrs);
CREATE INDEX idx_products_created ON products(created_at DESC);
CREATE INDEX idx_orders_customer ON orders(customer_id);
CREATE INDEX idx_orders_date ON orders(order_date DESC);
CREATE INDEX idx_orders_status ON orders(status);
CREATE INDEX idx_order_items_order ON order_items(order_id);
CREATE INDEX idx_order_items_product ON order_items(product_id);
CREATE INDEX idx_reviews_product ON reviews(product_id);
CREATE INDEX idx_reviews_rating ON reviews(product_id, rating);
CREATE INDEX idx_reviews_created ON reviews(created_at DESC);
CREATE INDEX idx_inventory_qty ON inventory(qty_available) WHERE qty_available > 0;
CREATE INDEX idx_customers_email ON customers(email);
CREATE INDEX idx_customers_tier ON customers(tier);
CREATE INDEX idx_categories_parent ON categories(parent_id);

ANALYZE;

SQL

    local total_rows
    total_rows=$("$PSQL" -p "$PGPORT" -d "$PGDB" -t -A -c "
        SELECT 'products=' || (SELECT count(*) FROM products) ||
               ' customers=' || (SELECT count(*) FROM customers) ||
               ' orders=' || (SELECT count(*) FROM orders) ||
               ' items=' || (SELECT count(*) FROM order_items) ||
               ' reviews=' || (SELECT count(*) FROM reviews);")
    echo "  Data loaded: $total_rows"
}

# ================================================================
# pgbench query scripts — realistic e-commerce read patterns
# ================================================================
write_pgbench_scripts() {
    local N_PRODUCTS=$((5000 * SCALE))
    local N_CUSTOMERS=$((10000 * SCALE))
    local N_ORDERS=$((100000 * SCALE))
    local N_CATEGORIES=200

    # --- Q1: Product detail page (30% of traffic) ---
    # Customer views a product: fetch product + inventory + avg rating + review count
    cat > "$TMPDIR/q_product_detail.sql" <<EOSQL
\set pid random(1, $N_PRODUCTS)
SELECT p.product_id, p.name, p.description, p.brand, p.price, p.weight_kg,
       p.attrs, c.name AS category_name,
       i.qty_available,
       COALESCE(rs.avg_rating, 0) AS avg_rating,
       COALESCE(rs.review_count, 0) AS review_count
FROM products p
JOIN categories c ON c.category_id = p.category_id
LEFT JOIN inventory i ON i.product_id = p.product_id
LEFT JOIN LATERAL (
    SELECT round(avg(rating)::numeric, 1) AS avg_rating, count(*) AS review_count
    FROM reviews r WHERE r.product_id = p.product_id
) rs ON true
WHERE p.product_id = :pid;
EOSQL

    # --- Q2: Category listing with aggregates (20% of traffic) ---
    # Browse category page: products sorted by popularity, with price range
    cat > "$TMPDIR/q_category_browse.sql" <<EOSQL
\set cid random(1, $N_CATEGORIES)
SELECT p.product_id, p.name, p.brand, p.price,
       COALESCE(rs.avg_rating, 0) AS avg_rating,
       COALESCE(rs.review_count, 0) AS review_count,
       i.qty_available > 0 AS in_stock
FROM products p
LEFT JOIN LATERAL (
    SELECT round(avg(rating)::numeric, 1) AS avg_rating, count(*) AS review_count
    FROM reviews r WHERE r.product_id = p.product_id
) rs ON true
LEFT JOIN inventory i ON i.product_id = p.product_id
WHERE p.category_id = :cid AND p.is_active
ORDER BY rs.review_count DESC NULLS LAST, p.price ASC
LIMIT 20;
EOSQL

    # --- Q3: Search / filter (15% of traffic) ---
    # Price range + brand filter with pagination
    cat > "$TMPDIR/q_search_filter.sql" <<EOSQL
\set min_price random(10, 200)
\set max_price random(300, 1000)
\set brand_idx random(0, 7)
\set page_offset random(0, 5)
SELECT p.product_id, p.name, p.brand, p.price,
       c.name AS category_name,
       i.qty_available > 0 AS in_stock
FROM products p
JOIN categories c ON c.category_id = p.category_id
LEFT JOIN inventory i ON i.product_id = p.product_id
WHERE p.is_active
  AND p.price BETWEEN :min_price AND :max_price
  AND p.brand = (ARRAY['Acme','TechCo','ProBuild','QuickShip','ValueMax','PremiumPlus','EcoLine','MegaCorp'])[1 + :brand_idx]
ORDER BY p.price ASC
LIMIT 20 OFFSET :page_offset * 20;
EOSQL

    # --- Q4: Customer order history (15% of traffic) ---
    # View past orders with items and totals
    cat > "$TMPDIR/q_order_history.sql" <<EOSQL
\set cust_id random(1, $N_CUSTOMERS)
SELECT o.order_id, o.status, o.order_date, o.total,
       o.payment_method, o.shipping_method,
       count(oi.item_id) AS item_count,
       sum(oi.quantity) AS total_items
FROM orders o
JOIN order_items oi ON oi.order_id = o.order_id
WHERE o.customer_id = :cust_id
GROUP BY o.order_id, o.status, o.order_date, o.total,
         o.payment_method, o.shipping_method
ORDER BY o.order_date DESC
LIMIT 10;
EOSQL

    # --- Q5: Order detail (10% of traffic) ---
    # Full order with line items, product names, and computations
    cat > "$TMPDIR/q_order_detail.sql" <<EOSQL
\set oid random(1, $N_ORDERS)
SELECT o.order_id, o.status, o.order_date, o.ship_date,
       o.subtotal, o.tax, o.shipping, o.total,
       o.payment_method, o.shipping_method,
       cust.first_name || ' ' || cust.last_name AS customer_name,
       cust.email,
       oi.quantity, oi.unit_price, oi.discount_pct, oi.line_total,
       p.name AS product_name, p.sku
FROM orders o
JOIN customers cust ON cust.customer_id = o.customer_id
JOIN order_items oi ON oi.order_id = o.order_id
JOIN products p ON p.product_id = oi.product_id
WHERE o.order_id = :oid;
EOSQL

    # --- Q6: Product reviews page (5% of traffic) ---
    # Reviews for a product with rating distribution
    cat > "$TMPDIR/q_reviews.sql" <<EOSQL
\set pid random(1, $N_PRODUCTS)
SELECT r.rating, r.title, r.body, r.helpful_votes, r.verified,
       r.created_at,
       c.first_name || ' ' || substr(c.last_name, 1, 1) || '.' AS reviewer
FROM reviews r
JOIN customers c ON c.customer_id = r.customer_id
WHERE r.product_id = :pid
ORDER BY r.helpful_votes DESC, r.created_at DESC
LIMIT 10;
EOSQL

    # --- Q7: Dashboard / analytics (3% of traffic) ---
    # Sales summary — heavier aggregation query
    cat > "$TMPDIR/q_sales_dashboard.sql" <<EOSQL
\set days_back random(7, 90)
SELECT date_trunc('day', o.order_date) AS day,
       count(DISTINCT o.order_id) AS orders,
       count(DISTINCT o.customer_id) AS unique_customers,
       sum(o.total) AS revenue,
       avg(o.total) AS avg_order_value,
       sum(oi.quantity) AS items_sold
FROM orders o
JOIN order_items oi ON oi.order_id = o.order_id
WHERE o.order_date >= now() - make_interval(days => :days_back)
  AND o.status IN ('completed', 'shipped')
GROUP BY date_trunc('day', o.order_date)
ORDER BY day DESC;
EOSQL

    # --- Q8: Top products report (2% of traffic) ---
    # Heaviest query — multi-join aggregation
    cat > "$TMPDIR/q_top_products.sql" <<EOSQL
\set days_back random(30, 180)
SELECT p.product_id, p.name, p.brand, p.price,
       c.name AS category_name,
       count(DISTINCT oi.order_id) AS order_count,
       sum(oi.quantity) AS units_sold,
       sum(oi.line_total) AS revenue,
       COALESCE(rv.avg_rating, 0) AS avg_rating
FROM order_items oi
JOIN orders o ON o.order_id = oi.order_id
JOIN products p ON p.product_id = oi.product_id
JOIN categories c ON c.category_id = p.category_id
LEFT JOIN LATERAL (
    SELECT round(avg(rating)::numeric, 1) AS avg_rating
    FROM reviews r WHERE r.product_id = p.product_id
) rv ON true
WHERE o.order_date >= now() - make_interval(days => :days_back)
  AND o.status IN ('completed', 'shipped')
GROUP BY p.product_id, p.name, p.brand, p.price, c.name, rv.avg_rating
ORDER BY revenue DESC
LIMIT 20;
EOSQL
}

# ================================================================
# Run pgbench with given JIT settings
# ================================================================
run_bench() {
    local label="$1"
    local jit_setting="$2"
    local backend_setting="$3"

    echo ""
    echo "--- Running: $label (${DURATION}s, ${CLIENTS} clients) ---"

    # Apply settings via a setup script
    cat > "$TMPDIR/setup.sql" <<EOSQL
$jit_setting
$backend_setting
EOSQL

    # Warmup
    if [ "$WARMUP" -gt 0 ]; then
        "$PGBENCH" -p "$PGPORT" -d "$PGDB" \
            -f "$TMPDIR/q_product_detail.sql@30" \
            -f "$TMPDIR/q_category_browse.sql@20" \
            -f "$TMPDIR/q_search_filter.sql@15" \
            -f "$TMPDIR/q_order_history.sql@15" \
            -f "$TMPDIR/q_order_detail.sql@10" \
            -f "$TMPDIR/q_reviews.sql@5" \
            -f "$TMPDIR/q_sales_dashboard.sql@3" \
            -f "$TMPDIR/q_top_products.sql@2" \
            -c "$CLIENTS" -j "$THREADS" -T "$WARMUP" \
            --no-vacuum -n \
            > /dev/null 2>&1 || true
    fi

    # Actual run
    local output
    output=$("$PGBENCH" -p "$PGPORT" -d "$PGDB" \
        -f "$TMPDIR/q_product_detail.sql@30" \
        -f "$TMPDIR/q_category_browse.sql@20" \
        -f "$TMPDIR/q_search_filter.sql@15" \
        -f "$TMPDIR/q_order_history.sql@15" \
        -f "$TMPDIR/q_order_detail.sql@10" \
        -f "$TMPDIR/q_reviews.sql@5" \
        -f "$TMPDIR/q_sales_dashboard.sql@3" \
        -f "$TMPDIR/q_top_products.sql@2" \
        -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
        --no-vacuum -n -P 5 \
        2>&1)

    # Show progress lines
    echo "$output" | grep "^progress:" || true

    local tps
    tps=$(echo "$output" | grep "^tps" | head -1 | awk '{print $3}')
    local lat
    lat=$(echo "$output" | grep "^latency average" | head -1 | awk '{print $4}')
    local ntx
    ntx=$(echo "$output" | grep "^number of transactions actually processed" | head -1 | awk '{print $NF}')

    echo "  TPS: $tps  |  Avg latency: ${lat}ms  |  Total txns: $ntx"

    # Store result
    RESULTS+=("$label|$tps|$lat|$ntx")
}

# ================================================================
# Apply JIT settings per backend
# ================================================================
apply_jit_settings() {
    local backend="$1"

    case "$backend" in
        nojit)
            # Set jit=off globally for the test
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "ALTER DATABASE $PGDB SET jit = off;"
            ;;
        sljit|asmjit|mir)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = on;
                ALTER DATABASE $PGDB SET jit_above_cost = 0;
            "
            "$PSQL" -p "$PGPORT" -d postgres -q -c \
                "ALTER SYSTEM SET jit_provider = 'pg_jitter_$backend';"
            "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 1
            ;;
        auto)
            "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
                ALTER DATABASE $PGDB SET jit = on;
                ALTER DATABASE $PGDB SET jit_above_cost = 0;
            "
            "$PSQL" -p "$PGPORT" -d postgres -q -c \
                "ALTER SYSTEM SET jit_provider = 'pg_jitter';"
            "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1
            sleep 1
            ;;
    esac
}

# ================================================================
# Save and restore original provider
# ================================================================
ORIG_PROVIDER="$("$PSQL" -p "$PGPORT" -d postgres -t -A -c "SHOW jit_provider;" 2>/dev/null)"

restore_provider() {
    if [ -n "$ORIG_PROVIDER" ]; then
        "$PSQL" -p "$PGPORT" -d postgres -q -c \
            "ALTER SYSTEM SET jit_provider = '$ORIG_PROVIDER';" 2>/dev/null || true
        "$PGCTL" -D "$PGDATA" restart -l "$PGDATA/logfile" -w >/dev/null 2>&1 || true
    fi
    # Reset database-level JIT settings
    "$PSQL" -p "$PGPORT" -d "$PGDB" -q -c "
        ALTER DATABASE $PGDB RESET jit;
        ALTER DATABASE $PGDB RESET jit_above_cost;
    " 2>/dev/null || true
}
trap "restore_provider; rm -rf $TMPDIR" EXIT

# ================================================================
# Main
# ================================================================
echo "============================================"
echo "  E-Commerce pgbench Workload Benchmark"
echo "============================================"
echo ""
echo "  Port:       $PGPORT"
echo "  Database:   $PGDB"
echo "  Scale:      $SCALE (~$((100000 * SCALE)) orders)"
echo "  Clients:    $CLIENTS"
echo "  Threads:    $THREADS"
echo "  Duration:   ${DURATION}s per backend"
echo "  Warmup:     ${WARMUP}s"
echo "  Backends:   $BACKENDS"
echo ""

if [ "$SKIP_LOAD" -eq 0 ]; then
    load_data
fi

write_pgbench_scripts

declare -a RESULTS

for backend in $BACKENDS; do
    apply_jit_settings "$backend"
    run_bench "$backend" "" ""
done

# ================================================================
# Summary
# ================================================================
echo ""
echo "============================================"
echo "  RESULTS SUMMARY"
echo "============================================"
printf "%-12s %10s %12s %12s\n" "Backend" "TPS" "Avg Lat(ms)" "Total Txns"
printf "%-12s %10s %12s %12s\n" "--------" "--------" "----------" "----------"

NOJIT_TPS=""
for result in "${RESULTS[@]}"; do
    IFS='|' read -r label tps lat ntx <<< "$result"

    if [ "$label" = "nojit" ]; then
        NOJIT_TPS="$tps"
        printf "%-12s %10s %12s %12s  (baseline)\n" "$label" "$tps" "$lat" "$ntx"
    elif [ -n "$NOJIT_TPS" ] && [ -n "$tps" ]; then
        pct=""
        pct=$(echo "scale=2; ($tps / $NOJIT_TPS - 1) * 100" | bc -l 2>/dev/null || echo "?")
        if [ "$pct" != "?" ]; then
            # Add + sign for positive
            if echo "$pct" | grep -q "^-"; then
                printf "%-12s %10s %12s %12s  (%s%%)\n" "$label" "$tps" "$lat" "$ntx" "$pct"
            else
                printf "%-12s %10s %12s %12s  (+%s%%)\n" "$label" "$tps" "$lat" "$ntx" "$pct"
            fi
        else
            printf "%-12s %10s %12s %12s\n" "$label" "$tps" "$lat" "$ntx"
        fi
    else
        printf "%-12s %10s %12s %12s\n" "$label" "$tps" "$lat" "$ntx"
    fi
done

echo "============================================"
echo ""
echo "Query mix: product_detail(30%), category_browse(20%), search_filter(15%),"
echo "           order_history(15%), order_detail(10%), reviews(5%),"
echo "           sales_dashboard(3%), top_products(2%)"
echo ""
