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

Same atomic-commit-per-task pattern as (E) plan. Three stages: **instrument → ship default-on → cleanup**. No env gate — the priority lift is correct behavior and ships unconditionally (round-1 m1 fix).

**Stage independence (round-3 MINOR-1 note):** T1.1 ships the priority-lift functionality and is independently shippable for correctness. T2.1's reset wiring is a probe-accuracy improvement for multi-mission sessions — required for clean diagnostics across mission transitions, but NOT required for the priority-lift itself to work correctly. If T1.1 ships without T2.1, the lift still functions in production; only the multi-mission `[SPOT_PRIORITY_LIFT v1] event=first_hit` diagnostic degrades to "fires once per exe run, not once per mission."

---

## Stage 0 — Instrument (no behavior change)

### T0.1 — Add `[SPOT_PRIORITY_LIFT v1]` probe block

**Files:** `mclib/camera.cpp` (`Camera::updateLights` at [:1887](mclib/camera.cpp))

**Changes (per round-1 C3 fix — explicit declarations to ensure T0.1 compiles + baseline is observable):**
- At file scope (top of `camera.cpp` near the existing T1.15/T1.16 statics at [:1873-1885](mclib/camera.cpp)), add:
  - `static const bool s_liftTrace = (std::getenv("MC2_SPOT_LIFT_TRACE") != nullptr);`
  - `static bool s_liftFirstHit = false;`
  - `static unsigned long s_liftFrames = 0;`
  - `static unsigned long s_liftWindowPriority = 0;`
  - `static unsigned long s_liftWindowDeferred = 0;`
- Inside `Camera::updateLights`, at the top (right after `numActiveLights = numTerrainLights = 0;` at [:1891](mclib/camera.cpp)), add:
  - `long s_liftPriorityCount = 0;` (function-local; resets per call)
  - `long numDeferred = 0;` (function-local; resets per call — round-1 C2 fix)
- At the END of `Camera::updateLights`, AFTER the existing main loop (line 1983), add the full emit block per spec §3.7. **Round-2 M2 fix — mirror existing T1.15 pattern (increment-before-check) so first summary fires at frame 600 NOT frame 0:**
  ```cpp
  // [SPOT_PRIORITY_LIFT v1] — see spec for full rationale
  const long priorityCount = s_liftPriorityCount;
  const long deferredCount = numDeferred;
  if (!s_liftFirstHit && (priorityCount > 0 || deferredCount > 0)) {
      std::fprintf(stderr,
          "[SPOT_PRIORITY_LIFT v1] event=first_hit priority=%ld deferred=%ld total_active=%ld\n",
          priorityCount, deferredCount, (long)numActiveLights);
      std::fflush(stderr);
      s_liftFirstHit = true;
  }
  ++s_liftFrames;  // BEFORE summary check (mirrors T1.15 pattern at camera.cpp:1892 / :1985)
  s_liftWindowPriority += priorityCount;
  s_liftWindowDeferred += deferredCount;
  if (s_liftTrace && (s_liftFrames % 600 == 0)) {
      std::fprintf(stderr,
          "[SPOT_PRIORITY_LIFT v1] event=summary frames=%lu avg_priority=%.2f avg_deferred=%.2f\n",
          s_liftFrames,
          (double)s_liftWindowPriority / 600.0,
          (double)s_liftWindowDeferred / 600.0);
      std::fflush(stderr);
      s_liftWindowPriority = 0;
      s_liftWindowDeferred = 0;
  }
  ```
- NO reorder logic yet. `s_liftPriorityCount` stays 0 throughout (no `++` calls yet); `numDeferred` stays 0 throughout (no `++` calls or flush loop yet). T1.1 will add those.

**Verification:** smoke mc2_10 with `MC2_SPOT_LIFT_TRACE=1` shows summary line firing every 600 frames with `avg_priority=0.00 avg_deferred=0.00`. The `event=first_hit` line should NOT fire (gated on `priority > 0 || deferred > 0` which are both zero in T0.1). Probe wiring confirmed; baseline observable; no behavior change.

**Commit:** `feat(spotlight-lift T0.1): wire [SPOT_PRIORITY_LIFT v1] probe scaffolding`

---

## Stage 1 — Priority lift implementation (default-on, no gate)

Per spec §3.4: tagging mechanism (`is_e_slot`) is already populated unconditionally. No env gate needed for the lift itself — it's the new correct behavior.

### T1.1 — Implement two-pass append in `Camera::updateLights`

**Files:** `mclib/camera.cpp` (`Camera::updateLights` at [:1887-1983](mclib/camera.cpp))

