# Terrain LOD Chunk System — Implementation Spec

**Date:** 2026-06-07
**Source of truth:** This file (contract). `docs/terrain-lod-chunk-design.md` (design intent).
**Note on design doc divergence:** `terrain-lod-chunk-design.md` errors corrected here
(and in the design doc itself):
(1) Superchunk count: was '12x12=144' — wrong. Correct: `ceil(51/4)=13`, 13x13=169.
(2) uint32 overflow: was '136 years' — wrong. Correct: ~2.26 years.
(3) `blocksMapSide` formula: was `ceil(vertices/20)` — wrong. Must be `ceil(quads/20)`
    = `ceil((vertices-1)/20)` = `(vertices+18)/20`. For 1021: gives 51 (not 52).
(4) `TerrainDrawCommand._pad`: filled with `quadCountsPacked` to carry block quad dims
    across boundary without leaking `TerrainBlockMeta*`.
(5) Tacmap line in hierarchy: stale. Primary terrain is LOD chunk mesh; tacmap is overlay.
(6) calcThisFrame migration: `lastLitFrame` NOT added — `calcThisFrame & 1` lighting guard
    does not exist in the codebase. Water-only migration (`lastWaterFrame` only).
**Status:** Pre-implementation contract. No code changes yet.

---

## Hard Architectural Contract

### Q1. What owns render authority?

CPU terrain code (mclib/) owns WHAT to draw: the block list, per-block LOD level,
per-block inFrustum flag. It emits `TerrainDrawCommand` structs.

RenderSpine / GameOS layer owns HOW and WHEN draw work enters the GPU pass system.
GameOS executes draw commands, manages GL buffer objects, calls `gl*()`, and
determines pass execution order.

Terrain code does NOT own pass execution order. It does NOT schedule or sequence passes.

### Q2. What data crosses the render spine boundary?

Only `TerrainDrawCommand` structs cross from mclib/ terrain code to the GameOS layer.

```cpp
struct TerrainDrawCommand {
    int blockOriginX;      // vertex-grid origin: blockX * 20
    int blockOriginY;      // vertex-grid origin: blockY * 20
    int lodStep;           // 1, 2, 4, 5, 10, or 20
    int quadCountsPacked;  // (quadCountX & 0xFF) | ((quadCountY & 0xFF) << 8)
                           // GameOS unpacks: qcX = cmd.quadCountsPacked & 0xFF;
                           //                 qcY = (cmd.quadCountsPacked >> 8) & 0xFF;
                           // Range: 1-20. For 1021-vertex map always 20|20<<8.
};
static_assert(sizeof(TerrainDrawCommand) == 16);
```

`TerrainBlockMeta`, `ObjBlockInfo`, `PostcompVertex` pointers, raw GL handles,
projected vertex data, and quad lists do NOT cross the boundary. `quadCountsPacked`
carries the per-block quad dimensions so GameOS can build correct index counts and
pass the right uniforms without accessing any mclib-internal struct.

### Q3. What code is allowed to touch GL / GPU state?

Only GameOS layer code in `GameOS/gameos/` may call `gl*()` directly.
`mclib/` terrain code may NOT call `gl*()`. `mclib/` submits draw commands and calls
GameOS API functions (e.g., `gos_TerrainLodChunk_UploadHeightPatch()`); GameOS
executes the GL work.

If `MC2_RENDER_CONTRACT_ASSERT=1`, any `gl*()` call site outside `GameOS/gameos/`
fires a fatal assert at runtime.

### Q4. What is the fast-path-six contract?

- **Cardinality:** per-block per-frame (one `TerrainDrawCommand` per visible block).
- **Authority:** CPU heightfield `mapData->blocks[].elevation` is ground truth.
  GPU height SSBO at binding 23 is a cached copy. CPU wins on conflict (dirty-patch
  overwrites GPU).
- **Slice kind:** dispatch-changing for Phase 4+ (emits draw calls into GameOS pass
  system); observational for Phase 2 shadow mode (cull only, no draw commands emitted
  to new path).
- **Boundary:** `TerrainDrawCommand` buffer is mclib-internal until submitted via
  GameOS API function. The GameOS API function is the boundary crossing point.
- **Env vars:** `getenv()` at runtime. Listed in `docs/tier1_env_vars.md`.
  Primary gate: `MC2_TERRAIN_LOD_CHUNK`. Assertion gate: `MC2_RENDER_CONTRACT_ASSERT`.
- **Gates:** every invariant listed in the Classification Table has a corresponding
  implementation task that writes it.

### Q5. What cardinality is legal?

- One `TerrainDrawCommand` per visible block per frame. Zero for culled blocks.
- One full height SSBO upload per map load (all `realVerticesMapSide^2` floats).
- One height SSBO dirty-patch per terrain-edited block per frame.
- One AABB cull pass per frame (superchunk then per-block).
- One LOD selection per block per frame.
- Zero per-vertex CPU work in the new draw path. No projection. No `makeLists`.
  No `geometry()` loop.

### Q6. What legacy path remains during shadow mode?

During shadow mode (Phase 2), `makeLists()` + `geometry()` + `TerrainQuad::draw()`
all run unchanged. The new AABB cull runs in parallel but does NOT feed draw calls.
`ObjBlockInfo.active` is still set by the old `geometry()` path during Phase 2.

