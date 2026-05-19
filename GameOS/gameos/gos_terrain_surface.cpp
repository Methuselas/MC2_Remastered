// GameOS/gameos/gos_terrain_surface.cpp
//
// [TERRAIN_SURFACE] continuous-surface producer -- PR-1 mission-load
// generation (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES NOTHING).
//
// Plan : docs/superpowers/plans/2026-05-18-terrain-continuous-surface-producer-plan.md
// Design: docs/superpowers/specs/2026-05-18-terrain-continuous-surface-producer-design.md
//
// Generates the four mission-static resources (design Section 1.3):
//   1. surface vertex array  -- one pos+normal per map-stable vertexNum,
//      de-duplicated from the dense recipe corners (a grid node shared by up
//      to 4 quads appears ONCE here).
//   2. mission-static index buffer -- two triangles / cell, emitted in the
//      per-tile worldQuadUVMode() diagonal parity order (design MINOR-1).
//   3. per-tile material table -- EVOLVED from TerrainQuadRecipe (_wp0/_wp1/
//      _wp2/_wp3/uvExt), copied byte-for-byte from the dense recipe so the
//      surface's per-fragment material model is identical to the armed path.
//   4. per-edge adjacency -- neighbor cell indices for the PR-3 crack-free
//      seam stage (built here, consumed later).
//
// M-4 STOCK-ONLY FENCE (HARD, memory/stock_install_must_remain_playable.md):
// the generation source is the dense TerrainQuadRecipe + MapData::getBlocks(),
// both derived from unmodified stock map/tile asset data at MISSION LOAD with
// NO arming / ComputePreflight / per-frame-compute precondition and NO
// savegame field. The entry point is Terrain::primeMissionTerrainCache (the
// mission-load recipe-build chokepoint) -- NOT ComputePreflight. The fence
// asserts the surface resources are populated at mission-load completion
// BEFORE the first frame.
//
// Re-grep confirmation (plan PR-1 "Re-grep at execution (do NOT assume)",
// verified at write-time on the nifty tree):
//   - gos_terrain_indirect::BuildDenseRecipe() is called from
//     Terrain::primeMissionTerrainCache (mclib/terrain.cpp:638), which is the
//     mission-load cache build -- INDEPENDENT of ComputePreflight
//     (terrain.cpp:1921, a per-frame arming entry). PR-1 hooks the SAME
//     mission-load chokepoint, after BuildDenseRecipe.
//   - gos_terrain_indirect::InvalidateAllRecipes() fires from MapData
//     (mclib/mapdata.cpp:154/199/923 -- newInit / shadow-recalc /
//     normal-recompute), the rebuild chokepoint PR-1 mirrors for regeneration.
//
// Instrumentation (SAME commit, debug_instrumentation_rule.md): the PR-0
// dormant [TERRAIN_SURFACE v1] channel (terrain_surface_trace.h, default-OFF)
// gains its first call sites here -- mission-load generation lifecycle, the
// stock-only-fence assertion result, and teardown. Lifecycle boundaries ONLY,
// coarse once-per-mission -- never per-quad / per-vertex (the cost_split
// observer-effect lesson).

#include "gos_terrain_surface.h"
#include "gos_terrain_surface_schema.h"
#include "../../mclib/terrain_surface_trace.h"  // PR-0 [TERRAIN_SURFACE v1] channel

#include "gos_terrain_indirect.h"               // RecipeForVertexNum / IsDenseRecipeReady
#include "gos_terrain_patch_stream.h"           // TerrainQuadRecipe

#include "../../mclib/terrain.h"                // Terrain:: statics, MapData
#include "../../mclib/mapdata.h"                // MapData
#include "../../mclib/vertex.h"                 // PostcompVertex

#include <vector>
#include <cstring>
#include <cstdlib>

