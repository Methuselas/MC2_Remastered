# `MC2_STATIC_UPDATE_SKIP=1` Residual — Debug Strategy

> **Status:** Debug strategy doc. Author: 2026-05-06.
> **Predecessor:** `memory/update_skip_touch_regression.md` (two-mechanism framing).
> **Goal:** Close the residual mechanism so the skip-path can default-on and `appearance->update()` becomes `appearance->touch()` for static props — collapsing the per-actor 9 µs hot path to ~100 ns.

## Why this matters now

Tracy capture (2026-05-06) shows `appearanceUpdate` at 9.52 µs/call × 954 actors = 9.08 ms/frame. With `MC2_STATIC_UPDATE_SKIP=1` enabled, that path is replaced by `appearance->touch()` which is essentially free (~100 ns). **Every other modernization slice is downstream of this — Track C3 routes the cull gates, but the per-call cost itself only dies via the skip path.**

The skip path is built and shipped. It's gated default-off because of a black-billboard regression that has TWO mechanisms — one closed (texture-cache pin patch), one open (the residual).

This document scopes the residual investigation.

## What we know about the residual

Per `update_skip_touch_regression.md` and the smoke verification table:

- `event=draw_black=0` — at draw time, the GL texture ID is valid.
- `cpu_render_no_texture=0` and `cpu_render_dead_handle=0` — neither CPU fallback path sees a dead handle.
- `event=evict=114, event=evict_skipped=860, leakedPins=0` — pin patch is mechanically correct.
- **Visual: still black during camera motion. Pause restores in-view assets, out-of-view stay broken.**

So the residual is NOT a texture-resolution problem. The geometry renders correctly (correct shape silhouette), but with RGB=0. That's a **lighting/color** problem, not a **texture** problem.

Three layered "stale leaf snapshot" fix attempts (`c31dad6`, `0e7a4aa`, `c4c3157`) did NOT help. That further confirms the issue is upstream of leaf state.

## Live candidate hypotheses (revised post-code-grounding)

### H1 — REJECTED on code re-read: `cachedGpuLightIndex_` is per-instance, not per-type

Initial framing claimed `cachedGpuLightIndex_` was per-type. Code re-read at `msl.cpp:128-148` shows `TG_TypeMultiShape::CreateFrom()` does `newShape = new TG_MultiShape;` — fresh allocation per appearance instance. Each `BldgAppearance::bldgShape` is its own object with its own `cachedGpuLightIndex_` field.

**This means H1 (writer-order drift on shared multi-shape) is mechanically incorrect.** Multiple actors do NOT share `cachedGpuLightIndex_`. The flush() reads `rng.multi->getCachedGpuLightIndex()` where `rng.multi` is the per-instance multi-shape pointer; that's per-actor, not shared.

H1 stays as a documented dead-end so future investigation doesn't re-trip into the same wrong assumption.

### H4 — `lightData_` never populated for mission-load-registered + static-skipped actors (PRIMARY SUSPECT, post-code-grounding)

**Mechanism:** `lightData_` is a per-LEAF (`TG_Shape`) field populated ONLY by `TransformMultiShape` (i.e., during a full `BldgAppearance::update()` / `TreeAppearance::update()`). It's NOT populated by `registerStatic()` / `TransformMultiShape_BuildRecipe()` — those only bake position-related fields.

Under UPDATE_SKIP, `appearance->touch()` runs INSTEAD of `update()`, which calls `bldgShape->ResubmitCachedGpuLightData()` → `firstLeaf->ResubmitCachedLightData()` → `mcTextureManager->addLightDataStructure(&lightData_)`. This re-submits the cached `lightData_` to get a fresh slot index for this frame.

**But if `lightData_` was never populated** (default-zero state because no full `update()` ever ran on this actor):

