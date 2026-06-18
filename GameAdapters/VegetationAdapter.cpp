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

#include "../../GameOS/gameos/diagnostic_trace.h"

#include <vector>
#include <algorithm> // std::max
#include <cstdlib>   // std::getenv, std::atoi
#include <cstdio>    // fprintf, snprintf
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

// Atlas frame palettes per terrain type.
// vegetation_atlas_v2.png: 2 rows x 4 cols = 8 frames.
//   Row 0 (frames 0-3): grass / ground-cover variants
//   Row 1 (frames 4-7): ferns, shrubs, taller plants
// dominant = how many leading frames to pick the clump dominant from.
struct FramePalette { uint8_t f[8]; int dominant; int n; };

[[nodiscard]] FramePalette framePalette(short ttype) noexcept
{
    switch (ttype) {
    case MC_FORESTFLOOR_TYPE:
    case MC_MOSS_TYPE:
        return {{ 4, 5, 6, 7, 0, 1, 2, 3 }, 4, 8};  // ferns dominant, some grass
    case MC_GRASS_TYPE:
    case MC_SLIMY_TYPE:
        return {{ 0, 1, 2, 3, 6, 7, 4, 5 }, 4, 6};  // grass dominant, occasional shrub
    case MC_MUD_TYPE:
    default:
        return {{ 0, 1, 2, 0, 1, 2, 0, 1 }, 3, 3};  // short grass only
    }
}

// Bilinear-interpolated hash on a coarse grid → smooth spatial blobs.
// Returns [0,1].  Used as secondary variation WITHIN terrain-type zones;
// terrain type is still the primary gate.
[[nodiscard]] float macroZone(float wx, float wy, float cellSize = 512.0f) noexcept
{
    const float gx = wx / cellSize;
    const float gy = wy / cellSize;
    const int cx = static_cast<int>(gx >= 0.0f ? gx : gx - 1.0f);
    const int cy = static_cast<int>(gy >= 0.0f ? gy : gy - 1.0f);
    float fx = gx - static_cast<float>(cx);
    float fy = gy - static_cast<float>(cy);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float h00 = hashf(static_cast<float>(cx    ), static_cast<float>(cy    ));
    const float h10 = hashf(static_cast<float>(cx + 1), static_cast<float>(cy    ));
    const float h01 = hashf(static_cast<float>(cx    ), static_cast<float>(cy + 1));
    const float h11 = hashf(static_cast<float>(cx + 1), static_cast<float>(cy + 1));
    return h00*(1-fx)*(1-fy) + h10*fx*(1-fy) + h01*(1-fx)*fy + h11*fx*fy;
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

    // Placement counters — logged + emitted as VEG_HEALTH JSONL at end.
    int diagCellsTotal = 0;
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
            ++diagCellsTotal;

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

            (void)gameMap;

            // Water rejection at grid centre — counted by diagWater.
            // Do NOT remove until placement_health event confirms:
            //   cells_total > 0 AND reject_water < cells_total AND instances > 0
            {
                const float sampleElev = land->getTerrainElevation(samplePos);
                if (sampleElev <= Terrain::waterElevation) { ++diagWater; continue; }
            }

            // macroZone [0,1] — secondary variation WITHIN terrain-type zones.
            // Terrain type is the primary gate: green stays mostly covered,
            // brown/dirt is lighter but NOT zero.
            //   kFull  (green):  bottom 25% rejected -> 75% coverage
            //   kSparse (brown): bottom 60% rejected -> 40% coverage
            const float zone = macroZone(wx, wy);
            const float zoneThresh = (vdc == VegDensity::kFull) ? 0.25f : 0.35f;
            if (zone < zoneThresh) { ++diagNotGreen; continue; }
            const float zoneDepth = (zone - zoneThresh) / (1.0f - zoneThresh);

            // Cards per clump: kFull = 6..density/2 (6..12), kSparse = 1..2.
            const int clumpMax = (vdc == VegDensity::kFull) ? density / 2 : 2;
            const int clumpMin = (vdc == VegDensity::kFull) ? 6 : 1;
            const int count = clumpMin + static_cast<int>(zoneDepth * static_cast<float>(clumpMax - clumpMin));

            const float mapHalf = mapSpan * 0.5f;

            // Jitter clump centre by half grid step — breaks visible 32 WU rows.
            const float jx = (hashf(wx * 0.017f, wy * 0.013f + 1234.0f) - 0.5f) * kGridStep;
            const float jy = (hashf(wy * 0.019f, wx * 0.023f + 5678.0f) - 0.5f) * kGridStep;
            const float clumpCx = wx + jx;
            const float clumpCy = wy + jy;

            for (int i = 0; i < count; ++i) {
                if (static_cast<int>(instances.size()) >= maxInst) break;

                const float fi = static_cast<float>(i);

                const float h1 = hashf(clumpCx + fi * 73.1f,  clumpCy + fi *  7.3f);
                const float h2 = hashf(clumpCy + fi * 83.7f,  clumpCx + fi *  3.1f);
                const float h3 = hashf(clumpCx + fi * 199.1f, clumpCy + fi * 17.3f);
                const float h4 = hashf(clumpCy + fi * 151.3f, clumpCx + fi * 11.7f);
                const float h5 = hashf(clumpCx + fi * 37.9f,  clumpCy + fi * 59.1f);
                const float h6 = hashf(clumpCy + fi * 61.3f,  clumpCx + fi * 43.1f);

                const float instWx = clumpCx + (h1 - 0.5f) * 20.0f;
                const float instWy = clumpCy + (h2 - 0.5f) * 20.0f;

                const Stuff::Vector3D instPos(instWx, instWy, 0.0f);
                const float elevation = land->getTerrainElevation(instPos);
                if (elevation <= Terrain::waterElevation) continue;

                GosVegetation::Instance inst;
                inst.x          = instWx - originX - mapHalf;
                inst.y          = instWy - originY + mapHalf;
                inst.z          = elevation + 0.5f;
                inst.yaw        = h3 * 6.2831f;
                inst.scale      = 5.0f + h4 * 5.0f;
                // Species: dominant frame for this clump (70%), fill from palette (30%).
                const FramePalette pal = framePalette(ttype);
                const float domHash = hashf(clumpCx * 0.11f, clumpCy * 0.09f + 9999.0f);
                const uint8_t domFrame = pal.f[static_cast<int>(domHash * static_cast<float>(pal.dominant)) % pal.dominant];
                const uint32_t frame = (h5 < 0.70f)
                    ? static_cast<uint32_t>(domFrame)
                    : static_cast<uint32_t>(pal.f[static_cast<int>(h5 * static_cast<float>(pal.n)) % pal.n]);
                inst.atlasFrame = frame;
                inst.seed       = h6;

                instances.push_back(inst);
            }
        }
    }

    GosVegetation::uploadInstances(instances.data(),
                                   static_cast<uint32_t>(instances.size()));

    const unsigned instCount = static_cast<unsigned>(instances.size());

    // Always log: needed to diagnose zero-instance cases without re-running.
    fprintf(stderr,
        "[VEG v1] placement_health cells_total=%d oob=%d terrain_type=%d "
        "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f\n",
        diagCellsTotal, diagOOB, diagHard,
        diagWater, diagNotGreen, diagOverlay,
        instCount, maxInst, static_cast<double>(mapSpan));
    fflush(stderr);

    // Structured VEG_HEALTH JSONL placement event (MCP-queryable).
    {
        char dataJson[512];
        snprintf(dataJson, sizeof(dataJson),
            "{"
            "\"event\":\"placement_health\","
            "\"cells_total\":%d,"
            "\"reject_oob\":%d,"
            "\"reject_terrain_type\":%d,"
            "\"reject_water\":%d,"
            "\"reject_density\":%d,"
            "\"reject_overlay\":%d,"
            "\"instances_generated\":%u,"
            "\"max_instances\":%d"
            "}",
            diagCellsTotal,
            diagOOB, diagHard, diagWater, diagNotGreen, diagOverlay,
            instCount, maxInst);
        mc2_diag::writeEvent("VEGETATION", 1, 0, dataJson);
    }

    // Also write to a file so we can read it without console access.
    if (FILE* diagF = fopen("veg_diag.txt", "w")) {
        fprintf(diagF,
            "[VEG v1] placement_health cells_total=%d oob=%d terrain_type=%d "
            "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f\n",
            diagCellsTotal, diagOOB, diagHard,
            diagWater, diagNotGreen, diagOverlay,
            instCount, maxInst, static_cast<double>(mapSpan));
        fclose(diagF);
    }
}

