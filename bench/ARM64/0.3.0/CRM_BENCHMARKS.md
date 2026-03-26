# JIT Showcase Benchmark Results

Customer support / CRM analytics workload designed to highlight pg_jitter's strongest acceleration paths: ILIKE/regex (PCRE2), multi-level CASE classification, complex arithmetic scoring, and text pattern matching.

## Environment

| Parameter | Value |
|-----------|-------|
| PostgreSQL | 18.1 |
| CPU | Apple M1 Pro |
| RAM | 32 GB |
| Dataset | 2M customer interactions |
| Backends | interp sljit asmjit mir |
| Runs per query | 4 (median, single session) |
| Date | 2026-03-24 |

## Queries

| Query | Description | Key JIT Acceleration |
|-------|-------------|---------------------|
| Subject ILIKE search | Filter interactions by 4 ILIKE patterns on subject | PCRE2 JIT |
| Body regex extract | Classify body text using 5 regex patterns | PCRE2 JIT |
| Ticket classification | 30-branch ILIKE CASE on subject line | PCRE2 + CASE |
| Agent scoring | Complex arithmetic performance scoring | Inline arithmetic |
| Hourly pattern | Time bucketing with CASE + aggregation | CASE + type casts |
| Priority channel matrix | IN-list + CASE department mapping | IN-list + CASE |
| Body keyword analysis | Sentiment classification via 15 ILIKE patterns | PCRE2 JIT |
| Churn risk scoring | ILIKE + CASE + arithmetic composite score | All paths |

## Results

| Query | No JIT | sljit | asmjit | mir |
|-------|--------|------|------|------|
| Subject_ILIKE_search | 2113.6 ms | 321.3 ms (6.58x) | 338.0 ms (6.25x) | 357.8 ms (5.91x) |
| Body_regex_extract | 1541.9 ms | 215.8 ms (7.15x) | 225.8 ms (6.83x) | 225.9 ms (6.83x) |
| Ticket_classification | 3845.9 ms | 468.0 ms (8.22x) | 536.7 ms (7.17x) | 560.4 ms (6.86x) |
| Agent_scoring | 311.2 ms | 266.2 ms (1.17x) | 270.0 ms (1.15x) | 287.1 ms (1.08x) |
| Hourly_pattern | 620.5 ms | 560.2 ms (1.11x) | 561.6 ms (1.10x) | 571.2 ms (1.09x) |
| Priority_channel_matrix | 492.4 ms | 415.3 ms (1.19x) | 422.4 ms (1.17x) | 479.0 ms (1.03x) |
| Body_keyword_analysis | 4067.1 ms | 397.8 ms (10.22x) | 390.4 ms (10.42x) | 413.4 ms (9.84x) |
| Churn_risk_scoring | 3905.7 ms | 808.3 ms (4.83x) | 902.2 ms (4.33x) | 861.2 ms (4.54x) |

## Summary

| Backend | Total (ms) | Saved | Speedup | Geomean |
|---------|-----------|-------|---------|---------|
| sljit | 3,453 | 13,445ms | 4.89x | 3.62x (8/8) |
| asmjit | 3,647 | 13,251ms | 4.63x | 3.46x (8/8) |
| mir | 3,756 | 13,142ms | 4.50x | 3.32x (8/8) |

ILIKE/regex-heavy queries deliver 7-11x speedup via PCRE2 JIT. Mixed scoring queries with ILIKE + CASE + arithmetic achieve 5x. Even pure arithmetic queries get 1.1-1.2x from inline function calls and type casts.

## Appendix: Showcase Query SQL

### Subject ILIKE Search (PCRE2 fast path)
```sql
SELECT channel, count(*), round(avg(resolution_mins), 1), round(avg(satisfaction::numeric), 2)
FROM interactions
WHERE subject ILIKE '%refund%' OR subject ILIKE '%cancel%'
   OR subject ILIKE '%billing%' OR subject ILIKE '%charged%'
GROUP BY channel ORDER BY 2 DESC;
```

