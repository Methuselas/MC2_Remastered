# quadSetupTextures Retirement - Direction-Setting Recon

> **SUPERSEDED 2026-05-19 by the recon-EXTENSION.** This doc's verdict
> ("PATCH (justified) / small NS2 de-dup / NS1 framing is inertia") is
> OVERTURNED. The recon-extension (fresh-context terrain-indirect +
> cpu-gpu-offload advisors, greybeard) found this doc MISSED that commit
> `60f2ef8` already made the drawPass kill default-ON: `TerrainQuad::draw`
> (sole consumer of the setupTextures member-shuttle) is ALREADY skipped
> on the default path, so the per-quad walk is a true ORPHAN PRODUCER ->
> retiring it is a code-vindicated **META-FIX**, not a patch. The
> DRAWALPHA "HIGH-risk Stage-2b vapor orphan" here is FALSIFIED (dead
> since `521d83a`, pixel-suppressed). The `MC2_WATER_INVPROJ_PARITY`
> probe was user-authorized and RUN: result **DIVERGENT** -> (c) is a
> joint re-home into slimReduce, not the free-delete this doc hoped for.
> Authoritative current state: the HANDOFF memory STATUS block +
> `memory/water_invproj_parity_is_DIVERGENT_not_freebie.md` + the
> approved orphan-walk-retirement spec. Read those, not this verdict.


Date: 2026-05-19
Worktree: `claude/nifty-mendeleev` (HEAD 7c0defc)
Status: RECON ONLY. No code, no build, nothing committed. This document
informs a brainstorming design; it does not propose an implementation.
Advisors adopted: `mc2-terrain-indirect-expert`, `mc2-cpu-gpu-offload-expert`.
Greybeard skill run (5-part ruling at the end).

All file:line citations grep-verified in the post-7c0defc tree at write-time.

---

## 0. Premise note (gate is satisfied; one prior-doc correction)

The user supplied a CLEAN total-frame Tracy timeline (NOT cost-split, NOT
capped-FPS, NOT per-quad chrono) placing `Terrain::geometry
quadSetupTextures` as a real substantial GameLogic-thread zone under
`Frame -> GameLogic -> Mission::update -> ... -> Terrain::geometry ->
Terrain::geometry quadSetupTextures`. Per the prompt this gate is NOT to be
re-litigated, and the code confirms the zone is real: it is a named
`ZoneScopedN("Terrain::geometry quadSetupTextures")` (terrain.cpp:1935)
wrapping a `for (i=0;i<numberQuads;i++) currentQuad->setupTextures()` loop
(terrain.cpp:1975-2004) plus several per-frame compute-dispatch trio calls.

Correction to the dispatch prompt: the cited prior plan
`docs/superpowers/plans/2026-05-16-terrain-recipe-duplication-retirement.md`
does NOT exist by that name. The actual terminal ruling lives in
`docs/superpowers/specs/2026-05-19-terrain-surface-architecture-reevaluation.md`
(the doc that STOPPED the continuous-surface campaign) which cites the
terminal-CPU memory `cost_split_instrumentation_is_observer_effect_dominated.md`
(2026-05-16). This recon reconciles with THAT (Section 3), since it is the
binding ruling, and the substance the prompt describes is identical.

---

## 1. What `quadSetupTextures` actually does per-frame

The Tracy zone (terrain.cpp:1935) brackets TWO distinct kinds of work:

### 1.A The compute-dispatch trio (NOT per-quad; already GPU)

Before the quad loop, inside the zone (terrain.cpp:1940-1956):

- `gos_terrain_indirect::ComputePreflight()`
- `gos_terrain_lighting::BeginFrame()` / `PackAndDispatch()` /
  `CopyResultsToVertexPool(quadList, numberQuads)`
- `gos_terrain_indirect::ComputeDispatch()`
- `WaterStream::BeginFrameNarrow()`

These are O(frame) GPU-dispatch orchestration, not per-quad CPU recompute.
They are NOT the retirement target; they are the *already-shipped* offload.
Their presence inside this Tracy zone means the zone's wall time is not a
pure measure of the per-quad CPU loop - a chunk is GPU submit / ring
management. (Relevant when scoring "the zone -> ~0": part of it is
irreducible dispatch.)

### 1.B The per-quad `setupTextures()` loop (the actual CPU body)

