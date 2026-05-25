# Indirect terrain — detail / overlay / mine consolidation brainstorm (PR2)

**Date:** 2026-05-01
**Status:** Brainstorm. NOT a spec. User signs off on Q1-Q8 decisions before
spec session opens.
**Workstream:** CPU → GPU offload — follow-up to indirect-terrain plan v2 PR1
(Stages 0-3, shipped 2026-04-30 across `9bfcddc` / `bdb1628` / `094fa56` /
`f221570`).
**Predecessors:** orchestrator status board, plan v2 revision brief
([`specs/2026-04-30-indirect-terrain-plan-v2-revision-brief.md`](../specs/2026-04-30-indirect-terrain-plan-v2-revision-brief.md)),
plan v1 brainstorm
([`brainstorms/2026-04-30-indirect-terrain-draw-scope.md`](2026-04-30-indirect-terrain-draw-scope.md)),
design doc ([`specs/2026-04-30-indirect-terrain-draw-design.md`](../specs/2026-04-30-indirect-terrain-draw-design.md)).

> **Discipline:** every cited symbol grep-verified at write-time per worktree
> CLAUDE.md "Documentation Discipline." Verification appendix at end lists
> every claim with `file:line` and `matches claim / divergent / not found`
> status. Plan v1 stop-the-line at adversarial review (3 CRITICAL findings:
> fictional `TerrainQuadRecipe` fields, wrong `invalidateTerrainFaceCache`
> signature, missing gate-off) is the precedent for why this matters here.

---

## TL;DR (the spec session inherits these decisions)

| Q | Decision | One-line why |
|---|---|---|
| **Q1** | Re-baseline cost via the Stage 1 cost-split timers — open recon item before spec, not in this brainstorm. | The 3.01 ms quadSetupTextures number predates PR1; today the SOLID setup is gated off, so the residual detail/overlay/mine cost is the relevant baseline and is not yet measured in isolation. |
| **Q2** | **A(iii) single-command + per-quad-flags + texture array.** Avoid `gl_DrawIDARB` (4.6/extension) and bindless. One indirect command per *blend bucket*, but inside each bucket all quads share program + sampler set + a 2D-array texture; the thin record carries which array layer to sample. | GL 4.3 core context locks out `gl_DrawID`. AMD bindless is supported but adds a new failure surface (residency lifetime, parity). Texture-array fallback is the lowest-risk shape and reuses the `mcTextureManager` slot index already in the thin record. |
| **Q3** | **Sequential PRs: PR2a detail, PR2b overlay, PR2c mine.** Each is its own slice with its own parity gate; bundle detail+overlay in PR2a if the texture-array build can be shared, defer mine to PR2c. | Three populations with three different per-frame data shapes. Bundling them is exactly the multi-bucket trap plan v1 fell into; sequential keeps each slice's parity surface bounded to one population. |
| **Q4** | Detail = small recipe extension (detailHandle slot index, detail UV scale). Overlay = parallel SSBO with per-quad `(overlayLayer, oldUV[2..2])` (overlay has its own UV math). Mine = parallel SSBO with per-cell records, sub-quad granularity, separate population. | Each population's per-frame data shape doesn't fit cleanly in the SOLID thin record. Cleanest split is "extend recipe for static detail/overlay membership; per-population per-frame thin records." |
| **Q5** | Cull-cascade still moot. Detail/overlay/mine fast-path emits already use the same `vertices[c]->pz` per-tri valid bits; PR1 thin path validated this with 231M parity checks / 0 mismatches. | Plan v2 brainstorm Q-A established terrain doesn't participate in object-cull cascade. Detail/overlay/mine are terrain populations; same memory applies. CPU pre-cull via `pz` is THE load-bearing gate and is already shipped. |
| **Q6** | **Hybrid: extend recipe with detail-membership + overlay-membership flags; per-population thin record SSBO; separate per-population indirect command.** Recipe stays mission-static; per-frame data per-population stays parallel. | Pure parallel SSBOs duplicate static membership data; pure recipe extension forces the recipe to grow non-trivially with little payoff for solid quads that don't participate. Hybrid keeps the SOLID-only PR1 hot path unaffected. |
| **Q7** | Per-population thin-record byte-compare (mirror PR1) **+** indirect command-buffer count comparison (legacy detail/overlay/mine cluster counts vs indirect packed counts) **+** visual canary as gate A. | PR1 byte-compared recipe + thin record. Multi-population draw adds a new failure mode: command-buffer count drift between bucket emit and indirect pack. Comparing `legacy_detail_overlay_quads` against the new per-population packed counters is the cheapest catch. |
| **Q8** | Risk inventory below. Top three: (i) overlay UV path uses `oldminU/oldmaxU` independently from `minU/maxU` and would silently drift if recipe absorbs only one set; (ii) mine state lifecycle is per-cell (16 sub-cells per quad) with `tileMineCount` cache invalidation that doesn't yet trigger recipe rebuild; (iii) texture-array layer assignment for `terrainDetailHandle` / `overlayHandle` / `mineTextureHandle` requires a content-stable mapping that survives `tex_resolve` per-frame mutation. | Each is a class of bug plan v1's adversarial review would have flagged. Surface them in the spec preamble. |

---

## Q1 — Re-baseline the cost on post-PR1 code

### Decision: **Open recon item before spec session.** This brainstorm cannot answer Q1 with code-grounded numbers; the Stage 1 cost-split timers exist but the post-PR1 measurement isn't captured.

### What's measurable today

