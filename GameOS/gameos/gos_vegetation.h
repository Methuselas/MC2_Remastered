#pragma once
#include <cstdint>

namespace GosVegetation {

    struct Instance {
        float x, y, z;       // MC2 world pos (x=east, y=north, z=elev)
        float yaw;            // rotation about Z (radians)
        float scale;          // uniform scale (typically 0.8..1.4)
        uint32_t atlasFrame;  // 0..3
        float seed;           // per-instance random [0,1] for dither stability
    };
    static_assert(sizeof(Instance) == 28, "GosVegetation::Instance must be 28 bytes");

    void init();
    void shutdown();
    void setAtlasPath(const char* path);
    void uploadInstances(const Instance* instances, uint32_t count);

    // worldToClipGL_4x4: 16 floats row-major (from Stuff::Matrix4D entries)
    // camPos: MC2/MLR space (x=left(-east), y=elev, z=forward(north))
    // lightDir: world-space light direction (x,y,z,w) - terrainLightDir convention
    // time: elapsed seconds
    void flush(const float* worldToClipGL_4x4,
               float camX, float camY, float camZ,
               float lightDirX, float lightDirY, float lightDirZ, float lightDirW,
               float time);

    [[nodiscard]] bool isEnabled();       // checks MC2_VEGETATION_CARDS env var
    [[nodiscard]] uint32_t instanceCount();

} // namespace GosVegetation
