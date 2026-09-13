#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
make -f "$ROOT_DIR/native/Makefile" -j 4
printf 'Native backend: %s\n' "$ROOT_DIR/build/native/astrolibrary"
