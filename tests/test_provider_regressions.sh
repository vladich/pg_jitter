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

case "$(uname -s)" in
    Darwin) PG_DYNLIB_VAR=DYLD_LIBRARY_PATH; PG_PATH_SEP=: ;;
    MINGW*|MSYS*|CYGWIN*) PG_DYNLIB_VAR=PATH; PG_PATH_SEP=';' ;;
    *) PG_DYNLIB_VAR=LD_LIBRARY_PATH; PG_PATH_SEP=: ;;
esac
PG_DYNLIB_PATH="$PG_LIBDIR"
if [ -d "$PG_LIBDIR/postgresql" ] && [ "$PG_LIBDIR/postgresql" != "$PG_LIBDIR" ]; then
    PG_DYNLIB_PATH="$PG_LIBDIR/postgresql$PG_PATH_SEP$PG_DYNLIB_PATH"
fi

pg_env() {
    local current="${!PG_DYNLIB_VAR:-}"

    if [ -n "$current" ]; then
        env "$PG_DYNLIB_VAR=$PG_DYNLIB_PATH$PG_PATH_SEP$current" "$@"
    else
        env "$PG_DYNLIB_VAR=$PG_DYNLIB_PATH" "$@"
    fi
}

psql_cmd() {
    pg_env "$PSQL" -h "$PGHOST" -p "$PGPORT" -d "$PGDB" -X "$@"
}

server_ready() {
    pg_env "$PG_ISREADY" -h "$PGHOST" -p "$PGPORT" -d "$PGDB" >/dev/null 2>&1
}

detect_backends() {
    local found=()
    local ext

    ext="$(library_extension)"

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

library_extension() {
    local ext

    case "$(uname -s)" in
        Darwin) ext=dylib ;;
        MINGW*|MSYS*|CYGWIN*) ext=dll ;;
        *) ext=so ;;
    esac

    printf '%s\n' "$ext"
}

default_in_hash_expected() {
    case "$(uname -m | tr '[:upper:]' '[:lower:]')" in
        x86_64|amd64) printf 'crc32\n' ;;
        *) printf 'pg\n' ;;
    esac
}

sql_literal() {
    printf '%s' "$1" | sed "s/'/''/g"
}

provider_library() {
    local backend="$1"
    local ext dir lib

    ext="$(library_extension)"
    lib="pg_jitter_${backend}.${ext}"

    for dir in "${PKGLIB_CANDIDATES[@]}"; do
        if [ -f "$dir/$lib" ]; then
            printf '%s/%s\n' "$dir" "$lib"
            return 0
        fi
    done

    printf 'pg_jitter_%s\n' "$backend"
}

if [ "$BACKEND" = "all" ]; then
    BACKENDS=()
    while IFS= read -r backend; do
        BACKENDS+=("$backend")
    done < <(detect_backends)
else
    read -r -a BACKENDS <<< "$BACKEND"
fi

PG_VERSION_NUM="$(psql_cmd -q -t -A -v ON_ERROR_STOP=1 -c "SHOW server_version_num;" | tr -d '[:space:]')"
BACKEND_OUTFILE="$(mktemp "${TMPDIR:-/tmp}/pg_jitter_provider_backend.XXXXXX")"
trap 'rm -f "$BACKEND_OUTFILE"' EXIT

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

    if [ "$PG_VERSION_NUM" -ge 170000 ]; then
        printf "SET pg_jitter.backend = '%s';\n" "$backend"
    fi
}

counter_backends() {
    local backend="$1"

    if [ "$backend" = "auto" ]; then
        detect_backends
    else
        printf '%s\n' "$backend"
    fi
}

third_party_counter_functions() {
    local backend="$1"
    local counter_backend provider_lib

    while IFS= read -r counter_backend; do
        provider_lib="$(sql_literal "$(provider_library "$counter_backend")")"
        cat <<SQL
CREATE OR REPLACE FUNCTION pg_temp.pg_jitter_reset_third_party_counters_${counter_backend}()
RETURNS void AS '$provider_lib', 'pg_jitter_reset_third_party_counters'
LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION pg_temp.pg_jitter_third_party_counter_${counter_backend}(text)
RETURNS int8 AS '$provider_lib', 'pg_jitter_third_party_counter'
LANGUAGE C STRICT;
SQL
    done < <(counter_backends "$backend")
}

third_party_counter_reset_calls() {
    local backend="$1"
    local counter_backend

    while IFS= read -r counter_backend; do
        printf 'SELECT pg_temp.pg_jitter_reset_third_party_counters_%s();\n' \
            "$counter_backend"
    done < <(counter_backends "$backend")
}

third_party_counter_expr() {
    local backend="$1"
    local counter_name="$2"
    local counter_backend sep=""

    printf '('
    while IFS= read -r counter_backend; do
        printf "%spg_temp.pg_jitter_third_party_counter_%s('%s')" \
            "$sep" "$counter_backend" "$counter_name"
        sep=" + "
    done < <(counter_backends "$backend")
    printf ')'
}

