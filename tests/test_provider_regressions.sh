#!/usr/bin/env bash
# test_provider_regressions.sh - targeted pg_jitter provider correctness checks.
#
# These checks cover bugs that are easy to miss in broad pg_regress output:
#   - expected SQL errors must not crash the backend
#   - numeric arithmetic must match PostgreSQL's exact Numeric semantics
set -euo pipefail

PG_CONFIG="${PG_CONFIG:-pg_config}"
PGHOST="${PGHOST:-/tmp}"
PGPORT="${PGPORT:-5432}"
PGDB="${PGDB:-postgres}"
BACKEND="${BACKEND:-all}"

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --pg-config PATH       pg_config to use (default: $PG_CONFIG)
  --host HOST            PostgreSQL host/socket directory (default: $PGHOST)
  --port PORT            PostgreSQL port (default: $PGPORT)
  --db DB                Database name (default: $PGDB)
  --backend NAME|all     Backend to test: sljit, asmjit, mir, auto, or all
                         (default: $BACKEND)
  -h, --help             Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --pg-config) PG_CONFIG="$2"; shift 2 ;;
        --host) PGHOST="$2"; shift 2 ;;
        --port) PGPORT="$2"; shift 2 ;;
        --db) PGDB="$2"; shift 2 ;;
        --backend) BACKEND="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

PGBIN="$("$PG_CONFIG" --bindir)"
PKGLIBDIR="$("$PG_CONFIG" --pkglibdir)"
PG_LIBDIR="$("$PG_CONFIG" --libdir)"
PKGLIB_CANDIDATES=("$PKGLIBDIR")
if [ -d "$PG_LIBDIR/postgresql" ] && [ "$PG_LIBDIR/postgresql" != "$PKGLIBDIR" ]; then
    PKGLIB_CANDIDATES+=("$PG_LIBDIR/postgresql")
fi
PSQL="$PGBIN/psql"
PG_ISREADY="$PGBIN/pg_isready"

psql_cmd() {
    "$PSQL" -h "$PGHOST" -p "$PGPORT" -d "$PGDB" -X "$@"
}

server_ready() {
    "$PG_ISREADY" -h "$PGHOST" -p "$PGPORT" -d "$PGDB" >/dev/null 2>&1
}

