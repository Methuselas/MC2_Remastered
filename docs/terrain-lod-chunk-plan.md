# Terrain LOD Chunk System — Implementation Plan

**Date:** 2026-06-07
**Spec (contract):** `docs/terrain-lod-chunk-spec.md`
**Design (intent):** `docs/terrain-lod-chunk-design.md`
**Status:** Pre-implementation. No code changes yet.

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan phase-by-phase. Every phase has explicit acceptance criteria and a smoke gate. Do not begin a phase until the previous phase's smoke gate passes.

---

## Architectural Contract (must be answered before Phase 1)

Any phase that violates this contract is not landable.

**Q1. What owns render authority?**
CPU terrain code (`mclib/`) owns WHAT to draw: the block list, per-block LOD level, per-block `inFrustum` flag. It emits `TerrainDrawCommand` structs into a CPU-side buffer. RenderSpine / GameOS layer owns HOW and WHEN draw work enters the GPU pass system. Terrain code does NOT own pass execution order and does NOT schedule or sequence passes.

CRITICAL: `Terrain::update()` in `mclib/terrain.cpp` must NOT call `gos_TerrainLodChunk_SubmitDrawCommands()` directly — that violates pass-execution ownership. The compliant design: `Terrain::update()` writes `s_drawCmds[]` and `s_cmdCount`, then returns. A RenderSpine-owned flush site in `code/gamecam.cpp` or `GameOS/gameos/gameos_graphics.cpp` calls `gos_TerrainLodChunk_SubmitDrawCommands(s_drawCmds, s_cmdCount)` at the correct pass-sequenced moment in the frame. Phase 4 must add this flush caller. See Phase 4 for flush-caller assignment.

**Q2. What data crosses the render spine boundary?**
Only `TerrainDrawCommand` structs (16 bytes, POD — see Phase 6 note on struct size) cross from `mclib/` terrain code to the GameOS layer via the flush caller. `TerrainBlockMeta`, `ObjBlockInfo`, `PostcompVertex` pointers, raw GL handles, projected vertex data, and quad lists do NOT cross the boundary.

**Q3. What code is allowed to touch GL / GPU state?**
Only `GameOS/gameos/` code may call `gl*()`. `mclib/` terrain code may NOT call `gl*()`. `mclib/` calls GameOS API functions (`gos_TerrainLodChunk_*()`); GameOS executes the GL work. `MC2_RENDER_CONTRACT_ASSERT=1` fires a fatal assert if `mclib/` calls `gl*()` directly.

**Q4. What is the fast-path-six contract?**
- Cardinality: per-block per-frame (one `TerrainDrawCommand` per visible block).
- Authority: CPU `mapData->blocks[].elevation` is ground truth; GPU height SSBO binding 23 is a cached copy. CPU wins on conflict.
- Slice kind: dispatch-changing for Phase 4+ (emits draw calls); observational for Phase 2 (cull only, no draw commands emitted to new path).
- Boundary: `TerrainDrawCommand` buffer is mclib-internal until submitted via the RenderSpine flush caller.
- Env vars: `getenv()` at runtime; listed in `docs/tier1_env_vars.md`. Primary: `MC2_TERRAIN_LOD_CHUNK`. Assert: `MC2_RENDER_CONTRACT_ASSERT`.
- Gates: every invariant in the spec Classification Table has a corresponding task that writes it.

**Q5. What cardinality is legal?**
One `TerrainDrawCommand` per visible block per frame; zero for culled blocks. One full height SSBO upload per map load. One height SSBO dirty-patch per edited block per edit. One AABB cull pass per frame. One LOD selection per block per frame. Zero per-vertex CPU work in the new draw path.

**Q6. What legacy path remains during shadow mode?**
During Phase 2 shadow mode, `makeLists()` + `geometry()` + `TerrainQuad::draw()` all run unchanged. The new AABB cull runs in parallel but does NOT feed draw calls. `ObjBlockInfo.active` is still set by old `geometry()` during Phase 2. New cull produces `blockMeta.inFrustum` in parallel; results are compared against `ObjBlockInfo.active` for validation logging only (Phase 2). The `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` write of `ObjBlockInfo.active` from `blockMeta.inFrustum` is gated to Phase 2 only; it must be removed or guarded after Phase 4 cutover (because `geometry()` is suppressed in Phase 4+, `objBlockInfo[i].active` is never updated by the old path, and the comparison always produces false negatives).

**Q7. What exact env flag gates the new path?**
`MC2_TERRAIN_LOD_CHUNK=1` (runtime `getenv()`; entry required in `docs/tier1_env_vars.md` before Phase 1 merge). Without this flag the old path runs 100% unchanged. Phase 1: enables AABB allocation only. Phase 2: enables shadow-mode cull. Phase 3: enables height SSBO upload. Phase 4+: enables GPU draw path. Secondary gate: `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` enables bridge of `ObjBlockInfo.active` from `blockMeta.inFrustum` in Phase 2 only — disabled and removed in Phase 4 cutover.

There is NO `MC2_TERRAIN_LOD_CHUNK_PHASE` compile-time constant. Phases are implicit in what code is compiled. All phase gates use `getenv()` at runtime. See renderspine constraint 5.

**Q8. What smoke test proves no boundary violation?**
Tier1 smoke at 30s with both `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. If any `mclib/` terrain code calls `gl*()` directly, the assert fires with a fatal error naming the call site. Smoke pass = no assert, no crash, terrain renders.

---

## GL Discipline Rule

`mclib/` terrain code NEVER calls `gl*()`. All GPU work goes through GameOS layer functions in `GameOS/gameos/`. New functions needed in `GameOS/gameos/` are listed in the phase that needs them. This is enforced at runtime by `MC2_RENDER_CONTRACT_ASSERT=1`.

---

## Legacy Path Rule

Old `makeLists()` / `geometry()` / `TerrainQuad::draw()` path remains fully functional behind `MC2_TERRAIN_LOD_CHUNK=0` (default) through Phase 7. Phase 8 deletes it. No phase before 8 may break the old path when `MC2_TERRAIN_LOD_CHUNK` is unset. Every phase's smoke gate is run with the flag UNSET as well as set.

---

## Phase 1: TerrainBlockMeta + SuperchunkMeta — allocation, AABB, dirty flag

**Slice kind:** observational (no GL, no pixel change, no draw dispatch)
**Goal:** Allocate per-block and per-superchunk metadata arrays from `terrainHeap` at map load and compute world-space AABBs from the 21x21 vertex footprint.
**GL discipline:** None. Zero GPU work. Zero GL calls anywhere in this phase.

### Prereqs (merge-blockers, must land in same commit)

1. Add env var entries to `docs/tier1_env_vars.md`:
   ```
   ## Terrain LOD chunk
   - MC2_TERRAIN_LOD_CHUNK=1 — chunked heightfield LOD renderer. Default OFF.
   - MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1 — bridge ObjBlockInfo.active from blockMeta.inFrustum (Phase 2 only). Default OFF.
   ```

2. **Map size 1021 support — CRITICAL prereq (landability blocker):**

   The validator at `terrain.cpp:327-330` uses two `STOP()` hard-crash macros (NOT soft-asserts):
   ```cpp
   // terrain.cpp:327-330 (actual code):
   if (realVerticesMapSide < 60 || realVerticesMapSide > 2048)
       STOP(("Terrain grid size %d out of supported range [60, 2048]", realVerticesMapSide));
   if (realVerticesMapSide % verticesBlockSide != 0)
       STOP(("Terrain grid size %d not divisible by verticesBlockSide (%d)", realVerticesMapSide, verticesBlockSide));
   ```

   **Corrected analysis — 1021 does NOT trigger the divisibility STOP:**

   The divisibility check must be on **quads** (vertices-1), not vertices:
   - `(1021 - 1) % 20 = 1020 % 20 = 0` → condition FALSE → STOP does NOT fire for 1021.
   - No env-gate bypass needed for the 1021-vertex target.

   a. Change `terrain.cpp:329-330` to check the quad domain:
      ```cpp
      if ((realVerticesMapSide - 1) % verticesBlockSide != 0) {
          if (!getenv("MC2_TERRAIN_LOD_CHUNK"))
              STOP(("Terrain quad count %d not divisible by verticesBlockSide (%d)",
                    realVerticesMapSide - 1, verticesBlockSide));
          // Partial-edge blocks permitted under MC2_TERRAIN_LOD_CHUNK=1.
          // quadCountX = min(20, (realVerticesMapSide-1) - originX).
      }
      ```
      For 1021: condition is false — no STOP. For old 120-vertex maps: `119%20=19` → condition
      true; without the flag, STOP fires (preserving old validation). With the flag, proceeds.

   b. `terrainChunkSide` computation (terrain.cpp:392) — ceiling over quads:
      ```cpp
      // terrainChunkSide = ceil((realVerticesMapSide-1) / verticesBlockSide)
      int terrainChunkSide = (realVerticesMapSide - 1 + verticesBlockSide - 1) / verticesBlockSide;
      // = (realVerticesMapSide + verticesBlockSide - 2) / verticesBlockSide;
      // 1021→51  120→6  60→3
      ```
      Replace the `s_blocksMapSide` member name with `s_terrainChunkSide` throughout.

   **Two-count rule — must use explicit names, never alias:**
   ```cpp
   // Render chunk count (LOD draw unit array size):
   int terrainChunkSide = ((realVerticesMapSide - 1) + verticesBlockSide - 1) / verticesBlockSide;

   // Vertex bucket count (ObjBlockInfo[] array size — existing allocation):
   // Existing code uses truncating divide: realVerticesMapSide / verticesBlockSide
   // Do not replace old ObjBlockInfo allocation formula; keep old code intact.
   ```

   | Map     | terrainChunkSide | existing ObjBlockInfo side | diverge? |
   |---------|-----------------|---------------------------|----------|
   | 60      | 3               | 3                         | no       |
   | 120     | 6               | 6                         | no       |
   | 1021    | **51**          | **51** (1021/20 trunc.)   | no*      |

   *For 1021, existing truncating divide gives 51. `ceil(1021/20)=52` would be a 52nd
   zero-quad bucket — never instantiate it. `terrainChunkSide=51` is correct.
   Arrays `s_blockMeta`, `s_superchunkMeta`, `s_drawCmds` all use `terrainChunkSide`.
   Do not size from ObjBlockInfo[] count or old `blocksMapSide`.

   **1021-vertex map: NO partial blocks, NO OOB samples:**
   `terrainChunkSide=51`, all blocks `quadCountX=quadCountY=20`,
   last block origin=(1000,1000), sample range 1000..1020, all in [0,1020].

   **120-vertex map: one partial render chunk per edge:**
   `terrainChunkSide=6`, last block origin=100, `quadCountX=quadCountY=19`,
   sample range 100..119. The 120th vertex is the seam to OOB — NOT sampled in new path.

   c. `recomputeBlockAabb()` uses `bm.quadCountX`/`bm.quadCountY` (not constant 20).

### New Files

- `GameOS/gameos/gos_terrain_lod_chunk.h` — declares `TerrainDrawCommand`, `TERRAIN_HEIGHT_SSBO_BINDING`, and all three `gos_TerrainLodChunk_*` API functions. No GL includes.
- `GameOS/gameos/gos_terrain_lod_chunk.cpp` — stub implementations: all three functions are no-ops. No GL calls. Enables linking.

### Modified Files

- `mclib/terrain.h`
  - Add `TerrainBlockMeta` struct (alongside `ObjBlockInfo`): `originX`, `originY`, `quadCountX`, `quadCountY`, `minElev`, `maxElev`, `dirtyAabb`, `inFrustum`, `lodLevel`.
  - `quadCountX` and `quadCountY` are set at allocation time:
    `quadCountX = min(verticesBlockSide, (realVerticesMapSide-1) - bx*verticesBlockSide)`.
    For 1021-vertex map: all blocks = 20 (no partial). For 120-vertex maps: last block = 19.
  - Add `SuperchunkMeta` struct: `worldMinX/MaxX/Y/Z`, `inFrustum`, `_pad[3]`.
  - Add `TerrainDrawCommand` struct (or `#include "GameOS/gameos/gos_terrain_lod_chunk.h"` — no GL transitive include; verify).
  - Add `static TerrainBlockMeta* s_blockMeta` and `static SuperchunkMeta* s_superchunkMeta` to `Terrain` class.
  - Add `static TerrainDrawCommand* s_drawCmds` and `static int s_cmdCount` to `Terrain` class.
  - Add `static uint32_t gCurrentFrame` to `Terrain` class.
  - Add `static int superchunkSide` to `Terrain` class.
  - Add `static int s_terrainChunkSide` (= `ceil(quads/20)`; 1021→51).

