# (E) SpotLight_ → Real Illumination — Implementation Plan

- **Status:** DRAFT — ready to execute
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Spec:** [docs/superpowers/specs/2026-05-20-spotlight-real-illumination-design.md](../specs/2026-05-20-spotlight-real-illumination-design.md)
- **Goal:** Replace opaque cone billboard rendering for `SpotLight_*`-prefixed child shapes with real `TG_Light` registrations through the existing `addWorldLight` pipeline. Ship to all four affected populations (building static props, mech actors, both via the same retire-geometry + register-light pattern). Default-on after soak.
- **Substitutive test:** F3 RenderDoc capture shows zero cone draws for `SpotLight_*` children; `worldLights[2..N]` populated as expected; visible illumination spillage on terrain/props/mechs around lamp positions in mc2_04 (night) canary; Vedette/LRMC visual regression check on mc2_24 (the f77f135 revert canary class).

## Risk register before execution

- **R1.** Cross-mission lifecycle leak. Existing anubis `pointLight` is leaked at mech destruction in `mech3d.cpp` (advisor flagged). Don't inherit when generalising; pair every `addWorldLight` with a `removeWorldLight` in the destroy hook.
- **R2.** `active=false` discipline. Off-screen mechs / culled buildings must set `active=false` or they consume per-shape best-16 slots from visible shapes. Mirror [mech3d.cpp:3382](mclib/mech3d.cpp).
- **R3.** addWorldLight is O(N) first-empty-slot scan. After 256→1024 bump, never call per-frame. One-shot at registration only.
- **R4.** Per-instance not per-type. Building instances have different world transforms; register at instance-spawn, not at TG_TypeShape construction.
- **R5.** Day/night gating. CPU path skips spotlight cones during day at [tgl.cpp:1728](mclib/tgl.cpp). Real lights should follow the same convention — use `active = isNight && inView` to match.
- **R6.** Vedette/LRMC canary. The f77f135 revert was triggered by a state-leak that made Vedettes invisible. (E) does NOT add a new draw path, so the same class doesn't apply, but Stage 1 smoke MUST include mc2_24 visual confirmation as a paranoia check.

## Per-task atomic commit rule

Every T-numbered task lands as one commit. Commit messages reference task number (e.g. `feat(spotlight-real T1.3): ...`). If a task surfaces an unexpected blocker, STOP, file the deviation, surface to user before continuing — do NOT bundle the workaround into the same commit.

---

## Stage 0 — Instrument (no behavior change)

### T0.1 — `MC2_SPOTLIGHT_REAL_TRACE` registration counter at static-prop batcher

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp`

**Changes:**
- Add env-gated bool `s_spotlightRealTrace` reading `MC2_SPOTLIGHT_REAL_TRACE`.
- At `submitMultiShape`, when `child->isSpotlight==true`, increment monotonic + window counters. First-hit always-on (one-line stderr) so any operator sees confirmation without env.
- 600-flush summary print, env-gated, matches `[INSTR v1]` schema family.
- Tag: `[SPOTLIGHT_REAL_TRACE v1]`.

**Pattern to mirror:** the now-deleted `[SPOTLIGHT_TRACE v1]` block from f77f135 (which was reverted with d91e639 but is documented in `memory/spotlight_billboards_static_prop_opaque_bug.md`). Same shape, new tag.

**Verification:** smoke run with `MC2_SPOTLIGHT_REAL_TRACE=1` on mc2_04 + mc2_10 shows the counter incrementing. First-hit stderr line confirms first SpotLight_ child name.

### T0.2 — Same counter at mech batcher skip site

**Files:** `GameOS/gameos/gos_mech_batcher.cpp` around line 553-563 (the existing skip_spotlight branch).

**Changes:**
- At the skip site, add counter increment + first-hit print. Tag: `[SPOTLIGHT_REAL_TRACE v1]` `event=mech_skip`.

**Verification:** mc2_24 smoke run shows mech spotlight skip counter > 0 (mech batcher is being exercised).

### T0.3 — Baseline measurement run

**Files:** none (data-gathering).

**Action:** Run `MC2_SPOTLIGHT_REAL_TRACE=1` smoke on mc2_04 (night, the visual proof case), mc2_10 (tier1 stress), mc2_24 (mech-heavy / Vedette/LRMC).

**Capture:** mission-load population count, per-frame submitMultiShape counts, mech-skip counts. Confirm user's "extremely low" expectation. If counts come back surprisingly high (>500 per mission), surface before Stage 1.

**Commit:** none — just artifact capture.

---

## Stage 1 — Gated implementation (`MC2_SPOTLIGHT_REAL=1`, default-off)

### T1.1 — Bump CPU world pool 256 → 1024

**Files:** `mclib/tgl.h`, `mclib/camera.cpp`

**Changes:**
- `mclib/tgl.h:170` — `#define MAX_LIGHTS_IN_WORLD 1024`.
- Audit allocations at `mclib/camera.cpp:411` (worldLights), `:423` (activeLights), `:430` (terrainLights). All three multiply `sizeof(TG_LightPtr) * MAX_LIGHTS_IN_WORLD`; the macro substitution carries through automatically. 1024 × 8 bytes × 3 arrays = 24KB total. Verify the `memset` calls at `:412`, `:424`, `:431` use the macro consistently (no hard-coded `256`).
- Grep `MAX_LIGHTS_IN_WORLD` across `mclib/` and confirm no usage assumes the value is exactly 256. Shader-side `MAX_LIGHTS_IN_WORLD = 16` in [shaders/include/lighting.hglsl:23](shaders/include/lighting.hglsl) is a DIFFERENT scope (per-shape array) and DOES NOT change.

