#!/bin/bash
# bench_jit_showcase.sh — Showcase benchmark highlighting pg_jitter's strengths
#
# Realistic customer interaction analytics on a CRM/support system:
# text pattern matching, multi-level classification, time-based scoring.
# Designed so expression evaluation dominates runtime (70%+).
set +e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGPORT="${PGPORT:-5528}"
PGDB="bench_showcase"
SKIP_LOAD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2;;
        --port)      PGPORT="$2";    shift 2;;
        --skip-load) SKIP_LOAD=1;    shift;;
        *) echo "Unknown: $1"; exit 1;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
psql_cmd() { "$PGBIN/psql" -p "$PGPORT" -d "$PGDB" -X "$@"; }

if [ "$SKIP_LOAD" -eq 0 ]; then
    echo "Creating database..."
    "$PGBIN/psql" -p "$PGPORT" -d postgres -Xc "DROP DATABASE IF EXISTS $PGDB;" 2>/dev/null
    "$PGBIN/psql" -p "$PGPORT" -d postgres -Xc "CREATE DATABASE $PGDB;"

    echo "Loading data..."
    psql_cmd -q << 'SETUP'
-- Customer support interactions: 2M rows, text-heavy
CREATE TABLE interactions (
    id          bigint GENERATED ALWAYS AS IDENTITY,
    ts          timestamptz NOT NULL,
    customer_id int NOT NULL,
    channel     text NOT NULL,      -- 'email','chat','phone','social','web'
    agent_id    int,
    subject     text NOT NULL,      -- free-text subject line
    body        text NOT NULL,      -- message body
    category    text,               -- assigned category
    priority    smallint DEFAULT 3, -- 1=critical .. 5=low
    resolution_mins int,
    satisfaction smallint,           -- 1-5 CSAT score
    tags        text[]              -- array of tags
);

INSERT INTO interactions (ts, customer_id, channel, agent_id, subject, body,
    category, priority, resolution_mins, satisfaction, tags)
SELECT
    '2023-01-01'::timestamptz + (random() * 730 * 86400) * interval '1 second',
    (random() * 99999 + 1)::int,
    (ARRAY['email','chat','phone','social','web'])[1 + (i % 5)],
    (random() * 200 + 1)::int,
    -- Realistic subject lines with searchable keywords
    (ARRAY[
        'Cannot login to my account',
        'Billing issue - double charged',
        'Shipping delay on order #' || (i % 100000),
        'Product defective - requesting refund',
        'How to upgrade my subscription',
        'Password reset not working',
        'Account locked after failed attempts',
        'Request for invoice copy',
        'Cancel my subscription immediately',
        'Bug report: app crashes on startup',
        'Feature request: dark mode support',
        'Complaint about customer service',
        'Technical support needed urgently',
        'Integration API returning errors',
        'Data export not working properly',
        'Premium plan pricing question',
        'Refund status inquiry',
        'Delivery address change request',
        'Promotional offer not applied',
        'Security concern - suspicious activity'
    ])[1 + (i % 20)],
    -- Body with varied content for ILIKE matching
    'Dear support team, ' ||
    (ARRAY[
        'I have been experiencing issues with my account access. Error code: ERR-' || (i % 999),
        'Please review my recent transaction. Reference: TXN-' || (i % 50000),
        'The product I received is not as described. Order: ORD-' || (i % 80000),
        'I need urgent assistance with a critical production issue.',
        'Could you please help me understand the premium features?',
        'My subscription was charged twice this month. Please investigate.',
        'The mobile app keeps crashing after the latest update version ' || (i % 50) || '.0',
        'I would like to request a full refund for order ORD-' || (i % 80000),
        'The API endpoint /v2/export returns HTTP 500 intermittently.',
        'Please escalate this ticket - waiting for 3 days without response.'
    ])[1 + (i % 10)] ||
    ' Thank you for your assistance. Customer ID: CID-' || ((i * 7) % 100000),
    (ARRAY['billing','technical','shipping','account','general',
           'refund','subscription','security','feedback','integration'])[1 + (i % 10)],
    1 + (i % 5),
    CASE WHEN random() < 0.8 THEN (random() * 480 + 5)::int ELSE NULL END,
    CASE WHEN random() < 0.7 THEN 1 + (random() * 4)::int ELSE NULL END,
    ARRAY(SELECT (ARRAY['urgent','escalated','vip','new_customer','returning',
                        'enterprise','trial','churning','upsell','bug',
                        'feature_request','complaint','praise','billing_issue',
                        'data_loss','security','compliance','mobile','api','web']
          )[1 + ((i * (g+1)) % 20)]
          FROM generate_series(1, 1 + (i % 4)) g)
