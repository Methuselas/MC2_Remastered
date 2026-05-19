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
| (b) `setupTextures` recipe->member shuttle (quad.cpp:1004-1008, `CostSplitRecipeCacheScope` ~987) | Consumer `draw` default-dead via 60f2ef8 | Slice 1: DELETE walk, repoint consumers to cache |
| DRAWALPHA detail reservation (`addTerrainTriangles` quad.cpp:668; dead since 521d83a, quad.cpp:2409) | Zero pixels; pure dead enqueue | Slice 1: DELETE reservation |
| (c) water-projection 6-tuple (quad.cpp water block; second writer) | Probe DIVERGENT - contributes UNIQUE extrema | Slice 2: joint re-home into slimReduce |
| (a) `CopyResultsToVertexPool` scatter (gos_terrain_lighting.cpp:834) | LOAD-BEARING - armed indirect packer (quad.cpp:2389) + water-overlay (quad.cpp:2465) re-read scattered pool | Slice 3 (PREP only) |

## 2. Slice 1 - retire the orphan producer walk (NOW)

**Delete** the per-quad member-store walk: in `setupTextures`
(quad.cpp), the `recipe`->member assignments (`isCement`/`terrainHandle`/
`terrainDetailHandle`/`overlayHandle`/`uvData` at quad.cpp:1004-1008) and
the dead DRAWALPHA `addTriangleBulk(detail, DRAWALPHA, 2)` path in
`addTerrainTriangles` (quad.cpp:668+).

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

## 6. Sequencing & ops

- Order: Slice 1 -> Slice 2 (independent; 1 first - largest, lowest
  risk, consumer already default-dead). Slice 3 prep-only.
- Atomic commits per slice (bisectable). Subagent-driven execution per
  the worktree discipline; main agent runs long builds backgrounded.
- Per-slice gate: build RelWithDebInfo + full relink + v0.4 deploy, then
  20s 1-2-mission `--keep-logs` smoke (user ops; breakage immediately
  obvious). User is along to drive the worst-case zoomed-out-big-map
  clean total-frame Tracy (the substitutive proof) and visual canary.
- Slice 2 extra gate: `MC2_WATER_INVPROJ_PARITY` re-run -> `identical`.

## 7. Verification items (not blockers)

1. Confirm `Counters_GetLegacyDrawAlphaDetailQuads()` == 0 on an armed
   parity-summary log (load-bearing proof DRAWALPHA is truly dead before
   deleting it).
2. The user's flame graph still shows a `drawPass`-area zone; on the
   default path that zone is structurally ~empty (deployed exe IS HEAD -
   the probe smoke proved it). Disambiguate the label (likely a
   mislabeled neighbour: drawMine/drawLine/drawWater under
   `Terrain::render`) during the Slice-1 Tracy proof - not a blocker.
3. Re-pin every file:line at plan time (post-48ba0d8 drift).
