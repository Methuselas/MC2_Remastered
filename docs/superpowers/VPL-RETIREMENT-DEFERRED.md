# VPL Retirement - Consolidated Deferred Follow-ups

The VertexProjectLoop (VPL) retirement is complete end-to-end (Steps 1-9 +
overlay-pz precursor + parity-infra deletion + legacy-lighting both-env
retirement, plan `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`,
landed through HEAD `96642cc`). This file consolidates every post-retirement
follow-up that was deliberately deferred so nothing is lost to the closeout.

Each item: what + why-deferred + where-documented. All file:line anchors
grep-verified at HEAD `96642cc` (2026-05-15). Symbols are authoritative;
line numbers drift - re-grep before acting on any anchor.

None of these are blockers. The retirement shipped without them by design.

---

## 1. SSBO `GpuTerrainVertexInput.hazeFactor` field removal

**What:** Remove the now-dead `float hazeFactor;` field from the
`GpuTerrainVertexInput` std430 struct - lockstep across GLSL + C++ + the
`static_assert(sizeof==32)`, as ONE commit per
`memory/cpp_glsl_ubo_struct_lockstep.md`.

**Why deferred:** Step 7 used keep-field-stop-reading (smallest blast
radius). The populate write is neutralized to `0.0f` at
`GameOS/gameos/gos_terrain_lighting.cpp:593`
(`vi.hazeFactor = 0.0f;`); the field is retained in both
`GameOS/gameos/gos_terrain_lighting.h:38` (`float hazeFactor;`, commented
`DEAD post-Step-7`) and `shaders/include/terrain_lighting_shared.hglsl:10`
(`struct GpuTerrainVertexInput { vec2 xy; float elevation; float hazeFactor; ... }`,
commented dead at `:7`). The field is 4 padded bytes under `alignas(16)`;
`static_assert(sizeof(GpuTerrainVertexInput) == 32, ...)` at
`gos_terrain_lighting.h:46` holds whether the field is present or removed,
so removal is stride-neutral. Deferred to a dedicated cleanup after the
Step 8c soak so the lockstep edit is its own auditable commit.

**Documented:** plan v3.3 lockstep paragraph (plan `:351`), plan deferred
list (plan `:576`).

## 2. `.codex_tmp_isolate/*` scratch-file removal

**DONE 2026-05-16 (commit `521e092`).** `git rm` of all three files
(7387 lines), isolated hygiene commit.

**What:** `git rm` the three tracked scratch files
`.codex_tmp_isolate/quad_desired.cpp`, `.codex_tmp_isolate/quad_desired.patch`,
`.codex_tmp_isolate/quad_head.cpp` (confirmed tracked via `git ls-files` at
HEAD `96642cc`).

**Why deferred:** Non-build orphans (working scratch from the overlay-pz
redesign) that pollute repo-wide greps for `quad`/`projectForTerrainAdmission`
with stale duplicate hits. Removing them is a trivial hygiene commit kept
out of the retirement landing commits to keep those diffs scoped to the
architectural change.

**Documented:** this tracker (no prior plan anchor; surfaced during the
Step 10 grep-drift audit).

## 3. `quad.cpp` fastpath probe-placement pattern (recurring)

**What:** The `MC2_M2D_PZ_PARITY` probe in `mclib/quad.cpp` went silent
twice during the overlay-pz redesign because the probe was placed where
its comparison input was vacuous (the v1 unconditional-reprojection
regression, then a scoping mismatch). The probe-placement *pattern* needs
a fix-or-retire decision, not the probe itself.

**Why deferred:** Bit-identity of the production overlay-pz gate
(`mclib/quad.cpp:2159-2189`: `vertices[c]->clipInfo == 0` sentinel +
on-site `eye->projectForTerrainAdmission(ov3D, osp)` at `:2176`, NEVER
reading `vertices[c]->pz`) is proven BY CONSTRUCTION in
`docs/superpowers/reviews/2026-05-15-overlay-pz-v2-bit-identity-proof.md`.
The probe (`mclib/quad.cpp:2191-2247`, the only `vertices[c]->pz` read at
`:2213`, probe-only local) is demoted/belt-and-suspenders, so its
silent-twice history is non-blocking. The recurring placement bug pattern
is a methodology cleanup, not a correctness gap.

**Documented:** `docs/superpowers/reviews/2026-05-15-overlay-pz-scoped-redesign-rereview.md`
(Finding 1, Finding 5); bit-identity proof
`docs/superpowers/reviews/2026-05-15-overlay-pz-v2-bit-identity-proof.md`.

## 4. `[RING_MVP_DELTA v1]` FNV-compute residual (Step 9 leftover)

**DONE 2026-05-16 (commit `c6b2888`).** Whole probe-8 block (FNV
compute AND the per-frame `glGetBufferSubData` on the indirect cmd
buffer) wrapped in `static const bool s_ringMvpProbe = getenv(
"MC2_RING_TRACE")` -- same gate as the Step 9 writers. No behavior
change in the default config (gate unset). NOTE: compile/smoke NOT run
-- the worktree carries unrelated uncommitted shadow WIP in the same
file; validate on the next build of that WIP.

**What:** Gate or remove the per-frame FNV fingerprint compute at
`GameOS/gameos/gameos_graphics.cpp:2683-2724`. It computes `drawFp`
(FNV over 12 floats) and reads `dispatchFp`/`dispatchFrame` EVERY frame,
UNGATED - unlike the Step 9 writers in
`GameOS/gameos/gos_terrain_indirect.cpp:1651` which are guarded by
`g_envRingTrace` (`MC2_RING_TRACE`, `gos_terrain_indirect.cpp:1473`).

**Why deferred:** Step 9 demoted the Fix A path (the per-slot MVP snapshot
that fed `terrainOverrideThinMVP`) behind `MC2_RING_TRACE=1` (default-off);
`terrainOverrideThinMVP`'s cached uniform loc is `-1` so the override is
inert regardless. The FNV residual is inert (the only effect is the
`fprintf` inside `if (drawFp != dispatchFp)` at `:2696`, and the comparison
itself has no load-bearing consumer post-Fix-B). Not load-bearing, not a
regression - just a per-frame compute that should be folded behind the
same env gate as the rest of the Step 9 demotion. Cleanup, not a blocker.

**Documented:** plan Step 9 / Fix A demote (plan `:545`); this tracker.

## 5. Tracy `~475 µs` CPU-recovery measurement (the plan's perf gate)

**What:** Quantify the actual CPU recovery from the VPL retirement with a
Tracy capture (Wolfman-zoom worst-case, before/after).

**Why deferred:** The `vertexProjectLoop` Tracy zone is confirmed GONE
(zero `ZoneScopedN` matches for any VPL-body zone in `mclib/terrain.cpp`
at HEAD; the surviving zone is `ZoneScopedN("Terrain::geometry slimReduce")`
at `terrain.cpp:1466`). Because the zone no longer exists, the precise
CPU delta is unmeasured. The plan's headline `~475 µs` figure is the
plan's PRE-retirement estimate of the loop cost, not a post-retirement
measurement. Steady-state fps is flat (~141) - this is the
RE-HOME-NOT-ELIMINATE reality: the per-vertex `projectForTerrainAdmission`
projection still runs in the slim loop (`terrain.cpp:1544`); the loop was
not eliminated, only its dead per-vertex `px/py/pz/pw` writes and the
duplicate cull/reduction passes were. Flat fps is NOT a regression; the
actual win is the deleted dead writes + the collapsed double-iteration,
which is sub-frame and needs a Tracy capture to isolate.

