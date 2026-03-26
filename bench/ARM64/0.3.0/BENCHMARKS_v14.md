# pg_jitter Benchmark Results

Comprehensive benchmark comparing pg_jitter JIT backends against PostgreSQL's interpreter and LLVM JIT.

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | PostgreSQL 14.20 |
| OS | Darwin 25.2.0 arm64 |
| CPU | Apple M1 Pro |
| RAM | 32 GB |
| Backends tested | interp llvmjit sljit asmjit mir |
| Runs per query | 3 (median) |
| Warmup runs | 2 |
| Parallel workers | 0 (disabled) |
| Date | 2026-03-24 |

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
- Speedup shown as Nx relative to interpreter: >1x = faster, <1x = slower

## Results

### Basic Aggregation

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| SUM_int | 44.011 ms | 65.815 ms (0.67x) | 40.170 ms (1.10x) | 40.036 ms (1.10x) | 46.408 ms (0.95x) |
| COUNT_star | 34.047 ms | 56.104 ms (0.61x) | 35.684 ms (0.95x) | 35.169 ms (0.97x) | 35.084 ms (0.97x) |
| GroupBy_5agg | 119.702 ms | 155.325 ms (0.77x) | 101.247 ms (1.18x) | 99.061 ms (1.21x) | 127.379 ms (0.94x) |
| GroupBy_100K_grp | 171.645 ms | 210.498 ms (0.82x) | 152.668 ms (1.12x) | 147.689 ms (1.16x) | 166.028 ms (1.03x) |
| COUNT_DISTINCT | 104.392 ms | 128.409 ms (0.81x) | 102.118 ms (1.02x) | 100.593 ms (1.04x) | 109.933 ms (0.95x) |

### Hash Joins

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| HashJoin_single | 501.583 ms | 503.809 ms (1.00x) | 479.352 ms (1.05x) | 388.358 ms (1.29x) | 460.975 ms (1.09x) |
| HashJoin_composite | 504.498 ms | 450.985 ms (1.12x) | 451.372 ms (1.12x) | 407.824 ms (1.24x) | 453.191 ms (1.11x) |
| HashJoin_filter | 366.620 ms | 369.746 ms (0.99x) | 354.432 ms (1.03x) | 323.447 ms (1.13x) | 449.249 ms (0.82x) |
| HashJoin_GroupBy | 796.655 ms | 646.938 ms (1.23x) | 671.104 ms (1.19x) | 676.042 ms (1.18x) | 737.804 ms (1.08x) |

### Outer Joins

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| LeftJoin | 179.973 ms | 178.070 ms (1.01x) | 147.777 ms (1.22x) | 142.237 ms (1.27x) | 156.742 ms (1.15x) |
| RightJoin | 171.376 ms | 189.966 ms (0.90x) | 159.020 ms (1.08x) | 150.310 ms (1.14x) | 165.268 ms (1.04x) |
| FullOuterJoin | 77.584 ms | 112.560 ms (0.69x) | 70.472 ms (1.10x) | 69.318 ms (1.12x) | 76.412 ms (1.02x) |

### Semi/Anti Joins

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| EXISTS_semi | 191.179 ms | 217.517 ms (0.88x) | 175.727 ms (1.09x) | 187.696 ms (1.02x) | 217.124 ms (0.88x) |
| NOT_EXISTS_anti | 143.713 ms | 169.991 ms (0.85x) | 139.870 ms (1.03x) | 136.131 ms (1.06x) | 138.858 ms (1.03x) |
| IN_subquery | 143.286 ms | 188.640 ms (0.76x) | 142.326 ms (1.01x) | 139.939 ms (1.02x) | 151.654 ms (0.94x) |

### Set Operations

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| INTERSECT | 200.439 ms | 217.735 ms (0.92x) | 235.981 ms (0.85x) | 247.416 ms (0.81x) | 214.794 ms (0.93x) |
| EXCEPT | 208.867 ms | 218.623 ms (0.96x) | 212.683 ms (0.98x) | 224.751 ms (0.93x) | 217.925 ms (0.96x) |
| UNION_ALL_agg | 105.287 ms | 108.300 ms (0.97x) | 85.481 ms (1.23x) | 84.006 ms (1.25x) | 85.250 ms (1.24x) |

### Expressions

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| CASE_simple | 54.626 ms | 74.985 ms (0.73x) | 50.341 ms (1.09x) | 49.364 ms (1.11x) | 54.633 ms (1.00x) |
| CASE_searched_4way | 61.029 ms | 77.208 ms (0.79x) | 52.940 ms (1.15x) | 51.715 ms (1.18x) | 59.663 ms (1.02x) |
| COALESCE_NULLIF | 49.206 ms | 68.468 ms (0.72x) | 44.746 ms (1.10x) | 42.631 ms (1.15x) | 50.966 ms (0.97x) |
| Bool_AND_OR | 58.922 ms | 77.284 ms (0.76x) | 52.305 ms (1.13x) | 51.910 ms (1.14x) | 58.098 ms (1.01x) |
| Arith_expr | 57.345 ms | 72.978 ms (0.79x) | 47.380 ms (1.21x) | 45.087 ms (1.27x) | 52.723 ms (1.09x) |

### Subqueries

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Scalar_subq | 103.566 ms | 130.095 ms (0.80x) | 93.220 ms (1.11x) | 89.355 ms (1.16x) | 104.885 ms (0.99x) |
| Correlated_subq | 180.328 ms | 201.906 ms (0.89x) | 166.482 ms (1.08x) | 164.016 ms (1.10x) | 178.961 ms (1.01x) |
| LATERAL_top3 | 48.995 ms | 63.659 ms (0.77x) | 37.900 ms (1.29x) | 37.039 ms (1.32x) | 43.019 ms (1.14x) |

### Date/Timestamp

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Date_extract | 148.490 ms | 163.004 ms (0.91x) | 134.801 ms (1.10x) | 132.440 ms (1.12x) | 153.775 ms (0.97x) |
| Timestamp_trunc | 139.054 ms | 161.761 ms (0.86x) | 132.523 ms (1.05x) | 139.677 ms (1.00x) | 145.506 ms (0.96x) |
| Interval_arith | 95.917 ms | 132.305 ms (0.72x) | 35.079 ms (2.73x) | 39.378 ms (2.44x) | 38.192 ms (2.51x) |
| Timestamp_diff | 76.239 ms | 99.400 ms (0.77x) | 72.376 ms (1.05x) | 71.109 ms (1.07x) | 80.225 ms (0.95x) |

### Text/String

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Text_EQ_filter | 26.122 ms | 56.027 ms (0.47x) | 25.949 ms (1.01x) | 26.171 ms (1.00x) | 28.072 ms (0.93x) |
| Text_LIKE | 32.921 ms | 65.004 ms (0.51x) | 28.151 ms (1.17x) | 30.390 ms (1.08x) | 30.111 ms (1.09x) |
| Text_concat_agg | 22.301 ms | 66.309 ms (0.34x) | 22.766 ms (0.98x) | 22.208 ms (1.00x) | 24.673 ms (0.90x) |
| Text_length_expr | 59.112 ms | 97.802 ms (0.60x) | 56.660 ms (1.04x) | 56.393 ms (1.05x) | 58.142 ms (1.02x) |
| Text_EQ_c_coll | 25.405 ms | 60.917 ms (0.42x) | 25.988 ms (0.98x) | 25.755 ms (0.99x) | 27.912 ms (0.91x) |
| Text_NE_c_coll | 32.556 ms | 63.987 ms (0.51x) | 33.261 ms (0.98x) | 32.335 ms (1.01x) | 35.903 ms (0.91x) |
| Text_LT_c_coll | 28.804 ms | 63.985 ms (0.45x) | 28.279 ms (1.02x) | 28.116 ms (1.02x) | 30.252 ms (0.95x) |
| Text_sort_c | 45.394 ms | 70.910 ms (0.64x) | 46.110 ms (0.98x) | 46.367 ms (0.98x) | 48.141 ms (0.94x) |

### LIKE (StringZilla)

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| LIKE_prefix | 27.676 ms | 58.719 ms (0.47x) | 23.442 ms (1.18x) | 25.928 ms (1.07x) | 26.820 ms (1.03x) |
| LIKE_suffix | 50.293 ms | 82.998 ms (0.61x) | 25.314 ms (1.99x) | 26.374 ms (1.91x) | 27.030 ms (1.86x) |
| LIKE_interior | 50.391 ms | 83.967 ms (0.60x) | 30.670 ms (1.64x) | 31.855 ms (1.58x) | 31.772 ms (1.59x) |
| LIKE_exact | 30.621 ms | 59.975 ms (0.51x) | 24.586 ms (1.25x) | 24.824 ms (1.23x) | 26.260 ms (1.17x) |
| NOT_LIKE_prefix | 34.227 ms | 64.110 ms (0.53x) | 30.199 ms (1.13x) | 32.257 ms (1.06x) | 33.482 ms (1.02x) |
| NOT_LIKE_interior | 57.490 ms | 92.990 ms (0.62x) | 37.141 ms (1.55x) | 37.658 ms (1.53x) | 37.509 ms (1.53x) |

