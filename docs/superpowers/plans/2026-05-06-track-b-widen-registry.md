# Track B — Widen Static-Prop Registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote `GpuStaticPropRegistry` from "fast-path replay of cull-approved instances that have rendered at least once" to "single source of truth for ALL world-static-prop instances in the mission." Mission-load bulk registration handles the dominant population (every prop spawned in `addObject`); a small `registerStaticProp(Appearance*, ...)` API handles late types (artillery towers, vTOL, mech-bay refits). Static fields (`modelMatrix`, `firstColorOffset`, `flags`, `aRGBHighlight`, `fogRGB`) bake at registration; the per-frame `cachedFrame_`-stamped lighting cache stays as-is for dynamic lights. The first-frame race is fixed structurally by pre-populating `cachedFrame_` at registration so the existing flush invariant accepts the entry on its first flush.

**Architecture:** Two-tier registration with one shared registry. Mission-load tier walks `objList` immediately after `loadTerrainObjects` and before `finalizeGeometry()` (`code/mission.cpp:2809..3044`), calling a new `Appearance::registerStatic()` virtual that builds a `GpuStaticPropInstance` recipe per leaf without calling `submitMultiShape`. Late-spawn tier exposes `registerStaticProp(Appearance*)` callable from the gameplay code that constructs `BldgAppearance` outside `addObject` (`code/artlry.cpp:1577,1645,1713`, `code/missiongui.cpp:4294,4336,5750,5756`, `code/warrior.cpp:7576`). The registry's existing `flush()` invariant (`gos_static_prop_registry.cpp:144`) is preserved; what changes is that registered entries arrive with `cachedFrame_ = g_mc2FrameCounter` already set, so the very first flush after registration sees a valid stamp without needing a prior `update()`. Dynamic lighting still flows through `CacheGpuLightData()`/`ResubmitCachedGpuLightData` (`mclib/msl.cpp:1789,1811`); only the substrate population widens.

**Tech Stack:** C++ (engine, MSVC RelWithDebInfo), MC2 OpenGL renderer, existing `GpuStaticPropRegistry`/`GpuStaticPropBatcher` substrate, `[STATIC_PROP]`/`[OBJBATCHER v1]`/`[TGL_POOL v1]`/`[DESTROY v1]` instrumentation, `scripts/run_smoke.py` tier1 gate.

**Spec references:**
- Decisions: `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` (Q4, Q5, Q6 + cross-cutting items 5-8)
- Recon: `docs/superpowers/explorations/2026-05-06-track-b-widen-registry-recon.md`
- Roadmap: `docs/superpowers/mc3-rendering-modernization-roadmap.md` (Track B section)
- Predecessor slice (which Track B widens): `docs/superpowers/specs/2026-05-05-slice3c-static-prop-registry-design.md`
- **Parallel prep slice (sibling, MUST READ before execution):** `docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md`

---

## Coordination with parallel prep work — MUST READ

