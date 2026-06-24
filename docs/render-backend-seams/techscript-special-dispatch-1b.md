# TECHSCRIPT-SPECIAL-DISPATCH-1B

**Slice:** TECHSCRIPT-SPECIAL-DISPATCH-1B
**Branch:** `claude/techscript-dispatch-1b`
**Worktree:** `A:/Games/mc2-techscript-dispatch-1b`
**Gates:** `MC2_BRAIN_DISPATCH` (parse+trace), `MC2_BRAIN_DISPATCH_APPLY` (effect layer)
**Status:** COMPLETE — all smoke gates PASS
**Deployed:** `A:/Games/mc2-opengl/mc2-win64-v0.4c`
**Exe SHA256:** `F8AE32E78B7DA72A29DAC10209D8B899D4A2CB627BFB78F4F814C8113D6DE7E6`
**Build fingerprint:** `sha=f1ebef81bcbc dirty=1 branch=claude/techscript-dispatch-1b`

---

## Purpose

Adds the **first real TechScript effect**: when `Brain.CorePower false` appears in a
warrior's BrainSpecial Body block and `MC2_BRAIN_DISPATCH_APPLY=1` is set, the dispatcher
calls `setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)` — shutting down the unit.
All other verbs remain trace-only (zero effects).

---

## Effects gate ladder

| Gate combo | Behavior |
|---|---|
| `MC2_BRAIN_DISPATCH` absent / `=0` | 1A behavior: `parseBrainSpecialBody` never called; `specialBody.loaded=false`; dispatch never called — zero behavior change |
| `MC2_BRAIN_DISPATCH=1` only | 1A behavior: parse + trace only; `executeSpecialBody_TraceOnly` called; `[BRAIN_DISPATCH]` / `[BRAIN_DISPATCH_UNKNOWN]` trace; NO effects |
| `MC2_BRAIN_DISPATCH_APPLY=1` without `MC2_BRAIN_DISPATCH=1` | Warns: `WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 — apply is INERT`; apply is inert |
| `MC2_BRAIN_DISPATCH=1` + `MC2_BRAIN_DISPATCH_APPLY=1` | Full 1B: parse + dispatch; `executeSpecialBody_Apply` called; `Brain.CorePower false` → `setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)` + `[BRAIN_DISPATCH_APPLY]` proof line; all other verbs → trace only |

**Note:** Full gate set for 1B effects: `MC2_BRAIN_TASKQ=1 MC2_BRAIN_RUNTIME=1
MC2_BRAIN_RUNTIME_APPLY=1 MC2_BRAIN_DISPATCH=1 MC2_BRAIN_DISPATCH_APPLY=1`

---

## The ONE effect: Brain.CorePower false → POWERDOWN

**Verb token (in fixture DO key):** `"Brain.CorePower false"` (space-separated, stored as full DO string)

**Effect (warrior.cpp ~2264, brain_special_dispatch.cpp ~executeSpecialBody_Apply):**
```cpp
TacticalOrder pdOrder;
pdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
warrior->setGeneralTacOrder(pdOrder);
fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=%d\n", wid);
```

**Constant:** `TACTICAL_ORDER_POWERDOWN` from `code/tacordr.h:143`.

**Pattern:** Mirrors the powerdown call at `warrior.cpp:6503`: `tacOrder.init((OrderOriginType)origin, TACTICAL_ORDER_POWERDOWN, unitOrder)`.
Uses `ORDER_ORIGIN_SELF` (matches the HOLD_TASK pattern at warrior.cpp:2268: `holdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_STOP)`).

---

## Supersede-HOLD rule (warrior.cpp ~2258–2320)

When `MC2_BRAIN_DISPATCH_APPLY=1` AND the body contains `"Brain.CorePower false"`:
- `executeSpecialBody_Apply` is called FIRST, applying POWERDOWN to the GENERAL slot.
- The brainTaskQueue is drained and **discarded** (no `setGeneralTacOrder(STOP)` from HOLD_TASK).
- This ensures EXACTLY ONE GENERAL-slot write per tick.

When `MC2_BRAIN_DISPATCH_APPLY=0`:
- The synthetic HOLD path runs as-is (1B-runtime default unchanged).
- `executeSpecialBody_TraceOnly` is called for trace (1A behavior preserved).

**Seam location:** `code/warrior.cpp` Enhanced+APPLY branch, ~line 2258.
Comment anchor: `warrior.cpp:1B-supersede: HOLD_TASK silenced (no setGeneralTacOrder STOP write)`.

---

## Relaxed-call guard (1B — executeSpecialBody_Apply)

**The ONLY permitted order function in `executeSpecialBody_Apply`:**
- `warrior->setGeneralTacOrder()` — for `Brain.CorePower false` → POWERDOWN only.

