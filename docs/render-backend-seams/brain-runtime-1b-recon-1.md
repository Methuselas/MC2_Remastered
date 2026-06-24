# BRAIN-RUNTIME-1B-RECON-1 — Scoping the first behavior-changing slice

**Date:** 2026-06-23  
**Status:** RECON — read-only. No code changed.  
**Prereqs confirmed:** BRAIN-RUNTIME-1A (compute+trace shipped, gate `MC2_BRAIN_RUNTIME`, nifty `7b2fee53`), BRAIN-FIT-SCHEMA-1 (checker + fixtures shipped), TASK-SCHEDULER-CORE-1 (gate `MC2_BRAIN_TASKQ`, drain stub live).

---

## §1 — MINIMAL MODE LOADER

### §1.1 — Where mode lives in mission_ai.fit

Per `docs/render-backend-seams/SCOPE-BRAIN-FIT-SCHEMA-1.md`, the schema puts one `Brain { }` block per warrior in a companion file `<mission_stem>_ai.fit` (e.g. `mc2_01_ai.fit` alongside `mc2_01.fit`). The mode field is a top-level key in that block:

```fit
Brain {
    unitRef = "Warrior4"
    mode    = "Enhanced"        // "Legacy" | "Hybrid" | "Enhanced"
    OPORD { slot = "Primary" type = "Patrol" looping = 1 }
    fallback_policy = "HoldPosition"
}
```

Mode is **per-unit** (per Brain block), not per-mission. A single `mission_ai.fit` may have Warrior4 as Enhanced and Warrior6 as Legacy (different ABL brains for different AI roles on the same map). A mission-level default could be added later via a top-level `MissionDefault { mode = "Hybrid" }` block, but that is deferred — in 1B every Brain block must carry its own `mode=` key.

The modder's carver_v_enhanced data (`C:/Users/Joe/Downloads/GameAsset/carver_v_enhanced/missions/mc2_01/mission.fit`) uses `compatibilityMode = "Enhanced"` inline in each `[WarriorN]` block within the main mission.fit rather than a companion file. 1B **does not** parse the modder's inline format — that is a later migration path. 1B opens a separate `*_ai.fit` only.

### §1.2 — unitRef binding

`unitRef = "Warrior4"` maps directly to the `[Warrior4]` section index in mission.fit / `MechWarrior::warriorList[4]`. This is the same integer-suffixed convention used by `loadBrainParameters` today (`code/mission.cpp:2947` — `sprintf(warriorName,"Warrior%d",i)` for `i` in `1..numWarriors`). The binding is by **warrior list position (1-based index)**, NOT by vehicleWID or pilot name. This is safe: warrior list position is stable for the lifetime of a loaded mission and is how all existing brain parameter loads work.

No new concept is needed: `unitRef = "Warrior4"` → `std::sscanf(unitRef, "Warrior%d", &idx)` → `MechWarrior::warriorList[idx]`.

### §1.3 — Load hook in mission.cpp

The correct insertion point is immediately after the existing `loadBrainParameters` loop:

```
code/mission.cpp:3024-3031  — existing loop: loadBrainParameters(missionFile, i) for each warrior
code/mission.cpp:3033+      — 1B insertion: open <stem>_ai.fit, iterate Brain blocks, set brainRuntime->mode per warrior
```

Specifically, after `mission.cpp:3031` (end of `loadBrainParameters` loop), add:

```cpp
// MC2_BRAIN_RUNTIME: load companion mission_ai.fit (1B — mode per warrior)
initBrainRuntimeGate();
if (s_brainRuntimeEnabled) {
    // derive stem: missionFile->getFilePath() → strip extension → append "_ai.fit"
    // open with FitIniFile (absent = silent skip, all warriors stay Legacy)
    // for each Brain block: sscanf unitRef→idx, set warriorList[idx]->brainRuntime->mode
    loadMissionAIFit(missionFile);
}
```

`loadMissionAIFit()` is a new static function in `mission.cpp` (no new .cpp file needed). It uses the existing `FitIniFile` API (`seekBlock`, `readIdString`, etc.) — the same API used by `loadBrainParameters` directly above it.

