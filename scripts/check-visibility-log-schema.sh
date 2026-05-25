#!/bin/sh
# scripts/check-visibility-log-schema.sh
#
# Locks the [VISIBILITY v1] log schema so M3/M4 decisions stay explicit.
#
# Invariants enforced:
#   (1) The literal "[VISIBILITY v1] frame=" must appear in source --
#       confirms the schema string has not been renamed or dropped.
#
#   (2) "terrain=deferred" must appear in the visibility log emit --
#       terrain is NOT counted as 0; it is explicitly deferred.
#       This distinction matters: 0 implies "nothing visible", while
#       "deferred" signals "not yet wired, CPU canonical path still owns".
#
#   (3) "vfx=prohibited" must appear in the visibility log emit --
#       VFX shaders are PROHIBITED from writing object IDs (last-write-wins
#       on R32_UINT attachment-2 clobbers mech pick under additive particles;
#       see scripts/check-vfx-no-objectid.sh). "prohibited" is the contract;
#       "0" would silently hide it.
#
# Source of truth: RenderWorld/RenderWorld.cpp (the only emitter).
# Run before committing any change to RenderWorld/RenderWorld.cpp or
# RenderWorld/VisibilityRequest.h.
#
# Exit 0 = all invariants hold. Exit 1 = at least one invariant violated.

set -e
TARGET="RenderWorld/RenderWorld.cpp"
failures=0

check() {
    label="$1"
    pattern="$2"
    if ! grep -qF "$pattern" "$TARGET"; then
        echo "[VISIBILITY-SCHEMA] FAIL ($label): pattern not found in $TARGET"
        echo "  Expected: $pattern"
        failures=1
    fi
}

check "schema-token"    '[VISIBILITY v1] frame='
check "terrain-tag"     'terrain=deferred'
check "vfx-tag"         'vfx=prohibited'

if [ $failures -eq 0 ]; then
    echo "OK"
fi
exit $failures