- `mclib/terrain.cpp`
  - `Terrain::init()` (near `terrain.cpp:456`): compute `s_terrainChunkSide`; allocate `s_blockMeta`, `s_superchunkMeta`, `s_drawCmds` from `terrainHeap`; `memset` to zero; for each block, set `quadCountX = min(verticesBlockSide, (realVerticesMapSide-1) - bx*verticesBlockSide)` and `quadCountY` similarly; call `recomputeBlockAabb()` for all blocks; compute superchunk AABBs; set `gCurrentFrame = 1`; `s_cmdCount = 0`.
  - `Terrain::destroy()` (near `terrain.cpp:693`): free `s_blockMeta`, `s_superchunkMeta`, `s_drawCmds`.
  - Add static function `recomputeBlockAabb(TerrainBlockMeta& bm)`: scans vertex footprint using `bm.quadCountX` and `bm.quadCountY` (not the constant 20), clamps OOB to 33.0f `blankVertex` elevation, clears `dirtyAabb`.
  - Add static function `recomputeSuperchunkAabb(int scX, int scY)`: union of constituent block world-space AABBs.
  - Add static function `chooseLOD(const Stuff::Vector3D& camPos, const TerrainBlockMeta& bm, float halfMap)`: returns 0-5 per distance table.
  - `terrainChunkSide = (realVerticesMapSide - 1 + verticesBlockSide - 1) / verticesBlockSide;`
    (ceiling over quads; 1021→51, 120→6, 60→3; replaces the truncating vertex-divide at line 392).

### GameOS Functions Needed

`gos_terrain_lod_chunk.h` stub only (no GL). All three stubs are no-ops.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` — runtime `getenv()`. Add to `docs/tier1_env_vars.md` in this commit (prereq above). Allocation gated: if env var not set, `s_blockMeta` etc. remain NULL and no work happens. The quad-domain divisibility bypass (for maps where `(vertices-1)%20 != 0`) also gates on `getenv("MC2_TERRAIN_LOD_CHUNK")`. NOTE: for 1021-vertex map, `(1021-1)%20=0` so no bypass fires — the new validator passes without the flag.

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo with no new warnings.
2. Map loads without crash or assert with `MC2_TERRAIN_LOD_CHUNK=1` for BOTH divisible maps (e.g., 120-vertex) AND 1021-vertex maps. `s_blockMeta` is non-NULL.
3. `AABB[0].minElev` and `AABB[0].maxElev` are plausible (non-zero, non-FLT_MAX) in a debugger watch.
4. For a 1021-vertex map: `s_terrainChunkSide == 51`. Last block origin == (1000, 1000). `quadCountX == 20`, `quadCountY == 20`. No OOB sample (range 1000..1020 ⊆ [0,1020]). Verified in debugger. NOTE: there is NO 52nd block and NO 1-quad partial block — that was the old wrong formula.
5. Map reload (mission restart) does not double-free or leak (sentinel check: second `Terrain::init()` run after `Terrain::destroy()` passes without crash).
6. `static_assert(sizeof(TerrainDrawCommand) == 16)` passes at compile time.
7. Invariant I6: tier1 smoke with `MC2_TERRAIN_LOD_CHUNK` UNSET must match pre-mod baseline (zero pixel diff).
8. Smoke gate (flag set): `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` set in shell. No crash, no assert, terrain renders as before.

### Dependencies

None. Phase 1 is the base.

---

## Phase 2: Block AABB cull shadow mode — parallel cull, mismatch log, ObjBlockInfo bridge

**Slice kind:** observational (AABB cull runs; does NOT feed draw path; no pixel change)
**Goal:** Run the two-level frustum cull (superchunk then block) in `Terrain::update()` alongside the unchanged legacy path, validate it produces zero false negatives, and optionally bridge `ObjBlockInfo.active` from the new cull result.
**GL discipline:** None. Zero GPU work.

### Modified Files

- `mclib/terrain.cpp`
  - `Terrain::update()` (after `terrain.cpp:988-989` `makeLists()` + `geometry()` calls, so `ObjBlockInfo.active` is current-frame when compared):
    - Under `MC2_TERRAIN_LOD_CHUNK=1` gate: increment `gCurrentFrame`, run two-level AABB cull (superchunk outer loop, per-block inner loop), build `s_drawCmds[]`, set `s_cmdCount`.
    - **CRIT-1 (catastrophic-axis invariant):** The cull-state write `s_blockMeta[i].inFrustum = cullResult` MUST be written BEFORE any `continue`/skip gate in the cull loop. If the write is placed after a skip, blocks at the frustum edge are silently dropped (objects/mechs vanish). Pseudocode:
      ```cpp
      // CORRECT order — write inFrustum before any continue:
      bm.inFrustum = blockPassesCull;
      if (!bm.inFrustum) continue;
      // ... choose LOD, emit draw command ...
      ```
    - After cull: run validation loop (Phase 2 only, guarded by `#ifdef TERRAIN_LOD_CHUNK_PHASE2_VALIDATE` or equivalent compile-time flag) comparing `s_blockMeta[i].inFrustum` vs `objBlockInfo[i].active`. Count false negatives (`!inFrustum && active`). Log at most once per 60 frames (throttled). Log via `MC2_LOG("TerrainLODChunk: %d false negatives -- Phase 3 advance BLOCKED", falseNegatives)`.
    - **Phase 2 validation guard:** The `s_blockMeta[i].inFrustum` vs `objBlockInfo[i].active` comparison MUST be guarded so it only runs in Phase 2 (while `geometry()` still runs and `objBlockInfo[i].active` is updated). In Phase 4+, `geometry()` is suppressed, `objBlockInfo[i].active` is never updated, and the comparison always produces false negatives. Remove or disable this comparison block before Phase 4 cutover.
    - `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK` write: after validation log, if `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1`, write `objBlockInfo[i].active = s_blockMeta[i].inFrustum` for all blocks. This bridge is Phase 2 only; it must be removed before Phase 4 cutover (Phase 4 disables `geometry()`, making `objBlockInfo[i].active` stale).
    - Do NOT call `gos_TerrainLodChunk_SubmitDrawCommands()` in this phase. `s_cmdCount` is computed but not submitted.
  - Add `ZoneScopedN("Terrain::geometry slimReduce")` Tracy zone around the new cull loop (coarse zone only; no per-block child zones).

### GameOS Functions Needed

