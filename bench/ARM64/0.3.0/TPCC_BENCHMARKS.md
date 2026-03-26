# TPC-C Benchmark Results

TPC-C OLTP workload on pg_jitter. Individual query latencies measured via EXPLAIN ANALYZE (server-side, no client overhead).

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | 18.1 |
| CPU | Apple M1 Pro |
| RAM | 32 GB |
| Warehouses | 10 |
| jit_above_cost | 200 |
| Backends | interp sljit asmjit mir auto |
| Runs per query | 7 (median, single session) |
| Date | 2026-03-24 |

## Results

| Query | No JIT | sljit | asmjit | mir | auto |
|-------|--------|------|------|------|------|
| PK_warehouse | 0.002 ms | 0.002 ms (1.00x) | 0.002 ms (1.00x) | 0.002 ms (1.00x) | 0.002 ms (1.00x) |
| PK_district | 0.005 ms | 0.005 ms (1.00x) | 0.005 ms (1.00x) | 0.005 ms (1.00x) | 0.005 ms (1.00x) |
| PK_customer | 0.003 ms | 0.003 ms (1.00x) | 0.002 ms (1.50x) | 0.002 ms (1.50x) | 0.002 ms (1.50x) |
| PK_item | 0.002 ms | 0.002 ms (1.00x) | 0.002 ms (1.00x) | 0.002 ms (1.00x) | 0.002 ms (1.00x) |
| UPD_warehouse | 0.004 ms | 0.005 ms (0.80x) | 0.005 ms (0.80x) | 0.005 ms (0.80x) | 0.005 ms (0.80x) |
| UPD_customer | 0.006 ms | 0.006 ms (1.00x) | 0.007 ms (0.86x) | 0.006 ms (1.00x) | 0.006 ms (1.00x) |
| SCAN_cust_name | 1.004 ms | 0.959 ms (1.05x) | 1.119 ms (0.90x) | 1.555 ms (0.65x) | 1.033 ms (0.97x) |
| SCAN_orders_range | 0.236 ms | 0.239 ms (0.99x) | 0.319 ms (0.74x) | 0.771 ms (0.31x) | 0.243 ms (0.97x) |
| SCAN_orderlines | 0.088 ms | 0.090 ms (0.98x) | 0.159 ms (0.55x) | 0.621 ms (0.14x) | 0.090 ms (0.98x) |
| AGG_district_rev | 40.594 ms | 37.710 ms (1.08x) | 38.170 ms (1.06x) | 43.220 ms (0.94x) | 38.333 ms (1.06x) |
| AGG_cust_stats | 1.487 ms | 1.479 ms (1.01x) | 1.769 ms (0.84x) | 2.608 ms (0.57x) | 1.686 ms (0.88x) |
| JOIN_order_detail | 0.641 ms | 0.658 ms (0.97x) | 1.220 ms (0.53x) | 3.453 ms (0.19x) | 0.915 ms (0.70x) |
| SL_stock_check | 0.007 ms | 0.048 ms (0.15x) | 0.372 ms (0.02x) | 1.614 ms (0.00x) | 0.157 ms (0.04x) |
| EXPR_order_prio | 6.337 ms | 5.829 ms (1.09x) | 5.917 ms (1.07x) | 7.434 ms (0.85x) | 5.951 ms (1.06x) |
| EXPR_cust_tier | 10.275 ms | 9.305 ms (1.10x) | 10.110 ms (1.02x) | 11.776 ms (0.87x) | 9.888 ms (1.04x) |

## Summary

| Backend | Total (ms) | Speedup | Geomean (all) | Geomean (>1ms) |
|---------|-----------|---------|---------------|----------------|
| sljit | 56 | 1.08x | 0.881x | 1.063x (5/5) |
| asmjit | 59 | 1.03x | 0.688x | 0.973x (3/5) |
| mir | 73 | 0.83x | 0.466x | 0.762x (0/5) |
| auto | 58 | 1.04x | 0.801x | 1.001x (3/5) |

Point lookups (PK_*) correctly skipped by jit_above_cost=200. Queries >1ms show the meaningful JIT impact on OLTP.

## Appendix: TPC-C Queries

### Point Lookups (PK index, JIT skipped at threshold=200)

