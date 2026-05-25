# Track C — Modifier Registry: v1, v2, or Hybrid?

**Date:** 2026-04-30
**Mode:** Research / design exploration. No code or schema changes proposed here.
**Predecessors:**
- [`2026-04-30-track-c-battletech-mw5-mod-scene-research.md`](2026-04-30-track-c-battletech-mw5-mod-scene-research.md) §3, §5.4
- [`../specs/2026-04-30-battletech-modder-conventions-design.md`](../specs/2026-04-30-battletech-modder-conventions-design.md) §B.4, §B.9, §C.4
- [`../specs/2026-04-30-track-f-scalable-hierarchical-ai-design.md`](../specs/2026-04-30-track-f-scalable-hierarchical-ai-design.md) §4–§5
- [`2026-04-30-track-c-lua-api-surface-catalog.md`](2026-04-30-track-c-lua-api-surface-catalog.md) §3
- [`2026-04-30-track-c-lua-trampolines-and-tests.md`](2026-04-30-track-c-lua-trampolines-and-tests.md) (perf reference)

The framing question, copied from the BT-conventions design spec §C.4: **several BT-IP mod features (quirks, pilot abilities, weapon component tags, mech chassis affinities, faction passives) all want a shared "modifier" abstraction. Should we ship a first-class modifier registry in v1, defer to v2, or hybrid?** This doc produces a decision the user can validate.

---

## §1 — What modifiers actually do

Concretely, a "modifier" is a named, stackable, optionally-conditional perturbation of a numeric (or sometimes enum) attribute on a target. Every BT-IP feature listed above can be expressed as a small set of modifiers attached to an entity, applied at query time.

**Modifier semantics break into five orthogonal axes.** Each axis needs a default and an explicit "corner case" rule:

1. **Operation kind.** Three useful primitives:
   - `add` — `attribute += value`
   - `mul` — `attribute *= factor` (multiplicatively combined when stacked)
   - `set_floor` / `set_ceiling` / `set` — `attribute = max/min/exact value` (used for caps, immunities, "armor cannot drop below 1")

   Default: every modifier declares its op kind explicitly. No magic.

2. **Stack semantics** when two modifiers target the same `(entity, stat)`:
   - **Additive bucket** — all `add` modifiers sum, then apply once. Two `+5` quirks → `+10`.
   - **Multiplicative bucket** — all `mul` factors multiply. Two `*0.9` → `*0.81` (not `*0.8`).
   - **Order**: add bucket applied first, then mul bucket, then floor/ceiling. This matches HBS BattleTech's convention and is the rule most BT modders know.

   Corner case: `set` ops short-circuit later modifiers in the same bucket *unless* they have higher `priority`. Document this loudly — it's the most common foot-gun.

3. **Conditionality.** A modifier may carry a predicate that is evaluated at query time. Examples: "only if target distance > 400m", "only if heat below 50%", "only if pilot has ability X". The predicate is a Lua callable or a registered named predicate (for fast paths). Default: no predicate = always-on.

4. **Temporality.**
   - **Permanent** — quirks, faction passives, chassis affinities. Live for entity lifetime.
   - **Per-mission** — pilot abilities like "first kill bonus" that reset on `Mission.End`.
   - **Timed** — heat penalty for N seconds; status effects.
   - **Per-action** — called shots, single-shot bonuses; auto-revert after one consumption.

   Default: `temporality: "permanent"`. Each non-permanent kind has an engine-side reaper (`Mission.End` hook, timer wheel, consumption counter).

5. **Application order / priority.** When two modifiers in the same bucket disagree about precedence (rare but real for `set` ops), an integer `priority` field (default 0) breaks the tie. Higher wins. This matches Spring/RTS engine convention and HBS's `Priority` field on `EffectData`.

The real complexity isn't any single axis — it's the *combinatorial* of (op kind × stack bucket × conditional × temporal × priority) at every stat read. This is why a generic registry is tempting *and* dangerous.

---

## §2 — BattleTech precedent (what RogueTech / BTA / HBS actually ship)

The HBS BattleTech codebase (and ModTek + Abilifier on top of it) is the closest reference. Three observations from the public docs and what's visible in the BT mod-scene research:

**HBS uses a generic `StatisticEffectData` shape** for every "modifies a number on something" feature. The abilifier / RogueTech / BTA stack all extend this same shape. The fields that matter (paraphrased from public Abilifier and BT 1.x community docs):

