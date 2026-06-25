# BRAIN-FSM-DSL-RECON-1

**Date:** 2026-06-24  
**Branch:** claude/animated-prop-cook-recon-1 (nifty worktree)  
**Status:** RECON COMPLETE — VERDICT: GO  
**Gate name (proposed):** `MC2_BRAIN_FSM`  
**Prereqs:** `MC2_BRAIN_DISPATCH=1`, `MC2_BRAIN_DISPATCH_VAR=1`

---

## 1. CURRENT STATE: What the 1C Scanner Found

### Scanner location

`code/brain_special_dispatch.cpp` lines 1562–1675, declared `code/brain_special_dispatch.h` lines 252–257.  
Call site: `code/mission.cpp` lines 3161–3216, gate: `MC2_BRAIN_DISPATCH_FSM_TODO=1`.

### What the scanner does

Second raw-text pass over `data/missions/<name>_specials.fit`. Looks for lines containing:

```
; TODO: manual ABL line: <payload>
```

Payload classified by regex into `FsmTodoKind` enum:

| Kind | Pattern | `name` field |
|---|---|---|
| `STATE_DEF` | `state <name>;` | state name |
| `STATE_END` | `endstate;` | empty |
| `TRANS` | `trans <target>;` | target state name |
| `TRANS_BACK` | `transBack;` | empty |
| `OTHER_TODO` | anything else | full payload |

Results go into `BrainSpecialBody::fsmTodos` (cap 64), then a temporary `tmpBody` that is **immediately discarded** after emitting diagnostic stderr lines. Zero execution — pure inventory.

### Carver FSM shape (ground truth)

- **Total .fit files in carver_v_enhanced:** 169
- **Files with FSM TODOs:** 46 (27% of all .fit files)
- **Coverage of mission specials:** 100% — every `mission_specials.fit` carries FSM state machine structure; the 73% without FSMs are structural files (campaign.fit, logistics.fit, etc.)
- **Conclusion: FSM is universal for actual brain/AI content**

### Real state names found (top-frequency across 46 missions)

| State | Count | Meaning |
|---|---|---|
| `start` | 661 | spawn init / idle |
| `attack` | 616 | combat engage |
| `Base1Patrol` / `Base2Patrol` | 58 | patrol route cycling |
| `baseAssault` / `BaseAssault` | 46 | objective assault |
| `Base1Escort` | 24 | escort behavior |
| `MoveFromSpawn` | 14 | initial deployment |
| `GuardBase` / `GuardArea` | 22 | area hold |
| `GoToBase` / `goTOHPG` | 22 | repositioning |
| `RunToBase1/2` | 16 | retreat to base |
| `PatrolBase` / `*Patrol` | ~23 | various patrol |
| `Wait4Force` | 8 | coordination wait |
| `SpawnIN` | 8 | spawn-in animation |
| `runAway` | 4 | flee behavior |
| `Ambush` | 5 | ambush setup |

**95 unique state names total.** The dominant pattern is a 2–3 state machine: `start → attack` (with `transBack` to resume prior state after combat), plus mission-specific named states for patrol routes, assault, escort.

### Transition patterns

- `transBack` — 610 occurrences. The dominant transition. ABL semantics: pop to the state that called (or entered) the current state — resume-after-interrupt. E.g. `start` spawns into `attack` when threat detected; `attack` does `transBack` when threat cleared → returns to `start` (or patrol).
- Condition-based transitions are rare. Found examples:
  - Timer: `if camTime > 4500 then` (advance after delay)
  - Trigger zone: `if isTriggerAreaHit(NorthBridgeTrigger) AND checkObjectiveStatus(1)==0 then`
  - Multi-objective: `if obj0_check AND obj1_check AND obj2_check AND obj3_check then`
- Most missions use **explicit `trans <target>` + `transBack`** without conditions — conditions are in the ABL body logic, not the FSM transition declarations.

### Representative TODO block (mc2_17)

