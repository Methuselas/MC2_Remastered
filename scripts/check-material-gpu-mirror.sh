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

# --- MATERIAL-GPU-STRUCT-STRIDE-CHECK-1 -------------------------------------
# Struct-ABI safety across ALL MaterialGpu[] producer/consumer lanes (the
# "split-brain" the GPU-MATERIAL-CONTRACT-RECON-1 flagged: 3 SSBO tables —
# static-prop binding 5 [shader-read], mech profile binding 7, mech per-actor
# binding 2 — all share the ONE 32 B MaterialGpu struct). This guards the struct
# ABI those lanes implicitly agree on; it is NOT a binding-number contract
# (binding occupancy is WARN-covered by check-binding-slots.py) and NOT material
# unification (M4, deferred — see docs/render-backend-seams/gpu-material-contract-recon-1.md).
# It catches: a SECOND divergent MaterialGpu definition (C++ or GLSL) instead of
# reusing the one struct / #include; removal of the 32 B size static_assert; a
# GLSL MaterialTable buffer block whose element type is not MaterialGpu.
STRIDE_PY='
import os, re, sys
root = sys.argv[1]
CANON = ["albedoTex","normalTex","metallicRoughnessTex","emissiveTex",
         "flags","baseColorFactor","metallicFactor","roughnessFactor"]
viol = []

def fields_in(path, start_re):
    seen, ins = [], False
    for line in open(path, encoding="utf-8", errors="replace"):
        if re.search(start_re, line): ins = True
        if ins:
            for f in CANON:
                if f not in seen and re.search(r"\b"+f+r"\b", line): seen.append(f)
            if re.match(r"\s*\};", line) and seen: break
    return seen

# 1. C++ struct DEFINITIONS (brace; exclude forward decls "struct MaterialGpu;")
cpp_defs = []
for d in ["RenderCore","GameOS","mclib","code"]:
    base = os.path.join(root, d)
    for dp,_,fns in os.walk(base):
        for fn in fns:
            if not fn.endswith((".h",".hpp",".cpp",".cc",".cxx")): continue
            p = os.path.join(dp, fn)
            for line in open(p, encoding="utf-8", errors="replace"):
                if re.search(r"\bstruct\b[^;]*\bMaterialGpu\b[^;]*\{", line):
                    cpp_defs.append(os.path.relpath(p, root).replace("\\","/"))
                    break
if len(cpp_defs) != 1:
    viol.append("expected exactly 1 C++ struct MaterialGpu definition, found %d: %s"
                % (len(cpp_defs), cpp_defs))

# 2. size static_assert present
mg = os.path.join(root, "RenderCore", "MaterialGpu.h")
if "sizeof(MaterialGpu) == 32" not in open(mg, encoding="utf-8", errors="replace").read():
    viol.append("RenderCore/MaterialGpu.h lost its static_assert(sizeof(MaterialGpu) == 32)")

# 3. GLSL struct DEFINITIONS — exactly 1 (the mirror); any def must match CANON
glsl_defs = []
shroot = os.path.join(root, "shaders")
for dp,_,fns in os.walk(shroot):
    for fn in fns:
        if not fn.endswith((".frag",".vert",".comp",".tesc",".tese",".geom",".glsl",".hglsl")): continue
        p = os.path.join(dp, fn); rel = os.path.relpath(p, root).replace("\\","/")
        txt = open(p, encoding="utf-8", errors="replace").read()
        if re.search(r"\bstruct\s+MaterialGpu\s*\{", txt):
            glsl_defs.append(rel)
            got = fields_in(p, r"\bstruct\s+MaterialGpu\s*\{")
            if got != CANON:
                viol.append("GLSL %s struct MaterialGpu fields %s != canonical %s" % (rel, got, CANON))
if len(glsl_defs) != 1:
    viol.append("expected exactly 1 GLSL struct MaterialGpu definition (the mirror), found %d: %s"
                % (len(glsl_defs), glsl_defs))

# 4. every GLSL "buffer MaterialTable {" block uses MaterialGpu as element type
for dp,_,fns in os.walk(shroot):
    for fn in fns:
        if not fn.endswith((".frag",".vert",".comp",".tesc",".tese",".geom",".glsl",".hglsl")): continue
        p = os.path.join(dp, fn); rel = os.path.relpath(p, root).replace("\\","/")
        lines = open(p, encoding="utf-8", errors="replace").read().splitlines()
        for i,l in enumerate(lines):
            if re.search(r"\bbuffer\s+MaterialTable\b", l) and "//" not in l.split("buffer")[0]:
                window = "\n".join(lines[i:i+12])
                if "MaterialGpu" not in window:
                    viol.append("GLSL %s:%d buffer MaterialTable block does not reference MaterialGpu" % (rel, i+1))

for v in viol: print("ERROR (stride-check): " + v, file=sys.stderr)
sys.exit(1 if viol else 0)
'
if ! python3 -c "$STRIDE_PY" "$REPO_ROOT"; then
    echo "ERROR: MaterialGpu struct-stride/ABI cross-check failed (see above)." >&2
    FAIL=1
fi

[ "$FAIL" -ne 0 ] && exit 1
[ "$QUIET" -eq 0 ] && echo "OK: MaterialGpu field order matches (8 fields) + struct-ABI cross-check (1 C++ def, 1 GLSL def, size assert, MaterialTable element type)"
exit 0