### LIKE (underscore)

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| LIKE_under_prefix | 27.592 ms | 62.761 ms (0.44x) | 25.126 ms (1.10x) | 26.399 ms (1.05x) | 26.928 ms (1.02x) |
| LIKE_under_mid | 50.950 ms | 83.707 ms (0.61x) | 45.801 ms (1.11x) | 49.070 ms (1.04x) | 48.680 ms (1.05x) |
| LIKE_under_only | 35.962 ms | 64.529 ms (0.56x) | 31.222 ms (1.15x) | 31.945 ms (1.13x) | 33.482 ms (1.07x) |
| LIKE_pct_under_mix | 29.181 ms | 60.433 ms (0.48x) | 32.772 ms (0.89x) | 35.253 ms (0.83x) | 35.026 ms (0.83x) |

### ILIKE/Regex (PCRE2)

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| LIKE_multi | 48.314 ms | 79.278 ms (0.61x) | 48.917 ms (0.99x) | 50.560 ms (0.96x) | 51.340 ms (0.94x) |
| LIKE_3wild | 50.541 ms | 82.699 ms (0.61x) | 64.153 ms (0.79x) | 65.949 ms (0.77x) | 66.262 ms (0.76x) |
| ILIKE_prefix | 143.133 ms | 170.259 ms (0.84x) | 25.507 ms (5.61x) | 26.518 ms (5.40x) | 27.256 ms (5.25x) |
| ILIKE_interior | 164.873 ms | 189.434 ms (0.87x) | 29.578 ms (5.57x) | 31.905 ms (5.17x) | 31.994 ms (5.15x) |
| ILIKE_suffix | 161.205 ms | 187.931 ms (0.86x) | 50.051 ms (3.22x) | 50.307 ms (3.20x) | 50.591 ms (3.19x) |
| ILIKE_exact | 114.281 ms | 138.426 ms (0.83x) | 25.033 ms (4.57x) | 25.173 ms (4.54x) | 26.614 ms (4.29x) |
| NOT_ILIKE_int | 172.264 ms | 197.820 ms (0.87x) | 36.544 ms (4.71x) | 38.486 ms (4.48x) | 38.700 ms (4.45x) |
| Regex_anchor | 124.286 ms | 155.869 ms (0.80x) | 32.928 ms (3.77x) | 34.851 ms (3.57x) | 35.867 ms (3.47x) |
| Regex_mid | 266.576 ms | 299.203 ms (0.89x) | 90.660 ms (2.94x) | 89.807 ms (2.97x) | 90.646 ms (2.94x) |
| Regex_neg | 84.057 ms | 117.143 ms (0.72x) | 31.172 ms (2.70x) | 32.583 ms (2.58x) | 32.686 ms (2.57x) |
| Regex_icase | 70.250 ms | 103.464 ms (0.68x) | 25.256 ms (2.78x) | 26.519 ms (2.65x) | 27.058 ms (2.60x) |
| Regex_neg_icase | 76.373 ms | 109.224 ms (0.70x) | 31.601 ms (2.42x) | 33.225 ms (2.30x) | 34.049 ms (2.24x) |
| Regex_charclass | 218.686 ms | 252.950 ms (0.86x) | 86.762 ms (2.52x) | 88.165 ms (2.48x) | 89.796 ms (2.44x) |
| Regex_alternation | 75.429 ms | 108.968 ms (0.69x) | 27.085 ms (2.78x) | 28.294 ms (2.67x) | 29.321 ms (2.57x) |

### IN list / Sort

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| IN_list_20 | 60.887 ms | 92.737 ms (0.66x) | 38.127 ms (1.60x) | 36.730 ms (1.66x) | 40.992 ms (1.49x) |
| IN_list_small | 58.357 ms | 85.432 ms (0.68x) | 36.592 ms (1.59x) | 34.541 ms (1.69x) | 37.895 ms (1.54x) |
| IN_text_5 | 59.532 ms | 86.066 ms (0.69x) | 59.663 ms (1.00x) | 59.933 ms (0.99x) | 61.564 ms (0.97x) |
| IN_text_20 | 36.475 ms | 66.070 ms (0.55x) | 30.594 ms (1.19x) | 30.930 ms (1.18x) | 32.449 ms (1.12x) |
| Sort_int_500k | 71.867 ms | 91.278 ms (0.79x) | 69.547 ms (1.03x) | 68.545 ms (1.05x) | 72.792 ms (0.99x) |
| Sort_int_grpby | 87.478 ms | 105.202 ms (0.83x) | 82.336 ms (1.06x) | 87.266 ms (1.00x) | 97.589 ms (0.90x) |

### Numeric

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Numeric_agg | 82.534 ms | 123.355 ms (0.67x) | 82.674 ms (1.00x) | 82.996 ms (0.99x) | 88.256 ms (0.94x) |
| Numeric_arith | 98.994 ms | 127.237 ms (0.78x) | 96.834 ms (1.02x) | 98.969 ms (1.00x) | 100.435 ms (0.99x) |

### JSONB

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| JSONB_extract_num | 60.760 ms | 99.729 ms (0.61x) | 60.093 ms (1.01x) | 60.942 ms (1.00x) | 61.559 ms (0.99x) |
| JSONB_extract_str | 40.921 ms | 72.255 ms (0.57x) | 40.684 ms (1.01x) | 39.227 ms (1.04x) | 43.810 ms (0.93x) |
| JSONB_filter | 57.941 ms | 96.635 ms (0.60x) | 58.127 ms (1.00x) | 59.762 ms (0.97x) | 59.833 ms (0.97x) |
| JSONB_agg | 145.039 ms | 184.159 ms (0.79x) | 141.186 ms (1.03x) | 140.327 ms (1.03x) | 145.672 ms (1.00x) |
| JSONB_contains | 59.496 ms | 88.271 ms (0.67x) | 58.767 ms (1.01x) | 57.485 ms (1.03x) | 59.554 ms (1.00x) |
| JSONB_exists_key | 43.283 ms | 75.481 ms (0.57x) | 44.151 ms (0.98x) | 43.573 ms (0.99x) | 46.159 ms (0.94x) |

### Arrays

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Array_overlap | 37.800 ms | 94.305 ms (0.40x) | 38.450 ms (0.98x) | 37.609 ms (1.00x) | 39.495 ms (0.96x) |
| Array_contains | 28.404 ms | 62.753 ms (0.45x) | 29.368 ms (0.97x) | 29.007 ms (0.98x) | 30.499 ms (0.93x) |
| Unnest_agg | 64.327 ms | 116.798 ms (0.55x) | 62.466 ms (1.03x) | 62.497 ms (1.03x) | 64.643 ms (1.00x) |

### Wide Row / Deform

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Wide_10col_sum | 87.464 ms | 102.106 ms (0.86x) | 66.972 ms (1.31x) | 67.646 ms (1.29x) | 82.473 ms (1.06x) |
| Wide_20col_sum | 139.869 ms | 141.584 ms (0.99x) | 98.764 ms (1.42x) | 97.231 ms (1.44x) | 121.733 ms (1.15x) |
| Wide_GroupBy_expr | 168.616 ms | 220.126 ms (0.77x) | 151.111 ms (1.12x) | 147.273 ms (1.14x) | 157.584 ms (1.07x) |

### Super-Wide Tables

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| Wide100_sum | 58.414 ms | 160.208 ms (0.36x) | 56.014 ms (1.04x) | 57.541 ms (1.02x) | 56.369 ms (1.04x) |
| Wide100_groupby | 88.072 ms | 262.374 ms (0.34x) | 87.253 ms (1.01x) | 91.099 ms (0.97x) | 90.289 ms (0.98x) |
| Wide100_filter | 53.960 ms | 146.324 ms (0.37x) | 52.192 ms (1.03x) | 50.823 ms (1.06x) | 49.983 ms (1.08x) |
| Wide300_sum | 52.538 ms | 286.305 ms (0.18x) | 53.301 ms (0.99x) | 55.961 ms (0.94x) | 53.648 ms (0.98x) |
| Wide300_groupby | 63.879 ms | 514.178 ms (0.12x) | 66.191 ms (0.96x) | 72.583 ms (0.88x) | 67.636 ms (0.94x) |
| Wide300_filter | 51.198 ms | 275.645 ms (0.19x) | 50.474 ms (1.01x) | 52.276 ms (0.98x) | 50.323 ms (1.02x) |
| Wide1K_sum | 79.263 ms | 1185.237 ms (0.07x) | 79.888 ms (0.99x) | 80.469 ms (0.98x) | 80.442 ms (0.99x) |
| Wide1K_groupby | 84.108 ms | 2203.175 ms (0.04x) | 86.570 ms (0.97x) | 88.164 ms (0.95x) | 87.082 ms (0.97x) |
| Wide1K_filter | 78.362 ms | 1156.119 ms (0.07x) | 80.496 ms (0.97x) | 80.463 ms (0.97x) | 79.953 ms (0.98x) |

### Partitioned

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| PartScan_filter | 26.879 ms | 58.942 ms (0.46x) | 25.922 ms (1.04x) | 25.555 ms (1.05x) | 28.335 ms (0.95x) |
| PartScan_agg_all | 106.112 ms | 135.633 ms (0.78x) | 94.895 ms (1.12x) | 103.814 ms (1.02x) | 113.481 ms (0.94x) |

### Extreme Expressions

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| CASE_20way | 97.077 ms | 95.030 ms (1.02x) | 57.588 ms (1.69x) | 56.680 ms (1.71x) | 63.050 ms (1.54x) |
| Bool_20cond | 71.301 ms | 89.213 ms (0.80x) | 57.210 ms (1.25x) | 58.094 ms (1.23x) | 67.572 ms (1.06x) |
| Agg_complex_10 | 199.408 ms | 202.556 ms (0.98x) | 139.413 ms (1.43x) | 144.338 ms (1.38x) | 180.557 ms (1.10x) |

