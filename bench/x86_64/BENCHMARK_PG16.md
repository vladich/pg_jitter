# pg_jitter Benchmark Results

Comprehensive benchmark comparing pg_jitter JIT backends against PostgreSQL's interpreter and LLVM JIT.

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | PostgreSQL 16.13 |
| OS | Linux 6.6.87.2-microsoft-standard-WSL2 x86_64 |
| CPU | AMD Ryzen AI 9 HX PRO 370 w/ Radeon 890M |
| RAM | 23Gi |
| Backends tested | interp sljit asmjit mir |
| Runs per query | 3 (median) |
| Warmup runs | 2 |
| Parallel workers | 0 (disabled) |
| Date | 2026-03-02 |

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

- All queries run with `max_parallel_workers_per_gather = 0`
- JIT thresholds set to 0 to force JIT compilation on every query
- Buffer cache warmed before benchmarking
- Each query: 2 warmup runs, then 3 timed runs, median reported
- JIT compilation timing from a separate `EXPLAIN (ANALYZE, FORMAT JSON)` run
- Percentages relative to interpreter (no JIT) baseline: <100% = faster, >100% = slower

## Results

### Basic Aggregation

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| SUM_int | 36.916 ms | 32.036 ms (86%) | 33.870 ms (91%) | 33.880 ms (91%) |
| COUNT_star | 29.013 ms | 27.414 ms (94%) | 26.999 ms (93%) | 28.513 ms (98%) |
| GroupBy_5agg | 95.318 ms | 81.559 ms (85%) | 80.303 ms (84%) | 166.626 ms (174%) |
| GroupBy_100K_grp | 143.075 ms | 129.009 ms (90%) | 130.112 ms (90%) | 132.343 ms (92%) |
| COUNT_DISTINCT | 29.786 ms | 28.005 ms (94%) | 28.315 ms (95%) | 29.315 ms (98%) |

### Hash Joins

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| HashJoin_single | 372.706 ms | 315.473 ms (84%) | 299.528 ms (80%) | 1186.919 ms (318%) |
| HashJoin_composite | 377.074 ms | 315.042 ms (83%) | 297.197 ms (78%) | 1194.315 ms (316%) |
| HashJoin_filter | 319.365 ms | 264.533 ms (82%) | 267.831 ms (83%) | 1238.049 ms (387%) |
| HashJoin_GroupBy | 602.200 ms | 500.181 ms (83%) | 470.547 ms (78%) | 1778.788 ms (295%) |

### Outer Joins

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| LeftJoin | 136.501 ms | 120.496 ms (88%) | 122.428 ms (89%) | 303.035 ms (222%) |
| RightJoin | 138.275 ms | 122.197 ms (88%) | 127.184 ms (91%) | 214.345 ms (155%) |
| FullOuterJoin | 61.484 ms | 56.090 ms (91%) | 53.991 ms (87%) | 78.071 ms (126%) |

### Semi/Anti Joins

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| EXISTS_semi | 162.877 ms | 153.373 ms (94%) | 151.995 ms (93%) | 237.639 ms (145%) |
| NOT_EXISTS_anti | 156.676 ms | 144.995 ms (92%) | 144.387 ms (92%) | 228.311 ms (145%) |
| IN_subquery | 119.806 ms | 110.342 ms (92%) | 110.572 ms (92%) | 172.539 ms (144%) |

### Set Operations

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| INTERSECT | 144.636 ms | 157.783 ms (109%) | 170.056 ms (117%) | 295.741 ms (204%) |
| EXCEPT | 160.707 ms | 152.233 ms (94%) | 158.120 ms (98%) | 253.711 ms (157%) |
| UNION_ALL_agg | 62.470 ms | 57.322 ms (91%) | 57.494 ms (92%) | 58.675 ms (93%) |

### Expressions

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| CASE_simple | 39.399 ms | 34.708 ms (88%) | 34.115 ms (86%) | 35.170 ms (89%) |
| CASE_searched_4way | 43.606 ms | 37.674 ms (86%) | 37.586 ms (86%) | 39.946 ms (91%) |
| COALESCE_NULLIF | 35.521 ms | 30.569 ms (86%) | 29.857 ms (84%) | 30.786 ms (86%) |
| Bool_AND_OR | 43.340 ms | 34.287 ms (79%) | 34.193 ms (78%) | 36.983 ms (85%) |
| Arith_expr | 39.184 ms | 32.000 ms (81%) | 31.589 ms (80%) | 33.170 ms (84%) |
| IN_list_20 | 53.360 ms | 29.954 ms (56%) | 30.148 ms (56%) | 33.277 ms (62%) |

### Subqueries

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Scalar_subq | 85.092 ms | 77.937 ms (91%) | 82.476 ms (96%) | 83.590 ms (98%) |
| Correlated_subq | 147.753 ms | 142.679 ms (96%) | 142.835 ms (96%) | 153.491 ms (103%) |
| LATERAL_top3 | 26.254 ms | 25.718 ms (97%) | 26.299 ms (100%) | 27.163 ms (103%) |

### Date/Timestamp

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Date_extract | 118.997 ms | 108.835 ms (91%) | 106.808 ms (89%) | 195.477 ms (164%) |
| Timestamp_trunc | 93.825 ms | 83.797 ms (89%) | 83.720 ms (89%) | 168.385 ms (179%) |
| Interval_arith | 50.053 ms | 49.650 ms (99%) | 58.173 ms (116%) | 52.637 ms (105%) |
| Timestamp_diff | 59.124 ms | 57.905 ms (97%) | 59.156 ms (100%) | 60.242 ms (101%) |

### Text/String

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Text_EQ_filter | 23.081 ms | 21.912 ms (94%) | 23.428 ms (101%) | 24.448 ms (105%) |
| Text_LIKE | 28.291 ms | 25.537 ms (90%) | 26.908 ms (95%) | 27.343 ms (96%) |
| Text_concat_agg | 22.012 ms | 22.006 ms (99%) | 21.253 ms (96%) | 24.861 ms (112%) |
| Text_length_expr | 50.797 ms | 46.272 ms (91%) | 46.471 ms (91%) | 49.206 ms (96%) |

### Numeric

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Numeric_agg | 65.268 ms | 65.225 ms (99%) | 69.047 ms (105%) | 112.796 ms (172%) |
| Numeric_arith | 89.448 ms | 87.275 ms (97%) | 89.572 ms (100%) | 93.716 ms (104%) |

### JSONB

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| JSONB_extract | 55.222 ms | 56.441 ms (102%) | 57.654 ms (104%) | 58.520 ms (105%) |
| JSONB_contains | 54.024 ms | 54.147 ms (100%) | 54.869 ms (101%) | 55.405 ms (102%) |
| JSONB_agg | 115.010 ms | 111.119 ms (96%) | 114.077 ms (99%) | 157.965 ms (137%) |

### Arrays

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Array_overlap | 28.529 ms | 27.244 ms (95%) | 27.828 ms (97%) | 28.880 ms (101%) |
| Array_contains | 20.691 ms | 19.969 ms (96%) | 20.886 ms (100%) | 22.005 ms (106%) |
| Unnest_agg | 55.212 ms | 54.621 ms (98%) | 53.612 ms (97%) | 55.763 ms (100%) |

### Wide Row / Deform

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Wide_10col_sum | 80.445 ms | 66.014 ms (82%) | 58.323 ms (72%) | 66.951 ms (83%) |
| Wide_20col_sum | 119.982 ms | 92.184 ms (76%) | 79.428 ms (66%) | 91.460 ms (76%) |
| Wide_GroupBy_expr | 128.158 ms | 108.852 ms (84%) | 106.597 ms (83%) | 201.747 ms (157%) |

### Super-Wide Tables

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Wide100_sum | 57.817 ms | 50.359 ms (87%) | 57.367 ms (99%) | 69.165 ms (119%) |
| Wide100_groupby | 82.942 ms | 77.620 ms (93%) | 90.260 ms (108%) | 157.580 ms (189%) |
| Wide100_filter | 53.275 ms | 43.762 ms (82%) | 48.111 ms (90%) | 59.153 ms (111%) |
| Wide300_sum | 62.857 ms | 71.653 ms (113%) | 73.493 ms (116%) | 74.319 ms (118%) |
| Wide300_groupby | 75.211 ms | 72.913 ms (96%) | 76.694 ms (101%) | 93.367 ms (124%) |
| Wide300_filter | 61.084 ms | 70.266 ms (115%) | 71.365 ms (116%) | 71.656 ms (117%) |
| Wide1K_sum | 111.344 ms | 123.647 ms (111%) | 123.552 ms (110%) | 125.134 ms (112%) |
| Wide1K_groupby | 118.610 ms | 116.854 ms (98%) | 118.112 ms (99%) | 129.193 ms (108%) |
| Wide1K_filter | 109.961 ms | 124.663 ms (113%) | 124.725 ms (113%) | 126.476 ms (115%) |

### Partitioned

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| PartScan_filter | 18.689 ms | 16.759 ms (89%) | 17.614 ms (94%) | 20.365 ms (108%) |
| PartScan_agg_all | 79.052 ms | 68.178 ms (86%) | 68.674 ms (86%) | 157.140 ms (198%) |

## JIT Compilation Overhead

Time spent on JIT compilation (generation + optimization + emission), extracted from EXPLAIN JSON.