None.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` — main gate (already in `tier1_env_vars.md` from Phase 1).
`MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` — bridge sub-gate (already in `tier1_env_vars.md` from Phase 1). Enable only after validation confirms zero false negatives.

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. With `MC2_TERRAIN_LOD_CHUNK=1`: log shows zero false negatives across tier1 5-mission run. If any false negatives appear, they must be investigated and fixed before proceeding to Phase 3. Invariant I2 must hold.
3. **CRIT-1 verified:** The `inFrustum` write is placed before the `continue` gate in the cull loop. Code review sign-off required; the Phase 2 PR must include a reviewer comment confirming write-before-gate ordering.
4. With `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1`: objects activate/deactivate correctly (no missing object activations visible in gameplay).
5. Legacy path unchanged when `MC2_TERRAIN_LOD_CHUNK` is unset: tier1 smoke passes with no pixel diff vs pre-Phase-2 baseline.
6. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1`. No crash, no assert, no false-negative log entries after warm-up frame.

### Dependencies

Depends on Phase 1 (TerrainBlockMeta + SuperchunkMeta allocated and AABB computed).

---

## Phase 3: GPU height SSBO upload + calcThisFrame water-guard migration

**Slice kind:** dispatch-changing (first GPU buffer upload; no draw calls yet; no pixel change from terrain itself)
**Goal:** Upload the full heightfield to GPU SSBO binding 23 at map load; patch dirty blocks on terrain edit; migrate `Vertex::calcThisFrame` water-guard to a frame-stamp field.
**GL discipline:** All GL buffer work in `GameOS/gameos/gos_terrain_lod_chunk.cpp`. `mclib/` calls `gos_TerrainLodChunk_UploadHeightFull()` and `gos_TerrainLodChunk_UploadHeightPatch()` only.

### calcThisFrame analysis (CRITICAL — spec was wrong)

Grep of `mclib/quad.cpp` shows that `calcThisFrame` has ONLY one active bit pattern in the current codebase:
- `calcThisFrame & 2` (water guard) — used at quad.cpp:996, 1047, 1051, 1100, 1104, 1153, 1157, 1206 inside `setupTextures()` to prevent double water computation.
- There is NO `calcThisFrame & 1` lighting guard in the current codebase. The plan's previous description of a `calcThisFrame & 1` lighting guard was inaccurate; that pattern does not exist and must not be migrated.

**Phase 3 migration scope is therefore limited to:**
- Migrate the water guard (`calcThisFrame & 2` / `calcThisFrame |= 2`) to `lastWaterFrame != Terrain::gCurrentFrame`.
- Leave `calcThisFrame` field in place (old path compatibility; removed in Phase 8).
- `lastLitFrame` field is NOT added in this phase; no lighting-guard migration is needed.

**`setVertexHeight()` dirty-patch block coordinates (MAJOR — was unspecified):**

`setVertexHeight(int VertexIndex, float Val)` takes a flat linear index into the `realVerticesMapSide x realVerticesMapSide` vertex array. To mark the correct block dirty:
```cpp
int col = VertexIndex % realVerticesMapSide;   // x/column coordinate
int row = VertexIndex / realVerticesMapSide;   // y/row coordinate
int bx  = col / verticesBlockSide;            // block column (truncating)
int by  = row / verticesBlockSide;            // block row (truncating)
// Clamp for partial-edge blocks:
bx = min(bx, s_blocksMapSide - 1);
by = min(by, s_blocksMapSide - 1);
int blockIdx = by * s_blocksMapSide + bx;
s_blockMeta[blockIdx].dirtyAabb = true;
```
For partial-edge blocks (1021-vertex map: last block column/row has `quadCountX/Y = 1`), the clamp ensures the index stays in range. The implementer must use this exact formula; do not use `col / 20` without the clamp.

### Prereqs (merge-blockers)

1. Grep worktree for `glBindBufferBase.*23\b` and `layout.*binding.*=.*23` to confirm slot 23 is free. Update `docs/render-binding-registry.md` row 23 from TBD to:
   `| 23 | TERRAIN_HEIGHT_SSBO_BINDING / TerrainHeightBuf | terrain LOD chunk | SSBO | GameOS/gameos/gos_terrain_lod_chunk.cpp | shaders/terrain_lod_chunk.vert:layout(binding=23) | Added Phase 3. Full map float[] upload at load; dirty-patch on edit. |`
   This update is hard gate I-registry-23.

### New Files

- `shaders/terrain_lod_chunk.vert` — stub terrain LOD vertex shader. Declares `layout(binding = 23, std430) readonly buffer TerrainHeightBuf { float heights[]; }`. No fragment output yet. Used to verify SSBO binding compiles.
- `shaders/terrain_lod_chunk.frag` — stub fragment shader (output solid debug color). Used to verify shader pipeline compiles.

### Modified Files

- `GameOS/gameos/gos_terrain_lod_chunk.h`
  - Add `constexpr uint32_t TERRAIN_HEIGHT_SSBO_BINDING = 23u`.
  - Declare `void gos_TerrainLodChunk_Init()` and `void gos_TerrainLodChunk_Destroy()`.

- `GameOS/gameos/gos_terrain_lod_chunk.cpp`
  - Implement `gos_TerrainLodChunk_Init()`: call `glGenBuffers(1, &s_heightSsbo)` (SSBO allocated but not filled until first `UploadHeightFull` call).
  - Implement `gos_TerrainLodChunk_Destroy()`: `glDeleteBuffers`.
  - Implement `gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)`: `glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_heightSsbo)`, `glBufferData`, `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, s_heightSsbo)`.
  - Implement `gos_TerrainLodChunk_UploadHeightPatch(...)`: `glBindBuffer`, `glBufferSubData` at correct byte offset, assert upload byte count == `(blockMeta.quadCountY+1)*(blockMeta.quadCountX+1)*sizeof(float)` (invariant I8; uses actual block quad counts, not constant 20).
  - `gos_TerrainLodChunk_SubmitDrawCommands` remains a no-op in this phase.

- `GameOS/gameos/gameos_graphics.cpp` (or terrain init hook)
  - Call `gos_TerrainLodChunk_Init()` after GL context established.
  - Call `gos_TerrainLodChunk_Destroy()` at shutdown.

- `mclib/terrain.cpp`
  - `Terrain::init()`: after `mapData` loaded and under `MC2_TERRAIN_LOD_CHUNK=1`, build `float[]` from `mapData->blocks[i].elevation` and call `gos_TerrainLodChunk_UploadHeightFull()`. Invariant I3 debug-build check: compare SSBO content vs CPU ground truth (sequential, first frame only, debug build only).
  - `Terrain::setVertexHeight()` call site: when dirty, compute `(bx, by)` from `VertexIndex` using the formula above, mark `s_blockMeta[blockIdx].dirtyAabb = true`, build patch array and call `gos_TerrainLodChunk_UploadHeightPatch()`.

- `mclib/vertex.h`
  - Add `uint32_t lastWaterFrame` after `calcThisFrame` (do NOT add `lastLitFrame`; no lighting guard exists to migrate).
  - `Vertex::init()`: set `lastWaterFrame = 0`.
  - Do NOT remove `calcThisFrame` in this phase. Phase 8 removes it.

- `mclib/quad.cpp` (`setupTextures()`)
  - Replace water bit-guard pattern with frame-stamp pattern:
    - Old: `if (!(v->calcThisFrame & 2)) { calcWater(v); v->calcThisFrame |= 2; }` (appears at lines 996/1047, 1051/1100, 1104/1153, 1157/1206)
    - New: `if (v->lastWaterFrame != Terrain::gCurrentFrame) { calcWater(v); v->lastWaterFrame = Terrain::gCurrentFrame; }`
  - `calcThisFrame` field and its existing bit-2 write remain in the old path (bit-2 clear in `makeLists` sweep also remains). Only the NEW frame-stamp check is added; the old bit-2 path is NOT removed in this phase.
  - Note: acceptance criterion 5 ("setupTextures() no longer triggers full-window calcThisFrame sweep reset") reflects the new frame-stamp path; the old `calcThisFrame & 2` sweep reset in `makeLists` still runs for the legacy path and is correct.

### GameOS Functions Needed

