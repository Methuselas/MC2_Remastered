// GameOS/gameos/gos_terrain_indirect.cpp
//
// Indirect terrain SOLID-only PR1 — Stage 0 + Stage 2 implementation.
//
// See gos_terrain_indirect.h for the slice overview, the Tracy zone name
// reservations, and the public API contract (counter units = per-quad,
// parity-printer schema, env-gate semantics).
//
// Stage 0: env-gate readers, N1 counters, parity-printer skeleton.
// Stage 2: dense TerrainQuadRecipe SSBO build/reset/invalidate/flush,
//          parity body comparing recipe against live quadList values.
// Stage 3 wires the per-frame thin-record packer, indirect-command builder,
// preflight-arming, and bridge entry.

#include "gos_terrain_indirect.h"
#include "gos_terrain_patch_stream.h"  // TerrainQuadRecipe
#include "gpu_driven_common.h"         // gpu_driven::IsTerrainSolidEnabled
#include "gos_terrain_lighting.h"      // gos_terrain_lighting::GetOutputSsbo()
#include "gos_static_prop_killswitch.h" // gos_GetTerrainMVPMat4()

#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // memcpy

#include <GL/glew.h>

// MC2 types — resolve relative path from GameOS/gameos/
#include "../../mclib/terrain.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/mapdata.h"
#include "../../mclib/move.h"       // PR2c Stage 1c — MissionMap, GameMap, MAPCELL_DIM
#include "../../mclib/terrtxm.h"    // TERRAIN_TXM_SIZE (extern int)
#include "../../mclib/txmmgr.h"     // MC_MAXTEXTURES (cement node-index space)
#include "gos_terrain_bridge.h"     // gos_terrain_bridge_glTextureForGosHandle (cement readback)

#include "../gameos/gos_profiler.h"

// CEMENT_DIAG: file-scope extern for the global mission name buffer
// (defined in code/mechcmd2.cpp:180 and code/logmain.cpp:88 as
// `char missionName[1024]`).  Used by BuildCementCatalogAtlas summary line.
extern char missionName[];

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Env-gate readers
// ---------------------------------------------------------------------------

bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        // Default-on (Stage C of plan v2.3). Literal "0" opts out; absent or
        // anything else = on. Default-on flip arrived bundled into e22fa3a
        // 2026-05-01; was previously deferred pending the gosFX/MLR white-
        // saturation bug (resolved 2026-05-01 in commit e9bf756 by
        // glDisable(GL_BLEND) before the postprocess composite). Verified
        // clean on tier1 5/5 with both MC2_TERRAIN_INDIRECT=0 (legacy
        // regression) and default-on configs 2026-05-01 sess 3 — no
        // destroys delta, FPS parity within noise.
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}

bool IsParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsTraceEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_TRACE");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsCostSplitEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_COST_SPLIT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// N1 counter storage (private to this TU; cross-TU access via public Add/Get)
// ---------------------------------------------------------------------------
namespace {
long long s_legacy_solid_setup_quads     = 0;
long long s_indirect_solid_packed_quads  = 0;
long long s_legacy_detail_overlay_quads  = 0;
// PR2c Stage 0c — independent mine counters.
long long s_legacy_mine_enqueue_quads    = 0;
long long s_legacy_mine_draw_quads       = 0;
long long s_indirect_mine_drawn_cells    = 0;
// PR2a Stage 0a — M2c detail-emit counter (drops to zero post Stage 1a).
long long s_m2c_detail_emit_quads        = 0;
// PR2b Stage 0b — overlay counters (drop to zero post Stage 3b gate-off).
long long s_m2d_overlay_emit_quads       = 0;
long long s_indirect_overlay_packed_quads = 0;
long long s_gos_push_overlay_calls       = 0;  // probe at producer body
}  // namespace

namespace gos_terrain_indirect {

void Counters_AddLegacySolidSetupQuad()      { ++s_legacy_solid_setup_quads; }
void Counters_AddIndirectSolidPackedQuad()   { ++s_indirect_solid_packed_quads; }
void Counters_AddLegacyDetailOverlayQuad()   { ++s_legacy_detail_overlay_quads; }
void Counters_AddLegacyMineEnqueueQuad()     { ++s_legacy_mine_enqueue_quads; }
void Counters_AddLegacyMineDrawQuad()        { ++s_legacy_mine_draw_quads; }
void Counters_AddIndirectMineDrawnCells(long long n) { s_indirect_mine_drawn_cells += n; }

long long Counters_GetLegacySolidSetupQuads()    { return s_legacy_solid_setup_quads; }
long long Counters_GetIndirectSolidPackedQuads() { return s_indirect_solid_packed_quads; }
long long Counters_GetLegacyDetailOverlayQuads() { return s_legacy_detail_overlay_quads; }
long long Counters_GetLegacyMineEnqueueQuads()   { return s_legacy_mine_enqueue_quads; }
long long Counters_GetLegacyMineDrawQuads()      { return s_legacy_mine_draw_quads; }
long long Counters_GetIndirectMineDrawnCells()   { return s_indirect_mine_drawn_cells; }
// PR2a Stage 0a — M2c detail-emit counter.
void      Counters_AddM2cDetailEmitQuad()        { ++s_m2c_detail_emit_quads; }
long long Counters_GetM2cDetailEmitQuads()       { return s_m2c_detail_emit_quads; }
// PR2b Stage 0b — overlay counters.
void      Counters_AddM2dOverlayEmitQuad()       { ++s_m2d_overlay_emit_quads; }
void      Counters_AddIndirectOverlayPackedQuad(){ ++s_indirect_overlay_packed_quads; }
void      Counters_AddGosPushOverlayCall()       { ++s_gos_push_overlay_calls; }
long long Counters_GetM2dOverlayEmitQuads()      { return s_m2d_overlay_emit_quads; }
long long Counters_GetIndirectOverlayPackedQuads(){ return s_indirect_overlay_packed_quads; }
long long Counters_GetGosPushOverlayCalls()      { return s_gos_push_overlay_calls; }

// PR2c — Stage 4 default-on flip 2026-05-08.
// Static-bake mine path retires ~932 µs/frame (mc2_01 baseline:
// enqueueTerrainMineState ~54 µs + drawMine ~886 µs → 0). Tier1 5/5 PASS
// with arming verified across PR2c Stages 0c/1c/2c. Literal "0" opts out
// for bisection; any other value (including unset) opts in.
bool IsMineEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_MINE");
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}

// PR2b Stage 0b — env-gate readers.
bool IsOverlayEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_OVERLAY");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsOverlayParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

// Stage 3b wires the real preflight latch; Stage 0b/1b/2b stub always-false
// so gate-off sites compile and stay dormant until Stage 3b lands the draw.
bool IsFrameOverlayArmed() { return false; }

// IsFrameMineArmed is defined further down (after Stage 1c's
// g_mineTextureArrayReady storage). Forward decl is in the header.

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// Stage 1 cost-split accumulators
// ---------------------------------------------------------------------------
namespace {
long long s_solidBranchNanosThisFrame   = 0;
long long s_detailOverlayNanosThisFrame = 0;
long long s_mineEnqueueNanosThisFrame   = 0;  // PR2c Stage 0c
long long s_mineDrawNanosThisFrame      = 0;  // PR2c Stage 0c
long long s_overlayNanosThisFrame       = 0;  // PR2b Stage 0b
long long s_waterVertProjNanosThisFrame = 0;  // 1A-alt Slice 0
long long s_lightingNanosThisFrame      = 0;  // 1A-alt Slice 0
long long s_recipeCacheNanosThisFrame   = 0;  // 1A-alt Slice 0
long long s_setupTotalNanosThisFrame    = 0;  // 1A-alt Slice 0 follow-up
long long s_cacheResidentNanosThisFrame = 0;  // 1A-alt Slice 0 follow-up
long long s_visibilityCheckNanosThisFrame = 0;  // 1A-alt Slice 0 follow-up #2
long long s_solidBranchNanosTotal       = 0;
long long s_detailOverlayNanosTotal     = 0;
long long s_mineEnqueueNanosTotal       = 0;  // PR2c Stage 0c
long long s_mineDrawNanosTotal          = 0;  // PR2c Stage 0c
long long s_overlayNanosTotal           = 0;  // PR2b Stage 0b
long long s_waterVertProjNanosTotal     = 0;  // 1A-alt Slice 0
long long s_lightingNanosTotal          = 0;  // 1A-alt Slice 0
long long s_recipeCacheNanosTotal       = 0;  // 1A-alt Slice 0
long long s_setupTotalNanosTotal        = 0;  // 1A-alt Slice 0 follow-up
long long s_cacheResidentNanosTotal     = 0;  // 1A-alt Slice 0 follow-up
long long s_visibilityCheckNanosTotal   = 0;  // 1A-alt Slice 0 follow-up #2
int       s_costSplitFramesObserved     = 0;
}  // namespace

namespace gos_terrain_indirect {

void CostSplit_AddSolidNanos(long long n)         { s_solidBranchNanosThisFrame  += n; }
void CostSplit_AddDetailOverlayNanos(long long n) { s_detailOverlayNanosThisFrame += n; }
void CostSplit_AddMineEnqueueNanos(long long n)   { s_mineEnqueueNanosThisFrame  += n; }
void CostSplit_AddMineDrawNanos(long long n)      { s_mineDrawNanosThisFrame     += n; }
void CostSplit_AddOverlayNanos(long long n)       { s_overlayNanosThisFrame      += n; }
void CostSplit_AddWaterVertProjNanos(long long n) { s_waterVertProjNanosThisFrame += n; }
void CostSplit_AddLightingNanos(long long n)      { s_lightingNanosThisFrame      += n; }
void CostSplit_AddRecipeCacheNanos(long long n)   { s_recipeCacheNanosThisFrame   += n; }
void CostSplit_AddSetupTotalNanos(long long n)    { s_setupTotalNanosThisFrame   += n; }
void CostSplit_AddCacheResidentNanos(long long n) { s_cacheResidentNanosThisFrame += n; }
void CostSplit_AddVisibilityCheckNanos(long long n) { s_visibilityCheckNanosThisFrame += n; }

void CostSplit_RollFrame() {
    if (!IsCostSplitEnabled()) return;
    s_solidBranchNanosTotal       += s_solidBranchNanosThisFrame;
    s_detailOverlayNanosTotal     += s_detailOverlayNanosThisFrame;
    s_mineEnqueueNanosTotal       += s_mineEnqueueNanosThisFrame;
    s_mineDrawNanosTotal          += s_mineDrawNanosThisFrame;
    s_overlayNanosTotal           += s_overlayNanosThisFrame;
    s_waterVertProjNanosTotal     += s_waterVertProjNanosThisFrame;
    s_lightingNanosTotal          += s_lightingNanosThisFrame;
    s_recipeCacheNanosTotal       += s_recipeCacheNanosThisFrame;
    s_setupTotalNanosTotal        += s_setupTotalNanosThisFrame;
    s_cacheResidentNanosTotal     += s_cacheResidentNanosThisFrame;
    s_visibilityCheckNanosTotal   += s_visibilityCheckNanosThisFrame;
    ++s_costSplitFramesObserved;
    s_solidBranchNanosThisFrame    = 0;
    s_detailOverlayNanosThisFrame  = 0;
    s_mineEnqueueNanosThisFrame    = 0;
    s_mineDrawNanosThisFrame       = 0;
    s_overlayNanosThisFrame        = 0;
    s_waterVertProjNanosThisFrame  = 0;
    s_lightingNanosThisFrame       = 0;
    s_recipeCacheNanosThisFrame    = 0;
    s_setupTotalNanosThisFrame     = 0;
    s_cacheResidentNanosThisFrame  = 0;
    s_visibilityCheckNanosThisFrame = 0;
}

long long CostSplit_GetSolidNanosTotal()           { return s_solidBranchNanosTotal; }
long long CostSplit_GetDetailOverlayNanosTotal()   { return s_detailOverlayNanosTotal; }
long long CostSplit_GetMineEnqueueNanosTotal()     { return s_mineEnqueueNanosTotal; }
long long CostSplit_GetMineDrawNanosTotal()        { return s_mineDrawNanosTotal; }
long long CostSplit_GetOverlayNanosTotal()         { return s_overlayNanosTotal; }
long long CostSplit_GetWaterVertProjNanosTotal()   { return s_waterVertProjNanosTotal; }
long long CostSplit_GetLightingNanosTotal()        { return s_lightingNanosTotal; }
long long CostSplit_GetRecipeCacheNanosTotal()     { return s_recipeCacheNanosTotal; }
long long CostSplit_GetSetupTotalNanosTotal()      { return s_setupTotalNanosTotal; }
long long CostSplit_GetCacheResidentNanosTotal()   { return s_cacheResidentNanosTotal; }
long long CostSplit_GetVisibilityCheckNanosTotal() { return s_visibilityCheckNanosTotal; }
int       CostSplit_GetFramesObserved()            { return s_costSplitFramesObserved; }

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// Parity-printer + 600-frame summary
// ---------------------------------------------------------------------------
namespace {
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryQuads        = 0;
long long s_paritySummaryMismatches   = 0;
}  // namespace

namespace gos_terrain_indirect {

void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field,
                         uint32_t legacy, uint32_t fast) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    fprintf(stderr,
            "[TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=%d quad=%d "
            "layer=%s tri=%d vert=%d field=%s legacy=0x%08X fast=0x%08X\n",
            frame, quad, layer ? layer : "?", tri, vert, field ? field : "?",
            legacy, fast);
    fflush(stderr);
}

