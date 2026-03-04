#!/bin/bash
# build.sh — Build pg_jitter backends (macOS / Linux)
#
# Usage:
#   ./build.sh [--pg-config PATH] [sljit|asmjit|mir|meta|all] [cmake args...]
#
# Examples:
#   ./build.sh                                          # build all backends
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

# Map target to backend list for CMake
case "$TARGET" in
    sljit)  BACKENDS_LIST="sljit" ;;
    asmjit) BACKENDS_LIST="asmjit" ;;
    mir)    BACKENDS_LIST="mir" ;;
    meta)   BACKENDS_LIST="" ;;
    all)    BACKENDS_LIST="" ;;  # empty = use CMakeLists.txt default (auto-detect)
esac

# Determine PG version for per-version build directory
PG_VERSION=$("$PG_CONFIG" --version | sed 's/[^0-9]*\([0-9]*\).*/\1/')
BUILD_DIR="$SCRIPT_DIR/build/pg$PG_VERSION"

echo ""
echo "=== Building pg_jitter ($TARGET) for PostgreSQL $PG_VERSION ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CMAKE_ARGS=(
    -DPG_CONFIG="$PG_CONFIG"
)

# Only override backends if a specific one was requested
if [ "$TARGET" != "all" ] && [ "$TARGET" != "meta" ]; then
    CMAKE_ARGS+=(-DPG_JITTER_BACKENDS="$BACKENDS_LIST")
elif [ "$TARGET" = "meta" ]; then
    CMAKE_ARGS+=(-DPG_JITTER_BACKENDS="")
fi

cmake "$SCRIPT_DIR" "${CMAKE_ARGS[@]}" "${CMAKE_EXTRA_ARGS[@]}"

make -j"$JOBS"

echo ""
echo "=== Done ==="
