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

## 12. Zoomed-out big-map `Terrain::IndirectDraw` cost (regression, but PRE-VPL: commit 08bd3b2)

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

---

## Status summary

| # | Item | Class | Blocker? |
|---|------|-------|----------|
| 1 | hazeFactor SSBO field removal | lockstep cleanup | no |
| 2 | `.codex_tmp_isolate/*` git rm | repo hygiene | no |
| 3 | quad.cpp probe-placement pattern | methodology | no |
| 4 | `[RING_MVP_DELTA v1]` FNV residual | inert-cleanup | no |
| 5 | Tracy ~475 us measurement (slimReduce gated `6c9d4b5`) | perf-quantify | no |
| 6 | Step 3 interactive picking UAT | honest gap | no |
| 7 | overlay-decal GPU port | sibling slice | no |
| 8 | dedicated-water-path edge-clamp | follow-up | no |
| 9 | GetApproximateLength precision | characterized | no |
| 10 | zoom-only terrain/decal/water z-fight | pre-existing, NOT VPL | no |
| 11 | invisible mechs mc2_04/05 FROM SAVE | pre-existing, NOT VPL (data-flow proven) | no |
| 12 | zoomed-out big-map IndirectDraw cost | regression, PRE-VPL (08bd3b2) | no |

Items 1-9 are the VPL cleanup/measurement/coverage tail (non-blocking;
retirement architecturally complete). Items 10-11 are pre-existing bugs
surfaced during post-retirement testing, explicitly NOT caused by the
retirement (cull producer byte-identical; depth divergence structural) -
tracked here so they are not lost, to be handled as their own slices.
