# PR2c — Mine static-bake (design)

**Status:** spec, ready for executor.
**Date:** 2026-05-08
**Predecessors:** PR1 SOLID + cement-atlas; PR2a + PR2b specs (independent
ship order); PR2 Stage 0 recon docs ([1](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md)
through [6](../explorations/2026-05-07-pr2-stage0-recon-6-render-order-hooks.md)).
**Brainstorm:** [`2026-05-01-detail-overlay-consolidation-scope.md`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)
— **Q4-mine and Q6-mine answers REPLACED.** The brainstorm scoped PR2c
as a per-frame indirect-draw with parallel `MineCellRecipeSSBO` keyed
by `(vertexNum × 16 + cellIdx)` + per-frame thin record + indirect
command buffer. User direction 2026-05-08 reframed: mines are sparse
(2-3 missions max, small overlay each), state changes only on
gameplay events, and **really only need to be drawn once** — not
iterated per frame. This spec implements the reframe.

> Discipline: every cited symbol grep-verified at write-time per worktree
> CLAUDE.md "Documentation Discipline." Verification appendix at end.

---

## Goal

Retire the per-frame mine setup + draw cost (~157 µs/frame measured
2026-05-08, on missions WITH mines AND on missions WITHOUT mines —
the legacy iteration runs unconditionally, gated only at the
per-quad `tileHasMines` short-circuit which still costs the dispatch).

Replace with a static, mission-scoped vertex buffer rebuilt only when
mine state actually changes. Per-frame work for mines collapses to
**one `glDrawArrays` against a static VBO**, or zero if the mission
has no mines.

The legacy paths to retire:

- [`mclib/quad.cpp:251`](../../../mclib/quad.cpp) — `static void enqueueTerrainMineState(TerrainQuad& quad)`,
  called from [`quad.cpp:760`](../../../mclib/quad.cpp) inside `setupTextures()`'s
  per-quad loop.
- [`mclib/quad.cpp:4240`](../../../mclib/quad.cpp) — `void TerrainQuad::drawMine(void)`,
  called from [`mclib/terrain.cpp:1011`](../../../mclib/terrain.cpp)
  inside the per-quad terrain render walk.

---

## Architecture

```
Static (built once at mission init OR on first setMine event):
└── MineStaticVBO_GL  vertex buffer (GL_ARRAY_BUFFER)
    Layout: gos_VERTEX-derived per cell (alpha-textured tris).
    Total geometry = 2 tris × 16 cells × N mine-bearing tiles.
    Stock tier1 worst case: small (mines on ≤3 missions per user
    direction; per-mission peak likely < 1000 cells = < 6000 verts).
    
    Built by walking MissionMap once: for each cell where map.mine != 0,
    emit 2 triangles in world space using the cell's elevation and
    the mine/blown texture-array layer (1 = mine, 2 = blown → maps to
    layer 0 / layer 1 in the texture array).

└── MineTextureArrayGL  GL_TEXTURE_2D_ARRAY at sampler unit 5
    2 layers × 16×16 RGBA8:
      layer 0 = mineTextureHandle  (defaults/mine_00.tga, value=1)
      layer 1 = blownTextureHandle (defaults/minescorch_00.tga, value=2)
    Built once at first build via the same slot→layer pattern PR1
    cement-atlas uses (mcTextureManager slot index, NOT gosHandle).
    sampler2DArray with NEAREST + CLAMP_TO_EDGE (gosHint_DisableMipmap
    matches the legacy load flags at quad.cpp:524, :531).

Per-frame:
└── Render.TerrainMines zone — ONE glDrawArrays(GL_TRIANGLES, 0, vertCount)
    Or skip entirely if vertCount == 0 (mission has no mines and no
    minelayer activity has fired). No CPU iteration. No SSBO bind
    overhead beyond program + texture-array.

Invalidation:
└── On MissionMap::setMine (single chokepoint per recon-5):
      mark MineStaticVBO dirty.
    On MissionMap::rebuildTileMineCounts (after readPacket):
      mark MineStaticVBO dirty (full rebuild needed).
    Rebuild lazily: at next paint cycle, if dirty, walk MissionMap
    cells once and re-emit the VBO. Single rebuild per state-change
    burst; if 50 cells change in one frame (chain explosion), still
    one VBO rebuild the next frame.
```

