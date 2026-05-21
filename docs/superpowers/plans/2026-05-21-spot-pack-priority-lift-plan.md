# (E') SpotLight_ Priority Lift — Implementation Plan

- **Status:** DRAFT — ready for adversarial review
- **Date:** 2026-05-21
- **Worktree:** `claude/nifty-mendeleev`
- **Source spec:** [2026-05-21-spot-pack-scored-selection-design.md](../specs/2026-05-21-spot-pack-scored-selection-design.md) (v4.1, READY-TO-PLAN per round-4 adversarial)
- **Slice ID:** (E') — extends (E) SpotLight_ real illumination
- **Goal:** Replace `Camera::updateLights`'s `activeLights[]` registration-order assembly with priority-lifted assembly: SpotLight_-tagged POINT/SPOT lights win FIFO truncation at `GatherLightsParameters`. ~55 LOC, one function modified, no shader/SSBO/accessor changes.

## Substitutive completion criteria

1. `Camera::updateLights` reorder logic active in production (no env gate).
2. `[SPOT_PRIORITY_LIFT v1] event=first_hit` line fires on mc2_10 with priority≥4.
3. mc2_10 intro visual canary: mech/GV bodies show yellow contribution from their SpotLight_ children (where current state shows nothing).
4. mc2_05 (night) shows building lamps illuminating ground around them (unchanged from current — building slots already make 16-cap).
5. mc2_24 Vedette/LRMC integrity preserved (the f77f135 revert canary class).
6. LIGHTBRIDGE template-cache hit rate unchanged (verify via existing `[LIGHTBRIDGE v1]` env-gated counters).
7. No regression in `addLightDataStructure` dedup growth (verify via existing `[LIGHT_DEDUP v1]` env-gated counters).

## Risk register (R1-R8)

- **R1.** `is_e_slot()` map drainage. Anubis leaks per spec §3.5; not introduced by this slice. Document but don't fix here.
- **R2.** Frame-ordering: `Camera::updateLights` may be called twice per frame (once from eye-update, once from `removeWorldLight` during actor destroy). Probe statics tick twice. Acceptable diagnostic accuracy loss; not correctness.
- **R3.** `s_deferredNonPriority[]` static re-entrancy. Engine single-threaded; `updateLights` doesn't recurse into itself. Safe.
- **R4.** Visual canary on multiple missions before flipping default-on. mc2_05 (night, building canary), mc2_10 (mech+GV canary), mc2_24 (Vedette/LRMC paranoia).
- **R5.** `numActiveLights` count unchanged by reorder — no per-shape iteration cost increase in `tgl.cpp:1968` CPU-lit path.
- **R6.** No new GL state, no new SSBO layout — Vulkan-prep compliant by absence.
- **R7.** `--clean-first` not required — modifying one function body, no header/class layout change.
- **R8.** Stage gating user-driven per worktree CLAUDE.md "Smoke sessions are USER-DRIVEN."

## Stage shape

Same atomic-commit-per-task pattern as (E) plan. Three stages: instrument → gate-on → cleanup-and-promote.

---

## Stage 0 — Instrument (no behavior change)

### T0.1 — Add `[SPOT_PRIORITY_LIFT v1]` probe block (gated default-off)

**Files:** `mclib/camera.cpp` (`Camera::updateLights`)

**Changes:**
- Add file-scope static `s_liftTrace` reading `MC2_SPOT_LIFT_TRACE`.
- Add file-scope statics `s_liftFirstHit`, `s_liftFrames`, `s_liftWindowPriority`, `s_liftWindowDeferred`.
- Add local `s_liftPriorityCount` (function-scope, per-call reset).
- NO reorder logic yet — only the probe scaffolding. Probe counts will be 0 in this commit (priority branch doesn't exist).

**Verification:** smoke mc2_10 with `MC2_SPOT_LIFT_TRACE=1` shows the periodic summary line firing every 600 frames with `avg_priority=0.00 avg_deferred=0.00`. Confirms probe is wired without changing behavior.

**Commit:** `feat(spotlight-lift T0.1): wire [SPOT_PRIORITY_LIFT v1] probe scaffolding`

---

## Stage 1 — Priority lift implementation (default-on, no gate)

Per spec §3.4: tagging mechanism (`is_e_slot`) is already populated unconditionally. No env gate needed for the lift itself — it's the new correct behavior.

### T1.1 — Implement two-pass append in `Camera::updateLights`

**Files:** `mclib/camera.cpp` (`Camera::updateLights` at [:1887-1983](mclib/camera.cpp))

**Changes:**
- Add file-scope `static TG_LightPtr s_deferredNonPriority[MAX_LIGHTS_IN_WORLD]` per spec §3.2.
- In the existing `for (long i = 0; i < MAX_LIGHTS_IN_WORLD; ++i)` loop:
  - Keep `if (!worldLights[i]) continue` (existing).
  - Keep TG_LIGHT_TERRAIN branch (existing at [:1902-1915](mclib/camera.cpp)).
  - Keep AMBIENT/INFINITE branch (existing at [:1917-1925](mclib/camera.cpp)). AMBIENT/INFINITE still go into `activeLights[]` first.
  - **Modify** the POINT/SPOT branch (existing at [:1927-1968](mclib/camera.cpp)):
    - Keep the existing `light->active = projectForLightingShadow(...)` assignment.
    - Keep the existing `terrainLights[numTerrainLights++] = light` append (terrain pipeline unaffected).
    - Replace the `activeLights[numActiveLights++] = light` unconditional append with the priority split:
      - `if (mc2_spotlight_diag::is_e_slot(i, nullptr))` → append immediately + `++s_liftPriorityCount`
      - `else` → append to `s_deferredNonPriority[numDeferred++]`
- After the main loop, add the deferred-flush:
  ```cpp
  for (long j = 0; j < numDeferred && numActiveLights < MAX_LIGHTS_IN_WORLD; ++j) {
      activeLights[numActiveLights++] = s_deferredNonPriority[j];
  }
  ```
- After the deferred-flush, wire the probe emit (first-hit always-on, summary env-gated per T0.1 scaffolding).

**Include addition:** `#include "spotlight_diag.h"` at top of `camera.cpp` if not already present.

**Verification:** smoke mc2_10 with NO env vars:
- `[SPOT_PRIORITY_LIFT v1] event=first_hit` fires once with priority≥4.
- With `MC2_SPOT_LIFT_TRACE=1`: per-summary lines show `avg_priority` ~4-7 (matches T1.16 baseline).
- mc2_10 visual canary: mech/GV bodies show yellow contribution from their SpotLight_ children.

**Commit:** `feat(spotlight-lift T1.1): priority-lift activeLights assembly in Camera::updateLights`

### T1.2 — Visual canary across 3 missions

**Files:** none (validation gate)

**Action:** USER-DRIVEN smoke per worktree CLAUDE.md "Smoke sessions are USER-DRIVEN":
1. `set MC2_SPOT_LIFT_TRACE=1 && py -3 scripts\run_smoke.py --mission mc2_10 --duration 20 --keep-logs` → expect priority>=4 first-hit; mech/GV illumination visible
2. `set MC2_SPOT_LIFT_TRACE=1 && py -3 scripts\run_smoke.py --mission mc2_05 --duration 20 --keep-logs` → expect priority>=2 (mc2_05 has building lamps); ground illumination near lamps visible
3. `set MC2_SPOT_LIFT_TRACE=1 && py -3 scripts\run_smoke.py --mission mc2_24 --duration 20 --keep-logs` → Vedette/LRMC integrity check

NO `--kill-existing` (user kill-aware). Single mission per command (user discipline).

If any mission fails (crash, GL_DEBUG_HIGH abort, Vedette invisibility, priority count 0 on missions where SpotLight_ exist), STOP and surface.

**Commit:** none (validation gate)

---

## Stage 2 — Cleanup and OQ6 reset wiring

### T2.1 — Wire `mc2_spotlight_diag::reset()` + probe-statics reset to mission boundary (OQ6)

**Files:** `mclib/spotlight_diag.cpp`, `mclib/camera.cpp` (probe-statics reset)

**Changes:**
- Add a `mc2_spotlight_diag::reset_probe_statics()` exposing the camera.cpp file-scope statics through a setter (cleanest implementation: a single `void resetSpotlightLiftProbe()` in camera.cpp's namespace, exported via a tiny header).
- Find mission-boundary teardown site. Per [memory/mission_load_inits_mirror_init_per_subsystem.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\mission_load_inits_mirror_init_per_subsystem.md), the canonical mission teardown is `Mission::destroy` or similar. Grep at plan-execute time to find exact line.
- At mission teardown: call `mc2_spotlight_diag::reset()` + `resetSpotlightLiftProbe()`.

**Verification:** smoke two-mission sequence (load mc2_10, then mc2_05) with `MC2_SPOT_LIFT_TRACE=1`. Expect `event=first_hit` to fire ONCE per mission (not just on first mission of session).

**Commit:** `feat(spotlight-lift T2.1): wire reset() to mission boundary — OQ6 fix`

### T2.2 — Final substitutive verification

**Files:** none

**Action:**
1. Tier1 smoke (no env vars): mc2_05 + mc2_10 + mc2_24 all PASS.
2. RenderDoc frame capture on mc2_10 intro: mech `lightDataIndex` slot's `TG_HWLightsData` contains the SpotLight_ POINT entry within first 16 entries.
3. With `MC2_LIGHTBRIDGE_TRACE=1` (if such env exists per existing infrastructure) or equivalent: confirm template-cache hit rate unchanged from pre-(E') baseline.

**Commit:** none (validation gate)

---

## Out-of-scope items (filed)

- **Anubis pointLight leak fix** — per spec §3.5 Option X. Independent slice; required before any future Option Y promotion. Filed.
- **Scored selection** — when content scale exceeds 16 simultaneously-active SpotLight_ POINTs. Future (F)-class.
- **Clustered/forward+ lighting** — future architectural rework. (F)-class+.

## Total implementation scope

- **Files modified:** `mclib/camera.cpp` (one function modified + probe block).
- **Optional cleanup file**: `mclib/spotlight_diag.cpp` (add `reset_probe_statics` shim or similar; T2.1).
- **Total LOC:** ~55 in `camera.cpp` (T0.1+T1.1) + ~10 in spotlight_diag/mission-teardown (T2.1) = ~65 LOC.
- **No shader changes. No SSBO layout changes. No new TU. No accessor signature changes. No header layout changes (no `--clean-first` required).**

## Cross-references

- Spec: [2026-05-21-spot-pack-scored-selection-design.md](../specs/2026-05-21-spot-pack-scored-selection-design.md) (v4.1)
- T1.16 diagnostic: `tests/smoke/artifacts/2026-05-21T06-48-58/mc2_10.log`
- (E) plan: [2026-05-20-spotlight-real-illumination-plan.md](2026-05-20-spotlight-real-illumination-plan.md)
- (F) lighting MODEL rework: [memory/terrain_lighting_compute_kernel_saturation.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\terrain_lighting_compute_kernel_saturation.md) — separate orthogonal slice
- Worktree CLAUDE.md "Smoke sessions are USER-DRIVEN" — verification discipline
- [memory/feedback_class_layout_change_needs_clean_first.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_class_layout_change_needs_clean_first.md) — not triggered (no class layout change)