detect_backends() {
    local found=()
    local ext

    case "$(uname -s)" in
        Darwin) ext=dylib ;;
        MINGW*|MSYS*|CYGWIN*) ext=dll ;;
        *) ext=so ;;
    esac

    for b in sljit asmjit mir; do
        for dir in "${PKGLIB_CANDIDATES[@]}"; do
            if [ -f "$dir/pg_jitter_${b}.${ext}" ]; then
                found+=("$b")
                break
            fi
        done
    done

    if [ ${#found[@]} -eq 0 ]; then
        echo "ERROR: no pg_jitter backend libraries found in ${PKGLIB_CANDIDATES[*]}" >&2
        exit 1
    fi

    printf '%s\n' "${found[@]}"
}

if [ "$BACKEND" = "all" ]; then
    BACKENDS=()
    while IFS= read -r backend; do
        BACKENDS+=("$backend")
    done < <(detect_backends)
else
    read -r -a BACKENDS <<< "$BACKEND"
fi

JIT_SETTINGS="
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SET pg_jitter.min_expr_steps = 0;
"

FAIL=0

backend_settings() {
    local backend="$1"

    printf "SET pg_jitter.backend = '%s';\n" "$backend"
}

int4_array_1_to_n() {
    local n="$1"
    local i sep=""

    if [ "$n" -le 0 ]; then
        printf "'{}'::int4[]"
    else
        printf "'{"
        for ((i = 1; i <= n; i++)); do
            printf "%s%s" "$sep" "$i"
            sep=","
        done
        printf "}'::int4[]"
    fi
}

int4_array_min_and_1_to_n() {
    local n="$1"
    local i sep=""

    if [ "$n" -le 0 ]; then
        printf "'{-2147483648}'::int4[]"
    else
        printf "'{-2147483648"
        sep=","
        for ((i = 1; i <= n; i++)); do
            printf "%s%s" "$sep" "$i"
        done
        printf "}'::int4[]"
    fi
}

inlist_check_rows() {
    local n arr expected_not first=1
    local sizes=(0 1 4 8 9 20 32 33 64 128 129 256 4096 4097 5000)

    for n in "${sizes[@]}"; do
        arr="$(int4_array_1_to_n "$n")"
        if [ "$first" -eq 0 ]; then
            printf ',\n'
        fi
        first=0
        printf "('in_%s', (SELECT count(*) = %s FROM pg_jitter_inlist_vals WHERE v + 0 = ANY (%s)))" \
            "$n" "$n" "$arr"

        if [ "$n" -eq 0 ]; then
            expected_not=6008
        else
            expected_not=$((6007 - n))
        fi
        printf ',\n'
        printf "('not_in_%s', (SELECT count(*) = %s FROM pg_jitter_inlist_vals WHERE v + 0 <> ALL (%s)))" \
            "$n" "$expected_not" "$arr"
    done

    printf ',\n'
    printf "('duplicates', (SELECT count(*) = 3 FROM pg_jitter_inlist_vals WHERE v + 0 = ANY ('{1,1,2,2,3}'::int4[])))"
    printf ',\n'
    printf "('null_in', (SELECT count(*) = 2 FROM pg_jitter_inlist_vals WHERE v + 0 = ANY (ARRAY[1,NULL,2]::int4[])))"
    printf ',\n'
    printf "('null_not_in', (SELECT count(*) = 0 FROM pg_jitter_inlist_vals WHERE v + 0 <> ALL (ARRAY[1,NULL,2]::int4[])))"
	    printf ',\n'
	    printf "('int32_min_large', (SELECT count(*) = 5001 FROM pg_jitter_inlist_vals WHERE v + 0 = ANY (%s)))" \
	        "$(int4_array_min_and_1_to_n 5000)"
	    printf ',\n'
	    printf "('simd_pad_1', (SELECT count(*) = 1 FROM pg_jitter_inlist_pad_vals WHERE v + 0 = ANY ('{1}'::int4[])))"
	    printf ',\n'
	    printf "('simd_pad_5', (SELECT count(*) = 5 FROM pg_jitter_inlist_pad_vals WHERE v + 0 = ANY ('{1,2,3,4,5}'::int4[])))"
	    printf ',\n'
	    printf "('simd_pad_21', (SELECT count(*) = 21 FROM pg_jitter_inlist_pad_vals WHERE v + 0 = ANY (%s)))" \
	        "$(int4_array_1_to_n 21)"
}

for backend in "${BACKENDS[@]}"; do
    echo "=== backend: $backend ==="

    if ! psql_cmd -q -v ON_ERROR_STOP=1 -c "$(backend_settings "$backend")
SHOW pg_jitter.backend;" >/tmp/pg_jitter_provider_backend.out 2>&1; then
        echo "FAIL: could not select pg_jitter backend '$backend'"
        sed -n '1,80p' /tmp/pg_jitter_provider_backend.out
        FAIL=$((FAIL + 1))
        continue
    fi

    backend_jit_settings="
$(backend_settings "$backend")
$JIT_SETTINGS
"
    simd_inlist_settings=""
    if [ "$backend" = "sljit" ]; then
        simd_inlist_settings="SET pg_jitter.in_simd_max = 64;"
    fi

    overflow_sql="$backend_jit_settings
SELECT i.f1, i.f1 * int2 '2' AS x
FROM (VALUES (0::int2), (1234::int2), (32767::int2)) AS i(f1);"

    set +e
    overflow_out="$(printf '%s\n' "$overflow_sql" | psql_cmd -v ON_ERROR_STOP=0 2>&1)"
    overflow_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: int2 overflow crashed or wedged the server"
        printf '%s\n' "$overflow_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif ! printf '%s\n' "$overflow_out" | grep -q "smallint out of range"; then
        echo "FAIL: int2 overflow did not raise the expected SQL error"
        printf '%s\n' "$overflow_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    else
        echo "PASS: int2 overflow reports ERROR and server stays up"
    fi

    numeric_sql="$backend_jit_settings
WITH vals(a, b, exp_add, exp_sub, exp_mul) AS (
  VALUES
    (500::numeric, 1::numeric,
     501::numeric, 499::numeric, 500::numeric),
    (1000::numeric, 1::numeric,
     1001::numeric, 999::numeric, 1000::numeric),
    (12345::numeric, 1::numeric,
     12346::numeric, 12344::numeric, 12345::numeric),
    (12.34::numeric, 5.6::numeric,
     17.94::numeric, 6.74::numeric, 69.104::numeric),
    (7799465.7219::numeric, 4.31::numeric,
     7799470.0319::numeric, 7799461.4119::numeric,
     33615697.261389::numeric)
)
SELECT bool_and(a + b = exp_add)
   AND bool_and(a - b = exp_sub)
   AND bool_and(a * b = exp_mul)
FROM vals;"

    numeric_out="$(printf '%s\n' "$numeric_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1 || true)"
    numeric_out="$(printf '%s\n' "$numeric_out" | tr -d '[:space:]')"
    if [ "$numeric_out" != "t" ]; then
        echo "FAIL: numeric add/sub/mul diverged from PostgreSQL"
        printf '%s\n' "$numeric_out"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: numeric add/sub/mul match PostgreSQL"
    fi

    cmp3_sql="$backend_jit_settings