```
; TODO: manual ABL line: state attack;
; TODO: manual ABL line:   integer  tacticState;
; TODO: manual ABL line:   update;
; TODO: manual ABL line:   transBack;
; TODO: manual ABL line: endstate;
; TODO: manual ABL line: state RunsAways;
; TODO: manual ABL line:   update;
; TODO: manual ABL line: endstate;
; TODO: manual ABL line: state baseAssault;
; TODO: manual ABL line:   update;
; TODO: manual ABL line: endstate;
; TODO: manual ABL line: state GuardBase;
; TODO: manual ABL line:   worldPosition  baseGuard;
; TODO: manual ABL line:   update;
; TODO: manual ABL line: endstate;
```

Key insight: each state contains local variable declarations (e.g. `integer tacticState`, `worldPosition baseGuard`) — these are state-scoped temporaries. The DSL's first cut can ignore state-scoped vars; they map to Unit Var writes.

---

## 2. DSL DESIGN — The Verbs

### Chosen model: FLAT GUARDS

**Rejected: state-block scoping** (`State engage { ... }`) — requires extending the brace-block scanner to handle 3 nesting levels (TechSpecial → Body → State). The current scanner is hardcoded to 3 states (OUTER/IN_SPECIAL/IN_BODY). Adding a 4th level is non-trivial and bloats the parser.

**Rejected: transition table** (`Trans patrol -> engage WHEN <cond>`) — requires a separate table structure outside Body, a new parsing pass, and doesn't fit the existing linear DO-verb model at all.

**Chosen: flat guards** — `Unit.InState` is a conditional gate verb; `Unit.SetState` is an explicit transition verb. Both fit the existing linear DO-verb model without parser changes.

#### Rationale

- Carver shape: `transBack` is the dominant transition (610 occurrences). In flat-guard model this is `Unit.SetState "start"` (explicit) or a `Unit.SetStatePrev` verb.
- The existing scanner already handles payloads as a linear list of DO verbs. Adding two new verb kinds requires zero parser changes — just two new `if (verb starts with "Unit.InState")` branches in `executeSpecialBody_Apply`.
- FITini scanner is already 3-level. Flat guards don't require a 4th level.

### Complete DSL verb set (proposed)

```
; --- TRANSITION VERBS ---
DO Unit.SetState "stateName"
    ; Immediately transitions current-warrior state to <stateName>.
    ; Stops executing remaining verbs in current body (implicit early-exit).

DO Unit.SetStatePrev
    ; Transitions to the previously-recorded state (equivalent to ABL transBack).
    ; Stops executing remaining verbs in current body.

; --- GUARD VERBS ---
DO Unit.InState "stateName"
    ; Guard: if warrior's current state != <stateName>, skip all subsequent DOs in this body.
    ; Does NOT stop execution — it sets a per-body "state gate active" flag.
    ; Multiple InState checks in one body = first false gate wins (short-circuit).

DO Unit.NotInState "stateName"
    ; Guard: if warrior's current state == <stateName>, skip remaining DOs.
    ; (Convenience inverse — avoids needing a separate NOT construct.)

; --- QUERY VERBS (future, 1K-B) ---
DO Unit.SetStateIf "stateName" "varKey" "varValue"
    ; Conditional transition: if Var.Get(varKey) == varValue then SetState(stateName).
    ; Deferred to 1K-B.
```

### How it reads in a specials.fit body

```
TechSpecial {
    alias = "patrol_brain"
    type  = "UPDATE"
    Body {
        DO Unit.InState "patrol"
        DO OPORD.CoreMoveTo 512.0 0.0 512.0
        STOP
    }
}

TechSpecial {
    alias = "engage_brain"
    type  = "UPDATE"
    Body {
        DO Unit.InState "attack"
        DO OPORD.CoreAttack 0
        STOP
    }
}

TechSpecial {
    alias = "threat_detected"
    type  = "ALARM"
    Body {
        DO Unit.SetState "attack"
        STOP
    }
}

TechSpecial {
    alias = "threat_cleared"
    type  = "ALARM"
    Body {
        DO Unit.SetStatePrev
        STOP
    }
}
```

This exactly mirrors the carver's `start → attack (transBack)` dominant pattern.

---

## 3. STATE STORAGE

### Recommendation: dedicated fields on MechBrainRuntime