void GameAdapters::Vegetation::flush(const float* terrainLightDir_4f, float time,
                                     float camChunkX, float camChunkY, float camChunkZ)
{
    if (!GosVegetation::isEnabled()) return;

    // Build per-block LOD visibility from terrain chunk metadata and upload to SSBO.
    // The SSBO tells the vertex shader which blocks to cull (0) or draw at LOD1 (1) or LOD0 (2).
    const int chunkSide  = Terrain::s_terrainChunkSide;
    const int blockCount = chunkSide * chunkSide;
    int diagLod0 = 0, diagLod1 = 0, diagCulled = 0;
    if (blockCount > 0 && Terrain::s_blockMeta) {
        static thread_local std::vector<uint32_t> s_visBuffer;
        s_visBuffer.resize(static_cast<size_t>(blockCount));
        for (int i = 0; i < blockCount; ++i) {
            // Never cull to 0u — inFrustum toggles on edge blocks each frame causing
            // single-frame flashes.  GPU clips off-screen verts automatically.
            const int lod = static_cast<int>(Terrain::s_blockMeta[i].lodLevel);
            s_visBuffer[i] = (lod <= 1) ? 2u : 1u;
            if (s_visBuffer[i] == 2u) ++diagLod0;
            else if (s_visBuffer[i] == 1u) ++diagLod1;
            else ++diagCulled;
        }
        GosVegetation::setBlockVisibility(s_visBuffer.data(),
                                          static_cast<uint32_t>(blockCount));
    }

    // Emit first-frame draw_health event (MCP-queryable).
    static int s_flushCallCount = 0;
    if (++s_flushCallCount == 1 && mc2_diag::tagEnabled("VEGETATION")) {
        char dataJson[512];
        snprintf(dataJson, sizeof(dataJson),
            "{"
            "\"event\":\"draw_health\","
            "\"flush_called\":true,"
            "\"instances\":%u,"
            "\"terrain_blocks\":%d,"
            "\"lod0_blocks\":%d,"
            "\"lod1_blocks\":%d,"
            "\"culled_blocks\":%d"
            "}",
            GosVegetation::instanceCount(),
            blockCount, diagLod0, diagLod1, diagCulled);
        mc2_diag::writeEvent("VEGETATION", 1, 0, dataJson);
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