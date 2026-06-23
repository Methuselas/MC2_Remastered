# BRAIN-AI-2.14-TECHSCRIPT-RECON-1 — Feasibility of the MC2R Brain & AI 2.14 spec vs. current engine

Recon-only. Assesses GitHub discussion #19 ("MC2R — Brain & AI System 2.14, Req TechScript") against the
canonical source root `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. **Parallel-to-ABL
design** — the spec explicitly preserves legacy ABL (Legacy/Hybrid/Enhanced modes). This doc does NOT
propose replacing ABL. Cross-references `docs/render-backend-seams/borrow-scripting-1.md` (ABL crash-safety
slices), on which TechScript feasibility depends.

---

## §1 — Spec digest (2.14, faithful)

- **Brain object** attaches to a mission unit by unique ref; holds 60+ behavioral switches (combat / doctrine
  / sensor / survival / targeting / movement / faction), threshold values, tactic weights, and OPORDs.
- **9 core OPORD types:** Patrol, Guard, MoveTo, Sentry, Escort, Ambush, Scout, Attack, Withdraw, plus
  logistics/engineering support variants.
- **Tactic selection = probability WEIGHTS** across named tactics (IndirectFire, HullDown, FightingWithdraw,
  Pursue, HitAndRun…), modulated at runtime by pilot stats (Gunnery/Piloting + traits Leadership / Discipline /
  Aggressiveness / Courage; tiers Green / Regular / Veteran / Elite).
- **Archetype system:** presets bundled as reusable `.fit` files (House Davion, Clan Honor, Pirate, LRM Fire
  Support, Scout, support variants).
- **TechScript Execution Layer:** C++ runtime routes execution to "TechScript Specials" — structured
  behavioral programs that read brain switches as context. Runs **parallel to ABL**. Goal: compose new behavior
  without reimplementing low-level engine verbs.
- **Logistics:** timed service jobs (repair / reload / replenish / capture / sabotage); repair trucks, supply
  convoys, repair bays, ammo dumps, depots as a capturable / protectable / interdictable network. Infantry
  OPORDs for building capture + sabotage (penalized called-shot leg attacks).
- **Clan Honor doctrine:** 9 switches (Zellbrigen, honor-break escalation, dueling, rear-attack restriction,
  dezgra responses).
- **Command chain:** up to 3 sequential OPORDs (Primary/Secondary/Tertiary); on final completion the unit posts
  a `RequestOrdersTask` up the command hierarchy; fallback policy (hold position / hold fire / withdraw / loop
  primary / keep requesting).
- **Mission editor:** brain panel with tactic sliders (live normalized %), archetype dropdown, categorical
  switch groups (highlight changed), OPORD slot manager with map-click waypoint placement.
- **Data format:** typed-block FIT syntax (distinct from legacy ini-format FIT); parser extension supports mixed
  syntax during migration; semantic source-neutral registry keys + author-facing aliases.
- **Runtime:** priority task queue, 7 tiers (Logistics … Background), deterministic save/load, full decision
  visibility. Brain runtime owns OPORD-slot state + event intake + RequestOrdersTask dispatch; TechScript
  Specials own tactical execution.
- **Baseline scope LOCK (release):** single universal Brain runtime; OPORD slots with request_orders; protected
  TechScript core Specials; weighted tactic selection w/ aliases; BrainArchetype presets; ABL
  Legacy/Hybrid/Enhanced compat; timed logistics jobs; support behavior; infantry capture/sabotage; player radio
  calls. **Deferred post-release:** batchall, bidding force reduction, safcon, Trial setup, pre-mission
  force-list edit, campaign-map Clan politics.

---

## §2 — Feasibility matrix (vs. ground-truth source)

| Spec component | Current engine equivalent (file : concept) | Status | Effort | Risk | Notes |
|---|---|---|---|---|---|
| Brain attached to unit by unique ref | `code/warrior.cpp` `MechWarrior` per-mover; `ABLModulePtr brain` + `warriorBrainHandle`; `vehicleWID` (`GameObjectWatchID`) | **EXISTS** | — | — | Per-warrior brain already attached by watch-ID. 2.14 "Brain" = a new struct hung next to it. |
| 60+ behavioral switches / thresholds | `warrior.h` `MemoryCell memory[NUM_MEMORY_CELLS]` (60 cells), `SituationOrders`, `AttackOrders`, `brainState` | **PARTIAL** | M | Low | 60 untyped int/float cells already loaded from FIT (`loadBrainParameters` w/ `NumCells`/`MemType`). Net-new: *named, typed, schema-validated* switch registry over them. |
| 9 OPORD types | `code/tacordr.h` `TacticalOrderCode` (25 codes): PATROL_PATH, GUARD, MOVETO_POINT/OBJECT, ESCORT, FOLLOW, ATTACK_OBJECT/POINT, WITHDRAW, CAPTURE, REFIT, GETFIXED, RECOVER, HOLD_FIRE, WAIT… | **EXISTS (≈8/9)** | S | Low | Patrol/Guard/MoveTo/Escort/Attack/Withdraw/Capture all present as tac-orders. Sentry/Ambush/Scout = compositions of existing primitives (wait+guard / hold-fire+ambush / move+sensor), not new verbs. |
| Weighted tactic selection | `tacordr.h` `TacticType` (FLANK_L/R/REAR, STOP_AND_FIRE, TURRET, JOUST); `AttackOrders.tactic`; brain picks tactic in ABL today | **PARTIAL** | M | Med | Tactic *enum* + per-order tactic field exist; **net-new = the probability-weight table + runtime modulation by pilot stats**. Named tactics (HullDown/HitAndRun) are new behaviors, not just new weights. |
| Pilot stats / traits / tiers | `warrior.h` skills[PILOTING/SENSORS/GUNNERY], `professionalism`, `decorum`, `aggressiveness`, `courage`, `rank` (GREEN/REGULAR/VETERAN/ELITE/ACE), specialty skills | **EXISTS** | — | — | Spec's Leadership/Discipline ≈ professionalism/decorum (alias). Gunnery/Piloting/Aggressiveness/Courage/tiers are 1:1. Pure aliasing job. |
| Archetype `.fit` presets | `warrior.cpp` `loadBrainParameters(FitIniFile*)` already loads per-warrior brain blocks from FIT; `brainStr` names an `.abl` | **PARTIAL** | M | Low | FIT brain-param loading exists. Net-new = an archetype FIT schema (switch+weight+OPORD bundle) and a resolver that layers archetype → per-unit overrides. |
| TechScript Execution Layer / "Specials" | `data/defs/text/.../editor_techscript.fit` (legacy MC2 **trigger/condition-action** "TechScript": SetFlag, PlayBIK, DisplayTextMessage…); `code/ablmc2.cpp` 291 `ABLi_addFunction` native bindings | **PARTIAL / NET-NEW** | L | **HIGH** | ⚠ Name collision: legacy "TechScript" = the editor's mission-trigger condition/action FSM (media/flag verbs), NOT a brain runtime. The 2.14 "TechScript Specials" = **net-new** structured C++ behavior programs. The *native-binding registry* (`ABLi_addFunction`) is the closest existing mechanism and the model to extend. |
| Priority task queue, 7 tiers | NONE. Decision flow is **ad-hoc per-cadence**: `warrior.cpp:5023` `runBrain()` fires when `brainUpdate <= scenarioTime` (+`BrainUpdateFrequency`, staggered per warrior `numWarriors%30`); separate `combatUpdate`/`movementUpdate` timers. Tac-orders use a flat queue (`tacOrderQueue[16]`, `MAX_QUEUED_TACORDERS 2000`). | **NET-NEW** | L | Med | No tiered priority scheduler exists. There IS a per-warrior staggered update cadence and a flat tac-order FIFO with looping (`tacOrderQueueLooping`). The new 7-tier queue is genuinely new infra but has a clean insertion point (the `runBrain` cadence gate). |
| OPORD slots + RequestOrdersTask | `warrior.h` `tacOrder[NUM_ORDERSTATES]` (GENERAL/PLAYER/ALARM — 3 slots!), `timeOfLastOrders`, `getCommander()`, `MoverGroup`/`Team`/`Commander` hierarchy | **PARTIAL** | M | Med | 3 order-state slots ≈ the Primary/Secondary/Tertiary idea. `timeOfLastOrders` + commander chain = the request-orders skeleton. Net-new = explicit sequential-completion → post-up-hierarchy + fallback policy enum. |
| Logistics timed jobs (repair/reload/capture/sabotage) | `tacordr.h` TACTICAL_ORDER_CAPTURE/REFIT/GETFIXED/RECOVER; `code/logisticsdata.cpp`, `logisticsmissioninfo.cpp`; repair/salvage referenced in `mover.cpp`,`gvehicl.cpp`,`turret.cpp` | **PARTIAL** | L | Med | Capture/refit/getfixed/recover tac-orders exist as one-shot orders. Logistics *campaign* layer exists (between-mission). Net-new = **in-mission timed service-job network** (trucks/convoys/depots as live capturable/interdictable nodes) — substantial. |
| Infantry capture + sabotage (leg called-shot) | `gvehicl.cpp` GroundVehicle; aimLocation/called-shot fields in `AttackOrders.aimLocation` | **PARTIAL / NET-NEW** | L | Med | Aim-location/called-shot plumbing exists. Dedicated infantry-unit behavior + building-sabotage + penalty model = mostly net-new. |
| Clan Honor 9 switches (Zellbrigen…) | NONE in code; doctrine logic lives in mod ABL today (Omnitech etc.) | **NET-NEW** | M | Low | Pure data+rule layer over the switch registry + tactic table. No engine primitive needed beyond targeting-filter hooks. |
| Editor brain panel (sliders/archetype/OPORD/map-click) | `editor/EditorResourceCatalog.cpp` references techscript; ImGui editor TUs (`EditRel.exe`, EditorBridge); existing waypoint map-click in tac-order UI (`warrior.cpp drawWaypointPath`); legacy techscript condition/action editor | **PARTIAL** | L | Med | Editor exists w/ ImGui + a legacy TechScript editor + waypoint placement. Net-new = a dedicated Brain panel widget set. EditRel is GPU-path-only (no CPU fallbacks). |
| Typed-block FIT + mixed syntax + alias registry | `mclib/inifile.cpp` `FitIniFile` (`seekBlock`, `readIdLong/Float/String`) — **already block-structured + typed-read**; `editor_techscript.fit` shows typed `String{ key= text= legacyId= }` blocks already in use | **PARTIAL** | M | Low | FIT is *already* a typed-block format with a parser that does typed reads and named blocks. "Distinct typed-block FIT" is largely a **schema/convention** layer, not a new parser. Alias registry (semantic key ↔ author alias) is net-new but small. |
| Deterministic save/load | `code/saveload.cpp`; `MechWarriorData`/`SaveableMoveOrders` POD mirrors already serialize brain memory, tac-orders, attack/move/situation orders, queue | **PARTIAL** | M | Med | Save/load harness + POD-mirror pattern exists and already round-trips the brain. Net-new = extend the schema for Brain/OPORD-slot/task-queue state w/ versioning. |
| Player radio calls | `code/warrior.cpp` `radioMessage()`, `RadioLog`, `radio` | **EXISTS** | S | Low | Radio system fully present. |

---

## §3 — Dependency order (what must land first)

```
            ┌─ ABL-OPCODE-BOUNDS-HARDEN (borrow-scripting-1)  ── PREREQ for any native-binding growth
            │     │
            │     └─ ABL-RUNTIME-SOFTFAIL ──┐
            │                               │
