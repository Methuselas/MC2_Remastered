# quadSetupTextures Orphan-Walk Retirement - Design (approved)

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
| (b) `setupTextures` recipe->member shuttle (quad.cpp:1004-1008, `CostSplitRecipeCacheScope` ~987) | Consumer `draw` default-dead via 60f2ef8 | Slice 1: delete the recipe->member ORPHAN PRODUCER + repoint consumers to cache (NOT a full-walk-removal claim - residue stays) |
| DRAWALPHA detail reservation (`addTerrainTriangles` quad.cpp:668; dead since 521d83a, quad.cpp:2409) | Zero pixels; pure dead enqueue | Slice 1: DELETE reservation |
| (c) water-projection 6-tuple (quad.cpp water block; second writer) | Probe DIVERGENT - contributes UNIQUE extrema | Slice 2: joint re-home into slimReduce |
| (a) `CopyResultsToVertexPool` scatter (gos_terrain_lighting.cpp:834) | LOAD-BEARING - armed indirect packer (quad.cpp:2389) + water-overlay (quad.cpp:2465) re-read scattered pool | Slice 3 (PREP only) |

## 2. Slice 1 - retire the recipe->member orphan producer + dead DRAWALPHA (NOW)

Target wording (false-victory guard, per review must-fix #3): "delete
the recipe->member orphan producer and the DRAWALPHA dead reservation;
do NOT claim full `setupTextures` walk removal until the residue is
separately fused or retired." The honest done-test is "zone net-shrinks
with no displaced cost," not "zone gone."

**Delete** the per-quad member-store assignments: in `setupTextures`
(quad.cpp), the `recipe`->member assignments (`isCement`/`terrainHandle`/
`terrainDetailHandle`/`overlayHandle`/`uvData` at quad.cpp:1004-1008) and
the dead DRAWALPHA `addTriangleBulk(detail, DRAWALPHA, 2)` path in
`addTerrainTriangles` (quad.cpp:668+).

**Compile-enforce the sole-consumer proof (review must-fix #1).** The
plan MUST enumerate every reader of the five `TerrainQuad` fields
(`grep -n "->\(isCement\|terrainHandle\|terrainDetailHandle\|overlay
Handle\|uvData\)" mclib/*.cpp GameOS/**/*.cpp`). Then:
- If ALL readers are repointed to the cache entry: DELETE the five
  fields from `TerrainQuad` outright (the strongest proof - any
  surviving/reintroduced reader becomes a compile error).
- If any armed-path residue genuinely still needs a field: privatize /
  rename it (e.g. `terrainHandle_RETIRED_USE_CACHE`) so every accidental
  or future read fails loudly at compile time, not silently on stale
  data.
Grep-only "draw is the sole consumer" is NOT sufficient; the retirement
must be mechanically enforced.

**DRAWALPHA deletion is a pre-gated render-path change (review must-fix
#2).** It is a real render-path deletion, not an optional cleanup.
Sequence, in order, no skipping:
1. Run an armed parity-summary smoke; confirm
   `Counters_GetLegacyDrawAlphaDetailQuads()` == 0 (telemetry proof the
   path is cold - substitutive, not assumptive).
2. Only then delete the DRAWALPHA reservation.
3. Re-run visual/parity smoke (default-armed + `=0` revert).

**Repoint** the two consumers to read the static Shape-C entry directly:
- `TerrainQuad::draw` member reads -> `getTerrainFaceCacheEntry(tileR,
  tileC)` (mapdata.cpp:232) returning `WorldQuadTerrainCacheEntry`
  (fields `terrainHandle`/`terrainDetailHandle`/`overlayHandle`/`uvData`/
  `isCement()` all present). Key is the STABLE `tileR/tileC`, NOT the
  camera-windowed `quadList` slot.
- The `MC2_TERRAIN_INDIRECT_OVERLAY=0` revert loop (terrain.cpp drawPass
  `if` branch) also reads the cache directly - serves BOTH paths, so
  SOLID-emit for cement is NOT removed (class-3 untouched).

Reuse opportunity to verify at plan time: `enqueueCachedTerrainTriangles
(const WorldQuadTerrainCacheEntry&)` (quad.cpp:502) already exists - a
cache-direct enqueue that may be the substitution machinery.

**Scope boundary (removes the ambiguity):** Slice 1 deletes ONLY the
recipe->member assignments + the DRAWALPHA sub-emit and repoints `draw`.
It does NOT delete `setupTextures` wholesale. Residue that STAYS (separate
concerns, not this slice): the posTile decode / `ensureTerrainFaceCache
EntryResident` residency touch; the water fast-path narrow-append
predicate; the (c) water-projection block (owned by Slice 2); any
armed-path SOLID/indirect emit. The plan must enumerate, per
`setupTextures` statement, KEEP vs DELETE before editing.

**Done-criterion (substitutive):** the `setupTextures` member-store walk
+ DRAWALPHA reservation are DELETED (not flagged/bypassed); on a clean
non-COST_SPLIT total-frame Tracy at worst-case zoomed-out-big-map the
`Terrain::geometry quadSetupTextures` zone net-shrinks with NO displaced
cost into `draw`/mission-load (the bake already exists); both default-
armed and `=0`-revert render identically; class-3 cement untouched.
Reject capped-FPS / per-quad-chrono / cost-split absolutes
(`cost_split_instrumentation_is_observer_effect_dominated.md`).

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

### Slice-1 acceptance gates (ALL required, in order)
1. Armed parity-summary smoke; `Counters_GetLegacyDrawAlphaDetailQuads()`
   == 0 BEFORE deleting the DRAWALPHA reservation (pre-delete gate).
2. Build RelWithDebInfo + full relink.
3. Stale-member retirement compile-enforced (fields deleted, or
   privatized/renamed so any reader fails to compile).
4. Default-armed path visual parity.
5. `MC2_TERRAIN_INDIRECT_OVERLAY=0` revert path visual parity.
6. Cement canary scene/map visually unchanged (explicit canary, NOT
   "unchanged by reasoning").
7. Map-edge / camera-window boundary visual smoke (tileR/tileC identity
   must hold at call sites - cache lookup O(1) + resident on BOTH the
   default and `=0` paths).
8. Clean non-COST_SPLIT total-frame Tracy at worst-case zoomed-out-big-
   map: `Terrain::geometry quadSetupTextures` net-shrinks with NO
   obvious displaced cost into `draw` or mission load.

### Slice-2 acceptance gates (ALL required)
1. `MC2_WATER_INVPROJ_PARITY` flips to `result=identical`.
2. Cardinality-equivalence argued in the plan (same water extrema
   candidate set as the old block).
3. Cursor->ground / camera / move / cull smoke on a water-heavy map.
4. Clean Tracy: the old water-reduction zone is GONE and `slimReduce`
   picks up only the expected per-water-vertex branch cost (net
   neutral-or-negative).

## 7. Verification items

1. (Promoted to Slice-1 gate #1 - the DRAWALPHA counter==0 pre-delete
   check is no longer "non-blocking.")
2. The user's `drawPass`-area flame-graph zone was a CLICK-LAUNCH
   default capture (user-clarified 2026-05-19), NOT a worst-case Tracy -
   confirmed NOT load-bearing for this design. On the default path that
   zone is structurally ~empty (deployed exe IS HEAD; the probe smoke
   proved it). The substitutive proof is the user-driven worst-case
   zoomed-out-big-map Tracy regardless. Disambiguate the label (likely a
   mislabeled neighbour: drawMine/drawLine/drawWater under
   `Terrain::render`) during the Slice-1 Tracy proof - cosmetic.
3. Re-pin every file:line at plan time (post-48ba0d8 drift).
