# BRAIN-RUNTIME-RECON-1 — Brain runtime design, TechScript naming, modder DSL findings

**Date:** 2026-06-23
**Status:** RECON/DESIGN — read-only. No code changed.
**Prereqs verified:** ABL native-binding hardening (complete), BRAIN-FIT-SCHEMA-1 (scoped), TASK-SCHEDULER-CORE-1 (shipped, gate MC2_BRAIN_TASKQ, smoke 3/3).

---

## §1 — TechScript naming: three meanings, one call

### The three meanings

| Label | Where it lives | What it IS |
|---|---|---|
| **Legacy TechScript** | `data/defs/text/en_us/editor/editor_techscript.fit`; `code/ablmc2.cpp` | The existing editor mission-trigger condition/action FSM — media/flag verbs (SetFlag, PlayBIK, DisplayTextMessage). Unrelated to brains. |
| **Modder's "TechScript" DSL** | `carver_v_enhanced` campaign.fit `Mode="TechScript"`, per-mission `Runtime="TechScript"`, `mission_specials.fit` | A FIT-native data format the modder designed to replace ABL scripts. Uses `TechSpecial { ... }` blocks with `DO` commands. |
| **GDD's "TechScript Specials" C++ layer** | Proposed in brain-ai-2.14-techscript-recon-1.md §1, spine step after BRAIN-RUNTIME-1 | The C++ execution layer routing a brain to a registered behavior program. Internal name to choose: `BrainSpecial`. |

### The naming call

**ADOPT the modder's file naming convention; reject the modder's `TechScript` surface naming; use `BrainSpecial` internally.**

Reasoning:

1. `TechScript` already means something in our codebase (the editor FSM). Code searches, modder docs, and error messages will collide. `SCOPE-BRAIN-FIT-SCHEMA-1.md:448` explicitly reserves `BrainSpecial*` for the dispatch layer.
2. The modder's file structure (`mission_specials.fit`, key/alias registry, `TechSpecial` block schema) is a clean prior art for our own `mission_ai.fit` companion design — **adopt the pattern, rename the symbols**. Our equivalent = `BrainSpecial { key= alias= type= Body { ... } }`.
3. The modder's `Mode="TechScript"` / `Runtime="TechScript"` in campaign/mission.fit → our equivalent is `Mode="Enhanced"` (aligns with the `compatibilityMode="Enhanced"` field already in the modder's own mission.fit Brain blocks — he already used this name for per-unit granularity).
4. So: modder content that says `Runtime="TechScript"` maps to our `Mode="Enhanced"`. His `TechSpecial` blocks map to our `BrainSpecial` blocks. His DO-command vocabulary is the **canonical input** for the `BrainSpecial` verb set (see §6 for the full table).

