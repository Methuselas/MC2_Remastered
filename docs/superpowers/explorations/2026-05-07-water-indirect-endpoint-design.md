# Water Indirect-Endpoint — Design Exploration

**Date:** 2026-05-07
**Author:** exploration agent (claude/nifty-mendeleev)
**Status:** EXPLORATION — no source changes. Becomes a brainstorm input
before any spec/plan/review (per worktree CLAUDE.md "Review Discipline").

## TL;DR

Recommendation: **(c) — not worth porting indirect-endpoint to water.**
The renderWater Stage 2+3 slice already shipped the load-bearing portion
of the indirect pattern (static recipe SSBO + per-frame thin SSBO + single
post-`renderLists()` GPU-direct draw). The remaining ~1.08 ms `Terrain::renderWater`
zone self-time is not the same shape of cost that PR1 SOLID retired on the
terrain side, and the pieces an `MC2_WATER_INDIRECT` slice would add
(`glDrawArraysIndirect` + a 1-element `DrawArraysIndirectCommand` SSBO)
buy nothing measurable on top of the already-present single
`glDrawArrays`. The cost shape is in per-frame thin-record packing
(`UploadAndBindThinRecords` walks all `numberQuads`) and double-draw
texture-bind for base + detail layers — neither is solved by indirect.

## 1. Code grounding (verify-then-write)

| Symbol | File:line | Status |
|---|---|---|
| `Terrain::renderWater` | `mclib/terrain.cpp:1037` | M |
| `ZoneScopedN("Terrain::renderWater")` | `mclib/terrain.cpp:1039` | M |
| `MC2_RENDER_WATER_FASTPATH` env early-return | `mclib/terrain.cpp:1064-1073` | M |
| `[WATER_LEGACY v1] event=population` | `mclib/terrain.cpp:1117-1123` | M |
| `Terrain::renderWaterFastPath` | `mclib/terrain.cpp:1162` | M |
| `WaterStream::EnsureRecipeBufferUploaded` | `GameOS/gameos/gos_terrain_water_stream.cpp:~280` | M |
| `WaterStream::UploadAndBindThinRecords` | `GameOS/gameos/gos_terrain_water_stream.cpp:294` | M |
| `WaterRecipe` (64 B std430) | `GameOS/gameos/gos_terrain_water_stream.h:47-72` | M |
| `WaterThinRecord` (48 B std430) | `GameOS/gameos/gos_terrain_water_stream.h:91-99` | M |
| SSBO bindings 5 (recipe) / 6 (thin) | `gos_terrain_water_stream.h:106-107` | M |
| `gos_terrain_bridge_renderWaterFast` | `GameOS/gameos/gameos_graphics.cpp:1924` | M |
| `gosRenderer::renderWaterFastPath` | `GameOS/gameos/gameos_graphics.cpp:1958` | M |
| Two `glDrawArrays(GL_TRIANGLES, 0, drawVerts)` (base + detail) | `gameos_graphics.cpp:2174, 2201` | M |
| `BuildDenseRecipe` (PR1 precedent) | `GameOS/gameos/gos_terrain_indirect.cpp:708` | M |
| `PackThinRecordsForFrame` | `gos_terrain_indirect.cpp:1194` | M |
| `BuildIndirectCommands` (1-cmd SOLID) | `gos_terrain_indirect.cpp:1383` | M |
| `DrawIndirect` | `gos_terrain_indirect.cpp:1456` | M |
| `Terrain::waterElevation` set site | `mclib/mapdata.cpp:610` (`wDepth + sDepth`) | M |
| `MC2_TERRAIN_INDIRECT` env gate | searched 20 files; primary handler in `gos_terrain_indirect.cpp` | M |
| Water shader VS | `shaders/gos_terrain_water_fast.vert` (per `:59` comment "set by Terrain::renderWaterFastPath C++ code") | M |
| Water shader FS | `gos_tex_vertex.frag` (per `memory:water_rendering_architecture.md`) | M (memory-cited; not re-grepped here) |