Shadow mode = `MC2_TERRAIN_LOD_CHUNK=1` but Phase 4 cutover not yet executed.
Bridge state is explicit: old path runs unchanged. New AABB cull (Phase 2) produces
`blockMeta.inFrustum` in parallel; results are compared against `ObjBlockInfo.active`
for validation logging only.

### Q7. What exact env flag gates the new path?

`MC2_TERRAIN_LOD_CHUNK=1` (runtime, `getenv()`, entry required in
`docs/tier1_env_vars.md` before Phase 1 merge).

Flag semantics by phase:
- **Phase 1:** enables `TerrainBlockMeta[]` / `SuperchunkMeta[]` allocation and
  AABB computation at map load. No draw changes.
- **Phase 2:** enables AABB cull in `Terrain::update()`. Old path unchanged.
  Validation log active. Ordered after `geometry()` so `ObjBlockInfo.active` is
  current-frame data when compared.
- **Phase 3:** enables height SSBO upload at load and dirty-patch on edit.
  `calcThisFrame` migration active.
- **Phase 4:** enables GPU chunk draw path. Old draw path disabled when new path
  active. Single fixed LOD (LOD2) initially.
- **Phase 5+:** same gate. Per-block LOD, skirts, etc. build on Phase 4.

Without `MC2_TERRAIN_LOD_CHUNK=1` the old path runs 100% unchanged.

`MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` (runtime, `getenv()`, listed in
`docs/tier1_env_vars.md` alongside `MC2_TERRAIN_LOD_CHUNK`): enables Phase 2+
bridge that writes `objBlockInfo[i].active = s_blockMeta[i].inFrustum` each frame.
This gate is runtime-only; it is never `#ifdef`-gated at compile time.

### Q8. What smoke test proves no boundary violation?

Tier1 smoke at 30s with both `MC2_TERRAIN_LOD_CHUNK=1` AND
`MC2_RENDER_CONTRACT_ASSERT=1` set.

If any terrain code in `mclib/` calls `gl*()` directly, the assert fires with a
fatal error naming the call site. Smoke pass = no assert, no crash, terrain renders.

Invocation:
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py `
    --tier tier1 --duration 30 --keep-logs
```
Run with both env vars set in the shell before invoking.

---

## Classification Table

| Field | Value |
|---|---|
| Slice kind | dispatch-changing |
| Changes pixels | yes |
| Touches GL state | yes — GameOS layer only (`GameOS/gameos/`) |
| Data source | mixed: CPU cull/LOD selection; GPU height fetch via SSBO binding 23 |
| Authority | CPU `mapData->blocks[].elevation` is ground truth; GPU SSBO binding 23 is cached copy |
| Cardinality — draw commands | one per visible block per frame |
| Cardinality — SSBO full upload | one per map load |
| Cardinality — SSBO dirty-patch | one per edited block per frame |
| Cardinality — cull | one AABB test per block per frame |
| Boundary crossed | `TerrainDrawCommand` only (mclib → GameOS API); no mclib structs in `code/` |
| Env gate | `MC2_TERRAIN_LOD_CHUNK=1` (runtime `getenv()`; entry required in `docs/tier1_env_vars.md`) |
| Assertion gate | `MC2_RENDER_CONTRACT_ASSERT=1` fires if mclib calls `gl*()` directly |

### Hard Invariants

| # | Invariant | Implementation task |
|---|---|---|
| I1 | No `gl*()` call in any `mclib/` file. All GL work in `GameOS/gameos/`. | Phase 1: add `MC2_RENDER_CONTRACT_ASSERT` guard; verify with per-file grep before merge |
| I2 | `blockMeta.inFrustum=false` NEVER occurs when `ObjBlockInfo.active=true`. AABB cull is conservative (false positives allowed; false negatives forbidden). | Phase 2: validation log + mismatch counter; zero mismatches required to advance to Phase 3 |
| I3 | Height SSBO content matches `mapData->blocks[].elevation` for all non-dirty blocks at frame start. | Phase 3: first-frame sequential compare on debug build |
| I4 | Exactly one `TerrainDrawCommand` emitted per visible block per frame. No duplicates. `expectedVisible` is a CPU pre-count from the `s_blockMeta` inFrustum scan (CPU accessor, diagnostic-only). | Phase 4: Tracy zone assert `expectedVisible == cmdCount` |
| I5 | `lodStep` in every emitted command is one of {1, 2, 4, 5, 10, 20}. | Phase 5: assert in `chooseLOD`; `static_assert` on `k_lodSteps` table size == 6 |
| I6 | Old `makeLists()` / `geometry()` / `TerrainQuad::draw()` run unmodified when `MC2_TERRAIN_LOD_CHUNK` unset. | Phase 1: tier1 smoke with flag unset must match pre-mod baseline (zero pixel diff) |
| I7 | `TerrainBlockMeta` and `SuperchunkMeta` arrays allocated from `Terrain::terrainHeap`; freed in `Terrain::destroy()`. | Phase 1: heap accounting; sentinel on map reload |
| I8 | Height SSBO dirty-patch writes exactly `(quadCountY+1)` rows of `(quadCountX+1)` floats. No full re-upload on edit. | Phase 3: assert `uploadByteCount == (bm.quadCountY+1)*(bm.quadCountX+1)*sizeof(float)` |

