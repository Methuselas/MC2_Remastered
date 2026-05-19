# quadSetupTextures Orphan-Walk Retirement - Design

> **EFFORT CONCLUDED 2026-05-19: NO ENGINE CHANGE. Both slices terminally
> dead.** Slice 1: the setupTextures walk IS the per-frame camera
> visibility cull (not an orphan). Slice 2: the water 6-tuple is gated
> by a per-quad predicate over `clipInfo` that slimReduce itself
> produces - folding into slimReduce is circular, deleting is unsafe
> (probe DIVERGENT = real extrema), relocating = inertia. The recon's
> clean-decomposition model was falsified 6x by code. The original
> recon's "inertia / nothing substitutive to bank" verdict is
> vindicated, now exhaustively code-proven. Bankable outputs: the
> drawPass doc-lie fixes (48ba0d8), the DIVERGENT probe result, and the
> proof itself (postmortem `2026-05-19-slice1-postmortem.md`,
> `memory/setuptextures_is_a_multiwriter_tangle_not_a_clean_shuttle.md`,
> `memory/HANDOFF_actual_terrain_perframe_cull_fix.md`). Everything
> below is HISTORICAL.

Date: 2026-05-19
Worktree: `claude/nifty-mendeleev`, HEAD 48ba0d8
Status: APPROVED design. Supersedes
`2026-05-19-quadsetuptextures-retirement-recon.md` (PATCH/inertia verdict
overturned). Inputs: fresh-context terrain-indirect + cpu-gpu-offload
advisor recon-extension (both ran greybeard + adversarial rigor);
`MC2_WATER_INVPROJ_PARITY` probe RUN -> DIVERGENT
(`memory/water_invproj_parity_is_DIVERGENT_not_freebie.md`). All symbols
grep-verified at HEAD 48ba0d8; line numbers drift, re-pin at plan time.

## 1. The code-vindicated reframe

The recon scored per-quad *body* cost ("O(1), inertia"). The real
finding: commit `60f2ef8` made `MC2_TERRAIN_INDIRECT_OVERLAY`
default-ON, so on the stock path `Terrain::render drawPass` skips the
`currentQuad->draw()` loop entirely (terrain.cpp drawPass gate, the
`else` branch is live by default). `TerrainQuad::draw` is the SOLE
consumer of the per-quad recipe->member shuttle written by
`setupTextures`. **The consumer is already dead by default; the
per-frame O(N) `setupTextures` walk is a true orphan producer outliving
its substitutively-retired consumer.** Eliminating it is a
code-vindicated META-FIX (both advisors), honestly chartered NS2
structural orphan-producer retirement - NOT an NS1 ms claim and NOT a
compute port (recipe resolution is already an O(1) mission-load
`buildTerrainFaceCache` fetch).

The per-frame terrain walks:

| Walk | Status (HEAD 48ba0d8) | Action |
|---|---|---|
| `slimReduce` (terrain.cpp `ZoneScopedN("Terrain::geometry slimReduce")`) | Camera-dependent, irreducible, proven sole producer of cull + the leastZ/mostZ/leastW/mostW/leastWY/mostWY 6-tuple | KEEP |
| (b) `setupTextures` per-quad walk | NOT an orphan: it IS the per-frame camera-dependent visibility cull (handle 0xffffffff sentinel = cull channel; quad.cpp:955-967 write, 2064 read) | Slice 1 DEAD/CANCELLED - irreducible like slimReduce; a static cache has no camera. Do not re-attempt |
| DRAWALPHA detail reservation (`addTerrainTriangles` quad.cpp:668) | Dead-pixel claim UNPROVEN (counter mis-targeted; live txmmgr DRAWALPHA passes) | OUT OF SCOPE (user-ruled 2026-05-19; stays as-is, not a regression) |
| (c) water-projection 6-tuple (quad.cpp water block; second writer) | Probe DIVERGENT - contributes UNIQUE extrema | Slice 2: joint re-home into slimReduce |
| (a) `CopyResultsToVertexPool` scatter (gos_terrain_lighting.cpp:834) | LOAD-BEARING - armed indirect packer (quad.cpp:2389) + water-overlay (quad.cpp:2465) re-read scattered pool | Slice 3 (PREP only) |

## 2. Slice 1 - DEAD / CANCELLED (premise falsified at root 2026-05-19)

