#pragma once
//---------------------------------------------------------------------------
// Track A1 — object admission predicate (clip-space frustum).
//
// This is the modern bool decision used by Camera::projectForObjectAdmission
// when MC2_OBJECT_ADMISSION_PREDICATE=modern, AND the function the trace
// system uses to compute its homogClipFull candidate predicate. Both
// callers must use the same function — drift would invalidate the
// dual-run parity gate.
//
// Spec: docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md
// Plan: docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
//---------------------------------------------------------------------------

namespace Stuff { class Vector4D; }

// Mode probed at startup from MC2_OBJECT_ADMISSION_PREDICATE env var.
enum class ObjectAdmissionPredicateMode {
    Legacy,    // default: bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Called lazily from objectAdmissionPredicateMode().
void objectAdmissionPredicate_init();

// Read the active mode. Lazily initializes — startup ordering is non-load-bearing.
ObjectAdmissionPredicateMode objectAdmissionPredicateMode();

// The predicate itself. Returns true iff the homogeneous clip-space point is
// inside the canonical clip volume:
//   rawClip.w >  0
//   |rawClip.x| <= rawClip.w   (left/right planes)
//   |rawClip.y| <= rawClip.w   (top/bottom planes)
//   0 <= rawClip.z <= rawClip.w (near/far planes; MC2 uses D3D-style [0,w])
//
// Note: rawClip is `xformCoords` from inside Camera::projectZ — the post-multiply,
// pre-divide clip-space coord. Always use the value captured via
// LegacyProjectionResult::rawClip; do NOT pass the post-divide screen vector.
bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip);

// Self-test entry point. Runs the unit cases listed in
// docs/superpowers/plans/2026-05-06-track-a1-object-admission-predicate.md
// Task 3. Prints `[OBJECT_ADMISSION v1] event=selftest_pass|fail case=<name>`
// per case. Returns the number of failures (0 = all pass). Caller is
// expected to fail loudly on non-zero return — see Task 4.
int objectAdmissionPredicate_selftest();

//---------------------------------------------------------------------------
// Track A2 - effect admission mode (sibling of the Track A1 object mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag so the two slices
// can be flipped on independently during soak.
//---------------------------------------------------------------------------

enum class EffectAdmissionPredicateMode {
    Legacy,    // default: bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe. Idempotent. Called lazily from effectAdmissionPredicateMode().
void effectAdmissionPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
EffectAdmissionPredicateMode effectAdmissionPredicateMode();

//---------------------------------------------------------------------------
// Track A1 - lighting/shadow predicate mode (sibling of object + effect modes).
// Same clipSpaceFrustumAdmit predicate; separate env flag for independent soak.
// Default: Legacy (lighting/shadow activation gating is low-risk; flip after
// broader canary coverage of object/effect modes).
//---------------------------------------------------------------------------

enum class LightingShadowPredicateMode {
    Legacy,    // bool comes from projectZ's screen-rect test
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe. Idempotent. Called lazily from lightingShadowPredicateMode().
void lightingShadowPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
LightingShadowPredicateMode lightingShadowPredicateMode();

//---------------------------------------------------------------------------
// Track A1 - debug overlay predicate mode (sibling of lighting/shadow mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag for independent soak.
// Default: Legacy (debug overlay draws are lab-only; low-risk flip candidate).
//---------------------------------------------------------------------------

enum class DebugOverlayPredicateMode {
    Legacy,    // bool comes from legacy projectZ screen-rect acceptance
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Reads env MC2_DEBUG_OVERLAY_PREDICATE_MODE
// (Legacy / Modern). Default = Legacy.
void debugOverlayPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
DebugOverlayPredicateMode debugOverlayPredicateMode();

//---------------------------------------------------------------------------
// Track A1 - selection picking predicate mode (sibling of lighting/shadow mode).
// Same clipSpaceFrustumAdmit predicate; separate env flag for independent soak.
// Default: Legacy (picking bool is discarded by callers; screen.xy still read).
//---------------------------------------------------------------------------

enum class SelectionPickingPredicateMode {
    Legacy,    // bool comes from legacy projectZ screen-rect acceptance
    Modern,    // bool comes from clipSpaceFrustumAdmit(rawClip)
};

// One-time probe (idempotent). Reads env MC2_SELECTION_PICKING_PREDICATE_MODE
// (Legacy / Modern). Default = Legacy.
void selectionPickingPredicate_init();

// Read the active mode. Lazy-initializes; startup ordering is non-load-bearing.
SelectionPickingPredicateMode selectionPickingPredicateMode();