### CASE Binary Search

| Query | No JIT | llvmjit | sljit | asmjit | mir |
|-------|--------|------|------|------|------|
| CASE_lt_20 | 98.005 ms | 94.152 ms (1.04x) | 58.812 ms (1.67x) | 57.623 ms (1.70x) | 64.038 ms (1.53x) |
| CASE_lt_50 | 165.333 ms | 137.083 ms (1.21x) | 59.145 ms (2.80x) | 58.783 ms (2.81x) | 64.435 ms (2.57x) |
| CASE_lt_100 | 260.098 ms | 214.661 ms (1.21x) | 63.170 ms (4.12x) | 63.067 ms (4.12x) | 68.074 ms (3.82x) |
| CASE_eq_20 | 132.307 ms | 97.509 ms (1.36x) | 58.263 ms (2.27x) | 58.133 ms (2.28x) | 64.336 ms (2.06x) |
| CASE_eq_50 | 258.006 ms | 177.444 ms (1.45x) | 57.480 ms (4.49x) | 56.686 ms (4.55x) | 62.168 ms (4.15x) |
| CASE_eq_100 | 452.837 ms | 308.395 ms (1.47x) | 62.272 ms (7.27x) | 61.062 ms (7.42x) | 67.145 ms (6.74x) |
| CASE_txt_eq_50 | 749.602 ms | 645.643 ms (1.16x) | 90.177 ms (8.31x) | 91.896 ms (8.16x) | 99.968 ms (7.50x) |
| CASE_range_50 | 267.300 ms | 225.373 ms (1.19x) | 59.330 ms (4.51x) | 59.041 ms (4.53x) | 63.683 ms (4.20x) |
| CASE_ts_range_30 | 1301.996 ms | 1265.472 ms (1.03x) | 1194.290 ms (1.09x) | 1241.633 ms (1.05x) | 1287.864 ms (1.01x) |
| CASE_gt_50 | 166.892 ms | 138.861 ms (1.20x) | 58.884 ms (2.83x) | 59.480 ms (2.81x) | 64.343 ms (2.59x) |
| CASE_between_50 | 267.677 ms | 226.452 ms (1.18x) | 58.641 ms (4.56x) | 58.762 ms (4.56x) | 64.154 ms (4.17x) |
| CASE_nested_3lvl | 82.051 ms | 90.219 ms (0.91x) | 61.870 ms (1.33x) | 63.477 ms (1.29x) | 73.796 ms (1.11x) |

## JIT Compilation Overhead

Time spent on JIT compilation (generation + optimization + emission), extracted from EXPLAIN JSON.

| Query | llvmjit | sljit | asmjit | mir |
|-------|------|------|------|------|
| SUM_int | 21.902 ms | 0.059 ms | 0.139 ms | 0.399 ms |
| COUNT_star | 16.619 ms | - | - | - |
| GroupBy_5agg | 45.263 ms | 0.087 ms | 0.242 ms | 1.219 ms |
| GroupBy_100K_grp | 38.797 ms | 0.111 ms | 0.234 ms | 0.867 ms |
| COUNT_DISTINCT | 22.17 ms | 0.059 ms | 0.111 ms | 0.39 ms |
| HashJoin_single | 43.037 ms | 0.085 ms | 0.287 ms | 1.001 ms |
| HashJoin_composite | 45.715 ms | 0.085 ms | 0.258 ms | 0.929 ms |
| HashJoin_filter | 54.78 ms | 0.094 ms | 0.351 ms | 1.217 ms |
| HashJoin_GroupBy | 55.583 ms | 0.091 ms | 0.354 ms | 1.392 ms |
| LeftJoin | 40.078 ms | 0.084 ms | 0.345 ms | 0.915 ms |
| RightJoin | 40.377 ms | 0.085 ms | 0.235 ms | 0.878 ms |
| FullOuterJoin | 47.084 ms | 0.083 ms | 0.372 ms | 1.249 ms |
| EXISTS_semi | 41.257 ms | 0.087 ms | 0.237 ms | 0.804 ms |
| NOT_EXISTS_anti | 32.019 ms | 0.055 ms | 0.141 ms | 0.458 ms |
| IN_subquery | 41.744 ms | 0.08 ms | 0.245 ms | 0.776 ms |
| INTERSECT | 29.025 ms | 0.051 ms | 0.17 ms | 0.647 ms |
| EXCEPT | 28.424 ms | 0.049 ms | 0.16 ms | 0.624 ms |
| UNION_ALL_agg | 29.232 ms | 0.049 ms | 0.088 ms | 0.415 ms |
| CASE_simple | 23.795 ms | 0.058 ms | 0.136 ms | 0.433 ms |
| CASE_searched_4way | 23.604 ms | 0.084 ms | 0.152 ms | 0.55 ms |
| COALESCE_NULLIF | 23.792 ms | 0.068 ms | 0.137 ms | 0.469 ms |
| Bool_AND_OR | 27.481 ms | 0.065 ms | 0.186 ms | 0.715 ms |
| Arith_expr | 25.653 ms | 0.067 ms | 0.145 ms | 0.556 ms |
| Scalar_subq | 38.65 ms | 0.074 ms | 0.251 ms | 0.799 ms |
| Correlated_subq | 38.064 ms | 0.083 ms | 0.244 ms | 0.893 ms |
| LATERAL_top3 | 24.507 ms | 0.074 ms | 0.165 ms | 0.695 ms |
| Date_extract | 31.375 ms | 0.082 ms | 0.238 ms | 0.896 ms |
| Timestamp_trunc | 28.483 ms | 0.076 ms | 0.207 ms | 0.684 ms |
| Interval_arith | 26.409 ms | 0.064 ms | 0.143 ms | 0.514 ms |
| Timestamp_diff | 30.418 ms | 0.072 ms | 0.169 ms | 0.593 ms |
| Text_EQ_filter | 26.556 ms | 0.067 ms | 0.143 ms | 0.478 ms |
| Text_LIKE | 28.373 ms | 0.114 ms | 0.177 ms | 0.447 ms |
| Text_concat_agg | 40.931 ms | 0.089 ms | 0.243 ms | 0.811 ms |
| Text_length_expr | 35.69 ms | 0.069 ms | 0.149 ms | 0.5 ms |
| Text_EQ_c_coll | 49.999 ms | 0.059 ms | 0.132 ms | 0.481 ms |
| Text_NE_c_coll | 26.218 ms | 0.072 ms | 0.136 ms | 0.491 ms |
| Text_LT_c_coll | 27.944 ms | 0.058 ms | 0.117 ms | 0.397 ms |
| Text_sort_c | 21.243 ms | 0.058 ms | 0.12 ms | 0.377 ms |
| LIKE_prefix | 27.103 ms | 0.071 ms | 0.123 ms | 0.413 ms |
| LIKE_suffix | 26.179 ms | 0.054 ms | 0.129 ms | 0.399 ms |
| LIKE_interior | 26.15 ms | 0.062 ms | 0.131 ms | 0.427 ms |
| LIKE_exact | 25.766 ms | 0.099 ms | 0.162 ms | 0.448 ms |
| NOT_LIKE_prefix | 26.949 ms | 0.066 ms | 0.138 ms | 0.409 ms |
| NOT_LIKE_interior | 29.265 ms | 0.071 ms | 0.119 ms | 0.413 ms |
| LIKE_under_prefix | 27.543 ms | 0.104 ms | 0.223 ms | 0.478 ms |
| LIKE_under_mid | 28.595 ms | 0.101 ms | 0.17 ms | 0.449 ms |
| LIKE_under_only | 25.13 ms | 0.103 ms | 0.159 ms | 0.459 ms |
| LIKE_pct_under_mix | 27.79 ms | 0.103 ms | 0.161 ms | 0.477 ms |
| LIKE_multi | 25.823 ms | 0.096 ms | 0.169 ms | 0.42 ms |
| LIKE_3wild | 27.526 ms | 0.117 ms | 0.209 ms | 0.461 ms |
| ILIKE_prefix | 25.939 ms | 0.111 ms | 0.172 ms | 0.427 ms |
| ILIKE_interior | 26.139 ms | 0.11 ms | 0.185 ms | 0.447 ms |
| ILIKE_suffix | 26.704 ms | 0.156 ms | 0.174 ms | 0.441 ms |
| ILIKE_exact | 24.618 ms | 0.132 ms | 0.185 ms | 0.451 ms |
| NOT_ILIKE_int | 27.254 ms | 0.112 ms | 0.177 ms | 0.444 ms |
| Regex_anchor | 26.829 ms | 0.106 ms | 0.172 ms | 0.489 ms |
| Regex_mid | 26.554 ms | 0.111 ms | 0.17 ms | 0.455 ms |
| Regex_neg | 27.032 ms | 0.132 ms | 0.199 ms | 0.451 ms |
| Regex_icase | 27.315 ms | 0.115 ms | 0.174 ms | 0.44 ms |
| Regex_neg_icase | 27.437 ms | 0.109 ms | 0.185 ms | 0.455 ms |
| Regex_charclass | 29.322 ms | 0.109 ms | 0.17 ms | 0.452 ms |
| Regex_alternation | 26.413 ms | 0.11 ms | 0.181 ms | 0.424 ms |
| IN_list_20 | 24.066 ms | 0.065 ms | 0.148 ms | 0.546 ms |
| IN_list_small | 24.323 ms | 0.07 ms | 0.138 ms | 0.491 ms |
| IN_text_5 | 23.336 ms | 0.066 ms | 0.123 ms | 0.381 ms |
| IN_text_20 | 23.629 ms | 0.069 ms | 0.124 ms | 0.406 ms |
| Sort_int_500k | 21.703 ms | 0.058 ms | 0.117 ms | 0.421 ms |
| Sort_int_grpby | 27.978 ms | 0.085 ms | 0.188 ms | 0.679 ms |
| Numeric_agg | 39.143 ms | 0.082 ms | 0.223 ms | 0.759 ms |
| Numeric_arith | 30.016 ms | 0.068 ms | 0.146 ms | 0.518 ms |
| JSONB_extract_num | 31.29 ms | 0.069 ms | 0.134 ms | 0.48 ms |
| JSONB_extract_str | 27.563 ms | 0.062 ms | 0.123 ms | 0.447 ms |
| JSONB_filter | 31.233 ms | 0.062 ms | 0.136 ms | 0.481 ms |
| JSONB_agg | 41.886 ms | 0.08 ms | 0.237 ms | 0.872 ms |
| JSONB_contains | 25.662 ms | 0.06 ms | 0.124 ms | 0.393 ms |
| JSONB_exists_key | 27.402 ms | 0.058 ms | 0.119 ms | 0.422 ms |
| Array_overlap | 28.234 ms | 0.076 ms | 0.136 ms | 0.455 ms |
| Array_contains | 27.423 ms | 0.061 ms | 0.13 ms | 0.451 ms |
| Unnest_agg | 33.232 ms | 0.078 ms | 0.211 ms | 0.851 ms |
| Wide_10col_sum | 31.78 ms | 0.076 ms | 0.204 ms | 0.85 ms |
| Wide_20col_sum | 40.441 ms | 0.083 ms | 0.311 ms | 1.358 ms |
| Wide_GroupBy_expr | 58.268 ms | 0.095 ms | 0.366 ms | 1.06 ms |
| Wide100_sum | 107.435 ms | 0.122 ms | 0.616 ms | 0.35 ms |
| Wide100_groupby | 172.919 ms | 0.195 ms | 1.112 ms | 0.746 ms |
| Wide100_filter | 97.727 ms | 0.133 ms | 0.596 ms | 0.359 ms |
| Wide300_sum | 226.712 ms | 0.259 ms | 1.667 ms | 0.37 ms |
| Wide300_groupby | 439.446 ms | 0.492 ms | 3.086 ms | 0.773 ms |
| Wide300_filter | 214.559 ms | 0.273 ms | 1.653 ms | 0.37 ms |
| Wide1K_sum | 1053.862 ms | 0.056 ms | 0.097 ms | 0.374 ms |
| Wide1K_groupby | 2079.014 ms | 0.069 ms | 0.183 ms | 0.728 ms |
| Wide1K_filter | 1035.802 ms | 0.049 ms | 0.097 ms | 0.352 ms |
| PartScan_filter | 31.081 ms | 0.077 ms | 0.219 ms | 0.936 ms |
| PartScan_agg_all | 37.838 ms | 0.085 ms | 0.335 ms | 1.155 ms |
| CASE_20way | 31.048 ms | 0.063 ms | 0.147 ms | 0.436 ms |
| Bool_20cond | 33.554 ms | 0.078 ms | 0.282 ms | 1.499 ms |
| Agg_complex_10 | 58.091 ms | 0.101 ms | 0.399 ms | 2.819 ms |
| CASE_lt_20 | 30.388 ms | 0.069 ms | 0.146 ms | 0.432 ms |
| CASE_lt_50 | 40.928 ms | 0.156 ms | 0.141 ms | 0.508 ms |
| CASE_lt_100 | 58.201 ms | 0.073 ms | 0.151 ms | 0.541 ms |
| CASE_eq_20 | 30.446 ms | 0.063 ms | 0.137 ms | 0.445 ms |
| CASE_eq_50 | 42.058 ms | 0.073 ms | 0.141 ms | 0.479 ms |
| CASE_eq_100 | 58.825 ms | 0.08 ms | 0.153 ms | 0.504 ms |
| CASE_txt_eq_50 | 49.206 ms | 0.076 ms | 0.226 ms | 1.34 ms |
| CASE_range_50 | 78.514 ms | 0.072 ms | 0.147 ms | 0.475 ms |
| CASE_ts_range_30 | 120.849 ms | 0.26 ms | 2.477 ms | 17.622 ms |
| CASE_gt_50 | 40.399 ms | 0.066 ms | 0.148 ms | 0.447 ms |
| CASE_between_50 | 77.374 ms | 0.074 ms | 0.148 ms | 0.507 ms |
| CASE_nested_3lvl | 32.413 ms | 0.093 ms | 0.288 ms | 1.287 ms |

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

