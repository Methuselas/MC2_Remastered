#!/bin/bash
# tools/material_cook/tests/run_tests.sh
#
# Smoke-tests the validate_manifest.py tool against fixture manifests.
# Uses --skip-file-checks so no real KTX2 assets are required.
#
# Exit 0 = all pass. Exit 1 = any failure.
#
# Usage (from repo root or any directory):
#   sh tools/material_cook/tests/run_tests.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VALIDATOR="$SCRIPT_DIR/../validate_manifest.py"
PASS=0
FAIL=0

run_test() {
    local label="$1"
    local expect_exit="$2"
    local manifest="$3"
    shift 3
    local extra_args=("$@")

    set +e
    output=$(python3 "$VALIDATOR" "${extra_args[@]}" "$manifest" 2>&1)
    actual_exit=$?
    set -e

    if [ "$actual_exit" -eq "$expect_exit" ]; then
        echo "PASS  $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $label (expected exit $expect_exit, got $actual_exit)"
        echo "      output: $output"
        FAIL=$((FAIL + 1))
    fi
}

# valid_basic: expect exit 0
run_test "valid_basic (--skip-file-checks)" 0 \
    "$SCRIPT_DIR/valid_basic/materials.json" \
    --skip-file-checks

# invalid_missing_albedo: expect exit 1
run_test "invalid_missing_albedo (--skip-file-checks)" 1 \
    "$SCRIPT_DIR/invalid_missing_albedo/materials.json" \
    --skip-file-checks

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
