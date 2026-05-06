# Track F — AI Replacement (Lua-Driven Brain) — Design Spec

**Date:** 2026-04-30
**Status:** Draft (foundational architecture; not implementation-ready)
**Scope:** Defines the architecture for replacing per-warrior AI behaviour with Lua-authored brains, without modifying stock `corebrain.abx`. Companion to Track C (Lua wiring), Track E (JSON manifests), and the Modders' Paradise roadmap. This is a *foundation* spec; an implementation-shape document follows when the gates listed in §13 land.
**Predecessors (read in order):**
- `specs/2026-04-29-modders-paradise-roadmap-design.md` §5 conventions, §6 Track C/D/E
- `explorations/2026-04-30-track-c-battletech-mw5-mod-scene-research.md` §4 Story 5 + §5 gap #8 (this is where Track F was first surfaced as the missing piece)
- `explorations/2026-04-30-track-c-blocking-questions-resolution.md` Q3 (magicpatrol/guard/escort kill, shadow rule, `_impl` extraction pattern)
- `explorations/2026-04-30-track-c-abl-to-lua-reverse-direction.md` (`mc2luadispatch_*` family; engine-side Option B emit pattern)
- `explorations/2026-04-30-track-c-lua-loading-lifecycle.md` (single-VM-with-stage-gates; per-mission lifecycle)
- `memory/magic_abl_contamination_rule.md` (load-bearing: stock `corebrain.abx` is byte-immutable in the stock release path)
- `memory/carver5_mission_playable.md`, `memory/omnitech_abl_stubs_session.md`, `memory/mco_omnitech_integration_attempt.md` (the two real total-conversion attempts and where their AI-brain blockers landed)

---

## §1 — Goal and non-goals

### Goal

A total-conversion mod ships a Lua-authored "brain" that fully replaces the behaviour stock `corebrain.abx` would have computed for the warriors the mod controls. The mod does **not** modify any byte of stock `corebrain.abx` (the magic-ABL-contamination rule remains in force). The Lua brain owns target selection, movement decisions, fire decisions, retreat decisions, and FSM state for each replaced warrior.

The minimum-viable "ship" condition for v1: a Lance of mechs running a mod's Lua brain can:

1. **Patrol** a designated path or area while idle.
2. **Engage on sight** — detect a hostile contact via the engine's existing sensor model and transition out of patrol.
3. **Attack a target** — close to weapons range, face the target, fire at least one weapon group at it.
4. **Break-off when destroyed-or-fled** — the brain ceases issuing orders and reports a clean terminal state when its warrior is dead, captured, or has fled the map.

These four verbs are what `corebrain.abx`'s simplest stock states already do. Reproducing them in Lua proves the seam works.

### Non-goals

- **Total ABL VM replacement.** ABL stays. Stock missions ship `.abx` mission scripts and stock `corebrain.abx`; both keep loading via `code/mission.cpp:2266-2278` (`ABLi_loadLibrary`). Track F replaces *brain dispatch for opted-in warriors only*; everything else routes through ABL exactly as today.
- **Multiplayer-deterministic AI.** Lua is non-deterministic across implementations under some conditions (table iteration order, FP reordering — see roadmap §10 question 2). Track F ships a single-player AI replacement. MP determinism is deferred to a future spec; if/when it lands, the mechanism is Spring/BAR's widget/gadget split applied to the brain backend.
- **A complete combat AI in v1.** Heat management, formation maintenance, terrain-aware approach, retreat-to-repair-truck, NavMesh-quality path planning — not in scope. v1 ships the *seam* and a working-but-simple reference brain that proves the four verbs above. Modders deliver depth.
- **Replacement of `pilotAlarm` callbacks for stock teams.** The 19-entry `pilotAlarmFunctionName[]` table at `code/warrior.cpp:155-175` continues to drive ABL alarm handlers for any warrior whose team is not opted into Lua. Lua brains receive alarms via the event system (§7) but do not displace ABL alarms for non-opted-in teams.

---

## §2 — Stock AI architecture inventory

### What is `corebrain.abx`?

`corebrain.abx` is the compiled-bytecode shared library that every warrior's per-pilot `.abl` brain links against. It defines the FSM primitives (the `core*` family — `coreGuard`, `corePatrol`, `coreWait`, `coreAttack`) plus shared decision routines that per-pilot brains call into. It is loaded once per mission load at `code/mission.cpp:2275-2278`:

```cpp
FullPathFileName libraryFileName2;
libraryFileName2.init(missionPath, "corebrain", ".abx");
library = ABLi_loadLibrary(libraryFileName2, &numErrors, &numLinesProcessed);
gosASSERT(library != NULL);
```

It is *also* loaded by the saveload path at `code/saveload.cpp:1023`. Stock retail ships a 42206-byte `corebrain.abx` (md5 `75f9bbdf…`); the v0.2 hotfix accidentally shipped a 42786-byte modified copy and that caused the "passive enemies / inert turrets / broken HQ convoy" regression — see `memory/magic_abl_contamination_rule.md`. **Any architecture that ships in Track F must keep the stock `corebrain.abx` bytes literally untouched in the stock release path.**

### Per-warrior brain lifecycle

1. **Allocation.** `MechWarrior::setBrain(long brainHandle)` at `code/warrior.cpp:2091-2120` allocates a fresh `ABLModule` per warrior, calls `brain->init(brainHandle)`, then walks `pilotAlarmFunctionName[i]` to populate `brainAlarmCallback[NUM_PILOT_ALARMS]` for each of the 19 alarm types via `brain->findFunction(name, true)`. The brain handle is the preprocessed `.abl` script identified by name in the mission file (`code/mission.cpp:2371-2395`); the brain name string is stashed in `MechWarrior::brainStr`.
2. **Per-tick execution.** `MechWarrior::update()` calls `runBrain()` at `code/warrior.cpp:4974-4977`:

   ```cpp
   if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
       runBrain();
       brainUpdate += BrainUpdateFrequency;
   }
   ```

   `BrainUpdateFrequency` is `2.25` seconds (`code/warrior.cpp:222`) — i.e. **the brain ticks at ≈0.44 Hz, not at 30 Hz**. This is a load-bearing fact for the perf budget (§12). Each tick sets the globals `CurGroup`, `CurObject`, `CurObjectClass`, `CurWarrior`, `CurContact`, `curEventID`, `curEventTrigger` (`code/warrior.cpp:2150-2156`), then calls `brain->execute()`. After execution `runBrain` reads `brain->getInteger()` as the brain's error code.
