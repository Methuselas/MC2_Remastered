# Per-Object Cull GPU-Side — Recon (v2, REVISED)

- **Status:** RECON — SUPERSEDED by [(C) Light retirement spec](../specs/2026-05-20-light-gameobject-retirement-design.md). User architectural call 2026-05-20: rip Light out entirely; real lighting (`TG_Light`) already ships independently. The L1/L2/L3 layered analysis below is retained for arc context but the recommendation is OBSOLETE.
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Original verdict (now superseded):** Three layered meta-fixes (L1 transitional, L2 transitional, L3 architectural). L3 was the META-META the recon converged on. **The actual architectural call went one level higher — delete `Light` entirely; "real lighting" is `TG_Light` which already exists.** See (C) spec.
- **v1 retraction note:** the first pass hypothesized lights were SSBO illumination contributors with sphere-of-influence shape. **Wrong.** Grep of [code/actor.cpp:361-413](code/actor.cpp) (VFXAppearance::render) shows lights are 2D animated billboard sprites that read `screenPos.x, screenPos.y` for placement (line 379). They DO need projection output, not sphere-frustum admission. The v1 DEFER recommendation rested on a false premise; revised analysis below.
- **All file:line citations grep-verified at write time.**

---

## 1. What "Light" actually is in MC2

Reading the code:

- [code/light.h:76](code/light.h) — `class Light : public GameObject`
- [code/light.cpp:178-182](code/light.cpp) — appearance class is `VFXAppearance` (VFX_APPEAR)
- [code/actor.cpp:379](code/actor.cpp) — `VFXAppearance::render` writes a `TextureElement` at `(screenPos.x, screenPos.y)` with `appearType->actorStateData[].textureSize` — **2D billboard sprite**
- [code/actor.cpp:385](code/actor.cpp) — gets per-frame terrain-light coloring (`land->getTerrainLight(position, ...)`)
- [code/actor.cpp:454-490](code/actor.cpp) — `VFXAppearance::update` advances animation frames per-frame; comment line 467: "Must update animation frame numbers, even if not visible!!"
- [code/light.cpp:25](code/light.cpp) — `LIGHT_DEPTH_FIXUP = -500` (depth offset for billboard composition)
- [code/light.cpp:112-113](code/light.cpp) — position adjusts ONCE at create-time (altitudeOffset); not touched again
- [code/objmgr.cpp:985-996](code/objmgr.cpp) — `GameObjectManager::getLight()` is the pool allocator (ring-buffer reuse, sized by `numLights`)
- [code/objmgr.cpp:3461](code/objmgr.cpp) — `createLight` is **dead code** (zero callers — grep confirmed)
- Live allocator path: pool `getLight()` called by external spawn sites (weapon/carnage effects)

**Conclusion:** "Light" in MC2 is a **dynamic, pool-allocated 2D billboard sprite effect** — muzzle flashes, weapon hit visuals, explosion flashes, possibly building-window glows. Pool size from mission `.fit` configuration. Position stable once spawned. Pool ring-reuses slots as lights expire. The class name is historically misleading — these are NOT illumination contributors to a `LightsData` SSBO; they are 2D sprites drawn via screen-position projection.

**Population at F3 baseline:** ~950 active concurrent allocations during stationary smoke. Each calls `Light::update` → `onScreen()` → `projectForObjectAdmission` once per frame.

## 2. Why this is NOT Stage 0.5 §4 by another name

v1 of this recon claimed it was. Wrong. The differences:

| Dimension | Stage 0.5 §4 (props/mechs) | Light path (this recon) |
|---|---|---|
| Producer being migrated | `inView` per-actor coarse cull | `screenPos` 2D projection |
| Consumer chain | `cachedGpuLightIndex_`, `registry::flush`, mech/prop submit | `VFXAppearance::render` 2D billboard placement |
| `windowsVisible` stamp readers | many subsystems via canBeSeen chain | `light.cpp:141` render gate (just one) |
| Bug-class on update/render gate split | resurrects black-tree (stale `cachedGpuLightIndex_`) | "light doesn't appear that frame" — visibility regression, not stale-state |
| Requires readback coupling | yes (§4 attempt) | NO if we don't repoint at readback |
| Blast radius | mech / static-prop / building / tree (everything) | lights only (2D sprite effects) |

The §4 bug class needs (a) the readback channel, (b) the update/render split, (c) consumers that hold derived state across frames. Lights don't carry frame-stamped derived state (anim frame advances unconditionally per actor.cpp:467). The §4 cautionary tale does NOT apply directly. v1 inherited the framing too eagerly.

## 3. Three meta-fix layers

### L1 — Transitional: position-static cache (lowest risk)