> **Update 2026-05-06 (rev 2):** the original framing of this section claimed
> the alpha-test self-awareness slice closes the
> `MC2_STATIC_UPDATE_SKIP=1` black-billboard regression. **That framing is
> stale.** The user-interactive pause/unpause discriminator plus a 137K-line
> `[TEX_LIFECYCLE v1]` trace (`tests/smoke/artifacts/2026-05-06T10-52-34/mc2_01.log`)
> reframed the root cause as **stale `gosTextureHandle` after texture-manager
> eviction**, not a material-classification gap. The closing fix is the
> texture-pin spec at
> `docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md`, which
> attaches pin ownership to `RecipeRange` lifetime (see "RecipeRange Extension
> Table" below for the field-level coordination this introduces). Alpha-test
> self-awareness remains useful as a *sibling* hardening slice for material
> classification on the `treeDmgShape` / `GenericAppearance` paths, but it is
> not the root-cause fix for the visible black-billboard symptom.

Two parallel prep slices ship alongside Track B; both are structurally
complementary and stack with Track B's mission-load bulk register:

- **Alpha-test prep (parallel sibling) makes the registration *payload*
  correct regardless of registration *timing*** by reading the `a_`
  texture-name convention at `registerType()` time. **Hardens material
  classification** for `treeDmgShape` / `GenericAppearance` paths that
  load alpha-test assets without `SetAlphaTest`. Spec:
  `docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md`.
  Touches `gos_static_prop_batcher.cpp` and the `registerType()` function.
- **Texture-pin prep (parallel sibling) closes the
  `MC2_STATIC_UPDATE_SKIP=1` black-billboard regression** by giving
  registry-owned static-prop recipes a texture-lifetime contract.
  `registerRecipe` pins every referenced `mcTextureNodeIndex` and records
  the pinned set on the range; `invalidate(regIdx)` releases pins for that
  range; `destroy()` is the mission-teardown safety net for any
  unreleased ranges. First-render fallback (`bdactor.cpp:1668`, `:4313`)
  routes through `registerRecipe` and inherits pinning automatically.
  Spec: `docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md`.
  Touches `mclib/txmmgr.{h,cpp}` and
  `GameOS/gameos/gos_static_prop_registry.{h,cpp}`.
- **Track B (this plan) widens the registration *scope*** from
  cull-approved fast-path replay to every world-static-prop in the
  mission. Touches the same file set + `Mission::start()` + spawn sites.

**The two slices stack — neither preempts the other.** Per the prep spec
§Track B handoff hooks (lines 442-475):

1. The prep spec changes `registerType`'s signature to
   `registerType(TG_TypeShape*, TG_TypeMultiShape*)` (Option 1, the
   internal-only signature change). Track B's mission-load bulk register
   and `registerStaticProp` API inherit that new signature — calls must
   pass the parent `TG_TypeMultiShape*` through. Verify at write-time of
   each Track B task that touches `registerType` calls: read the prep
   spec's current state in `gos_static_prop_batcher.cpp` and mirror its
   call shape.
2. The prep spec adds an `event=warn` per-type trap that fires when a
   type has a `_a` texture but `alphaTestOn=0`. Track B's mission-load
   walk will register more types — operator should grep
   `[REG_TYPE v1] event=warn` in soak logs and surface load-site authors
   to fix during Track B's bulk-register cycle.
3. The prep spec's `flagSource` / `effectiveSource` trace fields stay
   active under Track B; the prep spec's parity check
   (`effective=0x0` count post-fix = 0 for known alpha-cutout typeIDs)
   continues to gate Track B's flip-on.

**Suspected interaction with Track B Risk Row 4 (firstColorOffset
asymmetry):** the prep spec touches `registerType`'s per-pkt loop where
`pkt.materialFlags` is baked. Track B's `buildRecipeFromShape` extraction
(Task 1 spike outcome A) bakes `firstColorOffset` in the same vicinity.
If the prep spec lands first (expected), Track B inherits a freshly-touched
loop body — re-grep at Task 1 plan-time before extraction. If a latent
`firstColorOffset` bug surfaces during prep-spec smoke, escalate via that
session, NOT this Track B branch.

**Sequencing:**
- Prep spec ships → soaks → flips default-on (its own gate).
- Track B execution starts AFTER prep spec ships (signature change +
  `event=warn` instrumentation are baseline).
- Track B's parity gate inherits prep spec's `effective=0x0` zero
  requirement, plus Track B's own `submit_legacy=0` + DESTROY count +
  identity + `[STATIC_FIRST_FRAME v1]=0` checks.

**Worktree CLAUDE.md rules in force:**
- Build: `cmake --build build64 --config RelWithDebInfo`
- Stock install must remain playable (default = legacy registry behavior until soak passes)
- Tier1 smoke is the regression gate
- Cull gates are load-bearing — Track B widens the registered population but must NOT widen any cull bypass; per-frame `update()` discipline preserved
- Debug instrumentation rule: lifecycle reworks land env-gated `[SUBSYS v1]` prints in same commit
- Documentation discipline: every cited symbol grep-verified at write-time

---

## Hard constraints before execution (post-advisor pass)

These three constraints must hold throughout Track B implementation.
Each blocks the slice from advancing if violated.

### HC-1 — No `void*` cast-compatibility for Bldg/Tree registration

The plan-writer's draft used a `void*` + cast-layout struct to write
`recipeIndex` / `registered` / `shape` into either
`BldgAppearance::StaticRegistration` or
`TreeAppearance::StaticRegistration`, relying on field-order parity.
**This is rejected.** Cast-layout compatibility is too fragile for
registry ownership code — silent failures if a future commit changes
field order on one but not the other.

Replacement: typed setter callbacks OR a shared interface struct.
Concretely, either:
- (a) Each Appearance class exposes a typed `setStaticRegistration(uint32_t recipeIdx)` virtual that the registry calls through the shared base `Appearance*`, OR
- (b) Both classes inherit from a small `IStaticRegistrable` interface defining the typed fields, and the registry holds `IStaticRegistrable*` not `void*`.

Pick at Task 2 plan-time; both are sound. The forbidden pattern is
typeless `void*` plus offset-based layout assumption.

### HC-2 — `firstColorOffset` ownership decided at Task 1, not deferred

Per Q16 in the brainstorm-decisions doc: Task 1's factoring spike
**cannot close** without a written architectural decision on
`firstColorOffset` ownership. Three valid answers (bake-at-register,
patch-per-frame, recipe-field redesign); the spike commits to one with
rationale before Task 2 begins.

The plan today has `firstColorOffset = 0` as a placeholder. **That is
not a final answer; that is a "decide this in Task 1."** Wider Bldg
population may surface a correctness bug if the value isn't truly
static. The recon flagged this as a candidate latent slice-3.C bug.
"We'll patch it if it breaks" is not an acceptable resolution.

If Task 1 cannot reach a confident decision, escalate to brainstorm or
a small recon-spike rather than punting. The cost of a wrong commit
here is correctness, not polish.

### HC-3 — First-render fallback retirement gated on invariant proof

Plan Task 9.6 currently retires the first-render lazy-fallback path
after Track B's mission-load + register-on-spawn coverage looks complete.
**This is rejected as time-only-gated.** A type that is genuinely
first-of-its-kind at a late-spawn site (no actor of that type
constructed earlier in the mission) would silently never register if
the fallback retires before that case is covered.

Retirement is gated on **proving the invariant**: every type reachable
from a `registerStaticProp()` call site has been seen by `registerType`
at least once before the call. Concretely:

- Add an instrumented assertion or `[STATIC_PROP_REG v1] event=type_unknown_at_late_spawn` log at the late-spawn path that fires when `registerStaticProp` finds the type not in `s_typeIndex`.
- Soak under tier1 plus an explicit "spawn one of every late-type during a single mission" stress test.
- Counter must read zero across soak before fallback retirement is even *considered*.
- If the counter is non-zero, root-cause the case (probably a registration ordering bug) and either fix or keep the fallback indefinitely.

The lazy fallback can stay forever as a safety net if the invariant
proof is incomplete. Retiring code that turns out to be load-bearing
is a worse outcome than carrying unused legacy.

---

## Background guardrails (5 advisor sharpenings inherited from A1)

1. **Lazy-init env probes.** Any new env flag (`MC2_STATIC_PROP_MISSION_LOAD_REG`, `MC2_STATIC_PROP_BAKE_AT_REG`, `MC2_STATIC_FIRST_FRAME_TRACE`) probes via the existing `parseEnvBoolWithDefault` pattern at `gos_static_prop_registry.cpp:23-29`. No startup-ordering hazards.
2. **Hard-fail self-tests.** A new `objectAdmissionPredicate_selftest`-style routine for the registration-time recipe builder (Task 2) hard-aborts on any assertion failure when `MC2_STATIC_PROP_BAKE_SELFTEST=1`. Operator opt-in; not silent.
3. **No drift between trace and production.** The `[STATIC_FIRST_FRAME v1]` counter (Task 5) reads the same `cachedFrame_` field the production flush reads. Both call into the same `getCachedFrame()` accessor at `mclib/msl.h:337`. No parallel-implementation hazard.
4. **Single-run captures for parity.** Pool peaks (Task 4) and registered-count baselines (Task 8) come from one tier1 run with `MC2_TGL_POOL_TRACE=1` + the new `[STATIC_PROP_REG v1]` summary. No across-run normalization needed at this stage.
5. **Identity diff for DESTROY.** The Track B parity gate (Task 7) reuses the A1 sed-normalize pattern: `obj=PTR; frame=N` substitutions; `kind+reason+gate-state-snapshot` tuple is the stable identity proxy until `[DESTROY v1]` line format gains a `seq=<id>` field.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `GameOS/gameos/gos_static_prop_registry.h` | Modify | Add `registerStaticProp(Appearance*)` API + `[STATIC_FIRST_FRAME v1]` counter accessor. |
| `GameOS/gameos/gos_static_prop_registry.cpp` | Modify | Implement `registerStaticProp`; pre-populate `cachedFrame_` at registration (structural first-frame fix); `[STATIC_FIRST_FRAME v1]` counter inside `flush()`; `[STATIC_PROP_REG v1]` summary on mission-load completion. |
| `GameOS/gameos/gos_static_prop_batcher.h` | Modify | Declare `buildRecipeFromShape(TG_Shape*, ..., GpuStaticPropInstance*)` — the "build without submit" surface (alternative path; see Task 1). |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | Modify | Implement `buildRecipeFromShape` (extracted from `submit()` body) OR document the "synchronous-update walk" alternative. Decision in Task 1. |
| `mclib/appear.h` | Modify | Add `virtual void registerStatic()` default no-op + `virtual void unregisterStatic()` for paired teardown. |
| `mclib/bdactor.h` | Modify | Override `registerStatic`/`unregisterStatic` for `BldgAppearance` and `TreeAppearance`. |
| `mclib/bdactor.cpp` | Modify | Implement overrides. Both call into `GpuStaticPropRegistry::registerStaticProp`. The existing first-render registration block at `bdactor.cpp:1662-1672` (Bldg) and `:4285-4293` (Tree) remains as a defensive fallback when mission-load registration didn't fire (env opt-out, late spawn). |
| `code/mission.cpp` | Modify | Add a registration walk between `loadTerrainObjects` and `finalizeGeometry` (between lines 2809 and 3044). |
| `code/objmgr.cpp` | Modify | Expose `enumerateStaticEligibleActors` helper for the mission-load walk to iterate `terrainObjects[]`/`buildings[]`/`turrets[]`/`gates[]`. |
| `code/artlry.cpp` | Modify | After `appearance->init(...)` at lines 1584, 1652, 1720 (the three Artillery::init paths — verify line numbers at write-time), call `registerStaticProp` on the BldgAppearance. |
| `code/missiongui.cpp` | Modify | After each `vTol[commanderID] = new BldgAppearance` site (4294, 4336, 5750, 5756), call `registerStaticProp`. |
| `code/warrior.cpp` | Modify | After `appearance = new BldgAppearance` at 7576, call `registerStaticProp`. |
| `code/simplecamera.cpp` | Read-only check | The `BldgAppearance` at line 495 is the simplecamera mech-preview; verify it's HUD-class (out of Track B scope, allowlisted via existing `06ac847` path) — do NOT add `registerStaticProp` there. |
| `code/gamecam.cpp:654` | Read-only check | Compass `BldgAppearance` is HUD allowlist (per Q5); explicitly NOT registered. |
| `GameOS/gameos/gameosmain.cpp` | Modify | Wire startup banner addition: `[INSTR v1] static_prop_mission_load_reg=on|off`. |
| `docs/superpowers/specs/2026-05-06-track-b-baseline-measurements.md` | Create | Per-tier1-mission baseline: pool peaks, registered counts, `[STATIC_FIRST_FRAME v1]` baseline (must be zero), pre-flip values. |
| `docs/superpowers/specs/2026-05-06-track-b-acceptance-envelope.md` | Create | Submit-legacy=0 confirmation, DESTROY count + identity parity, pool peak deltas. |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_b_widen_static_prop_registry.md` | Create | Memory file capturing flip-on date, peaks, soak findings. |
| `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` | Modify | Index entry for the new memory file. |

**Decomposition rationale:** the registry's existing API (`registerRecipe`, `markVisible`, `invalidate`, `flush`) is already well-factored — Track B layers a wider entry point (`registerStaticProp`) onto the same machinery. The hardest design decision is in Task 1 (the factoring spike); after that resolves, the rest is mechanical. Mission-load and late-spawn registration are intentionally separate code paths because the late-spawn sites are gameplay-code-owned and the mission-load walk is engine-code-owned — the layer separation matters.

---

## Task 1 — Resolve the "build batch without submitting" factoring (week-1 spike)

**This is the load-bearing design risk.** The existing first-render registration path (`bdactor.cpp:1662-1672`) uses `submitMultiShape() → getLastBuiltBatch() → registerRecipe()`. Mission-load registration runs BEFORE `finalizeGeometry()` AND before any actor's `update()`/`TransformMultiShape()` — which means at registration time the actors' `child->listOfVertices` and `child->listOfColors` are NULL, and `submitMultiShape` will silently skip every leaf (per `gos_static_prop_batcher.cpp:1189-1192`) and return a zero-batch success.

A registry walk that calls submitMultiShape directly during mission load would register empty recipes for every actor — silent failure, no draws.

**Resolve via spike before doing any other Track B work.** Two candidate approaches; pick one and document.

**Files (read-only at this stage; spike goal is design, not landed code):**
- Read: `GameOS/gameos/gos_static_prop_batcher.cpp:993-1223` (submitMultiShape body)
- Read: `GameOS/gameos/gos_static_prop_batcher.cpp:820-991` (submit body, where the actual instance is built)
- Read: `mclib/msl.cpp:1786-1811` (CacheGpuLightData / ResubmitCachedGpuLightData — what TransformMultiShape calls into)
- Read: `mclib/tgl.cpp` (TransformMultiShape — verify cost of synchronous one-time call per actor at mission load)

- [ ] **Step 1.1 — Spike candidate A: extract `buildRecipeFromShape` from `submit()`**

In `submit()` body at `gos_static_prop_batcher.cpp:820+`, the actual recipe construction populates `GpuStaticPropInstance{...}` from `shape`, `shapeToWorld`, `highlightARGB`, `fogARGB`, `flags`, `lightDataIndex`. This is the genuinely-static portion (the dynamic portion is the `firstColorOffset` patching done inside `submit()` based on per-frame upload state).

Spike:

- Walk `submit()` body and identify which lines build the recipe vs. which lines push it to per-frame upload state.
- Sketch a free function:

  ```cpp
  bool buildRecipeFromShape(TG_Shape* shape,
                            const Stuff::Matrix4D& shapeToWorld,
                            uint32_t highlightARGB,
                            uint32_t fogARGB,
                            uint32_t flags,
                            uint32_t lightDataIndex,
                            GpuStaticPropInstance* outRecipe);
  ```

  that builds the recipe WITHOUT touching per-frame state (`s_bucketsByType`, `s_lastBuiltBatch`, `s_counters`, the SSBO ring).
- Verify the recipe layout is fully reachable from those inputs alone. The 7 fields (`modelMatrix`, `typeID`, `firstColorOffset`, `flags`, `lightDataIndex`, `aRGBHighlight`, `fogRGB`) per `gos_static_prop_batcher.h:13-24`:
  - `modelMatrix` ← `shapeToWorld`
  - `typeID` ← `s_typeIndex.at(shape->myType)` (requires registerType to have run; this DOES happen during mission load, per recon §3 and `mission.cpp:1645` onMapLoad)
  - `firstColorOffset` ← 0 placeholder (per-frame value patched during static replay; current flush() does not patch this — investigate at spike whether it must be patched per-frame; recon §10 risk row "firstColorOffset becomes stale" flags this)
  - `flags` ← caller
  - `lightDataIndex` ← caller (will be UINT32_MAX placeholder at registration time; flush() patches per-frame)
  - `aRGBHighlight`, `fogRGB` ← caller
- Check: registerType walks `mission.cpp:1645 onMapLoad`. Confirm via grep that every actor type whose actors will be mission-load-registered has run through `registerType` by the time `loadTerrainObjects` returns. (Likely yes — the type-registration sweep walks every appearance type; actor creation in `addObject` triggers the appearance type's lazy registration. Verify.)

- [ ] **Step 1.2 — Spike candidate B: synchronous-update walk**

Alternative: instead of factoring submit, run a one-time synthetic `update()` per registered actor before the registration walk, so `TransformMultiShape` runs once and populates `listOfVertices`/`listOfColors`. Then call existing `submitMultiShape()`/`getLastBuiltBatch()` per actor.

Spike:

- Walk `Appearance::update()` discipline — what fields does it require to be valid? (`obj->getPosition()` is set by `obj->setTerrainPosition` at `objmgr.cpp:1248`, so position is valid.)
- Verify a synthetic update at mission-load time doesn't trip animation/activity state (per `isStaticEligible` at `bdactor.cpp:2638-2652` — the disqualifiers are `spinMe`/`bldgTypeHasAnimations`/`drawFlash`/`destructFX`/`activity`/`activity1`/`bdAnimationState != -1`; for mission-load actors none of these are active yet).
- Check the cost: ~3000 actors × one synthetic update is bounded but not free. Tracy-zone the spike to measure.

- [ ] **Step 1.3 — Pick approach + document**

Decision tree:

- **Candidate A clean factoring possible (recipe construction is genuinely separable from per-frame state):** proceed with Track B as designed. The mission-load walk calls `buildRecipeFromShape` per leaf and inserts via a new registry helper.
- **Candidate A not clean (recipe construction reads per-frame state inside submit body that resists extraction):** fall back to candidate B. Mission-load walk does synthetic update → submitMultiShape → registerRecipe (the existing path, just earlier).
- **Both spikes break unexpectedly:** revise scope. Add an interstitial slice: ship the `registerStaticProp(Appearance*)` API for late spawns first (Task 6 only), defer mission-load enumeration to a follow-up slice. Track B becomes "register-on-spawn for late types + `cachedFrame_` structural fix" with mission-load enumeration as a future widening.

Document the chosen approach as an addendum to the recon at `docs/superpowers/explorations/2026-05-06-track-b-widen-registry-recon.md` (append a "Spike outcome" section). Do not commit code from this task — this is design-only.

- [ ] **Step 1.4 — Update plan if approach changed**

If Candidate B is chosen, edit Tasks 2 and 5 below to swap "buildRecipeFromShape" for the synthetic-update path. If "interstitial slice" is chosen, defer Tasks 5, 8 to a follow-up plan and proceed with only Task 6 (late-spawn API) + Task 3 (structural cachedFrame_ fix) + Task 4 (instrumentation). Commit the plan-edit as a normal docs commit.

---

## Task 2 — Implement the chosen factoring (assumes Candidate A)

**If Task 1 picked Candidate B, this entire task swaps to "synthetic-update wrapper" — see Step 2.5 alternative.**

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 2.1 — Declare `buildRecipeFromShape` in batcher.h**

After the existing `submitCachedInstance` declaration at line 221, add:

```cpp
    // Track B: pure recipe-construction path. Builds a GpuStaticPropInstance
    // from genuinely-static inputs (shapeToWorld + per-shape constants).
    // Does NOT touch per-frame state (no s_bucketsByType insertion, no SSBO
    // ring, no counters). Returns false if shape's TG_TypeShape isn't
    // registered yet (caller must CPU-fallback for this actor).
    //
    // Used by GpuStaticPropRegistry::registerStaticProp for mission-load
    // and register-on-spawn registration paths, where TransformMultiShape
    // hasn't run yet so child->listOfVertices/listOfColors are NULL.
    [[nodiscard]] bool buildRecipeFromShape(TG_Shape* shape,
                                            const Stuff::Matrix4D& shapeToWorld,
                                            uint32_t highlightARGB,
                                            uint32_t fogARGB,
                                            uint32_t flags,
                                            GpuStaticPropInstance* outRecipe) const;
```

(`lightDataIndex` is intentionally NOT a parameter — registration time has no valid index; flush patches per-frame from `multi->getCachedGpuLightIndex()`.)

- [ ] **Step 2.2 — Implement in batcher.cpp**

Locate the recipe-building block inside `submit()` (at `gos_static_prop_batcher.cpp:820-991`; verify exact lines at write-time via `grep -n "GpuStaticPropInstance" GameOS/gameos/gos_static_prop_batcher.cpp | head -20`). The portion that constructs the per-instance struct is what's extracted.

Add after `submit()` body:

```cpp
bool GpuStaticPropBatcher::buildRecipeFromShape(
        TG_Shape* shape,
        const Stuff::Matrix4D& shapeToWorld,
        uint32_t highlightARGB,
        uint32_t fogARGB,
        uint32_t flags,
        GpuStaticPropInstance* outRecipe) const {
    if (!shape || !shape->myType || !outRecipe) return false;
    if (shape->myType->GetNodeType() != SHAPE_NODE) return false;
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(shape->myType);
    auto it = s_typeIndex.find(ts);
    if (it == s_typeIndex.end()) return false;  // type not registered

    // Populate the recipe directly. modelMatrix is shapeToWorld in row-major
    // form (uploaded GL_FALSE; see batcher.h:14 comment).
    *outRecipe = GpuStaticPropInstance{};
    // shapeToWorld → outRecipe->modelMatrix (row-major copy)
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            outRecipe->modelMatrix[r * 4 + c] = shapeToWorld(r, c);
        }
    }
    outRecipe->typeID           = it->second;
    outRecipe->firstColorOffset = 0;             // patched per-frame at flush
    outRecipe->flags            = flags;
    outRecipe->lightDataIndex   = 0xFFFFFFFFu;   // patched per-frame at flush
    // ARGB → float[4]: same unpacking as inside submit(). Verify against the
    // exact lines in submit() body — pattern must be byte-identical.
    outRecipe->aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    outRecipe->aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    outRecipe->aRGBHighlight[2] = ((highlightARGB      ) & 0xFF) / 255.0f;
    outRecipe->aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    outRecipe->fogRGB[0]        = ((fogARGB >> 16) & 0xFF) / 255.0f;
    outRecipe->fogRGB[1]        = ((fogARGB >>  8) & 0xFF) / 255.0f;
    outRecipe->fogRGB[2]        = ((fogARGB      ) & 0xFF) / 255.0f;
    outRecipe->fogRGB[3]        = ((fogARGB >> 24) & 0xFF) / 255.0f;
    return true;
}
```

(The exact aRGB unpack pattern MUST match `submit()`'s — copy verbatim from there. If `submit()` uses a different bit ordering, this version is wrong.)

- [ ] **Step 2.3 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build. New function is unreferenced — that's fine; Tasks 5-6 wire it.

- [ ] **Step 2.4 — Add hard-fail self-test (`MC2_STATIC_PROP_BAKE_SELFTEST=1`)**

Add a small self-test that builds a recipe for one synthetic shape using the new function, then re-builds via `submit()`'s path and confirms byte-identical output for the static fields. Per advisor sharpening #2 — opt-in, hard-aborts on mismatch.

Stub:

```cpp
// In gos_static_prop_batcher.cpp, near the top (after existing self-tests if any):
int GpuStaticPropBatcher_buildRecipeSelftest() {
    // Construct a synthetic TG_Shape with a registered TG_TypeShape pointer,
    // call buildRecipeFromShape, then call submit() with the same inputs and
    // capture s_lastBuiltBatch.back(). Compare the static fields (modelMatrix,
    // typeID, flags, aRGBHighlight, fogRGB) byte-identically.
    // Document any field that legitimately differs (lightDataIndex MUST differ
    // — registration uses sentinel; submit() uses the real index).
    // Print [STATIC_PROP_BAKE v1] event=selftest_pass|fail field=<name>.
    // Returns the number of mismatched fields.
    // Implementation: see Task 1 spike notes for the synthetic shape recipe.
    return 0;  // stub for now; real implementation lands during spike refinement
}
```

Wire from `GameOS/gameos/gameosmain.cpp` (alongside the other selftest gates) to call when `MC2_STATIC_PROP_BAKE_SELFTEST=1` is set, and `gosASSERT(false); std::abort();` on non-zero return — same pattern as Track A1 Task 4.3.

- [ ] **Step 2.5 — Alternative path if Candidate B (synthetic update) was picked**

If Task 1 chose synthetic-update over factoring: in this task, instead of `buildRecipeFromShape`, expose a helper:

```cpp
[[nodiscard]] bool ensureShapeTransformedOnce(Appearance* appear);
```

that calls `appear->update()` once if the actor's `child->listOfColors == NULL`. Hard-fails if update returns "destroy this actor" (which would indicate state misuse). The helper doesn't go in batcher.h — it goes alongside the registry (since it's an engine-level orchestration concern). Adapt Tasks 5-6 accordingly: instead of calling `buildRecipeFromShape`, the registry first calls `ensureShapeTransformedOnce`, then calls existing `submitMultiShape` → `getLastBuiltBatch` → store.

- [ ] **Step 2.6 — Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(track-b): add buildRecipeFromShape (extracted from submit)

Track B will register every static prop at mission load, before
TransformMultiShape has run, so child->listOfVertices/listOfColors are
still NULL. submitMultiShape would silently skip every leaf and return
a zero-batch success. buildRecipeFromShape walks only the
genuinely-static fields (modelMatrix, typeID, flags, aRGB*, fog*),
leaving firstColorOffset and lightDataIndex to be patched per-frame at
flush() time.

No production callers yet (Tasks 5/6).

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 3 — Pre-populate `cachedFrame_` at registration (structural first-frame fix)

**The Q6 structural fix.** At registration time (mission-load OR late-spawn), set `multi->cachedFrame_ = g_mc2FrameCounter`. The existing `flush()` invariant at `gos_static_prop_registry.cpp:144` checks `multi->getCachedFrame() != currentFrame` — if it's stale, skip. With pre-population in place, the very first flush after registration will see `cachedFrame_ == currentFrame` (since no frame has ticked between registration and that flush) and the entry is valid.

The next frame: `g_mc2FrameCounter++` happens at `gameosmain.cpp:941`. By the time that frame's flush runs, the actor's `update()` should have run (if the actor is in-view — the existing cull discipline). Update calls `CacheGpuLightData()` at `mclib/msl.cpp:1789` which restamps `cachedFrame_ = g_mc2FrameCounter`. So the gate stays satisfied. Offscreen actors' update is skipped; their `cachedFrame_` stays at the prior frame's value; flush correctly drops them.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_registry.cpp`
- Modify: `mclib/msl.h` (add a setter — `cachedFrame_` is currently only set inside `CacheGpuLightData`)

