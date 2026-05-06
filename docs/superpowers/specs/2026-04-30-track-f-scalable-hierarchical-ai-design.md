# Track F — Scalable Hierarchical AI Design

**Status:** Architecture design (supersedes the parallel `2026-04-30-track-f-ai-replacement-design.md`, which becomes the engine-AI inventory / foundational reference).
**Date:** 2026-04-30
**Author:** AI architecture pass following user direction toward a SupCom-style hierarchical, modder-rich AI.

> "We want something scalable. SupCom-style unit structures. Think of if we had individual pilots with their own decision weightings, and each mech also had things it was better or worse at. I want it to be scalable and moddable. We are freeing up CPU time elsewhere so AI can be more complicated."

This document specifies the *target* architecture for the AI replacement. Implementation lands in slices F-1..F-9 (§13). It does not commit any code; the matching foundational inventory doc covers what the engine currently feeds the per-warrior `brain->execute()` site (`code/warrior.cpp:2160`), the corebrain.abx ABL VM, and team-level stock AI in `code/team.cpp`.

> **⚠️ §4 (pilot personality) and §5 (chassis affinity) are reframed by the modifier-registry decision (2026-04-30).** Per [`../explorations/2026-04-30-track-c-modifier-registry-decision.md`](../explorations/2026-04-30-track-c-modifier-registry-decision.md) §9, MC2R ships a hybrid modifier registry in v1 that subsumes quirks, abilities, weapon-component tags, pilot personalities, **and chassis affinities** under a single primitive: `(stat, op ∈ {add,mul,set_floor,set_ceil}, value, named_predicate?, permanent|mission)`. The 20-dimension pilot weight catalog (§4) and 10-dimension chassis affinity table (§5) below should be read as **modifier sources that emit registry tuples**, not as bespoke standalone systems. The composition rule "pilot.weights × chassis.affinity_modifier" becomes natural multiplicative stacking via the registry's `op = mul`. Concrete reframe: a pilot personality JSON file becomes `{ "id": "aggressive", "modifiers": [...] }` rather than its own bespoke `weights{}` block; chassis affinity files do the same. Same data, same multiplicative semantics, single primitive — six consumers (quirks, abilities, weapon tags, personalities, affinities, faction passives) all unify.

> **⚠️ Tick-rate resolution (2026-04-30, post-F-1).** The engine's actual brain tick rate is **0.44 Hz** (`BrainUpdateFrequency = 2.25` at `code/warrior.cpp:222`, gate at `:4974-4977`), not 30 Hz as this doc's §2 and §11 originally specified. **Resolved direction (user input):** ship F-1/F-2 with the engine's native 0.44 Hz periodic + event-driven alarm callbacks (the 19 `pilotAlarmFunctionName[]` entries at `:4735-4855`). After validation, scale up to **1–2 Hz periodic** if richer responsiveness is wanted. The 30 Hz figure in §2 (Layer 4 — Unit) and §11 (perf budget) should be read as "target ceiling for richest AI"; the v1 default is the engine's native 0.44 Hz. At native rate, performance is ~140× cheaper than §11's table — well under any realistic budget. Sensor batching and threat heatmap rates from §11 still apply as-is. See [`2026-04-30-track-f-ai-replacement-design.md`](2026-04-30-track-f-ai-replacement-design.md) §"engine inventory" for the existing dispatch sites.

---

## §1 — Goal and design constraints

**Primary goal.** Replace MC2's per-warrior ABL `corebrain.abx` interpretation with a four-layer, hierarchical AI framework where each layer is data-driven, hot-reloadable, and Lua-replaceable per mod. Pilot personalities and mech chassis affinities compose multiplicatively to produce emergent variety from a small library of behaviors.

**Hard constraints.**

- **Scale target:** 100+ warriors at 30Hz tick on a representative mid-tier CPU (i5-class, 2020-era).
- **CPU budget:** ≤5 ms/frame total AI cost at saturation. Justified by Phase-A/B render headroom (terrain CPU pressure has been moved off the critical path; see `m2_thin_record_cpu_reduction.md` and `2026-04-29-track-a-render-headroom-status.md`).
- **Hierarchy:** Four layers (Strategic / Tactical / Operational / Unit) with explicit, narrow inter-layer contracts. No layer reaches around its neighbor.
- **Modding:** Each layer's logic is Lua-replaceable per-mod. Pilots, personalities, mech archetypes, behavior trees, lance compositions are data-driven JSON, hot-reloadable.
- **Stock-mission compatibility:** Stock missions must still play with stock AI. The mod AI is opt-in per-team (`team.aiBrain = "mod:hbs_3025/aggressive"`), and absent any opt-in the legacy ABL path runs unmodified — same semantic as Track C's reverse-direction event seam (see `2026-04-30-track-c-abl-to-lua-reverse-direction.md`).
- **Determinism:** v1 is single-threaded main-thread, deterministic enough for replays. Future-proofing for MP is in §12.
- **No new contamination:** This must not regress the magic-ABL-contamination rule (`memory/magic_abl_contamination_rule.md`) — the Lua AI runs *alongside* the stock corebrain.abx, never replacing the shipped binary asset, and only loaded when a mod registers a brain for a team.

