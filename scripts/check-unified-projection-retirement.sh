#!/bin/sh
# F1 unified-projection retirement gate (spec §6.2.1 + §6.2.2).
# Fails CI if legacy projection scaffolding survives Stage A.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ALLOWLIST="$SCRIPT_DIR/check-unified-projection-retirement.allowlist"

fail=0

# Uniform name retirement (terrainMVP renamed to u_worldToClipGL).
echo "Checking terrainMVP retirement..."
hits=$(grep -RE "terrainMVP|u_terrainMVP|gos_SetTerrainMVP" code GameOS mclib shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: legacy terrainMVP scaffolding found:"
    echo "$hits_filtered"
    fail=1
fi

# abs(.w) round-trip kludge retirement.
echo "Checking abs(clip.w) kludge retirement..."
hits=$(grep -RE "abs\(\s*[a-zA-Z_]+\.w\s*\)" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: abs(.w) kludge survives:"
    echo "$hits_filtered"
    fail=1
fi

# terrainViewport retirement.
echo "Checking terrainViewport retirement..."
hits=$(grep -RE "terrainViewport|u_terrainViewport" code GameOS mclib shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: terrainViewport survives:"
    echo "$hits_filtered"
    fail=1
fi

# Screen-rhw round-trip retirement.
echo "Checking screen-rhw round-trip retirement..."
hits=$(grep -RE "\*\s*rhw\s*[\+\-\*]" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: screen-rhw round-trip survives:"
    echo "$hits_filtered"
    fail=1
fi

# Bare mvp retirement in terrain shader set.
echo "Checking bare-mvp uniform retirement (terrain set only)..."
hits=$(grep -RE "\buniform[[:space:]]+mat4[[:space:]]+u?_?mvp\b" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST" 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: bare mvp uniform in non-allowlisted shader:"
    echo "$hits_filtered"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Either: (a) finish migrating the flagged sites, or (b) add to"
    echo "$ALLOWLIST with a documented reason."
    exit 1
fi

echo "PASS: unified-projection retirement complete."
