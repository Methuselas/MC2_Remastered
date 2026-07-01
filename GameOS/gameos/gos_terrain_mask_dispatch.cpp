// GameOS/gameos/gos_terrain_mask_dispatch.cpp
//
// Slice B4 Stage 1a: CPU mask build + SSBO upload. No draw yet.

#include "gos_terrain_mask_dispatch.h"
#include "gos_terrain_indirect.h"
#include "gos_terrain_water_stream.h"
#include "gos_terrain_bridge.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/tex_resolve_table.h"
#include "../../mclib/terrain.h"  // Terrain::waterElevation, Terrain::frameCos

#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GL/glew.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#  pragma intrinsic(_BitScanForward)
static inline int mc2_ctz_u32(uint32_t v) {
    unsigned long idx = 0;
    _BitScanForward(&idx, v);
    return (int)idx;
}
#else
static inline int mc2_ctz_u32(uint32_t v) { return __builtin_ctz(v); }
#endif

// Slice B4 Stage 1b — accessors implemented elsewhere.
extern "C" unsigned int    gos_terrain_indirect_getRecipeSSBO();
extern "C" int             gos_terrain_indirect_getRecipeMapSide();
extern "C" int             gos_terrain_indirect_getRecipeQuadCount();
extern "C" GLuint          gos_terrain_lighting_getOutputSSBO();

// (Removed dead TERRAIN_DEPTH_FUDGE define 2026-05-16: stale 0.001f with
// zero consumers since the pz-cull predicate was retired in the VPL Step 4
// pz-write retirement (pzVisible=true). It was also DESYNCED (0.001 vs the
// 0.002 everyone else used) and its comment was doubly stale -- a latent
// trap if a pz-cull were ever re-introduced here. Depth bias is now
// single-sourced in mclib/terrain_depth_bias.h. VPL-deferred item 10.)

using WaterStream::WaterRecipe;

