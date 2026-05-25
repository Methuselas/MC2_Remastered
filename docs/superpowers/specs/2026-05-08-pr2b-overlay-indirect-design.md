# PR2b — Overlay indirect endpoint (design)

**Status:** spec, ready for executor.
**Date:** 2026-05-08
**Predecessors:** PR1 SOLID + cement-atlas (default-on); PR2a detail
delete slice (independent, can ship in either order); PR2 Stage 0
recon docs ([1](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md)
through [6](../explorations/2026-05-07-pr2-stage0-recon-6-render-order-hooks.md)).
**Brainstorm:** [`2026-05-01-detail-overlay-consolidation-scope.md`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)
— Q1-Q8 decisions LOCKED, **with the following deltas applied per
recon:**
- Q4 overlay UV simplification per
  [recon-4](../explorations/2026-05-07-pr2-stage0-recon-4-overlay-uv-stability.md):
  UVs are literal constants, not per-overlay anchors. Recipe shape is
  `(presence, layer)`, not the brainstorm's per-corner UV pair.
- Render-order hook line-numbers re-grep'd per
  [recon-6](../explorations/2026-05-07-pr2-stage0-recon-6-render-order-hooks.md);
  use this spec's numbers, not the brainstorm's.

> Discipline: every cited symbol grep-verified at write-time per worktree
> CLAUDE.md "Documentation Discipline." Verification appendix at end.

---

## Goal

Replace the 4 LIVE M2d overlay emit sites at
[`mclib/quad.cpp:2056, :2063, :2072, :2079`](../../../mclib/quad.cpp)
(per-frame `gos_PushTerrainOverlay(w, overlayTexId)`) with an
indirect-draw endpoint mirroring PR1 SOLID's pattern:

- Mission-static `OverlayPresenceSSBO` (per-vertex, dense by
  `vertexNum`, built at `Terrain::primeMissionTerrainCache` time).
- Per-mission overlay texture array (built from actually-loaded
  overlay slots).
- Per-frame `OverlayThinRecordSSBO` (ring buffer, packed per emitting
  quad).
- Single `glMultiDrawArraysIndirect` per frame in a new
  `gos_terrain_overlay_bridge_drawIndirect` bridge.
- Hooks at the existing `Render.TerrainOverlays` zone in
  [`mclib/txmmgr.cpp:1777-1780`](../../../mclib/txmmgr.cpp) — replaces
  `gos_DrawTerrainOverlays()` body when armed.
- Default-on gate `MC2_TERRAIN_INDIRECT_OVERLAY` after parity + soak.

Net: retire the 4 per-frame `gos_PushTerrainOverlay` calls + the
per-quad `WorldOverlayVert wov_corner[4]` build at quad.cpp:2040-2048
+ the per-quad `tex_resolve(overlayHandle)` at quad.cpp:2037 in favor
of one indirect draw.

---

## Architecture

