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
#include <algorithm> // std::max
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

// Vegetation density tier for each terrain type.
// kNone   = no vegetation (bare rock, water, paved)
// kSparse = dry/transitional terrain, low density and only near kFull zones
// kFull   = green/lush terrain, full density
enum class VegDensity : int { kNone = 0, kSparse = 1, kFull = 2 };

[[nodiscard]] VegDensity classifyTerrain(short t) noexcept
{
    switch (t) {
    case MC_MUD_TYPE:
    case MC_MOSS_TYPE:
    case MC_FORESTFLOOR_TYPE:
    case MC_GRASS_TYPE:
    case MC_SLIMY_TYPE:
        return VegDensity::kFull;
    case MC_DIRT_TYPE:
    case MC_TUNDRA_TYPE:
        return VegDensity::kSparse;
    default:
        return VegDensity::kNone;  // water, ash, mountain, concrete, cliff, cement
    }
}

// Smooth spatial noise in [0,1] at ~400 WU scale for macro density patches.
[[nodiscard]] float smoothNoise(float wx, float wy) noexcept
{
    return fabsf(fmodf(sinf(wx * 0.0025f + wy * 0.0063f) * 43758.5f, 1.0f));
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

            const VegDensity vdc = classifyTerrain(ttype);
            if (vdc == VegDensity::kNone) { ++diagHard; continue; }

            // MissionMap getOverlay encodes movement costs for every cell (always != 0
            // for natural terrain), so overlay != 0 rejects everything — skip it.
            (void)gameMap;

            // Macro spatial noise [0,1] at ~400 WU scale for density variation.
            const float noise = smoothNoise(wx, wy);

            // Terrain type multiplier: kFull = 1.0, kSparse = 0.25.
            const float terrMult = (vdc == VegDensity::kFull) ? 1.0f : 0.25f;

            // nearForest scan (±2 tiles): boost kSparse cells adjacent to kFull zones.
            float nearBoost = 1.0f;
            if (vdc == VegDensity::kSparse) {
                for (int dr = -2; dr <= 2 && nearBoost < 2.0f; ++dr) {
                    for (int dc = -2; dc <= 2 && nearBoost < 2.0f; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        const long nr = lR + dr, nc = lC + dc;
                        if (nr >= 0 && nc >= 0 && nr < mapSide && nc < mapSide)
                            if (classifyTerrain(static_cast<short>(land->getTerrain(nr, nc))) == VegDensity::kFull)
                                nearBoost = 3.0f;
                    }
                }
            }

            // Instance count: floor = max(1, density * terrMult * noise * nearBoost) / 6.
            // density=25, kFull, noise=0.8 → max(1,20)/6 = 3. kSparse near forest, noise=0.8 → max(1,15)/6 = 2.
            const int count = std::max(1, static_cast<int>(
                static_cast<float>(density) * terrMult * noise * nearBoost)) / 6;
            if (count == 0) { ++diagNotGreen; continue; }

            const float mapHalf = mapSpan * 0.5f;

            // Full-cell scatter: each instance independently placed (no shared jitter center).
            for (int i = 0; i < count; ++i) {
                if (static_cast<int>(instances.size()) >= maxInst) break;

                const float fi = static_cast<float>(i);

                const float h1 = hashf(wx + fi * 73.1f,  wy + fi *  7.3f);
                const float h2 = hashf(wy + fi * 83.7f,  wx + fi *  3.1f);
                const float h3 = hashf(wx + fi * 199.1f, wy + fi * 17.3f);
                const float h4 = hashf(wy + fi * 151.3f, wx + fi * 11.7f);
                const float h5 = hashf(wx + fi * 37.9f,  wy + fi * 59.1f);
                const float h6 = hashf(wy + fi * 61.3f,  wx + fi * 43.1f);

                const float instWx = wx + (h1 - 0.5f) * kGridStep * 0.9f;
                const float instWy = wy + (h2 - 0.5f) * kGridStep * 0.9f;

                const Stuff::Vector3D instPos(instWx, instWy, 0.0f);
                const float elevation = land->getTerrainElevation(instPos);

                GosVegetation::Instance inst;
                inst.x          = instWx - originX - mapHalf;
                inst.y          = instWy - originY + mapHalf;
                inst.z          = elevation + 0.5f;  // lift card base off terrain surface
                inst.yaw        = h3 * 6.2831f;
                inst.scale      = 5.0f + h4 * 5.0f;  // 5-10 WU
                inst.atlasFrame = static_cast<uint32_t>(h5 * 4.0f) & 3u;
                inst.seed       = h6;

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