**The insight.** Lights don't move once spawned (light.cpp:112-113 sets position once on `OBJECT_FLAG_JUSTCREATED`, never thereafter). When the camera is stationary, `screenPos` for every light is invariant. The F3 baseline shows `stationary_pct = 99.7%`, so >99% of projections are recomputing the same answer.

**Fix.** Cache `(screenPos, windowsVisible)` per-light. Invalidate on:
- Camera moves (any camera state change → all caches dirty)
- Light is re-allocated from pool (per-light dirty)

Iteration becomes: `if (cache valid) reuse; else project and stamp cache`. Net behavior identical when invalid; ~5ns lookup when valid.

**Estimated savings.** In smoke (99.7% stationary): drops `eventdriven_projection_total` by ~99% during stationary frames. Worst-case motion frame cost unchanged. In real gameplay (estimated stationary ~30-60%): drops by 30-60% of the baseline.

**Risk.** Low. The cache is local to `Light::update`; no consumer changes; no new visibility channel introduced. The hardest part is "did the camera move?" — the engine already tracks that for the existing actor static-update-skip path (`seen=78354 / run=252 / skip=78102` in the F3 baseline note shows this machinery already works for actors; lights need to opt in).

**Substitutive test.** After landing, F3 (with the timing-bucket promotion from §5) shows `eventdriven_projection_total` p95 dropping by the expected fraction during stationary frames.

### L2 — Transitional: block-based pre-cull

**The insight.** Lights have positions. Positions map to terrain blocks. Terrain blocks already have an active/inactive flag (the cull system tracks visible blocks per `blockVisBits[]`). Lights in inactive blocks cannot possibly be on screen.

**Fix.** Bucket lights by terrain block at allocation time. Each frame, iterate active blocks → iterate lights in those blocks → project. Inactive-block lights skip projection entirely. Same `windowsVisible` semantics for the lights that do project.

**Estimated savings.** Active blocks at wolfman zoom-out are ~5-15% of the map. So ~85-95% of lights skip projection in worst case. Stationary baseline cache (L1) already gives ~99% reduction at stationary; L2 wins on MOTION frames where L1 doesn't help.

**Risk.** Medium. Block re-bucketing on pool realloc is one indirection per `getLight()` call (cheap). The cull's block-active flag must be the strict-superset version (sticky-bit `blockVisBits[]` after commit `91b6991`, per memory `stage_0_5_section_4_blocked_on_readback_non_superset.md`) — not the readback channel (which is the §4 bug class). Sticky-bit IS a strict-by-construction per-mission superset at the block level, so this is safe.

**Substitutive test.** With L1+L2: motion-frame `eventdriven_projection_total` also drops materially.

### L3 — META-META FIX: collapse Light into the (B) GPU particle pipeline

**The insight the user was pointing at.** Light is shape-identical to a gosFX CardCloud particle:

| Property | gosFX CardCloud | MC2 Light |
|---|---|---|
| Pool-allocated, dynamic spawn | yes | yes |
| Animated 2D billboard | yes | yes |
| Position-stable after spawn | yes | yes |
| Texture frame sequence | yes | yes (`actorStateData`) |
| Expire on lifespan end | yes (Effect::Execute returns false) | yes (`oneShotFlag` + animation end) |
| World position → 2D screen billboard | yes (currently via MLR) | yes (currently via `projectForObjectAdmission`) |

The TWO systems exist in parallel because of historical accident, not architectural necessity. Both solve "spawn 2D animated sprite at world position with terrain-light coloring and depth-fixup."

**Fix.** Extend the (B) GPU particle pipeline (currently being designed in the spawned-chip session) to subsume Light as a content category. After the migration:
- No `Light : public GameObject` class
- No `Light::update` calls to `onScreen()`
- No 950 `projectForObjectAdmission` calls/frame for lights
- Light spawn becomes "emit particle into the GPU particle SSBO with anim + terrain-color + depth-fixup attributes"
- The (B) vertex shader expands billboards using the unified-projection UBO directly

**Why this is the META-META FIX.** The slice this recon was nominally for ("Per-object cull GPU-side investigation") *as a slice* never makes sense in isolation. Even the cleanest standalone fix (L1+L2) is fighting an over-powered legacy infrastructure. The real answer is that **lights should not be GameObjects to begin with** — they're particles, and we're already building the particle pipeline.

**Required (B) spec additions** (the (B) design session needs to know):
- Texture animation frames per particle (gosFX CardCloud has this; trivial in (B))
- Per-particle terrain-light color sampling (NEW for (B); could be a per-particle SSBO field + a CPU-side sampling pass at emit, OR a GPU compute shader that samples the terrain light map per particle)
- Depth-fixup attribute per particle (NEW for (B); single float per particle)
- Content-side: route LightType `.fit` data into the (B) effect-spec loader (analogous to gosFX `mc2.fx` — same parser shape, different data file)

