# RenderWorld Slice M3 — TerrainRenderAdapter / terrain identity recon

Date: 2026-05-23
Status: RECON ONLY — no spec, no plan
Predecessors: M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6 (all SHIPPED 2026-05-23)
Author: recon subagent

## 1. Summary

- The terrain pass already renders **inside the scene FBO** that
  `beginScene` has bound with `setSceneDrawBuffers(MainSceneMRT, true)`
  at `GameOS/gameos/gos_postprocess.cpp:488` / `:497`. **No new
  `setSceneDrawBuffers` site is needed for M3.** The substrate is
  already armed when terrain draws.
- Four terrain frag shaders currently write only `FragColor`
  (attachment-0) + `GBuffer1` (attachment-1):
  `shaders/gos_terrain.frag:32,34`, `gos_terrain_water_mdi.frag:24,25`,
  `gos_terrain_mine_static.frag:29,31`, `terrain_overlay.frag:18`. A
  third color-attachment write (`layout(location=2) out uint v_objectId`
  under `#ifdef MC2_OBJECT_ID_BUFFER`) is the M3 shader-side delta.
- **There is NO CPU-side fallback that bypasses these frag shaders.**
  Per `mclib/terrain.cpp:1095-1126`, on the default path
  (`IsFrameSolidArmed() && IsFrameOverlayArmed()` both true since
  60f2ef8) the per-quad `draw()` loop is SKIPPED; the GPU-indirect
  pipeline is the only producer for SOLID + decals. Water uses the
  fast-path MDI program. The pure-MLR-terrain risk class that M2.5
  carried (the `mlr_mech_draws=N` measurable counter) **does not exist
  for terrain** — terrain has no MLR fallback. (Negative claim verified
  by `grep "MLR.*terrain"` across the tree: zero hits.)
- **Five identity-unit options evaluated** (see §3). Recommendation:
  **Option 5 (terrain unpickable) for M3 v1**, with a forward-compatible
  reservation of `RenderObjectKind::Terrain = 2`. Justification: the
  CPU-side terrain pick already exists, is correct, and emits world-XYZ
  / tileR / tileC via `inverseProject -> wPos -> worldToTile`; the GPU
  substrate would add zero information and introduces non-trivial
  per-pixel handle encoding cost across ~39,601 visible quads/frame.
- If a stronger identity is later needed (terrain editor, debug
  click-to-inspect-tile, decal authoring), **Option 1 (quad) and
  Option 2 (chunk) are both viable** within the 20-bit handle-index
  ceiling (`RenderCore::Handle.h:34,38`: index mask `0xFFFFF` = 1,048,575
  slots). At 39,601 visible quads + ~196,000 total map quads on
  GameVisibleVertices=200, even per-map-quad allocation fits.
- The CPU pick path's behavior **after `tryGameplayPick(req)` returns
  `miss`** is documented at `code/missiongui.cpp:1652,3660` etc.: the
  caller falls through to `Terrain::IsGameSelectTerrainPosition(wPos)`
  + `doMove(wPos)` for ground-target movement orders. This works today
  and is the M3-OFF behavior. Pickup callers (`tryStaticPropPick`,
  `tryMechPick`) only fire on Shift+LMB; plain-LMB ground-click is
  unaffected by M1.6/M2.6 and would be unaffected by M3.
- **Handle range proposal if Option 1 is later chosen:**
  `kTerrainHandleBase = 0x40000` (262,144) — leaves the 0x10000..0x3FFFF
  range (245,760 slots) for mech expansion above the current
  kMechHandleBase = 0x10000, and the 0x40000..0xFFFFF range (786,431
  slots) for terrain quads. 196K map quads + Option-1 future fit.
- **Stable lifetime?** Terrain quads have full-mission lifetime — they
  are allocated once at `Terrain::init` and torn down at `Terrain::destroy`
  (`mclib/terrain.cpp:895`); no in-mission churn. Generation bumps would
  be unnecessary outside the end-mission tear-down, satisfying the
  Handle invariants trivially.

## 2. Terrain rendering write paths

### 2.1 Frag shaders that write the visible terrain color attachment