`FitIniFile` absence handling: the open call returns a non-`NO_ERR` result if the file does not exist; the function treats that as a clean silent skip (all warriors remain Legacy). This pattern is already established in `mission.cpp` (`result = missionFile->seekBlock(...)` with `gosASSERT` is the existing style, but the new loader must use a guarded open, not assert on absence).

### §1.4 — MC2_BRAIN_RUNTIME_FORCE_MODE stays as override/fallback

Yes. The existing env-var override (`warrior.cpp:136-141`) was designed as a **global override** for all warriors, used for smoke testing and debugging. After 1B adds per-warrior loading from `mission_ai.fit`, the precedence is:

1. `MC2_BRAIN_RUNTIME_FORCE_MODE` (env) — overrides ALL warriors (global smoke/debug knob)
2. Per-warrior mode from `mission_ai.fit` (loaded at mission start)
3. Default: `BrainRuntimeMode::Legacy` (no `_ai.fit`, or no Brain block for this warrior)

This means smoke tests can still use `MC2_BRAIN_RUNTIME_FORCE_MODE=enhanced` to force all warriors into Enhanced without needing a fixture `_ai.fit`. The per-warrior loader runs first; then `initBrainRuntimeGate()` reads the env var; but the forced-mode override must be applied **after** per-warrior load, clamping `brainRuntime->mode` to the forced value on all warriors. Implementation detail: `loadMissionAIFit()` sets mode per-warrior from file; immediately after, if `s_brainRuntimeForcedMode != Legacy` (i.e., the env var was set), a second pass re-stamps all `brainRuntime->mode` values.

### §1.5 — Smallest loader shape

```cpp
// mission.cpp — after existing loadBrainParameters loop (line ~3031)
static void loadMissionAIFit(const char* missionStem) {
    // 1. Construct path: <missionStem>_ai.fit
    // 2. FitIniFile f; if (f.open(path) != NO_ERR) return;  // absent = silent
    // 3. while (f.seekBlock("Brain") == NO_ERR) {
    //      char unitRef[32]; f.readIdString("unitRef", unitRef, 31);
    //      int idx = 0; if (sscanf(unitRef, "Warrior%d", &idx) != 1) continue;
    //      if (idx < 1 || idx > MechWarrior::numWarriors) continue;
    //      char modeStr[32]; f.readIdString("mode", modeStr, 31);
    //      auto* w = MechWarrior::warriorList[idx];
    //      if (!w->brainRuntime) w->brainRuntime = new MechBrainRuntime();
    //      w->brainRuntime->mode = parseBrainMode(modeStr);  // "Legacy"→0, "Hybrid"→1, "Enhanced"→2
    //    }
    // 4. If MC2_BRAIN_RUNTIME_FORCE_MODE is set, re-stamp all warriors.
}
```

The OPORD slot data from `mission_ai.fit` (Primary/Secondary/Tertiary OPORD types, looping, waypointGroup, fallback_policy) is **read but not acted on in 1B** — those fields are stored in `MechBrainRuntime` future fields (or discarded) and deferred to the OPORD-SLOT-RUNTIME-1 slice. The 1B loader only needs `unitRef` and `mode` from each Brain block.

---

## §2 — MINIMAL FIRST APPLIED BEHAVIOR

### §2.1 — What is the smallest safe "apply"?

`REQUEST_ORDERS` is NOT the simplest first applied behavior. It requires:
- A commander chain check (`getCommander()`)
- A `receiveOrdersRequest()` method that doesn't exist yet
- Correct no-commander fallback logic
- Recursion guard (`lastRequestOrdersFrame`)
- The task fires when an OPORD slot completes — which requires OPORD completion detection

The simplest single applied task that writes one tacOrder slot directly is a **HOLD task**: push `TACTICAL_ORDER_STOP` (or `TACTICAL_ORDER_WAIT`) into `ORDERSTATE_GENERAL` via `setGeneralTacOrder`. This:
- Requires no commander hop
- Has no recursion risk
- Writes exactly one slot
- Has a visually observable effect (unit stops moving)
- Is a valid `TacticalOrderCode` (`TACTICAL_ORDER_STOP`, `tacordr.h:141`)