---

## tier1_env_vars.md prereq (Constraint 5)

The following entries MUST be added to `docs/tier1_env_vars.md` in the same Phase 1 commit:

```
## Terrain LOD chunk

- `MC2_TERRAIN_LOD_CHUNK=1` — chunked heightfield LOD renderer. Default **OFF**.
  Phase 1: AABB allocation only. Phase 2: shadow-mode cull. Phase 3: height SSBO.
  Phase 4+: GPU draw path replaces makeLists/geometry()/TerrainQuad::draw().
- `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` — bridge ObjBlockInfo.active from
  blockMeta.inFrustum each frame. Default **OFF**. Requires Phase 2 validation to
  pass (zero false negatives) before enabling. Runtime getenv(), never #ifdef.
```

---

## render-binding-registry.md prereq (Constraint 6)

Slot 23 is currently TBD in `docs/render-binding-registry.md` (no live
`glBindBufferBase(..., 23, ...)` call site found in worktree as of HEAD).
Before Phase 3 merges, the implementer MUST:
1. Grep the worktree for `glBindBufferBase.*23\b` and `layout.*binding.*=.*23` to
   confirm no live holder.
2. In the same commit that introduces `TERRAIN_HEIGHT_SSBO_BINDING = 23u`, update
   `docs/render-binding-registry.md` row 23 from TBD to:

| 23 | `TERRAIN_HEIGHT_SSBO_BINDING` / `TerrainHeightBuf` | terrain LOD chunk | SSBO | `GameOS/gameos/gos_terrain_lod_chunk.cpp` | `shaders/terrain_lod_chunk.vert:layout(binding=23)` | Added Phase 3. Full map float[] upload at load; dirty-patch on edit. |

This registry update is a hard gate for Phase 3 merge (I-registry-23).

---

## Overview & Goal

Replace the O(visibleVertices²) CPU terrain pipeline (`makeLists` -> `geometry` ->
`TerrainQuad::draw`) with a chunked heightfield renderer that scales to 1K x 1K maps.
CPU terrain code decides WHAT to draw (block list, per-block LOD level) by performing
AABB frustum culls and distance-based LOD selection, then emits `TerrainDrawCommand`
structs to the GameOS render spine, which owns HOW and WHEN those commands become GPU
draw calls. The heightfield lives in GPU SSBO binding 23; the vertex shader reconstructs
world positions from it at the chosen LOD stride, eliminating all per-vertex CPU
projection work.

---

## Coordinate System

The MC2 terrain uses centered world coordinates:

```
tileColToWorldCoord[i] = i * worldUnitsPerVertex - worldUnitsMapSide / 2.0   (X = east)
tileRowToWorldCoord[i] = worldUnitsMapSide / 2.0 - i * worldUnitsPerVertex   (Y = north, negated)
```

For a 120-vertex map: `worldUnitsMapSide = 120 * 128 = 15360`, so vertex 0 is at
X = -7680, Y = +7680. For a 1021-vertex map: `worldUnitsMapSide = 1021 * 128 = 130688`,
vertex 0 at X = -65344, Y = +65344.

**Block world-space AABB (correct):**
```cpp
float halfMap = Terrain::worldUnitsMapSide * 0.5f;
float wMinX =  float(bm.originX)                  * 128.0f - halfMap;
float wMaxX =  float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
float wMinY = -(float(bm.originY + bm.quadCountY) * 128.0f - halfMap);  // Y negated
float wMaxY = -(float(bm.originY)                 * 128.0f - halfMap);
```

Note: wMinY < wMaxY because the row-index Y axis is negated; originY gives the
northernmost (largest Y) corner.

**chooseLOD block-center (correct):**
```cpp
float cx = (float(bm.originX) + float(bm.quadCountX) * 0.5f) * 128.0f - halfMap;
float cy = halfMap - (float(bm.originY) + float(bm.quadCountY) * 0.5f) * 128.0f;
```

---

## TerrainBlockMeta Contract

Location: `mclib/terrain.h`, alongside `ObjBlockInfo`.

```cpp
struct TerrainBlockMeta {
    int     originX;     // blockX * 20 (vertex grid index, not world units)
    int     originY;     // blockY * 20
    int     quadCountX;  // min(20, (realVerticesMapSide-1) - originX); 1021→always 20
    int     quadCountY;  // min(20, (realVerticesMapSide-1) - originY); 1021→always 20
    float   minElev;     // min elevation over [originX..originX+quadCountX]
                         //                  x [originY..originY+quadCountY] inclusive
    float   maxElev;     // max elevation over same 21x21 footprint
    bool    dirtyAabb;   // height changed -> recompute AABB + patch SSBO
    bool    inFrustum;   // result of AABB cull this frame
    uint8_t lodLevel;    // 0-5, chosen each frame by distance metric
};
```

**Allocation:** `static TerrainBlockMeta* s_blockMeta` in `Terrain` class.
Allocated from `Terrain::terrainHeap`:
```cpp
s_blockMeta = (TerrainBlockMeta*)terrainHeap->Malloc(
    sizeof(TerrainBlockMeta) * terrainChunkSide * terrainChunkSide);
```
Freed in `Terrain::destroy()`. Array index: `bx + by * terrainChunkSide`.
`terrainChunkSide = (realVerticesMapSide + 18) / 20` (= `ceil(quads/20)`; 1021→51).