- `gos_TerrainLodChunk_Init()` — allocates GL SSBO.
- `gos_TerrainLodChunk_Destroy()` — frees GL SSBO.
- `gos_TerrainLodChunk_UploadHeightFull(const float*, int)` — full map upload.
- `gos_TerrainLodChunk_UploadHeightPatch(const float*, int blockIdx, const TerrainBlockMeta&)` — dirty-patch upload using actual block quad counts.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` — gates all new code in this phase. Add to `docs/tier1_env_vars.md` (already there from Phase 1).

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. `docs/render-binding-registry.md` row 23 updated (I-registry-23). Grep confirms no other binding-23 holder.
3. Invariant I3: debug build first-frame compare passes (SSBO content matches CPU elevation array for all `realVerticesMapSide^2` entries).
4. Invariant I8: dirty-patch assert uses `blockMeta.quadCountX`/`quadCountY` (not constant 20); fires correctly when byte count is wrong (unit test or manual verify via breakpoint).
5. For 1021-vertex map: `gos_TerrainLodChunk_UploadHeightPatch()` for the last block (quadCountX=20, quadCountY=20) uploads `21*21*sizeof(float)` = 1764 bytes. For a 120-vertex map last block (quadCountX=19, quadCountY=19): `20*20*sizeof(float)` = 1600 bytes. Assert uses actual block quad counts — not constant 20. Verify at a breakpoint.
6. Water rendering is unchanged with `MC2_TERRAIN_LOD_CHUNK=1`: no double-water, no missed-water defect. The `lastWaterFrame` frame-stamp correctly replaces the `calcThisFrame & 2` guard under the new path.
7. Legacy path unchanged when `MC2_TERRAIN_LOD_CHUNK` unset: tier1 smoke passes. The old `calcThisFrame & 2` path runs unchanged.
8. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. No assert, no crash.

### Dependencies

Depends on Phase 2 (AABB cull loop exists; `dirtyAabb` flag exists and is set by terrain edit path).

---

## Phase 4: Shared LOD index buffers + GPU chunk draw at fixed LOD

**Slice kind:** dispatch-changing (new terrain draw path active; old path disabled under flag)
**Goal:** Build 6 static shared index buffers (LOD0-LOD5), implement `gos_TerrainLodChunk_SubmitDrawCommands()` as a real draw call, assign a RenderSpine flush caller, and enable the GPU chunk draw path under `MC2_TERRAIN_LOD_CHUNK=1`. Old path disabled when flag is set.
**GL discipline:** All GL work (index buffer alloc, SSBO bind, draw call) in `GameOS/gameos/gos_terrain_lod_chunk.cpp`. `mclib/` writes `s_drawCmds[]` and `s_cmdCount`; the RenderSpine flush caller in `code/gamecam.cpp` submits them.

### Index buffer + quad count correctness

For the 1021-vertex map (primary target): ALL blocks have `quadCountX=quadCountY=20`. The 6
shared index buffers (one per LOD, built for full 20×20 blocks) are sufficient — no per-block
variants needed. The LOD5 buffer (step=20, 1×1 quad, 6 indices) is the most coarse case.

For partial-block maps (where `(vertices-1)%20 != 0`): `quadCountX`/`Y` may be < 20 in the
last row/column. In Phase 4, if partial blocks are present, use one of:
a. **Per-unique-(qcX,qcY,lodStep) index buffer** (preferred for correctness).
b. **Padded-to-20 with degenerate triangles** (simpler; each partial block draws 20×20 but
   with zero-area triangles in the empty region).

`u_blockQuadCountX`/`u_blockQuadCountY` uniforms come from `cmd.quadCountsPacked` (not from
`TerrainBlockMeta`). GameOS unpacks: `qcX = cmd.quadCountsPacked & 0xFF;
qcY = (cmd.quadCountsPacked >> 8) & 0xFF;`. The vertex shader uses these to compute gridW.

### Flush-caller assignment (CRITICAL — Q1 compliance)

`Terrain::update()` in `mclib/terrain.cpp` must NOT call `gos_TerrainLodChunk_SubmitDrawCommands()`. The compliant flush site:

- Add `Terrain::flushDrawCommands()` as a static public method in `mclib/terrain.h`/`terrain.cpp`:
  ```cpp
  void Terrain::flushDrawCommands() {
      if (s_cmdCount > 0)
          gos_TerrainLodChunk_SubmitDrawCommands(s_drawCmds, s_cmdCount);
  }
  ```
- Add call to `Terrain::flushDrawCommands()` in `code/gamecam.cpp` at the correct pass-sequenced moment in the frame (after shadow pass, before post-process — same position as the old `geometry()` draw). This is the sole flush caller.
- `Terrain::update()` only writes `s_drawCmds[]` and `s_cmdCount`; it never submits.

### Old-path interaction with IsFrameSolidArmed / IsFrameOverlayArmed (MAJOR — gate interaction)

The existing `Terrain::render()` draw loop (quad.cpp path) is already gated by:
```cpp
if (!(gos_terrain_indirect::IsFrameSolidArmed()
      && gos_terrain_indirect::IsFrameOverlayArmed()))
{ /* per-quad TerrainQuad::draw() loop */ }
```
Under `MC2_TERRAIN_LOD_CHUNK=1`, the new path must NOT interfere with the `IsFrameSolidArmed`/`IsFrameOverlayArmed` flags. The required behavior: add a separate outer gate for the new chunk path:
```cpp
// In Terrain::render():
if (getenv("MC2_TERRAIN_LOD_CHUNK")) {
    // New path: Terrain::flushDrawCommands() is called from gamecam.cpp, not here.
    // Skip the old draw loop unconditionally.
    return;  // or skip to minePass
}
// Old path (unchanged):
if (!(IsFrameSolidArmed() && IsFrameOverlayArmed())) {
    // ... per-quad draw() loop ...
}
```
The `IsFrameSolidArmed`/`IsFrameOverlayArmed` flags are NOT set or cleared by the new path. The new path bypasses this gate entirely via the separate outer return.

### Phase 2 validation / bridge code removal

Before Phase 4 ships: remove the `s_blockMeta[i].inFrustum` vs `objBlockInfo[i].active` comparison loop and the `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK` write from Phase 2. Both are stale in Phase 4+ (geometry() is suppressed, objBlockInfo[i].active is never updated). Remove, do not guard.

### New Files

- `shaders/terrain_lod_chunk.vert` — real implementation replacing Phase 3 stub. Reads `heights[]` from SSBO binding 23. Reconstructs centered world-space `worldPos` from `gl_VertexID`, `u_blockOriginX`, `u_blockOriginY`, `u_lodStep`, `u_blockQuadCountX`, `u_blockQuadCountY`, `u_mapSide`, `u_halfMap`. Applies `mvp` matrix for clip-space output. No `terrainMVP` uniform; no CPU-projected inputs. Uniforms: `u_blockOriginX` (int), `u_blockOriginY` (int), `u_lodStep` (int), `u_blockQuadCountX` (int), `u_blockQuadCountY` (int), `u_mapSide` (int), `u_halfMap` (float), `mvp` (mat4).
- `shaders/terrain_lod_chunk.frag` — real implementation: solid unlit color based on `worldPos.z` range for bring-up. PBR material left for Phase 10.

### Modified Files

- `GameOS/gameos/gos_terrain_lod_chunk.h`
  - Add `void gos_TerrainLodChunk_Init()` extension: now also calls `buildLodIndexBuffers()`.
  - Constant table: `constexpr int k_lodSteps[6] = {1, 2, 4, 5, 10, 20}`.

- `GameOS/gameos/gos_terrain_lod_chunk.cpp`
  - `buildLodIndexBuffers()` (called from `Init()`): for each LOD level 0-5, build `uint16_t[]` index list using two CCW triangles per quad. The builder accepts `(quadCountX, quadCountY, lodStep)` and generates the correct index count for partial-edge blocks. Call `glGenBuffers` + `glBufferData`. See partial-block correctness section above for approach.
  - `gos_TerrainLodChunk_SubmitDrawCommands(const TerrainDrawCommand* cmds, int count)`: if `count == 0`, strict no-op. Otherwise: bind height SSBO to binding 23; use LOD chunk shader program; for each command, set uniforms including `u_blockQuadCountX` and `u_blockQuadCountY`; bind appropriate LOD index buffer; call `glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, 0)`. Register a new RenderPassId for this draw (not `RenderPassId=2`; see spec R6). Restore ALL mutated GL state (color masks, draw buffers, depth state) before return.
  - Phase 4 uses per-draw uniform uploads (not indirect yet). Indirect path deferred.

- `mclib/terrain.cpp`
  - `Terrain::update()`: under `MC2_TERRAIN_LOD_CHUNK=1`, run cull + fill `s_drawCmds[]` + set `s_cmdCount`. Do NOT call `SubmitDrawCommands` here.
  - `Terrain::flushDrawCommands()`: new static method, calls `gos_TerrainLodChunk_SubmitDrawCommands(s_drawCmds, s_cmdCount)` if `s_cmdCount > 0`. This is the only submit point.
  - `Terrain::render()`: under `MC2_TERRAIN_LOD_CHUNK=1`, skip the old draw loop (return before `IsFrameSolidArmed` gate, as shown above).
  - Under `MC2_TERRAIN_LOD_CHUNK=1`: skip `makeLists()`, `geometry()` calls. Old path runs only when flag is unset.
  - Tracy: `ZoneScopedN("Terrain::geometry slimReduce")` already added in Phase 2. Add `ZoneScopedN("Terrain::drawChunks")` around `flushDrawCommands()` call in gamecam.cpp.
  - Assert (Phase 4): `expectedVisible` pre-count from `s_blockMeta` inFrustum scan; Tracy zone assert `expectedVisible == s_cmdCount` (invariant I4, diagnostic-only; CPU accessor).

- `code/gamecam.cpp`
  - Add `Terrain::flushDrawCommands()` call at the correct pass-sequenced frame position (after shadow pass, before post-process). This is the ONLY flush caller.

- `docs/render-binding-registry.md`
  - Register new RenderPassId for terrain LOD chunk draw in same commit (spec R6). PassId must NOT be `RenderPassId=2`.

### GameOS Functions Needed

`gos_TerrainLodChunk_SubmitDrawCommands(const TerrainDrawCommand*, int)` — real implementation with GL draw calls.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` — runtime `getenv()` (already in `tier1_env_vars.md`). Gates both the new draw command fill and the suppression of the old path. No compile-time `MC2_TERRAIN_LOD_CHUNK_PHASE` constant; this would violate renderspine constraint 5.

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. With `MC2_TERRAIN_LOD_CHUNK=1`: terrain renders on screen via new path. Solid elevation-based shading visible (no black screen, no missing patches).
3. All blocks visible from overhead camera produce draw commands. No terrain holes in `inFrustum` blocks.
4. `static_assert` on `k_lodSteps` table size == 6 compiles (invariant I5 compile-time check).
5. Invariant I4: `expectedVisible == s_cmdCount` assert does not fire during 30s smoke.
6. No `gl*()` call in `mclib/` — `MC2_RENDER_CONTRACT_ASSERT=1` does not fire.
7. Old path runs correctly when `MC2_TERRAIN_LOD_CHUNK` is unset: tier1 smoke with flag unset passes.
8. New `RenderPassId` registered in `docs/render-binding-registry.md` (spec R6).
9. `Terrain::flushDrawCommands()` is called from `code/gamecam.cpp` only; no submit call in `mclib/terrain.cpp`. Verified by grep: `grep -n "SubmitDrawCommands" mclib/terrain.cpp` returns zero matches.
10. For 1021-vertex map: partial-edge blocks render without GPU errors. No `GL_INVALID_VALUE` from `glDrawElements`. Verify with `MC2_RENDER_CONTRACT_ASSERT=1`.
11. Phase 2 validation loop and `BRIDGE_OBJBLOCK` write are absent from the codebase (removed, not disabled). Verified by grep.
12. Smoke gate (new path): `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. No assert, no crash, terrain visible.

### Dependencies

Depends on Phase 3 (height SSBO populated; stub `SubmitDrawCommands` exists; shader files exist).

---

## Phase 5: Per-block LOD distance selection + neighbor delta <= 1

**Slice kind:** dispatch-changing (changes which LOD index buffer each block uses per frame)
**Goal:** Enable per-block distance-based LOD selection (`chooseLOD()`) and add a neighbor-delta clamping pass to keep adjacent blocks within 1 LOD level of each other before skirts are added.
**GL discipline:** No new GL work. GameOS draw path unchanged; only `lodStep` value in `TerrainDrawCommand` varies per block.

### Modified Files

- `mclib/terrain.cpp`
  - `Terrain::update()` cull loop: call `chooseLOD(cameraPos, bm, halfMap)` after `bm.inFrustum=true`; store result in `bm.lodLevel`. Already coded in Phase 2 cull loop (Phase 2 had single fixed LOD; Phase 5 activates the real function).
  - After cull loop and before command emission: add neighbor-delta clamp pass. For each `inFrustum` block, check 4 neighbors; if any neighbor's `lodLevel` is more than 1 below this block's `lodLevel`, reduce this block's `lodLevel` by 1. Iterate until stable (max 5 passes; terminates because lodLevel is bounded 0-5).
  - Command emission: use `k_lodSteps[bm.lodLevel]` (already in place from Phase 4 at fixed LOD2; Phase 5 makes it dynamic).
  - `static_assert(sizeof(k_lodSteps)/sizeof(k_lodSteps[0]) == 6)` (invariant I5).
  - Assert in `chooseLOD()`: result must be in `[0,5]`; assert fires otherwise (invariant I5 runtime).

- `shaders/terrain_lod_chunk.vert`
  - `u_lodStep` uniform now varies per draw call (was fixed at 4 in Phase 4). No shader change needed if uniform was already wired; verify.

### GameOS Functions Needed

None.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` (existing).

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. LOD transitions visible when zooming: near blocks have finer grids, far blocks have coarser. No blocks jump more than 1 LOD level from adjacent blocks.
3. Invariant I5 assert in `chooseLOD()` does not fire across 30s smoke.
4. `static_assert` on `k_lodSteps` size == 6 compiles.
5. Cracks visible at LOD boundaries are expected and acceptable (skirts added Phase 6).
6. Old path unchanged when flag unset.
7. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. No crash, no assert.