CREATE TEMP TABLE pg_jitter_cmp3_bool(a bool, b bool, exp int4);
INSERT INTO pg_jitter_cmp3_bool VALUES
  (false, false, 0), (false, true, -1), (true, false, 1), (true, true, 0);
CREATE TEMP TABLE pg_jitter_cmp3_int8(a int8, b int8, exp int4);
INSERT INTO pg_jitter_cmp3_int8 VALUES
  ('-9223372036854775808'::int8, -1::int8, -1),
  (-1::int8, '-9223372036854775808'::int8, 1),
  (0::int8, 0::int8, 0),
  (9223372036854775807::int8, 0::int8, 1),
  (0::int8, 9223372036854775807::int8, -1);
CREATE TEMP TABLE pg_jitter_cmp3_cash(a money, b money, exp int4);
INSERT INTO pg_jitter_cmp3_cash VALUES
  ((-100)::money, 0::money, -1),
  (0::money, (-100)::money, 1),
  (42::money, 42::money, 0);
WITH checks(name, ok) AS (
  VALUES
    ('btboolcmp', (SELECT bool_and(pg_catalog.btboolcmp(a, b) = exp) FROM pg_jitter_cmp3_bool)),
    ('btint8cmp', (SELECT bool_and(pg_catalog.btint8cmp(a, b) = exp) FROM pg_jitter_cmp3_int8)),
    ('cash_cmp', (SELECT bool_and(pg_catalog.cash_cmp(a, b) = exp) FROM pg_jitter_cmp3_cash))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

    set +e
    cmp3_out="$(printf '%s\n' "$cmp3_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    cmp3_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: Tier0 compare coverage crashed or wedged the server"
        printf '%s\n' "$cmp3_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$cmp3_rc" -ne 0 ]; then
        echo "FAIL: Tier0 compare coverage raised an unexpected SQL error"
        printf '%s\n' "$cmp3_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        cmp3_out="$(printf '%s\n' "$cmp3_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$cmp3_out" != "ok" ]; then
            echo "FAIL: Tier0 compare checks failed: $cmp3_out"
            FAIL=$((FAIL + 1))
        else
            echo "PASS: Tier0 three-way comparisons match PostgreSQL"
        fi
    fi

    endian_sql="$backend_jit_settings
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_endian_text(v text);
INSERT INTO pg_jitter_endian_text VALUES
  ('enable_hashjoin'), ('enable_mergejoin'), ('enable_seqscan'),
  ('jit'), ('disable_cost');