**Recommended minimal first applied task type: `HOLD_TASK`**

### §2.2 — Exact contract for HOLD_TASK

```
Task entry in BrainTaskQueue:
    priority_tier      = 3 (Tactical — mid-priority; not emergency, not background)
    type               = HOLD_TASK  (new enum value in BrainTaskEntry, 1B)
    warrior_id         = vehicleWID of target warrior

Drain loop (warrior.cpp, inside the brain cadence gate at ~line 5074):
    pops entry with type == HOLD_TASK
    → if (brainRuntime->mode == Enhanced):
        TacticalOrder holdOrder;
        holdOrder.init();
        holdOrder.code   = TACTICAL_ORDER_STOP;
        holdOrder.origin = ORDER_ORIGIN_BRAIN;   // new enum value, or reuse existing
        setGeneralTacOrder(holdOrder);
        trace: "[BRAIN_RT] HOLD_TASK applied wid=%d → ORDERSTATE_GENERAL STOP"
    → if (brainRuntime->mode == Hybrid):
        setPlayerTacOrder(holdOrder);            // Hybrid: Brain owns PLAYER slot only
    → if (mode == Legacy):
        discard (should not be pushed in Legacy, but guard silently)
```

`TacticalOrder::init()` is already defined (`tacordr.h:212+`); sets `code = TACTICAL_ORDER_NONE`, clears fields. 1B overrides `code` and `origin` after `init()`.

`ORDER_ORIGIN_BRAIN` is a new `OrderOriginType` value. Check `tacordr.h` for the existing enum. If it's too risky to add a new origin (ABI concern for save/load), use `ORDER_ORIGIN_PLAYER` as a temporary stand-in with a TODO comment — but the preferred choice is a new value since Brain-origin orders should be distinguishable in diagnostics.

### §2.3 — What the unit does

When `HOLD_TASK` is drained:
1. `setGeneralTacOrder(holdOrder)` sets `tacOrder[ORDERSTATE_GENERAL] = holdOrder` and `newTacOrderReceived[ORDERSTATE_GENERAL] = true` (`warrior.cpp:4564-4568`).
2. The tac-order dispatch at `warrior.cpp:5117-5165` picks it up: `case ORDERSTATE_GENERAL: ... else if (newTacOrderReceived[ORDERSTATE_GENERAL]) newTacOrder = tacOrder[ORDERSTATE_GENERAL]`.
3. The unit receives `TACTICAL_ORDER_STOP` → stops in place and holds.

This is the minimal observable behavior: a unit that was previously ABL-driven (moving on patrol, etc.) transitions to Brain-driven HOLD when Enhanced mode is active.

### §2.4 — No REQUEST_ORDERS in 1B

`REQUEST_ORDERS` is deferred to the OPORD-SLOT-RUNTIME-1 slice. The commander chain (`receiveOrdersRequest()`) is a new method that needs more design work. In 1B, when a warrior runs out of OPORD tasks, the fallback behavior is determined by `opordFallbackPolicy`. For HOLD_TASK-only 1B, this is moot — the HOLD task is manually pushed (via the APPLY gate) rather than generated by OPORD completion.

**How 1B pushes a HOLD_TASK:** For the first applied behavior test, the task is pushed once at mission start (inside `loadMissionAIFit` or at the first brain cadence tick for Enhanced warriors, using a `!brainRuntime->initialHoldPushed` guard). This is the minimal provable path: Enhanced warrior gets one HOLD_TASK pushed at init → drain pops it → `setGeneralTacOrder(STOP)` → unit holds.

---

## §3 — ABL SHORT-CIRCUIT POINT

### §3.1 — Exact call to gate (Enhanced mode)

In `runBrain()` (`warrior.cpp:2168`), the ABL execution is a single call:

```cpp
warrior.cpp:2204    brain->execute();
```