- [ ] **Step 3.1 — Add `setCachedFrame` to `TG_MultiShape`**

In `mclib/msl.h` near the `getCachedFrame()` accessor at line 337:

```cpp
        uint32_t getCachedFrame()        const { return cachedFrame_; }
        // Track B: setter exposed for GpuStaticPropRegistry::registerStaticProp
        // pre-population (structural first-frame fix). Otherwise cachedFrame_
        // is only mutated inside CacheGpuLightData / ResubmitCachedGpuLightData.
        void     setCachedFrame(uint32_t f)    { cachedFrame_ = f; }
```

- [ ] **Step 3.2 — Pre-populate in `registerRecipe`**

In `gos_static_prop_registry.cpp:90-102` (the existing `registerRecipe` body), at the end (after the `s_recipeRanges.push_back(rng);` line), add:

```cpp
    // Track B: structural first-frame fix. Pre-populate cachedFrame_ so the
    // very first flush after registration passes the
    // `getCachedFrame() != currentFrame` invariant at line 144 without
    // requiring a prior CacheGpuLightData() call. Without this, a
    // mission-load-registered actor would silently drop on frame 0
    // because cachedFrame_ would still be the UINT32_MAX sentinel.
    //
    // The next frame, update() runs and ResubmitCachedGpuLightData restamps
    // this; offscreen actors whose update is cull-skipped retain whatever
    // value was here, becoming stale and getting correctly dropped at flush.
    multi->setCachedFrame(g_mc2FrameCounter);
```