**Partial edge blocks:** `quadCountX = min(20, (realVerticesMapSide-1) - originX)`.
Same for `quadCountY`. For a 1021-vertex map: `(1021-1) % 20 = 0`, so all 51
blocks have `quadCountX == quadCountY == 20` — no partial blocks, no OOB samples.
Partial blocks arise only when `(realVerticesMapSide-1) % 20 != 0`.

---

## SuperchunkMeta Contract

Location: `mclib/terrain.h`, alongside `TerrainBlockMeta`.

```cpp
struct SuperchunkMeta {
    float   worldMinX, worldMaxX;  // union AABB of constituent block AABBs (centered world space)
    float   worldMinY, worldMaxY;
    float   worldMinZ, worldMaxZ;
    bool    inFrustum;
    uint8_t _pad[3];
};
```

Superchunk `(scX, scY)` covers blocks `[scX*4 .. scX*4+3]` x `[scY*4 .. scY*4+3]`
clamped to `[0, blocksMapSide)`.

Dimensions: `superchunkSide = (blocksMapSide + 3) / 4`.
For a 1021-vertex map (`blocksMapSide = 51`): `superchunkSide = (51+3)/4 = 13`,
**13x13 = 169 superchunks**. Last row/col of superchunks is partial (covers 3 blocks
instead of 4). NOTE: the design doc figure of 12x12=144 is wrong and has been corrected
here — 12 superchunks covers only 48 blocks, missing the last 3 rows/columns.

**Allocation:** `static SuperchunkMeta* s_superchunkMeta` allocated from
`Terrain::terrainHeap`:
```cpp
s_superchunkMeta = (SuperchunkMeta*)terrainHeap->Malloc(
    sizeof(SuperchunkMeta) * superchunkSide * superchunkSide);
```
Freed in `Terrain::destroy()`.

**AABB union:** `worldMin/MaxX/Y/Z` = min/max over all constituent `TerrainBlockMeta`
world extents (in centered world space). Recomputed when any constituent block has
`dirtyAabb=true`.

---

## TerrainDrawCommand (spine boundary struct)

This is the ONLY data that crosses from terrain (mclib/) to the render spine (GameOS).

```cpp
// mclib/terrain.h (declaration only; no GL includes in this header)
struct TerrainDrawCommand {
    int blockOriginX;      // vertex-grid X origin = blockX * 20
    int blockOriginY;      // vertex-grid Y origin = blockY * 20
    int lodStep;           // one of {1, 2, 4, 5, 10, 20}
    int quadCountsPacked;  // (quadCountX & 0xFF) | ((quadCountY & 0xFF) << 8)
                           // GameOS unpacks for index count + shader uniforms.
                           // No TerrainBlockMeta* crossing the boundary.
};
static_assert(sizeof(TerrainDrawCommand) == 16);
```

**Submission API (declared in `GameOS/gameos/gos_terrain_lod_chunk.h`,
implemented in `GameOS/gameos/gos_terrain_lod_chunk.cpp`):**
```cpp
// Submit block draw commands built this frame.
// cmds: CPU-side array (mclib-owned memory).
// count: number of valid entries. count==0 is a strict no-op (no GL calls).
// GameOS uploads to GL draw-indirect buffer, binds index/height SSBOs, issues draws.
// mclib does NOT call gl*() to accomplish this.
void gos_TerrainLodChunk_SubmitDrawCommands(const TerrainDrawCommand* cmds, int count);

// Upload full height map at load time.
// elevations: float[mapSide * mapSide] row-major.
void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide);

// Patch a single block's rows after terrain edit.
// rowData: float[(quadCountY+1) * (quadCountX+1)] row-major.
void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide);
```

mclib never calls `glBindBuffer`, `glBufferSubData`, `glDrawArraysIndirect`, or any
other `gl*()` function. It calls only the `gos_TerrainLodChunk_*` API.

**Phase 1 stub requirement:** `gos_terrain_lod_chunk.h` and a do-nothing
`gos_terrain_lod_chunk.cpp` implementing all three functions as stubs (no GL calls)
MUST be added in Phase 1 alongside the mclib changes. This ensures Phase 1 compiles
and links without error. The stubs become real implementations in Phases 3-4.

---

## LOD Index Buffer Layout

Six static GPU index buffers, built once inside `gos_TerrainLodChunk_Init()` in
`GameOS/gameos/gos_terrain_lod_chunk.cpp`. `gos_TerrainLodChunk_Init()` is called
from `gos_Init()` (or its terrain-init hook) after the GL context is established.
Freed in `gos_TerrainLodChunk_Destroy()`. Reused for every block draw at that LOD;
only `blockOriginX/Y` and `lodStep` vary per draw (passed as uniforms or packed data).

Local grid: `localGridX` in `[0 .. 20/lodStep - 1]`, same for `localGridY`.
Vertex index within buffer for sample `(gx, gy)`:
```
vertIdx = gx + gy * (20/lodStep + 1)
```

