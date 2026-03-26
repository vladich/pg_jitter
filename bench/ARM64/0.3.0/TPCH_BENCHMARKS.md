# TPC-H Benchmark Results (SF=1)

Standard TPC-H decision support benchmark comparing pg_jitter backends.

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | 18.1 |
| CPU | Apple M1 Pro |
| RAM | 32 GB |
| Scale Factor | 1 (~1GB, 6M lineitem rows) |
| Backends | interp sljit asmjit mir auto |
| Runs per query | 3 (median) |
| Parallel workers | 0 (disabled) |
| Date | 2026-03-24 |

## Results

| Query | No JIT | sljit | asmjit | mir | auto |
|-------|--------|------|------|------|------|
| Q1 | 2670.9 ms | 2420.7 ms (1.10x) | 2520.1 ms (1.06x) | 2651.5 ms (1.01x) | 2447.3 ms (1.09x) |
| Q2 | 121.1 ms | 114.5 ms (1.06x) | 117.4 ms (1.03x) | 119.7 ms (1.01x) | 115.6 ms (1.05x) |
| Q3 | 732.1 ms | 667.3 ms (1.10x) | 679.7 ms (1.08x) | 718.1 ms (1.02x) | 685.5 ms (1.07x) |
| Q4 | 131.6 ms | 133.8 ms (0.98x) | 134.1 ms (0.98x) | 136.9 ms (0.96x) | 134.3 ms (0.98x) |
| Q5 | 233.1 ms | 208.0 ms (1.12x) | 210.9 ms (1.11x) | 213.0 ms (1.09x) | 207.6 ms (1.12x) |
| Q6 | 113.1 ms | 115.7 ms (0.98x) | 115.7 ms (0.98x) | 117.9 ms (0.96x) | 119.4 ms (0.95x) |
| Q7 | 508.8 ms | 478.7 ms (1.06x) | 492.8 ms (1.03x) | 486.0 ms (1.05x) | 488.9 ms (1.04x) |
| Q8 | 197.9 ms | 194.4 ms (1.02x) | 197.6 ms (1.00x) | 198.6 ms (1.00x) | 194.4 ms (1.02x) |
| Q9 | 1857.8 ms | 1800.0 ms (1.03x) | 1850.6 ms (1.00x) | 1864.8 ms (1.00x) | 1815.7 ms (1.02x) |
| Q10 | 778.2 ms | 731.2 ms (1.06x) | 723.8 ms (1.08x) | 753.9 ms (1.03x) | 707.6 ms (1.10x) |
| Q11 | 53.4 ms | 55.3 ms (0.97x) | 55.7 ms (0.96x) | 57.4 ms (0.93x) | 55.6 ms (0.96x) |
| Q12 | 1042.5 ms | 917.2 ms (1.14x) | 919.6 ms (1.13x) | 985.6 ms (1.06x) | 919.0 ms (1.13x) |
| Q13 | 634.7 ms | 510.4 ms (1.24x) | 556.8 ms (1.14x) | 540.9 ms (1.17x) | 497.2 ms (1.28x) |
| Q14 | 127.1 ms | 126.8 ms (1.00x) | 127.6 ms (1.00x) | 132.6 ms (0.96x) | 126.2 ms (1.01x) |
| Q15 | 166.6 ms | 165.9 ms (1.00x) | 167.9 ms (0.99x) | 171.9 ms (0.97x) | 167.0 ms (1.00x) |
| Q16 | 431.5 ms | 423.6 ms (1.02x) | 425.9 ms (1.01x) | 432.6 ms (1.00x) | 423.8 ms (1.02x) |
| Q17 | 2625.3 ms | 2601.7 ms (1.01x) | 2609.7 ms (1.01x) | 2625.6 ms (1.00x) | 2583.9 ms (1.02x) |
| Q18 | 1734.1 ms | 1677.1 ms (1.03x) | 1669.3 ms (1.04x) | 1756.4 ms (0.99x) | 1645.8 ms (1.05x) |
| Q19 | 50.8 ms | 48.2 ms (1.05x) | 49.5 ms (1.03x) | 52.6 ms (0.97x) | 47.4 ms (1.07x) |
| Q20 | 784.3 ms | 747.0 ms (1.05x) | 748.3 ms (1.05x) | 764.7 ms (1.03x) | 749.7 ms (1.05x) |
| Q21 | 801.8 ms | 724.9 ms (1.11x) | 734.1 ms (1.09x) | 769.5 ms (1.04x) | 740.6 ms (1.08x) |
| Q22 | 77.3 ms | 75.3 ms (1.03x) | 76.6 ms (1.01x) | 78.4 ms (0.99x) | 75.2 ms (1.03x) |