**Rejected: Var store** (`__state` reserved key) — linear scan over 32 VarEntry per warrior per state check. State is checked on every brain tick for every warrior; adding that to the hot path is unacceptable. Also string equality on every guard verb is avoidable.

**Rejected: enum** — 95 unique state names; an enum requires codegen or a central registry; mod content adds new names. Not viable for mod-driven content.

**Chosen: two char fields on MechBrainRuntime**

Add to `code/mech_brain_runtime.h` after `retreatEffectApplied` (currently line ~147):

```cpp
char   currentState[32];   // current FSM state name, "" = no FSM / default
char   prevState[32];      // for SetStatePrev (transBack equivalent)
```

- 64 bytes per warrior. At 512 warriors = 32KB — negligible.
- State name length cap = 31 chars (null-terminated). 95 carver state names, longest observed ~18 chars (`enterTheRebels`). Safe.
- Initialized at warrior spawn/mission-load: `currentState[0] = '\0'` (unnamed default state).
- Mission-ephemeral: zeroed in `MechBrainRuntime` constructor and at `resetForMission()`.
- `prevState` filled by `SetState`: before overwriting `currentState`, copy to `prevState`.
- `SetStatePrev`: copy `prevState` → `currentState`, zero `prevState` (one level of stack — sufficient for `transBack` pattern).

### Initialization / default state

Empty string `""` = default/unnamed state. `Unit.InState "attack"` when `currentState==""` → false (gate fires, body skips). Brains without FSM verbs never set state → `currentState` stays `""` → InState guards always fail (safe no-op, backward compatible).

To set initial state: first `TechSpecial` of type `INIT` calls `DO Unit.SetState "start"`.

---

## 4. TRANSITIONS — WHAT TRIGGERS THEM

### First cut: explicit SetState only (1K-A)

Transitions are triggered by **explicit `DO Unit.SetState "x"` verbs** in a `TechSpecial` body. No automatic condition evaluation. This exactly matches how carver ABL brains work: a trigger event fires a TechSpecial, that special calls `trans target` — the trigger system (already extant via `pendingEventMask` + `ALARM` body type) IS the condition mechanism.

**No new condition system needed for 1K-A.** The existing event → TechSpecial dispatch IS the FSM input. A threat-detected event fires an ALARM TechSpecial that does `SetState "attack"`. That's it.

### Var-based guards (1K-B, deferred)

`DO Unit.SetStateIf "stateName" "varKey" "varValue"` — conditional transition inside a body, after checking a Var. Deferred. The Var store (`MC2_BRAIN_DISPATCH_VAR`) is already shipped; this just adds one more verb kind that calls `VarStore::get()` then optionally calls the state-change path.

### Built-in predicates (1K-C+, deferred)

`WHEN targetVisible`, `WHEN healthBelow 30` etc. Deferred. Out of scope for 1K.

---

## 5. HOW InState GATES EFFECT VERBS

### Integration point: executeSpecialBody_Apply

`code/brain_special_dispatch.cpp` — the function that iterates `body.verbs` and dispatches each DO verb to an effect handler. Currently a linear scan with if/else chains.

**Add a single gating field to the execution context (stack-local, not stored):**

```cpp
bool stateGateOpen = true;  // starts open (no InState seen yet)
```

Before dispatching any DO verb, check:

```cpp
if (!stateGateOpen) continue;  // all verbs after a failed InState are skipped
```

When `Unit.InState "x"` is processed:

```cpp
if (strcmp(runtime->currentState, stateName) != 0) {
    stateGateOpen = false;
}
// (if match: stateGateOpen stays true, execution continues normally)
```

When `Unit.SetState "x"` is processed:

```cpp
strncpy(runtime->prevState, runtime->currentState, 31);
strncpy(runtime->currentState, stateName, 31);
runtime->currentState[31] = '\0';
return;  // early exit — no subsequent verbs run after a state transition
```

When `Unit.SetStatePrev` is processed:

```cpp
char tmp[32];
strncpy(tmp, runtime->prevState, 32);
strncpy(runtime->prevState, runtime->currentState, 31);
strncpy(runtime->currentState, tmp, 31);
return;  // early exit
```

### Effect on the 6 existing verb handlers