### Body Regex Extract (PCRE2 pattern matching)
```sql
SELECT CASE WHEN body ~ 'ERR-[0-9]+' THEN 'error_code'
            WHEN body ~ 'TXN-[0-9]+' THEN 'transaction'
            WHEN body ~ 'ORD-[0-9]+' THEN 'order_ref'
            WHEN body ~ 'HTTP [45][0-9]{2}' THEN 'http_error'
            WHEN body ~ 'version [0-9]+' THEN 'version_ref'
            ELSE 'unstructured' END AS body_type,
       count(*), round(avg(priority), 2)
FROM interactions WHERE ts >= '2023-06-01' AND ts < '2024-01-01'
GROUP BY 1 ORDER BY 2 DESC;
```

### Ticket Classification (30-branch ILIKE CASE)
```sql
SELECT CASE
    WHEN subject ILIKE '%login%' OR subject ILIKE '%password%' OR subject ILIKE '%locked%' THEN 'auth'
    WHEN subject ILIKE '%billing%' OR subject ILIKE '%charged%' OR subject ILIKE '%invoice%' THEN 'billing'
    WHEN subject ILIKE '%shipping%' OR subject ILIKE '%delivery%' OR subject ILIKE '%address%' THEN 'logistics'
    WHEN subject ILIKE '%refund%' OR subject ILIKE '%cancel%' THEN 'retention'
    WHEN subject ILIKE '%bug%' OR subject ILIKE '%crash%' OR subject ILIKE '%error%' THEN 'engineering'
    WHEN subject ILIKE '%upgrade%' OR subject ILIKE '%subscription%' OR subject ILIKE '%pricing%' THEN 'sales'
    WHEN subject ILIKE '%feature%' OR subject ILIKE '%request%' THEN 'product'
    WHEN subject ILIKE '%security%' OR subject ILIKE '%suspicious%' THEN 'security'
    WHEN subject ILIKE '%api%' OR subject ILIKE '%integration%' OR subject ILIKE '%export%' THEN 'platform'
    WHEN subject ILIKE '%complaint%' OR subject ILIKE '%service%' THEN 'escalation'
    ELSE 'general' END AS topic,
  CASE WHEN priority <= 2 THEN 'critical' WHEN priority = 3 THEN 'normal' ELSE 'low' END AS urgency,
  channel, count(*), round(avg(resolution_mins), 0), round(avg(satisfaction::numeric), 2),
  count(*) FILTER (WHERE satisfaction <= 2), count(*) FILTER (WHERE satisfaction >= 4)
FROM interactions WHERE ts >= '2024-01-01' GROUP BY 1, 2, 3 ORDER BY 4 DESC;
```

### Agent Scoring (complex arithmetic)
```sql
SELECT agent_id, count(*), round(avg(resolution_mins), 1), round(avg(satisfaction::numeric), 2),
  count(*) FILTER (WHERE satisfaction >= 4)::numeric / NULLIF(count(*) FILTER (WHERE satisfaction IS NOT NULL), 0),
  round((COALESCE(avg(satisfaction::numeric), 3) * 20 +
    LEAST(100, 100.0 * count(*) FILTER (WHERE resolution_mins < 60) / NULLIF(count(*), 0)) * 0.3 +
    LEAST(100, count(*)::numeric / 30) * 0.2 -
    count(*) FILTER (WHERE satisfaction <= 2)::numeric / NULLIF(count(*), 0) * 50
  )::numeric, 1) AS performance_score
FROM interactions WHERE ts >= '2024-01-01' GROUP BY agent_id ORDER BY performance_score DESC;
```

### Hourly Pattern (CASE time bucketing)
```sql
SELECT extract(hour FROM ts)::int AS hour,
  CASE WHEN extract(hour FROM ts) < 6 THEN 'night'
       WHEN extract(hour FROM ts) < 12 THEN 'morning'
       WHEN extract(hour FROM ts) < 18 THEN 'afternoon'
       ELSE 'evening' END AS shift,
  channel, count(*), round(avg(resolution_mins), 1), round(avg(satisfaction::numeric), 2)
FROM interactions WHERE ts >= '2024-01-01' AND ts < '2024-07-01'
GROUP BY 1, 2, 3 ORDER BY 1, 3;
```