void ParityFrameTick(int quadsCheckedThisFrame) {
    ++s_paritySummaryFrames;
    s_paritySummaryQuads      += quadsCheckedThisFrame;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;
    if (s_paritySummaryFrames % 600 == 0) {
        // Cost-split columns are appended only when MC2_TERRAIN_COST_SPLIT
        // is set — otherwise the all-zero noise confuses readers.
        const int    csFrames = CostSplit_GetFramesObserved();
        const bool   csOn     = (csFrames > 0);
        const long long csSolidNs   = csOn ? CostSplit_GetSolidNanosTotal()         : 0;
        const long long csDetailNs  = csOn ? CostSplit_GetDetailOverlayNanosTotal() : 0;
        const long long csMineEnqNs = csOn ? CostSplit_GetMineEnqueueNanosTotal()   : 0;
        const long long csMineDrwNs = csOn ? CostSplit_GetMineDrawNanosTotal()      : 0;
        const long long csOverlayNs = csOn ? CostSplit_GetOverlayNanosTotal()       : 0;
        const long long csWvpNs     = csOn ? CostSplit_GetWaterVertProjNanosTotal() : 0;
        const long long csLightNs   = csOn ? CostSplit_GetLightingNanosTotal()      : 0;
        const long long csRecipeNs  = csOn ? CostSplit_GetRecipeCacheNanosTotal()   : 0;
        const long long csSetupNs   = csOn ? CostSplit_GetSetupTotalNanosTotal()    : 0;
        const long long csResNs     = csOn ? CostSplit_GetCacheResidentNanosTotal() : 0;
        const long long csVisNs     = csOn ? CostSplit_GetVisibilityCheckNanosTotal() : 0;
        const long long csSolidPerFrame    = csOn ? csSolidNs   / csFrames : 0;
        const long long csDetailPerFrame   = csOn ? csDetailNs  / csFrames : 0;
        const long long csMineEnqPerFrame  = csOn ? csMineEnqNs / csFrames : 0;
        const long long csMineDrwPerFrame  = csOn ? csMineDrwNs / csFrames : 0;
        const long long csOverlayPerFrame  = csOn ? csOverlayNs / csFrames : 0;
        const long long csWvpPerFrame      = csOn ? csWvpNs     / csFrames : 0;
        const long long csLightPerFrame    = csOn ? csLightNs   / csFrames : 0;
        const long long csRecipePerFrame   = csOn ? csRecipeNs  / csFrames : 0;
        const long long csSetupPerFrame    = csOn ? csSetupNs   / csFrames : 0;
        const long long csResPerFrame      = csOn ? csResNs     / csFrames : 0;
        const long long csVisPerFrame      = csOn ? csVisNs     / csFrames : 0;
        if (csOn) {
            fprintf(stderr,
                    "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                    "quads_checked=%lld total_mismatches=%lld "
                    "legacy_solid_setup_quads=%lld "
                    "indirect_solid_packed_quads=%lld "
                    "legacy_detail_overlay_quads=%lld "
                    "legacy_mine_enqueue_quads=%lld "
                    "legacy_mine_draw_quads=%lld "
                    "indirect_mine_drawn_cells=%lld "
                    "m2c_detail_emit_quads=%lld "
                    "legacy_m2d_overlay_emit_quads=%lld "
                    "indirect_overlay_packed_quads=%lld "
                    "gos_push_overlay_calls=%lld "
                    "solid_branch_ns_per_frame=%lld "
                    "detail_overlay_branch_ns_per_frame=%lld "
                    "mine_enqueue_ns_per_frame=%lld "
                    "mine_draw_ns_per_frame=%lld "
                    "overlay_ns_per_frame=%lld "
                    "water_vert_proj_ns_per_frame=%lld "
                    "lighting_ns_per_frame=%lld "
                    "recipe_cache_ns_per_frame=%lld "
                    "setup_total_ns_per_frame=%lld "
                    "cache_resident_ns_per_frame=%lld "
                    "visibility_check_ns_per_frame=%lld "
                    "frames_observed=%d\n",
                    s_paritySummaryFrames,
                    s_paritySummaryQuads,
                    s_paritySummaryMismatches,
                    Counters_GetLegacySolidSetupQuads(),
                    Counters_GetIndirectSolidPackedQuads(),
                    Counters_GetLegacyDetailOverlayQuads(),
                    Counters_GetLegacyMineEnqueueQuads(),
                    Counters_GetLegacyMineDrawQuads(),
                    Counters_GetIndirectMineDrawnCells(),
                    Counters_GetM2cDetailEmitQuads(),
                    Counters_GetM2dOverlayEmitQuads(),
                    Counters_GetIndirectOverlayPackedQuads(),
                    Counters_GetGosPushOverlayCalls(),
                    csSolidPerFrame,
                    csDetailPerFrame,
                    csMineEnqPerFrame,
                    csMineDrwPerFrame,
                    csOverlayPerFrame,
                    csWvpPerFrame,
                    csLightPerFrame,
                    csRecipePerFrame,
                    csSetupPerFrame,
                    csResPerFrame,
                    csVisPerFrame,
                    csFrames);
        } else {
            fprintf(stderr,
                    "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                    "quads_checked=%lld total_mismatches=%lld "
                    "legacy_solid_setup_quads=%lld "
                    "indirect_solid_packed_quads=%lld "
                    "legacy_detail_overlay_quads=%lld "
                    "legacy_mine_enqueue_quads=%lld "
                    "legacy_mine_draw_quads=%lld "
                    "indirect_mine_drawn_cells=%lld "
                    "m2c_detail_emit_quads=%lld "
                    "legacy_m2d_overlay_emit_quads=%lld "
                    "indirect_overlay_packed_quads=%lld "
                    "gos_push_overlay_calls=%lld\n",
                    s_paritySummaryFrames,
                    s_paritySummaryQuads,
                    s_paritySummaryMismatches,
                    Counters_GetLegacySolidSetupQuads(),
                    Counters_GetIndirectSolidPackedQuads(),
                    Counters_GetLegacyDetailOverlayQuads(),
                    Counters_GetLegacyMineEnqueueQuads(),
                    Counters_GetLegacyMineDrawQuads(),
                    Counters_GetIndirectMineDrawnCells(),
                    Counters_GetM2cDetailEmitQuads(),
                    Counters_GetM2dOverlayEmitQuads(),
                    Counters_GetIndirectOverlayPackedQuads(),
                    Counters_GetGosPushOverlayCalls());
        }
        fflush(stderr);
    }
}

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// Stage 2 — dense recipe SSBO storage and helpers
// ---------------------------------------------------------------------------

namespace {

// Module-private: dense recipe array indexed by vertexNum (= mx + my * mapSide).
// Sized mapSide² when built; cleared on ResetDenseRecipe.
std::vector<TerrainQuadRecipe> g_denseRecipes;
std::vector<bool>              g_denseRecipeDirty;
bool                           g_denseRecipeAnyDirty = false;
GLuint                         g_recipeSSBO          = 0;
int32_t                        g_recipeMapSide       = 0;
bool                           g_recipeReady         = false;

// Mission-latch for trace reset
static bool s_firstDrawPrintedThisMission = false;

// Cached trace flag (avoids repeated getenv calls in hot paths)
static bool s_indirectTrace = false;
static bool s_indirectTraceKnown = false;
static bool traceOn() {
    if (!s_indirectTraceKnown) {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_TRACE");
        s_indirectTrace = (v && v[0] == '1' && v[1] == '\0');
        s_indirectTraceKnown = true;
    }
    return s_indirectTrace;
}

// Map MC2 terrain type enum (0-20) to PBR material index (0-3).
// Mirrors quad.cpp terrainTypeToMaterial (file-static, not exported) and
// gos_terrain_water_stream.cpp terrainTypeToMaterialLocal.
// MUST stay in sync. Any drift shows up immediately in the parity check
// _wp0 comparison the next time MC2_TERRAIN_INDIRECT_PARITY_CHECK runs.
// 0=Rock, 1=Grass, 2=Dirt, 3=Concrete
inline uint8_t terrainTypeToMaterialLocal(uint32_t terrainType) {
    switch (terrainType) {
        case 3:  case 8:  case 9:  case 12:           return 1; // Grass
        case 2:  case 4:                              return 2; // Dirt
        case 10: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20:           return 3; // Concrete
        default:                                      return 0; // Rock
    }
}

// Populate one slot in the dense recipe array from MapData.
// Called both from BuildDenseRecipe (full build at primeMissionTerrainCache)
// and from InvalidateRecipeForVertexNum (in-gameplay precise rebuild).
//
// Judgment call on UV defaults during in-gameplay invalidation:
//   setTerrain() calls invalidateTerrainFaceCache(), which frees the entire
//   Shape C cache. After that, getTerrainFaceCacheEntry returns nullptr until
//   the next primeMissionTerrainCache call (mission reload). For in-gameplay
//   mutations we therefore fall back to the default UV values from
//   quad.cpp:1734-1737 — the same fallback legacy uses once the cache is gone.
//   This is correct: default UVs are the pre-Shape-C half-texel padding values.
void buildRecipeSlot(int32_t vn, TerrainQuadRecipe& out) {
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 0) { memset(&out, 0, sizeof(TerrainQuadRecipe)); return; }

    // vn = top-left corner vertexNum = mx + my * mapSide
    const long mx = vn % mapSide;
    const long my = vn / mapSide;

    // Edge vertex: no valid quad. Zero out.
    if (mx >= mapSide - 1 || my >= mapSide - 1) {
        memset(&out, 0, sizeof(TerrainQuadRecipe));
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData ? Terrain::mapData->getBlocks() : nullptr;
    if (!blocks) { memset(&out, 0, sizeof(TerrainQuadRecipe)); return; }

    const long halfSide = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;

    // Corner layout (matches water stream and quad.cpp):
    //   v0 = (mx,   my)     top-left
    //   v1 = (mx+1, my)     top-right
    //   v2 = (mx+1, my+1)   bottom-right
    //   v3 = (mx,   my+1)   bottom-left
    const PostcompVertex& p0 = blocks[mx       + my       * mapSide];
    const PostcompVertex& p1 = blocks[(mx + 1) + my       * mapSide];
    const PostcompVertex& p2 = blocks[(mx + 1) + (my + 1) * mapSide];
    const PostcompVertex& p3 = blocks[mx       + (my + 1) * mapSide];

    // World X/Y from map indices (gos_terrain_water_stream.cpp:125-130)
    const float wx0 = float(mx     - halfSide) * wupv;
    const float wy0 = float(halfSide - my    ) * wupv;
    const float wx1 = float(mx + 1 - halfSide) * wupv;
    const float wy1 = wy0;
    const float wx2 = wx1;
    const float wy2 = float(halfSide - (my + 1)) * wupv;
    const float wx3 = wx0;
    const float wy3 = wy2;

    out.wx0 = wx0; out.wy0 = wy0; out.wz0 = p0.elevation; out._wp0 = 0.f;
    out.wx1 = wx1; out.wy1 = wy1; out.wz1 = p1.elevation; out._wp1 = 0.f;
    out.wx2 = wx2; out.wy2 = wy2; out.wz2 = p2.elevation; out._wp2 = 0.f;
    out.wx3 = wx3; out.wy3 = wy3; out.wz3 = p3.elevation; out._wp3 = 0.f;

    out.nx0 = p0.vertexNormal.x; out.ny0 = p0.vertexNormal.y; out.nz0 = p0.vertexNormal.z; out._np0 = 0.f;
    out.nx1 = p1.vertexNormal.x; out.ny1 = p1.vertexNormal.y; out.nz1 = p1.vertexNormal.z; out._np1 = 0.f;
    out.nx2 = p2.vertexNormal.x; out.ny2 = p2.vertexNormal.y; out.nz2 = p2.vertexNormal.z; out._np2 = 0.f;
    out.nx3 = p3.vertexNormal.x; out.ny3 = p3.vertexNormal.y; out.nz3 = p3.vertexNormal.z; out._np3 = 0.f;

    // UV extents — mirror quad.cpp:1734-1748 logic.
    // Default: half-texel padding. Override from Shape C cache when available.
    float minU = 0.5f / TERRAIN_TXM_SIZE;
    float maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
    float minV = minU;
    float maxV = maxU;

    if (Terrain::terrainTextures2 && Terrain::mapData) {
        const MapData::WorldQuadTerrainCacheEntry* entry =
            Terrain::mapData->getTerrainFaceCacheEntry(my, mx);  // (tileR=my, tileC=mx)
        if (entry && entry->isValid()) {
            // Mirror quad.cpp:1743: only use uvData when NOT (overlayHandle==0xffffffff && isCement)
            const bool isCement  = entry->isCement();
            const bool noOverlay = (entry->overlayHandle == 0xffffffffu);
            if (!(noOverlay && isCement)) {
                minU = entry->uvData.minU;
                minV = entry->uvData.minV;
                maxU = entry->uvData.maxU;
                maxV = entry->uvData.maxV;
            }
            // Bake terrain texture nodeId into _wp2 for GPU-direct dispatch.
            // _wp3 (cementWord) is baked in PopulateRecipeCementWords() after
            // BuildCementCatalogAtlas() because g_cementLayerIndexBySlot isn't
            // ready yet at buildRecipeSlot() time.
            const uint32_t nodeId = (uint32_t)entry->terrainHandle;
            memcpy(&out._wp2, &nodeId, 4);
        }
        // If entry is NULL or !isValid(): _wp2 remains 0 → GPU skips quad.
    }

    out.minU = minU; out.minV = minV; out.maxU = maxU; out.maxV = maxV;

    // Pack 4 corner material types into _wp0 (bit-preserving; shader reads
    // via floatBitsToUint per gos_terrain_thin.vert:122).
    {
        const uint32_t m0 = terrainTypeToMaterialLocal(p0.terrainType);
        const uint32_t m1 = terrainTypeToMaterialLocal(p1.terrainType);
        const uint32_t m2 = terrainTypeToMaterialLocal(p2.terrainType);
        const uint32_t m3 = terrainTypeToMaterialLocal(p3.terrainType);
        const uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
        memcpy(&out._wp0, &tpacked, 4);
    }
}


}  // anonymous namespace

