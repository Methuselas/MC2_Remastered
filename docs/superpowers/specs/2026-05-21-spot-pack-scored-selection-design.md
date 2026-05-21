# Per-Shape Light-Pack SpotLight_ Priority Lift — Design Spec

- **Status:** DRAFT v2 — addresses round-1 adversarial review (3 CRITICALs + 5 MAJORs)
- **Date:** 2026-05-21
- **Worktree:** `claude/nifty-mendeleev`
- **Slice ID:** (E') — extends (E) SpotLight_ real illumination; in (E)'s scope per greybeard ruling
- **Round-1 review verdict:** BLOCKING-CRITICAL (3 CRITICALs found). v1 was rewritten ground-up; this is v2.
- **Companion specs:** [(E) SpotLight_ retirement](2026-05-20-spotlight-real-illumination-design.md); (F) terrain lighting saturation (separate, orthogonal).
- **All file:line citations grep-verified at write time against `.claude/worktrees/nifty-mendeleev/`.**

---

## 1. What v1 got wrong (record so future readers don't repeat)

Round-1 adversarial review found 3 CRITICALs that made v1 unimplementable:

**C1 (v1) — `s_listOfLights[]` index space conflation.** v1 said "(E)'s lights at slots 18-67 are above the 16-cap." Wrong. `s_listOfLights[]` is the pointer to `eye->worldLights[]` set via [tgl.cpp:1655 `TG_Shape::SetLightList`](mclib/tgl.cpp). It carries the WHOLE world pool. FIFO walks all of it, breaks after 16 ACTIVE-pass-throughs in pool-iteration-order. The truncation IS real — but the cause is "first 16 active win in iteration order," not "slot index above cap."

**C2 (v1) — Caller survey missed 5+ sites + had false citations.** Round-1 reviewer ran a clean grep and found:
- `txmmgr.cpp:1338` and `txmmgr.cpp:1347` (LIGHTBRIDGE template path — both hit and miss invoke `GatherLightsParameters`)
- `mech3d.cpp:4636` (`mechShape->CacheGpuLightData()`)
- `genactor.cpp:1225` (`genShape->CacheGpuLightData()`)
- `gos_static_prop_batcher.cpp:2644` (fallback `firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()`)
- `bdactor.cpp:1897` and `:1904` (`shape->CacheGpuLightData()` — bldg static D2/MISS paths)
- v1's `gvactor.cpp` CacheGpuLightData citation was FICTIONAL — GVs only call `SetLightList`, not `CacheGpuLightData`. GVs reach the gather via the batcher fallback at `gos_static_prop_batcher.cpp:2644`.

**C3 (v1) — LIGHTBRIDGE template-cache regression.** v1's per-actor scored selection would have broken the FNV+memcmp template dedup at [txmmgr.cpp:1342-1357](mclib/txmmgr.cpp). The template key `sceneLightTemplateKey(actorLightSource)` does NOT currently include actor position. Per-actor scoring would force position into the key → unique-per-actor templates → cache hit rate → 0 → undoing the 8.5ms→0.4ms `addLightDataStructure` optimization landed in commit `996aff4`. v1 claimed "perf-neutral, ~20µs"; reality would have been an ~8ms regression.

Plus 5 MAJORs: tgl.cpp:1968 CPU vertex lighting also FIFOs `s_listOfLights[]` (divergent CPU/GPU if only one is fixed); frame ordering bug for moving mechs; K1=4 partition starves weapon-bolt POINTs; missing `std::partial_sort` specification; `light->active` filter re-introduces the camera-gate bug.

## 2. Correct symptom→cause framing

### What the data actually shows

T1.16 diagnostic on mc2_10: of the 54 (E)-owned slots in `worldLights[]`, only ~12-15% land `active=true` in `Camera::updateLights` summary windows. The 86%+ that fail are positionally-distant from the camera frustum. **Within active=true slots**, mc2_10 intro shows mech/GV bodies visibly NOT illuminating despite slot=36 mech and GV cluster 45/56-59 being in active state.

### The real bug class

`GatherLightsParameters` at [txmmgr.cpp:1561](mclib/txmmgr.cpp) walks `s_listOfLights[0..s_numLights-1]` (which equals `worldLights[0..numLights-1]`) and accumulates lights with `active==true` into the per-shape `TG_HWLightsData`. The walk breaks at the first 16 active-pass-throughs. **Selection criterion: pool iteration order × active-flag.** No relevance scoring, no priority, no shape-position awareness.

When `s_listOfLights[]` contains a mix of base-scene lights (AMBIENT slot 1, INFINITE slot 0, weapon-bolt POINTs at slots 2-N) plus (E)'s SpotLight_ POINTs (registered after via `addWorldLight`), the iteration-order is registration-order. (E)'s lights register at world-pool indices 18-67 because (E)'s `addWorldLight` happens during per-frame `Mech3DAppearance::update` / `GVAppearance::update` AFTER the scene's base lights at slots 0-17. So they appear at the END of the iteration order.

For shapes near (E) lights, the FIFO consumes its 16-slot budget on the EARLIER base-scene lights and never reaches the (E) spotlights, despite them being more relevant to the shape's local illumination.

### Why the fix is "priority lift," not "scored selection"

The data shows ~5-7 SpotLight_ POINTs active per frame in mc2_10's camera zone (T1.16 summary windows). All comfortably fit within the 16-cap if they're at the FRONT of the iteration order. Reordering `s_listOfLights[]` so SpotLight_-tagged POINTs come FIRST is sufficient to ensure they always survive truncation.

This preserves:
- LIGHTBRIDGE template hit rate (the reordered list is scene-state-dependent, not per-actor)
- `addLightDataStructure` dedup (every shape sees the same prefix-ordered list → same `TG_HWLightsData` for shapes with same active set)
- Per-shape CPU cost (no per-shape sort)

And accepts the limitation:
- All shapes get ALL active SpotLight_ POINTs, not the K-nearest. With 5-7 spotlights total and 16-slot cap, irrelevant spotlights still "fit." Bandwidth-wise: those spotlights contribute zero to shapes outside their `farDistance` (the shader's `GetFalloff` returns 0 → no visual artifact, just wasted slots).
- If/when content grows beyond ~12 simultaneously-active SpotLight_ POINTs, this approach starts truncating spotlights again. At that point we need scored selection (or clustered lighting). This is a future (F)-class problem with content-driven trigger; not a v1 concern.

## 3. Proposed META-FIX: SpotLight_ priority-front-insertion at SetLightList

### 3.1 Architectural shape

Modify `TG_Shape::SetLightList(TG_LightPtr* lightList, DWORD nLights)` at [mclib/tgl.cpp:1655](mclib/tgl.cpp) (or its callers — see §3.2 for the exact insertion site decision) to produce a reordered light list with SpotLight_-tagged POINT lights at the front, before any other POINT/SPOT/TERRAIN lights.

Stable ordering principle: **lights of higher priority class appear before lights of lower priority class.** Within a class, original iteration order preserved.

Priority classes (highest first):
1. **AMBIENT, INFINITE** — scene-global; always-active; cheap.
2. **SpotLight_-tagged POINT/SPOT** — (E)-registered or anubis-class actor-attached lights. Tagged by [mclib/spotlight_diag.h](mclib/spotlight_diag.h) `is_e_slot()` (already exists from T1.16 instrumentation).
3. **Other POINT/SPOT/TERRAIN** — weapon-bolt POINTs, per-building terrain ambients, anubis searchlight (TG_LIGHT_SPOT), unclassified.

The FIFO walk in `GatherLightsParameters` reaches class 1 first (always packed; budget ~2-4 slots), then class 2 (packed within the remaining 12-14 slots; in stock content fits comfortably), then class 3 fills remaining slots if any.

### 3.2 Where the reorder happens — decision

Two candidate sites:

**Option A: at `Camera::updateLights`** ([mclib/camera.cpp:1871](mclib/camera.cpp)) — after the active-flag pass, rebuild `worldLights[]` to put SpotLight_ tagged slots first. Affects EVERY caller of `eye->getWorldLights()` (via SetLightList plumbing). Single touchpoint.

**Option B: at each `SetLightList` call site** — every caller (15 sites per §3.3) builds its own reordered list before calling `SetLightList`. More duplication; allows per-actor reorder policies if ever needed (currently no).

**Recommendation: Option A.** Single touchpoint, no caller changes. Reordered `worldLights[]` flows naturally to every consumer.

### 3.3 Full caller survey (grep-verified, 2026-05-21)

`SetLightList(eye->getWorldLights(), eye->getNumLights())` callers — 15 sites:

| File | Line | Caller context | Affected? |
|---|---|---|---|
| `mclib/bdactor.cpp` | 2291 | `BldgAppearance::update` → bldgShape | YES (reordered list propagates) |
| `mclib/bdactor.cpp` | 2383 | `BldgAppearance::update` → bldgShadowShape | YES |
| `mclib/bdactor.cpp` | 4552 | `TreeAppearance::update` → treeShape | YES |
| `mclib/bdactor.cpp` | 4606 | `TreeAppearance::update` → treeShadowShape | YES |
| `mclib/genactor.cpp` | 1207 | `GenericAppearance::render` → `SetLightList(NULL, 0)` zaps | NEUTRAL (no list) |
| `mclib/gvactor.cpp` | 2530 | `GVAppearance::update` → gvShadowShape | YES |
| `mclib/gvactor.cpp` | 2541 | `GVAppearance::update` → gvShape | YES |
| `mclib/gvactor.cpp` | 2828 | sensorTriangleShape | YES |
| `mclib/gvactor.cpp` | 2834 | sensorCircleShape | YES |
| `mclib/mech3d.cpp` | 3551 | `Mech3DAppearance::updateGeometry` → mechShadowShape | YES |
| `mclib/mech3d.cpp` | 3587 | `Mech3DAppearance::updateGeometry` → mechShape | YES |
| `mclib/mech3d.cpp` | 3827 | sensorTriangleShape | YES |
| `mclib/mech3d.cpp` | 3833 | sensorSquareShape | YES |
| `mclib/mech3d.cpp` | 4715 | leftArm | YES |
| `mclib/mech3d.cpp` | 4799 | rightArm | YES |

Per Option A, ZERO of these sites need code change. They consume `eye->getWorldLights()` which transparently returns the reordered list after this slice lands.

### 3.4 GatherLightsParameters / CacheGpuLightData / GatherGpuObjectLightDataOnly call sites

For completeness (these are the downstream consumers of the reordered list — NO change needed per Option A):

| File | Line | Caller |
|---|---|---|
| `mclib/txmmgr.cpp` | 1338 | LIGHTBRIDGE no-actor-light passthrough |
| `mclib/txmmgr.cpp` | 1347 | LIGHTBRIDGE template-MISS path |
| `mclib/tgl.cpp` | 2644 | `GatherGpuObjectLightDataOnly` → `GatherLightsParameters` |
| `mclib/msl.cpp` | 1924 | `TG_MultiShape::CacheGpuLightData` → first SHAPE_NODE leaf → `GatherGpuObjectLightDataOnly` |
| `mclib/bdactor.cpp` | 1897 | `mc2CacheOrBakeStaticGpuLight` D2 legacy `shape->CacheGpuLightData()` |
| `mclib/bdactor.cpp` | 1904 | `mc2CacheOrBakeStaticGpuLight` static MISS `shape->CacheGpuLightData()` |
| `mclib/genactor.cpp` | 1225 | `GenericAppearance::render` `genShape->CacheGpuLightData()` |
| `mclib/mech3d.cpp` | 4636 | `Mech3DAppearance::render` `mechShape->CacheGpuLightData()` |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | 2644 | batcher fallback `firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()` (used by GVs and bldg static fallback path) |

These all consume the FIFO order of `s_listOfLights[]` after the priority lift, so they automatically get the desired behavior. **The fix is in ONE function, with a small data structure on the camera side.**

### 3.5 LIGHTBRIDGE template-cache audit (response to C3)

The template key at [txmmgr.cpp:1018-1019](mclib/txmmgr.cpp) `sceneLightTemplateKey` is `FNV1A(actorLightSource, ...)`. It does NOT include actor position. After the priority lift:
- `s_listOfLights[]` ordering changes once per frame (when `Camera::updateLights` runs)
- All shapes within a frame see the SAME reordered list
- All shapes that compute the same template key still hit the same template entry
- Cache hit rate UNCHANGED

The reordered list might mean different lights are at slots 0-15 than before. The `TG_HWLightsData` contents differ from the pre-(E') world, but they're consistent across same-template shapes within a frame.

**No template-cache regression.** This is the key correctness property of Option A.

### 3.6 `addLightDataStructure` dedup audit

The downstream dedup at [txmmgr.cpp:1283-1322](mclib/txmmgr.cpp) `addLightDataStructure` FNV+memcmps the full `TG_HWLightsData`. After the priority lift:
- Shapes with the same template key get the same `TG_HWLightsData` content
- The FNV+memcmp dedup map hits as before
- Worst-case "200 unique entries" feared in v1 doesn't materialize because per-actor variability is gone

**No dedup regression.** Confirmed.

### 3.7 CPU vertex lighting path (response to MAJOR M2)

[mclib/tgl.cpp:1968](mclib/tgl.cpp) and adjacent CPU vertex-lighting loops also FIFO `s_listOfLights[]`. After Option A:
- Same `s_listOfLights[]` is reordered upstream at `Camera::updateLights`
- CPU vertex lighting automatically benefits from the priority lift
- No GPU/CPU lighting divergence introduced

The CPU and GPU paths produce the same SpotLight_ contribution for the same shapes. This was M2 in round-1; addressed by virtue of using a shared-data fix instead of a per-call-site fix.

### 3.8 Frame ordering (response to MAJOR M3)

`Camera::updateLights` runs once per frame, BEFORE `Mech3DAppearance::update`/`GVAppearance::update`/`BldgAppearance::update` call `SetLightList`. So:
- T0: Camera::updateLights runs → `worldLights[]` reordered + active flags set
- T1..TN: Actor updates run → each calls `SetLightList(eye->getWorldLights(), ...)` → sees reordered list

The (E) lights set position INSIDE the actor update via `SetPosition`. But the priority lift is a CLASS-MEMBERSHIP test (`is_e_slot()`), not position-dependent. The reorder result doesn't change frame-to-frame for any given set of registered slots. Position updates within the same frame don't invalidate the ordering.

**No frame-ordering bug.** M3 addressed.

### 3.9 Weapon-bolt and other POINTs (response to MAJOR M4)

Weapon-bolt POINTs at [code/weaponbolt.cpp:1262](code/weaponbolt.cpp) registered via `addWorldLight` get classified as "Other POINT/SPOT" (class 3, NOT class 2 SpotLight_-tagged). They appear after class 1+2 in the reordered list. In a busy combat frame with many weapon-bolts, they fill remaining slots after SpotLight_.

Acceptable: weapon-bolt POINTs are typically transient (a few frames per bolt), highly localized (small radius), and don't need globally-priority status. SpotLight_-tagged lights are persistent and represent the (E)-design-intent of "this is a deliberate scene light."

If specific content needs weapon-bolt POINTs to take priority over SpotLight_, that's a content-design problem orthogonal to this slice; we'd extend the classification.

### 3.10 `light->active` filter (response to MAJOR M5)

The priority lift only reorders slots; it doesn't filter by `active`. The downstream FIFO still respects `active==true` for inclusion. So:
- A SpotLight_ POINT with `active=false` (because `Camera::updateLights` rejected its world position) is at slot 0..M but skipped by FIFO walk
- Next active SpotLight_ POINT at slot M+1 gets the slot
- If all SpotLight_ are inactive, FIFO falls through to class 3 lights
- Camera-gate bug is NOT re-introduced by the priority lift

The original camera-gate symptom (mc2_10 mechs/GVs not illuminating despite active=1) is independently fixed by this slice because the relevant SpotLight_ at active=true slots now wins the FIFO that was previously consuming its budget on base-scene lights.

For the 86%+ slots that are active=false due to the camera frustum point-test: those don't contribute regardless. That's the (F) clustered/widened-frustum-test problem; out of scope here.

## 4. Greybeard 5-question ruling

1. **Subsystem pin.** `worldLights[]` iteration order, as established by `Camera::updateLights` at [camera.cpp:1871](mclib/camera.cpp) and consumed via `s_listOfLights[]` by every `GatherLightsParameters` / CPU-vertex-lighting / template-cache call.

2. **Symptom vs cause.** Symptom: mech/GV bodies don't illuminate despite (E) lights registered+active. Cause: FIFO truncation at 16-cap consumes its budget on base-scene lights that register first; (E)'s SpotLight_ POINTs register later and never reach the cap in iteration order.

3. **The meta-fix.** Reorder `worldLights[]` at `Camera::updateLights` time to place SpotLight_-tagged POINTs ahead of generic POINT/SPOT/TERRAIN. The FIFO truncation reaches them within 16 by construction. Single function modified; no caller changes; LIGHTBRIDGE and dedup preserved.

4. **Substitutive test.** Pre-existing T1.16 probe will see the 5-7 (E)-active SpotLight_ POINTs at the FRONT of `s_listOfLights[]` per frame. New visual canary on mc2_10 intro: mech/GV bodies show yellow contribution from their attached spotlights. New `[SPOTLIGHT_PRIORITY_LIFT v1]` summary: confirms reordering fires and that (E) slots survive FIFO walk for sample shapes.

5. **Verdict.** **META-FIX**, single function, ~50 LOC + a sort function. Bug class retired: "later-registered SpotLight_ POINTs lose FIFO contest against earlier-registered base lights." Compatible with future scoring (just enrich the comparator). Compatible with future clustered (the reorder is a no-op when clustered lookup replaces FIFO).

## 5. Implementation shape

### 5.1 Code change (single function modification)

In [mclib/camera.cpp:1871-1923](mclib/camera.cpp) `Camera::updateLights()`, AFTER the existing active-flag pass, add a final partition-sort step:

```cpp
// (E') priority lift: put SpotLight_-tagged POINTs ahead of generic
// POINT/SPOT/TERRAIN within s_listOfLights iteration order, so the
// FIFO walk in GatherLightsParameters reaches them within the
// 16-slot cap before consuming budget on base-scene lights.
// See docs/superpowers/specs/2026-05-21-spot-pack-scored-selection-design.md
std::stable_partition(
    /* worldLights[] from index 2 onward — preserve slots 0/1 for sun/ambient */,
    /* end of populated range */,
    [](TG_LightPtr l) -> bool {
        // Priority class 2: SpotLight_-tagged. Class 1 (AMBIENT/INFINITE)
        // is already at slots 0-1 by Camera construction (see camera.cpp:417/444).
        return l && mc2_spotlight_diag::is_e_slot(slot_index_for(l), nullptr);
    });
```

Two snags to resolve in plan-phase:
- **Snag 1: `worldLights[]` is a `TG_LightPtr*` array; partition works on iterators, not array indices.** Use `std::stable_partition(&worldLights[2], &worldLights[2+populated_range], ...)`. `populated_range` is `numLights`-2 to skip the always-present sun (slot 0) and ambient (slot 1).
- **Snag 2: `is_e_slot()` currently takes a slot index, not a `TG_LightPtr`.** After partition, slot indices SHIFT. Either: (a) iterate `worldLights[]` BEFORE partition, build a `set<TG_LightPtr>` of e-lights, then partition by membership in the set. (b) Reverse-lookup slot-from-pointer (linear scan: O(N²) overall — N=1024). Recommendation: (a) — one allocation per frame.

### 5.2 Slot-index invariants — IMPORTANT correctness concern

T1.16's `[SPOT_DIAG v1] event=overwrite_first_seen` probe at [camera.cpp:1923](mclib/camera.cpp) tracks lights by `slot=<i>` where `i` is the index into `worldLights[]`. After this slice's reorder, the slot index for a given light changes between frames. Downstream code that uses slot index as a stable identifier across frames would break.

**Audit:** grep for stored `worldLights[]` indices across the codebase. Identified storage sites:
- `BldgAppearance::lightId` (DWORD, stores `addWorldLight` return value)
- `Mech3DAppearance::lightId` (anubis pointLight)
- Similar fields on weapon-bolt and other actor types
- `spotlightSlotIds_` vectors on BldgAppearance/Mech3DAppearance/GVAppearance (added by (E))

These all assume `worldLights[slotId] == storedLight` stays true across frames. **The reorder breaks this invariant unless we ALSO update stored slot indices on every reorder.**

**Mitigation options:**

**M1: De-reference by pointer, not slot.** Replace `worldLights[slotId]` reads with stored `TG_LightPtr` direct reads. Slot index becomes informational only. `removeWorldLight` already takes `(slotNum, light)` and verifies `worldLights[slotNum] == light` — could verify by pointer alone, slot is redundant.

**M2: Do NOT physically reorder `worldLights[]`.** Instead, populate a SEPARATE `s_orderedLightView[]` that the SetLightList path consumes. The world pool indices stay stable; the iteration order varies. `eye->getWorldLights()` returns the ordered view; `worldLights[slotId]` still works for slot-based reads.

**M3 (preferred per plan-phase recommendation): M2 — preserve `worldLights[]` index stability.**

This makes the spec change more nuanced: the reorder is at the CONSUMPTION side (whatever `getWorldLights()` returns to `SetLightList` callers), NOT at the storage side. The storage `worldLights[]` array stays in registration order with stable slot indices. A second array `s_orderedLights[]` is computed each frame at `updateLights` time.

Cost: extra 1024-pointer copy per frame. Trivial.

### 5.3 Spec proposes M3 as default

**Updated proposal:**
- Add `s_orderedLights[]` array (parallel to `worldLights[]`) maintained at `Camera::updateLights` time.
- Populate it via stable partition: AMBIENT/INFINITE at front (slots 0-1 from `worldLights`), SpotLight_-tagged POINTs next, others last.
- `getWorldLights()` returns the ordered view.
- `worldLights[]` storage is unchanged; slot-by-index reads still work; T1.16 probe and lightId fields stay valid.

This adds ~24KB BSS (`s_orderedLights[1024]` of TG_LightPtr) and 1024-pointer copy per frame (~8µs).

## 6. Vulkan-prep audit (unchanged from v1)

| Requirement | This slice |
|---|---|
| Explicit device-mediated binding | Not applicable — no new GL bindings |
| No implicit cross-call GL state | Not applicable — no new GL state |
| std430 lockstep | Not applicable — SSBO layout unchanged |
| `[0,1]` depth | Not applicable — no depth |
| Enqueue/flush patterns | Not applicable — runs at `Camera::updateLights` |
| No full RHI ahead of need | Compliant — pure CPU reorder, no abstraction layer added |

**Vulkan-ready by absence.** Pure CPU-side data reordering.

## 7. Future-extension hooks

The priority comparator is the swap-point for future expansion:

- **Today: class-based partition** (`is_e_slot()` membership).
- **Future scored selection** (if SpotLight_ active count exceeds 16-cap): replace the partition with a sort by relevance to a scene-representative actor position (e.g., camera focus position). One comparator change.
- **Future clustered lighting**: replace `Camera::updateLights` reorder entirely with a per-tile cluster build. `s_orderedLights[]` becomes a per-cluster array instead of a global ordered view. Caller changes are zero (still call `getWorldLights()`).

The decoupling is at the data structure: `worldLights[]` (storage) vs `s_orderedLights[]` (presentation). Future architectural moves replace the presentation builder, leaving storage stable.

## 8. Adversarial considerations

- **A1. AMBIENT/INFINITE assumed at slots 0-1.** The camera initialization at [camera.cpp:417-446](mclib/camera.cpp) populates slot 0 (TG_LIGHT_INFINITE = sun) and slot 1 (TG_LIGHT_AMBIENT). If a future change moves them, the reorder logic at slots 2+ breaks. Document the invariant; consider asserting it at startup.
- **A2. SpotLight_ tagging via `mc2_spotlight_diag::is_e_slot()`.** This is currently env-gated diagnostic infrastructure. Promote it to always-on for the priority-lift path; document as load-bearing for (E') correctness.
- **A3. Anubis searchlight uses TG_LIGHT_SPOT.** Pre-existing anubis at [mech3d.cpp:3342](mclib/mech3d.cpp) is NOT tagged as `is_e_slot()` because it was never registered through (E)'s lazy-init path. Should it be priority-lifted? Plan-phase decision: tag anubis too, OR add a second SpotLight_ check via TG_LIGHT_SPOT type AND `isSpotlight=true` shape tag.
- **A4. SetLightList(NULL, 0) at genactor.cpp:1207.** GenericAppearance::render zaps the light list. This bypasses the priority lift entirely — generic appearance objects don't get any lights. Acceptable: they don't render lit anyway.
- **A5. Reorder cost.** 1024-pointer copy + O(N) partition per frame ≈ 8µs. Well below the 100µs CPU-projection budget.
- **A6. Bake-static path.** `mc2WriteStaticLightSlot` uses recipe-indexed cached light data. The reorder doesn't invalidate baked static slots (those are per-recipe, position-cached). Buildings rebaked from a reordered list see the new priority on next bake.
- **A7. Per-frame ordering stability.** `is_e_slot()` membership is set at `addWorldLight` time and cleared at `removeWorldLight` time. Across frames within a mission, membership is stable for a given light. Reorder result is deterministic; no flicker.

## 9. Open questions for plan-phase

- **OQ1.** Should anubis (TG_LIGHT_SPOT, pre-existing) be priority-lifted too? Recommendation: yes — add `is_anubis_spotlight()` companion to `is_e_slot()` OR generalize the priority-lift test to "any TG_LIGHT_SPOT OR any (E)-registered TG_LIGHT_POINT."
- **OQ2.** Slot-by-index storage assumption — confirm via grep that no consumer outside of `worldLights[storedSlotId] == storedLight`-style reads exists. If a render-thread reads `worldLights[i]` directly assuming a stable convention (e.g. `worldLights[0]` is always sun), the M3 ordered-view approach is mandatory.
- **OQ3.** Should `s_orderedLights[]` be re-built at every `Camera::updateLights`, or only when SpotLight_ membership changes? Lazy rebuild would save ~8µs/frame but adds invalidation complexity. Recommend eager (rebuild every frame); 8µs is negligible.
- **OQ4.** Tagging `addWorldLight` from (E)'s lazy-init blocks: should we also tag from `weaponbolt.cpp:1262` POINT lights? Or strictly only SpotLight_-child-shape-derived registrations? Recommend: only SpotLight_-child — weapon-bolt POINTs are transient and shouldn't compete for permanent priority slots.

## 10. Cross-references

- Round-1 review verdict: BLOCKING-CRITICAL — preserved as §1 of this v2 spec for learning continuity
- Spec (E): [2026-05-20-spotlight-real-illumination-design.md](2026-05-20-spotlight-real-illumination-design.md)
- (F) lighting MODEL rework brief: [terrain_lighting_compute_kernel_saturation.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\terrain_lighting_compute_kernel_saturation.md)
- LIGHTBRIDGE template-cache origin: commit `996aff4` (8.5ms→0.4ms optimization)
- T1.16 diagnostic artifact: `tests/smoke/artifacts/2026-05-21T06-48-58/mc2_10.log`
- `mc2_spotlight_diag` (`is_e_slot()` infrastructure): [mclib/spotlight_diag.h](mclib/spotlight_diag.h)
- Greybeard skill: [.claude/skills/greybeard.md](../../.claude/skills/greybeard.md)
- Adversarial review skill: [.claude/skills/adversarial-plan-review.md](../../.claude/skills/adversarial-plan-review.md)
