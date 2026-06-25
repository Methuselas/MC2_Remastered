# BRAIN-FSM-1K-A — Per-warrior FSM State + Flat-Guard DSL

**Branch:** `claude/brain-fsm-1k-a`  
**Gate:** `MC2_BRAIN_FSM` (default OFF)  
**Status:** SHIPPED (build green, harness 21/21 pass)  
**Prereqs:** `MC2_BRAIN_DISPATCH=1`, `MC2_BRAIN_DISPATCH_APPLY=1`  
**Full relink:** yes (MechBrainRuntime layout +64 bytes)

---

## State Fields

Added to `code/mech_brain_runtime.h` after `retreatEffectApplied`:

```cpp
char currentState[32] = {};  // current FSM state name, "" = no-FSM / default
char prevState[32]    = {};  // for Unit.SetStatePrev (transBack equivalent)
```

- 64 bytes per warrior. At 512 warriors = 32KB (negligible).
- Zero-initialized by default ctor brace-init. Mission-ephemeral, not serialized.
- `currentState[0]=='\0'` = unnamed default state. `Unit.InState` guards always fail on unnamed state → safe no-op for non-FSM brains.

---

## 4 Verb Handlers

All in `code/brain_special_dispatch.cpp`, `executeSpecialBody_Apply()`:

| Verb | Handler location | Semantics |
|---|---|---|
| `Unit.SetState "x"` | ~line 904 | prevState←currentState; currentState←x; **early return** (no order call). Trace: `[BRAIN_FSM_SETSTATE] from=<prev> to=<x> wid=<W>` |
| `Unit.SetStatePrev` | ~line 925 | swap currentState↔prevState; **early return** (no order call). Trace: `[BRAIN_FSM_SETSTATEPREV] now=<cur> wid=<W>` |
| `Unit.InState "x"` | ~line 940 | if currentState != x → stateGateOpen=false. Trace: `[BRAIN_FSM_INSTATE] state=<x> match=<0|1> wid=<W>` |
| `Unit.NotInState "x"` | ~line 957 | if currentState == x → stateGateOpen=false. Trace: `[BRAIN_FSM_NOTINSTATE] state=<x> match=<0|1> wid=<W>` |

FSM verbs **NEVER** call `setGeneralTacOrder`. The relaxed_guard_doc checker count = 6 (unchanged).

---

## Flat-Guard Mechanism (stateGateOpen)

Single `bool stateGateOpen = true` local in `executeSpecialBody_Apply`, declared at function entry.

**ONE line** at top of verb-dispatch loop:
```cpp
if (!stateGateOpen) continue;
```

This is the ONLY change to the dispatch loop structure. The 6 effect handlers (POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, WITHDRAW) are **UNCHANGED** — they are gated transparently.

---

## Gate OFF — Byte-Identical Proof

- `s_brainFsmGate()` is a static-once helper (lazy `static const bool`). When `MC2_BRAIN_FSM` is unset/0, returns false.
- `fsmGate=false` → the FSM `else if (fsmGate && ...)` branches never execute.
- `stateGateOpen` is declared but never set to false → `if (!stateGateOpen) continue` never fires.
- FSM verb strings (`Unit.SetState`, etc.) fall through to `isRecognizedVerb` → `[BRAIN_DISPATCH] ... (no effect this verb)` trace. Same as any other recognized-but-unimplemented verb.
- State fields `currentState`/`prevState` are initialized to `{}` and never read or written when gate OFF.
- **Net effect: zero behavioral change for all existing brains when MC2_BRAIN_FSM is absent/0.**

---

## kRecognizedVerbs Addition

Added to `kRecognizedVerbs[]`:
```cpp
"Unit.SetState",
"Unit.SetStatePrev",
"Unit.InState",
"Unit.NotInState",
```

This ensures FSM verb tokens produce `[BRAIN_DISPATCH] ... (no effect this verb)` (not `[BRAIN_DISPATCH_UNKNOWN]`) when gate OFF.

---

## Harness FSM Fixture

`tests/fixtures/brain_runtime/mc2_01_fsm_specials.fit` — raw-brace format, 4 TechSpecial bodies:

1. `fsm_setstate`: `DO Unit.SetState "engage"` — transitions `""` → `"engage"`.
2. `fsm_instate_match`: `DO Unit.InState "engage"` + `DO OPORD.CoreGuard` — gate OPEN, CoreGuard fires (1 order).
3. `fsm_instate_mismatch`: `DO Unit.InState "patrol"` + `DO OPORD.CoreGuard` — gate CLOSED, CoreGuard NOT fired (0 orders).
4. `fsm_setstateprev`: `DO Unit.SetStatePrev` — swaps back to `""`.

Fixture uses `fsm_sequential: true` in manifest.json — harness runs all 4 bodies in order on one `FsmMechWarrior` (state persists). `FsmMechWarrior::getBrainRuntime()` returns a real `MechBrainRuntime`. Total orders = 1 (only gate-open CoreGuard).

Expected trace substrings verified:
- `[BRAIN_FSM_SETSTATE] from= to=engage`
- `[BRAIN_FSM_INSTATE] state=engage match=1`
- `[BRAIN_FSM_INSTATE] state=patrol match=0`
- `[BRAIN_FSM_SETSTATEPREV]`

---

## Harness Infrastructure Added

- `FsmMechWarrior` class in `tools/brain_dispatch_harness/stubs/include/warrior.h`: subclass of `MechWarrior` with a real `MechBrainRuntime fsmRuntime` + `orderCount` sink.
- `fsm_sequential` fixture flag in `FixtureEntry` + `harness_main.cpp`: runs all index bodies sequentially on one warrior for multi-body state-persistence tests.

---

## What Is Deferred (1K-B/C/D)

| Item | Slice |
|---|---|
| `Unit.SetStateIf "x" "varKey" "varValue"` — conditional transition via Var lookup | 1K-B |
| State-gated effect-verb defaults (`Unit.RequireState`) | 1K-C |
| `currentState`/`prevState` save-game serialization | 1K-D |
| `WAYPOINT_REACHED` event type (needed for patrol cycling) | OPORD.CorePatrol slice |
| Var-based transition conditions (`WHEN` clauses) | 1K-B+ |

---

## Checker Status

| Checker | Result |
|---|---|
| `check_brain_relaxed_guard_doc.py` | PASS (6=6=6) |
| `check_brain_fixture_manifest.py` | PASS (23 entries, 23 files) |
| `check_brain_fixture_deploy_sync.py` | SKIP (not yet deployed) |
