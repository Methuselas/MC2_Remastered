# Terrain LOD Chunk System — Design Document

**Date:** 2026-06-07  
**Goal:** Support 1K×1K maps with in-game zoom-out via chunked heightfield LOD.  
**Status:** Design / pre-implementation. No code changes yet.

---

## Context

Current terrain pipeline:
```
makeLists() → geometry() → TerrainQuad::draw() per quad
```

All O(visibleVerticesPerSide²) CPU work. Cannot scale to 1K maps. At 1024² visible
vertices: ~12ms geometry() projection alone, likely worse due to cache misses.

Target: replace with chunked heightfield renderer. CPU decides what to draw and at what
LOD; GPU draws terrain patches from a shared heightfield buffer.

---

## Hierarchy

```
Map (1K×1K)
└── Superchunks (4×4 blocks = 80×80 quads each)
    └── Blocks / leaf chunks (20×20 quads, 21×21 height samples)
        └── Height samples in blocks[] PostcompVertex array
```

**Blocks** = draw unit. One indirect draw call per visible block at its LOD.  
**Superchunks** = culling/management group. Fast "whole region is far/invisible" decision.  
**Map** = full-map zoom-out via LOD chunk terrain mesh (LOD4/LOD5 at extreme distance).
Tacmap remains an overlay/fallback/debug view, not the primary terrain render.

---

## Block Semantics (ground truth from recon)

### Vertex vs quad count

`verticesBlockSide = 20` means **20 vertices per side** (indices 0..19).

| Metric | Value |
|---|---|
| Vertices per block | 20×20 = 400 |
| Internal quads | 19×19 = 361 |
| Seam quads (at +X/+Y edge) | 1 row + 1 col = 39 more quads, owned by this block |
| Total quads owned | 20×20 = 400 (19×19 internal + seam row + seam col) |
| Block stride (origin-to-origin) | 20 × 128 = 2560 world units = `worldUnitsBlockSide` |

**Seam quad ownership:** a quad is keyed to the block of its v0 (top-left vertex). Seam
quads on the +X and +Y edges of block B have v0 inside block B but use vertex 0 of the
adjacent block for v1/v2. This is the existing makeLists behavior.

### Render footprint = 21×21

To draw all 400 quads owned by a block (including seam quads), the render chunk needs:
- 20×20 vertices from own block (indices 0..19)
- 1 extra vertex from each +X and +Y neighbor (their index 0)

**Render footprint: 21×21 height samples.** AABB must also cover this 21×21 range.

### Block indexing

```cpp
blockX   = absVertexX / 20;
blockY   = absVertexY / 20;
blockNum = blockX + blockY * terrainChunkSide;

// Block world origin:
originWorldX = blockX * 20 * 128;   // = blockX * worldUnitsBlockSide
originWorldY = blockY * 20 * 128;
```

**Two distinct counts — do not conflate:**

| Name | Formula | Purpose | 1021 result |
|---|---|---|---|
| `terrainChunkSide` | `(vertices+18)/20` = `ceil(quads/20)` | render chunk grid (LOD draw units) | 51 |
| `objectBlockSide` | `vertices/20` (truncating) | ObjBlockInfo[] vertex buckets | 51 |

For the 1021-vertex target, both agree at 51 — no divergence. The 1021 value is chosen
deliberately so `(1021-1)%20 = 0`: 51 clean blocks, all `quadCountX=20`, no partial blocks,
no OOB samples. For old maps (60, 80, 100, 120), both formulas also agree. They diverge only
for maps where `(vertices-1)%20 != 0` (e.g., 1020-vertex map → last block quadCountX=19).

`terrainChunkSide = (realVerticesMapSide + 18) / 20` in integer arithmetic,
  which equals `ceil((realVerticesMapSide-1) / 20)` = `ceil(quads / verticesBlockSide)`.
  1021→51, 120→6, 60→3.

---

## Map Sizing for 1K

Current supported sizes: 60, 80, 100, 120 vertex sides. All multiples of 20.

For 1K target:
- **1021 vertices per side** → 1020 quads per side → 51 clean blocks of 20 quads → 51×51 = 2601 blocks
- Store realVerticesMapSide = 1021 in map format. New supported size.
- OOB vertices (if camera window extends past edge) → existing `blankVertex` clamp already handles.

Alternative: 1020 vertices (51*20 exactly) but last seam quad on +X/+Y edge would reference OOB → blankVertex. Acceptable but cleaner to use 1021.

---

## LOD System

