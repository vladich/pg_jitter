#!/bin/bash
# install.sh — Install pg_jitter backends and restart PostgreSQL (macOS / Linux)
#
# Usage: ./install.sh [--pg-config PATH] [--pgdata DIR] [sljit|asmjit|mir|all]
#
# pg_config resolution (first match wins):
#   1. --pg-config PATH argument
#   2. PG_CONFIG environment variable
#   3. pg_config from PATH
#
# pgdata resolution (first match wins):
#   1. --pgdata DIR argument
#   2. PGDATA environment variable
#   3. Query running PostgreSQL via psql
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Parse named arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --pg-config)
            [ -z "${2:-}" ] && { echo "ERROR: --pg-config requires a path argument"; exit 1; }
            PG_CONFIG="$2"; shift 2 ;;
        --pgdata)
            [ -z "${2:-}" ] && { echo "ERROR: --pgdata requires a path argument"; exit 1; }
            PGDATA="$2"; shift 2 ;;
        *) break ;;
    esac
done

PG_CONFIG="${PG_CONFIG:-pg_config}"

if ! command -v "$PG_CONFIG" > /dev/null 2>&1 && [ ! -x "$PG_CONFIG" ]; then
    echo "ERROR: pg_config not found: $PG_CONFIG"
    echo "  Use: ./install.sh --pg-config /path/to/pg_config"
    exit 1
fi

PGBIN="$("$PG_CONFIG" --bindir)"
PGCTL="$PGBIN/pg_ctl"
PKGLIBDIR=$("$PG_CONFIG" --pkglibdir)

# Resolve data directory
if [ -n "${PGDATA:-}" ]; then
    PG_DATA="$PGDATA"
else
    PG_DATA="$("$PGBIN/psql" -p "${PGPORT:-5432}" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || true)"
fi

if [ -z "$PG_DATA" ] || [ ! -d "$PG_DATA" ]; then
    echo "WARNING: PostgreSQL data directory not found."
    echo "  Set PGDATA or use --pgdata DIR, or ensure PostgreSQL is running."
    echo "  Libraries will be installed but PostgreSQL will NOT be restarted."
    NO_RESTART=1
fi

# Detect extension
case "$(uname -s)" in
    Darwin) EXT="dylib" ;;
    *)      EXT="so" ;;
esac

# Parse target
TARGET="${1:-all}"
case "$TARGET" in
    sljit|asmjit|mir) BACKENDS=("$TARGET") ;;
    all)              BACKENDS=(sljit asmjit mir) ;;
    *)                echo "Usage: $0 [--pg-config PATH] [sljit|asmjit|mir|all]"; exit 1 ;;
esac

echo "=== pg_jitter install ($TARGET) ==="
echo "  Target: $PKGLIBDIR"

# Find and copy dylibs (support both flat and per-backend build layouts)
find_dylib() {
    local b="$1"
    local lib="pg_jitter_$b.$EXT"
    for dir in "$SCRIPT_DIR/build/$b" "$SCRIPT_DIR/build"; do
        if [ -f "$dir/$lib" ]; then
            echo "$dir/$lib"
            return 0
        fi
    done
    return 1
}

missing=0
for b in "${BACKENDS[@]}"; do
    if ! find_dylib "$b" > /dev/null 2>&1; then
        echo "ERROR: pg_jitter_$b.$EXT not found in build/ — run ./build.sh $b first"
        missing=1
    fi
done
[ "$missing" -eq 1 ] && exit 1

for b in "${BACKENDS[@]}"; do
    src=$(find_dylib "$b")
    cp "$src" "$PKGLIBDIR/"
    echo "  pg_jitter_$b.$EXT installed"
done

# Restart PostgreSQL
if [ "${NO_RESTART:-}" = "1" ]; then
    echo ""
    echo "Skipping PostgreSQL restart (data directory unknown)."
    echo "Restart PostgreSQL manually to load the new libraries."
else
    echo ""
    if "$PGCTL" -D "$PG_DATA" status > /dev/null 2>&1; then
        "$PGCTL" -D "$PG_DATA" restart -l /tmp/pg_jitter.log 2>&1
    else
        echo "PostgreSQL is not running. Starting..."
        "$PGCTL" -D "$PG_DATA" start -l /tmp/pg_jitter.log 2>&1
    fi

    # Show status
    PORT=$(sed -n '4p' "$PG_DATA/postmaster.pid" 2>/dev/null || echo 5432)
    PROVIDER=$("$PGBIN/psql" -p "$PORT" -d postgres -t -A \
        -c "SHOW jit_provider;" 2>/dev/null || echo "unknown")
    echo ""
    echo "Active jit_provider: $PROVIDER"
    echo ""
    echo "To switch: psql -p $PORT -c \"ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';\""
fi