Zero changes to the 6 effect verb handlers themselves. The `stateGateOpen` check is a single `continue` inserted at the TOP of the verb-dispatch loop. All existing verbs — `POWERDOWN`, `EJECT`, `GUARD`, `MOVETO`, `ATTACK`, `WITHDRAW` — are gated transparently.

### Intent queue compatibility

When `MC2_BRAIN_INTENT_QUEUE=1`: `SetState`/`SetStatePrev` write directly to `runtime->currentState` (per-warrior-local, see §6 below — no queueing needed). Effect verbs that are gated-off by `stateGateOpen` never reach `emitBrainIntent()`, so no intent is queued for them. Correct.

---

## 6. DETERMINISM + SUBSTRATE FIT

### Per-warrior-local, NOT through commit phase

`currentState` is **per-warrior private state** with no cross-warrior dependency. The only writer is the warrior's own brain tick. No two warriors write each other's `currentState`. Therefore:

- **No commit-phase coordination needed** for state transitions.
- `SetState` writes `currentState` immediately during `executeSpecialBody_Apply` (same as the `*EffectApplied` once-guards already on `MechBrainRuntime`).
- This is identical in pattern to `dispatchEffectApplied`, `guardEffectApplied`, etc. — already per-warrior-local writes during the dispatch function.

### Threading safety

`FRAME_JOBS_TOUCH=1` runs `touchWorkerPrepass()` in parallel but brain dispatch runs in the main thread (not worker-safe — `isTouchWorkerSafe()` is false for brain-bearing actors). State reads/writes during brain dispatch are single-threaded by construction.

### Fixed-tick cadence

One `executeSpecialBody_Apply` call per brain tick per warrior. State transitions take effect immediately within that tick. The `stateGateOpen` flag is stack-local (no persistence between ticks). A SetState in tick N affects InState guards starting in tick N+1 (the SetState body exits immediately; other bodies in the same tick may fire, but they will see the new state). This is correct ABL semantics — `trans` in ABL also takes effect for the next `update` cycle.

---

## 7. RELATIONSHIP TO PATROL

`OPORD.CorePatrol` (queued next, lane B) needs to cycle through waypoints — a stateful behavior. With the FSM DSL:

```
TechSpecial {
    alias = "patrol_init"
    type  = "INIT"
    Body {
        DO Unit.SetState "patrol_1"
        STOP
    }
}

TechSpecial {
    alias = "patrol_wp1_update"
    type  = "UPDATE"
    Body {
        DO Unit.InState "patrol_1"
        DO OPORD.CoreMoveTo 200.0 0.0 300.0
        STOP
    }
}

TechSpecial {
    alias = "patrol_wp1_arrived"
    type  = "WAYPOINT_REACHED"
    Body {
        DO Unit.InState "patrol_1"
        DO Unit.SetState "patrol_2"
        STOP
    }
}

TechSpecial {
    alias = "patrol_wp2_update"
    type  = "UPDATE"
    Body {
        DO Unit.InState "patrol_2"
        DO OPORD.CoreMoveTo 500.0 0.0 100.0
        STOP
    }
}

TechSpecial {
    alias = "patrol_wp2_arrived"
    type  = "WAYPOINT_REACHED"
    Body {
        DO Unit.InState "patrol_2"
        DO Unit.SetState "patrol_1"
        STOP
    }
}

TechSpecial {
    alias = "patrol_threatened"
    type  = "ALARM"
    Body {
        DO Unit.SetState "attack"
        STOP
    }
}

TechSpecial {
    alias = "patrol_all_clear"
    type  = "ALARM"
    Body {
        DO Unit.SetStatePrev
        STOP
    }
}
```

**The DSL can express patrol.** Each waypoint is a state; arrival fires a WAYPOINT_REACHED event (already implicit in CoreMoveTo completion callback); SetState advances to next waypoint state. Attack interrupts via SetState; SetStatePrev resumes prior waypoint state.

Note: `WAYPOINT_REACHED` event type doesn't exist yet — that's `OPORD.CorePatrol`'s job to add. The FSM DSL itself is expressive enough now.

---

## 8. SCOPED SLICE PLAN

### 1K-A: State storage + SetState + InState guard (FIRST BUILDABLE SLICE)

