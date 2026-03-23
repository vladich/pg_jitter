-- bench_setup_rle.sql — Benchmark tables for RLE optimization testing
-- Two types per column count: group-typed (long same-type runs) vs random-typed (mixed)
--
-- Group-typed: all-int4 columns (maximum RLE benefit)
-- Random-typed: cycling through varchar/int/bigint/bool/numeric/timestamp (no long runs)

\timing on

-- ================================================================
-- Group-typed tables: all int4 columns (ideal for RLE)
-- ================================================================

-- 100 columns, all integer
CREATE TABLE IF NOT EXISTS rle_grp_100 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
BEGIN
    -- Add columns if table has only 2 columns (just created)
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_grp_100') <= 2 THEN
        FOR i IN 1..100 LOOP
            EXECUTE format('ALTER TABLE rle_grp_100 ADD COLUMN c%s integer', lpad(i::text, 4, '0'));
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_grp_100 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_grp_100 (500000 rows, 100 int4 cols)...';
        INSERT INTO rle_grp_100
        SELECT i, i % 500,
               i % 100, i % 101, i % 102, i % 103, i % 104, i % 105, i % 106, i % 107, i % 108, i % 109,
               i % 110, i % 111, i % 112, i % 113, i % 114, i % 115, i % 116, i % 117, i % 118, i % 119,
               i % 120, i % 121, i % 122, i % 123, i % 124, i % 125, i % 126, i % 127, i % 128, i % 129,
               i % 130, i % 131, i % 132, i % 133, i % 134, i % 135, i % 136, i % 137, i % 138, i % 139,
               i % 140, i % 141, i % 142, i % 143, i % 144, i % 145, i % 146, i % 147, i % 148, i % 149,
               i % 150, i % 151, i % 152, i % 153, i % 154, i % 155, i % 156, i % 157, i % 158, i % 159,
               i % 160, i % 161, i % 162, i % 163, i % 164, i % 165, i % 166, i % 167, i % 168, i % 169,
               i % 170, i % 171, i % 172, i % 173, i % 174, i % 175, i % 176, i % 177, i % 178, i % 179,
               i % 180, i % 181, i % 182, i % 183, i % 184, i % 185, i % 186, i % 187, i % 188, i % 189,
               i % 190, i % 191, i % 192, i % 193, i % 194, i % 195, i % 196, i % 197, i % 198, i % 199
        FROM generate_series(1, 500000) i;
    END IF;
END $$;
ANALYZE rle_grp_100;

-- 300 columns, all integer
CREATE TABLE IF NOT EXISTS rle_grp_300 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_grp_300') <= 2 THEN
        FOR i IN 1..300 LOOP
            EXECUTE format('ALTER TABLE rle_grp_300 ADD COLUMN c%s integer', lpad(i::text, 4, '0'));
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_grp_300 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_grp_300 (200000 rows, 300 int4 cols)...';
        EXECUTE '
        INSERT INTO rle_grp_300
        SELECT i, i % 500, ' ||
            (SELECT string_agg('i % ' || (99 + g)::text, ', ' ORDER BY g) FROM generate_series(1, 300) g) ||
        ' FROM generate_series(1, 200000) i';
    END IF;
END $$;
ANALYZE rle_grp_300;

-- 1000 columns, all integer
CREATE TABLE IF NOT EXISTS rle_grp_1000 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_grp_1000') <= 2 THEN
        FOR i IN 1..1000 LOOP
            EXECUTE format('ALTER TABLE rle_grp_1000 ADD COLUMN c%s integer', lpad(i::text, 4, '0'));
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_grp_1000 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_grp_1000 (100000 rows, 1000 int4 cols)...';
        EXECUTE '
        INSERT INTO rle_grp_1000
        SELECT i, i % 500, ' ||
            (SELECT string_agg('i % ' || (99 + g)::text, ', ' ORDER BY g) FROM generate_series(1, 1000) g) ||
        ' FROM generate_series(1, 100000) i';
    END IF;
END $$;
ANALYZE rle_grp_1000;

-- ================================================================
-- Random-typed tables: cycling types (worst case for RLE)
-- Cycle: int4, varchar, bigint, bool, numeric, timestamp (6 types, no run > 1)
-- ================================================================