// ---------------------------------------------------------------------------
// Colormap atlas — single GL texture covering the full merged colormap.
//
// cpuColorMap (terrtxm2.h:93) already holds the entire RGBA atlas in CPU
// memory, retained at mission load.  We upload it once as a plain
// GL_TEXTURE_2D so gos_terrain_bridge_drawIndirect can bind it at unit 0
// instead of per-bucket tile binds.
//
// Atlas UV formula (mirrors terrtxm2.cpp:resolveTextureHandle):
//   posX  = (worldX - mapTopLeft3d.x) * oneOverWorldUnitsMapSide
//   posY  = (mapTopLeft3d.y - worldY) * oneOverWorldUnitsMapSide
//   tileX = floor((posX + 0.0005) * numTexturesAcross)
//   tileY = floor((posY + 0.0005) * numTexturesAcross)
//   atlasUV = (vec2(tileX, tileY) + perTileUV) / numTexturesAcross
//
// The per-tile UV (from recipe.uvData) is already in [0,1] within a tile.
// Dividing by numTexturesAcross converts tile-local to atlas-absolute.
// ---------------------------------------------------------------------------

namespace {

static GLuint  g_atlasGLTex              = 0;
static int     g_atlasSize               = 0;
static float   g_atlasNumTexturesAcross  = 0.f;
static float   g_atlasMapTopLeftX        = 0.f;
static float   g_atlasMapTopLeftY        = 0.f;
static float   g_atlasOneOverWorldUnits  = 0.f;

// ---------------------------------------------------------------------------
// Cement catalog atlas — single GL_TEXTURE_2D, packed grid of N cement tile
// textures.  Built once per mission at BuildDenseRecipe() time via GPU
// readback from already-resident catalog textures (textureData[0] in
// tileRAMHeap is dead in stock gameplay — quickLoad gates the RAM path at
// terrtxm.cpp:561).  Bound at unit 3 by gos_terrain_bridge_drawIndirect.
//
// LAYER MAP: keyed by mcTextureNodeIndex (NOT textures[] slot).
//   q.terrainHandle returned by quad.cpp:546 (getTextureHandle) is the
//   nodeIdx, NOT the slot — see V22 in Verification Appendix.
// ---------------------------------------------------------------------------
static GLuint  g_cementAtlasGLTex          = 0;
static int     g_cementAtlasGridSide       = 0;   // cells per row/col (power of 2)
static int     g_cementAtlasTileCount      = 0;   // distinct cement entries enumerated
static bool    g_cementLayerMapReady       = false;
static int     g_cementCatalogTruncated    = 0;   // 1 if N>=255 cap hit (Gate A FAIL)

// Dense lookup: mcTextureNodeIndex → atlas layer-index (0..N-1).
// Sized MC_MAXTEXTURES = 4096 (mclib/txmmgr.h:44) — node-index space.
// 0xFFFF = "not cement / not in atlas".
static uint16_t g_cementLayerIndexByNodeIdx[MC_MAXTEXTURES];

// Slot-keyed lookup: textures[] slot index → atlas layer-index (0..N-1).
// SLOT IS THE STABLE KEY (persistent across frames).  nodeIdx (mcTextureManager
// handle) mutates per-frame per memory/mc2_texture_handle_is_live.md, so a
// nodeIdx-keyed lookup miss-hits per frame → cement validity bit flickers
// → visible concrete flicker.  Slot is allocated once by initTexture and is
// stable for the mission lifetime.
//
// Sized MC_MAX_TERRAIN_TXMS = 3000 (terrtxm.h:34) — the textures[] cap.
// 0xFFFF = "not cement / not in atlas".
static uint16_t g_cementLayerIndexBySlot[MC_MAX_TERRAIN_TXMS];

// ---------------------------------------------------------------------------
// PopulateRecipeCementWords — bake cementWord into recipe._wp3 for every slot.
// Must be called AFTER BuildCementCatalogAtlas() because g_cementLayerIndexBySlot
// is not populated until that function runs.
// Marks all modified slots dirty so FlushDirtyRecipeSlotsToGPU uploads them.
// ---------------------------------------------------------------------------
static void PopulateRecipeCementWords() {
    if (!Terrain::mapData) return;
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 0) return;
    const size_t N = g_denseRecipes.size();
    constexpr uint32_t kCementLayerValidBit = 0x80000000u;

    for (size_t vn = 0; vn < N; ++vn) {
        const long mx = (long)vn % mapSide;
        const long my = (long)vn / mapSide;
        if (mx >= mapSide - 1 || my >= mapSide - 1) continue;

        uint32_t cementWord = 0u;
        if (g_cementLayerMapReady) {
            const DWORD texData = Terrain::mapData->getTexture(my, mx);
            const DWORD slot    = texData & 0xFFFFu;
            if (slot < (DWORD)MC_MAX_TERRAIN_TXMS) {
                const uint16_t idx = g_cementLayerIndexBySlot[slot];
                if (idx != 0xFFFFu)
                    cementWord = kCementLayerValidBit | ((uint32_t)idx & 0xFFFFu);
            }
        }
        memcpy(&g_denseRecipes[vn]._wp3, &cementWord, 4);
        g_denseRecipeDirty[vn]  = true;
    }
    g_denseRecipeAnyDirty = true;
}

// ---------------------------------------------------------------------------
// CollectUniqueNodeIds — build list of unique terrain texture nodeIds from
// the baked _wp2 fields. Called once per mission after recipes are fully
// populated. Used by UploadTerrainHandleLUT() to resolve handles per frame
// without iterating the full quadList.
// ---------------------------------------------------------------------------
static std::vector<uint32_t> g_uniqueTerrainNodeIds;

static void CollectUniqueNodeIds() {
    g_uniqueTerrainNodeIds.clear();
    static bool seen[MC_MAXTEXTURES];
    memset(seen, 0, sizeof(seen));
    for (const TerrainQuadRecipe& rec : g_denseRecipes) {
        uint32_t nodeId;
        memcpy(&nodeId, &rec._wp2, 4);
        if (nodeId == 0 || nodeId >= (uint32_t)MC_MAXTEXTURES) continue;
        if (!seen[nodeId]) {
            seen[nodeId] = true;
            g_uniqueTerrainNodeIds.push_back(nodeId);
        }
    }
}

// Per-frame counter — incremented when the packer sees a quad whose
// q.terrainHandle is non-zero AND maps to no cement layer.  A non-zero count
// after Stage A.4 is wired indicates an enumeration miss (debug discipline).
static uint32_t g_cementPackUnmappedCount = 0;

// Diagnostic Test 1 — per-frame cement classification flip detection.
// Reset at the start of PackThinRecordsForFrame, emitted every 60 frames
// when MC2_TERRAIN_INDIRECT_TRACE is on.
static uint32_t g_cementMappedThisFrame       = 0;  // valid cement layer found
static uint32_t g_concreteAllCornersThisFrame = 0;  // _wp0 == 3,3,3,3 (genuine pure-cement quad)

void BuildColormapAtlas() {
    ZoneScopedN("Terrain::IndirectAtlasUpload");
    if (!Terrain::terrainTextures2) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=atlas_skip reason=no_terrainTextures2\n");
        return;
    }
    auto* tcm = Terrain::terrainTextures2;
    if (!tcm->cpuColorMap || tcm->cpuColorMapSize <= 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=atlas_skip reason=no_cpuColorMap\n");
        return;
    }

    if (g_atlasGLTex == 0) glGenTextures(1, &g_atlasGLTex);
    glBindTexture(GL_TEXTURE_2D, g_atlasGLTex);
    // cpuColorMap is BGRA-in-memory (mc2_argb_packing memory note: MC2's textures
    // are BGRA). Upload format param = GL_BGRA so the driver swizzles to RGBA8
    // storage at upload time.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 tcm->cpuColorMapSize, tcm->cpuColorMapSize,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, tcm->cpuColorMap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_atlasSize              = tcm->cpuColorMapSize;
    g_atlasNumTexturesAcross = tcm->getNumTexturesAcross();
    g_atlasMapTopLeftX       = Terrain::mapTopLeft3d.x;
    g_atlasMapTopLeftY       = Terrain::mapTopLeft3d.y;
    g_atlasOneOverWorldUnits = Terrain::oneOverWorldUnitsMapSide;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=atlas_built size=%d numTilesAcross=%.4f "
               "mapTopLeftX=%.3f mapTopLeftY=%.3f oneOverWorldUnits=%.9f gltex=%u\n",
               g_atlasSize, g_atlasNumTexturesAcross,
               g_atlasMapTopLeftX, g_atlasMapTopLeftY, g_atlasOneOverWorldUnits,
               (unsigned)g_atlasGLTex);
        fflush(stdout);
    }
}

// Bridge accessor: gosHandle → GLuint texture name (declared in
// gos_terrain_bridge.h, included above).  Implemented at
// GameOS/gameos/gameos_graphics.cpp:1775-1781.

// BuildCementCatalogAtlas — GPU readback path (B1 of plan v2.1).
// Runs at BuildDenseRecipe() time.  Walks textures[0..nextAvailable-1],
// filters by isCement(slot), reads each cement tile via glGetTexImage,
// blits into a packed grid atlas, uploads as a single GL_TEXTURE_2D.
//
// quickLoad-safe: does not depend on tileRAMHeap textureData[0] (which
// is NULL in stock gameplay per terrtxm.cpp:561 enclosing gate).
//
// NO MIPMAPS: cement atlas cells are packed without inter-cell gutters,
// so glGenerateMipmap would bleed neighboring cells.  Sampler is GL_LINEAR
// min/mag; potential shimmer at distance/oblique angles is accepted —
// Gate A includes a distance/oblique screenshot to surface this.  Per-cell
// mip generation with gutters is a follow-up slice.
void BuildCementCatalogAtlas() {
    ZoneScopedN("Terrain::IndirectCementAtlasUpload");

    if (!Terrain::terrainTextures) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_terrainTextures\n");
        return;
    }
    auto* tt = Terrain::terrainTextures;

    const int txmSize = TERRAIN_TXM_SIZE;  // extern int, typically 64 (terrtxm.cpp:51)
    const long lastSlot = tt->getNextAvailableSlot();
    if (lastSlot <= 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_slots\n");
        return;
    }

    // Pass 1: enumerate cement slots, resolve each to (nodeIdx, GLuint).
    std::vector<int>    cementSlots;
    std::vector<DWORD>  cementNodeIndices;
    std::vector<GLuint> cementGLTextures;
    cementSlots.reserve(64);
    cementNodeIndices.reserve(64);
    cementGLTextures.reserve(64);
    bool truncated = false;

    // CEMENT_DIAG (temporary, gated by MC2_TERRAIN_INDIRECT_TRACE).
    // Walk the FULL slot range without the 255-cap break, classify each
    // cement-flagged slot as base (< firstTransition) or transition,
    // and emit a per-slot line + summary.  See MEMORY: cement_diag breakdown.
    const long firstTransition = tt->getFirstTransition();
    long diagTotal = 0, diagBase = 0, diagTransition = 0;

    for (long slot = 0; slot < lastSlot; ++slot) {
        if (!tt->isCement((DWORD)slot)) continue;
        if (tt->isAlpha((DWORD)slot)) continue;  // pure-cement only (brainstorm Q1)
        const DWORD nodeIdx = tt->peekTextureHandle((DWORD)slot);

        // CEMENT_DIAG per-slot line.
        const int isBase = (firstTransition < 0 || slot < firstTransition) ? 1 : 0;
        ++diagTotal;
        if (isBase) ++diagBase; else ++diagTransition;
        if (traceOn()) {
            printf("[CEMENT_DIAG] slot=%ld nodeIdx=%u isBase=%d (firstTransition=%ld nextAvailable=%ld)\n",
                   slot, (unsigned)nodeIdx, isBase,
                   firstTransition, lastSlot);
        }

        if (nodeIdx == 0xffffffffu) continue;
        if (nodeIdx >= (DWORD)MC_MAXTEXTURES) {
            if (traceOn()) {
                printf("[TERRAIN_INDIRECT v1] event=cement_atlas_nodeidx_oob "
                       "slot=%ld nodeIdx=%u cap=%d\n",
                       slot, (unsigned)nodeIdx, (int)MC_MAXTEXTURES);
                fflush(stdout);
            }
            continue;
        }
        const DWORD gosHandle = tex_resolve(nodeIdx);
        if (gosHandle == 0u) continue;
        const GLuint glTex = gos_terrain_bridge_glTextureForGosHandle((unsigned)gosHandle);
        if (glTex == 0) continue;
        // Record-but-don't-break past atlas budget cap: we still want the
        // diag totals to reflect TRUE counts for tier1.  Atlas allocation
        // gated on size below to avoid memory blowup.
        // Cap = 1024 = atlas budget cap (gridSide=32, 2048x2048 = 16 MB).
        // NOT the _pad0 encoding cap (16-bit field, max 65535).  If anyone
        // ever exceeds 1024, the atlas budget needs revisiting.
        if (cementNodeIndices.size() < 1024) {
            cementSlots.push_back((int)slot);
            cementNodeIndices.push_back(nodeIdx);
            cementGLTextures.push_back(glTex);
        } else {
            truncated = true;
        }
    }

    // CEMENT_DIAG summary — always emit when trace on, regardless of cap.
    if (traceOn()) {
        printf("[CEMENT_DIAG] summary mission=%s total_cement=%ld base=%ld transitions=%ld nextAvailable=%ld firstTransition=%ld\n",
               (::missionName[0] ? ::missionName : "unknown"),
               diagTotal, diagBase, diagTransition,
               lastSlot, firstTransition);
        fflush(stdout);
    }

    // Memory blowup guard: if true cement count > 1024, skip atlas alloc
    // entirely (diag-only run).  Tier1 expected <300 per plan; this is
    // a safety net while the diag is in tree.
    if (diagTotal > 1024) {
        if (traceOn()) {
            printf("[CEMENT_DIAG] atlas_alloc_skipped diagTotal=%ld exceeds 1024 cap\n", diagTotal);
            fflush(stdout);
        }
        return;
    }

    const int N = (int)cementNodeIndices.size();
    if (N == 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_cement_tiles count=0\n");
        return;
    }

    // Build nodeIdx → layer-index map.
    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    for (int k = 0; k < N; ++k) {
        g_cementLayerIndexByNodeIdx[cementNodeIndices[k]] = (uint16_t)k;
    }

    // Build slot → layer-index map.  Slot is the STABLE key (nodeIdx mutates
    // per-frame per memory/mc2_texture_handle_is_live.md).  This is the lookup
    // the per-frame packer uses; the nodeIdx map is kept for one-commit dead-code.
    memset(g_cementLayerIndexBySlot, 0xFF, sizeof(g_cementLayerIndexBySlot));
    for (int k = 0; k < N; ++k) {
        if (cementSlots[k] >= 0 && cementSlots[k] < MC_MAX_TERRAIN_TXMS) {
            g_cementLayerIndexBySlot[cementSlots[k]] = (uint16_t)k;
        }
    }

    // Grid: smallest power-of-2 side fitting N cells in a square.
    int gridSide = 1;
    while (gridSide * gridSide < N) gridSide <<= 1;
    const int atlasPixelSide = gridSide * txmSize;

    std::vector<uint32_t> atlasBuf((size_t)atlasPixelSide * atlasPixelSide, 0u);
    std::vector<uint32_t> tileBuf((size_t)txmSize * txmSize, 0u);

    // Save GL state (V24).
    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0Binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (int k = 0; k < N; ++k) {
        glBindTexture(GL_TEXTURE_2D, cementGLTextures[k]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, tileBuf.data());

        const int col  = k % gridSide;
        const int row  = k / gridSide;
        const int dstX = col * txmSize;
        const int dstY = row * txmSize;
        for (int py = 0; py < txmSize; ++py) {
            const uint32_t* srcRow = &tileBuf[(size_t)py * txmSize];
            uint32_t*       dstRow = &atlasBuf[(size_t)(dstY + py) * atlasPixelSide + dstX];
            memcpy(dstRow, srcRow, (size_t)txmSize * sizeof(uint32_t));
        }
    }

    if (g_cementAtlasGLTex == 0) glGenTextures(1, &g_cementAtlasGLTex);
    glBindTexture(GL_TEXTURE_2D, g_cementAtlasGLTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 atlasPixelSide, atlasPixelSide, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, atlasBuf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // no mips — see header comment
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Restore state (V24).
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glActiveTexture((GLenum)savedActive);

    g_cementAtlasGridSide    = gridSide;
    g_cementAtlasTileCount   = N;
    g_cementCatalogTruncated = truncated ? 1 : 0;
    g_cementLayerMapReady    = true;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=cement_catalog_built tile_count=%d "
               "atlas_size=%dx%d grid_side=%d gltex=%u truncated=%d "
               "unmapped_pack_count=%u\n",
               N, atlasPixelSide, atlasPixelSide, gridSide,
               (unsigned)g_cementAtlasGLTex,
               g_cementCatalogTruncated,
               g_cementPackUnmappedCount);
        if (truncated) {
            printf("[TERRAIN_INDIRECT v1] event=cement_catalog_truncated count=1024\n");
        }
        fflush(stdout);
    }
}

}  // anonymous namespace (atlas helpers)