verify_backend_active() {
    local backend="$1"
    local expected_provider active_provider smoke_out

    if [ "$PG_VERSION_NUM" -ge 170000 ]; then
        expected_provider="pg_jitter"
    else
        expected_provider="pg_jitter_$backend"
    fi

    active_provider="$(psql_cmd -q -t -A -v ON_ERROR_STOP=1 -c "SHOW jit_provider;" | tail -n 1)"
    if [ "$active_provider" != "$expected_provider" ]; then
        printf 'expected jit_provider=%s, got %s\n' "$expected_provider" "$active_provider"
        printf 'Activate the provider with ALTER SYSTEM and restart before running this script.\n'
        return 1
    fi

    if [ "$PG_VERSION_NUM" -ge 170000 ]; then
        local backend_output active_backend

        backend_output="$(psql_cmd -q -t -A -v ON_ERROR_STOP=1 -c "$(backend_settings "$backend")
SHOW pg_jitter.backend;" 2>&1)" || {
            printf '%s\n' "$backend_output"
            return 1
        }
        active_backend="$(printf '%s\n' "$backend_output" | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$active_backend" != "$backend" ]; then
            printf 'expected pg_jitter.backend=%s, got output:\n%s\n' "$backend" "$backend_output"
            return 1
        fi
    fi

    smoke_out="$(psql_cmd -q -t -A -v ON_ERROR_STOP=1 -c "$(backend_settings "$backend")
SET jit = on;
SET jit_above_cost = 0;
SET jit_inline_above_cost = 0;
SET jit_optimize_above_cost = 0;
SELECT SUM(i) FROM generate_series(1,1000) AS g(i) WHERE i > 10;" 2>&1)" || {
        printf 'forced-JIT smoke query failed:\n%s\n' "$smoke_out"
        return 1
    }

    if printf '%s\n' "$smoke_out" |
       grep -q 'invalid value for parameter "pg_jitter.backend"'; then
        printf 'forced-JIT smoke query reported invalid backend:\n%s\n' "$smoke_out"
        return 1
    fi

    return 0
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

    if ! verify_backend_active "$backend" > "$BACKEND_OUTFILE" 2>&1; then
        echo "FAIL: could not select pg_jitter backend '$backend'"
        sed -n '1,80p' "$BACKEND_OUTFILE"
        FAIL=$((FAIL + 1))
        continue
    fi

    backend_jit_settings="
$(backend_settings "$backend")
$JIT_SETTINGS
"
    third_party_counter_sql="$(third_party_counter_functions "$backend")"
    third_party_counter_reset_sql="$(third_party_counter_reset_calls "$backend")"
    pcre2_compile_counter_expr="$(third_party_counter_expr "$backend" pcre2_compile)"
    pcre2_match_counter_expr="$(third_party_counter_expr "$backend" pcre2_match)"
    yyjson_is_json_counter_expr="$(third_party_counter_expr "$backend" yyjson_is_json)"
    yyjson_jsonb_in_counter_expr="$(third_party_counter_expr "$backend" yyjson_jsonb_in)"
    simd_inlist_settings=""
    if [ "$backend" = "sljit" ]; then
        simd_inlist_settings="SET pg_jitter.in_simd_max = 64;"
    fi
    crc32_inlist_settings=""
    if [ "$backend" = "sljit" ]; then
        crc32_inlist_settings="SET pg_jitter.in_hash = 'crc32';"
    fi

    if [ "$backend" = "sljit" ]; then
        expected_in_hash="$(default_in_hash_expected)"
        set +e
        default_in_hash_out="$(printf '%s\nSELECT count(*) FROM generate_series(1, 8) AS g WHERE g + 0 > 0;\nSHOW pg_jitter.in_hash;\n' "$backend_jit_settings" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
        default_in_hash_rc=$?
        set -e

        if ! server_ready; then
            echo "FAIL: default IN-list hash policy crashed or wedged the server"
            printf '%s\n' "$default_in_hash_out" | sed -n '1,80p'
            FAIL=$((FAIL + 1))
        elif [ "$default_in_hash_rc" -ne 0 ]; then
            echo "FAIL: default IN-list hash policy could not be read"
            printf '%s\n' "$default_in_hash_out" | sed -n '1,80p'
            FAIL=$((FAIL + 1))
        else
            default_in_hash_out="$(printf '%s\n' "$default_in_hash_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
            if [ "$default_in_hash_out" != "$expected_in_hash" ]; then
                echo "FAIL: default pg_jitter.in_hash expected $expected_in_hash, got $default_in_hash_out"
                FAIL=$((FAIL + 1))
            else
                echo "PASS: default IN-list hash policy is $expected_in_hash"
            fi
        fi
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

    shift_sql="$(backend_settings "$backend")
SET enable_indexscan = off;
SET enable_bitmapscan = off;
DROP TABLE IF EXISTS pg_jitter_shift_vals;
CREATE TEMP TABLE pg_jitter_shift_vals(a2 int2, a4 int4, a8 int8, s int4);
INSERT INTO pg_jitter_shift_vals VALUES
  (1, 1, 1, 0),
  (1, 1, 1, 31),
  (1, 1, 1, 32),
  (1, 1, 1, -1),
  (-1, -1, -1, 31),
  (-1, -1, -1, 32),
  (-1, -1, -1, 64),
  (-1, -1, -1, -1);
SET jit = off;
CREATE TEMP TABLE pg_jitter_shift_expected AS
SELECT a2 << s AS i2l, a2 >> s AS i2r,
       a4 << s AS i4l, a4 >> s AS i4r,
       a8 << s AS i8l, a8 >> s AS i8r
FROM pg_jitter_shift_vals
ORDER BY a2, a4, a8, s;
$backend_jit_settings
CREATE TEMP TABLE pg_jitter_shift_got AS
SELECT a2 << s AS i2l, a2 >> s AS i2r,
       a4 << s AS i4l, a4 >> s AS i4r,
       a8 << s AS i8l, a8 >> s AS i8r