1. `addLightDataStructure(&zeros)` returns slot N (or finds existing match — ALL "zeros" instances dedupe to the same slot).
2. `cachedGpuLightIndex_` gets slot N.
3. Registry flush reads slot N's content via the GPU UBO.
4. Slot N contains all-zero lighting (no lights, all colors 0,0,0).
5. GLSL `calc_light` with no lights returns `base_light + 0` = `base_light`. For tree leaves with `aRGBLight=0xFF000000` and `BaseVertexColor=0`, base_light is zero.
6. **Black tree.**

**Why this newly-fits the symptoms post-Track-B:**

- "Bug worsened recently": Track B's `registerStatic()` mission-load enumeration ran for every static prop at mission start. Before Track B, registration was lazy first-render — the first render call's `update()` populated `lightData_` BEFORE any UPDATE_SKIP fired. With Track B, mission-load registers BEFORE any `update()` runs; UPDATE_SKIP then fires on these actors with empty `lightData_`.
- "Camera motion only": actors that come into view for the first time hit static-skip path; their `lightData_` is empty.
- "Pause fixes in-view": pause stalls update calls but pre-existing-correct `cachedGpuLightIndex_` slots persist. Whether pause "fixes" depends on whether the pause-resumption path runs a one-time `update()` (timing-dependent).
- "Out-of-view stays broken": those actors were mission-load-registered but never had `update()` populate `lightData_`. They stay in zero-light state forever.

**The diagnostic to confirm H4:** check what `peekLightSlot(cachedGpuLightIndex_)` returns for a black actor. If `peek.numLights == 0` and/or `peek.firstColor` is zero — confirmed. The instrumentation for this is already in tree at `gos_static_prop_registry.cpp:330-342` (capped at 16 lines per session — may need raising for confirmation capture).

**Fix (H4 confirmed by code re-read):** set `needsFullBakeNextFrame = true` in `BldgAppearance::registerStatic()` and `TreeAppearance::registerStatic()` after the recipe batch lands. This leverages the EXISTING late-registration recovery mechanism — `IsStaticNow()` returns false while the flag is set, forcing a single full `update()` on the first post-registration frame which populates `lightData_`. The flag self-clears (`needsFullBakeNextFrame = false` at bdactor.cpp:2313) after the bake. Subsequent frames proceed via static-skip with valid cached data.

**Code-grounded confirmation of the bug:**

`BldgAppearance::registerStatic()` at `bdactor.cpp:2680-2731` calls only:
- `bldgShape->TransformMultiShape_BuildRecipe(&xlat, &rot)` — positions only, `lightData_` untouched.
- `GpuStaticPropBatcher::instance().buildRecipeFromShape(...)` per leaf — builds GPU recipe.
- `GpuStaticPropRegistry::registerRecipe(...)` — registers.
- Sets `staticReg.registered = true`, `staticReg.shape = bldgShape`, `staticReg.recipeIndex = regIdx`.

After this, `IsStaticNow()` at `bdactor.cpp:2735` returns:
```cpp
return staticReg.registered      // TRUE
    && staticReg.shape == bldgShape  // TRUE
    && !needsFullBakeNextFrame   // TRUE (never set in registerStatic)
    && isStaticEligible();       // TRUE (isStaticEligible re-checks)
```

So UPDATE_SKIP fires on the first frame for mission-load-registered actors that have NEVER had a full `update()` run. `touch()` re-submits empty `lightData_` → all-zero lighting → black.

**Cost of the fix:** one extra `update()` call per static actor at mission load. For 1000 actors at 9.52 µs/call = ~9.5 ms ONE-TIME at mission load. After the first frame, all actors are static-skipped properly with valid cached data.

**Pre-Track-B behavior (why this only manifests now):** before Track B, registration was lazy-first-render. The first render call ran `update()` first (because `IsStaticNow()` was false until staticReg.registered was set), THEN `update()` set up the recipe and populated `lightData_`, THEN registered. Mission-load enumeration in Track B inverts that order — registration BEFORE first update — exposing the gap.

### H2 — Camera-motion-induced LOD swap with stale `lightData_`