```
Static (built once at Terrain::primeMissionTerrainCache):
├── OverlayPresenceSSBO[mapSide² entries]
│   indexed by vertexNum = topLeftX + topLeftY * Terrain::realVerticesMapSide
│   (per mapdata.cpp:1119; identical schema to PR1 SOLID recipe)
│   per-record (8 B):
│     uint  flags         // bit0 = presence; bit1-7 reserved
│     uint  layer         // texture-array layer index (slot-keyed,
│                         //  not gosHandle — per mc2_texture_handle_is_live)
│   Memory cost: mapSide² × 8 B. At 256² = 512 KB; at 384² = 1.15 MB.
│   Bounded; allocated once, reused across missions via Reset+Build.
│
└── OverlayTextureArrayGL  GL_TEXTURE_2D_ARRAY at sampler unit 4
    Layers: per-mission overlay count (≤ 475 worst-case catalog;
            empirically ~100 per mission per recon-2)
    Dim: 64×64 RGBA8 stock / 256×256 with 4x upscale active
    Built from mcTextureManager slot indices via glGetTexImage +
    glTexSubImage3D (mirrors PR1 BuildCementCatalogAtlas pattern).

Per-frame:
├── OverlayThinRecordSSBO[N_emitting]   triple-buffered ring
│   N ≤ peak overlay-bearing quads/frame (~5K stock per recon-2)
│   per-record (24 B):
│     uint  recipeIdx     // back-ref into PR1 SOLID recipe (reuse worldPos0..3)
│     uint  layer         // copied from OverlayPresenceSSBO; per-frame stable
│                         // but cheap to re-emit (avoids extra SSBO bind in VS)
│     uint  flags         // bit0 = pzTri1Valid, bit1 = pzTri2Valid, bit2 = uvMode
│     uint  fogPacked     // (vertices[c]->fogRGB >> 24) & 0xFF, per-corner
│                         // packed 4×8 bits; matches WorldOverlayVert.fog
│     uint  argb0..3      // per-corner; matches WorldOverlayVert.argb
│
├── OverlayIndirectCmdSSBO[1]
│   one DrawArraysIndirectCommand: count = thinCount × 6 (verts/quad),
│   first = 0, instanceCount = 1, baseInstance = 0.
│   Mirrors PR1 indirect cmd buffer pattern at gos_terrain_indirect.cpp:912-926.

Draw: ONE glMultiDrawArraysIndirect inside the Render.TerrainOverlays zone
  - Replaces gos_DrawTerrainOverlays() body when armed.
  - VS = new shader gos_terrain_overlay_thin.vert (#version 430):
      reads OverlayThinRecordSSBO + worldPos0..3 from PR1 SOLID recipe
      via recipeIdx back-reference; computes corner positions in clip
      space; reads layer from thin record; samples gl_VertexID-derived
      uv from the const inset (1/128, 127/128 — recon-4 finding).
  - FS = derived from existing terrain_overlay.frag at
      shaders/terrain_overlay.frag, modified to sample sampler2DArray
      at unit 4 instead of the legacy single-texture binding. Pixel
      math otherwise identical.
```

### Key recipe simplifications per recon-4

- **No per-corner UV pair in recipe.** `oldminU/oldmaxU/oldminV/oldmaxV`
  are literal constants at [`mclib/quad.cpp:1772-1775`](../../../mclib/quad.cpp);
  recipe carries layer index only. UVs become a shader-side
  `const vec4 kOverlayUVInset = vec4(0.0078125, 0.9921875, 0.0078125,
  0.9921875);`.
- **`OVERLAY_ELEV_OFFSET = 0.15f`** at [`mclib/quad.cpp:2050`](../../../mclib/quad.cpp)
  also becomes a shader `const float`.
- **Worldpos comes from PR1's recipe via back-ref.** No duplication. The
  PR1 SOLID `worldPos0..3` (vec4 × 4) per recipe at
  [`shaders/gos_terrain_thin.vert:13-17`](../../../shaders/gos_terrain_thin.vert)
  is mission-stable and shared.

### Cull path

Per [recon-5](../explorations/2026-05-07-pr2-stage0-recon-5-setmine-chokepoint.md)
and PR1 precedent, terrain populations don't participate in the
object cull-cascade. CPU pre-cull via per-vertex `pz` is the
load-bearing gate. PR2b's per-frame thin-record build mirrors PR1
SOLID's pattern: walk live `Terrain::quadList`, dereference each
quad's top-left vertexNum, look up presence+layer in
`OverlayPresenceSSBO`, skip if presence==0 OR if pz invalid for both
tris, otherwise pack thin record. Skip set per
`memory/water_ssbo_pattern.md`.

---

## Stages

### Stage 0b — scaffolding (1 commit, mirrors PR1 `9bfcddc`)

**Goal:** env gate + N1 counters + cost-split timer. No behavior
change; arming preflight returns false unconditionally.

**Code change:**

- [`gos_terrain_indirect.h/.cpp`](../../../GameOS/gameos/gos_terrain_indirect.cpp):
  add namespace symbols mirroring PR1's solid path —
  `IsFrameOverlayArmed()`, `Counters_AddIndirectOverlayPackedQuad()`,
  `Counters_AddM2dOverlayEmitQuad()`. Counter declarations only;
  bodies stub-no-op.