`TerrainQuad::setupTextures` (quad.cpp:720) per quad does:

(a) **Recipe resolution -> member assignment.** Under Shape-C
(`s_shapeCEnabled`, default-ON, quad.cpp:312-317) the path is
`tryGetCachedTerrainRecipe(cachedEntry, recipe)` (quad.cpp:649-661): an
**O(1) struct copy** out of the mission-load-baked
`WorldQuadTerrainCacheEntry` (terrainHandle / terrainDetailHandle /
overlayHandle / uvData / isCement / isAlpha). Fallback
`buildTerrainRecipeInline` (quad.cpp:620-645) only runs when the cache
miss/disabled. The cache itself is built ONCE at mission load by
`MapData::buildTerrainFaceCache` (mapdata.cpp:245-316) -> the per-frame
recompute of texture handles / uv / cement classification is **already
retired**. The remaining per-frame cost here is: a `posTile` decode, a
`getTerrainFaceCacheEntry` index (mapdata.cpp:232), an
`ensureTerrainFaceCacheEntryResident` residency touch (quad.cpp:982),
six member-field stores, and `addTerrainTriangles`.

(b) **`addTerrainTriangles(recipe)`** (quad.cpp:668-718): the SOLID
sub-emit is gated OFF when indirect SOLID is armed
(`BeginLegacySolidCluster()` returns `!IsFrameSolidArmed()`,
quad.cpp:251-253). The DRAWALPHA detail-overlay sub-emit is **NOT**
gated (quad.cpp:686-690, 702-706) - those `addTriangleBulk(detail,
DRAWALPHA, 2)` calls run every armed frame and are real per-frame
master-array reservation work.

(c) **The water-projection block** (quad.cpp:1037-1287, the
`CostSplitWaterVertProjScope` body): for every quad touching water
(`pVertex->water & 1`), per vertex it calls
`eye->projectForTerrainAdmission(vertex3D, screenPos)`
(quad.cpp:1076/1146/1216/1286), writes `clipInfo`, and conditionally
contributes to the file-scope `leastZ/mostZ/leastW/mostW/leastWY/mostWY`
6-tuple (quad.cpp:1099-1122 etc.). The per-vertex `wx/wy/wz/ww` screen
writes are gated by `legacyWaterDraw = !WaterFastPathOwnsArmedDraw()`
(quad.cpp:761, 1089-1095) - i.e. SKIPPED when the water fast path owns
the armed draw. But the projection call, `clipInfo`, `calcThisFrame|=2`,
and the 6-tuple reduction stay UNCONDITIONAL (the CONCERN-1 boundary,
quad.cpp:755-760).

(d) Mine-state legacy reservation `enqueueTerrainMineState(*this)`, gated
off when `IsFrameMineArmed()` (quad.cpp:1024-1026).

**Redundant (already deletable / already gated):**
- Per-frame texture-handle / uv / cement recompute: ALREADY banked by
  Shape-C `buildTerrainFaceCache`. The per-frame path is an O(1) cache
  fetch. (Confirms the prior terminal ruling - see Section 3.)
- SOLID `addTriangleBulk` and legacy mine reservation: ALREADY armed-gated
  off.
- `wx/wy/wz/ww` water screen writes: ALREADY gated off when the water fast
  path owns the armed draw.

**Genuinely per-frame-necessary residue (WHY it is per-frame):**
- The water-vertex `eye->projectForTerrainAdmission` + `clipInfo` +
  `leastZ/mostZ/...` 6-tuple. It is per-frame because it depends on the
  **camera** (`eye`) which moves every frame; the projected screen-space
  extrema cannot be derived from static world-AABB bounds under the oblique
  cinematic projection (perspective divide does not preserve ordering -
  this is stated verbatim at terrain.cpp:1625-1630). This feeds
  `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)`
  (terrain.cpp:2115), which drives cursor->ground / move / camera / cull
  inverse projection.
- The un-gated DRAWALPHA detail-overlay `addTriangleBulk` reservations.

### 1.C The critical structural fact: slimReduce already owns the projection