Two CCW triangles per quad:
```
tri0: (gx, gy), (gx, gy+1), (gx+1, gy+1)
tri1: (gx, gy), (gx+1, gy+1), (gx+1, gy)
```

| LOD | lodStep | Grid | Quads | Index count |
|-----|---------|------|-------|-------------|
| 0 | 1 | 20x20 | 400 | 2400 |
| 1 | 2 | 10x10 | 100 | 600 |
| 2 | 4 | 5x5 | 25 | 150 |
| 3 | 5 | 4x4 | 16 | 96 |
| 4 | 10 | 2x2 | 4 | 24 |
| 5 | 20 | 1x1 | 1 | 6 |

Index element type: `uint16_t`. Max index at LOD0 = `21*21 - 1 = 440`; fits uint16.

---

## Height SSBO Interface

**Binding slot: 23.**
Slot 23 has no live `glBindBufferBase` or `layout(binding=23)` site in the current
worktree (verified by grep). Before Phase 3 merge, the implementer must re-run the
grep to confirm it is still free and then update `docs/render-binding-registry.md`
as described in the prereq section above.

Constant defined in `GameOS/gameos/gos_terrain_lod_chunk.h`:
```cpp
constexpr uint32_t TERRAIN_HEIGHT_SSBO_BINDING = 23u;
```

**Format:** `float[]`, row-major, `[x + y * realVerticesMapSide]`.
Total size at 1021-vertex map: `1021 * 1021 * 4 = ~4.17 MB`.

**CPU ground truth:** `mapData->blocks[x + y * realVerticesMapSide].elevation`.
GPU SSBO is a cache. CPU wins on conflict: dirty-patch overwrites GPU.

**Full upload at map load:**
```cpp
// In Terrain::init() after mapData loaded (Phase 3+, gated MC2_TERRAIN_LOD_CHUNK=1):
int n = realVerticesMapSide * realVerticesMapSide;
std::vector<float> elev(n);
for (int i = 0; i < n; ++i)
    elev[i] = mapData->blocks[i].elevation;
gos_TerrainLodChunk_UploadHeightFull(elev.data(), realVerticesMapSide);
```

**Dirty-patch on terrain edit:**
```cpp
// When block (bx, by) has dirtyAabb=true after setVertexHeight():
int rows = bm.quadCountY + 1, cols = bm.quadCountX + 1;
std::vector<float> patch(rows * cols);
for (int dy = 0; dy < rows; ++dy)
    for (int dx = 0; dx < cols; ++dx)
        patch[dx + dy * cols] = mapData->blocks[
            (bm.originX+dx) + (bm.originY+dy) * realVerticesMapSide].elevation;
gos_TerrainLodChunk_UploadHeightPatch(
    patch.data(), bm.originX, bm.originY,
    bm.quadCountX, bm.quadCountY, realVerticesMapSide);
```

**Vertex shader access (GLSL) — uses centered world space:**
```glsl
layout(binding = 23, std430) readonly buffer TerrainHeightBuf {
    float heights[];
};

// Uniforms supplied by GameOS from TerrainDrawCommand data:
// uniform int  u_blockOriginX;     // = blockOriginX from command
// uniform int  u_blockOriginY;     // = blockOriginY from command
// uniform int  u_lodStep;          // = lodStep from command
// uniform int  u_blockQuadCountX;  // = cmd.quadCountsPacked & 0xFF
// uniform int  u_blockQuadCountY;  // = (cmd.quadCountsPacked >> 8) & 0xFF
// uniform int  u_mapSide;          // = realVerticesMapSide
// uniform float u_halfMap;         // = worldUnitsMapSide * 0.5

// In vertex shader main():
// gridW = quads-per-row/lodStep + 1 vertices per row
int gridW      = u_blockQuadCountX / u_lodStep + 1;
int localGridX = gl_VertexID % gridW;
int localGridY = gl_VertexID / gridW;
int mapX = clamp(u_blockOriginX + localGridX * u_lodStep, 0, u_mapSide - 1);
int mapY = clamp(u_blockOriginY + localGridY * u_lodStep, 0, u_mapSide - 1);
float h = heights[mapX + mapY * u_mapSide];

// Centered world space: col=X (east), row=Y (north, negated)
float worldX = float(mapX) * 128.0 - u_halfMap;
float worldY = u_halfMap - float(mapY) * 128.0;  // Y negated: row 0 = north
vec3 worldPos = vec3(worldX, worldY, h);
```

**OOB:** vertex shader clamps `mapX`/`mapY` to `[0, mapSide-1]` before indexing.
Edge samples use the actual border vertex elevation.

---

## Per-frame CPU Loop

Inserted into `Terrain::update()` under `MC2_TERRAIN_LOD_CHUNK=1` guard.
Ordering: runs AFTER `makeLists()` and `geometry()` in Phase 2 (so `ObjBlockInfo.active`
reflects current-frame data when the validation comparison occurs). In Phase 4+, the old
path is disabled and ordering relative to it is moot; the new loop runs in its place.