**Mechanism:** Trees use multi-LOD multi-shapes. `treeShape` swaps to a different LOD multi-shape based on camera distance. Each multi-shape has its own `lightData_`. After swap, the NEW multi's `lightData_` may be stale (set during the actor's last update on the OLD multi).

**Why this fits some symptoms:**
- Camera motion triggers LOD swap.
- Camera motion causes black.

**Why this is weaker than H1:**
- LOD swap should re-trigger `update()` for the actor anyway (per LOD-swap callback), which resets `lightData_`.
- Doesn't explain the "pause fixes" symptom — pause doesn't reset LOD.

### H3 — `GatherLightsParameters` capture timing under skip path

**Mechanism:** Under `UPDATE_SKIP`, `appearance->touch()` runs INSTEAD of `update()`. Touch should call `ResubmitCachedGpuLightData` (which uses the cached `lightData_`). If the cached `lightData_` was captured at a moment when `TG_Shape::s_listOfLights` was in a transient state (mid-rebuild, mid-update), it captures bad data.

**Why this fits:**
- Skip path = touch instead of update = different capture timing.

**Why this is weaker than H1:**
- The pin-patch verification showed `pin/unpin balanced` and `event=recache_multi` instrumentation works. If H3 were active, we'd see bad cached data persist; instead pause fixes in-view assets, suggesting the actor RECEIVES correct data when its render path runs — but persists bad data when it doesn't.

## Decision tree — minimum experiments to discriminate

### Step 1: Bisect by population (10 minutes)

Run with skip enabled, force-dynamic per population. If trees-only-static produces black trees but buildings-static-only works:

```bash
# Trees on static, buildings on dynamic
MC2_STATIC_UPDATE_SKIP=1 MC2_FORCE_DYNAMIC_BUILDINGS=1 mc2.exe -mission mc2_01

# Buildings on static, trees on dynamic
MC2_STATIC_UPDATE_SKIP=1 MC2_FORCE_DYNAMIC_TREES=1 mc2.exe -mission mc2_01
```