// Bridge accessors — declared extern in gameos_graphics.cpp.
GLuint gos_terrain_indirect_getAtlasGLTex()            { return g_atlasGLTex; }
float  gos_terrain_indirect_getNumTexturesAcross()     { return g_atlasNumTexturesAcross; }
float  gos_terrain_indirect_getAtlasMapTopLeftX()      { return g_atlasMapTopLeftX; }
float  gos_terrain_indirect_getAtlasMapTopLeftY()      { return g_atlasMapTopLeftY; }
float  gos_terrain_indirect_getAtlasOneOverWorldUnits(){ return g_atlasOneOverWorldUnits; }

GLuint gos_terrain_indirect_getCementAtlasGLTex()    { return g_cementAtlasGLTex; }
int    gos_terrain_indirect_getCementAtlasGridSide() { return g_cementAtlasGridSide; }
bool   gos_terrain_indirect_isCementAtlasReady()     { return g_cementLayerMapReady && g_cementAtlasGLTex != 0; }
float  gos_terrain_indirect_getWorldUnitsPerVertex() { return Terrain::worldUnitsPerVertex; }

// ---------------------------------------------------------------------------
// Stage 2 public API
// ---------------------------------------------------------------------------

namespace gos_terrain_indirect {

void BuildDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeBuild");
    if (!Terrain::mapData) return;

    g_recipeMapSide = Terrain::realVerticesMapSide;
    const size_t N  = (size_t)g_recipeMapSide * (size_t)g_recipeMapSide;

    g_denseRecipes.assign(N, TerrainQuadRecipe{});
    g_denseRecipeDirty.assign(N, false);
    g_denseRecipeAnyDirty        = false;
    g_recipeReady                = false;
    s_firstDrawPrintedThisMission = false;

    // BuildDenseRecipe is called AFTER mapData->buildTerrainFaceCache (terrain.cpp:585),
    // so the Shape C cache is populated and getTerrainFaceCacheEntry returns valid entries.
    for (int32_t vn = 0; vn < (int32_t)N; ++vn) {
        buildRecipeSlot(vn, g_denseRecipes[vn]);
    }

    // Full GPU upload on mission load.
    if (g_recipeSSBO == 0) glGenBuffers(1, &g_recipeSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(N * sizeof(TerrainQuadRecipe)),
                 g_denseRecipes.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    g_recipeReady = true;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=recipe_build mapSide=%d entries=%zu "
               "bytes=%zu ssbo=%u\n",
               g_recipeMapSide, N, N * sizeof(TerrainQuadRecipe),
               (unsigned)g_recipeSSBO);
        fflush(stdout);
    }

    // Upload the merged colormap atlas for the indirect draw bridge.
    // Must run after recipe build so terrainTextures2 is ready.
    BuildColormapAtlas();

    // Build cement catalog atlas via GPU readback (textureData[0] is NULL in
    // stock gameplay; see plan v2.1 §C1/B1).
    BuildCementCatalogAtlas();

    // Bake cementWord into _wp3 now that g_cementLayerIndexBySlot is ready.
    PopulateRecipeCementWords();

    // Collect unique terrain texture nodeIds for per-frame LUT upload.
    CollectUniqueNodeIds();
}

void ResetDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeReset");

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=recipe_reset ssbo=%u\n",
               (unsigned)g_recipeSSBO);
        fflush(stdout);
    }

    g_denseRecipes.clear();
    g_denseRecipeDirty.clear();
    g_denseRecipeAnyDirty        = false;
    g_recipeMapSide              = 0;
    g_recipeReady                = false;
    s_firstDrawPrintedThisMission = false;
    // g_recipeSSBO stays allocated — reused by next mission's BuildDenseRecipe.
    // Mirrors WaterStream::Reset() pattern.

    // Tear down the atlas GL texture (per-mission; rebuilt by BuildColormapAtlas).
    if (g_atlasGLTex != 0) {
        glDeleteTextures(1, &g_atlasGLTex);
        g_atlasGLTex = 0;
    }
    g_atlasSize              = 0;
    g_atlasNumTexturesAcross = 0.f;
    g_atlasMapTopLeftX       = 0.f;
    g_atlasMapTopLeftY       = 0.f;
    g_atlasOneOverWorldUnits = 0.f;

    // Cement catalog atlas teardown — mirror g_atlasGLTex pattern.
    if (g_cementAtlasGLTex != 0) {
        glDeleteTextures(1, &g_cementAtlasGLTex);
        g_cementAtlasGLTex = 0;
    }
    g_cementAtlasGridSide    = 0;
    g_cementAtlasTileCount   = 0;
    g_cementLayerMapReady    = false;
    g_cementCatalogTruncated = 0;
    g_cementPackUnmappedCount = 0;
    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    memset(g_cementLayerIndexBySlot,    0xFF, sizeof(g_cementLayerIndexBySlot));

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=cement_catalog_reset\n");
        fflush(stdout);
    }
}

bool IsDenseRecipeReady() {
    return g_recipeReady && g_recipeSSBO != 0;
}

const TerrainQuadRecipe* RecipeForVertexNum(int32_t vn) {
    if (vn < 0) return nullptr;
    if (static_cast<size_t>(vn) >= g_denseRecipes.size()) return nullptr;
    return &g_denseRecipes[vn];
}

void InvalidateRecipeForVertexNum(int32_t vn) {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    if (vn < 0 || static_cast<size_t>(vn) >= g_denseRecipes.size()) return;
    buildRecipeSlot(vn, g_denseRecipes[vn]);
    g_denseRecipeDirty[vn] = true;
    g_denseRecipeAnyDirty  = true;
    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=invalidate vn=%d\n", vn);
        fflush(stdout);
    }
}

void InvalidateAllRecipes() {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    const size_t N = g_denseRecipes.size();
    for (size_t vn = 0; vn < N; ++vn) {
        buildRecipeSlot((int32_t)vn, g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = true;
    }
    g_denseRecipeAnyDirty = true;
    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=invalidate_all entries=%zu\n", N);
        fflush(stdout);
    }
}