```cpp
// Terrain::update(), new path -- gated on MC2_TERRAIN_LOD_CHUNK=1.
// Phase 2: runs after geometry() so ObjBlockInfo.active is current-frame.
// Phase 4+: runs instead of makeLists()/geometry()/TerrainQuad::draw().

if (!getenv("MC2_TERRAIN_LOD_CHUNK")) goto legacy_path;  // runtime gate

++gCurrentFrame;  // starts at 1; never reset

// s_drawCmds: heap-allocated pointer, size blocksMapSide*blocksMapSide entries.
// Allocated in Terrain::init() alongside s_blockMeta:
//   s_drawCmds = (TerrainDrawCommand*)terrainHeap->Malloc(
//       sizeof(TerrainDrawCommand) * blocksMapSide * blocksMapSide);
// Freed in Terrain::destroy().
int cmdCount = 0;

// Cache frustum planes once per frame (uses Camera::cacheFrustumPlanes()).
// eye->cacheFrustumPlanes() is already called earlier in Terrain::update()
// for the legacy path; verify it runs before this loop under the new path too.
const float (*planes)[4] = eye->getCachedFrustumPlanes();

float halfMap = Terrain::worldUnitsMapSide * 0.5f;

// Superchunk coarse cull.
int scSide = (blocksMapSide + 3) / 4;
for (int scY = 0; scY < scSide; ++scY) {
    for (int scX = 0; scX < scSide; ++scX) {
        SuperchunkMeta& sc = s_superchunkMeta[scX + scY * scSide];
        // Camera::quadAabbInFrustum: p-vertex conservative AABB-vs-frustum test.
        // Declared in mclib/camera.h; planes from cacheFrustumPlanes().
        Stuff::Vector3D scMn(sc.worldMinX, sc.worldMinY, sc.worldMinZ);
        Stuff::Vector3D scMx(sc.worldMaxX, sc.worldMaxY, sc.worldMaxZ);
        sc.inFrustum = eye->quadAabbInFrustum(planes, scMn, scMx);
        if (!sc.inFrustum) continue;

        // Per-block cull and LOD within visible superchunk.
        for (int dy = 0; dy < 4; ++dy) {
            for (int dx = 0; dx < 4; ++dx) {
                int bx = scX * 4 + dx, by = scY * 4 + dy;
                if (bx >= blocksMapSide || by >= blocksMapSide) continue;
                TerrainBlockMeta& bm = s_blockMeta[bx + by * blocksMapSide];

                // World-space AABB in centered coordinates.
                float wMinX =  float(bm.originX)                  * 128.0f - halfMap;
                float wMaxX =  float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
                float wMaxY =  halfMap - float(bm.originY)                  * 128.0f;
                float wMinY =  halfMap - float(bm.originY + bm.quadCountY)  * 128.0f;

                Stuff::Vector3D bmMn(wMinX, wMinY, bm.minElev);
                Stuff::Vector3D bmMx(wMaxX, wMaxY, bm.maxElev);
                bm.inFrustum = eye->quadAabbInFrustum(planes, bmMn, bmMx);
                if (!bm.inFrustum) continue;

                bm.lodLevel = chooseLOD(cameraPos, bm, halfMap);

                static const int k_lodSteps[6] = {1, 2, 4, 5, 10, 20};
                s_drawCmds[cmdCount++] = {
                    bm.originX, bm.originY,
                    k_lodSteps[bm.lodLevel],
                    (bm.quadCountX & 0xFF) | ((bm.quadCountY & 0xFF) << 8)
                };
            }
        }
    }
}

// Phase 2 validation (shadow mode): compare with ObjBlockInfo.active from old path.
// Runs only in Phase 2 (old path still active). Throttled: logs at most once per 60
// frames per validation session to keep logs usable.
{
    static int s_validFrames = 0;
    static int s_logThrottle = 0;
    int falseNegatives = 0;
    for (int i = 0; i < blocksMapSide * blocksMapSide; ++i) {
        if (!s_blockMeta[i].inFrustum && objBlockInfo[i].active)
            ++falseNegatives;
    }
    if (falseNegatives > 0) {
        ++s_logThrottle;
        if (s_logThrottle >= 60) {
            s_logThrottle = 0;
            MC2_LOG("TerrainLODChunk: %d false negatives -- Phase 3 advance BLOCKED",
                    falseNegatives);
        }
    }
}

// Phase 2+: bridge ObjBlockInfo.active from blockMeta (runtime gate, not #ifdef).
// Enable only after Phase 2 validation confirms zero false negatives.
if (getenv("MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK")) {
    for (int i = 0; i < blocksMapSide * blocksMapSide; ++i)
        objBlockInfo[i].active = s_blockMeta[i].inFrustum;
}

// Phase 4+: submit to GameOS. Guarded by explicit phase check so Phases 1-3
// produce zero GL output. count==0 is also a strict no-op per gos contract.
// Replace the phase-guard with a real flag once Phase 4 is active.
#if MC2_TERRAIN_LOD_CHUNK_PHASE >= 4
gos_TerrainLodChunk_SubmitDrawCommands(s_drawCmds, cmdCount);
#endif
// Note: MC2_TERRAIN_LOD_CHUNK_PHASE is a CMake build-time constant set per phase.
// It is NOT a runtime env var. The runtime env var MC2_TERRAIN_LOD_CHUNK controls
// whether this block runs at all; the phase constant controls which features within
// it are compiled. In Phase 4 build, the call is compiled in but count==0 when all
// blocks are culled, which is still a no-op per the gos contract.
```

