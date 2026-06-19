// GameAdapters/VegetationAdapter.cpp
//
// Vegetation card placement scan and per-frame flush.
// This is a GameAdapters bridge TU: it may reach both game-side headers
// (mclib/) and engine-side headers (GameOS/gameos/).

#include "VegetationAdapter.h"

// Engine side.
#include "gos_vegetation.h"
#include "utils/Image.h"   // Image + getBytesPerPixel (inline in Image.h)
#include <GL/glew.h>        // GLuint, glGenTextures, etc. (THIRDPARTY_INCLUDE_DIRS)

// Game side -- terrain geometry + type queries.
#include "../mclib/terrain.h"
// Game side -- terrain type codes (TerrainType enum).
#include "../mclib/dmapdata.h"
// Game side -- MissionMap overlay / water queries.
#include "../mclib/move.h"

// Stuff::Matrix4D (entries[16], column-major) + Stuff::Vector3D.
// Full chain required (matrix.hpp alone creates circular include ordering).
#include "Stuff/Stuff.hpp"

#include "../../GameOS/gameos/diagnostic_trace.h"

#include <vector>
#include <algorithm> // std::max
#include <cstdlib>   // std::getenv, std::atoi
#include <cstdio>    // fprintf, snprintf, fopen, fclose, fread
#include <cstring>   // strstr, strlen, memcpy
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