3. **Alarm-driven execution.** Out-of-tick events fire alarm callbacks via `MechWarrior::checkAlarms()` at `code/warrior.cpp:4735-4855`. Each alarm dispatches the matching `brainAlarmCallback[code]` through a *second* `brain->execute(NULL, brainAlarmCallback[code])` at `code/warrior.cpp:4675` and `:4838`. So the brain runs at one cadence (~0.44 Hz) plus alarm-driven interrupts.
4. **Per-team gate.** `brainsEnabled[MAX_TEAMS]` (`code/warrior.cpp:284`, with 8-team capacity) lets external code disable AI per team. Stock teams default to `true`; the gate is exercised by the player-controlled team (`teamId == -1`).
5. **Teardown.** `setBrain` deletes the previous `ABLModule` and clears the alarm callback table (`code/warrior.cpp:2095-2102`).

### What ABL primitives does the brain call?

The full classification lives in `explorations/2026-04-30-track-c-lua-api-surface-catalog.md`. Brain-side primitives concentrate in:

- **Sensor / perception:** `getContactsList`, `getContactsCount`, `objectVisible` — populate the contact list the brain iterates on.
- **State / FSM:** `getBrainState`, `setBrainState` (`code/ablmc2.cpp:5082`), `getCurrentState`. This is how the FSM walks itself.
- **Decision:** `getMyTarget`, `getThreatToObject`, `getDistanceToObject`, `objectStatus`.
- **Action:** `attackObject`, `attackPoint`, `moveToPoint`, `withdraw`, `dropOrders`, `setBrainState` (transition), `useSpecial`.
- **Memory:** `getRealMemory`/`setRealMemory`/`getIntegerMemory`/`setIntegerMemory` for per-warrior persistent state across brain ticks.
- **Alarms:** the 19 names in `pilotAlarmFunctionName[]` at `code/warrior.cpp:155-175`.

These are a subset of the ~289 ABL extension functions registered in `code/ablmc2.cpp:8049+`. Track C-3 already binds the read-side of most of them as `mc2.*` Lua APIs.

### What state does the brain mutate?

- **Warrior state:** target lock (`attackOrders.targetWID`, `code/warrior.cpp:2312`), attack-order timestamp, alarm state, brain memory slots, FSM state via `setBrainState`.
- **Movement orders:** the warrior's tactical-order slot (`curTacOrder` — see `code/warrior.cpp:2167`). The brain doesn't directly drive the locomotion controller — it sets a high-level `TacticalOrder` and the engine's tac-order resolver in `calcTacOrder` (`code/warrior.cpp:2167-2179`) translates it into actual movement.
- **Fire orders:** `weaponsStatus` (computed every tick at `code/warrior.cpp:4936-4944`); the brain decides whether to authorize fire by mutating attack-orders.
- **Goal plan:** `setUseGoalPlan`, `mainGoalAction`/`mainGoalObjectWID`/`mainGoalLocation`/`mainGoalControlRadius` — the long-term-objective slot (`code/warrior.cpp:2163-2167`).

Critical: the brain does **not** own physics, animation, weapons-firing timing, or hit-resolution. Those are engine systems consumed via the action API. The brain decides; the engine executes. Track F preserves this split.

---

## §3 — Three architectural options

### Option A — Per-warrior Lua brain with full replacement

Each opted-in warrior owns a `LuaBrain` instance. `runBrain()` checks "is this warrior Lua-controlled?" and, if so, calls into the Lua brain via a sol2 protected function instead of `brain->execute()`. The Lua brain owns FSM state, transitions, action dispatch.

**VM topology.** Track C resolved (impl-shape §7) on **single `sol::state` per mission** with stage-gated views, not VM-per-warrior. Option A inherits that: per-warrior *brain instances* are Lua tables stored inside the single per-mission VM, not separate VMs. So "VM-per-warrior memory cost" is not actually a problem; only "table-per-warrior memory cost" is, and that's hundreds of bytes per warrior.

**Pros.**

- Cleanest separation. Modder writes brain logic in Lua; engine never sees ABL for these warriors.
- Per-warrior state lives in a Lua table (`self`). Modders use ordinary Lua semantics.
- No stock-content patching. Stock `corebrain.abx` is bytes-untouched.
- Composes naturally with the deferred-event drain Track C-3 already plans (`blocking-questions-resolution.md` Q1 option (a)).

**Cons.**

- Per-tick Lua overhead for opted-in warriors. At `BrainUpdateFrequency=2.25s` and 100 warriors that's ~44 calls/sec — trivial. The **alarm-driven path** is the real concern: 19 alarm types, each potentially firing every brain-tick cycle, multiplied by 100 warriors. Worst case ≈800 Lua calls/sec from alarms (most alarms don't fire on most warriors most ticks; realistic load is a few dozen).
- Reentrancy if a Lua action call (e.g. `ctx.fire_at(target)`) triggers an engine event that fires another Lua handler. Solvable via the same `in_abl_dispatch_` reentrancy guard the reverse-direction doc already specifies.
- Modder-side complexity: writing a *full* brain from scratch is a lot. v1 mitigates by shipping a reference brain modders can copy-and-extend.

**Performance estimate.** ~44 Hz brain ticks × 100 warriors × ~20 µs/tick ≈ 90 µs/frame at saturation, well under the §12 budget.

**Stock-mission compatibility.** Perfect — stock teams aren't opted-in, so they keep running stock ABL brains via the unchanged `runBrain()` path.

**Modder ergonomics.** Highest. The modder writes one Lua file per brain, registers it, declares which teams it controls in the mod manifest. No `.abx` patch tooling, no FST overlay, no stock-content drift.

**Recommendation:** this is the recommended option (see §4).

### Option B — Mission-level Lua "AI controller" coordinating ABL stubs

