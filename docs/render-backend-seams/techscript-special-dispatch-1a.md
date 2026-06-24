# TECHSCRIPT-SPECIAL-DISPATCH-1A

**Slice:** TECHSCRIPT-SPECIAL-DISPATCH-1A  
**Branch:** `claude/techscript-dispatch-1a`  
**Worktree:** `A:/Games/mc2-techscript-dispatch-1a`  
**Gate:** `MC2_BRAIN_DISPATCH` (default OFF)  
**Status:** SHIPPED — all smoke gates PASS  
**Deployed:** `A:/Games/mc2-opengl/mc2-win64-v0.4c`  
**Exe SHA256:** `32461BA30D2A9DFA5681FE7997BDB94462703CC26F1DAF6EA576BF2959857785`  

---

## Purpose

Adds a **trace-only** BrainSpecial dispatch shell that parses per-mission `_specials.fit`
files and logs DO-verb tokens to stderr — no tac-order writes, no state mutations.
This is the minimum seam for future 1B/DISPATCH-1 active dispatch.

---

## Trace-only contract

`executeSpecialBody_TraceOnly` calls **only**:
- `std::fprintf(stderr, ...)` / `std::fflush(stderr)`
- Loop + string comparison via `isRecognizedVerb()`

**Forbidden-call list** — this function MUST NOT call any of:
- `setGeneralTacOrder`, `setPlayerTacOrder`, `setAlarmTacOrder`
- `requestHelp`, `requestTarget`
- `clearCurTacOrder`, `setMainGoal`, `calcTacOrder`, `coreMoveTo`
- Any movement / attack / order function

Enforced by code inspection and compile-time comment contract in the TU.
No warrior pointer, no MechWarrior type, no ABL integration exists in `brain_special_dispatch.cpp`.

---

## Seam locations

| File | Line | Description |
|---|---|---|
| `code/mission.cpp` | ~3101–3113 | Mission-load gate: if `MC2_BRAIN_DISPATCH=1`, iterates warriors with allocated `brainRuntime` and calls `parseBrainSpecialBody(missionName, w->getBrainRuntime()->specialBody)` |
| `code/warrior.cpp` | ~2277–2279 | Per-warrior per-frame: if `brainRuntime->specialBody.loaded`, calls `executeSpecialBody_TraceOnly(brainRuntime->specialBody, vehicleWID)` |
| `code/brain_special_dispatch.h` | 1–29 | Public API + forbidden-call guard comment |
| `code/brain_special_dispatch.cpp` | 1–121 | Implementation: `parseBrainSpecialBody` + `executeSpecialBody_TraceOnly` |
| `code/mech_brain_runtime.h` | 35 | `BrainSpecialBody specialBody` member added to `MechBrainRuntime` |
| `CMakeLists.txt` | ~346 | `code/brain_special_dispatch.cpp` added to mc2 source list |

---

## Recognized verbs (1A set)

From recon of stock BrainSpecial Body blocks:
- `Brain.CorePower`
- `Brain.CoreAttack`
- `OPORD.CoreGuard`
- `OPORD.CorePatrol`
- `OPORD.CoreMoveTo`
- `Unit.Retreat`
- `HOLD`

All other verbs produce `[BRAIN_DISPATCH_UNKNOWN]` trace.

---

## Fixture file

`tests/fixtures/brain_runtime/mc2_01_specials.fit` — minimal BrainSpecial block with 4 verbs:
- `Brain.CorePower` (recognized)
- `OPORD.CoreGuard` (recognized)
- `Unit.InState` (unknown — exercises the UNKNOWN trace path)
- `Var.Set` (unknown)

---

## Gate behavior

| Gate combo | Behavior |
|---|---|
| `MC2_BRAIN_DISPATCH` absent / `=0` | `parseBrainSpecialBody` never called; `specialBody.loaded=false`; `executeSpecialBody_TraceOnly` never called — zero behavior change |
| `MC2_BRAIN_DISPATCH=1` | Mission-load parses `_specials.fit` for each warrior with `brainRuntime`; per-frame trace fires for each warrior with loaded verbs |
| `MC2_BRAIN_DISPATCH=1` + file absent | Silent (file open returns non-NO_ERR); warrior's `specialBody.loaded` stays false |

---

## Acceptance evidence

All three smoke gates run on `mc2_01`, 30s, stock profile, `mc2-win64-v0.4c`.

| Run | Gates | Result | Frames | Avg FPS |
|---|---|---|---|---|
| A | OFF (baseline) | PASS | 2467 | 82 |
| B | RUNTIME=1 APPLY=1 DISPATCH=0 | PASS | 2464 | 82 |
| C | RUNTIME=1 APPLY=1 DISPATCH=1 | PASS | 2455 | 82 |

Deployed exe fingerprint (SHA256): `32461BA30D2A9DFA5681FE7997BDB94462703CC26F1DAF6EA576BF2959857785`

---

## Forward path

- **BRAIN-RUNTIME-1B / DISPATCH-1:** wire `executeSpecialBody_TraceOnly` results into actual slot writes; requires `MC2_BRAIN_DISPATCH` gate promotion from trace-only to active dispatch.
- **Gesture map:** map recognized verbs to existing `MechWarrior` tac-order setters (one verb at a time, each behind its own sub-gate).
