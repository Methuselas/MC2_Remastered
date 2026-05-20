// tests/unit/test_projection.cpp
// Tier 2: unit tests for the engine's clip-space depth helpers.
//
// Subjects under test:
//   - projectZ(vec4 clip)        -> float in [0, 1) for in-front points
//   - inverseProjectZ(float pz)  -> view-space z for a given projected pz
//
// Why this exists: memory/clip_w_sign_trap.md. The Stuff projection
// produces both signs of clip.w for visible verts in MC2, so any
// `sign(clip.w)` front-vs-back test silently misclassifies. projectZ()
// is the load-bearing helper that does it right. A test enforcing
// "projectZ on a known in-front vert returns [0,1) regardless of
// clip.w sign" would have caught the underlying bug class on first
// run.
//
// SCOPE-STOPPED 2026-05-20: projectZ is a member function of class Camera
// (mclib/camera.h:431, signature
// `bool Camera::projectZ(Stuff::Vector3D&, Stuff::Vector4D&, LegacyProjectionResult*)`),
// NOT the free `float projectZ(const vec4&)` this sketch hypothesized.
// Extracting the arithmetic out of the Camera class would touch camera.h
// (header consumed by ~30 TUs, class-layout change risk per
// memory/feedback_class_layout_change_needs_clean_first.md), camera.cpp,
// every wrapper at camera.h:516-617, plus the projectz_overlay / trace
// sidecars (mclib/projectz_overlay.{cpp,h}, mclib/projectz_trace.{cpp,h}).
// Far over the ~3-file budget for Tier 2. Revisit when the projectz
// policy-split campaign (docs/superpowers/specs/projectz-policy-split-report.md)
// retires the Camera-bound entry point, at which point the arithmetic
// helper is a natural leaf TU.

#include "doctest.h"
#include <cmath>

// Provisional symbol surface; scope-stopped, see header comment.
#if 0
struct vec4 { float x, y, z, w; };
extern float projectZ(const vec4& clip);
extern float inverseProjectZ(float pz);


TEST_CASE("projectZ on a vert at the near plane returns ~0") {
    // For a standard projection with reverse-Z disabled, a vert exactly
    // on the near plane projects to clip.z/clip.w ~= 0 (in [0,1) space).
    // Use a synthetic clip vert (post-projection) -- this isolates
    // projectZ from any MVP math.
    const vec4 at_near{ 0.0f, 0.0f, 0.0f, 1.0f };
    CHECK(projectZ(at_near) == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("projectZ on a vert just inside far returns ~1 (but strictly < 1)") {
    const vec4 near_far{ 0.0f, 0.0f, 0.9999f, 1.0f };
    const float pz = projectZ(near_far);
    CHECK(pz < 1.0f);
    CHECK(pz == doctest::Approx(0.9999f).epsilon(1e-4));
}

TEST_CASE("projectZ is robust to negative clip.w (the Stuff-matrix trap)") {
    // memory/clip_w_sign_trap.md: an in-front vertex can yield clip.w < 0
    // under the engine's projection. A correct projectZ returns the same
    // pz regardless of (clip.z, clip.w) sign pair, as long as the point
    // is geometrically in front of the camera.
    //
    // Pair up two clip vectors that describe the SAME world-space point
    // under sign-flipped (clip.z, clip.w). Their projectZ values must
    // match.
    const vec4 a{ 0.0f, 0.0f,  0.5f,  1.0f };
    const vec4 b{ 0.0f, 0.0f, -0.5f, -1.0f };
    CHECK(projectZ(a) == doctest::Approx(projectZ(b)).epsilon(1e-6));
}

TEST_CASE("projectZ output for in-front points lies in [0, 1)") {
    // Sample a range of in-front depths. All must land in the strict
    // [0, 1) interval -- this is the contract the engine's culling /
    // depth-fudge code relies on.
    for (float z = 0.0f; z < 1.0f; z += 0.1f) {
        const vec4 v{ 0.0f, 0.0f, z, 1.0f };
        const float pz = projectZ(v);
        CHECK(pz >= 0.0f);
        CHECK(pz <  1.0f);
    }
}

TEST_CASE("inverseProjectZ is the left-inverse of projectZ") {
    // Round-trip property: for any in-front clip vert,
    //   inverseProjectZ(projectZ(v))  recovers v.z / v.w  (i.e. NDC z).
    for (float z = 0.05f; z < 0.95f; z += 0.1f) {
        const vec4 v{ 0.0f, 0.0f, z, 1.0f };
        const float pz   = projectZ(v);
        const float back = inverseProjectZ(pz);
        CHECK(back == doctest::Approx(z).epsilon(1e-5));
    }
}

TEST_CASE("projectZ is monotonic in clip-space depth") {
    // Strict ordering: a vert farther from the camera projects to a
    // larger pz. This is the property the GPU's depth-test side relies
    // on; if it ever breaks, every depth-bias / fence-seam fix becomes
    // direction-ambiguous.
    float prev = -1.0f;
    for (float z = 0.0f; z < 1.0f; z += 0.05f) {
        const vec4 v{ 0.0f, 0.0f, z, 1.0f };
        const float pz = projectZ(v);
        CHECK(pz > prev);
        prev = pz;
    }
}

// NOTE for the reverse-Z arm of the engine
// (memory/clip_w_sign_trap.md notes reverse-Z fence-seam paths exist):
// when reverse-Z is enabled the monotonicity is INVERTED. The right
// test is to parametrize this whole file on the reverse-Z flag once
// it surfaces in the leaf TU, and have both arms asserted explicitly.
#endif // SCOPE-STOPPED projection tests

// Placeholder test case so the TU isn't empty (doctest is happy either
// way; this just keeps a visible breadcrumb in the test list).
TEST_CASE("projection tests scope-stopped" * doctest::skip(true)) {
    // See top-of-file SCOPE-STOPPED header for the extraction blocker.
}