// Read a float env var.  Returns defaultVal if not set or empty.
[[nodiscard]] float envFloat(const char* name, float defaultVal) noexcept
{
    const char* v = std::getenv(name);
    if (!v || !v[0]) return defaultVal;
    return static_cast<float>(std::atof(v));
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
// Default 14.0 WU.  Override with MC2_VEG_GRID_SPACING (e.g. 2.0 for denser grass).
// Clamped to [1.0, 256.0] to prevent runaway instance counts.
[[nodiscard]] static float getGridStep() noexcept
{
    static float cached = -1.0f;
    if (cached < 0.0f) {
        float v = envFloat("MC2_VEG_GRID_SPACING", 14.0f);
        if (v < 1.0f)   v = 1.0f;
        if (v > 256.0f) v = 256.0f;
        cached = v;
    }
    return cached;
}

// ---------------------------------------------------------------------------
// Placement accept/reject helpers
// ---------------------------------------------------------------------------

// Vegetation density tier from terrain material type (getTerrain codes).
// kHard   = absolute no-vegetation: water, concrete, cement, cliff, none
// kNone   = hard exclude (kept for 15% fringe logic at borders)
// kSparse = transitional surfaces: dirt, ash, mountain, slimy
// kFull   = vegetated surfaces: grass, moss, forest floor, mud, tundra
enum class VegDensity : int { kHard = -1, kNone = 0, kSparse = 1, kFull = 2 };

// ---------------------------------------------------------------------------
// Vegetation schema — struct and statics (defined here so framePalette can use them)
// ---------------------------------------------------------------------------

// Maximum cards per schema list (atlas is 4×2 = 8 frames max).
static constexpr int kMaxSchemaCards = 8;
// Maximum card name length (without extension).
static constexpr int kMaxCardNameLen = 64;

struct VegSchema {
    char name[64];
    char cards_full[kMaxSchemaCards][kMaxCardNameLen];
    int  cards_full_count;
    char cards_sparse[kMaxSchemaCards][kMaxCardNameLen];
    int  cards_sparse_count;
    bool loaded;
};

// Active schema (zeroed = no schema loaded).
static VegSchema s_activeSchema = {};
static bool s_schemaActive = false;

// Atlas frame palettes per vegetation density tier.
// vegetation_atlas_v2.png: 2 rows x 4 cols = 8 frames.
//   Row 0 (frames 0-3): grass / ground-cover variants
//   Row 1 (frames 4-7): ferns, shrubs, taller plants
// dominant = how many leading frames to pick the clump dominant from.
//
// When a schema is active, cards_full maps to kFull and cards_sparse to kSparse.
// NOTE: frames 4-7 (ferns/shrubs) are excluded from the legacy palette — grass only.
struct FramePalette { uint8_t f[8]; int dominant; int n; };

[[nodiscard]] FramePalette framePalette(VegDensity vdc) noexcept
{
    if (s_schemaActive) {
        // Schema-driven palette.  cards_full indices = 0..full_count-1,
        // cards_sparse indices = 0..sparse_count-1 (subset of full, same frames).
        FramePalette pal{};
        const int nFull   = std::min(s_activeSchema.cards_full_count,   8);
        const int nSparse = std::min(s_activeSchema.cards_sparse_count, 8);

        if (vdc == VegDensity::kFull && nFull > 0) {
            for (int i = 0; i < nFull; ++i) pal.f[i] = static_cast<uint8_t>(i);
            pal.dominant = nFull;
            pal.n        = nFull;
        } else if (nSparse > 0) {
            for (int i = 0; i < nSparse; ++i) pal.f[i] = static_cast<uint8_t>(i);
            pal.dominant = nSparse;
            pal.n        = nSparse;
        } else {
            // Fallback: use frame 0 only.
            pal.f[0] = 0; pal.dominant = 1; pal.n = 1;
        }
        return pal;
    }

    // Legacy hard-coded palette (no schema).
    if (vdc == VegDensity::kFull)
        return {{ 0, 1, 2, 3, 0, 1, 2, 3 }, 4, 4};  // grass only, all 4 variants
    // kSparse: short dry grass only
    return {{ 0, 1, 2, 0, 1, 2, 0, 1 }, 3, 3};
}

// Classify a terrain cell by its actual material type code from getTerrain().
// This is the authoritative classification — it uses what the engine actually
// assigned to each tile, not what color the colormap happens to render.
//
// TerrainType codes (dmapdata.h):
//   0  MC_BLUEWATER_TYPE   → kHard (water)
//   1  MC_GREEN_WATER_TYPE → kHard (water)
//   2  MC_MUD_TYPE         → kFull (moist ground, vegetation OK)
//   3  MC_MOSS_TYPE        → kFull
//   4  MC_DIRT_TYPE        → kSparse
//   5  MC_ASH_TYPE         → kSparse (barren but not paved)
//   6  MC_MOUNTAIN_TYPE    → kSparse (rocky slopes, sparse)
//   7  MC_TUNDRA_TYPE      → kFull (tundra grass)
//   8  MC_FORESTFLOOR_TYPE → kFull
//   9  MC_GRASS_TYPE       → kFull
//  10  MC_CONCRETE_TYPE    → kHard (paved surface, no vegetation)
//  11  MC_CLIFF_TYPE       → kHard (sheer rock face)
//  12  MC_SLIMY_TYPE       → kSparse (damp, sparse growth OK)
//  13  MC_CEMENT2_TYPE     → kHard
//  14  MC_CEMENT3_TYPE     → kHard
//  15  MC_CEMENT4_TYPE     → kHard
//  16  MC_CEMENT5_TYPE     → kHard
//  17  MC_CEMENT6_TYPE     → kHard
//  18  MC_CEMENT7_TYPE     → kHard
//  19  MC_CEMENT8_TYPE     → kHard
//  20  MC_NONE_TYPE        → kHard (unassigned)
[[nodiscard]] VegDensity classifyTerrainType(int terrainCode) noexcept {
    switch (terrainCode) {
        // Water — handled by elevation gate too, belt-and-suspenders.
        case MC_BLUEWATER_TYPE:
        case MC_GREEN_WATER_TYPE:
        // Paved / engineered surfaces — absolutely no vegetation.
        case MC_CONCRETE_TYPE:
        case MC_CLIFF_TYPE:
        case MC_CEMENT2_TYPE:
        case MC_CEMENT3_TYPE:
        case MC_CEMENT4_TYPE:
        case MC_CEMENT5_TYPE:
        case MC_CEMENT6_TYPE:
        case MC_CEMENT7_TYPE:
        case MC_CEMENT8_TYPE:
        case MC_NONE_TYPE:
            return VegDensity::kHard;

        // Transitional / sparse ground.
        case MC_DIRT_TYPE:
        case MC_ASH_TYPE:
        case MC_MOUNTAIN_TYPE:
        case MC_SLIMY_TYPE:
            return VegDensity::kSparse;

        // Vegetated surfaces.
        case MC_MUD_TYPE:
        case MC_MOSS_TYPE:
        case MC_TUNDRA_TYPE:
        case MC_FORESTFLOOR_TYPE:
        case MC_GRASS_TYPE:
        default:
            // Default kFull for any unknown code (future terrain types should be green).
            return VegDensity::kFull;
    }
}

// ---------------------------------------------------------------------------
// Vegetation schema — JSON helpers and atlas builder
// ---------------------------------------------------------------------------

// Minimal hand-rolled JSON string-array extractor.
// Finds key "keyName": ["val1", "val2", ...] in flat JSON.
// Returns count of values stored (up to maxVals), 0 on failure.
static int jsonGetStringArray(const char* json, const char* keyName,
                              char (*out)[kMaxCardNameLen], int maxVals) noexcept
{
    // Build needle: "keyName"
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", keyName);
    const char* p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    // Skip whitespace and colon
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '[') return 0;
    p++;  // skip '['

    int count = 0;
    while (*p && *p != ']' && count < maxVals) {
        // Skip whitespace and commas
        while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (*p == ']' || *p == '\0') break;
        if (*p != '"') { p++; continue; }
        p++;  // skip opening quote
        const char* end = strchr(p, '"');
        if (!end) break;
        int len = static_cast<int>(end - p);
        if (len >= kMaxCardNameLen) len = kMaxCardNameLen - 1;
        memcpy(out[count], p, static_cast<size_t>(len));
        out[count][len] = '\0';
        count++;
        p = end + 1;
    }
    return count;
}