**Documented:** plan Step 8c verification (plan `:554`,
"Wolfman zoom CPU time drops by the measured loop cost (~475 µs)").

**Update 2026-05-15 (commit `6c9d4b5`):** user Tracy capture quantified
`slimReduce` at mean 662 µs / median 638 µs / p99 1.05 ms. Root cause of
the heaviness: the slim loop called `projectForTerrainAdmission`
UNCONDITIONALLY per vertex, where pre-8c production projected only
`onScreen` vertices. Restored the pre-8c gate (project iff `onScreenR ||
!clipUsesOnScreen`); cull superset bit-identical by construction (cull
write textually unchanged, `clipR` identical). This removes the
slim-loop-introduced over-projection. The headline `~475 µs` before/after
A/B is still unmeasured (VPL zone deleted with the body); user should
re-capture the `slimReduce` zone post-`6c9d4b5` to see the new cost.

## 6. Step 3 interactive picking UAT (honest gap)

**What:** Manual user-driven UAT of cursor tracking, mech click-select,
marquee drag-select, build-menu placement, salvage placement, and tacmap
F-key viewport - at `mc2_01`, `mc2_10` (substrate-heavy), `mc2_17`
(water-heavy).

**Why deferred:** Step 3 re-homed picking onto its own CPU camera-frustum
x quad-AABB primitive (`mclib/camera.cpp:593` `Camera` member,
`mclib/camera.cpp:741` recursion-free tile selection). Bit-identity +
the frustum self-test passed, but passive smoke CANNOT open the tacmap or
drive the cursor/marquee - the runner has no input injection. This is an
honest interactive-coverage gap, not a known defect. Requires a
user-driven smoke session.

**Documented:** plan Step 3 per-step gate (plan `:550`); plan Step 6
tacmap note (plan `:552`, "passive smoke cannot open the tacmap (honest
gap)").

## 7. Overlay-decal GPU port (deferred sibling slice)

**What:** The dedicated typed world-space overlay/decal batch path that
the render contract's Bucket A3 / D2 / Priority-2 describe as the target
end state for alpha-cement / craters / footprints / terrain decals.

**Why deferred:** A separate slice, not part of VPL retirement. VPL
retirement only re-homed the overlay-pz VISIBILITY GATE
(`mclib/quad.cpp:2159-2189`) off `vertices[c]->pz`; the overlay still
submits through the M2d `gos_PushTerrainOverlay` decal producer. The full
GPU port is the deferred sibling.

**Documented:** stub at
`docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`
(confirmed present at HEAD).

## 8. Dedicated-water-path edge-clamp

**What:** Add the `outsidePlayArea` world-edge `hazeFactor=1.0` clamp to
the dedicated water draw path (`shaders/gos_terrain_water_mdi.frag` +
`shaders/gpu_driven_water.comp`) IF that path bypasses the lighting
compute's `fogRGB` output.

**Why deferred:** Step 7's inline-worldPos `outsidePlayArea` clamp fixed
out-of-bounds fog only for water that consumes the lighting compute's
`fogRGB` (terrain-lit water). Whether the dedicated water draw path
consumes that output is UNVERIFIED - a post-Step-7 slice must grep-confirm
the data flow before deciding if the clamp is needed there too. Tracked
follow-up, not a Step 7 blocker.

**Documented:** plan deferred list (plan `:574`, v3.3,
`mc2-shader-expert`).

## 9. `GetApproximateLength` -> `length()` precision under non-stock fog

**What:** A characterized ~10% per-vertex distance delta between VPL's old
CPU `distanceToEye` (`GetApproximateLength`, octagonal approx - the slim
loop preserves this exactly at `mclib/terrain.cpp:1484`
`objectCenter.GetApproximateLength()`) and the inline shader's exact
`length()` (`shaders/gos_terrain_lighting.comp:302`).

**Why deferred:** Fully MASKED in the stock config: stock
`Camera::MinHazeDistance == MaxClipDistance`, so the fog-ramp branch is
never taken and `hazeFactor` is binary 0.0/1.0 (an approx-vs-exact delta
cannot change a binary clamp). It becomes a visible fog-band shift only
the moment a mission/mod ships `MinHazeDistance != MaxClipDistance` (a
live ramp). NOT a regression for stock; tracked so a future non-default-fog
author recognizes the shift as this characterized, accepted re-derivation
rather than a new defect.

**Documented:** plan deferred list (plan `:575`, v3.4); cite
`memory/parity_probe_100pct_can_be_correct_redesign_report.md`.

## 10. Zoom-only terrain/decal + terrain/water z-fighting (NOT VPL fallout)

**What:** Z-fighting between terrain and decals, and terrain and water,
ONLY when the camera zooms (pan/rotate clean). Reported 2026-05-15.

**Why deferred / why NOT a VPL regression:** Structural and pre-existing.
Two independent causes (terrain-indirect-expert + render-expert verified):
(a) THREE conflicting `#define TERRAIN_DEPTH_FUDGE` translation-unit-local
values compiled simultaneously - `quad.cpp:1997` `0.002f`, `tgl.cpp:2868`
`0.000f` (stale `.codex_tmp_isolate` copy `0.001f`, not compiled). A
constant added to post-divide NDC z is the textbook zoom-failure: NDC
depth is nonlinear in camera distance, so a constant bias tuned at default
zoom no longer separates surfaces at other zooms; pan/rotate hold camera
distance fixed so it stays hidden. (b) Water depth `wz`
(`gos_terrain_water_stream.cpp:475`) is projected from `ourCos +
Terrain::waterElevation` (animated wave plane) while terrain `pz` projects
true heightfield `elevation` - two different world-Z through the same
camera diverge nonlinearly with zoom. 250ab4a/6c9d4b5 did not introduce
this; `MC2_TERRAIN_INDIRECT=0` is predicted NOT to clear it (structural,
not indirect-pipeline). **Fix direction:** unify `TERRAIN_DEPTH_FUDGE` to
one header constant + apply decal/water bias as distance-proportional /
`glPolygonOffset`-equivalent rather than a constant NDC offset. Own slice
(has a design fork: polygon-offset vs distance-scaled bias).

**SCOPE UPDATE 2026-05-16 (user first-hand visual, supersedes the
terrain-indirect advisor's PARTIAL/water-seam recommendation):** the
advisor predicted a residual ANIMATED shoreline shimmer (water `wz`
from `ourCos+waterElevation` vs static terrain `elevation`) requiring a
separate "water-depth-projection-unification" seam. User observed the
shoreline z-fight DOES NOT MOVE in-mission at all -> the animated
component is imperceptible/not the defect. Per CLAUDE.md (user visual
outranks grep hypothesis): the **water-depth-projection-unification
seam is DROPPED** (solves a non-problem). The shoreline is a static
mistuned constant -- the undocumented `WATER_DEPTH_FUDGE =
TERRAIN_DEPTH_FUDGE + 0.0005f` (`mclib/quad.cpp:2006`) "is just off"
(user). #10/Seam 2 -- header-unify the macro family (only
`quad.cpp:1997` 0.002f is LIVE; `tgl.cpp:2868` 0.000f is a DEAD define
no consumer; `.codex_tmp_isolate` copies git-tracked = item 2) +
carry/CORRECT the `WATER = TERRAIN + delta` relationship + the decided
vertex-stage distance-proportional clip-z bias -- is now the COMPLETE
shoreline fix, not partial. No water seam follow-up.

