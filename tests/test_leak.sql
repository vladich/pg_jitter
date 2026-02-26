-- test_leak.sql — Memory leak detection for JIT providers
--
-- Measures backend RSS before and after ~3000 JIT-compiled queries.
-- Uses EXECUTE to force fresh planning + JIT compilation each time.
--
-- Usage: psql -p PORT -d postgres -f tests/test_leak.sql

\set ON_ERROR_STOP on

-- Helper: read backend RSS in kB from /proc
CREATE OR REPLACE FUNCTION pg_backend_rss_kb() RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    contents text;
BEGIN
    SELECT pg_read_file('/proc/' || pg_backend_pid() || '/status')
        INTO contents;
    RETURN (regexp_match(contents, 'VmRSS:\s+(\d+)'))[1]::bigint;
END;
$$;

-- Force JIT on everything
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;

-- Small table — we want JIT overhead, not I/O
DROP TABLE IF EXISTS leak_test;
CREATE TABLE leak_test AS
    SELECT i AS id, i % 100 AS grp, (random() * 10000)::int AS val
    FROM generate_series(1, 10000) i;
ANALYZE leak_test;

-- Warmup + measure + run + measure — all in one DO block
DO $$
DECLARE
    dummy bigint;
    rss_before bigint;
    rss_after bigint;
    delta bigint;
BEGIN
    -- Warmup: a few queries to stabilize one-time allocations
    FOR i IN 1..20 LOOP
        EXECUTE 'SELECT sum(val + id * ' || i || ') FROM leak_test WHERE grp < 50'
            INTO dummy;
    END LOOP;

    -- Measure baseline
    rss_before := pg_backend_rss_kb();
    RAISE NOTICE 'RSS before: % kB', rss_before;

    -- Run 3000 queries via EXECUTE — each gets a fresh plan + JIT compilation
    FOR i IN 1..3000 LOOP
        CASE i % 6
            WHEN 0 THEN
                EXECUTE 'SELECT sum(val + id * ' || (i % 7 + 1)::text
                    || ') FROM leak_test WHERE grp < ' || (i % 80 + 10)::text
                    INTO dummy;
            WHEN 1 THEN
                EXECUTE 'SELECT count(*) FROM leak_test WHERE val > '
                    || (i % 5000)::text || ' AND id < ' || (i % 8000 + 1000)::text
                    INTO dummy;
            WHEN 2 THEN
                EXECUTE 'SELECT sum(CASE WHEN val > ' || (i % 3000)::text
                    || ' THEN val ELSE 0 END) FROM leak_test WHERE grp = '
                    || (i % 100)::text
                    INTO dummy;
            WHEN 3 THEN
                EXECUTE 'SELECT max(val) - min(val) + count(*) FROM leak_test'
                    || ' WHERE grp BETWEEN ' || (i % 50)::text
                    || ' AND ' || (i % 50 + 20)::text
                    INTO dummy;
            WHEN 4 THEN
                EXECUTE 'SELECT sum(val * id + val - id) FROM leak_test'
                    || ' WHERE val < ' || (i % 9000 + 500)::text
                    INTO dummy;
            WHEN 5 THEN
                EXECUTE 'SELECT count(*) + sum(val) FROM leak_test'
                    || ' WHERE grp = ' || (i % 100)::text
                    INTO dummy;
        END CASE;
    END LOOP;

    -- Measure after
    rss_after := pg_backend_rss_kb();
    delta := rss_after - rss_before;

    RAISE NOTICE 'RSS after:  % kB', rss_after;
    RAISE NOTICE 'Delta:      % kB', delta;
    IF delta > 5120 THEN
        RAISE NOTICE 'VERDICT:    LEAK SUSPECTED (>5 MB growth after 3000 queries)';
    ELSE
        RAISE NOTICE 'VERDICT:    OK (delta within tolerance)';
    END IF;
END $$;

DROP TABLE IF EXISTS leak_test;
DROP FUNCTION IF EXISTS pg_backend_rss_kb();