// Minimal JSON string field extractor (key: "value").
static bool jsonGetString(const char* json, const char* key,
                          char* out, int outSz) noexcept
{
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    const char* end = strchr(p, '"');
    if (!end) return false;
    int len = static_cast<int>(end - p);
    if (len >= outSz) len = outSz - 1;
    memcpy(out, p, static_cast<size_t>(len));
    out[len] = '\0';
    return true;
}

// Load and parse a schema JSON file.  Returns false on failure.
[[nodiscard]] static bool loadVegSchema(const char* schemaPath, VegSchema* out) noexcept
{
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(schemaPath, "rb");
    if (!f) {
        fprintf(stderr, "[VEG schema] event=load_failed path=%s reason=file_open\n", schemaPath);
        fflush(stderr);
        return false;
    }
    char buf[4096];
    const size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';

    jsonGetString(buf, "name", out->name, sizeof(out->name));
    out->cards_full_count   = jsonGetStringArray(buf, "cards_full",   out->cards_full,   kMaxSchemaCards);
    out->cards_sparse_count = jsonGetStringArray(buf, "cards_sparse", out->cards_sparse, kMaxSchemaCards);
    out->loaded = (out->cards_full_count > 0);

    fprintf(stderr,
        "[VEG schema] event=loaded name=%s full=%d sparse=%d path=%s\n",
        out->name, out->cards_full_count, out->cards_sparse_count, schemaPath);
    fflush(stderr);
    return out->loaded;
}