```sql
-- PK_warehouse (cost=8.2, skipped)
SELECT w_tax FROM warehouse WHERE w_id = :w_id;

-- PK_district (cost=8.3, skipped)
SELECT d_tax, d_next_o_id FROM district WHERE d_w_id = :w_id AND d_id = :d_id;

-- PK_customer (cost=8.4, skipped)
SELECT c_discount, c_last, c_credit FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :c_id;

-- PK_item (cost=8.3, skipped)
SELECT i_price, i_name, i_data FROM item WHERE i_id = :i_id;
```

### Single-Row Updates

```sql
-- UPD_warehouse
UPDATE warehouse SET w_ytd = w_ytd + 1234.56 WHERE w_id = :w_id;

-- UPD_customer
UPDATE customer SET c_balance = c_balance - 1234.56,
  c_ytd_payment = c_ytd_payment + 1234.56, c_payment_cnt = c_payment_cnt + 1
WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_id = :c_id;
```

### Multi-Row Scans & Aggregations

```sql
-- SCAN_cust_name
SELECT c_id, c_first, c_last, c_balance FROM customer
WHERE c_w_id = :w_id AND c_d_id = :d_id AND c_last LIKE 'OUGHT%' ORDER BY c_first;

-- SCAN_orders_range
SELECT o_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt FROM oorder
WHERE o_w_id = :w_id AND o_d_id = :d_id AND o_id BETWEEN 1000 AND 2000;

-- SCAN_orderlines
SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line
WHERE ol_w_id = :w_id AND ol_d_id = :d_id AND ol_o_id BETWEEN 1000 AND 1100;

-- AGG_district_rev
SELECT o.o_d_id, count(*), sum(ol_amount), avg(ol_amount) FROM order_line ol
JOIN oorder o ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
WHERE o.o_w_id = :w_id AND o.o_id BETWEEN 1 AND 1000 GROUP BY o.o_d_id;

-- AGG_cust_stats
SELECT c_credit, count(*), sum(c_balance), avg(c_discount), min(c_ytd_payment), max(c_ytd_payment)
FROM customer WHERE c_w_id = :w_id AND c_d_id = :d_id GROUP BY c_credit;

-- JOIN_order_detail
SELECT o.o_id, c.c_last, c.c_credit, o.o_entry_d, o.o_ol_cnt, sum(ol.ol_amount) AS total
FROM oorder o JOIN customer c ON c.c_w_id = o.o_w_id AND c.c_d_id = o.o_d_id AND c.c_id = o.o_c_id
JOIN order_line ol ON ol.ol_w_id = o.o_w_id AND ol.ol_d_id = o.o_d_id AND ol.ol_o_id = o.o_id
WHERE o.o_w_id = :w_id AND o.o_d_id = :d_id AND o.o_id BETWEEN 2500 AND 2600
GROUP BY o.o_id, c.c_last, c.c_credit, o.o_entry_d, o.o_ol_cnt ORDER BY total DESC;
```

### Stock Level (TPC-C's most complex query)

```sql
SELECT count(DISTINCT s_i_id) FROM order_line JOIN stock ON s_i_id = ol_i_id AND s_w_id = ol_w_id
WHERE ol_w_id = 1 AND ol_d_id = 1
  AND ol_o_id BETWEEN (SELECT d_next_o_id - 20 FROM district WHERE d_w_id = 1 AND d_id = 1)
      AND (SELECT d_next_o_id - 1 FROM district WHERE d_w_id = 1 AND d_id = 1)
  AND s_quantity < :threshold;
```

### Expression-Heavy Analytics on TPC-C Data

```sql
-- EXPR_order_prio
SELECT CASE WHEN o_carrier_id IS NOT NULL THEN 'delivered'
            WHEN o_carrier_id IS NULL AND o_entry_d > now() - interval '30 day' THEN 'pending'
            ELSE 'old_pending' END AS status,
       count(*), avg(o_ol_cnt) FROM oorder WHERE o_w_id = :w_id GROUP BY 1;

-- EXPR_cust_tier
SELECT CASE WHEN c_balance > 1000 THEN 'high' WHEN c_balance > 0 THEN 'medium'
            WHEN c_balance > -100 THEN 'low' ELSE 'negative' END AS tier,
       c_credit, count(*), sum(c_balance), avg(c_discount)
FROM customer WHERE c_w_id = :w_id GROUP BY 1, 2 ORDER BY 4 DESC;
```