### Method: stride-based grid sampling (geomipmapping)

No per-frame mesh simplification. No arbitrary triangle decimation. Just draw a coarser
grid over the same heightfield by increasing the step between sampled vertices.

Shared index buffers — one set per LOD level, reused for every block. Only block origin
and LOD level vary per draw.

### LOD levels for 20-quad block

Steps that divide 20 evenly: 1, 2, 4, 5, 10, 20.

| LOD | Step | Grid | Quads/block | Triangles/block |
|---|---|---|---|---|
| LOD0 | 1 | 20×20 | 400 | 800 |
| LOD1 | 2 | 10×10 | 100 | 200 |
| LOD2 | 4 | 5×5 | 25 | 50 |
| LOD3 | 5 | 4×4 | 16 | 32 |
| LOD4 | 10 | 2×2 | 4 | 8 |
| LOD5 | 20 | 1×1 | 1 | 2 |  ← panic button for extreme zoom-out

Sample positions per edge: 0, step, 2×step, ..., 20. Always hits the far edge at 20.
All steps (1,2,4,5,10,20) divide 20 evenly — clean index buffers, no fractional sampling.

**Full 1K map at LOD4:** 2601 × 8 = **20,808 triangles**.  
**Full 1K map at LOD5:** 2601 × 2 = **5,202 triangles**.  
At these counts terrain is not the bottleneck — draw command count and object clutter dominate.

### LOD selection (first version: distance-based)

```cpp
float d = length(cameraPos.xy - blockCenter.xy);

int lod;
if      (d < 3000)  lod = 0;
else if (d < 7000)  lod = 1;
else if (d < 15000) lod = 2;
else if (d < 35000) lod = 3;
else if (d < 80000) lod = 4;
else                lod = 5;    // full 1K zoom-out
```

Thresholds in world units. Tune after first render bring-up. Later replace with
screen-space error metric for camera-angle-correct LOD.

**Neighbor LOD delta constraint:** during bring-up, clamp so adjacent blocks differ by
at most 1 LOD level. Prevents worst-case crack gaps before skirts are in.

---

## GPU Pipeline

```
CPU each frame:
  1. clearObjBlocksActive() — reset visibility
  2. For each block (2601 for 1K map):
       frustumCull(block.aabb) → skip or mark visible
       lod = chooseLOD(camera, block)
       emit DrawCommand { blockOriginX, blockOriginY, lod }
  3. Upload DrawCommand buffer to GPU
  4. GPU draws all visible blocks via indirect draw
     Vertex shader: reads height from heightSSBO[mapX + mapY * mapSide]

GPU vertex shader (sketch):
  int mapX = blockOriginX + localGridX * lodStep;
  int mapY = blockOriginY + localGridY * lodStep;
  float h  = heightSSBO[mapX + mapY * mapSide];
  worldPos = float3(mapX * 128.0, mapY * 128.0, h);
```

CPU cost for 2601-block frame: 2601 AABB frustum tests + LOD distance computes.
Negligible. No per-vertex projection. No makeLists. No geometry() loop.

---

## What to Bake vs Not

### Baked/cached (compute once or on dirty):

| Data | When | Where |
|---|---|---|
| Per-block AABB (minElev, maxElev, worldXYZ bounds) | Map load + terrain edit | `TerrainBlockMeta[]` CPU array |
| Superchunk AABB (union of 4×4 block AABBs) | Map load + any block dirty | `SuperchunkMeta[]` CPU array |
| Height SSBO / texture | Map load + terrain edit dirty regions | GPU buffer, patch dirty blocks only |
| Normal SSBO/texture | Map load + lighting recalc | GPU buffer |
| Material/texture-ID texture | Map load | GPU texture |
| Light SSBO | Lighting recalc | GPU buffer |
| Shared LOD index buffers (LOD0–LOD5) | Once at init | GPU static buffers |

### Never baked per-frame:

- Per-visible vertex list (makeLists output)
- Per-visible quad list
- Per-block unique mesh geometry
- Simplified mesh via CPU decimation

---

## New Data Structures

### TerrainBlockMeta (new, parallel to ObjBlockInfo)