Immediately before this call is the setup of `CurGroup`, `CurObject`, `CurObjectClass`, `CurWarrior`, `CurContact`, `curEventID`, `curEventTrigger` (`warrior.cpp:2194-2202`). After it comes `setUseGoalPlan(...)`, `calcTacOrder(...)`, and `clearAlarmsHistory()`.

**The gate for Enhanced mode short-circuit:**

```cpp
// warrior.cpp:2204 region — 1B insertion
if (brainRuntime && brainRuntime->mode == BrainRuntimeMode::Enhanced) {
    // ABL brain->execute() is SKIPPED for Enhanced units.
    // Brain runtime owns all three tac-order slots; ABL must not write them.
    // MUST NOT SKIP: clearAlarmsHistory() at line 2246 (below)
    // MUST NOT SKIP: setUseGoalPlan() at line 2207 (side-effect on goal planner flag)
    // Jump to post-execute cleanup:
    goto brain_execute_done;  // or restructure with if/else
}
brain->execute();
// ... existing useGoalPlan / calcTacOrder block ...
brain_execute_done:
clearAlarmsHistory();  // line 2246 — MUST run in all modes
```

Prefer `if/else` over `goto` for legibility. The restructure is:

```cpp
if (!(brainRuntime && brainRuntime->mode == BrainRuntimeMode::Enhanced)) {
    brain->execute();
    setUseGoalPlan(!MPlayer && (getCommander() != Commander::home));
    if (useGoalPlan) {
        // ... calcTacOrder block (lines 2210-2223) ...
    }
    CurGroup = NULL; CurObject = NULL; CurObjectClass = 0;
    CurWarrior = NULL; CurContact = NULL;
    brainErr = brain->getInteger();  // line 2234
    // ... switch(brainErr) ...
}
clearAlarmsHistory();   // always runs
```

### §3.2 — What MUST NOT be skipped