### Dependencies

Depends on Phase 4 (GPU chunk draw active; `chooseLOD()` function exists from Phase 1).

---

## Phase 6: Skirts for crack sealing + unrestricted mixed LOD

**Slice kind:** dispatch-changing (adds skirt geometry; removes neighbor-delta clamp)
**Goal:** Add per-block skirt geometry (thin downward fringe around each block's perimeter) to hide LOD-transition cracks. After skirts are confirmed working, remove the neighbor-delta clamp from Phase 5.
**GL discipline:** New skirt index buffers built in `gos_terrain_lod_chunk.cpp`. No `mclib/` GL calls.

### TerrainDrawCommand struct size — RESOLVED

Phase 1 uses `quadCountsPacked` in the former `_pad` field. `TerrainDrawCommand` stays at 16 bytes.
`skirtDepth` for Phase 6: use a **parallel array** approach (Option A from old draft):
- Add `static float s_skirtDepths[MAX_BLOCKS]` in `mclib/terrain.cpp`.
- Fill alongside `s_drawCmds[]` in the cull loop: `s_skirtDepths[cmdCount] = bm.maxElev - bm.minElev + k_skirtMargin`.
- Extend `gos_TerrainLodChunk_SubmitDrawCommands()` signature to accept `const float* skirtDepths` (parallel array, same indexing as `cmds`).
- `static_assert(sizeof(TerrainDrawCommand) == 16)` remains valid — no change.

### Modified Files

- `GameOS/gameos/gos_terrain_lod_chunk.cpp`
  - `buildLodIndexBuffers()`: for each LOD level, also build a skirt index buffer. Skirt = 4 edge strips (±X, ±Y), each strip is a row/column of quads extruded downward. Each edge strip at LOD L has `(blockQuadCountX_or_Y / lodStep)` quads (uses actual block quad counts, not constant 20). Total skirt triangles per block: 4 * (effective quads per edge) * 2.
  - Skirt vertex shader reads edge vertex height from SSBO and extrudes by a fixed downward offset (uniform `u_skirtDepth` or per-command skirtDepth from chosen struct/array approach).
  - `gos_TerrainLodChunk_SubmitDrawCommands()`: after main LOD draw for each block, issue skirt draw using the LOD-matched skirt index buffer. Skirt uses same `u_blockOriginX/Y`, `u_lodStep`, `u_blockQuadCountX/Y`; adds `u_skirtDepth` uniform.

- `mclib/terrain.h` / `mclib/terrain.cpp`
  - Apply the chosen struct/array decision (Option A or B) from the decision above. Update `static_assert` accordingly if Option B.
  - Remove neighbor-delta clamp pass from Phase 5 after skirts confirmed stable.

- `shaders/terrain_lod_chunk.vert`
  - Add `u_skirtDepth` uniform. If `gl_VertexID` flags a skirt vertex (or separate draw with skirt-specific shader variant), use `worldPos.z = u_blockBottomZ` for bottom skirt row.

### GameOS Functions Needed

None beyond what Phase 4 established (skirt submission is inside `SubmitDrawCommands`).

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` (existing).

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo. `static_assert(sizeof(TerrainDrawCommand) == 16)` passes (struct stays 16 bytes; skirt depth uses parallel array).
2. LOD-boundary cracks not visible at any LOD combination in-game. Pan camera rapidly across map: no visible seams at block transitions.
3. Mixed LOD (e.g., LOD0 near camera, LOD4 at horizon) renders cleanly with no T-junction gaps.
4. Neighbor-delta clamp removed (code deleted, not just disabled).
5. Old path unchanged when flag unset.
6. `gos_TerrainLodChunk_SubmitDrawCommands()` now accepts `const float* skirtDepths` parallel array. `s_skirtDepths[]` allocated alongside `s_drawCmds[]`. No struct expansion.
7. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. No crash, no assert.

### Dependencies

Depends on Phase 5 (per-block LOD selection active; `minElev`/`maxElev` per block computed).

---

## Phase 7: Mine enqueue off drawMine(); inverseProject -> heightfield raycast; visibility separation

**Slice kind:** dispatch-changing (changes mine enqueue path and picking path; migrates side-effects from legacy functions)
**Goal:** Decouple mine enqueue from `TerrainQuad::drawMine()` and replace `Camera::inverseProject()` terrain picking with a heightfield raycast. After this phase, `makeLists()`, `geometry()`, and `TerrainQuad::draw()` have zero remaining consumers under `MC2_TERRAIN_LOD_CHUNK=1`.
**GL discipline:** No new GL calls in `mclib/`. Mine enqueue uses existing `mcTextureManager->addTriangle()` path unchanged.

### Mine enqueue — corrected consumer location

The plan previously cited `setupTextures()` at `mclib/quad.cpp:167,239,355,461` as the mine enqueue source. This was WRONG. Actual mine enqueue is in `TerrainQuad::drawMine()` (quad.cpp:3977+), which is called from `Terrain::render()` in the `minePass` zone (terrain.cpp:1170-1187), gated by `!gos_terrain_indirect::IsFrameMineArmed()`. The `setupTextures()` call at quad.cpp:775/854 calls `GameMap->getMine()` to collect mine data but does NOT enqueue draw triangles — the triangle emit is in `drawMine()`.

Under `MC2_TERRAIN_LOD_CHUNK=1`, the minePass zone in `Terrain::render()` must be replaced with a new mine-walk that does not iterate all visible quads via the old quad list.

**Pre-Phase-7 consumer checklist (ALL must be confirmed migrated before Phase 8):**

| Consumer | File | Migration |
|---|---|---|
| `makeLists()` call | `mclib/terrain.cpp:988-989` | Disabled under flag in Phase 4 |
| `geometry()` call | `mclib/terrain.cpp:988-989` | Disabled under flag in Phase 4 |
| `TerrainQuad::draw()` loop | `mclib/terrain.cpp` (drawPass zone) | Suppressed under flag in Phase 4 via outer gate |
| `TerrainQuad::drawMine()` per-quad loop | `mclib/terrain.cpp:1170-1187` (minePass zone) | Phase 7 (this phase) |
| `inverseProject()` terrain picking | `code/missiongui.cpp:739` | Phase 7 (this phase) |
| `calcThisFrame & 2` water sweep reset | `mclib/mapdata.cpp` (makeLists) | Obsoleted by Phase 3 `lastWaterFrame` migration; sweep reset remains in old path only |
| `setObjBlockActive` in geometry() | `mclib/terrain.cpp:1087` | Phase 2 bridge removed Phase 4; object activation via `objBlockInfo` must be re-examined if game logic outside render reads it |

### Modified Files

- `mclib/terrain.cpp`
  - Add `void Terrain::enqueueMines()`: walk `s_blockMeta[]` for `inFrustum=true` blocks; for each block, iterate tile cells within the block footprint; call `GameMap->getMine(actualCellRow, actualCellCol)` directly; if mine present, emit mine draw triangle via `mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA)` using the same triangle data as `drawMine()` emits. Call `enqueueMines()` from `Terrain::render()` under `MC2_TERRAIN_LOD_CHUNK=1` in place of the per-quad `drawMine()` loop.
  - In `Terrain::render()` under `MC2_TERRAIN_LOD_CHUNK=1`: skip the minePass per-quad `drawMine()` loop; call `Terrain::enqueueMines()` instead.
  - Do NOT call `setupTextures()` from the new render path.

- `mclib/camera.cpp` or new `mclib/terrain_pick.cpp`
  - Add `bool Terrain::heightfieldRaycast(const Stuff::Vector2DOf<long>& screenPos, Stuff::Vector3D& outWorld)`: ray from camera origin through `screenPos` (using camera inverse projection for ray direction only). March ray through heightfield cells at LOD0. Test two triangles per cell. Binary refine at crossing. Return nearest hit in `outWorld`.
  - `Stuff::Vector2DOf<long>` is an mclib-internal type. Its use in `code/missiongui.cpp` crosses the `code/`→`mclib/` boundary. Per renderspine constraint 4, this crossing must name its seam. Resolution: declare `Stuff::Vector2DOf<long>` as an approved POD crossing type in `docs/render-contract.md` (it is a plain 2-int struct with no render-internal members), OR provide a `GameAdapters` wrapper `TerrainPick_ScreenToWorld(int sx, int sy, float& wx, float& wy, float& wz)` that `code/missiongui.cpp` calls instead of the templated method. The GameAdapters approach is preferred; it keeps `Stuff::Vector2DOf<long>` mclib-internal.
  - **If using GameAdapters approach:** add `bool TerrainPick_ScreenToWorld(int sx, int sy, float& wx, float& wy, float& wz)` to `GameOS/gameos/gos_terrain_lod_chunk.h` (or a new `mclib/terrain_pick_adapter.h`); implement in terms of `Terrain::heightfieldRaycast()`; `code/missiongui.cpp` calls this adapter.

- `code/missiongui.cpp:739`
  - Under `MC2_TERRAIN_LOD_CHUNK=1`, replace `eye->inverseProject(mouseXY, wPos)` with `TerrainPick_ScreenToWorld(mouseX, mouseY, wPos.x, wPos.y, wPos.z)` (or equivalent adapter call).

- `mclib/quad.cpp`
  - `setupTextures()`: the `getMine()` calls inside `setupTextures()` collect mine DATA only; they do not emit draw triangles. Under `MC2_TERRAIN_LOD_CHUNK=1`, these data-collection calls may be skipped if `setupTextures()` is not called at all. No guard needed inside `setupTextures()` itself; simply do not call `setupTextures()` from the new path.

### GameOS Functions Needed

None (or `TerrainPick_ScreenToWorld` adapter stub if GameAdapters approach chosen — add to `gos_terrain_lod_chunk.h` or a new adapter header).

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` (existing).

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. Mines render correctly on all tier1 missions with `MC2_TERRAIN_LOD_CHUNK=1`. No mine triangles duplicated or missing.
3. Mouse click on terrain picks correct world position (heightfield raycast result matches expected tile under cursor, tested manually on a known terrain feature).
4. Consumer checklist above: all rows confirmed migrated. Sign-off required before Phase 8 begins.
5. Boundary seam for `code/missiongui.cpp` → terrain pick is documented (either GameAdapters adapter or approved POD crossing in `docs/render-contract.md`). Renderspine constraint 4 satisfied.
6. Old path unchanged when flag unset: `drawMine()` still called from minePass loop; `missiongui.cpp` still uses `inverseProject()`.
7. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. No crash, no assert, mines visible.