**STATE 2026-05-16 (constant approach EXHAUSTED -> distance-proportional
is now the active fix; user decision).** Sequence of record:
(1) Single-constant unification (water all 0.003) shipped, REGRESSED
map edges -- TES low-LOD tiles broke through water (the 89d7c4f
over-bias failure mode). (2) Corrected to a TWO-REGIME constant interim
(Deploy A, in v0.4): `WATER_DEPTH_FUDGE_FAST=0.003`
(gos_terrain_water_fast.vert/_mdi.vert) vs
`WATER_DEPTH_FUDGE_RASTER=0.0025` (quad.cpp CPU raster +
gos_terrain_mask_water.vert); `TERRAIN_DEPTH_FUDGE=0.002` all terrain;
genuine desync (mask_water's false-peer comment) fixed; dead defines
`tgl.cpp:2868` + `gos_terrain_mask_dispatch.cpp:44` deleted;
single-source header `mclib/terrain_depth_bias.h` +
`shaders/include/terrain_depth_bias.hglsl` (lockstep) is the SEAM.
Two adversarial reviews PROCEED/PROCEED-WITH-CHANGES. (3) git history
89d7c4f-vs-6ff6c5c + the regression proved NO constant satisfies both
regimes at all zooms; user decided to retire the constant and implement
the DEFERRED distance-proportional clip-z bias as the PERMANENT fix.
Shader-expert spec: bias = `B * rhw` (clip-space constant / clip.w)
makes the NDC offset scale ~`1/z_eye`, flattening the zoom dependence
to first order and SUBSUMING the FAST/RASTER split into one
`TERRAIN_CLIP_BIAS` + `WATER_CLIP_DELTA` (header collapses; FAST/RASTER
retired). 6 CRITICAL numeric assumptions (calibration `W0` =
default-zoom clip.w; min/max clip.w for far-underflow + near-zoom
lake-bottom bound) require MEASURED values, not grep -- gamecam zoom
range is dynamic (camera.cpp:2407+). Grounding via env-gated
`[DEPTHBIAS_CALIB v1]` probe (quad.cpp terrain emit, logs pw=|1/clip.w|
+ min/max; INERT) shipped in v0.4 on the Deploy-A baseline. NEXT: one
mc2_17 zoom-sweep -> measured W0+range -> mandated adversarial review
grep-closing the 6 CRITICALs with real numbers -> implement on the
header seam -> multi-zoom user visual + CPU/GPU parity probe.

**HALF-MAP-SHADOW ROOT CAUSE CORRECTED 2026-05-16 (prior dynamic-caster
theory INVALIDATED -- it was a probe artifact + wrong subsystem):** the
earlier line "VPL-#11 late-lance activates a drop-zone static prop with
Identity transform into g_shadowShapes (tgl.cpp:3064), render-expert
root-caused, Deploy-B caster-gate" is WRONG and was reverted. Two
independent invalidations: (a) mandated adversarial review grep-proved
the `shadow_caster_register` probe + the Deploy-B gate read DEAD matrix
indices -- Stuff `Matrix4D` is COLUMN-major (translation at
`entries[3]/[7]/[11]` per `msl.cpp:1207-1209`; `operator=(AffineMatrix4D)`
hard-zeros `entries[12..14]`), so the probe's "2054/2054
isIdentityXlate=1 persistent origin shape" was an artifact of reading
the always-zero projective column; Deploy-B default-on would have
suppressed 100% of dynamic object shadows scene-wide. The render-expert
spec had cited `entries[12/13/14]`; only the review's matrix.cpp grep
caught it. (b) User first-hand reframe (authoritative): the half-map
shadow is SOFT/gaussian + "computes once from camera, never updates,
shifts on pause" = the STATIC terrain shadow map, NOT the (sharp)
dynamic object-caster path. Dynamic-caster code fully reverted.
**ACTUAL root cause (render-expert, code-grounded):** the static
terrain shadow FBO is fed the CAMERA-WINDOWED terrain quadList and its
depth is cleared ONLY on frame 1 then GL_LESS-accumulated forever
(`gos_postprocess.cpp:1160` "NO glClear -- accumulate"); the >100u
camera-move re-render gate (`txmmgr.cpp:1538`) misfires on mc2_17
(sub-100u settling) so the map freezes to the build-time camera's
terrain -- a soft half-map wash. Light-space matrix is correct
(world-fixed, build-once). **Meta fix = arch doc
`docs/plans/static-terrain-shadow-architecture.md` Phase 1 (delete the
camera-motion accumulation trigger) + Phase 2 (build the full-map
static terrain shadow once at mission init; Phase 2's real work is the
full-map terrain feed, parallel to terrain-contract cleanup).**
Confirm-probe-first: `[SHADOWSTATIC v1]` (env `MC2_DEBUG_SHADOW_STATIC`,
INERT) ships in v0.4 -- gate-decision (`txmmgr.cpp:1538`) +
feed-volume (post `gos_EndShadowPrePass`); one mc2_17 pan/zoom/pause
session proves decision=reused-forever + camera-windowed feed, THEN
Phase 1/2 implemented under mandated review. SEPARATE deferred defect
(filed, do NOT couple): `GpuMechBatcher::flushShadow()` is a literal
no-op (`gos_mech_batcher.cpp:341`) -> GPU-batched mechs never populate
`g_shadowShapes` -> mechs cast no dynamic shadow (broken several
builds); its own slice after the static meta fix. NOT a depth-bias
issue; do not couple to #10.

**EXECUTABLE IMPLEMENTATION SPEC 2026-05-16 (plan LOCKED: probe-proven
root cause + terrain-indirect plan + mandated review PROCEED-WITH-CHANGES
+ shader-expert resolved; M-1 de-risked to trivial). Implement as ONE
coherent commit (catastrophic-axis):**

ROOT CAUSE (settled): static shadow ortho is correct & built-once
(`[SHADOWFRUSTUM v1] n=1 mapHalfExtent=6400 orthoHalf=9503.5`); build &
sample share `pp->getLightSpaceMatrix()`. Bug = shadow FBO fed only the
camera-windowed `masterVertexNodes` subset (`txmmgr.cpp` ~1585-1612),
never the full map -> near-empty depth atlas -> soft half-map wash.

1. FULL-MAP FEED (replace the camera-windowed loop on the first-frame
   latch only): iterate absolute tiles `absY,absX in [0,
   Terrain::realVerticesMapSide-1)`; 4 corners from
   `&blocks[absX+absY*realVerticesMapSide]`; world pos
   `vx=(absX-halfVerticesMapSide)*worldUnitsPerVertex`,
   `vy=(halfVerticesMapSide-absY)*worldUnitsPerVertex` (mapdata.cpp:1138-9);
   `gos_TERRAIN_EXTRA` per vert exactly as `fillTerrainExtra`
   (quad.cpp:~341: wx=vx,wy=vy,wz=pVertex->elevation,n=vertexNormal),
   `gos_VERTEX` triple per the TerrainQuad per-uvMode 2-tri emission
   (read quad.cpp:2557/2700 vs 2915/3056 for exact order); submit via
   the SAME `gos_DrawShadowBatchTessellated`+`gos_SetRenderState`
   path (reuses `shadow_terrain.tesc/.tese` unchanged).
   M-1 SOLVED BY CONSTRUCTION: `uvMode` = makeLists if/else
   (mapdata.cpp:1210-1243) with `xby2=absX&1, yby2=absY&1`. Since
   `(topLeftX+x)&1 == (topLeftX&1)^(x&1)`, makeLists'
   `(x&1)^(topLeftX&1)` IS absolute-tile parity -> full-map absolute
   loop = byte-identical per-tile diagonals to the scene at ANY camera.
   M-2: chunk per texture node AND hard-split any chunk nearing 60000
   verts / 65535 idx (indexed_tris_ cap ~gameos_graphics.cpp:3285;
   addVertices SILENTLY drops on overflow); probe emits per-chunk max.
   Tess uniforms: reuse LIVE `terrain_tess_level_`/`_displace_scale_`/
   `_detail_tiling_` as the current shadow prepass block
   (gameos_graphics.cpp:~4336-4360); cameraPos/tessDistanceRange dead
   in both terrain tesc -> leave; NO depth-bias change (prepass has
   zero TERRAIN_DEPTH_FUDGE; decoupled from #10).
2. C-1 (CRITICAL, ship in THIS commit): latch
   (`staticLightMatrixBuilt_` reset only at gos_postprocess.cpp:1120
   FBO-init; `s_terrainShadowPrimed` txmmgr static) has NO per-mission
   reset -> after Phase-1 deletes camera-accumulation, mission 2+
   freezes mission-1 shadows. Add per-mission RE-ARM in
   `Terrain::destroy` (terrain.cpp:710, once per mission exit): reset
   `s_terrainShadowPrimed` (new `gos_ResetStaticShadowState()`
   accessor) + `gosPostProcess::staticLightMatrixBuilt_=false` (new
   public reset). `blocks` is one-shot populated at load
   (mapdata.cpp:204 newInit, review-confirmed not camera-lazy).
3. PHASE-1 RETIRE (order: add fn dead-code -> swap firstFrame feed
   clear=true -> delete gate -> delete prime+rebuild API): camera gate
   (`lastShadowCamX/Y/Z`,`shadowCamDist`,`shadowCacheThreshold`,
   condition txmmgr ~1530-1568), prime block (~1509-1523),
   `gos_ShadowRebuildPending`/`gos_RequestFullShadowRebuild`/
   `gos_ClearShadowRebuildPending`/`s_shadowRebuildPending`
   (gameos_graphics.cpp ~6589-6592 + gameos.hpp + txmmgr callsites;
   review-confirmed only-caller is the prime block, safe), the
   `[SHADOWSTATIC v1] event=gate` probe. KEEP+repurpose the feed-volume
   probe -> `event=fullmap_build builds=N nodes/verts/tris bbox vs
   ±mapHalfExtent perChunkMax`. Stock fallback: blocks null /
   realVerticesMapSide==0 -> existing camera-windowed walk (no crash).
4. VERIFY: mandated re-review of the diff -> build/deploy v0.4 ->
   user mc2_01 (Alt+3 FULL-map atlas; cast shadow correct & STABLE on
   pan/zoom) + a 2nd mission (reload not frozen = C-1 proof).

Status: spec LOCKED & de-risked; NOT yet implemented (deliberately not
rushed at session tail on catastrophic-axis code). Self-contained
execution contract — resume here.

================================================================
RESOLVED 2026-05-16 (static giant A) -- commit `0c421d1`, user-visual-
confirmed. Supersedes the handoff below for artifact (A):
================================================================
Root cause of the giant NE-axis half-map STATIC shadow was NOT feed
scope and NOT the sample-side matrix source -- it was a DEPTH-CONVENTION
mismatch. The engine sets `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`
globally (`GameOS/gameos/gameosmain.cpp`) and the scene ortho was
already [0,1] (`mclib/camera.cpp:2032/2037`), but `buildStaticLightMatrix`
AND `buildDynamicLightMatrix` (`gos_postprocess.cpp`) still emitted
classic GL [-1,1] clip-z. Under ZERO_TO_ONE the near half of each light
frustum (clip-z < 0) was hardware-clipped pre-rasterization -> wedge
atlas (user Alt+F2 screenshot: triangular built region) -> hard half-map
boundary. Fixed lockstep in ONE commit (mandated adversarial review
BLOCK'd a static-only scope: `shadow_screen.frag` `sampleShadowMap` is a
shared helper called with BOTH matrices -> user chose symmetric scope):
both ortho z-rows -> `-1/(far-near)` / `-near/(far-near)`; `.xy`-only
remap in `shadow.hglsl` (calcShadow + calcDynamicShadow) and
`shadow_screen.frag`; same-commit env-gated `[SHADOWZRANGE v1]` probe
(`MC2_DEBUG_SHADOW_ZRANGE`). User confirmed: static atlas now full, NE
half-map boundary GONE. Feed-scope Phase 1+2 stays (sound, orthogonal).

STILL OPEN (artifact B, separate slice -- NOT this fix's scope): the
dynamic shadow map's wrong PROJECTION/FOCUS (camera-following grid).
The depth-convention fix removed the dynamic path's wedge too but did
NOT (and was not expected to) correct the dynamic focus matrix. That is
the dynamic-shadow-projection slice; the handoff disambiguation below
(A vs B) still applies for it.

UNRELATED defect filed 2026-05-16 (do NOT couple to shadows): GPU-driven
compute shaders `#include` fails -- `gpu_driven::BuildComputeProgramFromFile`
(`gpu_driven_common.cpp:149`) bypasses the engine include preprocessor.
Spun off as its own task.

================================================================
FRESH-SESSION HANDOFF 2026-05-16 (read this FIRST; supersedes the
"implemented/PROCEED" optimism above for the VISIBLE artifact):
================================================================
The static Phase 1+2 full-map-feed fix WAS implemented (7 files),
built clean, mandated-review PROCEED, deployed v0.4 (exe
`ff12a33038ae`). **USER-VERIFIED ON mc2_01: IT DID NOT FIX THE VISIBLE
SHADOW.** => feed-scope was NOT the (whole) root cause of the visible
static artifact, OR a second dominant static-path bug exists. The
full-map feed is sound engineering and should stay (the camera-windowed
feed WAS wrong), but the giant shadow persists -> **static-path root
cause is STILL OPEN.** Do NOT assume Phase 1+2 solved it.

CRITICAL DISAMBIGUATION (user authoritative, the thing this whole arc
kept conflating): there are TWO SEPARATE shadow defects, not one
"half-map shadow":
  (A) GIANT HALF-SCREEN SHADOW = the STATIC path. STILL BROKEN after
      Phase 1+2. Root cause unconfirmed. Probe-proven facts that still
      hold: ortho light matrix correct & built-once
      (`[SHADOWFRUSTUM v1] n=1 mapHalfExtent=6400 orthoHalf=9503.5`),
      build & sample share `pp->getLightSpaceMatrix()`. Since the feed
      is now full-map yet the giant shadow persists, the next suspect
      is the SAMPLE-side projection / depth-compare (how the scene
      shader projects worldPos into static-shadow space and tests it),
      NOT the feed. Re-investigate sample-side `calcShadow` /
      `shadow.hglsl` / the lightSpaceMatrix actually bound at scene
      draw vs at the build, and the static shadow depth/compare.
  (B) THE SOFT DARK "BLOB" in the latest screenshots = the DYNAMIC
      shadow path, which the user ENABLED to investigate. Its
      PROJECTION IS WRONG (user first-hand diagnosis). Distinct from
      (A). May or may not be the same as the filed
      `GpuMechBatcher::flushShadow()` no-op — fresh session must
      clarify. The dynamic path is a camera-following fixed-size grid
      that must NOT project cross-map; "projection is wrong" =
      investigate the dynamic light-space/focus matrix + the
      dynamic-shadow sample projection.

DISCIPLINE NOTE for the fresh session: BEFORE any root-causing, use the
in-game debug overlays (Alt+3 static map; the dynamic-shadow overlay)
to pin WHICH visible artifact maps to WHICH subsystem (static vs
dynamic) — this arc burned enormous cycles because "the half-map
shadow" silently conflated two subsystems' overlapping artifacts and
advisor theories chased the wrong one repeatedly; the user's
debug-overlay observations were each authoritative and each overrode
theory. Disambiguate-by-overlay FIRST, then probe-confirm, then fix.

State of probes in v0.4 (all inert/env-gated): `[SHADOWSTATIC v1]
event=fullmap_build` (MC2_DEBUG_SHADOW_STATIC), `[SHADOWFRUSTUM v1]`
(MC2_DEBUG_SHADOW_FRUSTUM), `[DEPTHBIAS_CALIB v2]`
(MC2_DEBUG_SHADOW... wait MC2_DEPTHBIAS_CALIB). #10 ships the
two-constant interim (FAST 0.003 / RASTER 0.0025). PARKED: #10
distance-proportional (single-W0 premise invalidated by real clip.w
distribution ~137-14550/frame; needs shader-expert rework, NOT a
scalar W0). The Phase-1 retired camera-accum machinery + C-1 reset are
implemented and review-clean — keep them; they are correct regardless
of the (A) root cause.

## 11. Invisible mechs on mc2_04/05 FROM A SAVEGAME (pre-existing, NOT VPL)

**What:** Some mechs render invisible on campaign missions mc2_04 and
mc2_05 **only when the mission is loaded from a savegame**; fresh mission
start is clean. Reported 2026-05-15.

**Why NOT a VPL regression (data-flow proof, not just commit-range):**
The savegame-restore path is the SAME mech path as fresh start
(`GameObjectManager::Load` objmgr.cpp:3691 -> `new BattleMech` ->
`init(true,objType)` builds the appearance identically (mech.cpp:1256/
1309/1315) -> `BattleMech::Load` -> `Mech3DAppearance::copyFrom`
mech3d.cpp:5306, which restores only pose/animation scalars and
deliberately does NOT restore `status`, mech3d.cpp:5327). Grep-verified
ZERO references to `px/py/pz/pw`, `objVertexActive`, or `objBlockInfo`
in mech.cpp/mech3d.cpp -- mech appearance/visibility has NO data
dependency on any state the VPL retirement touched. Mechs render-gate on
`getExists()`, not the terrain cull cascade. Zero commits to mover/
objmgr/mech3d/warrior/mission/save-load in the VPL range; 250ab4a/6c9d4b5
cull-bit-identical by construction. `copyFrom` is a known PARTIAL restore
(`status` intentionally dropped) -- savegame-only mech bugs are a latent
class in the deserialization path, no smoke exercises it.

**Decisive user bisection (if confirmation wanted):** build `12ad8dc~1`
(parent of the first behavior-changing VPL commit), load the SAME
mc2_04/05 saves. Mechs still invisible -> pre-existing CONFIRMED, VPL
ruled out. (Evidence already strongly supports pre-existing without this.)

**Own slice:** dedicated savegame-deserialization debug -- focus
`Mech3DAppearance::copyFrom` (mech3d.cpp:5306, esp. status-not-restored
:5327), `Mover::Load`, `MechWarrior::Load`, and per-save pilot/team/
gesture state specific to mc2_04/05. NOT a terrain patch, NOT bundled
into the retirement trail. Use the mech-runtime advisor + user visual
repro (USER-DRIVEN: savegame load cannot be automated in smoke).

**CORRECTION 2026-05-15 (mech-update-geometry advisor + user RenderDoc
`mechHalfwayBuilt.html` frame 2088 EID 3931):** the `status`-not-restored
lead above is **data-flow-DISPROVEN as the invisibility cause**. `status`
(copyFrom-dropped at `mech3d.cpp:5327`) feeds ONLY `lightsOut` /
ambient-only shading (`mech3d.cpp:2546-2550`), never a submit/visibility
gate -- a defaulted `status` yields a fully-lit mech, not an absent one.
Real mechanism (grep-verified): Track D batches per mech TYPE via
`TypeLodKey{mechType,currentLOD}` (`gos_mech_batcher.cpp:573`); a type
whose `registerTypeLod` is skipped or runs AFTER
`GpuMechBatcher::finalizeGeometry()` (`mission.cpp:3115`) fails the
`s_typeLodIndex` lookup for every instance (`gos_mech_batcher.cpp:575-579`,
late-reg), and `mechGpuCullSkip` suppresses the CPU fallback
(`mech3d.cpp:2582`) -> the whole type emits ZERO draws. User RenderDoc
confirms: the 4 visible mechs (one type) = EID 3931 `(36 idx,4 inst)`;
the 5 invisible (a different single type) have NO draw anywhere in the
frame -- a missing per-type batch, not degenerate instances. The
remaining open question is the TRIGGER: why the invisible type's
registration is missed/late ONLY on the savegame load path -- a
load-ORDER question deferred to `mc2-mission-data-expert` (Load
sequence: `BattleMech::Load`/`Mover::Load`/`MechWarrior::Load` vs the
`finalizeGeometry()` boundary). Settle empirically via the
`[MECHRESTORE v1]` / `MC2_MECH_RESTORE_TRACE` probe (3 emit sites:
`registerTypeLod` success+late-skip `gos_mech_batcher.cpp:334/338-344`,
post-`submitActor` discriminator `mech3d.cpp:2561`, `copyFrom` tail
`mech3d.cpp:5306`) -- converts the un-smokeable savegame repro into a
one-load log read. Full analysis: observations note
`docs/observations/2026-05-15-render-vpl-deferred-retirement-topology.md`
sibling + this session's advisor report.

**ROOT CAUSE 2026-05-15 (mc2-mission-data-expert, grep-verified):** the
in-mission-save load path `Mission::load` (`saveload.cpp:627`, invoked
from `mechcmd2.cpp:2389` `loadInMissionSave`) is a PARALLEL mission-setup
implementation that does NOT route through `Mission::init`.
`GpuMechBatcher::finalizeGeometry()` has exactly ONE callsite in the
codebase -- `mission.cpp:3115`, the unconditional tail of
`Mission::init`. The save path calls `Mission::destroy()`
(`saveload.cpp:687`) -> `GpuMechBatcher::onMapUnload()`
(`mission.cpp:3260`, resets `s_geometryFinalized=false`, clears
`s_typeLodIndex`), then re-spawns all mechs via `ObjectManager->Load`
(`saveload.cpp:1351` -> `objmgr.cpp:3698-3710`) with lazy
`getAppearance`->`registerTypeLod`, but NEVER calls `finalizeGeometry()`
afterward. `s_geometryFinalized` stays false -> `submitActor`
fast-rejects at `gos_mech_batcher.cpp:570` (`!s_geometryFinalized`).
The per-type "5 of one type gone, 4 of another fine" expression is
governed by the `appearanceTypeList` `numUsers++` dedup early-return
(`apprtype.cpp:235-238`, surviving types skip re-`init()`/re-register)
interacting with the `onMapUnload` `s_typeLodIndex` clear and the
conditional `appearanceTypeList->destroy()` (`mission.cpp:3440`,
`initLogistics` branch only): a type whose `Mech3DAppearanceType*` +
`s_typeLodIndex` entry survived the original `Mission::init` still
renders; a re-created or save-only type re-registered post-`onMapUnload`
with no finalize does not. One open runtime detail (which
`Mission::destroy` overload at `saveload.cpp:687`; whether
`appearanceTypeList` is torn down) is answered directly by the probe's
`appearanceListNull` field. **FIX FORK (deferred to
mc2-mech-update-geometry / a debug slice -- NOT yet decided):** (a)
`Mission::load` calls `GpuMechBatcher::finalizeGeometry()` at its tail
(after `saveload.cpp:1351`, mirroring `mission.cpp:3115`) -- minimal,
mirrors the known-good path; (b) batcher supports post-finalize
incremental type registration + VBO append -- general, larger blast
radius. Probe adds a save-path emit in `Mission::load` after
`destroy()` (`saveload.cpp:687`) and after `ObjectManager->Load`
(`saveload.cpp:1351`): `[MECHRESTORE v1] event=saveload_phase
phase={post_destroy|post_objmgr_load} mechFinalized=<isFinalized()>
appearanceListNull=<appearanceTypeList==NULL>` (needs a 2-line
`GpuMechBatcher::isFinalized()` accessor). Catastrophic-axis (silent
invisible units = gameplay-breaking); fix slice warrants mandated
adversarial review per the soak-waiver+probes+reviews discipline.

**PROBE-CONFIRMED REDIRECT 2026-05-16 (the mc2_10 repro is a DIFFERENT
path; supersedes the Mission::load attribution as THIS repro's cause):**
The user's mc2_10 "load from save" produced `[MECHRESTORE v1]` with ZERO
`event=saveload_phase` and ZERO `event=copyfrom` lines -> `Mission::load`
/ `Mech3DAppearance::copyFrom` are NOT executed for a campaign `.fit`
save resume. That path is `LogisticsData::load` -> `Logistics::
beginMission` -> `mission->init(... MISSION_LOAD_SP_LOGISTICS)` whose
tail finalizes the batcher at `mission.cpp:3115`; THEN the SP_LOGISTICS
branch instantiates the player force-group lance via
`mission->addMover()` at `logistics.cpp:780/785` -- AFTER finalize -->
`registerTypeLod` (`mech3d.cpp:329`) hits `s_geometryFinalized==true`
-> `[MECHBATCHER v1] event=late_register` -> probe `result=late_skip` ->
those actors `event=submit submitted=0 lateReg=1` -> CPU fallback also
suppressed -> invisible. Probe-verified: invisible type
`0000019351C43BD0` (player lance) late; visible type `000001939B70D880`
(mission-script units) registered pre-finalize. Fresh start routes the
player lance pre-finalize; campaign save-resume routes it post-finalize
-> "only from savegame". TWO ORTHOGONAL FIXES, do not collapse:
(1) the `Mission::load` two-half change (`onMapLoad`+`finalizeGeometry`)
already implemented -- KEEP, it is correct coverage for the genuine
`.ims` in-mission-quicksave path (Ctrl+Alt+Shift+Z mission.cpp:297 /
pause-window pausewindow.cpp:391, via `copyFrom`), just not this repro;
(2) THE mc2_10 FIX: a `GpuMechBatcher::finalizePending()` re-finalize at
`logistics.cpp:789` (after the SP force-group loop, before
`initMechs()` ~:803). Batcher spec (mech-update-geometry advisor):
`registerTypeLod` (`gos_mech_batcher.cpp:340-350`) must STAGE-not-drop
when finalized + set `s_pendingLateTypes`; new `finalizePending()`
fence-waits + deletes `s_sharedVao/Vbo/Ibo` (glBufferStorage IMMUTABLE
-- must delete before recreate) + `s_geometryFinalized=false` + re-runs
`finalizeGeometry()` from the regrown staging, NEVER clearing
`s_typeLodIndex/s_typeLodRecords/s_packets` (append-only -> existing
indices stable). No render occurs between `mission.cpp:3115` and
`logistics.cpp:789` (first `mission->update()` logistics.cpp:808) so no
draw consumes a half-built index -- this no-render-in-gap invariant is
load-bearing and fragile. MANDATED adversarial review must grep-close 3
items before implementation: (a) zero `GpuMechBatcher::flush`/
`renderLists` between mission.cpp:3115 and logistics.cpp:808 INCLUDING
`mission->start()` (logistics.cpp:807) ordering vs :789; (b)
glBufferStorage delete-recreate fully fence-drained vs persistent-mapped
ring SSBOs / `s_fence[MECH_RING_FRAMES]`; (c) `GpuStaticPropBatcher`
truly needs no parallel `finalizePending` (SP roster is movers-only --
verify). Probe extensions to land same-commit (env `MC2_MECH_RESTORE_
TRACE`): `result=staged_pending`, `event=finalize_pending types=N
lateAdded=M rebuilt=1`, `event=late_type_visible type=%p`.

**ADVERSARIAL REVIEW: BLOCK 2026-05-16 (C1 -- the staging-source
premise above is FALSE).** `finalizeGeometry` does
`s_stagingVbo.clear(); shrink_to_fit()` (+ IBO) at
`gos_mech_batcher.cpp:565-566`, so after Init's finalize
(`mission.cpp:3115`) staging is EMPTY. A re-stage-then-re-finalize
rebuilds the shared VBO from LATE-TYPE-ONLY data at `baseVertex=0`
while the ~13 visible types' `s_packets` keep ABSOLUTE offsets into the
old layout (`:481-489`) -> all 13 currently-visible types render
garbage/vanish. The "regrown staging containing original + late"
premise is wrong; do NOT implement as specced. Review grep-CLOSED the
other two: (a) no render between `mission.cpp:3115` and
`logistics.cpp:789` (`flush` only via `renderLists` from frame loop;
`mission->start()`/`update()` after :789) -- the seam+timing are SOUND;
(b) fence/immutable-buffer concern moot given (a); (c) SP path is
movers-only, no `GpuStaticPropBatcher` parallel needed. Only the
STAGING SOURCE is broken. STAGING-SOURCE FORK (needs decision before
implementation): (i) do NOT clear staging at first finalize (drop
`:565-566`; keep `s_stagingVbo/Ibo` resident whole-mission so late
append truly regrows original+late) -- smallest code, costs bounded
persistent CPU RAM for the full mech VBO, contradicts the `:110`
"staging cleared after finalize" design comment; (ii) `finalizePending`
rebuilds staging by re-walking all `s_typeLodRecords` from
`rec.sourceNode0`/`owningTypeShape` (`:384/:488`) -- no persistent RAM,
but depends on those `TG_TypeShape*` still alive post-init + re-runs
triangle-soup extraction (more code/failure surface); (iii) append-only
SECOND VBO for late types + per-packet buffer-id (true incremental, no
rebuild/fence concern, but biggest change in the hot `flush` draw
path). Tracker premise corrected to reflect this BLOCK.

**COMMITTED 2026-05-16 (`63bd655`).** Was deployed in v0.4 and
user-confirmed but uncommitted until this history-hygiene pass.

**FIXED 2026-05-16 (option (i) implemented, user-confirmed).** User
chose fork (i). Implemented: `gos_mech_batcher.cpp` -- removed the
`s_stagingVbo/Ibo.clear()+shrink_to_fit()` at the old `:565-566`
(staging now RETAINED whole-mission, cleared only at `onMapLoad()`;
`:110`/declaration comments updated); `registerTypeLod` post-finalize
branch now STAGES-not-drops (falls through to the staging body, sets
`s_pendingLateTypes`, `++s_lateStagedCount`, probe `result=staged_
pending`, no longer `++s_disallowedLateRegEvents`); new
`GpuMechBatcher::finalizePending()` (header + impl) fence-drains,
deletes `s_sharedVao/Vbo/Ibo/s_sampler`, sets `s_geometryFinalized=
false`, calls `finalizeGeometry()` (rebuilds from full retained
staging; `s_typeLodIndex/records/packets` untouched -> stable indices),
emits `[MECHBATCHER v1] event=finalize_pending` + `[MECHRESTORE v1]
event=finalize_pending types=N lateAdded=M rebuilt=1`. Call seam:
`GpuMechBatcher::instance().finalizePending();` in
`code/logistics.cpp` immediately before `mission->missionInterface->
initMechs()` (after the SP force-group `addMover` waves, before
`mission->start()`/`update()` -> inside the review-proven no-render
gap); no-op on non-late paths. Built RelWithDebInfo clean (exit 0,
forced relink), deployed `mc2-win64-v0.4` (md5-verified
`1b042499...`). **User loaded the mc2_10 save FROM THE MENU (campaign
SP_LOGISTICS resume, NOT `.ims` quicksave -- confirms the path) and
visually confirmed the previously-invisible mechs now render.** The
`Mission::load` two-half change remains in `code/saveload.cpp` as
orthogonal `.ims`/`copyFrom` quicksave-path coverage (KEEP, per both
advisors). Probe `[MECHRESTORE v1]` stays gated-off
(`MC2_MECH_RESTORE_TRACE`). CAVEAT: the tier1 automated regression
smoke for this build did NOT complete (user killed it to run the save
test); the catastrophic-axis change is empirically confirmed for #11
itself but the normal-render-path regression sweep is OUTSTANDING --
re-run `run_smoke.py --tier tier1` opportunistically.

## 12. Zoomed-out big-map `Terrain::IndirectDraw` cost (regression, PRE-VPL: 08bd3b2) -- FIXED 073dba4

**FIXED 2026-05-15 (commit `073dba4`, Approach A):** camera-windowed
solid dispatch restored (per-frame `solidWin` window SSBO binding 9 +
`u_useWindow`), window sourced lag-free from the slim loop's cull
production, `MC2_TERRAIN_SOLID_NARROW=0` escape hatch == HEAD. Mandated
independent adversarial review caught 2 CRITICAL (dead `assert` tripwire
under `/DNDEBUG`; 1-frame-lag terrain-vanish) + 2 MAJOR pre-commit; all
resolved. Validated tier1 5/5 PASS, `MC2_TERRAIN_SOLID_WINDOW_PARITY=1`
every `total_dropped=0` / zero `event=catastrophic`, escape hatch ==
HEAD. REMAINING (user-driven, not blocking): big-map zoomed-out Tracy
before/after to quantify the win, and parity `total_dropped=0`
confirmation through a camera-motion segment (headless smoke is
default-camera). The armed stderr tripwire will catch any drop live.

Original analysis below (retained for context):



**What:** On a big map zoomed out all the way, `Terrain::IndirectDraw`
(GameOS/gameos/gameos_graphics.cpp:~2480) is ~7.8 ms mean / 18 ms p99
every frame; total frame ~25 ms. Reported 2026-05-15.

**Root cause (advisor git-bisected):** commit `08bd3b2` (2026-05-11,
"retire BuildSolidQuadWindowSSBO") - 4 days BEFORE the VPL retirement
(8b `12ad8dc` / 8c `0c8e06b`, 2026-05-15). Pre-08bd3b2 the indirect
compute dispatched over the camera-windowed `quadList` (rebuilt per
frame by `makeLists()`, terrain.cpp:907 - small/bounded). 08bd3b2
deleted that windowed walk and hard-wired the dispatch to
`g_denseRecipes.size()` (`gos_terrain_indirect.cpp:1922/2145/2588`) =
the ENTIRE MAP every frame, camera-independent (the compute only
edge/invalid-skips, no frustum/distance/LOD). Zoomed out on a big map
= whole map drawn every frame. NOT a VPL-retirement regression
(250ab4a/6c9d4b5 exonerated: indirect clipPos packed by
PackThinRecordsForFrame/ComputeDispatch, not slim-loop sp; 8b/8c land
after 08bd3b2). `Render.GpuStaticProps` is INDEPENDENT (Track C
frustum cull, no terrain-recipe coupling) - its zoom-out cost is
pre-existing expected prop-count, not coupled to this.

**Decisive check:** `git show 08bd3b2 -- GameOS/gameos/gos_terrain_indirect.cpp`
(windowed `BuildSolidQuadWindowSSBO()` -> `g_denseRecipes.size()`).
A/B: build `08bd3b2^`, same big map zoomed out, compare IndirectDraw ms.

**Own slice (design fork - needs decision):** decouple the indirect
DRAW set from the full recipe range; restore a tighter in-view /
frustum / distance-LOD bound for DRAW while keeping the recipe SSBO
map-stable for storage (the draw-vs-storage split the slim loop
already applies for reduction-vs-cull). Constraint: must NOT
reintroduce the per-frame CPU recipe walk 08bd3b2 removed, and must
NOT add `glGetBufferSubData`/CPU readback to bound the dispatch
(Track C substrate sync; see memory/track_c_substrate_regression.md /
substrate_coalesce_sync_point_lesson.md). Candidate approaches:
(A) windowed-index buffer (GPU-built visible-quad index list,
dispatch over that, index into stable recipe SSBO);
(B) in-compute-shader frustum/distance reject (dispatch full but
early-out non-visible threads - cuts draw, not dispatch threads);
(C) hybrid GPU compaction. Catastrophic-axis-adjacent (compute
dispatch + substrate intersection) - warrants adversarial review.

## 13. `Render.GpuStaticProps` 22.88ms GPU-self zoomed-out (regression, PRE-VPL: a2a6058)

**What:** On a big map zoomed all the way out, `Render.GpuStaticProps`
(mclib/txmmgr.cpp:~1753-1782) = 23.35ms GPU exec / 22.88ms GPU-self
(97.97%), CPU setup 529us -- ~the entire frame in one zone. Dominates
`Terrain::IndirectDraw`. Reported 2026-05-15.

**Root cause (render-perf + gameos advisor): HONEST OVERDRAW, NOT a
stall.** Commit `a2a6058` (2026-05-12, "fix(bldg): pin BldgAppearance
to LOD 0") replaced distance-based LOD selection with a hard LOD-0 pin
for ALL static props (`mclib/bdactor.cpp:1386-1415`), as a workaround
for an unrelated LOD-1+ invisibility bug. Combined with a frustum-ONLY
GPU cull (no distance/LOD/screen-size term -- `shaders/gpu_cull_
predicate.glsl:84 clipSpaceFrustumAdmitDilated`, far bound only the
~12000u far plane) and a deliberate GPU-path `inView` bypass
(`bdactor.cpp:1648-1654`, `:1211`), zoomed-out-big-map = every prop on
the map x full LOD-0 geometry x every frame, via 2x
`glMultiDrawElementsIndirect` (`gos_static_prop_batcher.cpp:3250/3272`).
The `glClientWaitSync` ring-fence (`:2659`) only AMPLIFIES (GPU falls
>RING_FRAMES behind), it is not the cause. The historic substrate
`glGetBufferSubData` stall was already fixed (gpu_cull_compute.cpp:
834-842 reads a CPU-side counter; remaining readback is 600-frame
parity-only). NOT a VPL regression (a2a6058 predates 8b/8c; same
bug-class as 08bd3b2/the LOD pin -- fine at default camera, never
stress-tested zoomed-out). Pre-existing frustum-only cull is older;
a2a6058 is what turned "distant props cheap (low LOD)" into "every
distant prop full geometry".

**Cheapest decisive check (user-driven, no rebuild):** Tracy two
captures same max-zoom big-map camera, `MC2_GPU_CULL_SUBSTRATE=0` vs
`=1` (substrate=0 renders ZERO static props per
memory/substrate_off_renders_no_static_props.md). 22.88ms -> ~0 with
substrate off == confirmed prop workload. Or RenderDoc one frame:
the two glMultiDrawElementsIndirect primitive/instance counts.

**DESIGN DECISION (user, 2026-05-15):** Fork B, the ROUTE-TO-COARSER
variant -- a GPU-side distance->LOD SELECTOR. HARD CONSTRAINT: distant
buildings MUST still render, at a LOWER LOD; they must NEVER be
distance-culled/hidden (MC2 has long sightlines on big maps; hiding
far buildings is a gameplay/visual regression). So the fix replaces
the a2a6058 LOD-0 pin (`bdactor.cpp:1386-1415`) with a GPU-computed
distance/screen-size -> LOD-bucket selection; the frustum cull stays
(props leave only when truly off-frustum, not by distance).

**Dependency / long-pole -- RESOLVED by prep (2026-05-15, advisor):**
the LOD-1+ "zero fragments" bug is SPECIFIC to the runtime CPU
`bldgShape = bldgShape[lod]->CreateFrom()` swap (NULL per-instance
listOfVertices/listOfColors colliding with the cull-gated update()
pass -- a cull_gates_are_load_bearing cascade; gos_static_prop_
batcher.cpp:2559 skip). It is NOT in the GPU draw. ALL LOD typeIDs are
already registered (`bdactor.cpp:830`) + baked into the immutable VBO
(`mission.cpp:3114`). VERDICT: the fix is FEASIBLE via sequencing (ii)
-- a GPU-side per-instance distance->LOD typeID selector that NEVER
does the CPU bldgShape swap. The a2a6058 LOD-0 CPU pin STAYS; only the
draw picks a coarser registered typeID for distant instances. This
sidesteps the invisibility bug by construction; sequencing (i) (fix
the CPU swap first) is strictly worse (deep cull-desync, the cascade
a2a6058 fled). NOT a blocker anymore.

**Scope:** bdactor.cpp render/registerStatic (emit per-instance
LOD-set, no swap), gos_static_prop_batcher.{h,cpp} (per-instance LOD
index / per-type LOD-typeID table), gpu_cull compute +
gpu_cull_predicate.glsl (distance->LOD-bucket term; frustum cull
stays; NEVER distance-reject), GpuStaticPropRegistry recipe. Asset
constraint: not every prop type has LOD-1+ -> GPU selector MUST clamp
the LOD index to the type's registered-LOD count (LOD-0 always
exists). Catastrophic-axis (mandated independent review required):
std430 lockstep for the per-instance LOD index
(cpp_glsl_ubo_struct_lockstep, one-commit), wrong-LOD-everywhere
(parity probe vs pinned LOD-0 at default zoom = byte-identical),
missing-LOD clamp, indirect/cull-count desync (item-12 class).

**ROOT CAUSE DISPUTED (user domain knowledge, 2026-05-15) -- do NOT
accept the a2a6058 attribution verbatim:** the advisor blamed the
a2a6058 building LOD-0 pin, but the user (who knows the content)
states: trees were ALREADY full-LOD pre-a2a6058 (a2a6058 changed ONLY
buildings), and the affected maps have FEW buildings. So "buildings
pinned to LOD-0" is unlikely to drive 22.88ms. Most probable actual
cause: PRE-EXISTING full-LOD TREE prop volume (lots of tree static
props, never LOD'd, unrelated to a2a6058) -- i.e. GpuStaticProps cost
is largely pre-existing and NOT an a2a6058 regression. The
substrate=0/=1 check shows TOTAL static-prop cost, not a tree-vs-
building split, so it cannot by itself settle this; a per-category
(tree vs bldg) instance/primitive count (RenderDoc on one zoomed-out
frame, or a category-split probe) is what would actually settle it.
The GPU distance->LOD selector still helps regardless (it LODs trees
too, the dominant volume), but the headline "regression / a2a6058"
framing is downgraded to "likely pre-existing tree volume, attribution
unconfirmed".

**Measurement note (2026-05-15):** a user run showed Terrain::
IndirectDraw ~1ms (down from 7.8-9ms) -- consistent with the #12
(073dba4) windowing win, BUT that run was on a remote-desktop session
with different monitor scaling/resolution (a large fragment-bound GPU
confounder, bigger than the camera-input confounder in
smoke_autonomous_run_pattern.md s4). NOT a clean #12 validation; a
same-resolution non-remote MC2_TERRAIN_SOLID_NARROW=0-vs-default A/B
is still needed for the real #12 number. NO #13 cull/LOD code change
has been made (verified git: last code commit is 073dba4 / #12).

**Pending before implementation (deferred -- session wrap):** settle
the tree-vs-building attribution first (per-category count), THEN if a
GPU distance->LOD selector is still wanted: plan -> mandated
independent adversarial review -> parity probe -> smoke matrix (the
#12 / 073dba4 discipline). Not started; tracked, non-blocking.

**Housekeeping:** commit a2a6058 cites `memory/bldg_animation_lod_
swap_unsafe.md` which is NOT present on disk -- a stale memory
reference to repair separately (non-blocking).

---

## Status summary

| # | Item | Class | Blocker? |
|---|------|-------|----------|
| 1 | hazeFactor SSBO field removal | lockstep cleanup | no |
| 2 | `.codex_tmp_isolate/*` git rm | FIXED 521e092 | no |
| 3 | quad.cpp probe-placement pattern | methodology | no |
| 4 | `[RING_MVP_DELTA v1]` FNV residual | FIXED c6b2888 (compile pending) | no |
| 5 | Tracy ~475 us measurement (slimReduce gated `6c9d4b5`) | perf-quantify | no |
| 6 | Step 3 interactive picking UAT | honest gap | no |
| 7 | overlay-decal GPU port | sibling slice | no |
| 8 | dedicated-water-path edge-clamp | follow-up | no |
| 9 | GetApproximateLength precision | characterized | no |
| 10 | zoom-only terrain/decal/water z-fight | two-regime interim COMMITTED 4c57d1e; perm distance-prop PARKED | no |
| 11 | invisible mechs mc2_04/05 FROM SAVE | FIXED+COMMITTED 63bd655 (fork i) | no |
| 12 | zoomed-out big-map IndirectDraw cost | FIXED 073dba4 (Approach A) | no |
| 13 | GpuStaticProps 22.88ms zoomed-out | attribution DISPUTED (likely pre-existing tree volume, NOT a2a6058) | no |

Items 1-9 are the VPL cleanup/measurement/coverage tail (non-blocking;
retirement architecturally complete). Items 10-11 are pre-existing bugs
surfaced during post-retirement testing, explicitly NOT caused by the
retirement (cull producer byte-identical; depth divergence structural) -
tracked here so they are not lost, to be handled as their own slices.
