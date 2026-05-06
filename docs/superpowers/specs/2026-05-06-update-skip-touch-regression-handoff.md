# Handoff: `MC2_STATIC_UPDATE_SKIP=1` touch-path regression

> Last major engine push for the MC2 OpenGL port. Slice 3 is shipped except for one deferred
> regression. **Don't paper over the bug — characterize it.**
> See `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_dont_paper_over_bugs.md`.

## Quick facts

- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Branch:** `nifty-mendeleev` (`claude/nifty-mendeleev`)
- **HEAD commit:** `afcd75b feat(3c+3d): static-prop registry default-on with cull-aware replay`
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe`
- **CMake binary:** `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## What's already shipped (don't re-investigate)

`afcd75b` shipped slice 3.C (trees) + 3.D (`BldgAppearance`) static-prop registry with default-on (`MC2_STATIC_PROP_REGISTRY=1`). It also includes a **cull-aware frame-stamp fix**:
- `mclib/msl.{h,cpp}` adds `cachedFrame_` on `TG_MultiShape`, stamped to `g_mc2FrameCounter` in `CacheGpuLightData()` and `ResubmitCachedGpuLightData()`.
- `GameOS/gameos/gos_static_prop_registry.cpp::flush()` skips entries whose `multi->getCachedFrame() != g_mc2FrameCounter`.
- This handles offscreen actors whose `update()` was cull-skipped while `render()` (via `g_useGpuStaticProps` cull-bypass) emitted `markVisible` — they silently don't draw that frame and resume next frame.

User-confirmed visually clean at maximum zoom (project's "wolfman zoom" — see `memory/wolfman_is_max_zoom.md`, just a legacy label for max zoom out).

## The deferred regression

With `MC2_STATIC_UPDATE_SKIP=1` set, ~99% of in-view actors hit the `touch()` path instead of `update()`. **Visual:** widespread black billboards across most trees and buildings. The cull-aware fix solves *flush-time* staleness for offscreen actors. The `UPDATE_SKIP=1` regression is **independent** — it manifests universally across all on-screen actors once the env is set.

**Test signature (this is the bug to fix):**
```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
MC2_STATIC_UPDATE_SKIP=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs
# Then visually inspect — trees and buildings render as black silhouettes
```

**Banner verification** (so you know the env actually took effect — see `memory/msvc_incremental_link_silent_staleness.md` for why this matters):
```
[INSTR v1] enabled: ... static_update_skip=1 static_prop_registry=1 ...
```

**Counter from the smoke run that established the regression** (after 600 frames):
```
[STATIC_UPDATE v1] frame=600 seen=67164 run=308 skip=66856 dyn_falling=0 dyn_other=0
```

That's `seen=67164` actor-frames, `skip=66856` (touch() called instead of update()), `run=308` (the unregistered/early ones).

## The control flow

The substitution gate is at `code/terrobj.cpp:703-716`:
```cpp
const bool appearanceClaimsStatic = appearance->IsStaticNow();
const bool ownerForcesDynamic     = getFlag(OBJECT_FLAG_FALLING);
const bool gpuPath = g_useGpuObjects || g_useGpuStaticProps;

if (ownerForcesDynamic)
    appearance->invalidateStaticRegistration();

if (s_staticUpdateSkip && gpuPath && appearanceClaimsStatic && !ownerForcesDynamic) {
    ++g_staticUpdateCounters.updates_skipped;
    if (s_staticUpdateTrace) {
        printf("[STATIC_UPDATE v1] event=skip frame=%u obj=%p\n",
            g_mc2FrameCounter, (void*)this);
        fflush(stdout);
    }
    appearance->touch();          // <-- the substituted path
} else {
    ++g_staticUpdateCounters.updates_run;
    if (ownerForcesDynamic && appearanceClaimsStatic)
        ++g_staticUpdateCounters.dyn_falling;
    appearance->update();         // <-- the normal path
}
```

`s_staticUpdateSkip` is parsed at file scope at `code/terrobj.cpp:88`:
```cpp
static const bool s_staticUpdateSkip = ParseEnvBool("MC2_STATIC_UPDATE_SKIP");
```
where `ParseEnvBool` (terrobj.cpp:79-85) treats unset/`"0"`/`"false"`/`"off"`/`"no"` as false, anything else as true.

## The substituted touch() path

`mclib/bdactor.cpp:4626` (`TreeAppearance::touch`):
```cpp
void TreeAppearance::touch()
{
    if (treeShape) {
        treeShape->ResubmitCachedGpuLightData();
        treeShape->Touch();
    }
}
```

The 3.D `BldgAppearance::touch` mirror is in `bdactor.cpp` (search for `BldgAppearance::touch`); same shape with `bldgShape`.

`mclib/msl.cpp:1793-1813` (`TG_MultiShape::ResubmitCachedGpuLightData`):
```cpp
void TG_MultiShape::ResubmitCachedGpuLightData()
{
    if (!g_useGpuObjects) return;

    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        firstShapeNodeLeaf = child;
        break;
    }

    if (firstShapeNodeLeaf != nullptr) {
        cachedGpuLightIndex_ = firstShapeNodeLeaf->ResubmitCachedLightData();
        cachedFrame_         = g_mc2FrameCounter;
    }
}
```

`mclib/tgl.cpp:2854-2857` (`TG_Shape::ResubmitCachedLightData`):
```cpp
uint32_t TG_Shape::ResubmitCachedLightData()
{
    return mcTextureManager->addLightDataStructure(&lightData_);
}
```

That's the entire touch-path lighting pipeline. It re-submits the per-shape `lightData_` (filled once during the bootstrap update's `CacheGpuLightData`) and trusts the dedup-cache to return a slot whose content matches.