FROM generate_series(1, 2000000) i;

CREATE INDEX idx_interactions_ts ON interactions (ts);
CREATE INDEX idx_interactions_cust ON interactions (customer_id);
ANALYZE interactions;

CREATE EXTENSION IF NOT EXISTS pg_prewarm;
SELECT pg_prewarm('interactions'::regclass, 'buffer');
SELECT pg_prewarm('idx_interactions_ts'::regclass, 'buffer');
SETUP
    echo "Done. $(psql_cmd -tA -c "SELECT count(*) FROM interactions;") rows loaded."
fi

echo ""
echo "=== pg_jitter Showcase Benchmark ==="
echo ""

run_query() {
    local label="$1" query="$2"
    printf "%-35s" "$label"
    for backend in interp sljit asmjit mir; do
        jit="on"; [ "$backend" = "interp" ] && jit="off"
        set_be=""; [ "$backend" != "interp" ] && set_be="SET pg_jitter.backend = '$backend';"
        med=$(psql_cmd -tA -c "
SET jit = $jit; SET jit_above_cost = 0; SET jit_inline_above_cost = 500000;
SET jit_optimize_above_cost = 500000; SET max_parallel_workers_per_gather = 0;
$set_be
$query;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON) $query;
" 2>/dev/null | grep "Execution Time" | sort -t: -k2 -n | sed -n '2p' | sed 's/.*: //' | sed 's/ ms//')
        [ -z "$med" ] && med="ERR"
        echo "${label},${backend},${med}" >> /tmp/showcase_results.csv
        if [ "$backend" = "interp" ]; then
            base="$med"; printf " %8s" "$med"
        elif [ "$med" != "ERR" ] && [ "$base" != "ERR" ]; then
            spd=$(echo "scale=2; $base / $med" | bc 2>/dev/null)
            printf "  %7s (${spd}x)" "$med"
        else printf "  %17s" "$med"; fi
    done; echo ""
}

echo "query,backend,exec_time_ms" > /tmp/showcase_results.csv
printf "%-35s %8s  %17s  %17s  %17s\n" "Query" "No JIT" "sljit" "asmjit" "mir"
echo "-------------------------------------------------------------------------------------------------------------"

# 1. ILIKE subject line search — PCRE2 on every row
run_query "Subject_ILIKE_search" \
"SELECT channel, count(*),
       round(avg(resolution_mins), 1) AS avg_resolution,
       round(avg(satisfaction::numeric), 2) AS avg_csat
FROM interactions
WHERE subject ILIKE '%refund%' OR subject ILIKE '%cancel%'
   OR subject ILIKE '%billing%' OR subject ILIKE '%charged%'
GROUP BY channel ORDER BY 2 DESC"

# 2. Body text regex — PCRE2 pattern extraction
run_query "Body_regex_extract" \
"SELECT
  CASE WHEN body ~ 'ERR-[0-9]+' THEN 'error_code'
       WHEN body ~ 'TXN-[0-9]+' THEN 'transaction'
       WHEN body ~ 'ORD-[0-9]+' THEN 'order_ref'
       WHEN body ~ 'HTTP [45][0-9]{2}' THEN 'http_error'
       WHEN body ~ 'version [0-9]+' THEN 'version_ref'
       ELSE 'unstructured' END AS body_type,
  count(*), round(avg(priority), 2) AS avg_priority
FROM interactions
WHERE ts >= '2023-06-01' AND ts < '2024-01-01'
GROUP BY 1 ORDER BY 2 DESC"