**Text_EQ_c_coll**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text = 'prefix_42' COLLATE "C"
```

**Text_NE_c_coll**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text <> 'prefix_42' COLLATE "C"
```

**Text_LT_c_coll**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text < 'prefix_50' COLLATE "C"
```

**Text_sort_c**
```sql
SELECT grp_text FROM text_data ORDER BY grp_text COLLATE "C" LIMIT 10
```

### LIKE (StringZilla)

**LIKE_prefix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE 'c4ca%'
```

**LIKE_suffix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE '%ff'
```

**LIKE_interior**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE '%abc%'
```

**LIKE_exact**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text LIKE 'prefix_42'
```

**NOT_LIKE_prefix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text NOT LIKE 'c4ca%'
```

**NOT_LIKE_interior**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text NOT LIKE '%abc%'
```

### LIKE (underscore)

**LIKE_under_prefix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE 'c4c_4%'
```

**LIKE_under_mid**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE '%a_c%'
```

**LIKE_under_only**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text LIKE 'prefix___'
```

**LIKE_pct_under_mix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE '_4%a_b%'
```

### ILIKE/Regex (PCRE2)

**LIKE_multi**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE '%a%b%'
```

**LIKE_3wild**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text LIKE '%a%b%c%'
```

**ILIKE_prefix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ILIKE 'C4CA%'
```

**ILIKE_interior**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ILIKE '%ABC%'
```

**ILIKE_suffix**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ILIKE '%FF'
```

**ILIKE_exact**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text ILIKE 'PREFIX_42'
```

**NOT_ILIKE_int**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text NOT ILIKE '%ABC%'
```

**Regex_anchor**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ~ '^[0-9a-f]{4}'
```

**Regex_mid**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ~ '[0-9]{3}[a-f]{3}'
```

**Regex_neg**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text !~ '^[0-9]'
```

**Regex_icase**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ~* '^C4CA'
```

**Regex_neg_icase**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text !~* '^C4CA'
```

**Regex_charclass**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ~ '[a-f]{5,}'
```

**Regex_alternation**
```sql
SELECT COUNT(*) FROM text_data WHERE hash_text ~ '^(c4|e4|a8)'
```

### IN list / Sort

**IN_list_20**
```sql
SELECT COUNT(*) FROM bench_data WHERE val1 + 0 IN (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20)
```

**IN_list_small**
```sql
SELECT COUNT(*) FROM join_left WHERE key1 + 0 IN (1,2,3,4,5,6,7,8,9,10)
```

**IN_text_5**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text IN ('prefix_1','prefix_10','prefix_20','prefix_50','prefix_99')
```

**IN_text_20**
```sql
SELECT COUNT(*) FROM text_data WHERE grp_text IN ('prefix_1','prefix_5','prefix_10','prefix_15','prefix_20','prefix_25','prefix_30','prefix_35','prefix_40','prefix_45','prefix_50','prefix_55','prefix_60','prefix_65','prefix_70','prefix_75','prefix_80','prefix_85','prefix_90','prefix_99')
```

**Sort_int_500k**
```sql
SELECT key1 FROM join_left ORDER BY key1 + 0 LIMIT 10
```

**Sort_int_grpby**
```sql
SELECT key1 % 100, COUNT(*) FROM join_left GROUP BY key1 % 100 ORDER BY COUNT(*) DESC LIMIT 10
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

**JSONB_extract_num**
```sql
SELECT SUM((doc->>'a')::int) FROM jsonb_data
```

**JSONB_extract_str**
```sql
SELECT COUNT(doc->>'b') FROM jsonb_data
```

**JSONB_filter**
```sql
SELECT COUNT(*) FROM jsonb_data WHERE (doc->>'a')::int = 42
```

**JSONB_agg**
```sql
SELECT grp_jsonb, COUNT(*), SUM((doc->>'a')::int) FROM jsonb_data GROUP BY grp_jsonb
```

**JSONB_contains**
```sql
SELECT COUNT(*) FROM jsonb_data WHERE doc @> '{"a": 42}'
```