## The expected-correct (non-skip) path

`mclib/msl.cpp:1770-1791` (`TG_MultiShape::CacheGpuLightData`) — same first-leaf scan, but calls `firstShapeNodeLeaf->GatherGpuObjectLightDataOnly()`:
```cpp
void TG_MultiShape::CacheGpuLightData()
{
    if (!g_useGpuObjects) return;

    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        firstShapeNodeLeaf = child;
        break;
    }

    if (firstShapeNodeLeaf != nullptr) {
        cachedGpuLightIndex_ = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();
        cachedFrame_         = g_mc2FrameCounter;
    }
}
```

`mclib/tgl.cpp:2848-2852` (`TG_Shape::GatherGpuObjectLightDataOnly`):
```cpp
uint32_t TG_Shape::GatherGpuObjectLightDataOnly()
{
    GatherLightsParameters(&lightData_);
    return mcTextureManager->addLightDataStructure(&lightData_);
}
```

The critical difference: `GatherGpuObjectLightDataOnly` REFILLS `lightData_` first via `GatherLightsParameters`. `ResubmitCachedLightData` does NOT — it trusts the cached snapshot.

`GatherLightsParameters` body at `mclib/txmmgr.cpp:959` reads class-static `TG_Shape::s_listOfLights` and `s_numLights`:
```cpp
void GatherLightsParameters(TG_HWLightsData* lights)
{
    gosASSERT(lights);
    ...
    const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
    const DWORD numLights = TG_Shape::s_numLights;

    for (uint32_t iLight = 0; iLight < numLights; iLight++) {
        if (num_lights == max_num_lights) break;
        if ((listOfLights[iLight] != NULL) && (listOfLights[iLight]->active)) {
            ...
            num_lights++;
        }
    }
    // ... eventually writes lights->numLights_ = num_lights;
}
```

Note: `s_listOfLights` is class-static. It's set per-actor via `SetLightList()` in update() (e.g., `bdactor.cpp:4445` for trees, `bdactor.cpp:2224` for buildings). With `UPDATE_SKIP=1`, `SetLightList` does NOT run for skipped actors. But `ResubmitCachedLightData` doesn't read `s_listOfLights` — it reads the per-actor `lightData_` already filled.

## The struct