---

## AABB Computation

Called at map load (all blocks) and whenever `blockMeta.dirtyAabb=true` (edited blocks).
World-space values use centered coordinates (see Coordinate System section).

```cpp
void recomputeBlockAabb(TerrainBlockMeta& bm) {
    bm.minElev =  FLT_MAX;
    bm.maxElev = -FLT_MAX;

    // Inclusive: covers own 21x21 footprint including seam edge samples.
    for (int y = bm.originY; y <= bm.originY + bm.quadCountY; ++y) {
        for (int x = bm.originX; x <= bm.originX + bm.quadCountX; ++x) {
            float elev;
            if (x >= Terrain::realVerticesMapSide || y >= Terrain::realVerticesMapSide)
                elev = 33.0f;  // blankVertex elevation
            else
                elev = mapData->blocks[x + y * Terrain::realVerticesMapSide].elevation;
            if (elev < bm.minElev) bm.minElev = elev;
            if (elev > bm.maxElev) bm.maxElev = elev;
        }
    }
    bm.dirtyAabb = false;
}

// World-space AABB (centered coordinates, passed to quadAabbInFrustum):
// float halfMap = Terrain::worldUnitsMapSide * 0.5f;
// wMinX = float(bm.originX)                  * 128.0f - halfMap;
// wMaxX = float(bm.originX + bm.quadCountX)  * 128.0f - halfMap;
// wMinY = halfMap - float(bm.originY + bm.quadCountY) * 128.0f;  // Y negated
// wMaxY = halfMap - float(bm.originY)                 * 128.0f;
// wMinZ = bm.minElev
// wMaxZ = bm.maxElev
```

Superchunk AABB union: iterate constituent blocks, expand running min/max of their
world-space AABBs. Recomputed whenever any constituent block is recomputed.

---

## Map Size Validator

`terrain.cpp:297-302` contains a validator (PAUSE/soft-assert) for unsupported map sizes.
The Phase 1 commit must add 1021 to the supported-sizes list. The PAUSE behavior is
preserved (not escalated to hard assert) so tools that probe the validator path continue
to function.

---

## calcThisFrame Migration

Phase 3 replaces `Vertex::calcThisFrame` (bitmask + sweep reset) with two
frame-stamp fields (no sweep required).

**Changes to `Vertex` struct (`mclib/vertex.h`):**
```cpp
// Remove:
DWORD calcThisFrame;

// Add:
uint32_t lastLitFrame;    // frame number when lighting last computed; init to 0
uint32_t lastWaterFrame;  // frame number when water last computed; init to 0
```

**Frame counter (add to `Terrain` class in `mclib/terrain.h`):**
```cpp
static uint32_t gCurrentFrame;  // incremented once per Terrain::update(); init to 1
```

**Guard replacement in `setupTextures()` / equivalent callers:**
```cpp
// Old (requires full-window reset sweep in makeLists):
if (!(v->calcThisFrame & 1)) { calcLighting(v); v->calcThisFrame |= 1; }
if (!(v->calcThisFrame & 2)) { calcWater(v);    v->calcThisFrame |= 2; }

// New (no sweep; stale entries fire on first touch this frame):
if (v->lastLitFrame   != gCurrentFrame) { calcLighting(v); v->lastLitFrame   = gCurrentFrame; }
if (v->lastWaterFrame != gCurrentFrame) { calcWater(v);    v->lastWaterFrame = gCurrentFrame; }
```

`Vertex::init()` sets `lastLitFrame = lastWaterFrame = 0`.
`gCurrentFrame` inits to 1 in `Terrain::init()`. First touch always fires (0 != 1).
Overflow at 60 fps: ~2.26 years (uint32_max / 60 / 3600 / 24 / 365 ≈ 2.26).
At wrap, one frame of redundant recalc (correct). NOTE: The design doc states 136
years — this is wrong; that would require uint64. The correct figure is 2.26 years.

`calcThisFrame` is NOT deleted until Phase 8. The old path continues to use it
during Phases 3-7. The new frame-stamp fields coexist in the struct.

---

## Shadow Mode Validation (Phase 2)

Phase 2 runs the new AABB cull alongside the unchanged `geometry()` path and logs
any false negatives. The validation loop runs AFTER `geometry()` so `ObjBlockInfo.active`
is current-frame data.

> **2026-06-08:** Phase 8 re-scoped deletion → dependency EXTRACTION. The
> "retired in Phase 8" components are the FINAL deletion step (8z), gated on
> extracting live non-render consumers first. See the "DEPENDENCY TRUTH" block
> in `terrain-lod-chunk-plan.md` (Phase 8): it corrects the false premise that
> Phase 4 suppresses `geometry()` — it does NOT; `geometry()` runs
> unconditionally (mission.cpp:569) and is the sole producer of
> `objBlockInfo.active`, read by `objmgr.cpp:2193` for object lifecycle.

Throttling: log at most once per 60 frames (see per-frame CPU loop above).

Validation passes when: zero false negatives across the tier1 5-mission smoke at 30s.
Phase 3 may not proceed until validation passes.

Conservative direction (permitted): `blockMeta.inFrustum=true`, `ObjBlockInfo.active=false`.
This means the new cull returns visible when the old vertex-cull returns invisible.
Benign — slightly more draw commands, no missing geometry.