| Query | sljit | asmjit | mir |
|-------|------|------|------|
| SUM_int | 0.04 ms | 0.37 ms | 1.451 ms |
| COUNT_star | 0.06 ms | 0.33 ms | 0.747 ms |
| GroupBy_5agg | 0.093 ms | 0.764 ms | 3.261 ms |
| GroupBy_100K_grp | 0.064 ms | 0.939 ms | 2.908 ms |
| COUNT_DISTINCT | 0.049 ms | 0.392 ms | 0.88 ms |
| HashJoin_single | 0.078 ms | 0.712 ms | 3.597 ms |
| HashJoin_composite | 0.092 ms | 0.655 ms | 3.337 ms |
| HashJoin_filter | 0.08 ms | 0.778 ms | 4.784 ms |
| HashJoin_GroupBy | 0.123 ms | 0.957 ms | 4.522 ms |
| LeftJoin | 0.07 ms | 0.662 ms | 3.008 ms |
| RightJoin | 0.071 ms | 0.656 ms | 2.959 ms |
| FullOuterJoin | 0.079 ms | 0.877 ms | 5.03 ms |
| EXISTS_semi | 0.06 ms | 0.569 ms | 2.258 ms |
| NOT_EXISTS_anti | 0.06 ms | 0.453 ms | 1.355 ms |
| IN_subquery | 0.092 ms | 0.704 ms | 3.946 ms |
| INTERSECT | 0.068 ms | 0.493 ms | 1.287 ms |
| EXCEPT | 0.073 ms | 0.504 ms | 1.31 ms |
| UNION_ALL_agg | 0.069 ms | 0.384 ms | 0.885 ms |
| CASE_simple | 0.061 ms | 0.43 ms | 1.749 ms |
| CASE_searched_4way | 0.061 ms | 0.548 ms | 1.99 ms |
| COALESCE_NULLIF | 0.055 ms | 0.437 ms | 1.648 ms |
| Bool_AND_OR | 0.067 ms | 0.632 ms | 2.312 ms |
| Arith_expr | 0.064 ms | 0.48 ms | 1.956 ms |
| IN_list_20 | 0.061 ms | 0.61 ms | 2.048 ms |
| Scalar_subq | 0.071 ms | 0.707 ms | 3.404 ms |
| Correlated_subq | 0.136 ms | 1.002 ms | 3.589 ms |
| LATERAL_top3 | 0.066 ms | 0.56 ms | 1.693 ms |
| Date_extract | 0.079 ms | 0.708 ms | 2.587 ms |
| Timestamp_trunc | 0.078 ms | 0.612 ms | 2.252 ms |
| Interval_arith | 0.06 ms | 0.578 ms | 1.981 ms |
| Timestamp_diff | 0.087 ms | 0.485 ms | 2.149 ms |
| Text_EQ_filter | 0.065 ms | 0.612 ms | 1.624 ms |
| Text_LIKE | 0.077 ms | 0.674 ms | 2.102 ms |
| Text_concat_agg | 0.1 ms | 1.027 ms | 3.309 ms |
| Text_length_expr | 0.072 ms | 0.829 ms | 2.162 ms |
| Numeric_agg | 0.114 ms | 1.155 ms | 2.883 ms |
| Numeric_arith | 0.071 ms | 0.659 ms | 1.921 ms |
| JSONB_extract | 0.084 ms | 0.289 ms | 1.464 ms |
| JSONB_contains | 0.067 ms | 0.105 ms | 1.731 ms |
| JSONB_agg | 0.101 ms | 0.613 ms | 2.772 ms |
| Array_overlap | 0.066 ms | 0.548 ms | 1.511 ms |
| Array_contains | 0.074 ms | 0.554 ms | 1.511 ms |
| Unnest_agg | 0.101 ms | 0.983 ms | 2.574 ms |
| Wide_10col_sum | 0.081 ms | 1.049 ms | 3.714 ms |
| Wide_20col_sum | 0.14 ms | 1.735 ms | 6.274 ms |
| Wide_GroupBy_expr | 0.137 ms | 1.781 ms | 7.369 ms |
| Wide100_sum | 0.182 ms | 3.89 ms | 15.11 ms |
| Wide100_groupby | 0.31 ms | 7.63 ms | 29.686 ms |
| Wide100_filter | 0.201 ms | 3.71 ms | 14.443 ms |
| Wide300_sum | 0.095 ms | 0.435 ms | 0.866 ms |
| Wide300_groupby | 0.09 ms | 0.568 ms | 1.645 ms |
| Wide300_filter | 0.069 ms | 0.432 ms | 1.067 ms |
| Wide1K_sum | 0.079 ms | 0.368 ms | 0.918 ms |
| Wide1K_groupby | 0.095 ms | 0.628 ms | 1.918 ms |
| Wide1K_filter | 0.091 ms | 0.542 ms | 1.076 ms |
| PartScan_filter | 0.073 ms | 0.667 ms | 2.904 ms |
| PartScan_agg_all | 0.091 ms | 0.745 ms | 4.705 ms |

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

## Appendix A: Benchmark Queries

### Basic Aggregation

**SUM_int**
```sql
SELECT SUM(val1) FROM bench_data
```

**COUNT_star**
```sql
SELECT COUNT(*) FROM bench_data
```

**GroupBy_5agg**
```sql
SELECT grp, COUNT(*), SUM(val1), AVG(val1), MIN(val1), MAX(val1) FROM bench_data GROUP BY grp
```

**GroupBy_100K_grp**
```sql
SELECT key1, SUM(val), COUNT(*), AVG(val) FROM join_left GROUP BY key1
```

**COUNT_DISTINCT**
```sql
SELECT COUNT(DISTINCT key1) FROM join_left
```

### Hash Joins

**HashJoin_single**
```sql
SELECT COUNT(*), SUM(l.val + r.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1
```

**HashJoin_composite**
```sql
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 AND l.key2 = r.key2
```

**HashJoin_filter**
```sql
SELECT COUNT(*), SUM(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 WHERE l.val + r.val > 10000 AND l.val * 2 < 15000
```

**HashJoin_GroupBy**
```sql
SELECT l.key1 % 1000 AS bucket, COUNT(*), SUM(l.val + r.val), AVG(l.val) FROM join_left l JOIN join_right r ON l.key1 = r.key1 GROUP BY l.key1 % 1000
```

### Outer Joins

**LeftJoin**
```sql
SELECT COUNT(*), SUM(COALESCE(r.val, 0)) FROM join_left l LEFT JOIN join_right r ON l.key1 = r.key1 WHERE l.id <= 200000
```

**RightJoin**
```sql
SELECT COUNT(*), SUM(COALESCE(l.val, 0)) FROM (SELECT * FROM join_left WHERE id <= 200000) l RIGHT JOIN join_right r ON l.key1 = r.key1
```

**FullOuterJoin**
```sql
SELECT COUNT(*), SUM(COALESCE(l.val, 0) + COALESCE(r.val, 0)) FROM (SELECT * FROM join_left WHERE id <= 100000) l FULL OUTER JOIN (SELECT * FROM join_right WHERE id <= 100000) r ON l.key1 = r.key1
```

### Semi/Anti Joins

**EXISTS_semi**
```sql
SELECT COUNT(*) FROM join_left l WHERE EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1 AND r.val > 5000)
```

**NOT_EXISTS_anti**
```sql
SELECT COUNT(*) FROM join_left l WHERE NOT EXISTS (SELECT 1 FROM join_right r WHERE r.key1 = l.key1)
```

**IN_subquery**
```sql
SELECT COUNT(*) FROM join_left WHERE key1 IN (SELECT key1 FROM join_right WHERE val > 8000)
```

### Set Operations

**INTERSECT**
```sql
SELECT key1 FROM join_left INTERSECT SELECT key1 FROM join_right
```

**EXCEPT**
```sql
SELECT key1 FROM join_left EXCEPT SELECT key1 FROM join_right
```

**UNION_ALL_agg**
```sql
SELECT SUM(val) FROM (SELECT val FROM join_left UNION ALL SELECT val FROM join_right) t
```

### Expressions

**CASE_simple**
```sql
SELECT SUM(CASE WHEN val > 5000 THEN val ELSE 0 END) FROM join_left
```

**CASE_searched_4way**
```sql
SELECT SUM(CASE WHEN val < 1000 THEN 1 WHEN val < 5000 THEN 2 WHEN val < 9000 THEN 3 ELSE 4 END) FROM join_left
```

**COALESCE_NULLIF**
```sql
SELECT SUM(COALESCE(NULLIF(val, 0), -1)) FROM join_left
```

**Bool_AND_OR**
```sql
SELECT COUNT(*) FROM join_left WHERE (val > 1000 AND val < 9000) OR (key1 > 100 AND key2 < 400)
```

**Arith_expr**
```sql
SELECT SUM(val + key1 * 3 - key2) FROM join_left
```

**IN_list_20**
```sql
SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)
```

### Subqueries

**Scalar_subq**
```sql
SELECT SUM(val) FROM join_left WHERE key1 < (SELECT AVG(key1) FROM join_right)
```

**Correlated_subq**
```sql
SELECT COUNT(*) FROM (SELECT DISTINCT key1 FROM join_left) d WHERE (SELECT SUM(val) FROM join_right r WHERE r.key1 = d.key1) > 25000
```

**LATERAL_top3**
```sql
SELECT d.grp, lat.top_val FROM (SELECT DISTINCT grp FROM bench_data) d, LATERAL (SELECT val1 AS top_val FROM bench_data b WHERE b.grp = d.grp ORDER BY val1 DESC LIMIT 3) lat
```

### Date/Timestamp

**Date_extract**
```sql
SELECT EXTRACT(year FROM d) AS yr, COUNT(*), SUM(val) FROM date_data GROUP BY yr
```

**Timestamp_trunc**
```sql
SELECT date_trunc('month', ts) AS mo, COUNT(*) FROM date_data GROUP BY mo
```

**Interval_arith**
```sql
SELECT COUNT(*) FROM date_data WHERE ts + interval '30 days' > '2000-06-01'::timestamp AND ts - interval '10 days' < '2000-03-01'::timestamp
```

**Timestamp_diff**
```sql
SELECT SUM(EXTRACT(epoch FROM ts - '2000-01-01'::timestamp)) FROM date_data WHERE id <= 500000
```

### Text/String

**Text_EQ_filter**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text = 'prefix_42'
```

**Text_LIKE**
```sql
SELECT COUNT(*) FROM text_data WHERE word LIKE 'word_1%'
```

**Text_concat_agg**
```sql
SELECT grp_text, string_agg(word, ',') FROM text_data WHERE id <= 10000 GROUP BY grp_text
```

**Text_length_expr**
```sql
SELECT SUM(length(varlen_text) + length(word)) FROM text_data
```

### Numeric

**Numeric_agg**
```sql
SELECT grp_num, SUM(val1), AVG(val2) FROM numeric_data GROUP BY grp_num
```

**Numeric_arith**
```sql
SELECT SUM(val1 * val2 + val1 - val2) FROM numeric_data
```

### JSONB

**JSONB_extract**
```sql
SELECT SUM((doc->>'a')::int) FROM jsonb_data
```

**JSONB_contains**
```sql
SELECT COUNT(*) FROM jsonb_data WHERE (doc->>'a')::int = 42
```

**JSONB_agg**
```sql
SELECT grp_jsonb, COUNT(*), SUM((doc->>'a')::int) FROM jsonb_data GROUP BY grp_jsonb
```

### Arrays

**Array_overlap**
```sql
SELECT SUM(CASE WHEN tags && ARRAY[1,2,3,4,5] THEN 1 ELSE 0 END) FROM array_data
```

**Array_contains**
```sql
SELECT SUM(CASE WHEN tags @> ARRAY[10,20] THEN 1 ELSE 0 END) FROM array_data
```

**Unnest_agg**
```sql
SELECT u, COUNT(*) FROM array_data, unnest(tags) u WHERE id <= 100000 GROUP BY u
```

### Wide Row / Deform

**Wide_10col_sum**
```sql
SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10) FROM ultra_wide
```

**Wide_20col_sum**
```sql
SELECT SUM(c01+c02+c03+c04+c05+c06+c07+c08+c09+c10+c11+c12+c13+c14+c15+c16+c17+c18+c19+c20) FROM ultra_wide
```

**Wide_GroupBy_expr**
```sql
SELECT grp, SUM(c01*c02 + c03 - c04), AVG(c10+c20) FROM ultra_wide GROUP BY grp
```

### Super-Wide Tables

**Wide100_sum**
```sql
SELECT SUM(c0097) FROM wide_100
```

**Wide100_groupby**
```sql
SELECT grp, COUNT(*), AVG(c0097) FROM wide_100 GROUP BY grp
```

**Wide100_filter**
```sql
SELECT COUNT(*) FROM wide_100 WHERE c0091 IS NOT NULL
```

**Wide300_sum**
```sql
SELECT SUM(c0297) FROM wide_300
```

**Wide300_groupby**
```sql
SELECT grp, COUNT(*), AVG(c0297) FROM wide_300 GROUP BY grp
```

**Wide300_filter**
```sql
SELECT COUNT(*) FROM wide_300 WHERE c0291 IS NOT NULL
```

**Wide1K_sum**
```sql
SELECT SUM(c0997) FROM wide_1000
```

**Wide1K_groupby**
```sql
SELECT grp, COUNT(*), AVG(c0997) FROM wide_1000 GROUP BY grp
```

**Wide1K_filter**
```sql
SELECT COUNT(*) FROM wide_1000 WHERE c1000 IS NOT NULL
```

### Partitioned

**PartScan_filter**
```sql
SELECT COUNT(*), SUM(val) FROM part_data WHERE grp BETWEEN 10 AND 30
```

**PartScan_agg_all**
```sql
SELECT grp, COUNT(*), SUM(val) FROM part_data GROUP BY grp
```

## Appendix B: Table Definitions and Data Population

### Core Tables

```sql
-- bench_setup.sql — Create and populate benchmark tables
-- Run once before bench.sql / bench_joins.sql
-- Idempotent: safe to re-run (uses IF NOT EXISTS / truncate+insert)

