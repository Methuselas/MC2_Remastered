// GameOS/gameos/gos_terrain_mask_dispatch.cpp
//
// Slice B4 Stage 1a: CPU mask build + SSBO upload. No draw yet.

#include "gos_terrain_mask_dispatch.h"
#include "gos_terrain_indirect.h"
#include "gos_terrain_water_stream.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/tex_resolve_table.h"

#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GL/glew.h>

// TERRAIN_DEPTH_FUDGE: not in a shared header; mirrors gos_terrain_indirect.cpp:1243
// which itself mirrors quad.cpp:1911. Use the same value so the SOLID mask predicate
// has bit-identical threshold to PackThinRecordsForFrame (parity requirement).
#ifndef TERRAIN_DEPTH_FUDGE
static constexpr float TERRAIN_DEPTH_FUDGE = 0.001f;
#endif

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
    glGenBuffers(1, &s_solidMaskSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_solidMaskSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, maskBytes, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

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

        // (3) Per-tri pz check — mirrors :1430-1448
        bool pzc[4];
        for (int c = 0; c < 4; ++c) {
            float pz_adj = q.vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
        }
        const int uvMode = (int)q.uvMode;
        bool pzTri1, pzTri2;
        if (uvMode == 1 /*BOTTOMLEFT*/) {
            pzTri1 = pzc[0] && pzc[1] && pzc[3];
            pzTri2 = pzc[1] && pzc[2] && pzc[3];
        } else {
            pzTri1 = pzc[0] && pzc[1] && pzc[2];
            pzTri2 = pzc[0] && pzc[2] && pzc[3];
        }
        const bool pzVisible = pzTri1 || pzTri2;

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

    MASK_TRACE("event=build_done solidSet=%d waterSet=%d numQuads=%ld",
               solidSet, waterSet, numQuads);
}

// ---------------------------------------------------------------------------
// Draw stubs (Stage 1a — real bodies land in Stage 1b/1c)
// ---------------------------------------------------------------------------

bool DrawMaskSolid() {
    return false;
}

bool DrawMaskWater() {
    return false;
}

uint32_t GetSolidMaskSSBO() {
    return (uint32_t)s_solidMaskSSBO;
}

uint32_t GetWaterMaskSSBO() {
    return (uint32_t)s_waterMaskSSBO;
}

} // namespace gos_terrain_mask_dispatch
