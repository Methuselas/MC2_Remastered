# PR2 Stage 0 recon — item 6: render-order hook sites per population

**Date:** 2026-05-07
**Status:** Recon. Closes brainstorm open recon item #6
([`brainstorms/2026-05-01-detail-overlay-consolidation-scope.md:837-841`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)).
**Scope:** Inventory `mclib/txmmgr.cpp` `renderLists()` Tracy zones; map each
to the legacy / modern data path it consumes; identify the right hook
location for PR2a (detail), PR2b (overlay), PR2c (mine).

> Discipline: every cited symbol grep-verified at write-time per worktree
> CLAUDE.md "Documentation Discipline." Status table at end.

---

## TL;DR

1. **Brainstorm-cited line numbers have drifted.** Today's zones live at
   different lines than the brainstorm cited; updated map below.
2. **Detail's render path has been dead-pixel-output since commit
   `521d83a` (2026-04-16, "Fix terrain underlayer artifact").** The
   `Render.Overlays` zone explicitly resets-and-skips any non-water
   alpha+terrain node at [`mclib/txmmgr.cpp:1818-1822`](../../../mclib/txmmgr.cpp).
   `mcTextureManager->addVertices(terrainDetailHandle, ...)` produces
   nodes with `MC2_ISTERRAIN | MC2_DRAWALPHA` and no water flag, so they
   match the suppress predicate and are dropped. **PR2a is therefore a
   delete slice, not a port** — the M2c inline emit code at
   `quad.cpp:1961-2024` runs every frame producing zero visible pixels.
   No draw bridge, no recipe extension, no texture array, no parity
   test (visual diff is N/A). Recovers CPU cost only. A future
   "tessellation-aware detail layer" feature is independent scope, not
   PR2a's continuation.
3. **Overlay's render path is live** through `gos_DrawTerrainOverlays()`
   at the new `Render.TerrainOverlays` zone. PR2b's draw bridge replaces
   `gos_PushTerrainOverlay` queueing with indirect-draw at the same hook.
4. **Mine's render path is live** through the legacy
   `MC2_ISCRATERS|MC2_ISTERRAIN` masterVertexNodes scan at the
   non-`Render.Overlays` (legacy "Render.Overlays") loop at
   [`mclib/txmmgr.cpp:1937-2001`](../../../mclib/txmmgr.cpp); but the comment block at
   `:1933-1935` indicates cement overlays + decals are now drawn earlier
   via `gos_DrawTerrainOverlays/Decals`. Mine geometry comes from
   `drawMine()` which still flows through `addVertices`. Hook for PR2c
   needs a focused walk before the spec is written.

---

## Zone inventory (current, post-`8b43db1`)

| # | Zone name | First line | Consumer | Population fed by |
|---|---|---|---|---|
| 1 | `Camera.SceneDataUpload` | [`txmmgr.cpp:1400`](../../../mclib/txmmgr.cpp) | scene-uniform UBO upload | (state) |
| 2 | `Render.3DObjects` | [`txmmgr.cpp:1422`](../../../mclib/txmmgr.cpp) | `masterHardwareVertexNodes` (TG_Shapes) | objects, mechs, buildings |
| 3 | `Shadow.StaticAccum` | [`txmmgr.cpp:1530`](../../../mclib/txmmgr.cpp) | terrain extras → shadow ortho FBO | terrain shadow caster |
| 4 | `Shadow.DynPass` | [`txmmgr.cpp:1581`](../../../mclib/txmmgr.cpp) | `g_shadowShapes[]` | per-frame shadow casters |
| 5 | `Render.TerrainSolid` | [`txmmgr.cpp:1617`](../../../mclib/txmmgr.cpp) | `gos_terrain_indirect::DrawIndirect()` if armed; else `TerrainPatchStream::flush()`; else legacy `masterVertexNodes` SOLID loop | terrain SOLID base |
| 6 | `Render.GpuStaticProps` | [`txmmgr.cpp:1743`](../../../mclib/txmmgr.cpp) | `GpuStaticPropRegistry::flush() → gpu_cull::compute_dispatch() → GpuStaticPropBatcher::flush()` | static props (substrate stays OFF per [memory/track_c_substrate_regression.md](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/track_c_substrate_regression.md)) |
| 7 | `Render.TerrainOverlays` | [`txmmgr.cpp:1777`](../../../mclib/txmmgr.cpp) | `gos_DrawTerrainOverlays()` (drains `gos_PushTerrainOverlay` queue) | **overlay (cement transitions, etc.)** |
| 8 | `Render.Decals` | [`txmmgr.cpp:1782`](../../../mclib/txmmgr.cpp) | `gos_DrawDecals()` (drains `gos_PushDecal` queue) | **decals** (separate; not in PR2 scope) |
| 9 | `Render.Overlays` | [`txmmgr.cpp:1789`](../../../mclib/txmmgr.cpp) | legacy `masterVertexNodes` scan; gated to **water-only** by predicate at `:1818-1822` | water; **detail is filtered out here** |
| 10 | `Render.NoUnderlayer` | [`txmmgr.cpp:1873`](../../../mclib/txmmgr.cpp) | legacy `masterVertexNodes` scan; `MC2_GPUOVERLAY` SOLID alpha-terrain | sebi-era no-underlayer overlays |
| 11 | (unnamed shadow loop) | [`txmmgr.cpp:1959-2001`](../../../mclib/txmmgr.cpp) | legacy `MC2_ISSHADOWS\|MC2_DRAWALPHA` scan | legacy gosFX-style shadow plates |
| 12 | (unnamed alpha non-terrain loop) | [`txmmgr.cpp:2011+`](../../../mclib/txmmgr.cpp) | legacy `!MC2_ISTERRAIN&!MC2_ISSHADOWS&!MC2_ISCOMPASS&!MC2_ISCRATERS&MC2_DRAWALPHA` scan | non-terrain alpha objects |