- [ ] **Step 3.3 — Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build.

- [ ] **Step 3.4 — Tier1 sanity check (no behavior change without env opt-in)**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS. Behavior unchanged because no new mission-load walk is wired yet (Tasks 5-6) — the only callers of `registerRecipe` today are the two first-render registration blocks in `bdactor.cpp:1668-1672, :4290-4292`, where `cachedFrame_` is already correctly stamped (CacheGpuLightData ran in the same render() before submitMultiShape returned). The new line is a no-op rewrite to the same value at those callsites.

- [ ] **Step 3.5 — Commit**

```bash
git add GameOS/gameos/gos_static_prop_registry.cpp mclib/msl.h
git commit -m "feat(track-b): pre-populate cachedFrame_ at registration

Structural fix for the Track B mission-load registration first-frame
race (per Q6 of brainstorm-decisions doc). At registration time,
cachedFrame_ is set to g_mc2FrameCounter so the existing flush()
staleness gate accepts the entry without requiring a prior
CacheGpuLightData() call. Subsequent frames refresh via the existing
TG_Shape::CacheGpuLightData path; this only changes the boundary
condition at registration.

Existing first-render callers (bdactor.cpp:1668, :4290) are no-op
rewrites — CacheGpuLightData already stamped cachedFrame_ in the same
render() before submitMultiShape returned.

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 4 — Wire `[STATIC_FIRST_FRAME v1]` counter

The counter (per Q6) is **proof-of-fix**, not deferral gate. It must read zero across tier1 with the structural fix in place. Non-zero indicates the pre-populated stamp didn't satisfy the flush invariant — escalate.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_registry.h` — accessor declaration
- Modify: `GameOS/gameos/gos_static_prop_registry.cpp` — counter increment + summary

- [ ] **Step 4.1 — Add counter and accessor**

In `gos_static_prop_registry.h` after the existing function declarations:

```cpp
// Track B [STATIC_FIRST_FRAME v1] counter. Increments inside flush() each
// time an entry is skipped due to cachedFrame_ != currentFrame on a frame
// where the entry's recipeIndex has just been registered (i.e., this is
// the entry's FIRST flush). With the structural pre-population fix this
// must always be zero across tier1; non-zero indicates the flush invariant
// doesn't accept the pre-populated stamp.
uint64_t getStaticFirstFrameSkipCount();
```

- [ ] **Step 4.2 — Implement counter inside `flush()`**

In `gos_static_prop_registry.cpp`:

```cpp
namespace {
// ... existing namespace contents ...
static uint64_t s_firstFrameSkipCount = 0;
// Per-regIdx "this is the first flush since registration" tracking. We don't
// need a hash set — we can store a "registeredOnFrame" field on RecipeRange
// itself and flag the skip only when the *first* flush after registration
// fails the invariant.
}

// extend RecipeRange:
struct RecipeRange {
    uint32_t       first;
    uint32_t       count;
    TG_MultiShape* multi;
    // Track B (this plan):
    uint32_t       registeredOnFrame;  // g_mc2FrameCounter at registerRecipe()
    bool           firstFlushSeen;     // cleared in registerRecipe; set on first flush
    // Texture-pin sibling (docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md):
    std::vector<DWORD> pinnedTextureNodes;  // mcTextureNodeIndex values pinned for this range
    bool               pinsReleased;        // double-release guard for invalidate→destroy ordering
};

// Field-level coordination with the texture-pin sibling spec.
// Both extensions are required and stack cleanly: the pin sibling adds
// pinnedTextureNodes / pinsReleased; Track B adds registeredOnFrame /
// firstFlushSeen. Whichever lands first must reserve space for the other's
// fields by leaving the struct extensible (a std::vector + bool tail is
// trivially safe). When both are in tree, registerRecipe() must populate
// all four (Track B: stamp registeredOnFrame, clear firstFlushSeen; pin
// sibling: walk multi's textures, pin each, push nodeIdx onto
// pinnedTextureNodes, set pinsReleased=false).
```

In `registerRecipe`, set `rng.registeredOnFrame = g_mc2FrameCounter; rng.firstFlushSeen = false;`.

In `flush()`, just before the `if (rng.multi->getCachedFrame() != currentFrame) continue;` line at :144, check whether this is the first flush for this entry:

```cpp
        const bool isFirstFlush = !rng.firstFlushSeen;
        if (rng.multi->getCachedFrame() != currentFrame) {
            if (isFirstFlush) {
                ++s_firstFrameSkipCount;
                // Cap warning prints to first 16 to keep logs sane (mirrors
                // the existing flush-trace cap pattern at :161).
                static int s_firstFrameWarnPrinted = 0;
                if (s_firstFrameWarnPrinted < 16) {
                    ++s_firstFrameWarnPrinted;
                    fprintf(stderr,
                        "[STATIC_FIRST_FRAME v1] event=skip_first_flush regIdx=%u "
                        "registeredOnFrame=%u currentFrame=%u cachedFrame=%u\n",
                        regIdx, rng.registeredOnFrame, currentFrame,
                        rng.multi->getCachedFrame());
                    fflush(stderr);
                }
            }
            continue;
        }
        rng.firstFlushSeen = true;
```

(The `rng` here is currently `const RecipeRange&` at :133 — change to non-const reference, or fetch by index for the mutation. Verify at write-time.)

Add accessor at file scope:

```cpp
uint64_t GpuStaticPropRegistry::getStaticFirstFrameSkipCount() {
    return s_firstFrameSkipCount;
}
```