---

## Visibility Separation Contract

| Flag | Owner | Used by |
|---|---|---|
| `blockMeta.inFrustum` | terrain render | GPU draw command emission (new path, Phase 4+) |
| `ObjBlockInfo.active` | object system | object activation / deactivation; bridged from `blockMeta.inFrustum` after Phase 2 validation |

Mine enqueue (Phase 7): walk visible block list (`s_blockMeta[i].inFrustum`),
query mine grid by tile coords. Does NOT call `setupTextures()` as a side effect.

Picking (Phase 7): heightfield raycast using `mapData->blocks[].elevation` at LOD0
resolution regardless of rendered LOD. Does NOT use projected `quadList` or
`Camera::inverseProject()` clip/pz data.

Object activation (Phase 2+): after Phase 2 validation confirms zero false negatives,
`ObjBlockInfo.active[i] = s_blockMeta[i].inFrustum` replaces the old `geometry()`
side-effect setter. Gate: runtime `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1`.

---

## Legacy Bridge State

Components that run unchanged until explicitly retired:

| Component | File | Exit phase |
|---|---|---|
| `MapData::makeLists()` | `mclib/mapdata.cpp` | Phase 8 |
| `Terrain::geometry()` (both loops) | `mclib/terrain.cpp` | Phase 8 |
| `TerrainQuad::draw()` CPU rasterization | `mclib/quad.cpp` | Phase 8 |
| `Vertex::calcThisFrame` field | `mclib/vertex.h` | Phase 8 |
| `ObjBlockInfo.active` set by `geometry()` | `mclib/terrain.cpp` | Phase 2 -> bridged; field kept Phase 8 |

All bridge state runs unmodified when `MC2_TERRAIN_LOD_CHUNK=0`.
All bridge state runs unmodified during Phases 1-2 regardless of flag.
Bridge state is deleted in Phase 8 in a single cleanup commit.
Per render-contract Rule 5: bridge state is explicitly called out here with an exit plan.

---

## Render Contract Compliance

**R1 -- World-space authoritative:** vertex shader reconstructs world position from
`heights[mapX + mapY * mapSide]` with centering offset applied. No CPU `projectZ`.
No projected vertex coordinates cross the boundary.

**R2 -- Visibility = GPU clip-space:** AABB CPU pre-cull is conservative (reduces
draw call count). GPU vertex shader performs exact clip-space discard. No CPU-side
exact pixel visibility gating.

**R3 -- Shadow deferred:** shadow stream integration deferred to Phase 10. No shadow
work in Phases 1-9. New terrain path emits no shadow draw commands.

**R4 -- No `projectZ` calls on new path:** no projection functions called in the new
code paths. `Vertex::px/py/pz/pw` not written by the new path.

**R5 -- Bridge state:** old path (`makeLists` / `geometry` / `TerrainQuad::draw`) is
called out above in Legacy Bridge State with explicit exit plan Phase 8.

**R6 -- RenderPassId:** The new chunk draw path does not claim `RenderPassId=2`
(terrain passive, `viewUniformsBound=N`, `pipelineDescRegistered=N`,
`snapshotRowAuthoritative=N`). Phase 4 registers a new PassId for the LOD chunk draw.
The registry entry for that PassId is added in the same Phase 4 commit.

---

## LOD Distance Selection

```cpp
// Returns lod index 0-5.
// halfMap: Terrain::worldUnitsMapSide * 0.5f
uint8_t chooseLOD(const Stuff::Vector3D& camPos, const TerrainBlockMeta& bm, float halfMap) {
    // Block center in centered world space.
    float cx = (float(bm.originX) + float(bm.quadCountX) * 0.5f) * 128.0f - halfMap;
    float cy = halfMap - (float(bm.originY) + float(bm.quadCountY) * 0.5f) * 128.0f;
    float dx = camPos.x - cx, dy = camPos.y - cy;
    float d  = sqrtf(dx*dx + dy*dy);

    if      (d <  3000.0f) return 0;
    else if (d <  7000.0f) return 1;
    else if (d < 15000.0f) return 2;
    else if (d < 35000.0f) return 3;
    else if (d < 80000.0f) return 4;
    else                   return 5;
}
```

Thresholds in world units. Tuned after first render bring-up (Phase 4).

**Neighbor delta clamp (Phase 5, pre-skirts):** after initial LOD assignment,
clamp so no adjacent blocks differ by more than 1 LOD level. Prevents worst-case
crack gaps before skirts are added. Applied as a second pass over `s_blockMeta`
before command emission.

---

## Out of Scope

The following are explicitly NOT part of this spec:

- Normal SSBO / tangent-space lighting from heightfield (Phase 10)
- Material / texture-ID streaming to new terrain path
- Light SSBO integration with new terrain path (Phase 10)
- Shadow map sampling from new terrain path (Phase 10)
- Superchunk coarse draw (16 x LOD5 blocks -> 1 draw command) (Phase 9)
- Screen-space error LOD metric (post-Phase 5 refinement)
- Stitch index buffers (T-junction elimination without skirts; not planned)
- Water rendering integration with new terrain path
- Editor terrain deformation performance path
- Texture splatting / PBR material integration
