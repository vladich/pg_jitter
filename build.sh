#!/bin/bash
# build.sh — Build pg_jitter backends (macOS / Linux)
#
# Usage:
#   ./build.sh [sljit|asmjit|mir|all] [cmake args...]
#
# Examples:
#   ./build.sh                                          # build all 3
#   ./build.sh sljit                                    # build sljit only
#   ./build.sh sljit -DPG_JITTER_USE_LLVM=ON            # sljit with LLVM blobs
#   ./build.sh mir -DMIR_DIR=/opt/mir                   # custom MIR path
#   PG_CONFIG=/opt/pg18/bin/pg_config ./build.sh all    # custom PG install
#
# Dependency paths (override with -D flags):
#   -DPG_CONFIG=...   Path to pg_config       (default: $PG_CONFIG or pg_config from PATH)
#   -DSLJIT_DIR=...   Path to sljit source    (default: ../sljit relative to pg_jitter)
#   -DASMJIT_DIR=...  Path to asmjit source   (default: ../asmjit relative to pg_jitter)
#   -DMIR_DIR=...     Path to MIR source      (default: ../mir relative to pg_jitter)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Parse target (first arg if it matches a backend name)
TARGET="all"
CMAKE_EXTRA_ARGS=()
if [ $# -gt 0 ]; then
    case "$1" in
        sljit|asmjit|mir|all) TARGET="$1"; shift ;;
    esac
    CMAKE_EXTRA_ARGS=("$@")
fi

if [ ! -x "$PG_CONFIG" ]; then
    echo "ERROR: pg_config not found at $PG_CONFIG"
    echo "  Set PG_CONFIG env var: PG_CONFIG=/path/to/pg_config ./build.sh"
    exit 1
fi

build_backend() {
    local name="$1"
    shift
    local build_dir="$SCRIPT_DIR/build/$name"

    echo ""
    echo "=== Building $name ==="
    mkdir -p "$build_dir"
    cd "$build_dir"

    cmake "$SCRIPT_DIR/cmake" \
        -DPG_CONFIG="$PG_CONFIG" \
        -DBACKEND="$name" \
        "$@"

    make -j"$JOBS" "pg_jitter_$name"
    echo "  -> $(ls -lh "$build_dir/pg_jitter_$name.dylib" 2>/dev/null || ls -lh "$build_dir/pg_jitter_$name.so" 2>/dev/null | awk '{print $5, $NF}')"
}

case "$TARGET" in
    sljit)  build_backend sljit  "${CMAKE_EXTRA_ARGS[@]}" ;;
    asmjit) build_backend asmjit "${CMAKE_EXTRA_ARGS[@]}" ;;
    mir)    build_backend mir    "${CMAKE_EXTRA_ARGS[@]}" ;;
    all)
        build_backend sljit  "${CMAKE_EXTRA_ARGS[@]}"
        build_backend asmjit "${CMAKE_EXTRA_ARGS[@]}"
        build_backend mir    "${CMAKE_EXTRA_ARGS[@]}"
        ;;
esac

echo ""
echo "=== Done ==="