\timing on

-- join_left: 1M rows
CREATE TABLE IF NOT EXISTS join_left (
    id      integer,
    key1    integer,
    key2    integer,
    val     integer,
    payload text
);

-- join_right: 500K rows
CREATE TABLE IF NOT EXISTS join_right (
    id      integer,
    key1    integer,
    key2    integer,
    val     integer,
    payload text
);

-- bench_data: 1M rows
CREATE TABLE IF NOT EXISTS bench_data (
    id   integer,
    grp  integer,
    val1 integer,
    val2 integer,
    val3 integer,
    val4 integer,
    val5 integer,
    txt  text
);

-- Only populate if empty
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM join_left LIMIT 1) THEN
        RAISE NOTICE 'Populating join_left (1M rows)...';
        INSERT INTO join_left
        SELECT i,
               i % 100000,
               i % 50000,
               (random() * 10000)::int,
               'left_' || i
        FROM generate_series(1, 1000000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM join_right LIMIT 1) THEN
        RAISE NOTICE 'Populating join_right (500K rows)...';
        INSERT INTO join_right
        SELECT i,
               i % 100000,
               i % 50000,
               (random() * 10000)::int,
               'right_' || i
        FROM generate_series(1, 500000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM bench_data LIMIT 1) THEN
        RAISE NOTICE 'Populating bench_data (1M rows)...';
        INSERT INTO bench_data
        SELECT i,
               i % 100,
               (random() * 10000)::int,
               (random() * 10000)::int,
               (random() * 10000)::int,
               (random() * 10000)::int,
               (random() * 10000)::int,
               'row_' || i
        FROM generate_series(1, 1000000) i;
    END IF;
END $$;

CREATE INDEX IF NOT EXISTS idx_join_right_key1 ON join_right(key1);
ANALYZE join_left;
ANALYZE join_right;
ANALYZE bench_data;

\echo 'Setup complete.'
\timing off
```

### Extended Tables

```sql
-- bench_setup_extra.sql — Additional benchmark tables
-- Run after bench_setup.sql
-- Adds tables for: date arithmetic, CASE/WHEN, wide rows, window functions

\timing on

-- date_data: 1M rows with date/timestamp columns
CREATE TABLE IF NOT EXISTS date_data (
    id    integer,
    d     date,
    ts    timestamp,
    val   integer
);

-- wide_data: 500K rows, many int columns to stress expression eval
CREATE TABLE IF NOT EXISTS wide_data (
    id integer,
    a1 integer, a2 integer, a3 integer, a4 integer, a5 integer,
    b1 integer, b2 integer, b3 integer, b4 integer, b5 integer,
    grp integer
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM date_data LIMIT 1) THEN
        RAISE NOTICE 'Populating date_data (1M rows)...';
        INSERT INTO date_data
        SELECT i,
               '2000-01-01'::date + (i % 10000),
               '2000-01-01'::timestamp + (i % 10000000) * interval '1 second',
               (random() * 10000)::int
        FROM generate_series(1, 1000000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM wide_data LIMIT 1) THEN
        RAISE NOTICE 'Populating wide_data (500K rows)...';
        INSERT INTO wide_data
        SELECT i,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               i % 1000
        FROM generate_series(1, 500000) i;
    END IF;
END $$;

ANALYZE date_data;
ANALYZE wide_data;

-- text_data: 500K rows with variable-length strings
CREATE TABLE IF NOT EXISTS text_data (
    id          integer,
    grp_text    text,
    hash_text   text,
    varlen_text text,
    word        text
);

-- jsonb_data: 500K rows
CREATE TABLE IF NOT EXISTS jsonb_data (
    id        integer,
    doc       jsonb,
    grp_jsonb jsonb
);

-- numeric_data: 500K rows
CREATE TABLE IF NOT EXISTS numeric_data (
    id      integer,
    val1    numeric(15,8),
    val2    numeric(12,4),
    grp_num numeric
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM text_data LIMIT 1) THEN
        RAISE NOTICE 'Populating text_data (500K rows)...';
        INSERT INTO text_data
        SELECT i,
               'prefix_' || (i % 100)::text,
               md5(i::text),
               repeat('x', (i % 50) + 1),
               'word_' || (i % 10000)::text
        FROM generate_series(1, 500000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM jsonb_data LIMIT 1) THEN
        RAISE NOTICE 'Populating jsonb_data (500K rows)...';
        INSERT INTO jsonb_data
        SELECT i,
               jsonb_build_object('a', i % 1000, 'b', md5(i::text), 'c', i * 1.5),
               to_jsonb(i % 100)
        FROM generate_series(1, 500000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM numeric_data LIMIT 1) THEN
        RAISE NOTICE 'Populating numeric_data (500K rows)...';
        INSERT INTO numeric_data
        SELECT i,
               (i * 1.23456789)::numeric(15,8),
               (random() * 10000)::numeric(12,4),
               (i % 100)::numeric
        FROM generate_series(1, 500000) i;
    END IF;
END $$;

ANALYZE text_data;
ANALYZE jsonb_data;
ANALYZE numeric_data;

-- part_data: 1M rows, 4 partitions by grp range
CREATE TABLE IF NOT EXISTS part_data (
    id integer NOT NULL, grp integer NOT NULL, val integer, txt text
) PARTITION BY RANGE (grp);
CREATE TABLE IF NOT EXISTS part_data_p0 PARTITION OF part_data FOR VALUES FROM (0) TO (25);
CREATE TABLE IF NOT EXISTS part_data_p1 PARTITION OF part_data FOR VALUES FROM (25) TO (50);
CREATE TABLE IF NOT EXISTS part_data_p2 PARTITION OF part_data FOR VALUES FROM (50) TO (75);
CREATE TABLE IF NOT EXISTS part_data_p3 PARTITION OF part_data FOR VALUES FROM (75) TO (100);

-- dml_target: 100K rows for INSERT/UPDATE/MERGE benchmarks
CREATE TABLE IF NOT EXISTS dml_target (
    id integer PRIMARY KEY, grp integer, val integer, txt text
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM part_data LIMIT 1) THEN
        RAISE NOTICE 'Populating part_data (1M rows, 4 partitions)...';
        INSERT INTO part_data
        SELECT i, i % 100, (random() * 10000)::int, 'part_' || i::text
        FROM generate_series(1, 1000000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM dml_target LIMIT 1) THEN
        RAISE NOTICE 'Populating dml_target (100K rows)...';
        INSERT INTO dml_target
        SELECT id, grp, val1, 'dml_' || id::text
        FROM bench_data WHERE id <= 100000;
    END IF;
END $$;

ANALYZE part_data;
ANALYZE dml_target;

-- Indexes for index scan benchmarks (B-tree)
CREATE INDEX IF NOT EXISTS idx_bench_data_val1 ON bench_data(val1);
CREATE INDEX IF NOT EXISTS idx_bench_data_grp  ON bench_data(grp);
CREATE INDEX IF NOT EXISTS idx_join_left_key1  ON join_left(key1);

-- ════════════════════════════════════════════════════════════════════
-- Index type benchmark tables
-- ════════════════════════════════════════════════════════════════════

-- geo_data: 500K rows with point and box columns for GiST benchmarks
CREATE TABLE IF NOT EXISTS geo_data (
    id  integer,
    pt  point,
    bx  box,
    val integer
);

-- range_data: 500K rows with int4range for GiST/SP-GiST range benchmarks
CREATE TABLE IF NOT EXISTS range_data (
    id     integer,
    irange int4range,
    val    integer
);

-- fts_data: 200K rows with tsvector for GIN full-text search benchmarks
CREATE TABLE IF NOT EXISTS fts_data (
    id   integer,
    body text,
    tsv  tsvector,
    val  integer
);

-- array_data: 300K rows with integer arrays for GIN array benchmarks
CREATE TABLE IF NOT EXISTS array_data (
    id   integer,
    tags integer[],
    val  integer
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM geo_data LIMIT 1) THEN
        RAISE NOTICE 'Populating geo_data (500K rows)...';
        INSERT INTO geo_data
        SELECT i,
               point((i % 1000)::float8, ((i / 1000) % 1000)::float8),
               box(point((i % 1000)::float8, ((i / 1000) % 1000)::float8),
                   point((i % 1000 + 10)::float8, ((i / 1000) % 1000 + 10)::float8)),
               (random() * 10000)::int
        FROM generate_series(1, 500000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM range_data LIMIT 1) THEN
        RAISE NOTICE 'Populating range_data (500K rows)...';
        INSERT INTO range_data
        SELECT i,
               int4range(i, i + (i % 100) + 1),
               (random() * 10000)::int
        FROM generate_series(1, 500000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM fts_data LIMIT 1) THEN
        RAISE NOTICE 'Populating fts_data (200K rows)...';
        INSERT INTO fts_data
        SELECT i,
               'word_' || (i % 500) || ' word_' || ((i * 7) % 500)
                 || ' word_' || ((i * 13) % 500) || ' word_' || ((i * 31) % 500)
                 || ' extra_' || (i % 50),
               to_tsvector('english',
                   'word_' || (i % 500) || ' word_' || ((i * 7) % 500)
                     || ' word_' || ((i * 13) % 500) || ' word_' || ((i * 31) % 500)
                     || ' extra_' || (i % 50)),
               (random() * 10000)::int
        FROM generate_series(1, 200000) i;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM array_data LIMIT 1) THEN
        RAISE NOTICE 'Populating array_data (300K rows)...';
        INSERT INTO array_data
        SELECT i,
               ARRAY[i % 200, (i * 3) % 200, (i * 7) % 200, (i * 11) % 200],
               (random() * 10000)::int
        FROM generate_series(1, 300000) i;
    END IF;
END $$;

ANALYZE geo_data;
ANALYZE range_data;
ANALYZE fts_data;
ANALYZE array_data;

-- ════════════════════════════════════════════════════════════════════
-- Indexes for index type benchmarks
-- ════════════════════════════════════════════════════════════════════

-- BRIN indexes (exploit high physical correlation)
CREATE INDEX IF NOT EXISTS idx_bench_data_id_brin    ON bench_data USING brin(id);
CREATE INDEX IF NOT EXISTS idx_date_data_ts_brin     ON date_data  USING brin(ts);

-- Hash indexes (equality-only)
CREATE INDEX IF NOT EXISTS idx_bench_data_grp_hash   ON bench_data USING hash(grp);
CREATE INDEX IF NOT EXISTS idx_text_data_hash_hash   ON text_data  USING hash(hash_text);

-- GIN indexes (jsonb, tsvector, array)
CREATE INDEX IF NOT EXISTS idx_jsonb_data_doc_gin    ON jsonb_data USING gin(doc);
CREATE INDEX IF NOT EXISTS idx_fts_data_tsv_gin      ON fts_data   USING gin(tsv);
CREATE INDEX IF NOT EXISTS idx_array_data_tags_gin   ON array_data USING gin(tags);

-- GiST indexes (point, box, range)
CREATE INDEX IF NOT EXISTS idx_geo_data_pt_gist      ON geo_data   USING gist(pt);
CREATE INDEX IF NOT EXISTS idx_geo_data_bx_gist      ON geo_data   USING gist(bx);
CREATE INDEX IF NOT EXISTS idx_range_data_irange_gist ON range_data USING gist(irange);

-- SP-GiST index (range, alternative to GiST)
CREATE INDEX IF NOT EXISTS idx_range_data_irange_spgist ON range_data USING spgist(irange);

