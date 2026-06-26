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
// REVERSE-Z [near->1, far->0] depth regime. The depth comparison is GEQUAL:
// the LARGER NDC z wins. Under reverse-Z every signed bias INVERTS relative
// to the old forward-Z/LEQUAL convention so the tie-break invariants
// (decals win over terrain; water loses) are preserved. The 0.002 terrain
// magnitude is unchanged (it was the post-glClipControl "doubled" value);
// only its SIGN flips for reverse-Z.
//
// Post-matrix-share design (Fix B): water (FAST regime) and overlays/decals
// project through the bit-identical baked terrain MVP -- symmetric mirror.
// They are co-planar in world space; depth ordering is maintained by two small,
// oppositely-signed epsilons off the shared terrain reference. Single-source
// the mechanism, keep regimes as separate named constants (correction
// 2026-05-16, VPL-#10; do NOT collapse different regimes to one value).
//
//   OVERLAY_DEPTH_BIAS > 0 > WATER_DEPTH_BIAS   (reverse-Z / GEQUAL)
//   TERRAIN + WATER_DEPTH_BIAS < TERRAIN < TERRAIN + OVERLAY_DEPTH_BIAS
//   (all three are now negative; TERRAIN_DEPTH_FUDGE = -0.002)
//
//   WATER_DEPTH_BIAS   < 0: water strictly LOSES the shoreline GL_GEQUAL
//                           tie (smaller reverse-Z z; preserves v0.3
//                           staircase-scar invariant).
//   OVERLAY_DEPTH_BIAS > 0: decals/overlays strictly WIN the GEQUAL tie
//                           over terrain (larger reverse-Z z; replaces the
//                           removed host glPolygonOffset(-1,-1)).
//
// WATER HAS TWO LEGITIMATE REGIMES -- NOT a desync to collapse:
//
//   FAST  (WATER_DEPTH_BIAS = -0.0005 -> absolute -0.0025): the GPU water VS
//     fast paths gos_terrain_water_fast.vert / _mdi.vert. Post-matrix-share,
//     these project through the same MVP as terrain; delta -0.0005 is the
//     co-planar epsilon under reverse-Z.
//   RASTER (WATER_DEPTH_DELTA_RASTER = -0.0005 -> absolute -0.0025): the
//     legacy CPU raster water (mclib/quad.cpp) and mask-water VS
//     (gos_terrain_mask_water.vert). 89d7c4f chose magnitude 0.0005 as "the
//     smallest reliable separation that prevents shoreline tile sparkle
//     WITHOUT over-biasing water behind underwater terrain". The RASTER
//     regime ALSO flips for reverse-Z: the legacy CPU-raster water +
//     inverseProjectZ path is reconciled by the COORDINATE-TRANSFORM fence
//     seam (reverse-Z -> forward-Z before the legacy fudge math), NOT by
//     keeping any constant un-flipped. The Fix-B-era "RASTER ... do NOT
//     touch / do NOT change" instruction is EXPLICITLY SUPERSEDED by
//     reverse-Z (design Section 9).
//
// Note: FAST and RASTER currently resolve to the same absolute value
// (-0.0025) by coincidence -- the co-planar epsilon (WATER_DEPTH_BIAS
// -0.0005) happens to equal the RASTER delta. They remain SEPARATE named
// constants because their future change trajectories differ.
//
// Hard bound either regime: |water absolute| < 2*|TERRAIN| (never magnitude
// >=0.002 over terrain -> proven lake-bottom punch-through, 2026-05-06 /
// gos_terrain_water_fast.vert:327-364). Current |absolute| 0.0025 < 0.004
// bound. Expressed for the now-negative signs: water-abs > 2*TERRAIN.
// TERRAIN-DEPTH-BIAS-OWNERSHIP-1: terrain opaque now writes TRUE depth (0). The
// old -0.002 (net -0.004 GPU / per-vertex CPU) recessed terrain by a constant NDC
// offset whose reverse-Z WORLD equivalent grows with distance, so distant terrain
// sat far behind true depth and every opaque object on it poked through. Terrain
// at true depth -> objects (true depth) occlude correctly. Co-planar layers below
// keep SMALL relative epsilons off true terrain (overlay wins, water loses, veg
// wins by a hair). LOCKSTEP with shaders/include/terrain_depth_bias.hglsl.
namespace mc2depth {
// Layered contract, reverse-Z / GEQUAL: terrain 0 < VEG < OVERLAY < OBJECT < WATER.
constexpr float TERRAIN_DEPTH_FUDGE      = 0.0f;      // true opaque depth (was -0.002f)
constexpr float VEG_DEPTH_BIAS           =  0.00003f; // > 0 veg wins over ground, < OVERLAY
constexpr float OVERLAY_DEPTH_BIAS       =  0.00005f; // > 0 decals/cement win over terrain & veg
constexpr float OBJECT_DEPTH_BIAS        =  0.0001f;  // > OVERLAY: props/buildings/mechs on top of cement; tiny -> no show-through
constexpr float WATER_DEPTH_BIAS         =  0.00025f; // > 0 water surface wins over deeper lake-bottom terrain (old net +0.00025)
constexpr float WATER_DEPTH_FUDGE_FAST   =  0.00025f; // FAST regime: water surface ahead of true terrain
constexpr float WATER_DEPTH_FUDGE_RASTER =  0.00025f; // RASTER regime: same surface bias
// Back-compat alias: the legacy CPU raster consumer name (quad.cpp wz + WATER_DEPTH_FUDGE).
constexpr float WATER_DEPTH_FUDGE        = WATER_DEPTH_FUDGE_RASTER;                       // 0.00025f

static_assert(
    VEG_DEPTH_BIAS > 0.0f && VEG_DEPTH_BIAS < OVERLAY_DEPTH_BIAS &&
    OVERLAY_DEPTH_BIAS < OBJECT_DEPTH_BIAS &&
    WATER_DEPTH_BIAS > 0.0f && WATER_DEPTH_BIAS < 0.004f,
    "True-depth layered ordering: 0 < VEG < OVERLAY < OBJECT < WATER, |water|<0.004 bound");
}
