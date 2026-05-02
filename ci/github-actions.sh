#!/usr/bin/env bash
# github-actions.sh - executable GitHub Actions logic for pg_jitter.
#
# This script is intentionally usable both from .github/workflows and from a
# shell on a comparable Linux/macOS machine.  The workflow should contain
# orchestration only; the commands that build and test pg_jitter live here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE="${GITHUB_WORKSPACE:-$(cd "$SCRIPT_DIR/.." && pwd)}"
if command -v cygpath >/dev/null 2>&1; then
    WORKSPACE="$(cygpath -u "$WORKSPACE")"
fi

POSTGRES_VERSION="${POSTGRES_VERSION:-}"
PG_CONFIG_PATH="${PG_CONFIG:-pg_config}"
CONNURI="${CONNURI:-}"
PG_SRC="${PG_SRC:-$WORKSPACE/postgres-src}"
SKIP_SETUP=0
MIR_REF="717efbd196c38d1680f7e4b06c976b08e0fc9271"

usage() {
    cat <<EOF
Usage: $0 COMMAND [options]

Commands:
  setup-postgres          Install/start PostgreSQL and emit connection URI
  verify-pg-config        Print pg_config details
  prepare-deps            Clone third-party dependencies used by CI
  build-pcre2             Build the static PCRE2 library used by CI
  prepare-postgres-source Clone PostgreSQL source matching pg_config version
  build-unix              Build pg_jitter with CI dependency paths
  install-unix            Install pg_jitter into pg_config's pkglibdir
  configure-postgres      Configure jit_provider and restart PostgreSQL
  build-regress-libs      Build PostgreSQL regression helper libraries
  run-regression-tests    Run pg_regress plus pg_jitter targeted checks
  all-unix                Run the Unix CI sequence above

Environment:
  GHA_BACKENDS            Space-separated backend list for regression tests
  GHA_REGRESS_TIMEOUT     Per regression command timeout, default: 10m; 0 disables

Options:
  --postgres-version N    PostgreSQL major version, 14..18
  --pg-config PATH        pg_config path/name
  --connuri URI           PostgreSQL connection URI
  --pg-src DIR            PostgreSQL source tree path
  --skip-setup            all-unix: use existing PostgreSQL server
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        setup-postgres|verify-pg-config|prepare-deps|build-pcre2|prepare-postgres-source|build-unix|install-unix|configure-postgres|build-regress-libs|run-regression-tests|all-unix)
            COMMAND="$1"; shift; break ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "Unknown command: $1" >&2; usage >&2; exit 1 ;;
    esac
done

COMMAND="${COMMAND:-}"
[ -n "$COMMAND" ] || { usage >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --postgres-version) POSTGRES_VERSION="$2"; shift 2 ;;
        --pg-config) PG_CONFIG_PATH="$2"; shift 2 ;;
        --connuri) CONNURI="$2"; shift 2 ;;
        --pg-src) PG_SRC="$2"; shift 2 ;;
        --skip-setup) SKIP_SETUP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if command -v cygpath >/dev/null 2>&1; then
    PG_SRC="$(cygpath -u "$PG_SRC")"
    case "$PG_CONFIG_PATH" in
        */*|*\\*|?:*) PG_CONFIG_PATH="$(cygpath -u "$PG_CONFIG_PATH")" ;;
    esac
fi

if [ -x "$PG_CONFIG_PATH" ]; then
    PG_CONFIG_PATH="$(cd "$(dirname "$PG_CONFIG_PATH")" && pwd)/$(basename "$PG_CONFIG_PATH")"
elif command -v "$PG_CONFIG_PATH" >/dev/null 2>&1; then
    PG_CONFIG_PATH="$(command -v "$PG_CONFIG_PATH")"
fi
if [ -x "$PG_CONFIG_PATH" ]; then
    export PATH="$(dirname "$PG_CONFIG_PATH"):$PATH"
    PG_LIBDIR="$("$PG_CONFIG_PATH" --libdir 2>/dev/null || true)"
    if [ -n "$PG_LIBDIR" ]; then
        case "$(uname -s)" in
            Darwin)
                export DYLD_LIBRARY_PATH="$PG_LIBDIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
                ;;
            MINGW*|MSYS*|CYGWIN*)
                export PATH="$PG_LIBDIR:$PATH"
                ;;
            *)
                export LD_LIBRARY_PATH="$PG_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                ;;
        esac
    fi
fi

pg_major() {
    if [ -n "$POSTGRES_VERSION" ]; then
        printf '%s\n' "$POSTGRES_VERSION"
    else
        "$PG_CONFIG_PATH" --version | sed -E 's/PostgreSQL ([0-9]+).*/\1/'
    fi
}