**Verification:** clean build (RelWithDebInfo, `--clean-first` per [memory/feedback_class_layout_change_needs_clean_first.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_class_layout_change_needs_clean_first.md) — header changes affect class layout). Smoke tier1 with no env vars: passes unchanged.

### T1.2 — Env-gate plumbing

**Files:** Wherever process-startup env reads live (likely `code/mechcmd2.cpp` or a startup module).

**Changes:**
- Add `static bool g_spotlightReal = false;` (file-scope) and read `MC2_SPOTLIGHT_REAL` once at startup. Default-off.
- Export via inline getter `bool isSpotlightRealEnabled()` from a shared header. No new symbol bloat.

**Verification:** startup banner `[INSTR v1] enabled: ...` includes `spotlight_real=0` when env unset.

### T1.3 — Static-prop batcher: skip cone for isSpotlight

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp` (submitMultiShape around the existing `flags |= (1u << 2)` site)

**Changes:**
- Wrap the existing `child->isSpotlight` packet emission in `if (isSpotlightRealEnabled() && child->isSpotlight) { /* skip — light is handled at BldgAppearance::init */ continue; }`. Effectively: when gate ON, the cone is NOT submitted.
- Keep the existing `flags |= (1u << 2)` path for the gate-off case (no behavior change when default-off).

**Verification:** With `MC2_SPOTLIGHT_REAL=1`, RenderDoc capture on mc2_04 shows no static-prop packets with the spotlight bit set in the coalesce multidraw. With env unset: behavior unchanged.

### T1.4 — Building-side TG_Light registration at instance spawn

**Files:** `mclib/bdactor.cpp` (BldgAppearance::init at line 612), `mclib/bdactor.h` (add a vector field)

**Changes:**
- In `BldgAppearance` header: add `std::vector<TG_LightPtr> spotlightLights_;` and `std::vector<long> spotlightSlotIds_;` member fields (paired arrays; one entry per registered SpotLight_ child).
- In `BldgAppearance::init` (bdactor.cpp:612): walk MultiShape children. For each `child->isSpotlight`:
  1. `TG_LightPtr light = (TG_LightPtr)systemHeap->Malloc(sizeof(TG_Light));`
  2. `light->init(TG_LIGHT_POINT);` (per OQ2 v1 decision — POINT not SPOT for simplicity)
  3. Compute world-space position from building world transform + child local position (use existing `getNodeIdPosition` pattern from `bdactor.cpp:1041`).
  4. `light->SetPosition(&worldPos);`
  5. `light->SetaRGB(0xffe8c870);` (hardcoded warm — OQ3 v1 default)
  6. `light->SetIntensity(0.5f);` (initial; tune in canary)
  7. `light->SetFalloffDistances(20.0f, 80.0f);` (hardcoded — OQ4 v1 default)
  8. `light->active = false;` (gated by per-frame update in T1.7)
  9. `long slotId = eye->addWorldLight(light);`
  10. If `slotId == -1`: free the malloc, log a one-line warning, continue (pool overflow). With 1024-pool this should never fire on stock content.
  11. Push light + slotId into the paired vectors.
- Guard the entire registration block on `isSpotlightRealEnabled()` so env-off behavior is unchanged.

**Verification:** With env on, mission load of mc2_04 produces N `addWorldLight` calls matching the T0.3 baseline. Confirm via `MC2_SPOTLIGHT_REAL_TRACE` counters.

### T1.5 — Building destroy hook: removeWorldLight

**Files:** `mclib/bdactor.cpp` (`BldgAppearance::destroy` — locate via grep)

**Changes:**
- Walk `spotlightLights_` / `spotlightSlotIds_`. For each: `eye->removeWorldLight(slotId, light); systemHeap->Free(light);`. Clear both vectors.

**Verification:** Smoke run loads mc2_04 twice in sequence (mission reload). Mission 2's `addWorldLight` call count matches mission 1's (no slot leak from previous). Verify via the trace counter and a one-line stderr at `removeWorldLight`.

### T1.6 — Mech-side: generalize anubis pattern from single node to all SpotLight_

**Files:** `mclib/mech3d.cpp` (UpdateGeometry region around line 3333-3383)

**Changes:**
- Refactor the existing anubis block to iterate ALL child shapes with `isSpotlight==true`, not just the hardcoded `"SLCircle_anubis"` node lookup.
- Replace `lightCircleNodeIndex` (single-node cached id) with `std::vector<long> spotlightNodeIds_;` (one entry per spotlight child).
- Replace `pointLight` single pointer with `std::vector<TG_LightPtr> spotlightLights_;` and `std::vector<long> spotlightSlotIds_;`.
- At first-night-visibility (preserve the `eye->isNight` lazy-init gate): for each spotlight child node, malloc + init + addWorldLight + initial SetPosition/SetaRGB/SetIntensity/SetFalloffDistances, same as anubis (TG_LIGHT_SPOT to preserve directionality is acceptable here OR TG_LIGHT_POINT for v1 consistency — pick one and document).
- Guard new code on `isSpotlightRealEnabled()`.

**Risk:** the existing anubis `lightCircleNodeIndex = -1` initial-state check is the lazy-init key. Generalising it means tracking lazy-init per node. Use `spotlightNodeIds_.empty()` as the equivalent.

**Decision needed in this task (resolve before commit):** TG_LIGHT_POINT (simpler — no direction extraction) or TG_LIGHT_SPOT (matches anubis precedent — has spotDir / maxSpotLength). Recommend POINT for v1; SPOT for v2 if directionality is missed.

### T1.7 — Mech per-frame in-place update

**Files:** `mclib/mech3d.cpp` (UpdateGeometry, after the lazy-init block)

**Changes:**
- For each registered spotlight light (after init):
  - Look up the spotlight child node's current world position via `getNodeIdPosition`.
  - `light->SetPosition(&pos);`
  - `light->SetLightToWorld(&lightToWorldMatrix);` (per the anubis pattern at mech3d.cpp:3378)
  - `light->active = (eye->isNight && inView && (sensorLevel > 4) && !InEditor);` — match anubis's gate semantics; add `inView` so culled mechs don't consume best-16 slots (R2).
- NO `removeWorldLight` / `addWorldLight` per frame (R3).

**Verification:** Tracy capture on mc2_24 shows no per-frame pool churn. `addWorldLight` first-hit prints fire only at first-night-visibility per mech.

### T1.8 — Mech destroy hook: removeWorldLight

**Files:** Mech destruction site (grep `MechAppearance::destroy` or equivalent).

**Changes:** mirror T1.5 for the mech path. Walk vectors, removeWorldLight + free, clear.

**Verification:** Smoke run with multiple mech deaths (mc2_24 combat) doesn't leak slots. Confirm via trace counter and stderr at removeWorldLight.

### T1.9 — Stage 1 visual canary

**Files:** none (validation).

**Action:**
1. Smoke `mc2_04 --duration 30 --keep-logs` with `MC2_SPOTLIGHT_REAL=1` — expect: lamps no longer render as opaque triangles; surrounding terrain/props/mechs show illumination spillage.
2. Smoke `mc2_10` — expect: no regression; spotlight counts low.
3. Smoke `mc2_24` (Vedette/LRMC canary) — expect: Vedettes and LRMCs render correctly (the f77f135 revert canary). If any actor goes invisible, STOP and surface.
4. RenderDoc capture on one mc2_04 frame: confirm no cone draws from static_prop batcher with the spotlight bit; confirm `worldLights[2..N]` populated.

**Commit:** none (validation gate); if all four steps pass, proceed to Stage 2.

---

## Stage 2 — Default-on flip + soak

### T2.1 — Flip the env-gate default

**Files:** wherever T1.2 reads `MC2_SPOTLIGHT_REAL`.

**Changes:** invert the default. New behavior: `MC2_SPOTLIGHT_REAL=0` is the opt-out; default behaves as if `=1`. Update startup banner.

**Verification:** smoke tier1 with no env vars passes (matches T1.9 with env on). Smoke with `MC2_SPOTLIGHT_REAL=0` passes (matches pre-T1.1 behavior).

### T2.2 — Soak

**Action:** User-driven canary across all tier1 + mc2_04 + mc2_24. Multi-day soak in master session. Watch for: visual regressions during mission transitions, save/load edge cases, day/night transition behavior, Vedette/LRMC integrity.

**Commit:** none (validation gate).

---

## Stage 3 — Delete the gated code (substitutive completion)

Only after Stage 2 soak passes.

### T3.1 — Remove env-gate

**Files:** T1.2 + T1.3 + T1.4 + T1.5 + T1.6 + T1.7 + T1.8 callsites.

**Changes:**
- Delete the env var read.
- Delete `isSpotlightRealEnabled()` guards. The new code paths become unconditional.
- Keep the trace counter (gated on `MC2_SPOTLIGHT_REAL_TRACE`) for future diagnostics per the Debug Instrumentation Rule (demote-not-delete).

### T3.2 — Delete the dead static-prop spotlight bit

**Files:** `GameOS/gameos/gos_static_prop_batcher.cpp` (submitMultiShape), `shaders/static_prop.vert`

**Changes:**
- Remove the `flags |= (1u << 2)` branch for spotlight children entirely (cone is never drawn now).
- In `static_prop.vert`, remove the bit-2 read path for kFlagIsSpotlight if it's no longer referenced by any live draw.

### T3.3 — Mech batcher skip cleanup

**Files:** `GameOS/gameos/gos_mech_batcher.cpp:553-563`

**Changes:**
- Update the comment block to reflect the new architecture (spotlights are now handled as TG_Lights via MechAppearance, not skipped here). Keep the geometry skip (spotlight children should never emit cone geometry through any path).

### T3.4 — Final smoke + Tracy capture

**Action:**
- mc2_04 + mc2_10 + mc2_24 smoke after deletion.
- Tracy capture: no `cone_emit` / spotlight packet drawn anywhere.
- RenderDoc: `worldLights` populated; static_prop draws contain no spotlight packets; mech draws contain no skipped spotlight children.

**Commit:** none — Stage 3's value is the deletion commits T3.1-T3.3, not the validation run.

---

## Out-of-scope items (file as follow-ups, not blockers)

- **OQ2 / TG_LIGHT_SPOT directional cones:** v2 work if v1 POINT illumination feels too omnidirectional.
- **OQ3 / per-light color from texture sampling:** v2 if all-warm-yellow lamps don't fit mission moods.
- **OQ4 / falloff distances from cone shape bbox:** v2 if hardcoded distances look wrong on specific shapes.
- **OQ7 / day-night transition mid-mission:** if MC2 supports time-of-day transitions, the `active` flag re-eval is per-frame so it should already work; not a separate task.
- **LIGHTBAKE v2 baked path:** only if dynamic pool overflows OR per-shape best-16 truncates visibly. With 1024 pool and "extremely low" actual population, neither is expected.
- **Vedette/LRMC anubis-leak audit:** the existing anubis path has a documented `removeWorldLight` gap (advisor flagged). (E) doesn't make it worse but doesn't fix it. File as standalone follow-up.

---

## Substitutive completion criteria (the spec's §5 step 4)

`(E)` is "done" when ALL of these hold:

1. Build under default flags (no `MC2_SPOTLIGHT_REAL` env set) shows the new behavior.
2. RenderDoc capture on mc2_04: zero static-prop draws contain spotlight-tagged packets.
3. RenderDoc capture on mc2_24: zero mech-batcher skip-spotlight events; mechs with spotlight nodes show illumination spillage on surrounding terrain/buildings.
4. `worldLights[2..N]` populated; `addWorldLight` first-hit count matches T0.3 baseline (within combat variance).
5. Visual canary: mc2_04 night-time lamp posts illuminate the ground around them (NOT just the building they sit on).
6. mc2_24 Vedette/LRMC integrity unchanged from baseline.
7. Mission reload (load mc2_04 twice in sequence): no slot leak in worldLights.
8. T3.1-T3.3 deletion commits have landed; env gate is gone from code.

## Cross-references

- Spec: [docs/superpowers/specs/2026-05-20-spotlight-real-illumination-design.md](../specs/2026-05-20-spotlight-real-illumination-design.md)
- Pattern source: [mclib/mech3d.cpp:3333-3383](../../mclib/mech3d.cpp) (anubis searchlight)
- Memory: [feedback_static_prop_subpass_program_switch.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_static_prop_subpass_program_switch.md) — why we don't repeat f77f135
- Memory: [spotlight_billboards_static_prop_opaque_bug.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\spotlight_billboards_static_prop_opaque_bug.md) — the original bug analysis
- Memory: [mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md) — mission init/teardown discipline
- Memory: [cpp_glsl_ubo_struct_lockstep.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cpp_glsl_ubo_struct_lockstep.md) — for any future shader-array bump
- Memory: [feedback_class_layout_change_needs_clean_first.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_class_layout_change_needs_clean_first.md) — T1.1 build discipline