> **SLICE 1 IS DEAD IN EVERY FORM. Do not execute, do not re-attempt.**
> Execution proved the foundational premise false: `setupTextures`'s
> per-quad walk IS the per-frame camera-dependent terrain visibility
> cull - `!isTerrainQuadVisible(*this)` writes the handle `0xffffffff`
> sentinel (quad.cpp:955-967), `draw()` early-outs on it (quad.cpp:2064);
> the sentinel IS the cull channel. The static Shape-C cache has no
> camera, so repointing `draw`/the hoist at it defeats per-quad cull
> (full-map render = catastrophic zoomed-out regression). There is no
> "orphan producer" - the recon/recon-extension/both advisors/design/
> outside-review/Narrow-A all modeled a static shuttle and were wrong.
> Any "fix" still computes per-quad visibility every frame = the
> additive-inertia marathon the discipline forbids. Authoritative
> record: `memory/setuptextures_is_a_multiwriter_tangle_not_a_clean_
> shuttle.md`. The rest of this section is HISTORICAL. **Slice 2 (sec 3)
> is independent and remains the live deliverable; Slice 3 (sec 4) =
> prep doc only.**

### (historical) original Slice 1 text follows

**SCOPE CHANGE 2026-05-19 (user-ruled): DRAWALPHA-reservation deletion
DROPPED from this effort.** Reason: the planned pre-delete gate
(`legacy_drawalpha_detail_quads`==0) instruments the draw()-INTERNAL
DRAWALPHA site (quad.cpp:2716/2863/3039/3184), which is trivially 0 on
armed frames because `draw()` is skipped wholesale - it does NOT prove
the `addTerrainTriangles` reservation (quad.cpp ~688/704, runs every
armed frame via `setupTextures`) is pixel-dead, and txmmgr.cpp has LIVE
`MC2_ISTERRAIN & MC2_DRAWALPHA` terrain passes (~2196-2199/~2408-2410).
Deleting it on that evidence would be an assume-dead substitution = the
orphan risk that killed the prior campaign. The DRAWALPHA reservation
stays AS-IS (pre-existing, not a regression). See
`memory/drawalpha_counter_instruments_wrong_site.md`. Slice 1 is now
purely the recipe->member orphan-producer retirement.