### Dependencies

Depends on Phase 4 (old draw path disabled under flag; new path active). Depends on Phase 6 (terrain renders cleanly; stable baseline for picking validation).

---

## Phase 8: Legacy dependency EXTRACTION (was: "delete legacy draw path")

> **DEPENDENCY TRUTH — 2026-06-08 (supersedes earlier Phase 8 framing and several stale premises below).**
> A four-investigator audit (cull-fix validated first: frustum-plane X-mirror fixed in `Camera::extractFrustumPlanes`, commit a280dde2) found Phase 8 is **NOT a pure deletion**. The legacy path is still doing **non-render work** that the new path does not yet own. Phase 8 is re-scoped from *deletion* to *dependency extraction*. Actual deletion is the FINAL step, gated on the prereqs below.
>
> **Stale premises in this doc that are FALSE (do not trust them):**
> - ❌ "Phase 4 suppresses `geometry()`" (asserted ~lines 219-221, 412-416). **FALSE.** `geometry()` runs **unconditionally** every frame via `code/mission.cpp:569` (and the editor via `editor/EditorInterface.cpp:759`). Phase 4 only gated `draw()` and `makeLists()`, never `geometry()`. This is *why* the flag path works today.
> - ❌ `geometry()` is called from `Terrain::update()`. **FALSE** — only `makeLists()` is (terrain.cpp:1434, guarded `!getenv(MC2_TERRAIN_LOD_CHUNK)`).
> - ❌ A `Terrain::enqueueMines()` exists. **FALSE** — the migrated entry point is `TerrainQuad::enqueueMinesFromGrid()` (quad.cpp:487).
>
> **Live consumers that BLOCK blanket deletion (each needs extraction first):**
> | Blocker | Evidence | Why it blocks |
> |---|---|---|
> | `objBlockInfo[].active` producer | `geometry()` slimReduce loop (terrain.cpp:2786) is the SOLE writer; `objmgr.cpp:2193` READS it every frame to gate object `update()`/lifecycle (gates/turrets/power — `cull_gates_are_load_bearing`) | Delete `geometry()` → terrain-object logic silently dies |
> | `makeLists()` / `geometry()` lists | The **GPU-indirect** recipe build itself consumes the camera-windowed `vertexList`/`quadList` (`EditorData.h:30-34`) | These are SHARED DATA producers, not legacy-draw artifacts. Game AND editor depend equally. |
> | `calcThisFrame` field | GPU lighting feeder reads it (`gos_terrain_lighting.cpp:652,656,1000`) — outside `mclib/code/`, so the acceptance grep at criterion 1 would MISS it | Delete field → GPU lighting breaks |
> | Mine grid enqueue | Under the flag, `makeLists()` is skipped → `quadList`/`numberQuads` empty → `enqueueMinesFromGrid(quadList,…)` (terrain.cpp:2026) is a **no-op**; mines survive only via default-on indirect `DrawMineStatic`. Verified 2026-06-08 (MINE_AB diagnostic: 0 lines under flag, fires under legacy). | Mine ownership under the flag is the indirect path, not the grid path — confirm before deleting `drawMine()` |
> | Terrain picking | New `Terrain::raycastTerrain` (inside `Camera::inverseProject`, camera.cpp:894) **falls back to the legacy quadList path on a miss**; never runtime-parity-tested | Delete legacy pick → misses have no fallback |
>
> **Editor is NOT a total blocker (corrected):** the editor runs the canonical **default-on modern chain** (GPU indirect/lighting/static-props/ViewUniforms) and inherits default-on arming, so the legacy DRAW loops (`TerrainQuad::draw`, `drawMine`, per-quad `setupTextures`) **self-skip in the editor exactly like the game**. The editor's only hard terrain dependency is the **data/list production** (`makeLists`/`geometry`) — the same shared dep the game's indirect path has. So "editor migration" is NOT a precondition for deleting the legacy *draw* surface; the real long-pole is re-homing the indirect recipe build off `vertexList`/`quadList`.
>
> **Extraction sub-phases (do in evidence order; deletion is last):**
> - **8a — `objBlockInfo.active` re-home.** Biggest game blocker. New producer = a standalone block-visibility pass writing `objBlockInfo[].active` (keep `objectBlockActive` a SEPARATE, possibly more-conservative concept than `terrainBlockVisible` — start with `terrainBlockVisible` expanded by a one-block margin, or match old semantics if grep proves them). Must survive `geometry()` deletion.
> - **8b — indirect recipe build re-home.** Decouple the GPU-indirect recipe build from `makeLists`/`geometry` `vertexList`/`quadList` (shared game+editor long-pole). Until done, `makeLists`/`geometry` Loop1 STAY.
> - **8c — `calcThisFrame` / lighting feeder.** Migrate `gos_terrain_lighting` off `calcThisFrame` (explicit dirty/light-gen stamps) before deleting the field. Until done, keep the field.
> - **8d — mine path decision.** Make the flag relationship explicit: under the flag, mines = indirect `DrawMineStatic` (grid enqueue is a no-op due to empty quadList). Decide whether `drawMine()` is retired (requires indirect-mine as the committed steady state) or kept.
> - **8e — picking fallback removal.** Only after: raycast hit-rate telemetry + raycast-vs-inverseProject parity test (instrument added 2026-06-08: `MC2_TERRAIN_PICK_PARITY`) + defined miss behavior. Then remove the legacy quadList fallback.
> - **8z — deletion.** Only after 8a–8e + Phase 10 fidelity. The original deletion checklist below is this final step.
>
> **Suggested phase reorder:** 8 (extract non-render deps) → 9 (editor/compat strategy) → 10 (terrain fidelity: normals/materials/lights/shadows) → 11 (default-on validation) → 12 (deletion). Making the chunk path the only path BEFORE Phase 10 ships visibly-lower-fidelity terrain — premature.

---

### 8a/8b/8c Extraction Spec (2026-06-08, from producer-contract recon)

