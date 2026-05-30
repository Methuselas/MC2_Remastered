// tests/unit/test_depth_hzb.cpp
// HZB-DEPTH-CONVENTION-TESTS-1: GL-free CPU reference tests that pin the
// reverse-Z / Hi-Z (HZB) depth contract BEFORE any runtime HZB pyramid or
// occlusion-culling code exists.
//
// WHY THIS FILE EXISTS
// --------------------
// A wrong HZB min/max reduction convention does not crash -- it silently
// over-culls visible geometry, producing intermittent invisible-object bugs
// that are extremely expensive to diagnose later (see the month-long
// shadow/water invisible-prop arcs in this project's history). This file is
// semantic infrastructure: it encodes the depth contract as executable,
// CI-checked assertions so the future runtime HZB slice
// (TRACKRV-HZB-VISIBILITY-OPUS-1) has a fixed target to match.
//
// PROJECT DEPTH CONTRACT (recon-verified, do NOT change without re-verifying):
//   * Reverse-Z:        near plane -> depth 1.0, far plane -> depth 0.0.
//                       LARGER depth value == CLOSER to the camera.
//                       (gameosmain.cpp glClearDepth(0.0f); gameos_graphics.cpp
//                        glDepthFunc(GL_GREATER); appear.h HUD_DEPTH 0.9999f;
//                        terrain_depth_bias.h "GEQUAL: the LARGER NDC z wins".)
//   * Clip / NDC depth: ZERO_TO_ONE  (glClipControl(GL_LOWER_LEFT,
//                       GL_ZERO_TO_ONE) -- mandatory / fail-closed). Window
//                       depth and NDC z both live in [0,1].
//   * Sky / far / no-geometry sentinel: depth == 0.0 (clear value).
//
// All math below is LOCAL REFERENCE MATH. The project has no depth-linearize
// or HZB-reduce helper yet (recon: none found). When the runtime HZB lands it
// MUST reproduce these exact rules; if a reusable helper is later promoted
// into RenderCore, port these tests to call it instead of the locals here.
#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Reference depth math (reverse-Z, ZERO_TO_ONE). Local to the test; mirrors
// the regime the engine enforces at runtime.
// ---------------------------------------------------------------------------

// Map a positive view-space distance (eye units, +ahead) to reverse-Z
// ZERO_TO_ONE window depth for a standard perspective projection.
//
//   forward-Z 0..1 :  z01 = far/(far-near) * (1 - near/d)
//   reverse-Z 0..1 :  depth = 1 - z01   (near->1, far->0)
//
// This is the *intuition* model used by the tests -- the runtime uses the GPU
// projection matrix, but the near=1/far=0 endpoints and monotonic ordering
// below are the contract that matters for HZB.
double reverseZDepthFromDistance(double dist, double nearD, double farD) {
    const double z01 = (farD / (farD - nearD)) * (1.0 - nearD / dist);
    return 1.0 - z01;
}

// "Is a closer than b?" under the project's reverse-Z convention.
bool isCloser(double depthA, double depthB) { return depthA > depthB; }

// Sky / far / no-geometry test. Far == clear == 0.0.
bool isSkyOrFar(double depth) { return depth <= 0.0; }

// ---- HZB reduction --------------------------------------------------------
//
// CONSERVATIVE OCCLUSION REDUCTION FOR REVERSE-Z == min().
//
// Derivation (the whole reason this file exists):
//   The scene depth buffer holds, per pixel, the NEAREST surface -> the
//   LARGEST reverse-Z value. An object is occluded across a tile iff its
//   closest point lies behind the occluder at EVERY pixel of the tile. The
//   WEAKEST occluder (the one that hides the least) is the farthest-back
//   surface in the tile == the SMALLEST depth value. To never cull a visible
//   object, the coarse HZB texel must store that weakest/farthest occluder,
//   i.e. the MINIMUM child depth. So the reverse-Z HZB is a MIN pyramid.
//
//   Cull test then reads: cull iff  objClosestDepth (max over footprint)
//                                   <  hzbTexel (min over footprint).
//
//   Forward-Z (near=0/far=1) would store MAX. Reverse-Z FLIPS this to MIN.
//   Storing MAX here would record the nearest occluder and over-cull objects
//   that are actually visible behind a thin foreground sliver -> invisible
//   objects. That inversion is the entire trap this slice guards against.
double hzbReduce2x2(double a, double b, double c, double d) {
    return std::min(std::min(a, b), std::min(c, d));
}