### Why static-bake, not per-frame indirect

Brainstorm Q4-mine assumed mines need per-frame indirect-draw with
thin-record packing. That's the right shape for SOLID terrain (every
quad emitted every frame, lighting + visibility per-frame). It's the
WRONG shape for mines:

- Mine geometry doesn't move per-frame (cell elevation is static).
- Mine texture doesn't change per-frame (layer is fixed by mineState
  value, which mutates only on `setMine` events — gameplay-rare).
- Lighting on mine sprites is decorative (alpha-cutout sprite, not
  surface-shaded).

Per-frame iteration produces the same vertex buffer 60 times per
second. Static-bake produces it ~once per mission, plus N times per
mission where N = number of `setMine` events. Empirically tiny.

### Per-mission lifecycle

- `Terrain::primeMissionTerrainCache`: call `BuildMineStaticVBO()` once
  after `MissionMap` is fully populated. Walks all cells; if any have
  mineState != 0, emits geometry.
- `Terrain::destroy`: call `ResetMineStaticVBO()` — keeps GL buffer
  allocated, CPU-clears state and zeros vert count (WaterStream pattern
  per `memory/water_ssbo_pattern.md`). Next mission's
  `BuildMineStaticVBO` reuses the buffer.
- `MissionMap::setMine`: set `g_mineVBODirty = true`. No rebuild
  inline (cheap — single bool write).
- `MissionMap::rebuildTileMineCounts`: same — set dirty.
- Per-frame entry to `Render.TerrainMines`: if dirty, rebuild VBO,
  clear dirty.

The "gate updates on minelayer-unit inputs" insight from user 2026-05-08:
the chokepoint at `MissionMap::setMine` already covers all mine
mutations, including pre-placed mines (loaded via init), AI minelayer
laying mines, weapon impacts triggering mine state 2, etc. No need
to inspect the calling unit type — single hook is sufficient.

### Why texture-array instead of two samplers

Mine bucket has only 2 textures (layers). PR1 cement-atlas + PR2b
overlay-array both use `GL_TEXTURE_2D_ARRAY`; PR2c using the same
shape keeps the sampling pattern uniform across all three buckets.
Cost is identical at 2 layers (one texture-array vs two samplers
both bind once per frame). The consistency argument wins.

---

## Stages

### Stage 0c — scaffolding + 157 µs baseline confirmation (1 commit)

**Goal:** env gate + counters + cost-split timer wrapping
`enqueueTerrainMineState` and `drawMine`. Confirms the 157 µs/frame
baseline figure. No behavior change.

**Code change:**

- [`gos_terrain_indirect.h/.cpp`](../../../GameOS/gameos/gos_terrain_indirect.cpp):
  add `IsFrameMineArmed()`, `Counters_AddIndirectMineDrawnCells()`,
  `Counters_AddLegacyMineEnqueueQuad()` mirroring PR1's pattern.
- [`gameosmain.cpp`](../../../GameOS/gameos/gameosmain.cpp): `[INSTR v1]`
  banner adds `MC2_TERRAIN_INDIRECT_MINE`. 600-frame summary line
  extends.
- [`scripts/run_smoke.py`](../../../scripts/run_smoke.py): env-allowlist
  the new flag.
- Cost-split timer `CostSplitMineEnqueueScope` brackets
  [`quad.cpp:251`](../../../mclib/quad.cpp) `enqueueTerrainMineState` body.
  `CostSplitMineDrawScope` brackets [`quad.cpp:4240`](../../../mclib/quad.cpp)
  `drawMine()` body. Both env-gated by `MC2_TERRAIN_COST_SPLIT=1`
  (existing flag).

**Tier1 verification:** smoke gate clean (no behavior change).
Cost-split timer output should show ~157 µs combined across
`enqueueTerrainMineState` + `drawMine` per the user's 2026-05-08
measurement, with the bulk in `enqueueTerrainMineState` (the
per-frame iteration even on mine-free missions).

**Commit message:** `feat(pr2c): scaffold mine indirect endpoint
(env gate + counters + cost-split timer)`.

### Stage 1c — MineStaticVBO + texture-array + invalidation hooks (1 commit)

**Goal:** mission-static infrastructure. No draw yet; legacy path
runs unchanged.