```cpp
struct TerrainBlockMeta {
    int     originX;        // blockX * 20 (vertex index, not world units)
    int     originY;        // blockY * 20
    int     quadCountX;     // = min(20, (realVerticesMapSide-1) - originX); 1021→always 20
    int     quadCountY;     // = min(20, (realVerticesMapSide-1) - originY); 1021→always 20

    float   minElev;        // min elevation over 21×21 sample footprint
    float   maxElev;        // max elevation over 21×21 sample footprint

    bool    dirtyAabb;      // height changed → recompute AABB + patch SSBO
    bool    inFrustum;      // result of AABB cull this frame

    uint8_t lodLevel;       // 0-5, chosen each frame
};
// Allocated: TerrainBlockMeta blockMeta[blocksMapSide * blocksMapSide];
```

AABB scan — inclusive of seam edge samples:
```cpp
for (int y = originY; y <= originY + quadCountY; ++y)   // note: <=, not <
    for (int x = originX; x <= originX + quadCountX; ++x)
        sample blocks[x + y * realVerticesMapSide].elevation (clamp OOB to 33.0f)
```

### SuperchunkMeta (new)

```cpp
struct SuperchunkMeta {
    float   worldMinX, worldMaxX;   // union of 4×4 block AABBs
    float   worldMinY, worldMaxY;
    float   worldMinZ, worldMaxZ;
    bool    inFrustum;
};
// Superchunk at (scX, scY) covers blocks [scX*4 .. scX*4+3] × [scY*4 .. scY*4+3]
// For 51-block-side map: superchunkSide = (51+3)/4 = 13 → 13×13 = 169 superchunks (ceiling division)
```

Superchunk cull happens first. If superchunk AABB is outside frustum, skip all 16 blocks.
If inside, test each block individually.

---

## calcThisFrame Migration

**Field is live and load-bearing.** 9 reads, 8 writes in `setupTextures()`. Acts as
per-vertex lazy-eval dedup: prevents redundant lighting and water projection when a vertex
is shared by multiple adjacent quads in the same frame.

**Do not delete yet.** Required until setupTextures()/CPU quad draw path fully retired
(Phase 7 at earliest).

**Migration (Phase 3):** replace bit-clearing pattern with frame generation counter.

Old pattern (requires full-window clear in makeLists):
```cpp
// makeLists:  v->calcThisFrame = 0;
// setupTextures (water guard only — no lighting guard exists in current codebase):
if (!(v->calcThisFrame & 2)) { calcWater(v); v->calcThisFrame |= 2; }
```

New pattern (no sweep needed):
```cpp
// In Vertex struct — add alongside DWORD calcThisFrame (removed Phase 8):
uint32_t lastWaterFrame;    // frame number when water last computed
// NOTE: lastLitFrame is NOT added — calcThisFrame & 1 lighting guard does not
//       exist in the current codebase. Migration scope is water-only.

// In setupTextures:
if (v->lastWaterFrame != gCurrentFrame) { calcWater(v); v->lastWaterFrame = gCurrentFrame; }
```

No reset sweep. Stale entries are harmless — first touch this frame updates the counter.
Overflow at 60fps: uint32_max / 60 / 3600 / 24 / 365 ≈ 2.26 years. Use uint64 if longer
runtime without restart is required. Uninitialized vertices start at 0; gCurrentFrame
starts at 1 so first touch always fires correctly.

This removes the last reason makeLists must run before setupTextures.

---

## Crack Handling

Adjacent blocks at different LOD levels create vertex gaps at shared edges.

**Phase 4 (single LOD):** no cracks — all blocks at same LOD.

**Phase 5 (mixed LOD, pre-skirts):** enforce neighbor LOD delta ≤ 1. Reduces worst-case
gap size. Still has visible cracks but survivable for validation.

**Phase 6:** add skirts. Each block emits a thin downward fringe around its perimeter,
hiding cracks below neighboring surfaces. Enables unrestricted mixed LOD.

Skirt geometry: for each block edge, emit a quad strip using edge vertices extruded
downward by `maxElev - minElev + margin`. Four strips per block (±X, ±Y edges). Simple
to generate; added as extra primitives after the main LOD mesh.

**Not planned (unless skirts fail):** stitch index buffers keyed per neighbor-LOD
combination (4 edges × 5 LOD choices = many buffer variants). Significant complexity.
Only if skirts produce unacceptable visual artifacts.

---

## Superchunk Strategy

**First bring-up:** don't use superchunks for drawing. Draw all visible blocks
individually. At LOD4, 2601 blocks × 8 triangles = 20K triangles — indirect draw handles
this fine.

**Later optimization:** if all 16 blocks in a superchunk want LOD4, replace 16 draw
commands with 1 superchunk draw command (one large coarse patch). Reduces draw command
count. Not needed for correctness; only for performance at extreme zoom-out.

