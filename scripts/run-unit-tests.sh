#!/usr/bin/env bash
# scripts/run-unit-tests.sh
# One-command build+run of the OFFLINE doctest unit suite (tests/unit/mc2_tests).
# Pure-logic only -- no GL context, no engine, no deploy, no smoke. This is the
# "prove it offline" loop for pure kernels (VIEW-CURRENCY-HARNESS-1 currency math,
# FRAME-GRAPH-SKELETON-1 resource-DAG validation, projection/xform/HZB conventions,
# etc). Seconds to run; use it instead of a full build->deploy->smoke when the change
# is pure CPU logic.
#
# Usage:
#   bash scripts/run-unit-tests.sh                  # build + run the whole suite
#   bash scripts/run-unit-tests.sh --ts=FrameGraph  # only the FrameGraph suite
#   bash scripts/run-unit-tests.sh --ts=ViewCurrency,FrameGraph
#   MC2_CMAKE=/path/to/cmake.exe bash scripts/run-unit-tests.sh   # override cmake
#
# Standalone build dir (build64-tests/) -- never touches the engine build64/ cache.
# Exit 0 iff configure, build, and every test pass.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE="${MC2_CMAKE:-C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe}"
CFG="${MC2_TEST_CONFIG:-RelWithDebInfo}"
BUILD_DIR="$ROOT/build64-tests"

echo "[run-unit-tests] configure ($BUILD_DIR)"
"$CMAKE" -S "$ROOT/tests/unit" -B "$BUILD_DIR" -G "Visual Studio 17 2022" -A x64 >/dev/null

echo "[run-unit-tests] build mc2_tests ($CFG)"
"$CMAKE" --build "$BUILD_DIR" --config "$CFG" --target mc2_tests

echo "[run-unit-tests] run"
exec "$BUILD_DIR/$CFG/mc2_tests.exe" "$@"