**Code change:**

- `MineStaticVBO_GL` and `MineTextureArrayGL` declared in
  `gos_terrain_indirect.cpp` alongside `g_denseRecipes`.
- `BuildMineStaticVBO()`: walks `MissionMap` cell-by-cell; for each
  `cell.mine != 0`, emits 6 vertices (2 tris per cell) into the VBO
  with cell elevation + texture-array layer (0 for state=1, 1 for
  state=2). Uses `mcTextureManager` slot indices for the layer
  mapping (per `mc2_texture_handle_is_live.md` slot-vs-handle
  discipline). Mirrors PR1 `BuildCementCatalogAtlas` pattern at
  [`gos_terrain_indirect.cpp:486-708`](../../../GameOS/gameos/gos_terrain_indirect.cpp).
- `ResetMineStaticVBO()`: CPU-clear vert count, keep buffer allocated.
- `BuildMineTextureArray()`: 2-layer GL_TEXTURE_2D_ARRAY, 16×16 RGBA8,
  populated from `mineTextureHandle` (slot for `defaults/mine_00.tga`)
  and `blownTextureHandle` (slot for `defaults/minescorch_00.tga`)
  via `glGetTexImage` + `glTexSubImage3D`. NEAREST filter, no mips.
- Wire into chokepoints:
  - `Terrain::primeMissionTerrainCache`: call `BuildMineStaticVBO()`
    + `BuildMineTextureArray()` after `BuildDenseRecipe()`. The
    texture-array build can short-circuit if already built (mission
    teardown keeps it across reloads — the source TGAs don't change
    mid-process).
  - [`mclib/move.h:634-646`](../../../mclib/move.h) `MissionMap::setMine`:
    after the cell mutation + `tileMineCount` maintenance, if
    `IsMineArmingFlagSet()` (i.e., the env gate is on, regardless
    of frame-arm state), set the static `g_mineVBODirty = true`.
    No rebuild inline. **Single hook covers all 20 GameMap->setMine
    callsites + 2 internal MissionMap::setMine callsites per
    [recon-5](../explorations/2026-05-07-pr2-stage0-recon-5-setmine-chokepoint.md).**
  - [`mclib/move.cpp:875`](../../../mclib/move.cpp)
    `MissionMap::rebuildTileMineCounts`: same — set dirty post-rebuild.
- `RebuildMineStaticVBOIfDirty()`: lazy rebuild called from the bridge
  at draw time. Walks `MissionMap` once if dirty.

**Tier1 verification:** smoke gate clean (no behavior change yet).
Counter `indirect_mine_drawn_cells` reads zero (no draw site wired
yet). Counter `legacy_mine_enqueue_quads` continues showing legacy
work — confirms invalidation hooks don't disrupt legacy state.

**Commit message:** `feat(pr2c): build MineStaticVBO + 2-layer mine
texture-array + invalidation hooks`.

### Stage 2c — bridge function + draw + legacy gate-off (1 commit; partial-landing-hazard rule per N2)

**Goal:** ship the static-draw + legacy gate-off in the same commit.

**Code change:**

- `gos_terrain_mine_bridge_drawStatic` in
  [`gameos_graphics.cpp`](../../../GameOS/gameos/gameos_graphics.cpp),
  mirroring `gos_terrain_bridge_drawIndirect` at line 2275 — but
  simpler. State save/restore (VAO, sampler unit 5, depth, blend,
  color-mask), bind `MineStaticVBO_GL`, bind mine texture-array at
  unit 5, `glDrawArrays(GL_TRIANGLES, 0, mineVertCount)`. No SSBOs.
  No indirect command buffer. No per-frame data upload. AMD attrib-0
  `glEnableVertexAttribArray(0)` per usual.
- `DrawMineStatic()` in `gos_terrain_indirect.cpp` calls
  `RebuildMineStaticVBOIfDirty()` then the bridge.
- New zone in [`mclib/txmmgr.cpp`](../../../mclib/txmmgr.cpp):
  insert between `Render.TerrainOverlays` (current line 1777)
  and `Render.Decals` (current line 1782):
  ```cpp
  {
      ZoneScopedN("Render.TerrainMines");
      TracyGpuZone("Render.TerrainMines");
      if (gos_terrain_indirect::IsFrameMineArmed()) {
          gos_terrain_indirect::DrawMineStatic();
      }
  }
  ```
  Recommended placement per recon-6 + 2026-05-08 reasoning: mines
  paint above cement transitions / road overlays (TerrainOverlays
  zone), beneath crater decals (Decals zone). State=2 (blown mine
  sprite) coexists with crater decals for a hit-mine — both layered
  correctly under this ordering.