// Conservative reverse-Z cull decision (matches the comment above).
// objClosestDepth = max reverse-Z depth over the object's screen footprint.
// hzbTexel        = reduced (min) occluder depth for the covered tile.
bool hzbCullsObject(double objClosestDepth, double hzbTexel) {
    return objClosestDepth < hzbTexel;
}

// ---- Mip dimension ladder -------------------------------------------------
//
// Each level halves each dimension INDEPENDENTLY and ROUNDS UP (ceil), floored
// at 1, so the coarse level fully covers the finer one with a clamped 2x2
// fetch -- no source texel is ever dropped on an odd extent. This differs from
// GL's native glGenerateMipmap, which (a) AVERAGES (wrong: HZB needs min) and
// (b) floors the size. The runtime HZB therefore needs a custom reduction
// pass; it cannot lean on glGenerateMipmap. Round-up keeps the chain
// conservative without per-level 3x3 edge taps.
uint32_t hzbNextMipDim(uint32_t d) {
    if (d <= 1u) return 1u;            // 1 is the terminal, idempotent extent
    return (d + 1u) / 2u;             // ceil(d/2)
}

// Full ladder for a single axis, terminating at and INCLUDING the 1 extent.
std::vector<uint32_t> hzbMipLadder(uint32_t base) {
    std::vector<uint32_t> levels;
    uint32_t d = base == 0u ? 0u : base;
    if (d == 0u) return levels;       // degenerate; no levels
    levels.push_back(d);
    while (d > 1u) {
        d = hzbNextMipDim(d);
        levels.push_back(d);
    }
    return levels;
}

} // namespace