**What "scalable" means here.** Adding a new pilot personality, a new mech chassis affinity profile, or a new behavior tree must be a single JSON or Lua file ship, no engine recompile. Adding a new strategic doctrine for a TC mod must not require touching tactical or operational logic.

---

## §2 — The four-layer architecture

Each layer is a tick-rate-distinct decision phase. Layers are *strict* — a higher-numbered layer never observes lower-frequency state directly; it consumes the *contract output* of the layer above it. This is the trick that bounds total cost: cheap layers run often, expensive layers run rarely.

### Layer 1 — Strategic AI (per team / per faction)

- **Tick rate:** 1 Hz (every 30th engine tick).
- **Scope:** One brain per *team* in the mission. Teams = factions in MC1-MC2 parlance.
- **Input state:** Mission objectives, team-wide map control (control points captured), faction relationships (allied/hostile), global threat heatmap (1Hz update from sensor fusion), contract context (offensive vs defensive vs raid), elapsed time, casualty ratio.
- **Output decisions:** A small ordered list of `StrategicOrder` records — high-level intents like *push north corridor*, *defend objective 2*, *retreat to LZ*, *withdraw and regroup*.
- **Modder API:** `mc2.ai.strategic.brain("brain_id")` declarative state machine.
- **Example:** A *defensive* strategic AI prioritizes holding objectives; an *aggressive* one pushes toward enemy concentration; a *mercenary* one weighs salvage opportunities and routes to disabled enemy mechs.

### Layer 2 — Tactical AI (per lance / per platoon)

- **Tick rate:** 3 Hz (every 10th engine tick).
- **Scope:** One brain per lance. A 100-warrior battle = ~25 lances.
- **Input state:** Strategic order from L1, lance composition (chassis IDs and pilot IDs), local enemy contacts (sensor-confirmed within ~1500m), terrain summary (elevation samples, cover hints from terrain classifier), lance cohesion / damage distribution.
- **Output decisions:** Formation (line, wedge, vee, column, scatter), per-warrior role assignment (point, anchor, flanker, overwatch, ammo-conservation, medic-target), target priority queue (3–5 ranked enemies), fire allocation matrix (who shoots whom), retreat trigger (lance damage threshold + composition asymmetry).
- **Modder API:** `mc2.ai.tactical.lance("doctrine_id")` with role allocation.
- **Example:** A *fire support* tactical AI assigns long-range chassis (Madcat, Catapult) to overwatch positions and brawler chassis (Hunchback, Centurion) to a screening line.

### Layer 3 — Operational AI (per warrior, behavior-tree-based)

- **Tick rate:** 10 Hz (every 3rd engine tick).
- **Scope:** One BT instance per warrior.
- **Input state:** Tactical assignment from L2, current target list, weapon load + heat + ammo, position and velocity, sensor data (visible enemies and bearings), pilot personality weights, chassis affinities.
- **Output decisions:** Currently-selected behavior — `engage`, `pursue`, `break_off`, `cool_down`, `seek_cover`, `use_special` (jump, special weapon, ECM toggle).
- **Modder API:** `mc2.ai.operational.behavior("name", tree)` building behavior trees from primitive nodes.
- **Example:** A *brawler* behavior tree — close distance → fire all weapons → re-engage if heat low → break off if armor < 30%.

### Layer 4 — Unit AI (per warrior, low-latency)

- **Tick rate:** 30 Hz (every engine tick).
- **Scope:** One per warrior; engine-side primitive translator.
- **Input state:** Behavior order from L3, immediate state (heat now, ammo now, damage now, cockpit critical flags).
- **Output decisions:** Concrete actions translated into engine calls — `move_to(point)`, `fire_weapon_group(g)`, `eject()`, `jump_to(point)`, `power_down()`.
- **Modder API:** Usually *not* directly modded; engine primitives handle translation. Modders only override for special cases — Clan honor codes, OmniMech weapon-pod swaps, salvage-priority eject logic.
- **Example:** Emergency-eject when cockpit damage critical AND pilot personality `risk_aversion` > 0.7.

---

## §3 — Inter-layer contracts

Concrete C++ struct shapes define the seams. Layers communicate *only* through these.