ABL still ticks for every warrior. The per-warrior brain `.abl` is replaced by a *thin* shared brain that delegates each decision to the mission-level Lua AI controller via the reverse-direction `mc2luadispatch_*` primitives. The Lua side is one "AI controller" object per team that decides target priorities, formations, retreat conditions; the per-warrior ABL brain is a stub that just dispatches and obeys.

**Pros.**

- Single Lua controller per team — natural fit for high-level decisions like target priority and formation.
- Cheaper per-frame than Option A (one decision pass per team, not per warrior).
- ABL stays in the loop, so the engine's `CurWarrior` / `CurObject` / `CurContact` globals remain valid the whole time — we don't have to teach the rest of the engine that a warrior might not have an ABL brain.

**Cons.**

- **Modders can't ship pure-data mods.** They have to author both Lua *and* a thin ABL stub `.abl` file (or we ship a generic dispatcher `.abl` that every Lua-controlled warrior uses). The latter is workable but adds an FST loose-file constraint — the dispatcher `.abl` has to be present in `data/abl/` for the engine to find it.
- Less granular per-warrior behaviour. If two warriors on the same team want different brains (e.g. a scout brain vs. an assault brain), the controller has to fan out internally. Doable, but the ergonomics aren't natural.
- **The reverse-direction primitives expect ABL-driven dispatch.** They're cheap when ABL is mid-execution. But they hard-require *some* ABL to be running — if the mod author wanted to ditch ABL entirely for their team, they can't.

**Performance estimate.** Maybe 10-15% cheaper than Option A in CPU, but Option A is already well under budget. The saving doesn't matter.

**Stock-mission compatibility.** Same as Option A — stock teams ignore the controller.

**Modder ergonomics.** Worse than A. Modders need both Lua and ABL fluency, even for trivial brains.

**Recommendation: not picked.** Option A subsumes this — a modder who *wants* a single-controller-per-team architecture can build that *on top of* Option A's per-warrior brains by having every brain delegate to a shared decision module.

### Option C — Hybrid: ABL fires events, Lua decides actions, ABL executes orders

Stock corebrain (or a thin replacement) fires `mc2luadispatch_*("AI.WarriorTick", warrior_id)` at well-defined decision points. Lua handler returns an action enum. Engine executes the action via existing C-level primitives.

**Pros.**

- Composes with the reverse-direction `mc2luadispatch_*` family already designed.
- No new VM; modders write only the decision logic.

**Cons (load-bearing).**

- **Requires `corebrain.abx` patching.** Per `abl-to-lua-reverse-direction.md` §6 Option A, this means shipping a patched `corebrain.abx` via the loose-file FST override at `data/abl/corebrain.abx`. That mechanism is fine for *mod content*, but the corebrain belongs to the engine, not to a mod — every mod that wants this hook would either need to own a fork of the corebrain patch, or all mods would have to share the same engine-shipped patched corebrain. The reverse-direction doc explicitly defers Option A to M1 because "we don't have a working patch-build pipeline for the corebrain `.abl` source."
- The contamination rule (`memory/magic_abl_contamination_rule.md`) makes corebrain patches load-bearing-fragile. The v0.2 hotfix shipped a 580-byte-larger corebrain and broke enemy AI engagement entirely. Patching corebrain is something we can do but want to do as rarely and as visibly as possible.
- Hybrid means the modder is still indirectly bound to ABL — they can't deliver an AI behaviour the patched corebrain doesn't already emit an event for.

**Performance estimate.** Equivalent to Option A.

**Stock-mission compatibility.** *Risky.* Patching `corebrain.abx` for the events to fire affects stock missions too — they execute the patched corebrain. We'd need to prove byte-for-byte behavioural equivalence on the non-emit paths. The audit script in `docs/observations/2026-04-25-abl-library-shadow-rule.md` is a starting point but doesn't prove behavioural equivalence, only name-shadow equivalence.