- Legacy gate-off:
  - [`mclib/quad.cpp:760`](../../../mclib/quad.cpp) `enqueueTerrainMineState(*this);`
    wrap with `if (!gos_terrain_indirect::IsFrameMineArmed()) { ... }`.
  - [`mclib/terrain.cpp:1011`](../../../mclib/terrain.cpp)
    `currentQuad->drawMine();` wrap likewise.
- Shader: new `shaders/gos_terrain_mine_static.vert` (`#version 430`) +
  `shaders/gos_terrain_mine_static.frag`. Vertex shader transforms
  world-space cell vertices through `terrainMVP` (per
  `terrain_mvp_gl_false.md`); samples sampler2DArray at unit 5;
  alpha-tested output.

**N2 rule (partial-landing hazard):** Stage 2c's draw + gate-off must
land together. Splitting them ships either:
- Draw without gate-off → both static AND legacy emit fire; mine
  geometry duplicated, perf gate fails (move work, don't eliminate).
- Gate-off without draw → mines disappear from missions that have
  them. Visible regression on the 2-3 mine-bearing missions.

**Tier1 verification:** smoke gate clean with `MC2_TERRAIN_INDIRECT_MINE=1`.
Cost-split timer should show `enqueueTerrainMineState` and
`drawMine` per-frame µs drop to ~zero. Tracy `Render.TerrainMines`
zone delta shows the static draw cost (one `glDrawArrays`,
expected < 5 µs per frame including dirty-rebuild amortization).

**Commit message:** `feat(pr2c): static-bake mine VBO draw + retire
per-frame enqueueTerrainMineState/drawMine`.

---

## Parity / verification gates

### Gate A (visual canary)

Mine sprites visible on the mine-bearing tier1 (or fallback tier2)
mission(s). Identifying which missions actually have mines is a
prerequisite — see "Open prerequisite" below. Once identified, smoke
diff on those missions must show:

- Mines render in the same world position as legacy.
- Mine sprite (state=1) and blown-mine sprite (state=2) both render
  correctly post a mid-mission state transition (e.g., a vehicle
  drives over a mine).
- Crater decals from weapon impacts overpaint mines correctly (Decals
  zone runs after Render.TerrainMines).

### Gate B (counter-cardinality cross-check)

Pre-Stage-2c: `legacy_mine_enqueue_quads` per frame counts mine quads
the legacy path enqueues. Post-Stage-2c with `MC2_TERRAIN_INDIRECT_MINE=1`:
that counter must drop to zero (gate-off fired). Concurrently,
`indirect_mine_drawn_cells` must equal the cell count the legacy path
would have emitted.

### Gate C (perf)

Tracy CPU delta on the smoke camera, post Stage 2c:

- `enqueueTerrainMineState` µs → 0 (path no longer fires).
- `drawMine` µs → 0.
- New `Render.TerrainMines` GPU zone µs → < 5 expected (one draw
  call + texture-array sample on at most a few thousand verts).

Net CPU win per the user's 2026-05-08 baseline: ~157 µs/frame on
ALL missions (the per-frame iteration ran unconditionally).

### Soak

7-day default-off + parity counter armed. Default-on flip after
soak passes.

---

## Risks / hazards

### R1. State-transition rebuild triggers VBO churn during chain explosions

A chain mine explosion can fire `setMine` multiple times in one
gameplay tick (mine A explodes → triggers mine B → state transitions
cascade). Each `setMine` call sets `g_mineVBODirty = true`. The
dirty flag is idempotent — multiple sets do nothing extra. Rebuild
fires once at next paint cycle.

**Mitigation:** the dirty-flag debounce is automatic. No code
mitigation needed; spec executor should verify by triggering a chain
explosion in QA and confirming exactly one VBO rebuild fires per
frame regardless of `setMine` call count.

### R2. Mid-mission rebuild stutter

`BuildMineStaticVBO()` walks the entire MissionMap (256² to 384²
cells). At 384² × ~1 µs per cell read = ~150 ms worst case if naive.

**Mitigation:** the walk only checks `cell.mine != 0`; non-mine
cells short-circuit. With sparse mine placement (per user, 2-3
missions × small overlay), the actual work is ≤ a few thousand cells.
Rebuild cost should be sub-millisecond. **Spec-executor must measure**:
if rebuild exceeds 1 ms, switch to `tileMineCount`-driven outer loop
(skip whole 4×4 tiles where `tileMineCount == 0`) — the same fast-skip
the legacy path uses at [`mclib/quad.cpp:264`](../../../mclib/quad.cpp).

### R3. Texture-array sampling on alpha-cutout sprites

Mine sprites are alpha-textured (per [`quad.cpp:524, :531`](../../../mclib/quad.cpp)
`gos_Texture_Alpha`). Texture array sampling preserves alpha; shader
must alpha-test (`if (texColor.a < 0.5) discard;`) to match legacy
look. Confirm at Stage 2c shader implementation.

### R4. Cell elevation source

Legacy `drawMine` builds vertex elevation from per-cell terrain data.
PR2c's `BuildMineStaticVBO` must use the same source — likely
`Terrain::terrainElevation(cellY, cellX)` or `MapData::blocks[].elevation`.

**Mitigation:** spec-executor reads the legacy `drawMine` body at
[`quad.cpp:4240`](../../../mclib/quad.cpp) and mirrors its elevation
source byte-for-byte. Visual canary catches divergence.

### R5. Sampler unit assignment collision

PR1 cement-atlas binds at unit 3, PR2b proposes unit 4, PR2c proposes
unit 5. Spec-executor verifies unit 5 is unclaimed via
`grep "glActiveTexture(GL_TEXTURE5)\|glBindSampler(5"` in
gameos/shaders/.

### R6. Pre-placed mines vs gameplay-laid mines

Pre-placed mines (loaded via `MissionMap::init` from mission packet
data) and gameplay-laid mines (via the 20 callsites in code/) both
end at `MissionMap::setMine`. **Both routes are covered by the
single dirty-flag hook.** No special handling needed.

The internal-init call at [`mclib/move.cpp:991`](../../../mclib/move.cpp)
fires for every cell during init; if there are no pre-placed mines
the iteration is fast (cell.mine == 0 → setMine(row,col,0) →
old==new, no tileMineCount mutation, dirty flag still gets set).
**This is an acceptable rebuild trigger** — fires once at mission
init, before any rendering, walks an empty cell set. No perf concern.

---

## Open prerequisite (resolved by spec-executor at Stage 0c)

**Identify which tier1 missions actually have mines.** Earlier
recon docs cited mc2_24 sourced from brainstorm Q3, but the user
corrected on 2026-05-08 that mc2_24 has no mines. Spec-executor at
Stage 0c verifies by:

- Running each tier1 mission with `MC2_TERRAIN_COST_SPLIT=1` and
  observing `legacy_mine_enqueue_quads` counter — non-zero on
  mine-bearing missions.
- OR spelunking stock mission `.pak` files for `MapCell.mine != 0`
  pre-placed values.
- OR inspecting which missions have minelayer units in their force
  pool.

This is a Stage 0c output, not a ship-blocker. The spec is
mechanically executable on any mission set; the visual canary at
Stage 2c uses whichever mission(s) the executor identifies.

---

## Rollback

Per stage:

- Stage 0c: harmless. Counters orphan, cost-split timers orphan.
- Stage 1c: harmless. Static VBO + texture-array allocate but draw
  never fires.
- Stage 2c: revertible by `git revert`. Default flag is off; soak
  window allows full operator-level rollback before default-on flip.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `enqueueTerrainMineState` (legacy path 1) | [`mclib/quad.cpp:251`](../../../mclib/quad.cpp) — `static void enqueueTerrainMineState(TerrainQuad& quad)` | M |
| 2 | `enqueueTerrainMineState` callsite | [`mclib/quad.cpp:760`](../../../mclib/quad.cpp) — inside `setupTextures()` per-quad loop | M |
| 3 | `TerrainQuad::drawMine` (legacy path 2) | [`mclib/quad.cpp:4240`](../../../mclib/quad.cpp) — `void TerrainQuad::drawMine (void)` | M |
| 4 | `drawMine` callsite | [`mclib/terrain.cpp:1011`](../../../mclib/terrain.cpp) — `currentQuad->drawMine();` | M |
| 5 | `MissionMap::setMine` chokepoint | [`mclib/move.h:634-646`](../../../mclib/move.h) — single mutation site for all 20 GameMap->setMine callers (per recon-5) | M |
| 6 | `MissionMap::rebuildTileMineCounts` | declaration [`mclib/move.h:659-661`](../../../mclib/move.h); definition [`mclib/move.cpp:875`](../../../mclib/move.cpp) | M |
| 7 | mine textures (2 distinct) | [`mclib/quad.cpp:524`](../../../mclib/quad.cpp) `mine_00.tga`, [`:531`](../../../mclib/quad.cpp) `minescorch_00.tga` | M |
| 8 | mine TGA dimensions | 16×16 RGBA8 each (verified by recon-2 disk inventory) | M |
| 9 | mine texture flags | `gos_Texture_Alpha \| gosHint_DisableMipmap \| gosHint_DontShrink` per [`quad.cpp:524, :531`](../../../mclib/quad.cpp) | M |
| 10 | `MissionMap::init` cell-load setMine call | [`mclib/move.cpp:991`](../../../mclib/move.cpp) — `setMine(row, col, mapData[...].mine)` per cell | M |
| 11 | `tileHasMines` fast-skip in legacy `enqueueTerrainMineState` | [`mclib/quad.cpp:264`](../../../mclib/quad.cpp) — `if (!GameMap->tileHasMines(tileR, tileC)) return;` | M |
| 12 | `Render.TerrainOverlays` zone (insertion point reference) | [`mclib/txmmgr.cpp:1777`](../../../mclib/txmmgr.cpp) | M |
| 13 | `Render.Decals` zone (insertion point reference) | [`mclib/txmmgr.cpp:1782`](../../../mclib/txmmgr.cpp) | M |
| 14 | PR1 cement-atlas slot-keyed lookup pattern (mirror) | `g_cementLayerIndexBySlot` per [`memory/indirect_terrain_solid_endpoint.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/indirect_terrain_solid_endpoint.md) | M |
| 15 | PR1 SOLID bridge template | [`gameos_graphics.cpp:2275`](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 16 | `terrainMVP` upload uses `GL_FALSE` | `memory/terrain_mvp_gl_false.md` | M |
| 17 | mc2_texture_handle_is_live: store slot, not handle | `memory/mc2_texture_handle_is_live.md` | M |
| 18 | WaterStream "keep buffer, CPU-clear state" pattern | `memory/water_ssbo_pattern.md` | M |
| 19 | 157 µs/frame baseline (pre-PR2c) | user direction 2026-05-08 — measured perf number on the legacy paths even on mine-free missions | M (user-attested; Stage 0c re-confirms via cost-split timer) |
| 20 | Mines static — no per-frame mutation absent gameplay event | user direction 2026-05-08 + recon-5 finding (`tileMineCount` updates only on `setMine` 0↔non-zero transitions) | M |
| 21 | Brainstorm Q4-mine and Q6-mine REPLACED by static-bake | user direction 2026-05-08; brainstorm's per-frame indirect-draw shape was over-engineered | M (architectural delta documented) |

**Status summary:** 21 entries; 21 M, 0 D, 0 NF.

---

## Architectural decisions that need user/advisor sign-off before executor session

1. **Sampler unit 5 unclaimed?** Stage 2c implementation prereq.
   Cheap to verify via grep at executor time; flagging here for
   continuity with PR2b's same open question (unit 4).
2. **Pre-placed mine inventory across stock content.** If NO stock
   tier1 mission has mines (pre-placed OR gameplay-laid), PR2c's
   visual canary requires a tier2 fallback OR a synthetic test
   scenario. Spec-executor verifies at Stage 0c via cost-split
   counter on tier1; if all-zero, escalate to tier2.
3. **Render.TerrainMines zone placement.** Spec recommends between
   TerrainOverlays and Decals. Spec-executor validates visually at
   Stage 2c with a chain-explosion canary. If decals visibly
   misorder, swap to non-terrain alpha drain location (zone #12 per
   recon-6).