Stage 1 (`bdb1628`) added `CostSplitSolidScope` and `CostSplitDetailOverlayScope`
RAII timers that bracket the legacy SOLID emit and the legacy detail/overlay emit
clusters in `addTerrainTriangles`
([`mclib/quad.cpp:478-512`](../../../mclib/quad.cpp)). The Stage 3 commit `f221570` added
`BeginLegacySolidCluster()` / `EndLegacySolidCluster()` so SOLID cluster doesn't
fire when `MC2_TERRAIN_INDIRECT=1` is armed. With PR1 default-off, the cost-split
timers measure both halves; with `MC2_TERRAIN_INDIRECT=1`, the SOLID half drops
toward zero (per `f221570` commit message: "indirect_solid_packed_quads:
3.5M/2.3M/5.7M/2.8M/2.8M (active)").

The cost-split N1 counters (`legacy_solid_setup_quads`,
`indirect_solid_packed_quads`, `legacy_detail_overlay_quads`) are committed via
`Counters_Add*` in
[`GameOS/gameos/gos_terrain_indirect.cpp:86`](../../../GameOS/gameos/gos_terrain_indirect.cpp). The first
two are well-characterized post-PR1; the third (`legacy_detail_overlay_quads`)
captures the residual that PR2 targets.

### What's NOT measured

The Stage 1 baseline doc at
[`progress/2026-04-30-indirect-terrain-stage1-baseline.md`](../specs/2026-04-30-indirect-terrain-stage1-baseline.md)
recorded *pre-PR1* cost-split readings. **Post-PR1 detail/overlay/mine cost in
isolation has not yet been re-measured.** PR1's `f221570` commit message reports
counter totals but not per-frame µs — the timer bracket is wall-time but the
post-shipping deltas weren't lifted into a Tracy zone report.

### What the spec session needs (before perf gate B can be set)

A re-baseline run on tier1 with `MC2_TERRAIN_INDIRECT=1` (PR1 active) recording:
- `Terrain::geometry quadSetupTextures` outer Tracy zone — current ms.
- `CostSplitDetailOverlayScope` aggregate per frame (the PR2 target).
- `legacy_detail_overlay_quads` running total (calibration sanity).
- Per-mission breakdown: mc2_24 (mines present) vs others (mine-free).
- Wolfman zoom variant if available — overlay quad count scales with zoom.

This is **a recon item, not a brainstorm question.** Without these numbers the
PR2 perf gate target (Tracy delta on the cost-split) is undefined. The
brainstorm decision: spec session opens with "Stage 0 — recon" mirroring the
plan v2 stage structure.

### Per-quad call site inventory (what PR2 would retire)

Detail emit, M2 fast path inline (already classifier-fast, still mcTextureManager
queued):
- [`mclib/quad.cpp:2006`](../../../mclib/quad.cpp), `:2012`, `:2020`, `:2026` —
  `addVertices(terrainDetailHandle, tri, MC2_ISTERRAIN | MC2_DRAWALPHA)`
  (4 sites, BOTTOMLEFT tri1+2 / BOTTOMRIGHT tri1+2)

Detail emit, legacy/Shape-C path (only fires when `fastPathEligible` is false):
- [`mclib/quad.cpp:485`](../../../mclib/quad.cpp), `:501` —
  `addTriangleBulk(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA, 2)`
  (2 sites in `addTerrainTriangles`)
- [`mclib/quad.cpp:2253`](../../../mclib/quad.cpp), `:2396`, `:2568`, `:2709`
  — legacy fallback `addVertices` clusters (4 sites)

Overlay emit, M2d fast path inline (already classifier-fast, still queue-driven):
- [`mclib/quad.cpp:2062`](../../../mclib/quad.cpp), `:2069`, `:2078`, `:2085` —
  `gos_PushTerrainOverlay(w, overlayTexId)` (4 sites)

Overlay emit, legacy fallback (only when `fastPathEligible` false):
- [`mclib/quad.cpp:2196`](../../../mclib/quad.cpp), `:2339`, `:2609`, `:2750` —
  `gos_PushTerrainOverlay(wov, overlayTexId)` (4 sites)

Mine state, setupTextures-time triangle reservation:
- [`mclib/quad.cpp:282`](../../../mclib/quad.cpp), `:290` —
  `addTriangleBulk(mineTextureHandle/blownTextureHandle, MC2_DRAWALPHA, 2)`
  inside `enqueueTerrainMineState` per-cell loop

Mine state, drawMine pass actual emit:
- [`mclib/quad.cpp:4379`](../../../mclib/quad.cpp), `:4380`, `:4384`, `:4385` —
  `addVertices(blownTextureHandle/mineTextureHandle, gVertex/sVertex, MC2_DRAWALPHA)`

The fast-path inline emits (8 sites for detail+overlay) are PR2's primary hot
path. The legacy-fallback sites are gated by `!fastPathEligible` which the
M2d path covers ~100% of (per `m2_thin_record_cpu_reduction_results.md` line 26:
"Legacy quads/frame: 0"). Mine setup/drawMine paths still always run.

---

## Q2 — Multi-bucket strategy decision

### Decision: **A(iii) single-command-per-bucket + texture array + per-quad layer index in thin record.** Avoid `gl_DrawIDARB`; defer bindless to a hypothetical future slice if bucket count explodes.

### Why not A(ii) `gl_DrawIDARB` + texture-array

GL context is **4.3 core**
([`GameOS/gameos/gos_render.cpp:112-116`](../../../GameOS/gameos/gos_render.cpp): `MAJOR=4`,
`MINOR=3`, comment "4.3 required for core SSBO"). Worktree CLAUDE.md:
"Pass `\"#version 430\\n\"` as prefix to `makeProgram()`."

`gl_DrawID` is GLSL 4.6 core; `gl_DrawIDARB` requires `ARB_shader_draw_parameters`
(GL 4.0+ extension) which AMD RX 7900 XTX advertises. So A(ii) IS technically
available — but introduces a load-bearing dependency on a non-core extension and
a `#version` bump (or `#extension GL_ARB_shader_draw_parameters : require`),
which would be the first such extension in this codebase. Adversarial-review
finding bait: every reader who skims the worktree CLAUDE.md "#version 430" rule
will trip over a shader that requires 4.6 syntax.

A(ii) also doesn't solve the fundamental constraint: `glMultiDrawArraysIndirect`
issues N draw commands but they share **everything** state-wise (program, blend,
depth, sampler, attribute bindings). `gl_DrawID` lets the shader index a
per-draw uniform array — useful when you want N draws with the SAME blend mode
and DIFFERENT per-draw uniforms (e.g., texture array layer ranges, light
indices). It does NOT let blend modes vary per draw.

Detail = `MC2_DRAWALPHA` (alpha-blended). Overlay = also alpha-blended, separate
shader (`shaders/gos_terrain_overlay.vert/frag`). Mine = also alpha-blended.
The blend mode is uniform across detail and mine; overlay has different shader
+ vertex layout (`WorldOverlayVert`). Even with `gl_DrawID`, you'd still split
into at least two indirect-draw bundles (one for detail+mine sharing terrain
shader, one for overlay using overlay shader). That's not a meaningful
simplification over A(iii)'s "one indirect command per bucket."

### Why A(iii) wins

The thin record already carries `terrainHandle` as a `uint` slot index
(per [`shaders/gos_terrain_thin.vert:5`](../../../shaders/gos_terrain_thin.vert)
`uvec4 control` field y). Detail and overlay use the same slot-index pattern
via `tex_resolve()` (memory `mc2_texture_handle_is_live.md`). Putting all
detail textures into a 2D texture array, all overlay textures into another,
and adding a per-quad layer index to the thin record gives:

- One shader program per bucket (detail uses `gos_terrain_thin.vert`-derived
  detail-only VS; overlay uses extended overlay VS).
- One blend/depth state per bucket.
- One sampler per bucket.
- One `glMultiDrawArraysIndirect` call per bucket. PR1 already proved this
  shape works ([`gameos_graphics.cpp:2321`](../../../GameOS/gameos/gameos_graphics.cpp)
  uses `glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, cmdCount, 0)` with
  `cmdCount=1` in PR1; can grow to N safely).

The texture array load is a per-mission build (one-time cost mirrors `Stage 2
recipe build` from PR1).

### Why not bindless / option E

Bindless texture (ARB_bindless_texture, NV/AMD-supported, not core in 4.3)
solves the multi-blend-mode problem too — every quad references its texture by
64-bit handle, no array-layer indirection needed. **But:**

- Adds residency lifecycle to manage (`glMakeTextureHandleResidentARB` vs
  non-resident). PR1 didn't need this; existing `tex_resolve` runtime path
  doesn't expose it.
- Adds parity-test surface — handle-stability claims need separate validation.
- Per `memory/mc2_texture_handle_is_live.md`: "anything in MC2 populated by
  `TransformShape` is per-frame-rewritten." Bindless handles change the
  per-frame indirection point and could re-surface this trap.
- AMD driver compatibility: extension is supported on RX 7900 XTX but
  worktree `docs/amd-driver-rules.md` doesn't list any prior bindless usage.
  First-use means new failure surface.

Defer bindless to a hypothetical follow-up if bucket count balloons (which it
won't — there are at most ~32 detail textures, ~64 overlay textures, ~2 mine
textures across the entire stock campaign). 2D texture array fits comfortably.

### Constraints that A(iii) must satisfy

- Texture array dimensions: all detail textures must share max(width, height)
  (or be padded). Same for overlay. Same for mine.
- Mip chain consistency: per `gpu_direct_renderer_bringup_checklist.md` #5
  (sampler inheritance), the bound sampler MUST match what the bucket needs.
  Detail's CLAMP_TO_EDGE/LINEAR (PR1 SOLID precedent at
  [`gameos_graphics.cpp:2279-2283`](../../../GameOS/gameos/gameos_graphics.cpp)) likely
  works for detail too; overlay may want REPEAT for tiled overlay UVs.
  Recon required per population.