**Modder ergonomics.** Better than B (no per-mod `.abl` writing) but worse than A (modder is bound to whatever decision points the patched corebrain emits; can't add new ones without a new engine release).

**Recommendation: not picked for v1.** Option C is *complementary* to Option A, not a replacement: once Option A ships, a mod that wants to *augment* stock AI rather than replace it can hook the (eventually patched) corebrain via the reverse-direction primitives. That's a Track C-1+ concern, not a Track F concern. Track F's problem is total replacement, and Option A solves it directly.

---

## §4 — Recommended option: Option A with reference brain

Pick Option A. Justifications, ranked:

1. **It is the only option that lets a TC mod ship pure data + Lua and zero ABL.** The two real total-conversion attempts on file (`memory/carver5_mission_playable.md`, `memory/mco_omnitech_integration_attempt.md`) both blocked at "the AI brain crashes when its `.abx` does something the engine doesn't expect." Option A removes the `.abx` from the equation entirely for opted-in warriors. There is no AI-brain crash to land on, because there is no ABL brain executing.
2. **It composes cleanly with Track C's already-resolved architecture.** Single per-mission `sol::state`, deferred-event drain, `*_impl` C-extraction for engine actions, namespace-locked `mc2.*` API. Track F's brain just runs *inside* that same VM at brain-tick time.
3. **It preserves stock missions trivially.** No `.abx` is patched. No FST override is required. Stock teams keep running stock ABL brains; their `runBrain()` path is byte-untouched.
4. **It is the smallest viable seam.** The new dispatch site is one `if` in `MechWarrior::runBrain()`; a similar `if` in `checkAlarms()`. Lua brain instances live in a `std::unordered_map<warriorWID, BrainHandle>` keyed off the warrior's WID. That's it for the engine side.

### Modder-facing API sketch

The brain a modder writes for v1:

```lua
-- mods/btx_total_conversion/scripts/ai/brain_assault.lua
--
-- Patrol → Engage → Attack → Break-off (the four verbs from §1)

local brain = mc2.ai.brain("brain_assault")

-- State definitions. Each function is called at brain-tick (≈ every 2.25s).
brain:state("patrol", function(self, ctx)
  if not self.path_index then self.path_index = 1 end
  local waypoint = self.patrol_path[self.path_index]
  ctx.move_to(waypoint)
  if ctx.distance_to(waypoint) < 50 then
    self.path_index = (self.path_index % #self.patrol_path) + 1
  end
end)

brain:state("engage", function(self, ctx)
  local target = self.target
  if not target or not ctx.object_exists(target) then
    return brain:transition_to("patrol")
  end
  if ctx.distance_to_object(target) > ctx.weapons_range_max() then
    ctx.move_to(ctx.object_position(target))
  else
    ctx.fire_at(target, "all")
  end
end)

brain:state("breakoff", function(self, ctx)
  ctx.drop_orders()
  -- terminal: brain reports done, engine stops ticking it
  return brain:terminate()
end)

-- Transitions. Evaluated at brain-tick after the current state's body runs,
-- and immediately when an alarm fires.
brain:transition("patrol -> engage", function(self, ctx)
  local enemies = ctx.scan({ radius = 800, faction = "enemy" })
  if #enemies > 0 then
    self.target = enemies[1].id
    return true
  end
  return false
end)

brain:transition("engage -> breakoff", function(self, ctx)
  return ctx.is_dead_or_fled(ctx.self_id)
end)

-- Alarm hooks. Map onto pilotAlarmFunctionName[].
-- These run at alarm-fire time, NOT at brain-tick time, but they share the
-- deferred-event drain so they see consistent state.
brain:on_alarm("hit_by_weaponfire", function(self, ctx, attacker_id)
  if not self.target then
    self.target = attacker_id
    return brain:transition_to("engage")
  end
end)

-- Initialization. Called once per warrior when the brain is first attached.
brain:on_init(function(self, ctx)
  self.patrol_path = ctx.warrior.patrol_path or { ctx.warrior.position }
  self.target = nil
  return brain:transition_to("patrol")
end)

return brain
```

The mod's `mod.json`:

```json
{
  "id": "btx_total_conversion",
  "name": "BattleTech Extended Total Conversion",
  "version": "0.1.0",
  "mc2_api_version": 1,
  "depends": { "mc2": ">=0.3.0" },
  "ai": {
    "controls_teams": [2, 3],
    "default_brain": "brain_assault",
    "team_brains": {
      "2": "brain_assault",
      "3": "brain_scout"
    }
  }
}
```

The `ai.controls_teams` field is the opt-in mechanism (§10). For warriors on those team IDs, `runBrain()` routes to the named Lua brain instead of calling `brain->execute()`.

---

## §5 — Engine-side primitives needed

The Lua brain calls into engine logic via the same `mc2.*` namespace Track C-3 ships, plus a small Track-F-specific namespace `mc2.ai.*`. Promotion table (against `track-c-lua-api-surface-catalog.md` classifications):

| Capability | Underlying ABL function | Today | v1 needed |
|---|---|---|---|
| `ctx.move_to(pos)` | `setMoveOrder` / tac-order machinery | INTERNAL | promote to STABLE |
| `ctx.fire_at(target, group)` | engine fire-control | INTERNAL | promote to STABLE |
| `ctx.drop_orders()` | `dropOrders` / clear `curTacOrder` | EXPERIMENTAL | promote to STABLE |
| `ctx.scan({radius, faction})` | `getContactsList` + `objectVisible` filter | INTERNAL (contacts list) + STABLE (objectVisible) | new wrapper |
| `ctx.distance_to(pos)` / `distance_to_object(id)` | `execDistanceToPosition` / `execDistanceToObject` | STABLE | as-is |
| `ctx.object_exists(id)` | `execObjectExists` | STABLE | as-is |
| `ctx.object_position(id)` | `execGetObjectPosition` | STABLE | as-is |
| `ctx.is_dead_or_fled(id)` | `execIsDeadOrFled` | STABLE | as-is |
| `ctx.weapons_range_max()` | weapon-group introspection | EXPERIMENTAL | promote to STABLE |
| `ctx.use_special(name)` | `useSpecial` / jump-jets | EXPERIMENTAL | promote to STABLE |
| `ctx.warrior.position` | `execGetObjectPosition(self_id)` | STABLE | as-is (read view, §6) |
| `ctx.warrior.heat` | mech heat lookup | EXPERIMENTAL | promote to STABLE |
| `ctx.warrior.weapons[]` | weapon list | EXPERIMENTAL | promote to STABLE |
| `mc2.ai.brain(id)` | (new) brain-builder factory | none | new |
| `brain:state` / `brain:transition` / `brain:on_alarm` / `brain:on_init` | (new) brain-DSL methods | none | new |

The `*_impl` extraction pattern from `blocking-questions-resolution.md` Q1 applies to every "promote to STABLE" row. Net engine work for Track F-1 is: extract `_impl` for the 5-7 action primitives that don't have one yet, then add the brain DSL on top of an already-correct binding pattern.

---

## §6 — Per-warrior state model

The `ctx.warrior` table exposed to brain bodies is a *facade* over `MechWarrior` accessors, not direct struct access. All members are read-only by default; mutation goes through action methods (§8) that perform validation.

```lua
ctx.self_id           -- warrior watch-id (engine's stable object handle)
ctx.warrior.position  -- {x,y,z} in MC2 world coords (raw east/north/elev)
ctx.warrior.heading   -- radians; facing direction
ctx.warrior.heat      -- 0..1 fraction of overheat threshold
ctx.warrior.armor     -- {ct,lt,rt,la,ra,head,ll,rl} -> 0..1 fraction
ctx.warrior.team      -- engine team id (matches mod.json ai.controls_teams)
ctx.warrior.commander -- player-vs-AI commander id
ctx.warrior.weapons   -- array of { id, group, range_max, range_min, ammo, ready }
ctx.warrior.target    -- last target watch-id, or nil
ctx.warrior.tac_order -- { code, target_wid, location } -- current tac-order slot
ctx.warrior.is_destroyed
ctx.warrior.is_disabled
ctx.warrior.is_withdrawn
```

**Read-write semantics.** `ctx.warrior` is a `__newindex`-trapped table that throws on direct mutation. The brain mutates via:

- **State stash:** `self.<key> = value` writes to the brain *instance* table, which persists across ticks for that warrior. This is the modder-facing equivalent of the ABL `setRealMemory`/`setIntegerMemory` slots — Lua tables instead of fixed indexed slots.
- **Engine actions:** §8 below.

There is no setter for `ctx.warrior.heat` or `ctx.warrior.armor` — those are engine state, not brain state. A brain that wants to "heal itself" calls a privileged `ctx._dev_heal()` (which is gated behind a `MC2_AI_DEBUG=1` env var; modders ship without it).

**Persistence across saves.** The `self` table is serialized via `mc2.persist` (per `loading-lifecycle.md` save/load). Only plain values, tables of plain values, and registered brain references survive — closures and userdata are dropped at save time with a warning logged.

---

## §7 — Sensor / perception model

The brain queries the world via `ctx.scan(filter)` and a few targeted primitives.

```lua
ctx.scan({
  radius   = 800,        -- world units; required
  faction  = "enemy",    -- "enemy" | "friendly" | "neutral" | <team_id>
  los      = true,       -- require line-of-sight; default true
  type     = "mech",     -- optional: "mech"|"vehicle"|"turret"|"building"|"any"
  max      = 16,         -- optional cap on returned list size
})
-- returns array of { id, position, type, faction, distance, last_seen, threat }
```

Implementation: `ctx.scan` is a binding around the engine's existing contact list (`getContactsList`) + visibility filter (`objectVisible`) + radius/type/faction post-filter. The contact list is *already* the engine's fog-of-war + sensor-range result — Track F does not invent a new sensor model.

**Interaction with stock fog-of-war.** The engine's contact list respects sensor range and line-of-sight per the stock rules. Our brain sees what the warrior sees, no more. A mod that wants to *bypass* fog-of-war (e.g. for an "all-knowing AI" gameplay style) declares it in `mod.json`:

```json
"ai": {
  "controls_teams": [2, 3],
  "fog_of_war": "ignore"   // "respect" (default) | "ignore" | "team_shared"
}
```

`"team_shared"` reads the union of all friendlies' contact lists — useful for a coordinated lance. `"ignore"` returns every object on the map filtered by `radius` only, ignoring `objectVisible`. Both modes log a one-shot warning at brain init: `[AI] event=fog_override mod=<id> mode=<...>`. The engine's stock contact list is never mutated by the brain — modes are read-side filters.

A mod that wants *stricter* perception (e.g. line-of-sight only, no radar) calls `ctx.scan({los=true, sensor=false})`. The `sensor=false` flag is a v1.1 add; v1 ships only `los`-respect/ignore.

---

## §8 — Action dispatch

The brain decides; the engine executes. Actions are the only mutation path.

```lua
ctx.move_to(point)                       -- queue a move tac-order
ctx.move_to_object(target_id, follow=true)
ctx.face(direction_or_object)            -- rotate-only
ctx.fire_at(target_id, weapon_group)     -- "all" | "primary" | "secondary" | {ids}
ctx.fire_at_point(point, weapon_group)
ctx.use_special(name)                    -- "jump_jets" | mod-defined
ctx.drop_orders()                        -- clear curTacOrder
ctx.eject()                              -- terminal
ctx.set_goal(action, target, location, radius)  -- long-term goal slot
```

**Synchronous or queued?** Both. Each action lands in a per-warrior intent slot at call time; the engine consumes intents at the end of the brain tick. **Within a single tick, the last-call-wins for any given slot:** if the brain calls `ctx.move_to(A)` then `ctx.move_to(B)`, only B is sent. This is the same shape as ABL's `setMoveOrder` semantics — consecutive calls within one brain tick clobber each other in the engine state too.

**Conflict resolution across slots.** Move and fire are independent slots. `ctx.move_to(A)` and `ctx.fire_at(target)` both apply. `ctx.drop_orders()` clears every slot. `ctx.eject()` is terminal — subsequent calls in the same tick are no-ops with a warning logged.

**Reentrancy.** Actions are `*_impl` C functions per the Q1 pattern; they don't reenter the ABL stack. They *can* fire engine events (e.g. weapon-discharge fires `engine.WeaponFired`). Those events drain on the next deferred-event drain, not synchronously inside the brain tick — see `loading-lifecycle.md`.

---

## §9 — FSM helpers vs decision tree vs behavior tree

The BattleTech-IP modding scene splits roughly:

- **Classic ABL / corebrain.abx** — explicit FSM with named states.
- **RogueTech, BTA3062** — behaviour trees (selectors + sequences + decorators), implemented in C# DLLs on top of HBS BattleTech.
- **MW5 TacOps and YAML-AI mods** — closer to a hybrid: state buckets per role, evaluator scoring inside each bucket.

Forcing one paradigm is the wrong call. Track F ships the **low-level FSM primitives** (`mc2.ai.brain`, `:state`, `:transition`, `:on_alarm`) as the canonical seam, plus a **helper module `mc2.ai.tree`** built on top for behaviour-tree authors:

```lua
local tree = mc2.ai.tree
local root = tree.selector({
  tree.sequence({
    tree.condition(function(ctx) return ctx.scan({radius=800}).count > 0 end),
    tree.action(function(self, ctx) ctx.fire_at(self.target, "all") end),
  }),
  tree.action(function(self, ctx) ctx.move_to(self.next_waypoint) end),
})
local brain = mc2.ai.brain("scout"):body(root)
return brain
```

`mc2.ai.tree` is pure Lua, not engine code. It compiles a tree spec into a single brain-tick body that calls the FSM primitives underneath. Modders pick per brain. Behaviour-tree authors get their preferred paradigm; FSM-comfortable modders skip the helper.

A scoring/utility-AI helper (`mc2.ai.score`) follows the same pattern in v1.1.

---

## §10 — AI replacement opt-in mechanism

The mod manifest declares which teams it controls:

```json
"ai": {
  "controls_teams": [2, 3],
  "default_brain": "brain_assault",
  "team_brains":   { "2": "brain_assault", "3": "brain_scout" },
  "fog_of_war":    "respect"
}
```

Engine routing logic, evaluated at warrior-attach time (i.e. when `MechWarrior::setBrain` is called):

```cpp
long MechWarrior::setBrain(long brainHandle) {
    // ... existing teardown ...

    if (g_LuaVM && g_LuaVM->HasAIControlForTeam(teamId)) {
        std::string brainName = g_LuaVM->ResolveBrainNameForTeam(teamId);
        luaBrain_ = g_LuaVM->AttachBrain(brainName, getWatchID());
        luaBrainActive_ = true;
        return NO_ERR;
    }
    // ... existing ABL path ...
}
```

`runBrain()` then forks at the top:

```cpp
long MechWarrior::runBrain() {
    if (luaBrainActive_) {
        return g_LuaVM->TickBrain(luaBrain_, scenarioTime);
    }
    if (!brain) return 0;
    // ... existing CurWarrior/CurObject setup + brain->execute() ...
}
```

`checkAlarms()` does the same fork: for Lua-controlled warriors, alarm callbacks dispatch through `g_LuaVM->FireAlarm(luaBrain_, alarmCode, triggers)` instead of `brain->execute(NULL, brainAlarmCallback[code])`.

Critically: stock `corebrain.abx` is still loaded by `code/mission.cpp:2275-2278`, even when *every* team is Lua-controlled. The library may be unused, but loading it costs ≈ms per mission and keeps the engine's symbol-resolver paths byte-untouched. This is a contamination-rule-respecting design.

A mod cannot opt-out of this: stock corebrain is part of the engine baseline; Lua brains layer on top.

---

## §11 — Stock-mission compatibility

A mod replacing AI must not break stock mc2_01 through mc2_24. Verification:

1. **Stock `corebrain.abx` is byte-untouched** in the stock release path. No FST override is created. The contamination rule (`memory/magic_abl_contamination_rule.md`) holds.
2. **Stock teams default to ABL execution.** A mod that doesn't list a team in `ai.controls_teams` does not affect that team's brain.
3. **Mods overriding stock missions must declare so explicitly.** A mod that wants to take over Team 1 (the player team, `teamId == -1` in stock for the human commander; AI teams are `0..7`) in stock missions sets `ai.controls_teams: [<team_id>]` and accepts the regression-test burden. This is documented in the modder-facing docs as "if your `controls_teams` overlaps a stock mission's enemy team list, you own that mission's behaviour."
4. **Smoke matrix.** Track F ships with a regression run against tier1 (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24` — see worktree CLAUDE.md "Smoke Gate") with no mods loaded. The brain dispatch fork in `runBrain()` must be a perfect no-op when `g_LuaVM->HasAIControlForTeam(teamId)` returns false. The audit gate is: profile `runBrain` on tier1 with vs. without the dispatch fork; require <1% delta in CPU cost.

A separate audit catches accidental contamination: a hash-check of `data/missions/corebrain.abx` against stock's md5 `75f9bbdf…` runs at engine init when any AI mod is loaded, and emits `[AI] event=corebrain_hash status=ok` (or `status=mismatch path=…` if it ever drifts). The existence of this check makes the contamination rule operationally enforced, not just documented.

---

## §12 — Performance budget

The naive estimate "100 warriors × 30 Hz = 3000 brain ticks/sec" is **wrong for MC2.** Brain ticks at `BrainUpdateFrequency = 2.25s` (`code/warrior.cpp:222`), which is ≈0.44 Hz. Concrete numbers:

- **Brain ticks at saturation:** 100 warriors × 0.44 Hz ≈ 44 ticks/sec.
- **Alarm fires:** worst-case 2-3/warrior/sec under heavy combat ≈ 250 fires/sec.
- **Sensor scans:** at most 1/brain-tick + 0-2 per state-body call ≈ 100-200 scans/sec.
- **Action dispatches:** ~1-3/brain-tick × 44 ticks/sec ≈ 100-150 actions/sec.

Per-call costs (sol2 numbers from `track-c-lua-trampolines-and-tests.md`):

- Lua function call with closure: ~300 ns
- `*_impl` C call through sol2 binding: ~500 ns
- Sensor scan (~16 candidate iteration + visibility check): ~10-20 µs
- State-body execution (a typical brain-state body): ~5-30 µs

Total CPU at saturation:

```
brain ticks:  44 × 30 µs    =   1.3 ms/sec
alarms:      250 × 5 µs     =   1.3 ms/sec
scans:       200 × 15 µs    =   3.0 ms/sec
actions:     150 × 1 µs     =   0.2 ms/sec
─────────────────────────────────────────
total                       ≈   5.8 ms/sec  =  0.6% of one core
```

That's well under the **5 ms/frame** budget the task brief allows. The dominant cost is sensor scans, and those are dominated by *engine-side* contact-list iteration, not Lua. Lua is essentially free at this load.

**Saturation cliff.** The danger zone is alarms during combat surges. A 4-vs-4 mech firefight produces ~50-100 alarms/sec/team. Under that load Lua dispatch climbs to ~3 ms/frame for ~1 second bursts. The deferred-event drain (per `blocking-questions-resolution.md`) batches these to once per frame, capping the worst case.

The §11 audit-gate constraint (<1% delta in `runBrain` cost on tier1 with no mods) protects the cold path.

---

## §13 — Sub-slices

Each independently buildable, each with a clear gate. Modeled on Track C's M0/M1/M2 sequencing.

### F-1 — Engine fork + minimal brain DSL

Smallest possible slice that proves the seam:

- Add `luaBrainActive_` and `luaBrain_` fields to `MechWarrior`.
- Fork `runBrain()` and `checkAlarms()` at the top with the routing logic from §10.
- Add `g_LuaVM->HasAIControlForTeam`, `AttachBrain`, `TickBrain`, `FireAlarm` shims.
- Implement `mc2.ai.brain(id)`, `:state`, `:transition`, `:on_init` in pure Lua/sol2 atop existing Track C bindings.
- Ship a reference brain `mods/test/scripts/ai/brain_idle.lua` that just logs once per tick and never moves.

**Gate:** boot `mc2_01` with `mods/test` loaded, `ai.controls_teams=[]` (no team controlled). All stock missions pass tier1 with <1% perf delta.

Then enable a single team:

**Gate B:** `mods/test` with `ai.controls_teams=[2]` on a custom test mission with one Team-2 mech. Mech idles, brain logs at 0.44 Hz, no crashes, no contamination of stock teams.

### F-2 — Sensor + action

Wire `ctx.scan`, `ctx.move_to`, `ctx.fire_at`, `ctx.drop_orders`, plus `ctx.warrior` read facade. Promote 5 EXPERIMENTAL bindings to STABLE per §5.

**Gate:** reference brain `brain_assault.lua` from §4 boots, patrols, engages on sight, attacks, terminates clean. Run the four-verb scenario as the canonical test. No reentrancy crashes, no ABL stack corruption.

### F-3 — Alarms + persistence

Map all 19 `pilotAlarmFunctionName[]` entries to `:on_alarm` hook names. Wire `self`-table persistence across save/load via `mc2.persist`.

**Gate:** brain receives `hit_by_weaponfire` alarm and transitions to engage. Save mid-mission, reload, brain resumes in correct state with target intact.

### F-4 — Behaviour-tree helper

`mc2.ai.tree` selector/sequence/condition/action primitives implemented in pure Lua atop F-1's DSL.

**Gate:** a behaviour-tree-authored brain produces equivalent behaviour to the FSM-authored reference brain on the four-verb scenario.

### F-5 — Fog-of-war modes + team-shared scan

Implement `fog_of_war: respect|ignore|team_shared` in `ctx.scan`.

**Gate:** a brain with `team_shared` correctly aggregates contacts from all friendlies on the team.

### F-6 — Reference brain pack

Ship 4 reference brains in `mods/example_ai/scripts/ai/`:
- `brain_assault.lua` (close-and-attack)
- `brain_scout.lua` (high-speed patrol + report-and-disengage)
- `brain_fire_support.lua` (long-range positioning)
- `brain_brawler.lua` (close-quarters specialist)

Modeled on BTA's role-mix design (see §14).

**Gate:** all four brains run a 5-minute test mission without crashes. Each demonstrates its role.

### F-7 — Documentation + JSON Schema

Publish `docs/lua-ai.md`, `mods/example_ai/`, and a `brain.schema.json` for `mod.json`'s `ai` block (so VS Code gives modders autocomplete).

**Gate:** a fresh modder can author a working brain in under an hour starting from the docs.

F-1 + F-2 are the minimum viable Track F. F-3 through F-7 close the gap to a usable v1.

---

## §14 — Free wins to adopt

1. **BTA3062 / RogueTech "role" tagging** as an organizing principle for brain selection. A mod's `team_brains` map can also use roles (`"assault"`, `"scout"`, `"fire_support"`, `"brawler"`) instead of brain IDs, with the engine resolving role → brain via the mod's role table. The reference brain pack in F-6 ships role-named brains as the canonical mapping. (Source: BTA3062 wiki, role-based lance-composition rules.)
2. **RogueTech behaviour-tree DSL shape** — selectors / sequences / conditions / actions / decorators — adopted verbatim by `mc2.ai.tree`. The naming is convergent across the BT/MW5 modding scene; we'd alienate authors by inventing our own. (Source: RogueTech `behaviour_*.json` files; `BattletechModders/RogueTech` repo.)
3. **MW5 TacOps "stance" tags** (`advance_aggressive`, `cautious`, `hold_ground`, `withdraw`) as a `ctx.set_stance(name)` convenience that compiles to a tac-order tweak. v1.1; not v1. (Source: MW5 TacOps mod's stance system.)
4. **YAML's `equipment-properties.json` shape** for declaring brain-affecting passive modifiers (e.g. a "command-mech" quirk that boosts nearby friendlies' brain decision quality). v1.1+. (Source: `mw5mercs-modding/yaml-docs`, equipment-properties schema.)
5. **Coyote's Mission Pack "encounter table" pattern** for spawning AI lances on demand inside a mission. Composes with our `mc2.lance.compose` (from `track-c-battletech-mw5-mod-scene-research.md` Story 2). The brain side just needs to handle "mid-mission attach" gracefully — already covered by F-1's `setBrain` routing. (Source: Coyote's Mission Pack source on Nexus.)
6. **Skill Tree Rebuild's "abilities consume brain ticks" pattern** — pilot abilities can register tick handlers separately from the brain. Lets modders add per-pilot specials without forking the brain DSL. v1.1 add via `mc2.ai.ability(name, handler)`. (Source: HBS BT Skill Tree Rebuild + AbilityRealizer DLL.)

---

## §15 — Open questions

1. **Brain hot-reload mid-mission.** F-1 ships hot-reload at mission boundaries. Mid-mission hot-reload requires deciding what happens to brain state. Drop and re-init? Preserve `self` tables and re-attach? Likely answer: drop; modders save state via `mc2.persist` if they want survival.
2. **Multi-mod brain conflicts.** Two mods both list `controls_teams: [2]` — who wins? Probably last-loaded-by-Kahn-order, with a warning logged. Needs explicit decision before F-1.
3. **Stock-mission overrides.** A mod wants to take over enemy AI in stock `mc2_07`. Does it need to declare per-mission? Current §11 says "list the team in `controls_teams`, accept the regression burden." Worth a per-mission override field for finer control: `ai.team_overrides_per_mission`. Defer to F-7.
4. **Brain-vs-brain combat consistency.** When two Lua-controlled teams fight each other, both brains' decisions are deterministic w.r.t. their inputs but their *interleaving* with the engine tick is nondeterministic. Acceptable for SP. Needs explicit answer if MP ships.
5. **Action-conflict policy refinement.** §8 says last-call-wins per slot. What about `move_to(A); fire_at(B)` then a transition-to-different-state in the same tick that doesn't override either? Probably: state body is the only thing that runs per tick; transitions are evaluated *after* the body. Lock down the order.
6. **Brain DSL vs raw Lua.** Should we mandate the `:state` / `:transition` DSL, or accept raw `function(ctx) ... end` brains? Current preference: DSL is the public API; raw is allowed via `brain:body(fn)` for power users. Confirm at F-1.
7. **Per-warrior brain override.** Sometimes a modder wants Hero Mech X to use a different brain than the rest of the team. Add a per-warrior `ai_brain` field in the warrior `.fit` or in the mission file? Defer to F-3.
8. **Scoring/utility-AI helper.** `mc2.ai.score` shape — pure-Lua scorer-list-with-weights, or a more structured DSL? Defer to v1.1.
9. **Performance budget enforcement.** If a brain consumes >1 ms in a single tick, do we kill it? Throttle? Log? Track C's per-mission opcode budget (5M opcodes/tick from `lua-sandbox-and-errors.md`) applies, but a brain can still busy-loop within budget. Likely answer: hard timeout via Lua hook every N opcodes, error-out the brain, log, fall back to a no-op idle brain.
10. **Brain-side debugging.** Modders want a step-by-step inspector of "which state did this warrior just enter, why did the transition fire, what did `ctx.scan` see?" Needs an in-game ImGui panel — Track B integration. Defer to F-7.

---

## §16 — Track F's relationship to other tracks

**Depends on:**
- **Track C-1 through C-3** — Sol2 + Lua VM + the ~85 STABLE bindings + the deferred-event drain + namespace lock. Track F is a major *consumer* of Track C; it cannot start until C-3 ships.
- **Track C reverse-direction primitives** (`abl-to-lua-reverse-direction.md`) — used for the Option B engine-emit pattern when alarms fire from the engine into Lua. Track F's alarm dispatch uses the same engine-side `Dispatch` function the reverse-direction doc designs.
- **Mod manifest schema** (roadmap §5.2) — the `ai` block adds new fields. Coordinate with whatever lands the manifest validator.

**Enables:**
- **Total conversions.** With Track F in place plus Track D (Assimp mech import) and Track E (JSON manifests for stats), a 3025-era BattleTech total conversion can ship as pure data + Lua + `.glb` and zero ABL. The two attempts in `memory/carver5_mission_playable.md` and `memory/mco_omnitech_integration_attempt.md` would have unblocked at this point.
- **Mods that augment-rather-than-replace stock AI.** A mod can list `controls_teams: []` and instead just register `mc2.events.on("AI.WarriorTick", ...)` handlers via the reverse-direction primitives — once Option C's corebrain-patch infrastructure ships in a future M-slice. Track F provides the *replacement* path; reverse-direction provides the *augmentation* path. They compose.
- **Opt-in difficulty mods.** TTRulez_AIMod-class mods (smarter target selection, better positioning) ship as Lua brains the player can swap in without forking the engine.

**Does not depend on:**
- **Track D (Assimp mech importer).** The brain works just as well on stock `.ase` mechs as on imported `.glb` mechs. They often pair in TC mods, but neither blocks the other.
- **Track E (JSON manifests).** Brains can be authored against stock content. Most TC mods will pair Track F with Track E to also ship custom mech stats, but the brain itself is stat-agnostic.
- **Track B (ImGui).** A brain debugger panel is a nice-to-have (F-7's open question), not a v1 requirement.

**Sequencing recommendation.** Ship Track F-1 + F-2 *immediately* after Track C-3 lands. The two together unlock the most-requested capability in the BT/MW5 modding scene (per §5 of `track-c-battletech-mw5-mod-scene-research.md`), and they share enough of Track C's infrastructure that the engine work is essentially additive — no new VM, no new dispatch path, no new event system. Just one fork in `runBrain()` and a Lua-side DSL.

---

## §17 — References

**Sister specs and explorations in this worktree:**
- `specs/2026-04-29-modders-paradise-roadmap-design.md` (parent roadmap)
- `explorations/2026-04-30-track-c-battletech-mw5-mod-scene-research.md` (the gap that surfaced this track; §4 Story 5 + §5 #8)
- `explorations/2026-04-30-track-c-blocking-questions-resolution.md` (`*_impl` extraction pattern; magicpatrol shadow rule)
- `explorations/2026-04-30-track-c-abl-to-lua-reverse-direction.md` (`mc2luadispatch_*` family)
- `explorations/2026-04-30-track-c-lua-loading-lifecycle.md` (per-mission VM, deferred drain)
- `explorations/2026-04-30-track-c-lua-api-surface-catalog.md` (binding tier classification)
- `explorations/2026-04-30-track-c-lua-trampolines-and-tests.md` (perf numbers)
- `specs/2026-04-30-battletech-modder-conventions-design.md` (modder-facing conventions)

**Engine code referenced (file:line):**
- `code/warrior.cpp:155-175` — `pilotAlarmFunctionName[]` 19-entry table.
- `code/warrior.cpp:222` — `BrainUpdateFrequency = 2.25` (the load-bearing tick rate).
- `code/warrior.cpp:284` — `brainsEnabled[MAX_TEAMS]` per-team gate (8 teams).
- `code/warrior.cpp:2083-2120` — `setBrainName` + `setBrain`: per-warrior brain allocation lifecycle.
- `code/warrior.cpp:2124-2205` — `runBrain()`: per-tick execution body, the primary fork point for §10.
- `code/warrior.cpp:2150-2156` — `CurGroup`/`CurObject`/`CurWarrior` global setup before `brain->execute()`.
- `code/warrior.cpp:2160` — `brain->execute()` (the dispatch we replace for opted-in warriors).
- `code/warrior.cpp:4675` — alarm-driven `brain->execute(NULL, brainAlarmCallback[code])` (immediate alarm path).
- `code/warrior.cpp:4735-4855` — `checkAlarms()`: full alarm dispatch loop (the second fork point for §10).
- `code/warrior.cpp:4838` — alarm-driven `brain->execute()` (queued alarm path).
- `code/warrior.cpp:4974-4977` — the `brainUpdate <= scenarioTime` gate that drives the 0.44 Hz tick.
- `code/mission.cpp:2275-2278` — stock `corebrain.abx` load (untouched by Track F).
- `code/mission.cpp:2371-2395` — per-warrior `.abl` brain preprocess + `setBrain` call.
- `code/saveload.cpp:1023` — corebrain reload on save-game restore.

**Memory entries:**
- `memory/magic_abl_contamination_rule.md` (load-bearing — stock corebrain.abx is byte-immutable)
- `memory/carver5_mission_playable.md` (TC attempt #1; AI-brain blocker)
- `memory/omnitech_abl_stubs_session.md` (TC attempt #2; AI-brain blocker)
- `memory/mco_omnitech_integration_attempt.md` (TC attempt #3; AI-brain blocker)

**External references:**
- BTA3062 role-based AI: `github.com/Syrkres/BTA3062`
- RogueTech behaviour trees: `github.com/BattletechModders/RogueTech` (search for `behaviour_*.json`)
- TTRulez_AIMod3: `nexusmods.com/mechwarrior5mercenaries/mods/269`
- HBS BT Skill Tree Rebuild + AbilityRealizer: `nexusmods.com/battletech/mods/647`, `github.com/BattletechModders/AbilityRealizer`