**Goal (re-anchored on the perf hotspot):** retire the unconditional O(`realVerticesMapSide²`) `geometry()` slimReduce per-vertex walk (terrain.cpp:2748) — the structural blocker for 1K×1K maps (~1M verts/frame). slimReduce keeps two outputs alive under the flag; re-home BOTH onto the O(blocks) chunk grid, then gate the per-vertex loop off.

**The two surviving outputs (recon-confirmed):**
- **A. object-active set:** `objBlockInfo[b].active = true` AND `objVertexActive[vn] = true`, set `if (rv->clipInfo)`. `clipInfo == onScreenR` = the **768u/384u-dilated angular cone** cull (terrain.cpp:2753-2805), NOT a tight frustum. Per-frame reset to `false` is in `objmgr.cpp:520` (additive within frame — keep). Readers AND-gate on `objVertexActive[obj->getVertexNum()]` (objmgr.cpp:1822/1960/2201/3546) plus `objBlockInfo[].active` (objmgr.cpp:2193 lifecycle). **FP-safe, FN-unsafe.**
- **B. solid recipe window:** `gos_terrain_indirect::AppendSolidWindowCandidate(vertexNum)` per cull-active vertex, gated `clipInfo && SolidWindowEnabled() && RecipeForVertexNum(vn)`. Consumer = order-independent flat `uint[]` SSBO (binding 9); empty→full-range fallback exists. Must be filled between `BeginFrameSolidWindow()` and `ComputeDispatch()`.

**Replacement = ONE block-driven pass** (`Terrain::produceActiveSetFromChunks()`), called under `MC2_TERRAIN_LOD_CHUNK=1` in `geometry()` in place of the slimReduce loop. Prototype already exists: `MC2_BLOCK_FRUSTUM_FALLBACK` (terrain.cpp:2967-3000) iterates blocks and writes `objVertexActive` over each block's `[rowStart,rowEnd)×[colStart,colEnd)` span — lift that loop.
```
BeginFrameSolidWindow();
for each chunk block (bx,by) with effectiveActive(bx,by):       // see margin below
    // A: object-active  (reindex 2-D — chunk side != objBlock side!)
    if (bx < blocksMapSide && by < blocksMapSide)
        objBlockInfo[bx + by*blocksMapSide].active = true;
    for each vertex (col,row) in block span:
        vn = row*realVerticesMapSide + col;
        objVertexActive[vn] = true;
        // B: solid window
        if (SolidWindowEnabled() && RecipeForVertexNum(vn))
            AppendSolidWindowCandidate(vn);
```

**Margin — the load-bearing correctness rule (superset obligation):** `s_blockMeta[].inFrustum` is a block-AABB frustum test (the one just fixed in a280dde2). It is TIGHTER than the legacy dilated angular cone near-camera and at edges. To guarantee `chunkActiveSet ⊇ legacyActiveSet` (FN=0), `effectiveActive(bx,by)` must dilate:
- **near-field:** any block whose AABB is within `CLIP_THRESHOLD_DISTANCE` (768u) of the camera → force active (legacy = "everything within 768u on-screen");
- **one-block neighbor ring** around each `inFrustum` block (covers the 384u extent slack + AABB tightness);
- (alternative: expand block AABBs by 384u before the frustum test — pick whichever proves FN=0).

**Index mapping (must-not-get-wrong):** `s_terrainChunkSide = ceil((realVerticesMapSide-1)/verticesBlockSide)`; `blocksMapSide = realVerticesMapSide/verticesBlockSide` (FLOOR). They diverge on non-multiple-of-20 maps. Convert via 2-D `(bx,by)` with `bx,by < blocksMapSide` guard. `vertexNum = row*realVerticesMapSide + col` (row-major). `verticesBlockSide = 20`.

**Sub-phases & gating:**
- **8a:** implement `produceActiveSetFromChunks()` producing A only; run it ALONGSIDE legacy slimReduce (do NOT gate slimReduce off yet). Add A/B diagnostic `MC2_TERRAIN_ACTIVE_AB`: assert `chunkSet ⊇ legacySet`, log `falseNeg` (must be 0) and `falsePos` (informational). Tune margin until falseNeg==0 across tier1 + a 1K map + full camera sweep (pan/zoom/rotate).
- **8b:** extend the same pass to produce B (solid window); validate with the EXISTING parity probe (gos_terrain_indirect.cpp:2293, `dropped` must stay 0) — it compares the window against `objVertexActive`. Since the same pass now produces both, parity is structural.
- **8c:** ONLY after 8a falseNeg==0 AND 8b dropped==0 on the sweep: gate the per-vertex slimReduce loop off under `MC2_TERRAIN_LOD_CHUNK=1` (skip the `for ri<numberVertices` walk; run `produceActiveSetFromChunks()` instead). Legacy slimReduce stays for flag-unset + editor.

**Acceptance (8a–8c):**
1. Flag unset: legacy `geometry()` byte-unchanged.
2. Flag set: per-vertex slimReduce loop NOT entered (verify via Tracy `Terrain::geometry slimReduce` zone absent / a counter).
3. `MC2_TERRAIN_ACTIVE_AB`: falseNeg==0 vs legacy across tier1 + 1K map + camera sweep.
4. Solid-window parity probe: `dropped`==0.
5. objBlockInfo.active count ≈ legacy or conservative superset; no object vanish, no gate/turret/power logic stall (manual check on a gate mission).
6. 1K map: no O(`realVerticesMapSide²`) terrain CPU loop per frame (Tracy slimReduce zone gone; cost now O(visible blocks × ~21²)).

---

### FINAL deletion step (8z) — BLOCKED until 8a–8e + Phase 10 complete

**Slice kind:** cleanup (dead code deletion; no behavior change under MC2_TERRAIN_LOD_CHUNK=1)
**Goal:** Remove all legacy terrain draw path code, once every dependency above is extracted.
**GL discipline:** No new GL calls. Deletions only.

**HARD GATE: deletion is BLOCKED unless 8a–8e are complete AND the acceptance grep also covers `GameOS/` (calcThisFrame lighting feeder), not just `mclib/ code/`.**

### Modified Files

- `mclib/mapdata.cpp`
  - Delete `MapData::makeLists()` function body and declaration.

- `mclib/terrain.cpp`
  - Delete `Terrain::geometry()` Loop 1 (vertex projection loop, lines 1620+).
  - Delete `Terrain::geometry()` Loop 2 (`setupTextures` call loop) — verify no remaining callers first.
  - Delete `Terrain::render()` `TerrainQuad::draw()` loop (drawPass zone, old path).
  - Delete `Terrain::render()` minePass per-quad `drawMine()` loop (replaced by `enqueueMines()`).
  - Delete calls to `makeLists()` and `geometry()` in `Terrain::update()`.
  - Delete `setObjBlockActive()` / `clearObjBlocksActive()` calls that were inside geometry() (replaced by Phase 2 bridge, itself removed in Phase 4).
  - Remove all `MC2_TERRAIN_LOD_CHUNK` branch guards — the flag now has no old path to gate.

- `mclib/vertex.h`
  - Delete `DWORD calcThisFrame` field.
  - `Vertex::init()`: remove `calcThisFrame = 0` initialization.

- `mclib/quad.cpp`
  - Delete `calcThisFrame` bit-read/write in `setupTextures()` (already migrated in Phase 3; remove dead code).
  - Simplify `setupTextures()` — remove any `MC2_TERRAIN_LOD_CHUNK` guards added in Phase 7 (they are now dead).

- `mclib/terrain.h`
  - Remove `static ObjBlockInfo* objBlockInfo` or retain if game-logic (object activation) outside the render path still reads `objBlockInfo[i].active`. Verify by grep before deleting.

### GameOS Functions Needed

None.

### Env Gate