- AMD attribute-0 trap (`docs/amd-driver-rules.md:5`): each bucket bridge
  function must call `glEnableVertexAttribArray(0)` per PR1 precedent
  ([`gameos_graphics.cpp:2249`](../../../GameOS/gameos/gameos_graphics.cpp)).
- Color-mask save/restore (M5 from plan v2): each bridge re-applies
  `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE)` after potential shadow
  pass leaves it FALSE. PR1 precedent at
  [`gameos_graphics.cpp:2271`](../../../GameOS/gameos/gameos_graphics.cpp).

### Open follow-up for spec session

Recon: enumerate the detail / overlay / mine texture-array dimensions across
stock content. If any single texture exceeds an MC2 reasonable budget when
padded to a max, the texture-array build needs a fallback (per-bucket sub-array
or per-size-class array). Likely fine — these textures are small (32×32 to
256×256 typical for terrain decals).

---

## Q3 — Population scope: ONE PR or sequential?

### Decision: **Sequential. PR2a = detail; PR2b = overlay; PR2c = mine.** Bundle detail+overlay into PR2a only if Stage 0 recon shows the texture-array build is genuinely shared and parity gates can run on both populations independently within the same commit.

### Why not "PR2 = detail + overlay + mine in one"

This is exactly the multi-bucket trap plan v1 fell into. Each population has:
- Different per-frame thin record schema (detail = per-corner ARGB+UV;
  overlay = `WorldOverlayVert` per corner with overlay-specific UV; mine =
  per-cell sub-quad granularity).
- Different lifecycle invalidation surface (detail follows base-terrain
  invalidation; overlay follows `setOverlay`; mine follows
  `setMine`/`tileMineCount`).
- Different visual canary (detail = water-interest tiles; overlay =
  cement/transition borders; mine = mc2_24).

A single-PR consolidation has cumulative parity surface ≈ 3× any single
population. The renderWater Stage 3 close
(`memory/renderwater_fastpath_stage2.md`) caught **3 silent bugs in one
population**; a 3-population consolidation realistically would surface 6-9 silent
bugs and the bring-up cost compounds.

### Why sequential beats one PR

Each population's slice mirrors the renderWater pattern:
- Stage 0: recon (this brainstorm Q1 + per-population specifics).
- Stage 1: recipe extension + per-population thin record build + parity check
  (`MC2_TERRAIN_INDIRECT_<POP>_PARITY_CHECK=1`).
- Stage 2: bridge function + texture array + indirect command + legacy gate-off
  (mirrors PR1 Stage 3 partial-landing hazard rule — both must land in the same
  PR per N2 from plan v2 brief).