(Brainstorm cited zone hooks at lines 1297/1414/1430/1437/1442; current
zone hooks land at 1617/1743/1777/1782/1789. Drift is +320 lines, caused
by lighting/shadow refactors landed in `1f4ef44` and `8b43db1` 2026-05-07.)

---

## Per-population hook decision

### PR2a — detail

**Today's path:** `quad.cpp:2000-2021` calls
`mcTextureManager->addVertices(terrainDetailHandle, tri,
MC2_ISTERRAIN | MC2_DRAWALPHA)`. Nodes accumulate in `masterVertexNodes`.
At zone #9 `Render.Overlays`, the predicate at
[`txmmgr.cpp:1809-1813`](../../../mclib/txmmgr.cpp)
admits `MC2_ISTERRAIN & MC2_DRAWALPHA & !MC2_ISCRATERS & alphatest-match`
nodes — detail nodes match. But [`:1818-1822`](../../../mclib/txmmgr.cpp)
unconditionally resets-and-`continue`s for any node lacking
`MC2_ISWATER` or `MC2_ISWATERDETAIL`. Detail nodes have neither.
Verified: no path in the worktree ORs `MC2_ISWATER` into a
detail-textured node (grep across `mclib/` + `GameOS/gameos/` for
`flags|=.*MC2_ISWATER`/`MC2_ISWATER\\b` returns 0 producer matches).

**History:** suppress block introduced in `521d83a` 2026-04-16
(`Fix terrain underlayer artifact and clean up shader path`). The commit
message and the in-line comment at `:1815-1817` confirm intent: the
flat-plane detail layer showed through displaced tessellated terrain as
a dark striped under-pattern, and the design choice was to drop the
legacy detail draw rather than try to elevate it.

**Spec implication: PR2a is a delete slice.**

- M2c inline detail emit at `quad.cpp:1961-2024` produces zero pixels
  today and has since 2026-04-16.
- PR2a's deliverable: delete the dead M2c emit code. Recovers the
  per-frame CPU cost (clampUVs, corner build, addVertices push). No
  draw bridge, no recipe extension, no texture array, no parity test.
- Brainstorm Q4 detail-recipe-extension answer is moot.
- A future "tessellation-aware detail layer" — re-introducing detail
  rendering for displaced terrain — is independent scope and should
  be brainstormed separately on its own merits, NOT bolted onto
  PR2a as a "while we're here."

### PR2b — overlay

