#!/bin/bash
# build-deb.sh — Build pg_jitter DEBs for all PG versions inside an Ubuntu/Debian container
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/common.sh"

: "${DISTRO_CODENAME:=$(lsb_release -cs 2>/dev/null || echo unknown)}"
ARCH=$(uname -m)
DEB_ARCH=$(deb_arch)
OUTPUT_DIR="$SRC_DIR/output"
mkdir -p "$OUTPUT_DIR"

echo "=== pg_jitter DEB build ==="
echo "  Version:  ${PG_JITTER_VERSION}-${PG_JITTER_RELEASE}.${DISTRO_CODENAME}"
echo "  Arch:     ${ARCH} (deb: ${DEB_ARCH})"
echo "  PG:       ${PG_VERSIONS}"

# --- Install build tools ---
echo "--- Installing build tools"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq cmake g++ make git curl lsb-release gnupg ruby ruby-dev

# Install fpm
echo "--- Installing fpm"
gem install --no-document fpm -v 1.15.1

# --- Setup PGDG repo ---
echo "--- Setting up PGDG repository"
curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
  | gpg --dearmor -o /usr/share/keyrings/pgdg.gpg
CODENAME=$(lsb_release -cs)
echo "deb [signed-by=/usr/share/keyrings/pgdg.gpg] https://apt.postgresql.org/pub/repos/apt ${CODENAME}-pgdg main" \
  > /etc/apt/sources.list.d/pgdg.list
apt-get update -qq

# --- Clone and build dependencies (once) ---
clone_dependencies "$SRC_DIR"
build_pcre2 "$SRC_DIR"

# --- Build for each PG version ---
for PG_VER in $PG_VERSIONS; do
  echo "--- Building pg_jitter for PostgreSQL ${PG_VER}"

  PG_PKG="postgresql-server-dev-${PG_VER}"
  PG_CONFIG="/usr/lib/postgresql/${PG_VER}/bin/pg_config"

  # Install PG devel package
  if ! apt-get install -y -qq "$PG_PKG" 2>/dev/null; then
    echo "  WARNING: ${PG_PKG} not available, skipping PG${PG_VER}"
    continue
  fi

  if [ ! -x "$PG_CONFIG" ]; then
    echo "  WARNING: pg_config not found at ${PG_CONFIG}, skipping PG${PG_VER}"
    continue
  fi

  BUILD_DIR="$SRC_DIR/build_deb_${PG_VER}"
  INSTALL_ROOT="/tmp/pkg_root_${PG_VER}"

  rm -rf "$BUILD_DIR" "$INSTALL_ROOT"

  build_pg_jitter "$PG_CONFIG" "$SRC_DIR" "$BUILD_DIR" "$INSTALL_ROOT"

  # Determine package name and dependencies
  PKG_NAME="postgresql-${PG_VER}-pg-jitter"
  PKG_DEP="postgresql-${PG_VER}"

  echo "--- Creating DEB: ${PKG_NAME}_${PG_JITTER_VERSION}"
  fpm -s dir -t deb \
    -n "$PKG_NAME" \
    -v "${PG_JITTER_VERSION}-${PG_JITTER_RELEASE}.${CODENAME}" \
    --architecture "$DEB_ARCH" \
    --depends "$PKG_DEP" \
    --description "pg_jitter JIT provider for PostgreSQL ${PG_VER}" \
    --url "https://github.com/vladich/pg_jitter" \
    --license "PostgreSQL" \
    --vendor "PgCypher" \
    --category database \
    -p "$OUTPUT_DIR/" \
    -C "$INSTALL_ROOT" \
    .

  rm -rf "$BUILD_DIR" "$INSTALL_ROOT"
  echo "  Done: $(ls -1 "$OUTPUT_DIR"/${PKG_NAME}*.deb 2>/dev/null | tail -1)"
done

echo ""
echo "=== DEB build complete ==="
ls -lh "$OUTPUT_DIR"/*.deb 2>/dev/null || echo "  No DEBs produced"