// Build a GL_TEXTURE_2D atlas (4 cols × 2 rows, each card 512×512, RGBA)
// from a list of card base names.  Packs cards_full into the first rows,
// cards_sparse continues (they may overlap at lower indices — framePalette
// limits which indices are used per density tier).
//
// Layout: cards[0..N-1] fill frames 0..N-1 left-to-right, top-to-bottom.
// Atlas is always 2048×1024 (matching the legacy vegetation_atlas_v2.png).
// Returns 0 on failure (all frames black = missing card is transparent).
[[nodiscard]] static GLuint buildSchemaAtlas(
    const VegSchema& schema, const char* cardDir) noexcept
{
    constexpr int kAtlasCols = 4;
    constexpr int kAtlasRows = 2;
    constexpr int kCardW = 512;
    constexpr int kCardH = 512;
    constexpr int kAtlasW = kAtlasCols * kCardW;  // 2048
    constexpr int kAtlasH = kAtlasRows * kCardH;  // 1024
    constexpr int kBpp    = 4;  // RGBA

    // Allocate atlas pixel buffer (zero = transparent black).
    const size_t atlasBytes = static_cast<size_t>(kAtlasW) * kAtlasH * kBpp;
    std::vector<unsigned char> atlasBuf(atlasBytes, 0);

    // Collect all card names to blit: full first, then any sparse-only cards.
    // cards_full always provides the primary set; cards_sparse is a subset.
    // We pack full cards into frames 0..full-1, total up to 8 frames.
    const int nCards = std::min(schema.cards_full_count, kAtlasCols * kAtlasRows);

    char cardPath[512];
    for (int i = 0; i < nCards; ++i) {
        // Try .ktx2 first (TODO: cook pipeline), then .png fallback.
        // Currently only .png is produced by extract_veg_cards.py.
        snprintf(cardPath, sizeof(cardPath), "%s/%s.ktx2", cardDir, schema.cards_full[i]);
        bool usePng = true;
        // KTX2 loading not implemented yet — always use PNG.
        (void)usePng;

        snprintf(cardPath, sizeof(cardPath), "%s/%s.png", cardDir, schema.cards_full[i]);

        Image img;
        if (!img.loadFromFile(cardPath)) {
            fprintf(stderr,
                "[VEG schema] event=card_missing frame=%d path=%s (frame will be black)\n",
                i, cardPath);
            fflush(stderr);
            continue;
        }

        const int cw = img.getWidth();
        const int ch = img.getHeight();
        const int bpp = getBytesPerPixel(img.getFormat());
        const unsigned char* src = img.getPixels();

        // Destination tile in atlas.
        const int dstCol = i % kAtlasCols;
        const int dstRow = i / kAtlasCols;
        const int dstX = dstCol * kCardW;
        const int dstY = dstRow * kCardH;

        // Blit card into atlas (clamp if card size differs from kCardW/kCardH).
        const int blitW = std::min(cw, kCardW);
        const int blitH = std::min(ch, kCardH);

        for (int row = 0; row < blitH; ++row) {
            for (int col = 0; col < blitW; ++col) {
                const int srcIdx = (row * cw + col) * bpp;
                const int dstIdx = ((dstY + row) * kAtlasW + (dstX + col)) * kBpp;
                atlasBuf[static_cast<size_t>(dstIdx) + 0] = src[srcIdx + 0];  // R
                atlasBuf[static_cast<size_t>(dstIdx) + 1] = (bpp > 1) ? src[srcIdx + 1] : src[srcIdx]; // G
                atlasBuf[static_cast<size_t>(dstIdx) + 2] = (bpp > 2) ? src[srcIdx + 2] : src[srcIdx]; // B
                atlasBuf[static_cast<size_t>(dstIdx) + 3] = (bpp > 3) ? src[srcIdx + 3] : 0xFF;        // A
            }
        }

        fprintf(stderr,
            "[VEG schema] event=card_blitted frame=%d name=%s size=%dx%d bpp=%d\n",
            i, schema.cards_full[i], cw, ch, bpp);
        fflush(stderr);
    }

    // Upload to GL.
    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasW, kAtlasH,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, atlasBuf.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    fprintf(stderr,
        "[VEG schema] event=atlas_built cards=%d size=%dx%d texId=%u\n",
        nCards, kAtlasW, kAtlasH, texId);
    fflush(stderr);
    return texId;
}

// ---------------------------------------------------------------------------
// Density tier helpers
// ---------------------------------------------------------------------------

// MC2_VEGETATION_DENSITY: 0=off, 1=low, 2=med, 3=high (default 3).
// Returns the density value (0-3).  Cached.
[[nodiscard]] static int vegDensityTier() noexcept
{
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_VEGETATION_DENSITY");
        if (!v || !v[0]) {
            // Legacy: MC2_VEGETATION_DENSITY not set.
            // If MC2_VEGETATION_CARDS is set, default to density 3 (high).
            cached = 3;
        } else {
            cached = std::atoi(v);
            if (cached < 0) cached = 0;
            if (cached > 3) cached = 3;
        }
    }
    return cached;
}

// Clump acceptance probability for each density tier.
// Tier 3 = 1.0 (100%), Tier 2 = 0.60, Tier 1 = 0.25, Tier 0 = 0.0 (off).
static constexpr float kDensityProb[4] = { 0.0f, 0.25f, 0.60f, 1.0f };