## Summary

| Backend | Geomean | Total speedup | Queries faster |
|---------|---------|---------------|----------------|
| sljit | 1.051x | 1.06x | 19/22 |
| asmjit | 1.035x | 1.05x | 17/22 |
| mir | 1.009x | 1.02x | 10/22 |
| auto | 1.049x | 1.06x | 18/22 |

All times in ms (median of 3 runs). Parallel workers disabled. work_mem = 256MB.

## Appendix: TPC-H Queries

### Q1 — Pricing Summary Report
```sql
SELECT l_returnflag, l_linestatus, sum(l_quantity) as sum_qty,
       sum(l_extendedprice) as sum_base_price,
       sum(l_extendedprice*(1-l_discount)) as sum_disc_price,
       sum(l_extendedprice*(1-l_discount)*(1+l_tax)) as sum_charge,
       avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*)
FROM lineitem
WHERE l_shipdate <= date '1998-12-01' - interval '90 day'
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
```

### Q2 — Minimum Cost Supplier
```sql
SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
FROM part, supplier, partsupp, nation, region
WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey
  AND p_size = 15 AND p_type LIKE '%BRASS'
  AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = 'EUROPE'
  AND ps_supplycost = (SELECT min(ps_supplycost) FROM partsupp, supplier, nation, region
    WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey
      AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = 'EUROPE')
ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100;
```

### Q3 — Shipping Priority
```sql
SELECT l_orderkey, sum(l_extendedprice*(1-l_discount)) as revenue, o_orderdate, o_shippriority
FROM customer, orders, lineitem
WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey
  AND o_orderdate < date '1995-03-15' AND l_shipdate > date '1995-03-15'
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate LIMIT 10;
```

### Q4 — Order Priority Checking
```sql
SELECT o_orderpriority, count(*) as order_count
FROM orders WHERE o_orderdate >= date '1993-07-01'
  AND o_orderdate < date '1993-07-01' + interval '3 month'
  AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
GROUP BY o_orderpriority ORDER BY o_orderpriority;
```

### Q5 — Local Supplier Volume
```sql
SELECT n_name, sum(l_extendedprice*(1-l_discount)) as revenue
FROM customer, orders, lineitem, supplier, nation, region
WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey
  AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
  AND r_name = 'ASIA' AND o_orderdate >= date '1994-01-01'
  AND o_orderdate < date '1994-01-01' + interval '1 year'
GROUP BY n_name ORDER BY revenue DESC;
```

### Q6 — Forecasting Revenue Change
```sql
SELECT sum(l_extendedprice*l_discount) as revenue
FROM lineitem WHERE l_shipdate >= date '1994-01-01'
  AND l_shipdate < date '1994-01-01' + interval '1 year'
  AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24;
```

### Q7 — Volume Shipping
```sql
SELECT supp_nation, cust_nation, l_year, sum(volume) as revenue
FROM (SELECT n1.n_name as supp_nation, n2.n_name as cust_nation,
       extract(year from l_shipdate) as l_year, l_extendedprice*(1-l_discount) as volume
  FROM supplier, lineitem, orders, customer, nation n1, nation n2
  WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey AND c_custkey = o_custkey
    AND s_nationkey = n1.n_nationkey AND c_nationkey = n2.n_nationkey
    AND ((n1.n_name='FRANCE' AND n2.n_name='GERMANY') OR (n1.n_name='GERMANY' AND n2.n_name='FRANCE'))
    AND l_shipdate BETWEEN date '1995-01-01' AND date '1996-12-31') as shipping
GROUP BY supp_nation, cust_nation, l_year ORDER BY supp_nation, cust_nation, l_year;
```

### Q8–Q22
Remaining queries follow standard TPC-H specification with default substitution parameters. Full SQL available in `tests/bench_tpch.sh`.
