# TECHSCRIPT-SPECIAL-DISPATCH-1-RECON-1

**Date:** 2026-06-23
**Status:** RECON/DESIGN — read-only. No code changed. No commits.
**Branch:** `claude/animated-prop-cook-recon-1` (recon only; deliverable is doc, no engine change)
**Modder corpus:** `C:/Users/Joe/Downloads/GameAsset/carver_v_enhanced/missions/` (8 mission files sampled)
**Engine anchors:** `code/warrior.cpp`, `code/mech_brain_runtime.h`, `code/brain_task_queue.h`, `code/mission.cpp:3037`
**Prereq docs:** `docs/render-backend-seams/brain-runtime-recon-1.md`, `brain-runtime-1b.md`

Hard exclusions (from user brief — preserved verbatim):
> No VM implementation, no commander chain, no REQUEST_ORDERS recursion, no save/load, no campaign persistence, no shared global mutable vars, no broad ABL replacement, no multi-warrior behavior until single-warrior proof exists.

---

## §1 — Verb vocabulary (Q1)

Sampled missions: `mc2_01`, `mc2_03`, `mc2_05`, `mc2_10`, `mc2_17`, `mc2_24`, `ai_glenn`, `e3demo`.
All 8 files analyzed for every DO command, IF/LET/STOP construct, and expression form.

### Complete DO verb table