CREATE TEMP TABLE pg_jitter_endian_vals(v int4);
INSERT INTO pg_jitter_endian_vals SELECT g::int4 FROM generate_series(1, 32) AS g;
WITH checks(name, ok) AS (
  VALUES
    ('short_text_eq',
     (SELECT count(*) = 1 FROM pg_jitter_endian_text
      WHERE v = 'enable_hashjoin')),
    ('short_text_ne',
     (SELECT count(*) = 4 FROM pg_jitter_endian_text
      WHERE v <> 'enable_hashjoin')),
    ('prefix_like',
     (SELECT count(*) = 3 FROM pg_jitter_endian_text
      WHERE v LIKE 'enable%')),
    ('prefix_not_like',
     (SELECT count(*) = 2 FROM pg_jitter_endian_text
      WHERE v NOT LIKE 'enable%')),
    ('regex_prefix',
     (SELECT count(*) = 3 FROM pg_jitter_endian_text
      WHERE v ~ '^enable')),
    ('arrayexpr_int4',
     (SELECT bool_and(array_to_string(ARRAY[v, v + 1, v + 2]::int4[], ',') =
                      concat_ws(',', v, v + 1, v + 2))
      FROM pg_jitter_endian_vals)),
    ('arrayexpr_int2',
     (SELECT bool_and(array_to_string(ARRAY[v::int2, (v + 1)::int2]::int2[], ',') =
                      concat_ws(',', v::int2, (v + 1)::int2))
      FROM pg_jitter_endian_vals)),
    ('arrayexpr_bool',
     (SELECT bool_and(array_to_string(ARRAY[(v % 2 = 0), (v % 3 = 0)]::bool[], ',') =
                      concat_ws(',', (v % 2 = 0), (v % 3 = 0)))
      FROM pg_jitter_endian_vals))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

    set +e
    endian_out="$(printf '%s\n' "$endian_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    endian_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: endian-sensitive text/array coverage crashed or wedged the server"
        printf '%s\n' "$endian_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$endian_rc" -ne 0 ]; then
        echo "FAIL: endian-sensitive text/array coverage raised an unexpected SQL error"
        printf '%s\n' "$endian_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        endian_out="$(printf '%s\n' "$endian_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$endian_out" != "ok" ]; then
            echo "FAIL: endian-sensitive text/array checks failed: $endian_out"
            FAIL=$((FAIL + 1))
        else
            echo "PASS: endian-sensitive text/array checks match PostgreSQL"
        fi
    fi

    inlist_sql="$backend_jit_settings