**Today's path:** M2d inline emit at `quad.cpp:2035-2083` calls
`gos_PushTerrainOverlay(w, overlayTexId)`. Producer is implemented at
[`gameos_graphics.cpp:6104`](../../../GameOS/gameos/gameos_graphics.cpp) (re-grep'd 2026-05-08; was :5892, drifted +212 lines).
Drain: `gos_DrawTerrainOverlays()` at `gameos_graphics.cpp:5900`,
called from zone #7 `Render.TerrainOverlays` at
[`txmmgr.cpp:1779`](../../../mclib/txmmgr.cpp). Path is live and pixels
are visible.

**Spec implication:**

- PR2b's draw bridge **replaces** the `gos_PushTerrainOverlay` queue
  with an indirect-draw + parallel `OverlayThinRecordSSBO`. Hooks at
  the same zone #7 location.
- Legacy gate-off: same zone, swap `gos_DrawTerrainOverlays()` for
  `gos_terrain_indirect::DrawOverlayIndirect()` (or wrap with the
  arming check, mirroring the SOLID arming pattern at
  [`txmmgr.cpp:1649-1659`](../../../mclib/txmmgr.cpp)).
- Parity gate has visible signal — both byte-compare on records AND
  pixel canary on cement-transition tiles.

**Recommended hook:** zone #7 `Render.TerrainOverlays` —
`gos_DrawTerrainOverlays()` body conditional on
`gos_terrain_indirect::IsFrameOverlayArmed()`, mirroring the SOLID
pattern.

### PR2c — mine

**Today's path:** `enqueueTerrainMineState()` at
[`quad.cpp:282,290`](../../../mclib/quad.cpp) calls `addTriangleBulk` to
reserve slots. `drawMine()` at [`quad.cpp:4246+`](../../../mclib/quad.cpp)
emits geometry via `addVertices(blownTextureHandle/mineTextureHandle,
gVertex/sVertex, MC2_DRAWALPHA)` (note: NOT `MC2_ISTERRAIN`).

**Mine emit flag check:** `MC2_DRAWALPHA` only; no `MC2_ISTERRAIN`. So
mine nodes are filtered IN by zone #12 (the legacy non-terrain alpha
loop at [`txmmgr.cpp:2017-2023`](../../../mclib/txmmgr.cpp), predicate
`!MC2_ISTERRAIN & !MC2_ISSHADOWS & !MC2_ISCOMPASS & !MC2_ISCRATERS &
MC2_DRAWALPHA`). This is the LIVE drain.