**CRITICAL — round-1 M1 + round-2 M1 fix — surgical replacement, preserve existing probes, enumerate all activeLights append sites:**

Round 2 grep-verified that `activeLights[numActiveLights++] = light;` appears at THREE places in `Camera::updateLights`:

| Line | Branch | In scope for T1.1? |
|---|---|---|
| `camera.cpp:1911` | TG_LIGHT_TERRAIN branch | **NO** — leave unchanged |
| `camera.cpp:1922` | AMBIENT/INFINITE branch | **NO** — leave unchanged |
| `camera.cpp:1979` | POINT/SPOT branch | **YES** — this is the ONLY one T1.1 replaces |

T1.1 modifies ONLY the POINT/SPOT branch append at line 1979. The TERRAIN and AMBIENT/INFINITE appends stay untouched (they're outside the priority-lift scope; AMBIENT/INFINITE/TERRAIN are scene-global lights that should always reach `activeLights[]` regardless of priority).

The POINT/SPOT branch at [camera.cpp:1927-1981](mclib/camera.cpp) contains:
- The existing `light->active = projectForLightingShadow(...)` assignment
- The existing `terrainLights[numTerrainLights++] = light` append
- T1.15's per-slot first-seen probe block at approximately [:1936-1960](mclib/camera.cpp)
- T1.16's per-slot transition probe block at approximately [:1961-1978](mclib/camera.cpp)
- The `activeLights[numActiveLights++] = light;` append at [:1979](mclib/camera.cpp) (the LAST line in the branch)

**ONLY the single line `activeLights[numActiveLights++] = light;` at [:1979](mclib/camera.cpp) is replaced.** All T1.15 and T1.16 probe code is preserved verbatim per worktree CLAUDE.md "Debug Instrumentation Rule: demote-not-delete."

**Changes:**
- Add file-scope `static TG_LightPtr s_deferredNonPriority[MAX_LIGHTS_IN_WORLD];` near the other file-scope statics (no thread safety needed; engine single-threaded for this path per `memory/vulkan_prep_explicit_device_discipline.md`).
- In `Camera::updateLights`, at the POINT/SPOT branch ([camera.cpp:1927-1981](mclib/camera.cpp)):
  - Keep the existing `light->active = projectForLightingShadow(...)` assignment unchanged.
  - Keep the existing `terrainLights[numTerrainLights++] = light` append unchanged.
  - Keep the existing T1.15 `[SPOT_DIAG v1] event=overwrite_first_seen` block unchanged.
  - Keep the existing T1.16 `[SPOT_DIAG v1] event=our_slot_transition` block unchanged.
  - **Replace** the single statement `activeLights[numActiveLights++] = light;` at approximately [:1979](mclib/camera.cpp) with the priority split:
    ```cpp
    if (mc2_spotlight_diag::is_e_slot(i, nullptr)) {
        // (E') priority lift: SpotLight_-tagged POINT/SPOT survives FIFO 16-cap
        activeLights[numActiveLights++] = light;
        ++s_liftPriorityCount;
    } else {
        // Non-priority POINT/SPOT/non-TERRAIN: defer to post-loop flush
        s_deferredNonPriority[numDeferred++] = light;
    }
    ```
- After the main loop closing brace (line 1981 of current code), BEFORE the T0.1 probe emit block added in T0.1, add the deferred-flush:
  ```cpp
  // (E') flush deferred non-priority POINT/SPOT into activeLights after
  // all priority lights have been appended. FIFO truncation in
  // GatherLightsParameters now reaches priority lights first.
  for (long j = 0; j < numDeferred && numActiveLights < MAX_LIGHTS_IN_WORLD; ++j) {
      activeLights[numActiveLights++] = s_deferredNonPriority[j];
  }
  ```

**Include addition:** `#include "spotlight_diag.h"` at top of `camera.cpp` if not already present (T0.1 may have added it already).

**Verification:** smoke mc2_10 with NO env vars:
- `[SPOT_PRIORITY_LIFT v1] event=first_hit` fires once (wiring confirmation only — any priority > 0 counts; the gate is just "did the lift fire at all").
- With `MC2_SPOT_LIFT_TRACE=1`: per-summary lines on mc2_10 show `avg_priority>=3.0` across the first 600-frame summary window (round-1 M4 threshold fix — matches T1.16 baseline of 5-7 active priority lights, with slack for actor-destroy `updateLights` calls during teardown that may see a smaller subset).
- **Round-2 MINOR-4 note**: `++s_liftFrames` ticks on EVERY `Camera::updateLights` call, including the destroy-triggered calls from `removeWorldLight` (see [camera.h:829, :843](mclib/camera.h)). Probe statistics MAY include over-counting during teardown; this is acknowledged as diagnostic-accuracy-only per spec §A5 and does not affect the priority-lift correctness.
- mc2_10 visual canary: mech/GV bodies show yellow contribution from their SpotLight_ children.

**Commit:** `feat(spotlight-lift T1.1): priority-lift activeLights assembly in Camera::updateLights`

### T1.2 — Visual canary across 3 missions

**Files:** none (validation gate)

**Action:** USER-DRIVEN smoke per worktree CLAUDE.md "Smoke sessions are USER-DRIVEN". Round-1 M3 fix: absolute paths + 30s duration (worktree canonical):
1. `set MC2_SPOT_LIFT_TRACE=1 && py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --mission mc2_10 --duration 30 --keep-logs` → expect `avg_priority>=3.0` first summary window; mech/GV illumination visible
2. `set MC2_SPOT_LIFT_TRACE=1 && py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --mission mc2_05 --duration 30 --keep-logs` → expect `avg_priority>=1.0` (mc2_05 has building lamps); ground illumination near lamps visible
3. `set MC2_SPOT_LIFT_TRACE=1 && py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --mission mc2_24 --duration 30 --keep-logs` → Vedette/LRMC integrity check

NO `--kill-existing` (user kill-aware). Single mission per command (user discipline).

If any mission fails (crash, GL_DEBUG_HIGH abort, Vedette invisibility, priority count 0 on missions where SpotLight_ exist), STOP and surface.

**Commit:** none (validation gate)

---

## Stage 2 — Cleanup and OQ6 reset wiring

### T2.1 — Wire `mc2_spotlight_diag::reset()` + probe-statics reset to mission boundary (OQ6)

**Files:** `mclib/camera.h` (declaration), `mclib/camera.cpp` (definition), `code/mission.cpp` (Mission::init call site), `code/saveload.cpp` (Mission::load call site — round-4 CRITICAL-2 fix)

**Design — all prior-round fixes consolidated:**

Camera.cpp owns the `[SPOT_PRIORITY_LIFT v1]` probe statics (internal linkage). Add a free function `resetSpotlightLiftProbe()` in camera.cpp. Declare it in camera.h. Both `Mission::init` AND `Mission::load` call the resets — they are PARALLEL mission-START boundaries. `mclib/spotlight_diag.cpp` is NOT modified.

**Why mission-START not Mission::destroy (round-3 MAJOR-1):** during `Mission::destroy`, per-actor destroy hooks call `removeWorldLight` → `updateLights()` cascade ([camera.h:829, :843](mclib/camera.h)). Those post-reset `updateLights` calls would re-assert `s_liftFirstHit=true` and tick `s_liftFrames`. Mission-START is the only boundary where no actor-destroy can fire.

**Why BOTH Mission::init and Mission::load (round-4 CRITICAL-2):** `Mission::load` at [code/saveload.cpp:640](code/saveload.cpp) is the quicksave-resume path; it bypasses `Mission::init`. The codebase's existing convention is that both init paths get parallel hard_resets — see `mc2_cpu_proj_cost::hard_reset("Mission::init")` at [mission.cpp:1687](code/mission.cpp) AND `mc2_cpu_proj_cost::hard_reset("Mission::load")` at [saveload.cpp:645](code/saveload.cpp). Mirror that pattern exactly.

**Changes:**

1. In `mclib/camera.cpp` (next to the lift-probe statics at file scope), add:
   ```cpp
   // Reset the [SPOT_PRIORITY_LIFT v1] probe state for a new mission.
   // Called from Mission::init (mission.cpp:1687) AND Mission::load
   // (saveload.cpp:645) to mirror the mc2_cpu_proj_cost::hard_reset
   // pattern used by both init paths.
   void resetSpotlightLiftProbe() {
       s_liftFirstHit = false;
       s_liftFrames = 0;
       s_liftWindowPriority = 0;
       s_liftWindowDeferred = 0;
   }
   ```

2. In `mclib/camera.h`, add the declaration:
   ```cpp
   void resetSpotlightLiftProbe();
   ```

3. In `code/mission.cpp` at `Mission::init` immediately after the existing `::mc2_cpu_proj_cost::hard_reset("Mission::init");` at line [1687](code/mission.cpp):
   ```cpp
   // (E') reset diagnostic state — mirrors the cpu_proj_cost pattern at line 1687
   mc2_spotlight_diag::reset();
   resetSpotlightLiftProbe();
   ```

4. In `code/saveload.cpp` at `Mission::load` immediately after the existing `::mc2_cpu_proj_cost::hard_reset("Mission::load");` at line [645](code/saveload.cpp):
   ```cpp
   // (E') reset diagnostic state — mirrors the cpu_proj_cost pattern at line 645
   mc2_spotlight_diag::reset();
   resetSpotlightLiftProbe();
   ```

5. **Include audit:**
   - `code/mission.cpp` has `#include "mclib.h"` (line 14) → transitively includes `camera.h` via `mclib.h:135`. No direct `camera.h` include needed.
   - `code/mission.cpp` does NOT include `spotlight_diag.h` (grep-verified). **Add `#include "spotlight_diag.h"` unconditionally** to mission.cpp.
   - `code/saveload.cpp`: grep at plan-execute time for `mclib.h` / `spotlight_diag.h` / `camera.h` includes. Add `#include "spotlight_diag.h"` if not transitively available. `camera.h` is almost certainly available via mclib.h or similar; verify.

**Verification:**
- Two-mission canary: load mc2_10, then mc2_05, with `MC2_SPOT_LIFT_TRACE=1`. Expect `event=first_hit` to fire ONCE per mission (two distinct lines in artifact log).
- Quicksave-resume canary: load mc2_10, quicksave, quickload — expect a NEW `event=first_hit` line after the quickload.

**Commit:** `feat(spotlight-lift T2.1): wire reset() to Mission::init + Mission::load — OQ6 fix`

### T2.2 — Final substitutive verification

**Files:** none (validation gate; round-3 MINOR-2 fix: explicitly USER-DRIVEN per worktree CLAUDE.md "Smoke sessions are USER-DRIVEN")

**Action (USER-DRIVEN):**
1. Single-mission smoke (no env vars), one-at-a-time per user discipline: mc2_05, mc2_10, mc2_24 all PASS.
2. **(Optional, USER-DRIVEN)** RenderDoc frame capture on mc2_10 intro: mech `lightDataIndex` slot's `TG_HWLightsData` contains the SpotLight_ POINT entry within first 16 entries. User decides whether to run RenderDoc; not blocking for merge.
3. **(Optional, USER-DRIVEN)** With `MC2_OBJECT_RECON_TRACY=1` (existing infrastructure, gates `[LIGHTBRIDGE v1]` counters per txmmgr.cpp:1076-1085): confirm template-cache hit rate unchanged from pre-(E') baseline.

**Commit:** none (validation gate)

---

## Out-of-scope items (filed)

- **Anubis pointLight leak fix** — per spec §3.5 Option X. Independent slice; required before any future Option Y promotion. Filed.
- **Scored selection** — when content scale exceeds 16 simultaneously-active SpotLight_ POINTs. Future (F)-class.
- **Clustered/forward+ lighting** — future architectural rework. (F)-class+.

## Total implementation scope

- **Files modified:**
  - `mclib/camera.cpp` (T0.1 probe scaffolding + T1.1 priority-lift body + T2.1 reset function)
  - `mclib/camera.h` (T2.1 `resetSpotlightLiftProbe()` declaration)
  - `code/mission.cpp` (T2.1 reset calls at `Mission::init:1687`)
  - `code/saveload.cpp` (T2.1 reset calls at `Mission::load:645` — round-4 CRITICAL-2 fix)
- **NOT modified:** `mclib/spotlight_diag.cpp` (round-1 C1 fix — corrected from plan v1)
- **Total LOC:** ~55 in `camera.cpp` (T0.1+T1.1+T2.1 function) + ~2 in `camera.h` + ~5 in `mission.cpp` = ~62 LOC.
- **No shader changes. No SSBO layout changes. No new TU. No accessor signature changes. No header layout changes (no `--clean-first` required).**

## Cross-references

- Spec: [2026-05-21-spot-pack-scored-selection-design.md](../specs/2026-05-21-spot-pack-scored-selection-design.md) (v4.1)
- T1.16 diagnostic: `tests/smoke/artifacts/2026-05-21T06-48-58/mc2_10.log`
- (E) plan: [2026-05-20-spotlight-real-illumination-plan.md](2026-05-20-spotlight-real-illumination-plan.md)
- (F) lighting MODEL rework: [memory/terrain_lighting_compute_kernel_saturation.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\terrain_lighting_compute_kernel_saturation.md) — separate orthogonal slice
- Worktree CLAUDE.md "Smoke sessions are USER-DRIVEN" — verification discipline
- [memory/feedback_class_layout_change_needs_clean_first.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_class_layout_change_needs_clean_first.md) — not triggered (no class layout change)