TechScript Specials are MORE native C++ bindings → they inherit the exact
~250-binding unvalidated-out-pointer crash class. Hardening that surface is a
hard prerequisite before adding a new family of native behavior verbs.
            │                               │
   ┌────────┴───────────────┐              │
   ▼                        ▼              ▼
BRAIN-FIT-SCHEMA-1   TASK-SCHEDULER-CORE-1   (soft-fail makes a bad Special non-fatal)
   │  (typed switch/         │ (7-tier queue;
   │   weight/OPORD          │  insertion at the
   │   registry + aliases)   │  runBrain cadence gate)
   │                         │
   ▼                         ▼
BRAIN-RUNTIME-1  ◄───────────┘   (owns OPORD slots + event intake + RequestOrdersTask)
   │
   ├─► OPORD-SLOT-RUNTIME-1   (Primary/Secondary/Tertiary + request_orders + fallback)
   ├─► TACTIC-WEIGHT-SELECT-1 (weight table modulated by existing pilot stats)
   ├─► TECHSCRIPT-SPECIAL-DISPATCH-1  (depends on ABL hardening)
   ├─► BRAIN-ARCHETYPE-FIT-1  (archetype .fit resolver, layered overrides)
   ├─► BRAIN-SAVELOAD-1       (extend saveload.cpp POD mirror + version)
   └─► EDITOR-BRAIN-PANEL-1   (ImGui panel; depends on FIT-SCHEMA + OPORD runtime)

