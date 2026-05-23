#pragma once
// F6 T2: shared terrain-admission mode flag consumed by terrain.cpp (slimReduce
// loop) and quad.cpp (TerrainQuad::setupTextures water-corner sites).
// MC2_TERRAIN_ADMISSION_MODERN=1 -> Modern (frustum-plane test).
// Default: Legacy (projectZ). Lambda-static idempotent latch.
namespace mc2_terrain_admission {
inline bool isModern() {
    static const bool s = (getenv("MC2_TERRAIN_ADMISSION_MODERN") != nullptr);
    return s;
}
} // namespace mc2_terrain_admission