FROM pg_jitter_shift_vals
ORDER BY a2, a4, a8, s;
WITH diff AS (
  (SELECT * FROM pg_jitter_shift_expected EXCEPT ALL
   SELECT * FROM pg_jitter_shift_got)
  UNION ALL
  (SELECT * FROM pg_jitter_shift_got EXCEPT ALL
   SELECT * FROM pg_jitter_shift_expected)
)
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM diff)
            THEN 'ok'
            ELSE 'integer_shift_edges_mismatch'
       END;"

    set +e
    shift_out="$(printf '%s\n' "$shift_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    shift_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: integer shift edge coverage crashed or wedged the server"
        printf '%s\n' "$shift_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$shift_rc" -ne 0 ]; then
        echo "FAIL: integer shift edge coverage raised an unexpected SQL error"
        printf '%s\n' "$shift_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        shift_out="$(printf '%s\n' "$shift_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$shift_out" != "ok" ]; then
            echo "FAIL: integer shift edge checks failed: $shift_out"
            FAIL=$((FAIL + 1))
        else
            echo "PASS: integer shift edge checks match PostgreSQL"
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
CREATE TEMP TABLE pg_jitter_endian_float4_agg(v float4);
INSERT INTO pg_jitter_endian_float4_agg VALUES
  (7.8::float4), (99.097::float4), (0.09561::float4), (324.78::float4);
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
      FROM pg_jitter_endian_vals)),
    ('float4_sum',
     (SELECT sum(v)::numeric(12,3) = 431.773::numeric(12,3)
      FROM pg_jitter_endian_float4_agg)),
    ('float4_max',
     (SELECT max(v)::numeric(12,2) = 324.78::numeric(12,2)
      FROM pg_jitter_endian_float4_agg))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

    set +e
    endian_out="$(printf '%s\n' "$endian_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    endian_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: endian-sensitive text/array/float4 coverage crashed or wedged the server"
        printf '%s\n' "$endian_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$endian_rc" -ne 0 ]; then
        echo "FAIL: endian-sensitive text/array/float4 coverage raised an unexpected SQL error"
        printf '%s\n' "$endian_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        endian_out="$(printf '%s\n' "$endian_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$endian_out" != "ok" ]; then
            echo "FAIL: endian-sensitive text/array/float4 checks failed: $endian_out"
            FAIL=$((FAIL + 1))
        else
            echo "PASS: endian-sensitive text/array/float4 checks match PostgreSQL"
        fi
    fi

    pcre2_sql="$backend_jit_settings
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_pcre2_text(v text);
INSERT INTO pg_jitter_pcre2_text VALUES
  ('Alpha123omega'),
  (repeat('x', 260) || 'alpha123omega' || repeat('y', 260)),
  (repeat('z', 260) || 'ALPHA456OMEGA' || repeat('q', 260)),
  (repeat('n', 300) || 'no_match');
CREATE TEMP TABLE pg_jitter_pcre2_regex_edge(v text);
INSERT INTO pg_jitter_pcre2_regex_edge VALUES
  ('a' || chr(10)),
  ('word'),
  ('swords'),
  ('abb'),
  ('aba'),
  (repeat('a', 255));
WITH checks(name, ok) AS (
  VALUES
    ('short_ilike',
     (SELECT count(*) = 1 FROM pg_jitter_pcre2_text
      WHERE v ILIKE 'alpha123%')),
    ('long_like_underscore',
     (SELECT count(*) = 1 FROM pg_jitter_pcre2_text
      WHERE v LIKE '%alpha_23omega%')),
    ('long_regex_captures',
     (SELECT count(*) = 1 FROM pg_jitter_pcre2_text
      WHERE v ~ '(alpha)([0-9]+)(omega)')),
    ('long_regex_icase_captures',
     (SELECT count(*) = 3 FROM pg_jitter_pcre2_text
      WHERE v ~* '(alpha)([0-9]+)(omega)')),
    ('long_not_regex_icase',
     (SELECT count(*) = 1 FROM pg_jitter_pcre2_text
      WHERE v !~* '(alpha)([0-9]+)(omega)')),
    ('raw_regex_hard_end',
     (SELECT count(*) = 0 FROM pg_jitter_pcre2_regex_edge
      WHERE v = 'a' || chr(10) AND v ~ 'a$')),
    ('raw_regex_word_boundary',
     (SELECT count(*) = 1 FROM pg_jitter_pcre2_regex_edge
      WHERE v ~ '\\mword\\M')),
    ('raw_regex_lookaround_capture_numbering',
     (SELECT count(*) = 1 AND min(v) = 'abb'
      FROM pg_jitter_pcre2_regex_edge
      WHERE v ~ '(?=(a))a(b)\\1')),
    ('raw_regex_max_bound',
     (SELECT count(*) = 1
      FROM pg_jitter_pcre2_regex_edge
      WHERE v ~ '^a{255}$'))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

    set +e
    pcre2_out="$(printf '%s\n' "$pcre2_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    pcre2_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: PCRE2 LIKE/regex coverage crashed or wedged the server"
        printf '%s\n' "$pcre2_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$pcre2_rc" -ne 0 ]; then
        echo "FAIL: PCRE2 LIKE/regex coverage raised an unexpected SQL error"
        printf '%s\n' "$pcre2_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        pcre2_out="$(printf '%s\n' "$pcre2_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$pcre2_out" != "ok" ]; then
            echo "FAIL: PCRE2 LIKE/regex checks failed: $pcre2_out"
            FAIL=$((FAIL + 1))
        else
            echo "PASS: PCRE2 LIKE/regex checks match PostgreSQL"
	        fi
	    fi

	    pcre2_contract_sql="$backend_jit_settings
