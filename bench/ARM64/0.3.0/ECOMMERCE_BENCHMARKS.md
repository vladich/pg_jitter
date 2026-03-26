# E-commerce Sustained Benchmark Results

Sustained OLTP workload simulating an e-commerce platform with mixed read/write transactions.

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | 18.1 |
| CPU | Apple M1 Pro |
| RAM | 32 GB |
| Duration | 60s per backend |
| Clients | 8 |
| Scale | 10 |
| Date | 2026-03-24 |

## Query Mix

| Transaction | Weight |
|-------------|--------|
| product_detail | 30% |
| category_browse | 20% |
| search_filter | 15% |
| order_history | 15% |
| order_detail | 10% |
| reviews | 5% |
| sales_dashboard | 3% |
| top_products | 2% |

## Results

Backend         TPS    Lat(ms)       Txns   CPU avg CPU peak   RSS avg RSS peak
-------      ------   --------   --------   ------- --------   ------- --------
nojit      317.603705     25.032      19253    788.5%   806.7%   10139MB  10597MB  (baseline)
sljit      361.139140     22.009      21906    786.3%   800.0%    5529MB   5761MB  (+13.7000%)
asmjit     344.211537     23.071      20941    787.9%   799.2%    5616MB   5810MB  (+8.3700%)
mir        324.111718     24.546      19632    786.5%   799.1%    5528MB   5760MB  (+2.0400%)
auto       355.384729     22.425      21467    781.0%   800.0%    5555MB   5786MB  (+11.8900%)

Query mix: product_detail(30%) category_browse(20%) search_filter(15%)
           order_history(15%) order_detail(10%) reviews(5%)
           sales_dashboard(3%) top_products(2%)

## Analysis

sljit delivers the best OLTP throughput with lowest latency and lowest memory usage. The e-commerce workload has short, index-heavy transactions where JIT compilation overhead matters — sljit's ~50μs compilation time is amortized across many queries per connection.

## Appendix: Transaction SQL

### product_detail (30%)
```sql
SELECT p.*, c.name AS category_name,
       avg(r.rating) AS avg_rating, count(r.review_id) AS review_count
FROM products p JOIN categories c ON p.category_id = c.category_id
LEFT JOIN reviews r ON r.product_id = p.product_id
WHERE p.product_id = :pid
GROUP BY p.product_id, c.name;
```

### category_browse (20%)
```sql
SELECT p.product_id, p.name, p.price, p.description,
       avg(r.rating) AS avg_rating, count(r.review_id) AS num_reviews
FROM products p LEFT JOIN reviews r ON r.product_id = p.product_id
WHERE p.category_id = :cid AND p.price BETWEEN :lo AND :hi
GROUP BY p.product_id ORDER BY p.price LIMIT 20 OFFSET :off;
```

### search_filter (15%)
```sql
SELECT p.product_id, p.name, p.price, c.name AS category
FROM products p JOIN categories c ON p.category_id = c.category_id
WHERE p.name ILIKE '%' || :term || '%' OR p.description ILIKE '%' || :term || '%'
ORDER BY p.price LIMIT 20;
```

### order_history (15%)
```sql
SELECT o.order_id, o.order_date, o.total_amount, o.status,
       count(oi.order_item_id) AS num_items
FROM orders o JOIN order_items oi ON o.order_id = oi.order_id
WHERE o.customer_id = :cust_id
GROUP BY o.order_id ORDER BY o.order_date DESC LIMIT 10;
```

### order_detail (10%)
```sql
SELECT o.*, oi.quantity, oi.unit_price, p.name AS product_name
FROM orders o JOIN order_items oi ON o.order_id = oi.order_id
JOIN products p ON oi.product_id = p.product_id
WHERE o.order_id = :oid;
```

### reviews (5%)
```sql
SELECT r.*, c.first_name || ' ' || c.last_name AS reviewer
FROM reviews r JOIN customers c ON r.customer_id = c.customer_id
WHERE r.product_id = :pid ORDER BY r.created_at DESC LIMIT 10;
```

### sales_dashboard (3%)
```sql
SELECT date_trunc('day', o.order_date) AS day, count(*) AS orders,
       sum(o.total_amount) AS revenue, avg(o.total_amount) AS avg_order
FROM orders o WHERE o.order_date >= now() - interval '30 days'
GROUP BY 1 ORDER BY 1;
```

### top_products (2%)
```sql
SELECT p.product_id, p.name, sum(oi.quantity) AS units_sold,
       sum(oi.quantity * oi.unit_price) AS revenue
FROM order_items oi JOIN products p ON oi.product_id = p.product_id
JOIN orders o ON oi.order_id = o.order_id
WHERE o.order_date >= now() - interval '7 days'
GROUP BY p.product_id, p.name ORDER BY revenue DESC LIMIT 20;
```

Full SQL available in `tests/bench_ecommerce_sustained.sh`.