namespace gos_terrain_surface {

// ---------------------------------------------------------------------------
// Env kill-switch -- read once, cached (mirrors gos_terrain_indirect::IsEnabled
// and the existing MC2_TERRAIN_INDIRECT / overlay gates). DEFAULT-OFF: only a
// literal non-"0" value turns generation on (plan Section F, Waves 0-3).
// ---------------------------------------------------------------------------
bool IsEnabled() {
    static const bool s_enabled = []() {
        const char* v = getenv("MC2_TERRAIN_SURFACE");
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    return s_enabled;
}

namespace {

// Mission-static surface state. CPU-side authoritative copies; PR-1 owns only
// the CPU generation + the stock-only fence (no draw, so no GL upload yet --
// the GL SSBO objects are a PR-2 item where the VS first reads them).
std::vector<TerrainSurfaceVertex>    g_vertices;
std::vector<uint32_t>                g_indices;
std::vector<TerrainSurfaceTile>      g_tiles;
std::vector<TerrainSurfaceAdjacency> g_adjacency;
int32_t                              g_mapSide   = 0;
bool                                 g_generated = false;
// PR-2: bumped on every successful (re)generation so the GPU-upload bridge
// (gameos_graphics.cpp) can detect a rebuild and re-upload the SSBOs. 0 means
// "never generated"; the bridge caches the last-uploaded epoch.
uint32_t                             g_genEpoch  = 0;

// uvMode parity -- IDENTICAL to mclib/mapdata.cpp:115 worldQuadUVMode(tileR,
// tileC) = ((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT, with
// tileR = my, tileC = mx. Returns true for the BOTTOMLEFT diagonal. Mirrors
// gpu_driven_terrain_solid.comp:349 isBottomLeft = ((mx&1) != (my&1)).
inline bool cellIsBottomLeft(long mx, long my) {
    return ((mx & 1L) != (my & 1L));
}

} // namespace

void ResetForMission() {
    const bool wasGenerated = g_generated;
    const size_t vtx = g_vertices.size();
    const size_t idx = g_indices.size();

    g_vertices.clear();
    g_indices.clear();
    g_tiles.clear();
    g_adjacency.clear();
    g_mapSide   = 0;
    g_generated = false;

    if (wasGenerated) {
        TS_TRACE("event=teardown reason=mission_reset prev_verts=%zu prev_indices=%zu",
                 vtx, idx);
    }
}

void GenerateForMission() {
    // Kill-switch OFF (Waves 0-3 default): generation is skipped entirely --
    // zero cost, behaviour-neutral, the surface never exists. PR-1 must be a
    // no-op with the surface disabled (plan PR-1 DONE: "default-OFF: resources
    // not built ... behavior-neutral with the surface disabled").
    if (!IsEnabled()) return;

    // Idempotent rebuild head (also covers RegenerateForMission()).
    ResetForMission();

    // ---- Stock-only fence, part 1: the generation SOURCE must be the
    // mission-load dense recipe (stock-derived, no arming precondition).
    // If the recipe is not ready at mission-load completion the fence FAILS
    // loudly and generation is abandoned (no partial surface). The recipe is
    // built by gos_terrain_indirect::BuildDenseRecipe at terrain.cpp:638,
    // which is the SAME mission-load chokepoint we are called from, just
    // before us -- so a not-ready recipe here means a stock mission could not
    // build the surface, exactly the M-4 condition the fence guards.
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 1 || !Terrain::mapData) {
        TS_TRACE("event=gen_fence_fail reason=no_mapdata mapSide=%ld", mapSide);
        return;
    }
    if (!gos_terrain_indirect::IsDenseRecipeReady()) {
        TS_TRACE("event=gen_fence_fail reason=recipe_not_ready_at_mission_load "
                 "mapSide=%ld", mapSide);
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    if (!blocks) {
        TS_TRACE("event=gen_fence_fail reason=no_blocks mapSide=%ld", mapSide);
        return;
    }

    g_mapSide = (int32_t)mapSide;
    const long  cells    = mapSide - 1;          // quad cells per side
    const long  halfSide = Terrain::halfVerticesMapSide;
    const float wupv     = Terrain::worldUnitsPerVertex;

    TS_TRACE("event=gen_begin mapSide=%ld cells_per_side=%ld src=dense_recipe",
             mapSide, cells);

    // ---- 1. Surface vertex array: one node per vertexNum, de-duplicated.
    // World X/Y from map indices match buildRecipeSlot exactly
    // (gos_terrain_indirect.cpp:639-646); elevation + normal come from the
    // node's own PostcompVertex (the SAME stock source buildRecipeSlot reads).
    const size_t nNodes = (size_t)mapSide * (size_t)mapSide;
    g_vertices.resize(nNodes);
    for (long my = 0; my < mapSide; ++my) {
        for (long mx = 0; mx < mapSide; ++mx) {
            const size_t vn = (size_t)mx + (size_t)my * (size_t)mapSide;
            const PostcompVertex& p = blocks[mx + my * mapSide];
            TerrainSurfaceVertex& v = g_vertices[vn];
            v.px = float(mx - halfSide) * wupv;
            v.py = float(halfSide - my) * wupv;
            v.pz = p.elevation;
            v._ppad = 0.f;
            v.nx = p.vertexNormal.x;
            v.ny = p.vertexNormal.y;
            v.nz = p.vertexNormal.z;
            v._npad = 0.f;
        }
    }

    // ---- 2. Mission-static index buffer + 3. per-tile material table +
    //         4. per-edge adjacency. One entry per quad cell (mx,my) with
    // mx,my in [0, cells). Cell index = mx + my*cells.
    const size_t nCells = (size_t)cells * (size_t)cells;
    g_indices.reserve(nCells * kSurfaceIndicesPerCell);
    g_tiles.resize(nCells);
    g_adjacency.resize(nCells);

    for (long my = 0; my < cells; ++my) {
        for (long mx = 0; mx < cells; ++mx) {
            const size_t cell = (size_t)mx + (size_t)my * (size_t)cells;

            // Corner node vertexNums (top-left = vn0; matches buildRecipeSlot
            // corner layout v0..v3).
            const uint32_t c0 = (uint32_t)( mx       +  my       * mapSide); // (mx,  my)
            const uint32_t c1 = (uint32_t)((mx + 1)  +  my       * mapSide); // (mx+1,my)
            const uint32_t c2 = (uint32_t)((mx + 1)  + (my + 1)  * mapSide); // (mx+1,my+1)
            const uint32_t c3 = (uint32_t)( mx       + (my + 1)  * mapSide); // (mx,  my+1)

            // Diagonal parity per worldQuadUVMode(my, mx):
            //   BOTTOMRIGHT (not bottomleft): (0,1,2)+(0,2,3)
            //   BOTTOMLEFT                  : (0,1,3)+(1,2,3)
            if (cellIsBottomLeft(mx, my)) {
                g_indices.push_back(c0); g_indices.push_back(c1); g_indices.push_back(c3);
                g_indices.push_back(c1); g_indices.push_back(c2); g_indices.push_back(c3);
            } else {
                g_indices.push_back(c0); g_indices.push_back(c1); g_indices.push_back(c2);
                g_indices.push_back(c0); g_indices.push_back(c2); g_indices.push_back(c3);
            }

            // Per-tile material table: copy the dense recipe slot (keyed by
            // the cell's top-left vertexNum, exactly how the armed path
            // resolves per-fragment material). Byte-for-byte from the recipe
            // so PR-2's per-fragment model is identical (design Section 2).
            const int32_t vn0 = (int32_t)c0;
            const TerrainQuadRecipe* rec =
                gos_terrain_indirect::RecipeForVertexNum(vn0);
            TerrainSurfaceTile& t = g_tiles[cell];
            if (rec) {
                std::memcpy(&t.wp0, &rec->_wp0, 4);
                std::memcpy(&t.wp1, &rec->_wp1, 4);
                std::memcpy(&t.wp2, &rec->_wp2, 4);
                std::memcpy(&t.wp3, &rec->_wp3, 4);
                t.minU = rec->minU; t.minV = rec->minV;
                t.maxU = rec->maxU; t.maxV = rec->maxV;
            } else {
                std::memset(&t, 0, sizeof(t));
            }

            // Per-edge adjacency: neighbor cell index, kSurfaceNoNeighbor at
            // the map edge. Edge order matches the schema header.
            TerrainSurfaceAdjacency& a = g_adjacency[cell];
            a.nbrTop    = (my > 0)         ? (uint32_t)((size_t)mx + (size_t)(my - 1) * (size_t)cells) : kSurfaceNoNeighbor;
            a.nbrRight  = (mx < cells - 1) ? (uint32_t)((size_t)(mx + 1) + (size_t)my * (size_t)cells) : kSurfaceNoNeighbor;
            a.nbrBottom = (my < cells - 1) ? (uint32_t)((size_t)mx + (size_t)(my + 1) * (size_t)cells) : kSurfaceNoNeighbor;
            a.nbrLeft   = (mx > 0)         ? (uint32_t)((size_t)(mx - 1) + (size_t)my * (size_t)cells) : kSurfaceNoNeighbor;
        }
    }

    g_generated = true;

    // ---- Stock-only fence, part 2: assert all four resources are populated
    // at mission-load completion BEFORE the first frame, from the stock recipe
    // source only (no arming / savegame). A size mismatch == a malformed
    // surface; report it and drop generation rather than ship a partial mesh.
    const bool fenceOK =
        g_vertices.size()  == nNodes &&
        g_indices.size()   == nCells * kSurfaceIndicesPerCell &&
        g_tiles.size()     == nCells &&
        g_adjacency.size() == nCells &&
        g_generated;

    if (!fenceOK) {
        TS_TRACE("event=gen_fence_fail reason=size_mismatch verts=%zu/%zu "
                 "indices=%zu/%zu tiles=%zu/%zu adj=%zu/%zu",
                 g_vertices.size(), nNodes,
                 g_indices.size(), nCells * kSurfaceIndicesPerCell,
                 g_tiles.size(), nCells,
                 g_adjacency.size(), nCells);
        ResetForMission();
        return;
    }

    // PR-2: a complete, fence-passed generation -- advance the epoch so the
    // GPU-upload bridge re-uploads the surface SSBOs on the next draw.
    ++g_genEpoch;

    TS_TRACE("event=gen_complete mapSide=%d verts=%zu indices=%zu tiles=%zu "
             "adj=%zu epoch=%u src=stock_dense_recipe arming=none savegame=none",
             g_mapSide, g_vertices.size(), g_indices.size(),
             g_tiles.size(), g_adjacency.size(), g_genEpoch);
    TS_TRACE("event=stock_fence_pass detail=resources_populated_at_mission_load_"
             "before_first_frame_from_stock_only");
}

void RegenerateForMission() {
    if (!IsEnabled()) return;
    // Mission restart / shadow recalc / normal recompute -- the dense recipe
    // has just been rebuilt by InvalidateAllRecipes; rebuild the surface from
    // it. GenerateForMission() is itself idempotent (ResetForMission head).
    TS_TRACE("event=regenerate reason=invalidate_all_recipes");
    GenerateForMission();
}

bool     IsGenerated()       { return g_generated; }
uint32_t GetVertexCount()    { return (uint32_t)g_vertices.size(); }
uint32_t GetIndexCount()     { return (uint32_t)g_indices.size(); }
uint32_t GetTileCount()      { return (uint32_t)g_tiles.size(); }
uint32_t GetAdjacencyCount() { return (uint32_t)g_adjacency.size(); }
int32_t  GetMapSide()        { return g_mapSide; }

// PR-2 GPU-upload accessors -- read-only views into the mission-static CPU
// buffers. Empty/null when not generated (the bridge guards on IsGenerated()).
const void* GetVertexData()  { return g_vertices.empty() ? nullptr : (const void*)g_vertices.data(); }
const void* GetIndexData()   { return g_indices.empty()  ? nullptr : (const void*)g_indices.data();  }
uint32_t    GetGenerationEpoch() { return g_genEpoch; }

} // namespace gos_terrain_surface
