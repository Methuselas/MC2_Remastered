#pragma once
#include <cstdint>

namespace GosVegetation {

    struct Instance {
        float x, y, z;       // terrain-chunk space: x=east_centered, y=north_centered, z=elevation
        float yaw;            // rotation about Z (elevation) axis (radians)
        float scale;          // uniform scale in WU
        uint32_t atlasFrame;  // 0..7
        float seed;           // per-instance random [0,1] for dither stability
        // blockIdx removed — computed per-vertex in shader from world position
    };
    static_assert(sizeof(Instance) == 28, "GosVegetation::Instance must be 28 bytes");

    void init();
    void shutdown();
    void setAtlasPath(const char* path);
    // Replace the atlas texture with a caller-owned GL texture (e.g. built from
    // schema card files).  Must be called after init().  The previous atlas texture
    // is deleted.  texId ownership transfers to GosVegetation; do not delete it.
    // Parameter type is unsigned int to avoid pulling <GL/glew.h> into this header.
    void setAtlasTexId(unsigned int texId);
    void uploadInstances(const Instance* instances, uint32_t count);

    // Per-block visibility: 1=draw, 0=cull.  Derived from Terrain::s_blockMeta[i].lodLevel.
    // Call once per frame before flush().  blockCount = s_terrainChunkSide^2.
    void setBlockVisibility(const uint32_t* visible, uint32_t blockCount);

    // Draws all uploaded instances.
    // lightDir: world-space light direction (x,y,z,w) - terrainLightDir convention.
    // time: elapsed seconds.
    // camChunkX/Y/Z: camera position in terrain-chunk space (for wind LOD fade).
    // mapHalfWU: worldUnitsMapSide * 0.5 (for blockIdx computation in shader).
    // blockSideWU: worldUnitsBlockSide (2560.0f).
    // chunkSide: s_terrainChunkSide (block grid dimension).
    void flush(float lightDirX, float lightDirY, float lightDirZ, float lightDirW,
               float time,
               float camChunkX, float camChunkY, float camChunkZ,
               float mapHalfWU, float blockSideWU, int chunkSide);

    [[nodiscard]] bool isEnabled();       // checks MC2_VEGETATION_CARDS env var
    [[nodiscard]] uint32_t instanceCount();

} // namespace GosVegetation