$third_party_counter_sql
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	CREATE TEMP TABLE pg_jitter_pcre2_contract_text(v text);
	INSERT INTO pg_jitter_pcre2_contract_text VALUES
	  ('abc'),
	  ('abc' || chr(10)),
	  ('a' || chr(10) || 'b'),
	  ('word'),
	  ('sword'),
	  ('words'),
	  ('Alpha123omega'),
	  ('alpha_123omega'),
	  (repeat('x', 260) || 'alpha123omega' || repeat('y', 260)),
	  ('abb'),
	  ('aba'),
	  ('é'),
	  ('_');
	SET jit = off;
	CREATE TEMP TABLE pg_jitter_pcre2_contract_native AS
	SELECT v,
	       v LIKE 'a_c' AS like_single,
	       v LIKE 'abc%' AS like_prefix,
	       v LIKE '%bc' AS like_suffix,
	       v LIKE '%alpha\_123%' AS like_escaped_floating,
	       v ILIKE 'alpha123%' AS ilike_prefix,
	       v ~ '^a.c$' AS regex_dot,
	       v ~ 'a$' AS regex_hard_end,
	       v ~ '\\mword\\M' AS regex_word,
	       v ~ '(?=(a))a(b)\\1' AS regex_lookaround_capture,
	       v ~ '^[[:alpha:]]$' AS regex_alpha,
	       v ~ '^.$' AS regex_any
	FROM pg_jitter_pcre2_contract_text;
$third_party_counter_reset_sql
	SET jit = on;
	CREATE TEMP TABLE pg_jitter_pcre2_contract_jit AS
	SELECT v,
	       v LIKE 'a_c' AS like_single,
	       v LIKE 'abc%' AS like_prefix,
	       v LIKE '%bc' AS like_suffix,
	       v LIKE '%alpha\_123%' AS like_escaped_floating,
	       v ILIKE 'alpha123%' AS ilike_prefix,
	       v ~ '^a.c$' AS regex_dot,
	       v ~ 'a$' AS regex_hard_end,
	       v ~ '\\mword\\M' AS regex_word,
	       v ~ '(?=(a))a(b)\\1' AS regex_lookaround_capture,
	       v ~ '^[[:alpha:]]$' AS regex_alpha,
	       v ~ '^.$' AS regex_any
	FROM pg_jitter_pcre2_contract_text;
	WITH diff AS (
	  (SELECT * FROM pg_jitter_pcre2_contract_native
	   EXCEPT
	   SELECT * FROM pg_jitter_pcre2_contract_jit)
	  UNION ALL
	  (SELECT * FROM pg_jitter_pcre2_contract_jit
	   EXCEPT
	   SELECT * FROM pg_jitter_pcre2_contract_native)
	),
	diff_count AS (
	  SELECT count(*) AS n FROM diff
	),
	counters AS (
	  SELECT $pcre2_compile_counter_expr AS pcre2_compile,
	         $pcre2_match_counter_expr AS pcre2_match
	)
	SELECT CASE
	         WHEN diff_count.n <> 0 THEN 'diff=' || diff_count.n::text
	         WHEN counters.pcre2_compile <= 0 THEN 'pcre2_compile_not_selected'
	         WHEN counters.pcre2_match <= 0 THEN 'pcre2_match_not_selected'
	         ELSE 'ok'
	       END
	FROM diff_count, counters;"

	    set +e
	    pcre2_contract_out="$(printf '%s\n' "$pcre2_contract_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
	    pcre2_contract_rc=$?
	    set -e

	    if ! server_ready; then
	        echo "FAIL: PCRE2 native-vs-JIT contract coverage crashed or wedged the server"
	        printf '%s\n' "$pcre2_contract_out" | sed -n '1,80p'
	        FAIL=$((FAIL + 1))
	    elif [ "$pcre2_contract_rc" -ne 0 ]; then
	        echo "FAIL: PCRE2 native-vs-JIT contract coverage raised an unexpected SQL error"
	        printf '%s\n' "$pcre2_contract_out" | sed -n '1,120p'
	        FAIL=$((FAIL + 1))
	    else
	        pcre2_contract_out="$(printf '%s\n' "$pcre2_contract_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
	        if [ "$pcre2_contract_out" != "ok" ]; then
	            echo "FAIL: PCRE2 native-vs-JIT contract mismatch: $pcre2_contract_out"
	            FAIL=$((FAIL + 1))
	        else
	            echo "PASS: PCRE2 native-vs-JIT contract corpus matches PostgreSQL"
	        fi
	    fi

	    pcre2_lookaround_error_sql="$backend_jit_settings
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	CREATE TEMP TABLE pg_jitter_pcre2_lookaround_error(v text);
INSERT INTO pg_jitter_pcre2_lookaround_error VALUES ('aa');
SELECT count(*) FROM pg_jitter_pcre2_lookaround_error
WHERE v ~ '(a)(?=\\1)';"

    set +e
    pcre2_lookaround_error_out="$(printf '%s\n' "$pcre2_lookaround_error_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    pcre2_lookaround_error_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: PCRE2 lookaround backref error coverage crashed or wedged the server"
        printf '%s\n' "$pcre2_lookaround_error_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$pcre2_lookaround_error_rc" -eq 0 ]; then
        echo "FAIL: PCRE2 lookaround backref coverage did not raise PostgreSQL regex error"
        printf '%s\n' "$pcre2_lookaround_error_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif printf '%s\n' "$pcre2_lookaround_error_out" | grep -q "invalid regular expression"; then
        echo "PASS: PCRE2 lookaround backrefs fall back to PostgreSQL regex errors"
    else
        echo "FAIL: PCRE2 lookaround backref coverage raised an unexpected error"
        printf '%s\n' "$pcre2_lookaround_error_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    fi

    pcre2_bound_error_sql="$backend_jit_settings
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_pcre2_bound_error(v text);
INSERT INTO pg_jitter_pcre2_bound_error VALUES (repeat('a', 256));
SELECT count(*) FROM pg_jitter_pcre2_bound_error
WHERE v ~ 'a{256}';"

    set +e
    pcre2_bound_error_out="$(printf '%s\n' "$pcre2_bound_error_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    pcre2_bound_error_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: PCRE2 over-DUPMAX bound coverage crashed or wedged the server"
        printf '%s\n' "$pcre2_bound_error_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$pcre2_bound_error_rc" -eq 0 ]; then
        echo "FAIL: PCRE2 over-DUPMAX bound coverage did not raise PostgreSQL regex error"
        printf '%s\n' "$pcre2_bound_error_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif printf '%s\n' "$pcre2_bound_error_out" | grep -q "invalid regular expression"; then
        echo "PASS: PCRE2 over-DUPMAX bounds fall back to PostgreSQL regex errors"
    else
        echo "FAIL: PCRE2 over-DUPMAX bound coverage raised an unexpected error"
        printf '%s\n' "$pcre2_bound_error_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    fi

    pcre2_constraint_quantifier_error_sql="$backend_jit_settings
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_pcre2_constraint_error(v text);
INSERT INTO pg_jitter_pcre2_constraint_error VALUES ('word');
SELECT count(*) FROM pg_jitter_pcre2_constraint_error
WHERE v ~ '\\m+';"

    set +e
    pcre2_constraint_quantifier_error_out="$(printf '%s\n' "$pcre2_constraint_quantifier_error_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    pcre2_constraint_quantifier_error_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: PCRE2 quantified-constraint coverage crashed or wedged the server"
        printf '%s\n' "$pcre2_constraint_quantifier_error_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$pcre2_constraint_quantifier_error_rc" -eq 0 ]; then
        echo "FAIL: PCRE2 quantified-constraint coverage did not raise PostgreSQL regex error"
        printf '%s\n' "$pcre2_constraint_quantifier_error_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif printf '%s\n' "$pcre2_constraint_quantifier_error_out" | grep -q "invalid regular expression"; then
        echo "PASS: PCRE2 quantified constraints fall back to PostgreSQL regex errors"
    else
        echo "FAIL: PCRE2 quantified-constraint coverage raised an unexpected error"
        printf '%s\n' "$pcre2_constraint_quantifier_error_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    fi

    if [ "$PG_VERSION_NUM" -ge 160000 ]; then
        yyjson_is_json_sql="$backend_jit_settings
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_yyjson_is_json(name text, js text);
INSERT INTO pg_jitter_yyjson_is_json VALUES
  ('plain_nul_escape', '\"\\u0000\"'),
  ('plain_lone_surrogate', '{\"x\":\"\\uD800\"}'),
  ('plain_bad_hex', '\"\\uD80Z\"'),
  ('unique_duplicate_decoded_key', '{\"a\":1,\"\\u0061\":2}'),
  ('unique_lone_surrogate_value', '{\"x\":\"\\uD800\"}'),
  ('unique_nul_key', '{\"\\u0000\":1}'),
  ('unique_nested_ok', '{\"a\":1,\"b\":[{\"a\":1},{\"a\":2}]}');