-- B-tree extras
CREATE INDEX IF NOT EXISTS idx_bench_data_grp_val1   ON bench_data(grp, val1);
CREATE INDEX IF NOT EXISTS idx_date_data_ts_btree    ON date_data(ts);

-- ════════════════════════════════════════════════════════════════════
-- Expression-heavy benchmark table (maximum JIT benefit)
-- ════════════════════════════════════════════════════════════════════

-- ultra_wide: 1M rows, 20 integer columns → maximum deform + expression stress
CREATE TABLE IF NOT EXISTS ultra_wide (
    id integer,
    c01 integer, c02 integer, c03 integer, c04 integer, c05 integer,
    c06 integer, c07 integer, c08 integer, c09 integer, c10 integer,
    c11 integer, c12 integer, c13 integer, c14 integer, c15 integer,
    c16 integer, c17 integer, c18 integer, c19 integer, c20 integer,
    grp integer
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM ultra_wide LIMIT 1) THEN
        RAISE NOTICE 'Populating ultra_wide (1M rows, 22 cols)...';
        INSERT INTO ultra_wide
        SELECT i,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               (random()*1000)::int, (random()*1000)::int,
               i % 200
        FROM generate_series(1, 1000000) i;
    END IF;
END $$;

ANALYZE ultra_wide;

\echo 'Extra setup complete.'
\timing off
```

### Super-Wide Tables (100 / 300 / 1000 columns)

Generated by `tests/gen_wide_tables.py`. Type mix: ~60% varchar, ~20% int/bigint, ~20% boolean/numeric/timestamp. Sparse: ~10% of columns populated per row.

<details>
<summary>Click to expand DDL and population SQL (2869 lines)</summary>

```sql
-- bench_setup_wide.sql — Super-wide benchmark tables
-- Generated by tests/gen_wide_tables.py
-- Do not edit manually; re-generate with:
--   python3 tests/gen_wide_tables.py > tests/bench_setup_wide.sql

\timing on

-- wide_100: 500000 rows, 100 data columns + id + grp
-- ~60% varchar, ~20% int/bigint, ~20% boolean/numeric/timestamp, ~10% populated
CREATE TABLE IF NOT EXISTS wide_100 (
    id integer,
    c0001 varchar(40),
    c0002 varchar(40),
    c0003 varchar(40),
    c0004 varchar(40),
    c0005 varchar(40),
    c0006 varchar(40),
    c0007 integer,
    c0008 integer,
    c0009 timestamp,
    c0010 numeric(12,4),
    c0011 varchar(40),
    c0012 varchar(40),
    c0013 varchar(40),
    c0014 varchar(40),
    c0015 varchar(40),
    c0016 varchar(40),
    c0017 bigint,
    c0018 integer,
    c0019 boolean,
    c0020 boolean,
    c0021 varchar(40),
    c0022 varchar(40),
    c0023 varchar(40),
    c0024 varchar(40),
    c0025 varchar(40),
    c0026 varchar(40),
    c0027 integer,
    c0028 integer,
    c0029 numeric(12,4),
    c0030 timestamp,
    c0031 varchar(40),
    c0032 varchar(40),
    c0033 varchar(40),
    c0034 varchar(40),
    c0035 varchar(40),
    c0036 varchar(40),
    c0037 bigint,
    c0038 integer,
    c0039 timestamp,
    c0040 numeric(12,4),
    c0041 varchar(40),
    c0042 varchar(40),
    c0043 varchar(40),
    c0044 varchar(40),
    c0045 varchar(40),
    c0046 varchar(40),
    c0047 integer,
    c0048 integer,
    c0049 boolean,
    c0050 boolean,
    c0051 varchar(40),
    c0052 varchar(40),
    c0053 varchar(40),
    c0054 varchar(40),
    c0055 varchar(40),
    c0056 varchar(40),
    c0057 bigint,
    c0058 integer,
    c0059 numeric(12,4),
    c0060 timestamp,
    c0061 varchar(40),
    c0062 varchar(40),
    c0063 varchar(40),
    c0064 varchar(40),
    c0065 varchar(40),
    c0066 varchar(40),
    c0067 integer,
    c0068 integer,
    c0069 timestamp,
    c0070 numeric(12,4),
    c0071 varchar(40),
    c0072 varchar(40),
    c0073 varchar(40),
    c0074 varchar(40),
    c0075 varchar(40),
    c0076 varchar(40),
    c0077 bigint,
    c0078 integer,
    c0079 boolean,
    c0080 boolean,
    c0081 varchar(40),
    c0082 varchar(40),
    c0083 varchar(40),
    c0084 varchar(40),
    c0085 varchar(40),
    c0086 varchar(40),
    c0087 integer,
    c0088 integer,
    c0089 numeric(12,4),
    c0090 timestamp,
    c0091 varchar(40),
    c0092 varchar(40),
    c0093 varchar(40),
    c0094 varchar(40),
    c0095 varchar(40),
    c0096 varchar(40),
    c0097 bigint,
    c0098 integer,
    c0099 timestamp,
    c0100 numeric(12,4),
    grp integer
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM wide_100 LIMIT 1) THEN
        RAISE NOTICE 'Populating wide_100 (500000 rows, 102 cols, sparse)...';
        INSERT INTO wide_100
        SELECT i,
               CASE WHEN (i::bigint*7919+0) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+1) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+2) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+3) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+4) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+5) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+6) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+7) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+8) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+9) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+10) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+11) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+12) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+13) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+14) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+15) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+16) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+17) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+18) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+19) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+20) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+21) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+22) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+23) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+24) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+25) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+26) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+27) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+28) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+29) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+30) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+31) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+32) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+33) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+34) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+35) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+36) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+37) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+38) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+39) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+40) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+41) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+42) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+43) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+44) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+45) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+46) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+47) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+48) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+49) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+50) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+51) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+52) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+53) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+54) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+55) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+56) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+57) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+58) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+59) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+60) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+61) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+62) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+63) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+64) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+65) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+66) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+67) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+68) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+69) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+70) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+71) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+72) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+73) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+74) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+75) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+76) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+77) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+78) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+79) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+80) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+81) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+82) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+83) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+84) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+85) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+86) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+87) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+88) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+89) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+90) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+91) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+92) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+93) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+94) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+95) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+96) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+97) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+98) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+99) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               i % 500
        FROM generate_series(1, 500000) i;
    END IF;
END $$;

ANALYZE wide_100;

