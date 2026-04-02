#!/bin/bash
# build-rpm.sh — Build pg_jitter RPMs for all PG versions inside a Rocky/Fedora container
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/common.sh"

: "${DISTRO_TAG:=el9}"
ARCH=$(uname -m)
OUTPUT_DIR="$SRC_DIR/output"
mkdir -p "$OUTPUT_DIR"

echo "=== pg_jitter RPM build ==="
echo "  Version:  ${PG_JITTER_VERSION}-${PG_JITTER_RELEASE}.${DISTRO_TAG}"
echo "  Arch:     ${ARCH}"
echo "  PG:       ${PG_VERSIONS}"

# --- Install build tools ---
echo "--- Installing build tools"
if command -v dnf &>/dev/null; then
  PKG_MGR=dnf
else
  PKG_MGR=yum
fi

$PKG_MGR install -y gcc gcc-c++ cmake make git ruby ruby-devel rpm-build redhat-rpm-config

# Enable extra repos for dependencies
if [ -f /etc/os-release ]; then
  . /etc/os-release
  case "$ID" in
    rocky|almalinux|centos)
      case "$VERSION_ID" in
        8*) $PKG_MGR install -y epel-release && $PKG_MGR config-manager --set-enabled powertools || true ;;
        9*) $PKG_MGR install -y epel-release && $PKG_MGR config-manager --set-enabled crb || true ;;
      esac
      ;;
    fedora)
      $PKG_MGR module disable -y postgresql 2>/dev/null || true
      ;;
  esac
fi

# Install fpm
echo "--- Installing fpm"
gem install --no-document fpm

# --- Setup PGDG repo ---
echo "--- Setting up PGDG repository"
if [ -f /etc/redhat-release ]; then
  . /etc/os-release
  case "$ID" in
    rocky|almalinux|centos|rhel)
      MAJOR="${VERSION_ID%%.*}"
      $PKG_MGR install -y "https://download.postgresql.org/pub/repos/yum/reporpms/EL-${MAJOR}-${ARCH}/pgdg-redhat-repo-latest.noarch.rpm" || true
      ;;
    fedora)
      $PKG_MGR install -y "https://download.postgresql.org/pub/repos/yum/reporpms/F-${VERSION_ID}-${ARCH}/pgdg-fedora-repo-latest.noarch.rpm" || true
      ;;
  esac
  # Disable built-in PG module if present
  $PKG_MGR module disable -y postgresql 2>/dev/null || true
fi

# --- Clone and build dependencies (once) ---
clone_dependencies "$SRC_DIR"
build_pcre2 "$SRC_DIR"

# --- Build for each PG version ---
for PG_VER in $PG_VERSIONS; do
  echo "--- Building pg_jitter for PostgreSQL ${PG_VER}"

  PG_PKG="postgresql${PG_VER}-devel"
  PG_CONFIG="/usr/pgsql-${PG_VER}/bin/pg_config"

  # Install PG devel package
  if ! $PKG_MGR install -y "$PG_PKG" 2>/dev/null; then
    echo "  WARNING: ${PG_PKG} not available, skipping PG${PG_VER}"
    continue
  fi

  if [ ! -x "$PG_CONFIG" ]; then
    echo "  WARNING: pg_config not found at ${PG_CONFIG}, skipping PG${PG_VER}"
    continue
  fi

  BUILD_DIR="$SRC_DIR/build_rpm_${PG_VER}"
  INSTALL_ROOT="/tmp/pkg_root_${PG_VER}"

  rm -rf "$BUILD_DIR" "$INSTALL_ROOT"

  build_pg_jitter "$PG_CONFIG" "$SRC_DIR" "$BUILD_DIR" "$INSTALL_ROOT"

  # Determine package name and dependencies
  PKG_NAME="pg_jitter_${PG_VER}"
  PKG_DEP="postgresql${PG_VER}-server"

  echo "--- Creating RPM: ${PKG_NAME}-${PG_JITTER_VERSION}"
  fpm -s dir -t rpm \
    -n "$PKG_NAME" \
    -v "$PG_JITTER_VERSION" \
    --iteration "${PG_JITTER_RELEASE}.${DISTRO_TAG}" \
    --architecture "$ARCH" \
    --depends "$PKG_DEP" \
    --description "pg_jitter JIT provider for PostgreSQL ${PG_VER}" \
    --url "https://github.com/vladich/pg_jitter" \
    --license "PostgreSQL" \
    --vendor "PgCypher" \
    --category "Applications/Databases" \
    -p "$OUTPUT_DIR/" \
    -C "$INSTALL_ROOT" \
    .

  rm -rf "$BUILD_DIR" "$INSTALL_ROOT"
  echo "  Done: $(ls -1 "$OUTPUT_DIR"/${PKG_NAME}*.rpm 2>/dev/null | tail -1)"
done

echo ""
echo "=== RPM build complete ==="
ls -lh "$OUTPUT_DIR"/*.rpm 2>/dev/null || echo "  No RPMs produced"