WITH checks(name, ok) AS (
  VALUES
    ('syntax_allows_nul_escape',
     (SELECT bool_and(js IS JSON)
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'plain_nul_escape')),
    ('syntax_allows_lone_surrogate',
     (SELECT bool_and(js IS JSON)
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'plain_lone_surrogate')),
    ('syntax_rejects_bad_hex',
     (SELECT bool_and(NOT (js IS JSON))
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'plain_bad_hex')),
    ('unique_rejects_duplicate_decoded_key',
     (SELECT bool_and(NOT (js IS JSON WITH UNIQUE KEYS))
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'unique_duplicate_decoded_key')),
    ('unique_rejects_lone_surrogate_value',
     (SELECT bool_and(NOT (js IS JSON WITH UNIQUE KEYS))
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'unique_lone_surrogate_value')),
    ('unique_rejects_nul_key',
     (SELECT bool_and(NOT (js IS JSON WITH UNIQUE KEYS))
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'unique_nul_key')),
    ('unique_accepts_nested_ok',
     (SELECT bool_and(js IS JSON WITH UNIQUE KEYS)
      FROM pg_jitter_yyjson_is_json
      WHERE name = 'unique_nested_ok'))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

        set +e
        yyjson_is_json_out="$(printf '%s\n' "$yyjson_is_json_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
        yyjson_is_json_rc=$?
        set -e

        if ! server_ready; then
            echo "FAIL: yyjson IS JSON coverage crashed or wedged the server"
            printf '%s\n' "$yyjson_is_json_out" | sed -n '1,80p'
            FAIL=$((FAIL + 1))
        elif [ "$yyjson_is_json_rc" -ne 0 ]; then
            echo "FAIL: yyjson IS JSON coverage raised an unexpected SQL error"
            printf '%s\n' "$yyjson_is_json_out" | sed -n '1,120p'
            FAIL=$((FAIL + 1))
	        else
	            yyjson_is_json_out="$(printf '%s\n' "$yyjson_is_json_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
	            if [ "$yyjson_is_json_out" != "ok" ]; then
	                echo "FAIL: yyjson IS JSON checks failed: $yyjson_is_json_out"
                FAIL=$((FAIL + 1))
            else
	                echo "PASS: yyjson IS JSON syntax and unique-key semantics match PostgreSQL"
	            fi
	        fi

	        yyjson_contract_sql="$backend_jit_settings
$third_party_counter_sql
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	CREATE TEMP TABLE pg_jitter_yyjson_contract_corpus(name text, js text);
	INSERT INTO pg_jitter_yyjson_contract_corpus VALUES
	  ('empty', ''),
	  ('space', '   '),
	  ('short_object', '{\"a\":1}'),
	  ('short_array', '[1,true,null]'),
	  ('short_scalar', '\"x\"'),
	  ('plain_nul_escape', '\"\\u0000\"'),
	  ('plain_lone_surrogate', '{\"x\":\"\\uD800\"}'),
	  ('unicode_pair', '\"\\uD834\\uDD1E\"'),
	  ('bad_pair', '\"\\uD834x\"'),
	  ('dup_plain', '{\"a\":1,\"a\":2}'),
	  ('dup_decoded', '{\"a\":1,\"\\u0061\":2}'),
	  ('nested_dup', '{\"a\":[{\"b\":1,\"b\":2}]}'),
	  ('nested_distinct', '{\"a\":[{\"b\":1},{\"b\":2}]}'),
	  ('large', '{\"a\":[' || repeat('{\"b\":1},', 90) || '{\"c\":2}]}');
	SET jit = off;
	CREATE TEMP TABLE pg_jitter_yyjson_contract_native AS
	SELECT name,
	       js IS JSON AS is_json,
	       js IS JSON OBJECT AS is_object,
	       js IS JSON ARRAY AS is_array,
	       js IS JSON SCALAR AS is_scalar,
	       js IS JSON WITH UNIQUE KEYS AS unique_keys
	FROM pg_jitter_yyjson_contract_corpus;
$third_party_counter_reset_sql
	SET jit = on;
	CREATE TEMP TABLE pg_jitter_yyjson_contract_jit AS
	SELECT name,
	       js IS JSON AS is_json,
	       js IS JSON OBJECT AS is_object,
	       js IS JSON ARRAY AS is_array,
	       js IS JSON SCALAR AS is_scalar,
	       js IS JSON WITH UNIQUE KEYS AS unique_keys
	FROM pg_jitter_yyjson_contract_corpus;
	CREATE TEMP TABLE pg_jitter_yyjson_contract_valid(name text, js text);
	INSERT INTO pg_jitter_yyjson_contract_valid VALUES
	  ('short_object', '{\"a\":1}'),
	  ('short_array', '[1,true,null]'),
	  ('short_scalar', '\"x\"'),
	  ('unicode_pair', '\"\\uD834\\uDD1E\"'),
	  ('large', '{\"a\":[' || repeat('{\"b\":1},', 90) || '{\"c\":2}]}');
	SET jit = off;
	CREATE TEMP TABLE pg_jitter_yyjson_jsonb_contract_native AS
	SELECT name, js::jsonb::text AS rendered
	FROM pg_jitter_yyjson_contract_valid;
	SET jit = on;
	CREATE TEMP TABLE pg_jitter_yyjson_jsonb_contract_jit AS
	SELECT name, js::jsonb::text AS rendered
	FROM pg_jitter_yyjson_contract_valid;
	WITH diff AS (
	  SELECT 'is_json' AS kind, count(*) AS n
	  FROM (
	    (SELECT * FROM pg_jitter_yyjson_contract_native
	     EXCEPT
	     SELECT * FROM pg_jitter_yyjson_contract_jit)
	    UNION ALL
	    (SELECT * FROM pg_jitter_yyjson_contract_jit
	     EXCEPT
	     SELECT * FROM pg_jitter_yyjson_contract_native)
	  ) d
	  UNION ALL
	  SELECT 'jsonb', count(*)
	  FROM (
	    (SELECT * FROM pg_jitter_yyjson_jsonb_contract_native
	     EXCEPT
	     SELECT * FROM pg_jitter_yyjson_jsonb_contract_jit)
	    UNION ALL
	    (SELECT * FROM pg_jitter_yyjson_jsonb_contract_jit
	     EXCEPT
	     SELECT * FROM pg_jitter_yyjson_jsonb_contract_native)
	  ) d
	),
	diff_summary AS (
	  SELECT COALESCE(string_agg(kind || '=' || n::text, ', ' ORDER BY kind)
	                  FILTER (WHERE n <> 0), 'ok') AS result
	  FROM diff
	),
	counters AS (
	  SELECT $yyjson_is_json_counter_expr AS yyjson_is_json,
	         $yyjson_jsonb_in_counter_expr AS yyjson_jsonb_in
	)
	SELECT CASE
	         WHEN diff_summary.result <> 'ok' THEN diff_summary.result
	         WHEN counters.yyjson_is_json <= 0 THEN 'yyjson_is_json_not_selected'
	         WHEN counters.yyjson_jsonb_in <= 0 THEN 'yyjson_jsonb_in_not_selected'
	         ELSE 'ok'
	       END
	FROM diff_summary, counters;"

	        set +e
	        yyjson_contract_out="$(printf '%s\n' "$yyjson_contract_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
	        yyjson_contract_rc=$?
	        set -e

	        if ! server_ready; then
	            echo "FAIL: yyjson native-vs-JIT contract coverage crashed or wedged the server"
	            printf '%s\n' "$yyjson_contract_out" | sed -n '1,80p'
	            FAIL=$((FAIL + 1))
	        elif [ "$yyjson_contract_rc" -ne 0 ]; then
	            echo "FAIL: yyjson native-vs-JIT contract coverage raised an unexpected SQL error"
	            printf '%s\n' "$yyjson_contract_out" | sed -n '1,120p'
	            FAIL=$((FAIL + 1))
	        else
	            yyjson_contract_out="$(printf '%s\n' "$yyjson_contract_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
	            if [ "$yyjson_contract_out" != "ok" ]; then
	                echo "FAIL: yyjson native-vs-JIT contract mismatch: $yyjson_contract_out"
	                FAIL=$((FAIL + 1))
	            else
	                echo "PASS: yyjson native-vs-JIT contract corpus matches PostgreSQL"
	            fi
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

    inlist_policy_sql="$backend_jit_settings
SET pg_jitter.in_hash = 'pg';
SET pg_jitter.in_bsearch_max = 128;
SET pg_jitter.in_text_hash = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_inlist_policy_vals(v int4);
INSERT INTO pg_jitter_inlist_policy_vals
SELECT g::int4 FROM generate_series(-5, 6000) AS g;
INSERT INTO pg_jitter_inlist_policy_vals VALUES (NULL), (-2147483648);
WITH checks(name, ok) AS (
  VALUES
    ('pg_fallback_129',
     (SELECT count(*) = 129
      FROM pg_jitter_inlist_policy_vals
      WHERE v + 0 = ANY ($(int4_array_1_to_n 129)))),
    ('pg_fallback_4096',
     (SELECT count(*) = 4096
      FROM pg_jitter_inlist_policy_vals
      WHERE v + 0 = ANY ($(int4_array_1_to_n 4096)))),
    ('pg_fallback_4097',
     (SELECT count(*) = 4097
      FROM pg_jitter_inlist_policy_vals
      WHERE v + 0 = ANY ($(int4_array_1_to_n 4097)))),
    ('pg_fallback_int32_min',
     (SELECT count(*) = 5001
      FROM pg_jitter_inlist_policy_vals
      WHERE v + 0 = ANY ($(int4_array_min_and_1_to_n 5000)))),
    ('pg_fallback_not_in',
     (SELECT count(*) = 1910
      FROM pg_jitter_inlist_policy_vals
      WHERE v + 0 <> ALL ($(int4_array_1_to_n 4097))))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

    set +e
    inlist_policy_out="$(printf '%s\n' "$inlist_policy_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
    inlist_policy_rc=$?
    set -e

    if ! server_ready; then
        echo "FAIL: PostgreSQL fallback IN-list policy crashed or wedged the server"
        printf '%s\n' "$inlist_policy_out" | sed -n '1,80p'
        FAIL=$((FAIL + 1))
    elif [ "$inlist_policy_rc" -ne 0 ]; then
        echo "FAIL: PostgreSQL fallback IN-list policy raised an unexpected SQL error"
        printf '%s\n' "$inlist_policy_out" | sed -n '1,120p'
        FAIL=$((FAIL + 1))
    else
        inlist_policy_out="$(printf '%s\n' "$inlist_policy_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
        if [ "$inlist_policy_out" != "ok" ]; then
            echo "FAIL: PostgreSQL fallback IN-list policy checks failed: $inlist_policy_out"
            FAIL=$((FAIL + 1))
        else
            echo "PASS: PostgreSQL fallback IN-list policy matches PostgreSQL"
        fi
    fi

    if [ -n "$crc32_inlist_settings" ]; then
        inlist_crc32_sql="$backend_jit_settings
$crc32_inlist_settings
SET pg_jitter.in_bsearch_max = 128;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE pg_jitter_inlist_crc32_vals(v int4);
INSERT INTO pg_jitter_inlist_crc32_vals
SELECT g::int4 FROM generate_series(-5, 6000) AS g;
INSERT INTO pg_jitter_inlist_crc32_vals VALUES (NULL), (-2147483648);
WITH checks(name, ok) AS (
  VALUES
    ('crc32_4097',
     (SELECT count(*) = 4097
      FROM pg_jitter_inlist_crc32_vals
      WHERE v + 0 = ANY ($(int4_array_1_to_n 4097)))),
    ('crc32_int32_min',
     (SELECT count(*) = 5001
      FROM pg_jitter_inlist_crc32_vals
      WHERE v + 0 = ANY ($(int4_array_min_and_1_to_n 5000)))),
    ('crc32_not_in',
     (SELECT count(*) = 1910
      FROM pg_jitter_inlist_crc32_vals
      WHERE v + 0 <> ALL ($(int4_array_1_to_n 4097))))
)
SELECT COALESCE(string_agg(name, ', ' ORDER BY name) FILTER (WHERE NOT ok), 'ok')
FROM checks;"

        set +e
        inlist_crc32_out="$(printf '%s\n' "$inlist_crc32_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
        inlist_crc32_rc=$?
        set -e

        if ! server_ready; then
            echo "FAIL: SLJIT CRC32 IN-list policy crashed or wedged the server"
            printf '%s\n' "$inlist_crc32_out" | sed -n '1,80p'
            FAIL=$((FAIL + 1))
        elif [ "$inlist_crc32_rc" -ne 0 ]; then
            echo "FAIL: SLJIT CRC32 IN-list policy raised an unexpected SQL error"
            printf '%s\n' "$inlist_crc32_out" | sed -n '1,120p'
            FAIL=$((FAIL + 1))
        else
            inlist_crc32_out="$(printf '%s\n' "$inlist_crc32_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
            if [ "$inlist_crc32_out" != "ok" ]; then
                echo "FAIL: SLJIT CRC32 IN-list policy checks failed: $inlist_crc32_out"
                FAIL=$((FAIL + 1))
            else
                echo "PASS: SLJIT CRC32 IN-list opt-in matches PostgreSQL"
            fi
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

	    if [ "$backend" = "sljit" ]; then
	        parallel_shared_sql="$backend_jit_settings
	SET enable_indexscan = off;
	SET enable_bitmapscan = off;
	SET pg_jitter.parallel_mode = 'shared';
	SET max_parallel_workers_per_gather = 4;
	SET parallel_leader_participation = off;
	SET parallel_setup_cost = 0;
	SET parallel_tuple_cost = 0;
	SET min_parallel_table_scan_size = 0;
	SET min_parallel_index_scan_size = 0;
	DROP TABLE IF EXISTS pg_jitter_pshape_a;
	DROP TABLE IF EXISTS pg_jitter_pshape_b;
	DO \$\$
	DECLARE
	  n int := 130;
	  rows int := 40000;
	  cols_a text;
	  cols_b text;
	BEGIN
	  SELECT string_agg(
	           format('CASE WHEN g %% 7 = 0 THEN NULL ELSE %s END AS c%s',
	                  CASE WHEN i % 3 = 0 THEN 'g::int4'
	                       WHEN i % 3 = 1 THEN '(''txt_'' || g)::text'
	                       ELSE '(g::float8 / 3.0)' END,
	                  lpad(i::text, 3, '0')),
	           ', ' ORDER BY i)
	  INTO cols_a
	  FROM generate_series(1, n) AS s(i);

	  SELECT string_agg(
	           format('CASE WHEN g %% 11 = 0 THEN NULL ELSE (g + %s)::int4 END AS c%s',
	                  i, lpad(i::text, 3, '0')),
	           ', ' ORDER BY i)
	  INTO cols_b
	  FROM generate_series(1, n) AS s(i);

	  EXECUTE format('CREATE TABLE pg_jitter_pshape_a AS SELECT g AS id, g %% 10 AS grp, %s FROM generate_series(1, %s) AS g',
	                 cols_a, rows);
	  EXECUTE format('CREATE TABLE pg_jitter_pshape_b AS SELECT g AS id, g %% 10 AS grp, %s FROM generate_series(1, %s) AS g',
	                 cols_b, rows);
	END
	\$\$;
	ALTER TABLE pg_jitter_pshape_a SET (parallel_workers = 4);
	ALTER TABLE pg_jitter_pshape_b SET (parallel_workers = 4);
	ANALYZE pg_jitter_pshape_a;
	ANALYZE pg_jitter_pshape_b;
	CREATE OR REPLACE FUNCTION pg_temp.pg_jitter_parallel_compare(query text)
	RETURNS text LANGUAGE plpgsql AS \$\$
	DECLARE
	  serial_result text;
	  parallel_result text;
	BEGIN
	  EXECUTE 'SET max_parallel_workers_per_gather = 0';
	  EXECUTE 'SELECT (' || query || ')::text' INTO serial_result;
	  EXECUTE 'SET max_parallel_workers_per_gather = 4';
	  EXECUTE 'SELECT (' || query || ')::text' INTO parallel_result;

	  IF serial_result IS DISTINCT FROM parallel_result THEN
	    RETURN format('serial=%s parallel=%s', serial_result, parallel_result);
	  END IF;

	  RETURN 'ok';
	END
	\$\$;
	SELECT pg_temp.pg_jitter_parallel_compare(
	  \$q\$SELECT sum(total) FROM (
	         SELECT COALESCE(sum(c128::numeric), 0)::bigint AS total
	         FROM pg_jitter_pshape_a
	         WHERE c128 IS NOT NULL
	         UNION ALL
	         SELECT COALESCE(sum(c128::numeric), 0)::bigint AS total
	         FROM pg_jitter_pshape_b
	         WHERE c128 IS NOT NULL
	       ) s\$q\$);
	DROP TABLE pg_jitter_pshape_a;
	DROP TABLE pg_jitter_pshape_b;"

	        set +e
	        parallel_shared_out="$(printf '%s\n' "$parallel_shared_sql" | psql_cmd -q -t -A -v ON_ERROR_STOP=1 2>&1)"
	        parallel_shared_rc=$?
	        set -e

	        if ! server_ready; then
	            echo "FAIL: parallel shared wide-shape coverage crashed or wedged the server"
	            printf '%s\n' "$parallel_shared_out" | sed -n '1,80p'
	            FAIL=$((FAIL + 1))
	        elif [ "$parallel_shared_rc" -ne 0 ]; then
	            echo "FAIL: parallel shared wide-shape coverage raised an unexpected SQL error"
	            printf '%s\n' "$parallel_shared_out" | sed -n '1,120p'
	            FAIL=$((FAIL + 1))
	        else
	            parallel_shared_out="$(printf '%s\n' "$parallel_shared_out" | tr -d '\r' | sed '/^[[:space:]]*$/d' | tail -n 1)"
	            if [ "$parallel_shared_out" != "ok" ]; then
	                echo "FAIL: parallel shared wide-shape checks failed: $parallel_shared_out"
	                FAIL=$((FAIL + 1))
	            else
	                echo "PASS: parallel shared wide-shape guard matches PostgreSQL"
	            fi
	        fi
	    fi
	done

	exit "$FAIL"
