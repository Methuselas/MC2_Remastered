// GameAdapters/VegetationAdapter.cpp
//
// Vegetation card placement scan and per-frame flush.
// This is a GameAdapters bridge TU: it may reach both game-side headers
// (mclib/) and engine-side headers (GameOS/gameos/).

#include "VegetationAdapter.h"

// Engine side.
#include "gos_vegetation.h"

// Game side -- terrain geometry + type queries.
#include "../mclib/terrain.h"
// Game side -- MissionMap overlay / water queries.
#include "../mclib/move.h"
// Terrain type enum (TerrainType, MC_*_TYPE values).
#include "../mclib/dmapdata.h"

// Stuff::Matrix4D (entries[16], column-major) + Stuff::Vector3D.
// Full chain required (matrix.hpp alone creates circular include ordering).
#include "Stuff/Stuff.hpp"

#include <vector>
#include <cstdlib>   // std::getenv, std::atoi
#include <cstdio>    // fprintf
#include <cmath>     // sinf, fmodf, fabsf

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Read an integer env var.  Returns defaultVal if not set or empty.
[[nodiscard]] int envInt(const char* name, int defaultVal) noexcept
{
    const char* v = std::getenv(name);
    if (!v || !v[0]) return defaultVal;
    return std::atoi(v);
}