# 3. Multi-level CASE classification — 25+ branches
run_query "Ticket_classification" \
"SELECT
  CASE WHEN subject ILIKE '%login%' OR subject ILIKE '%password%' OR subject ILIKE '%locked%' THEN 'authentication'
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
  channel,
  count(*) AS tickets,
  round(avg(resolution_mins), 0) AS avg_resolve_min,
  round(avg(satisfaction::numeric), 2) AS csat,
  count(*) FILTER (WHERE satisfaction <= 2) AS detractors,
  count(*) FILTER (WHERE satisfaction >= 4) AS promoters
FROM interactions
WHERE ts >= '2024-01-01'
GROUP BY 1, 2, 3
ORDER BY tickets DESC"

# 4. Agent performance scoring — heavy arithmetic per-row
run_query "Agent_scoring" \
"SELECT agent_id,
  count(*) AS tickets,
  round(avg(resolution_mins), 1) AS avg_resolution,
  round(avg(satisfaction::numeric), 2) AS avg_csat,
  count(*) FILTER (WHERE satisfaction >= 4)::numeric / NULLIF(count(*) FILTER (WHERE satisfaction IS NOT NULL), 0) AS promoter_rate,
  count(*) FILTER (WHERE resolution_mins < 30)::numeric / NULLIF(count(*), 0) AS fast_resolve_rate,
  round((
    COALESCE(avg(satisfaction::numeric), 3) * 20 +
    LEAST(100, 100.0 * count(*) FILTER (WHERE resolution_mins < 60) / NULLIF(count(*), 0)) * 0.3 +
    LEAST(100, count(*)::numeric / 30) * 0.2 -
    count(*) FILTER (WHERE satisfaction <= 2)::numeric / NULLIF(count(*), 0) * 50
  )::numeric, 1) AS performance_score
FROM interactions
WHERE ts >= '2024-01-01'
GROUP BY agent_id
ORDER BY performance_score DESC"

# 5. Time-of-day pattern with interval arithmetic
run_query "Hourly_pattern" \
"SELECT
  extract(hour FROM ts) AS hour,
  CASE WHEN extract(hour FROM ts) < 6 THEN 'night'
       WHEN extract(hour FROM ts) < 12 THEN 'morning'
       WHEN extract(hour FROM ts) < 18 THEN 'afternoon'
       ELSE 'evening' END AS shift,
  channel,
  count(*) AS tickets,
  round(avg(extract(epoch FROM (ts - '2024-01-01'::timestamptz))), 0) AS avg_offset_seconds,
  round(avg(resolution_mins), 1) AS avg_resolve,
  round(avg(satisfaction::numeric), 2) AS csat
FROM interactions
WHERE ts >= '2024-01-01' AND ts < '2024-07-01'
GROUP BY 1, 2, 3
ORDER BY 1, 3"

# 6. Priority IN-list with text classification
run_query "Priority_channel_matrix" \
"SELECT
  priority,
  channel,
  CASE WHEN category IN ('billing','refund','subscription') THEN 'revenue'
       WHEN category IN ('technical','integration','security') THEN 'engineering'
       WHEN category IN ('shipping','account') THEN 'operations'
       ELSE 'other' END AS dept,
  count(*) AS tickets,
  round(avg(resolution_mins), 0) AS resolve_min,
  count(*) FILTER (WHERE satisfaction IS NOT NULL AND satisfaction <= 2) AS unhappy
FROM interactions
WHERE priority IN (1, 2, 3)
  AND channel IN ('email', 'chat', 'phone')
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3"

