-- TPC-C schema for PostgreSQL (pgbench-based implementation)
-- Follows the TPC-C specification with 9 tables.
--
-- Scale: :warehouses (1 warehouse ≈ 100MB data)

-- ================================================================
-- Schema
-- ================================================================
DROP TABLE IF EXISTS order_line CASCADE;
DROP TABLE IF EXISTS new_order CASCADE;
DROP TABLE IF EXISTS oorder CASCADE;
DROP TABLE IF EXISTS history CASCADE;
DROP TABLE IF EXISTS stock CASCADE;
DROP TABLE IF EXISTS item CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS district CASCADE;
DROP TABLE IF EXISTS warehouse CASCADE;

CREATE TABLE warehouse (
    w_id        INTEGER NOT NULL,
    w_name      VARCHAR(10),
    w_street_1  VARCHAR(20),
    w_street_2  VARCHAR(20),
    w_city      VARCHAR(20),
    w_state     CHAR(2),
    w_zip       CHAR(9),
    w_tax       DECIMAL(4,4),
    w_ytd       DECIMAL(12,2),
    PRIMARY KEY (w_id)
);

CREATE TABLE district (
    d_id        INTEGER NOT NULL,
    d_w_id      INTEGER NOT NULL,
    d_name      VARCHAR(10),
    d_street_1  VARCHAR(20),
    d_street_2  VARCHAR(20),
    d_city      VARCHAR(20),
    d_state     CHAR(2),
    d_zip       CHAR(9),
    d_tax       DECIMAL(4,4),
    d_ytd       DECIMAL(12,2),
    d_next_o_id INTEGER,
    PRIMARY KEY (d_w_id, d_id)
);

CREATE TABLE customer (
    c_id          INTEGER NOT NULL,
    c_d_id        INTEGER NOT NULL,
    c_w_id        INTEGER NOT NULL,
    c_first       VARCHAR(16),
    c_middle      CHAR(2),
    c_last        VARCHAR(16),
    c_street_1    VARCHAR(20),
    c_street_2    VARCHAR(20),
    c_city        VARCHAR(20),
    c_state       CHAR(2),
    c_zip         CHAR(9),
    c_phone       CHAR(16),
    c_since       TIMESTAMP,
    c_credit      CHAR(2),
    c_credit_lim  DECIMAL(12,2),
    c_discount    DECIMAL(4,4),
    c_balance     DECIMAL(12,2),
    c_ytd_payment DECIMAL(12,2),
    c_payment_cnt INTEGER,
    c_delivery_cnt INTEGER,
    c_data        VARCHAR(500),
    PRIMARY KEY (c_w_id, c_d_id, c_id)
);

CREATE TABLE history (
    h_c_id    INTEGER,
    h_c_d_id  INTEGER,
    h_c_w_id  INTEGER,
    h_d_id    INTEGER,
    h_w_id    INTEGER,
    h_date    TIMESTAMP,
    h_amount  DECIMAL(6,2),
    h_data    VARCHAR(24)
);

CREATE TABLE oorder (
    o_id          INTEGER NOT NULL,
    o_d_id        INTEGER NOT NULL,
    o_w_id        INTEGER NOT NULL,
    o_c_id        INTEGER,
    o_entry_d     TIMESTAMP,
    o_carrier_id  INTEGER,
    o_ol_cnt      INTEGER,
    o_all_local   INTEGER,
    PRIMARY KEY (o_w_id, o_d_id, o_id)
);

CREATE TABLE new_order (
    no_o_id   INTEGER NOT NULL,
    no_d_id   INTEGER NOT NULL,
    no_w_id   INTEGER NOT NULL,
    PRIMARY KEY (no_w_id, no_d_id, no_o_id)
);

CREATE TABLE item (
    i_id    INTEGER NOT NULL,
    i_im_id INTEGER,
    i_name  VARCHAR(24),
    i_price DECIMAL(5,2),
    i_data  VARCHAR(50),
    PRIMARY KEY (i_id)
);

CREATE TABLE stock (
    s_i_id       INTEGER NOT NULL,
    s_w_id       INTEGER NOT NULL,
    s_quantity   INTEGER,
    s_dist_01    CHAR(24),
    s_dist_02    CHAR(24),
    s_dist_03    CHAR(24),
    s_dist_04    CHAR(24),
    s_dist_05    CHAR(24),
    s_dist_06    CHAR(24),
    s_dist_07    CHAR(24),
    s_dist_08    CHAR(24),
    s_dist_09    CHAR(24),
    s_dist_10    CHAR(24),
    s_ytd        INTEGER,
    s_order_cnt  INTEGER,
    s_remote_cnt INTEGER,
    s_data       VARCHAR(50),
    PRIMARY KEY (s_w_id, s_i_id)
);

CREATE TABLE order_line (
    ol_o_id         INTEGER NOT NULL,
    ol_d_id         INTEGER NOT NULL,
    ol_w_id         INTEGER NOT NULL,
    ol_number       INTEGER NOT NULL,
    ol_i_id         INTEGER,
    ol_supply_w_id  INTEGER,
    ol_delivery_d   TIMESTAMP,
    ol_quantity      INTEGER,
    ol_amount        DECIMAL(6,2),
    ol_dist_info     CHAR(24),
    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
);

-- ================================================================
-- Indexes (beyond PKs)
-- ================================================================
CREATE INDEX idx_customer_name ON customer (c_w_id, c_d_id, c_last, c_first);
CREATE INDEX idx_oorder_carrier ON oorder (o_w_id, o_d_id, o_carrier_id, o_id);
CREATE INDEX idx_order_line_oid ON order_line (ol_w_id, ol_d_id, ol_o_id);
