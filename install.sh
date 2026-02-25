#!/bin/bash
# install.sh — Install pg_jitter backends and restart PostgreSQL (macOS / Linux)
# Usage: ./install.sh [sljit|asmjit|mir|all]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGCTL="$("$PG_CONFIG" --bindir)/pg_ctl"
PG_DATA="${PGDATA:-$("$("$PG_CONFIG" --bindir)/psql" -p "${PGPORT:-5432}" -d postgres -t -A -c "SHOW data_directory;" 2>/dev/null || echo "$HOME/pgdata")}"

PKGLIBDIR=$("$PG_CONFIG" --pkglibdir)

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
    *)                echo "Usage: $0 [sljit|asmjit|mir|all]"; exit 1 ;;
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
echo ""
"$PGCTL" -D "$PG_DATA" restart -l /tmp/pg_jitter.log 2>&1

# Show status
PORT=$(sed -n '4p' "$PG_DATA/postmaster.pid" 2>/dev/null || echo 5432)
PROVIDER=$("$(dirname "$PG_CONFIG")/psql" -p "$PORT" -d postgres -t -A \
    -c "SHOW jit_provider;" 2>/dev/null || echo "unknown")
echo ""
echo "Active jit_provider: $PROVIDER"
echo ""
echo "To switch: psql -p $PORT -c \"ALTER SYSTEM SET jit_provider = 'pg_jitter_sljit';\""