**Scope (explicit, buildable, harness-provable offline):**

1. Add `char currentState[32]` and `char prevState[32]` to `MechBrainRuntime` after `retreatEffectApplied`. Zero-init in constructor and `resetForMission()`.
2. In `executeSpecialBody_Apply` (`brain_special_dispatch.cpp`): add `bool stateGateOpen = true` local. At top of verb-dispatch loop, add `if (!stateGateOpen) continue`. Add handlers for:
   - `"Unit.SetState"` — copy prevState←currentState, strncpy currentState, `return` early
   - `"Unit.SetStatePrev"` — swap currentState↔prevState, `return` early
   - `"Unit.InState"` — if strcmp fails, `stateGateOpen = false`
   - `"Unit.NotInState"` — if strcmp succeeds, `stateGateOpen = false`
3. Gate: `MC2_BRAIN_FSM=1` wraps the new verb handlers. Without gate, new verb strings fall through as unrecognized (existing warning path) — safe no-op. Existing bodies that don't use FSM verbs are unaffected.
4. **Requires:** `MC2_BRAIN_DISPATCH=1` (prereq, same as all brain dispatch). Does NOT require `MC2_BRAIN_INTENT_QUEUE` or `MC2_BRAIN_VAR_MISSION`.

**Harness-provable offline (no engine run needed):**

The offline brain harness (if it exists) or a minimal unit test file can construct a `MechBrainRuntime`, populate a `BrainSpecialBody` with verbs including `Unit.SetState`, `Unit.InState`, and effect verbs, call `executeSpecialBody_Apply` directly, and assert:
- After `SetState "attack"`: `runtime.currentState == "attack"`, `runtime.prevState == ""`
- `InState "attack"` body with `CoreAttack` verb: `attackEffectApplied == 1`
- `InState "patrol"` body with `CoreAttack` verb (state is "attack"): `attackEffectApplied == 0` (gated)
- `SetStatePrev` after `SetState "attack"`: returns to prior state

No engine start needed. No mission load needed. Pure unit test against `brain_special_dispatch.cpp` + `mech_brain_runtime.h`.

**Full relink needed?** Yes — `MechBrainRuntime` layout changes (added 64 bytes). All TUs that include `mech_brain_runtime.h` need recompile. Use `cmake --build ... --clean-first` or delete mc2.exe + affected .obj.

### 1K-B: Var-based transition conditions

`DO Unit.SetStateIf "stateName" "varKey" "varValue"` — conditional SetState via Var lookup. Requires `MC2_BRAIN_DISPATCH_VAR=1`. New verb kind, no layout changes.

### 1K-C: State-gated effect verb defaults

Extend: if a body has NO `InState` verb and warrior HAS a non-empty currentState, optionally default-gate effect verbs. Conservative: this is opt-in via `DO Unit.RequireState` (a body-level "this body only runs in named states" declaration). Deferred.

### 1K-D: FSM state persistence across reload (save/load)

`currentState` / `prevState` added to save-game serialization. Deferred.

---

## VERDICT

**GO**

**Chosen DSL model: FLAT GUARDS**

- Fits existing linear DO-verb parser (zero parser changes)
- `Unit.InState` = `stateGateOpen` flag, stack-local, zero persistent overhead
- `Unit.SetState` / `Unit.SetStatePrev` = direct writes to 64-byte addition on `MechBrainRuntime`
- Determinism: per-warrior-local writes, no commit-phase needed, single-threaded brain dispatch
- Patrol: expressible with state-per-waypoint + WAYPOINT_REACHED event (event type is future CorePatrol work, not FSM-DSL work)
- Carver shape fit: `start → attack (transBack)` dominant pattern → `SetState "attack"` / `SetStatePrev` — exact match

**1K-A first slice:**  
2 char[32] fields on `MechBrainRuntime` + 4 new verb handlers in `executeSpecialBody_Apply` + `stateGateOpen` loop flag + `MC2_BRAIN_FSM` gate. Full relink, no engine run required for offline unit-test verification. Zero impact on existing brains without gate. Fast cycle: single TU change to `brain_special_dispatch.cpp` + `mech_brain_runtime.h`.