# 7. ILIKE body search — full text scan with pattern matching
run_query "Body_keyword_analysis" \
"SELECT
  CASE WHEN body ILIKE '%urgent%' OR body ILIKE '%critical%' OR body ILIKE '%asap%' THEN 'urgent_language'
       WHEN body ILIKE '%please escalate%' OR body ILIKE '%waiting%' OR body ILIKE '%no response%' THEN 'frustrated'
       WHEN body ILIKE '%thank%' OR body ILIKE '%appreciate%' OR body ILIKE '%great%' THEN 'positive'
       WHEN body ILIKE '%refund%' OR body ILIKE '%money back%' OR body ILIKE '%charged twice%' THEN 'financial'
       WHEN body ILIKE '%crash%' OR body ILIKE '%error%' OR body ILIKE '%bug%' OR body ILIKE '%broken%' THEN 'technical'
       ELSE 'neutral' END AS sentiment,
  count(*) AS tickets,
  round(avg(priority), 2) AS avg_priority,
  round(avg(satisfaction::numeric), 2) AS avg_csat,
  round(avg(resolution_mins), 0) AS avg_resolve
FROM interactions
WHERE ts >= '2024-01-01'
GROUP BY 1
ORDER BY tickets DESC"

# 8. Customer churn risk — complex scoring with ILIKE + CASE + arithmetic
run_query "Churn_risk_scoring" \
"WITH cust_metrics AS (
  SELECT customer_id,
    count(*) AS ticket_count,
    count(*) FILTER (WHERE priority <= 2) AS critical_count,
    avg(satisfaction::numeric) AS avg_csat,
    max(ts) AS last_contact,
    count(*) FILTER (WHERE subject ILIKE '%cancel%' OR subject ILIKE '%refund%') AS churn_signals,
    count(*) FILTER (WHERE body ILIKE '%disappointed%' OR body ILIKE '%terrible%'
                       OR body ILIKE '%worst%' OR body ILIKE '%unacceptable%') AS negative_sentiment
  FROM interactions
  WHERE ts >= '2024-01-01'
  GROUP BY customer_id
)
SELECT
  CASE WHEN churn_score >= 80 THEN 'critical'
       WHEN churn_score >= 60 THEN 'high'
       WHEN churn_score >= 40 THEN 'medium'
       WHEN churn_score >= 20 THEN 'low'
       ELSE 'safe' END AS risk_level,
  count(*) AS customers,
  round(avg(churn_score), 1) AS avg_score,
  round(avg(ticket_count), 1) AS avg_tickets,
  round(avg(avg_csat), 2) AS avg_csat
FROM (
  SELECT *,
    round((
      COALESCE(churn_signals * 20, 0) +
      COALESCE(negative_sentiment * 15, 0) +
      COALESCE(critical_count * 10, 0) +
      CASE WHEN avg_csat IS NULL THEN 20
           WHEN avg_csat < 2 THEN 40
           WHEN avg_csat < 3 THEN 25
           WHEN avg_csat < 4 THEN 10
           ELSE 0 END +
      CASE WHEN last_contact < now() - interval '90 day' THEN 30
           WHEN last_contact < now() - interval '60 day' THEN 20
           WHEN last_contact < now() - interval '30 day' THEN 10
           ELSE 0 END
    )::numeric / GREATEST(ticket_count, 1) * 10, 1) AS churn_score
  FROM cust_metrics
) scored
GROUP BY 1
ORDER BY avg_score DESC"

echo ""
echo "============================================================================================================="
python3 -c "
import csv, math
data = {}; queries = []; backends = []
with open('/tmp/showcase_results.csv') as f:
    for row in csv.DictReader(f):
        q, b, t = row['query'], row['backend'], row['exec_time_ms']
        if not t or t == 'ERR': continue
        if b not in backends: backends.append(b)
        if q not in data: data[q] = {}; queries.append(q)
        data[q][b] = float(t)
jit = [b for b in backends if b != 'interp']
for b in jit:
    vals = [(q, data[q]['interp'], data[q].get(b)) for q in queries if data[q].get(b)]
    total_i = sum(v[1] for v in vals)
    total_j = sum(v[2] for v in vals)
    geo = [math.log(v[1]/v[2]) for v in vals]
    gm = math.exp(sum(geo)/len(geo))
    nf = sum(1 for g in geo if g > 0)
    print(f'{b:>8}: total={total_j:.0f}ms  saved={total_i-total_j:.0f}ms  ({total_i/total_j:.2f}x)  geomean={gm:.3f}x  ({nf}/{len(geo)} faster)')
"