// ---------------------------------------------------------------------------

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

    // Density tier gate.  Density=0 means off even if MC2_VEGETATION_CARDS=1.
    const int densityTier = vegDensityTier();
    if (densityTier == 0) {
        fprintf(stderr, "[VEG] event=density_off MC2_VEGETATION_DENSITY=0 skipping placement\n");
        fflush(stderr);
        GosVegetation::uploadInstances(nullptr, 0);
        return;
    }
    const float densityProb = kDensityProb[densityTier];

    GosVegetation::init();  // no-op if already initialized

    // Schema loading.  MC2_VEGETATION_SCHEMA selects the JSON schema
    // (default: "grasslands").  Schema files live in data/vegetation/schemas/.
    // Card images live in data/vegetation/cards/.
    {
        const char* schemaName = std::getenv("MC2_VEGETATION_SCHEMA");
        if (!schemaName || !schemaName[0]) schemaName = "grasslands";

        char schemaPath[512];
        snprintf(schemaPath, sizeof(schemaPath),
                 "data/vegetation/schemas/%s.json", schemaName);

        s_schemaActive = false;
        if (loadVegSchema(schemaPath, &s_activeSchema)) {
            GLuint atlasId = buildSchemaAtlas(s_activeSchema, "data/vegetation/cards");
            if (atlasId) {
                GosVegetation::setAtlasTexId(atlasId);
                s_schemaActive = true;
                fprintf(stderr,
                    "[VEG] event=schema_active name=%s density_tier=%d prob=%.2f\n",
                    s_activeSchema.name, densityTier, static_cast<double>(densityProb));
            } else {
                fprintf(stderr,
                    "[VEG] event=schema_atlas_failed name=%s falling_back_to_legacy_atlas\n",
                    s_activeSchema.name);
            }
        } else {
            fprintf(stderr,
                "[VEG] event=schema_not_found path=%s using_legacy_atlas\n", schemaPath);
        }
        fflush(stderr);
    }

    // MC2_VEGETATION_CLUMP_MAX: max cards per clump (internal tuning knob, default 25).
    // Distinct from MC2_VEGETATION_DENSITY which is the 0-3 tier selector.
    const int density = envInt("MC2_VEGETATION_CLUMP_MAX", 25);
    const int maxInst = envInt("MC2_VEGETATION_MAX", 5000000);

    // Terrain map is square.  mapTopLeft3d is the top-left (max-Y, min-X)
    // corner in world space.  worldUnitsMapSide is the total span.
    // X runs from mapTopLeft3d.x rightward; Y decreases downward (row).
    const float originX = Terrain::mapTopLeft3d.x;
    const float originY = Terrain::mapTopLeft3d.y;   // max world-Y
    const float mapSpan = Terrain::worldUnitsMapSide;

    std::vector<GosVegetation::Instance> instances;
    instances.reserve(static_cast<size_t>(maxInst));

    // Classification uses getTerrain() tile type codes — no pre-built map needed.
    // This is always available as long as mapData is initialized (which it is at
    // missionLoaded time). No fallback required.

    // Placement counters — logged + emitted as VEG_HEALTH JSONL at end.
    int diagCellsTotal = 0;
    int diagOOB = 0, diagHard = 0, diagNotGreen = 0, diagOverlay = 0, diagWater = 0;
    // Per-instance rejection counters (inside the clump loop).
    int instTriedTotal = 0, instRejWater = 0, instRejHard = 0, instRejNoneFringe = 0,
        instRejOverlay = 0, instRejSlope = 0;

    // World-unit spacing between grid sample points.
    // Reads MC2_VEG_GRID_SPACING env var (default 14.0 WU, clamped to [1.0, 256.0]).
    // Smaller = denser placement; maxInst cap prevents runaway counts.
    const float gridStep = getGridStep();

    // MC2_VEG_CARD_SIZE: base card height in WU (default 1.0 WU).
    // Each instance adds random variation: base + h4 * (base * 0.5) so cards vary
    // from base to 1.5× base.  For grasslands_dense set to 0.8 WU.
    const float cardSizeBase = envFloat("MC2_VEG_CARD_SIZE", 1.0f);

    // Walk the map in world-space steps of gridStep.
    // wy decreases (southward) because MissionMap row increases as Y decreases.
    for (float wy = originY - gridStep * 0.5f;
         wy > (originY - mapSpan + gridStep);
         wy -= gridStep)
    {
        if (static_cast<int>(instances.size()) >= maxInst) break;

        for (float wx = originX + gridStep * 0.5f;
             wx < (originX + mapSpan - gridStep);
             wx += gridStep)
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

            // Classify by actual terrain material type (getTerrain tile code).
            // Cement/concrete/water/cliff → kHard (rejected).
            // Dirt/ash/mountain/slimy → kSparse. Grass/moss/mud/tundra/forest → kFull.
            const int terrainCode = land->getTerrain(static_cast<long>(tileR),
                                                     static_cast<long>(tileC));
            const VegDensity vdc = classifyTerrainType(terrainCode);
            if (vdc == VegDensity::kHard) { ++diagHard; continue; }

            (void)gameMap;

            // Water rejection at grid centre — counted by diagWater.
            // Do NOT remove until placement_health event confirms:
            //   cells_total > 0 AND reject_water < cells_total AND instances > 0
            {
                const float sampleElev = land->getTerrainElevation(samplePos);
                if (sampleElev <= Terrain::waterElevation) { ++diagWater; continue; }
            }

            // Density tier rejection: probabilistic per-clump gate.
            // At tier 3 (prob=1.0) all clumps pass; lower tiers thin out.
            // Uses a position hash for stability (same result every call for same position).
            if (densityProb < 1.0f) {
                const float h = hashf(wx * 0.003f, wy * 0.005f + 77777.7f);
                if (h > densityProb) { ++diagNotGreen; continue; }
            }

            // macroZone [0,1] — secondary variation WITHIN terrain-type zones.
            // Large blob (512 WU) gives the overall patchy coverage.
            // Fine blob (80 WU) perturbs the threshold to roughen straight edges.
            const float zone      = macroZone(wx, wy);
            const float zoneEdge  = macroZone(wx, wy, 80.0f);  // fine-scale edge roughness
            // kFull:   base threshold 0.20, perturbed ±0.08 by fine blob → 0.12..0.28
            // kSparse: base threshold 0.10, perturbed ±0.06 by fine blob → 0.04..0.16
            const float zoneThresh = (vdc == VegDensity::kFull)
                                   ? 0.20f + (zoneEdge - 0.5f) * 0.16f
                                   : 0.10f + (zoneEdge - 0.5f) * 0.12f;
            if (zone < zoneThresh) { ++diagNotGreen; continue; }
            const float zoneDepth = (zone - zoneThresh) / (1.0f - zoneThresh);

            // Cards per clump: kFull = 6..density/2 (6..12), kSparse = 2..5.
            const int clumpMax = (vdc == VegDensity::kFull) ? density / 2 : 5;
            const int clumpMin = (vdc == VegDensity::kFull) ? 6 : 2;
            const int count = clumpMin + static_cast<int>(zoneDepth * static_cast<float>(clumpMax - clumpMin));

            const float mapHalf = mapSpan * 0.5f;

            // Jitter clump centre by half grid step — breaks visible grid rows.
            const float jx = (hashf(wx * 0.017f, wy * 0.013f + 1234.0f) - 0.5f) * gridStep;
            const float jy = (hashf(wy * 0.019f, wx * 0.023f + 5678.0f) - 0.5f) * gridStep;
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
                const float h7 = hashf(clumpCx + fi * 113.7f, clumpCy + fi * 89.3f);

                const float instWx = clumpCx + (h1 - 0.5f) * 20.0f;
                const float instWy = clumpCy + (h2 - 0.5f) * 20.0f;

                ++instTriedTotal;
                const Stuff::Vector3D instPos(instWx, instWy, 0.0f);
                const float elevation = land->getTerrainElevation(instPos);
                if (elevation <= Terrain::waterElevation) { ++instRejWater; continue; }

                // Per-instance terrain type classification.
                // kHard (cement/concrete/water/cliff): always reject — no fringe.
                // kNone is not returned by classifyTerrainType (kHard covers hard-excludes).
                {
                    int iInstR = 0, iInstC = 0, iInstSR = 0, iInstSC = 0;
                    land->worldToTileCell(instPos, iInstR, iInstC, iInstSR, iInstSC);
                    if (iInstR >= 0 && iInstC >= 0 &&
                        iInstR < static_cast<int>(mapSide) && iInstC < static_cast<int>(mapSide)) {
                        const int instCode = land->getTerrain(static_cast<long>(iInstR),
                                                              static_cast<long>(iInstC));
                        const VegDensity instVdc = classifyTerrainType(instCode);
                        if (instVdc == VegDensity::kHard) { ++instRejHard; continue; }
                    }
                }
                // Reject paved roads and bridges via overlay code.
                // ROUGH (detail normal) and DIRT_ROAD are allowed; paved/bridges are not.
                {
                    int iTR = 0, iTC = 0, iSR = 0, iSC = 0;
                    land->worldToTileCell(instPos, iTR, iTC, iSR, iSC);
                    if (iTR >= 0 && iTC >= 0 &&
                        iTR < static_cast<int>(mapSide) && iTC < static_cast<int>(mapSide)) {
                        Overlays ovType = INVALID_OVERLAY;
                        DWORD ovOffset = 0;
                        land->getOverlay(static_cast<long>(iTR), static_cast<long>(iTC), ovType, ovOffset);
                        if (ovType != INVALID_OVERLAY && ovType != ROUGH && ovType != DIRT_ROAD)
                            { ++instRejOverlay; continue; }
                    }
                }

                // Slope gate: getTerrainAngle() returns DEGREES.
                // Hard limit 60°: cliff faces above this never get vegetation.
                // Soft fade zone 45°..60°: reject with linearly increasing
                // probability so the edge is organic rather than a sharp line.
                // Position-based hash (deterministic, no rand()) for the fade.
                {
                    Stuff::Vector3D slopeNormal;
                    const float angle = land->getTerrainAngle(instPos, &slopeNormal);
                    constexpr float kSlopeHard  = 60.0f;   // hard reject above this
                    constexpr float kSlopeSoft  = 45.0f;   // fade begins here
                    if (angle >= kSlopeHard)
                        { ++instRejSlope; continue; }
                    if (angle > kSlopeSoft) {
                        // t in [0,1]: 0 at soft start, 1 at hard limit
                        const float t = (angle - kSlopeSoft) / (kSlopeHard - kSlopeSoft);
                        const float h8 = hashf(instWx * 97.3f, instWy * 137.9f);
                        if (h8 < t)
                            { ++instRejSlope; continue; }
                    }
                }

                GosVegetation::Instance inst;
                inst.x          = instWx - originX - mapHalf;
                inst.y          = instWy - originY + mapHalf;
                inst.z          = elevation + 0.5f;
                inst.yaw        = h3 * 6.2831f;
                // Scale: cardSizeBase to 1.5× cardSizeBase with random variation.
                // Default (MC2_VEG_CARD_SIZE=1.0): 1.0–1.5 WU.
                // Dense grass (MC2_VEG_CARD_SIZE=0.8): 0.8–1.2 WU.
                inst.scale      = cardSizeBase + h4 * (cardSizeBase * 0.5f);
                // Species: dominant frame for this clump (70%), fill from palette (30%).
                const FramePalette pal = framePalette(vdc);
                const float domHash = hashf(clumpCx * 0.11f, clumpCy * 0.09f + 9999.0f);
                const uint8_t domFrame = pal.f[static_cast<int>(domHash * static_cast<float>(pal.dominant)) % pal.dominant];
                const uint32_t frame = (h5 < 0.70f)
                    ? static_cast<uint32_t>(domFrame)
                    : static_cast<uint32_t>(pal.f[static_cast<int>(h5 * static_cast<float>(pal.n)) % pal.n]);
                // Card role (bits 4-5): 0=vertical 40%, 1=tilted 25%, 2=top 15%, 3=ground 20%.
                const uint32_t cardRole = (h7 < 0.40f) ? 0u : (h7 < 0.65f) ? 1u : (h7 < 0.80f) ? 2u : 3u;
                inst.atlasFrame = (cardRole << 4u) | (frame & 0x0Fu);
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
        "[VEG v3] placement_health cells_total=%d oob=%d terrain_type_reject=%d "
        "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f waterElev=%.2f "
        "density_tier=%d schema=%s\n",
        diagCellsTotal, diagOOB, diagHard,
        diagWater, diagNotGreen, diagOverlay,
        instCount, maxInst, static_cast<double>(mapSpan),
        static_cast<double>(Terrain::waterElevation),
        densityTier, s_schemaActive ? s_activeSchema.name : "legacy");
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
            "\"max_instances\":%d,"
            "\"classifier\":\"terrain_type\""
            "}",
            diagCellsTotal,
            diagOOB, diagHard, diagWater, diagNotGreen, diagOverlay,
            instCount, maxInst);
        mc2_diag::writeEvent("VEGETATION", 1, 0, dataJson);
    }

    // Sample elevation range to understand water/terrain relationship.
    float elevMin = 1e30f, elevMax = -1e30f;
    {
        const float stepX = mapSpan / 40.0f;
        const float stepY = mapSpan / 40.0f;
        for (int si = 0; si < 40; ++si) {
            for (int sj = 0; sj < 40; ++sj) {
                const float sx = originX + stepX * (si + 0.5f);
                const float sy = originY - stepY * (sj + 0.5f);
                const Stuff::Vector3D sp(sx, sy, 0.0f);
                const float e = land->getTerrainElevation(sp);
                if (e < elevMin) elevMin = e;
                if (e > elevMax) elevMax = e;
            }
        }
    }

    // Also write to a file so we can read it without console access.
    if (FILE* diagF = fopen("veg_diag.txt", "w")) {
        fprintf(diagF,
            "[VEG v3] placement_health cells_total=%d oob=%d terrain_type_reject=%d "
            "water=%d density=%d overlay=%d instances=%u max=%d mapSpan=%.0f "
            "waterElev=%.2f elevRange=%.2f..%.2f\n",
            diagCellsTotal, diagOOB, diagHard,
            diagWater, diagNotGreen, diagOverlay,
            instCount, maxInst, static_cast<double>(mapSpan),
            static_cast<double>(Terrain::waterElevation),
            static_cast<double>(elevMin), static_cast<double>(elevMax));
        fprintf(diagF,
            "[VEG v3] per_inst tried=%d rej_water=%d rej_hard=%d "
            "rej_none_fringe=%d rej_overlay=%d rej_slope=%d passed=%u\n",
            instTriedTotal, instRejWater, instRejHard,
            instRejNoneFringe, instRejOverlay, instRejSlope, instCount);
        // 5x5 terrain-type classification grid (rows: N..S, cols: W..E).
        fprintf(diagF, "[VEG v3] terrain_type_class_grid 5x5 (row=N..S col=W..E): F=full S=sparse H=hard\n");
        for (int gr = 0; gr < 5; ++gr) {
            const float gy = originY - mapSpan * ((gr + 0.5f) / 5.0f);
            for (int gc = 0; gc < 5; ++gc) {
                const float gx = originX + mapSpan * ((gc + 0.5f) / 5.0f);
                const float ge = land->getTerrainElevation(Stuff::Vector3D(gx, gy, 0.0f));
                int gTR = 0, gTC = 0, gSR = 0, gSC = 0;
                land->worldToTileCell(Stuff::Vector3D(gx, gy, 0.0f), gTR, gTC, gSR, gSC);
                const long gMapSide = static_cast<long>(Terrain::realVerticesMapSide);
                VegDensity gvd = VegDensity::kHard;
                if (gTR >= 0 && gTC >= 0 && gTR < gMapSide && gTC < gMapSide) {
                    gvd = classifyTerrainType(land->getTerrain(gTR, gTC));
                }
                const char cls = (gvd == VegDensity::kFull) ? 'F'
                               : (gvd == VegDensity::kSparse) ? 'S' : 'H';
                fprintf(diagF, "  [%d,%d] class=%c elev=%.1f\n", gr, gc, cls,
                        static_cast<double>(ge));
            }
        }
        fclose(diagF);
    }
}

void GameAdapters::Vegetation::flush(const float* terrainLightDir_4f, float time,
                                     float camChunkX, float camChunkY, float camChunkZ)
{
    if (!GosVegetation::isEnabled()) return;

    // Camera height gate: skip all vegetation rendering when camera altitude >= 100 WU.
    // Instances are still present in the GPU buffer (generated at mission load); this
    // is purely a per-frame draw skip.  Provides a natural LOD: at ground level (<100 WU)
    // dense grass is visible; at altitude the overhead view draws nothing (grass too small
    // to be meaningful from high up and would tank fill-rate).
    // MC2_VEG_CAM_HEIGHT_GATE: override threshold in WU (default 100.0; 0=disable gate).
    {
        const float heightGate = envFloat("MC2_VEG_CAM_HEIGHT_GATE", 100.0f);
        if (heightGate > 0.0f && camChunkZ >= heightGate) return;
    }

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