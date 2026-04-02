#!/bin/bash
# common.sh — shared functions for pg_jitter package builds
set -euo pipefail

: "${PG_JITTER_VERSION:=0.3.0}"
: "${PG_JITTER_RELEASE:=1}"
: "${PG_VERSIONS:=14 15 16 17 18}"

clone_dependencies() {
  local DEPS_DIR="${1:-.}/deps"
  [ -d "$DEPS_DIR/sljit" ] && return 0
  echo "--- Cloning dependencies"
  mkdir -p "$DEPS_DIR"
  git clone --depth 1 https://github.com/vladich/sljit.git        "$DEPS_DIR/sljit"
  git clone --depth 1 https://github.com/vladich/mir-patched.git   "$DEPS_DIR/mir"
  git clone --depth 1 https://github.com/asmjit/asmjit.git        "$DEPS_DIR/asmjit"
  git clone --depth 1 https://github.com/ashvardanian/StringZilla.git "$DEPS_DIR/stringzilla"
  git clone --depth 1 https://github.com/PCRE2Project/pcre2.git    "$DEPS_DIR/pcre2"
  git clone --depth 1 https://github.com/simdjson/simdjson.git     "$DEPS_DIR/simdjson"
}

build_pcre2() {
  local PCRE2_DIR="${1:-.}/deps/pcre2"
  [ -f "$PCRE2_DIR/build/libpcre2-8.a" ] && return 0
  echo "--- Building PCRE2"
  cmake -B "$PCRE2_DIR/build" -S "$PCRE2_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPCRE2_SUPPORT_JIT=ON \
    -DPCRE2_SUPPORT_UNICODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DPCRE2_STATIC_PIC=ON \
    -DPCRE2_BUILD_TESTS=OFF \
    -DPCRE2_BUILD_PCRE2GREP=OFF
  cmake --build "$PCRE2_DIR/build" -j"$(nproc)"
}

detect_backends() {
  local ARCH
  ARCH=$(uname -m)
  case "$ARCH" in
    x86_64|amd64|aarch64|arm64)
      echo "sljit;asmjit;mir"
      ;;
    *)
      echo "sljit;mir"
      ;;
  esac
}

deb_arch() {
  local ARCH
  ARCH=$(uname -m)
  case "$ARCH" in
    x86_64)  echo "amd64" ;;
    aarch64) echo "arm64" ;;
    ppc64le) echo "ppc64el" ;;
    *)       echo "$ARCH" ;;
  esac
}

build_pg_jitter() {
  local PG_CONFIG="$1"
  local SRC_DIR="$2"
  local BUILD_DIR="$3"
  local INSTALL_ROOT="$4"
  local BACKENDS
  BACKENDS=$(detect_backends)

  cmake -B "$BUILD_DIR" -S "$SRC_DIR" \
    -DPG_CONFIG="$PG_CONFIG" \
    -DSLJIT_DIR="$SRC_DIR/deps/sljit" \
    -DASMJIT_DIR="$SRC_DIR/deps/asmjit" \
    -DMIR_DIR="$SRC_DIR/deps/mir" \
    -DSTRINGZILLA_DIR="$SRC_DIR/deps/stringzilla" \
    -DPCRE2_DIR="$SRC_DIR/deps/pcre2" \
    -DSIMDJSON_DIR="$SRC_DIR/deps/simdjson/singleheader" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DPG_JITTER_BACKENDS=$BACKENDS"

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  DESTDIR="$INSTALL_ROOT" cmake --install "$BUILD_DIR"
}