void FlushDirtyRecipeSlotsToGPU() {
    if (!g_denseRecipeAnyDirty || g_recipeSSBO == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    const size_t N = g_denseRecipes.size();
    for (size_t vn = 0; vn < N; ++vn) {
        if (!g_denseRecipeDirty[vn]) continue;
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(vn * sizeof(TerrainQuadRecipe)),
                        sizeof(TerrainQuadRecipe),
                        &g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = false;
    }
    g_denseRecipeAnyDirty = false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ---------------------------------------------------------------------------
// Stage 2 parity body
// ---------------------------------------------------------------------------
//
// Walks the live quadList, looks up the dense recipe by top-left vertexNum,
// and byte-compares recipe input fields against the legacy-equivalent values
// derived directly from the live quad vertex pointers.
//
// Skip set (mirrors water_ssbo_pattern.md):
//   - null pointer guards on q.vertices[0..3] and ->pVertex
//   - blank-vertex skip: any vertexNum < 0
//   - recipe coverage gate: RecipeForVertexNum returns nullptr → skip
//
// Compared fields: corner positions (wx*/wy*/wz*), corner normals (nx*/ny*/nz*),
// UV extents (minU/minV/maxU/maxV), _wp0 terrainType-pack bits.
// NOT compared: _wp1.._wp3, _np0.._np3 (always 0.f padding),
//               post-projection data (per water_ssbo_pattern.md — sub-ULP drift).
//
int ParityCompareRecipeFrame() {
    if (!IsParityCheckEnabled()) return 0;
    if (!g_recipeReady || g_denseRecipes.empty()) return 0;
    if (!land) return 0;

    const long total            = land->getNumQuads();
    const TerrainQuadPtr quads  = land->getQuadList();
    if (!quads || total <= 0) return 0;

    static long long s_parityFrameIdx = 0;
    const long long frame = ++s_parityFrameIdx;

    const long  mapSide  = Terrain::realVerticesMapSide;
    const long  halfSide = Terrain::halfVerticesMapSide;
    const float wupv     = Terrain::worldUnitsPerVertex;

    int quadsChecked = 0;

    for (long qi = 0; qi < total; ++qi) {
        const TerrainQuad& q = quads[qi];

        // Null-pointer guards
        if (!q.vertices[0] || !q.vertices[1] || !q.vertices[2] || !q.vertices[3]) continue;
        if (!q.vertices[0]->pVertex || !q.vertices[1]->pVertex ||
            !q.vertices[2]->pVertex || !q.vertices[3]->pVertex) continue;

        // Blank-vertex skip
        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;

        const int32_t vn0 = (int32_t)q.vertices[0]->vertexNum;

        // Recipe coverage gate
        const TerrainQuadRecipe* rec = RecipeForVertexNum(vn0);
        if (!rec) continue;

        ++quadsChecked;

        // Derive expected corner positions from map index arithmetic
        const long  mx = vn0 % mapSide;
        const long  my = vn0 / mapSide;
        const float e_wx0 = float(mx     - halfSide) * wupv;
        const float e_wy0 = float(halfSide - my    ) * wupv;
        const float e_wx1 = float(mx + 1 - halfSide) * wupv;
        const float e_wy1 = e_wy0;
        const float e_wx2 = e_wx1;
        const float e_wy2 = float(halfSide - (my + 1)) * wupv;
        const float e_wx3 = e_wx0;
        const float e_wy3 = e_wy2;

        // Expected elevations and normals from live pVertex
        const float e_wz0 = q.vertices[0]->pVertex->elevation;
        const float e_wz1 = q.vertices[1]->pVertex->elevation;
        const float e_wz2 = q.vertices[2]->pVertex->elevation;
        const float e_wz3 = q.vertices[3]->pVertex->elevation;

        const float e_nx0 = q.vertices[0]->pVertex->vertexNormal.x;
        const float e_ny0 = q.vertices[0]->pVertex->vertexNormal.y;
        const float e_nz0 = q.vertices[0]->pVertex->vertexNormal.z;
        const float e_nx1 = q.vertices[1]->pVertex->vertexNormal.x;
        const float e_ny1 = q.vertices[1]->pVertex->vertexNormal.y;
        const float e_nz1 = q.vertices[1]->pVertex->vertexNormal.z;
        const float e_nx2 = q.vertices[2]->pVertex->vertexNormal.x;
        const float e_ny2 = q.vertices[2]->pVertex->vertexNormal.y;
        const float e_nz2 = q.vertices[2]->pVertex->vertexNormal.z;
        const float e_nx3 = q.vertices[3]->pVertex->vertexNormal.x;
        const float e_ny3 = q.vertices[3]->pVertex->vertexNormal.y;
        const float e_nz3 = q.vertices[3]->pVertex->vertexNormal.z;

        // Expected UV extents: mirror buildRecipeSlot exactly — read from the
        // terrain face cache entry (NOT from q.uvData, which is only set by the
        // Shape C hot-path and stays zero for quads that bypass it).
        // This comparison is apples-to-apples: recipe was built from the cache;
        // parity derives the expected value from the same cache.
        float e_minU = 0.5f / TERRAIN_TXM_SIZE;
        float e_maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
        float e_minV = e_minU;
        float e_maxV = e_maxU;
        if (Terrain::terrainTextures2 && Terrain::mapData) {
            const MapData::WorldQuadTerrainCacheEntry* entry =
                Terrain::mapData->getTerrainFaceCacheEntry(my, mx);
            if (entry && entry->isValid()) {
                const bool isCement  = entry->isCement();
                const bool noOverlay = (entry->overlayHandle == 0xffffffffu);
                if (!(noOverlay && isCement)) {
                    e_minU = entry->uvData.minU;
                    e_minV = entry->uvData.minV;
                    e_maxU = entry->uvData.maxU;
                    e_maxV = entry->uvData.maxV;
                }
            }
        }

        // Expected _wp0 (terrainType pack)
        const uint32_t e_m0 = terrainTypeToMaterialLocal(q.vertices[0]->pVertex->terrainType);
        const uint32_t e_m1 = terrainTypeToMaterialLocal(q.vertices[1]->pVertex->terrainType);
        const uint32_t e_m2 = terrainTypeToMaterialLocal(q.vertices[2]->pVertex->terrainType);
        const uint32_t e_m3 = terrainTypeToMaterialLocal(q.vertices[3]->pVertex->terrainType);
        const uint32_t e_tpacked = e_m0 | (e_m1 << 8) | (e_m2 << 16) | (e_m3 << 24);

        uint32_t g_tpacked = 0;
        memcpy(&g_tpacked, &rec->_wp0, 4);

        // Helper: bit-cast float to uint32 for exact mismatch comparison
        // (avoids NaN != NaN false positives and keeps hex output informative).
#define FCMP(fname, got_f, exp_f) \
        do { \
            uint32_t _g = 0, _e = 0; \
            float _gf = (got_f), _ef = (exp_f); \
            memcpy(&_g, &_gf, 4); memcpy(&_e, &_ef, 4); \
            if (_g != _e) { \
                ParityPrintMismatch((int)frame, (int)qi, "recipe", 0, 0, fname, _e, _g); \
            } \
        } while(0)

        FCMP("wx0", rec->wx0, e_wx0);
        FCMP("wy0", rec->wy0, e_wy0);
        FCMP("wz0", rec->wz0, e_wz0);
        FCMP("wx1", rec->wx1, e_wx1);
        FCMP("wy1", rec->wy1, e_wy1);
        FCMP("wz1", rec->wz1, e_wz1);
        FCMP("wx2", rec->wx2, e_wx2);
        FCMP("wy2", rec->wy2, e_wy2);
        FCMP("wz2", rec->wz2, e_wz2);
        FCMP("wx3", rec->wx3, e_wx3);
        FCMP("wy3", rec->wy3, e_wy3);
        FCMP("wz3", rec->wz3, e_wz3);

        FCMP("nx0", rec->nx0, e_nx0);
        FCMP("ny0", rec->ny0, e_ny0);
        FCMP("nz0", rec->nz0, e_nz0);
        FCMP("nx1", rec->nx1, e_nx1);
        FCMP("ny1", rec->ny1, e_ny1);
        FCMP("nz1", rec->nz1, e_nz1);
        FCMP("nx2", rec->nx2, e_nx2);
        FCMP("ny2", rec->ny2, e_ny2);
        FCMP("nz2", rec->nz2, e_nz2);
        FCMP("nx3", rec->nx3, e_nx3);
        FCMP("ny3", rec->ny3, e_ny3);
        FCMP("nz3", rec->nz3, e_nz3);

        FCMP("minU", rec->minU, e_minU);
        FCMP("minV", rec->minV, e_minV);
        FCMP("maxU", rec->maxU, e_maxU);
        FCMP("maxV", rec->maxV, e_maxV);

        if (g_tpacked != e_tpacked) {
            ParityPrintMismatch((int)frame, (int)qi, "recipe", 0, 0,
                                "_wp0", e_tpacked, g_tpacked);
        }

#undef FCMP
    }

    return quadsChecked;
}

}  // namespace gos_terrain_indirect  // Stage 2 block

// ---------------------------------------------------------------------------
// Stage 3 — per-frame thin-record packer, indirect-command builder,
//            preflight arming, and DrawIndirect thin executor.
//
// Bridge function (gos_terrain_bridge_drawIndirect) lives in gameos_graphics.cpp
// where gosRenderer state is accessible.  This file owns the CPU-side logic;
// the bridge is called by DrawIndirect() after arming.
// ---------------------------------------------------------------------------

// Forward-declare the bridge (defined in gameos_graphics.cpp).
// Signature uses unsigned int (not GLuint) to match gos_terrain_bridge.h
// without pulling in GL headers there.
bool gos_terrain_bridge_drawIndirect(int cmdCount, unsigned int recipeSSBO,
                                     unsigned int thinRecordSSBO,
                                     unsigned int indirectCmdBuffer);

// Include pVertex via PostcompVertex — already available through mapdata.h.
// (terrain.h already included above, which pulls mapdata.h.)
#include "../../mclib/vertex.h"     // ScreenVertex, vertexNum
// tex_resolve — lazy per-frame memoization. Header includes txmmgr.h.
#include "../../mclib/tex_resolve_table.h"

// Include TERRAIN_DEPTH_FUDGE for the per-tri pz check.
// Defined in quad.cpp as a local constant, re-stated here.
// sync: quad.cpp:1832 uses `vertices[c]->pz + TERRAIN_DEPTH_FUDGE` with FUDGE=0.001f
#ifndef TERRAIN_DEPTH_FUDGE
static constexpr float TERRAIN_DEPTH_FUDGE = 0.001f;
#endif

namespace {

// ---------------------------------------------------------------------------
// Thin-record SSBO (triple-buffered, GPU_STREAM_DRAW).
// Mirrors the M2 thin-record ring in gos_terrain_patch_stream.cpp.
// ---------------------------------------------------------------------------
static constexpr int    kThinRingFrames    = 3;
static constexpr size_t kMaxThinRecords    = 65536u;  // ≥ max visible quads
static constexpr size_t kThinRecordBytes   = kMaxThinRecords * sizeof(TerrainQuadThinRecord);

static GLuint g_thinRecordSSBO              = 0;
static int    g_thinRingSlot                = 0;
static GLsync g_thinRingFences[kThinRingFrames] = { 0, 0, 0 };

// Indirect command buffer — driver-only, GL_DRAW_INDIRECT_BUFFER.
// Sized for 16 commands (256 B); PR1 emits exactly 1.
struct DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};
static_assert(sizeof(DrawArraysIndirectCommand) == 16,
    "DrawArraysIndirectCommand is 4 GLuints = 16 B per GL spec");

static constexpr size_t kIndirectCmdBufferBytes = 16 * sizeof(DrawArraysIndirectCommand);
static GLuint g_indirectCmdBuffer = 0;

// Per-frame arming state (reset each frame by ComputePreflight).
static bool  s_frameSolidArmed           = false;
static int   s_frameSolidPackedThinCount = 0;
static int   s_frameSolidCmdCount        = 0;

// Process-sticky hard-failure latch.
static bool  s_processArmingDisabled     = false;

// first_draw lifecycle latch (reset by ResetDenseRecipe / mission teardown).
// Declared extern in the anonymous ns of the Stage 2 block; re-stated here
// via file-scope bool below.  Use the existing s_firstDrawPrintedThisMission
// that's already declared in the Stage 2 anonymous namespace above.

// ResourcesReady lazy-alloc state.
static bool  s_resourcesAllocated = false;
static bool  s_resourcesReady     = false;

// ---------------------------------------------------------------------------
// Phase C Stage 2 — SOLID GPU compute resources.
// Lazy-allocated on first ComputeDispatch(); destroyed on ResetDenseRecipe.
// ---------------------------------------------------------------------------
static GLuint  g_solidComputeProgram    = 0;
static GLuint  g_solidCmdPatchProgram   = 0;
static GLuint  g_solidBucketHeaderSsbo  = 0;
static GLuint  g_terrainHandleLutSSBO   = 0;

// Cached uniform locations — populated once when programs are compiled.
// Per-frame varying uniforms (u_windowCount, u_terrainMVP, u_alphaOverride)
// are still uploaded every frame; only the locations themselves are cached.
static GLint g_locSolidWC  = -1;   // u_windowCount   (per-frame)
static GLint g_locSolidAO  = -1;   // u_alphaOverride (per-frame, may be absent)
static GLint g_locSolidMVP = -1;   // u_terrainMVP    (per-frame)
static GLint g_locCmdVPE   = -1;   // u_vertsPerElement (constant, uploaded once)
static GLint g_locCmdCC    = -1;   // u_cmdCount        (constant, uploaded once)


// Flag: whether ComputeDispatch() ran the GPU path this frame.
static bool s_solidGpuDispatchRanThisFrame = false;

// Phase C Stage 2 parity counters — gated by gpu_driven::IsParityEnabled().
static uint64_t s_solidParityFrames     = 0;
static uint64_t s_solidParityQuads      = 0;
static uint64_t s_solidParityMismatches = 0;

// ---------------------------------------------------------------------------
// ResourcesReady() — lazy-allocate thin-record SSBO + indirect buffer.
// Called from ComputePreflight; returns true once both are allocated.
// ---------------------------------------------------------------------------
static bool ResourcesReady() {
    if (s_resourcesReady) return true;
    if (s_resourcesAllocated) return false;  // already tried and failed

    // Thin-record SSBO: triple-buffered, GL_STREAM_DRAW.
    if (g_thinRecordSSBO == 0) {
        glGenBuffers(1, &g_thinRecordSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(kThinRingFrames * kThinRecordBytes),
                     nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (g_thinRecordSSBO == 0) {
            if (traceOn()) {
                fprintf(stderr, "[TERRAIN_INDIRECT v1] event=resources_alloc_fail reason=thin_ssbo\n");
                fflush(stderr);
            }
            s_resourcesAllocated = true;
            return false;
        }
    }

    // Indirect command buffer.
    if (g_indirectCmdBuffer == 0) {
        glGenBuffers(1, &g_indirectCmdBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glBufferData(GL_DRAW_INDIRECT_BUFFER,
                     (GLsizeiptr)kIndirectCmdBufferBytes,
                     nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        if (g_indirectCmdBuffer == 0) {
            if (traceOn()) {
                fprintf(stderr, "[TERRAIN_INDIRECT v1] event=resources_alloc_fail reason=indirect_buf\n");
                fflush(stderr);
            }
            s_resourcesAllocated = true;
            return false;
        }
    }

    // Atlas guard: refuse to arm if BuildColormapAtlas didn't produce a texture.
    // This keeps ResourcesReady() idempotent-false until BuildDenseRecipe has run
    // (and thus BuildColormapAtlas has run). Without this, ComputePreflight could
    // arm and the bridge would draw with no texture bound — reproducing the grey bug.
    if (g_atlasGLTex == 0) {
        if (traceOn()) {
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=resources_not_ready reason=atlas_not_built\n");
            fflush(stderr);
        }
        // Don't mark s_resourcesAllocated — retry next frame once atlas is ready.
        return false;
    }

    s_resourcesAllocated = true;
    s_resourcesReady     = true;
    if (traceOn()) {
        fprintf(stderr,
            "[TERRAIN_INDIRECT v1] event=resources_ready "
            "thinSSBO=%u indirectBuf=%u ringFrames=%d maxThin=%zu atlasGLTex=%u\n",
            (unsigned)g_thinRecordSSBO, (unsigned)g_indirectCmdBuffer,
            kThinRingFrames, kMaxThinRecords, (unsigned)g_atlasGLTex);
        fflush(stderr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// InMissionTransition() — simple stub; returns false until needed.
// Stage 4's quintuple soak gate validates this boundary.
// ---------------------------------------------------------------------------
static inline bool InMissionTransition() { return false; }

// ---------------------------------------------------------------------------
// PackThinRecordsForFrame() — walks live quadList, packs thin-record SSBO
// ring slot for this frame.  Returns count of packed quads (≥ 0).
//
// Skip set (per memory/water_ssbo_pattern.md):
//   1. Pointer guards: q.vertices[0..3] and ->pVertex null check
//   2. Map-edge blank-vertex: vertexNum < 0 sentinel
//   3. Recipe coverage: RecipeForVertexNum returns nullptr
//   4. Per-tri pz check (drives flags bits 1, 2)
// ---------------------------------------------------------------------------
static int PackThinRecordsForFrame() {
    ZoneScopedN("Terrain::ThinRecordPack");

    // Diagnostic Test 1 — reset per-frame cement classification counters.
    g_cementMappedThisFrame       = 0;
    g_concreteAllCornersThisFrame = 0;

    if (!land) return 0;
    const long total          = land->getNumQuads();
    const TerrainQuadPtr quads = land->getQuadList();
    if (!quads || total <= 0) return 0;

    // Advance ring slot — wait on the fence for this slot before overwriting.
    g_thinRingSlot = (g_thinRingSlot + 1) % kThinRingFrames;
    if (g_thinRingFences[g_thinRingSlot]) {
        glClientWaitSync(g_thinRingFences[g_thinRingSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, 10000000u /* 10ms */);
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
        g_thinRingFences[g_thinRingSlot] = 0;
    }

    // Stage area: up to kMaxThinRecords records into a stack-local shadow.
    // glBufferSubData one shot at the end.
    static TerrainQuadThinRecord s_shadow[kMaxThinRecords];
    int packed = 0;

    for (long qi = 0; qi < total && (size_t)packed < kMaxThinRecords; ++qi) {
        const TerrainQuad& q = quads[qi];

        // 1. Pointer guards (pVertex not required here — we only need
        //    vertexNum + pz for the skip set; lightRGB comes from
        //    vertices[c]->lightRGB which only requires ScreenVertex).
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;

        // 2. Map-edge blank-vertex skip (vertexNum == -1 sentinel)
        const int32_t vn0 = q.vertices[0]->vertexNum;
        if (vn0 < 0 ||
            q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 ||
            q.vertices[3]->vertexNum < 0) continue;

        // 3. Recipe coverage gate
        const TerrainQuadRecipe* rec = gos_terrain_indirect::RecipeForVertexNum(vn0);
        if (!rec) continue;

        // terrainHandle: use tex_resolve for per-frame handle indirection
        // (memory/mc2_texture_handle_is_live.md — never cache raw handle).
        const uint32_t th = static_cast<uint32_t>(
            tex_resolve(static_cast<DWORD>(q.terrainHandle)));
        // Skip quads with no base terrain texture (detail-only quads).
        if (th == 0 || th == 0xffffffffu) continue;

        // 4. Per-tri pz check — mirrors quad.cpp:1836-1845 logic.
        //    vertices[c]->pz is pre-projected by vertexProjectLoop.
        bool pzc[4];
        for (int c = 0; c < 4; c++) {
            float pz_adj = q.vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
        }

        const int uvMode = q.uvMode;
        bool pzTri1, pzTri2;
        if (uvMode == 1 /*BOTTOMLEFT*/) {
            pzTri1 = pzc[0] && pzc[1] && pzc[3];
            pzTri2 = pzc[1] && pzc[2] && pzc[3];
        } else {
            // BOTTOMRIGHT / TOPRIGHT diagonal
            pzTri1 = pzc[0] && pzc[1] && pzc[2];
            pzTri2 = pzc[0] && pzc[2] && pzc[3];
        }
        if (!pzTri1 && !pzTri2) continue;  // both culled

        // lightRGB — mirrors quad.cpp:1860-1865 (lightRGBc lambda).
        // alphaOverride (whitens lighting when terrainTextures2 is active
        // and not pure-cement-with-no-overlay) is baked into the existing
        // M2 thin path.  We mirror the exact same logic here.
        // The recipe already carries terrainHandle; we just need per-corner light.
        // Judgment call: use the same simplified lightRGBc pattern the M2 thin
        // path uses — terrainTextures2 check for alphaOverride, selection check.
        const bool alphaOverride = (Terrain::terrainTextures2 != nullptr);
        auto lightRGBc = [&](int c) -> uint32_t {
            DWORD lc = q.vertices[c]->lightRGB;
            if (alphaOverride) lc = 0xffffffffu;
            if (q.vertices[c]->pVertex && q.vertices[c]->pVertex->selected)
                lc = static_cast<DWORD>(0xffff7fffu /*SELECTION_COLOR*/);
            return static_cast<uint32_t>(lc);
        };

        TerrainQuadThinRecord& tr = s_shadow[packed];
        tr.recipeIdx     = static_cast<uint32_t>(vn0);
        tr.terrainHandle = th;
        tr.flags         = static_cast<uint32_t>((uvMode == 1 ? 1u : 0u)
                         | (pzTri1 ? 2u : 0u)
                         | (pzTri2 ? 4u : 0u));
        // Cement layer-index lookup, keyed by textures[] SLOT (stable across
        // frames).  nodeIdx (q.terrainHandle) mutates per-frame per
        // memory/mc2_texture_handle_is_live.md, so a nodeIdx-keyed lookup
        // intermittently misses → cement validity bit flickers → visible
        // concrete flicker on indirect base + perceived flicker on alpha-cement
        // overlay composite.  Slot is allocated once by initTexture and stable
        // for the mission lifetime.  Re-derive the slot at packer time from
        // pVertex->textureData & 0xFFFFu (the same expression quad.cpp:546
        // passes to getTextureHandle).
        //
        // Encoding (V23, widened in V27):
        //   bit 31     = CEMENT_LAYER_VALID — disambiguates "layer 0" from "not cement"
        //   bits 30:16 = reserved for future layers (decals, scorch)
        //   bits 15:0  = cement atlas layer index (0..65535 encoding cap;
        //                practically capped at 1024 by atlas budget — see
        //                BuildCementCatalogAtlas)
        constexpr uint32_t kCementLayerValidBit = 0x80000000u;
        uint32_t cementWord = 0u;
        uint16_t idx = 0xFFFFu;
        if (g_cementLayerMapReady && q.vertices[0] && q.vertices[0]->pVertex) {
            const DWORD slot = q.vertices[0]->pVertex->textureData & 0xFFFFu;
            if (slot < (DWORD)MC_MAX_TERRAIN_TXMS) {
                idx = g_cementLayerIndexBySlot[slot];
                if (idx != 0xFFFFu) {
                    cementWord = kCementLayerValidBit | ((uint32_t)idx & 0xFFFFu);
                    ++g_cementMappedThisFrame;
                }
            }
            // Lifecycle counter: count quads that EXPECT a cement layer
            // (all 4 corner materials in recipe._wp0 == Concrete=3),
            // independent of whether the layer lookup succeeded.
            if (rec) {
                uint32_t tpacked = 0u;
                memcpy(&tpacked, &rec->_wp0, 4);
                const bool allConcrete =
                    ((tpacked        & 0xFFu) == 3u) &&
                    (((tpacked >> 8) & 0xFFu) == 3u) &&
                    (((tpacked >>16) & 0xFFu) == 3u) &&
                    (((tpacked >>24) & 0xFFu) == 3u);
                if (allConcrete) {
                    ++g_concreteAllCornersThisFrame;
                    if (idx == 0xFFFFu) ++g_cementPackUnmappedCount;
                }
            }
        }
        // Old nodeIdx-based lookup — kept as dead code for one commit; the
        // slot-keyed lookup above is what drives correctness now.  Slot is
        // stable across frames; nodeIdx is not.
        // const DWORD nodeIdx = (DWORD)q.terrainHandle;
        // if (nodeIdx < (DWORD)MC_MAXTEXTURES) {
        //     const uint16_t idx2 = g_cementLayerIndexByNodeIdx[nodeIdx];
        //     ...
        // }
        tr._pad0         = cementWord;
        tr.lightRGB0     = lightRGBc(0);
        tr.lightRGB1     = lightRGBc(1);
        tr.lightRGB2     = lightRGBc(2);
        tr.lightRGB3     = lightRGBc(3);

        gos_terrain_indirect::Counters_AddIndirectSolidPackedQuad();
        ++packed;
    }

    if (packed == 0) return 0;

    // Upload to the current ring slot.
    const GLintptr slotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                    slotOffset,
                    (GLsizeiptr)(packed * sizeof(TerrainQuadThinRecord)),
                    s_shadow);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Diagnostic Test 1 — per-frame cement classification trace (1/sec at 60fps).
    {
        static uint32_t s_frameCount = 0;
        ++s_frameCount;
        if (traceOn() && (s_frameCount % 60u == 0u)) {
            printf("[CEMENT_FRAME v1] frame=%u packed_total=%u cement_mapped=%u concrete_all_corners=%u\n",
                   (unsigned)s_frameCount, (unsigned)packed,
                   (unsigned)g_cementMappedThisFrame,
                   (unsigned)g_concreteAllCornersThisFrame);
            fflush(stdout);
        }
    }

    return packed;
}

// ---------------------------------------------------------------------------
// BuildIndirectCommands() — builds 1 DrawArraysIndirectCommand (PR1 SOLID-only).
// Returns 1 on success, 0 on error.
// ---------------------------------------------------------------------------
static int BuildIndirectCommands(int thinCount) {
    if (thinCount <= 0) return 0;

    DrawArraysIndirectCommand cmd{};
    cmd.count         = static_cast<GLuint>(thinCount * 6);
    cmd.instanceCount = 1u;
    cmd.first         = 0u;
    cmd.baseInstance  = 0u;

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                    (GLsizeiptr)sizeof(cmd), &cmd);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    return 1;
}

// ---------------------------------------------------------------------------
// UploadTerrainHandleLUT — Phase C Stage 2 (GPU-direct replacement for
// BuildSolidQuadWindowSSBO). Builds a uint[MC_MAXTEXTURES] LUT mapping
// nodeId → resolved gosHandle (tex_resolve result), then uploads it as a
// 16KB SSBO on binding 2. The GPU shader reads nodeId from recipe._wp2 and
// indexes into this LUT — eliminating the per-frame quadList CPU walk.
// Returns g_denseRecipes.size() as the GPU dispatch count (full recipe range).
// ---------------------------------------------------------------------------
static uint32_t UploadTerrainHandleLUT() {
    constexpr GLsizeiptr kLutBytes = MC_MAXTEXTURES * sizeof(uint32_t);

    if (g_terrainHandleLutSSBO == 0) {
        glGenBuffers(1, &g_terrainHandleLutSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_terrainHandleLutSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, kLutBytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Resolve unique nodeIds from the mission-static set — typically 20-50
    // entries regardless of camera position. O(uniqueTextures), not O(quads).
    static uint32_t lut[MC_MAXTEXTURES];
    memset(lut, 0, sizeof(lut));
    for (uint32_t nodeId : g_uniqueTerrainNodeIds) {
        if (nodeId > 0u && nodeId < (uint32_t)MC_MAXTEXTURES) {
            const uint32_t th = static_cast<uint32_t>(tex_resolve((DWORD)nodeId));
            lut[nodeId] = (th == 0u || th == 0xffffffffu) ? 0u : th;
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_terrainHandleLutSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kLutBytes, lut);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    return (uint32_t)g_denseRecipes.size();
}

}  // anonymous namespace (Stage 3 helpers)

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Stage 3 public API
// ---------------------------------------------------------------------------

bool IsFrameSolidArmed() {
    return s_frameSolidArmed && !s_processArmingDisabled;
}

void ForceDisableArmingForProcess() {
    s_processArmingDisabled = true;
}

void BeginFrame() {
    // Reset armed flag unconditionally every frame so mech-bay / menu frames
    // don't inherit the armed state from the last gameplay frame.
    // ComputePreflight() re-arms when terrain is actually present.
    static int s_beginFrameCount = 0;
    ++s_beginFrameCount;
    if (s_solidGpuDispatchRanThisFrame && !s_frameSolidArmed) {
        // Dispatch ran this frame but arming is already false — double BeginFrame call.
        fprintf(stderr, "[TERRAIN_INDIRECT v1] event=double_beginframe count=%d\n",
                s_beginFrameCount);
        fflush(stderr);
    }
    s_frameSolidArmed = false;
}

bool ComputePreflight() {
    ZoneScopedN("Terrain::IndirectPreflight");
    s_frameSolidArmed                = false;
    s_frameSolidPackedThinCount      = 0;
    s_frameSolidCmdCount             = 0;
    s_solidGpuDispatchRanThisFrame   = false;

    if (s_processArmingDisabled) return false;
    if (!IsEnabled())             return false;
    if (!IsDenseRecipeReady())    return false;
    if (!ResourcesReady())        return false;
    if (InMissionTransition())    return false;

    FlushDirtyRecipeSlotsToGPU();

    if (gpu_driven::IsTerrainSolidEnabled()) {
        // GPU path: arm immediately. ComputeDispatch() (called after Phase 1
        // PackAndDispatch) will build the window SSBO and issue the two-dispatch
        // sequence. cmd-patch shader writes the actual vert count into the indirect
        // cmd buffer; DrawIndirect() fires glMultiDrawArraysIndirect with cmdCount=1.
        s_frameSolidPackedThinCount = -1;   // sentinel: GPU path (logged only)
        s_frameSolidCmdCount        = 1;    // PR1: always 1 indirect draw command
        s_frameSolidArmed           = true;
        return true;
    }

    // CPU path (legacy — demote-don't-delete per CLAUDE.md debug instrumentation rule).
    const int thinCount = PackThinRecordsForFrame();
    if (thinCount == 0) {
        if (traceOn())
            printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_thin\n");
        return false;
    }

    const int cmdCount = BuildIndirectCommands(thinCount);
    if (cmdCount == 0) {
        if (traceOn())
            printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_cmd\n");
        return false;
    }

    s_frameSolidPackedThinCount = thinCount;
    s_frameSolidCmdCount        = cmdCount;
    s_frameSolidArmed           = true;
    return true;
}

void ComputeDispatch() {
    // Phase C Stage 2. Called AFTER gos_terrain_lighting::PackAndDispatch() so
    // Phase 1's lighting SSBO is populated with same-frame data.
    if (!s_frameSolidArmed)                    return;
    if (!gpu_driven::IsTerrainSolidEnabled())  return;

    ZoneScopedN("Terrain::SolidComputeDispatch");

    // Guard: Phase 1 lighting SSBO must be ready.
    const GLuint lightSsbo = gos_terrain_lighting::GetOutputSsbo();
    if (lightSsbo == 0) {
        static bool s_warnedLight = false;
        if (!s_warnedLight) {
            printf("[TERRAIN_INDIRECT v1] event=warn msg=solid_lighting_ssbo_not_ready\n");
            fflush(stdout);
            s_warnedLight = true;
        }
        return;
    }

    // Lazy-build compute programs and cache uniform locations + constant uniforms.
    if (g_solidComputeProgram == 0) {
        g_solidComputeProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_terrain_solid.comp", nullptr, 0, "gpu_driven_terrain_solid");
        if (g_solidComputeProgram == 0) {
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_compile_failed\n");
            fflush(stderr);
            ForceDisableArmingForProcess();
            return;
        }
        g_locSolidWC  = glGetUniformLocation(g_solidComputeProgram, "u_windowCount");
        g_locSolidAO  = glGetUniformLocation(g_solidComputeProgram, "u_alphaOverride");
        g_locSolidMVP = glGetUniformLocation(g_solidComputeProgram, "u_terrainMVP");
        // Upload constants once — these never change across frames.
        glUseProgram(g_solidComputeProgram);
        const GLint locMTR = glGetUniformLocation(g_solidComputeProgram, "u_maxThinRecords");
        const GLint locMS  = glGetUniformLocation(g_solidComputeProgram, "u_mapSide");
        if (locMTR >= 0) glUniform1i(locMTR, (int)kMaxThinRecords);
        if (locMS  >= 0) glUniform1i(locMS,  (int)Terrain::realVerticesMapSide);
        glUseProgram(0);
    }
    if (g_solidCmdPatchProgram == 0) {
        g_solidCmdPatchProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_cmd_patch.comp", nullptr, 0, "gpu_driven_cmd_patch_solid");
        if (g_solidCmdPatchProgram == 0) {
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=error msg=solid_cmd_patch_compile_failed\n");
            fflush(stderr);
            ForceDisableArmingForProcess();
            return;
        }
        g_locCmdVPE = glGetUniformLocation(g_solidCmdPatchProgram, "u_vertsPerElement");
        g_locCmdCC  = glGetUniformLocation(g_solidCmdPatchProgram, "u_cmdCount");
        // Upload constants once — 6 verts/element and 1 indirect cmd never change.
        glUseProgram(g_solidCmdPatchProgram);
        if (g_locCmdVPE >= 0) glUniform1i(g_locCmdVPE, 6);
        if (g_locCmdCC  >= 0) glUniform1i(g_locCmdCC,  1);
        const GLint locMT2 = glGetUniformLocation(g_solidCmdPatchProgram, "u_maxThinRecords");
        if (locMT2 >= 0) glUniform1i(locMT2, (int)kMaxThinRecords);
        glUseProgram(0);
    }
    // Lazy-allocate bucket header SSBO (16 B GpuDrivenBucketHeader).
    if (g_solidBucketHeaderSsbo == 0) {
        glGenBuffers(1, &g_solidBucketHeaderSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Upload per-frame terrain handle LUT (nodeId → gosHandle, ~20-50 entries).
    // GPU dispatches over full recipe range and culls edge/invalid quads itself.
    const uint32_t windowCount = UploadTerrainHandleLUT();
    if (windowCount == 0) {
        // Zero visible quads this frame; leave armed — DrawIndirect fires with
        // cmd.count=0 (after bucket-clear + cmd-patch). No visual artifact.
        // Still need to run both dispatches to keep cmd buffer in defined state.
    }

    // Advance the thin-record ring slot (same ring as CPU path).
    g_thinRingSlot = (g_thinRingSlot + 1) % kThinRingFrames;
    if (g_thinRingFences[g_thinRingSlot]) {
        glClientWaitSync(g_thinRingFences[g_thinRingSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, 10000000u /*10ms*/);
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
        g_thinRingFences[g_thinRingSlot] = 0;
    }
    const GLintptr thinSlotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);

    // Zero bucket header visibleCount before dispatch.
    {
        const uint32_t zero = 0u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, 16,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // ------------------------------------------------------------------
    // DISPATCH 1: cull/pack (gpu_driven_terrain_solid.comp)
    // Bindings: 0=recipe, 1=lighting, 2=terrainHandleLut, 3=thin, 6=header
    // ------------------------------------------------------------------
    glUseProgram(g_solidComputeProgram);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_recipeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, lightSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2,
                     g_terrainHandleLutSSBO ? g_terrainHandleLutSSBO : 0);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 3,
                      g_thinRecordSSBO, thinSlotOffset, (GLsizeiptr)kThinRecordBytes);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g_solidBucketHeaderSsbo);

    if (g_locSolidWC < 0 || g_locSolidMVP < 0) {
        fprintf(stderr,
            "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_missing_uniform "
            "u_windowCount=%d u_terrainMVP=%d\n",
            g_locSolidWC, g_locSolidMVP);
        fflush(stderr);
        glUseProgram(0);
        ForceDisableArmingForProcess();
        return;
    }

    glUniform1i(g_locSolidWC, (int)windowCount);
    if (g_locSolidAO >= 0)
        glUniform1i(g_locSolidAO, Terrain::terrainTextures2 != nullptr ? 1 : 0);

    {
        const float* mvp = gos_GetTerrainMVPMat4();
        if (!mvp) { glUseProgram(0); return; }
        glUniformMatrix4fv(g_locSolidMVP, 1, GL_FALSE, mvp);
    }

    const uint32_t groups = (windowCount + 63u) / 64u;
    if (groups > 0) glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // ------------------------------------------------------------------
    // DISPATCH 2: cmd-patch (gpu_driven_cmd_patch.comp)
    // Reads visibleCount from header → writes cmd.count to g_indirectCmdBuffer.
    // Constants (u_vertsPerElement, u_cmdCount, u_maxThinRecords) were uploaded
    // once at compile time and are not re-sent here.
    // ------------------------------------------------------------------
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_solidBucketHeaderSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_indirectCmdBuffer);

    glUseProgram(g_solidCmdPatchProgram);
    glDispatchCompute(1, 1, 1);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    glUseProgram(0);

    // Restore compute-only slots to clean state.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    // Leave slot 3 as the thin-record range (DrawIndirect's bridge reads it via
    // gos_terrain_bridge_drawIndirect which re-binds by recipeSSBO/thinRecordSSBO
    // arguments — no leak risk).

    s_solidGpuDispatchRanThisFrame = true;
}

bool DrawIndirect() {
    if (!IsFrameSolidArmed()) {
        static bool s_warnedOnce = false;
        if (!s_warnedOnce) {
            s_warnedOnce = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=draw_indirect_skip "
                   "armed=%d process_disabled=%d\n",
                   (int)s_frameSolidArmed, (int)s_processArmingDisabled);
            fflush(stderr);
        }
        return false;
    }
    ZoneScopedN("Terrain::SolidDrawIndirect");
    TracyGpuZone("Terrain::SolidDrawIndirect");

    const bool ok = gos_terrain_bridge_drawIndirect(
        s_frameSolidCmdCount,
        static_cast<unsigned int>(g_recipeSSBO),
        static_cast<unsigned int>(g_thinRecordSSBO),
        static_cast<unsigned int>(g_indirectCmdBuffer));

    if (!ok) {
        // Hard failure post-arming: gate-off already fired, cannot recover.
        static bool s_hardFailureLogged = false;
        if (!s_hardFailureLogged) {
            s_hardFailureLogged = true;
            fprintf(stderr,
                "[TERRAIN_INDIRECT v1] event=hard_failure reason=bridge_returned_false "
                "thin_count=%d cmd_count=%d "
                "advice=set MC2_TERRAIN_INDIRECT=0 to fall back to M2 legacy SOLID\n",
                s_frameSolidPackedThinCount, s_frameSolidCmdCount);
            fflush(stderr);
        }
        ForceDisableArmingForProcess();
        return false;
    }

    // first_draw lifecycle print — once per mission via the mission-latch.
    // s_firstDrawPrintedThisMission is declared in the Stage 2 anonymous ns;
    // it's reset by ResetDenseRecipe() at mission teardown.
    if (!s_firstDrawPrintedThisMission && traceOn()) {
        s_firstDrawPrintedThisMission = true;
        printf("[TERRAIN_INDIRECT v1] event=first_draw "
               "thin_count=%d cmd_count=%d ring_slot=%d\n",
               s_frameSolidPackedThinCount, s_frameSolidCmdCount, g_thinRingSlot);
        fflush(stdout);
    }

    // Ring fence for the slot just drawn.
    if (g_thinRingFences[g_thinRingSlot]) {
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
    }
    g_thinRingFences[g_thinRingSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    return true;
}

// ---------------------------------------------------------------------------
// ComputeDispatchParity_Check() — gated by MC2_GPU_DRIVEN_PARITY.
//
// Dual-run pattern (mirrors gos_terrain_water_stream.cpp:1364):
//   1. Read GPU thin records from g_thinRecordSSBO at the ring slot that
//      ComputeDispatch() wrote to.
//   2. Re-run PackThinRecordsForFrame() into the NEXT ring slot (CPU path).
//   3. Sort both sets by recipeIdx (atomicAdd order is non-deterministic).
//   4. Compare field-by-field: terrainHandle, flags, cementWord, lightRGB0..3.
//
// Call site: txmmgr.cpp immediately after DrawIndirect() (before GL state
// restore), so the compute output is already barrier-complete.
// ---------------------------------------------------------------------------
void ComputeDispatchParity_Check() {
    if (!gpu_driven::IsParityEnabled()) return;
    if (!s_solidGpuDispatchRanThisFrame) return;
    if (!g_thinRecordSSBO || !g_solidBucketHeaderSsbo) return;

    ++s_solidParityFrames;

    // Need GL_BUFFER_UPDATE_BARRIER_BIT in addition to the SSB barrier that
    // ComputeDispatch already issued, so glGetBufferSubData sees the writes.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

    // 1. GPU visible count from the bucket header.
    const int gpuRingSlot = g_thinRingSlot;
    gpu_driven::GpuDrivenBucketHeader hdr{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                       (GLsizeiptr)sizeof(hdr), &hdr);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    const uint32_t gpuCount = std::min(hdr.visibleCount, (uint32_t)kMaxThinRecords);

    // 2. Read GPU thin records from the slot compute wrote.
    std::vector<TerrainQuadThinRecord> gpuRecs(gpuCount);
    if (gpuCount > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           (GLintptr)((size_t)gpuRingSlot * kThinRecordBytes),
                           (GLsizeiptr)(gpuCount * sizeof(TerrainQuadThinRecord)),
                           gpuRecs.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // 3. CPU thin-record pack advances the ring to the NEXT slot.
    //    GPU output at gpuRingSlot remains intact (no fence wait for that slot here).
    const int cpuThinCount = PackThinRecordsForFrame();
    const int cpuRingSlot  = g_thinRingSlot;

    // 4. Read CPU thin records.
    std::vector<TerrainQuadThinRecord> cpuRecs;
    if (cpuThinCount > 0) {
        cpuRecs.resize((size_t)cpuThinCount);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           (GLintptr)((size_t)cpuRingSlot * kThinRecordBytes),
                           (GLsizeiptr)((size_t)cpuThinCount * sizeof(TerrainQuadThinRecord)),
                           cpuRecs.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    const uint32_t cpuCount = (uint32_t)cpuRecs.size();

    // 5. Sort by recipeIdx — GPU atomicAdd output order is non-deterministic.
    auto byRecipeIdx = [](const TerrainQuadThinRecord& a, const TerrainQuadThinRecord& b) {
        return a.recipeIdx < b.recipeIdx;
    };
    std::sort(gpuRecs.begin(), gpuRecs.end(), byRecipeIdx);
    std::sort(cpuRecs.begin(), cpuRecs.end(), byRecipeIdx);

    // 6. Walk merged sorted lists and compare field-by-field.
    constexpr uint32_t kMaxPrintsPerFrame = 16;
    uint32_t printed = 0;
    uint32_t quadsChecked = 0;
    uint32_t frameMismatches = 0;
    uint32_t gi = 0, ci = 0;

    while (gi < gpuCount || ci < cpuCount) {
        const uint32_t gpuIdx = (gi < gpuCount) ? gpuRecs[gi].recipeIdx : UINT32_MAX;
        const uint32_t cpuIdx = (ci < cpuCount) ? cpuRecs[ci].recipeIdx : UINT32_MAX;

        if (gpuIdx < cpuIdx) {
            ++frameMismatches;
            if (printed < kMaxPrintsPerFrame) {
                ++printed;
                fprintf(stderr,
                        "[GPU_DRIVEN_SOLID_PARITY v1] event=mismatch "
                        "frame=%llu recipe=%u gpu_only=1\n",
                        (unsigned long long)s_solidParityFrames, (unsigned)gpuIdx);
                fflush(stderr);
            }
            ++gi;
        } else if (cpuIdx < gpuIdx) {
            ++frameMismatches;
            if (printed < kMaxPrintsPerFrame) {
                ++printed;
                fprintf(stderr,
                        "[GPU_DRIVEN_SOLID_PARITY v1] event=mismatch "
                        "frame=%llu recipe=%u cpu_only=1\n",
                        (unsigned long long)s_solidParityFrames, (unsigned)cpuIdx);
                fflush(stderr);
            }
            ++ci;
        } else {
            const TerrainQuadThinRecord& g = gpuRecs[gi];
            const TerrainQuadThinRecord& c = cpuRecs[ci];
            ++quadsChecked;

#define SOLID_PARITY_FIELD1(field) \
    if (g.field != c.field) { \
        ++frameMismatches; \
        if (printed < kMaxPrintsPerFrame) { \
            ++printed; \
            fprintf(stderr, \
                    "[GPU_DRIVEN_SOLID_PARITY v1] event=mismatch " \
                    "frame=%llu recipe=%u field=" #field " gpu=0x%x cpu=0x%x\n", \
                    (unsigned long long)s_solidParityFrames, (unsigned)gpuIdx, \
                    (unsigned)g.field, (unsigned)c.field); \
            fflush(stderr); \
        } \
    }

            SOLID_PARITY_FIELD1(terrainHandle)
            SOLID_PARITY_FIELD1(flags)
            SOLID_PARITY_FIELD1(_pad0)  // cementWord in GLSL; _pad0 in C++ struct
#undef SOLID_PARITY_FIELD1

            if (g.lightRGB0 != c.lightRGB0 || g.lightRGB1 != c.lightRGB1 ||
                g.lightRGB2 != c.lightRGB2 || g.lightRGB3 != c.lightRGB3) {
                ++frameMismatches;
                if (printed < kMaxPrintsPerFrame) {
                    ++printed;
                    fprintf(stderr,
                            "[GPU_DRIVEN_SOLID_PARITY v1] event=mismatch "
                            "frame=%llu recipe=%u field=lightRGB "
                            "gpu=[0x%x,0x%x,0x%x,0x%x] cpu=[0x%x,0x%x,0x%x,0x%x]\n",
                            (unsigned long long)s_solidParityFrames, (unsigned)gpuIdx,
                            (unsigned)g.lightRGB0, (unsigned)g.lightRGB1,
                            (unsigned)g.lightRGB2, (unsigned)g.lightRGB3,
                            (unsigned)c.lightRGB0, (unsigned)c.lightRGB1,
                            (unsigned)c.lightRGB2, (unsigned)c.lightRGB3);
                    fflush(stderr);
                }
            }
            ++gi; ++ci;
        }
    }

    s_solidParityQuads      += quadsChecked;
    s_solidParityMismatches += frameMismatches;

    if (s_solidParityFrames % 600 == 0) {
        fprintf(stderr,
                "[GPU_DRIVEN_SOLID_PARITY v1] event=summary "
                "frames=%llu quads_checked=%llu total_mismatches=%llu\n",
                (unsigned long long)s_solidParityFrames,
                (unsigned long long)s_solidParityQuads,
                (unsigned long long)s_solidParityMismatches);
        fflush(stderr);
    }
}

}  // namespace gos_terrain_indirect  // Stage 3 block

// Bridge accessor for the current thin-record ring slot (used by
// gos_terrain_bridge_drawIndirect to compute glBindBufferRange offset).
int gos_terrain_indirect_getRingSlot() {
    return g_thinRingSlot;
}

// ---------------------------------------------------------------------------
// PR2c Stage 1c — mine static-bake infrastructure.
//
// Builds a per-mission mine VBO + 2-layer mine/blown texture-array. Both are
// populated lazily on first dirty event (i.e., first MissionMap::setMine
// call after Reset, including the per-cell init-time setMine loop). Per spec
// at 2026-05-08-pr2c-mine-static-bake-design.md.
//
// Stage 1c is build-and-invalidate scaffolding only — RebuildMineStaticVBOIfDirty
// is callable but not yet wired into per-frame from the bridge. Stage 2c
// adds the bridge + draw + legacy gate-off (single-PR per N2 partial-landing
// rule).
// ---------------------------------------------------------------------------

namespace {

GLuint g_mineStaticVBO_GL         = 0;
int    g_mineVertCount            = 0;
bool   g_mineVBODirty             = true;   // first build triggers via setMine init loop
bool   g_mineVBOFirstBuildPending = true;   // gates lazy texture-array build (R7)
GLuint g_mineTextureArrayGL       = 0;
bool   g_mineTextureArrayReady    = false;

// Vertex format: 3 float pos (world space) + 2 float uv (sprite 0..1) +
// 1 uint layer (0=mine, 1=blown). 24 bytes/vert; 6 verts/cell (2 tris).
struct MineVert {
    float    pos[3];
    float    uv[2];
    uint32_t layer;
};

}  // namespace

namespace gos_terrain_indirect {

void MarkMineDirty() {
    g_mineVBODirty = true;
}

void ResetMineStaticVBO() {
    g_mineVertCount            = 0;
    g_mineVBODirty             = true;
    g_mineVBOFirstBuildPending = true;
    // Keep g_mineStaticVBO_GL allocation across missions; reused by next
    // BuildMineStaticVBO via glBufferData (which orphans + reallocates the
    // backing store).
}

void ResetMineTextureArray() {
    g_mineTextureArrayReady = false;
    // Keep g_mineTextureArrayGL allocation across missions. Source TGAs
    // (defaults/mine_00.tga, defaults/minescorch_00.tga) are process-stable;
    // first BuildMineTextureArray call after reset re-marks ready without
    // re-allocating the GL texture.
}

void BuildMineTextureArray() {
    // Texture-array contents are process-stable; re-read only if first build
    // or invalidated.
    if (g_mineTextureArrayReady) return;

    // R7: handles must be loaded by setupTextures before this fires. If still
    // 0xffffffff, bail — the next dirty event retries. (Stage 2c invokes
    // RebuildMineStaticVBOIfDirty post first-paint via the bridge; setupTextures
    // runs per-quad each frame and lazy-loads the handles at quad.cpp:520-531
    // before any per-frame mine work happens.)
    const DWORD mineSlot  = TerrainQuad::mineTextureHandle;
    const DWORD blownSlot = TerrainQuad::blownTextureHandle;
    if (mineSlot == 0xffffffffu || blownSlot == 0xffffffffu) {
        if (IsTraceEnabled()) {
            printf("[TERRAIN_INDIRECT v1] event=mine_atlas_skip "
                   "reason=handles_not_loaded mine=%u blown=%u\n",
                   (unsigned)mineSlot, (unsigned)blownSlot);
            fflush(stdout);
        }
        return;
    }

    const DWORD mineGosHandle  = tex_resolve(mineSlot);
    const DWORD blownGosHandle = tex_resolve(blownSlot);
    if (mineGosHandle == 0u || blownGosHandle == 0u) return;

    const GLuint mineGLTex  = gos_terrain_bridge_glTextureForGosHandle((unsigned)mineGosHandle);
    const GLuint blownGLTex = gos_terrain_bridge_glTextureForGosHandle((unsigned)blownGosHandle);
    if (mineGLTex == 0 || blownGLTex == 0) return;

    constexpr int kW = 16;
    constexpr int kH = 16;
    std::vector<uint32_t> mineBuf((size_t)kW * kH, 0u);
    std::vector<uint32_t> blownBuf((size_t)kW * kH, 0u);

    // Save GL state (mirrors PR1 BuildCementCatalogAtlas pattern).
    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glBindTexture(GL_TEXTURE_2D, mineGLTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, mineBuf.data());
    glBindTexture(GL_TEXTURE_2D, blownGLTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, blownBuf.data());

    if (g_mineTextureArrayGL == 0) glGenTextures(1, &g_mineTextureArrayGL);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_mineTextureArrayGL);
    // Allocate 2-layer storage. glTexImage3D with NULL data orphans / reserves.
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kW, kH, 2,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, /*layer=*/0, kW, kH, 1,
                    GL_BGRA, GL_UNSIGNED_BYTE, mineBuf.data());
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, /*layer=*/1, kW, kH, 1,
                    GL_BGRA, GL_UNSIGNED_BYTE, blownBuf.data());
    // NEAREST filter, no mips — matches gosHint_DisableMipmap | DontShrink at
    // quad.cpp:524, :531. CLAMP_TO_EDGE: sprite UVs are 0..1, no edge-bleed risk.
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    // Restore state.
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0);
    glActiveTexture((GLenum)savedActive);

    g_mineTextureArrayReady = true;

    if (IsTraceEnabled()) {
        printf("[TERRAIN_INDIRECT v1] event=mine_atlas_built "
               "mineSlot=%u blownSlot=%u gltex=%u dim=%dx%d\n",
               (unsigned)mineSlot, (unsigned)blownSlot,
               (unsigned)g_mineTextureArrayGL, kW, kH);
        fflush(stdout);
    }
}

void BuildMineStaticVBO() {
    if (!GameMap || !land) {
        g_mineVertCount = 0;
        return;
    }

    // Empirically tiny in stock content — start with 64 cell capacity.
    std::vector<MineVert> verts;
    verts.reserve(64 * 6);

    const int W = (int)GameMap->width;
    const int H = (int)GameMap->height;
    const int TW = W / MAPCELL_DIM;
    const int TH = H / MAPCELL_DIM;
    const float halfCell = Terrain::worldUnitsPerCell * 0.5f;

    for (int tR = 0; tR < TH; ++tR) {
        for (int tC = 0; tC < TW; ++tC) {
            // tileHasMines short-circuits ~97% of tiles per recon-5.
            if (!GameMap->tileHasMines(tR, tC)) continue;
            for (int cR = 0; cR < MAPCELL_DIM; ++cR) {
                for (int cC = 0; cC < MAPCELL_DIM; ++cC) {
                    const int row = tR * MAPCELL_DIM + cR;
                    const int col = tC * MAPCELL_DIM + cC;
                    if (!GameMap->inBounds(row, col)) continue;
                    const unsigned long mine = GameMap->getMine(row, col);
                    if (mine == 0) continue;
                    const uint32_t layer = (mine == 2) ? 1u : 0u;

                    Stuff::Vector3D center;
                    land->tileCellToWorld(tR, tC, cR, cC, center);

                    // Four corners around cell center. tileCellToWorld returns
                    // cell center XY (with half-cell offset baked in per
                    // terrain.h:416-417); we expand to the 4 cell corners.
                    Stuff::Vector3D c00 = center, c10 = center;
                    Stuff::Vector3D c11 = center, c01 = center;
                    c00.x -= halfCell; c00.y += halfCell;  // top-left
                    c10.x += halfCell; c10.y += halfCell;  // top-right
                    c11.x += halfCell; c11.y -= halfCell;  // bottom-right
                    c01.x -= halfCell; c01.y -= halfCell;  // bottom-left
                    c00.z = land->getTerrainElevation(c00);
                    c10.z = land->getTerrainElevation(c10);
                    c11.z = land->getTerrainElevation(c11);
                    c01.z = land->getTerrainElevation(c01);

                    auto pushVert = [&](const Stuff::Vector3D& p, float u, float v) {
                        MineVert mv;
                        mv.pos[0] = p.x; mv.pos[1] = p.y; mv.pos[2] = p.z;
                        mv.uv[0]  = u;   mv.uv[1]  = v;
                        mv.layer  = layer;
                        verts.push_back(mv);
                    };
                    // Tri 1: c00 → c10 → c11
                    pushVert(c00, 0.0f, 0.0f);
                    pushVert(c10, 1.0f, 0.0f);
                    pushVert(c11, 1.0f, 1.0f);
                    // Tri 2: c00 → c11 → c01
                    pushVert(c00, 0.0f, 0.0f);
                    pushVert(c11, 1.0f, 1.0f);
                    pushVert(c01, 0.0f, 1.0f);
                }
            }
        }
    }

    g_mineVertCount = (int)verts.size();

    if (g_mineStaticVBO_GL == 0) glGenBuffers(1, &g_mineStaticVBO_GL);
    glBindBuffer(GL_ARRAY_BUFFER, g_mineStaticVBO_GL);
    // glBufferData with size>0 always uploads even if vector is empty
    // (verts.data() returns a non-null sentinel on empty MSVC vectors, but
    // we pass nullptr-on-empty defensively to match Khronos expectations).
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(MineVert)),
                 verts.empty() ? nullptr : verts.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (IsTraceEnabled()) {
        printf("[TERRAIN_INDIRECT v1] event=mine_vbo_built cells=%d verts=%d "
               "vbo_gl=%u\n",
               g_mineVertCount / 6, g_mineVertCount,
               (unsigned)g_mineStaticVBO_GL);
        fflush(stdout);
    }
}

void RebuildMineStaticVBOIfDirty() {
    if (!g_mineVBODirty) return;
    if (g_mineVBOFirstBuildPending) {
        BuildMineTextureArray();
        // Mark first-build done regardless of texture-array ready state —
        // BuildMineTextureArray handles its own retry via g_mineTextureArrayReady.
        g_mineVBOFirstBuildPending = false;
    }
    BuildMineStaticVBO();
    g_mineVBODirty = false;
}

unsigned int GetMineStaticVBO_GL()      { return g_mineStaticVBO_GL; }
int          GetMineVertCount()         { return g_mineVertCount; }
unsigned int GetMineTextureArrayGL()    { return g_mineTextureArrayGL; }
bool         IsMineTextureArrayReady()  { return g_mineTextureArrayReady; }

// PR2c Stage 2c — armed iff MC2_TERRAIN_INDIRECT_MINE=1 (env gate).
//
// We do NOT gate on g_mineTextureArrayReady here — that creates a bootstrap
// circular dependency: the texture-array can only be built inside
// BuildMineTextureArray, which is only called from RebuildMineStaticVBOIfDirty,
// which is only called from DrawMineStatic, which is only called from the
// Render.TerrainMines zone IF IsFrameMineArmed() returns true. Gating on
// readiness here means the first build never happens.
//
// Instead: arming = env on. DrawMineStatic handles the texture-not-ready
// case internally (returns true no-op until the next dirty event lazy-builds).
// First paint cycle after mission load: gate-offs fire, legacy paths skipped,
// DrawMineStatic invokes Rebuild* which lazy-builds the texture-array on
// first call (handles loaded by setupTextures on the SAME frame's earlier
// per-quad loop — R7 timing trap mitigated by paint-order: setupTextures
// runs before Render.TerrainMines).
bool IsFrameMineArmed() {
    return IsMineEnabled();
}

// PR2c Stage 2c — invoked from txmmgr.cpp's Render.TerrainMines zone. Lazy-builds
// VBO + texture-array on first dirty event; issues one glDrawArrays via the
// bridge if there's anything to draw.
bool DrawMineStatic() {
    RebuildMineStaticVBOIfDirty();
    if (g_mineVertCount <= 0) {
        // Successful zero-emit frame (mission has no mines yet). Legacy paths
        // are still gated off — that's the whole point.
        return true;
    }
    if (!g_mineTextureArrayReady) {
        // Build retry pending; the texture-array build couldn't complete (e.g.,
        // handles still 0xffffffff at first call). Treat as no-op until next
        // dirty event re-attempts.
        return true;
    }
    const bool ok = gos_terrain_bridge_drawMineStatic(
        g_mineVertCount,
        g_mineStaticVBO_GL,
        g_mineTextureArrayGL);
    if (ok) {
        // 6 verts per cell (2 tris * 3 verts).
        Counters_AddIndirectMineDrawnCells((long long)(g_mineVertCount / 6));
    }
    return ok;
}

}  // namespace gos_terrain_indirect

// C-linkage forwarder for mclib/move.h's inline setMine — avoids pulling
// the full gos_terrain_indirect.h into the widely-included move.h. The
// symbol is forward-declared at the call site in move.h:
//     extern void gos_terrain_indirect_MarkMineDirty();
// and resolved at link time.
void gos_terrain_indirect_MarkMineDirty() {
    gos_terrain_indirect::MarkMineDirty();
}
