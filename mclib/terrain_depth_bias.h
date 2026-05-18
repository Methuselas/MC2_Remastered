#pragma once
// Single source of truth for the terrain/water NDC depth bias ("fudge").
//
// LOCKSTEP: shaders/include/terrain_depth_bias.hglsl MUST carry byte-equal
// values and be edited in the SAME commit (see
// memory/cpp_glsl_ubo_struct_lockstep.md). The GLSL sibling cannot read
// this C++ header; the two files are hand-mirrored and verified together.
//
// Bias is added to POST-perspective-divide NDC z (clip.z * rhw, i.e.
// vertices[]->pz / ->wz on the CPU side), in the glClipControl(ZERO_TO_ONE)
// [0,1] depth regime. The 0.002 terrain value is the post-glClipControl
// "doubled" value (was 0.001 under the old [-1,1]->[0,1] remap).
//
// Post-matrix-share design (Fix B): water (FAST regime) and overlays/decals
// project through the bit-identical baked terrain MVP -- symmetric mirror.
// They are co-planar in world space; depth ordering is maintained by two small,
// oppositely-signed epsilons off the shared terrain reference. Single-source
// the mechanism, keep regimes as separate named constants (correction
// 2026-05-16, VPL-#10; do NOT collapse different regimes to one value).
//
//   OVERLAY_DEPTH_BIAS < 0 < WATER_DEPTH_BIAS
//   TERRAIN + OVERLAY_DEPTH_BIAS < TERRAIN < TERRAIN + WATER_DEPTH_BIAS
//
//   WATER_DEPTH_BIAS   > 0: water strictly LOSES the shoreline GL_LEQUAL
//                           tie (preserves v0.3 staircase-scar invariant).
//   OVERLAY_DEPTH_BIAS < 0: decals/overlays strictly WIN the tie over terrain
//                           (replaces removed host glPolygonOffset(-1,-1)).
//
// WATER HAS TWO LEGITIMATE REGIMES -- NOT a desync to collapse:
//
//   FAST  (WATER_DEPTH_BIAS = 0.0005 -> absolute 0.0025): the GPU water VS
//     fast paths gos_terrain_water_fast.vert / _mdi.vert. Post-matrix-share,
//     these project through the same MVP as terrain; delta 0.0005 is the
//     co-planar epsilon. (Pre-Fix B this was 0.001/0.003; that larger delta
//     compensated for projection divergence, which is now gone.)
//   RASTER (WATER_DEPTH_DELTA_RASTER = 0.0005 -> absolute 0.0025): the legacy
//     CPU raster water (mclib/quad.cpp) and mask-water VS
//     (gos_terrain_mask_water.vert). 89d7c4f chose 0.0005 as "the smallest
//     reliable separation that prevents shoreline tile sparkle WITHOUT
//     over-biasing water behind underwater terrain"; a larger delta makes TES
//     low-LOD tiles break through the water as blocky islands at map
//     edges/shore. RASTER is out of scope for Fix B -- do NOT touch it.
//
// Note: FAST and RASTER currently resolve to the same absolute value (0.0025)
// by coincidence -- the post-Fix-B co-planar epsilon (WATER_DEPTH_BIAS 0.0005)
// happens to equal the pre-existing RASTER delta. They remain SEPARATE named
// constants because their future change trajectories differ: FAST is the
// matrix-share co-planar regime and may retune independently; RASTER is the
// untouched legacy CPU-water path out of scope for Fix B.
//
// Hard bound either regime: water absolute < 2*TERRAIN (never >=0.002 over
// terrain -> proven lake-bottom punch-through, 2026-05-06 /
// gos_terrain_water_fast.vert:327-364). Current absolute 0.0025 < 0.004 bound.
namespace mc2depth {
constexpr float TERRAIN_DEPTH_FUDGE      = 0.002f;
// Fix B co-planar signed epsilons (FAST / matrix-share regime):
constexpr float WATER_DEPTH_BIAS         = 0.0005f;   // signed delta; > 0 water loses LEQUAL tie
constexpr float OVERLAY_DEPTH_BIAS       = -0.0005f;  // signed delta; < 0 decals win LEQUAL tie
constexpr float WATER_DEPTH_FUDGE_FAST   = TERRAIN_DEPTH_FUDGE + WATER_DEPTH_BIAS;   // 0.0025f; FAST regime
// RASTER regime (legacy CPU-raster water; out of scope for Fix B -- do NOT change):
constexpr float WATER_DEPTH_DELTA_RASTER = 0.0005f;
constexpr float WATER_DEPTH_FUDGE_RASTER = TERRAIN_DEPTH_FUDGE + WATER_DEPTH_DELTA_RASTER; // 0.0025f
// Back-compat alias: the legacy CPU raster consumer name. quad.cpp's
// WATER_DEPTH_FUDGE consumers (wz + WATER_DEPTH_FUDGE) are the RASTER regime.
constexpr float WATER_DEPTH_FUDGE        = WATER_DEPTH_FUDGE_RASTER;                       // 0.0025f

static_assert(
    OVERLAY_DEPTH_BIAS < 0.0f && 0.0f < WATER_DEPTH_BIAS &&
    (TERRAIN_DEPTH_FUDGE + WATER_DEPTH_BIAS) < 2.0f * TERRAIN_DEPTH_FUDGE,
    "Fix B depth ordering invariant: OVERLAY<0<WATER and water-abs<2*terrain");
}