**SCOPE CHANGE 2026-05-19 #2 (user-ruled): NARROW-A, honestly
downgraded.** The recon's "single recipe->member shuttle at 1004-1008,
sole consumer `draw`, compile-enforced sole-consumer proof" model is
FALSIFIED at HEAD: `setupTextures` is quad.cpp:720-2061 (~1340 lines),
the five members are written at >=6 sites (sentinel resets 741-746/
858-966; the legacy `!terrainTextures2` self-consuming SOLID/detail
branch 789-807/873-891; the recipe shuttle 1004-1008) and read by
`draw` (~40) AND a statically-dead in-`setupTextures` guard at 1410.
`draw` is NOT the sole consumer; the compile-enforce proof (former
review must-fix #1) CANNOT hold. See
`memory/setuptextures_is_a_multiwriter_tangle_not_a_clean_shuttle.md`.
Slice 1 is therefore re-scoped to NARROW-A:

- **Delete ONLY the five `recipe`->member assignments at
  quad.cpp:1004-1008.** Keep the `tryGetCachedTerrainRecipe` call /
  `recipe` local (still feeds `addTerrainTriangles(recipe)` - SOLID/
  cement contract, not in scope).
- **KEEP the five `TerrainQuad` fields** (the legacy `!terrainTextures2`
  branch, the sentinel resets, and the dead-1410 guard still reference
  them). NO field deletion, NO compile-enforce, NO privatize/rename.
- **Repoint every `TerrainQuad::draw` read of the five members
  (~40 reads, 2062-3309, bare-identifier `this->` access) to
  `getTerrainFaceCacheEntry(tileR,tileC)`.** Do NOT touch the legacy
  branch's self-consuming writes/reads (789-891), the sentinel resets,
  or the dead-1410 guard - they keep using the (retained) fields.

**Honest label:** this is a NARROW structural decoupling - `draw` reads
the immutable mission-load cache instead of per-frame-mutated quad
state - NOT the "META-FIX orphan-producer retirement" the recon framed,
NOT a perf claim (it removes 5 stores/quad/frame from a 1340-line walk
that otherwise stays). Value = decoupling/clarity, the user's
"simplify" goal, with zero behavior change. The former "net-shrink
Tracy" done-test downgrades to "no regression / negligible delta."

**The member consumers under Narrow-A (BOTH must be handled):**
1. `TerrainQuad::draw` member reads (~40, 2062-3309, bare-identifier)
   -> `getTerrainFaceCacheEntry(tileR,tileC)` (mapdata.cpp:232)
   `WorldQuadTerrainCacheEntry` (fields all present). Key on the STABLE
   `tileR/tileC`, NOT the camera-windowed `quadList` slot. Reuse
   `enqueueCachedTerrainTriangles(const WorldQuadTerrainCacheEntry&)`
   (quad.cpp:502) where a site is an enqueue.
2. The terrain.cpp drawPass hoist check (~1120,
   `currentQuad->terrainHandle == 0 && ->overlayHandle == 0xffffffff &&
   ->terrainDetailHandle == 0xffffffff` -> skip the quad). This runs on
   the `=0` revert path. After 1004-1008 is deleted, modern-path
   members are SENTINEL (0xffffffff) not recipe values, so this hoist's
   skip decision CHANGES (pure-water quads no longer hoist-skipped;
   they fall through to `draw()` which must early-out internally). The
   implementer MUST resolve this explicitly: either repoint the hoist
   to read the cache entry (behavior-preserving), OR prove `draw()`'s
   internal early-out is exactly behavior-equivalent for the formerly-
   hoist-skipped quads. The `=0`-revert smoke + visual parity is the
   load-bearing gate for this (that is the path where `draw()` and the
   hoist actually run; armed-default `draw()` is skipped wholesale).

**Do NOT touch:** the legacy `!terrainTextures2` self-consuming writes/
reads (789-891), the sentinel resets, the dead-1410 guard, the DRAWALPHA
reservation, the (c) water block (Slice 2), `tryGetCachedTerrainRecipe`/
`recipe`/`addTerrainTriangles`, pure-cement SOLID emit. KEEP the 5
fields. The plan must enumerate KEEP vs DELETE per statement.

**Done-criterion (Narrow-A, honestly downgraded):** the 5 recipe->member
assignments at 1004-1008 are DELETED; every `draw` member read + the
terrain.cpp hoist consumer are repointed/proven-equivalent;
default-armed AND `=0`-revert render IDENTICALLY (visual parity,
USER-observed - the `=0` path is load-bearing here); class-3 cement
untouched; clean non-COST_SPLIT total-frame Tracy shows NO regression
(negligible delta expected - this removes 5 stores/quad from a
1340-line walk; it is a decoupling, not a measurable win - do NOT claim
"net-shrink"). Reject capped-FPS / per-quad-chrono / cost-split
absolutes (`cost_split_instrumentation_is_observer_effect_dominated.md`).

## 3. Slice 2 - collapse the water 6-tuple onto slimReduce (NOW)

The probe is DIVERGENT: the `setupTextures` water block contributes
unique extrema (water elevation differs from terrain Z under the oblique
projection), feeding `eye->setInverseProject` (cursor->ground / move /
camera / cull inverse projection). It is NOT redundant; a free delete
was never available.

**Add** an UNCONDITIONAL water-tile-Z visit inside `slimReduce`: for
vertices with `pVertex->water & 1`, also fold the water-elevation Z into
the SAME min/max accumulators (one branch per already-visited vertex; no
second pass, no second `projectForTerrainAdmission`). **Delete** the
`setupTextures` water-projection reduction block, making `slimReduce` the
literal sole producer of the 6-tuple (the two-writer A..B span is
code-asserted; verify at plan time).

**Exact-equivalence constraint (review must-fix, Slice-2).** The new
slimReduce water visit MUST reuse the EXACT same admission predicate,
water-elevation source, projection helper, and accumulator-update
semantics as the old `setupTextures` water block - the ONLY change is
co-location inside the existing per-vertex visit. The plan must prove
**cardinality-equivalence**: the set of water extrema candidates the new
visit enumerates is identical to the old block's (same vertices, same
per-vertex contributions), not merely that the probe happens to flip to
`identical`. The probe is the empirical gate; the cardinality proof is
the structural gate - both required.

**Done-criterion:** re-run `MC2_WATER_INVPROJ_PARITY` -> must flip to
`result=identical` (extended slimReduce reproduces what the water block
produced) AND clean total-frame Tracy shows the water-reduction zone gone
with slimReduce rising at most one branch/water-vertex (net neutral-or-
negative). `setInverseProject` consumers unchanged (already read the
slimReduce-fed globals).

## 4. Slice 3 - SSBO-authoritative lighting repoint (PREP/SPEC ONLY)

NOT executed this effort. `CopyResultsToVertexPool` (gos_terrain_
lighting.cpp:834, the O(N)x4 scatter at ~895-905) is load-bearing: the
armed indirect thin-record packer (quad.cpp:2389) and water-overlay
(quad.cpp:2465) re-read the scattered legacy `Vertex*` pool. Real
armed-draw-path blast radius.