**Object visibility:** `ObjBlockInfo.active` driven by `blockMeta.inFrustum` after block
AABB cull. Keep separate from terrain LOD visibility — objects may have independent
activation rules (tactical radius, gameplay state, etc.).

---

## Mine / Picking Migration

### Mines

Currently: `TerrainQuad::setupTextures()` calls `GameMap->getMine()` for all 9 cells per
quad, every frame. Tied to CPU quad traversal.

After terrain LOD: walk visible block list, query mine grid directly from tile coords.
Mine enqueue does not require vertex projection. Decouple from setupTextures().

### Picking

Currently: `Camera::inverseProject()` scans projected CPU quadList for clipInfo/pz.

After terrain LOD: heightfield raycast.
```
ray from camera → intersect terrain XY plane bounds
march through heightfield cells
test two triangles per cell
binary refine at terrain crossing
return nearest hit
```
Always use LOD0 heightfield for picking precision regardless of rendered LOD.

---

## Phase Plan

| Phase | Description | Key outcome |
|---|---|---|
| **1** | Add `TerrainBlockMeta[]` + `SuperchunkMeta[]`. Compute AABBs from 21×21 footprint at map load. Add `dirtyAabb` flag. Support partial edge blocks. | Per-block AABB exists |
| **2** | Block AABB cull runs in **shadow mode**: sets `blockMeta.inFrustum`, old `geometry()` still sets `ObjBlockInfo.active`. Log mismatches. After validation, bridge: `ObjBlockInfo.active = blockMeta.inFrustum`. Separate terrain vs object visibility flags explicitly. | Cull validated, safe to proceed |
| **3** | Upload `blocks[].elevation` → GPU height SSBO. Patch dirty blocks on terrain edit. Migrate `calcThisFrame` → `lastLitFrame`/`lastWaterFrame` frame stamps. Remove makeLists reset dependency. | GPU height ready; no sweep reset |
| **4** | Build LOD0–LOD5 shared index buffers (steps 1,2,4,5,10,20). GPU chunk draw path: one indirect draw per visible block at single fixed LOD (LOD2 or LOD3). Vertex shader samples height SSBO. Old path still runs for validation. | Terrain renders without makeLists/geometry() |
| **5** | Enable per-block LOD distance selection. Enforce neighbor delta ≤ 1. | Per-block LOD working (cracks acceptable) |
| **6** | Add skirts. Enable unrestricted mixed LOD. | Crack-free mixed LOD |
| **7** | Decouple mine enqueue from `setupTextures()` (walk visible block list). Replace `inverseProject()` with heightfield raycast. Object/mine visibility fully separated from terrain render path. | Side-effects migrated |
| **8** | Delete `makeLists()`. Delete `geometry()` Loop 1. Delete `TerrainQuad::draw()` CPU path. Delete `calcThisFrame` field. | Old path dead |
| **9** | Superchunk coarse draw (optional): 16 LOD5 blocks → 1 superchunk draw command. | Draw count reduced at extreme zoom |
| **10** | Normal/material/light/shadow SSBOs. Dirty propagation for terrain edits. | Full fidelity |

---

## Key Constants (current)

```cpp
Terrain::verticesBlockSide   = 20          // vertices per block side
Terrain::worldUnitsPerVertex = 128.0f      // world units between vertices
Terrain::worldUnitsBlockSide = 2560.0f     // = 20 * 128, block origin stride
Terrain::terrainChunkSide    = (realVerticesMapSide + 18) / 20  // = ceil((vertices-1)/20) = ceil(quads/20)
                                                                   // 1021 → 51, 120 → 6, 60 → 3
Terrain::realVerticesMapSide = 120 (current), 1021 (target 1K)

// New:
superchunkSide               = 4           // blocks per superchunk side
superchunkWorldSize          = 4 * 2560 = 10240 world units
```

---

## References

- Recon: `mclib/mapdata.cpp:757–906` — makeLists() full body
- Recon: `mclib/terrain.cpp:943–1144` — geometry() two-phase structure
- Recon: `mclib/quad.cpp:108–1433` — setupTextures() calcThisFrame reads/writes
- Recon: `mclib/terrain.h:89–95` — ObjBlockInfo struct
- Recon: `mclib/terrain.cpp:71,80` — verticesBlockSide, worldUnitsBlockSide
- Recon: `mclib/vertex.h:32–63` — PostcompVertex struct
- Related: `docs/terrain-rv-arc-recon.md`, `docs/terrain-rv-arc-audit.md`