-- Helper to generate random-typed CREATE TABLE
-- 100 columns
CREATE TABLE IF NOT EXISTS rle_rnd_100 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
    t text;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_rnd_100') <= 2 THEN
        FOR i IN 1..100 LOOP
            CASE i % 6
                WHEN 0 THEN t := 'integer';
                WHEN 1 THEN t := 'varchar(40)';
                WHEN 2 THEN t := 'bigint';
                WHEN 3 THEN t := 'boolean';
                WHEN 4 THEN t := 'numeric(12,4)';
                WHEN 5 THEN t := 'timestamp';
            END CASE;
            EXECUTE format('ALTER TABLE rle_rnd_100 ADD COLUMN c%s %s', lpad(i::text, 4, '0'), t);
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_rnd_100 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_rnd_100 (500000 rows, 100 mixed-type cols)...';
        EXECUTE '
        INSERT INTO rle_rnd_100
        SELECT i, i % 500, ' ||
            (SELECT string_agg(
                CASE g % 6
                    WHEN 0 THEN '(i % ' || (99+g)::text || ')'
                    WHEN 1 THEN '''v'' || (i % 1000)'
                    WHEN 2 THEN 'i::bigint * ' || ((g % 7)+1)::text
                    WHEN 3 THEN '(i % ' || ((g % 5)+2)::text || ' = 0)'
                    WHEN 4 THEN '(random()*1000)::numeric(12,4)'
                    WHEN 5 THEN '''2020-01-01''::timestamp + (i % 1000000) * interval ''1 second'''
                END,
                ', ' ORDER BY g)
            FROM generate_series(1, 100) g) ||
        ' FROM generate_series(1, 500000) i';
    END IF;
END $$;
ANALYZE rle_rnd_100;

-- 300 columns
CREATE TABLE IF NOT EXISTS rle_rnd_300 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
    t text;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_rnd_300') <= 2 THEN
        FOR i IN 1..300 LOOP
            CASE i % 6
                WHEN 0 THEN t := 'integer';
                WHEN 1 THEN t := 'varchar(40)';
                WHEN 2 THEN t := 'bigint';
                WHEN 3 THEN t := 'boolean';
                WHEN 4 THEN t := 'numeric(12,4)';
                WHEN 5 THEN t := 'timestamp';
            END CASE;
            EXECUTE format('ALTER TABLE rle_rnd_300 ADD COLUMN c%s %s', lpad(i::text, 4, '0'), t);
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_rnd_300 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_rnd_300 (200000 rows, 300 mixed-type cols)...';
        EXECUTE '
        INSERT INTO rle_rnd_300
        SELECT i, i % 500, ' ||
            (SELECT string_agg(
                CASE g % 6
                    WHEN 0 THEN '(i % ' || (99+g)::text || ')'
                    WHEN 1 THEN '''v'' || (i % 1000)'
                    WHEN 2 THEN 'i::bigint * ' || ((g % 7)+1)::text
                    WHEN 3 THEN '(i % ' || ((g % 5)+2)::text || ' = 0)'
                    WHEN 4 THEN '(random()*1000)::numeric(12,4)'
                    WHEN 5 THEN '''2020-01-01''::timestamp + (i % 1000000) * interval ''1 second'''
                END,
                ', ' ORDER BY g)
            FROM generate_series(1, 300) g) ||
        ' FROM generate_series(1, 200000) i';
    END IF;
END $$;
ANALYZE rle_rnd_300;

-- 1000 columns
CREATE TABLE IF NOT EXISTS rle_rnd_1000 (
    id integer,
    grp integer
);
DO $$
DECLARE
    i int;
    t text;
BEGIN
    IF (SELECT count(*) FROM information_schema.columns WHERE table_name = 'rle_rnd_1000') <= 2 THEN
        FOR i IN 1..1000 LOOP
            CASE i % 6
                WHEN 0 THEN t := 'integer';
                WHEN 1 THEN t := 'varchar(40)';
                WHEN 2 THEN t := 'bigint';
                WHEN 3 THEN t := 'boolean';
                WHEN 4 THEN t := 'numeric(12,4)';
                WHEN 5 THEN t := 'timestamp';
            END CASE;
            EXECUTE format('ALTER TABLE rle_rnd_1000 ADD COLUMN c%s %s', lpad(i::text, 4, '0'), t);
        END LOOP;
    END IF;
END $$;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM rle_rnd_1000 LIMIT 1) THEN
        RAISE NOTICE 'Populating rle_rnd_1000 (100000 rows, 1000 mixed-type cols)...';
        EXECUTE '
        INSERT INTO rle_rnd_1000
        SELECT i, i % 500, ' ||
            (SELECT string_agg(
                CASE g % 6
                    WHEN 0 THEN '(i % ' || (99+g)::text || ')'
                    WHEN 1 THEN '''v'' || (i % 1000)'
                    WHEN 2 THEN 'i::bigint * ' || ((g % 7)+1)::text
                    WHEN 3 THEN '(i % ' || ((g % 5)+2)::text || ' = 0)'
                    WHEN 4 THEN '(random()*1000)::numeric(12,4)'
                    WHEN 5 THEN '''2020-01-01''::timestamp + (i % 1000000) * interval ''1 second'''
                END,
                ', ' ORDER BY g)
            FROM generate_series(1, 1000) g) ||
        ' FROM generate_series(1, 100000) i';
    END IF;
END $$;
ANALYZE rle_rnd_1000;

\echo 'RLE benchmark table setup complete.'
\timing off
