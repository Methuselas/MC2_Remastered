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
// WATER HAS TWO LEGITIMATE REGIMES -- this is NOT a desync to collapse
// (correction 2026-05-16 after a 1-constant unification regressed the
// map-edge terrain; see git 89d7c4f vs 6ff6c5c, VPL-#10):
//
//   FAST  (delta 0.001 -> 0.003): the GPU water vertex-shader fast paths
//     gos_terrain_water_fast.vert / _mdi.vert. 6ff6c5c set 0.003 because
//     on THOSE shaders delta 0.0005 made GL_LEQUAL coast ties
//     non-deterministic (shoreline shimmer).
//   RASTER (delta 0.0005 -> 0.0025): the legacy CPU raster water
//     (mclib/quad.cpp) and the mask-water VS (gos_terrain_mask_water.vert).
//     89d7c4f deliberately chose 0.0005 here as "the smallest reliable
//     separation that prevents shoreline tile sparkle WITHOUT over-biasing
//     water behind underwater terrain"; a larger delta makes TES low-LOD
//     tiles break through the water surface as blocky islands at map
//     edges/shore (the regression that a 1-constant 0.003 unification
//     reintroduced).
//
// The GENUINE #10 desync was only gos_terrain_mask_water.vert claiming (in
// a false comment) to match the FAST path while emitting 0.0025 -- its
// REAL peer is the CPU raster (RASTER regime), so it is resynced to
// RASTER, not FAST. terrain stays 0.002 on all four terrain sites.
//
// Hard bound either regime: water delta > 0 and < 2*TERRAIN (never
// >= 0.002 absolute over terrain -> proven lake-bottom-coverage failure,
// 2026-05-06 / gos_terrain_water_fast.vert:327-364). A single constant
// cannot satisfy both regimes; the permanent unifier is the deferred
// distance-proportional clip-z bias (memory/vulkan_aligned_depth_bias_ruling.md).
namespace mc2depth {
constexpr float TERRAIN_DEPTH_FUDGE      = 0.002f;
constexpr float WATER_DEPTH_DELTA_FAST   = 0.001f;
constexpr float WATER_DEPTH_DELTA_RASTER = 0.0005f;
constexpr float WATER_DEPTH_FUDGE_FAST   = TERRAIN_DEPTH_FUDGE + WATER_DEPTH_DELTA_FAST;   // 0.003f
constexpr float WATER_DEPTH_FUDGE_RASTER = TERRAIN_DEPTH_FUDGE + WATER_DEPTH_DELTA_RASTER; // 0.0025f
// Back-compat alias: the legacy CPU raster consumer name. quad.cpp's
// WATER_DEPTH_FUDGE consumers (wz + WATER_DEPTH_FUDGE) are the RASTER regime.
constexpr float WATER_DEPTH_FUDGE        = WATER_DEPTH_FUDGE_RASTER;                       // 0.0025f
}
