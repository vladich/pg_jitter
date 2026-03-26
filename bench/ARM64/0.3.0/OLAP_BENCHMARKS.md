# OLAP Sustained Benchmark Results

Sustained analytical workload on a star-schema data warehouse with heavy aggregations, window functions, complex expressions, and wide table deform.

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | 18.1 |
| CPU | Apple M1 Pro |
| RAM | 32 GB |
| Duration | 60s per backend |
| Clients | 4 |
| Scale | 10 (10M fact rows) |
| Date | 2026-03-24 |

## Query Mix

| Query | Weight | Description |
|-------|--------|-------------|
| quarterly_revenue | 20% | Full scan + hash join + 7 aggregates |
| top_products | 15% | Window functions (rank, ntile) + hash join |
| customer_segments | 15% | CASE expressions + aggregates + hash joins |
| yoy_comparison | 15% | Self-join + heavy expressions |
| rolling_averages | 10% | Window functions over large result sets |
| wide_table_deform | 10% | All 20+ product columns + expressions |
| store_scoring | 10% | Heavy CASE + arithmetic expressions |
| pareto_analysis | 5% | CTE + window + complex math |

## Results

Backend         TPS    Lat(ms)       Txns   CPU avg CPU peak   RSS avg RSS peak
-------      ------   --------   --------   ------- --------   ------- --------
nojit      2.104910   1882.127        130    393.9%   404.9%    6812MB   7617MB  (baseline)
sljit      2.997207   1330.129        181    399.0%   403.2%    6630MB   7179MB  (+42.3900%)
asmjit     2.780330   1431.764        169    393.0%   402.7%    6658MB   6888MB  (+32.0800%)
mir        2.605753   1525.368        159    393.0%   400.0%    6562MB   6807MB  (+23.7900%)
auto       2.646381   1504.271        162    398.1%   400.0%    6609MB   6773MB  (+25.7200%)

Query mix: quarterly_revenue(20%) top_products(15%) customer_segments(15%)
           yoy_comparison(15%) rolling_averages(10%) wide_table_deform(10%)
           store_scoring(10%) pareto_analysis(5%)

## Analysis

OLAP is pg_jitter's strongest workload. Heavy analytical queries with complex expressions, window functions, and aggregations over millions of rows spend 30-50% of runtime in expression evaluation — exactly what JIT accelerates. sljit delivers +42% TPS with the lowest latency. All pg_jitter backends use significantly less memory than the interpreter.

## Appendix: OLAP Query SQL

### Q1 — Quarterly Revenue (20%)
```sql
SELECT d.quarter, d.year, sum(f.line_total) AS gross_revenue,
       sum(f.profit) AS gross_profit, count(*) AS txn_count,
       count(DISTINCT f.customer_key) AS unique_customers,
       round(sum(f.profit) / NULLIF(sum(f.line_total), 0) * 100, 1) AS margin_pct
FROM fact_sales f JOIN dim_date d ON d.date_key = f.date_key
WHERE d.year = :yr GROUP BY d.quarter, d.year
ORDER BY d.quarter, gross_revenue DESC;
```

### Q2 — Top Products with Ranking (15%)
```sql
SELECT * FROM (
  SELECT p.product_key, p.name, p.brand, p.subcategory,
         sum(f.line_total) AS revenue, sum(f.profit) AS profit,
         count(DISTINCT f.customer_key) AS unique_buyers,
         rank() OVER (ORDER BY sum(f.line_total) DESC) AS revenue_rank,
         ntile(10) OVER (ORDER BY sum(f.line_total) DESC) AS revenue_decile
  FROM fact_sales f JOIN dim_product p ON p.product_key = f.product_key
  JOIN dim_date d ON d.date_key = f.date_key
  WHERE d.year = :yr AND p.category = :cat
  GROUP BY p.product_key, p.name, p.brand, p.subcategory
) ranked WHERE revenue_rank <= 50;
```

### Q3 — Customer Segmentation (15%)
```sql
SELECT c.segment, c.state, count(DISTINCT c.customer_key) AS customers,
       count(*) AS transactions, sum(f.line_total) AS total_spend,
       round(avg(f.line_total), 2) AS avg_transaction,
       count(CASE WHEN f.discount_pct > 0 THEN 1 END) AS discounted_txns,
       CASE WHEN sum(f.line_total)/NULLIF(count(DISTINCT c.customer_key),0) > 5000 THEN 'High Value'
            WHEN sum(f.line_total)/NULLIF(count(DISTINCT c.customer_key),0) > 1000 THEN 'Medium Value'
            ELSE 'Low Value' END AS value_tier
FROM fact_sales f JOIN dim_customer c ON c.customer_key = f.customer_key
JOIN dim_date d ON d.date_key = f.date_key WHERE d.year = :yr
GROUP BY c.segment, c.state ORDER BY total_spend DESC;
```