> Caveat: that loop predicate also requires `!MC2_ISCRATERS`. The
> `enqueueTerrainMineState` flag set is `MC2_DRAWALPHA` only — verified
> at `quad.cpp:282,290`. So mine emits pass the filter. (Confirmed: no
> ORs of `MC2_ISCRATERS` into mine textures grep'd.)

**Spec implication:**

- PR2c's draw bridge replaces the `addTriangleBulk` + `addVertices`
  reservation/fill split with a parallel `MineCellRecipeSSBO` +
  per-cell thin record + indirect draw. Hooks at zone #12, but
  zone #12 today is unnamed — the PR2c draw should be promoted to a
  named zone (`Render.TerrainMines`) for Tracy clarity.
- Parity gate visible only on mc2_24 in tier1 (per brainstorm Q3
  detail-before-overlay-before-mine sequencing rationale).

**PR2c reframed 2026-05-08: static-bake, NOT per-frame indirect-draw.**

User direction 2026-05-08: mines are sparse (2-3 missions max, small
overlay each), state changes only on gameplay events, and *really
only need to be drawn once* — not iterated per frame. Legacy
`enqueueTerrainMineState` + `drawMine` cost ~157 µs/frame
unconditionally (even on mine-free missions). The brainstorm's
per-frame indirect-draw architecture was over-engineered.

**Replaced by:** `2026-05-08-pr2c-mine-static-bake-design.md`. Static
VBO built at mission init, invalidated only on `MissionMap::setMine`
chokepoint. Per-frame work: one `glDrawArrays` (or zero if no mines).

The audit-scope doc previously here (`2026-05-08-pr2c-mine-zone-audit-scope.md`)
is **deleted as obsolete**. The render-order placement question
collapses under static-bake: recommended placement is between
`Render.TerrainOverlays` and `Render.Decals`, validated at
spec-execution time by visual canary on a chain-explosion test.

The mine-bearing mission identity (mc2_24 was user-corrected as
incorrect) is no longer a blocking prerequisite — it becomes a
Stage 0c output of the spec via cost-split counter telemetry.

The mine emit's flag set (`MC2_DRAWALPHA` only) routes it through the
non-terrain alpha drain at zone #12. This may be load-bearing for
blend ordering: mines may need to draw alongside non-terrain alpha
objects so explosions/projectiles composite correctly against mine
sprites, OR may need to draw earlier (as part of terrain) so decals
paint over them. Cannot be answered by reading the code — requires a
screen-recording diff on mc2_24 (the only mine-bearing tier1 mission).

**Audit recipe (block PR2c spec until complete):**
1. Capture mc2_24 with mines visible at current zone-12 placement
   (legacy non-terrain alpha drain, untouched).
2. Capture mc2_24 with a candidate `Render.TerrainMines` zone placed
   between `Render.TerrainOverlays` (zone #7) and `Render.Decals`
   (zone #8). Implement as a pixel-diff prototype, not as part of PR2c.
3. If outputs match: free to lock the new zone in PR2c spec.
4. If outputs diverge: PR2c port keeps mines in non-terrain alpha
   drain; the indirect-mine draw must hook there, not in a new
   `Render.Terrain*` zone.

The original layering predates `gos_DrawTerrainOverlays` and the
`Render.TerrainOverlays`/`Render.Decals` split. Mines vs cement vs
crater compositing has never been re-evaluated under the new render
order; the audit closes that question.

---

## Cross-population render-order summary

Today (top-to-bottom = early-to-late in frame):

```
Render.3DObjects                              [objects, mechs, buildings]
Shadow.StaticAccum / Shadow.DynPass           [shadow FBOs]
Render.TerrainSolid    ← PR1 SOLID hook       [terrain base + cement-atlas]
Render.GpuStaticProps                         [static props (sub-OFF)]
Render.TerrainOverlays ← PR2b hook            [cement-transition overlays]
Render.Decals                                 [crater decals]
Render.Overlays        (water-only after the suppress)  [water]
Render.NoUnderlayer                           [GPUOVERLAY layer]
(unnamed shadow loop)                         [legacy gosFX shadow plates]
(unnamed non-terrain alpha)                   [legacy non-terrain alpha; mines drain here today]
```

PR2 target placement:

```
Render.TerrainSolid     ← PR1 SOLID (no change)
                          (PR2a deletes dead M2c queue path; no new zone)
Render.GpuStaticProps
Render.TerrainOverlays  ← PR2b hook (indirect overlay)
Render.TerrainMines     ← PR2c hook (static-bake; one glDrawArrays/frame)
Render.Decals
Render.Overlays         ← water-only (unchanged)
...
```

---

## Open follow-ups

1. **PR2c is blocked on a visual-order audit on mc2_24.** Cannot lock
   the mine zone placement in spec until the screen-recording diff
   between current zone-12 (non-terrain alpha drain) and a candidate
   `Render.TerrainMines` placement is captured. See "PR2c — mine"
   section above for the audit recipe.
2. **Decal scope confirmation** — `Render.Decals` is named separately
   from `Render.TerrainOverlays` and is **not** in the brainstorm's
   PR2 scope. Confirm with user that decals stay legacy through
   PR2a/b/c and revisit only if PR2 timing makes a unified pass
   economical.
3. **Brainstorm line-number drift** — multiple spec-doc citations to
   the brainstorm reference `txmmgr.cpp:1297-1442` line range. Spec
   docs must re-grep these line numbers at write-time per discipline
   rule; this exploration's table above is the current truth.
4. **Tessellation-aware detail layer** (separate scope, NOT PR2a) —
   if/when the project decides to re-introduce detail rendering for
   displaced terrain, brainstorm it as its own feature with its own
   Q-set. Don't conflate with PR2a's CPU-cost retirement.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `Render.3DObjects` zone | [`mclib/txmmgr.cpp:1422`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.3DObjects")` | M |
| 2 | `Render.TerrainSolid` zone (NOT 1297 as brainstorm cited) | [`mclib/txmmgr.cpp:1617`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.TerrainSolid")` | D (brainstorm cited :1297; actual :1617) |
| 3 | `Render.GpuStaticProps` zone (NOT 1414) | [`mclib/txmmgr.cpp:1743`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.GpuStaticProps")` | D (brainstorm cited :1414; actual :1743) |
| 4 | `Render.TerrainOverlays` zone (NOT 1430) | [`mclib/txmmgr.cpp:1777`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.TerrainOverlays")` | D (brainstorm cited :1430; actual :1777) |
| 5 | `Render.Decals` zone (NOT 1437) | [`mclib/txmmgr.cpp:1782`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.Decals")` | D (brainstorm cited :1437; actual :1782) |
| 6 | `Render.Overlays` zone (NOT 1442) | [`mclib/txmmgr.cpp:1789`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.Overlays")` | D (brainstorm cited :1442; actual :1789) |
| 7 | Detail-suppress predicate at `Render.Overlays` | [`mclib/txmmgr.cpp:1818-1822`](../../../mclib/txmmgr.cpp) — `if (!ISWATER && !ISWATERDETAIL) { currentVertex=vertices; continue; }` | M |
| 8 | Suppress block introduced in `521d83a` | `git log -S "dark striped under-pattern" -- mclib/txmmgr.cpp` returns single match `521d83a 2026-04-16` | M |
| 9 | M2c detail emit flags = `MC2_ISTERRAIN \| MC2_DRAWALPHA` (no water) | [`mclib/quad.cpp:2001`](../../../mclib/quad.cpp), `:2007`, `:2015`, `:2021` | M |
| 10 | M2d overlay emit calls `gos_PushTerrainOverlay` (4 LIVE sites) | [`mclib/quad.cpp:2056`](../../../mclib/quad.cpp), `:2063`, `:2072`, `:2079` (M2d block). NOTE: there are also 4 dead Shape-C fallback sites at `:2190, :2333, :2603, :2744` (per `m2_thin_record_cpu_reduction_results.md` "Legacy quads/frame: 0"); 8 sites total in `quad.cpp`, 4 active. PR2b retires the live 4. | M |
| 11 | `gos_PushTerrainOverlay` definition | [`gameos_graphics.cpp:6104`](../../../GameOS/gameos/gameos_graphics.cpp) (re-grep'd 2026-05-08; was :5892, drifted +212 lines) | M |
| 12 | `gos_DrawTerrainOverlays()` definition | [`gameos_graphics.cpp:6112`](../../../GameOS/gameos/gameos_graphics.cpp) (re-grep'd 2026-05-08; was :5900, drifted +212 lines) | M |
| 13 | Mine emit flags = `MC2_DRAWALPHA` only | [`mclib/quad.cpp:282`](../../../mclib/quad.cpp), `:290` (`addTriangleBulk(..., MC2_DRAWALPHA, 2)`) | M |
| 14 | `drawMine()` emit sites (re-grep'd 2026-05-08) | `void TerrainQuad::drawMine (void)` defined at [`mclib/quad.cpp:4240`](../../../mclib/quad.cpp); 4 emit sites at [`:4373, :4374, :4378, :4379`](../../../mclib/quad.cpp) (`addVertices(blown/mineTextureHandle, gVertex/sVertex, MC2_DRAWALPHA)`). Brainstorm cited `:4379, :4380, :4384, :4385` (drift -1 to -6) | M (re-grep'd; brainstorm cite stale by ~6 lines) |
| 15 | No path ORs `MC2_ISWATER` into a detail-textured node | grep `flags\|=.*MC2_ISWATER` / `MC2_ISWATER\\b` across `mclib/` and `GameOS/gameos/` returns no producer matches; only consumer at `txmmgr.cpp:1818,1826` | M (negative claim defended via opposite-direction grep per worktree CLAUDE.md "Negative claims need opposite-direction grep") |
| 16 | Suppress comment text matches commit message intent | comment at [`mclib/txmmgr.cpp:1815-1817`](../../../mclib/txmmgr.cpp), commit `521d83a` 2026-04-16 message: "Fix terrain underlayer artifact" | M |
| 17 | PR1 SOLID arming pattern (precedent for PR2b/PR2c arming) | [`mclib/txmmgr.cpp:1649-1659`](../../../mclib/txmmgr.cpp) — `gos_terrain_indirect::IsFrameSolidArmed()` branch | M |
| 18 | `Render.NoUnderlayer` zone | [`mclib/txmmgr.cpp:1873`](../../../mclib/txmmgr.cpp) — `ZoneScopedN("Render.NoUnderlayer")` | M |
| 19 | Legacy non-terrain alpha loop (mine drain) | [`mclib/txmmgr.cpp:2017-2023`](../../../mclib/txmmgr.cpp) predicate | M |

**Status summary:** 19 entries; 12 M; 6 D (line-number drift, not
behavioral); 1 NF (deferred to spec time, brainstorm cite trusted as
2026-05-01 baseline).

---

## Decision input for spec session

PR2a is a **delete slice**:

- Spec stage structure: Stage 0a scaffolding (env gate +
  `m2c_detail_emit_quads` counter to confirm zero post-delete) +
  Stage 1a delete the M2c emit block at quad.cpp:1961-2024.
- No recipe extension. No draw bridge. No texture array. No parity
  test (visual diff is N/A — zero pixels before, zero pixels after).
- Brainstorm Q4 detail-recipe-extension answer is moot.
- Brainstorm Q6 hybrid-SSBO topology unchanged for overlay/mine.

PR2b (overlay) proceeds per brainstorm Q4/Q6/Q7, simplified per recon
item 4 (UVs are constants).

PR2c (mine) is now scoped as a static-bake spec per
`2026-05-08-pr2c-mine-static-bake-design.md` (replaces the per-frame
indirect-draw framing the brainstorm assumed). 3-stage spec, target
~157 µs/frame retirement.
