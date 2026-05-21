#!/usr/bin/env sh
# scripts/check-particles-no-cpu-projection.sh
#
# Pre-commit gate per integrated plan v5 §5.4 B1 Stage 1' Commit 3
# (v2 MAJOR-3 fold-in).
#
# B1-phase scope (this script today):
#   Forbid CPU-projection APIs INSIDE mclib/particles/. The GPU particle
#   pipeline is projection-only on the GPU; any CPU-side projectZ /
#   cameraToClip / worldToClipMatrix / projectForObjectAdmission /
#   projectForEffectAdmission call inside mclib/particles/ regresses the
#   substitutive-test discipline (memory/gpu_offload_must_be_substitutive_
#   not_additive.md) by silently re-introducing CPU projection work that
#   the gosFX retirement campaign exists to delete.
#
# A4-phase scope (FUTURE — script tightens when A4 lands):
#   Repo-wide ban on theClipper|MLRClipper with an explicit source-tree
#   allowlist (see plan §6.3, §6.7). The B1-phase scope is intentionally
#   narrow because gosFX/MLR is still alive during B1 and transitional
#   code may incidentally reference those names.
#
# Convention: matches scripts/check-destroy-invariant.sh + scripts/
# check-asset-scale-callers.sh style. Exit 0 = pass; nonzero = fail.

set -eu

FORBIDDEN_B1='cameraToClip|Camera::projectZ|worldToClipMatrix|projectForObjectAdmission|projectForEffectAdmission'

# Use git grep with an explicit pathspec so this gate doesn't false-positive
# on .planning/, docs/, memory/, or the gate script itself.
HITS=$(git grep -nE "$FORBIDDEN_B1" -- 'mclib/particles/**' || true)

if [ -n "$HITS" ]; then
    printf '%s\n' "[check-particles-no-cpu-projection] FAIL — forbidden CPU-projection API inside mclib/particles/:" >&2
    printf '%s\n' "$HITS" >&2
    printf '%s\n' "" >&2
    printf '%s\n' "Pattern: $FORBIDDEN_B1" >&2
    printf '%s\n' "Plan rationale: docs/superpowers/plans/2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md §5.4" >&2
    exit 1
fi

exit 0