- [ ] **Step 4.3 — Emit summary on mission unload**

> **Coordination note (added in lockstep with the texture-pin sibling spec, rev 3):**
> Both this plan and `docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md`
> mutate the same 4-line `destroy()` body. The pin spec is **authoritative** for
> the combined `destroy()` body (see "Combined `destroy()` body — AUTHORITATIVE
> when both this spec and Track B land" in that doc). If only Track B has landed
> at the time of merge, copy the snippet below verbatim and leave a `TODO:
> integrate texture-pin destroy() additions per
> docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md`
> comment so the next merge-train sees the gap.

In `gos_static_prop_registry.cpp:79-83 (destroy)` — Track-B-only addition:

```cpp
void destroy() {
    fprintf(stderr,
        "[STATIC_FIRST_FRAME v1] event=summary skip_count=%llu\n",
        (unsigned long long)s_firstFrameSkipCount);
    fflush(stderr);
    s_firstFrameSkipCount = 0;
    s_recipes.clear();          s_recipes.shrink_to_fit();
    s_recipeRanges.clear();     s_recipeRanges.shrink_to_fit();
    s_liveRangeIndices.clear(); s_liveRangeIndices.shrink_to_fit();
    // TODO: integrate texture-pin destroy() additions per
    //   docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
    //   ("Combined destroy() body" section). Adds a release-pins loop and a
    //   pin_summary emit BEFORE the clear/shrink calls.
}
```

- [ ] **Step 4.4 — Build and run sanity check**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 30 --kill-existing
grep -E "STATIC_FIRST_FRAME v1" tests/smoke/artifacts/<latest>/mc2_01*.log
```

Expected: `event=summary skip_count=0`. (Without the mission-load walk wired yet, registration only happens at first-render, where `cachedFrame_` was already stamped by CacheGpuLightData — so first-flush skips should be impossible. If non-zero appears even at this stage, the counter implementation has a logic error — fix before proceeding.)

- [ ] **Step 4.5 — Commit**

```bash
git add GameOS/gameos/gos_static_prop_registry.h GameOS/gameos/gos_static_prop_registry.cpp
git commit -m "feat(track-b): add [STATIC_FIRST_FRAME v1] counter

Per Q6 of brainstorm-decisions: counter ships as proof-of-fix for the
structural pre-population of cachedFrame_, NOT as a deferral gate.
Must read zero across tier1; non-zero indicates the flush invariant
doesn't accept the pre-populated stamp and we escalate.

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 5 — Mission-load registration walk

**Files:**
- Modify: `mclib/appear.h` — virtual declarations
- Modify: `mclib/bdactor.h` / `.cpp` — overrides for Bldg + Tree
- Modify: `GameOS/gameos/gos_static_prop_registry.h` / `.cpp` — `registerStaticProp` API
- Modify: `code/objmgr.cpp` — enumeration helper
- Modify: `code/mission.cpp` — call site between `loadTerrainObjects` and `finalizeGeometry`

- [ ] **Step 5.1 — Add `registerStatic` virtual to Appearance**

In `mclib/appear.h` near the existing `IsStaticNow`/`invalidateStaticRegistration` virtuals at lines 138/151, add:

```cpp
    // Track B: mission-load and late-spawn registration entry. Default no-op;
    // BldgAppearance and TreeAppearance override. Generic and GV explicitly
    // do NOT override (out of scope per Q5 — HUD allowlist via 06ac847 path).
    virtual void registerStatic() {}
```

- [ ] **Step 5.2 — Override on BldgAppearance and TreeAppearance**

In `mclib/bdactor.h:288-294` (Bldg block) add:

```cpp
        virtual void registerStatic() override;
```

Mirror in TreeAppearance's parallel block.

In `mclib/bdactor.cpp`, implement after the existing `IsStaticNow` body around `:2654` (Bldg) and `:4733` (Tree):

```cpp
void BldgAppearance::registerStatic() {
    if (!isStaticEligible()) return;
    if (staticReg.registered) return;        // idempotent
    if (!bldgShape) return;
    if (!GpuStaticPropRegistry::isEnabled()) return;
    GpuStaticPropRegistry::registerStaticProp(
        bldgShape,
        /* aRGBHighlight= */ 0u,             // Bldg uses per-shape; per-instance highlight comes via touch()
        /* fogRGB=        */ fogRGB,
        /* flags=         */ /* per-shape; fold lightsOut/isWindow/isSpotlight in registerStaticProp */ 0u,
        &staticReg);
    // staticReg fields populated by registerStaticProp on success.
}
```

(The flags/highlight handling needs to follow the per-leaf pattern — at registration time we don't have per-leaf flags yet because `submitMultiShape`'s second loop computed them per-child. The clean answer: `registerStaticProp` receives the multishape and walks its leaves itself, calling `buildRecipeFromShape` per leaf with per-leaf inputs. The Appearance::registerStatic just hands off the multishape pointer + actor-level state.)

- [ ] **Step 5.3 — Implement `GpuStaticPropRegistry::registerStaticProp`**

In `gos_static_prop_registry.h`:

```cpp
// Track B: mission-load + register-on-spawn entry point. Walks the multishape's
// leaves, calls buildRecipeFromShape per leaf, batches them into a recipe range,
// pre-populates cachedFrame_, and writes the recipeIndex into outStaticReg.
// Returns the recipeIndex (>= 0) on success, -1 if disabled, type unregistered,
// or no SHAPE_NODE leaves.
int32_t registerStaticProp(TG_MultiShape* multi,
                           uint32_t       aRGBHighlight,
                           uint32_t       fogRGB,
                           uint32_t       perActorFlags,
                           void*          outStaticReg);
```

(`outStaticReg` is `void*` to avoid pulling bdactor.h into the registry — caller casts to `BldgAppearance::StaticRegistration*` or `TreeAppearance::StaticRegistration*`. Both have the same field names: `recipeIndex`, `registered`, `shape`. Verify at write-time via grep.)

In `gos_static_prop_registry.cpp`:

```cpp
int32_t registerStaticProp(TG_MultiShape* multi,
                           uint32_t aRGBHighlight,
                           uint32_t fogRGB,
                           uint32_t perActorFlags,
                           void* outStaticReg) {
    if (!s_enabled || !multi) return -1;
    GpuStaticPropBatcher& batcher = GpuStaticPropBatcher::instance();
    std::vector<GpuStaticPropInstance> batch;
    batch.reserve(multi->numTG_Shapes);
    for (int i = 0; i < multi->numTG_Shapes; ++i) {
        TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node;
        if (!child->myType || child->myType->GetNodeType() != SHAPE_NODE) continue;
        uint32_t flags = perActorFlags;
        if (child->lightsOut)   flags |= (1u << 0);
        if (child->isWindow)    flags |= (1u << 1);
        if (child->isSpotlight) flags |= (1u << 2);
        Stuff::Matrix4D xform(rec.shapeToWorld);
        GpuStaticPropInstance inst;
        if (!batcher.buildRecipeFromShape(
                child, xform,
                child->aRGBHighlight, child->fogRGB,
                flags, &inst)) {
            // Unregistered type — abort the whole registration; fall back to
            // first-render path. Mirror submitMultiShape's "fail multishape on
            // any unregistered SHAPE_NODE" policy.
            return -1;
        }
        batch.push_back(inst);
    }
    if (batch.empty()) return -1;
    int32_t regIdx = registerRecipe(multi, batch);
    if (regIdx >= 0 && outStaticReg) {
        // Cast layout matches BldgAppearance::StaticRegistration AND
        // TreeAppearance::StaticRegistration (both have the same 3 fields in
        // the same order; verified at bdactor.h:209-214 and :503-508).
        struct StaticRegLayout {
            bool             registered;
            TG_MultiShape*   shape;
            int32_t          recipeIndex;
        };
        auto* sr = static_cast<StaticRegLayout*>(outStaticReg);
        sr->registered  = true;
        sr->shape       = multi;
        sr->recipeIndex = regIdx;
    }
    return regIdx;
}
```

(VERIFY: `BldgAppearance::StaticRegistration` field order. Per recon §9 + grep of `bdactor.h:209-214`: `bool registered; TG_MultiShapePtr shape; int32_t recipeIndex;`. The cast layout struct above MUST match this exactly. If layouts differ between Bldg and Tree, factor a tagged-union or pass field setter callbacks instead of the cast.)

- [ ] **Step 5.4 — Add `[STATIC_PROP_REG v1]` summary**

After mission-load registration walk completes, emit one line per mission with:
- total actors enumerated
- total registered (success)
- total skipped (unregistered type / not eligible)
- per-class breakdown (Bldg, Tree, Turret, Gate)

Add inside the walk site (Step 5.6 below).

- [ ] **Step 5.5 — Add enumeration helper to ObjectManager**

In `code/objmgr.cpp`, after the existing `loadTerrainObjects` definition, add:

```cpp
void GameObjectManager::registerStaticPropsForMissionLoad() {
    int totalEnumerated = 0, totalRegistered = 0, totalSkipped = 0;
    auto registerOne = [&](GameObjectPtr obj, const char* className) {
        if (!obj) return;
        ++totalEnumerated;
        Appearance* app = obj->getAppearance();
        if (!app) { ++totalSkipped; return; }
        // Snapshot before/after to detect success.
        // Bldg/Tree appearances have staticReg; cast via dynamic_cast or
        // virtual registerStatic() doing its own check (preferred — keeps
        // typing in mclib/).
        app->registerStatic();
        // Successful registration is observable via the appearance's
        // staticReg.registered field; both Bldg and Tree expose this via
        // a getter — add `bool isStaticRegistered() const` virtual to
        // Appearance if not already present.
        if (app->isStaticRegistered()) ++totalRegistered;
        else                            ++totalSkipped;
    };

    // Walk the same arrays loadTerrainObjects populated.
    for (long i = 0; i < numTerrainObjects; ++i)
        registerOne(terrainObjects[i], "TerrainObject");
    for (long i = 0; i < numBuildings; ++i)
        registerOne(buildings[i], "Building");
    for (long i = 0; i < numTurrets; ++i)
        registerOne(turrets[i], "Turret");
    for (long i = 0; i < numGates; ++i)
        registerOne(gates[i], "Gate");

    fprintf(stderr,
        "[STATIC_PROP_REG v1] event=mission_load enumerated=%d registered=%d skipped=%d\n",
        totalEnumerated, totalRegistered, totalSkipped);
    fflush(stderr);
}
```

(Verify `numTerrainObjects`/`terrainObjects`/`numBuildings`/`buildings`/`numTurrets`/`turrets`/`numGates`/`gates` field names against the actual class layout in `code/objmgr.h` at write-time.)

Add `bool isStaticRegistered() const { return false; }` virtual to `Appearance` and override in Bldg/Tree to read `staticReg.registered`.

- [ ] **Step 5.6 — Wire the call from Mission::init**

In `code/mission.cpp` between `loadTerrainObjects` (`:2809`) and `finalizeGeometry` (`:3044`), at a suitable seam (verify exact lines at write-time):

```cpp
{ ZoneScopedN("Mission::init ObjectManager::loadTerrainObjects"); ObjectManager->loadTerrainObjects(&pakFile, loadProgress, 30); }

// ... existing code between 2809 and 3044 stays unchanged ...

// Track B: mission-load static-prop registration walk. Runs AFTER all
// addObject calls have spawned actors and BEFORE finalizeGeometry uploads
// the shared VBO/IBO. registerStatic() builds recipe ranges via
// buildRecipeFromShape (no GL state required; submitMultiShape's
// listOfColors/listOfVertices NULL gate is bypassed by construction).
{
    ZoneScopedN("Mission::init Track B static-prop registration walk");
    ObjectManager->registerStaticPropsForMissionLoad();
}

GpuStaticPropBatcher::instance().finalizeGeometry();
```

- [ ] **Step 5.7 — Wire env opt-out**

The walk must be opt-out via `MC2_STATIC_PROP_MISSION_LOAD_REG=0` for the soak window. Default = on after Task 9 flip. During Tasks 5-8, default = off; soak runs explicitly set `MC2_STATIC_PROP_MISSION_LOAD_REG=1`.

In `gos_static_prop_registry.cpp` near the existing `s_enabled` parse:

```cpp
static const bool s_missionLoadRegEnabled =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_MISSION_LOAD_REG", false);

bool isMissionLoadRegEnabled() { return s_missionLoadRegEnabled; }
```

In `objmgr.cpp::registerStaticPropsForMissionLoad`, gate at the top:

```cpp
if (!GpuStaticPropRegistry::isMissionLoadRegEnabled()) return;
```

- [ ] **Step 5.8 — Build + tier1 (default-off, no behavior change)**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS, no behavior change.

- [ ] **Step 5.9 — Tier1 + 30s mc2_01 with mission-load walk on**

```bash
MC2_STATIC_PROP_MISSION_LOAD_REG=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 30 --kill-existing
grep "STATIC_PROP_REG v1" tests/smoke/artifacts/<latest>/mc2_01*.log
grep "STATIC_FIRST_FRAME v1" tests/smoke/artifacts/<latest>/mc2_01*.log
```

Expected:
- `event=mission_load enumerated=N registered=M skipped=K` with `M >> 0` (most actors registered).
- `event=summary skip_count=0` — proof the structural fix works.
- Any non-zero `[STATIC_FIRST_FRAME v1]` count = hard failure: the pre-populated stamp doesn't satisfy the flush invariant. Investigate before proceeding.

- [ ] **Step 5.10 — Commit**

```bash
git add mclib/appear.h mclib/bdactor.h mclib/bdactor.cpp \
        GameOS/gameos/gos_static_prop_registry.h GameOS/gameos/gos_static_prop_registry.cpp \
        code/objmgr.cpp code/objmgr.h code/mission.cpp
git commit -m "feat(track-b): mission-load static-prop registration walk

Walks every BldgAppearance/TreeAppearance spawned by addObject AFTER
loadTerrainObjects and BEFORE finalizeGeometry. registerStatic() calls
GpuStaticPropRegistry::registerStaticProp which uses
buildRecipeFromShape to construct recipes without requiring per-actor
TransformMultiShape to have run.

Default-off via MC2_STATIC_PROP_MISSION_LOAD_REG=0; soak via
MC2_STATIC_PROP_MISSION_LOAD_REG=1. The existing first-render
registration block in BldgAppearance::render / TreeAppearance::render
remains as defensive fallback.

[STATIC_PROP_REG v1] summary line emits per mission with
enumerated/registered/skipped counts. [STATIC_FIRST_FRAME v1] verified
zero on tier1 mc2_01 with mission-load walk on.

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 6 — Late-spawn `registerStaticProp` API + spawn-site wiring

**Per Q4: register-on-spawn API for late types. NO first-render lazy fallback as the official path** (the existing first-render block stays as a defensive fallback during soak only — Task 9 retires it).

**Files:**
- Modify: `code/artlry.cpp` — three Artillery::init paths
- Modify: `code/missiongui.cpp` — four vTol new sites
- Modify: `code/warrior.cpp` — one mech-bay refit site

- [ ] **Step 6.1 — Verify Artillery sites**

Confirm at write-time the three Artillery::init `new BldgAppearance` sites' line numbers:

```bash
grep -n "appearance = new BldgAppearance" code/artlry.cpp
```

Expected: 3 sites (1577, 1645, 1713 per recon — verify exact lines). Each is followed shortly by an `appearance->init(...)` call.

After each `appearance->init(...)` returns, before the function exits, add:

```cpp
appearance->registerStatic();
```

Why right after init: at this point `appearType` is set, `bldgShape` is constructed inside init, the actor's position/rotation are set by the caller (who finishes config after init returns), so registration could happen even later — but right after init is the simplest hook. Verify `bldgShape` is non-null at that point; if not, defer until position/rotation are set (one line below in the caller).

- [ ] **Step 6.2 — Wire vTol sites in missiongui.cpp**

For each of the four sites at 4294, 4336, 5750, 5756:

```cpp
vTol[commanderID] = new BldgAppearance;
// ... existing init/setup ...
vTol[commanderID]->registerStatic();   // Track B: register-on-spawn (Q4)
```

Place the new line after the existing setup that establishes shape+position+rotation. Verify the surrounding code at each site to find the right placement.

- [ ] **Step 6.3 — Wire warrior.cpp:7576**

```cpp
BldgAppearance* appearance = new BldgAppearance;
// ... existing init/setup ...
appearance->registerStatic();   // Track B: register-on-spawn (Q4)
```

- [ ] **Step 6.4 — Explicitly NOT registered: HUD allowlist sites**

Add documenting comment at:
- `code/gamecam.cpp:510` (skybox `theSky`) — comment: `// Track B Q5: skybox is HUD allowlist (06ac847), NOT a world static prop. Do not call registerStatic.`
- `code/gamecam.cpp:654` (compass) — same comment.
- `code/simplecamera.cpp:495` (mech preview) — verify scope; if HUD, same comment.

These three sites already inherit `Appearance::registerStatic()`'s default no-op, but the comment makes the boundary explicit per Q5.

- [ ] **Step 6.5 — Build + tier1 sanity (env off — only first-render path active)**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS. With `MC2_STATIC_PROP_MISSION_LOAD_REG=0`, `registerStatic()` calls inside late-spawn sites are still no-ops (`Appearance::registerStatic`'s default) until we change the gate. (Alternative: gate `registerStatic()` itself on `s_missionLoadRegEnabled` inside the Bldg override so all the new wiring is dormant by default.)

- [ ] **Step 6.6 — Tier1 mc2_01 with both env on**

```bash
MC2_STATIC_PROP_MISSION_LOAD_REG=1 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 30 --kill-existing
grep "STATIC_PROP_REG v1" tests/smoke/artifacts/<latest>/mc2_01*.log
```

Expected: `[STATIC_PROP_REG v1] event=late_register class=Building` lines emit during gameplay if any late-spawn fires (mc2_01 may not actually trigger artillery in 30s — that's fine, the wiring is exercised in other tier1 missions; mc2_17 is the canonical artillery-using test).

- [ ] **Step 6.7 — Commit**

```bash
git add code/artlry.cpp code/missiongui.cpp code/warrior.cpp code/gamecam.cpp code/simplecamera.cpp
git commit -m "feat(track-b): wire late-spawn registerStaticProp at gameplay sites

Per Q4 of brainstorm-decisions: artillery towers (artlry.cpp), vTol
spawns (missiongui.cpp), and mech-bay refit (warrior.cpp) call
registerStatic() immediately after the appearance is constructed and
positioned. HUD allowlist sites (skybox, compass, simplecamera) are
explicitly NOT wired and have explanatory comments per Q5.

The existing first-render registration block in *Appearance::render
remains as defensive fallback during soak; Task 9 retires it.

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 7 — Pool sizing audit

Per recon §6: face pool peaks at 44% on mc2_17 today. Track B's widening of the registered population must NOT translate to "more TGL pool allocations per frame" — once registered, the static-replay path bypasses TGL pool allocation. But we must verify that empirically.

**Files:**
- Create: `docs/superpowers/specs/2026-05-06-track-b-baseline-measurements.md`
- Possibly modify: `code/mission.cpp:3141-3154` (pool init sizes, only if any tier1 mission peaks above 80%)

- [ ] **Step 7.1 — Capture baseline pool peaks (mission-load OFF)**

```bash
MC2_TGL_POOL_TRACE=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "TGL_POOL v1.*event=mission_unload\|TGL_POOL v1.*summary" \
    tests/smoke/artifacts/<latest>/${m}*.log \
    > /tmp/track-b-pools-baseline-${m}.txt
done
```

Note that mc2_01/03/24 may not reach gameplay frames; record what's available.

- [ ] **Step 7.2 — Capture peak pool values with mission-load ON**

```bash
MC2_STATIC_PROP_MISSION_LOAD_REG=1 \
MC2_TGL_POOL_TRACE=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "TGL_POOL v1.*event=mission_unload\|TGL_POOL v1.*summary" \
    tests/smoke/artifacts/<latest>/${m}*.log \
    > /tmp/track-b-pools-modern-${m}.txt
done
```

- [ ] **Step 7.3 — Capture registered counts**

```bash
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "STATIC_PROP_REG v1" tests/smoke/artifacts/<latest>/${m}*.log \
    > /tmp/track-b-regcount-${m}.txt
done
```

This gives total mission static-prop count (= theoretical widened cap).

- [ ] **Step 7.4 — Author the baseline doc**

`docs/superpowers/specs/2026-05-06-track-b-baseline-measurements.md`:

```markdown
# Track B — Baseline Measurements

Captured: <date>
Source: tests/smoke/artifacts/<timestamp>/

## Per-mission TGL pool peaks (post-Task 7)

| Mission | Vertex peak | % of 500K | Face peak | % of 200K | Notes |
|---------|------------|-----------|-----------|-----------|-------|
| mc2_01  | <V>        | <%>       | <F>       | <%>       | <gameplay reached?> |
| mc2_03  | <V>        | <%>       | <F>       | <%>       | <gameplay reached?> |
| mc2_10  | <V>        | <%>       | <F>       | <%>       | gameplay |
| mc2_17  | <V>        | <%>       | <F>       | <%>       | gameplay (worst case from recon) |
| mc2_24  | <V>        | <%>       | <F>       | <%>       | wolfman zoom canary |

## Per-mission registered static-prop counts (post-Task 5)

| Mission | enumerated | registered | skipped | Notes |
|---------|-----------|-----------|---------|-------|
| mc2_01  | <E>       | <R>       | <S>     |       |
| ...     |           |           |         |       |

## Pool peak delta (mission-load OFF vs ON)

| Mission | Vertex Δ | Face Δ | Verdict |
|---------|----------|--------|---------|
| mc2_01  | <Δ>      | <Δ>    | <within 10%? > |
| ...     |          |        |        |

## Pool sizing decision

If any tier1 mission peaks above 80% (= 400K vertex / 160K face), bump
the pool init at code/mission.cpp:3141-3154. Current sizes are 500K
vertex/color/shadow, 200K face/triangle. Apply ratio multiplier so all
five pools scale together. Document the bump in this doc.

If all peaks remain under 80%, no bump needed. Document the decision.
```

- [ ] **Step 7.5 — If any mission >80%, bump pools**

In `code/mission.cpp:3141-3154`, scale all five pools by the appropriate factor (e.g., to 750K/300K). Add comment block linking to this baseline doc and explaining the bump.

- [ ] **Step 7.6 — Commit**

```bash
git add docs/superpowers/specs/2026-05-06-track-b-baseline-measurements.md
[ -n "$BUMPED_POOLS" ] && git add code/mission.cpp
git commit -m "docs(track-b): capture pool peak + registered-count baseline

Per-mission TGL pool peaks and total registered static-prop counts
across tier1, with mission-load registration both off and on. Verifies
the 'static replay does not allocate per-frame TGL' invariant per Track
B's design constraint (recon §6).

[Pool init bumped to N from 500K because mc2_XX hit Y%, OR no bump
needed because all missions stayed under 80%.]

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 8 — Parity verification + acceptance envelope

**Files:**
- Create: `docs/superpowers/specs/2026-05-06-track-b-acceptance-envelope.md`

- [ ] **Step 8.1 — `submit_legacy=0` confirmation**

The Track B widening is complete only if the `Legacy` submit population (`GpuStaticPropPopulation::Legacy` per `gos_static_prop_batcher.h:79-84`) reads zero across all tier1 missions in modern mode. Non-zero indicates an actor leaked through to the legacy path that wasn't caught by mission-load OR late-spawn registration.

```bash
MC2_STATIC_PROP_MISSION_LOAD_REG=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "OBJBATCHER v1.*Legacy\|submit_legacy" tests/smoke/artifacts/<latest>/${m}*.log \
    > /tmp/track-b-legacy-${m}.txt
done
```

Expected: every mission's `submit_legacy=0`. Any non-zero = hard failure; identify the leaking actor class and add register-on-spawn for it.

- [ ] **Step 8.2 — DESTROY count + identity parity**

Mirror Track A1 Task 7 pattern. Capture two runs (mission-load OFF and ON):

```bash
# OFF
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log | wc -l \
    > /tmp/track-b-destroy-off-${m}.count
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/track-b-destroy-off-${m}.norm
done

# ON
MC2_STATIC_PROP_MISSION_LOAD_REG=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log | wc -l \
    > /tmp/track-b-destroy-on-${m}.count
  grep "\[DESTROY v1\]" tests/smoke/artifacts/<latest>/${m}*.log \
    | sed -E 's/obj=0x[0-9a-fA-F]+/obj=PTR/; s/frame=[0-9]+/frame=N/' \
    > /tmp/track-b-destroy-on-${m}.norm
done

# Compare
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/track-b-destroy-off-${m}.count /tmp/track-b-destroy-on-${m}.count \
    && echo "${m}: count match" \
    || echo "${m}: COUNT MISMATCH"
done
for m in mc2_01 mc2_03 mc2_10 mc2_17 mc2_24; do
  diff /tmp/track-b-destroy-off-${m}.norm /tmp/track-b-destroy-on-${m}.norm \
    > /tmp/track-b-destroy-${m}.identity-diff
  if [ -s /tmp/track-b-destroy-${m}.identity-diff ]; then
    echo "${m}: IDENTITY DIFF (count match but different victims) — review"
  else
    echo "${m}: identity match"
  fi
done
```

Expected: count match AND identity match across all five missions. Identity diff with matching counts = same destruction count, different victims; review the diff before accepting.

- [ ] **Step 8.3 — Author acceptance envelope**

`docs/superpowers/specs/2026-05-06-track-b-acceptance-envelope.md`:

```markdown
# Track B — Acceptance Envelope

Captured: <date>
Source: tests/smoke/artifacts/<timestamp>/

## Hard gates

| Gate | Mission-load OFF | Mission-load ON | Status |
|------|------------------|------------------|--------|
| submit_legacy=0 across tier1 | n/a (legacy mode) | <0/missing-class> | <pass/fail> |
| [DESTROY v1] count match | <baseline> | <modern> | <pass/fail> |
| [DESTROY v1] identity match | <baseline> | <modern> | <pass/fail> |
| [STATIC_FIRST_FRAME v1] = 0 | n/a | 0 (required) | <pass/fail> |
| Pool peak Δ within 10% | <baseline> | <modern> | <pass/fail> |
| Visual parity at all zoom (incl wolfman mc2_24) | n/a | <screenshots> | <pass/fail> |
| Tier1 5/5 PASS in both modes | <pass> | <pass> | <pass/fail> |

## Visual smoke matrix

For mission-load ON, visual smoke covers:
- mc2_01 zoom-in/zoom-out cycle
- mc2_17 mid-mission artillery (canonical late-spawn test)
- mc2_24 wolfman zoom (worst case for offscreen-actor cull-skip)

Screenshots captured at:
- tier1 first-frame (per mission)
- mid-mission cinematic
- wolfman max-zoom-out (mc2_24 specifically)

Compare against legacy-mode baseline screenshots; any building-disappear,
shadow-flicker, or color-pop indicates the registry's flush invariant
failed for the wider population. Investigate before flipping default-on.

## Out-of-envelope conditions (HARD FAILURE)

- Any non-zero [STATIC_FIRST_FRAME v1] count → structural fix didn't satisfy flush invariant
- Any non-zero submit_legacy → register-on-spawn missed an actor class
- DESTROY count delta > 0 → cull cascade triggered by widening (per cull_gates_are_load_bearing.md)
- DESTROY identity diff with matching counts and victims outside the predicted classes
- Pool peak regression > 10% → static replay leaking into per-frame TGL allocation
```

- [ ] **Step 8.4 — Visual smoke pass**

Run mc2_01 + mc2_17 + mc2_24 each with mission-load ON, capture screenshots. Compare against legacy baseline. Wolfman zoom on mc2_24 is the canonical worst-case (per recon §10 risk row #5: "If wolfman zoom skips update for a registered actor whose render still wants to fire, the actor silently drops out").

- [ ] **Step 8.5 — Commit envelope**

```bash
git add docs/superpowers/specs/2026-05-06-track-b-acceptance-envelope.md
git commit -m "docs(track-b): author acceptance envelope (parity + visual)

Per Q4-Q6 of brainstorm-decisions: dual-mode capture confirms
submit_legacy=0, DESTROY count+identity match, pool peaks within 10%,
[STATIC_FIRST_FRAME v1] reads zero, visual parity at all zoom levels
including wolfman on mc2_24.

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

---

## Task 9 — Soak observation (≥1 week)

Track B's soak is **longer than A1's** (mirrors A1 pattern with 2x duration) because the substrate change is bigger — every static prop in the mission is now driven by the registry, not just the cull-approved subset. ≥1 full week of tier1 default-on before retiring the env opt-out.

- [ ] **Step 9.1 — Pre-flip default**

In `gos_static_prop_registry.cpp`:

```cpp
static const bool s_missionLoadRegEnabled =
    parseEnvBoolWithDefault("MC2_STATIC_PROP_MISSION_LOAD_REG", true);   // was false
```

- [ ] **Step 9.2 — Tier1 confirmation**

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```

Expected: 5/5 PASS, `[INSTR v1] static_prop_mission_load_reg=on` in headers.

- [ ] **Step 9.3 — Verify opt-out still works**

```bash
MC2_STATIC_PROP_MISSION_LOAD_REG=0 \
  py -3 scripts/run_smoke.py --tier tier1 --mission mc2_01 --duration 10 --kill-existing
```

Expected: passes, `[INSTR v1] static_prop_mission_load_reg=off`. The first-render registration path takes over.

- [ ] **Step 9.4 — Commit flip**

```bash
git add GameOS/gameos/gos_static_prop_registry.cpp
git commit -m "feat(track-b): flip mission-load static-prop registration default to ON

After 1+ week of clean soak with MC2_STATIC_PROP_MISSION_LOAD_REG=1
across tier1 stock missions: [STATIC_FIRST_FRAME v1]=0,
submit_legacy=0, DESTROY count+identity parity, pool peaks within 10%,
visual parity verified at all zoom levels including wolfman.

MC2_STATIC_PROP_MISSION_LOAD_REG=0 retained as opt-out for one more
soak cycle.

Plan: docs/superpowers/plans/2026-05-06-track-b-widen-registry.md
"
```

- [ ] **Step 9.5 — Daily smoke for ≥7 days**

Run `py -3 scripts/run_smoke.py --tier tier1 --kill-existing` once per day. Log results. Watch for:
- Non-deterministic regressions (once-per-N-runs DESTROY delta) — indicates a race the widening exposed
- `[STATIC_FIRST_FRAME v1]` spikes — indicates a flush-invariant edge case the structural fix missed
- Pool peak drift — indicates static replay started leaking into per-frame allocation

Document each day's results in the acceptance envelope doc.

- [ ] **Step 9.6 — Decision: retire first-render fallback**

After ≥7 days of clean soak, retire the first-render registration block at `bdactor.cpp:1662-1672` (Bldg) and `:4285-4292` (Tree). It served as defensive fallback during soak but is now redundant: every static prop is registered at mission-load OR late-spawn. Removing it is a follow-up commit (or a separate slice — at the operator's discretion).

If any soak day shows red, hold the retirement and investigate.

---

## Task 10 — Memory + index

**Files:**
- Create: `~/.claude/projects/A--Games-mc2-opengl-src/memory/track_b_widen_static_prop_registry.md`
- Modify: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

- [ ] **Step 10.1 — Memory file**

```markdown
---
name: track_b_widen_static_prop_registry
description: Track B shipped — static-prop registry now covers EVERY world static prop in the mission (mission-load + register-on-spawn); first-frame race fixed structurally
type: project
---

Track B of the MC3 rendering modernization arc shipped on <date>. Promotes
GpuStaticPropRegistry from "fast-path replay of cull-approved instances" to
"single source of truth for ALL world-static-prop instances in the mission."

**Mission-load tier:** ObjectManager::registerStaticPropsForMissionLoad walks
terrainObjects/buildings/turrets/gates immediately after loadTerrainObjects
and before finalizeGeometry (code/mission.cpp:2809..3044). Each
BldgAppearance/TreeAppearance::registerStatic calls
GpuStaticPropRegistry::registerStaticProp which uses
GpuStaticPropBatcher::buildRecipeFromShape to construct recipes without
requiring per-actor TransformMultiShape to have run yet.

**Late-spawn tier:** Artillery (artlry.cpp:1577,1645,1713), vTol
(missiongui.cpp:4294,4336,5750,5756), and warrior.cpp:7576 call
appearance->registerStatic() right after construction. HUD allowlist sites
(skybox at gamecam.cpp:510, compass at :654, simplecamera at :495) are
explicitly NOT registered (Q5 — HUD is the existing 06ac847 allowlist path).

**First-frame race:** structural fix in registerRecipe — pre-populates
multi->cachedFrame_ to g_mc2FrameCounter so the existing flush()
staleness gate accepts the entry without requiring a prior
CacheGpuLightData() call. [STATIC_FIRST_FRAME v1] counter reads zero
across tier1; non-zero would indicate the flush invariant doesn't
accept the pre-populated stamp.

**Static-field bake at registration:** modelMatrix, typeID, flags,
aRGBHighlight, fogRGB are baked once at registration via
buildRecipeFromShape. firstColorOffset patched per-frame at flush() (per-
frame because the color SSBO ring rotates). lightDataIndex patched per-
frame from multi->getCachedGpuLightIndex() — the existing slice 3.C
machinery, unchanged.

**Env flag (still present as opt-out):** MC2_STATIC_PROP_MISSION_LOAD_REG=0
restores the first-render-only registration path. Default = on.

**Acceptance envelope:** docs/superpowers/specs/2026-05-06-track-b-acceptance-envelope.md.

**Files touched:**
- GameOS/gameos/gos_static_prop_registry.{h,cpp} — registerStaticProp +
  cachedFrame_ pre-population + [STATIC_FIRST_FRAME v1] counter
- GameOS/gameos/gos_static_prop_batcher.{h,cpp} — buildRecipeFromShape
- mclib/appear.h — registerStatic / isStaticRegistered virtuals
- mclib/bdactor.{h,cpp} — Bldg + Tree overrides
- mclib/msl.h — setCachedFrame setter
- code/objmgr.{h,cpp} — registerStaticPropsForMissionLoad helper
- code/mission.cpp — walk-call between loadTerrainObjects and finalizeGeometry
- code/artlry.cpp / missiongui.cpp / warrior.cpp — late-spawn wiring

**Roadmap:** docs/superpowers/mc3-rendering-modernization-roadmap.md (Track B section).
```

- [ ] **Step 10.2 — Index in MEMORY.md**

Add to "Rendering / shaders" section:

```
- ⭐ [Track B shipped — registry covers every world static prop (<date>)](track_b_widen_static_prop_registry.md) — mission-load + register-on-spawn; cachedFrame_ pre-populated at register; default-on after 7-day soak
```

- [ ] **Step 10.3 — Save / commit**

(Memory may be unversioned per project convention; save-only if so.)

---

## Self-Review (run before declaring complete)

**Spec coverage:**
- Q4 (mission-load + register-on-spawn) → Tasks 5, 6
- Q5 (Generic descope, HUD allowlist preserved) → Task 6.4 (explicit comments at HUD sites)
- Q6 (structural cachedFrame_ pre-population + counter as proof) → Tasks 3, 4
- Recon §6 pool sizing audit → Task 7
- Recon §10 risk row 4 (firstColorOffset staleness) → resolved by NOT baking it (per-frame patch at flush)
- Recon §10 risk row 5 (cachedFrame_ on wider population) → Task 8.4 wolfman canary on mc2_24

**Placeholder scan:** Tasks 5.5, 7.4, 8.3, 10.1 contain `<date>`/`<timestamp>`/`<N>`/`<%>` placeholders that fill at execution time from captured artifacts. The recipe code blocks have no placeholders.

**Type consistency:** `BldgAppearance::StaticRegistration` field order verified at write-time against `bdactor.h:209-214`: `bool registered; TG_MultiShapePtr shape; int32_t recipeIndex;`. Tree's parallel struct at `:503-508` claimed identical by recon §9 — Step 5.3 has a TODO to re-verify before relying on the cast-layout pattern. If they differ, fall back to typed setters.

**Dependency on Task 1 spike outcome:** Task 2's body assumes Candidate A (clean factoring) succeeded. If spike picked Candidate B, Task 2 swaps wholesale (Step 2.5 documents the substitute). If spike picked "interstitial slice," Tasks 5, 7, 8 defer to a follow-up plan.

**Open question reminder:** Task 7 may or may not bump pools depending on captured peaks. The decision and any bump live in the same commit per Step 7.6.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-06-track-b-widen-registry.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks, fast iteration. Especially appropriate here because Task 1's spike is high-risk; isolating it in its own subagent prevents the spike's exploratory grepping from polluting later tasks' contexts.

2. **Inline Execution** — execute tasks in this session using `executing-plans`, batch with checkpoints for review.

Before execution: dispatch `adversarial-plan-review` per worktree CLAUDE.md "Review Discipline" — Track B widens load-bearing cull-adjacent infrastructure and qualifies as architectural-endpoint scope.

Which approach?