After Phase 8, `MC2_TERRAIN_LOD_CHUNK` is no longer needed as a guard (old path is gone). Retain the env var as a feature-enable or remove it. If removed, update `docs/tier1_env_vars.md` and remove from the smoke gate invocations above.

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo. Grep confirms zero references to `makeLists`, `calcThisFrame`, or the old `drawMine()` loop in non-editor `mclib/` or `code/`: `grep -rn "makeLists\|calcThisFrame" mclib/ code/` returns zero matches.
2. Terrain renders correctly: tier1 5-mission smoke with `MC2_RENDER_CONTRACT_ASSERT=1`. No assert, no crash, terrain visible, mines visible, picking works.
3. Binary size reduced relative to Phase 7 (optional sanity check).
4. No functional regression: terrain, mines, picking, object activation all work.
5. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_RENDER_CONTRACT_ASSERT=1`. Pass. (No `MC2_TERRAIN_LOD_CHUNK` needed; old path deleted. If env var retained: set it to 1.)

### Dependencies

Depends on Phase 7 (all consumers migrated; consumer checklist signed off).

---

## Phase 9: Superchunk coarse draw (optional optimization)

**Slice kind:** dispatch-changing (replaces 16 per-block LOD5 commands with 1 superchunk command)
**Goal:** When all 16 blocks in a superchunk are visible and all select LOD5, replace the 16 individual draw commands with a single superchunk-sized draw command covering the full 80x80 quad footprint.
**GL discipline:** New superchunk index buffer built in `gos_terrain_lod_chunk.cpp`. New `gos_TerrainLodChunk_SubmitSuperchunkCommands()` GameOS function.

**Note on partial superchunks (1021-vertex map):** Edge superchunks on a 1021-vertex map contain partial-edge blocks. The LOD5 coalesce optimization (all 16 blocks at LOD5) is only valid when all constituent blocks are full 20x20-quad blocks. Do not coalesce edge superchunks that contain partial-edge blocks. The coalesce check must verify `bm.quadCountX == verticesBlockSide && bm.quadCountY == verticesBlockSide` for all 16 blocks.

### Modified Files

- `GameOS/gameos/gos_terrain_lod_chunk.h`
  - Add `struct TerrainSuperchunkDrawCommand { int scOriginX; int scOriginY; int _pad[2]; };`
  - Declare `void gos_TerrainLodChunk_SubmitSuperchunkCommands(const TerrainSuperchunkDrawCommand* cmds, int count)`.

- `GameOS/gameos/gos_terrain_lod_chunk.cpp`
  - Build superchunk index buffer at `Init()`: 80x80 quads at LOD5 stride = one large triangle mesh. This buffer is static; reused per superchunk draw.
  - Implement `SubmitSuperchunkCommands()`: bind superchunk index buffer; for each command, set uniforms `u_blockOriginX/Y` to `scOriginX/Y`, `u_lodStep=20`, draw.

- `mclib/terrain.cpp`
  - Cull loop: after per-block LOD selection, check if all 16 blocks in a superchunk are `inFrustum && lodLevel==5 && quadCountX==20 && quadCountY==20`. If so, emit a `TerrainSuperchunkDrawCommand` instead of 16 individual `TerrainDrawCommand`s.
  - Call `gos_TerrainLodChunk_SubmitSuperchunkCommands()` from `Terrain::flushDrawCommands()` after the per-block `SubmitDrawCommands()` call.

- `code/gamecam.cpp`
  - The flush site already calls `Terrain::flushDrawCommands()`; no change needed here if superchunk flush is inside that method.

### GameOS Functions Needed

`gos_TerrainLodChunk_SubmitSuperchunkCommands(const TerrainSuperchunkDrawCommand*, int)` — new, real implementation.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` (existing). A `MC2_TERRAIN_LOD_CHUNK_SUPERCHUNK=1` sub-gate is acceptable for A/B testing.

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. At extreme zoom-out (full 1K map visible), draw command count for interior superchunks reduced (16 per-block commands replaced by 1 superchunk command). Edge superchunks with partial blocks remain as individual per-block commands. Verify via Tracy `Terrain::drawChunks` zone or a count log.
3. No visual difference from Phase 8 at extreme zoom-out.
4. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. Pass.

### Dependencies

Depends on Phase 8 (old path deleted; clean baseline). Phase 9 is optional and may be deferred indefinitely.

---

## Phase 10: Normal/material/light/shadow GPU streams; terrain edit dirty propagation

> **STATUS 2026-06-09: PHASE 10 COMPLETE — chunk terrain at near-parity with legacy; CUTOVER-READY.**
> Shipped (commits `9c99b217`→`b00a7930`, all opt-in `MC2_TERRAIN_LOD_CHUNK=1`): edge
> stitching (vertex neighbour-min snap, no T-junction cracks); shadows (shadow.hglsl,
> static+dynamic); smooth heightfield normal (frag bilinear central-diff, kills cliff
> faceting); material detail normals + legacy colour mapping (tints/cliff/fbm-breakup);
> concrete + cement catalog atlas (frag-side per-tile lookup, LOD-independent); and all
> ImGui tunables (hemisphere V1/V2, normals-from-height strength, material profile, POM,
> anti-tiling — byte-safe defaults). Plus a NORMAL-ARRAY mip-completeness fix (helped the
> legacy array path too) + `MC2_TERRAIN_NORMAL_ARRAY` default-on.
>
> **Implementation impl notes (NOT the planned _pbr.frag split — done inline in
> terrain_lod_chunk.frag/.vert):**
> - The frag does NOT use `terrain_lod_chunk_pbr.frag`; the production shading lives in
>   `terrain_lod_chunk.frag` directly.
> - **3 LOAD-BEARING DEPTH/MVP RULES** for the chunk draw (any new terrain draw must obey;
>   each was a real chunk-only bug this session): (1) net depth **-0.004** (legacy thin
>   path double-applies `TERRAIN_DEPTH_FUDGE`: vert `clip.z+=FUDGE*w` + frag `+FUDGE`) —
>   `4da9cfb1`; (2) project with the **baked dispatch MVP** water-cull/decals use
>   (`IsFrameSolidArmed()?getDispatchMvp16():live`, frame N-1) not the live MVP, or a
>   1-frame offset tears decals + drops shore water on motion — `67e4f5e4`; (3) bias depth
>   **PRE-DIVIDE in the vert**, never frag `gl_FragDepth` (AMD early-Z) — `67e4f5e4`.
> - Canonical record: `memory/HANDOFF_2026_06_09_terrain_lod_chunk_phase10_fidelity_cutover_prep.md`.
>
> **NEXT = Phase 11 default-on cutover** (gate flip + chunk-path smoke + editor/legacy
> fallback check), then Phase 8z deletion of slimReduce/makeLists.

**Slice kind:** dispatch-changing (adds per-frame GPU buffer uploads for normals, materials, lights, shadow)
**Goal:** Bring terrain LOD chunk path to full visual fidelity: GPU-side normal computation from heightfield, material/texture-ID splatting, light integration, and shadow map sampling. Also: full dirty propagation for real-time terrain edits (editor path).
**GL discipline:** All new SSBO/texture uploads in `gos_terrain_lod_chunk.cpp`. `mclib/` calls GameOS API only.

### New Files

- `shaders/terrain_lod_chunk_pbr.frag` — PBR fragment shader: samples material IDs from material texture, fetches per-material normal maps, applies `terrainLightDir`, POM, shadow map sampling. Replaces the Phase 4 solid-color stub.

### Modified Files

- `GameOS/gameos/gos_terrain_lod_chunk.h`
  - Declare `gos_TerrainLodChunk_UploadNormalPatch(...)`.
  - Declare `gos_TerrainLodChunk_UploadMaterialMap(...)`.
  - Declare `gos_TerrainLodChunk_UploadLightData(...)`.

- `GameOS/gameos/gos_terrain_lod_chunk.cpp`
  - Allocate normal SSBO and material texture at `Init()`. Upload at map load. Dirty-patch on terrain edit.
  - Shadow sampling: new terrain path samples from existing shadow map (binding from `gos_postprocess.cpp`; do not duplicate). Shadow stream deferred to spec R3 — see spec: "deferred to Phase 10."
  - Restore all GL state after draw.

- `mclib/terrain.cpp`
  - `setVertexHeight()` (terrain edit): when `s_blockMeta[blockIdx].dirtyAabb=true` (using block index from the linear VertexIndex formula from Phase 3), also recompute normal patch and call `gos_TerrainLodChunk_UploadNormalPatch()`.

- `shaders/terrain_lod_chunk.vert`
  - Compute per-vertex normal from heightfield finite differences (or pass to fragment shader for per-fragment normal from normal SSBO).

### GameOS Functions Needed

- `gos_TerrainLodChunk_UploadNormalPatch(...)`.
- `gos_TerrainLodChunk_UploadMaterialMap(...)`.
- `gos_TerrainLodChunk_UploadLightData(...)`.

### Env Gate

`MC2_TERRAIN_LOD_CHUNK=1` (existing).

### Acceptance Criteria

1. Build succeeds in RelWithDebInfo.
2. Terrain visual quality matches or exceeds old per-quad draw path: normal maps visible, material splatting visible, shadow receiving visible.
3. Terrain edits propagate correctly: editing a vertex in-game (if editor path active) updates SSBO within one frame, no stale height visible.
4. No `gl*()` in `mclib/` (MC2_RENDER_CONTRACT_ASSERT=1 does not fire).
5. Smoke gate: `py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` with `MC2_TERRAIN_LOD_CHUNK=1` AND `MC2_RENDER_CONTRACT_ASSERT=1`. Pass.

### Dependencies

Depends on Phase 8 (clean baseline). Phase 10 is the final fidelity phase; order of sub-tasks within Phase 10 (normals, materials, lights, shadows) may be sequenced independently.

---

## Invariant Traceability

| Invariant | Phase that writes it |
|---|---|
| I1: No gl*() in mclib/ | Phase 1 (MC2_RENDER_CONTRACT_ASSERT guard); verified per-phase |
| I2: AABB cull no false negatives | Phase 2 (validation log + zero-mismatch requirement); CRIT-1 write-before-gate ordering verified |
| I3: Height SSBO matches CPU elevation at frame start | Phase 3 (debug-build first-frame compare) |
| I4: Exactly one DrawCommand per visible block per frame | Phase 4 (Tracy zone assert expectedVisible==s_cmdCount) |
| I5: lodStep in {1,2,4,5,10,20}; k_lodSteps table size==6 | Phase 5 (chooseLOD assert + static_assert) |
| I6: Old path unmodified when MC2_TERRAIN_LOD_CHUNK unset | Phase 1+ (smoke gate with flag unset each phase) |
| I7: s_blockMeta/s_superchunkMeta from terrainHeap; freed in destroy() | Phase 1 (heap accounting + reload sentinel) |
| I8: Dirty-patch upload byte count exact (uses actual quadCountX/Y, not constant 20) | Phase 3 (assert in UploadHeightPatch) |
| I-registry-23: SSBO binding 23 registered before Phase 3 merge | Phase 3 (render-binding-registry.md update, hard gate) |
| I-struct-size: sizeof(TerrainDrawCommand) == 16 (or 20 if Phase 6 Option B) | Phase 1 (16); Phase 6 must update if Option B chosen |
| I-flush-caller: SubmitDrawCommands called only from gamecam.cpp flush site | Phase 4 (grep verify: zero matches in mclib/terrain.cpp) |
| I-partial-block: partial-edge blocks (quadCountX/Y < 20) handled correctly | Phase 1 (allocation) + Phase 4 (index buffers + shader uniforms) |