**Prep deliverable (in this spec, for the follow-on plan):** the
end-state is the indirect packer + water-overlay sourcing lighting from
the GPU lighting SSBO ring directly via the existing non-blocking 3-slot
tryConsume, deleting the CPU scatter while KEEPING the BAR->DRAM-shadow
sequential indirection (the anti-write-combining trap; never random WC
reads, never `glGetBufferSubData` on hot path). **Vulkan-prep upfront
(user-mandated):** design the SSBO consumer binding as explicit
device-mediated binding (`device.bindShaderStorageBuffer(...)`, no
implicit cross-call GL state) from the start, per
`memory/vulkan_prep_explicit_device_discipline.md` - prep, not a full
RHI. Carve-out: must not remove the legacy raster path that still
produces class-3 cement.

## 5. Hard carve-out (named, NOT solved here)

Class-3 pure cement (`overlayHandle == 0xffffffffu`,
gos_terrain_indirect.cpp:3881; skipped by the decal-bake) is rendered
ONLY by the SOLID indirect path. Unchanged at HEAD; NOT broken. It is
the rock the continuous-surface campaign died on. Slices 1-3 are
constructed to NOT remove SOLID-emit for cement. Resolving the
`useCementAtlas!=0` contract is a FUTURE precondition for any deeper
SOLID retirement - named only, do not re-walk (user already pinned;
`feedback_do_not_redisambiguate_already_pinned_subsystem`).

**Slice-3 prep is interface-shape ONLY (review).** The Vulkan-prep
binding design is a deliverable for the follow-on plan; it MUST NOT leak
into Slice 1 or Slice 2 acceptance - those two ship on their own gates
regardless of Slice-3 prep state.

## 6. Sequencing & ops

- Order: Slice 1 -> Slice 2 (independent; 1 first - largest, lowest
  risk, consumer already default-dead). Slice 3 prep-only.
- Atomic commits per slice (bisectable). Subagent-driven execution per
  the worktree discipline; main agent runs long builds backgrounded.
- Build RelWithDebInfo + full relink + v0.4 deploy before every smoke.

### Slice-1 acceptance gates (NARROW-A; ALL required, in order)
1. Build RelWithDebInfo + full relink (fields KEPT - no compile-enforce
   gate; that proof was falsified at HEAD).
2. Both member consumers handled: every `draw` read repointed to the
   cache AND the terrain.cpp ~1120 hoist check repointed-or-proven-
   equivalent.
3. `MC2_TERRAIN_INDIRECT_OVERLAY=0` revert path visual + behavior
   parity - **LOAD-BEARING** (this is the path where `draw()` and the
   hoist actually execute; armed-default skips `draw()` wholesale so it
   exercises almost nothing of this change).
4. Default-armed path visual parity (regression check).
5. Cement canary scene/map visually unchanged (explicit canary, NOT
   "unchanged by reasoning").
6. Map-edge / camera-window boundary visual smoke on the `=0` path
   (tileR/tileC identity must hold; cache O(1) + resident).
7. Clean non-COST_SPLIT total-frame Tracy: NO regression (negligible
   delta expected - decoupling, not a measurable win; do NOT assert
   "net-shrink").

### Slice-2 acceptance gates (ALL required)
1. `MC2_WATER_INVPROJ_PARITY` flips to `result=identical`.
2. Cardinality-equivalence argued in the plan (same water extrema
   candidate set as the old block).
3. Cursor->ground / camera / move / cull smoke on a water-heavy map.
4. Clean Tracy: the old water-reduction zone is GONE and `slimReduce`
   picks up only the expected per-water-vertex branch cost (net
   neutral-or-negative).

## 7. Verification items

1. DRAWALPHA-reservation deletion DROPPED (user-ruled 2026-05-19). The
   `legacy_drawalpha_detail_quads` counter instruments the wrong site
   (draw()-internal, trivially 0 armed; not the `addTerrainTriangles`
   reservation) and txmmgr has live `MC2_DRAWALPHA` terrain passes - no
   valid dead-pixel proof exists, so the reservation stays as-is. Full
   rationale: `memory/drawalpha_counter_instruments_wrong_site.md`. If
   ever retired later it needs a renderer/visual dead-pixel proof, NOT
   the counter.
2. The user's `drawPass`-area flame-graph zone was a CLICK-LAUNCH
   default capture (user-clarified 2026-05-19), NOT a worst-case Tracy -
   confirmed NOT load-bearing for this design. On the default path that
   zone is structurally ~empty (deployed exe IS HEAD; the probe smoke
   proved it). The substitutive proof is the user-driven worst-case
   zoomed-out-big-map Tracy regardless. Disambiguate the label (likely a
   mislabeled neighbour: drawMine/drawLine/drawWater under
   `Terrain::render`) during the Slice-1 Tracy proof - cosmetic.
3. Re-pin every file:line at plan time (post-48ba0d8 drift).
