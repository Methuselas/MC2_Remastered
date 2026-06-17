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
constexpr float kGridStep = 32.0f;

// ---------------------------------------------------------------------------
// Placement accept/reject helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool isGreenZone(short terrainType) noexcept
{
    switch (terrainType) {
    case MC_MUD_TYPE:         // predominant outdoor terrain in many missions
    case MC_MOSS_TYPE:
    case MC_FORESTFLOOR_TYPE:
    case MC_GRASS_TYPE:
    case MC_TUNDRA_TYPE:
    case MC_SLIMY_TYPE:
    case MC_DIRT_TYPE:
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

    const int density   = envInt("MC2_VEGETATION_DENSITY", 25);
    const int maxInst   = envInt("MC2_VEGETATION_MAX", 1500000);
    const bool debugLog = envSet("MC2_VEGETATION_DEBUG");

    // Terrain map is square.  mapTopLeft3d is the top-left (max-Y, min-X)
    // corner in world space.  worldUnitsMapSide is the total span.
    // X runs from mapTopLeft3d.x rightward; Y decreases downward (row).
    const float originX = Terrain::mapTopLeft3d.x;
    const float originY = Terrain::mapTopLeft3d.y;   // max world-Y
    const float mapSpan = Terrain::worldUnitsMapSide;

    std::vector<GosVegetation::Instance> instances;
    instances.reserve(static_cast<size_t>(maxInst));

    // Reject counters — logged at end to diagnose zero-placement.
    int diagOOB = 0, diagHard = 0, diagNotGreen = 0, diagOverlay = 0, diagWater = 0;

    if (gameMap) {
        fprintf(stderr, "[VegetationAdapter] gameMap: width=%d height=%d\n",
                gameMap->width, gameMap->height);
        fflush(stderr);
    }

    // Diagnostic: sample first 20 terrain type values to see what's on this map.
    {
        const float stepX = mapSpan / 20.0f;
        fprintf(stderr, "[VegetationAdapter] terrain-type sample (20 pts): ");
        for (int si = 0; si < 20; ++si) {
            const float sx = originX + stepX * (si + 0.5f);
            const float sy = originY - mapSpan * 0.5f;
            const Stuff::Vector3D sp(sx, sy, 0.0f);
            int tr = 0, tc = 0, cr = 0, cc = 0;
            land->worldToTileCell(sp, tr, tc, cr, cc);
            const long ms = static_cast<long>(Terrain::realVerticesMapSide);
            if (tr >= 0 && tc >= 0 && tr < ms && tc < ms)
                fprintf(stderr, "%d ", static_cast<int>(land->getTerrain(tr, tc)));
            else
                fprintf(stderr, "OOB ");
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }

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

            // worldToTileCell: tile indices for Terrain::getTerrain (vertex grid).
            // worldToCell: absolute cell indices for MissionMap (cell grid = 3x finer).
            // Both return row/col in the same spatial convention; use the right
            // index type for each API.
            const Stuff::Vector3D samplePos(wx, wy, 0.0f);
            int tileR = 0, tileC = 0, subR = 0, subC = 0;
            land->worldToTileCell(samplePos, tileR, tileC, subR, subC);
            const long mapSide = static_cast<long>(Terrain::realVerticesMapSide);
            if (tileR < 0 || tileC < 0 || tileR >= mapSide || tileC >= mapSide) { ++diagOOB; continue; }
            const long lR = static_cast<long>(tileR);
            const long lC = static_cast<long>(tileC);

            const short ttype = static_cast<short>(land->getTerrain(lR, lC));

            if (isHardReject(ttype)) { ++diagHard; continue; }
            if (!isGreenZone(ttype)) { ++diagNotGreen; continue; }

            // Water/overlay rejection via terrain type is sufficient:
            // MC_BLUEWATER_TYPE and MC_GREEN_WATER_TYPE are in isHardReject above.
            // MissionMap getOverlay encodes movement costs for every cell (always != 0
            // for natural terrain in mc2), so checking overlay != 0 rejects everything.
            // getDeepWater/getShallowWater duplicates what the terrain-type check already
            // handles — skipped for now.
            (void)gameMap;

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

                // Convert to terrain-chunk centered space (matching terrain_lod_chunk.vert):
                //   x = east_centered  = wx - originX - mapHalf
                //   y = north_centered = wy - originY + mapHalf  (originY = max north)
                const float mapHalf = mapSpan * 0.5f;

                GosVegetation::Instance inst;
                inst.x          = instWx - originX - mapHalf;
                inst.y          = instWy - originY + mapHalf;
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

    // Always log: needed to diagnose zero-instance cases without re-running.
    fprintf(stderr,
            "[VegetationAdapter] missionLoaded: placed=%u density=%d kGridStep=%.0f "
            "maxInst=%d mapSpan=%.0f\n"
            "  rejects: OOB=%d hard=%d notGreen=%d overlay=%d water=%d\n",
            static_cast<unsigned>(instances.size()),
            density, static_cast<double>(kGridStep),
            maxInst, static_cast<double>(mapSpan),
            diagOOB, diagHard, diagNotGreen, diagOverlay, diagWater);
    fflush(stderr);
}

void GameAdapters::Vegetation::flush(const float* terrainLightDir_4f, float time,
                                     float camChunkX, float camChunkY, float camChunkZ)
{
    if (!GosVegetation::isEnabled()) return;

    // Build per-block LOD visibility from terrain chunk metadata and upload to SSBO.
    // The SSBO tells the vertex shader which blocks to cull (0) or draw at LOD1 (1) or LOD0 (2).
    const int chunkSide  = Terrain::s_terrainChunkSide;
    const int blockCount = chunkSide * chunkSide;
    if (blockCount > 0 && Terrain::s_blockMeta) {
        static thread_local std::vector<uint32_t> s_visBuffer;
        s_visBuffer.resize(static_cast<size_t>(blockCount));
        for (int i = 0; i < blockCount; ++i) {
            const int lod = Terrain::s_blockMeta[i].lodLevel;
            s_visBuffer[i] = (lod == 0) ? 2u : (lod == 1) ? 1u : 0u;
        }
        GosVegetation::setBlockVisibility(s_visBuffer.data(),
                                          static_cast<uint32_t>(blockCount));
    }

    GosVegetation::flush(
        terrainLightDir_4f[0], terrainLightDir_4f[1],
        terrainLightDir_4f[2], terrainLightDir_4f[3],
        time,
        camChunkX, camChunkY, camChunkZ,
        Terrain::worldUnitsMapSide * 0.5f,
        Terrain::worldUnitsBlockSide,
        chunkSide);
}