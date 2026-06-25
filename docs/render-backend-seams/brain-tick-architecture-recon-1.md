# BRAIN-TICK-ARCHITECTURE-RECON-1

> Read-only recon. No code changed. 2026-06-24.
> Purpose: map all Brain/AI dependencies on frame timing and direct world-mutation
> so later slices (BRAIN-FIXED-TICK-1, BRAIN-DECISION-INTENT-QUEUE-1) can proceed safely.

---

## 1. runBrain Cadence Analysis

### How timing works today

`brainUpdate` is a `float` field on `MechWarrior` (warrior.h:731). It is initialised at
warrior birth (`warrior.cpp:983`):

```cpp
brainUpdate = (float)(numWarriors % 30) * 0.2;
```

This staggers warriors across a 0–5.8 s window in 0.2 s steps based on spawn order
(`numWarriors` at birth). It is a **sim-time offset**, not a render-frame offset.

The gate in `updateActions` (`warrior.cpp:5215`):

```cpp
if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
    runBrain();
    brainUpdate += BrainUpdateFrequency;   // warrior.cpp:5262
}
```

`BrainUpdateFrequency` is a global float initialised to **2.25 s** (`warrior.cpp:269`),
loaded from CSV at mission start via `mover.cpp` `readIdFloat`.

**VERDICT: cadence is fully sim-time-based.** `brainUpdate` is compared against
`scenarioTime`, which is the game-world elapsed seconds (`mission.cpp:716/729`). It does
not use render dt (`frameLength`), frame counter, or wall-clock. BRAIN-FIXED-TICK-1 can
keep the `brainUpdate <= scenarioTime` gate unchanged and simply change what runs inside it.

### What BRAIN-FIXED-TICK-1 changes vs. keeps

| Aspect | Current | BRAIN-FIXED-TICK-1 |
|---|---|---|
| Gate condition | `brainUpdate <= scenarioTime` | Keep as-is |
| Stagger init | `numWarriors % 30 * 0.2` | Keep as-is |
| Tick interval | `BrainUpdateFrequency` (2.25 s) | Replace with fixed constant (5–10 Hz = 0.1–0.2 s) |
| What runs inside | `brain->execute()` + `calcTacOrder` → immediate mutations | Fixed-tick: read snapshot → decide → enqueue INTENT |
| `brainUpdate` advance | `+= BrainUpdateFrequency` | `+= fixedTickInterval` |

`scenarioTime` is advanced in `mission.cpp` via `timeGetTime()` delta converted to
seconds — a pure sim clock, not render-frame-derived. No decoupling needed for the clock.

---

## 2. Dependency Inventory Table

### 2a. Time reads in the brain path

| Surface | File:line | Current behaviour | Classification | Notes |
|---|---|---|---|---|
| `scenarioTime` gate | warrior.cpp:5215 | tick enable gate | **safe for snapshot** — already sim-time | Keep: compare brainTickTime <= simTime |
| `brainUpdate += BrainUpdateFrequency` | warrior.cpp:5262 | advance next-tick time | **safe for snapshot** | BRAIN-FIXED-TICK-1: replace freq constant |
| `scenarioTime` in `MoveOrders::init` | warrior.cpp:1272 | timestamps move order | **needs serial commit** | stamp written at order creation, not at decision |
| `scenarioTime` in `AttackOrders::init` | warrior.cpp:1328 | timestamps attack order | **needs serial commit** | same |
| `scenarioTime` in radio cooldown checks | warrior.cpp:1376,1378,1381,1383,1395,1398 | rate-limiting radio messages | **main-thread-only API** | radio is side-effect; keep main-thread |
| `scenarioTime` in `getAttackersInTime` | warrior.cpp:2494 | query recent attackers | **safe for snapshot** — read-only query | snapshot field: attacker history |
| `frameLength` exposed to ABL | ablmc2.cpp:5970 (`execGetCameraFrameLength`) | ABL script can read last render frame length | **nondeterministic hazard** | ABL scripts calling `GetCameraFrameLength` couple brain decisions to render dt |
| `time(NULL)` | warrior.cpp:420 (`SeedRandomNumbers`) | seeds global RNG at mission start | **nondeterministic hazard** | one-shot, not per-tick; acceptable if seed fixed for determinism |
| `lastTargetTime == scenarioTime` | warrior.cpp:5268 | same-frame target recalc trigger | **needs serial commit** | written by `setTargetObject` in runBrain, read immediately after |

### 2b. Direct world-mutations from brain code

**Already through dispatch seam (brain_special_dispatch.cpp executeSpecialBody_Apply):**