**JSONB_exists_key**
```sql
SELECT COUNT(*) FROM jsonb_data WHERE doc ? 'a'
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

### Extreme Expressions

**CASE_20way**
```sql
SELECT SUM(CASE WHEN val < 500 THEN 1 WHEN val < 1000 THEN 2 WHEN val < 1500 THEN 3 WHEN val < 2000 THEN 4 WHEN val < 2500 THEN 5 WHEN val < 3000 THEN 6 WHEN val < 3500 THEN 7 WHEN val < 4000 THEN 8 WHEN val < 4500 THEN 9 WHEN val < 5000 THEN 10 WHEN val < 5500 THEN 11 WHEN val < 6000 THEN 12 WHEN val < 6500 THEN 13 WHEN val < 7000 THEN 14 WHEN val < 7500 THEN 15 WHEN val < 8000 THEN 16 WHEN val < 8500 THEN 17 WHEN val < 9000 THEN 18 WHEN val < 9500 THEN 19 ELSE 20 END) FROM join_left
```

**Bool_20cond**
```sql
SELECT COUNT(*) FROM join_left WHERE (val > 100 AND val < 9900 AND key1 > 10 AND key1 < 99990 AND key2 > 5 AND key2 < 49995) OR (val > 200 AND val < 9800 AND key1 > 20 AND key1 < 99980) OR (val > 300 AND val < 9700 AND key1 > 30 AND key1 < 99970) OR (val > 500 AND val < 9500 AND key2 > 100 AND key2 < 49900)
```

**Agg_complex_10**
```sql
SELECT grp, COUNT(*), SUM(val1*val1), AVG(val2+val3), MIN(val1-val2+val3), MAX(val4*2-val5), SUM(CASE WHEN val1>5000 THEN val2 ELSE val3 END), AVG(ABS(val1-val2)), SUM(val1%100+val2%100), COUNT(NULLIF(val3,0)) FROM bench_data GROUP BY grp
```

### CASE Binary Search

**CASE_lt_20**
```sql
SELECT SUM(CASE WHEN val1 < 250 THEN 1 WHEN val1 < 750 THEN 2 WHEN val1 < 1250 THEN 3 WHEN val1 < 1750 THEN 4 WHEN val1 < 2250 THEN 5 WHEN val1 < 2750 THEN 6 WHEN val1 < 3250 THEN 7 WHEN val1 < 3750 THEN 8 WHEN val1 < 4250 THEN 9 WHEN val1 < 4750 THEN 10 WHEN val1 < 5250 THEN 11 WHEN val1 < 5750 THEN 12 WHEN val1 < 6250 THEN 13 WHEN val1 < 6750 THEN 14 WHEN val1 < 7250 THEN 15 WHEN val1 < 7750 THEN 16 WHEN val1 < 8250 THEN 17 WHEN val1 < 8750 THEN 18 WHEN val1 < 9250 THEN 19 WHEN val1 < 9750 THEN 20 ELSE 21 END) FROM bench_data
```

**CASE_lt_50**
```sql
SELECT SUM(CASE WHEN val1 < 100 THEN 1 WHEN val1 < 300 THEN 2 WHEN val1 < 500 THEN 3 WHEN val1 < 700 THEN 4 WHEN val1 < 900 THEN 5 WHEN val1 < 1100 THEN 6 WHEN val1 < 1300 THEN 7 WHEN val1 < 1500 THEN 8 WHEN val1 < 1700 THEN 9 WHEN val1 < 1900 THEN 10 WHEN val1 < 2100 THEN 11 WHEN val1 < 2300 THEN 12 WHEN val1 < 2500 THEN 13 WHEN val1 < 2700 THEN 14 WHEN val1 < 2900 THEN 15 WHEN val1 < 3100 THEN 16 WHEN val1 < 3300 THEN 17 WHEN val1 < 3500 THEN 18 WHEN val1 < 3700 THEN 19 WHEN val1 < 3900 THEN 20 WHEN val1 < 4100 THEN 21 WHEN val1 < 4300 THEN 22 WHEN val1 < 4500 THEN 23 WHEN val1 < 4700 THEN 24 WHEN val1 < 4900 THEN 25 WHEN val1 < 5100 THEN 26 WHEN val1 < 5300 THEN 27 WHEN val1 < 5500 THEN 28 WHEN val1 < 5700 THEN 29 WHEN val1 < 5900 THEN 30 WHEN val1 < 6100 THEN 31 WHEN val1 < 6300 THEN 32 WHEN val1 < 6500 THEN 33 WHEN val1 < 6700 THEN 34 WHEN val1 < 6900 THEN 35 WHEN val1 < 7100 THEN 36 WHEN val1 < 7300 THEN 37 WHEN val1 < 7500 THEN 38 WHEN val1 < 7700 THEN 39 WHEN val1 < 7900 THEN 40 WHEN val1 < 8100 THEN 41 WHEN val1 < 8300 THEN 42 WHEN val1 < 8500 THEN 43 WHEN val1 < 8700 THEN 44 WHEN val1 < 8900 THEN 45 WHEN val1 < 9100 THEN 46 WHEN val1 < 9300 THEN 47 WHEN val1 < 9500 THEN 48 WHEN val1 < 9700 THEN 49 WHEN val1 < 9900 THEN 50 ELSE 51 END) FROM bench_data
```

**CASE_lt_100**
```sql
SELECT SUM(CASE WHEN val1 < 50 THEN 1 WHEN val1 < 150 THEN 2 WHEN val1 < 250 THEN 3 WHEN val1 < 350 THEN 4 WHEN val1 < 450 THEN 5 WHEN val1 < 550 THEN 6 WHEN val1 < 650 THEN 7 WHEN val1 < 750 THEN 8 WHEN val1 < 850 THEN 9 WHEN val1 < 950 THEN 10 WHEN val1 < 1050 THEN 11 WHEN val1 < 1150 THEN 12 WHEN val1 < 1250 THEN 13 WHEN val1 < 1350 THEN 14 WHEN val1 < 1450 THEN 15 WHEN val1 < 1550 THEN 16 WHEN val1 < 1650 THEN 17 WHEN val1 < 1750 THEN 18 WHEN val1 < 1850 THEN 19 WHEN val1 < 1950 THEN 20 WHEN val1 < 2050 THEN 21 WHEN val1 < 2150 THEN 22 WHEN val1 < 2250 THEN 23 WHEN val1 < 2350 THEN 24 WHEN val1 < 2450 THEN 25 WHEN val1 < 2550 THEN 26 WHEN val1 < 2650 THEN 27 WHEN val1 < 2750 THEN 28 WHEN val1 < 2850 THEN 29 WHEN val1 < 2950 THEN 30 WHEN val1 < 3050 THEN 31 WHEN val1 < 3150 THEN 32 WHEN val1 < 3250 THEN 33 WHEN val1 < 3350 THEN 34 WHEN val1 < 3450 THEN 35 WHEN val1 < 3550 THEN 36 WHEN val1 < 3650 THEN 37 WHEN val1 < 3750 THEN 38 WHEN val1 < 3850 THEN 39 WHEN val1 < 3950 THEN 40 WHEN val1 < 4050 THEN 41 WHEN val1 < 4150 THEN 42 WHEN val1 < 4250 THEN 43 WHEN val1 < 4350 THEN 44 WHEN val1 < 4450 THEN 45 WHEN val1 < 4550 THEN 46 WHEN val1 < 4650 THEN 47 WHEN val1 < 4750 THEN 48 WHEN val1 < 4850 THEN 49 WHEN val1 < 4950 THEN 50 WHEN val1 < 5050 THEN 51 WHEN val1 < 5150 THEN 52 WHEN val1 < 5250 THEN 53 WHEN val1 < 5350 THEN 54 WHEN val1 < 5450 THEN 55 WHEN val1 < 5550 THEN 56 WHEN val1 < 5650 THEN 57 WHEN val1 < 5750 THEN 58 WHEN val1 < 5850 THEN 59 WHEN val1 < 5950 THEN 60 WHEN val1 < 6050 THEN 61 WHEN val1 < 6150 THEN 62 WHEN val1 < 6250 THEN 63 WHEN val1 < 6350 THEN 64 WHEN val1 < 6450 THEN 65 WHEN val1 < 6550 THEN 66 WHEN val1 < 6650 THEN 67 WHEN val1 < 6750 THEN 68 WHEN val1 < 6850 THEN 69 WHEN val1 < 6950 THEN 70 WHEN val1 < 7050 THEN 71 WHEN val1 < 7150 THEN 72 WHEN val1 < 7250 THEN 73 WHEN val1 < 7350 THEN 74 WHEN val1 < 7450 THEN 75 WHEN val1 < 7550 THEN 76 WHEN val1 < 7650 THEN 77 WHEN val1 < 7750 THEN 78 WHEN val1 < 7850 THEN 79 WHEN val1 < 7950 THEN 80 WHEN val1 < 8050 THEN 81 WHEN val1 < 8150 THEN 82 WHEN val1 < 8250 THEN 83 WHEN val1 < 8350 THEN 84 WHEN val1 < 8450 THEN 85 WHEN val1 < 8550 THEN 86 WHEN val1 < 8650 THEN 87 WHEN val1 < 8750 THEN 88 WHEN val1 < 8850 THEN 89 WHEN val1 < 8950 THEN 90 WHEN val1 < 9050 THEN 91 WHEN val1 < 9150 THEN 92 WHEN val1 < 9250 THEN 93 WHEN val1 < 9350 THEN 94 WHEN val1 < 9450 THEN 95 WHEN val1 < 9550 THEN 96 WHEN val1 < 9650 THEN 97 WHEN val1 < 9750 THEN 98 WHEN val1 < 9850 THEN 99 WHEN val1 < 9950 THEN 100 ELSE 101 END) FROM bench_data
```

**CASE_eq_20**
```sql
SELECT SUM(CASE val1 WHEN 0 THEN 1 WHEN 500 THEN 2 WHEN 1000 THEN 3 WHEN 1500 THEN 4 WHEN 2000 THEN 5 WHEN 2500 THEN 6 WHEN 3000 THEN 7 WHEN 3500 THEN 8 WHEN 4000 THEN 9 WHEN 4500 THEN 10 WHEN 5000 THEN 11 WHEN 5500 THEN 12 WHEN 6000 THEN 13 WHEN 6500 THEN 14 WHEN 7000 THEN 15 WHEN 7500 THEN 16 WHEN 8000 THEN 17 WHEN 8500 THEN 18 WHEN 9000 THEN 19 WHEN 9500 THEN 20 ELSE 0 END) FROM bench_data
```

**CASE_eq_50**
```sql
SELECT SUM(CASE val1 WHEN 0 THEN 1 WHEN 200 THEN 2 WHEN 400 THEN 3 WHEN 600 THEN 4 WHEN 800 THEN 5 WHEN 1000 THEN 6 WHEN 1200 THEN 7 WHEN 1400 THEN 8 WHEN 1600 THEN 9 WHEN 1800 THEN 10 WHEN 2000 THEN 11 WHEN 2200 THEN 12 WHEN 2400 THEN 13 WHEN 2600 THEN 14 WHEN 2800 THEN 15 WHEN 3000 THEN 16 WHEN 3200 THEN 17 WHEN 3400 THEN 18 WHEN 3600 THEN 19 WHEN 3800 THEN 20 WHEN 4000 THEN 21 WHEN 4200 THEN 22 WHEN 4400 THEN 23 WHEN 4600 THEN 24 WHEN 4800 THEN 25 WHEN 5000 THEN 26 WHEN 5200 THEN 27 WHEN 5400 THEN 28 WHEN 5600 THEN 29 WHEN 5800 THEN 30 WHEN 6000 THEN 31 WHEN 6200 THEN 32 WHEN 6400 THEN 33 WHEN 6600 THEN 34 WHEN 6800 THEN 35 WHEN 7000 THEN 36 WHEN 7200 THEN 37 WHEN 7400 THEN 38 WHEN 7600 THEN 39 WHEN 7800 THEN 40 WHEN 8000 THEN 41 WHEN 8200 THEN 42 WHEN 8400 THEN 43 WHEN 8600 THEN 44 WHEN 8800 THEN 45 WHEN 9000 THEN 46 WHEN 9200 THEN 47 WHEN 9400 THEN 48 WHEN 9600 THEN 49 WHEN 9800 THEN 50 ELSE 0 END) FROM bench_data
```

**CASE_eq_100**
```sql
SELECT SUM(CASE val1 WHEN 0 THEN 1 WHEN 100 THEN 2 WHEN 200 THEN 3 WHEN 300 THEN 4 WHEN 400 THEN 5 WHEN 500 THEN 6 WHEN 600 THEN 7 WHEN 700 THEN 8 WHEN 800 THEN 9 WHEN 900 THEN 10 WHEN 1000 THEN 11 WHEN 1100 THEN 12 WHEN 1200 THEN 13 WHEN 1300 THEN 14 WHEN 1400 THEN 15 WHEN 1500 THEN 16 WHEN 1600 THEN 17 WHEN 1700 THEN 18 WHEN 1800 THEN 19 WHEN 1900 THEN 20 WHEN 2000 THEN 21 WHEN 2100 THEN 22 WHEN 2200 THEN 23 WHEN 2300 THEN 24 WHEN 2400 THEN 25 WHEN 2500 THEN 26 WHEN 2600 THEN 27 WHEN 2700 THEN 28 WHEN 2800 THEN 29 WHEN 2900 THEN 30 WHEN 3000 THEN 31 WHEN 3100 THEN 32 WHEN 3200 THEN 33 WHEN 3300 THEN 34 WHEN 3400 THEN 35 WHEN 3500 THEN 36 WHEN 3600 THEN 37 WHEN 3700 THEN 38 WHEN 3800 THEN 39 WHEN 3900 THEN 40 WHEN 4000 THEN 41 WHEN 4100 THEN 42 WHEN 4200 THEN 43 WHEN 4300 THEN 44 WHEN 4400 THEN 45 WHEN 4500 THEN 46 WHEN 4600 THEN 47 WHEN 4700 THEN 48 WHEN 4800 THEN 49 WHEN 4900 THEN 50 WHEN 5000 THEN 51 WHEN 5100 THEN 52 WHEN 5200 THEN 53 WHEN 5300 THEN 54 WHEN 5400 THEN 55 WHEN 5500 THEN 56 WHEN 5600 THEN 57 WHEN 5700 THEN 58 WHEN 5800 THEN 59 WHEN 5900 THEN 60 WHEN 6000 THEN 61 WHEN 6100 THEN 62 WHEN 6200 THEN 63 WHEN 6300 THEN 64 WHEN 6400 THEN 65 WHEN 6500 THEN 66 WHEN 6600 THEN 67 WHEN 6700 THEN 68 WHEN 6800 THEN 69 WHEN 6900 THEN 70 WHEN 7000 THEN 71 WHEN 7100 THEN 72 WHEN 7200 THEN 73 WHEN 7300 THEN 74 WHEN 7400 THEN 75 WHEN 7500 THEN 76 WHEN 7600 THEN 77 WHEN 7700 THEN 78 WHEN 7800 THEN 79 WHEN 7900 THEN 80 WHEN 8000 THEN 81 WHEN 8100 THEN 82 WHEN 8200 THEN 83 WHEN 8300 THEN 84 WHEN 8400 THEN 85 WHEN 8500 THEN 86 WHEN 8600 THEN 87 WHEN 8700 THEN 88 WHEN 8800 THEN 89 WHEN 8900 THEN 90 WHEN 9000 THEN 91 WHEN 9100 THEN 92 WHEN 9200 THEN 93 WHEN 9300 THEN 94 WHEN 9400 THEN 95 WHEN 9500 THEN 96 WHEN 9600 THEN 97 WHEN 9700 THEN 98 WHEN 9800 THEN 99 WHEN 9900 THEN 100 ELSE 0 END) FROM bench_data
```

**CASE_txt_eq_50**
```sql
SELECT SUM(CASE txt WHEN 'row_1' THEN 1 WHEN 'row_20001' THEN 2 WHEN 'row_40001' THEN 3 WHEN 'row_60001' THEN 4 WHEN 'row_80001' THEN 5 WHEN 'row_100001' THEN 6 WHEN 'row_120001' THEN 7 WHEN 'row_140001' THEN 8 WHEN 'row_160001' THEN 9 WHEN 'row_180001' THEN 10 WHEN 'row_200001' THEN 11 WHEN 'row_220001' THEN 12 WHEN 'row_240001' THEN 13 WHEN 'row_260001' THEN 14 WHEN 'row_280001' THEN 15 WHEN 'row_300001' THEN 16 WHEN 'row_320001' THEN 17 WHEN 'row_340001' THEN 18 WHEN 'row_360001' THEN 19 WHEN 'row_380001' THEN 20 WHEN 'row_400001' THEN 21 WHEN 'row_420001' THEN 22 WHEN 'row_440001' THEN 23 WHEN 'row_460001' THEN 24 WHEN 'row_480001' THEN 25 WHEN 'row_500001' THEN 26 WHEN 'row_520001' THEN 27 WHEN 'row_540001' THEN 28 WHEN 'row_560001' THEN 29 WHEN 'row_580001' THEN 30 WHEN 'row_600001' THEN 31 WHEN 'row_620001' THEN 32 WHEN 'row_640001' THEN 33 WHEN 'row_660001' THEN 34 WHEN 'row_680001' THEN 35 WHEN 'row_700001' THEN 36 WHEN 'row_720001' THEN 37 WHEN 'row_740001' THEN 38 WHEN 'row_760001' THEN 39 WHEN 'row_780001' THEN 40 WHEN 'row_800001' THEN 41 WHEN 'row_820001' THEN 42 WHEN 'row_840001' THEN 43 WHEN 'row_860001' THEN 44 WHEN 'row_880001' THEN 45 WHEN 'row_900001' THEN 46 WHEN 'row_920001' THEN 47 WHEN 'row_940001' THEN 48 WHEN 'row_960001' THEN 49 WHEN 'row_980001' THEN 50 ELSE 0 END) FROM bench_data
```

**CASE_range_50**
```sql
SELECT SUM(CASE WHEN val1 >= 0 AND val1 < 200 THEN 1 WHEN val1 >= 200 AND val1 < 400 THEN 2 WHEN val1 >= 400 AND val1 < 600 THEN 3 WHEN val1 >= 600 AND val1 < 800 THEN 4 WHEN val1 >= 800 AND val1 < 1000 THEN 5 WHEN val1 >= 1000 AND val1 < 1200 THEN 6 WHEN val1 >= 1200 AND val1 < 1400 THEN 7 WHEN val1 >= 1400 AND val1 < 1600 THEN 8 WHEN val1 >= 1600 AND val1 < 1800 THEN 9 WHEN val1 >= 1800 AND val1 < 2000 THEN 10 WHEN val1 >= 2000 AND val1 < 2200 THEN 11 WHEN val1 >= 2200 AND val1 < 2400 THEN 12 WHEN val1 >= 2400 AND val1 < 2600 THEN 13 WHEN val1 >= 2600 AND val1 < 2800 THEN 14 WHEN val1 >= 2800 AND val1 < 3000 THEN 15 WHEN val1 >= 3000 AND val1 < 3200 THEN 16 WHEN val1 >= 3200 AND val1 < 3400 THEN 17 WHEN val1 >= 3400 AND val1 < 3600 THEN 18 WHEN val1 >= 3600 AND val1 < 3800 THEN 19 WHEN val1 >= 3800 AND val1 < 4000 THEN 20 WHEN val1 >= 4000 AND val1 < 4200 THEN 21 WHEN val1 >= 4200 AND val1 < 4400 THEN 22 WHEN val1 >= 4400 AND val1 < 4600 THEN 23 WHEN val1 >= 4600 AND val1 < 4800 THEN 24 WHEN val1 >= 4800 AND val1 < 5000 THEN 25 WHEN val1 >= 5000 AND val1 < 5200 THEN 26 WHEN val1 >= 5200 AND val1 < 5400 THEN 27 WHEN val1 >= 5400 AND val1 < 5600 THEN 28 WHEN val1 >= 5600 AND val1 < 5800 THEN 29 WHEN val1 >= 5800 AND val1 < 6000 THEN 30 WHEN val1 >= 6000 AND val1 < 6200 THEN 31 WHEN val1 >= 6200 AND val1 < 6400 THEN 32 WHEN val1 >= 6400 AND val1 < 6600 THEN 33 WHEN val1 >= 6600 AND val1 < 6800 THEN 34 WHEN val1 >= 6800 AND val1 < 7000 THEN 35 WHEN val1 >= 7000 AND val1 < 7200 THEN 36 WHEN val1 >= 7200 AND val1 < 7400 THEN 37 WHEN val1 >= 7400 AND val1 < 7600 THEN 38 WHEN val1 >= 7600 AND val1 < 7800 THEN 39 WHEN val1 >= 7800 AND val1 < 8000 THEN 40 WHEN val1 >= 8000 AND val1 < 8200 THEN 41 WHEN val1 >= 8200 AND val1 < 8400 THEN 42 WHEN val1 >= 8400 AND val1 < 8600 THEN 43 WHEN val1 >= 8600 AND val1 < 8800 THEN 44 WHEN val1 >= 8800 AND val1 < 9000 THEN 45 WHEN val1 >= 9000 AND val1 < 9200 THEN 46 WHEN val1 >= 9200 AND val1 < 9400 THEN 47 WHEN val1 >= 9400 AND val1 < 9600 THEN 48 WHEN val1 >= 9600 AND val1 < 9800 THEN 49 WHEN val1 >= 9800 AND val1 < 10000 THEN 50 ELSE 0 END) FROM bench_data
```

**CASE_ts_range_30**
```sql
SELECT count(*), min(adjusted), max(adjusted) FROM (SELECT CASE WHEN ts >= '2000-01-01'::timestamptz AND ts < '2000-01-02'::timestamptz THEN ts + interval '10 seconds' WHEN ts >= '2000-01-02'::timestamptz AND ts < '2000-01-03'::timestamptz THEN ts + interval '20 seconds' WHEN ts >= '2000-01-03'::timestamptz AND ts < '2000-01-04'::timestamptz THEN ts + interval '30 seconds' WHEN ts >= '2000-01-04'::timestamptz AND ts < '2000-01-05'::timestamptz THEN ts + interval '40 seconds' WHEN ts >= '2000-01-05'::timestamptz AND ts < '2000-01-06'::timestamptz THEN ts + interval '50 seconds' WHEN ts >= '2000-01-06'::timestamptz AND ts < '2000-01-07'::timestamptz THEN ts + interval '60 seconds' WHEN ts >= '2000-01-07'::timestamptz AND ts < '2000-01-08'::timestamptz THEN ts + interval '70 seconds' WHEN ts >= '2000-01-08'::timestamptz AND ts < '2000-01-09'::timestamptz THEN ts + interval '80 seconds' WHEN ts >= '2000-01-09'::timestamptz AND ts < '2000-01-10'::timestamptz THEN ts + interval '90 seconds' WHEN ts >= '2000-01-10'::timestamptz AND ts < '2000-01-11'::timestamptz THEN ts + interval '100 seconds' WHEN ts >= '2000-01-11'::timestamptz AND ts < '2000-01-12'::timestamptz THEN ts + interval '110 seconds' WHEN ts >= '2000-01-12'::timestamptz AND ts < '2000-01-13'::timestamptz THEN ts + interval '120 seconds' WHEN ts >= '2000-01-13'::timestamptz AND ts < '2000-01-14'::timestamptz THEN ts + interval '130 seconds' WHEN ts >= '2000-01-14'::timestamptz AND ts < '2000-01-15'::timestamptz THEN ts + interval '140 seconds' WHEN ts >= '2000-01-15'::timestamptz AND ts < '2000-01-16'::timestamptz THEN ts + interval '150 seconds' WHEN ts >= '2000-01-16'::timestamptz AND ts < '2000-01-17'::timestamptz THEN ts + interval '160 seconds' WHEN ts >= '2000-01-17'::timestamptz AND ts < '2000-01-18'::timestamptz THEN ts + interval '170 seconds' WHEN ts >= '2000-01-18'::timestamptz AND ts < '2000-01-19'::timestamptz THEN ts + interval '180 seconds' WHEN ts >= '2000-01-19'::timestamptz AND ts < '2000-01-20'::timestamptz THEN ts + interval '190 seconds' WHEN ts >= '2000-01-20'::timestamptz AND ts < '2000-01-21'::timestamptz THEN ts + interval '200 seconds' WHEN ts >= '2000-01-21'::timestamptz AND ts < '2000-01-22'::timestamptz THEN ts + interval '210 seconds' WHEN ts >= '2000-01-22'::timestamptz AND ts < '2000-01-23'::timestamptz THEN ts + interval '220 seconds' WHEN ts >= '2000-01-23'::timestamptz AND ts < '2000-01-24'::timestamptz THEN ts + interval '230 seconds' WHEN ts >= '2000-01-24'::timestamptz AND ts < '2000-01-25'::timestamptz THEN ts + interval '240 seconds' WHEN ts >= '2000-01-25'::timestamptz AND ts < '2000-01-26'::timestamptz THEN ts + interval '250 seconds' WHEN ts >= '2000-01-26'::timestamptz AND ts < '2000-01-27'::timestamptz THEN ts + interval '260 seconds' WHEN ts >= '2000-01-27'::timestamptz AND ts < '2000-01-28'::timestamptz THEN ts + interval '270 seconds' WHEN ts >= '2000-01-28'::timestamptz AND ts < '2000-01-29'::timestamptz THEN ts + interval '280 seconds' WHEN ts >= '2000-01-29'::timestamptz AND ts < '2000-01-30'::timestamptz THEN ts + interval '290 seconds' WHEN ts >= '2000-01-30'::timestamptz AND ts < '2000-01-31'::timestamptz THEN ts + interval '300 seconds' ELSE ts END AS adjusted FROM date_data) sub
```

**CASE_gt_50**
```sql
SELECT SUM(CASE WHEN val1 > 10000 THEN 50 WHEN val1 > 9800 THEN 49 WHEN val1 > 9600 THEN 48 WHEN val1 > 9400 THEN 47 WHEN val1 > 9200 THEN 46 WHEN val1 > 9000 THEN 45 WHEN val1 > 8800 THEN 44 WHEN val1 > 8600 THEN 43 WHEN val1 > 8400 THEN 42 WHEN val1 > 8200 THEN 41 WHEN val1 > 8000 THEN 40 WHEN val1 > 7800 THEN 39 WHEN val1 > 7600 THEN 38 WHEN val1 > 7400 THEN 37 WHEN val1 > 7200 THEN 36 WHEN val1 > 7000 THEN 35 WHEN val1 > 6800 THEN 34 WHEN val1 > 6600 THEN 33 WHEN val1 > 6400 THEN 32 WHEN val1 > 6200 THEN 31 WHEN val1 > 6000 THEN 30 WHEN val1 > 5800 THEN 29 WHEN val1 > 5600 THEN 28 WHEN val1 > 5400 THEN 27 WHEN val1 > 5200 THEN 26 WHEN val1 > 5000 THEN 25 WHEN val1 > 4800 THEN 24 WHEN val1 > 4600 THEN 23 WHEN val1 > 4400 THEN 22 WHEN val1 > 4200 THEN 21 WHEN val1 > 4000 THEN 20 WHEN val1 > 3800 THEN 19 WHEN val1 > 3600 THEN 18 WHEN val1 > 3400 THEN 17 WHEN val1 > 3200 THEN 16 WHEN val1 > 3000 THEN 15 WHEN val1 > 2800 THEN 14 WHEN val1 > 2600 THEN 13 WHEN val1 > 2400 THEN 12 WHEN val1 > 2200 THEN 11 WHEN val1 > 2000 THEN 10 WHEN val1 > 1800 THEN 9 WHEN val1 > 1600 THEN 8 WHEN val1 > 1400 THEN 7 WHEN val1 > 1200 THEN 6 WHEN val1 > 1000 THEN 5 WHEN val1 > 800 THEN 4 WHEN val1 > 600 THEN 3 WHEN val1 > 400 THEN 2 WHEN val1 > 200 THEN 1 ELSE 0 END) FROM bench_data
```

**CASE_between_50**
```sql
SELECT SUM(CASE WHEN val1 BETWEEN 0 AND 199 THEN 1 WHEN val1 BETWEEN 200 AND 399 THEN 2 WHEN val1 BETWEEN 400 AND 599 THEN 3 WHEN val1 BETWEEN 600 AND 799 THEN 4 WHEN val1 BETWEEN 800 AND 999 THEN 5 WHEN val1 BETWEEN 1000 AND 1199 THEN 6 WHEN val1 BETWEEN 1200 AND 1399 THEN 7 WHEN val1 BETWEEN 1400 AND 1599 THEN 8 WHEN val1 BETWEEN 1600 AND 1799 THEN 9 WHEN val1 BETWEEN 1800 AND 1999 THEN 10 WHEN val1 BETWEEN 2000 AND 2199 THEN 11 WHEN val1 BETWEEN 2200 AND 2399 THEN 12 WHEN val1 BETWEEN 2400 AND 2599 THEN 13 WHEN val1 BETWEEN 2600 AND 2799 THEN 14 WHEN val1 BETWEEN 2800 AND 2999 THEN 15 WHEN val1 BETWEEN 3000 AND 3199 THEN 16 WHEN val1 BETWEEN 3200 AND 3399 THEN 17 WHEN val1 BETWEEN 3400 AND 3599 THEN 18 WHEN val1 BETWEEN 3600 AND 3799 THEN 19 WHEN val1 BETWEEN 3800 AND 3999 THEN 20 WHEN val1 BETWEEN 4000 AND 4199 THEN 21 WHEN val1 BETWEEN 4200 AND 4399 THEN 22 WHEN val1 BETWEEN 4400 AND 4599 THEN 23 WHEN val1 BETWEEN 4600 AND 4799 THEN 24 WHEN val1 BETWEEN 4800 AND 4999 THEN 25 WHEN val1 BETWEEN 5000 AND 5199 THEN 26 WHEN val1 BETWEEN 5200 AND 5399 THEN 27 WHEN val1 BETWEEN 5400 AND 5599 THEN 28 WHEN val1 BETWEEN 5600 AND 5799 THEN 29 WHEN val1 BETWEEN 5800 AND 5999 THEN 30 WHEN val1 BETWEEN 6000 AND 6199 THEN 31 WHEN val1 BETWEEN 6200 AND 6399 THEN 32 WHEN val1 BETWEEN 6400 AND 6599 THEN 33 WHEN val1 BETWEEN 6600 AND 6799 THEN 34 WHEN val1 BETWEEN 6800 AND 6999 THEN 35 WHEN val1 BETWEEN 7000 AND 7199 THEN 36 WHEN val1 BETWEEN 7200 AND 7399 THEN 37 WHEN val1 BETWEEN 7400 AND 7599 THEN 38 WHEN val1 BETWEEN 7600 AND 7799 THEN 39 WHEN val1 BETWEEN 7800 AND 7999 THEN 40 WHEN val1 BETWEEN 8000 AND 8199 THEN 41 WHEN val1 BETWEEN 8200 AND 8399 THEN 42 WHEN val1 BETWEEN 8400 AND 8599 THEN 43 WHEN val1 BETWEEN 8600 AND 8799 THEN 44 WHEN val1 BETWEEN 8800 AND 8999 THEN 45 WHEN val1 BETWEEN 9000 AND 9199 THEN 46 WHEN val1 BETWEEN 9200 AND 9399 THEN 47 WHEN val1 BETWEEN 9400 AND 9599 THEN 48 WHEN val1 BETWEEN 9600 AND 9799 THEN 49 WHEN val1 BETWEEN 9800 AND 9999 THEN 50 ELSE 0 END) FROM bench_data
```

**CASE_nested_3lvl**
```sql
SELECT SUM(r) FROM (SELECT CASE WHEN val1 >= 0 AND val1 < 1000 THEN CASE WHEN val2 < 1000 THEN CASE val3 WHEN 1 THEN 100 WHEN 2 THEN 200 WHEN 3 THEN 300 WHEN 4 THEN 400 ELSE -1 END WHEN val2 < 2000 THEN 10 WHEN val2 < 3000 THEN 20 WHEN val2 < 4000 THEN 30 ELSE -2 END WHEN val1 >= 1000 AND val1 < 2000 THEN 1 WHEN val1 >= 2000 AND val1 < 3000 THEN 2 WHEN val1 >= 3000 AND val1 < 4000 THEN 3 ELSE -3 END AS r FROM bench_data) sub
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

