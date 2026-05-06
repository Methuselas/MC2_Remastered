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