- `targetCollection` — what entity the effect attaches to (`Self`, `TargetActor`, `Creator`).
- `statisticName` — the stat to modify, e.g. `"HeatEfficiency"`, `"AccuracyModifier"`.
- `operation` — `Float_Add`, `Float_Multiply`, `Float_Replace`, `Set` (boolean), `Int_*` variants.
- `modValue` — the magnitude.
- `modType` — `System` (always-on), `LossOfInitiative`, etc. (mostly UI/category bookkeeping).
- `durationData` — how long: combat-end, mission-end, fixed-rounds, instant.

**Quirks reference `effectData[]`** — they're not bespoke. A quirk JSON is a thin wrapper around an array of `StatisticEffectData` records. RogueTech/BTA add 200+ quirks without modifying the engine because the shape composes.

**Pilot abilities use the same `effectData[]`** — that's the whole point of Abilifier. An ability JSON is `description + activation triggers + effectData[]`. The "modifier" abstraction is *already unified* in the HBS ecosystem; modders never had to pick "first-class registry vs ad-hoc per-system" because HBS made the choice for them.

**Status effects (heat penalties, called shots) use the same shape, with a `durationData` of `Round` or `Combat`.** Temporal modifiers were unified into the same registry from day one. The lesson is concrete: **if you don't do this, every modder reinvents it badly per-system.**

