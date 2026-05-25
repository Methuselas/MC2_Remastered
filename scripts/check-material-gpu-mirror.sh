#!/bin/bash
# scripts/check-material-gpu-mirror.sh
#
# Verifies that RenderCore/MaterialGpu.h and shaders/include/material_gpu.hglsl
# declare the MaterialGpu struct fields in the same order.
#
# Uses Python for extraction (avoids grep -P locale issues on Windows/MSYS2).
# The C++ static_asserts enforce byte offsets; this gate catches accidental
# field renames or reorders in the GLSL mirror that static_asserts cannot.
#
# Usage:
#   sh scripts/check-material-gpu-mirror.sh          (from repo root)
#   sh scripts/check-material-gpu-mirror.sh --quiet  (no output on success)
#
# Expected field order (authoritative; update here when struct changes):
#   albedoTex  normalTex  metallicRoughnessTex  emissiveTex
#   flags  baseColorFactor  metallicFactor  roughnessFactor
#
# Exit 0 = field names and order match. Exit 1 = mismatch.

set -euo pipefail

REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
CPP_FILE="$REPO_ROOT/RenderCore/MaterialGpu.h"
GLSL_FILE="$REPO_ROOT/shaders/include/material_gpu.hglsl"
QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

for f in "$CPP_FILE" "$GLSL_FILE"; do
    [ -f "$f" ] || { echo "ERROR: file not found: $f" >&2; exit 1; }
done

# Extract ordered field names using Python (portable, no grep -P needed).
# Only scans inside the struct MaterialGpu { ... } block to avoid false
# positives from field names appearing in comments or flag constants above.
EXTRACT_PY='
import sys, re
fields = ["albedoTex","normalTex","metallicRoughnessTex","emissiveTex",
          "flags","baseColorFactor","metallicFactor","roughnessFactor"]
seen = []
in_struct = False
for line in open(sys.argv[1], encoding="utf-8"):
    if re.search(r"\bstruct\b.*\bMaterialGpu\b", line):
        in_struct = True
    if in_struct:
        for f in fields:
            if f not in seen and re.search(r"\b" + f + r"\b", line):
                seen.append(f)
        # Stop at closing brace of struct (line starts with "}" or "};").
        if re.match(r"\s*\};", line) and len(seen) > 0:
            break
print("\n".join(seen))
'

CPP_FIELDS=$(python3 -c "$EXTRACT_PY" "$CPP_FILE")
GLSL_FIELDS=$(python3 -c "$EXTRACT_PY" "$GLSL_FILE")

FAIL=0

if [ "$CPP_FIELDS" != "$GLSL_FIELDS" ]; then
    echo "ERROR: MaterialGpu field order mismatch between C++ and GLSL:" >&2
    echo "" >&2
    printf "  %s:\n" "$CPP_FILE" >&2
    echo "$CPP_FIELDS" | sed 's/^/    /' >&2
    echo "" >&2
    printf "  %s:\n" "$GLSL_FILE" >&2
    echo "$GLSL_FIELDS" | sed 's/^/    /' >&2
    echo "" >&2
    echo "Fix: update both files to match the canonical order in this script." >&2
    FAIL=1
fi

# Verify all 8 expected fields present in both files.
EXPECTED="albedoTex normalTex metallicRoughnessTex emissiveTex flags baseColorFactor metallicFactor roughnessFactor"
for field in $EXPECTED; do
    if ! echo "$CPP_FIELDS" | grep -qx "$field"; then
        echo "ERROR: field '$field' missing in $CPP_FILE" >&2; FAIL=1
    fi
    if ! echo "$GLSL_FIELDS" | grep -qx "$field"; then
        echo "ERROR: field '$field' missing in $GLSL_FILE" >&2; FAIL=1
    fi
done

[ "$FAIL" -ne 0 ] && exit 1
[ "$QUIET" -eq 0 ] && echo "OK: MaterialGpu field order matches (8 fields)"
exit 0