`Terrain::geometry slimReduce` (terrain.cpp:1682) is a **separate Tracy
zone that runs BEFORE the quadSetupTextures loop in the same
`Terrain::geometry`**. Its own comment (terrain.cpp:1622-1651) states it is
the **proven sole producer of BOTH the cull cascade AND the
leastZ/mostZ/... reduction** (VPL Step 8c: "the slim reduce loop below is
the proven sole producer ... bit-identity proof"). It already owns the
per-vertex `projectForTerrainAdmission`.

So the setupTextures water-projection block's contribution to the 6-tuple
is **a SECOND writer of an already-produced reduction** - exactly the
multi-source-reduction hazard the offload advisor flags
(`<known_pitfalls>`: "Multi-source shared-state writers block isolated GPU
port"; Phase 1 Q4). The tree already contains an instrumented probe for
exactly this question: `MC2_WATER_INVPROJ_PARITY` takes snapshot A of the
6-tuple strictly after slimReduce and before quadSetupTextures
(terrain.cpp:1920-1932) and snapshot B strictly before setInverseProject
(terrain.cpp:2065+, divergence prints at :2095-2108). **Whether the
setupTextures water block contributes UNIQUE extrema (water-elevation Z
differs from terrain Z) or is fully redundant with slimReduce is an OPEN,
ALREADY-INSTRUMENTED empirical question** - it must be answered by running
that probe before any retirement is designed. If the probe shows zero
divergence, the entire 6-tuple contribution of the water block is
redundant and the genuinely-necessary residue collapses to almost nothing.

---

## 2. Consumer + repoint map

`quadSetupTextures` populates, per `TerrainQuad`: `terrainHandle`,
`terrainDetailHandle`, `overlayHandle`, `uvData`, `isCement`, plus the
per-vertex `clipInfo` / `wx/wy/wz/ww` / `calcThisFrame` and the file-scope
6-tuple; and it enqueues triangle reservations.

| Consumer | What it consumes | Substitutive repoint target | Orphan risk |
|---|---|---|---|
| `TerrainQuad::draw` solid/detail emit (quad.cpp:2062+, reads `terrainHandle`/`terrainDetailHandle`/`uvData`/`isCement` quad.cpp:2064-2136) | recipe member fields | The Shape-C `WorldQuadTerrainCacheEntry` is already the source; `draw` could read the cache entry directly instead of the member fields setupTextures copies into. Clean repoint. | LOW |
| `TerrainQuad::draw` overlay/decal producer (`gos_PushTerrainOverlay`, quad.cpp:2475-2803) - the LIVE cement-transition / runway / road decal path | `overlayHandle` / `isCement` | Shape-C cache entry (`overlayHandle`, `isCement()`, `isAlpha()`). Clean repoint for **class-2 alpha-cement** (decal-bake owned). | MEDIUM - see below |
| Water draw / inverse projection (`eye->setInverseProject`, terrain.cpp:2115) | `leastZ/mostZ/leastW/mostW/leastWY/mostWY` 6-tuple | **slimReduce already produces this** (terrain.cpp:1682, sole-producer proof). Repoint = delete the water-block reduction IF the `MC2_WATER_INVPROJ_PARITY` probe shows zero divergence. If it shows divergence, the water block is a genuine unique writer and CANNOT be deleted without porting the water-elevation-Z extrema into slimReduce (joint-port, offload-advisor option a). | MEDIUM - gated on probe result |
| Water fast path narrow append (terrain.cpp:1978-2001) | reads `waterHandle` set by setupTextures, OR the water-tile predicate when armed | Already has an armed-mode predicate that does NOT need `waterHandle` (terrain.cpp:1987-1995). Clean. | LOW |
| Legacy `addTriangleBulk` master-array reservations | SOLID gated off armed; DRAWALPHA detail NOT gated | Indirect path owns SOLID; the DRAWALPHA detail-overlay reservation has **no current indirect equivalent** (Stage 2b indirect overlay packer is documented vapor - `indirect_overlay_packed_quads` is a placeholder counter never incremented; terrain-indirect advisor `<known_pitfalls>` "Indirect overlay packer is vapor"). | HIGH (see below) |
| Mine state | `enqueueTerrainMineState` armed-gated off | Indirect mine path owns it when armed. Clean. | LOW |

### Orphan / cement-class risk (the rock the prior campaign died on)

**Class-3 PURE cement** (`isCement && !isAlpha`, `overlayHandle ==
0xffffffff`): per `memory/cement_is_part_of_the_decal_overlay_bake.md`,
pure cement is **NOT in the decal-bake** (the bake skips
`overlayHandle==0xffffffff`). Historically it was rendered by the legacy
per-quad / Fix-B SOLID terrain path as a flat base texture; the
continuous-surface design sources it via the `useCementAtlas != 0`
cement-catalog-atlas path - a SEPARATE mechanism. The prior
continuous-surface campaign died on exactly this open functional
regression (C-1 in the reevaluation doc). Any setupTextures retirement that
removes the SOLID-emit / member-field path for cement quads inherits the
**unresolved class-3 cement contract** as a hard precondition.

**Detail-overlay DRAWALPHA reservation orphan:** the un-gated
`addTriangleBulk(detail, DRAWALPHA, 2)` in `addTerrainTriangles` has no
armed indirect substitute (Stage 2b vapor). Retiring the setupTextures
loop without first implementing the indirect overlay packer would orphan
the detail-overlay layer the same way commit `9964d5a` killed all decals.

---

## 3. Reconciliation with the prior TERMINAL ruling

The binding prior ruling is the 2026-05-19
`terrain-surface-architecture-reevaluation.md` (which STOPPED the
continuous-surface campaign), resting on the terminal-CPU memory
`cost_split_instrumentation_is_observer_effect_dominated.md` (2026-05-16).
Its core claims, confronted with the code + the clean Tracy:

**Claim: "the retireable per-quad recipe work was ALREADY moved to
mission-load by Shape-C; the per-frame consumer is O(1)."**
-> **CONFIRMED by code.** `tryGetCachedTerrainRecipe` (quad.cpp:649-661) is
an O(1) struct copy from `buildTerrainFaceCache` output (mapdata.cpp:245).
The recipe-DUPLICATION sub-part of quadSetupTextures is genuinely already
retired. The clean Tracy does NOT resurrect it.

**Claim: "there is NO substitutive CPU-side per-quad terrain win
remaining; do NOT re-open without a fresh clean non-COST_SPLIT total-frame
Tracy that independently re-establishes a dominant terrain zone."**
-> The gate's *precondition* (a clean non-COST_SPLIT total-frame Tracy
re-establishing the zone) is, per the prompt, now **SATISFIED** - that
exact capture was the thing the reevaluation doc said "was never
produced." So the gate that the terminal ruling itself set up as the
re-open condition has been met. **However**, the terminal ruling's
*substance* is only partially superseded:

- The **recipe-duplication / texture-handle / cement-classification**
  sub-part: terminal ruling **still bites**. It is genuinely O(1) and
  already banked. A retirement framed as "delete the redundant per-frame
  recipe recompute" would net ~0 because there is no per-frame recompute -
  it is already a cache fetch. Saying otherwise would be inertia.
- The **genuinely-per-frame water-projection / 6-tuple residue**: this is
  a DIFFERENT, NARROWER thing the terminal ruling did NOT scope. But it is
  ALSO largely already owned by slimReduce (terrain.cpp:1682 sole-producer
  proof). The setupTextures water-block contribution is, at best, a
  redundant second writer (probe `MC2_WATER_INVPROJ_PARITY` exists to
  decide this) and at worst a small unique-extrema contribution.
- The **un-gated DRAWALPHA detail-overlay reservation**: genuinely
  per-frame, NOT covered by the terminal ruling, but blocked by the
  vaporware Stage 2b indirect overlay packer (terrain-indirect advisor).

**Honest verdict on the prior ruling:** it is **mostly still binding, not
superseded**, but for a more precise reason than "no clean Tracy." The
clean Tracy is real and the zone is real - but the code shows the zone's
substance is (i) compute-dispatch orchestration that is the *already
shipped* offload (not a target), (ii) an O(1) cache fetch (terminal ruling
correct, already banked), and (iii) a projection residue that **slimReduce
already owns as the proven sole producer**. The one real prior terrain CPU
substitutive win (drawPass/decal-bake retirement, `51039af`/`60f2ef8`)
was already banked. quadSetupTextures-as-named cannot bank a second
substitutive CPU win because its retireable substance was already retired
upstream and its residue is already produced elsewhere. The clean Tracy
re-establishes that the *Tracy zone wall-time* is non-trivial, but a large
fraction of that wall-time is GPU-dispatch orchestration + an already-O(1)
fetch, not deletable CPU recompute. **Do not inertia-justify a "delete the
recompute" framing; there is no recompute to delete.**

---

## 4. Approaches for the genuinely-necessary residue

The genuinely-necessary residue, post-redundancy-strip, is small and
two-part: (R1) the water-vertex projection + 6-tuple, (R2) the un-gated
DRAWALPHA detail-overlay reservation. Approaches assume R1 is the focus
(R2 is blocked on Stage 2b and is a separate, prerequisite slice).

**Approach (i): Collapse R1 into slimReduce (re-home, not GPU port).**
slimReduce is already the proven sole producer of the 6-tuple
(terrain.cpp:1682). Extend it to also visit water-elevation-Z for
water-tile vertices, then DELETE the setupTextures water-projection
reduction entirely. *Critical-path effect:* removes the redundant second
projection pass; the quadSetupTextures loop loses its heaviest residue.
*Consumer repoint:* `setInverseProject` already reads the slimReduce-fed
globals; no consumer change. *Vulkan-prep:* pure CPU re-home, no new GPU
state, aligns with the existing slim/cull discipline. *Done-criterion:*
the `MC2_WATER_INVPROJ_PARITY` probe (already in tree) shows zero
divergence post-merge AND the quadSetupTextures Tracy zone drops on a clean
total-frame capture with no displaced cost into slimReduce beyond the
water-Z visit. **Precondition: run the probe FIRST** - if the water block
already produces zero unique extrema, this approach is nearly free
(just delete the dead second writer). RECOMMENDED if probe shows
redundancy or near-redundancy.

**Approach (ii): GPU compute port of R1.** Port the water-vertex
projection to a compute shader (Phase 1 `gos_terrain_lighting` pattern as
template, offload advisor). *Critical-path effect:* removes CPU
projection. *Consumer repoint:* the 6-tuple is a reduction -> needs a GPU
reduction + a non-blocking 3-slot tryConsume readback
(`gpu_cull_readback.cpp` pattern; NEVER `glGetBufferSubData` on hot path -
substrate sync-stall lesson). *Multi-source hazard:* slimReduce ALSO writes
the 6-tuple on CPU; a GPU port of only the water writers produces an
inconsistent reduction (offload advisor Phase 1 Q4 / D1 - the exact
documented blocker). Would require joint-porting slimReduce's reduction
too. *Verdict:* heavyweight, drags in the slimReduce reduction, readback
sync risk for a tiny residue. NOT recommended over (i).

**Approach (iii): Extend the mission-load bake.** Cannot apply to R1: the
projection is camera-dependent (terrain.cpp:1625-1630 explicitly: not
derivable from static bounds under oblique projection). Mission-load bake
is the right answer for static recipe data - and that is exactly what
Shape-C ALREADY did. There is nothing left to push to load time here.

**Recommendation:** **Approach (i)**, strictly gated behind running the
existing `MC2_WATER_INVPROJ_PARITY` probe first. If the probe shows the
setupTextures water block contributes zero unique extrema beyond
slimReduce, R1 is dead code and its removal is a clean structural
de-duplication (NS2), not a measured CPU win. If it shows unique extrema,
(i) becomes a small joint re-home into slimReduce. Either way the framing
is **NS2 structural de-duplication, not an NS1 CPU-offload win** - because,
per Section 3, the offload was already banked upstream. R2 is a separate
slice blocked on Stage 2b (indirect overlay packer) and the class-3 cement
contract; it must NOT be bundled.

---

## 5. Greybeard 5-part ruling

**1. Subsystem pin.** Pinned with first-hand code evidence: the
`Terrain::geometry quadSetupTextures` Tracy zone (terrain.cpp:1935) is NOT
one homogeneous CPU cost. It is (a) GPU-dispatch orchestration trio
(terrain.cpp:1940-1956), (b) an O(1) Shape-C cache fetch +
member-store + armed-gated reservations (quad.cpp:720, 649-661, 251-253),
(c) a camera-dependent water-projection + 6-tuple reduction
(quad.cpp:1037-1287) that is a SECOND writer of a reduction slimReduce
(terrain.cpp:1682) already proves it solely produces. The artifact
("quadSetupTextures is a big GameLogic zone") spans three subsystems;
attributing it wholesale to "redundant per-frame recipe recompute" is the
wrong pin and would fake a confirmed root cause.

**2. Symptom vs cause.** Symptom: a substantial named GameLogic Tracy
zone. Proximate cause of the *retireable* fraction: a redundant second
projection/reduction pass over water vertices that slimReduce already
owns, plus un-gated detail-overlay reservations with no armed substitute.
Upstream condition that makes the symptom *look* like recoverable
recompute: the recipe work was already moved to mission-load (Shape-C) but
the zone name and the campaign framing still imply per-frame recompute that
no longer exists.

**3. The meta-fix.** The single upstream change that retires the bug
*class*: **delete the setupTextures water-projection reduction and let
slimReduce be the literal sole producer of the 6-tuple** (it is already
proven to be the sole producer for the non-water path; the water block is
the last redundant second writer). This makes "two writers of one
reduction can silently diverge / one blocks the other's port" impossible
by construction. It is a structural de-duplication (NS2), NOT an
NS1 CPU-offload win - the offload was already banked by Shape-C +
drawPass/decal-bake retirement (`51039af`/`60f2ef8`). Blast radius:
`eye->setInverseProject` consumers (cursor->ground / move / camera / cull
inverse projection); contained because the consumer already reads the
slimReduce-fed globals and the `MC2_WATER_INVPROJ_PARITY` probe already
exists to prove equivalence. The class-3 cement contract and the Stage 2b
overlay-packer vapor are PRE-EXISTING orphan blockers for the
detail-overlay residue (R2) - that residue must be carved out, not bundled.

**4. Substitutive test.** Done = the setupTextures water-projection
reduction is DELETED (not bypassed) and slimReduce is its literal sole
producer, proven by `MC2_WATER_INVPROJ_PARITY` zero-divergence on a clean
total-frame capture, with no displaced cost (slimReduce gains at most a
water-Z visit, net negative or neutral). Additive (keeping both writers
behind a flag) = NOT done. **But honesty requires stating:** the
substitutive CPU-zone-death test that the campaign framing reaches for is
the WRONG test here, for the same reason the reevaluation doc identified
for the continuous-surface campaign - the CPU substance was already banked
upstream. The correct test for THIS slice is the NS2 de-duplication
criterion above, not "quadSetupTextures Tracy zone -> 0" (a large fraction
of that zone is irreducible GPU-dispatch orchestration + an already-O(1)
fetch and will NOT go to zero no matter what).

**5. Verdict.**

`PATCH (justified)` - shading toward NEITHER if mis-framed.

The honest meta-fix (slimReduce as literal sole 6-tuple producer; delete
the redundant water-block reduction) is REAL and worth doing, but it is a
**small NS2 structural de-duplication**, NOT the NS1 CPU-offload retirement
the prompt's framing ("eliminate quadSetupTextures, re-home the necessary
residue off the GameLogic critical path") implies. There is no large
deletable per-frame recompute: Shape-C already retired it. Framing a
"quadSetupTextures retirement" as an NS1 substitutive CPU win **is
inertia** - the same inertia the 2026-05-19 reevaluation doc named when it
STOPPED the continuous-surface campaign for invoking a CPU-zone-death test
against a target with no remaining CPU substance.

Mandatory deferred-debt fields:
- **Named meta-fix:** delete the setupTextures water-projection reduction;
  slimReduce becomes the literal sole producer of
  leastZ/mostZ/leastW/mostW/leastWY/mostWY.
- **Deferral / scoping reason:** must be (a) gated behind running the
  already-present `MC2_WATER_INVPROJ_PARITY` probe to establish whether the
  water block contributes unique extrema at all - if zero divergence the
  meta-fix is nearly free and should proceed as a clean NS2 slice; if
  divergence, it is a small joint re-home into slimReduce; (b) explicitly
  RE-LABELED NS2 structural de-duplication, not NS1 CPU-offload, in any
  resulting plan (per the reevaluation doc's mandate that future
  terrain-surface work be honestly NS2-chartered); (c) the DRAWALPHA
  detail-overlay residue (R2) and any cement-class-3 path explicitly
  CARVED OUT - they are blocked on the vaporware Stage 2b indirect overlay
  packer and the unresolved class-3 cement contract, the exact rock the
  prior campaign died on.

**One-line verdict:** PATCH (justified) - the only honest, bankable change
here is a small NS2 slimReduce sole-producer de-duplication gated on an
existing probe; the NS1 "retire quadSetupTextures for a CPU win" framing is
inertia and must be rejected, because Shape-C + the drawPass/decal-bake
retirement already banked the only real terrain CPU substitutive win.