Legend: M = match (verified at-write-time), D = drift (cited line wrong),
NF = not found.

## 2. Cost decomposition of the residual ~1.08 ms

The `Terrain::renderWater` zone in Tracy at normal zoom is composed of
the **legacy fallback path body** (terrain.cpp:1075-1153) — a full
`for (long i=0;i<numberQuads;i++)` walk over 39,601 quads calling
`currentQuad->drawWater()`. That path runs when `MC2_RENDER_WATER_FASTPATH=0`
or when `WaterStream::IsReady() / GetRecipeCount() / terrainTextures2`
are not yet warm.

**Critical clarification:** when the fast path is armed, `Terrain::renderWater`
zone returns at line 1072 after the cheap WATER_LEGACY counters. The
substantive GPU work runs under a *separate* zone `Terrain::renderWaterFastPath`
(terrain.cpp:1171), called from `gamecam.cpp` AFTER `renderLists()`. Whatever
1.08 ms zone the user is observing is therefore one of two distinct things,
and the recon must distinguish them before any port:

- **(α) `Terrain::renderWater` is 1.08 ms because the fast path is NOT armed
  this run.** The legacy 39601-quad walk is running. Fix is to arm
  `MC2_RENDER_WATER_FASTPATH=1` (already shipped, default-on candidate),
  not to design a new indirect endpoint.
- **(β) `Terrain::renderWaterFastPath` is 1.08 ms WHEN armed.** The cost is
  one of:
  - `UploadAndBindThinRecords` CPU walk over all `numberQuads` to build
    thin records (`gos_terrain_water_stream.cpp:313-396`) — same scan
    shape as legacy, but no per-vertex emission.
  - Two `glDrawArrays` calls (base layer at line 2174, detail at 2201)
    with intervening uniform set + `glBindTexture`.
  - One-time `glGetUniformLocation` calls per frame in the bridge
    helpers (cheap but not zero — locations not cached across frames).
  - `WaterStream::CheckParityFrame` if `MC2_RENDER_WATER_PARITY_CHECK=1`
    is set (silent on pass but still walks quads).