`mclib/tgl.h:304-320` (`TG_HWLightsData`):
```cpp
struct TG_HWLightsData {
    float lightToWorld[MAX_HW_LIGHTS_IN_WORLD][16];
    float lightDir[MAX_HW_LIGHTS_IN_WORLD][4];
    float lightColor[MAX_HW_LIGHTS_IN_WORLD][4];
    float lightFalloff[MAX_HW_LIGHTS_IN_WORLD][4];
    int numLights_;
    int pad[3];

    TG_HWLightsData():numLights_(0) {  // <-- default ctor: numLights_=0
        memset(lightToWorld, 0, sizeof(lightToWorld));
        memset(lightDir, 0, sizeof(lightDir));
        memset(lightColor, 0, sizeof(lightColor));
        memset(lightFalloff, 0, sizeof(lightFalloff));
        pad[0] = pad[1] = pad[2] = 13;
    }
};
```

`mclib/tgl.h:745` — `TG_HWLightsData lightData_;` is a per-instance member of `TG_Shape` (verified, **NOT class-static**). Each leaf has its own.

## The shader consumer

`shaders/include/lighting.hglsl:191-271` (`calc_light`):
```glsl
vec3 calc_light(in int lights_index, in vec3 normal, in vec3 vertex_world_pos, in vec3 base_light)
{
    ObjectLights ld = light[lights_index];

    if (0 == ld.numLights.x)
#ifdef MC2_STATIC_PROP_LIGHTING
        return base_light;       // <-- tree leaves with aRGBLight=0xFF000000 + BaseVertexColor=0 → vec3(0)
#else
        return vec3(1, 1, 1);
#endif
    ...
}
```

Trees/buildings hit the `MC2_STATIC_PROP_LIGHTING` branch (defined in `static_prop.vert`). With `numLights==0`, vertex output is `base_light` which for tree leaves with `aRGBLight=0xFF000000` is `vec3(0) + g_scene.baseVertexColor.xyz`. `BaseVertexColor` is hardcoded `0x00000000` in `code/mechcmd2.cpp:164`, so `base_light = vec3(0)`. Result: black.

So the visual signature "black billboards" maps cleanly to the cached UBO slot's `numLights_` being zero (or negative-trivially-small contribution).

## Hypotheses (ranked, all UNTESTED)

### H1 — First-leaf identity drift (highest likelihood)

`CacheGpuLightData()` and `ResubmitCachedGpuLightData()` both iterate `listOfShapes[i]` and break at the first match for `processMe && rec.node && child->myType && child->myType->GetNodeType() == SHAPE_NODE`. If `listOfShapes` reorders OR `processMe` flags change between bootstrap and replay frames, the touch path reads a different leaf's `lightData_` than the bootstrap filled. The new "first leaf" has default-constructed `numLights_=0` — never went through `GatherLightsParameters`.

**Test:**
```cpp
// In ResubmitCachedGpuLightData and CacheGpuLightData, log:
fprintf(stderr, "[FIRST_LEAF] op=%s multi=%p firstLeaf=%p numLights=%d\n",
    "resubmit_or_cache", (void*)this, (void*)firstShapeNodeLeaf,
    firstShapeNodeLeaf ? firstShapeNodeLeaf->lightData_.numLights_ : -1);
```
Run with `MC2_STATIC_UPDATE_SKIP=1`. If the same multi's `firstLeaf` differs between bootstrap and any replay frame → H1 confirmed.

**Likely fix:** stash the first-leaf POINTER in `TG_MultiShape` at bootstrap time, reuse it in `ResubmitCachedGpuLightData` instead of re-scanning.

### H2 — `lightData_` is mutated by another path

The legacy `TG_Shape::Render()` at `tgl.cpp:2616` calls `GatherLightsParameters(&lightData_)` for the same shape:
```cpp
if (theShape->ib_ && theShape->vb_) {
    size_t numLights = MAX_HW_LIGHTS_IN_WORLD;
    GatherLightsParameters(&lightData_);
    ...
}
```
If a leaf takes BOTH the static-replay route AND the legacy render route in the same frame (e.g., shadow-render path, debug bypass, alpha-test fence), `lightData_` could be modified in flight. Specifically, if some other actor's `s_listOfLights` was set when this fires (because GatherLightsParameters reads class-static), `lightData_` gets cross-contaminated.