| Shader | Attachment-0 line | Attachment-1 line | Currently writes attachment-2? |
|---|---|---|---|
| `shaders/gos_terrain.frag` | 32 (`FragColor`) | 34 (`GBuffer1`) | NO |
| `shaders/gos_terrain_water_mdi.frag` | 24 (`FragColor`) | 25 (`GBuffer1`) | NO |
| `shaders/gos_terrain_mine_static.frag` | 29 (`FragColor`) | 31 (`GBuffer1`) | NO |
| `shaders/terrain_overlay.frag` | 18 (`FragColor`) | — | NO |

(`shaders/shadow_terrain.frag` writes depth only and is not part of the
main scene MRT — irrelevant to M3.)

These four are where a `layout(location=2) out uint v_objectId` under
`#ifdef MC2_OBJECT_ID_BUFFER` would land if M3 chose Option 1 or 2.
The pattern is identical to `shaders/static_prop.frag:71` (verified) and
`shaders/mech.frag` (per the M2.5 SHIPPED entry).

### 2.2 CPU-side render dispatch — the live path

`mclib/terrain.cpp:1059 Terrain::render(void)` is the single per-frame
entry point. The body executes:

1. **`maskBuild`** (line 1077-1081, env-gated) —
   `gos_terrain_mask_dispatch::BuildAndUploadMasksForFrame`.
2. **`drawPass`** (line 1083-1145) — *NO-OP on the default path*. The
   per-quad `currentQuad->draw()` loop is gated by
   `!(IsFrameSolidArmed() && IsFrameOverlayArmed())`. Since 60f2ef8
   (Stage-6 flip), both are true on the default path, so the loop is
   skipped. The else branch logs `[TERRAIN_DRAWPASS v1] event=retired`
   and is the live default.
3. **`minePass`** (line 1147-1164) — gated identically by
   `!IsFrameMineArmed()`; default-armed, loop skipped.
4. **`debugOverlays`** (line 1166-1192) — only when
   `drawTerrainGrid || DrawDebugCells || drawLOSGrid`; debug-only.

The visible terrain pixels come from the GPU-indirect path — the
solid+decal+water+mine programs are loaded at:

- `GameOS/gameos/gameos_graphics.cpp:1653` (`thin_terrain_prog_`,
  `gos_terrain_thin.vert + gos_terrain.frag`)
- `GameOS/gameos/gameos_graphics.cpp:1654` (`terrain_surface_prog_`,
  `gos_terrain_surface.vert + gos_terrain.frag` — TERRAIN_SURFACE PR-2
  continuous-surface VS, vertex-pulled from V-SSBO)
- `GameOS/gameos/gameos_graphics.cpp:1656` (`mine_static_prog_`,
  `gos_terrain_mine_static.{vert,frag}`)
- `GameOS/gameos/gameos_graphics.cpp:1657` (`mask_solid_prog_`,
  `gos_terrain_mask_solid.vert + gos_terrain.frag`)
- `GameOS/gameos/gameos_graphics.cpp:1827` (`overlayProg_`,
  `terrain_overlay.{vert,frag}`)
- `GameOS/gameos/gameos_graphics.cpp:2106-2108`
  (`gos_terrain_water_mdi.frag`)

These ~5 programs would each need the GLSL-prefix injection mirroring
`gos_static_prop_batcher.cpp:510-521` (M1.5 pattern) and
`gos_mech_batcher.cpp::loadProgramsIfNeeded()` (M2.5 pattern) IF M3
chose Option 1 or 2.

### 2.3 Scene-FBO bind ownership — already armed

Terrain renders entirely **inside the bind window owned by `beginScene`**:

- `GameOS/gameos/gos_postprocess.cpp:488` —
  `setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT, sceneObjectIdTex_ != 0)`
- `GameOS/gameos/gos_postprocess.cpp:497` — second call (paired pre-shadow
  re-arm).

No terrain-specific `glBindFramebuffer`/`glDrawBuffers` in
`mclib/terrain.cpp`, `mclib/quad.cpp`, `GameOS/gameos/gos_terrain_*`.
Negative grep (terrain TUs for `glDrawBuffers`) returns zero hits.

**Conclusion: §5 below — M3 needs no new `setSceneDrawBuffers` site.**

## 3. Terrain identity candidates — evaluation