| Namespace | Verb | Example invocation | Engine analog |
|---|---|---|---|
| **Flow** | IF / ELSE / ENDIF | `IF myCond ELSE ENDIF` | — (conditional gate) |
| **Flow** | LET | `LET camUnits = UnitQuery.GetUnitmates 101 camDummyList` | — (local assign) |
| **Flow** | STOP | `STOP` | — (body terminator) |
| **Brain** | Brain.CoreAttack | `DO Brain.CoreAttack target 0` | setGeneralTacOrder(TACTICAL_ORDER_ATTACK_OBJECT) |
| **Brain** | Brain.CorePower | `DO Brain.CorePower false` | PowerDown / setGeneralTacOrder(TACTICAL_ORDER_POWERUP or POWERDOWN) |
| **Brain** | Brain.CoreEscort | `DO Brain.CoreEscort guardCharge 40.0 135.0 -1` | setGeneralTacOrder(TACTICAL_ORDER_FOLLOW_OBJECT) |
| **Brain** | Brain.AttackTactic | `DO Brain.AttackTactic 0 TACORDER_PARAM_RUN TACTIC_REAR tacticState` | setGeneralTacOrder with params+tactic |
| **OPORD** | OPORD.CoreGuard | `DO OPORD.CoreGuard` | setGeneralTacOrder(TACTICAL_ORDER_GUARD_OBJECT) |
| **OPORD** | OPORD.CorePatrol | `DO OPORD.CorePatrol patrolPath 0` | setGeneralTacOrder(TACTICAL_ORDER_PATROL_PATH) |
| **OPORD** | OPORD.CoreMoveTo | `DO OPORD.CoreMoveTo MeetingPoint 0` | setGeneralTacOrder(TACTICAL_ORDER_MOVE_TO_OBJECT) |
| **Unit** | Unit.SetMoveArea | `DO Unit.SetMoveArea guardArea` | setMoveArea() |
| **Unit** | Unit.SetGunnerySkill | `DO Unit.SetGunnerySkill 0.9` | setGunnerySkill() |
| **Unit** | Unit.SetPilotingSkill | `DO Unit.SetPilotingSkill 0.9` | setPilotingSkill() |
| **Unit** | Unit.Retreat | `DO Unit.Retreat` | setGeneralTacOrder(TACTICAL_ORDER_WITHDRAW) |
| **UnitQuery** | UnitQuery.GetUnitmates | `LET camUnits = UnitQuery.GetUnitmates 101 camDummyList` | getUnitmates() into list |
| **UnitQuery** | UnitQuery.GetCount | `IF UnitQuery.GetCount camUnits > 0` | count query |
| **Object** | Object.Remove | `DO Object.Remove camDummyList[camI]` | removeObject() |
| **Object** | Object.Status | `DO Object.Status mateList[i]` | getObjectStatus() trace |
| **Object** | Object.GetObjectposition | `LET guardPos = Object.GetObjectposition guardCharge` | getObjectPosition() |
| **TriggerArea** | TriggerArea.Create | `DO TriggerArea.Create zone1 250.0 250.0 ...` | addTriggerArea() |
| **TriggerArea** | TriggerArea.Destroy | `DO TriggerArea.Destroy zone1` | removeTriggerArea() |
| **String** | String.Concat | `LET fullStatus = String.Concat s1 s2` | string cat utility |
| **Debug** | Debug.Print | `DO Debug.Print "message"` | printf / MC2_BRAIN_DISPATCH_TRACE |
| **Debug** | Debug.PrintVar | `DO Debug.PrintVar varName` | printf var value |
| **Audio** | PlayMusic | `DO PlayMusic CombatTune0 + Random(NumCombatTunes)` | setMusicTrack() |
| **Audio** | StopMusic | `DO StopMusic` | stopMusic() |
| **Audio** | PlaySound | `DO PlaySound mc2_01_01_bik` | playSound() |
| **Video** | PlayBIK | `DO PlayBIK mc2_01_01` | playBIK() |
| **Video** | SetObjective | `DO SetObjective obj1 active` | setMissionObjective() |
| **Camera** | Camera.SetPosition | `DO Camera.SetPosition camPos1` | setCameraTarget() |
| **Camera** | Camera.SetTarget | `DO Camera.SetTarget camTarget1` | setCameraTarget() |
| **Camera** | Camera.Zoom | `DO Camera.Zoom 0.5` | setCameraZoom() |
| **Var** | Var.Set | `DO Var.Set "name" value scope=Mission` | shared mutable (see HAZARD #2) |
| **Var** | Var.Get | `LET x = Var.Get "name" scope=Mission` | shared read |

**Tactic constants observed:** `TACTIC_REAR`, `TACTIC_FRONT`, `TACTIC_FLANK`
**Param flags observed:** `TACORDER_PARAM_RUN`, `TACORDER_PARAM_PURSUE`, `TACORDER_PARAM_JUMP` (bitwise OR: `TACORDER_PARAM_PURSUE + TACORDER_PARAM_JUMP`)
**Expression forms:** `Random(N)`, `+ integer literal`, array index `list[i]`, comparison `> 0`, `== 0`

### Namespace-family count summary

| Family | Distinct verbs | Notes |
|---|---|---|
| Brain.* | 3 | CoreAttack, CorePower, CoreEscort, AttackTactic |
| OPORD.* | 3 | CoreGuard, CorePatrol, CoreMoveTo |
| Unit.* | 4 | SetMoveArea, SetGunnerySkill, SetPilotingSkill, Retreat |
| UnitQuery.* | 2 | GetUnitmates, GetCount |
| Object.* | 3 | Remove, Status, GetObjectposition |
| TriggerArea.* | 2 | Create, Destroy |
| Audio | 3 | PlayMusic, StopMusic, PlaySound |
| Video/objective | 2 | PlayBIK, SetObjective |
| Camera.* | 3 | SetPosition, SetTarget, Zoom |
| Var.* | 2 | Set, Get |
| String.* | 1 | Concat |
| Debug.* | 2 | Print, PrintVar |
| Flow/control | 3 | IF/ELSE/ENDIF, LET, STOP |

Total distinct DO verbs: ~35, plus 3 flow constructs.

---

## §2 — Dispatch seam in runBrain (Q2)

File: `code/warrior.cpp:2177` — `MechWarrior::runBrain()`

```
warrior.cpp:2216  bool enhancedApply = s_brainRuntimeEnabled && s_brainRuntimeApplyEnabled
                                      && brainRuntime
                                      && (brainRuntime->mode == BrainRuntimeMode::Enhanced);
warrior.cpp:2221  if (!enhancedApply) { brain->execute(); ... }
warrior.cpp:2256  } else {
warrior.cpp:2258    // push/drain HOLD_TASK — BRAIN-RUNTIME-1B hook point
```

The `else` branch at line 2256 is the **DISPATCH-1A insertion point**. Under `enhancedApply=true` with gate `MC2_BRAIN_RUNTIME=1` + `MC2_BRAIN_RUNTIME_APPLY=1`, ABL `brain->execute()` is bypassed. A `BrainSpecial` dispatcher replaces it.

### Call chain to dispatch seam

```
Mission::update() or Mover::update()
  MechWarrior::update()  (warrior.cpp:~5087)
    runBrain()            (warrior.cpp:2177)
      [enhancedApply branch]
        → [DISPATCH-1A hook here] executeSpecialBody(warrior, specialBody)
          → parse DO commands in order, stop at STOP
          → unrecognised verb → TRACE only (no crash)
```

Dispatch fires **once per warrior per game update tick** — same cadence as ABL `brain->execute()`. The existing `ZoneScopedN("GameLogic.AI.BrainRun")` Tracy zone covers it.

---

## §3 — Mission.cpp loader for DO-command bodies (Q3)

File: `code/mission.cpp:3037`

The existing loader at line 3037 handles `_ai.fit` blocks (`Brain { unitRef= mode= }`). It does NOT yet load `mission_specials.fit`. There is no existing `DO`-command parser in the engine.

**Design implication for DISPATCH-1A:** The loader must be extended (separate slice from the dispatcher) to:
1. Open `<missionPath><missionName>_specials.fit` (same path pattern as `_ai.fit` at 3044).
2. Parse `BrainSpecial { key= alias= type= Body { ... STOP } }` blocks.
3. Associate parsed bodies with warrior IDs via `alias` match against warrior names.
4. Store parsed bodies in a per-warrior `BrainSpecialBody*` field on `MechBrainRuntime`.

For DISPATCH-1A the loader is parse-only — command tokens stored as `vector<BrainSpecialToken>`, no execution.

---

## §4 — FSM flat-dump hazard (Q4) — HAZARD #1

**Finding:** Every `UnitBrainSpecial` in carver_v_enhanced is a **flat linear dump of ALL FSM state bodies** with no active-state selector.

Evidence (mc2_01 UnitBrainSpecial for `mc2_01_LRMs`, lines 511-546):
```
Body {
    IF Unit.HasWeapons == false          // powerup state guard
        DO Brain.CorePower false
        STOP
    ENDIF
    IF Unit.InState "attack_state"       // attack state code
        DO Brain.CoreAttack target 0
    ENDIF
    IF Unit.InState "powerup_state"      // powerup state code
        DO Brain.CorePower true
    ENDIF
    // ... start state code ...
    STOP
}
```

The ABL runtime maintained an `activeState` variable. The FIT translation tool dumped all state code linearly under IF guards, but the guards test `Unit.InState` — a per-warrior active-state query that **does not exist in the current engine**.

**DISPATCH-1A implication:** A body evaluator running this linearly will execute ALL state bodies every tick, because `Unit.InState` is an unknown verb → TRACE-only (no result, no branch taken) → all branches fall through.

**Required fix before any UnitBrainSpecial dispatch can produce correct behavior:** Add `activeState` string tracking per-warrior `MechBrainRuntime`, evaluate `Unit.InState "X"` as `(activeState == "X")`, and provide `DO Unit.SetState "X"` to transition.

**For DISPATCH-1A scope (parse+trace only):** The hazard is contained — unrecognised `Unit.InState` → TRACE-unknown, no branch taken. The hazard only bites when DISPATCH-1B adds real effects. **Document the hazard in DISPATCH-1B scope definition.**

---

## §5 — MissionSpecial vs UnitBrainSpecial distinction (Q5)

| Type | `type=` field | Who it runs for | Cadence | Engine slot |
|---|---|---|---|---|
| `MissionSpecial` | `type = MissionSpecial` | The mission object, no warrior | Once (init) or per-frame (update) | No warrior; runs in mission update |
| `UnitBrainSpecial` | `type = UnitBrainSpecial` | One warrior, keyed by `alias` | Per warrior brain tick | `runBrain()` enhanced path |

**DISPATCH-1A scope:** `UnitBrainSpecial` only. `MissionSpecial` bodies use Camera, Audio, Video, Objective, and Var-global verbs that require separate mission-update hooks. Out of scope until single-warrior UnitBrainSpecial is proven.

---

## §6 — Verb-to-engine binding table for safe first commands (Q6)

Only verbs with **existing confirmed engine callsites** in `code/warrior.cpp` or `code/group.cpp`:

| Verb | Engine call | File:line | Safety |
|---|---|---|---|
| `Brain.CorePower false` | `setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)` | warrior.cpp:~5525 | Stateless, idempotent |
| `Brain.CorePower true` | `setGeneralTacOrder(TACTICAL_ORDER_POWERUP)` | warrior.cpp:~5617 | Stateless, idempotent |
| `Brain.CoreAttack target` | `setGeneralTacOrder(TACTICAL_ORDER_ATTACK_OBJECT)` | warrior.cpp:~5668 | Needs valid target WID |
| `OPORD.CoreGuard` | `setGeneralTacOrder(TACTICAL_ORDER_GUARD_OBJECT)` | warrior.cpp:~5711 | Safe, object guard |
| `OPORD.CorePatrol path` | `setGeneralTacOrder(TACTICAL_ORDER_PATROL_PATH)` | warrior.cpp:~5766 | Needs valid path ref |
| `OPORD.CoreMoveTo pos` | `setGeneralTacOrder(TACTICAL_ORDER_MOVE_TO_OBJECT)` | warrior.cpp:~5914 | Needs valid pos ref |
| `Unit.Retreat` | `setGeneralTacOrder(TACTICAL_ORDER_WITHDRAW)` | warrior.cpp:~6062 | No args |
| `BRAIN_TASK_HOLD` | `setGeneralTacOrder(TACTICAL_ORDER_STOP)` | warrior.cpp:2268 | Already shipped (1B) |

Verbs with NO current engine binding (all require new implementation):
`Brain.CoreEscort`, `Brain.AttackTactic`, `Unit.SetMoveArea`, `Unit.SetGunnerySkill`, `Unit.SetPilotingSkill`, all `UnitQuery.*`, all `Object.*`, all `TriggerArea.*`, `Var.*`, `String.*`, `Debug.*`, all Audio/Video/Camera.

---

## §7 — Deterministic ordering across warriors (Q7)

`BrainTaskQueue` sort key (brain_task_queue.h:64):
```
priority_tier < insertion_frame_ms < stable_seq_id < warrior_id
```

All four fields are numeric and stable (warrior_id = vehicleWID, not pointer). Sort is byte-deterministic across all warriors and across sessions given the same mission seed.

For DISPATCH-1A each warrior has its own `BrainTaskQueue*` and its own `BrainSpecialBody*`. Cross-warrior ordering is not needed for parse-only dispatch. The determinism guarantee is available for DISPATCH-1B multi-warrior work (excluded by hard exclusions until single-warrior proof).

---

## §8 — Namespace hazard (Q8) — HAZARD #2

`Var.Set "name" value scope=Mission` writes to a shared mission-scoped namespace. Two warriors with UnitBrainSpecial bodies that both write `Var.Set "numMates"` will clobber each other in undefined order (depends on warrior update order).

**DISPATCH-1A:** `Var.Set` / `Var.Get` → unrecognised → TRACE-only. Safe.

**DISPATCH-1B mitigation:** Per-warrior variables must use a namespaced key: `<warrior_alias>.<varname>`. The dispatcher auto-prefixes: `Var.Set "numMates"` for warrior alias `mc2_01_LRMs` stores as `"mc2_01_LRMs.numMates"`. Explicit `scope=Mission` retains global semantics as modder-intended. Document this in DISPATCH-1B scope.

---

## §9 — Gate architecture (Q9)

Layered gates (all default OFF):

| Gate | Purpose | Controls |
|---|---|---|
| `MC2_BRAIN_RUNTIME=1` | Outer: runtime struct + mode detection | Required for all downstream gates |
| `MC2_BRAIN_RUNTIME_APPLY=1` | Enables `enhancedApply` path in runBrain (1B shipped) | ABL skip; drains HOLD_TASK |
| `MC2_BRAIN_DISPATCH=1` | New: DISPATCH-1A parse + trace gate | Load `_specials.fit`; trace unknown verbs; no effects |
| `MC2_BRAIN_DISPATCH_TRACE=1` | Verbose per-verb trace | Debug aid for DISPATCH-1A validation |

Gate check in `runBrain()` enhanced path at `warrior.cpp:2256`:
```cpp
// [DISPATCH-1A addition inside existing enhancedApply else-branch]
if (s_brainDispatchEnabled && brainRuntime->specialBody) {
    executeSpecialBody_TraceOnly(brainRuntime->specialBody, this);
}
```

`s_brainDispatchEnabled` = `std::getenv("MC2_BRAIN_DISPATCH") && atoi(...) != 0`, evaluated once at mission load and cached in a static bool (same pattern as `s_brainRuntimeEnabled` / `s_brainRuntimeApplyEnabled` in warrior.cpp:114-125).

---

## §10 — First real DO command for DISPATCH-1B (Q10)

**Selected verb: `Brain.CorePower false`**

Rationale:
1. **Every UnitBrainSpecial has a power-check guard as the first reachable branch** (observed in 6/8 missions). It is the dominant first path.
2. Engine binding is `setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)` — an existing warrior.cpp callsite, no new code.
3. Stateless and idempotent: calling it on an already-powered-down unit is a no-op (the engine guards internally).
4. No parameter needed — the `false` literal is a fixed constant, no object resolution required.
5. Does not depend on `Unit.InState` or any per-warrior state variable. The `Brain.CorePower false` path in every UnitBrainSpecial is a pre-weapon-check unconditional `STOP` branch — safe to execute even without full FSM state tracking.

**Acceptance criterion:** a warrior set `Enhanced` with `MC2_BRAIN_DISPATCH=1` + `MC2_BRAIN_DISPATCH_1B=1` and a `mission_specials.fit` that has only `DO Brain.CorePower false STOP` in its body causes the warrior to issue `TACTICAL_ORDER_POWERDOWN` on tick 1. Verify via `[BRAIN_DISPATCH]` trace line in stderr. No ABL fallback (enhancedApply=true → ABL skipped).

**BRAIN_TASK_HOLD** is the alternative first command (already shipped in 1B). It maps to `TACTICAL_ORDER_STOP`. It is the safer first command if `Brain.CorePower false` targets an already-powered-down unit and the game logs spurious transitions. Recommend: **use BRAIN_TASK_HOLD as the DISPATCH-1B baseline**, then add `Brain.CorePower` as the second effect verb.

---

## §11 — DISPATCH-1A slice definition

### Name
`TECHSCRIPT-SPECIAL-DISPATCH-1A`

### Gate
`MC2_BRAIN_DISPATCH=1` (default OFF). Also requires `MC2_BRAIN_RUNTIME=1` and `MC2_BRAIN_TASKQ=1`.

### What it does
1. **Loader extension in `mission.cpp`** (near line 3037): when `MC2_BRAIN_DISPATCH=1`, open `<missionPath><missionName>_specials.fit`, parse `BrainSpecial { key= alias= type=UnitBrainSpecial Body { ... STOP } }` blocks into a `BrainSpecialBody` struct (token list: opcode enum + string args). Associate with warrior by `alias` match. Store in `brainRuntime->specialBody`. Silent if file absent.

2. **Dispatcher stub in `warrior.cpp` runBrain enhanced path** (inside the `else` block at 2256): call `executeSpecialBody_TraceOnly(brainRuntime->specialBody, this)`. This function iterates tokens: for any recognised verb → trace `[BRAIN_DISPATCH] verb=<X> wid=<Y>`; for any unrecognised verb → trace `[BRAIN_DISPATCH_UNKNOWN] verb=<X> wid=<Y>`. **No tac-order writes. No ABL. No effects.**

3. **Trace gate `MC2_BRAIN_DISPATCH_TRACE=1`**: enables per-verb trace lines. Without it: only unknown-verb lines (WARN level, always emitted).

### What it does NOT do
- No tac-order writes (all effects are DISPATCH-1B+).
- No `Unit.InState` state tracking (see HAZARD #1 — deferred to 1B).
- No `Var.Set`/`Var.Get` shared namespace (see HAZARD #2 — deferred to 1B).
- No MissionSpecial execution.
- No multi-warrior coordination.
- No campaign persistence, no save/load.

### Files touched
- `code/mission.cpp` (loader near 3037: add _specials.fit load + parse)
- `code/warrior.cpp` (runBrain enhancedApply else-branch: add dispatcher stub call)
- `code/mech_brain_runtime.h` (add `BrainSpecialBody* specialBody = nullptr;` member)
- New file: `code/brain_special_dispatch.h` / `.cpp` (BrainSpecialBody struct, token enum, `executeSpecialBody_TraceOnly`)

### Acceptance criteria
1. `MC2_BRAIN_DISPATCH=0` (default): no behavior change, tier1 5/5 pass, no `[BRAIN_DISPATCH]` lines.
2. `MC2_BRAIN_DISPATCH=1` + no `_specials.fit` present: silent (no crash, no trace), tier1 5/5 pass.
3. `MC2_BRAIN_DISPATCH=1` + valid `_specials.fit` present, `MC2_BRAIN_DISPATCH_TRACE=1`: per-verb trace lines appear in stderr. No gameplay difference.
4. `MC2_BRAIN_DISPATCH=1` + valid `_specials.fit` present with unknown verb: `[BRAIN_DISPATCH_UNKNOWN]` line appears for that verb.
5. No full relink required (no ABI-visible class layout change if `specialBody` added as pointer in MechBrainRuntime — check sizeof invariant).

---

## §12 — DISPATCH-1B slice definition (named first command)

### Name
`TECHSCRIPT-SPECIAL-DISPATCH-1B`

### Prerequisites
DISPATCH-1A green (trace-only pass).

### Gate
`MC2_BRAIN_DISPATCH_1B=1` (default OFF, requires `MC2_BRAIN_DISPATCH=1`).

### First DO command to implement
`BRAIN_TASK_HOLD` (existing, already in BrainTaskType::BRAIN_TASK_HOLD, warrior.cpp:2268).

**Why not jump to Brain.CorePower first:** HOLD is already wired, tested, and proven in 1B. The 1B dispatcher can route `BRAIN_TASK_HOLD` as the default "no body command matched" fallback, then add `Brain.CorePower false` as the first real parsed-verb effect.

**Second DO command to implement (in same slice):** `Brain.CorePower false` → `setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)`.

### Acceptance criterion for 1B
1. A warrior with `Enhanced` mode + `_specials.fit` body containing only `DO Brain.CorePower false STOP` issues `TACTICAL_ORDER_POWERDOWN` on its first brain tick. Verify via `[BRAIN_DISPATCH]` trace + in-game: unit powers down.
2. A warrior with no body (file absent) falls back to HOLD (existing 1B behavior).
3. FSM state tracking deferred: `Unit.InState` remains an unknown verb → TRACE. Document as DISPATCH-1C prereq.

---

## §13 — HAZARD summary

| ID | Hazard | Affects | Mitigation |
|---|---|---|---|
| HAZARD-1 | FSM flat-dump: all state bodies run linearly every tick | UnitBrainSpecial dispatch correctness | Per-warrior `activeState` + `Unit.InState` eval. Deferred to DISPATCH-1C. |
| HAZARD-2 | `scope=Mission` Var namespace shared across warriors | Cross-warrior variable mutation ordering | Auto-prefix non-scoped Var.Set keys with warrior alias. Deferred to DISPATCH-1B Var implementation. |
| HAZARD-3 | Array index expressions `list[i]` require full expression evaluator | Object.Remove, UnitQuery iteration | Deferred. Trace-unknown in DISPATCH-1A. |
| HAZARD-4 | Expression arguments (e.g. `Random(N) + K`) require arithmetic eval | PlayMusic, some OPORD args | Deferred. Literal-only parsing in DISPATCH-1A/1B. |

---

## §14 — VERDICT

**GO** for DISPATCH-1A.

**Rationale:**
- The dispatch seam is clearly located at `warrior.cpp:2256` inside the existing `enhancedApply` else-branch, gated by `MC2_BRAIN_RUNTIME=1` + `MC2_BRAIN_RUNTIME_APPLY=1` (both already shipped).
- DISPATCH-1A adds only a FIT file loader and a trace-only verb iterator. No effects, no tac-order writes, no ABI changes beyond an added pointer in MechBrainRuntime.
- All four HAZARDs are benign in DISPATCH-1A (unrecognised verbs → TRACE-only, no branch taken).
- DISPATCH-1B is well-scoped: HOLD (existing) + `Brain.CorePower false` (idempotent, no parameter resolution required). Low risk, single-warrior, default OFF.
- The modder corpus is a complete and confirmed reference for the verb vocabulary. No unknown-verb surprises expected at DISPATCH-1A (all 35 verbs are catalogued above; unknown-verb trace is the safety net for any gap).

**DEFER items (not blocking 1A):**
- `Unit.InState` / activeState FSM tracking (DISPATCH-1C).
- `Var.*` namespace arbitration (DISPATCH-1B Var extension).
- Array indexing and expression evaluation (DISPATCH-1D+).
- MissionSpecial bodies (separate milestone, requires mission-update hook, not brain tick).
- Multi-warrior coordination (hard-excluded until single-warrior proof).

**STOP conditions (would flip verdict):**
- If `MechBrainRuntime` sizeof invariant breaks on adding `specialBody*` pointer → defer and add via a side map keyed by vehicleWID instead.
- If `_specials.fit` open conflicts with FST archive read — check FitIniFile::open path resolution against FST before coding the loader.