```cpp
// L1 → L2 : strategic intent, refreshed at 1 Hz
struct StrategicOrder {
    enum class Type { Hold, Push, Withdraw, Flank, Recon, Salvage, Escort };
    Type     type;
    Vec3     anchor;          // world-space focal point
    float    radius;          // engagement radius around anchor
    int      priority;        // 0..100; ties broken in declaration order
    int      objectiveId;     // link to mission objective if applicable
    uint32_t orderId;         // monotonic; lances dedupe on this
};

// L2 → L3 : per-warrior tactical assignment, refreshed at 3 Hz
struct TacticalAssignment {
    enum class Role { Point, Anchor, Flanker, Overwatch, Sniper, Screener, Reserve };
    Role            role;
    WarriorHandle   primaryTarget;     // 0 = no preferred target
    Vec3            stationOrZone;     // hold here, or zone center
    float           stationRadius;
    uint8_t         fireAllocation[4]; // bitfield of approved target indices in lance target queue
    uint32_t        assignmentId;
};

// L3 → L4 : behavior selection, refreshed at 10 Hz
struct BehaviorOrder {
    BehaviorId behavior;       // FNV1a of behavior name; e.g. "brawler::close_and_alpha"
    Vec3       hint;           // behavior-defined hint (target pos, retreat point, etc.)
    float      params[4];      // behavior-defined scalar params
    uint32_t   orderId;
};

// L4 → engine : concrete action, every tick
struct UnitAction {
    enum class Type { Move, FireGroup, Jump, Eject, PowerDown, Special, Idle };
    Type     type;
    Vec3     target;
    uint32_t flags;            // weapon group bits, special-id, etc.
};
```

**Upward event flow.** Layers emit events that flow *up* the hierarchy through a typed event bus.

```cpp
struct AIEvent {
    enum class Kind {
        WeaponDestroyed, AmmoExhausted, HeatCritical, Damaged, TargetLost,
        TargetKilled, ContactGained, ContactLost, OrderComplete, OrderFailed,
        EjectionImminent, AssignmentAck, LanceBroken, ObjectiveSecured
    };
    Kind          kind;
    WarriorHandle src;
    uint32_t      relatedOrderId;
    float         payload[2];
};
```

L4 reports `WeaponDestroyed` → L3's BT may reroute to `cool_down`; L3 reports `OrderFailed: cannot_engage` → L2 may reassign roles; L2 reports `LanceBroken` → L1 may rewrite the strategic order ("withdraw" instead of "push").

Events are buffered and drained at the receiving layer's next tick — never synchronous up-calls. This bounds re-entrance and keeps tick costs predictable.

---

## §4 — Pilot personality system

Each pilot owns a vector of decision weights that bias every layer. Weights are *multipliers and offsets* applied to BT condition evaluations, not hardcoded branches.

```json
// mods/<id>/data/pilot_personalities/aggressive.json
{
  "id": "aggressive",
  "name": "Aggressive",
  "description": "Closes range, prefers engagement, low retreat threshold.",
  "weights": {
    "engage_threshold":     0.30,
    "retreat_threshold":    0.85,
    "range_preference":     "close",
    "heat_tolerance":       0.90,
    "ammo_conservation":    0.20,
    "cooperation":          0.50,
    "risk_aversion":        0.20,
    "target_priority":      "biggest",
    "patience":             0.25,
    "morale":               0.85,
    "discipline":           0.55,
    "honor":                0.40,
    "improvisation":        0.70,
    "self_preservation":    0.30,
    "tactical_awareness":   0.60,
    "fire_discipline":      0.40,
    "flanking_preference":  0.70,
    "support_inclination":  0.20,
    "lance_loyalty":        0.65,
    "vendetta_susceptibility": 0.55
  }
}
```

**Weight catalog (~20 dimensions).**

| Weight | Range | Meaning |
|---|---|---|
| engage_threshold | [0..1] | Lower → engages more readily; combined with chassis range affinity |
| retreat_threshold | [0..1] | Damage % at which retreat becomes considered |
| range_preference | enum | close / medium / long; chassis affinity may override |
| heat_tolerance | [0..1] | High → willing to redline; gates `cool_down` BT branch |
| ammo_conservation | [0..1] | Low → fires LRMs at single targets; high → holds for groups |
| cooperation | [0..1] | Formation discipline / how strictly L2 station is held |
| risk_aversion | [0..1] | Eject threshold, cover preference |
| target_priority | enum | biggest / weakest / closest / nearest_completion / vendetta |
| patience | [0..1] | Willingness to sit in overwatch vs initiate |
| morale | [0..1] | Modifies retreat & rout thresholds globally |
| discipline | [0..1] | Probability of accepting tactical reassignment |
| honor | [0..1] | Clan-flavor: refuse multi-on-one, refuse to fire on already-engaged target |
| improvisation | [0..1] | Likelihood of overriding L3 BT with opportunistic action |
| self_preservation | [0..1] | Cockpit-eject threshold; orthogonal to risk_aversion |
| tactical_awareness | [0..1] | Sensor sweep priority, contact-loss reaction speed |
| fire_discipline | [0..1] | Hold fire until target solution / chain-fire vs alpha |
| flanking_preference | [0..1] | BT bias toward `flank` over `engage_direct` |
| support_inclination | [0..1] | Bias toward defending damaged lancemate over self-objective |
| lance_loyalty | [0..1] | Refuse orders that abandon a damaged lancemate |
| vendetta_susceptibility | [0..1] | Sticks on a target that damaged the pilot |