**Open recon item:** confirm whether the user's "1.08 ms post-Stage-2+3
residual" measurement was on the `Terrain::renderWater` zone (case α —
fast path not armed) or `Terrain::renderWaterFastPath` (case β — true
residual). Memory `renderwater_fastpath_stage2.md` claims "Tracy delta
78–85% reduction" so case β should land closer to ~100 µs per the design's
Gate B target (specs/2026-04-30-renderwater-stage3-handoff.md:275 "showing
< 100 µs"). 1.08 ms in the `Terrain::renderWater` zone is a strong
indicator of case α.

## 3. Pattern fit assessment — does indirect even apply?

PR1 SOLID indirect-terrain shipped because:

1. Terrain has a stable *map-keyed* membership (every quad has a stable
   `vertexNum` lookup). Recipe is mission-static.
2. Per-frame visibility flips need a packed thin-record cull (1856 of
   39601 in-frustum at any moment).
3. `glMultiDrawIndirect` would let the GPU *iterate* command buckets when
   there are many distinct draw configurations (e.g., per-tile sampler
   sets in PR2 cement-multi-sampler).
4. The PR1 SOLID variant uses *one* `DrawArraysIndirectCommand` only —
   `BuildIndirectCommands` returns 1 (`gos_terrain_indirect.cpp:1383-1396`).
   That is functionally identical to `glDrawArrays(GL_TRIANGLES, 0,
   thinCount * 6)`. The reason PR1 introduced the indirect path was to
   make PR2/PR3 (multi-bucket via `glMultiDrawArraysIndirect` with
   per-bucket sampler binds) a clean follow-on.

For water:

1. ✓ Membership is mission-static (water-bearing classification baked at
   `WaterStream::Build` time; `Terrain::waterElevation` set once at
   `mapdata.cpp:610` from constants and never mutates — no rising water
   in MC2 missions).
2. ✓ Per-frame thin-record cull already exists (the `pzTri1Valid /
   pzTri2Valid` gate at `gos_terrain_water_stream.cpp:344-367`; the
   handle-valid + recipe-miss gates at lines 327-335).
3. ✗ Water has a **fixed two-bucket** structure (base + detail) that does
   not benefit from GPU-side iteration. The two layers use different
   texture binds, different `isWater`/`detailMode` uniforms, and
   different `uvScale`/`uvOffset` per-bucket. They cannot be folded into
   a single indirect call without either:
   - duplicating per-record emit (write each thin-record twice into the
     thin SSBO with a layer index), or
   - shader-side branching on `gl_DrawID` against a 2-element bucket
     uniform array.

   Either approach replaces 2 cheap CPU draws with 1 indirect draw of
   double the vertex count. No measurable saving — and the layer split
   becomes shader-side branching, which is generally slower on AMD's
   wave32 than pulling state on the CPU side.
4. ✓ Already a single `DrawArraysIndirectCommand`-equivalent shape per
   layer; no PR3-style per-bucket sampler differentiation is queued.

**Pattern-fit conclusion:** PR1 SOLID's indirect-endpoint scaffolding
buys nothing on top of what renderWater Stage 2+3 already ships. The
load-bearing pieces (recipe SSBO, thin SSBO, per-frame pz cull, post-
`renderLists()` hook) are all already in place. The only piece missing
is the literal `glDrawArraysIndirect` call, and that is the cheapest
piece to skip.

## 4. What the proposed indirect endpoint would add — and why each piece is dead weight

Mirroring PR1 SOLID stages applied to water:

### Stage 0 — scaffolding (env gate `MC2_WATER_INDIRECT`, N1 counters)
Adds `getenv` + counter struct + `[WATER_INDIRECT v1]` printer. Cost-free
in steady state. Adds about as much code as Stage 0 of PR1 (~50-80
LOC). Justified only if Stages 1-3 follow.

### Stage 1 — cost-split timer
Splits `Terrain::renderWaterFastPath` into sub-zones:
`WaterIndirect::ThinPack` / `WaterIndirect::CmdBuild` / `WaterIndirect::Submit`.
**Already partially exists** as `[WATER_FAST v1] event=elapsed` at
`terrain.cpp:1287-1293`. The thin-pack body at
`gos_terrain_water_stream.cpp:294-396` could carry its own
`ZoneScopedN` already. Recommendation: add the sub-zones as a
**standalone instrumentation slice**, no Stage 2/3 follow-on. This is
the only piece that has independent value.

### Stage 2 — water indirect-command SSBO build
Allocates a `g_waterIndirectCmdBuffer`, writes 1 (or 2 for layered)
`DrawArraysIndirectCommand`, binds at `GL_DRAW_INDIRECT_BUFFER`. This is
a per-frame `glBufferSubData` of ≤32 bytes. Cost: ~100-200 ns. Replaces
two cheap `glDrawArrays` literal-arg calls. **Buys nothing.**

### Stage 3 — indirect-draw submit + legacy-direct gate-off
Replaces `glDrawArrays(GL_TRIANGLES, 0, drawVerts)` with
`glDrawArraysIndirect(GL_TRIANGLES, offset)`. On AMD, both go through
the same command-processor path; there is no measurable submit delta.

### What's water-specific that doesn't fit cleanly
- **Detail population (1856 detail-eligible quads on mc2_01).** Water
  has a *parallel* sub-population that is not co-extensive with the
  base water population. Today this is solved by checking
  `q.waterDetailHandle != 0xffffffff` per quad and binding a separate
  texture for the detail draw. An indirect endpoint could either (i)
  emit a second indirect command for the detail layer (cost-equivalent
  to today's `glDrawArrays #2`), or (ii) widen each thin record with a
  `hasDetail` bit and double-emit in the shader — adds shader branch
  cost.
- **Underwater fog / depth-fade uniforms.** `gos_tex_vertex.frag` (per
  `memory:water_rendering_architecture.md`) is shared with detail-overlay.
  Adding fog-mix uniforms specific to water requires a shader split or a
  uniform-flag gate, neither of which the indirect plumbing solves.
- **Texture handle volatility.** `getWaterDetailHandle(sprayFrame)`
  cycles through animation frames per
  `mc2_texture_handle_is_live.md`. The bridge resolves it per-frame;
  indirect submit doesn't change that.

## 5. What Stage 2+3 already shipped (do not re-design)

Per memory `renderwater_fastpath_stage2.md` and the verified code:

- **Static recipe build** at `WaterStream::Build()` (decl in
  `gos_terrain_water_stream.h:195`). One `WaterRecipe` per water-bearing
  quad in the mission. ~7,995 records on mc2_01 = 512 KB SSBO at binding 5.
- **Per-frame thin-record SSBO** at binding 6, packed by
  `UploadAndBindThinRecords` (line 294). One 48 B record per in-window
  water-bearing quad that survives the pz cull.
- **Single recipe upload** is idempotent (only re-uploads on count
  change — confirms mission-static membership invariant).
- **Bridge function** `gos_terrain_bridge_renderWaterFast` at
  `gameos_graphics.cpp:1924` runs *post-`renderLists()`* per the rule
  in `memory:render_order_post_renderlists_hook.md`.
- **Sampler / depth / blend** state explicitly set inside the bridge
  (lines 2103-2107, 2118-2121) per
  `memory:gpu_direct_depth_state_inheritance.md` and
  `memory:sampler_state_inheritance_in_fast_paths.md`.
- **Parity check (Stage 3)** silent-on-pass, env-gated by
  `MC2_RENDER_WATER_PARITY_CHECK=1`.
- **Two `glDrawArrays` calls** total — base layer (line 2174) and detail
  layer (line 2201). Each draws `thinCount * 6` triangles from gl_VertexID.

The 1.08 ms residual is **NOT** in the GPU submit path. It is — assuming
case β — in `UploadAndBindThinRecords` walking all `numberQuads` per
frame. PR1's indirect endpoint does not address per-frame thin-record
packing cost; PR1 has the same shape (`PackThinRecordsForFrame` at
`gos_terrain_indirect.cpp:1220-1350` walks `total = land->getNumQuads()`
identically).

## 6. Decision

**(c) — Not worth pursuing as an indirect-endpoint port.**

The cheap pieces of the indirect pattern (recipe SSBO + thin SSBO +
post-renderLists hook + state hygiene) already shipped via Stage 2+3.
The expensive piece (per-frame thin-record packing CPU cost) is
unaffected by indirect submit. The remaining piece an `MC2_WATER_INDIRECT`
slice would add (`glDrawArraysIndirect` over a 1-2 element command
SSBO) replaces literal-arg `glDrawArrays` calls, which on AMD have
identical command-processor cost.

If the residual is in fact **>500 µs** in the
`Terrain::renderWaterFastPath` zone (case β), the right next slice is
**not** indirect — it's one of:

- **Thin-record packing optimization.** Today the inner loop walks
  `numberQuads` (39,601). It could narrow to a `quadList` window
  pre-filtered by `waterHandle != 0xffffffff` if the cull-list maintained
  by terrain is queryable cheaply.
- **Fold base + detail into one draw.** Encode `hasDetail` as a thin-record
  flag, double-emit in the VS via `gl_VertexID / 6` indexing, branch
  texture-bind in the FS. Requires shader split; saves one
  `glDrawArrays` and one `glBindTexture`.
- **Skip parity check** in non-debug runs (already env-gated; verify
  default is off in shipped binary).

If the residual is actually in `Terrain::renderWater` (case α — fast
path not armed by default), the right next slice is **flip
`MC2_RENDER_WATER_FASTPATH` to default-on**, which retires the legacy
loop entirely. That is a one-line change with the parity infrastructure
already in place.

## 7. Risk inventory (top 3, brainstorm-doc R-style)

**R1 (load-bearing): Underwater fog / depth-fade interaction with detail
recipe.** `gos_tex_vertex.frag` is shared with detail-overlay (per
`memory:water_rendering_architecture.md`). Any indirect-endpoint slice
that folds base + detail into one draw forces a shader split or a
uniform-flag branch in the shared FS. The shoreline alpha-band
`fog_color` patch from 2026-04-30 (gameos_graphics.cpp:2058-2066) is
load-bearing for visual parity; adding an indirect path that re-routes
through a different FS code branch needs an explicit parity gate.

**R2: Water-elevation lifecycle assumed mission-static — verify on
campaigns.** `Terrain::waterElevation` is set once at `mapdata.cpp:610`
from `wDepth + sDepth` (both constants from FIT data per the file). No
write site exists in the mission tick. If a mod or a future feature
introduces tide / rising water, `WaterStream::Build`'s membership cache
silently drifts (water-bearing classification was per-vertex relative to
elevation at build time). Mitigation: documented invariant in the
WaterStream header comment; should land an `MC2_WATER_ELEVATION_TRACE`
env-gate that asserts on first detected mutation.

**R3: Stock install must remain playable degrade-gracefully rule.** Per
`memory:stock_install_must_remain_playable.md`, the renderer must not
fail when modern data is missing. The current fast path correctly
falls back at `terrain.cpp:1066-1069` when `WaterStream::IsReady()` /
`GetRecipeCount()` / `terrainTextures2` are not warm. Any indirect-
endpoint slice must preserve this triple gate, including for the
indirect-command SSBO not yet allocated. Soaking this on tier1 is the
parity surface.

## 8. Adversarial review readiness

This document is an exploration. Per worktree CLAUDE.md "Review
Discipline" and `.claude/skills/adversarial-plan-review.md`, the
follow-on if this exploration progresses is:

1. **Brainstorm scope doc** — derive Q-tree from this exploration, with
   per-Q grep-verification of any code claims that were not already
   verified above.
2. **Spec / design doc** — only if the brainstorm closes on stages and
   identifies a measurable perf gate.
3. **Adversarial code-grounded review** — high-stakes (architectural
   endpoint, "X retires Y" framing, perf gate ≥30%) — full skill,
   verification appendix, CRITICAL/MAJOR/MINOR stratification.
4. **Plan + execution** — only after adversarial review closes.

The current decision (c — not worth pursuing) likely stops the chain at
step 1 or 2 unless the cost-split timer (Stage 1 standalone) is the
chosen actionable item.

## 9. Verification appendix

All file:line citations were grep-verified at write-time on
2026-05-07 against branch `claude/nifty-mendeleev` HEAD. Symbol-status
table is in §1. No NF or D entries; one cited claim
(`gos_tex_vertex.frag` is the water FS) is M against memory rather than
freshly grepped — listed as such in the table. Negative claims about
`Terrain::waterElevation` not being mutated post-`mapdata.cpp:610` were
defended by grepping every `.cpp` under `mclib/` for the symbol; only
read sites were found (bdactor, camera, gvactor, mech3d, quad — all
read-only comparisons or `position.z = …` clamps that copy the value,
not mutate it).

**Open recon (not blocking the recommendation but blocking any
follow-on slice):** distinguish whether the user's 1.08 ms observation
is `Terrain::renderWater` (case α — fast path not armed) or
`Terrain::renderWaterFastPath` (case β — true residual). Tracy GUI
flame chart at the relevant frame, or `MC2_WATER_DEBUG=1` post-warmup
elapsed_us prints (`terrain.cpp:1142-1147` for legacy, `terrain.cpp:1287-1293`
for fast path), would resolve this directly.