- [`gameosmain.cpp`](../../../GameOS/gameos/gameosmain.cpp):
  `[INSTR v1]` banner extends to include `MC2_TERRAIN_INDIRECT_OVERLAY`,
  `MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK`. 600-frame summary line
  extends to print the new counters.
- [`scripts/run_smoke.py`](../../../scripts/run_smoke.py): env-allowlist
  for the two new flags (mirrors PR1's 4-flag entry).
- Cost-split timer `CostSplitOverlayScope` brackets the M2d block at
  [`quad.cpp:2035-2083`](../../../mclib/quad.cpp) (includes both the
  per-quad `WorldOverlayVert` build and the 4 `gos_PushTerrainOverlay`
  call sites). Note: the existing `CostSplitDetailOverlayScope` brackets
  only the Shape-C fallback per
  [recon-1](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md);
  this is the missing fast-path bracket.

**Commit message:** `feat(pr2b): scaffold overlay indirect endpoint
(env gate + counters + cost-split timer)`.

### Stage 1b — overlay presence SSBO + texture array (1 commit, mirrors PR1 `094fa56`)

**Goal:** mission-static infrastructure. Build / Reset / Invalidate
hooks. No draw yet; legacy path runs unchanged.

**Code change:**

- `OverlayPresenceSSBO`: declare in `gos_terrain_indirect.cpp`
  alongside `g_denseRecipes` at line 228. `BuildOverlayPresence()`
  walks `MapData::blocks[]` once per mission, packing
  `(presence, layer)` per `vertexNum`. `ResetOverlayPresence()` and
  `InvalidateOverlayPresenceForVertexNum(vn)` mirror PR1's
  `BuildDenseRecipe`/`ResetDenseRecipe`/`InvalidateRecipeForVertexNum`
  pattern at lines 708/755/813.
- Wire into existing chokepoints:
  - `Terrain::primeMissionTerrainCache` calls `BuildOverlayPresence()`
    after `BuildDenseRecipe()` (no new chokepoint; reuse PR1's site).
  - [`mclib/mapdata.cpp:1378-1381`](../../../mclib/mapdata.cpp)
    `setTerrain` chain extends to also call
    `InvalidateOverlayPresenceForVertexNum(vn)` next to PR1's
    `InvalidateRecipeForVertexNum(vn)`. **This is the single hook
    point per [recon-4](../explorations/2026-05-07-pr2-stage0-recon-4-overlay-uv-stability.md)**;
    `MapData::setOverlay` ends at `setTerrain`, covered.
- `OverlayTextureArrayGL`: `BuildOverlayTextureArray()` walks
  `Terrain::terrainTextures->overlays[]`, identifies actually-loaded
  overlay slots, GPU-readbacks each via `glGetTexImage`, uploads as
  `GL_TEXTURE_2D_ARRAY` at unit 4. Mirrors PR1
  `BuildCementCatalogAtlas` at
  [`gos_terrain_indirect.cpp:486-708`](../../../GameOS/gameos/gos_terrain_indirect.cpp).
  Add startup runtime guard:
  `glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &n); assert(n >= 512);`
  per [recon-2 M1 finding](../explorations/2026-05-07-pr2-stage0-recon-2-texture-array-dimensions.md).
- Slot → layer mapping: `g_overlayLayerIndexBySlot[MC_MAX_TERRAIN_TXMS]`
  populated at array-build time. Per `mc2_texture_handle_is_live.md`
  the slot is the stable key, NOT the per-frame nodeIdx (PR1 cement-atlas
  precedent).

**Tier1 verification:** smoke gate clean; no draw change yet. Counter
`indirect_overlay_packed_quads` reads zero.

**Commit message:** `feat(pr2b): build OverlayPresenceSSBO + per-mission
overlay texture array`.

### Stage 2b — per-frame thin record + parity check (1 commit, mirrors PR1 thin-record landing)

**Goal:** populate `OverlayThinRecordSSBO` each frame. Add parity
check that byte-compares the indirect path's would-be draws against
the legacy `gos_PushTerrainOverlay` arg stream.

**Code change:**

- `PackOverlayThinRecordsForFrame()` in `gos_terrain_indirect.cpp`,
  mirroring `PackThinRecordsForFrame()` at line 793. Walks
  `Terrain::quadList` (camera-windowed per
  `quadlist_is_camera_windowed.md`); per quad, look up
  `OverlayPresenceSSBO[topLeftVertexNum]`; skip if presence==0;
  apply per-tri pz check identical to M2 fast path
  ([`quad.cpp:1870-1888`](../../../mclib/quad.cpp)); pack thin record
  with layer + flags + fogPacked + argb0..3.
- `MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK=1`: per-quad byte-compare
  the synthesized `WorldOverlayVert` triangle (worldPos from PR1
  recipe + UV from constants + fog/argb from current frame) against
  the legacy M2d block's `gos_PushTerrainOverlay` arg stream. Mirror
  PR1's `[TERRAIN_INDIRECT_PARITY v1]` printer convention; this slice
  uses `[TERRAIN_INDIRECT_OVERLAY_PARITY v1]`. Throttle 16 mismatches/
  frame, 600-frame summary line.
- Counter `indirect_overlay_packed_quads` extends 600-frame summary.

**Tier1 verification:** smoke gate with
`MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK=1` must report 0 mismatches
across all tier1 missions. Mirrors the 231M / 0 mismatches PR1
achieved.

**Commit message:** `feat(pr2b): pack OverlayThinRecord per-frame +
byte-compare parity gate`.

### Stage 3b — indirect draw + legacy gate-off (1 commit; partial-landing-hazard rule per N2)

**Goal:** ship the indirect draw and the legacy gate-off in the same
commit. Either both ship or neither.

**Code change:**

- `gos_terrain_overlay_bridge_drawIndirect` in
  [`gameos_graphics.cpp`](../../../GameOS/gameos/gameos_graphics.cpp),
  mirroring `gos_terrain_bridge_drawIndirect` at line 2275. State save/
  restore (VAO, sampler unit 4, depth, blend, color-mask), bind
  `OverlayThinRecordSSBO` + `OverlayIndirectCmdSSBO`, set sampler unit 4
  to LINEAR/CLAMP_TO_EDGE (or REPEAT — confirm at impl time per
  recon-2 sampler-discipline open question), `glEnableVertexAttribArray(0)`
  per AMD attrib-0 trap.
- `DrawOverlayIndirect()` in `gos_terrain_indirect.cpp` calls the
  bridge; matches `DrawIndirect()` shape from PR1.
- Hook in [`mclib/txmmgr.cpp:1777-1780`](../../../mclib/txmmgr.cpp): wrap
  `gos_DrawTerrainOverlays()` with arming check — when
  `IsFrameOverlayArmed()` is true, call `DrawOverlayIndirect()`
  instead.
- Legacy gate-off at [`quad.cpp:2035-2083`](../../../mclib/quad.cpp):
  add `BeginLegacyOverlayCluster()` / `EndLegacyOverlayCluster()`
  guards mirroring PR1 SOLID's `BeginLegacySolidCluster()` at
  [`quad.cpp:478-512`](../../../mclib/quad.cpp). When
  `IsFrameOverlayArmed()` is true, the M2d block becomes a no-op.
- New shader `shaders/gos_terrain_overlay_thin.vert` (`#version 430`).
  Pair with derived `shaders/gos_terrain_overlay_thin.frag` (or
  reuse `terrain_overlay.frag` with sampler2DArray patch).

**N2 rule (partial-landing hazard):** Stage 3b's draw + gate-off must
land together. Splitting them ships either:
- Draw without gate-off → both indirect AND legacy emit fire; perf
  gate fails (move work, don't eliminate it). PR1 plan v2 surfaced
  this hazard for SOLID; same applies here.
- Gate-off without draw → overlay disappears. Visible regression.

**Tier1 verification:** smoke gate clean with
`MC2_TERRAIN_INDIRECT_OVERLAY=1`. Visual canary: cement-transition
tiles, road borders, building footings — known overlay-bearing
content. 5/5 PASS required.

**Commit message:** `feat(pr2b): indirect overlay draw + legacy M2d
gate-off (default-off)`.

---

## Parity / verification gates

### Gate A (visual canary)

Tier1 + menu canary byte-equivalent or pixel-diff <0.5% pre vs post
default-on. Visual focus: cement transitions, dirt roads, paved
roads, bridge approaches. mc2_01 + mc2_03 are overlay-rich; mc2_24
exercises off-overlay-rich content.

### Gate B (parity SSBO byte-compare)

`MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK=1` reports 0 mismatches
across tier1. Mirror PR1's bar (231M+ checks / 0 mismatches).

### Gate C (perf)

Tracy `Render.TerrainOverlays` zone GPU delta + `quadSetupTextures`
CPU delta on smoke camera, post default-on flip. **Targets TBD per**
[recon-1 multi-zoom note](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md):
M2d emit volume scales with overlay-bearing quad count; baseline at
both normal zoom and full zoom out before locking gate values.

### Gate D (counter cardinality)

`indirect_overlay_packed_quads` per frame must equal the legacy
`m2d_overlay_emit_quads` count (Stage 0b counter) within ±1 (drop
attribution to per-frame skip-set timing). Catches admission-skip
divergence between legacy and indirect paths that byte-compare
misses (a quad that's skipped by indirect can't be byte-compared —
no record exists). PR1's brainstorm Q7 surfaced this failure mode.

### Soak

7-day default-off + parity-check armed. Default-on flip after
soak passes.

---

## Risks / hazards

### R1. Overlay-shader dual-source: legacy single-texture vs new sampler2DArray

`gos_DrawTerrainOverlays()` and the new bridge use different
samplers. If both run in the same frame (Stage 3b's gate-off fails
to fire on some quad), the new bridge's sampler unit-4 `GL_TEXTURE_2D_ARRAY`
binding can leak into the legacy draw path's expected
`GL_TEXTURE_2D` slot. Per PR1 cement-atlas pattern, save/restore at
unit 4 inside the bridge.

**Mitigation:** bridge's state save/restore covers unit 4.
Adversarial review at Stage 3b lands time.

### R2. Cement vs overlay double-binding

PR1's cement multi-sampler also indexes `terrainTextures` slots. Some
cement classes may overlap with what the overlay catalog
classifies as cement (per recon-2 R3). If the overlay bucket
includes cement classes, cement renders TWICE.

**Mitigation:** at array-build time, exclude any overlay slot that
is also classified as cement (using the same `numTextures > 9`
heuristic at [`terrtxm.cpp:745`](../../../mclib/terrtxm.cpp) PR1 uses).
Stage 1b implementer must add this filter; smoke gate visual canary
catches double-binding.

### R3. Overlay UV-inset constant divergence vs legacy

Recon-4 confirmed `oldminU/oldmaxU/oldminV/oldmaxV = 0.0078125 /
0.9921875` are constants. Spec hardcodes them in shader. If anyone
edits the literal in `quad.cpp:1772-1775` post-spec, shader and
legacy diverge silently.

**Mitigation:** shader comment cites
`mclib/quad.cpp:1772-1775` as source of truth. If the constants ever
need to change, both sites must be updated. Defense-in-depth: add a
build-time `static_assert(0.0078125f == 1.0f / 128.0f)` near the
shader-side constant declaration.

### R4. Per-frame fog/argb cost transferred to thin-record packing

Legacy M2d reads `vertices[c]->fogRGB`, `vertices[c]->lightRGB` per
emitted tri. Indirect path reads the same per emitting quad at
`PackOverlayThinRecordsForFrame` time. CPU cost moves; doesn't
eliminate. Net win comes from retiring `gos_PushTerrainOverlay`'s
per-tri batch-buffer push + the per-frame `WorldOverlayVert`
allocations.

**Mitigation:** Stage 0b cost-split timer establishes the baseline.
Stage 3b post-default-on perf gate confirms net negative. If fog/argb
packing dominates, a future slice can move per-corner color to
recipe (mission-static if lighting is mission-static, which it
isn't — defer).

### R5. AMD attrib-0 / VAO / color-mask traps multiply

New bridge function = new state-save/restore boilerplate. PR1 SOLID's
bridge at [`gameos_graphics.cpp:2217-2330`](../../../GameOS/gameos/gameos_graphics.cpp)
is the pattern. Spec calls out the 4 specific items to mirror:
`gos_RendererRebindVAO`, `glEnableVertexAttribArray(0)`, color-mask
true, depth state explicit.

**Mitigation:** Stage 3b implementer reads PR1 bridge as template.
Adversarial review at landing time.

### R6. Per-mission teardown for parallel SSBOs

`OverlayPresenceSSBO` and `OverlayTextureArrayGL` need explicit
mission-boundary lifecycle. PR1 wired this via
`Terrain::primeMissionTerrainCache` (build) and `Terrain::destroy`
(reset). PR2b reuses both chokepoints. WaterStream pattern: keep GL
buffer allocated, CPU-clear state per mission. Avoid reallocating GL
texture-array per mission if catalog dimensions are stable.

**Mitigation:** Stage 1b implementer mirrors PR1's site. Verify with
mission load → mission unload → mission load cycle on tier1.

---

## Rollback

Per stage:

- Stage 0b: harmless if reverted. Counters become orphan; cost-split
  timer becomes orphan. Both safe.
- Stage 1b: harmless if reverted. Builds infrastructure, no draw
  change. SSBO + texture-array become orphan allocations.
- Stage 2b: harmless if reverted. Pack function never called from
  draw path; parity check is opt-in via env gate.
- Stage 3b: revertible by `git revert`. Default flag is off; soak
  window allows full operator-level rollback via env vars before
  default-on flip.

Default-on flip is a separate commit per PR1 precedent.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | M2d block to retire (4 LIVE sites) | [`mclib/quad.cpp:2035-2083`](../../../mclib/quad.cpp); calls at `:2056, :2063, :2072, :2079` | M |
| 2 | `gos_PushTerrainOverlay` definition | [`gameos_graphics.cpp:6104`](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 3 | `gos_DrawTerrainOverlays` (drain) | [`gameos_graphics.cpp:6112`](../../../GameOS/gameos/gameos_graphics.cpp), called from [`txmmgr.cpp:1779`](../../../mclib/txmmgr.cpp) inside `Render.TerrainOverlays` | M |
| 4 | `Render.TerrainOverlays` zone first line | [`mclib/txmmgr.cpp:1777`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.TerrainOverlays")` | M |
| 5 | Overlay UV constants (recipe simplification source) | [`mclib/quad.cpp:1772-1775`](../../../mclib/quad.cpp) — 4 literal floats; recon-4 confirmed never mutated | M |
| 6 | `OVERLAY_ELEV_OFFSET = 0.15f` shader constant source | [`mclib/quad.cpp:2050`](../../../mclib/quad.cpp) | M |
| 7 | `WorldOverlayVert` struct (28 B) | [`GameOS/include/gameos.hpp:2330,2335`](../../../GameOS/include/gameos.hpp) | M |
| 8 | PR1 `BuildDenseRecipe` / `ResetDenseRecipe` / `InvalidateRecipeForVertexNum` (mirror points) | [`gos_terrain_indirect.cpp:708, :755, :813`](../../../GameOS/gameos/gos_terrain_indirect.cpp) | M |
| 9 | PR1 SOLID bridge (mirror template) | [`gameos_graphics.cpp:2275`](../../../GameOS/gameos/gameos_graphics.cpp) — `gos_terrain_bridge_drawIndirect` | M |
| 10 | `setTerrain` invalidation chain (extends to overlay) | [`mclib/mapdata.cpp:1378-1381`](../../../mclib/mapdata.cpp) — `gos_terrain_indirect::InvalidateRecipeForVertexNum(vn)` per affected vertex | M |
| 11 | `MapData::setOverlay` ends at `setTerrain` | [`mclib/mapdata.cpp:1282`](../../../mclib/mapdata.cpp) | M |
| 12 | `vertexNum` schema | `topLeftX + (topLeftY * Terrain::realVerticesMapSide)` per [`mapdata.cpp:1119`](../../../mclib/mapdata.cpp) | M |
| 13 | PR1 cement-atlas slot-keyed lookup (precedent) | `g_cementLayerIndexBySlot` per [`memory/indirect_terrain_solid_endpoint.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/indirect_terrain_solid_endpoint.md) | M |
| 14 | overlay catalog: 475 frames, 64×64 RGBA8 stock | recon-2 disk inventory at write-time; per-mission ≪ that | M |
| 15 | `Terrain::quadList` is camera-windowed | `memory/quadlist_is_camera_windowed.md` | M |
| 16 | PR1 SOLID gate-off pattern | [`quad.cpp:478-512`](../../../mclib/quad.cpp) — `BeginLegacySolidCluster()` / `EndLegacySolidCluster()` | M |
| 17 | AMD attrib-0 trap mitigation | [`docs/amd-driver-rules.md:5`](../../amd-driver-rules.md); [`gameos_graphics.cpp:2249`](../../../GameOS/gameos/gameos_graphics.cpp) PR1 bridge | M |
| 18 | `cull_gates_are_load_bearing.md` not applicable to terrain | recon-5 + brainstorm Q5 cite (terrain doesn't participate in object-cull cascade) | M |
| 19 | `GL_MAX_ARRAY_TEXTURE_LAYERS` runtime guard not currently in codebase | recon-2 M1 finding (zero `glGetIntegerv` callers); spec adds the guard at Stage 1b | M (negative; runtime-verified) |
| 20 | overlay shader pair filename | [`shaders/terrain_overlay.vert`](../../../shaders/terrain_overlay.vert) (NOT `gos_terrain_overlay.vert` — brainstorm cite was imprecise) | M (drift correction) |
| 21 | `terrain_overlay.vert` typed against `WorldOverlayVert` | [`shaders/terrain_overlay.vert:3`](../../../shaders/terrain_overlay.vert) | M |
| 22 | `Counters_AddLegacyDetailOverlayQuad()` declaration (idiom to mirror) | [`gos_terrain_indirect.h:106`](../../../GameOS/gameos/gos_terrain_indirect.h) | M |
| 23 | `IsFrameSolidArmed`/`DrawIndirect` namespace API (mirror points) | [`gos_terrain_indirect.h:162-163`](../../../GameOS/gameos/gos_terrain_indirect.h) | M |

**Status summary:** 23 entries; 23 M, 0 D, 0 NF.

---

## Architectural decisions that need user/advisor sign-off before executor session

1. **Sampler unit assignment.** PR1 cement-atlas binds at unit 3.
   PR2b proposes unit 4. If a future PR2c needs unit 5, that's fine;
   if instead PR2c reuses an existing unit, this assignment matters.
   Confirm 4 is unclaimed — verify at Stage 3b implementation time
   via grep on `glActiveTexture(GL_TEXTURE4)` / `glBindSampler(4`.
2. **Overlay vs cement double-binding** (R2). The exclusion
   filter is a small but real classification choice. If
   `terrainTextures->overlays[i].numTextures > 9` is an imperfect
   proxy for "this is a cement class, exclude from overlay
   bucket," visual regression on cement-overlay overlap could
   surface late. Prefer to surface this in adversarial review at
   Stage 1b landing.
3. **Sampler filter for overlay-array** (LINEAR/CLAMP_TO_EDGE vs
   LINEAR/REPEAT). Recon-2 flagged this open. Stock overlays at
   64×64 with the 1/128 inset are conservative against edge-bleed
   either way; defer to implementation but document choice.