namespace gos_terrain_mask_dispatch {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static bool    s_traceOn   = false;  // MC2_TERRAIN_MASK_DISPATCH_TRACE=1
static bool    s_masterOn  = false;  // MC2_TERRAIN_MASK_DISPATCH != "0" (and set)
static bool    s_initDone  = false;  // Init() succeeded
static bool    s_readyThisFrame = false;

// Worst-case mask size: 120×120 map = 14,400 quads, ceil(14400/32) = 450 uint32s
static constexpr int32_t kMaxMapSide     = 120;
static constexpr int32_t kMaxQuads       = kMaxMapSide * kMaxMapSide;
static constexpr int32_t kMaskWords      = (kMaxQuads + 31) / 32;  // 450

// CPU shadow buffers (reset each frame, rebuilt by BuildAndUploadMasksForFrame)
static std::vector<uint32_t> s_solidMask;
static std::vector<uint32_t> s_waterMask;

// GL SSBO names (allocated once, reused across missions)
static GLuint s_solidMaskSSBO = 0;
static GLuint s_waterMaskSSBO = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool MasterEnabled() {
    // Cached at Init() time; re-read here for the IsMaskDispatchEnabled() path
    // which may be called before Init() (safe: returns false if not init'd).
    const char* v = getenv("MC2_TERRAIN_MASK_DISPATCH");
    if (!v || !v[0]) return false;
    if (v[0] == '0' && v[1] == '\0') return false;
    return true;
}

#define MASK_TRACE(fmt, ...) \
    do { if (s_traceOn) { printf("[MASK_DISPATCH v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Init(int32_t mapSide) {
    s_traceOn = (getenv("MC2_TERRAIN_MASK_DISPATCH_TRACE") != nullptr);
    s_masterOn = MasterEnabled();

    if (!s_masterOn) {
        MASK_TRACE("event=init_skipped reason=master_disabled");
        return true;  // not an error; feature is just off
    }

    if (mapSide > kMaxMapSide) {
        fprintf(stderr,
            "[MASK_DISPATCH v1] event=init_fail reason=oversized_map mapSide=%d max=%d\n",
            (int)mapSide, (int)kMaxMapSide);
        fflush(stderr);
        s_initDone = false;
        return false;
    }

    if (s_initDone) {
        MASK_TRACE("event=init_noop reason=already_allocated");
        return true;
    }

    // Allocate CPU shadow buffers at worst-case size
    s_solidMask.assign(kMaskWords, 0u);
    s_waterMask.assign(kMaskWords, 0u);

    // Allocate GL SSBOs at worst-case size (never reallocated per mission)
    const GLsizeiptr maskBytes = (GLsizeiptr)(kMaskWords * sizeof(uint32_t));
    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_solidMaskSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_solidMaskSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, maskBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // TIER2-EXCLUDED: substrate-gated
    glGenBuffers(1, &s_waterMaskSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_waterMaskSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, maskBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_initDone = true;
    MASK_TRACE("event=init_done mapSide=%d maskWords=%d maskBytes=%lld",
               (int)mapSide, (int)kMaskWords, (long long)maskBytes);
    return true;
}

void Reset() {
    if (s_solidMask.size()) std::fill(s_solidMask.begin(), s_solidMask.end(), 0u);
    if (s_waterMask.size()) std::fill(s_waterMask.begin(), s_waterMask.end(), 0u);
    s_readyThisFrame = false;
    MASK_TRACE("event=reset");
}

// ---------------------------------------------------------------------------
// Per-frame predicates
// ---------------------------------------------------------------------------

bool IsMaskDispatchEnabled() {
    return MasterEnabled()
        && gos_terrain_indirect::IsDenseRecipeReady()
        && s_initDone;
}

bool IsMaskDispatchReady() {
    return s_readyThisFrame;
}

void BeginFrame() {
    s_readyThisFrame = false;
}

bool IsFrameMaskSolidArmed() {
    if (!s_readyThisFrame) return false;
    const char* v = getenv("MC2_TERRAIN_MASK_DISPATCH_SOLID");
    if (v && v[0] == '0' && v[1] == '\0') return false;
    return true;
}

bool IsFrameMaskWaterArmed() {
    if (!s_readyThisFrame) return false;
    const char* v = getenv("MC2_TERRAIN_MASK_DISPATCH_WATER");
    if (v && v[0] == '0' && v[1] == '\0') return false;
    return true;
}

// ---------------------------------------------------------------------------
// Mask build — fused single pass over quadList
// ---------------------------------------------------------------------------

void BuildAndUploadMasksForFrame(const TerrainQuadPtr quadList, long numQuads) {
    if (!s_initDone || !s_masterOn) return;

    // Clear CPU shadow buffers
    std::fill(s_solidMask.begin(), s_solidMask.end(), 0u);
    std::fill(s_waterMask.begin(), s_waterMask.end(), 0u);

    int32_t solidSet = 0, waterSet = 0;

    for (long qi = 0; qi < numQuads; ++qi) {
        const TerrainQuad& q = quadList[qi];

        // (1) Pointer guards — mirrors gos_terrain_indirect.cpp:1409-1410
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;

        // (2) Map-edge blank-vertex skip — mirrors :1413-1417
        const int32_t vn0 = q.vertices[0]->vertexNum;
        if (vn0 < 0 ||
            q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 ||
            q.vertices[3]->vertexNum < 0) continue;

        // Bounds check: vn0 must fit in the mask array
        if (vn0 >= kMaxQuads) continue;

        // (3) pz-cull retired (VPL pz-write retirement, plan Step 4).
        // GPU mask shader + raster clip handle off-screen rejection; the
        // CPU mask is conservatively inclusive (over-inclusion is correct
        // per VPL-plan Step 5 rationale (c)). Was: per-corner q.vertices[c]->pz
        // four-vertex pz-adj test (the last mask-dispatch VPL dependency).
        const bool pzVisible = true;

        // --- SOLID mask ---
        // (4) Recipe coverage gate — mirrors :1420
        if (pzVisible) {
            const TerrainQuadRecipe* rec =
                gos_terrain_indirect::RecipeForVertexNum(vn0);
            if (rec) {
                // (5) terrainHandle skip — CRITICAL: mirrors :1425-1428
                const uint32_t th = static_cast<uint32_t>(
                    tex_resolve(static_cast<DWORD>(q.terrainHandle)));
                if (th != 0 && th != 0xffffffffu) {
                    // Bit index = vn0 (corner-0 vertexNum = mapY*W+mapX).
                    // Stage 1b shader must read by vertexNum, NOT quadList slot.
                    s_solidMask[vn0 >> 5] |= (1u << (vn0 & 31));
                    ++solidSet;
                }
            }
        }

        // --- Water mask ---
        // Only water-bearing quads with a valid waterHandle
        if (pzVisible) {
            const WaterRecipe* wrec =
                WaterStream::RecipeForVertexNum(vn0);
            if (wrec) {
                if (q.waterHandle != 0xffffffffu) {
                    s_waterMask[vn0 >> 5] |= (1u << (vn0 & 31));
                    ++waterSet;
                }
            }
        }
    }

    // Upload both masks
    const GLsizeiptr maskBytes = (GLsizeiptr)(kMaskWords * sizeof(uint32_t));

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_solidMaskSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, maskBytes, s_solidMask.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_waterMaskSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, maskBytes, s_waterMask.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    s_readyThisFrame = true;

    MASK_TRACE("event=build_done solidSet=%d waterSet=%d numQuads=%ld pzCullRetired=1",
               solidSet, waterSet, numQuads);
}

// ---------------------------------------------------------------------------
// Draw stubs (Stage 1a — real bodies land in Stage 1b/1c)
// ---------------------------------------------------------------------------

bool DrawMaskSolid() {
    if (!s_readyThisFrame) return false;

    // Per-bucket killswitch (allows armed-water-only configurations)
    const char* armV = getenv("MC2_TERRAIN_MASK_DISPATCH_SOLID");
    if (armV && armV[0] == '0' && armV[1] == '\0') return false;

    const uint32_t solidSSBO  = s_solidMaskSSBO;
    const uint32_t recipeSSBO = (uint32_t)gos_terrain_indirect_getRecipeSSBO();
    const uint32_t lightSSBO  = (uint32_t)gos_terrain_lighting_getOutputSSBO();
    const int      quadCount  = gos_terrain_indirect_getRecipeQuadCount();
    const int      mapSide    = gos_terrain_indirect_getRecipeMapSide();

    if (solidSSBO == 0 || recipeSSBO == 0 || quadCount <= 0 || mapSide <= 0)
        return false;

    const bool ok = gos_terrain_bridge_drawMaskSolid(
        solidSSBO, recipeSSBO, lightSSBO, quadCount, mapSide);

    if (s_traceOn) {
        printf("[MASK_DISPATCH v1] event=draw_solid ok=%d quadCount=%d mapSide=%d\n",
               (int)ok, quadCount, mapSide);
        fflush(stdout);
    }

    return ok;
}

bool DrawMaskWater() {
    if (!s_readyThisFrame) return false;

    // Per-bucket killswitch (allows armed-solid-only configurations)
    const char* armV = getenv("MC2_TERRAIN_MASK_DISPATCH_WATER");
    if (armV && armV[0] == '0' && armV[1] == '\0') return false;

    const uint32_t waterMaskSSBO = s_waterMaskSSBO;
    const uint32_t recipeSSBO    = (uint32_t)WaterStream::EnsureRecipeBufferUploaded();
    const uint32_t lightSSBO     = (uint32_t)gos_terrain_lighting_getOutputSSBO();
    const int      recipeCount   = (int)WaterStream::GetRecipeCount();

    if (waterMaskSSBO == 0 || recipeSSBO == 0 || recipeCount <= 0) return false;

    const float wElev = Terrain::waterElevation;
    const float fCos  = Terrain::frameCos;

    const bool ok = gos_terrain_bridge_drawMaskWater(
        waterMaskSSBO, recipeSSBO, lightSSBO, recipeCount, wElev, fCos);

    if (s_traceOn) {
        printf("[MASK_DISPATCH v1] event=draw_water ok=%d recipeCount=%d\n",
               (int)ok, recipeCount);
        fflush(stdout);
    }

    return ok;
}

uint32_t GetSolidMaskSSBO() {
    return (uint32_t)s_solidMaskSSBO;
}

uint32_t GetWaterMaskSSBO() {
    return (uint32_t)s_waterMaskSSBO;
}

} // namespace gos_terrain_mask_dispatch
