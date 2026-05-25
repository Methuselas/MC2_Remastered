// GameOS/gameos/gos_terrain_surface_schema.h
#pragma once
//
// [TERRAIN_SURFACE] continuous-surface producer -- std430 buffer schema.
//
// Plan : docs/superpowers/plans/2026-05-18-terrain-continuous-surface-producer-plan.md
//        (PR-1, Wave 1 -- mission-load surface generation)
// Design: docs/superpowers/specs/2026-05-18-terrain-continuous-surface-producer-design.md
//        Section 1.3 (SSBO / buffer schema, std430 lockstep, Vulkan-prep).
//
// PR-1 is ADDITIVE / DEFAULT-OFF / DELETES NOTHING. These structs declare the
// four mission-static resources the continuous-surface producer generates at
// mission load by de-duplicating the existing dense TerrainQuadRecipe corners
// (gos_terrain_indirect.cpp buildRecipeSlot). Nothing in PR-1 draws or consumes
// the surface -- the VS / LOD stage / draw repoint land in PR-2..PR-4.
//
// LOCKSTEP DISCIPLINE (load-bearing, memory/cpp_glsl_ubo_struct_lockstep.md):
// every struct here carries a `static_assert(sizeof(...) == ...)` size pin and
// `offsetof` field pins, exactly like the existing 96 B / offset-32 thin-record
// asserts (gos_terrain_patch_stream.h:125-128). PR-2 adds the GLSL mirrors in
// the SAME commit as the surface VS that reads them; PR-1 only owns the C++
// side (no shader consumes the surface yet), so PR-1's lockstep obligation is
// the C++ size/offset pins compiling -- the GLSL-mirror half is a PR-2 item by
// the plan's wave graph (PR-2 = "Indexed-surface VS + std430 lockstep schema").
//
// std430 rules honored: every member sits on its natural alignment; vec4-shaped
// records are 16-aligned; no implicit padding straddles a 16 B boundary.

#include <cstdint>
#include <cstddef>   // offsetof

namespace gos_terrain_surface {

// ---------------------------------------------------------------------------
// 1. Surface vertex SSBO -- one position+normal per map-stable vertexNum.
//
// De-duplicated from the dense recipe corners (a grid node shared by up to 4
// quads appears ONCE here, keyed by its vertexNum = mx + my*mapSide). std430,
// 16-aligned: vec4 pos (xyz + w pad), vec4 normal (xyz + w pad). 32 B.
// ---------------------------------------------------------------------------
struct alignas(16) TerrainSurfaceVertex {
    float px, py, pz, _ppad;   // world-space position; _ppad = 0 (std430 vec4)
    float nx, ny, nz, _npad;   // vertex normal;        _npad = 0 (std430 vec4)
};
static_assert(sizeof(TerrainSurfaceVertex) == 32,
    "TerrainSurfaceVertex must be 32 bytes for std430 vec4[2] alignment");
static_assert(offsetof(TerrainSurfaceVertex, px) == 0,
    "TerrainSurfaceVertex.px must start at offset 0");
static_assert(offsetof(TerrainSurfaceVertex, nx) == 16,
    "TerrainSurfaceVertex.nx must start at offset 16 (second vec4)");

// ---------------------------------------------------------------------------
// 2. Per-tile material table -- EVOLVED from TerrainQuadRecipe's per-quad
//    indirection lanes into a per-tile lookup the surface indexes by
//    WorldPos->tile (NC1). Fields sourced byte-for-byte as today
//    (gos_terrain_indirect.cpp buildRecipeSlot / PopulateRecipeCementWords):
//      wp0 : 4-corner packed material types (m0|m1<<8|m2<<16|m3<<24)
//      wp1 : low bit uvMode (kept for the per-fragment material-weight blend,
//            NOT for triangulation -- triangulation lives in the IB winding)
//      wp2 : nodeId terrain-handle LUT key
//      wp3 : cement word (bit 31 valid + 16-bit cement-atlas layer)
//      uvExt : (minU, minV, maxU, maxV)
// 32 B (two std430 vec4-equivalent rows). Indexed by WorldPos->tile, never by
// a draw-primitive id.
// ---------------------------------------------------------------------------
struct alignas(16) TerrainSurfaceTile {
    uint32_t wp0, wp1, wp2, wp3;     // packed material / uvMode / nodeId / cement
    float    minU, minV, maxU, maxV; // uvExt
};
static_assert(sizeof(TerrainSurfaceTile) == 32,
    "TerrainSurfaceTile must be 32 bytes for std430 alignment");
static_assert(offsetof(TerrainSurfaceTile, wp0) == 0,
    "TerrainSurfaceTile.wp0 must start at offset 0");
static_assert(offsetof(TerrainSurfaceTile, minU) == 16,
    "TerrainSurfaceTile.minU must start at offset 16 (second vec4 row)");

// ---------------------------------------------------------------------------
// 3. Mission-static index buffer -- two triangles per quad cell, emitted in
//    the per-tile worldQuadUVMode() diagonal parity order (design MINOR-1):
//      BOTTOMRIGHT (parity (my&1)==(mx&1)): (0,1,2)+(0,2,3)
//      BOTTOMLEFT  (else)                 : (0,1,3)+(1,2,3)
//    Stored as a flat uint32 index array (6 indices per cell). std430 uint[]
//    (4 B stride) -- a baked index SSBO the VS will read by gl_VertexID
//    (Fork V default V-ssbo; no IBO/VAO element-array state, per
//    memory/element_array_buffer_is_vao_state_new_draw_paths_own_their_vao.md).
// No struct: it is `std::vector<uint32_t>`; documented here for the schema set.
// ---------------------------------------------------------------------------
constexpr uint32_t kSurfaceIndicesPerCell = 6u; // two triangles

// ---------------------------------------------------------------------------
// 4. Per-edge adjacency / LOD-band SSBO -- the neighbor data for crack-free
//    seams (design Section 3.3). Mission-static: per quad cell, the four
//    neighbor cell indices (-1 / 0xFFFFFFFF == no neighbor at the map edge),
//    consumed in PR-3 as `outerTess[e] = tessForBand(min(myBand,
//    neighborBand[e]))`. PR-1 only GENERATES it (no band stage yet). 16 B
//    (one std430 uvec4).
//   Edge order (consistent with the v0..v3 corner layout in buildRecipeSlot):
//     e0 = top    edge  (v0->v1) neighbor cell at (mx,   my-1)
//     e1 = right  edge  (v1->v2) neighbor cell at (mx+1, my  )
//     e2 = bottom edge  (v2->v3) neighbor cell at (mx,   my+1)
//     e3 = left   edge  (v3->v0) neighbor cell at (mx-1, my  )
// ---------------------------------------------------------------------------
struct alignas(16) TerrainSurfaceAdjacency {
    uint32_t nbrTop, nbrRight, nbrBottom, nbrLeft; // cell index or 0xFFFFFFFF
};
static_assert(sizeof(TerrainSurfaceAdjacency) == 16,
    "TerrainSurfaceAdjacency must be 16 bytes for std430 uvec4 alignment");
static_assert(offsetof(TerrainSurfaceAdjacency, nbrTop) == 0,
    "TerrainSurfaceAdjacency.nbrTop must start at offset 0");

constexpr uint32_t kSurfaceNoNeighbor = 0xFFFFFFFFu;

} // namespace gos_terrain_surface