| # | Unit | What it maps to (CPU) | Count per mission (mc2_24) | Lifetime | Handle range needed | Existing consumer? | Verdict |
|---|---|---|---|---|---|---|---|
| 1 | Quad / tile | `TerrainQuadPtr quadList[]` indexed 0..`numberQuads-1`. Mission-total quad count is `(realVerticesMapSide - 1)^2`. Visible window per frame `(visibleVerticesPerSide - 1)^2 ≈ 39,601`. | Visible: 39,601. Mission-total: ~196K-786K depending on map size (clipRange-driven; `GameVisibleVertices=200` hard lock at `mechcmd2.cpp:1481`). | Mission-lifetime (allocated `Terrain::init`, freed `Terrain::destroy` at `mclib/terrain.cpp:895-896`). | 20 bits = 1,048,575 slots. Fits worst-case. Propose `kTerrainHandleBase = 0x40000`. | None today. Potential: terrain editor (out-of-scope per PROJECT.md? unverified), debug click-to-inspect-quad. | VIABLE for future, but no consumer drives it today. |
| 2 | Chunk / block | `blocksMapSide` / `numObjBlocks` (`mclib/terrain.cpp:902,908`). One block ≈ 4×4 quads (per terrain layout). Roughly 16× smaller than per-quad count. | 12K-49K per mission (mission-total); ~2,500 visible. | Mission-lifetime (same allocator as quads). | 16 bits / 0x10000 slots fits per-mission. `kTerrainHandleBase = 0x40000` still fine. | None today. Potential: shadow/light cache invalidation (current code already uses `objBlockInfo` for this; doesn't need a Handle). | VIABLE; would coarsen pixel identity to 4×4 region. No clear consumer. |
| 3 | Heightmap vertex | `realVerticesMapSide^2` vertices addressed by `getVertexHeight(VertexIndex)` (`mclib/terrain.cpp:1050`). | Same scale as quads. | Mission-lifetime. | Same as Option 1. | None today. Editor use case is hypothetical. | NOT recommended — vertex-precision identity has no rendering-time consumer (frags interpolate from 3-4 vertices). |
| 4 | Triangle ID | Each quad is 2 triangles; `gl_PrimitiveID` accessible in frag. Mission-total ~400K-1.5M triangles. | Same scale × 2 vs Option 1. | Per-frame (re-tessellated each frame; `gl_PrimitiveID` is not stable across LOD changes). | 20 bits TIGHT; could overflow on largest maps. | None. | NOT recommended — instability across LOD + count pressure on handle width. |
| 5 | None — terrain unpickable | `lookupAtPixel` over a terrain pixel returns `invalid()` (because `v_objectId` is never written on terrain pixels; the attachment-2 default value is 0 from `glClearBufferuiv`). | n/a | n/a | n/a (no allocation). | Existing: legacy CPU-side `Terrain::IsGameSelectTerrainPosition(wPos)` + `doMove(wPos)` path remains the terrain interaction (gameplay movement-target). | **RECOMMENDED FOR M3 v1.** |

### Recommendation: Option 5 (terrain unpickable) for M3 v1

Rationale:

1. **No identified consumer.** The Shift+LMB inspect gesture (M1.6/M2.6
   pattern) targets specific game-object kinds; a "terrain inspect" with
   no fielded data (texture handle, terrain type, elevation, tile R/C)
   has no destination today. Adding the substrate without a consumer
   violates the load-bearing change-discipline rule in CLAUDE.md
   ("don't touch what you don't have to … every touch has blast radius").
2. **CPU pick already correct.** The current `wPos`-driven path
   (`code/missiongui.cpp:773-789`) does CPU-side inverseProject of the
   mouse to a world position, then `Terrain::IsGameSelectTerrainPosition`
   filters it. `worldToTile` (`mclib/terrain.h:368`) and
   `worldToTileCell` (`:387`) give exact tile R/C if needed. This is
   the producer for unit-move orders today; works without GPU input.
3. **Cost.** Adding `v_objectId` writes to 5 terrain programs × ~39,601
   visible quads × per-pixel cost is non-trivial; the M1.5 SHIPPED
   measurement budgeted ≤0.5ms p99 across the WHOLE substrate; terrain
   would dwarf that (terrain pixels dominate the framebuffer at RTS
   zoom). Empirically a per-pixel uint write is cheap on AMD, but
   gating 5 program reloads (and the shader-prefix injection
   discipline that goes with it) is real engineering cost.
4. **Forward compatibility preserved.** The
   `enum class RenderObjectKind : uint8_t { StaticProp=0, Mech=1,
   // Future: Terrain=2 ... }` (`RenderWorld/RenderWorld.h:131-135`)
   already reserves the slot. M3 v1 can ship as a documentation
   change + forward-decl reservation; M3 v2 (if a consumer ever
   materializes) lifts Option 1 or 2.
5. **`lookupAtPixel` semantic is already correct.** Per spec
   (`RenderWorld/RenderWorld.h:169-176`), `LookupResult{isValid=false}`
   is the documented return when the pixel is `0` (background, debug
   mode, or — with M3 Option-5 — terrain). Callers already gate on
   `isValid` first. No upstream code change needed.

### If Option 1 is later chosen: encoding sketch

- Handle index = `tileR * realVerticesMapSide + tileC` + `kTerrainHandleBase`.
- Static unique-per-mission, computed at `Terrain::init` not per-frame.
- One Handle per quad, stored on the quad — or computed on the fly
  inside the vertex shader from instance ID + per-frame `realVerticesMapSide`
  uniform.
- The recipe-index analog for terrain is the (R, C) tuple, NOT the
  terrainHandle texture handle (which is shared across many quads).

## 4. Existing terrain pick paths (CPU-side)

### 4.1 Plain-LMB ground-click (live; M3-irrelevant)

Path begins at `code/missiongui.cpp:773-789` (`MIF.LOS` zone). Mouse XY
inverse-projected at line 775 (`inverseProjectCacheValid = true`); the
result is `wPos` (a `Stuff::Vector3D` world position). Filtered through
`Terrain::IsGameSelectTerrainPosition(wPos)` at line 784 to confirm it
sits on the playable terrain (rejects out-of-bounds map clicks).

Consumers (file:line, all in `code/missiongui.cpp`):

| Line | Caller | Action |
|---|---|---|
| 784 | LOS update | line-of-sight + cell lookup for cursor display |
| 1414 | `updateOldStyle` ground branch | unit move via `doMove(wPos)` |
| 1652 | `updateAOEStyle` ground branch | unit move via `doMove(wPos)` (verified above) |
| 2117 | (style-specific path) | cursor / target |
| 3660 | RMB-release path | move-on-up |
| 4032 | (additional ground-target path) | cursor / target |

None of these consume a `RenderObjectHandle`. All consume `wPos`.

### 4.2 What happens after `tryGameplayPick(req)` returns `miss`

The two M2.6 callers (`code/missiongui.cpp:1541-1547` static-prop,
`:1552-1558` mech, both inside `updateOldStyle`; identical pair inside
`updateAOEStyle`) fire on Shift+LMB. On `Outcome::miss` they:

- **Static-prop caller** (`:6276-6294`): clears `clearLastGameplayPick()`,
  optionally emits `[GAMEPLAY_PICK v1] miss kind=StaticProp …` diagnostic.
  **No further side effect.** Control returns to the parent
  `updateOldStyle`/`updateAOEStyle` body which continues normal flow.
- **Mech caller** (around `:6425`): "silent on plain miss" comment in
  spec; emits diagnostic only on `MC2_MECH_PICK_DEBUG=1`. **No further
  side effect.**

The parent's normal flow at that point — for the user who Shift+clicked
on terrain — is whatever Shift+LMB normally meant pre-M1.6: additive
unit-selection toggle on the mover at `wPos` (if any), or no-op. The
Shift+LMB gesture does NOT trigger a move order (that's plain RMB or
RMB-release, line 1620 path).

**M3 Option-5 behavior:** `tryGameplayPick` returns `miss` for terrain
Shift+clicks (identical to today's behavior for "background" pixels —
which are not really background on a terrain-covered viewport, but the
absence of a `v_objectId` write means the attachment-2 pixel is the
`glClearBufferuiv(0)` value, which `lookupAtPixel` filters via the
`pixel==0` guard at `RenderWorld.cpp lookupAtPixel`). **No regression.**

## 5. `setSceneDrawBuffers` integration

Per §2.3, terrain draws inside the bind window of `beginScene`. The
five existing call sites of `setSceneDrawBuffers` from M1.5 C1
(`GameOS/gameos/gos_postprocess.cpp:339, 488, 497, 586, 696, 729` —
7 actual calls; the 5-sites doc number includes the C1 helper itself):

| Line | Caller | Mode | `objectIdAttachmentReady` |
|---|---|---|---|
| 339 | `createFBOs` | MainSceneMRT | `sceneObjectIdTex_ != 0` |
| 488 | `beginScene` (terrain-not-rendered branch) | MainSceneMRT | (same) |
| 497 | `beginScene` (terrain-rendered branch) | MainSceneMRT | true |
| 586 | `runScreenShadow` | SingleColor | false |
| 696 | `runGodRays` | SingleColor | false |
| 729 | `runShoreline` | SingleColor | false |

**Terrain draws between 488/497 (MRT armed) and the post-process
single-color rebinds at 586+.** The substrate is already armed when
terrain frags execute. M3 Options 1 or 2 would need *only the shader
edit*, not a new helper site.

If M3 chose Option 5 (recommended), even the shader edit is
unnecessary; terrain frags simply don't write attachment-2, leaving it
at the `glClearBufferuiv(GL_COLOR, 2, 0)` value from frame entry.

## 6. Legacy CPU fallback paths for terrain

There is **no CPU/MLR terrain render path** analogous to the
`mlr_mech_draws` gap that M2.5 had to instrument. Evidence:

- `mclib/terrain.cpp:1083-1145`: the per-quad CPU `draw()` loop is the
  fallback for the un-armed (revert) case ONLY. The default path skips
  it entirely.
- `mclib/quad.cpp::TerrainQuad::draw()` (per the indirect-cmd-gen
  retirement plan `docs/superpowers/plans/2026-05-14-cmd-patch-dispatch-retirement.md`)
  fills `MC_TextureManager` queues that ultimately produce GL submits
  via the GPU-batched path; no MLR involvement.
- Negative grep `"MLR.*terrain"` returns zero `.cpp` hits; `mlrclipper`
  consumers are all gosFX/mech-related (per the F1 unified-projection
  spec).

If M3 ever moved to Option 1, the only "legacy fallback" worth
instrumenting would be: "the user set `MC2_TERRAIN_INDIRECT_OVERLAY=0`
to revert to the per-quad CPU loop." In that mode the quad.cpp draw
path doesn't go through the GPU programs that would carry the
`v_objectId` write, so terrain pixels would still read as 0 →
`lookupAtPixel` returns `invalid()`. The fallback for the fallback is
itself Option 5; no separate counter is needed unless someone
re-enables full per-quad CPU rendering as a default (no such plan).

## 7. Handle range allocation proposal (forward-only; M3 v1 allocates nothing)

Current allocation:

- Static props: index range `[0, ~2641]` (mc2_24 baseline) — well under
  0x10000 by an order of magnitude.
- Mechs: index range `[0x10000, 0x10000 + slot]` with `kMechHandleBase
  = 0x00010000`.

Handle index is 20 bits = `0xFFFFF` = 1,048,575 (per
`RenderCore/Handle.h:34,38`).

If M3 v2 ever ships Option 1 (per-quad):

- **Propose `kTerrainHandleBase = 0x40000` = 262,144.**
- Reserves `0x10000..0x3FFFF` (245,760 slots) for mech expansion above
  the current allocation — gives mechs 4× headroom.
- Leaves `0x40000..0xFFFFF` (786,431 slots) for terrain quads.
- Worst-case map (clipRange-dominated) on `GameVisibleVertices=200` is
  ~196K visible quads but mission-total quads can exceed that for very
  large maps. 786K headroom is conservative.

If M3 v2 ships Option 2 (per-chunk/block):

- Same `kTerrainHandleBase = 0x40000` works trivially (block counts
  are ~16× smaller than quad counts).

Either choice avoids collision with `kMechHandleBase` and leaves a
contiguous post-terrain range `[0x100000..0xFFFFF]` (zero space — the
quad option takes nearly the rest). If a future kind needs space, the
ladder is:

- StaticProp: `[0, 0xFFFF]`
- Mech: `[0x10000, 0x3FFFF]`
- (Future) Terrain: `[0x40000, 0xFFFFF]` (claims rest of 20-bit range)

If both per-quad terrain AND per-pixel VFX/decal kinds need handles, the
20-bit ceiling becomes the real constraint; M3 spec would need to
re-evaluate (Option 2 chunk vs widening Handle).

## 8. Picking semantics options (what does a "terrain pick" mean?)

| Option | Semantic | Identity unit needed | Existing CPU support |
|---|---|---|---|
| A | Inspect-only (mirror M1.6/M2.6) | quad or block | Tile R/C via `worldToTile`, terrain type via `getTerrain(tileR, tileC)`, elevation via `getTerrainElevation(tileR, tileC)` (`terrain.h:266-268`) — fully available CPU-side. |
| B | Coordinate readback (return world XYZ of clicked pixel) | none | Already implemented at `missiongui.cpp:773-789` via inverseProject + `wPos`. **Done.** |
| C | Quad selection / highlight | quad | `selectVertex(tileR, tileC)` (`terrain.h:294`) suggests a CPU-side selection bit exists for editor purposes. Not exposed to gameplay. |
| D | Tile metadata lookup (terrainType, texture, elevation) | quad | All metadata getters exist (`terrain.h:264-268`). Driven by tile R/C, not a Handle. |

**Observation:** Every semantic (A-D) is satisfiable purely from
CPU-side `wPos` → `worldToTile` → metadata getters. The GPU substrate
does not add information; it only adds an alternate identity path
(pixel → Handle) when the CPU-side world-projection inverse is
unwanted (e.g., obstructed by a depth-occluding overlay or particle).
Today the terrain pass writes depth normally, so inverseProject is
already pixel-accurate.

The only semantic the GPU substrate would *uniquely* enable is:
**"the user shift-clicked on a pixel that is RENDERED as terrain, even
though there is a transparent VFX/overlay in front of it depth-wise."**
That edge case is not driven by any current consumer request.

## 9. Open questions for the M3 spec author

These cannot be decided without user input:

1. **Identity unit:** Quad / chunk / coord / none?
   - **Recon recommendation: Option 5 (none) for M3 v1.** Reserve
     `RenderObjectKind::Terrain = 2` in the enum without allocating a
     handle range. Defer Options 1/2 to M3 v2 if/when a consumer
     materializes.
2. **Picking semantic:** Inspect / coord readback / quad-select /
   metadata?
   - **Recon observation:** All 4 are CPU-satisfiable today. The
     GPU substrate adds no new info unless the use case is "pick
     through transparent overlays" (not in any current backlog).
3. **Is there an editor / debug use case that drives the choice?**
   - The codebase has vestiges of editor support (`selectVertex` in
     `terrain.h:294`, the `bool selected` debug bit at `quad.h:87`
     under `_DEBUG`). PROJECT.md not consulted by this recon (out of
     lean-intake scope); if it includes an editor north-star, M3 might
     prefer Option 1 + Semantic C to lay a substrate for that.
   - If no editor goal exists, Option 5 is the strict-minimum ship.
4. **Should terrain water (`gos_terrain_water_mdi.frag`) and overlay
   decals (`terrain_overlay.frag`) be treated as the same kind as base
   terrain, or as separate kinds?**
   - Water has different gameplay semantics (impassable for some unit
     types). Overlay decals (roads/cement) have different semantics
     (move-speed modifiers).
   - **Recon observation:** if Option 5 is chosen, this is moot. If
     Option 1 or 2 is chosen, the natural sub-kind split is:
     `Terrain::Base`, `Terrain::Water`, `Terrain::Decal`, `Terrain::Mine`.
5. **Does the M3 spec want to retire `Terrain::IsGameSelectTerrainPosition`
   in favor of a GPU-handle-driven path?**
   - **Recon recommendation: NO.** The CPU path is correct, fast, and
     used by 6+ call sites. Replacing it would be a much larger
     refactor than M3 should bear.

## 10. File:line citations table (grep-verified at write-time 2026-05-23)

| Symbol / claim | File | Line |
|---|---|---|
| `setSceneDrawBuffers` helper definition | `GameOS/gameos/gos_postprocess.cpp` | 31 |
| `setSceneDrawBuffers` call — createFBOs | `GameOS/gameos/gos_postprocess.cpp` | 339 |
| `setSceneDrawBuffers` call — beginScene (no terrain) | `GameOS/gameos/gos_postprocess.cpp` | 488 |
| `setSceneDrawBuffers` call — beginScene (terrain) | `GameOS/gameos/gos_postprocess.cpp` | 497 |
| `setSceneDrawBuffers` call — runScreenShadow | `GameOS/gameos/gos_postprocess.cpp` | 586 |
| `setSceneDrawBuffers` call — runGodRays | `GameOS/gameos/gos_postprocess.cpp` | 696 |
| `setSceneDrawBuffers` call — runShoreline | `GameOS/gameos/gos_postprocess.cpp` | 729 |
| `gos_terrain.frag` FragColor (loc 0) | `shaders/gos_terrain.frag` | 32 |
| `gos_terrain.frag` GBuffer1 (loc 1) | `shaders/gos_terrain.frag` | 34 |
| `gos_terrain_water_mdi.frag` FragColor (loc 0) | `shaders/gos_terrain_water_mdi.frag` | 24 |
| `gos_terrain_water_mdi.frag` GBuffer1 (loc 1) | `shaders/gos_terrain_water_mdi.frag` | 25 |
| `gos_terrain_mine_static.frag` FragColor | `shaders/gos_terrain_mine_static.frag` | 29 |
| `terrain_overlay.frag` FragColor | `shaders/terrain_overlay.frag` | 18 |
| `Terrain::render` definition | `mclib/terrain.cpp` | 1059 |
| drawPass loop (default-skipped) | `mclib/terrain.cpp` | 1085-1145 |
| numberQuads zero-init at destroy | `mclib/terrain.cpp` | 895-896 |
| `TerrainQuad` class definition | `mclib/quad.h` | 59 |
| `Terrain::worldToTile` inline | `mclib/terrain.h` | 368 |
| `Terrain::worldToTileCell` inline | `mclib/terrain.h` | 387 |
| `Terrain::getTerrain(tileR, tileC)` decl | `mclib/terrain.h` | 266 |
| `Terrain::getTerrainElevation` decl | `mclib/terrain.h` | 268 |
| `Terrain::selectVertex` decl (editor vestige) | `mclib/terrain.h` | 294 |
| `IsGameSelectTerrainPosition` call — LOS | `code/missiongui.cpp` | 784 |
| `IsGameSelectTerrainPosition` call — updateOldStyle ground | `code/missiongui.cpp` | 1414 |
| `IsGameSelectTerrainPosition` call — updateAOEStyle ground | `code/missiongui.cpp` | 1652 |
| `IsGameSelectTerrainPosition` call — RMB-release | `code/missiongui.cpp` | 3660 |
| `tryStaticPropPick` caller wrapper | `code/missiongui.cpp` | 6207 |
| `tryMechPick` caller wrapper | `code/missiongui.cpp` | 6305 |
| `[GAMEPLAY_PICK v1] miss` log path (static-prop) | `code/missiongui.cpp` | 6276-6294 |
| `RenderObjectKind` enum with Future:Terrain=2 comment | `RenderWorld/RenderWorld.h` | 131-135 |
| `Handle::make` 20-bit index encoding | `RenderCore/Handle.h` | 32-36 |
| `Handle::index()` mask 0xFFFFF | `RenderCore/Handle.h` | 38-40 |
| `LookupResult{isValid=false}` documented return | `RenderWorld/RenderWorld.h` | 169-176, 193-199 |
| `static_prop.frag` `v_objectId` pattern (location=2) | `shaders/static_prop.frag` | 71 |
| `static_prop.frag` `MC2_OBJECT_ID_BUFFER` guard | `shaders/static_prop.frag` | 56, 68, 174-181 |
| Visible-window quad count derivation | `docs/superpowers/explorations/2026-04-30-admission-early-guards-recon.md` | 93 |
| `kMechHandleBase = 0x00010000` reference | (CLAUDE.md M2 entry) | n/a |

RECON STATUS: COMPLETE