| Item | Line | Why it must still run |
|---|---|---|
| `clearAlarmsHistory()` | ~2246 | Clears per-tick alarm trigger history; if skipped, alarm arrays accumulate indefinitely and future alarm checks read stale data |
| `CurGroup/CurObject/CurWarrior = NULL` | ~2226-2230 | These are thread-local globals set before brain; if not cleared, next warrior's brain tick sees stale pointers |
| `setUseGoalPlan(...)` | 2207 | This sets the `useGoalPlan` flag based on commander chain; in Enhanced mode, the Brain runtime does not use goal planning, so it is safe to skip the `if (useGoalPlan)` block — but `useGoalPlan` should be forced false for Enhanced warriors rather than computing it from `getCommander()`. Simplest: just skip the call entirely for Enhanced (Brain runtime doesn't use goal planner). |

**What IS safe to skip entirely for Enhanced:**
- `brain->execute()` — Brain runtime replaces ABL
- `brain->getInteger()` — reads ABL return code; meaningless without execute
- `calcTacOrder(...)` — ABL goal planning; Brain runtime uses its own slot writes
- The `switch(brainErr)` block

### §3.3 — Hybrid write-ordering rule

For Hybrid mode (Brain owns PLAYER/ALARM, ABL owns GENERAL):

1. ABL `brain->execute()` runs **first** (unchanged). It may call `setGeneralTacOrder()`, `setAlarmTacOrder()`, possibly `setPlayerTacOrder()`.
2. After `brain->execute()` returns, the Brain runtime drain runs and may call `setPlayerTacOrder()` (HOLD_TASK in Hybrid pushes to PLAYER slot).
3. **Clobber rule:** Brain runtime MUST NOT call `setGeneralTacOrder()` in Hybrid mode. The drain loop checks `brainRuntime->mode == Hybrid` and routes to `setPlayerTacOrder()` only. ABL's `setPlayerTacOrder()` calls (if any) from within `brain->execute()` fire before Brain runtime writes; since Brain runtime writes last, it wins the PLAYER slot.
4. This ordering is inherent: drain runs in the same brain cadence tick, after `runBrain()` returns (see `warrior.cpp:5074` — drain is called before `runBrain()` in 1A, but 1B must restructure to run the APPLY drain **after** ABL so Hybrid ordering is correct).

**1B restructure of the cadence block (`warrior.cpp:5064-5103`):**

```
1. initBrainRuntimeGate()
2. if (s_brainTaskQEnabled && brainTaskQueue): drain() for non-apply tasks (GENERIC/ALARM_REACTION) — stub, unchanged
3. runBrain()  ← ABL executes here (or is skipped for Enhanced)
4. if (s_brainRuntimeEnabled && brainRuntime): drain() for APPLY tasks (HOLD_TASK, etc.)
   — this is the NEW apply-drain, runs AFTER ABL
```

This ensures: for Hybrid, ABL writes GENERAL first, then Brain drain writes PLAYER. For Enhanced, ABL is skipped, Brain drain writes GENERAL.

---

## §4 — ENHANCED FIXTURE + ACCEPTANCE

### §4.1 — Concrete test fixture

**Target mission:** `mc2_01` (tier1 mission, 30s smoke, Warrior4 is an AI patrol unit with `mc2_01_Pat1_1` brain — confirmed in carver_v_enhanced data; same warrior index exists in stock mission).

**Fixture file:** `data/missions/mc2_01/mc2_01_ai.fit` (new, created for test):

```fit
// mc2_01_ai.fit — BRAIN-RUNTIME-1B acceptance fixture
// Warrior4 = Enhanced mode. Brain runtime will push HOLD_TASK at init.
// Expected behavior: Warrior4 stops in place (STOP order) rather than
// executing its ABL patrol (mc2_01_Pat1_1).

SchemaVersion { version = 1 }

Brain {
    unitRef = "Warrior4"
    mode    = "Enhanced"
    OPORD { slot = "Primary" type = "Guard" looping = 0 }
    fallback_policy = "HoldPosition"
}
```

### §4.2 — What the unit should do

With `MC2_BRAIN_RUNTIME=1 MC2_BRAIN_TASKQ=1` and this fixture present:
- Warrior4 gets `brainRuntime->mode = Enhanced` at load
- First brain cadence tick: `brain->execute()` is SKIPPED
- Apply drain: pops `HOLD_TASK` → `setGeneralTacOrder(TACTICAL_ORDER_STOP)`
- **Observable:** Warrior4 does NOT patrol. It holds position. This is distinct from idle (idle = ABL brain doing nothing; STOP = explicit hold order received).

### §4.3 — Smoke delta signal

Pure non-crash is not sufficient. The delta signal must prove the order was **applied**, not just that the unit didn't crash.

**Delta signal method:** use `MC2_BRAIN_RUNTIME_TRACE=1` and check stderr for:
```
[BRAIN_RT] HOLD_TASK applied wid=<N> → ORDERSTATE_GENERAL STOP
```
The smoke harness captures stderr (confirmed: `scripts/run_smoke.py:229` — `subprocess.STDOUT` merges stderr into stdout for log capture). The acceptance check is that this line appears in the mc2_01 smoke log when gate-ON + fixture.

A second-order signal: if the unit starts at a known position and is still there after 30s (using a position snapshot from `MC2_DEBUG_STATE_DUMP`), that confirms STOP order was held. This is stronger but requires a position-delta extractor in the smoke harness — deferred. For 1B, the trace line is the acceptance delta.

### §4.4 — Gate matrix for acceptance

| Configuration | Expected result |
|---|---|
| Gate OFF (`MC2_BRAIN_RUNTIME=0`) | tier1 5/5 PASS, no `[BRAIN_RT]` in logs, behavior identical to baseline |
| Gate ON, no `mc2_01_ai.fit` present | tier1 5/5 PASS, all warriors Legacy (no apply), no `[BRAIN_RT] HOLD_TASK` |
| Gate ON + fixture, Warrior4 Enhanced | mc2_01 PASS (no crash), `[BRAIN_RT] HOLD_TASK applied wid=...` appears in log |
| Gate ON, `MC2_BRAIN_RUNTIME_FORCE_MODE=legacy` + fixture | All warriors forced Legacy override, no HOLD_TASK applied (env wins) |
| Gate ON, Hybrid (Warrior4 mode=Hybrid) | `[BRAIN_RT] HOLD_TASK applied wid=...` to PLAYER slot; ABL still writes GENERAL |
| Gate ON, mc2_10, mc2_24 (no fixture) | tier1 PASS, no regression |

---

## §5 — HARDENING TO BAKE IN

### §5.1 — Determinism of order flow

- `BrainTaskQueue::drain()` uses 4-key sort (tier → frame_ms → seq → warrior_id) from TASK-SCHEDULER-CORE-1 (`brain_task_queue.h:46-52`). This is already deterministic across runs given the same scenario time.
- `HOLD_TASK` is pushed once at mission init (or first brain cadence), not every tick. Guard: `!brainRuntime->initialHoldPushed` (new bool field, set on push). This prevents the queue from accumulating duplicate HOLD_TASK entries.
- Apply drain (`setGeneralTacOrder`) writes the warrior's slot; there is no cross-warrior shared state involved for HOLD_TASK. No additional determinism concern.
- `lastRequestOrdersFrame` in `MechBrainRuntime` is deferred to REQUEST_ORDERS (OPORD-SLOT-RUNTIME-1). Not needed for HOLD_TASK.

### §5.2 — No mid-mission save

`MechBrainRuntime` is explicitly non-serialized (`mech_brain_runtime.h:3-4`). Same policy as `BrainTaskQueue`. HOLD_TASK pushed at init will NOT be re-pushed after a save/load reload in a future BRAIN-SAVELOAD-1 slice (the push guard `initialHoldPushed` is reset at mission init). This is acceptable for 1B — the unit will re-receive the HOLD order at the next brain cadence tick after reload, which is correct behavior.

### §5.3 — Per-unit namespaced variable note

`Var.Set scope=Mission` is **deferred to TECHSCRIPT-SPECIAL-DISPATCH-1**. For 1B (HOLD_TASK only), there are no mission-scoped variables. When that dispatch layer is added, all UnitBrainSpecial variables MUST be namespaced `<warrior_id>.<varname>` (recon-1.md §7 Gap 2). This rule is baked into the schema design but not enforced in 1B code.

---

## §6 — SCOPED 1B BUILD SLICE

### §6.1 — Files to touch

| File | Change |
|---|---|
| `code/brain_task_queue.h` | Add `BrainTaskType` enum (`GENERIC=0`, `HOLD_TASK=1`, `REQUEST_ORDERS=2` reserved, `ALARM_REACTION=3`, `BRAIN_SPECIAL=4` deferred); add `type` field to `BrainTaskEntry`; add typed `pushHoldTask(warrior_id)` helper |
| `code/mech_brain_runtime.h` | Add `bool initialHoldPushed = false;` field; add `OrderOriginType brainOrderOrigin` note (or use existing enum value) |
| `code/warrior.h` | Add `receiveOrdersRequest()` declaration (stub, deferred); no layout change beyond 1A |
| `code/warrior.cpp` | (a) `initBrainRuntimeGate` — no change; (b) `runBrain()` — add Enhanced ABL short-circuit (`if mode==Enhanced: skip brain->execute(), skip calcTacOrder, clear globals, skip brainErr`); (c) brain cadence block (~5064) — restructure to run apply-drain AFTER `runBrain()`; (d) apply-drain: pop `HOLD_TASK` → `setGeneralTacOrder`/`setPlayerTacOrder` by mode; emit trace |
| `code/mission.cpp` | Add `loadMissionAIFit()` static function; call it after existing `loadBrainParameters` loop (~line 3031) inside `MC2_BRAIN_RUNTIME` gate |
| `data/missions/mc2_01/mc2_01_ai.fit` | New acceptance fixture (Enhanced Warrior4 HOLD) |
| `scripts/run_smoke.py` | No change needed (gates already in allowlist per 1A) |

### §6.2 — Gate

Reuse `MC2_BRAIN_RUNTIME` (default OFF). No new gate needed. The APPLY behavior is part of the `MC2_BRAIN_RUNTIME` gate's contract (1A was compute+trace-only; 1B adds apply). Add a sub-gate `MC2_BRAIN_RUNTIME_APPLY` (default OFF within `MC2_BRAIN_RUNTIME=1`) if the reviewer wants extra bisectability — recommended, because it separates "mode is read from file" from "orders are actually written", making regressions easier to bisect:

- `MC2_BRAIN_RUNTIME=1, MC2_BRAIN_RUNTIME_APPLY=0`: mode loaded, ABL short-circuited for Enhanced, but no slot writes (trace only — extends 1A behavior to per-warrior modes from file)
- `MC2_BRAIN_RUNTIME=1, MC2_BRAIN_RUNTIME_APPLY=1`: full apply — HOLD_TASK drain writes tac-order slots

### §6.3 — Relink

**YES — full relink required.** `MechBrainRuntime` gains a new field (`initialHoldPushed`), and `BrainTaskEntry` gains a `type` field. Both are members of structs instantiated via `new` in warrior.cpp; layout changes require relink of all TUs that include these headers.

### §6.4 — Acceptance (verbatim)

| Check | Acceptance |
|---|---|
| Smoke gate OFF (mc2_01, mc2_10, mc2_24, 30s each) | PASS 3/3 — no `[BRAIN_RT]` lines, behavior byte-identical to baseline |
| Smoke gate ON + APPLY=0, no fixture | PASS 3/3 — mode loads (default Legacy), no apply, no crash |
| Smoke gate ON + APPLY=1, mc2_01_ai.fit present | mc2_01 PASS (no crash), log contains `[BRAIN_RT] HOLD_TASK applied wid=<N>` |
| Smoke gate ON + APPLY=1, `MC2_BRAIN_RUNTIME_FORCE_MODE=legacy` | PASS — forced-Legacy override suppresses apply for all warriors |
| Gate ON + fixture, mc2_10 / mc2_24 (no `_ai.fit`) | PASS — no regression; all warriors Legacy |
| `check-brain-fit-schema.py` on mc2_01_ai.fit | PASS (0 FAIL) |
| Full relink confirmed | YES — delete mc2.exe + .obj for warrior.cpp / mission.cpp |

### §6.5 — What stays deferred

| Feature | Slice |
|---|---|
| REQUEST_ORDERS task, commander chain, `receiveOrdersRequest()` | OPORD-SLOT-RUNTIME-1 |
| OPORD slot completion detection, slot advancement | OPORD-SLOT-RUNTIME-1 |
| Primary/Secondary/Tertiary OPORD data loading from `mission_ai.fit` | OPORD-SLOT-RUNTIME-1 |
| ALARM_REACTION task type (non-stub) | ALARM-INTAKE-1 |
| BrainSpecial DO-command VM, verb dispatch | TECHSCRIPT-SPECIAL-DISPATCH-1 |
| `Var.Set scope=Mission` variable store | TECHSCRIPT-SPECIAL-DISPATCH-1 |
| Tactic weight loading + modulation | TACTIC-WEIGHT-SELECT-1 |
| Archetype `.fit` resolver | BRAIN-ARCHETYPE-FIT-1 |
| Inline Brain block in mission.fit (modder compat) | BRAIN-MISSIONFIT-INLINE-1 (new slice TBD) |
| Save/load of brain runtime state | BRAIN-SAVELOAD-1 |
| `ORDER_ORIGIN_BRAIN` enum value (if ABI-risky) | Same slice or deferred if save/load concern |
| Editor brain panel | EDITOR-BRAIN-PANEL-1 |

---

## §7 — OPEN ITEMS / RISKS

### Risk 1: `FitIniFile` block iteration for multiple Brain blocks

The `FitIniFile` API (`mclib/inifile.cpp`) uses `seekBlock(name)` which finds the FIRST block with that name. If `mission_ai.fit` has multiple `Brain { }` blocks, iterating them requires either: (a) using a fallback `seekBlock` with a loop pattern (check if the existing API supports repeated `seekBlock` calls to find successive same-name blocks — this is unconfirmed; grep `mclib/inifile.cpp` for `seekBlock` implementation before coding), or (b) number-suffix the blocks (`Brain1 { }`, `Brain2 { }`, etc.) as the existing `loadBrainParameters` does (`Warrior%d`). **Recommended for 1B:** use numbered blocks `Brain1 { }`, `Brain2 { }` matching the `Warrior%d` convention, to avoid the multi-same-name-block question entirely. The checker (`check-brain-fit-schema.py`) already handles unnumbered `Brain { }` blocks — it would need a minor update for numbered variant, OR keep unnumbered but use a simpler single-warrior fixture for 1B (one Brain block per file is unambiguous with any `seekBlock` implementation).

**Simplest for 1B:** one Brain block in the fixture file (Warrior4 only), so `seekBlock("Brain")` finds it unambiguously. Multi-warrior `mission_ai.fit` support is deferred to OPORD-SLOT-RUNTIME-1 when the full loader is built.

### Risk 2: `setGeneralTacOrder` clobber on next ABL tick

If `MC2_BRAIN_RUNTIME_APPLY=1` and the Enhanced warrior's ABL brain is not actually short-circuited (coding error), ABL will overwrite the Brain-applied HOLD order on the next cadence tick. The trace log will still show `[BRAIN_RT] HOLD_TASK applied` but the unit will resume patrol. Test: check that Warrior4 does NOT emit any patrol/move behavior in the mc2_01 30s smoke log. The ABL short-circuit at `runBrain():2204` is the guard; if it fires correctly, this is not a risk.

### Risk 3: `ORDER_ORIGIN_BRAIN` enum value

`OrderOriginType` is defined in `tacordr.h` (confirmed: `TacticalOrder::origin` field at `tacordr.h:222`). Check if this enum is serialized to save/load before adding a new value. If it is serialized, adding `ORDER_ORIGIN_BRAIN` after the existing values is safe (ordinals preserved); adding it in the middle would break save compatibility. For 1B, use `ORDER_ORIGIN_PLAYER` as a temporary stand-in and file a TODO. The distinction does not affect behavior in 1B (no save/load of brain runtime state).

### Risk 4: `warriorList` array bounds

`mission.cpp:2947` iterates `i` from `1` to `numWarriors` inclusive; `warriorList[i]` is 1-based. The `loadMissionAIFit` loader must clamp `idx` to `[1, MechWarrior::numWarriors]` and null-check `warriorList[idx]` before use. The existing `loadBrainParameters` at `warrior.cpp:7318` uses `seekBlock("Warrior%d", warriorId)` with the same convention — follow the same pattern.

---

## §8 — SUMMARY

**1B is the first behavior-changing slice.** It adds:
1. Per-warrior mode loading from `mission_ai.fit` (unitRef → Warrior%d index; mode= "Legacy"/"Hybrid"/"Enhanced")
2. ABL short-circuit for Enhanced warriors inside `runBrain()` (skip `brain->execute()` + goal planner; keep `clearAlarmsHistory()` + globals clear)
3. Apply drain: `HOLD_TASK` → `setGeneralTacOrder(TACTICAL_ORDER_STOP)` for Enhanced, `setPlayerTacOrder` for Hybrid
4. One acceptance fixture (`mc2_01_ai.fit`, Warrior4 Enhanced, HOLD) + trace-log delta signal

Gate: `MC2_BRAIN_RUNTIME=1 MC2_BRAIN_RUNTIME_APPLY=1` (both required; APPLY is the new sub-gate that separates 1A from 1B behavior).  
Relink: YES.  
Smoke: tier1 5/5 gate-OFF + gate-ON/APPLY-OFF/no-fixture + gate-ON/APPLY-ON/fixture (mc2_01) + mc2_10 + mc2_24 regression.  
Deferred: REQUEST_ORDERS, OPORD slots, BrainSpecial VM, variable store, archetype resolver, save/load.