runner_os() {
    if [ -n "${RUNNER_OS:-}" ]; then
        printf '%s\n' "$RUNNER_OS"
    else
        case "$(uname -s)" in
            Darwin) printf 'macOS\n' ;;
            Linux) printf 'Linux\n' ;;
            *) uname -s ;;
        esac
    fi
}

write_github_env() {
    local name="$1"
    local value="$2"

    if [ -n "${GITHUB_ENV:-}" ]; then
        printf '%s=%s\n' "$name" "$value" >> "$GITHUB_ENV"
    fi
    export "$name=$value"
}

write_github_output() {
    local name="$1"
    local value="$2"

    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        printf '%s=%s\n' "$name" "$value" >> "$GITHUB_OUTPUT"
    else
        printf '%s=%s\n' "$name" "$value"
    fi
}

add_github_path() {
    local path="$1"

    if [ -n "${GITHUB_PATH:-}" ]; then
        printf '%s\n' "$path" >> "$GITHUB_PATH"
    fi
    export PATH="$path:$PATH"
}

apt_retry() {
    local attempt

    for attempt in 1 2 3; do
        if sudo apt-get -o Acquire::Retries=5 "$@"; then
            return 0
        fi
        [ "$attempt" -lt 3 ] || break
        sleep $((attempt * 15))
        sudo apt-get -o Acquire::Retries=5 update -qq || true
    done

    return 1
}

free_tcp_port() {
    local port

    for port in 55432 55433 55434 55435 55436 55437 55438 55439 55440; do
        if command -v ss >/dev/null 2>&1; then
            if ! ss -ltn "sport = :$port" | grep -q ":$port"; then
                printf '%s\n' "$port"
                return 0
            fi
        elif command -v lsof >/dev/null 2>&1; then
            if ! lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
                printf '%s\n' "$port"
                return 0
            fi
        else
            printf '%s\n' "$port"
            return 0
        fi
    done

    echo "No free PostgreSQL test port found" >&2
    exit 1
}

safe_remove_dir() {
    local dir="$1"

    case "$dir" in
        ""|"/"|"/tmp"|"${HOME:-}"|"$WORKSPACE")
            echo "Refusing to remove unsafe directory: $dir" >&2
            exit 1
            ;;
    esac
    rm -rf "$dir"
}