### Q4 — Year-over-Year Comparison (15%)
```sql
SELECT cur.region, cur.month, cur.revenue, prev.revenue,
       cur.revenue - prev.revenue AS revenue_change,
       round((cur.revenue / NULLIF(prev.revenue, 0) - 1) * 100, 1) AS yoy_pct
FROM (SELECT s.region, d.month, sum(f.line_total) AS revenue
      FROM fact_sales f JOIN dim_date d ON d.date_key = f.date_key
      JOIN dim_store s ON s.store_key = f.store_key
      WHERE d.year = :yr GROUP BY s.region, d.month) cur
JOIN (SELECT s.region, d.month, sum(f.line_total) AS revenue
      FROM fact_sales f JOIN dim_date d ON d.date_key = f.date_key
      JOIN dim_store s ON s.store_key = f.store_key
      WHERE d.year = :yr - 1 GROUP BY s.region, d.month) prev
ON cur.region = prev.region AND cur.month = prev.month
ORDER BY cur.region, cur.month;
```

### Q5 — Monthly Rolling Averages (10%)
```sql
SELECT region, full_date, daily_revenue,
       round(avg(daily_revenue) OVER w, 2) AS rolling_7d_avg,
       round(avg(daily_revenue) OVER w2, 2) AS rolling_30d_avg,
       round(sum(daily_revenue) OVER (PARTITION BY region ORDER BY full_date
           ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW), 2) AS cumulative
FROM (SELECT s.region, d.full_date, sum(f.line_total) AS daily_revenue
      FROM fact_sales f JOIN dim_date d ON d.date_key = f.date_key
      JOIN dim_store s ON s.store_key = f.store_key WHERE d.year = :yr
      GROUP BY s.region, d.full_date) daily
WINDOW w AS (PARTITION BY region ORDER BY full_date ROWS 6 PRECEDING),
       w2 AS (PARTITION BY region ORDER BY full_date ROWS 29 PRECEDING)
ORDER BY region, full_date;
```

### Q6 — Wide Table Deform (10%)
```sql
SELECT p.product_key, p.sku, p.name, p.category, p.subcategory, p.brand,
       p.supplier, p.unit_cost, p.list_price, p.weight_kg, p.color, p.material,
       p.country_of_origin, p.warranty_months, p.margin_pct,
       sum(f.quantity) AS units, sum(f.line_total) AS revenue,
       round(avg(f.discount_pct), 2) AS avg_discount,
       CASE WHEN p.list_price < 50 THEN 'Budget' WHEN p.list_price < 150 THEN 'Mid-Range'
            WHEN p.list_price < 350 THEN 'Premium' ELSE 'Luxury' END AS price_tier
FROM fact_sales f JOIN dim_product p ON p.product_key = f.product_key
JOIN dim_date d ON d.date_key = f.date_key WHERE d.year = :yr
GROUP BY p.product_key, p.sku, p.name, p.category, p.subcategory, p.brand,
         p.supplier, p.unit_cost, p.list_price, p.weight_kg, p.color, p.material,
         p.country_of_origin, p.warranty_months, p.margin_pct
ORDER BY revenue DESC;
```

### Q7 — Store Performance Scoring (10%)
```sql
SELECT s.store_key, s.store_name, s.region, count(*) AS transactions,
       sum(f.line_total) AS revenue, sum(f.profit) AS profit,
       round(avg(f.line_total), 2) AS avg_basket,
       count(DISTINCT f.customer_key) AS unique_customers,
       round(sum(f.profit)/NULLIF(sum(f.line_total),0)*100, 1) AS margin_pct,
       round((sum(f.line_total)/NULLIF(max(s.sqft),0))*0.3 +
             (count(DISTINCT f.customer_key)::numeric/NULLIF(max(s.employees),0))*20 +
             (sum(f.profit)/NULLIF(sum(f.line_total),0))*100*0.3)::numeric, 1) AS perf_score
FROM fact_sales f JOIN dim_store s ON s.store_key = f.store_key
JOIN dim_date d ON d.date_key = f.date_key WHERE d.year = :yr
GROUP BY s.store_key, s.store_name, s.region ORDER BY revenue DESC;
```

### Q8 — Pareto/80-20 Analysis (5%)
```sql
WITH customer_revenue AS (
  SELECT c.customer_key, c.segment, sum(f.line_total) AS total_spend, count(*) AS num_txns
  FROM fact_sales f JOIN dim_customer c ON c.customer_key = f.customer_key
  JOIN dim_date d ON d.date_key = f.date_key WHERE d.year = :yr
  GROUP BY c.customer_key, c.segment
), ranked AS (
  SELECT *, row_number() OVER (ORDER BY total_spend DESC) AS spend_rank,
         sum(total_spend) OVER (ORDER BY total_spend DESC
             ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS cumul_spend,
         sum(total_spend) OVER () AS grand_total,
         ntile(100) OVER (ORDER BY total_spend DESC) AS percentile
  FROM customer_revenue
)
SELECT percentile, count(*) AS num_customers, round(sum(total_spend), 2),
       round(min(cumul_spend)/max(grand_total)*100, 1) AS cumul_pct
FROM ranked GROUP BY percentile ORDER BY percentile;
```

Full SQL available in `tests/bench_olap_sustained.sh`.