| Mutation | Dispatch verb | Status |
|---|---|---|
| `setGeneralTacOrder(POWERDOWN)` | Brain.CorePower false | DISPATCH seam (1B) |
| `setGeneralTacOrder(EJECT)` | Unit.Eject / coreEject | DISPATCH seam |
| `setGeneralTacOrder(GUARD)` | OPORD.CoreGuard / coreGuard | DISPATCH seam |
| `setGeneralTacOrder(MOVETO_POINT)` | OPORD.CoreMoveTo | DISPATCH seam |
| `setGeneralTacOrder(ATTACK_OBJECT)` | OPORD.CoreAttack | DISPATCH seam |
| `setGeneralTacOrder(WITHDRAW)` | Unit.Retreat / coreRetreat | DISPATCH seam |

Six verbs. All in `executeSpecialBody_Apply` (brain_special_dispatch.cpp:491+), gated
by `MC2_BRAIN_DISPATCH=1 MC2_BRAIN_DISPATCH_APPLY=1`. These are the natural intent-queue
candidates — they are already logically separated from execution.

**Legacy ABL-direct (NOT through dispatch seam):**

| Mutation | File:line | Path | Notes |
|---|---|---|---|
| `setGeneralTacOrder` (calcTacOrder result) | warrior.cpp:2237 | `runBrain` → `calcTacOrder` → write | mainline legacy path; all non-Enhanced warriors hit this |
| `setPlayerTacOrder` (calcTacOrder result) | warrior.cpp:2235 | same, home commander path | player-owned units |
| `setPlayerTacOrder` (queued tac order) | warrior.cpp:1781 | `updateActions` drain queue | separate from ABL; already has a tiny queue |
| `setAlarmTacOrder` | warrior.cpp:3237, 3297 | movement decision tree (escape path) | triggered by movement failure alarms |
| `setMainGoal` | warrior.cpp:2241 | on `calcTacOrder` failure/cancel | clears goal on error |
| `clearCurTacOrder` | warrior.cpp:2243 | on `calcTacOrder` error | paired with setMainGoal |
| `setGeneralTacOrder(HOLD)` | warrior.cpp:2360 | BRAIN-RUNTIME-1B Enhanced path | synthetic hold via task queue |
| `orderAttackObject` | ablmc2.cpp:1904, 1946 | ABL native function | direct object-pointer attack order; NOT through dispatch |
| `clearCurTacOrder` | ablmc2.cpp:7507 | ABL native function | direct clear |
| `setMainGoal` | ablmc2.cpp:7597 | ABL native function | direct goal override |

**Global mutable state written during brain tick:**

| Surface | File | Notes |
|---|---|---|
| `CurGroup`, `CurObject`, `CurObjectClass`, `CurWarrior`, `CurContact` | warrior.cpp:2204-2208 | Process-global ABL context; written at runBrain entry, cleared at exit — NOT thread-safe |
| `CurAlarm` | warrior.cpp:5027 | Per-alarm execution context global |
| `brainRuntime->initialHoldPushed` | warrior.cpp:2260 | Per-warrior; safe if one warrior at a time |
| `brainRuntime->dispatchEffectApplied` etc. | mech_brain_runtime.h:101-106 | Per-warrior once-guards |

---

## 3. Mutation Seam Inventory

### Already through dispatch seam
The 6 dispatch verbs listed in §2b are the foundation for BRAIN-DECISION-INTENT-QUEUE-1.
`executeSpecialBody_Apply` is the natural intent-emit point: change it to push an
`IntentEntry` instead of calling `setGeneralTacOrder` directly.

### Legacy ABL direct (work for BRAIN-DECISION-INTENT-QUEUE-1)
The big gap: `brain->execute()` (ABL interpreter) runs entire scripts synchronously and
calls C++ native functions that mutate tac-order slots directly via `setGeneralTacOrder`,
`setPlayerTacOrder`, `setAlarmTacOrder`, `orderAttackObject`, etc. These live in
`ablmc2.cpp` and are not interceptable without either:
1. Intercepting at the `setGeneralTacOrder` call site (per-warrior serial-commit model), or
2. Routing all ABL native order functions through an intent buffer before the interpreter runs.

Option 1 is feasible: `setGeneralTacOrder` writes to `tacOrder[ORDERSTATE_GENERAL]`
(warrior.cpp:4717). If that field becomes a "pending intent slot" rather than live state,
the existing call sites become intent-pushes without touching ABL.

---

## 4. Snapshot Field Shortlist (6 Effect Verbs)

Minimal read-only fields actually needed by POWERDOWN / EJECT / GUARD / MOVETO / ATTACK / RETREAT:

| Field | Type | Used by | Source |
|---|---|---|---|
| `vehicleWID` | int32_t | All verbs (warrior identity) | MechWarrior member |
| `teamId` | int32_t | All verbs (brainsEnabled gate) | MechWarrior member |
| `isDisabled()` | bool | POWERDOWN, EJECT guard | via getVehicle() |
| `getCommander() == Commander::home` | bool | MOVETO/ATTACK routing (player vs general slot) | MechWarrior::getCommander |
| `mainGoalObjectWID` | int32_t | ATTACK: target watch-ID | MechWarrior member |
| `mainGoalLocation` | Vector3D | MOVETO: destination | MechWarrior member |
| `mainGoalControlRadius` | float | MOVETO: radius | MechWarrior member |
| `attackOrders.targetWID` | int32_t | ATTACK current target | MechWarrior member |
| `brainRuntime->*EffectApplied` | uint8_t x6 | Once-guard per verb | MechBrainRuntime inline |

That is 9 fields. All are per-warrior (no shared state). All are readable without
acquiring any mutex (they live in the MechWarrior struct). The snapshot can be a
plain POD struct copied at tick entry — no pointer following needed for the 6 verbs.

**Fields NOT needed by the 6 verbs (defer to later slices):**
- Full contact list (ATTACK against sensor contacts needs this — BRAIN-SNAPSHOT-2)
- Weapon status (ATTACK damage computation — BRAIN-SNAPSHOT-3)
- Group/team member lists (coordination verbs — BRAIN-SNAPSHOT-4)

---

## 5. Nondeterminism Hazard List

Ranked by threading-kill severity:

### HAZARD-1: `CurGroup / CurObject / CurWarrior / CurContact / CurAlarm` — PROCESS GLOBALS
**File:** warrior.cpp:2204-2208, 5027
**Severity: THREADING KILLER.**
ABL execution is driven by these 5 globals. Every ABL native function dereferences them.
Running two warriors' brains concurrently would race on these globals.
**Mitigation:** For same-thread fixed tick this is not a problem (one warrior at a time).
For future worker-thread dispatch, these must be replaced with per-worker thread-locals or
passed as an explicit context struct.

### HAZARD-2: `frameLength` exposed via `execGetCameraFrameLength` — RENDER-DT IN DECISION
**File:** ablmc2.cpp:5955-5970
**Severity: CORRECTNESS HAZARD.**
Any ABL script calling `GetCameraFrameLength` couples a brain decision to render dt.
If brain tick runs at a fixed rate decoupled from render, `frameLength` will be stale.
**Mitigation:** In BRAIN-FIXED-TICK-1 replace with `brainTickDt` (fixed constant).
Grep for `GetCameraFrameLength` in `.abl` scripts to audit actual usage.

### HAZARD-3: `srand(time(NULL))` — WALL-CLOCK RNG SEED
**File:** warrior.cpp:418-421
**Severity: DETERMINISM HAZARD (offline harness / replay).**
Mission start seeds the global C RNG from wall clock. All random decisions in ABL
(e.g., `ABLi_popRandom` etc.) are non-reproducible across runs.
**Mitigation:** For offline harness / deterministic replay, seed from mission seed.
Not a threading hazard; tolerable for live gameplay.

### HAZARD-4: `ObjectManager->get(id)` / `getByWatchID` — LIVE POINTER DEREF IN BRAIN
**File:** warrior.cpp throughout; ablmc2.cpp:228, 368, 612, 692, 1903, etc.
**Severity: SNAPSHOT BOUNDARY HAZARD.**
Brain code dereferences live game-object pointers. Objects can be destroyed mid-frame
(UAF risk on worker threads). On main thread this is safe today only because the brain
runs inside the game-update loop before object destruction. Any future worker offload
must snapshot all reachable WIDs → stable pointers before dispatch.

### HAZARD-5: `combatDecisionTree()` / `movementDecisionTree()` — UNGUARDED RATE LOGIC
**File:** warrior.cpp:5121-5124
**Severity: SEQUENCING HAZARD.**
`combatDecisionTree` (`combatUpdate <= scenarioTime`) and `movementDecisionTree` run
inside `updateActions` every frame, interleaved with the brain tick. Both read the same
`tacOrder` slots that `runBrain` writes. A strict "snapshot → decide → commit" model
must account for these two paths — they also produce tac-order mutations.

---

## 6. FRAME-JOBS Precedent

The rendering lane's FRAME-JOBS-2D pattern (`appear.h:306-315`, `bdactor.cpp`,
`frame_jobs.h`) establishes the produce-off-thread/commit-serial model:

```
Phase 1 (worker threads):  touchWorkerPrepass()  — lock-free, reads cached state only
                                                    MUST NOT: acquire mutex, call txmmgr,
                                                    emit GL, touch shared state
Phase 2 (main thread):     touchSerialCommit()   — light-data resubmit, mutex OK
                           g_isFrameJobsWorker flag on worker TLS to catch violations
```

The epoch model: `touchSerialCommitFrame` is a `uint32_t` stamped with `g_mc2FrameCounter`
at commit entry. Path B (terrain loop re-entry) checks the stamp and skips if already
processed this frame. This prevents double-processing without a mutex.