Independent later: LOGISTICS-JOB-NETWORK-1, INFANTRY-CAPTURE-SABOTAGE-1, CLAN-HONOR-DOCTRINE-1
```

**Spine:** ABL hardening → (FIT schema ∥ task scheduler) → Brain runtime → {OPORD slots, tactic weights,
TechScript dispatch, archetypes, save/load} → editor panel. Logistics/infantry/Clan-Honor are leaf features.

---

## §4 — Slices (borrow-now / build-now / defer)

### Build-now (foundation, low risk, high leverage)

- **BRAIN-FIT-SCHEMA-1** — Define the typed-block Brain FIT schema (named switches, thresholds, tactic weights,
  OPORD slots) + the source-neutral key ↔ author-alias registry, riding on the existing `FitIniFile`
  block/typed-read parser. No new parser; a schema + validation + alias map. *Deps: none.* Pairs with the
  existing `loadBrainParameters` path. **Best first slice candidate.**
- **TASK-SCHEDULER-CORE-1** — The 7-tier priority task queue, inserted at the existing per-warrior
  `runBrain` cadence gate (`warrior.cpp:5023`). Default-OFF gate (e.g. `MC2_BRAIN_TASKQ`). Deterministic
  ordering (stable sort by tier then insert-seq) to protect save/load determinism. *Deps: none; pairs with FIT-SCHEMA.*
- **BRAIN-RUNTIME-1** — The single universal Brain struct hung off `MechWarrior`, owning OPORD-slot state +
  event intake (reuse `PilotAlarm`) + RequestOrdersTask dispatch. *Deps: FIT-SCHEMA, TASK-SCHEDULER.*
- **OPORD-SLOT-RUNTIME-1** — Primary/Secondary/Tertiary sequential slots (generalize the existing
  `tacOrder[NUM_ORDERSTATES]` 3-slot array) + completion → post-up-`getCommander()` + fallback-policy enum.
  *Deps: BRAIN-RUNTIME.*
- **TACTIC-WEIGHT-SELECT-1** — Probability-weight table over named tactics, modulated by the *existing* pilot
  stats (gunnery/piloting/aggressiveness/courage/rank). Alias Leadership←professionalism, Discipline←decorum.
  *Deps: FIT-SCHEMA.* New tactic *behaviors* (HullDown/HitAndRun) are separate follow-ups.
- **BRAIN-ARCHETYPE-FIT-1** — Archetype `.fit` files + layered resolver (archetype defaults ← per-unit
  overrides). *Deps: FIT-SCHEMA.*
- **BRAIN-SAVELOAD-1** — Extend `saveload.cpp` POD-mirror pattern (as `MechWarriorData` already does) for Brain
  / OPORD-slot / task-queue state, versioned. *Deps: BRAIN-RUNTIME.*

### Borrow-now (PREREQ — from borrow-scripting-1)

- **ABL-OPCODE-BOUNDS-HARDEN** *(borrow-scripting-1 slice)* — pop-side null/range validation across the ~250
  `exec*` bindings. **Hard prerequisite for TECHSCRIPT-SPECIAL-DISPATCH-1**: TechScript Specials are *more*
  native C++ bindings and inherit the same unvalidated-out-pointer crash class. Harden first.
- **ABL-RUNTIME-SOFTFAIL** *(borrow-scripting-1 slice)* — non-fatal per-call error path. Lets a buggy Special
  (or Hybrid-mode brain) fail soft instead of aborting the mission. Strong pairing with TechScript dispatch.

### Build-after-ABL-hardening

- **TECHSCRIPT-SPECIAL-DISPATCH-1** — The C++ execution layer that routes a brain to a registered "Special"
  (structured behavior program) which reads brain switches as context, modeled on the `ABLi_addFunction`
  registry. **Deps: ABL-OPCODE-BOUNDS-HARDEN + ABL-RUNTIME-SOFTFAIL + BRAIN-RUNTIME.** Highest-risk slice;
  do not start before the ABL binding surface is hardened.
- **EDITOR-BRAIN-PANEL-1** — ImGui Brain panel (tactic sliders w/ live normalized %, archetype dropdown,
  categorical switch groups w/ changed-highlight, OPORD slot manager w/ map-click waypoints). Reuse existing
  editor waypoint placement + legacy TechScript editor scaffolding. EditRel GPU-path-only. *Deps: FIT-SCHEMA +
  OPORD-SLOT-RUNTIME.*

### Defer (leaf features; not blocking the spine)

- **LOGISTICS-JOB-NETWORK-1** — in-mission timed service-job network (trucks/convoys/depots as live
  capturable/interdictable nodes). Large; capture/refit/recover tac-orders give a starting verb set.
- **INFANTRY-CAPTURE-SABOTAGE-1** — infantry behavior + building sabotage + called-shot leg penalty.
- **CLAN-HONOR-DOCTRINE-1** — 9 Zellbrigen switches as data over the switch registry + targeting-filter hooks.
- **All spec-deferred items** (batchall, bidding, safcon, Trial setup, pre-mission force edit, campaign Clan
  politics) — out of release scope per the spec's own LOCK.

---

## §5 — Risks / open questions

1. **Determinism vs. existing per-frame AI.** Current AI is NOT per-frame — `runBrain` fires on a staggered
   per-warrior cadence (`numWarriors%30`, `BrainUpdateFrequency`). A new 7-tier task queue must preserve
   deterministic ordering (multiplayer + save/load both depend on it; tac-orders already carry network
   pack/unpack + `pointLocalMoverId`/`groupFlags`). Stable tier+seq ordering is mandatory.
2. **Save/load schema churn.** `MechWarriorData`/`SaveableMoveOrders` are hand-mirrored PODs serialized in
   `saveload.cpp`. Adding Brain/queue state risks save-version breaks; needs explicit versioning + a default
   migration for old saves. Class-layout change → full relink (per build rules).
3. **ABL ↔ TechScript arbitration.** Legacy/Hybrid/Enhanced modes mean ABL brains and TechScript Specials can
   coexist on the same unit. Who owns the tac-order each tick? Need a clear precedence contract (spec says brain
   runtime owns slots, Specials own tactical execution — must be enforced, not just documented).
4. **Name collision: "TechScript."** The engine already ships a *TechScript* = the editor's mission-trigger
   condition/action FSM (`editor_techscript.fit`: SetFlag/PlayBIK/DisplayText). The 2.14 "TechScript Specials"
   are a *different* concept (brain behavior programs). This will confuse modders and code search — pick a
   distinct internal name (e.g. "BrainSpecial") to avoid conflating the two subsystems.
5. **Scope creep vs. locked baseline.** The spec's own LOCK is disciplined; risk is logistics/infantry pulling
   in map-network + pathing + economy work mid-foundation. Keep leaf features behind the spine.
6. **Binding-safety blast radius.** Adding a new native-verb family (Specials) *before* ABL-OPCODE-BOUNDS-HARDEN
   ships would multiply the existing ~250-binding crash surface. Sequencing is not optional.

---

## §6 — VERDICT

**GO** — a clear, low-risk first slice exists and the spec is well-matched to the engine's bones (per-warrior
brain w/ FIT params, 25-code tac-order system covering ~8/9 OPORDs, pilot stats/traits/tiers 1:1, a typed-block
FIT parser already in use, a 3-slot order-state array, commander chain, radio, and a save/load POD-mirror that
already round-trips the brain). The genuinely net-new infra is the **7-tier task scheduler**, the **named/typed
switch+weight registry**, and the **TechScript-Special dispatch layer** — and the last of these is gated behind
the already-scoped ABL binding-safety work in `borrow-scripting-1.md`.

**Recommended single best first slice: `BRAIN-FIT-SCHEMA-1`** — define the typed-block Brain FIT schema + the
semantic-key/author-alias registry on top of the existing `FitIniFile` block parser and `loadBrainParameters`
path. It is pure data/schema (no relink, no runtime risk, no ABL dependency), unblocks
TACTIC-WEIGHT-SELECT-1 / BRAIN-ARCHETYPE-FIT-1 / EDITOR-BRAIN-PANEL-1, and forces the team to settle the switch
vocabulary and alias model before any runtime is committed. Run **ABL-OPCODE-BOUNDS-HARDEN** (borrow-scripting-1)
in parallel as the prerequisite for the later TechScript-Special dispatch slice.
