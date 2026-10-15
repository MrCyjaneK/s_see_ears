#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=../apple.sh
source "$SCRIPT_DIR/../apple.sh"

apple_cmake_xcframework ears 17.0 15.6 include/ears.h -DBUILD_TEST_EARS=OFF