**One BT-specific oddity worth flagging:** HBS hardcodes the *list* of valid `statisticName` strings (it's a giant string-keyed dictionary on the `StatCollection` class). Modders who want a brand-new stat (e.g. "ShockTroopMoraleBonus") have to use a DLL to register it. That's a bug, not a feature, and we should avoid it: our registry should let mods declare new stats, not just new modifiers over a fixed stat list.

The MW5/YAML-side has a much weaker version of the same thing — `quirks.json` properties are a fixed enum (`Cooldown`, `Heat`, etc.) with no shared modifier object — and it shows. MW5 modders routinely ship YAML add-ons that work around the property enum being closed.

---

## §3 — The "first-class registry" v1 design

The Lua surface from the BT-conventions spec, fleshed out:

```lua
-- ===== modifier op registration (mostly engine-shipped, mods can add) =====
mc2.modifiers.register_op("add", { combine = "sum" })            -- engine default
mc2.modifiers.register_op("mul", { combine = "product" })        -- engine default
mc2.modifiers.register_op("set_floor", { combine = "max" })      -- engine default
-- mods can declare new ops only via the experimental tier (rare)

-- ===== stat declaration (mods add new stats; engine ships the core list) =====
mc2.modifiers.declare_stat("heat_dissipation", { default = 1.0, kind = "float" })
mc2.modifiers.declare_stat("to_hit_pct",       { default = 0.0, kind = "float" })

-- ===== quirk JSON references modifiers by stat + op + value =====
-- mods/<id>/data/quirks/torso_cooling.json:
{
  "id": "torso_cooling",
  "name": "Torso-Mounted Cooling Jacket",
  "modifiers": [
    { "stat": "heat_dissipation", "op": "mul", "value": 1.05 }
  ]
}

-- ===== pilot ability JSON, with a condition =====
{
  "id": "ace_gunner",
  "modifiers": [
    { "stat": "to_hit_pct", "op": "add", "value": 5,
      "condition": { "named": "target_in_optimal_range" } }
  ]
}

-- ===== runtime query =====
local eff = mc2.modifiers.effective(warriorId, "heat_dissipation")
-- engine walks attached modifiers, filters by condition, applies op order, returns scalar.
```

**Engine-side data structures** (sketch):

```cpp
struct ModifierOp { enum class Kind { Add, Mul, SetFloor, SetCeil, SetExact }; Kind kind; float value; int priority; };

struct ModifierInstance {
    StatId        stat;        // FNV1a("heat_dissipation")
    ModifierOp    op;
    int16_t       conditionId; // -1 = unconditional; else index into a predicate table
    uint8_t       temporality; // Permanent / Mission / Timed / OneShot
    uint16_t      sourceId;    // back-pointer for revert / introspection
    float         expiresAt;   // mission seconds; ignored unless Timed
};

struct ModifierBag {
    SmallVector<ModifierInstance, 8> mods;     // small-vec, most entities have <8
    mutable HashMap<StatId, float>    cache;    // resolved-stat cache
    mutable uint32_t                  generation;
    uint32_t                          dirtyGen; // bump on add/remove/condition-state-change
};
```

`ModifierBag` lives on `Warrior`, `MechObject`, `WeaponInstance`, `Team`, `Faction`. Same code path; same Lua surface. **One layer, five callers.**

**Condition table.** A small array of predicate fns, registered once at mod load. The 90% case is "always-on" (id = -1) and never costs anything. The 10% case is a Lua callable cached behind a registry id; the trampoline cost is the only Lua-touch (~80 ns per call per the trampolines doc).

**JSON schema** (`mods/<id>/data/quirks/<id>.schema.json`):
```json
{ "id":"string", "name":"string", "description":"string?",
  "modifiers": [{ "stat":"string", "op":"add|mul|set_floor|set_ceil|set",
                  "value":"number|bool", "priority":"int?",
                  "condition":{"named":"string","args":{}}?,
                  "temporality":"permanent|mission|timed|oneshot"? }] }
```

This is small, opinionated, and covers the BT precedent surface.

---

## §4 — The "deferred to v2" design

Without a registry, every system that wants modifiers writes its own. v1 surface looks like this:

```lua
-- quirks system: modders write per-quirk handlers in control.lua
mc2.on_event("Mech.HeatTick", function(mech)
    if mc2.quirk.has(mech.id, "torso_cooling") then
        mech.heat_dissipation = mech.heat_dissipation * 1.05
    end
    if mc2.quirk.has(mech.id, "another_cooling_quirk") then
        mech.heat_dissipation = mech.heat_dissipation * 1.10
    end
end)

-- pilot abilities: a different ad-hoc table
mc2.on_event("Combat.ToHitRoll", function(roll)
    local pilot = mc2.warrior.pilot(roll.shooter)
    if mc2.pilot.has_ability(pilot, "ace_gunner") and roll.in_optimal_range then
        roll.modifier = roll.modifier + 5
    end
end)
```

This is genuinely simpler in v1 — no schema, no registry, no caching layer, no condition table. A 200-line PR can ship it.

The price comes due fast. Concrete failure modes from the BT mod ecosystem when this approach was tried (pre-Abilifier, pre-CustomComponents):

- **No shared stacking rules.** Modder A's quirk multiplies, modder B's multiplies, but modder C did `heat_dissipation = 1.05` (replacement). The third quirk silently nukes the others. Bug reports flow to whichever quirk loaded last.
- **No introspection.** A debug command "what modifiers are affecting this mech's heat" can't exist generically — every system stores its modifiers differently.
- **No save/load of timed effects.** Each system writes its own persistence. Status effects half-survive saves, half-don't.
- **Per-event O(N) walks.** Every mod's quirk handler runs on every tick of `Mech.HeatTick`, even mechs that don't have its quirk. With 50 quirk-mods loaded, that's 50 redundant walks per tick.

The "add an event seam per system" approach scales linearly in surface area. 6 systems × 4 events each = 24 distinct event hooks for modders to learn, with no shared semantics. This is exactly the platform that BT modders escaped from when they built Abilifier.

**v2 deferral rules out the AI-layer unification of §7.** Pilot personalities can't share infrastructure with quirks if quirks don't have infrastructure.

---

## §5 — The hybrid (recommended)

**v1 ships a constrained modifier registry that nails the 80% case. v2 promotes the rest to STABLE.**

### v1 in scope

- **Stats.** Engine declares ~20 core stats (heat_dissipation, to_hit_pct, armor_back, accuracy, range_max, weapon_cooldown, movement_speed, sensor_range, eject_threshold, …). Mods can declare additional stats. Stats are float-typed only in v1.
- **Ops.** `add`, `mul`, `set_floor`, `set_ceil`. No `set_exact` in v1 (it's the foot-gun of §1; defer until a real use case needs it).
- **Stack semantics.** Add bucket → mul bucket → floor/ceil. Documented; no priority field in v1.
- **Conditions.** *Named, registered* predicates only — no inline Lua condition closures in v1. The named-predicate registry ships with ~10 BT-canonical predicates (`target_in_optimal_range`, `heat_below_50pct`, `armor_above_75pct`, etc.). Modders add more via `mc2.modifiers.register_predicate("name", fn)`.
- **Temporality.** `permanent` and `mission` only. No timed, no oneshot in v1.
- **Caching.** Dirty-flag cache on `ModifierBag`; recompute on add/remove/predicate-state-change.
- **Sources.** Quirks, pilot abilities, chassis affinities, faction passives all use it. Weapon component tags also use it (a tag is a named bag of modifiers).
- **Lua surface.** `mc2.modifiers.effective(entityId, statName) -> number`, `mc2.modifiers.add(...)`, `mc2.modifiers.remove(...)`, `mc2.modifiers.list(entityId) -> table`, `mc2.modifiers.declare_stat(...)`, `mc2.modifiers.register_predicate(...)`.

### v2 promotes to STABLE

- `set_exact` op + integer `priority`.
- `timed` and `oneshot` temporalities (timer wheel + consumption counters).
- Inline Lua condition closures (with the runtime cost called out).
- Int-typed and bool-typed stats (currently float-only).
- Modifier "tags" for filterable rollback (`mc2.modifiers.remove_by_tag("ace_gunner")`).
- `mc2.modifiers.register_op(...)` for new ops (mods that want bespoke stacking).

### Bright line for "this is v1 vs v2"

A v1 modifier is a tuple `(stat, op ∈ {add,mul,set_floor,set_ceil}, value, named_condition?, temporality ∈ {permanent,mission})`. **Anything that doesn't fit that tuple is v2.** This means a quirk like "if heat > 75, accuracy goes to zero" is v2 (it's a `set_exact`); a quirk like "armor back +10" is v1; a pilot ability like "+5% heat efficiency for 30 seconds after taking damage" is v2 (timed); "ace gunner: +5 to-hit when in optimal range" is v1 (named predicate, permanent).

The BT-canonical examples I checked from RogueTech and BTA mostly fit v1. Status effects (heat penalties, called shots) and named timed buffs are the v2 surface.

---

## §6 — Performance considerations

**Worst-case shape from the user prompt:** 100 warriors × ~30 stats × ~5 modifiers/stat = 15,000 stat lookups per frame. Naive: 15,000 × small-vec walk × predicate eval. A handful of µs even uncached, but multiplied by 30 Hz it's measurable.

**The actual budget.** Per the trampolines doc §perf, Sol2 Lua-to-C is 20–80 ns/call; per-warrior callbacks at 30 Hz already cost 0.1 ms/frame. The Track F doc budgets ≤5 ms/frame total AI cost; modifier resolution is a fraction of that.

**Caching strategy** (the only thing that matters at scale):

1. **Per-stat cached effective value.** First read computes; subsequent reads are O(1) hash lookup.
2. **Per-bag generation counter.** Adding/removing/expiring a modifier bumps the bag's generation. Cache stores (value, gen-at-compute). Cache hit when current gen matches.
3. **Predicate dirty-tracking.** This is the subtle bit. A "target_in_optimal_range" predicate is *recomputed every frame* — its truth depends on positions. Two paths:
   - **Frame-scoped predicate cache.** Predicate result computed once per frame per (predicate, entity) pair, reused across all stat lookups in the same frame.
   - **Skip predicate caching for hot stats.** For stats read more than once per frame per entity, cache the predicate result; for cold stats, evaluate inline.

   v1 ships path (1): a per-frame predicate result cache, ~20 entries per warrior in the worst case.

4. **No Lua call on the cached path.** A cached effective stat read is pure C++, never crosses the Lua/C boundary. This matters: the trampolines doc §perf gives us a budget where 100 warriors × 1 Lua call per frame is the comfortable ceiling for AI work; we cannot afford modifier resolution to add another 100×.

**Estimated cost at saturation** with v1 caching:
- Steady-state: ~0 ms/frame (cache hits, no recompute).
- Cache-invalidation event (mech takes damage, modifier added): one bag recompute on next read of any stat — ~10–30 µs total per warrior touched.
- Frame predicate recompute: ~5 named predicates × 100 warriors × ~50 ns each = ~25 µs/frame. Negligible.

**Risk:** the ad-hoc v2-deferral path of §4 has *no* caching layer — every `Mech.HeatTick` event handler re-walks every quirk every tick. v1 hybrid with caching is *cheaper* than v2-deferral at scale.

---

## §7 — Integration with the four AI layers

This is where the hybrid pays for itself — and where the user's intuition is correct.

**Pilot personality (Track F §4) is a modifier dictionary on decision-weights.** Today the spec hand-rolls a 20-key weight bag with custom shape. With the registry:

```json
{ "id": "aggressive",
  "modifiers": [
    { "stat": "engage_threshold",     "op": "set_floor", "value": 0.30 },
    { "stat": "retreat_threshold",    "op": "set_ceil",  "value": 0.85 },
    { "stat": "heat_tolerance",       "op": "add",       "value": 0.40 },
    { "stat": "vendetta_susceptibility","op":"add",       "value": 0.55 }
  ]
}
```

Pilot personalities become *just another modifier source* attached to the warrior. The 20-key weight bag is replaced by 20 declared stats + a pile of personality modifiers. Combine: stacking with chassis affinities is *automatic* — chassis affinities attach modifiers to the same stats, and the stack rules of §5 produce the multiplicative composition Track F §5 describes manually.

**Chassis affinity (Track F §5) is modifier multipliers on behavior tree conditions.** Same shape:

```json
{ "id": "madcat",
  "modifiers": [
    { "stat": "engagement_modifier",  "op": "mul", "value": 1.4 },
    { "stat": "alpha_strike_efficiency","op":"add","value": 0.85 },
    { "stat": "close_combat_penalty", "op": "add", "value": 0.40 }
  ]
}
```

The `effective_engage_distance` formula in Track F §5 collapses to one Lua call: `mc2.modifiers.effective(warriorId, "engagement_modifier")`. The "compose multiplicatively" rule is the registry's stack rule — written once, used five places.

**Faction passives** map cleanly: faction prototype owns a `ModifierBag`, every warrior on that team inherits via a virtual chain (modifier scopes: warrior bag, mech bag, faction bag, all read at query time and merged). v1 can implement this as "warrior bag includes a pointer to mech and faction bags; resolve walks all three"; the cache invalidation chain is straightforward.

**Weapon component tags** (B.4 in the conventions spec) are bags-of-modifiers attached to a weapon instance. AMS's `"reduces missile damage"` is a modifier on the weapon, evaluated when an incoming-missile event queries `weapon.effective("missile_damage_reduction_pct")`.

**The unification is not theoretical.** The HBS BT codebase has been here and the modders converged on this shape. The five+ systems above all become natural users of one engine seam, not five seams.

---

## §8 — Risks (what could make this go wrong)

1. **Generic registry that does everything badly.** The HBS `StatisticEffectData` shape is famously verbose; modders complain about boilerplate. Mitigation: ship JSON aliases for the common case (a quirk JSON with bare `{stat,op,value}` rows beats `targetCollection: "Self", statisticName: ..., operationType: "Float_Multiply"`). Optimize for the 80% case — the boilerplate is a tax, not a feature.

2. **Performance death by abstraction.** Every stat lookup walks a modifier list. Mitigation per §6: aggressive caching, generation counters, no Lua call on cached path. The risk *materializes* if we forget the caching layer — verifying it's present and correct is a prerequisite to v1 ship.

3. **Versioning nightmare.** Modifier semantics change between engine versions; every mod breaks. Mitigation: lock the v1 tuple (op set, stack rules, temporality enum) under `mc2_api_version=1` and *don't change them*. v2 adds; v2 doesn't break v1 modifiers. Stack-rule changes are an `mc2_api_version` bump.

4. **Stat-name collision across mods.** Mod A declares `"accuracy"` as float; mod B declares same name as int. Mitigation: stat names are namespaced by the mod that declares them (`"my-mod:accuracy"`); engine-canonical stats live in the `mc2:` namespace. Bare names default to `mc2:`. The conventions spec's "engine ships the core list" rule is load-bearing here.

5. **Hidden global state.** Modifiers attached to entities mutate game state in ways that aren't visible from the call site. Mitigation: the introspection API (`mc2.modifiers.list(id)`) and an in-game F-key debug overlay listing all active modifiers per selected unit. This is what BT modders use Abilifier's tooltip popup for; we want the engine to provide the equivalent.

6. **Stock-install drift.** If the engine starts depending on the registry for stock-mech behavior (`magic_abl_contamination_rule` territory), the stock install gains a runtime dependency it didn't have before. Mitigation: engine-side stock code never *requires* the registry to exist. The registry is a side-channel that mods write into; stock paths read raw stats unless a modifier is attached. This must be a hard architectural rule (call out in spec).

7. **The "what is a stat?" boundary.** Some BT modder use cases want to modify *enums* (range_preference: close→long), not numbers. v1 says "stats are float-typed only." Real risk: v1 enum-typed quirks have to wait for v2, and the workaround (encode as float) is ugly. Acceptable price for v1 scope discipline; document it.

---

## §9 — Recommendation

**Ship the hybrid (§5) in v1. Be opinionated about the bright line.** Concrete rationale:

- **The BT mod scene already converged on this shape.** RogueTech, BTA, Abilifier, CustomComponents — every major BT mod stack reinvented some version of "named stat-modifying effect bag." Modders coming from those ecosystems will look for it on day one. Shipping nothing forces them to reinvent it badly per-mod (§4); shipping the full v2 surface (timed, oneshot, inline closures) bets engineering capacity on use cases we haven't validated.

- **The hybrid v1 tuple covers the BT-canonical examples.** Every quirk listed in BT 1.x sourcebooks fits `(stat, op ∈ {add,mul,set_floor,set_ceil}, value, named_predicate?, permanent|mission)`. The v2 surface is real but BT-niche; defer it without losing the 80%.

- **The Track F unification (§7) collapses architectural surface area.** Pilot personality, chassis affinity, quirks, weapon tags, faction passives — five systems → one. The v2-deferral path duplicates infrastructure across them; the cost of not unifying is paid every time a new modder feature lands.

- **Performance is fine.** The caching strategy (§6) is well-understood and small. The trampoline budget already accommodates 100 warriors × 1 Lua call/frame; modifier resolution lives entirely in C++ on the cached path, well below that budget.

- **The risk surface is bounded.** The two real risks (boilerplate, versioning) are mitigated by JSON aliases for the common case and an `mc2_api_version`-locked v1 tuple. Both are mechanical, not architectural.

- **What I'd refuse to do in v1.** No `set_exact` op (foot-gun). No inline Lua condition closures (perf cliff). No timed/oneshot temporality (timer wheel is real engineering). No int/bool/enum stats (scope creep). Each of these is a v2 promotion candidate; ship them only when a real mod blocks on them.

The user asked because they were unsure between v1, v2, and hybrid. **Hybrid v1 is the answer.** It's the smallest viable shape that lets BT modders feel at home, it unifies five+ subsystems on one engine seam, the perf is in budget, and the v2 surface is a clean additive expansion that doesn't break v1 mods.

If forced to pick a *single concrete next step*: write the v1 schema (`modifier.schema.json`) and the C++ `ModifierBag` header alongside the conventions spec answer to §B.4 — those two artifacts pin the bright line and unblock both Track C (Lua surface) and Track F (pilot personality / chassis affinity refactor).

---

## §10 — Open questions

1. **Stat name canonicalization.** Engine ships `mc2:` core stats — what's the actual list? §5 hand-waves "~20"; needs an enumeration sweep across `code/warrior.cpp`, `code/weaponbolt.cpp`, and the Track F design's pilot weights / chassis affinities. A sibling exploration doc should produce the v1 canonical stat list.

2. **Predicate inventory.** §5 lists ~10 named predicates ("target_in_optimal_range", etc.). Real list TBD. Source from the BT-canonical conditions in RogueTech/BTA quirk JSONs once we audit them.

3. **Faction-passive scope chain.** §7 sketches "warrior bag → mech bag → faction bag" with a query-time merge. Caching is non-trivial when faction bag changes (every warrior on the team must invalidate). Worth a perf spike to confirm the generation-counter strategy scales.

4. **Hot-reload semantics.** When a modder edits a quirk JSON mid-mission, do already-applied modifier instances on existing entities update, or only newly-spawned ones? HBS gets this wrong (only new); we should aim for "live-update, with revert+reapply per entity touched" — but it requires the bag store the *source quirk id*, not just the resolved values.

5. **Save/load.** Permanent modifiers persist trivially (re-apply on load from quirk attachment). Mission-scoped reset on `Mission.End`. But what about modifiers added mid-mission by Lua scripts (`mc2.modifiers.add(warriorId, ...)`)? Do those persist across save/reload within the mission? Yes if we serialize the bag; no if not. Decide.

6. **AI-layer integration ordering.** Track F §4–§5 are written assuming bespoke pilot/chassis structures. If we ship the registry in v1, Track F either (a) refactors to use it, or (b) keeps its bespoke shape and we pay the unification cost later. Strong preference for (a); needs a Track F revision.

7. **Where is the `mc2.modifiers.*` API gated — STABLE or EXPERIMENTAL?** Recommend STABLE for v1 (it's the whole point), with the explicit understanding that the v2 surface (timed, oneshot, inline closures) lands under `mc2.experimental.modifiers.*` until promoted.

8. **Cross-mod modifier ordering when same source attaches to same stat.** Two mods both add an "armor +5" quirk under the same id. ModTek-style load-order rules apply (later wins on full-replace; merge accumulates on `$patch:"merge"`). Worth an explicit note in the v1 spec — modders coming from ModTek will assume it works the BT way.
