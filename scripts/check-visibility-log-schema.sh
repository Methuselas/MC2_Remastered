#!/bin/sh
# scripts/check-visibility-log-schema.sh
#
# Locks the [VISIBILITY v1] log schema so M3/M4 decisions stay explicit
# and V1A valid-flag tokens cannot be silently dropped.
#
# Invariants enforced:
#   (1) "[VISIBILITY v1] frame=" — schema not renamed or dropped.
#
#   (2) "terrain=deferred" — terrain NOT counted as 0; explicitly deferred.
#       0 implies "nothing visible"; "deferred" = CPU canonical path owns.
#
#   (3) "vfx=prohibited" — VFX PROHIBITED from writing object IDs.
#       (last-write-wins on R32_UINT clobbers mech pick under additive particles;
#       see scripts/check-vfx-no-objectid.sh). "prohibited" is the contract.
#
#   V1A additions:
#   (4) "sp_visible=" — static-prop per-frame visible count token.
#   (5) "sp_valid=" — explicit valid flag; "not armed" != "zero visible".
#   (6) "mech_visible=" — mech per-frame submit count token.
#   (7) "mech_valid=" — explicit valid flag.
#   (8) "gpu_valid=" — GPU readback valid flag; false until V1B.
#       Prevents "gpu_visible=0" being misread as "zero GPU-visible" when
#       readback is simply not armed.
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
check "sp-visible"      'sp_visible='
check "sp-valid"        'sp_valid='
check "mech-visible"    'mech_visible='
check "mech-valid"      'mech_valid='
check "gpu-valid"       'gpu_valid='

if [ $failures -eq 0 ]; then
    echo "OK"
fi
exit $failures