**Summary of the call:**
- Internal C++ symbol: `BrainSpecial` (dispatch layer, registered verbs, execution VM)
- Data file: `mission_specials.fit` → same filename (compatible with modder's data path)
- Block keyword in FIT: `BrainSpecial { ... }` (rename from modder's `TechSpecial`)
- Mode sentinel: `Mode="Enhanced"` in campaign.fit / `Runtime="Enhanced"` in mission.fit
- Legacy editor "TechScript" = untouched, unrelated

This is a DIVERGENCE from the modder's grammar. Modder content needs a one-line migration: `s/TechSpecial/BrainSpecial/g; s/Runtime = "TechScript"/Runtime = "Enhanced"/g`. His DO-command vocabulary, Block fields, and chaining model are **adopted verbatim**.

---

## §2 — BRAIN-RUNTIME-1 struct shape

### Existing engine anchors

- `code/warrior.h` — `MechWarrior` has: `tacOrder[NUM_ORDERSTATES]` (3 slots: GENERAL=0, PLAYER=1, ALARM=2), `ABLModulePtr brain`, `BrainTaskQueue* brainTaskQueue` (just shipped, TASK-SCHEDULER-CORE-1), `PilotAlarm alarm[NUM_PILOT_ALARMS]` (19 alarm types), `timeOfLastOrders`, `getCommander()`.
- `code/warrior.cpp:2142` — `MechWarrior::runBrain()` — calls `brain->execute()` then optionally calls `calcTacOrder()` → `setGeneralTacOrder()` or `setPlayerTacOrder()`.
- `code/warrior.cpp:~5023` — brain cadence gate, where TASK-SCHEDULER-CORE-1 inserted the `drain()` stub.

### The struct

A new `MechBrainRuntime` struct (or plain members grouped under an `mc2::BrainRuntime` struct) hung off `MechWarrior`:

```cpp
struct MechBrainRuntime {
    // OPORD slots — map directly onto tacOrder GENERAL/PLAYER/ALARM
    // Primary → ORDERSTATE_GENERAL (index 0)
    // Secondary → ORDERSTATE_PLAYER (index 1)  [repurposed; player overrides still dominate]
    // Tertiary → ORDERSTATE_ALARM (index 2)    [alarm state stays as-is semantically]
    uint8_t  activeOpordSlot;          // 0/1/2 — which slot is "running"
    uint8_t  opordFallbackPolicy;      // enum: HOLD | WITHDRAW | LOOP_PRIMARY | KEEP_REQUESTING
    uint8_t  completionFlags;          // bit 0=primary done, 1=secondary done, 2=tertiary done
    uint8_t  _pad;

    // Tactic weight table — maps to brain's FIT-loaded switch values
    // Modulator = pilot gunnery/aggressiveness/rank (existing warrior.h fields, NO new data)
    float    tacticWeights[8];         // indices mirror TacticType enum
    uint8_t  weightsNormalized;        // 1 = sum enforced to 1.0 at load time

    // Event intake
    uint8_t  pendingEventMask;         // bit flags for PilotAlarmType events relevant to Brain
    uint32_t lastRequestOrdersFrame;   // g_mc2FrameCounter at last RequestOrdersTask push

    // Compatibility mode
    uint8_t  mode;                     // 0=Legacy, 1=Hybrid, 2=Enhanced
    uint8_t  _pad2[3];
};
```

**OPORD slot mapping rationale:** The three existing `tacOrder[]` slots (GENERAL/PLAYER/ALARM) are a 3-slot order-state array — a natural Primary/Secondary/Tertiary mapping. We do NOT change the tac-order array itself; the Brain runtime reads/writes through the existing `setGeneralTacOrder` / `setPlayerTacOrder` / `setAlarmTacOrder` APIs. `activeOpordSlot` tracks which slot is currently executing.

**No new tac-order array.** Brain runtime consumes the existing `tacOrder[NUM_ORDERSTATES]` via existing setters. The "Primary slot" IS `ORDERSTATE_GENERAL`. The schema re-names, not re-wires.

**Lifecycle:** mission-ephemeral, same as `BrainTaskQueue`. Created in `MechWarrior::init()` (or on-demand at first brain cadence tick inside the `MC2_BRAIN_RUNTIME` gate). Destroyed in `MechWarrior::destroy()`. Not serialized (same non-persistence policy as BrainTaskQueue — if mid-mission save/load lands later, persistence is a separate versioned slice: BRAIN-SAVELOAD-1).

**Consumption of BrainTaskQueue:** The scheduler (`BrainTaskQueue`) is currently an inert stub (drain() does nothing). BRAIN-RUNTIME-1 is where the scheduler becomes live:

```
drain() loop → for each BrainTaskEntry popped:
    if entry.type == REQUEST_ORDERS_TASK:
        → check activeOpordSlot completion → advance slot or call requestOrdersUpChain()
    if entry.type == ALARM_REACTION:
        → set pendingEventMask bit → handled next runBrain() tick
    (other types reserved for TECHSCRIPT-SPECIAL-DISPATCH-1)
```

The drain now **writes a tacOrder slot** (via `setGeneralTacOrder` for slot advancement) or posts up the command chain (`getCommander()->receiveOrdersRequest()`). This is the scheduler transition from inert-stub to live.

---

## §3 — Hybrid arbitration

**The blocker:** when `MechBrainRuntime` (queue-driven) AND legacy ABL brain both want to write `ORDERSTATE_GENERAL`, who wins?

### Three modes

| Mode | Sentinel in FIT | Brain runtime active? | ABL active? | ORDERSTATE_GENERAL owner |
|---|---|---|---|---|
| **Legacy** | `Mode="Legacy"` (or absent) | NO | YES | ABL owns it exclusively. `MechBrainRuntime` does not write any tac-order slot. |
| **Hybrid** | `Mode="Hybrid"` | PARTIAL | YES | ABL owns GENERAL. Brain runtime owns PLAYER slot only (Secondary OPORD → `ORDERSTATE_PLAYER`). ABL can still call `setGeneralTacOrder`. |
| **Enhanced** | `Mode="Enhanced"` | YES | NO (ABL brain skipped) | Brain runtime owns ALL slots. `runBrain()` short-circuits `brain->execute()` when mode==Enhanced. |

### The arbitration rule

**Precedence chain (highest to lowest): PLAYER > ALARM > GENERAL**

This matches the existing tac-order priority model in `warrior.cpp` (player orders always override AI orders). The rule is:

1. **Legacy:** arbitration question is moot — Brain runtime never writes.
2. **Hybrid:** Brain runtime writes Secondary/Tertiary (PLAYER/ALARM slots). ABL writes GENERAL. Player commands still win (they always write PLAYER, which the existing priority chain already handles). The Brain runtime avoids GENERAL entirely.
3. **Enhanced:** Brain runtime owns all three slots. ABL `brain->execute()` is **not called** (skipped inside `runBrain()` behind `MC2_BRAIN_RUNTIME` gate when mode==Enhanced). The existing tac-order network (calcTacOrder, movementUpdate, combatUpdate) is unchanged — only the source of what fills the slots changes.

### Determinism preservation

- Task queue drain uses the same 4-key sort order from TASK-SCHEDULER-CORE-1 (tier → frame_ms → seq → warrior_id).
- Slot advancement (`activeOpordSlot++`) happens only inside the drain loop, never from a callback or interrupt.
- `pendingEventMask` is set only from the drain loop; consumed only at the top of the next `runBrain()` tick.
- No non-deterministic container (hash map, pointer sort) in the path.

---

## §4 — Event intake

**Reuse `PilotAlarm`, do not add a new event source.**

`PilotAlarm` (19 types, `warrior.h:290-317`) already covers the relevant brain events:
- `PILOT_ALARM_ATTACK_ORDER` — player order received
- `PILOT_ALARM_DEATH_OF_MATE` — RequestOrdersTask trigger (commander chain)
- `PILOT_ALARM_TARGET_OF_WEAPONFIRE` / `HIT_BY_WEAPONFIRE` — engagement triggers
- `PILOT_ALARM_GUARD_RADIUS_BREACH` — zone trigger
- `PILOT_ALARM_PLAYER_ORDER` — overrides / radio calls

The Brain runtime registers interest in a subset of `PilotAlarmType` values at mission start (based on the FIT schema's `alarmTriggers` list per BrainSpecial, deferred to TECHSCRIPT-SPECIAL-DISPATCH-1). For RUNTIME-1 scope, the event intake is simpler: the drain loop sets bits in `pendingEventMask` for any queued ALARM_REACTION task; `runBrain()` reads and clears the mask.

**`RequestOrdersTask` flow:**

```
Unit completes final tac-order in OPORD slot N
    → drain() next tick sees OPORD completion via tacOrder state
    → pops REQUEST_ORDERS_TASK entry from BrainTaskQueue
    → checks fallbackPolicy
    → if getCommander() != nullptr: getCommander()->receiveOrdersRequest(this)
      else: apply fallbackPolicy (HOLD / LOOP_PRIMARY / WITHDRAW)
```

`receiveOrdersRequest()` is a new method on `MechWarrior` / `Commander` (RUNTIME-1 scope). It posts a new task into the commander's own `BrainTaskQueue` at tier=1 (Directive).

---

## §5 — Execution model: RUNTIME-1 vs DISPATCH seam

**BRAIN-RUNTIME-1 owns: struct lifecycle, slot/event/scheduler state, mode arbitration, RequestOrdersTask.**

**BRAIN-RUNTIME-1 does NOT execute BrainSpecial DO-commands.** That is TECHSCRIPT-SPECIAL-DISPATCH-1.

The seam is:

```
BrainTaskQueue::drain()
    → recognizes entry.type == BRAIN_SPECIAL_TASK
    → queues a dispatch request: { key=entry.specialKey, ctx=BrainRuntimeContext }
    → TECHSCRIPT-SPECIAL-DISPATCH-1 owns the registry lookup + Body execution
```

In RUNTIME-1, `BRAIN_SPECIAL_TASK` entries are recognized but NOT dispatched — they are discarded with a trace log (`BRAIN_RUNTIME: deferred special <key> (DISPATCH not active)`). This keeps RUNTIME-1 bounded and allows DISPATCH-1 to be developed independently.

**RUNTIME-1 boundary (what it delivers):**
- `MechBrainRuntime` struct (hung off MechWarrior, gated)
- Mode sentinel reading from FIT (legacy/hybrid/enhanced per warrior)
- ABL short-circuit in Enhanced mode
- Drain now writes slot advancement + RequestOrdersTask (non-stub for these two types)
- `receiveOrdersRequest()` on the commander chain
- Event mask from drain loop
- Gate `MC2_BRAIN_RUNTIME` default OFF; Legacy mode = byte-identical to baseline

**TECHSCRIPT-SPECIAL-DISPATCH-1 boundary (deferred):**
- BrainSpecial block parser (mission_specials.fit → key/alias registry)
- DO-command VM (Body execution)
- `Var.Set scope=Mission` variable store
- Verb dispatch table (Brain.CoreAttack, OPORD.CorePatrol, etc.)
- Depends on: ABL-OPCODE-BOUNDS-HARDEN + ABL-RUNTIME-SOFTFAIL (already in the dependency spine)

---

## §6 — Modder DSL findings: DO-command vocabulary

### TechSpecial (→ BrainSpecial) block field schema

```
BrainSpecial {
    key               = "<dotted.key.string>"       ; required, globally unique
    alias             = "<Human.Readable.Name>"      ; display name
    type              = "MissionSpecial"             ; or "UnitBrainSpecial"
    sourceABLFunction = "<functionName>"             ; for MissionSpecial
    sourceABLBrain    = "<brainFsmName>"             ; for UnitBrainSpecial
    Body {
        ; DO commands, IF/ELSE/ENDIF, STOP
    }
}
```

`type="MissionSpecial"` = scenario-level (init/update). `type="UnitBrainSpecial"` = per-unit brain tick.

### DO-command vocabulary (full enumeration from carver_v_enhanced corpus)

| Namespace | Verb | Args summary |
|---|---|---|
| **TechSpecial** | `TechSpecial.Call "<key>"` | Chain to another BrainSpecial by key |
| **Flow** | `STOP` | Terminate body |
| | `Flow.Return <varname>` | Return scenario result |
| **Var** | `Var.Set "<name>" <val> scope=Mission` | Set mission-scoped variable |
| | `LET <name> = <funcCall>` | Assign function result (undocumented; see gap §7) |
| **Audio** | `PlayMusic <id>` | |
| | `PlaySound <id>` | |
| | `PlayWave "<file>" <priority>` | |
| | `PlayVideo "<bik>"` | |
| | `Audio.Stopvoiceover` | |
| | `Audio.Stopmusic` | |
| **Camera** | `Camera.SetPosition <vec>` | |
| | `Camera.GetPosition <out>` | |
| | `Camera.SetGoalPosition <vec> <speed>` | |
| | `Camera.SetRotation <rot>` | |
| | `Camera.GetRotation <out>` | |
| | `Camera.SetGoalRotation <rot> <speed>` | |
| | `Camera.SetMovieMode` | |
| | `Camera.EndMovieMode` | |
| | `Camera.FadeToColor <color> <dur>` | |
| **Brain** | `Brain.CoreAttack <targetId> <param>` | |
| | `Brain.AttackTactic <target> <param> <tactic> <stateVar>` | |
| | `Brain.CoreEject` | |
| | `Brain.CorePower <bool>` | |
| | `Brain.CoreWait` | |
| | `Brain.CoreEscort <targetId>` | |
| | `Brain.SetPilotState <unit> "<state>"` | |
| | `Brain.GetPilotState <out>` | |
| **OPORD** | `OPORD.CorePatrol <patrolState> <path> <attackHandle>` | |
| | `OPORD.CoreGuard <pos> <target> <attackHandle>` | |
| | `OPORD.CoreMoveTo <pos> <attackHandle>` | |
| | `OPORD.CoreMoveToObject <target> <attackHandle>` | |
| **Unit** | `Unit.ClearMoveOrders <flags>` | |
| | `Unit.SetMoveArea ...` | |
| | `Unit.OrderPatrol ...` | |
| | `Unit.TacAttack ...` | |
| | `Unit.TacMoveToObject ...` | |
| **UnitQuery** | `UnitQuery.SetTargetpriority <slot> <type> <id> <range> <criteria>` | |
| | `UnitQuery.SetPilotstate ...` | |
| **Object** | `Object.ConvertCoords <type> <world> <cell>` | |
| | `Object.SetInvulnerable <bool>` | |
| | `Object.Status ...` | |
| | `Object.Remove` | |
| | `Object.SetObjectdamage ...` | |
| **TriggerArea** | `TriggerArea.Add <x1> <y1> <x2> <y2> <team> <active>` | |
| **Debug** | `Debug.SetDebugstring <unit> <slot> "<str>"` | |
| | `Debug.Mcprint "<str>"` | |

### Mission.fit inline Brain block fields (modder's carver_v_enhanced format)

Per `[WarriorN]` section:

```
Brain {
    sourceABLBrain      = "<fsmName>"
    compatibilityMode   = "Enhanced"
    archetype           = "Archetype.InnerSphere.Standard" | "Archetype.Clan.Standard" | ...
    RequestHelp         = true | false
    EngageRadius        = <float>
    AttackerHelpRadius  = <int>
    DefenderHelpRadius  = <int>
    ReturnToPost        = true | false
    PoweredDown         = true
    WakeOnAttack        = true
    PrimaryOPORD { type = Patrol | Guard | Escort | Sentry | PlayerControlled
                   loop = true; Waypoint { index= x= y= } }
    SecondaryOPORD { type = Guard }
    Tactics { Standard=<w>; Flank=<w>; HitAndRun=<w>; Suppress=<w>;
              IndirectFire=<w>; Hover.SkirmishPass=<w>; Hover.WaterFlank=<w> }
    BrainTrigger { event="OnBrainTrigger"; run="<BrainSpecial key>" }
}
```

**Relation to our BRAIN-FIT-SCHEMA-1:** The modder's Brain block is a close prior art for our `mission_ai.fit` schema (SCOPE-BRAIN-FIT-SCHEMA-1). The field names and structure are adoptable with minimal renaming. `compatibilityMode` becomes `mode` (Legacy/Hybrid/Enhanced). `PrimaryOPORD`/`SecondaryOPORD` → our three OPORD slots. `BrainTrigger.run` → deferred to TECHSCRIPT-SPECIAL-DISPATCH-1.

### Chaining model

Two-level: a root `scenario_main` special calls `.init` then `.update` sequentially. Unit brains fire via `BrainTrigger`. No recursive chains, no loops back. `UnitBrainSpecial` bodies run flat (all DO commands every tick — see hardening gaps below).

---

## §7 — Hardening gaps in the modder's implementation

Listed in priority order. We adopt STRUCTURE; we do NOT inherit these assumptions.

### Gap 1 (critical): FSM states are flattened — no active-state tracking

**The biggest gap.** The modder's UnitBrainSpecial bodies translate ABL FSM states by emitting all state branches sequentially with no conditional guard on which state is active. Every tick, ALL states execute in order. ABL state-machine transitions (`trans`, `transBack`) have no TechScript equivalent and are silently dropped. The result: translated brains do NOT behave like the originals.

**Our fix:** TECHSCRIPT-SPECIAL-DISPATCH-1 must implement an active-state variable (persisted in `Var.Set scope=Mission` with a per-unit unique key, or in `MechBrainRuntime.activeStateKey`). The Brain runtime owns this slot; each UnitBrainSpecial body has a guard `IF Var.Get "<unit>.brain.state" == "<stateName>"` generated by the authoring tool or handwritten. State transitions = `Var.Set "<unit>.brain.state" "<newState>"`.

### Gap 2 (critical): `Var.Set scope=Mission` has no deterministic ordering guarantee

The modder assumes variables are set and read in script order. In our engine, if two BrainSpecials (from two warriors) fire in the same tick and both write the same mission variable, order depends on warrior update order, which depends on the `numWarriors%30` staggered cadence — NOT deterministic across save/load or multiplayer peers.

**Our fix:** BrainSpecial variables are namespaced per-unit (`<warrior_id>.<varname>`) unless explicitly declared mission-global. The BrainTaskQueue 4-key sort order (TASK-SCHEDULER-CORE-1) ensures cross-unit Special dispatch order is deterministic. The `Var.Set scope=Mission` global namespace is reserved for MissionSpecial (scenario scripts), not UnitBrainSpecials.

### Gap 3: No mid-mission save

The modder's `Var.Set scope=Mission` store has no persistence contract. Variables evaporate on save/reload. The modder did not address save/load.

**Our constraint (already in TASK-SCHEDULER-CORE-1 non-persistence statement):** Mission runtime is ephemeral. The Brain runtime's variable store and queue state are NOT serialized in RUNTIME-1. If true mid-mission save/load is confirmed later → BRAIN-SAVELOAD-1 (separate versioned slice). Do not design RUNTIME-1 around save/load.

### Gap 4: Unguarded `Object.Remove` and `Object.SetInvulnerable false` every tick

The modder calls these unconditionally on every update tick. In our engine, `Object.Remove` on an already-removed object is not guaranteed safe (depends on object manager guard coverage). `Object.SetInvulnerable false` every tick is a performance cost that also resets state set by other systems.

**Our fix:** TECHSCRIPT-SPECIAL-DISPATCH-1 must add existence guards before object-mutating verbs. `Object.Remove` → check `getByWatchID()` non-null first. `Object.SetInvulnerable` → add a `Var.Get` guard so it only fires on state transition, not every tick.

### Gap 5: `LET` syntax is undocumented / ambiguous

`LET camUnits = UnitQuery.GetUnitmates 101 camDummyList` in several init specials. No runtime spec for how `LET` binds a function-with-output-parameter result. The modder left this as generator output without defining the runtime behavior.

**Our fix:** `LET` is NOT adopted. All out-parameter patterns use `Var.Set` explicitly. Function results are either `DO Var.Set` patterns or passed as explicit out-parameter arguments in the verb signature. `LET` is rejected from the BrainSpecial grammar.

### Gap 6: Generated TODO markers = incomplete translation

Many Body blocks contain:
```
; TODO: manual ABL line: real[3] worldLoc;
; TODO: manual ABL line: state attack; endstate;
```

These represent ABL constructs the generator could not translate: local variable declarations, FSM state boundaries, and complex expressions. The modder left these as audit comments with no runtime equivalent.

**Our fix:** TECHSCRIPT-SPECIAL-DISPATCH-1's authoring tool must provide explicit `BrainVar.DeclareLocal` (for local-scope vars, cleared after each tick) and the active-state mechanism (Gap 1) to cover these cases. TODO markers in modder content are an audit signal, not a runtime feature.

### Gap 7: `IF` syntax has copy-paste parse errors

Several conditions have mismatched parentheses copied verbatim from ABL (extra `)` before `and`). A strict parser will reject these.

**Our fix:** The BrainSpecial parser is stricter than the modder's hand-generated format. We validate at file-load time and emit a non-fatal parse error (log + skip body, report key) rather than executing malformed conditions. ABL-RUNTIME-SOFTFAIL (already in the dependency spine) covers the fatal-error path.

---

## §8 — Scoped BRAIN-RUNTIME-1 first slice

### Scope

This is the smallest safe increment that:
- Makes the scheduler non-inert for the two most critical task types
- Establishes the mode model (Legacy/Hybrid/Enhanced)
- Wires RequestOrdersTask up the commander chain
- Leaves ALL BrainSpecial DO-command execution to TECHSCRIPT-SPECIAL-DISPATCH-1

### Files touched (estimated)

- `code/warrior.h` — add `MechBrainRuntime* brainRuntime = nullptr;` member; add `receiveOrdersRequest()` declaration
- `code/warrior.cpp` — init/destroy lifecycle for `brainRuntime`; mode-read from FIT at `loadBrainParameters`; Enhanced-mode ABL short-circuit in `runBrain()`; drain loop handling for REQUEST_ORDERS_TASK + ALARM_REACTION (replaces inert stub for these types); `receiveOrdersRequest()` implementation
- `code/brain_task_queue.h` — add `type` field to `BrainTaskEntry` (enum: GENERIC/REQUEST_ORDERS/ALARM_REACTION/BRAIN_SPECIAL), add `MechWarriorId` field for commander-chain routing
- `code/mission.cpp` — companion `mission_ai.fit` open (if exists); Brain block read per warrior; mode sentinel read

### Gate

`MC2_BRAIN_RUNTIME` (default OFF). Layered over `MC2_BRAIN_TASKQ` — both must be ON for the drain to be live. When `MC2_BRAIN_RUNTIME=0`: byte-identical to pre-RUNTIME-1 baseline. When `MC2_BRAIN_RUNTIME=1, MC2_BRAIN_TASKQ=0`: gate check logs a warning and no-ops (both gates required).

### Acceptance criteria

| Check | Acceptance |
|---|---|
| Smoke gate OFF (mc2_01, mc2_10, mc2_24) | PASS 3/3 — byte-identical to baseline |
| Smoke gate ON, no mission_ai.fit present | PASS 3/3 — no behavior change (all warriors default to Legacy mode) |
| Smoke gate ON, Enhanced-mode warrior | Unit's `brain->execute()` is NOT called; tac-order slots are written by drain loop only |
| REQUEST_ORDERS_TASK drain | Drain pops task → commander's queue receives entry → trace log `BRAIN_RUNTIME: request_orders warrior=N → commander=M` |
| Full relink | Required (new member on MechWarrior layout) |
| No BrainSpecial DO-command execution | Confirmed by design — `BRAIN_SPECIAL_TASK` entries discarded with trace log |
| No save/load changes | Confirmed — `brainRuntime` not serialized |

### What is deferred (NOT RUNTIME-1)

| Feature | Slice |
|---|---|
| BrainSpecial block parser, DO-command VM | TECHSCRIPT-SPECIAL-DISPATCH-1 |
| `Var.Set scope=Mission` variable store | TECHSCRIPT-SPECIAL-DISPATCH-1 |
| All Brain.\* / OPORD.\* / Unit.\* verbs | TECHSCRIPT-SPECIAL-DISPATCH-1 |
| Active-state tracking (FSM replacement) | TECHSCRIPT-SPECIAL-DISPATCH-1 |
| Tactic weight modulation by pilot stats | TACTIC-WEIGHT-SELECT-1 |
| Primary/Secondary/Tertiary completion flow | OPORD-SLOT-RUNTIME-1 (layered on RUNTIME-1) |
| Archetype .fit resolver | BRAIN-ARCHETYPE-FIT-1 |
| Save/load of Brain runtime state | BRAIN-SAVELOAD-1 |
| Editor brain panel | EDITOR-BRAIN-PANEL-1 |
| Clan Honor switches | CLAN-HONOR-DOCTRINE-1 |

---

## §9 — VERDICT

**GO.**

All three prereqs are confirmed done. The modder's DSL is a solid design-data prior art with a clear adoption path (rename `TechScript` → `BrainSpecial`/`Enhanced`, reject `LET`, fix FSM-flattening at DISPATCH layer). The engine has the exact struct anchors needed (3 tacOrder slots, PilotAlarm, BrainTaskQueue, getCommander chain). The dependency spine is clean and the first slice is tightly bounded.

**Recommended BRAIN-RUNTIME-1 build slice:**

```
Gate:        MC2_BRAIN_RUNTIME (default OFF), layered over MC2_BRAIN_TASKQ
Files:       warrior.h, warrior.cpp, brain_task_queue.h, mission.cpp
New symbols: MechBrainRuntime struct, receiveOrdersRequest(), initBrainRuntimeGate()
Relink:      YES (MechWarrior layout change)
Smoke:       tier1 3/3 gate-OFF + gate-ON (no mission_ai.fit) + gate-ON (Enhanced-mode fixture)
Deferred:    ALL BrainSpecial DO-command execution → TECHSCRIPT-SPECIAL-DISPATCH-1
```

**Top hardening gap NOT to inherit from modder:** FSM-state flattening (Gap 1). The modder's UnitBrainSpecial bodies execute ALL state branches every tick with no active-state guard. Our TECHSCRIPT-SPECIAL-DISPATCH-1 must implement a per-unit active-state variable before any UnitBrainSpecial body is considered correct.

**TechScript naming call:** adopt modder's file/block structure, rename to `BrainSpecial` / `Mode="Enhanced"`. Legacy editor "TechScript" is untouched.