**What Brain can copy:**
1. **Snapshot at tick entry:** copy per-warrior POD snapshot before `runBrain`. Same
   discipline as `touchWorkerPrepass` reading cached state.
2. **Intent buffer:** `runBrain` writes to per-warrior `BrainIntentSlot[3]` (GENERAL/
   PLAYER/ALARM) instead of directly to `tacOrder[]`. Same discipline as `touchWorkerPrepass`
   writing to per-instance state only.
3. **Serial commit:** after all warriors run, a commit pass copies `BrainIntentSlot` to
   `tacOrder[]`. Same as `touchSerialCommit`.
4. **Epoch guard:** stamp `brainTickFrame = g_mc2FrameCounter` at commit so alarm
   re-entry inside `checkAlarms` can detect double-commit.
5. **Worker-thread guard:** extend `g_isFrameJobsWorker` / `isFrameJobsWorkerThread()`
   to `g_isBrainWorker` when brain workers land.

The `BrainTaskQueue` (brain_task_queue.h) is already shaped as a produce→drain queue with
deterministic sort (tier → frame_ms → stable_seq_id → warrior_id). It is NOT yet an
intent queue (only holds HOLD_TASK stub), but its data model is correct:
`insertion_frame_ms = (uint32_t)(scenarioTime*1000+0.5)` — sim-time stamp, not render frame.
It lacks a "what to commit" payload beyond task type.

---

## 7. Risk and Sequencing Notes

### BRAIN-FIXED-TICK-1 (same-thread, no snapshot yet) — SAFE to proceed
- Change `BrainUpdateFrequency` to a fixed 0.1–0.2 s constant (or per-gate env var).
- Run `runBrain()` at that cadence on main thread, same as today.
- No threading. No snapshot. No intent queue.
- Risk: some ABL scripts call `GetCameraFrameLength` — audit first.

### What is blocked until snapshot exists
- Parallel brain workers (blocked by HAZARD-1: `CurWarrior` etc. globals).
- Deterministic offline harness (blocked by HAZARD-3: RNG seed, and HAZARD-4: live ptrs).
- True decoupling from `combatDecisionTree`/`movementDecisionTree` (HAZARD-5).

### Landmines
1. **`lastTargetTime == scenarioTime` read-after-write** (warrior.cpp:5268):
   `runBrain` sets `lastTargetWID`/`lastTargetTime` inside `setTargetObject`; the check
   immediately after (`if (lastTargetTime == scenarioTime)`) is a same-tick side-effect
   dependency. This must not be split across tick boundaries.
2. **Alarm callbacks call `brain->execute`** (warrior.cpp:5028, 5034) with a different
   entry point. These run inside `checkAlarms()` which is called after `runBrain`.
   Any intent-queue model must account for alarm-triggered brain runs independently
   of the main-cadence run.
3. **Enhanced mode HOLD suppression** (warrior.cpp:2344-2347): the dispatcher actively
   suppresses queued HOLD_TASKs when a dispatch verb applies. This "exactly one GENERAL-
   slot write per tick" invariant must be preserved in the intent-commit model.
4. **`setUseGoalPlan` is called every frame** (warrior.cpp:2227) unconditionally inside
   the ABL path. If `brain->execute()` is skipped (Enhanced mode), this also does not
   run. Goal-plan state may diverge. Audit before BRAIN-FIXED-TICK-1.

---

## 8. VERDICT

**GO for BRAIN-FIXED-TICK-1.**

The cadence gate is already pure sim-time — no render-dt coupling to remove. The stagger
is already correct. The only change is the tick interval constant and any future inner-body
refactor. Same-thread execution changes nothing about threading safety.

**The single most important thing to get right in BRAIN-FIXED-TICK-1:**
> Audit `GetCameraFrameLength` usage in `.abl` scripts BEFORE changing the tick rate.
> If any script uses it for movement timing (e.g., "move for N frames"), halving or
> tripling the tick rate will silently break those decisions. Replace the ABL native
> with a `brainTickDt` constant (the fixed interval in seconds) before the rate changes.

---

## Appendix: Key Constants

| Constant | Value | File |
|---|---|---|
| `BrainUpdateFrequency` | 2.25 s | warrior.cpp:269 |
| `MovementUpdateFrequency` | 5.0 s | warrior.cpp:270 |
| `CombatUpdateFrequency` | 0.25 s | warrior.cpp:271 |
| `CommandUpdateFrequency` | 6.0 s | warrior.cpp:272 |
| `stagger init` | `(numWarriors % 30) * 0.2` s | warrior.cpp:983 |
| `BrainTaskQueue::MAX_BRAIN_TASKS` | 8 | brain_task_queue.h:32 |
| `MechBrainRuntime::kVarStoreCap` | 32 | mech_brain_runtime.h:33 |