setup_postgres() {
    local version os pg_bindir pgdata port connuri

    version="$(pg_major)"
    os="$(runner_os)"
    port="${PGPORT:-$(free_tcp_port)}"
    pgdata="${GHA_PGDATA:-/tmp/pg_jitter_gha_pg${version}_${port}}"

    case "$os" in
        Linux)
            sudo install -d /usr/share/postgresql-common/pgdg
            sudo curl -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
                --fail --retry 5 --retry-delay 5 \
                https://www.postgresql.org/media/keys/ACCC4CF8.asc
            echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" |
                sudo tee /etc/apt/sources.list.d/pgdg.list >/dev/null
            apt_retry update -qq
            apt_retry install -y -qq "postgresql-$version" "postgresql-server-dev-$version"
            sudo pg_ctlcluster "$version" main stop >/dev/null 2>&1 || true
            sudo systemctl stop postgresql.service >/dev/null 2>&1 || true
            sudo service postgresql stop >/dev/null 2>&1 || true
            pg_bindir="$(/usr/lib/postgresql/$version/bin/pg_config --bindir)"
            ;;
        macOS)
            export HOMEBREW_GITHUB_ACTIONS=1
            export HOMEBREW_NO_ENV_HINTS=1
            export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
            export HOMEBREW_NO_INSTALL_CLEANUP=1
            export HOMEBREW_NO_INSTALL_UPGRADE=1
            brew install --quiet "postgresql@$version"
            brew unlink --quiet "postgresql@$version" || true
            brew link --quiet --overwrite "postgresql@$version"
            pg_bindir="$(brew --prefix "postgresql@$version")/bin"
            ;;
        *)
            echo "setup-postgres supports Linux/macOS only; got $os" >&2
            exit 1
            ;;
    esac

    add_github_path "$pg_bindir"
    PG_CONFIG_PATH="$pg_bindir/pg_config"
    write_github_env PG_CONFIG "$PG_CONFIG_PATH"

    "$pg_bindir/pg_ctl" -D "$pgdata" stop -m immediate -w >/dev/null 2>&1 || true
    safe_remove_dir "$pgdata"
    "$pg_bindir/initdb" -D "$pgdata" -U postgres -A trust --no-instructions

    {
        echo "listen_addresses = 'localhost'"
        echo "port = $port"
        echo "unix_socket_directories = '$pgdata'"
    } >> "$pgdata/postgresql.conf"

    if ! "$pg_bindir/pg_ctl" -D "$pgdata" start -l "$pgdata/logfile" -w; then
        echo "PostgreSQL failed to start; log follows:" >&2
        cat "$pgdata/logfile" >&2 || true
        exit 1
    fi
    connuri="postgresql://postgres:postgres@localhost:${port}/postgres"

    write_github_env CONNURI "$connuri"
    write_github_env PGPORT "$port"
    write_github_env PGDATA "$pgdata"
    write_github_output "connection-uri" "$connuri"
}

verify_pg_config() {
    "$PG_CONFIG_PATH" --version
    echo "includedir-server: $("$PG_CONFIG_PATH" --includedir-server)"
    echo "includedir:        $("$PG_CONFIG_PATH" --includedir)"
    echo "pkglibdir:         $("$PG_CONFIG_PATH" --pkglibdir)"
}

clone_repo() {
    local repo="$1"
    local dir="$2"
    local ref="${3:-}"

    if [ ! -d "$dir/.git" ]; then
        safe_remove_dir "$dir"
        git clone --depth 1 "https://github.com/$repo" "$dir"
    fi

    if [ -n "$ref" ]; then
        git -C "$dir" fetch --depth 1 origin "$ref" || true
        git -C "$dir" checkout -q "$ref"
    fi
}

prepare_deps() {
    mkdir -p "$WORKSPACE/deps"
    clone_repo vladich/sljit "$WORKSPACE/deps/sljit"
    clone_repo asmjit/asmjit "$WORKSPACE/deps/asmjit"
    clone_repo vladich/mir-patched "$WORKSPACE/deps/mir" "$MIR_REF"
    clone_repo ashvardanian/StringZilla "$WORKSPACE/deps/stringzilla"

    if [ "$(runner_os)" != "Windows" ]; then
        clone_repo ibireme/yyjson "$WORKSPACE/deps/yyjson" \
            f0fbeae7cc40218fd1af310391cdf83cfc1abff1
        if ! git -C "$WORKSPACE/deps/yyjson" grep -q \
             yyjson_read_pg_text_type_opts_err_ex -- src/yyjson.h; then
            git -C "$WORKSPACE/deps/yyjson" apply --whitespace=nowarn \
                "$WORKSPACE/patches/yyjson-pg-jitter.patch"
        fi
        git -C "$WORKSPACE/deps/yyjson" diff --check

        if [ ! -d "$WORKSPACE/deps/pcre2/.git" ]; then
            safe_remove_dir "$WORKSPACE/deps/pcre2"
            git clone --depth 1 --recurse-submodules --shallow-submodules \
                https://github.com/PCRE2Project/pcre2 \
                "$WORKSPACE/deps/pcre2"
        fi
    fi
}

