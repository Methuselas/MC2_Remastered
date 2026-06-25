# BRAIN-FSM-1K-B — Conditional FSM Transition via Var Store

**Branch:** `claude/brain-fsm-1k-b`  
**Gate:** `MC2_BRAIN_FSM` (default OFF)  
**Requires:** `MC2_BRAIN_DISPATCH=1`, `MC2_BRAIN_DISPATCH_APPLY=1`, `MC2_BRAIN_DISPATCH_VAR=1`  
**Status:** SHIPPED (build green, harness pass)  
**Full relink:** yes (no layout change, but `brain_special_dispatch.cpp` change)

---

## Verb

```
Unit.SetStateIf "stateName" "varKey" "varValue"
```

**Semantics:** If the unit var store's `varKey` (Unit scope) equals `varValue`, perform the SetState transition (`prevState←currentState; currentState←stateName`) and early-return (same as `Unit.SetState`). If no match: no transition, body continues (unlike SetState, the no-match path does NOT stop the body).

**Scope:** Unit-scope Var read only (mission-scope SetStateIf deferred to future slice).

**Comparison:** string equality only (`==`). No `>`, `<`, or regex operators (deferred).

---

## Arg Parsing

Three quoted args parsed from `vpCanon + 15` (after `"Unit.SetStateIf"`):

1. `stateName` — double-quoted target state name (up to 31 chars)
2. `varKey` — double-quoted var key to read from unit VarStore (up to 31 chars)
3. `varValue` — double-quoted expected value string (up to 31 chars)

Same inline quoted-token loop as used by `Unit.SetState` / `Unit.InState`.

---

## Gate / Requires-VAR Semantics

| MC2_BRAIN_FSM | MC2_BRAIN_DISPATCH_VAR | Behavior |
|---|---|---|
| OFF | any | Verb is `isRecognizedVerb` → `[BRAIN_DISPATCH] ... (no effect this verb)` trace. Byte-identical (no state change). |
| ON | OFF | Verb handler reached; one-time warning `[BRAIN_FSM_SETSTATEIF] WARNING: ... requires MC2_BRAIN_DISPATCH_VAR=1 — verb is INERT`; no transition; body continues. |
| ON | ON | Full behavior: read Var → compare → conditional SetState or no-op. |

---

## Trace Lines

Match path (transition fired):
```
[BRAIN_FSM_SETSTATEIF] var=<varKey> cur=<curVal> want=<varValue> match=1 from=<prev> to=<stateName> wid=<W>
```

No-match path (no transition):
```
[BRAIN_FSM_SETSTATEIF] var=<varKey> cur=<curVal> want=<varValue> match=0 wid=<W>
```

---

## Order-Call Count

Zero. SetStateIf reads a var and writes state fields only. `setGeneralTacOrder` call count = 6 (relaxed_guard_doc count UNCHANGED).

---

## kRecognizedVerbs Addition

```cpp
"Unit.SetStateIf",
```

Added after `"Unit.NotInState"` in `code/brain_special_dispatch.cpp`.

---

## Harness Fixture

`tests/fixtures/brain_runtime/mc2_01_fsm_setstateif_specials.fit` — raw-brace format, 4 sequential bodies on one `FsmMechWarrior`:

1. `setstateif_setup`: `Var.Set "ready" "1"` — prime unit var.
2. `setstateif_match`: `Unit.SetStateIf "engage" "ready" "1"` — match=1 → transition to `engage`; early return.
3. `setstateif_guard`: `Unit.InState "engage"` + `OPORD.CoreGuard` — gate OPEN (state=engage after match); CoreGuard records 1 order.
4. `setstateif_nomatch`: `Unit.SetStateIf "flee" "ready" "9"` — match=0 (ready="1" != "9") → no transition; stays `engage`.

Manifest entry: `fsm-setstateif-conditional-transition`, `fsm_sequential: true`.

---

## Deferred

| Item | Slice |
|---|---|
| Mission-scope `SetStateIf` (reads `g_missionVarStore`) | future |
| Comparison operators (`>`, `<`, `!=`) | future |
| Built-in predicates (targetVisible, etc.) | future |