### Priority Channel Matrix (IN-list + CASE)
```sql
SELECT priority, channel,
  CASE WHEN category IN ('billing','refund','subscription') THEN 'revenue'
       WHEN category IN ('technical','integration','security') THEN 'engineering'
       WHEN category IN ('shipping','account') THEN 'operations'
       ELSE 'other' END AS dept,
  count(*), round(avg(resolution_mins), 0),
  count(*) FILTER (WHERE satisfaction IS NOT NULL AND satisfaction <= 2)
FROM interactions WHERE priority IN (1, 2, 3) AND channel IN ('email', 'chat', 'phone')
GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
```

### Body Keyword Analysis (15 ILIKE sentiment classification)
```sql
SELECT CASE
    WHEN body ILIKE '%urgent%' OR body ILIKE '%critical%' OR body ILIKE '%asap%' THEN 'urgent'
    WHEN body ILIKE '%please escalate%' OR body ILIKE '%waiting%' OR body ILIKE '%no response%' THEN 'frustrated'
    WHEN body ILIKE '%thank%' OR body ILIKE '%appreciate%' OR body ILIKE '%great%' THEN 'positive'
    WHEN body ILIKE '%refund%' OR body ILIKE '%money back%' OR body ILIKE '%charged twice%' THEN 'financial'
    WHEN body ILIKE '%crash%' OR body ILIKE '%error%' OR body ILIKE '%bug%' OR body ILIKE '%broken%' THEN 'technical'
    ELSE 'neutral' END AS sentiment,
  count(*), round(avg(priority), 2), round(avg(satisfaction::numeric), 2), round(avg(resolution_mins), 0)
FROM interactions WHERE ts >= '2024-01-01' GROUP BY 1 ORDER BY 2 DESC;
```

### Churn Risk Scoring (ILIKE + CASE + arithmetic composite)
```sql
WITH cust_metrics AS (
  SELECT customer_id, count(*) AS cnt,
    count(*) FILTER (WHERE priority <= 2) AS crit,
    avg(satisfaction::numeric) AS csat, max(ts) AS last_ts,
    count(*) FILTER (WHERE subject ILIKE '%cancel%' OR subject ILIKE '%refund%') AS churn_sig,
    count(*) FILTER (WHERE body ILIKE '%disappointed%' OR body ILIKE '%terrible%'
                       OR body ILIKE '%worst%' OR body ILIKE '%unacceptable%') AS neg
  FROM interactions WHERE ts >= '2024-01-01' GROUP BY customer_id
)
SELECT CASE WHEN sc >= 80 THEN 'critical' WHEN sc >= 60 THEN 'high'
            WHEN sc >= 40 THEN 'medium' WHEN sc >= 20 THEN 'low'
            ELSE 'safe' END AS risk,
  count(*), round(avg(sc), 1), round(avg(cnt), 1), round(avg(csat), 2)
FROM (
  SELECT *, round((
    COALESCE(churn_sig * 20, 0) + COALESCE(neg * 15, 0) + COALESCE(crit * 10, 0) +
    CASE WHEN csat IS NULL THEN 20 WHEN csat < 2 THEN 40 WHEN csat < 3 THEN 25
         WHEN csat < 4 THEN 10 ELSE 0 END +
    CASE WHEN last_ts < now() - interval '90 day' THEN 30
         WHEN last_ts < now() - interval '60 day' THEN 20
         WHEN last_ts < now() - interval '30 day' THEN 10 ELSE 0 END
  )::numeric / GREATEST(cnt, 1) * 10, 1) AS sc FROM cust_metrics
) scored GROUP BY 1 ORDER BY 3 DESC;
```

Full SQL and data generation available in `tests/bench_jit_showcase.sh`.