$simd_inlist_settings
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_inlist_vals(v int4);
	INSERT INTO pg_jitter_inlist_vals
	SELECT g::int4 FROM generate_series(-5, 6000) AS g;
	INSERT INTO pg_jitter_inlist_vals VALUES (NULL), (-2147483648);
	CREATE TEMP TABLE pg_jitter_inlist_pad_vals(v int4);
	INSERT INTO pg_jitter_inlist_pad_vals
	SELECT g::int4 FROM generate_series(1, 21) AS g;
	INSERT INTO pg_jitter_inlist_pad_vals VALUES (-2147483647), (-2147483646);
	WITH checks(name, ok) AS (
  VALUES
  $(inlist_check_rows)
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

    set +e
    inlist_out="$(printf '%s\n' "$inlist_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    inlist_rc=$?
    set -e

	    if ! server_ready; then
	        echo "FAIL: integer IN-list coverage crashed or wedged the server"
	        printf '%s\n' "$inlist_out" | sed -n '1,80p'
	        FAIL=$((FAIL + 1))
    elif [ "$inlist_rc" -ne 0 ]; then
        echo "FAIL: integer IN-list coverage raised an unexpected SQL error"
        printf '%s\n' "$inlist_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        inlist_out="$(printf '%s\n' "$inlist_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$inlist_out" != "ok" ]; then
            echo "FAIL: integer IN-list checks failed: $inlist_out"
            FAIL=$((FAIL + 1))
        else
	            echo "PASS: integer IN-list edge cases match PostgreSQL"
	        fi
	    fi

	    partial_bitmap_sql="$backend_jit_settings
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	CREATE TEMP TABLE pg_jitter_partial_bitmap_10 AS
	SELECT g AS c1, g + 1 AS c2, g + 2 AS c3, g + 3 AS c4,
	       g + 4 AS c5, g + 5 AS c6, g + 6 AS c7, g + 7 AS c8,
	       g + 8 AS c9, NULL::int4 AS c10
	FROM generate_series(1, 5000) AS g;
	CREATE TEMP TABLE pg_jitter_partial_bitmap_18 AS
	SELECT g AS c1, g + 1 AS c2, g + 2 AS c3, g + 3 AS c4,
	       g + 4 AS c5, g + 5 AS c6, g + 6 AS c7, g + 7 AS c8,
	       g + 8 AS c9, g + 9 AS c10, g + 10 AS c11, g + 11 AS c12,
	       g + 12 AS c13, g + 13 AS c14, g + 14 AS c15, g + 15 AS c16,
	       g + 16 AS c17, NULL::int4 AS c18
	FROM generate_series(1, 5000) AS g;
	ANALYZE pg_jitter_partial_bitmap_10;
	ANALYZE pg_jitter_partial_bitmap_18;
	WITH checks(name, ok) AS (
	  VALUES
	    ('prefix_9_later_null',
	     (SELECT count(*) = 5000
	             AND sum(c9)::bigint = (5000::bigint * 5001 / 2 + 8 * 5000)
	      FROM pg_jitter_partial_bitmap_10
	      WHERE c9 > 0)),
	    ('prefix_17_later_null',
	     (SELECT count(*) = 5000
	             AND sum(c17)::bigint = (5000::bigint * 5001 / 2 + 16 * 5000)
	      FROM pg_jitter_partial_bitmap_18
	      WHERE c17 > 0))
	)
	SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
	FROM checks;"

	    set +e
	    partial_bitmap_out="$(printf '%s\n' "$partial_bitmap_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
	    partial_bitmap_rc=$?
	    set -e

	    if ! server_ready; then
	        echo "FAIL: partial-bitmap deform coverage crashed or wedged the server"
	        printf '%s\n' "$partial_bitmap_out" | sed -n '1,80p'
	        FAIL=$((FAIL + 1))
	    elif [ "$partial_bitmap_rc" -ne 0 ]; then
	        echo "FAIL: partial-bitmap deform coverage raised an unexpected SQL error"
	        printf '%s\n' "$partial_bitmap_out" | sed -n '1,120p'
	        FAIL=$((FAIL + 1))
	    else
	        partial_bitmap_out="$(printf '%s\n' "$partial_bitmap_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
	        if [ "$partial_bitmap_out" != "ok" ]; then
	            echo "FAIL: partial-bitmap deform checks failed: $partial_bitmap_out"
	            FAIL=$((FAIL + 1))
	        else
	            echo "PASS: partial-bitmap deform prefixes match PostgreSQL"
	        fi
	    fi

	    wide_uniform_sql="$backend_jit_settings
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	DO \$\$
	DECLARE
	  n int := 513;
	  rows int := 1000;
	  n_max int := 1600;
	  rows_max int := 200;
	  ddl_notnull text;
	  cols text;
	  sel text;
	  sel_alias text;
	  sel_sparse text;
	  ddl_max text;
	  cols_max text;
	  sel_max text;
	BEGIN
	  SELECT string_agg(format('c%s int4 NOT NULL', i), ', ' ORDER BY i)
	  INTO ddl_notnull
	  FROM generate_series(1, n) AS s(i);
	  SELECT string_agg(format('c%s', i), ', ' ORDER BY i)
	  INTO cols
	  FROM generate_series(1, n) AS s(i);
	  SELECT string_agg(format('(g + %s)::int4', i), ', ' ORDER BY i)
	  INTO sel
	  FROM generate_series(1, n) AS s(i);
	  SELECT string_agg(format('(g + %s)::int4 AS c%s', i, i), ', ' ORDER BY i)
	  INTO sel_alias
	  FROM generate_series(1, n) AS s(i);
	  SELECT string_agg(
	           CASE WHEN i = 100 THEN
	             format('CASE WHEN g %% 3 = 0 THEN NULL::int4 ELSE (g + %s)::int4 END AS c%s', i, i)
	           ELSE
	             format('(g + %s)::int4 AS c%s', i, i)
	           END,
	           ', ' ORDER BY i)
	  INTO sel_sparse
	  FROM generate_series(1, n) AS s(i);
	  SELECT string_agg(format('c%s int4 NOT NULL', i), ', ' ORDER BY i)
	  INTO ddl_max
	  FROM generate_series(1, n_max) AS s(i);
	  SELECT string_agg(format('c%s', i), ', ' ORDER BY i)
	  INTO cols_max
	  FROM generate_series(1, n_max) AS s(i);
	  SELECT string_agg(format('(g + %s)::int4', i), ', ' ORDER BY i)
	  INTO sel_max
	  FROM generate_series(1, n_max) AS s(i);

	  EXECUTE format('CREATE TEMP TABLE pg_jitter_wide_int4_notnull (%s)', ddl_notnull);
	  EXECUTE format('INSERT INTO pg_jitter_wide_int4_notnull(%s) SELECT %s FROM generate_series(1, %s) AS g',
	                 cols, sel, rows);
	  EXECUTE format('CREATE TEMP TABLE pg_jitter_wide_int4_nullable AS SELECT %s FROM generate_series(1, %s) AS g',
	                 sel_alias, rows);
	  EXECUTE format('CREATE TEMP TABLE pg_jitter_wide_int4_sparse AS SELECT %s FROM generate_series(1, %s) AS g',
	                 sel_sparse, rows);
	  EXECUTE format('CREATE TEMP TABLE pg_jitter_wide_int4_maxcols (%s)', ddl_max);
	  EXECUTE format('INSERT INTO pg_jitter_wide_int4_maxcols(%s) SELECT %s FROM generate_series(1, %s) AS g',
	                 cols_max, sel_max, rows_max);
	END
	\$\$;
	ANALYZE pg_jitter_wide_int4_notnull;
	ANALYZE pg_jitter_wide_int4_nullable;
	ANALYZE pg_jitter_wide_int4_sparse;
	ANALYZE pg_jitter_wide_int4_maxcols;
	WITH checks(name, ok) AS (
	  VALUES
	    ('wide_int4_notnull_tail',
	     (SELECT count(*) = 1000
	             AND sum(c513)::bigint = (1000::bigint * 1001 / 2 + 513::bigint * 1000)
	      FROM pg_jitter_wide_int4_notnull
	      WHERE c513 > 0)),
	    ('wide_int4_nullable_tail',
	     (SELECT count(*) = 1000
	             AND sum(c513)::bigint = (1000::bigint * 1001 / 2 + 513::bigint * 1000)
	      FROM pg_jitter_wide_int4_nullable
	      WHERE c513 > 0)),
	    ('wide_int4_sparse_actual_nulls',
	     (SELECT count(*) = 1000
	             AND count(c100) = 667
	             AND sum(c513)::bigint = (1000::bigint * 1001 / 2 + 513::bigint * 1000)
	      FROM pg_jitter_wide_int4_sparse
	      WHERE c513 > 0)),
	    ('wide_int4_maxcols',
	     (SELECT count(*) = 200
	             AND sum(c1600)::bigint = (200::bigint * 201 / 2 + 1600::bigint * 200)
	      FROM pg_jitter_wide_int4_maxcols
	      WHERE c1600 > 0))
	)
	SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
	FROM checks;"

	    set +e
	    wide_uniform_out="$(printf '%s\n' "$wide_uniform_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
	    wide_uniform_rc=$?
	    set -e

	    if ! server_ready; then
	        echo "FAIL: wide int4 deform coverage crashed or wedged the server"
	        printf '%s\n' "$wide_uniform_out" | sed -n '1,80p'
	        FAIL=$((FAIL + 1))
	    elif [ "$wide_uniform_rc" -ne 0 ]; then
	        echo "FAIL: wide int4 deform coverage raised an unexpected SQL error"
	        printf '%s\n' "$wide_uniform_out" | sed -n '1,120p'
	        FAIL=$((FAIL + 1))
	    else
	        wide_uniform_out="$(printf '%s\n' "$wide_uniform_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
	        if [ "$wide_uniform_out" != "ok" ]; then
	            echo "FAIL: wide int4 deform checks failed: $wide_uniform_out"
	            FAIL=$((FAIL + 1))
	        else
	            echo "PASS: wide int4 deform SIMD tails match PostgreSQL"
	        fi
	    fi
	done

	exit "$FAIL"