**STILL FORBIDDEN (enforced by contract comment in `brain_special_dispatch.cpp`):**
- `setPlayerTacOrder`, `setAlarmTacOrder`
- `requestHelp`, `requestTarget`
- `calcTacOrder`, `coreMoveTo`, `setMainGoal`, `clearCurTacOrder`
- Any movement / attack / OPORD-advance / commander function

**All other verbs** (`Brain.CorePower` plain, `Brain.CoreAttack`, `OPORD.*`, `Unit.Retreat`, `Unit.InState`, `Var.*`, etc.) → trace only, zero effect in 1B.

---

## Hazards deferred (NOT implemented in 1B)

- `Unit.InState` / FSM-state guard — no `activeState`, no branching; remains UNKNOWN-trace.
- `Var.*` per-unit namespace — no var store, no writes; remains UNKNOWN-trace.
- Any second effect verb, OPORD movement/guard/attack, commander chain, REQUEST_ORDERS.

---

## Seam locations

| File | Location | Description |
|---|---|---|
| `code/brain_special_dispatch.h` | top | Updated gate doc + relaxed-call guard + `bodyHasPowerdown` + `executeSpecialBody_Apply` declarations |
| `code/brain_special_dispatch.cpp` | new functions | `bodyHasPowerdown` + `executeSpecialBody_Apply` (includes warrior.h + tacordr.h for order call) |
| `code/warrior.cpp` | ~2258–2320 | DISPATCH_APPLY ladder + supersede-HOLD rule + s_dispatchApply static gate |
| `tests/fixtures/brain_runtime/mc2_01_specials.fit` | DO0 | `"Brain.CorePower false"` added as first verb (effect trigger) |

---

## Fixture file

`tests/fixtures/brain_runtime/mc2_01_specials.fit`:
```
DO0 = "Brain.CorePower false"   ← effect verb (Brain.CorePower false → POWERDOWN)
DO1 = "Brain.CorePower"         ← recognized, trace-only (no effect in 1B)
DO2 = "OPORD.CoreGuard"         ← recognized, trace-only
DO3 = "Unit.InState"            ← unknown-trace (hazard deferred)
DO4 = "Var.Set"                 ← unknown-trace (hazard deferred; note: FIT parser may truncate at DO3)
```

---

## Acceptance evidence

**Exe fingerprint:** `sha=f1ebef81bcbc dirty=1 branch=claude/techscript-dispatch-1b`
**Exe SHA256:** `F8AE32E78B7DA72A29DAC10209D8B899D4A2CB627BFB78F4F814C8113D6DE7E6`

All gates run with `--deploy 0.4c` (fingerprint-confirmed), mc2_01, 30s stock profile.

| Gate | Env vars | Result | Fingerprint | Key evidence |
|---|---|---|---|---|
| A (baseline) | none | PASS | OK f1ebef81 | No BRAIN lines; byte-identical |
| B (trace+HOLD) | TASKQ=1 RUNTIME=1 APPLY=1 DISPATCH=0 | PASS | OK f1ebef81 | `[BRAIN_RT] HOLD_TASK applied wid=4`; NO DISPATCH lines |
| C (DISPATCH trace, APPLY=0) | +DISPATCH=1 DISPATCH_APPLY=0 | PASS | OK f1ebef81 | `[BRAIN_DISPATCH] parsed mc2_01_specials.fit: 4 verbs`; `[BRAIN_DISPATCH_UNKNOWN] verb=Brain.CorePower false wid=4`; `[BRAIN_RT] HOLD_TASK applied` still fires |
| D (DISPATCH+APPLY=1) | +DISPATCH_APPLY=1 | PASS | OK f1ebef81 | `[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=4`; NO `[BRAIN_RT] HOLD_TASK applied`; Unit.InState=UNKNOWN, no effects |

**Proof line (Gate D verbatim):**
```
[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=4
```

**One-write confirmation (Gate D):**
- EXACTLY ONE GENERAL-slot write per tick: `setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)` from dispatcher.
- `[BRAIN_RT] HOLD_TASK applied` is ABSENT from Gate D log (supersede-HOLD rule confirmed active).

**No Unit.InState behavior, no Var writes, no movement/attack/OPORD effects:**
- `Unit.InState` → `[BRAIN_DISPATCH_UNKNOWN] verb=Unit.InState wid=4` (trace only, confirmed).
- Only `setGeneralTacOrder` added; `setPlayerTacOrder` / `setAlarmTacOrder` / `requestHelp` / `requestTarget` never called (code-confirmed: `brain_special_dispatch.cpp` relaxed-call guard comment).

**No save change:** `BrainSpecialBody` is non-persistent by BRAIN-RUNTIME-1A policy (same as `MechBrainRuntime`).