**Risk.** Medium-high scope, but the architectural cleanup is permanent and the (B) pipeline already has to exist.

**Substitutive test.** After L3 ships: `code/light.cpp`, `code/light.h` are deleted (or stub-only); `LightType` retired; `eventdriven_projection_total` baseline count from lights drops to 0; the GameObject Light pool slots are freed in `objmgr`.

**Reversibility.** Lower than L1/L2 (it's a real architectural change). But it composes with the (B) work that's happening anyway.

## 4. Greybeard ruling

1. **Subsystem pin.** 2D billboard sprite effect rendering via the GameObject `Light` class. Producer: [code/gameobj.cpp:2090](code/gameobj.cpp) called from [code/light.cpp:123](code/light.cpp). Consumer of `screenPos`: [code/actor.cpp:379](code/actor.cpp) `VFXAppearance::render`. Population: ~950 active concurrent pool allocations p50 stationary.

2. **Symptom vs cause.** Symptom: 950 unmeasured CPU `projectForObjectAdmission` calls/frame. Upstream condition: lights inherit `GameObject` because they were retrofitted into the actor-cull infrastructure; the projection is over-engineered for the consumer (which only needs `screenPos` for billboard placement, no full screen-rect-admission test). And: lights are a parallel system to gosFX with overlapping responsibilities; the dual-system constraint is what makes the perf problem hard to retire cleanly.

3. **The meta-fix.** Three valid candidates, layered:
   - **L1 (transitional):** cache `screenPos` while camera stationary + light position static. Tactical.
   - **L2 (transitional):** block-based pre-cull. Tactical.
   - **L3 (architectural, META-META):** absorb Light into the (B) GPU particle pipeline. Strategic. Retires the BUG CLASS "MC2 has two parallel 2D billboard effect systems with separate cull infrastructure."

4. **Substitutive test.** L1: F3 stationary count drops ~99%. L2: motion count also drops. L3: `Light` class deleted; `eventdriven_projection_total` light contribution → 0; substitutive by construction (producer AND consumer deleted together).

5. **Verdict.**
   - **L1 + L2 = `META-FIX (small)`.** Ship together as a single slice. Self-contained; no consumer migration; no readback coupling. Closes the F3 timing-bucket follow-up trigger.
   - **L3 = `META-FIX (architectural)`.** Conditional on (B) GPU particle pipeline spec absorbing Light's content shape. File now as a (B)-spec extension request; ship after (B) lands. NOT a standalone slice.
   - **Defer reasoning of v1 retracted.** §4 cautionary tale does not directly apply (no readback coupling, no update/render gate split, no derived stale-state in consumers).

## 5. No-risk preparatory move (same as v1)

Promote `SIDECAR_EVENTDRIVEN_PROJECT_Z` from count-only to a timed `BucketId` ([mclib/cpu_proj_cost_split.h:43](mclib/cpu_proj_cost_split.h)). The F3 spec's "n_calls > 0 AND time UNMEASURED with high n_calls → open separate follow-up slice" trigger has fired; this single-bucket addition gives baseline µs data for sizing L1+L2.

**Update (parallel session F3 mc2_10 worst-case landed during this recon):** the new capture sized `projection_total worst_window_p95 = 293 µs` with **99.9% attributable to `mlr_total` (gosFX)**. The `eventdriven_projection_total` bucket is still TIME-UNMEASURED with ~1000 calls/frame; memory `f3_mc2_10_worstcase_2026_05_20` explicitly flags this as "**potentially ~1ms**" pending R2 follow-up. If R2 confirms the ~1ms estimate, L1+L2 jumps from "marginal" to "must-ship" (1 ms = 10× the 100 µs CPU-projection budget). If R2 measures <100 µs, L1+L2 is below threshold and the recon collapses to "wait for L3 via (B) absorption." Sizing is BLOCKED ON R2.

## 6. Adversarial review of this recon

Step 2 (grep-verify cited symbols): every file:line in §1 verified at write time. Step 3 (interacts-with claims): the (B) absorption table in §3 L3 is grep-confirmed against `mclib/gosfx/cardcloud.cpp` and `code/actor.cpp` (texture animation, position-stable, lifespan) — both systems do exhibit the cited properties. Step 5 (perf claims): the L1 99% reduction is conditional on the camera-stationary fraction of F3 (99.7%); explicitly caveated. The L2 85-95% inactive-block fraction is rougher — depends on camera FOV/zoom; flagged as estimate.

**Critical risks the recon might be missing:**

- **R1.** `releaseLight` (`objmgr.cpp:1001`) called from where? If `Light::update` returning false triggers release (objmgr.cpp:2345 calls `MC2_DESTROY(lights[i], "update_false")`), then 950 might be a steady-state population where new lights are spawning at the rate old ones expire. In stationary smoke this is unlikely (no combat triggering muzzle flashes). Worth verifying: are the 950 lights LONG-LIVED or RAPIDLY-CHURNING? Cache invalidation cost depends on this.
- **R2.** The `lightAppearance->setInView(inView)` at light.cpp:124 — does anything downstream depend on the `inView` bit being precisely current? Adversarial check: read [mclib/appear.h:227](mclib/appear.h) `setInView` body — sets four flags (inView, renderVisible, simActive, aiPresentable) via `setVisibilityGatesFromLegacy`. The `simActive` flag in particular may gate simulation behavior (`memory/stage_0_5_section_4_blocked_on_readback_non_superset.md` discusses Stage 1 unconflation). If a cached `inView` is stale during motion, sim could behave wrong for one frame. Likely not an issue for lights (they don't simulate game-state), but flag for L1 design.
- **R3.** Is `windowsVisible` actually read elsewhere for lights? Grep for `light.*->windowsVisible` or `lights\[.*\]->windowsVisible`. Quick grep above showed no such read; light.cpp:141 reads `this->windowsVisible`. Confirmed sole consumer.
- **R4.** L3 absorption claim that lights are shape-identical to gosFX CardCloud — does (B) actually subsume terrain-light per-particle coloring? gosFX `mc2.fx` particles also color via similar means? Need to confirm in the (B) spec. The (B) session's spawned chip prompt did NOT name this specifically; surface to that session.
- **R5.** Mission script (ABL) hooks. v1 recon claimed no ABL gates effect completion (grep of 991 .abl files). Confirmed for effects; same likely true for lights but not re-greped. If ABL waits on a light, L3 retirement breaks scripts.
- **R6.** Save game: lights are pool-allocated dynamics, not persisted. Confirmed — no `Light` serialize/deserialize in saveload paths.

None of R1-R6 invalidate the recon, but R4 is the load-bearing one to surface to the (B) session.

## 7. Recommendation

**Three actionable carry-forwards, in priority order:**

1. **Surface L3 to the (B) GPU particle pipeline design session** (the spawned chip). The (B) spec must explicitly consider Light absorption — texture animation per-particle, terrain-light per-particle color, depth-fixup. If (B) lands without these, L3 is blocked and we re-fight this problem later.
2. **F3 timing-bucket promotion** (no-risk preparatory move). If user wants a single concrete now-slice while the architectural ones are deferred, this is the one. Trivial scope.
3. **L1 + L2 as a standalone slice** *only if* F3 timed data shows meaningful µs cost AND L3 will take >1 milestone to land. Otherwise wait for L3.

**Do NOT** treat this as a Stage 0.5 §4 sibling slice. v1 was wrong about that.

---

## Cross-references

- [memory/stage_0_5_section_4_blocked_on_readback_non_superset.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\stage_0_5_section_4_blocked_on_readback_non_superset.md) — the §4 block; does NOT directly apply here (premise of v1 was wrong)
- [memory/cull_gates_are_load_bearing.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cull_gates_are_load_bearing.md) — applies to L2 only (must use sticky-bit `blockVisBits[]`, not readback)
- [memory/feedback_offload_must_be_substitutive_not_additive.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_offload_must_be_substitutive_not_additive.md) — L3 is substitutive by construction; L1+L2 are substitutive within their scope
- [memory/policy_split_wrapper_grep_trap.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\policy_split_wrapper_grep_trap.md) — why the call site was hidden initially
- [memory/f3_tier1_baseline_2026_05_20.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\f3_tier1_baseline_2026_05_20.md) — the baseline; update with timed bucket data when new capture lands
- [docs/superpowers/specs/2026-05-20-gosfx-retirement-or-replacement-design.md](docs/superpowers/specs/2026-05-20-gosfx-retirement-or-replacement-design.md) — (A) gosFX retirement
- (B) GPU particle pipeline spec — pending; the spawned chip session will produce it. **This recon's §3 L3 must reach that session.**
- [docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md](docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md) — F3 spec
- [docs/superpowers/specs/2026-05-20-appearance-inview-unconflation-design.md](docs/superpowers/specs/2026-05-20-appearance-inview-unconflation-design.md) — alpha-Stage 1 v3 §4 inView decomposition (relevant if L1 cache touches `setInView`)