-- GIN indexes (tsvector, array)
-- NOTE: jsonb_data GIN index intentionally omitted to force seq scan in benchmarks
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

-- text_long: 200K rows with long strings (200-600 chars) for extreme LIKE/regex benchmarks
CREATE TABLE IF NOT EXISTS text_long (
    id          integer,
    long_text   text,
    mixed_text  text
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM text_long LIMIT 1) THEN
        RAISE NOTICE 'Populating text_long (200K rows, 200-600 char strings)...';
        INSERT INTO text_long
        SELECT i,
               md5(i::text) || md5((i+1)::text) || md5((i+2)::text) ||
               md5((i+3)::text) || md5((i+4)::text) || md5((i+5)::text) ||
               md5((i+6)::text) || md5((i+7)::text) || md5((i+8)::text) ||
               md5((i+9)::text) || md5((i+10)::text) || md5((i+11)::text) ||
               repeat(chr(97 + (i % 26)), (i % 200) + 1),
               'pfx_' || (i % 1000) || '_' || md5(i::text) || '_' ||
               md5((i*7)::text) || '_sfx'
        FROM generate_series(1, 200000) i;
    END IF;
END $$;

ANALYZE text_long;

-- ════════════════════════════════════════════════════════════════════
-- JSON text parsing benchmark table (simdjson acceleration)
-- ════════════════════════════════════════════════════════════════════