**Predictions:**
- If both populations show black: bug is in shared infrastructure (H1 most likely — both populations share `cachedGpuLightIndex_` write pattern).
- If only trees show black: H2 is in play (LOD swap is tree-specific).
- If only buildings show black: less common; building-specific shadow path or paint scheme issue.
- If neither show black with one population dynamic: confirms the bug requires the static path on BOTH populations (H1's "winner gets overwritten" mechanism is more likely).

### Step 2: Disable the registry entirely (5 minutes)

```bash
MC2_STATIC_UPDATE_SKIP=1 MC2_STATIC_PROP_REGISTRY=0 mc2.exe -mission mc2_01
```

This is the load-bearing test: skip path WITHOUT the registry's flush-time light-index patching.

**Predictions:**
- If black persists: bug is in `touch()` itself, not registry. Look at `ResubmitCachedGpuLightData` semantics.
- If black goes away: confirms the registry's flush-time patching is the mechanism (strong H1 evidence). The fix is then: per-instance lightDataIndex, not per-type.

### Step 3: Per-actor `cachedGpuLightIndex_` writer-order trace (30 minutes to instrument, 5 minutes to capture)

Add a per-multi-shape "last writer" diagnostic:

```cpp
// In TG_MultiShape:
uint32_t cachedGpuLightIndex_ = 0;
uint32_t cachedFrame_ = 0;
const void* lastWriter_ = nullptr;        // NEW: appearance pointer
uint32_t lastWriterFrame_ = 0;            // NEW
uint32_t writerCountThisFrame_ = 0;       // NEW: incremented on each write

void ResubmitCachedGpuLightData(const void* writer) {
    // ... existing code ...
    if (lastWriterFrame_ == g_mc2FrameCounter && lastWriter_ != writer) {
        ++writerCountThisFrame_;  // multiple writers per frame on same multi
    } else {
        writerCountThisFrame_ = 1;
        lastWriterFrame_ = g_mc2FrameCounter;
    }
    lastWriter_ = writer;

    static const bool s_trace = (getenv("MC2_LIGHT_INDEX_DRIFT") != nullptr);
    if (s_trace && writerCountThisFrame_ > 1) {
        printf("[LIGHT_INDEX_DRIFT v1] frame=%u multi=%p writer_now=%p writer_count=%u newSlot=%u\n",
               g_mc2FrameCounter, (void*)this, writer, writerCountThisFrame_, cachedGpuLightIndex_);
        fflush(stdout);
    }
}
```

**Predictions:**
- If `[LIGHT_INDEX_DRIFT v1]` fires hundreds of times per frame with `writer_count > 1`: H1 is confirmed. Multiple actors share one multi's cached light index. Fix: per-instance light-index (move from MultiShape to BldgAppearance/TreeAppearance fields).
- If it fires zero or rarely: H1 is NOT the mechanism. Look at H2 / H3.

### Step 4: Slot-content peek at flush time (10 minutes)

Already-built env var per memory: `MC2_STATIC_PROP_TRACE=1`. This logs slot content at flush time. Run with skip + this trace, capture a 30-second sample with camera motion, grep for slots with `numLights=0` or `color={0,0,0}`.

**Predictions:**
- If slots have valid lighting data but actors still render black: the bug is NOT in slot content — it's in slot LOOKUP (which slot does the registry pick?). Strengthens H1 via the lookup angle.
- If slots have all-zero lighting data: capture failure in `GatherLightsParameters`. Strengthens H3.

## What's the smallest winning experiment?

**Step 2 alone (disable registry, observe black persistence) decisively discriminates H1 from H2/H3** with 5 minutes of operator time. If black goes away with `MC2_STATIC_PROP_REGISTRY=0`, the residual is registry-mediated and H1 is the mechanism.

If H1 is confirmed, the **fix is structural**: move `cachedGpuLightIndex_` from `TG_MultiShape` (per-type) to `BldgAppearance::staticReg` / `TreeAppearance::staticReg` (per-instance). Each actor owns its own light slot index; flush-time patching reads from per-instance, not per-type.

Estimated fix size: ~30-50 lines + verification soak. Modest.

## Why this is high-priority

The skip-path delivering on its design intent collapses the per-actor cost from 9 µs to ~100 ns. At 954 actors, that's 9.08 ms → 95 µs. **A ~9 ms-per-frame win** at max zoom from a single targeted fix.

This is the highest-value perf win available in the rendering modernization arc. Track B's mission-load registration and Track C's compute cull are both downstream of this in terms of CPU savings — if the per-call cost itself dies, those slices' contribution shifts from "kill the cost" to "make the cost cheap to skip even when it runs."

## Recommended sequence

1. **Step 2 first** — disable registry, observe black behavior (5 minutes).
2. If H1 confirmed → write fix spec + plan + adversarial review + ship.
3. If H1 refuted → run Step 1 (population bisect) + Step 4 (slot-content peek).
4. After residual closes → flip `MC2_STATIC_UPDATE_SKIP=1` to default-on (with appropriate soak gate).

## Constraints inherited

Per `memory/update_skip_touch_regression.md` "Where to NOT start the next investigation":

- Do NOT add more layered texture-handle refresh code (texture is provably valid at draw time).
- Do NOT chase the alpha-test material-classification path (unrelated symptom).
- Do NOT revert the pin patch (closes a real bug — keep).

Plus per `memory/feedback_dont_paper_over_bugs.md`: any fix must be structural, not a workaround that masks the symptom.

## References

- `memory/update_skip_touch_regression.md` — two-mechanism framing
- `memory/pause_unpause_diagnostic_for_static_render_bugs.md` — discriminator history
- `docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md` — pin patch spec (mechanism 1, fixed)
- Pin patch commits: `52237d0` (txmmgr API), `d03ee3d` (registry wiring), `5a40f15` (cleanup revert)
- Failed leaf-refresh experiments: `c31dad6`, `0e7a4aa`, `c4c3157` (preserved on `wip-leaf-refresh-experiments`)