[[nodiscard]] bool envSet(const char* name) noexcept
{
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

// Cheap, deterministic hash in [0, 1).  Inputs are world-grid coordinates
// plus a per-instance discriminator so each card within a cell differs.
[[nodiscard]] float hashf(float a, float b) noexcept
{
    return fabsf(fmodf(sinf(a * 127.1f + b * 311.7f) * 43758.5f, 1.0f));
}

// World-unit spacing between sample points.
constexpr float kGridStep = 64.0f;

// ---------------------------------------------------------------------------
// Placement accept/reject helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool isGreenZone(short terrainType) noexcept
{
    switch (terrainType) {
    case MC_MOSS_TYPE:
    case MC_FORESTFLOOR_TYPE:
    case MC_GRASS_TYPE:
    case MC_SLIMY_TYPE:
    case MC_DIRT_TYPE:   // accepted at potentially reduced density
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isHardReject(short terrainType) noexcept
{
    switch (terrainType) {
    case MC_BLUEWATER_TYPE:
    case MC_GREEN_WATER_TYPE:
    case MC_ASH_TYPE:
    case MC_MOUNTAIN_TYPE:
    case MC_CONCRETE_TYPE:
    case MC_CLIFF_TYPE:
    case MC_CEMENT2_TYPE:
    case MC_CEMENT3_TYPE:
    case MC_CEMENT4_TYPE:
    case MC_CEMENT5_TYPE:
    case MC_CEMENT6_TYPE:
    case MC_CEMENT7_TYPE:
    case MC_CEMENT8_TYPE:
        return true;
    default:
        return false;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Adapter implementation
// ---------------------------------------------------------------------------

void GameAdapters::Vegetation::init()
{
    GosVegetation::init();
}

void GameAdapters::Vegetation::shutdown()
{
    GosVegetation::shutdown();
}

void GameAdapters::Vegetation::missionUnloaded()
{
    // Clear GPU instance list.
    GosVegetation::uploadInstances(nullptr, 0);
}

void GameAdapters::Vegetation::missionLoaded(Terrain* land, MissionMap* gameMap)
{
    if (!land) return;
    if (!GosVegetation::isEnabled()) return;
    GosVegetation::init();  // no-op if already initialized

    const int density   = envInt("MC2_VEGETATION_DENSITY", 4);
    const int maxInst   = envInt("MC2_VEGETATION_MAX", 50000);
    const bool debugLog = envSet("MC2_VEGETATION_DEBUG");

    // Terrain map is square.  mapTopLeft3d is the top-left (max-Y, min-X)
    // corner in world space.  worldUnitsMapSide is the total span.
    // X runs from mapTopLeft3d.x rightward; Y decreases downward (row).
    const float originX = Terrain::mapTopLeft3d.x;
    const float originY = Terrain::mapTopLeft3d.y;   // max world-Y
    const float mapSpan = Terrain::worldUnitsMapSide;

    std::vector<GosVegetation::Instance> instances;
    instances.reserve(static_cast<size_t>(maxInst));

    // Walk the map in world-space steps of kGridStep.
    // wy decreases (southward) because MissionMap row increases as Y decreases.
    for (float wy = originY - kGridStep * 0.5f;
         wy > (originY - mapSpan + kGridStep);
         wy -= kGridStep)
    {
        if (static_cast<int>(instances.size()) >= maxInst) break;

        for (float wx = originX + kGridStep * 0.5f;
             wx < (originX + mapSpan - kGridStep);
             wx += kGridStep)
        {
            if (static_cast<int>(instances.size()) >= maxInst) break;

            // z=0; getTerrainType only uses x/y for cell lookup.
            const Stuff::Vector3D samplePos(wx, wy, 0.0f);
            const short ttype = land->getTerrainType(samplePos);

            if (isHardReject(ttype)) continue;
            if (!isGreenZone(ttype))  continue;

            // MissionMap overlay / water rejection.
            if (gameMap) {
                int cellR = 0, cellC = 0;
                land->worldToCell(samplePos, cellR, cellC);
                const long lR = static_cast<long>(cellR);
                const long lC = static_cast<long>(cellC);
                if (gameMap->getOverlay(lR, lC) != 0)  continue;
                if (gameMap->getDeepWater(lR, lC))      continue;
                if (gameMap->getShallowWater(lR, lC))   continue;
            }

            // Per-cell jitter: constant across all instances within this cell.
            const float h1 = hashf(wx, wy);
            const float h2 = hashf(wy, wx);
            const float jitterX = (h1 - 0.5f) * kGridStep * 0.8f;
            const float jitterY = (h2 - 0.5f) * kGridStep * 0.8f;

            for (int i = 0; i < density; ++i) {
                if (static_cast<int>(instances.size()) >= maxInst) break;

                const float fi = static_cast<float>(i);

                const float h3 = hashf(wx + fi * 73.1f,  wy);
                const float h4 = hashf(wy + fi * 83.7f,  wx);
                const float h5 = hashf(wx + fi * 199.1f, wy + fi * 17.3f);
                const float h6 = hashf(wy + fi * 151.3f, wx + fi * 11.7f);
                const float h7 = hashf(wx + fi * 37.9f,  wy + fi * 59.1f);
                const float h8 = hashf(wy + fi * 61.3f,  wx + fi * 43.1f);

                const float instWx = wx + jitterX + h3 * 20.0f;
                const float instWy = wy + jitterY + h4 * 20.0f;

                const Stuff::Vector3D instPos(instWx, instWy, 0.0f);
                const float elevation = land->getTerrainElevation(instPos);

                GosVegetation::Instance inst;
                inst.x          = instWx;
                inst.y          = instWy;
                inst.z          = elevation;
                inst.yaw        = h5 * 6.2831f;
                inst.scale      = 0.8f + h6 * 0.7f;
                inst.atlasFrame = static_cast<uint32_t>(h7 * 4.0f) & 3u;
                inst.seed       = h8;

                instances.push_back(inst);
            }
        }
    }

    GosVegetation::uploadInstances(instances.data(),
                                   static_cast<uint32_t>(instances.size()));

    if (debugLog) {
        fprintf(stderr,
                "[VegetationAdapter] missionLoaded: %u instances "
                "(density=%d max=%d)\n",
                static_cast<unsigned>(instances.size()), density, maxInst);
        fflush(stderr);
    }
}

void GameAdapters::Vegetation::flush(const Stuff::Matrix4D& worldToClipGL,
                                     const Stuff::Vector3D&  cameraPos,
                                     const float*            terrainLightDir_4f,
                                     float                   time)
{
    if (!GosVegetation::isEnabled()) return;

    // Stuff::Matrix4D::entries is float[16], column-major.
    // GosVegetation::flush expects the same layout (GL_FALSE upload
    // convention matching gos_SetWorldToClipGL).
    GosVegetation::flush(
        worldToClipGL.entries,
        cameraPos.x, cameraPos.y, cameraPos.z,
        terrainLightDir_4f[0], terrainLightDir_4f[1],
        terrainLightDir_4f[2], terrainLightDir_4f[3],
        time);
}