build_pcre2() {
    cd "$WORKSPACE/deps/pcre2"
    mkdir -p build
    cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DPCRE2_SUPPORT_JIT=ON \
        -DPCRE2_SUPPORT_UNICODE=ON \
        -DPCRE2_BUILD_PCRE2_8=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF \
        -DPCRE2_BUILD_TESTS=OFF \
        -DPCRE2_STATIC_PIC=ON
    cmake --build . --target pcre2-8-static -j 2
    echo "PCRE2 library:"
    ls -la libpcre2-8.a
}

prepare_postgres_source() {
    local pg_full pg_ver pg_tag

    pg_full="$("$PG_CONFIG_PATH" --version)"
    pg_ver="$(echo "$pg_full" | sed -E 's/PostgreSQL ([0-9]+\.[0-9]+).*/\1/')"
    pg_tag="REL_$(echo "$pg_ver" | tr '.' '_')"
    echo "PostgreSQL version: $pg_full -> tag: $pg_tag"

    if [ ! -d "$PG_SRC/.git" ]; then
        safe_remove_dir "$PG_SRC"
        git clone --depth 1 --filter=blob:none --sparse --branch "$pg_tag" \
            https://github.com/postgres/postgres "$PG_SRC"
    else
        git -C "$PG_SRC" fetch --depth 1 origin "$pg_tag"
        git -C "$PG_SRC" checkout -q "$pg_tag"
    fi
    git -C "$PG_SRC" sparse-checkout set src/test/regress contrib/spi
}

build_unix() {
    local -a extra_args

    cd "$WORKSPACE"
    extra_args=(
        "-DSLJIT_DIR=$WORKSPACE/deps/sljit"
        "-DASMJIT_DIR=$WORKSPACE/deps/asmjit"
        "-DMIR_DIR=$WORKSPACE/deps/mir"
        "-DSTRINGZILLA_DIR=$WORKSPACE/deps/stringzilla"
        "-DPCRE2_DIR=$WORKSPACE/deps/pcre2"
        "-DYYJSON_DIR=$WORKSPACE/deps/yyjson"
    )
    if [ -n "${GETTEXT_INCLUDE:-}" ]; then
        extra_args+=(
            "-DCMAKE_C_FLAGS=$GETTEXT_INCLUDE"
            "-DCMAKE_CXX_FLAGS=$GETTEXT_INCLUDE"
        )
    fi
    ./build.sh --pg-config "$PG_CONFIG_PATH" all "${extra_args[@]}"
}

install_unix() {
    local pkglibdir

    cd "$WORKSPACE"
    pkglibdir="$("$PG_CONFIG_PATH" --pkglibdir)"
    if [ -w "$pkglibdir" ]; then
        ./install.sh --pg-config "$PG_CONFIG_PATH" all
    else
        sudo ./install.sh --pg-config "$PG_CONFIG_PATH" all
    fi
}

configure_postgres() {
    local pgdata

    [ -n "$CONNURI" ] || { echo "CONNURI is required" >&2; exit 1; }
    pgdata="$(psql -t -A -c "SHOW data_directory;" "$CONNURI")"
    echo "PGDATA: $pgdata"
    pg_ctl -D "$pgdata" stop -m fast
    {
        echo "jit_provider = 'pg_jitter'"
        echo "jit_above_cost = 0"
        echo "jit_inline_above_cost = 0"
        echo "jit_optimize_above_cost = 0"
    } >> "$pgdata/postgresql.conf"
    pg_ctl -D "$pgdata" start -l "$pgdata/logfile" -w
    sleep 1
    psql -c "SHOW jit_provider;" "$CONNURI"
}