**Test:** `grep -n "lightData_" mclib/tgl.cpp mclib/msl.cpp` to enumerate write sites. Walk each one and check whether it can fire during the static-replay frame for an UPDATE_SKIP'd actor.

### H3 — Bootstrap captured `lightData_` before lights were available

If the FIRST `update()` (which fills `lightData_` via `CacheGpuLightData → GatherGpuObjectLightDataOnly → GatherLightsParameters`) ran BEFORE the world-light list was established (e.g., before mission load completed `eye->getWorldLights()`), the bootstrap `lightData_` has `numLights_=0`. Frozen forever.

**Test:** at `bdactor.cpp:4445` (just before `treeShape->SetLightList(...)`) print `eye->getNumLights()`. If 0 on early frames → first-update problem. Possible fix: gate `staticReg.registered = (recipeIndex >= 0)` on additional check that `lightData_.numLights_ > 0` at registration time.

### H4 — `GatherLightsParameters` reads stale class-static during a touch frame
Lower likelihood — `ResubmitCachedLightData` doesn't call `GatherLightsParameters`, but the `lightData_` content was BAKED with whatever `s_listOfLights` was at bootstrap. If that bootstrap happened during a frame where `s_listOfLights` had been mutated by an earlier actor (different lighting setup), the captured `lightData_` reflects that.

**Test:** dump `lightData_` contents (numLights, lightDir[0].w/.xyz, lightColor[0].rgb, lightToWorld[0]) at bootstrap and confirm the numbers look reasonable for what the actor SHOULD see.

## Diagnostic env vars in tree (from `afcd75b`)

```bash
# Per-population operator escapes (force dynamic submitMultiShape, bypass static replay)
MC2_FORCE_DYNAMIC_TREES=1
MC2_FORCE_DYNAMIC_BUILDINGS=1

# Disable registry entirely (off-switch)
MC2_STATIC_PROP_REGISTRY=0

# Per-instance field comparison: dynamic submit vs static replay (caps 8 each)
MC2_TREE_DIAG_TRACE=1

# Slot peek at flush time: numLights/firstType/firstColor/structCount (cap 16)
MC2_STATIC_PROP_TRACE=1
# Output: [STATIC_PROP] flush regIdx=0 lightIdx=2 count=1 nL=1 type0=0 c0=(0.902,0.902,0.859) sc=61

# Per-skip-event log (one line per actor whose update was substituted by touch)
MC2_STATIC_UPDATE_TRACE=1
```

C++ accessors (callable from any TU that includes `mclib/txmmgr.h`):
```cpp
mcTextureManager->getLightStructCount();   // current count of populated slots
mcTextureManager->peekLightSlot(idx);      // returns LightSlotPeek { numLights, firstType, firstColorR/G/B }
```

The trace macros to add new prints:
- `gos_static_prop_batcher.cpp:46` — `TREE_DIAG(fmt, ...)` (gated by `MC2_TREE_DIAG_TRACE`)
- `gos_static_prop_registry.cpp:22` — `SP_TRACE(fmt, ...)` (gated by `MC2_STATIC_PROP_TRACE`)

## Smoke recipe

Per `memory/feedback_smoke_policy_30s_mc2_01.md`: **mission 1 only, 30 seconds, no menu canary**. The smoke harness can't tell black billboards from green ones — visual inspection by the user is required. "It either happens or it doesnt" — don't run longer.

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"

# Reproduce the regression (current state):
MC2_STATIC_UPDATE_SKIP=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs

# Add traces to instrument:
MC2_STATIC_UPDATE_SKIP=1 MC2_STATIC_PROP_TRACE=1 MC2_TREE_DIAG_TRACE=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs

# Confirm the FIX path works (force dynamic — no regression because static path bypassed):
MC2_STATIC_UPDATE_SKIP=1 MC2_FORCE_DYNAMIC_TREES=1 MC2_FORCE_DYNAMIC_BUILDINGS=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs

# Smoke artifacts land at:
# tests/smoke/artifacts/<timestamp>/mc2_01.log
# tests/smoke/artifacts/<timestamp>/report.md
```

**The bug is camera-motion-dependent in part** — but the touch-path lighting regression is universal once `UPDATE_SKIP=1`, you'll see it without panning. For interactive verify after a candidate fix, the user does turn-away-and-back testing as well.

## Build / deploy

**Always `--config RelWithDebInfo` (release crashes with GL_INVALID_ENUM):**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"

# Build
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
--build build64 --config RelWithDebInfo --target mc2

# Deploy (NEVER cp -r — silently fails on Windows/MSYS2)
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"
```

## Build hygiene reminder

Per `memory/msvc_incremental_link_silent_staleness.md`: when investigating a class-layout-touching commit and the bug "won't go away despite source fixes", **delete `mc2.exe` before relinking** (and possibly `.lib` files). MSVC's incremental linker can produce binaries that don't reflect current source. We hit this same pattern earlier today.

```bash
# Force a full relink:
rm -f build64/RelWithDebInfo/mc2.exe \
      build64/out/GameOS/gameos/RelWithDebInfo/gameos.lib \
      build64/out/mclib/RelWithDebInfo/mclib.lib
"...cmake.exe" --build build64 --config RelWithDebInfo --target mc2
```

The `[INSTR v1]` banner at startup is the runtime-confirmation signal — if it doesn't reflect the env var change you expected, the build was stale.

## Files modified by `afcd75b` (the slice 3.C+3.D + cull-aware fix ship commit)

```
GameOS/gameos/gos_static_prop_batcher.cpp    (TREE_DIAG macro + traces, matrix offset fix, TEX_HANDOFF cap)
GameOS/gameos/gos_static_prop_registry.cpp   (parseEnvBoolWithDefault, default-on flip,
                                              extern g_mc2FrameCounter, cull-aware skip,
                                              extended SP_TRACE)
mclib/bdactor.cpp                            (3.D BldgAppearance methods, MC2_FORCE_DYNAMIC_*
                                              env-gated escapes, registration block)
mclib/bdactor.h                              (3.D StaticRegistration struct + members + decls)
mclib/msl.cpp                                (cachedFrame_ stamp in CacheGpuLightData and
                                              ResubmitCachedGpuLightData; extern g_mc2FrameCounter)
mclib/msl.h                                  (cachedFrame_ field + getCachedFrame accessor)
mclib/txmmgr.cpp                             (peekLightSlot definition)
mclib/txmmgr.h                               (LightSlotPeek struct + getLightStructCount +
                                              peekLightSlot declarations)
```

## Recent commit graph (newest first; for context)

```
afcd75b feat(3c+3d): static-prop registry default-on with cull-aware replay
26ba0da feat(slice3c): re-enable IsStaticNow + fix touch() light re-submission
b6bcdd8 fix(3c): remove TreeAppearance::IsStaticNow() — outer skip unsafe for trees
10e4435 fix(3c): move CacheGpuLightData() from render() to touch() — fixes black trees
a98c6df feat(3c): extend [INSTR v1] banner with static_prop_registry field
bb13c66 feat(3c): wire frameBegin()/flush()/init()/destroy() registry call sites
9e2c76b feat(3c): terrobj.cpp — invalidateStaticRegistration on fall + touch() in skip branch
b92dcfe feat(3c): TreeAppearance::render() static-registry fast path + registration block
d8fd603 feat(slice3c): TreeAppearance touch/IsStaticNow/invalidateStaticRegistration + destroy guard
d6dc617 feat(3c): TreeAppearance::StaticRegistration struct + method declarations
f63ed82 feat(3c): add GpuStaticPropRegistry (frameBegin/registerRecipe/markVisible/invalidate/flush)
c38da27 feat(3c): batcher batch-capture (getLastBuiltBatch) + submitCachedInstance
6ad4fd4 feat(3c): add Appearance::touch() + invalidateStaticRegistration() virtual no-ops
a882af2 feat(3c): add TG_Shape::Touch() + TG_MultiShape::Touch() + getCachedGpuLightIndex()
```

## Memory pointers (read these first)

All under `~/.claude/projects/A--Games-mc2-opengl-src/memory/`:

