#!/bin/bash
# build.sh — Build pg_jitter backends (macOS / Linux)
#
# Usage:
#   ./build.sh [--pg-config PATH] [sljit|asmjit|mir|meta|all] [cmake args...]
#
# Examples:
#   ./build.sh                                          # build all 3
#   ./build.sh sljit                                    # build sljit only
#   ./build.sh --pg-config /opt/pg18/bin/pg_config all  # custom PG install
#   ./build.sh sljit -DPG_JITTER_USE_LLVM=ON            # sljit with LLVM blobs
#   ./build.sh mir -DMIR_DIR=/opt/mir                   # custom MIR path
#
# pg_config resolution (first match wins):
#   1. --pg-config PATH argument
#   2. PG_CONFIG environment variable
#   3. pg_config from PATH
#
# Dependency paths (override with -D flags):
#   -DSLJIT_DIR=...   Path to sljit source    (default: ../sljit relative to pg_jitter)
#   -DASMJIT_DIR=...  Path to asmjit source   (default: ../asmjit relative to pg_jitter)
#   -DMIR_DIR=...     Path to MIR source      (default: ../mir relative to pg_jitter)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Parse --pg-config if present
if [ "${1:-}" = "--pg-config" ]; then
    [ -z "${2:-}" ] && { echo "ERROR: --pg-config requires a path argument"; exit 1; }
    PG_CONFIG="$2"
    shift 2
fi

PG_CONFIG="${PG_CONFIG:-pg_config}"

# Parse target (first arg if it matches a backend name)
TARGET="all"
CMAKE_EXTRA_ARGS=()
if [ $# -gt 0 ]; then
    case "$1" in
        sljit|asmjit|mir|meta|all) TARGET="$1"; shift ;;
    esac
    CMAKE_EXTRA_ARGS=("$@")
fi

# Resolve pg_config to absolute path
if ! command -v "$PG_CONFIG" > /dev/null 2>&1 && [ ! -x "$PG_CONFIG" ]; then
    echo "ERROR: pg_config not found: $PG_CONFIG"
    echo "  Use: ./build.sh --pg-config /path/to/pg_config"
    echo "   or: PG_CONFIG=/path/to/pg_config ./build.sh"
    exit 1
fi

build_backend() {
    local name="$1"
    shift
    local build_dir="$SCRIPT_DIR/build/$name"

    # meta-provider target is "pg_jitter", not "pg_jitter_meta"
    local target="pg_jitter_$name"
    if [ "$name" = "meta" ]; then
        target="pg_jitter"
    fi

    echo ""
    echo "=== Building $name ==="
    mkdir -p "$build_dir"
    cd "$build_dir"

    cmake "$SCRIPT_DIR/cmake" \
        -DPG_CONFIG="$PG_CONFIG" \
        -DBACKEND="$name" \
        "$@"

    make -j"$JOBS" "$target"
    echo "  -> $(ls -lh "$build_dir/$target.dylib" 2>/dev/null || ls -lh "$build_dir/$target.so" 2>/dev/null | awk '{print $5, $NF}')"
}

case "$TARGET" in
    sljit)  build_backend sljit  "${CMAKE_EXTRA_ARGS[@]}" ;;
    asmjit) build_backend asmjit "${CMAKE_EXTRA_ARGS[@]}" ;;
    mir)    build_backend mir    "${CMAKE_EXTRA_ARGS[@]}" ;;
    meta)   build_backend meta   "${CMAKE_EXTRA_ARGS[@]}" ;;
    all)
        build_backend sljit  "${CMAKE_EXTRA_ARGS[@]}"
        build_backend asmjit "${CMAKE_EXTRA_ARGS[@]}"
        build_backend mir    "${CMAKE_EXTRA_ARGS[@]}"
        build_backend meta   "${CMAKE_EXTRA_ARGS[@]}"
        ;;
esac

echo ""
echo "=== Done ==="