-- json_text_bench: 500K rows of JSON stored as TEXT
-- Payloads range from ~120 to ~350+ bytes, well above the 64-byte simdjson threshold.
-- Three payload tiers exercise different parsing complexity:
--   small (~120B): flat object, 4 fields
--   medium (~200B): nested object + small array
--   large (~350B): deep nesting + larger array + more fields
CREATE TABLE IF NOT EXISTS json_text_bench (
    id   integer,
    doc  text,        -- JSON stored as text (the simdjson target)
    tier smallint     -- 0=small, 1=medium, 2=large
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM json_text_bench LIMIT 1) THEN
        RAISE NOTICE 'Populating json_text_bench (500K rows, 120-350B JSON text)...';
        INSERT INTO json_text_bench
        SELECT i,
               CASE (i % 3)
               WHEN 0 THEN
                   /* ~120 bytes: flat object */
                   jsonb_build_object(
                       'id', i,
                       'name', 'user_' || i,
                       'hash', md5(i::text),
                       'score', (i % 10000) * 1.5
                   )::text
               WHEN 1 THEN
                   /* ~200 bytes: nested object + array */
                   jsonb_build_object(
                       'id', i,
                       'profile', jsonb_build_object(
                           'name', 'user_' || i,
                           'email', 'u' || i || '@test.com',
                           'level', i % 100
                       ),
                       'tags', jsonb_build_array('t1', 't2', 't3', 't4'),
                       'active', (i % 2 = 0)
                   )::text
               ELSE
                   /* ~350 bytes: deep nesting + larger array */
                   jsonb_build_object(
                       'id', i,
                       'profile', jsonb_build_object(
                           'name', 'user_' || i,
                           'email', 'u' || i || '@example.com',
                           'address', jsonb_build_object(
                               'city', 'city_' || (i % 500),
                               'zip', 10000 + (i % 90000)
                           )
                       ),
                       'scores', jsonb_build_array(
                           i % 100, (i*3) % 100, (i*7) % 100,
                           (i*11) % 100, (i*13) % 100
                       ),
                       'meta', jsonb_build_object(
                           'created', '2025-01-01',
                           'version', i % 10,
                           'hash', md5(i::text)
                       )
                   )::text
               END,
               (i % 3)::smallint
        FROM generate_series(1, 500000) i;
    END IF;
END $$;

ANALYZE json_text_bench;

-- ════════════════════════════════════════════════════════════════════
-- Composite type benchmark table (FIELDSELECT opcode)
-- ════════════════════════════════════════════════════════════════════

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname = 'bench_composite') THEN
        CREATE TYPE bench_composite AS (a int, b int, c text);
    END IF;
END $$;

CREATE TABLE IF NOT EXISTS composite_data (
    id  integer,
    rec bench_composite
);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM composite_data LIMIT 1) THEN
        RAISE NOTICE 'Populating composite_data (500K rows)...';
        INSERT INTO composite_data
        SELECT i, ROW(i, i*2, 'val_' || i)::bench_composite
        FROM generate_series(1, 500000) i;
    END IF;
END $$;

ANALYZE composite_data;

\echo 'Extra setup complete.'
\timing off
```

### Super-Wide Tables (100 / 300 / 1000 columns)

Generated by `tests/gen_wide_tables.py`. Type mix: ~60% varchar, ~20% int/bigint, ~20% boolean/numeric/timestamp. Sparse: ~10% of columns populated per row.

<details>
<summary>Click to expand DDL and population SQL (    2869 lines)</summary>

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

*Generated by `tests/bench_comprehensive.sh` on 2026-03-24*