**Composition rule.** Weights act as biases multiplied (or added with clamp) into BT condition scores.

```lua
-- in operational behavior tree:
node:condition("should_engage", function(ctx)
    local d = ctx.distance_to_target()
    -- pilot biases base threshold; chassis biases the spread
    local base = 200 + (1 - ctx.pilot.engage_threshold) * 300            -- [200..500]
    local affinity = ctx.chassis.engagement_modifier or 1.0              -- chassis nudge
    return d < base * affinity
end)
```

Modders ship pilot personalities by dropping JSONs into `mods/<id>/data/pilot_personalities/`. Engine validates schema at mod-load and registers them with `mc2.prototypes.register("pilot_personality", ...)`.

---

## §5 — Mech chassis affinity system

Each chassis ships an affinity profile describing what behaviors *play to its strengths*. Chassis affinities and pilot personalities compose multiplicatively — an aggressive pilot in a long-range chassis still presses, but engagement *range* is biased outward.

```json
// mods/<id>/data/chassis_affinities/madcat.json
{
  "chassis_id": "madcat",
  "primary_role": "fire_support",
  "secondary_role": "skirmisher",
  "affinity": {
    "preferred_range":              "long",
    "engagement_modifier":          1.4,
    "mobility_class":               "medium",
    "heat_management_difficulty":   0.7,
    "armor_distribution":           "balanced",
    "weapon_payload":               "heavy",
    "alpha_strike_efficiency":      0.85,
    "sustained_fire_efficiency":    0.65,
    "close_combat_penalty":         0.40,
    "behavior_synergies":     ["overwatch", "standoff", "alpha_strike", "kite"],
    "behavior_anti_synergies":["brawl", "rush", "harassment"]
  }
}
```

**Affinity dimensions (~10).**

| Dimension | Type | Meaning |
|---|---|---|
| preferred_range | enum | close / medium / long |
| engagement_modifier | float | Multiplied into pilot engage threshold |
| mobility_class | enum | light / medium / heavy / immobile |
| heat_management_difficulty | [0..1] | High → BT prefers `cool_down` more often |
| armor_distribution | enum | balanced / front-heavy / back-heavy / fragile |
| weapon_payload | enum | light / medium / heavy / mixed |
| alpha_strike_efficiency | [0..1] | Bias toward `alpha_strike` in BT selector |
| sustained_fire_efficiency | [0..1] | Bias toward `chain_fire` |
| close_combat_penalty | [0..1] | Penalty in BT score for `close_and_engage` |
| behavior_synergies / anti_synergies | string list | BT selector adds/subtracts score weight |

**Composition with personality.**

```
effective_engage_distance =
    base_engage_distance(pilot.engage_threshold)
    * chassis.engagement_modifier
    * (1 + pilot.range_preference_score(chassis.preferred_range))
```

`range_preference_score` returns +0.2 if pilot and chassis agree, –0.3 if they disagree. So a *close-preferring aggressive* pilot in a *long-range Madcat* hits a compromise mid-range engagement profile — emergent variety from two simple data files.

Modders add chassis by dropping JSONs into `mods/<id>/data/chassis_affinities/<chassis_id>.json`.

---

## §6 — Behavior tree library

Behavior trees implement L3 (operational). Three options:

**Option A: Vendor [BehaviorTree.CPP](https://github.com/BehaviorTree/BehaviorTree.CPP) v4 (MIT).**
- Pros: mature, ROS-tested, XML+code definitions, blackboard system, port specs, async leaves, runtime introspection (Groot debugger).
- Cons: Adds a 3rdparty dep with its own CMake; XML node format is ROS-ish.

**Option B: Hand-roll a Lua BT library.**
- Pros: zero new C++ dep; modders see the source; trivial hot-reload.
- Cons: We'd reinvent ports, blackboards, async, decorators; ~1.5–2k LoC of carefully-written Lua before parity.

**Option C: Hybrid — C++ engine-side BT runtime, Lua DSL that compiles to runtime nodes.**
- Pros: tick cost stays in compiled code (helps the §11 budget); modders write declarative Lua.
- Cons: more upfront work; requires a stable IR between Lua DSL and C++ runtime.

**Recommendation: Option C, scoped tightly.** Start with a small (~12–15 node) C++ BT runtime (Sequence, Selector, Parallel, Inverter, Succeeder, Repeater, ConditionLeaf, ActionLeaf, plus three or four BT-specific decorators). The Lua DSL compiles to a flat IR (a `vector<NodeRecord>`) at mod-load, which the engine ticks. Hot-reload means recompiling the Lua DSL → IR; the runtime never JITs.

Why the DSL compiles to IR rather than Lua-driving each tick: tick cost. At 100 warriors × 10 Hz, BT ticks dominate. Each leaf condition / action *callback* can still be Lua (one Lua call per leaf eval), but the *traversal* runs in C++ — that's where the savings live.

---

## §7 — SupCom AI patterns to borrow

**Sorian AI** (FAF) and **M27 AI** (FAF) both extend SupCom2's PlatoonAI. The patterns transfer:

- **PlatoonAI shapes** — `LandAttackAI`, `AirAttackAI`, `ScoutAI`, `EngineerAI` map directly to our L2 lance archetypes: `BrawlerLance`, `FireSupportLance`, `ScoutLance`, `RecoveryLance`.
- **TaskGroup / TaskWatcher pattern** — yield-based scheduling. Each L2 lance's tactical brain runs as a coroutine that yields between micro-decisions. Ports cleanly to Lua (coroutines are first-class) and lets long-running tactical logic be written sequentially without blowing the per-tick budget.
- **GlobalAI threat assessment** — per-grid-cell threat heatmap. Adopted: a 64×64 `float` grid covering map extent, updated at 1 Hz from sensor confirms. L1 reads it for "where is the danger"; L2 reads it for "is my station threatened."
- **Aggressive build order priority list** — direct analog: tactical fire-allocation priority. Build a "target priority list" the way Sorian builds a "build priority list."
- **Auto-disable on repeat failure** — borrowed from Spring as well. If a Lua brain throws or returns ill-formed output 3+ times in 30 s, demote that team's brain to legacy ABL for the rest of the mission and surface a console warning. Same disposition as Track C's mod sandbox failure mode.

---

## §8 — BattleTech HBS AI patterns to borrow

BattleTech 2018 (HBS) and its mod ecosystem (RogueTech, BTA 3062, BEX) have an AI layer worth raiding:

- **AIPersonality (sigil cards)** — attribute-bag personalities very similar to our §4 weight set. RogueTech ships 30+ AIPersonality JSON files. We adopt the data-driven shape and the naming convention.
- **Influence map** — same idea as the SupCom threat heatmap, but with friendly/enemy/objective layers. We collapse to one "threat" layer for v1, leaving headroom for a multi-layer extension.
- **Behavior tree library** — HBS uses BT for unit AI. We do the same at L3.
- **ForceComposition tags** — lances tagged `("Brawler", "FireSupport")` etc., used to seed L2 doctrine selection. Adopted as the *primary_role* / *secondary_role* fields on chassis affinities.
- **RogueTech custom AIPersonality mods** — direct precedent that modders *do* ship AI variety; they ship 20–30 personalities per major TC. Validates §4 data-driven approach.

---

## §9 — Modder API at each layer

Full Lua surface. All registrations happen at mod-load (Track C event seam), validated against schema, and stored in the prototype registry.

```lua
-- ==== Strategic ====
local s = mc2.ai.strategic.brain("hold_objective_3025")
s:initial_state("defending")
s:state("defending", function(ctx)
    -- emit StrategicOrder records for lances
    ctx:order_lance("alpha", { type = "hold", anchor = ctx.objective(2), radius = 400, priority = 80 })
    ctx:order_lance("bravo", { type = "flank", anchor = ctx.enemy_centroid(), radius = 200, priority = 60 })
end)
s:transition("defending -> counterattack", function(ctx)
    return ctx.enemy_morale_below(0.4) or ctx.lance_state("alpha"):secured()
end)
s:state("counterattack", function(ctx) ... end)

-- ==== Tactical ====
local t = mc2.ai.tactical.lance("fire_support_lance")
t:on_assignment(function(ctx, order)
    local roles = ctx:assign_roles_by_chassis({
        long_range  = "overwatch",
        medium      = "skirmish",
        close       = "screener",
    })
    ctx:set_formation("vee")
    ctx:set_target_priority(function(a, b)
        return a.threat_score > b.threat_score
    end)
end)

-- ==== Operational (behavior tree) ====
local op = mc2.ai.operational.behavior("brawler", function(b)
    return b:selector(
        b:sequence(
            b:condition("cockpit_critical", function(ctx) return ctx.cockpit_armor() < 0.1 end),
            b:action("eject")
        ),
        b:sequence(
            b:condition("heat_critical",   function(ctx) return ctx.heat() > ctx.pilot.heat_tolerance end),
            b:action("cool_down")
        ),
        b:sequence(
            b:condition("has_target",      function(ctx) return ctx.has_target() end),
            b:condition("should_engage",   function(ctx) return ctx.distance_to_target() < ctx.effective_engage_range() end),
            b:action("close_distance"),
            b:action("alpha_strike")
        ),
        b:action("hold_station")
    )
end)

-- ==== Pilot personality (data only) ====
mc2.prototypes.register("pilot_personality", "natasha_kerensky", {
    weights = { engage_threshold = 0.15, honor = 0.95, vendetta_susceptibility = 0.9, ... }
})

-- ==== Chassis affinity (data only, JSON file is canonical) ====
mc2.prototypes.register("chassis_affinity", "madcat", { ... })

-- ==== Wiring ====
mc2.ai.bind_team(team = 2, brain = "hold_objective_3025")
mc2.ai.bind_lance(team = 2, lance = "alpha", doctrine = "fire_support_lance")
mc2.ai.bind_warrior(warrior_id = 17, behavior = "brawler", personality = "natasha_kerensky")
```

The `ctx` parameter is the same hot-binding Track C uses (see `2026-04-30-track-c-lua-api-surface-catalog.md`); each layer gets a tightly scoped subset (`StrategicCtx`, `TacticalCtx`, `BehaviorCtx`).

---

## §10 — Modder example: a 3025-era TC mod's AI

A "3025 TC" modder (think MechWarrior Living Legends-flavored 3025-era content) ships approximately:

- **Strategic brains** (~5–10): `ic_defensive`, `ic_aggressive`, `merc_salvage`, `clan_zellbrigen`, `pirate_raid`, `lyran_assault`, `kurita_honor`, `davion_combined_arms`, `liao_deception`.
- **Tactical lance doctrines** (~8–12): `fire_support_lance`, `brawler_lance`, `scout_lance`, `command_lance`, `pursuit_lance`, `recovery_lance`, `ambush_lance`, `clan_star`, `lyran_heavy_lance`, `liao_death_commando`.
- **Operational behaviors** (~15–20): `brawler`, `sniper`, `skirmisher`, `anchor`, `support`, `scout`, `ambusher`, `pursuer`, `kiter`, `harasser`, `bodyguard`, `commander`, `spotter`, `flanker`, `last_stand`, `green_recruit` (panicky variant), `veteran` (steady variant), `clan_duelist`, `pirate_opportunist`, `merc_self_preservation`.
- **Pilot personalities** (~10–15): `aggressive`, `cautious`, `disciplined`, `green`, `veteran`, `elite`, `madman`, `coward`, `clan_warrior`, `is_lance_commander`, `mercenary`, `vendetta`, `honor_bound`, `panicked`, `cool_under_fire`.
- **Chassis affinities** (~30–50): one per stock + introduced chassis variant (Atlas-D, Atlas-K, Atlas-RS, Madcat-Prime, Madcat-A, Madcat-B, Madcat-C, Madcat-D, Catapult-K2, Catapult-A1, etc.).

**Volume estimate.** ~80–110 JSON files (4–8 KB each) and ~3000–5000 Lua LoC across strategic state machines, tactical doctrines, and BT definitions. Roughly comparable to a focused RogueTech AI mod release. Hot-reload means a modder iterates one personality at a time without restarting the mission.

---

## §11 — Performance budget

Target: **<5 ms/frame at 100 warriors saturated.** Decomposition:

| Layer | Tick rate | Instances at 100 warriors | Ticks/sec | Per-tick budget | Per-frame at 60 FPS |
|---|---|---|---|---|---|
| L1 Strategic | 1 Hz | ~2–4 teams | ~3 | 300 µs | 0.005 ms (amortized) |
| L2 Tactical  | 3 Hz | ~25 lances  | ~75 | 60 µs  | ~0.075 ms |
| L3 Operational | 10 Hz | 100 warriors | 1000 | 30 µs | ~0.5 ms |
| L4 Unit | 30 Hz | 100 warriors | 3000 | 15 µs | ~0.75 ms |
| Sensor fusion (shared) | 5 Hz | 1 (batched) | 5 | 1 ms | ~0.08 ms |
| Threat heatmap (shared) | 1 Hz | 1 | 1 | 800 µs | ~0.013 ms |
| **Total** | | | | | **~1.4 ms** |

That comes in well under 5 ms with ~70% headroom for Lua overhead, mod inefficiency, BT depth growth, and per-mod brain cost variance.

**Sensor scans batched at 5 Hz** — instead of every warrior LOS-testing every other warrior every operational tick, the engine produces a single "visible from each team" matrix at 5 Hz that all L3/L4 instances read.

**Threat heatmap updated at 1 Hz** — 64×64 grid; cost is dominated by the splat phase; ≪1 ms.

**Lua reentry cost.** Each BT leaf is one `lua_call`, which lands at ~0.5–1 µs in a warm Sol2 binding. A typical BT eval visits 4–8 nodes; budget 5 µs of Lua per warrior per L3 tick (50% of the per-tick number). Stays safe.

---

## §12 — Determinism / multiplayer constraints

v1 is single-threaded, main-thread-only. Future MP support requires:

- **Pinned RNG seed per AI tick** — each warrior owns a deterministic PRNG seeded from `(missionSeed, warriorId, tick)`; never reads from a shared `rand()`.
- **Deterministic floating-point math** — for distance / threat calculations, prefer a fixed-point integer space (millimeters for distances, basis points for percentages). Floats stay for cosmetic / tie-break decisions.
- **No wall-clock dependencies** — AI tick uses engine sim-tick count, never `gos_GetTimeInMicroseconds`.
- **Lua VM in deterministic mode** — table iteration order pinned (Sol2 supports this via `state::create_named_table` and avoiding `pairs()` over hash tables). All hash maps replaced with Lua arrays for ordered iteration.
- **No I/O on hot path** — disk reads / file logging are queued and drained off-tick.
- **Floating-point flush-to-zero / round-mode pinned** — set MXCSR at sim entry on all participating clients.

These constraints are *cheap to honor up front* and *expensive to retrofit*. v1 doesn't need MP determinism, but every architectural decision listed above must be compatible with it. Out of scope: lockstep network protocol, rollback, predictive correction.

---

## §13 — Sub-slices (F-1 through F-9)

Each slice is independently buildable, with a shipping gate.

- **F-1 — Engine-side AI framework scaffolding.** Four C++ layer interfaces (`IStrategicBrain`, `ITacticalBrain`, `IOperationalBrain`, `IUnitBrain`), the shared event bus, the prototype registry. Stock ABL still drives behavior; new framework runs alongside, no team binds it. Gate: stock smoke pass unchanged.
- **F-2 — Lua bindings for L4 (Unit).** Sol2 surface for `mc2.ai.unit.override(...)`. Modders can override emergency-eject and weapon-group selection. Gate: a test mod overrides eject threshold and visibly changes behavior.
- **F-3 — Lua bindings for L3 (Operational) + behavior tree library.** C++ BT runtime + Lua DSL compiler. Ship 3 example behaviors (`brawler`, `sniper`, `skirmisher`). Gate: test mod opts a single team into the new L3, mission completes correctly.
- **F-4 — Lua bindings for L2 (Tactical) + role assignment.** Lance doctrine API; ship 3 example doctrines. Gate: a 4v4 test scenario shows visible formation / role differences vs stock.
- **F-5 — Lua bindings for L1 (Strategic) + threat heatmap.** Strategic state machine API; threat heatmap producer; ship 2 example brains. Gate: full hierarchical mod runs a custom mission, no regressions on stock.
- **F-6 — Pilot personality registration + data-driven weights.** JSON loader, schema validation, hot-reload. Gate: changing a JSON personality and reloading visibly changes behavior in-mission.
- **F-7 — Mech chassis affinity registration + multiplicative composition.** JSON loader, composition rules with personality. Gate: same pilot in different chassis behaves visibly differently; same chassis with different pilots also differs.
- **F-8 — Stock-mission compatibility regression gate.** Fully exercise stock ABL on stock missions with the new framework loaded but unbound. Smoke matrix from `2026-04-23-smoke-test-matrix-design.md` runs green.
- **F-9 — 3025-era TC demo mod.** Author the full content stack from §10 as a reference mod; ships in `release_assets/sample_mods/`. Gate: a moderately complex TC mission plays start-to-finish entirely on the new AI, framerate >120 FPS at 100 units.

---

## §14 — Free wins to adopt

- **From Sorian AI (FAF):** PlatoonAI archetype set, TaskGroup/TaskWatcher coroutine pattern, repeat-failure auto-disable.
- **From M27 AI (FAF):** layered influence map (we collapse to one layer for v1, but keep the layout so a future contributor can ship multi-layer), threat-vs-economy weighting (collapsed to threat-vs-objective for MC2).
- **From BT-HBS AI (sigil cards):** AIPersonality data shape, ForceComposition role tags.
- **From RogueTech / BTA:** convention that mods ship 20–30 personalities is the *target shape*, not the ceiling.
- **From OpenRA / Factorio / Spring** (per `2026-04-30-track-c-openra-factorio-spring-borrowing.md`): event-bus dispatch shape, sandbox failure disposition (auto-demote on repeated error), data-driven prototype registry.
- **From [BehaviorTree.CPP] v4:** node typology, blackboard concept (we adopt as `ctx`), Groot-style introspection (deferred to a debug overlay sub-slice).

---

## §15 — Open questions

1. **BT decorator set.** Which decorators ship in v1? At minimum: `Inverter`, `Succeeder`, `Repeater`, `RetryUntil`, `Cooldown`, `Timeout`. Is `RandomChoice` worth shipping, given determinism constraints (§12)?
2. **Personality interpolation.** When two pilots eject and a NPC subaltern takes over, do we interpolate or copy? Copy is simpler; interpolate is more "alive."
3. **Chassis affinity authority.** If a modder ships an affinity JSON for a chassis that is *also* used by a stock mission, does the mod's affinity override stock behavior even when the team isn't mod-bound? Default: no — affinity registration is namespaced by mod, and the stock chassis runs with a built-in "neutral" affinity unless mod-bound.
4. **Salvage-priority eject logic.** Mercenary personalities want to preserve mech for salvage. Does eject decision live in L3 (behavior) or L4 (unit)? Recommendation: L4, with the *threshold* read from the personality.
5. **Heatmap update cadence at scale.** At 200+ warriors, 1 Hz heatmap update may need 30 ms of CPU. Do we degrade to 0.5 Hz, or shard the splat phase across frames?
6. **Coroutine yield contract.** Sol2's coroutine support is solid, but yield-points need to be *predictable* under deterministic mode. Audit needed.
7. **Mod conflict resolution.** Two mods register a personality with the same id. Last-one-wins, or first-one-wins-with-warning? Track E manifest is the source of truth — last-loaded wins.
8. **Stock corebrain.abx coexistence.** Can stock teams' L3 still call into ABL? *Yes* — there's a built-in BT leaf `legacy_abl_tick` that hands off to corebrain.abx for one ABL frame. Lets modders mix-and-match.
9. **Network-replay determinism.** Defer to v2; keep v1 architecturally compatible per §12.
10. **Tutorial pedagogical surface.** What does a modder's "hello world" AI look like? Recommend shipping a 30-line `tutorial/aggressive_blob.lua` that registers one personality, one BT, and binds team 2.
11. **Performance fallback.** If a saturated mission exceeds the 5 ms budget, do we drop tick rates first (degrade L2 from 3 Hz to 1.5 Hz), or skip warriors (round-robin)? Recommendation: drop tick rates uniformly; predictable degradation.
12. **Inter-team visibility for AI.** Does an AI brain see *what its sensors confirm*, or *ground truth*? V1: sensor-confirmed only. Cheating AI is bad TC content.
13. **Fortune-favors retreat.** When does a panicked pilot break formation? Lance loyalty vs self-preservation tie-break — which wins, and is that a personality dial or a BT decorator?
14. **Mod debug surface.** Track B (ImGui) is the natural home for a threat heatmap visualizer + lance role display + BT live-trace. Coordinate with Track B's debug overlay design (`2026-04-30-track-b-imgui-implementation-shape.md`).
15. **Lua brain memory profile.** 100 warriors × ~5 KB of personality + state per warrior + 25 lance brains × ~10 KB + 4 strategic brains × ~30 KB = ~1.0 MB resident Lua state. Acceptable; document for capacity planning.

---

## §16 — Track F's relationship to other tracks

- **Track C (Sol2 / Lua bindings)** is the *precondition*. Track F cannot start until F-1 has the binding seam, and F-2..F-7 depend on the Track C event-dispatch and prototype-registry shapes (`2026-04-30-track-c-abl-to-lua-reverse-direction.md`, `2026-04-30-track-c-lua-implementation-shape.md`).
- **Track E (data manifests)** owns mod scanning, JSON schema validation, and hot-reload. Pilot personality and chassis affinity JSONs ride this pipeline; F-6 / F-7 are mostly schema + registry plumbing on top of Track E.
- **Track D (Assimp mech importer)** is fully *independent*. New chassis imported via Assimp ship affinity JSONs alongside the mesh; the AI does not care how the geometry got there.
- **Track B (ImGui)** is the optional *debug surface*. A threat heatmap overlay, lance role labels, and a BT live-trace pane are the natural way to debug AI mods. Defer to a Track B sub-slice once F-5 lands.
- **Track A (render headroom)** is the *enabler*. Phase-A/B work moved terrain CPU pressure out of the way; the 5 ms AI budget assumes that headroom holds. Any regression on Track A directly compresses the AI budget.

---

**Outcome.** A four-layer hierarchical AI framework where modders ship ~80 JSONs and ~5000 Lua LoC to deliver a TC's worth of AI variety. Engine-side cost stays under 5 ms/frame at 100 warriors. Stock missions and stock corebrain.abx remain untouched. Pilots feel like people; mechs feel like instruments those people are playing.
