-- bench_setup_rle_grouped.sql — Tables with grouped same-type columns for RLE testing
-- "Grouped" tables have blocks of 20 same-type columns (triggers RLE runs)
-- "Random" tables cycle through 6 types (no RLE runs)
-- Both have mixed types → use dispatch loop, not the all-uniform fast path

\timing on

-- ================================================================
-- Grouped tables: blocks of 20 same-type columns
-- Pattern: 20 int4, 20 varchar(40), 20 bigint, 20 int4, 20 varchar(40), ...
-- This gives runs of 20, exceeding RLE_MIN_RUN=4
-- ================================================================

-- 100 columns grouped
CREATE TABLE IF NOT EXISTS rle_blk_100 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
    t text;
    blk int;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_blk_100') <= 2 THEN
        FOR i IN 1..100 LOOP
            blk := ((i - 1) / 20) % 3;  -- 0,1,2 repeating in blocks of 20
            CASE blk
                WHEN 0 THEN t := 'integer';
                WHEN 1 THEN t := 'varchar(40)';
                WHEN 2 THEN t := 'bigint';
            END CASE;
            EXECUTE format('ALTER TABLE rle_blk_100 ADD COLUMN c%s %s', lpad(i::text, 4, '0'), t);
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_blk_100 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_blk_100 (500000 rows)...';
        EXECUTE '
        INSERT INTO rle_blk_100
        SELECT i, i % 500, ' ||
            (SELECT string_agg(
                CASE ((g - 1) / 20) % 3
                    WHEN 0 THEN '(i % ' || (99+g)::text || ')'
                    WHEN 1 THEN '''v'' || (i % 1000)'
                    WHEN 2 THEN 'i::bigint * ' || ((g % 7)+1)::text
                END,
                ', ' ORDER BY g)
            FROM generate_series(1, 100) g) ||
        ' FROM generate_series(1, 500000) i';
    END IF;
END $$;
ANALYZE rle_blk_100;

-- 300 columns grouped
CREATE TABLE IF NOT EXISTS rle_blk_300 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
    t text;
    blk int;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_blk_300') <= 2 THEN
        FOR i IN 1..300 LOOP
            blk := ((i - 1) / 20) % 3;
            CASE blk
                WHEN 0 THEN t := 'integer';
                WHEN 1 THEN t := 'varchar(40)';
                WHEN 2 THEN t := 'bigint';
            END CASE;
            EXECUTE format('ALTER TABLE rle_blk_300 ADD COLUMN c%s %s', lpad(i::text, 4, '0'), t);
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_blk_300 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_blk_300 (200000 rows)...';
        EXECUTE '
        INSERT INTO rle_blk_300
        SELECT i, i % 500, ' ||
            (SELECT string_agg(
                CASE ((g - 1) / 20) % 3
                    WHEN 0 THEN '(i % ' || (99+g)::text || ')'
                    WHEN 1 THEN '''v'' || (i % 1000)'
                    WHEN 2 THEN 'i::bigint * ' || ((g % 7)+1)::text
                END,
                ', ' ORDER BY g)
            FROM generate_series(1, 300) g) ||
        ' FROM generate_series(1, 200000) i';
    END IF;
END $$;
ANALYZE rle_blk_300;

\echo 'Grouped RLE tables ready.'
\timing off