TEST_SUITE("DepthHZB") {

// --- Depth convention ------------------------------------------------------

TEST_CASE("DepthConvention reverse Z maps near to one and far to zero") {
    const double nearD = 1.0;
    const double farD  = 1000.0;

    const double dNear = reverseZDepthFromDistance(nearD, nearD, farD);
    const double dFar  = reverseZDepthFromDistance(farD,  nearD, farD);

    CHECK(dNear == doctest::Approx(1.0)); // near plane -> 1.0
    CHECK(dFar  == doctest::Approx(0.0)); // far  plane -> 0.0
}

TEST_CASE("DepthConvention ZERO_TO_ONE keeps depth inside the unit range") {
    const double nearD = 1.0;
    const double farD  = 1000.0;
    // Sample across the frustum; every window-depth must stay in [0,1].
    for (double dist = nearD; dist <= farD; dist += 7.3) {
        const double depth = reverseZDepthFromDistance(dist, nearD, farD);
        CHECK(depth >= -1e-9);
        CHECK(depth <= 1.0 + 1e-9);
    }
}

TEST_CASE("DepthConvention larger depth means closer") {
    const double nearD = 1.0;
    const double farD  = 1000.0;

    const double dClose = reverseZDepthFromDistance(10.0,  nearD, farD);
    const double dMid   = reverseZDepthFromDistance(100.0, nearD, farD);
    const double dFar   = reverseZDepthFromDistance(900.0, nearD, farD);

    // Monotonic: nearer surfaces carry strictly larger reverse-Z depth.
    CHECK(dClose > dMid);
    CHECK(dMid   > dFar);

    CHECK(isCloser(dClose, dFar));
    CHECK_FALSE(isCloser(dFar, dClose));
}

TEST_CASE("DepthConvention far and sky sentinel is zero") {
    CHECK(isSkyOrFar(0.0));          // clear value / no geometry
    CHECK_FALSE(isSkyOrFar(0.25));   // real geometry is > 0
    CHECK_FALSE(isSkyOrFar(1.0));    // near plane is decidedly not sky
}

// --- HZB reduction convention ----------------------------------------------

TEST_CASE("HZB reverse Z parent stores farthest occluder via min reduction") {
    // Four child depths; smallest == farthest occluder under reverse-Z.
    const double nearest  = 0.90; // closest surface in the tile
    const double farthest = 0.10; // weakest occluder in the tile
    const double parent = hzbReduce2x2(nearest, 0.55, farthest, 0.40);

    // The conservative reverse-Z HZB keeps the farthest (min), NOT the nearest.
    CHECK(parent == doctest::Approx(farthest));
    CHECK(parent != doctest::Approx(nearest));
}

TEST_CASE("HZB conservative cull never hides a visible object") {
    // Tile occluders range from a thin near sliver (0.95) to a far wall (0.30).
    // Conservative reduction must keep the FAR wall (0.30) so an object that
    // pokes out behind the sliver is NOT culled.
    const double hzb = hzbReduce2x2(0.95, 0.80, 0.60, 0.30);
    CHECK(hzb == doctest::Approx(0.30));

    // Object whose closest point (0.50) is NEARER than the kept occluder
    // (0.30 is farther) -> visible -> must NOT be culled.
    CHECK_FALSE(hzbCullsObject(/*objClosest=*/0.50, hzb));

    // A MAX reduction would have stored 0.95 and wrongly culled this object:
    const double wrongMaxHzb = std::max(std::max(0.95, 0.80), std::max(0.60, 0.30));
    CHECK(hzbCullsObject(/*objClosest=*/0.50, wrongMaxHzb)); // the invisible-object bug
}

TEST_CASE("HZB conservative cull rejects a fully occluded object") {
    // Whole tile is a near wall; the farthest occluder is still close (0.70).
    const double hzb = hzbReduce2x2(0.85, 0.80, 0.75, 0.70);
    CHECK(hzb == doctest::Approx(0.70));

    // Object entirely behind that wall (closest point 0.40 < 0.70) -> occluded.
    CHECK(hzbCullsObject(/*objClosest=*/0.40, hzb));
}

TEST_CASE("HZB reduction is order independent and idempotent") {
    CHECK(hzbReduce2x2(0.1, 0.2, 0.3, 0.4) == doctest::Approx(0.1));
    CHECK(hzbReduce2x2(0.4, 0.3, 0.2, 0.1) == doctest::Approx(0.1));
    // Uniform tile reduces to itself (a flat region stays flat up the pyramid).
    CHECK(hzbReduce2x2(0.5, 0.5, 0.5, 0.5) == doctest::Approx(0.5));
}

// --- Mip dimension ladder ---------------------------------------------------

TEST_CASE("HZB mip dimensions halve for even extents") {
    CHECK(hzbNextMipDim(8) == 4u);
    CHECK(hzbNextMipDim(4) == 2u);
    CHECK(hzbNextMipDim(2) == 1u);
    CHECK(hzbMipLadder(8) == std::vector<uint32_t>({8, 4, 2, 1}));
}

TEST_CASE("HZB mip dimensions round up for odd extents") {
    CHECK(hzbNextMipDim(7) == 4u);  // ceil(3.5) -> 4, not floor 3
    CHECK(hzbNextMipDim(5) == 3u);
    CHECK(hzbNextMipDim(3) == 2u);
    // 7 reaches 1 via 4,2; 5 via 3,2. No source texel dropped on the way.
    CHECK(hzbMipLadder(7) == std::vector<uint32_t>({7, 4, 2, 1}));
    CHECK(hzbMipLadder(5) == std::vector<uint32_t>({5, 3, 2, 1}));
}

TEST_CASE("HZB mip dimensions reduce each axis independently for 1xN and Nx1") {
    // A 1xN (or Nx1) target collapses the long axis while the unit axis stays 1.
    CHECK(hzbMipLadder(1) == std::vector<uint32_t>({1}));
    CHECK(hzbMipLadder(6) == std::vector<uint32_t>({6, 3, 2, 1}));
    // e.g. a 1x6 image: rows ladder = {1}, cols ladder = {6,3,2,1}; the chain
    // length is max(1, 4) = 4 levels, the unit axis clamped at 1 throughout.
}

TEST_CASE("HZB 1x1 terminal mip is stable") {
    CHECK(hzbNextMipDim(1) == 1u);   // 1 reduces to 1 (idempotent terminal)
    CHECK(hzbNextMipDim(0) == 1u);   // defensive: never emit a 0 extent
    CHECK(hzbMipLadder(1) == std::vector<uint32_t>({1}));
    // Re-reducing the terminal must not grow or oscillate the chain.
    CHECK(hzbNextMipDim(hzbNextMipDim(1)) == 1u);
}

} // TEST_SUITE("DepthHZB")