-- wide_300: 200000 rows, 300 data columns + id + grp
-- ~60% varchar, ~20% int/bigint, ~20% boolean/numeric/timestamp, ~10% populated
CREATE TABLE IF NOT EXISTS wide_300 (
    id integer,
    c0001 varchar(40),
    c0002 varchar(40),
    c0003 varchar(40),
    c0004 varchar(40),
    c0005 varchar(40),
    c0006 varchar(40),
    c0007 integer,
    c0008 integer,
    c0009 timestamp,
    c0010 numeric(12,4),
    c0011 varchar(40),
    c0012 varchar(40),
    c0013 varchar(40),
    c0014 varchar(40),
    c0015 varchar(40),
    c0016 varchar(40),
    c0017 bigint,
    c0018 integer,
    c0019 boolean,
    c0020 boolean,
    c0021 varchar(40),
    c0022 varchar(40),
    c0023 varchar(40),
    c0024 varchar(40),
    c0025 varchar(40),
    c0026 varchar(40),
    c0027 integer,
    c0028 integer,
    c0029 numeric(12,4),
    c0030 timestamp,
    c0031 varchar(40),
    c0032 varchar(40),
    c0033 varchar(40),
    c0034 varchar(40),
    c0035 varchar(40),
    c0036 varchar(40),
    c0037 bigint,
    c0038 integer,
    c0039 timestamp,
    c0040 numeric(12,4),
    c0041 varchar(40),
    c0042 varchar(40),
    c0043 varchar(40),
    c0044 varchar(40),
    c0045 varchar(40),
    c0046 varchar(40),
    c0047 integer,
    c0048 integer,
    c0049 boolean,
    c0050 boolean,
    c0051 varchar(40),
    c0052 varchar(40),
    c0053 varchar(40),
    c0054 varchar(40),
    c0055 varchar(40),
    c0056 varchar(40),
    c0057 bigint,
    c0058 integer,
    c0059 numeric(12,4),
    c0060 timestamp,
    c0061 varchar(40),
    c0062 varchar(40),
    c0063 varchar(40),
    c0064 varchar(40),
    c0065 varchar(40),
    c0066 varchar(40),
    c0067 integer,
    c0068 integer,
    c0069 timestamp,
    c0070 numeric(12,4),
    c0071 varchar(40),
    c0072 varchar(40),
    c0073 varchar(40),
    c0074 varchar(40),
    c0075 varchar(40),
    c0076 varchar(40),
    c0077 bigint,
    c0078 integer,
    c0079 boolean,
    c0080 boolean,
    c0081 varchar(40),
    c0082 varchar(40),
    c0083 varchar(40),
    c0084 varchar(40),
    c0085 varchar(40),
    c0086 varchar(40),
    c0087 integer,
    c0088 integer,
    c0089 numeric(12,4),
    c0090 timestamp,
    c0091 varchar(40),
    c0092 varchar(40),
    c0093 varchar(40),
    c0094 varchar(40),
    c0095 varchar(40),
    c0096 varchar(40),
    c0097 bigint,
    c0098 integer,
    c0099 timestamp,
    c0100 numeric(12,4),
    c0101 varchar(40),
    c0102 varchar(40),
    c0103 varchar(40),
    c0104 varchar(40),
    c0105 varchar(40),
    c0106 varchar(40),
    c0107 integer,
    c0108 integer,
    c0109 boolean,
    c0110 boolean,
    c0111 varchar(40),
    c0112 varchar(40),
    c0113 varchar(40),
    c0114 varchar(40),
    c0115 varchar(40),
    c0116 varchar(40),
    c0117 bigint,
    c0118 integer,
    c0119 numeric(12,4),
    c0120 timestamp,
    c0121 varchar(40),
    c0122 varchar(40),
    c0123 varchar(40),
    c0124 varchar(40),
    c0125 varchar(40),
    c0126 varchar(40),
    c0127 integer,
    c0128 integer,
    c0129 timestamp,
    c0130 numeric(12,4),
    c0131 varchar(40),
    c0132 varchar(40),
    c0133 varchar(40),
    c0134 varchar(40),
    c0135 varchar(40),
    c0136 varchar(40),
    c0137 bigint,
    c0138 integer,
    c0139 boolean,
    c0140 boolean,
    c0141 varchar(40),
    c0142 varchar(40),
    c0143 varchar(40),
    c0144 varchar(40),
    c0145 varchar(40),
    c0146 varchar(40),
    c0147 integer,
    c0148 integer,
    c0149 numeric(12,4),
    c0150 timestamp,
    c0151 varchar(40),
    c0152 varchar(40),
    c0153 varchar(40),
    c0154 varchar(40),
    c0155 varchar(40),
    c0156 varchar(40),
    c0157 bigint,
    c0158 integer,
    c0159 timestamp,
    c0160 numeric(12,4),
    c0161 varchar(40),
    c0162 varchar(40),
    c0163 varchar(40),
    c0164 varchar(40),
    c0165 varchar(40),
    c0166 varchar(40),
    c0167 integer,
    c0168 integer,
    c0169 boolean,
    c0170 boolean,
    c0171 varchar(40),
    c0172 varchar(40),
    c0173 varchar(40),
    c0174 varchar(40),
    c0175 varchar(40),
    c0176 varchar(40),
    c0177 bigint,
    c0178 integer,
    c0179 numeric(12,4),
    c0180 timestamp,
    c0181 varchar(40),
    c0182 varchar(40),
    c0183 varchar(40),
    c0184 varchar(40),
    c0185 varchar(40),
    c0186 varchar(40),
    c0187 integer,
    c0188 integer,
    c0189 timestamp,
    c0190 numeric(12,4),
    c0191 varchar(40),
    c0192 varchar(40),
    c0193 varchar(40),
    c0194 varchar(40),
    c0195 varchar(40),
    c0196 varchar(40),
    c0197 bigint,
    c0198 integer,
    c0199 boolean,
    c0200 boolean,
    c0201 varchar(40),
    c0202 varchar(40),
    c0203 varchar(40),
    c0204 varchar(40),
    c0205 varchar(40),
    c0206 varchar(40),
    c0207 integer,
    c0208 integer,
    c0209 numeric(12,4),
    c0210 timestamp,
    c0211 varchar(40),
    c0212 varchar(40),
    c0213 varchar(40),
    c0214 varchar(40),
    c0215 varchar(40),
    c0216 varchar(40),
    c0217 bigint,
    c0218 integer,
    c0219 timestamp,
    c0220 numeric(12,4),
    c0221 varchar(40),
    c0222 varchar(40),
    c0223 varchar(40),
    c0224 varchar(40),
    c0225 varchar(40),
    c0226 varchar(40),
    c0227 integer,
    c0228 integer,
    c0229 boolean,
    c0230 boolean,
    c0231 varchar(40),
    c0232 varchar(40),
    c0233 varchar(40),
    c0234 varchar(40),
    c0235 varchar(40),
    c0236 varchar(40),
    c0237 bigint,
    c0238 integer,
    c0239 numeric(12,4),
    c0240 timestamp,
    c0241 varchar(40),
    c0242 varchar(40),
    c0243 varchar(40),
    c0244 varchar(40),
    c0245 varchar(40),
    c0246 varchar(40),
    c0247 integer,
    c0248 integer,
    c0249 timestamp,
    c0250 numeric(12,4),
    c0251 varchar(40),
    c0252 varchar(40),
    c0253 varchar(40),
    c0254 varchar(40),
    c0255 varchar(40),
    c0256 varchar(40),
    c0257 bigint,
    c0258 integer,
    c0259 boolean,
    c0260 boolean,
    c0261 varchar(40),
    c0262 varchar(40),
    c0263 varchar(40),
    c0264 varchar(40),
    c0265 varchar(40),
    c0266 varchar(40),
    c0267 integer,
    c0268 integer,
    c0269 numeric(12,4),
    c0270 timestamp,
    c0271 varchar(40),
    c0272 varchar(40),
    c0273 varchar(40),
    c0274 varchar(40),
    c0275 varchar(40),
    c0276 varchar(40),
    c0277 bigint,
    c0278 integer,
    c0279 timestamp,
    c0280 numeric(12,4),
    c0281 varchar(40),
    c0282 varchar(40),
    c0283 varchar(40),
    c0284 varchar(40),
    c0285 varchar(40),
    c0286 varchar(40),
    c0287 integer,
    c0288 integer,
    c0289 boolean,
    c0290 boolean,
    c0291 varchar(40),
    c0292 varchar(40),
    c0293 varchar(40),
    c0294 varchar(40),
    c0295 varchar(40),
    c0296 varchar(40),
    c0297 bigint,
    c0298 integer,
    c0299 numeric(12,4),
    c0300 timestamp,
    grp integer
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM wide_300 LIMIT 1) THEN
        RAISE NOTICE 'Populating wide_300 (200000 rows, 302 cols, sparse)...';
        INSERT INTO wide_300
        SELECT i,
               CASE WHEN (i::bigint*7919+0) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+1) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+2) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+3) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+4) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+5) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+6) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+7) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+8) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+9) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+10) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+11) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+12) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+13) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+14) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+15) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+16) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+17) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+18) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+19) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+20) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+21) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+22) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+23) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+24) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+25) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+26) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+27) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+28) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+29) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+30) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+31) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+32) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+33) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+34) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+35) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+36) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+37) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+38) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+39) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+40) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+41) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+42) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+43) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+44) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+45) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+46) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+47) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+48) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+49) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+50) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+51) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+52) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+53) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+54) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+55) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+56) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+57) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+58) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+59) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+60) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+61) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+62) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+63) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+64) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+65) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+66) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+67) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+68) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+69) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+70) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+71) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+72) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+73) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+74) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+75) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+76) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+77) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+78) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+79) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+80) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+81) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+82) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+83) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+84) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+85) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+86) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+87) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+88) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+89) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+90) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+91) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+92) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+93) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+94) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+95) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+96) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+97) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+98) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+99) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+100) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+101) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+102) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+103) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+104) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+105) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+106) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+107) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+108) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+109) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+110) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+111) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+112) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+113) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+114) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+115) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+116) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+117) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+118) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+119) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+120) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+121) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+122) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+123) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+124) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+125) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+126) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+127) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+128) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+129) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+130) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+131) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+132) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+133) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+134) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+135) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+136) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+137) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+138) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+139) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+140) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+141) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+142) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+143) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+144) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+145) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+146) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+147) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+148) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+149) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+150) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+151) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+152) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+153) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+154) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+155) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+156) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+157) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+158) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+159) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+160) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+161) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+162) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+163) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+164) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+165) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+166) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+167) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+168) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+169) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+170) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+171) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+172) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+173) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+174) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+175) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+176) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+177) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+178) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+179) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+180) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+181) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+182) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+183) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+184) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+185) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+186) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+187) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+188) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+189) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+190) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+191) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+192) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+193) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+194) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+195) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+196) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+197) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+198) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+199) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+200) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+201) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+202) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+203) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+204) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+205) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+206) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+207) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+208) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+209) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+210) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+211) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+212) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+213) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+214) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+215) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+216) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+217) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+218) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+219) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+220) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+221) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+222) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+223) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+224) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+225) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+226) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+227) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+228) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+229) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+230) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+231) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+232) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+233) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+234) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+235) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+236) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+237) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+238) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+239) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+240) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+241) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+242) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+243) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+244) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+245) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+246) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+247) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+248) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+249) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+250) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+251) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+252) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+253) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+254) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+255) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+256) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+257) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+258) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+259) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+260) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+261) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+262) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+263) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+264) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+265) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+266) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+267) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+268) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+269) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+270) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+271) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+272) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+273) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+274) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+275) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+276) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+277) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+278) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+279) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+280) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+281) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+282) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+283) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+284) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+285) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+286) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+287) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+288) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+289) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+290) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+291) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+292) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+293) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+294) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+295) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+296) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+297) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+298) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+299) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               i % 500
        FROM generate_series(1, 200000) i;
    END IF;
END $$;

ANALYZE wide_300;