| File | What it tells you |
|---|---|
| `update_skip_touch_regression.md` | The deferred regression with the same hypotheses, summarized |
| `black_tree_bug_investigation_state.md` | What the cull-aware fix did (already in tree); previous black-tree bug class |
| `feedback_dont_paper_over_bugs.md` | If the symptom resolves without a characterized fix, that's a question, not a victory — bisect |
| `feedback_smoke_policy_30s_mc2_01.md` | Smoke recipe (30s mc2_01 only); never tier1 5/5; never longer |
| `feedback_dont_pile_fixes_pre_verification.md` | Apply ONE candidate fix at a time, verify, then stack |
| `feedback_subagent_write_mode_verify.md` | Always `git diff` after any subagent write, before trusting summary |
| `wolfman_is_max_zoom.md` | "Wolfman" is just max zoom out; not a separate mode; testing has been at this zoom |
| `msvc_incremental_link_silent_staleness.md` | Delete `mc2.exe` for class-layout commits; runtime banner is the truth |
| `cull_gates_are_load_bearing.md` | MC2's inView/canBeSeen chain gates update() calls; load-bearing |
| `tg_shape_static_state_lifecycle_trap.md` | Class-static reset must clear related fields together; precedent for H2 |
| `cpp_glsl_ubo_struct_lockstep.md` | UBO struct mismatches between C++ and GLSL silently corrupt arr[i>0] |
| `feedback_always_dispatch_adversarial_review.md` | High-stakes plans get adversarial review; this slice qualifies |

## Definition of done

When you can:

1. Set `MC2_STATIC_UPDATE_SKIP=1`, run mc2_01 30s smoke + interactive turn-away-and-back testing
2. See no black billboards anywhere
3. Have a written root-cause explanation in `memory/update_skip_touch_regression.md`
4. (Optional, separate ship-gate decision) Flip the default to ON; confirm Tracy shows the design's ~1.2ms/frame win on the `appearanceUpdate` zone

…then this slice closes and the engine push is done.

## Tracy zones to look at if you flip the default

| Zone | Source | Expected delta with `UPDATE_SKIP=1` |
|---|---|---|
| `TerrainObject::update appearanceUpdate` | `code/terrobj.cpp:715` (calls `appearance->update()`) | Significant DROP (the headline ~1.2ms savings) |
| `Render.GpuStaticProps` | `mclib/txmmgr.cpp` (registry+batcher flush) | Slight rise (more cached instances flowing through) |
| `TreeAppearance::render` / `BldgAppearance::render` | If wrapped (grep `ZoneScopedN` in `bdactor.cpp`) | Drop (markVisible vs full submitMultiShape) |
| `GpuStaticProps.Flush` | `gos_static_prop_batcher.cpp:1303` | Reflects actual draw-call cost |

Tracy is interactive (desktop GUI), can't be invoked from a smoke. After fix lands, the user does the capture.

## Discipline

1. **Don't paper over the bug** — characterize it. `memory/feedback_dont_paper_over_bugs.md`.
2. **Apply ONE candidate fix at a time, then test before stacking** — `memory/feedback_dont_pile_fixes_pre_verification.md`.
3. **Verify subagent file changes via `git diff`** — `memory/feedback_subagent_write_mode_verify.md`.
4. **Visual smoke can miss camera-motion bugs** — interactive testing required. `memory/feedback_smoke_policy_30s_mc2_01.md`.
5. **Don't add feature gates from transient iteration observations** — `memory/feedback_dont_overgate_during_iteration.md`.
6. **Inherited reviewer citations must be re-grep'd** — `memory/feedback_inherited_citations_must_regrep.md`.
7. **For high-stakes plans, dispatch adversarial review immediately** — `memory/feedback_always_dispatch_adversarial_review.md`.

## Adversarial-review trigger

Per the worktree CLAUDE.md "Review Discipline" section and `memory/feedback_always_dispatch_adversarial_review.md`: this is an architectural-endpoint slice ("close last engine push", "default-on flip candidate"). Once a candidate fix is ready, dispatch the adversarial-plan-review skill via `Agent` with `subagent_type=general-purpose` and the `.claude/skills/adversarial-plan-review.md` recipe. Don't wait to be asked.
