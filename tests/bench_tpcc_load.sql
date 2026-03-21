-- TPC-C bulk data loader — set-based, ~30s for 10 warehouses
-- Expects: :warehouses variable set via psql \set

\set n_items 100000
\set n_customers 3000
\set n_orders 3000
\set n_districts 10
\set new_order_start 2101

\timing on

-- Items: 100,000 (fixed, warehouse-independent)
\echo Loading items...
INSERT INTO item (i_id, i_im_id, i_name, i_price, i_data)
SELECT i,
       (hashint4(i) & 0x7fffffff) % 10000 + 1,
       'item-' || lpad(i::text, 6, '0'),
       ((hashint4(i*7) & 0x7fffffff) % 9900 + 100)::numeric / 100,
       CASE WHEN (hashint4(i*13) & 0x7fffffff) % 10 = 0
            THEN 'ORIGINAL-' || md5(i::text)
            ELSE md5(i::text) END
FROM generate_series(1, :n_items) AS i;

-- Warehouses
\echo Loading warehouses...
INSERT INTO warehouse (w_id, w_name, w_street_1, w_street_2, w_city, w_state, w_zip, w_tax, w_ytd)
SELECT w, 'WH-' || w, 'street1-' || w, 'street2-' || w, 'city-' || w,
       chr(65 + w % 26) || chr(65 + (w * 7) % 26),
       lpad((w * 11111)::text, 9, '0'),
       ((hashint4(w) & 0x7fffffff) % 2000)::numeric / 10000,
       300000.00
FROM generate_series(1, :warehouses) AS w;

-- Districts: 10 per warehouse
\echo Loading districts...
INSERT INTO district (d_id, d_w_id, d_name, d_street_1, d_street_2, d_city, d_state, d_zip, d_tax, d_ytd, d_next_o_id)
SELECT d, w, 'D-' || d || '-W' || w, 'dst1', 'dst2', 'dcity',
       chr(65 + d % 26) || chr(65 + (d * 3) % 26),
       lpad((d * 22222)::text, 9, '0'),
       ((hashint4(d * 100 + w) & 0x7fffffff) % 2000)::numeric / 10000,
       30000.00,
       :n_orders + 1
FROM generate_series(1, :warehouses) AS w,
     generate_series(1, :n_districts) AS d;

-- Customers: 3000 per district
\echo Loading customers...
INSERT INTO customer (c_id, c_d_id, c_w_id, c_first, c_middle, c_last,
    c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_since,
    c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment,
    c_payment_cnt, c_delivery_cnt, c_data)
SELECT c, d, w,
    'first-' || c, 'OE',
    CASE WHEN c <= 1000
         THEN (ARRAY['BAR','OUGHT','ABLE','PRI','PRES','ESE','ANTI','CALLY','ATION','EING'])[1 + (c/100) % 10]
              || (ARRAY['BAR','OUGHT','ABLE','PRI','PRES','ESE','ANTI','CALLY','ATION','EING'])[1 + (c/10) % 10]
              || (ARRAY['BAR','OUGHT','ABLE','PRI','PRES','ESE','ANTI','CALLY','ATION','EING'])[1 + c % 10]
         ELSE 'LAST-' || c END,
    'cst1', 'cst2', 'ccity',
    chr(65 + c % 26) || chr(65 + (c * 3) % 26),
    lpad(((c * 33333) % 1000000000)::text, 9, '0'),
    lpad(((c * 44444) % 10000000000000000)::text, 16, '0'),
    now() - make_interval(secs => (hashint4(c * 1000 + d * 100 + w) & 0x7fffffff) % 31536000),
    CASE WHEN (hashint4(c * 17 + d + w) & 0x7fffffff) % 10 = 0 THEN 'BC' ELSE 'GC' END,
    50000.00,
    ((hashint4(c * 31 + d + w) & 0x7fffffff) % 5000)::numeric / 10000,
    -10.00, 10.00, 1, 0,
    md5(c::text || d::text || w::text)
FROM generate_series(1, :warehouses) AS w,
     generate_series(1, :n_districts) AS d,
     generate_series(1, :n_customers) AS c;

-- Orders: 3000 per district, with random ol_cnt 5-15
\echo Loading orders...
INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local)
SELECT o, d, w,
    (hashint4(o * 1000 + d * 100 + w) & 0x7fffffff) % :n_customers + 1,
    now() - make_interval(days => :n_orders - o),
    CASE WHEN o < :new_order_start THEN (hashint4(o * 7 + d + w) & 0x7fffffff) % 10 + 1 ELSE NULL END,
    (hashint4(o * 13 + d * 3 + w) & 0x7fffffff) % 11 + 5,
    1
FROM generate_series(1, :warehouses) AS w,
     generate_series(1, :n_districts) AS d,
     generate_series(1, :n_orders) AS o;

-- New orders: orders 2101-3000
\echo Loading new_orders...
INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
SELECT o, d, w
FROM generate_series(1, :warehouses) AS w,
     generate_series(1, :n_districts) AS d,
     generate_series(:new_order_start, :n_orders) AS o;

-- Order lines: variable count per order (use the ol_cnt from oorder)
\echo Loading order_lines...
INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number,
    ol_i_id, ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info)
SELECT o.o_id, o.o_d_id, o.o_w_id, ol_num,
    (hashint4(o.o_id * 100000 + o.o_d_id * 10000 + o.o_w_id * 1000 + ol_num) & 0x7fffffff) % :n_items + 1,
    o.o_w_id,
    CASE WHEN o.o_id < :new_order_start THEN o.o_entry_d ELSE NULL END,
    5,
    CASE WHEN o.o_id < :new_order_start THEN 0.00
         ELSE ((hashint4(o.o_id * 7 + ol_num) & 0x7fffffff) % 999999)::numeric / 100 END,
    'dist-info-' || lpad(ol_num::text, 2, '0')
FROM oorder o,
     LATERAL generate_series(1, o.o_ol_cnt) AS ol_num;

-- Stock: 100,000 items per warehouse
\echo Loading stock...
INSERT INTO stock (s_i_id, s_w_id, s_quantity,
    s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
    s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10,
    s_ytd, s_order_cnt, s_remote_cnt, s_data)
SELECT i, w,
    (hashint4(i * 100 + w) & 0x7fffffff) % 91 + 10,
    md5(i::text || '01'), md5(i::text || '02'), md5(i::text || '03'),
    md5(i::text || '04'), md5(i::text || '05'), md5(i::text || '06'),
    md5(i::text || '07'), md5(i::text || '08'), md5(i::text || '09'),
    md5(i::text || '10'),
    0, 0, 0,
    CASE WHEN (hashint4(i * 13 + w) & 0x7fffffff) % 10 = 0
         THEN 'ORIGINAL-' || md5(i::text)
         ELSE md5(i::text) END
FROM generate_series(1, :warehouses) AS w,
     generate_series(1, :n_items) AS i;

-- History
\echo Loading history...
INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
SELECT c, d, w, d, w, now(), 10.00, 'init-hist'
FROM generate_series(1, :warehouses) AS w,
     generate_series(1, :n_districts) AS d,
     generate_series(1, :n_customers) AS c;

\echo Data loading complete.
\timing off