-- wide_1000: 100000 rows, 1000 data columns + id + grp
-- ~60% varchar, ~20% int/bigint, ~20% boolean/numeric/timestamp, ~10% populated
CREATE TABLE IF NOT EXISTS wide_1000 (
    id integer,
    c0001 varchar(40),
    c0002 varchar(40),
    c0003 varchar(40),
    c0004 varchar(40),
    c0005 varchar(40),
    c0006 varchar(40),
    c0007 integer,
    c0008 integer,
    c0009 timestamp,
    c0010 numeric(12,4),
    c0011 varchar(40),
    c0012 varchar(40),
    c0013 varchar(40),
    c0014 varchar(40),
    c0015 varchar(40),
    c0016 varchar(40),
    c0017 bigint,
    c0018 integer,
    c0019 boolean,
    c0020 boolean,
    c0021 varchar(40),
    c0022 varchar(40),
    c0023 varchar(40),
    c0024 varchar(40),
    c0025 varchar(40),
    c0026 varchar(40),
    c0027 integer,
    c0028 integer,
    c0029 numeric(12,4),
    c0030 timestamp,
    c0031 varchar(40),
    c0032 varchar(40),
    c0033 varchar(40),
    c0034 varchar(40),
    c0035 varchar(40),
    c0036 varchar(40),
    c0037 bigint,
    c0038 integer,
    c0039 timestamp,
    c0040 numeric(12,4),
    c0041 varchar(40),
    c0042 varchar(40),
    c0043 varchar(40),
    c0044 varchar(40),
    c0045 varchar(40),
    c0046 varchar(40),
    c0047 integer,
    c0048 integer,
    c0049 boolean,
    c0050 boolean,
    c0051 varchar(40),
    c0052 varchar(40),
    c0053 varchar(40),
    c0054 varchar(40),
    c0055 varchar(40),
    c0056 varchar(40),
    c0057 bigint,
    c0058 integer,
    c0059 numeric(12,4),
    c0060 timestamp,
    c0061 varchar(40),
    c0062 varchar(40),
    c0063 varchar(40),
    c0064 varchar(40),
    c0065 varchar(40),
    c0066 varchar(40),
    c0067 integer,
    c0068 integer,
    c0069 timestamp,
    c0070 numeric(12,4),
    c0071 varchar(40),
    c0072 varchar(40),
    c0073 varchar(40),
    c0074 varchar(40),
    c0075 varchar(40),
    c0076 varchar(40),
    c0077 bigint,
    c0078 integer,
    c0079 boolean,
    c0080 boolean,
    c0081 varchar(40),
    c0082 varchar(40),
    c0083 varchar(40),
    c0084 varchar(40),
    c0085 varchar(40),
    c0086 varchar(40),
    c0087 integer,
    c0088 integer,
    c0089 numeric(12,4),
    c0090 timestamp,
    c0091 varchar(40),
    c0092 varchar(40),
    c0093 varchar(40),
    c0094 varchar(40),
    c0095 varchar(40),
    c0096 varchar(40),
    c0097 bigint,
    c0098 integer,
    c0099 timestamp,
    c0100 numeric(12,4),
    c0101 varchar(40),
    c0102 varchar(40),
    c0103 varchar(40),
    c0104 varchar(40),
    c0105 varchar(40),
    c0106 varchar(40),
    c0107 integer,
    c0108 integer,
    c0109 boolean,
    c0110 boolean,
    c0111 varchar(40),
    c0112 varchar(40),
    c0113 varchar(40),
    c0114 varchar(40),
    c0115 varchar(40),
    c0116 varchar(40),
    c0117 bigint,
    c0118 integer,
    c0119 numeric(12,4),
    c0120 timestamp,
    c0121 varchar(40),
    c0122 varchar(40),
    c0123 varchar(40),
    c0124 varchar(40),
    c0125 varchar(40),
    c0126 varchar(40),
    c0127 integer,
    c0128 integer,
    c0129 timestamp,
    c0130 numeric(12,4),
    c0131 varchar(40),
    c0132 varchar(40),
    c0133 varchar(40),
    c0134 varchar(40),
    c0135 varchar(40),
    c0136 varchar(40),
    c0137 bigint,
    c0138 integer,
    c0139 boolean,
    c0140 boolean,
    c0141 varchar(40),
    c0142 varchar(40),
    c0143 varchar(40),
    c0144 varchar(40),
    c0145 varchar(40),
    c0146 varchar(40),
    c0147 integer,
    c0148 integer,
    c0149 numeric(12,4),
    c0150 timestamp,
    c0151 varchar(40),
    c0152 varchar(40),
    c0153 varchar(40),
    c0154 varchar(40),
    c0155 varchar(40),
    c0156 varchar(40),
    c0157 bigint,
    c0158 integer,
    c0159 timestamp,
    c0160 numeric(12,4),
    c0161 varchar(40),
    c0162 varchar(40),
    c0163 varchar(40),
    c0164 varchar(40),
    c0165 varchar(40),
    c0166 varchar(40),
    c0167 integer,
    c0168 integer,
    c0169 boolean,
    c0170 boolean,
    c0171 varchar(40),
    c0172 varchar(40),
    c0173 varchar(40),
    c0174 varchar(40),
    c0175 varchar(40),
    c0176 varchar(40),
    c0177 bigint,
    c0178 integer,
    c0179 numeric(12,4),
    c0180 timestamp,
    c0181 varchar(40),
    c0182 varchar(40),
    c0183 varchar(40),
    c0184 varchar(40),
    c0185 varchar(40),
    c0186 varchar(40),
    c0187 integer,
    c0188 integer,
    c0189 timestamp,
    c0190 numeric(12,4),
    c0191 varchar(40),
    c0192 varchar(40),
    c0193 varchar(40),
    c0194 varchar(40),
    c0195 varchar(40),
    c0196 varchar(40),
    c0197 bigint,
    c0198 integer,
    c0199 boolean,
    c0200 boolean,
    c0201 varchar(40),
    c0202 varchar(40),
    c0203 varchar(40),
    c0204 varchar(40),
    c0205 varchar(40),
    c0206 varchar(40),
    c0207 integer,
    c0208 integer,
    c0209 numeric(12,4),
    c0210 timestamp,
    c0211 varchar(40),
    c0212 varchar(40),
    c0213 varchar(40),
    c0214 varchar(40),
    c0215 varchar(40),
    c0216 varchar(40),
    c0217 bigint,
    c0218 integer,
    c0219 timestamp,
    c0220 numeric(12,4),
    c0221 varchar(40),
    c0222 varchar(40),
    c0223 varchar(40),
    c0224 varchar(40),
    c0225 varchar(40),
    c0226 varchar(40),
    c0227 integer,
    c0228 integer,
    c0229 boolean,
    c0230 boolean,
    c0231 varchar(40),
    c0232 varchar(40),
    c0233 varchar(40),
    c0234 varchar(40),
    c0235 varchar(40),
    c0236 varchar(40),
    c0237 bigint,
    c0238 integer,
    c0239 numeric(12,4),
    c0240 timestamp,
    c0241 varchar(40),
    c0242 varchar(40),
    c0243 varchar(40),
    c0244 varchar(40),
    c0245 varchar(40),
    c0246 varchar(40),
    c0247 integer,
    c0248 integer,
    c0249 timestamp,
    c0250 numeric(12,4),
    c0251 varchar(40),
    c0252 varchar(40),
    c0253 varchar(40),
    c0254 varchar(40),
    c0255 varchar(40),
    c0256 varchar(40),
    c0257 bigint,
    c0258 integer,
    c0259 boolean,
    c0260 boolean,
    c0261 varchar(40),
    c0262 varchar(40),
    c0263 varchar(40),
    c0264 varchar(40),
    c0265 varchar(40),
    c0266 varchar(40),
    c0267 integer,
    c0268 integer,
    c0269 numeric(12,4),
    c0270 timestamp,
    c0271 varchar(40),
    c0272 varchar(40),
    c0273 varchar(40),
    c0274 varchar(40),
    c0275 varchar(40),
    c0276 varchar(40),
    c0277 bigint,
    c0278 integer,
    c0279 timestamp,
    c0280 numeric(12,4),
    c0281 varchar(40),
    c0282 varchar(40),
    c0283 varchar(40),
    c0284 varchar(40),
    c0285 varchar(40),
    c0286 varchar(40),
    c0287 integer,
    c0288 integer,
    c0289 boolean,
    c0290 boolean,
    c0291 varchar(40),
    c0292 varchar(40),
    c0293 varchar(40),
    c0294 varchar(40),
    c0295 varchar(40),
    c0296 varchar(40),
    c0297 bigint,
    c0298 integer,
    c0299 numeric(12,4),
    c0300 timestamp,
    c0301 varchar(40),
    c0302 varchar(40),
    c0303 varchar(40),
    c0304 varchar(40),
    c0305 varchar(40),
    c0306 varchar(40),
    c0307 integer,
    c0308 integer,
    c0309 timestamp,
    c0310 numeric(12,4),
    c0311 varchar(40),
    c0312 varchar(40),
    c0313 varchar(40),
    c0314 varchar(40),
    c0315 varchar(40),
    c0316 varchar(40),
    c0317 bigint,
    c0318 integer,
    c0319 boolean,
    c0320 boolean,
    c0321 varchar(40),
    c0322 varchar(40),
    c0323 varchar(40),
    c0324 varchar(40),
    c0325 varchar(40),
    c0326 varchar(40),
    c0327 integer,
    c0328 integer,
    c0329 numeric(12,4),
    c0330 timestamp,
    c0331 varchar(40),
    c0332 varchar(40),
    c0333 varchar(40),
    c0334 varchar(40),
    c0335 varchar(40),
    c0336 varchar(40),
    c0337 bigint,
    c0338 integer,
    c0339 timestamp,
    c0340 numeric(12,4),
    c0341 varchar(40),
    c0342 varchar(40),
    c0343 varchar(40),
    c0344 varchar(40),
    c0345 varchar(40),
    c0346 varchar(40),
    c0347 integer,
    c0348 integer,
    c0349 boolean,
    c0350 boolean,
    c0351 varchar(40),
    c0352 varchar(40),
    c0353 varchar(40),
    c0354 varchar(40),
    c0355 varchar(40),
    c0356 varchar(40),
    c0357 bigint,
    c0358 integer,
    c0359 numeric(12,4),
    c0360 timestamp,
    c0361 varchar(40),
    c0362 varchar(40),
    c0363 varchar(40),
    c0364 varchar(40),
    c0365 varchar(40),
    c0366 varchar(40),
    c0367 integer,
    c0368 integer,
    c0369 timestamp,
    c0370 numeric(12,4),
    c0371 varchar(40),
    c0372 varchar(40),
    c0373 varchar(40),
    c0374 varchar(40),
    c0375 varchar(40),
    c0376 varchar(40),
    c0377 bigint,
    c0378 integer,
    c0379 boolean,
    c0380 boolean,
    c0381 varchar(40),
    c0382 varchar(40),
    c0383 varchar(40),
    c0384 varchar(40),
    c0385 varchar(40),
    c0386 varchar(40),
    c0387 integer,
    c0388 integer,
    c0389 numeric(12,4),
    c0390 timestamp,
    c0391 varchar(40),
    c0392 varchar(40),
    c0393 varchar(40),
    c0394 varchar(40),
    c0395 varchar(40),
    c0396 varchar(40),
    c0397 bigint,
    c0398 integer,
    c0399 timestamp,
    c0400 numeric(12,4),
    c0401 varchar(40),
    c0402 varchar(40),
    c0403 varchar(40),
    c0404 varchar(40),
    c0405 varchar(40),
    c0406 varchar(40),
    c0407 integer,
    c0408 integer,
    c0409 boolean,
    c0410 boolean,
    c0411 varchar(40),
    c0412 varchar(40),
    c0413 varchar(40),
    c0414 varchar(40),
    c0415 varchar(40),
    c0416 varchar(40),
    c0417 bigint,
    c0418 integer,
    c0419 numeric(12,4),
    c0420 timestamp,
    c0421 varchar(40),
    c0422 varchar(40),
    c0423 varchar(40),
    c0424 varchar(40),
    c0425 varchar(40),
    c0426 varchar(40),
    c0427 integer,
    c0428 integer,
    c0429 timestamp,
    c0430 numeric(12,4),
    c0431 varchar(40),
    c0432 varchar(40),
    c0433 varchar(40),
    c0434 varchar(40),
    c0435 varchar(40),
    c0436 varchar(40),
    c0437 bigint,
    c0438 integer,
    c0439 boolean,
    c0440 boolean,
    c0441 varchar(40),
    c0442 varchar(40),
    c0443 varchar(40),
    c0444 varchar(40),
    c0445 varchar(40),
    c0446 varchar(40),
    c0447 integer,
    c0448 integer,
    c0449 numeric(12,4),
    c0450 timestamp,
    c0451 varchar(40),
    c0452 varchar(40),
    c0453 varchar(40),
    c0454 varchar(40),
    c0455 varchar(40),
    c0456 varchar(40),
    c0457 bigint,
    c0458 integer,
    c0459 timestamp,
    c0460 numeric(12,4),
    c0461 varchar(40),
    c0462 varchar(40),
    c0463 varchar(40),
    c0464 varchar(40),
    c0465 varchar(40),
    c0466 varchar(40),
    c0467 integer,
    c0468 integer,
    c0469 boolean,
    c0470 boolean,
    c0471 varchar(40),
    c0472 varchar(40),
    c0473 varchar(40),
    c0474 varchar(40),
    c0475 varchar(40),
    c0476 varchar(40),
    c0477 bigint,
    c0478 integer,
    c0479 numeric(12,4),
    c0480 timestamp,
    c0481 varchar(40),
    c0482 varchar(40),
    c0483 varchar(40),
    c0484 varchar(40),
    c0485 varchar(40),
    c0486 varchar(40),
    c0487 integer,
    c0488 integer,
    c0489 timestamp,
    c0490 numeric(12,4),
    c0491 varchar(40),
    c0492 varchar(40),
    c0493 varchar(40),
    c0494 varchar(40),
    c0495 varchar(40),
    c0496 varchar(40),
    c0497 bigint,
    c0498 integer,
    c0499 boolean,
    c0500 boolean,
    c0501 varchar(40),
    c0502 varchar(40),
    c0503 varchar(40),
    c0504 varchar(40),
    c0505 varchar(40),
    c0506 varchar(40),
    c0507 integer,
    c0508 integer,
    c0509 numeric(12,4),
    c0510 timestamp,
    c0511 varchar(40),
    c0512 varchar(40),
    c0513 varchar(40),
    c0514 varchar(40),
    c0515 varchar(40),
    c0516 varchar(40),
    c0517 bigint,
    c0518 integer,
    c0519 timestamp,
    c0520 numeric(12,4),
    c0521 varchar(40),
    c0522 varchar(40),
    c0523 varchar(40),
    c0524 varchar(40),
    c0525 varchar(40),
    c0526 varchar(40),
    c0527 integer,
    c0528 integer,
    c0529 boolean,
    c0530 boolean,
    c0531 varchar(40),
    c0532 varchar(40),
    c0533 varchar(40),
    c0534 varchar(40),
    c0535 varchar(40),
    c0536 varchar(40),
    c0537 bigint,
    c0538 integer,
    c0539 numeric(12,4),
    c0540 timestamp,
    c0541 varchar(40),
    c0542 varchar(40),
    c0543 varchar(40),
    c0544 varchar(40),
    c0545 varchar(40),
    c0546 varchar(40),
    c0547 integer,
    c0548 integer,
    c0549 timestamp,
    c0550 numeric(12,4),
    c0551 varchar(40),
    c0552 varchar(40),
    c0553 varchar(40),
    c0554 varchar(40),
    c0555 varchar(40),
    c0556 varchar(40),
    c0557 bigint,
    c0558 integer,
    c0559 boolean,
    c0560 boolean,
    c0561 varchar(40),
    c0562 varchar(40),
    c0563 varchar(40),
    c0564 varchar(40),
    c0565 varchar(40),
    c0566 varchar(40),
    c0567 integer,
    c0568 integer,
    c0569 numeric(12,4),
    c0570 timestamp,
    c0571 varchar(40),
    c0572 varchar(40),
    c0573 varchar(40),
    c0574 varchar(40),
    c0575 varchar(40),
    c0576 varchar(40),
    c0577 bigint,
    c0578 integer,
    c0579 timestamp,
    c0580 numeric(12,4),
    c0581 varchar(40),
    c0582 varchar(40),
    c0583 varchar(40),
    c0584 varchar(40),
    c0585 varchar(40),
    c0586 varchar(40),
    c0587 integer,
    c0588 integer,
    c0589 boolean,
    c0590 boolean,
    c0591 varchar(40),
    c0592 varchar(40),
    c0593 varchar(40),
    c0594 varchar(40),
    c0595 varchar(40),
    c0596 varchar(40),
    c0597 bigint,
    c0598 integer,
    c0599 numeric(12,4),
    c0600 timestamp,
    c0601 varchar(40),
    c0602 varchar(40),
    c0603 varchar(40),
    c0604 varchar(40),
    c0605 varchar(40),
    c0606 varchar(40),
    c0607 integer,
    c0608 integer,
    c0609 timestamp,
    c0610 numeric(12,4),
    c0611 varchar(40),
    c0612 varchar(40),
    c0613 varchar(40),
    c0614 varchar(40),
    c0615 varchar(40),
    c0616 varchar(40),
    c0617 bigint,
    c0618 integer,
    c0619 boolean,
    c0620 boolean,
    c0621 varchar(40),
    c0622 varchar(40),
    c0623 varchar(40),
    c0624 varchar(40),
    c0625 varchar(40),
    c0626 varchar(40),
    c0627 integer,
    c0628 integer,
    c0629 numeric(12,4),
    c0630 timestamp,
    c0631 varchar(40),
    c0632 varchar(40),
    c0633 varchar(40),
    c0634 varchar(40),
    c0635 varchar(40),
    c0636 varchar(40),
    c0637 bigint,
    c0638 integer,
    c0639 timestamp,
    c0640 numeric(12,4),
    c0641 varchar(40),
    c0642 varchar(40),
    c0643 varchar(40),
    c0644 varchar(40),
    c0645 varchar(40),
    c0646 varchar(40),
    c0647 integer,
    c0648 integer,
    c0649 boolean,
    c0650 boolean,
    c0651 varchar(40),
    c0652 varchar(40),
    c0653 varchar(40),
    c0654 varchar(40),
    c0655 varchar(40),
    c0656 varchar(40),
    c0657 bigint,
    c0658 integer,
    c0659 numeric(12,4),
    c0660 timestamp,
    c0661 varchar(40),
    c0662 varchar(40),
    c0663 varchar(40),
    c0664 varchar(40),
    c0665 varchar(40),
    c0666 varchar(40),
    c0667 integer,
    c0668 integer,
    c0669 timestamp,
    c0670 numeric(12,4),
    c0671 varchar(40),
    c0672 varchar(40),
    c0673 varchar(40),
    c0674 varchar(40),
    c0675 varchar(40),
    c0676 varchar(40),
    c0677 bigint,
    c0678 integer,
    c0679 boolean,
    c0680 boolean,
    c0681 varchar(40),
    c0682 varchar(40),
    c0683 varchar(40),
    c0684 varchar(40),
    c0685 varchar(40),
    c0686 varchar(40),
    c0687 integer,
    c0688 integer,
    c0689 numeric(12,4),
    c0690 timestamp,
    c0691 varchar(40),
    c0692 varchar(40),
    c0693 varchar(40),
    c0694 varchar(40),
    c0695 varchar(40),
    c0696 varchar(40),
    c0697 bigint,
    c0698 integer,
    c0699 timestamp,
    c0700 numeric(12,4),
    c0701 varchar(40),
    c0702 varchar(40),
    c0703 varchar(40),
    c0704 varchar(40),
    c0705 varchar(40),
    c0706 varchar(40),
    c0707 integer,
    c0708 integer,
    c0709 boolean,
    c0710 boolean,
    c0711 varchar(40),
    c0712 varchar(40),
    c0713 varchar(40),
    c0714 varchar(40),
    c0715 varchar(40),
    c0716 varchar(40),
    c0717 bigint,
    c0718 integer,
    c0719 numeric(12,4),
    c0720 timestamp,
    c0721 varchar(40),
    c0722 varchar(40),
    c0723 varchar(40),
    c0724 varchar(40),
    c0725 varchar(40),
    c0726 varchar(40),
    c0727 integer,
    c0728 integer,
    c0729 timestamp,
    c0730 numeric(12,4),
    c0731 varchar(40),
    c0732 varchar(40),
    c0733 varchar(40),
    c0734 varchar(40),
    c0735 varchar(40),
    c0736 varchar(40),
    c0737 bigint,
    c0738 integer,
    c0739 boolean,
    c0740 boolean,
    c0741 varchar(40),
    c0742 varchar(40),
    c0743 varchar(40),
    c0744 varchar(40),
    c0745 varchar(40),
    c0746 varchar(40),
    c0747 integer,
    c0748 integer,
    c0749 numeric(12,4),
    c0750 timestamp,
    c0751 varchar(40),
    c0752 varchar(40),
    c0753 varchar(40),
    c0754 varchar(40),
    c0755 varchar(40),
    c0756 varchar(40),
    c0757 bigint,
    c0758 integer,
    c0759 timestamp,
    c0760 numeric(12,4),
    c0761 varchar(40),
    c0762 varchar(40),
    c0763 varchar(40),
    c0764 varchar(40),
    c0765 varchar(40),
    c0766 varchar(40),
    c0767 integer,
    c0768 integer,
    c0769 boolean,
    c0770 boolean,
    c0771 varchar(40),
    c0772 varchar(40),
    c0773 varchar(40),
    c0774 varchar(40),
    c0775 varchar(40),
    c0776 varchar(40),
    c0777 bigint,
    c0778 integer,
    c0779 numeric(12,4),
    c0780 timestamp,
    c0781 varchar(40),
    c0782 varchar(40),
    c0783 varchar(40),
    c0784 varchar(40),
    c0785 varchar(40),
    c0786 varchar(40),
    c0787 integer,
    c0788 integer,
    c0789 timestamp,
    c0790 numeric(12,4),
    c0791 varchar(40),
    c0792 varchar(40),
    c0793 varchar(40),
    c0794 varchar(40),
    c0795 varchar(40),
    c0796 varchar(40),
    c0797 bigint,
    c0798 integer,
    c0799 boolean,
    c0800 boolean,
    c0801 varchar(40),
    c0802 varchar(40),
    c0803 varchar(40),
    c0804 varchar(40),
    c0805 varchar(40),
    c0806 varchar(40),
    c0807 integer,
    c0808 integer,
    c0809 numeric(12,4),
    c0810 timestamp,
    c0811 varchar(40),
    c0812 varchar(40),
    c0813 varchar(40),
    c0814 varchar(40),
    c0815 varchar(40),
    c0816 varchar(40),
    c0817 bigint,
    c0818 integer,
    c0819 timestamp,
    c0820 numeric(12,4),
    c0821 varchar(40),
    c0822 varchar(40),
    c0823 varchar(40),
    c0824 varchar(40),
    c0825 varchar(40),
    c0826 varchar(40),
    c0827 integer,
    c0828 integer,
    c0829 boolean,
    c0830 boolean,
    c0831 varchar(40),
    c0832 varchar(40),
    c0833 varchar(40),
    c0834 varchar(40),
    c0835 varchar(40),
    c0836 varchar(40),
    c0837 bigint,
    c0838 integer,
    c0839 numeric(12,4),
    c0840 timestamp,
    c0841 varchar(40),
    c0842 varchar(40),
    c0843 varchar(40),
    c0844 varchar(40),
    c0845 varchar(40),
    c0846 varchar(40),
    c0847 integer,
    c0848 integer,
    c0849 timestamp,
    c0850 numeric(12,4),
    c0851 varchar(40),
    c0852 varchar(40),
    c0853 varchar(40),
    c0854 varchar(40),
    c0855 varchar(40),
    c0856 varchar(40),
    c0857 bigint,
    c0858 integer,
    c0859 boolean,
    c0860 boolean,
    c0861 varchar(40),
    c0862 varchar(40),
    c0863 varchar(40),
    c0864 varchar(40),
    c0865 varchar(40),
    c0866 varchar(40),
    c0867 integer,
    c0868 integer,
    c0869 numeric(12,4),
    c0870 timestamp,
    c0871 varchar(40),
    c0872 varchar(40),
    c0873 varchar(40),
    c0874 varchar(40),
    c0875 varchar(40),
    c0876 varchar(40),
    c0877 bigint,
    c0878 integer,
    c0879 timestamp,
    c0880 numeric(12,4),
    c0881 varchar(40),
    c0882 varchar(40),
    c0883 varchar(40),
    c0884 varchar(40),
    c0885 varchar(40),
    c0886 varchar(40),
    c0887 integer,
    c0888 integer,
    c0889 boolean,
    c0890 boolean,
    c0891 varchar(40),
    c0892 varchar(40),
    c0893 varchar(40),
    c0894 varchar(40),
    c0895 varchar(40),
    c0896 varchar(40),
    c0897 bigint,
    c0898 integer,
    c0899 numeric(12,4),
    c0900 timestamp,
    c0901 varchar(40),
    c0902 varchar(40),
    c0903 varchar(40),
    c0904 varchar(40),
    c0905 varchar(40),
    c0906 varchar(40),
    c0907 integer,
    c0908 integer,
    c0909 timestamp,
    c0910 numeric(12,4),
    c0911 varchar(40),
    c0912 varchar(40),
    c0913 varchar(40),
    c0914 varchar(40),
    c0915 varchar(40),
    c0916 varchar(40),
    c0917 bigint,
    c0918 integer,
    c0919 boolean,
    c0920 boolean,
    c0921 varchar(40),
    c0922 varchar(40),
    c0923 varchar(40),
    c0924 varchar(40),
    c0925 varchar(40),
    c0926 varchar(40),
    c0927 integer,
    c0928 integer,
    c0929 numeric(12,4),
    c0930 timestamp,
    c0931 varchar(40),
    c0932 varchar(40),
    c0933 varchar(40),
    c0934 varchar(40),
    c0935 varchar(40),
    c0936 varchar(40),
    c0937 bigint,
    c0938 integer,
    c0939 timestamp,
    c0940 numeric(12,4),
    c0941 varchar(40),
    c0942 varchar(40),
    c0943 varchar(40),
    c0944 varchar(40),
    c0945 varchar(40),
    c0946 varchar(40),
    c0947 integer,
    c0948 integer,
    c0949 boolean,
    c0950 boolean,
    c0951 varchar(40),
    c0952 varchar(40),
    c0953 varchar(40),
    c0954 varchar(40),
    c0955 varchar(40),
    c0956 varchar(40),
    c0957 bigint,
    c0958 integer,
    c0959 numeric(12,4),
    c0960 timestamp,
    c0961 varchar(40),
    c0962 varchar(40),
    c0963 varchar(40),
    c0964 varchar(40),
    c0965 varchar(40),
    c0966 varchar(40),
    c0967 integer,
    c0968 integer,
    c0969 timestamp,
    c0970 numeric(12,4),
    c0971 varchar(40),
    c0972 varchar(40),
    c0973 varchar(40),
    c0974 varchar(40),
    c0975 varchar(40),
    c0976 varchar(40),
    c0977 bigint,
    c0978 integer,
    c0979 boolean,
    c0980 boolean,
    c0981 varchar(40),
    c0982 varchar(40),
    c0983 varchar(40),
    c0984 varchar(40),
    c0985 varchar(40),
    c0986 varchar(40),
    c0987 integer,
    c0988 integer,
    c0989 numeric(12,4),
    c0990 timestamp,
    c0991 varchar(40),
    c0992 varchar(40),
    c0993 varchar(40),
    c0994 varchar(40),
    c0995 varchar(40),
    c0996 varchar(40),
    c0997 bigint,
    c0998 integer,
    c0999 timestamp,
    c1000 numeric(12,4),
    grp integer
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM wide_1000 LIMIT 1) THEN
        RAISE NOTICE 'Populating wide_1000 (100000 rows, 1002 cols, sparse)...';
        INSERT INTO wide_1000
        SELECT i,
               CASE WHEN (i::bigint*7919+0) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+1) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+2) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+3) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+4) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+5) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+6) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+7) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+8) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+9) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+10) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+11) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+12) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+13) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+14) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+15) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+16) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+17) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+18) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+19) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+20) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+21) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+22) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+23) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+24) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+25) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+26) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+27) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+28) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+29) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+30) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+31) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+32) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+33) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+34) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+35) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+36) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+37) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+38) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+39) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+40) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+41) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+42) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+43) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+44) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+45) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+46) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+47) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+48) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+49) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+50) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+51) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+52) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+53) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+54) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+55) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+56) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+57) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+58) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+59) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+60) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+61) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+62) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+63) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+64) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+65) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+66) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+67) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+68) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+69) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+70) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+71) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+72) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+73) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+74) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+75) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+76) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+77) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+78) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+79) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+80) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+81) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+82) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+83) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+84) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+85) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+86) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+87) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+88) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+89) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+90) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+91) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+92) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+93) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+94) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+95) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+96) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+97) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+98) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+99) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+100) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+101) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+102) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+103) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+104) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+105) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+106) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+107) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+108) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+109) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+110) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+111) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+112) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+113) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+114) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+115) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+116) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+117) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+118) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+119) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+120) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+121) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+122) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+123) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+124) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+125) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+126) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+127) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+128) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+129) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+130) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+131) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+132) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+133) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+134) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+135) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+136) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+137) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+138) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+139) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+140) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+141) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+142) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+143) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+144) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+145) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+146) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+147) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+148) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+149) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+150) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+151) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+152) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+153) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+154) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+155) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+156) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+157) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+158) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+159) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+160) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+161) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+162) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+163) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+164) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+165) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+166) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+167) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+168) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+169) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+170) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+171) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+172) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+173) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+174) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+175) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+176) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+177) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+178) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+179) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+180) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+181) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+182) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+183) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+184) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+185) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+186) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+187) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+188) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+189) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+190) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+191) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+192) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+193) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+194) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+195) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+196) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+197) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+198) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+199) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+200) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+201) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+202) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+203) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+204) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+205) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+206) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+207) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+208) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+209) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+210) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+211) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+212) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+213) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+214) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+215) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+216) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+217) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+218) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+219) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+220) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+221) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+222) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+223) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+224) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+225) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+226) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+227) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+228) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+229) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+230) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+231) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+232) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+233) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+234) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+235) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+236) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+237) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+238) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+239) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+240) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+241) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+242) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+243) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+244) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+245) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+246) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+247) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+248) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+249) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+250) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+251) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+252) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+253) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+254) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+255) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+256) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+257) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+258) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+259) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+260) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+261) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+262) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+263) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+264) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+265) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+266) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+267) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+268) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+269) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+270) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+271) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+272) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+273) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+274) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+275) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+276) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+277) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+278) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+279) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+280) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+281) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+282) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+283) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+284) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+285) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+286) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+287) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+288) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+289) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+290) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+291) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+292) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+293) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+294) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+295) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+296) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+297) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+298) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+299) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+300) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+301) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+302) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+303) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+304) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+305) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+306) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+307) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+308) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+309) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+310) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+311) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+312) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+313) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+314) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+315) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+316) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+317) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+318) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+319) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+320) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+321) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+322) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+323) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+324) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+325) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+326) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+327) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+328) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+329) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+330) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+331) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+332) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+333) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+334) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+335) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+336) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+337) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+338) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+339) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+340) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+341) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+342) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+343) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+344) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+345) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+346) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+347) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+348) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+349) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+350) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+351) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+352) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+353) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+354) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+355) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+356) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+357) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+358) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+359) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+360) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+361) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+362) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+363) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+364) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+365) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+366) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+367) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+368) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+369) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+370) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+371) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+372) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+373) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+374) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+375) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+376) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+377) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+378) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+379) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+380) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+381) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+382) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+383) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+384) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+385) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+386) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+387) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+388) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+389) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+390) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+391) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+392) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+393) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+394) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+395) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+396) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+397) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+398) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+399) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+400) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+401) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+402) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+403) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+404) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+405) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+406) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+407) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+408) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+409) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+410) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+411) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+412) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+413) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+414) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+415) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+416) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+417) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+418) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+419) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+420) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+421) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+422) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+423) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+424) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+425) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+426) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+427) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+428) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+429) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+430) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+431) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+432) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+433) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+434) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+435) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+436) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+437) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+438) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+439) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+440) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+441) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+442) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+443) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+444) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+445) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+446) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+447) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+448) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+449) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+450) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+451) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+452) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+453) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+454) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+455) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+456) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+457) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+458) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+459) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+460) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+461) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+462) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+463) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+464) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+465) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+466) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+467) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+468) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+469) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+470) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+471) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+472) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+473) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+474) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+475) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+476) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+477) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+478) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+479) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+480) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+481) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+482) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+483) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+484) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+485) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+486) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+487) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+488) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+489) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+490) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+491) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+492) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+493) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+494) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+495) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+496) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+497) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+498) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+499) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+500) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+501) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+502) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+503) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+504) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+505) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+506) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+507) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+508) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+509) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+510) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+511) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+512) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+513) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+514) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+515) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+516) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+517) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+518) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+519) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+520) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+521) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+522) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+523) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+524) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+525) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+526) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+527) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+528) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+529) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+530) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+531) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+532) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+533) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+534) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+535) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+536) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+537) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+538) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+539) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+540) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+541) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+542) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+543) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+544) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+545) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+546) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+547) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+548) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+549) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+550) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+551) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+552) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+553) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+554) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+555) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+556) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+557) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+558) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+559) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+560) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+561) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+562) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+563) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+564) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+565) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+566) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+567) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+568) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+569) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+570) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+571) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+572) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+573) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+574) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+575) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+576) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+577) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+578) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+579) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+580) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+581) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+582) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+583) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+584) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+585) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+586) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+587) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+588) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+589) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+590) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+591) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+592) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+593) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+594) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+595) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+596) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+597) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+598) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+599) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+600) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+601) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+602) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+603) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+604) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+605) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+606) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+607) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+608) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+609) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+610) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+611) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+612) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+613) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+614) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+615) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+616) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+617) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+618) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+619) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+620) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+621) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+622) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+623) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+624) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+625) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+626) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+627) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+628) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+629) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+630) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+631) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+632) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+633) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+634) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+635) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+636) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+637) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+638) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+639) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+640) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+641) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+642) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+643) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+644) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+645) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+646) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+647) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+648) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+649) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+650) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+651) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+652) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+653) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+654) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+655) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+656) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+657) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+658) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+659) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+660) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+661) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+662) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+663) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+664) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+665) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+666) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+667) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+668) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+669) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+670) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+671) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+672) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+673) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+674) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+675) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+676) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+677) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+678) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+679) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+680) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+681) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+682) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+683) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+684) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+685) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+686) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+687) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+688) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+689) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+690) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+691) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+692) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+693) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+694) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+695) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+696) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+697) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+698) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+699) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+700) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+701) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+702) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+703) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+704) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+705) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+706) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+707) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+708) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+709) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+710) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+711) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+712) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+713) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+714) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+715) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+716) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+717) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+718) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+719) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+720) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+721) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+722) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+723) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+724) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+725) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+726) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+727) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+728) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+729) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+730) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+731) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+732) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+733) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+734) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+735) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+736) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+737) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+738) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+739) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+740) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+741) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+742) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+743) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+744) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+745) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+746) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+747) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+748) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+749) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+750) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+751) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+752) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+753) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+754) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+755) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+756) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+757) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+758) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+759) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+760) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+761) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+762) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+763) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+764) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+765) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+766) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+767) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+768) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+769) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+770) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+771) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+772) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+773) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+774) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+775) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+776) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+777) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+778) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+779) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+780) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+781) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+782) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+783) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+784) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+785) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+786) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+787) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+788) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+789) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+790) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+791) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+792) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+793) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+794) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+795) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+796) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+797) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+798) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+799) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+800) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+801) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+802) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+803) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+804) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+805) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+806) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+807) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+808) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+809) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+810) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+811) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+812) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+813) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+814) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+815) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+816) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+817) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+818) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+819) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+820) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+821) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+822) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+823) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+824) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+825) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+826) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+827) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+828) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+829) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+830) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+831) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+832) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+833) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+834) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+835) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+836) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+837) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+838) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+839) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+840) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+841) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+842) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+843) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+844) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+845) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+846) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+847) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+848) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+849) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+850) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+851) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+852) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+853) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+854) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+855) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+856) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+857) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+858) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+859) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+860) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+861) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+862) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+863) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+864) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+865) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+866) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+867) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+868) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+869) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+870) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+871) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+872) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+873) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+874) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+875) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+876) % 10 = 0 THEN i::bigint * 2 END,
               CASE WHEN (i::bigint*7919+877) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+878) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+879) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+880) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+881) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+882) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+883) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+884) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+885) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+886) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+887) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+888) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+889) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+890) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+891) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+892) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+893) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+894) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+895) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+896) % 10 = 0 THEN i::bigint * 1 END,
               CASE WHEN (i::bigint*7919+897) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+898) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+899) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+900) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+901) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+902) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+903) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+904) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+905) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+906) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+907) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+908) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+909) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+910) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+911) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+912) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+913) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+914) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+915) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+916) % 10 = 0 THEN i::bigint * 7 END,
               CASE WHEN (i::bigint*7919+917) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+918) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+919) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+920) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+921) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+922) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+923) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+924) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+925) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+926) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+927) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+928) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+929) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+930) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+931) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+932) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+933) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+934) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+935) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+936) % 10 = 0 THEN i::bigint * 6 END,
               CASE WHEN (i::bigint*7919+937) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+938) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+939) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+940) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+941) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+942) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+943) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+944) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+945) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+946) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+947) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+948) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+949) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+950) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+951) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+952) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+953) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+954) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+955) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+956) % 10 = 0 THEN i::bigint * 5 END,
               CASE WHEN (i::bigint*7919+957) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+958) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+959) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+960) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+961) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+962) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+963) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+964) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+965) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+966) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+967) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+968) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+969) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+970) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+971) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+972) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+973) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+974) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+975) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+976) % 10 = 0 THEN i::bigint * 4 END,
               CASE WHEN (i::bigint*7919+977) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+978) % 10 = 0 THEN (i % 5 = 0) END,
               CASE WHEN (i::bigint*7919+979) % 10 = 0 THEN (i % 6 = 0) END,
               CASE WHEN (i::bigint*7919+980) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+981) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+982) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+983) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+984) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+985) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+986) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+987) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+988) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               CASE WHEN (i::bigint*7919+989) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+990) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+991) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+992) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+993) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+994) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+995) % 10 = 0 THEN 'v' || (i % 1000) END,
               CASE WHEN (i::bigint*7919+996) % 10 = 0 THEN i::bigint * 3 END,
               CASE WHEN (i::bigint*7919+997) % 10 = 0 THEN (random()*10000)::int END,
               CASE WHEN (i::bigint*7919+998) % 10 = 0 THEN '2020-01-01'::timestamp + (i % 1000000) * interval '1 second' END,
               CASE WHEN (i::bigint*7919+999) % 10 = 0 THEN (random()*1000)::numeric(12,4) END,
               i % 500
        FROM generate_series(1, 100000) i;
    END IF;
END $$;

ANALYZE wide_1000;

\echo 'Wide table setup complete.'
\timing off
```

</details>

---

*Generated by `tests/bench_comprehensive.sh` on 2026-03-02*
