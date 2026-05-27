#!/bin/bash
# scripts/check-shader-schema.sh
#
# Validates shader ABI interfaces defined in tools/shader_schema/manifest.json
# against shader_reflect golden JSON output and GLSL/C++ source.
#
# Part of SHADER-SCHEMA-1. Wraps tools/shader_schema/validate.py.
#
# Checks:
#   - SSBO struct size matches manifest.expectedSize (via array_stride in golden)
#   - Required shader goldens present (compilation passed)
#   - GLSL std430 layout matches C++ offsetof static_asserts
#
# Usage:
#   sh scripts/check-shader-schema.sh          (from repo root or any subdir)
#   sh scripts/check-shader-schema.sh --quiet  (suppress PASS lines)
#
# Run via cmake:
#   cmake --build build64 --target shader_schema
#
# Exit 0 = all interfaces PASS. Exit 1 = any FAIL or error.

set -euo pipefail
REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$REPO_ROOT"
python3 tools/shader_schema/validate.py "$@"