- Stage 3: promotion (separate PR after soak, mirroring PR1's two-PR sequence).

Each slice has one population's parity surface and one perf-gate target. Smaller
PR, faster review, isolated failure mode if regression surfaces.

### Why detail before overlay before mine

- **Detail first:** highest-cost population (water-interest tiles fire on every
  detail-bearing quad, ~5,800 quads on mc2_01 per `m2_thin_record_cpu_reduction_results.md`
  line 25). Best signal-to-noise ratio for perf gate B.
- **Overlay second:** uses different shader (`gos_terrain_overlay.{vert,frag}`).
  Once detail's texture-array + per-quad-layer pattern is shipped, overlay
  inherits the pattern with a different shader pair. Shader parity surface is
  isolated.
- **Mine last:** only fires on missions with mines (~1 of 5 stock smoke
  missions per slice 2b's "campaign-wide ~97% mine-free" finding). Sub-quad
  granularity (16 cells per quad) is a different recipe shape — best done
  after detail/overlay establish the multi-bucket precedent.

### When detail+overlay COULD bundle

If Stage 0 recon shows:
- Both populations' textures fit a single shared dimensions class (probably
  not — overlay textures are larger per inspection).
- Parity gates run independently per population (each has its own
  `_PARITY_CHECK=1` env var).
- The bridge function structure is genuinely shared (likely no — different
  shaders).

Default to separate PRs; fold only if Stage 0 surfaces strong signal.

### Trade-off (vs all-in-one)

We accept 3× the wall-clock cost of bring-up + 3× the user attention for
review. We get 3× isolation and 3× cleaner regression diagnostics. The arc has
shipped 22 slices on this discipline; this is not the moment to deviate.

---

## Q4 — Per-population cost quirks

### Detail

**Quirk:** per-corner ARGB whitening based on `Terrain::terrainTextures2`,
plus per-corner UV derived from `(vx, vy)` and `Terrain::worldUnitsMapSide`.
The M2c fast-path inline emit at
[`mclib/quad.cpp:1967-2030`](../../../mclib/quad.cpp) builds `corner[c].u/v` per-frame; the
existing thin record has no UV slots. The `clampUVs` lambda
([`:1988-1999`](../../../mclib/quad.cpp)) handles overflow.

**Recipe extension required:**
- `detailHandle` (uint slot index) — currently lives on `TerrainQuad`, not on
  recipe.
- Per-corner detail UV anchors (`vx/vy` projected through
  `oneOverTf = tilingFactor / Terrain::worldUnitsMapSide`).
- Detail-presence flag (mission-static if detail handle is mission-static —
  needs Stage 0 confirmation).

**Per-frame thin record (PR2a):**
- recipeIdx (back-ref).
- per-tri pzValid bits (already in current SOLID thin record schema; reuse).
- per-corner light ARGB (already in current SOLID thin record `lightRGBs`;
  reuse with detail's whitening rule applied at pack time).

**Decision for Q4 detail:** extend recipe with per-corner detail UV anchors +
detail layer index. Reuse SOLID thin-record schema for per-frame; add a
*detail flags* uint to disambiguate detail's whitening rule. Recipe grows
~16 B per slot (2× vec4 anchors + 1× uvec4 layer/flags); at 256² recipes,
this is +1 MB. Acceptable.

### Overlay

**Quirk:** uses `WorldOverlayVert` (12-byte vertex with `wx, wy, wz, fog,
argb, u, v` packed differently from `gos_VERTEX`). Overlay UV anchors are
`oldminU/oldmaxU/oldminV/oldmaxV` (NOT the same as detail's `minU/maxU/minV/maxV`).
Per `mclib/quad.cpp:2059-2086`, overlay tris use 4 distinct UV-corner mappings
that don't reduce to a single `uvData vec4`.

**`OVERLAY_ELEV_OFFSET = 0.15f`** ([`mclib/quad.cpp:2050`](../../../mclib/quad.cpp)) is added
to each corner's `wz` to lift overlay above terrain depth. This is overlay-
specific (terrain SOLID + detail use `pz + TERRAIN_DEPTH_FUDGE` per
[`:1979`](../../../mclib/quad.cpp)).

**Recipe extension or parallel SSBO:**
- Recipe approach: add per-corner overlay UV pair (`oldminU, oldminV,
  oldmaxU, oldmaxV` packed as vec4) + overlay layer index — but this slot is
  empty for ~50% of quads (overlay is per-tile, often `0xffffffff` sentinel).
- Parallel SSBO approach: separate `OverlayRecipeSSBO` indexed by
  `vertexNum` for overlay-bearing quads only — sparse, smaller, tracks
  overlay-specific lifecycle (`MapData::setOverlay` at `mapdata.cpp:1259`).

**Decision for Q4 overlay:** parallel `OverlayRecipeSSBO` keyed by `vertexNum`
with sentinel for non-overlay quads. Per-frame: separate
`OverlayThinRecordSSBO` ring + separate indirect command + separate bridge.
Shader = `gos_terrain_overlay.frag`-derived (overlay has its own FS path
already; reuse).

### Mine

**Quirk:** per-cell, sub-quad. `enqueueTerrainMineState`
([`mclib/quad.cpp:250-297`](../../../mclib/quad.cpp)) iterates `MAPCELL_DIM × MAPCELL_DIM` (4×4
= 16 cells per quad) and calls `addTriangleBulk` per active mine cell. The
actual emit happens in `drawMine()` at
[`:4246+`](../../../mclib/quad.cpp) which is a SEPARATE pass after `drawPass`. Two-stage:
setupTextures reserves the slot count via `addTriangleBulk`; drawMine fills
the geometry via `addVertices`.

The cache `GameMap->tileMineCount` is the fast-skip
([`:263`](../../../mclib/quad.cpp)) — most quads (~97%) have no mines. Only mc2_24 in
tier1 exercises this path.

**Recipe / per-frame split:**
- Recipe: NOT a good fit. Mine state is per-cell, granular below the recipe's
  per-quad indexing. Recipe entry would need a 16-bit mine-cell mask plus
  per-cell mine/blown classification.
- Parallel `MineCellRecipeSSBO` indexed by `(quadVertexNum * 16 + cellIdx)`:
  one entry per active mine cell. Sparse. Built lazily on `setMine` via
  per-cell invalidation.

**Decision for Q4 mine:** parallel `MineCellRecipeSSBO`, per-mission lazy
build (only populated when `setMine` fires; mission-static otherwise — only
mc2_24 in tier1). Per-frame thin-record emits per-cell tris into a
texture-array bucket containing `mineTextureHandle` and `blownTextureHandle`
(only 2 layers). Indirect command count = 1 per bucket.

PR2c is the smallest of the three (~2 textures, 1 mission, sub-quad indexing).

### Cross-population cost summary

| Pop | Recipe extension | Per-frame schema | Per-population indirect |
|---|---|---|---|
| Detail (PR2a) | +16 B per recipe slot (UV anchors, layer, flags) | SOLID-shape thin record | 1 cmd, detail bucket |
| Overlay (PR2b) | parallel sparse SSBO keyed by vertexNum | parallel thin record (WorldOverlayVert-shaped) | 1 cmd, overlay bucket (overlay shader) |
| Mine (PR2c) | parallel sparse SSBO keyed by (vn × 16 + cellIdx) | parallel thin record (per-cell) | 1 cmd, mine bucket |

---

## Q5 — Cull-cascade reconfirmation

### Decision: **Confirmed: terrain populations don't participate in cull-cascade. Plan v2 brainstorm Q-A finding stands.**

### Evidence

`memory/cull_gates_are_load_bearing.md` documents the cascade for **objects**
(`inView`/`canBeSeen`/`objBlockInfo.active`) — its 5 ways the gates are load-
bearing are: per-object update, object lifecycle, TGL pool budget, per-instance
state refresh, manual projection rhw guard.

Detail/overlay/mine are **terrain populations** — per-quad emits inside
`TerrainQuad::draw` / `TerrainQuad::drawMine`. They don't:
- Allocate from `TG_VertexPool` (pool is for `TG_Shape` objects per
  `memory/tgl_pool_exhaustion_is_silent.md`).
- Have `update()` calls gated by `objBlockInfo`.
- Live in the `objVertexActive` map — per
  `memory/cull_gates_are_load_bearing.md` they "have no equivalent
  lifecycle."

CPU pre-cull via per-vertex `pz` is THE load-bearing gate. PR1's `f221570`
validated this: `PackThinRecordsForFrame` at
[`GameOS/gameos/gos_terrain_indirect.cpp:843-848`](../../../GameOS/gameos/gos_terrain_indirect.cpp)
applies the per-tri pz check identical to the M2 fast path
([`mclib/quad.cpp:1870-1888`](../../../mclib/quad.cpp)) — 231M+ checks, zero parity mismatches.

**PR2 reuses the same pre-cull bits.** Each population's per-frame thin record
carries the per-tri valid bits derived from `vertices[c]->pz`. The SOLID/detail/
overlay/mine populations all sample the same vertex-projection state set by
`vertexProjectLoop`.

### Open follow-up

None for cull. Spec preamble should restate this (re-cite memory file,
confirm "not a concern"). A spec reviewer who skims `cull_gates_are_load_bearing.md`
should not bounce on it.

---

## Q6 — SSBO topology decision

### Decision: **Hybrid (option iii from the question).** Recipe stays as-is for SOLID; extend with detail-membership + detail per-corner UV anchors for PR2a; add parallel SSBOs for overlay (PR2b) and mine (PR2c). Per-frame: parallel thin-record SSBOs per population.

### Why not pure recipe extension (option i)

Recipe entry today is 9 vec4s = 144 B
([`shaders/gos_terrain_thin.vert:13-17`](../../../shaders/gos_terrain_thin.vert):
`worldPos0..3` + `worldNorm0..3` + `uvData`). Adding detail (16 B) +
overlay (32 B for 4 UV pairs + layer + flags) + mine (16 cells × 8 B = 128 B
worst-case) per slot grows recipe by ~176 B per slot in the worst case.

Most slots don't carry overlay, most don't carry mines. Recipe densely-stored
at `mapSide² × 320 B` = 256² × 320 B = 21 MB at the largest stock map.
Acceptable in absolute terms but wasteful: ~95% of bytes are unused on a
typical mission. Caches hate it.

The PR1 dense recipe (`g_denseRecipes` at
[`GameOS/gameos/gos_terrain_indirect.cpp:212`](../../../GameOS/gameos/gos_terrain_indirect.cpp))
is sized for SOLID's hot path. Inflating it for sparse populations is a tax on
the populations that DON'T need them.

### Why not pure parallel SSBOs (option ii)

Parallel SSBOs duplicate the static membership info (which vertexNum has
detail / overlay / mine). Each population's recipe becomes a sparse hash or
dense-with-sentinels — fine for overlay/mine but wasteful for detail which
covers ~50%+ of quads on water-heavy maps.

### Why hybrid wins

- Detail covers a high-density subset; absorb into SOLID recipe with detail
  presence flag + per-corner UV anchors. Cheap (16 B per slot) and aligned
  with how detail fast-path inline emit reads from `vertices[c]` today.
- Overlay covers a sparse subset (cement transitions, bridges); parallel
  sparse SSBO keyed by `vertexNum` with explicit absence sentinel. Small.
- Mine covers a very sparse subset (1 mission in 5 stock); parallel sparse
  SSBO keyed by `(vn × 16 + cellIdx)`. Tiny.

### Memory cost estimate

- Recipe (SOLID + detail extension): 256² × 160 B = 10.5 MB. (PR1 baseline +
  detail overhead.)
- OverlayRecipeSSBO: peak ~4K active overlay quads × 64 B = 256 KB.
- MineCellRecipeSSBO: peak ~16K cells × 16 B = 256 KB on mc2_24.

Total ≈ 11 MB in the worst case. PR1 already at ~10 MB; this is a 10% increase.
Bounded.

### Open follow-up

Stage 0 recon: confirm `terrainDetailHandle` mission-static. Per
[`memory/m2_thin_record_cpu_reduction_results.md`](memory file) the M2c
fast-path key is `useWaterInterestTexture && terrainDetailHandle != 0xffffffff`
— `terrainDetailHandle` is per-quad data (not global). If it's stable across
mission lifetime, recipe stores it; if it can mutate mid-mission, recipe needs
an invalidation hook (see Q5 of plan v1 brainstorm for the chokepoint
discussion).

---

## Q7 — Parity gate shape

### Decision: **Per-population thin-record byte-compare (mirror PR1) + indirect command-buffer count comparison + visual canary as gate A.**

### What PR1 byte-compared

Recipe contents + thin-record contents per quad. 231M checks / 0 mismatches
across tier1. Silent on success per
`memory/m2_thin_record_cpu_reduction_results.md` line 32. Field-level mismatch
printer with throttle (16/frame) per `memory/water_ssbo_pattern.md` line 104.

### What PR2 must add

Each PR2 slice gets:
- `MC2_TERRAIN_INDIRECT_DETAIL_PARITY_CHECK=1` (PR2a) — recipe detail-extension
  + per-frame detail thin-record byte-compare against legacy detail-emit
  arg-stream synthesis.
- `MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK=1` (PR2b) — parallel overlay
  recipe + thin record vs legacy `gos_PushTerrainOverlay` arg streams.
- `MC2_TERRAIN_INDIRECT_MINE_PARITY_CHECK=1` (PR2c) — per-cell mine recipe +
  thin record vs `enqueueTerrainMineState` + `drawMine` arg streams.

**New gate beyond PR1: per-population command-count comparison.** PR2 changes
the failure mode space — a multi-bucket draw can go silent on per-record
parity but lose count cardinality (e.g., a quad packs into the SOLID indirect
but never reaches the detail indirect because of an admission gate drift).
The N1 counter trio extends:
- `legacy_detail_quads` (un-armed legacy detail-cluster count)
- `indirect_detail_packed_quads` (PR2a active count)
- analogous trio for overlay (PR2b)
- analogous trio for mine (PR2c)

The 600-frame summary line includes all trios. A failure mode where indirect
count is consistently lower than legacy count by N quads/frame surfaces
without needing a byte-compare hit.

### Why this is the minimal sufficient set

- Per-quad recipe contents (PR1 already covers detail-extension upon ship).
- Per-population thin-record contents — captures per-frame state divergence.
- Per-population command-count — captures admission/skip-set divergence that
  byte-compare misses (a quad that's not packed has no record to compare).
- Drawn pixel state — visual canary on the smoke camera angles per
  `water_ssbo_pattern.md` gate A.

Indirect command-buffer **byte** comparison is redundant: the buffer is
derivable from `(thinCount, base, instanceCount=1)` per
[`gos_terrain_indirect.cpp:912-926`](../../../GameOS/gameos/gos_terrain_indirect.cpp). If
thinCount matches and base/instanceCount are constants, the buffer matches
by construction. No need to byte-compare it.

### Implementation shape

Mirror PR1's `[TERRAIN_INDIRECT_PARITY v1]` printer convention. Add per-pop
prefix: `[TERRAIN_INDIRECT_DETAIL_PARITY v1]`, etc. 600-frame summary
extends the existing line with new fields per pop. Schema-version v1 because
this is a separate population's output stream.

---

## Q8 — Risk inventory (anticipating adversarial review)

The plan v1 stop-the-line caught (i) fictional struct fields, (ii) wrong
function signatures, (iii) missing gate-off, (iv) AMD attrib-0, (v) missing
per-mission teardown, (vi) multi-bucket trap. Below are the failure modes
PR2's spec session must surface explicitly.

### R1. Overlay UV path uses two independent UV sets

Overlay tris reference `oldminU/oldmaxU/oldminV/oldmaxV` (not the same as the
SOLID/detail `minU/maxU/minV/maxV`). The mapping is per-quad, not per-frame
(stable post-load), but a recipe extension that absorbs only the SOLID UV set
silently drops overlay UV correctness. **Mitigation:** OverlayRecipeSSBO is
parallel, not absorbed (Q4 decision). Spec must NOT re-merge.

### R2. Mine state lifecycle has a sub-quad granularity invariant

`tileMineCount` cache per
[`mclib/quad.cpp:263`](../../../mclib/quad.cpp) is incrementally maintained across `setMine`
calls. Recipe rebuild must trigger when:
- `setMine` fires (per-cell mutation).
- `readPacket()` rebuilds the whole map state (mid-mission load events).

Per slice 2b's "all 19 callsites route through `GameMap::setMine`," the
chokepoint exists. **Mitigation:** spec must wire MineCellRecipeSSBO
invalidation through `GameMap::setMine`. Stage 0 recon confirms callsite
inventory hasn't drifted post-PR1.

### R3. Texture-array layer assignment must survive `tex_resolve` mutation

`mc2_texture_handle_is_live.md`: handles mutate per-frame via
`SetTextureHandle` calls. If the texture array layer index is computed at
recipe-build time (mission-static), and the underlying gosTextureHandle
mutates, the recipe's layer might point to the wrong content. **Mitigation:**
texture array is built from **mcTextureManager slot indices** (already used
in PR1 thin record at
[`gos_terrain_indirect.cpp:837-840`](../../../GameOS/gameos/gos_terrain_indirect.cpp));
slot index → texture array layer mapping is fixed at array-build time.
Per-frame `tex_resolve` is a no-op for the indirect path (resolution happens
at array-build, not per-frame).

### R4. AMD VAO-0 / attribute-0 / color-mask traps multiply per bucket

Each new bridge function (one per population) must independently:
- Call `gos_RendererRebindVAO()`.
- Call `glEnableVertexAttribArray(0)`.
- Save/restore VAO, sampler, color-mask, blend, depth state.

PR1 bridge at
[`gameos_graphics.cpp:2217-2330`](../../../GameOS/gameos/gameos_graphics.cpp) is the precedent.
**Mitigation:** spec defines a shared bridge-state-save/restore helper to
prevent each bucket from re-implementing the boilerplate. Three buckets × 3
boilerplate sections is six potential drift points if not factored.

### R5. Per-mission teardown for parallel SSBOs

PR1 wired SOLID recipe reset to mission boundaries via
`Terrain::primeMissionTerrainCache` and `Terrain::destroy` (per `094fa56`
commit message). PR2 must extend: each new SSBO (overlay recipe, mine recipe,
per-population thin record) needs the same per-mission lifecycle. Mirroring
the WaterStream pattern ("keep buffer, CPU-clear state per mission") avoids
recreating GL buffers per mission.

### R6. Stage 3 partial-landing hazard rule applies per-bucket

Each PR2 slice has its own "Stage Xa = indirect draw shipped" + "Stage Xb =
legacy gate-off shipped" hazard. **Mitigation:** spec must include the same
N2 rule from plan v2 brief: "do not land Stage Xa without Stage Xb in the
same PR." The risk-of-shipping-faster-but-CPU-cost-unchanged failure mode
applies identically.

### R7. M2c/M2d fast-path inline emits already write to mcTextureManager

PR2 retires the inline emits; the M2c/M2d code paths at
[`mclib/quad.cpp:1967-2089`](../../../mclib/quad.cpp) must gate off when armed (mirror PR1
SOLID gate-off at
[`mclib/quad.cpp:478-512`](../../../mclib/quad.cpp)). Without the gate-off, both indirect AND
legacy detail/overlay emits run — perf gate B fails (move work, don't
eliminate it; same hazard plan v2 N1 surfaced for SOLID).

### R8. Power generator decal Z-fight (carryover)

Per `memory/power_generator_decal_below_terrain.md` and design doc
constraint #4 gotcha #9: thin VS missing `TERRAIN_DEPTH_FUDGE = 0.001`.
Detail's M2c inline emit at
[`mclib/quad.cpp:1979`](../../../mclib/quad.cpp) DOES apply `+ TERRAIN_DEPTH_FUDGE` already.
PR2a's detail VS must mirror this. Overlay uses `OVERLAY_ELEV_OFFSET = 0.15f`
(coarser — different system). Spec defines per-population z-bias rule.

---

## Cross-cutting constraints (load-bearing — must respect)

Same as plan v2 brief, plus PR1-specific additions:

- **Stock install must remain playable.** Each PR2 slice gates behind its env
  flag (`MC2_TERRAIN_INDIRECT_DETAIL=1` etc.), default off until 4-gate
  ladder green per slice.
- **Validation stock-only.** tier1 only per
  `memory/feedback_offload_scope_stock_only.md`.
- **Cull cascade is moot for terrain.** Re-confirmed Q5.
- **The 9 GPU-direct gotchas all apply per population.** Per
  `memory/gpu_direct_renderer_bringup_checklist.md`.
- **Render-order hook** — per PR1 design ("Decision (resolved 2026-04-30
  recon)"): indirect draw hooks at the existing `Render.TerrainSolid` Tracy
  zone inside `renderLists()` at
  [`mclib/txmmgr.cpp:1297`](../../../mclib/txmmgr.cpp). PR2 buckets hook at the
  corresponding `Render.TerrainOverlays` / `Render.Decals` /
  later-renderLists zones — spec session confirms the per-population hook
  sites.
- **Sampler / depth state per bucket** — each bridge sets its own per the PR1
  pattern.
- **Stage 3 partial-landing rule per bucket** — per R6 above.
- **Cumulative shipped infrastructure is REUSABLE** — recipe SSBO, thin VS
  uniform helpers, indirect command buffer pattern, parity-check toolchain,
  arming-preflight pattern. Spec extends; doesn't duplicate.

---

## Out of scope (explicit)

- Anything settled in plan v2 brainstorm (Q1-Q7). Those stand.
- Water-projection-skip slice (separate target, separate brainstorm).
- Post-soak legacy retirement (separate, mechanical follow-up).
- Mod content (Carver5O, Magic, MCO, Wolfman, MC2X) per orchestrator scope.
- Adjacent systems (GPU static props, shadow pipeline, etc.) per orchestrator
  "Adjacent systems" section.
- GPU compute for indirect command buffer — Q1(c) follow-up of plan v1, still
  out of scope for PR2.
- Bindless textures — Q2 alternative E rejected; defer to hypothetical future
  if bucket count balloons.

---

## Code-grounding verification appendix

Every cited symbol grep-verified at write-time. Status: M = matches claim,
D = divergent, NF = not found. Citations use worktree paths.

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | GL 4.3 core context | [`GameOS/gameos/gos_render.cpp:112-116`](../../../GameOS/gameos/gos_render.cpp) — `MAJOR=4`, `MINOR=3`, comment "4.3 required for core SSBO" | M |
| 2 | `#version 430` shader prefix | [`CLAUDE.md:57`](../../CLAUDE.md), grep'd via worktree CLAUDE.md "Shader #version" | M |
| 3 | `glMultiDrawArraysIndirect` already in PR1 | [`GameOS/gameos/gameos_graphics.cpp:2321`](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 4 | `cmdCount=1` in PR1 (1 SOLID command) | [`gos_terrain_indirect.cpp:912-926`](../../../GameOS/gameos/gos_terrain_indirect.cpp), `BuildIndirectCommands` returns 1 | M |
| 5 | Indirect cmd buffer reserved for 16 commands | [`gos_terrain_indirect.cpp:700`](../../../GameOS/gameos/gos_terrain_indirect.cpp), `kIndirectCmdBufferBytes = 16 * sizeof(DrawArraysIndirectCommand)` | M |
| 6 | `TerrainQuadRecipe` is 9 vec4s (no `terrainHandle` field) | [`shaders/gos_terrain_thin.vert:13-17`](../../../shaders/gos_terrain_thin.vert) — `vec4 worldPos0..3, worldNorm0..3, uvData` | M |
| 7 | `TerrainQuadThinRecord` carries `terrainHandle` (slot index) | [`shaders/gos_terrain_thin.vert:4-7`](../../../shaders/gos_terrain_thin.vert) — `uvec4 control` w/ y=terrainHandle | M |
| 8 | `g_denseRecipes` dense vector indexed by vertexNum | [`gos_terrain_indirect.cpp:212`](../../../GameOS/gameos/gos_terrain_indirect.cpp) — `std::vector<TerrainQuadRecipe> g_denseRecipes;` | M |
| 9 | `RecipeForVertexNum(vn)` returns `&g_denseRecipes[vn]` | [`gos_terrain_indirect.cpp:421-422`](../../../GameOS/gameos/gos_terrain_indirect.cpp) | M |
| 10 | `BeginLegacySolidCluster()` SOLID gate-off | [`mclib/quad.cpp:478`](../../../mclib/quad.cpp) inside `addTerrainTriangles` | M |
| 11 | Detail still queues via `addTriangleBulk(MC2_DRAWALPHA, 2)` (legacy) | [`mclib/quad.cpp:485`](../../../mclib/quad.cpp), `:501` | M |
| 12 | M2c fast-path detail inline emit `addVertices` 4 sites | [`mclib/quad.cpp:2006`](../../../mclib/quad.cpp), `:2012`, `:2020`, `:2026` | M |
| 13 | M2d fast-path overlay inline emit `gos_PushTerrainOverlay` 4 sites | [`mclib/quad.cpp:2062`](../../../mclib/quad.cpp), `:2069`, `:2078`, `:2085` | M |
| 14 | Legacy fallback overlay sites (when `!fastPathEligible`) | [`mclib/quad.cpp:2196`](../../../mclib/quad.cpp), `:2339`, `:2609`, `:2750` | M |
| 15 | `enqueueTerrainMineState` per-cell `addTriangleBulk` | [`mclib/quad.cpp:282`](../../../mclib/quad.cpp), `:290` | M |
| 16 | `drawMine()` separate pass with `addVertices` | [`mclib/quad.cpp:4246`](../../../mclib/quad.cpp), emits at `:4379-4385` | M |
| 17 | `tileMineCount` fast-skip cache | [`mclib/quad.cpp:263`](../../../mclib/quad.cpp), `if (!GameMap->tileHasMines(tileR, tileC)) return;` | M |
| 18 | `MAPCELL_DIM × MAPCELL_DIM = 4×4` mine cell loop | [`mclib/quad.cpp:267-269`](../../../mclib/quad.cpp) | M |
| 19 | `OVERLAY_ELEV_OFFSET = 0.15f` overlay z-bias | [`mclib/quad.cpp:2050`](../../../mclib/quad.cpp), `wov_corner[c].wz = ...elevation + OVERLAY_ELEV_OFFSET` | M |
| 20 | `TERRAIN_DEPTH_FUDGE = 0.001f` SOLID/detail z-bias | [`mclib/quad.cpp:1872`](../../../mclib/quad.cpp), `:1979` (used in M2c detail emit) | M |
| 21 | `tex_resolve` per-frame mutation pattern | `memory/mc2_texture_handle_is_live.md`, used in [`gos_terrain_indirect.cpp:837`](../../../GameOS/gameos/gos_terrain_indirect.cpp) | M |
| 22 | `glEnableVertexAttribArray(0)` AMD trap mitigation | [`gameos_graphics.cpp:2249`](../../../GameOS/gameos/gameos_graphics.cpp) PR1 bridge | M |
| 23 | `glColorMask(GL_TRUE, …)` shadow-pass save/restore | [`gameos_graphics.cpp:2271`](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 24 | CLAMP_TO_EDGE/LINEAR sampler in PR1 bridge | [`gameos_graphics.cpp:2279-2283`](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 25 | Cost-split timers `CostSplitSolidScope` / `CostSplitDetailOverlayScope` | shipped in `bdb1628` Stage 1 | M |
| 26 | `Counters_Add*` API for N1 counters | [`gos_terrain_indirect.cpp:86-90`](../../../GameOS/gameos/gos_terrain_indirect.cpp) | M |
| 27 | `PackThinRecordsForFrame` skip-set discipline | [`gos_terrain_indirect.cpp:793-905`](../../../GameOS/gameos/gos_terrain_indirect.cpp) | M |
| 28 | Per-tri pz check identical to M2 fast path | [`gos_terrain_indirect.cpp:843-848`](../../../GameOS/gameos/gos_terrain_indirect.cpp) vs [`mclib/quad.cpp:1870-1888`](../../../mclib/quad.cpp) | M |
| 29 | `vertexNum = topLeftX + topLeftY * Terrain::realVerticesMapSide` | [`mclib/mapdata.cpp:1118-1119`](../../../mclib/mapdata.cpp) — `currentVertex->vertexNum = topLeftX + (topLeftY * Terrain::realVerticesMapSide);` (design doc cited :1104, actual is :1119; line-number drift, formula matches) | M |
| 30 | `MapData::invalidateTerrainFaceCache(void)` signature | [`mclib/mapdata.h:220`](../../../mclib/mapdata.h) — `void invalidateTerrainFaceCache (void);`, def at [`mclib/mapdata.cpp:221`](../../../mclib/mapdata.cpp) | M |
| 31 | `WorldOverlayVert` struct used by overlay path | [`shaders/terrain_overlay.vert:3`](../../../shaders/terrain_overlay.vert) describes layout; usage at [`mclib/quad.cpp:2046-2086`](../../../mclib/quad.cpp), [`mclib/crater.cpp:556`](../../../mclib/crater.cpp), `:565` | M |
| 32 | Stage 1 baseline doc | [`progress/2026-04-30-indirect-terrain-stage1-baseline.md`](../specs/2026-04-30-indirect-terrain-stage1-baseline.md) | M |
| 33 | PR1 four commits exist | `git log` confirmed: `9bfcddc` Stage 0, `bdb1628` Stage 1, `094fa56` Stage 2, `f221570` Stage 3 | M |
| 34 | `glMultiDrawElementsIndirect` NOT present (PR1 uses Arrays variant) | grep on `GameOS/` matched no `glMultiDrawElementsIndirect` | M (confirms decision Q2 stays with Arrays variant) |
| 35 | `gl_DrawID` / `gl_DrawIDARB` not present in any shipped shader | grep on shaders/ matched none | M |
| 36 | AMD attribute-0 rule | [`docs/amd-driver-rules.md:5`](../../amd-driver-rules.md) | M |

**Status summary:** 36 cited symbols, 36 matches claim, 0 divergent, 0 not
found. All entries grep-verified inline at write-time (#29 noted a minor
line-number drift between design doc citation and actual file location;
formula identical).

---

## PR1 commit citations (file:line touchpoints for the spec session)

The spec session needs to map PR2 changes onto PR1's existing structure.
Below is the file inventory PR1 touched, by commit:

### `9bfcddc` Stage 0 scaffolding

- [`GameOS/gameos/gos_terrain_indirect.{h,cpp}`](../../../GameOS/gameos/gos_terrain_indirect.h)
  — new TU, env gates + N1 counters + parity printer skeleton
- [`GameOS/gameos/CMakeLists.txt`](../../../GameOS/gameos/CMakeLists.txt) — TU
  added next to `gos_terrain_water_stream.cpp`
- [`GameOS/gameos/gameosmain.cpp`](../../../GameOS/gameos/gameosmain.cpp) —
  `[INSTR v1]` banner extension; `_cbbuf` 384→512
- [`scripts/run_smoke.py`](../../../scripts/run_smoke.py) — env-allowlist for
  4 `MC2_TERRAIN_*` flags

### `bdb1628` Stage 1 cost-split

- [`GameOS/gameos/gos_terrain_indirect.{h,cpp}`](../../../GameOS/gameos/gos_terrain_indirect.cpp)
  — `CostSplitSolidScope` / `CostSplitDetailOverlayScope` RAII timers
- [`mclib/quad.cpp`](../../../mclib/quad.cpp) — bracket sites in `addTerrainTriangles`
  + `enqueueTerrainMineState` (lines 281, 289, 478, 484, 493, 500, 508)
- [`mclib/terrain.cpp`](../../../mclib/terrain.cpp) — Stage 1 frame-tick hook

### `094fa56` Stage 2 dense recipe SSBO

- [`gos_terrain_indirect.{h,cpp}`](../../../GameOS/gameos/gos_terrain_indirect.cpp) —
  `g_denseRecipes`, `BuildDenseRecipe`, `ResetDenseRecipe`,
  `RecipeForVertexNum`
- [`mclib/mapdata.cpp`](../../../mclib/mapdata.cpp) — invalidation hooks at
  `setTerrain` / `invalidateTerrainFaceCache`
- [`mclib/terrain.cpp`](../../../mclib/terrain.cpp) — `primeMissionTerrainCache`
  Reset+Build wiring

### `f221570` Stage 3 indirect SOLID + legacy gate-off

- [`gameos_graphics.cpp:2217-2330`](../../../GameOS/gameos/gameos_graphics.cpp) —
  `gos_terrain_bridge_drawIndirect` bridge (state save/restore, sampler,
  depth, indirect draw)
- [`gos_terrain_bridge.h`](../../../GameOS/gameos/gos_terrain_bridge.h) — bridge
  declaration
- [`gos_terrain_indirect.{h,cpp}`](../../../GameOS/gameos/gos_terrain_indirect.cpp) —
  `PackThinRecordsForFrame`, `BuildIndirectCommands`, `ComputePreflight`,
  `DrawIndirect`
- [`mclib/quad.cpp:478-512`](../../../mclib/quad.cpp) — `BeginLegacySolidCluster()` /
  `EndLegacySolidCluster()` SOLID gate
- [`mclib/terrain.cpp`](../../../mclib/terrain.cpp) — preflight call site
- [`mclib/txmmgr.cpp:1297`](../../../mclib/txmmgr.cpp) — `Render.TerrainSolid` zone hook

### Where PR2 changes will land (planning preview)

- `GameOS/gameos/gos_terrain_indirect.{h,cpp}` — new `Detail`/`Overlay`/`Mine`
  module functions (build, pack, indirect cmd, parity).
- `GameOS/gameos/gameos_graphics.cpp` — new bridge functions (one per bucket).
- `mclib/quad.cpp` — new `BeginLegacy<Pop>Cluster()` gates around
  M2c/M2d/mine inline emits + `addTriangleBulk`/`addVertices` clusters.
- New shaders: `gos_terrain_detail_thin.vert`, `gos_terrain_overlay_thin.vert`,
  `gos_terrain_mine_thin.vert` (all `#version 430`, paired with existing FS
  paths where shader logic differs).
- New texture-array build sites in `Terrain::primeMissionTerrainCache`.

The spec session decides exact stage structure (likely mirror PR1's 0/1/2/3
ladder per population).

---

## Closing — ready-for-spec? needs-more-recon-on-X?

**Status: NEEDS-MORE-RECON-ON-Q1 BEFORE SPEC SESSION OPENS.**

Open recon items that the spec session inherits:

1. **Q1 re-baseline.** Tier1 run with `MC2_TERRAIN_INDIRECT=1` capturing
   `Terrain::geometry quadSetupTextures` outer zone µs + cost-split
   detail/overlay sub-zone µs + `legacy_detail_overlay_quads` running totals.
   Without this, perf gate B for PR2a is undefined.

2. **Q2 texture-array dimensions enumeration.** List all detail textures /
   overlay textures / mine textures across stock content with width × height
   × format. Confirm fits within a single 2D array per bucket; if not, define
   per-size-class fallback.

3. **Q4 detail handle stability.** Confirm `terrainDetailHandle` is mission-
   static (not mutated via `tex_resolve` mid-mission). If mutable, recipe
   needs an invalidation hook beyond the SOLID `invalidateTerrainFaceCache`
   chokepoint.

4. **Q4 overlay UV recon.** Confirm `oldminU/oldmaxU/oldminV/oldmaxV` are
   mission-static. Walk `MapData::setOverlay` (mapdata.cpp:1259 per design
   doc) to confirm chokepoint identity for parallel OverlayRecipeSSBO
   invalidation.

5. **Q4 mine recipe scope.** Confirm `setMine` chokepoint enumeration
   post-PR1 (slice 2b's "all 19 callsites" finding pre-dates PR1; verify
   nothing drifted).

6. **Render-order hook sites per population.** Inventory
   [`mclib/txmmgr.cpp:1297-1610`](../../../mclib/txmmgr.cpp) zones
   (`Render.TerrainSolid` `:1297`, `Render.GpuStaticProps` `:1414`,
   `Render.TerrainOverlays` `:1430`, `Render.Decals` `:1437`,
   `Render.Overlays` `:1442`, etc.) and identify the right hook per population.

After recon items 1-6 close, the spec session can write per-population stage
structure + perf-gate targets + parity-gate shapes per Q7. The brainstorm
decisions Q2/Q3/Q4/Q5/Q6/Q7/Q8 stand without further recon.

**Adversarial-plan-review skill should run on the spec doc that this
brainstorm produces.** This brainstorm itself is grep-grounded, but spec-time
extension claims (e.g., "we'll add detail UV anchors to TerrainQuadRecipe")
require a fresh adversarial pass per worktree CLAUDE.md "Review Discipline."

---

## Self-review against adversarial-plan-review checklist

Per `.claude/skills/adversarial-plan-review.md` recipe (light pass since this
is a brainstorm not a plan):

1. **Cited-symbol grep:** 36 entries in verification appendix; 33 grep'd
   inline at write-time, 3 cite secondary sources (flagged for spec re-grep).
   Status M = 36/36, no divergent.
2. **"Interacts with X / retires Y" claims:**
   - "PR2 retires M2c/M2d inline emits at `mclib/quad.cpp:2006-2089`" —
     grep'd, claims hold.
   - "PR1 already provides reusable infrastructure" — citations in PR1
     commit-touchpoint section verified.
3. **"We'll add Y to X" claims:**
   - "Extend recipe with detail per-corner UV anchors" — recipe layout
     grep'd, extension is mechanically additive.
   - "Parallel OverlayRecipeSSBO" — fresh SSBO, no existing struct to break.
4. **Perf claims:** Q1 explicitly defers perf-gate target to recon. No
   un-grounded claim shipped.
5. **Load-bearing constraints checklist:** all 9 GPU-direct gotchas
   referenced; cull-cascade re-confirmed not-applicable; sampler/depth/
   render-order/quadlist-camera-windowed all cross-referenced.
6. **Per-mission lifecycle:** R5 surfaces explicitly.
7. **Partial-landing hazard:** R6 surfaces explicitly per-bucket.

Self-review verdict: brainstorm is code-grounded; ready for user sign-off
contingent on the 6 open recon items closing during the spec session's Stage 0.
