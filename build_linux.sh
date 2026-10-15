#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build dist
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSBS_PREFIX="$SBS_PREFIX" -DCMAKE_CXX_COMPILER=g++
cmake --build build -j"$(nproc)"
cp -f build/libears.so dist/
cp -f build/test_ears dist/