build_regress_libs() {
    local dlpath incserver inc src base extra_cflags
    local sources

    dlpath="$PG_SRC/src/test/regress"
    incserver="$("$PG_CONFIG_PATH" --includedir-server)"
    inc="$("$PG_CONFIG_PATH" --includedir)"
    sources="src/test/regress/regress.c contrib/spi/autoinc.c contrib/spi/refint.c contrib/spi/insert_username.c contrib/spi/moddatetime.c"

    cd "$PG_SRC"
    for src in $sources; do
        base="$(basename "$src" .c)"
        extra_cflags=""
        if [ "$base" = "refint" ]; then
            extra_cflags="-DREFINT_VERBOSE"
        fi
        cc -fPIC -I"$incserver" -I"$inc" ${GETTEXT_INCLUDE:-} $extra_cflags \
            -c "$src" -o "$base.o"
        if [ "$(uname)" = "Darwin" ]; then
            cc -bundle -undefined dynamic_lookup "$base.o" -o "$dlpath/$base.dylib"
            cp "$dlpath/$base.dylib" "$dlpath/$base.so"
        else
            cc -shared "$base.o" -o "$dlpath/$base.so"
        fi
    done

    echo "Built libraries:"
    ls "$dlpath"/*.so "$dlpath"/*.dylib 2>/dev/null || true
}

backends_for_version() {
    local version="$1"
    local machine

    machine="$(uname -m)"
    if [ "$(runner_os)" = "Linux" ] &&
       { [ "$machine" = "aarch64" ] || [ "$machine" = "arm64" ]; }; then
        if [ "$version" -lt 17 ]; then
            printf 'sljit asmjit\n'
        else
            printf 'sljit asmjit auto\n'
        fi
    elif [ "$version" -lt 17 ]; then
        printf 'sljit asmjit mir\n'
    else
        printf 'sljit asmjit mir auto\n'
    fi
}

run_with_regress_timeout() {
    local label="$1"
    local limit="${GHA_REGRESS_TIMEOUT:-10m}"
    local timeout_bin=""
    local rc

    shift
    echo "----- $label -----"
    echo "Started: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"

    if [ -n "$limit" ] && [ "$limit" != "0" ]; then
        if command -v timeout >/dev/null 2>&1; then
            timeout_bin="timeout"
        elif command -v gtimeout >/dev/null 2>&1; then
            timeout_bin="gtimeout"
        fi
    fi

    if [ -n "$timeout_bin" ]; then
        "$timeout_bin" --kill-after=30s "$limit" "$@"
        rc=$?
    else
        if [ -n "$limit" ] && [ "$limit" != "0" ]; then
            echo "No timeout command available; running without timeout."
        fi
        "$@"
        rc=$?
    fi

    echo "Finished: $(date -u '+%Y-%m-%dT%H:%M:%SZ') rc=$rc"
    if [ "$rc" -eq 124 ]; then
        echo "$label timed out after $limit" >&2
    fi
    return "$rc"
}

pg_port_from_connuri() {
    psql -t -A -c "SHOW port;" "$CONNURI" | tr -d '[:space:]'
}

set_jit_backend() {
    local pgdata="$1"
    local version="$2"
    local backend="$3"

    if [ -f "$pgdata/postgresql.auto.conf" ]; then
        grep -Ev '^(jit_provider|pg_jitter\.)' "$pgdata/postgresql.auto.conf" \
            > "$pgdata/postgresql.auto.conf.tmp" || true
    else
        : > "$pgdata/postgresql.auto.conf.tmp"
    fi
    mv "$pgdata/postgresql.auto.conf.tmp" "$pgdata/postgresql.auto.conf"
    if [ "$version" -ge 17 ]; then
        echo "jit_provider = 'pg_jitter'" >> "$pgdata/postgresql.auto.conf"
        echo "pg_jitter.backend = '$backend'" >> "$pgdata/postgresql.auto.conf"
    else
        echo "jit_provider = 'pg_jitter_$backend'" >> "$pgdata/postgresql.auto.conf"
    fi
    pg_ctl -D "$pgdata" restart -l "$pgdata/logfile" -w
    sleep 1
}

run_regression_tests() {
    local version backends pgport pgdata outdir backend pg_regress_rc provider_rc surface_rc
    local targeted_failed=0

    [ -n "$CONNURI" ] || { echo "CONNURI is required" >&2; exit 1; }
    version="$(pg_major)"
    backends="${GHA_BACKENDS:-$(backends_for_version "$version")}"
    pgport="$(pg_port_from_connuri)"
    pgdata="$(psql -t -A -c "SHOW data_directory;" "$CONNURI")"
    outdir="$WORKSPACE/regress-output"

    export PGUSER="${PGUSER:-postgres}"
    export PGPASSWORD="${PGPASSWORD:-postgres}"
    export PG_JITTER_REGRESS_STREAM_LOG="${PG_JITTER_REGRESS_STREAM_LOG:-1}"

    echo "Using pg_regress backends: $backends"
    echo "Per regression command timeout: ${GHA_REGRESS_TIMEOUT:-10m}"

    for backend in $backends; do
        echo "============================================"
        echo "  pg_regress: $backend"
        echo "============================================"

        set +e
        run_with_regress_timeout "pg_regress backend $backend" \
            "$WORKSPACE/tests/run_pg_regress.sh" \
            --pg-config "$PG_CONFIG_PATH" \
            --pg-src "$PG_SRC" \
            --port "$pgport" \
            --host localhost \
            --use-running-cluster \
            --allow-destructive-cleanup \
            --backends "$backend" \
            --output-dir "$outdir/pg_regress_$backend"
        pg_regress_rc=$?
        set -e

        if [ "$pg_regress_rc" -ne 0 ]; then
            echo "$backend pg_regress: FAILED (rc=$pg_regress_rc)"
            return "$pg_regress_rc"
        fi
        echo "$backend pg_regress: PASSED"

        echo "============================================"
        echo "  targeted regression tests: $backend"
        echo "============================================"

        set_jit_backend "$pgdata" "$version" "$backend"

        set +e
        run_with_regress_timeout "targeted provider regressions backend $backend" \
            "$WORKSPACE/tests/test_provider_regressions.sh" \
            --pg-config "$PG_CONFIG_PATH" \
            --host localhost \
            --port "$pgport" \
            --db postgres \
            --backend "$backend"
        provider_rc=$?

        run_with_regress_timeout "targeted function surface regressions backend $backend" \
            "$WORKSPACE/tests/test_function_surface.sh" \
            --pg-config "$PG_CONFIG_PATH" \
            --host localhost \
            --port "$pgport" \
            --db postgres \
            --backend "$backend"
        surface_rc=$?
        set -e

        if [ "$provider_rc" -ne 0 ]; then
            echo "$backend targeted provider regressions: FAILED"
            targeted_failed=$((targeted_failed + 1))
        else
            echo "$backend targeted provider regressions: PASSED"
        fi

        if [ "$surface_rc" -ne 0 ]; then
            echo "$backend function surface regressions: FAILED"
            targeted_failed=$((targeted_failed + 1))
        else
            echo "$backend function surface regressions: PASSED"
        fi
    done

    [ "$targeted_failed" -eq 0 ]
}

all_unix() {
    if [ "$SKIP_SETUP" -eq 0 ]; then
        setup_postgres
    fi
    verify_pg_config
    prepare_deps
    build_pcre2
    prepare_postgres_source
    build_unix
    install_unix
    configure_postgres
    build_regress_libs
    run_regression_tests
}

case "$COMMAND" in
    setup-postgres) setup_postgres ;;
    verify-pg-config) verify_pg_config ;;
    prepare-deps) prepare_deps ;;
    build-pcre2) build_pcre2 ;;
    prepare-postgres-source) prepare_postgres_source ;;
    build-unix) build_unix ;;
    install-unix) install_unix ;;
    configure-postgres) configure_postgres ;;
    build-regress-libs) build_regress_libs ;;
    run-regression-tests) run_regression_tests ;;
    all-unix) all_unix ;;
esac
