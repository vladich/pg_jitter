#!/usr/bin/env bash
# Compatibility wrapper for the manifest-driven function surface regression.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/test_function_surface.py" "$@"
