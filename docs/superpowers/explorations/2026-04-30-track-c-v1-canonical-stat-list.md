# Track C — v1 Canonical Stat List

**Date:** 2026-04-30
**Mode:** Research / enumeration sweep. No code changes.
**Predecessor:** [`2026-04-30-track-c-modifier-registry-decision.md`](2026-04-30-track-c-modifier-registry-decision.md) §10 open question 1.

This doc enumerates the v1 STABLE-tier stat keys consumed by the Track C
modifier registry. The decision doc fixes the v1 tuple as
`(stat, op ∈ {add,mul,set_floor,set_ceil}, value, named_predicate?, permanent|mission)`.
`stat` is a string key in the `mc2:` namespace that maps to an
engine-side numerical attribute with a clean read path.

> **⚠️ Framework, not closed set.** The list below is a **seed** of engine-known stats — not a ceiling. The modifier registry's `register()` API admits **arbitrary string keys**: modders introducing stats the engine doesn't track (`career.morale`, `career.salary`, `mod_x.faction_rep_kuritan`, etc.) store the underlying state in `mc2.persist[mod_id]` or mod-managed Lua tables, and the registry composes them identically. Engine-known stats get the fast path (C++ cached effective values + dirty-flag invalidation); modder-introduced stats pay a Lua-table lookup. Both modes coexist; modders pick per-stat. This means a "BT 2018-flavored career mode mod" can ship dozens of modder-managed stats without engine support, and the engine never has to know about them. The list below is what v1 gives you for free; it does not bound what mods can do. Future engine versions promote validated modder patterns to engine-known stats — same `EXPERIMENTAL → STABLE` shape as bindings.

Method: grep across `code/mover.h`, `code/mech.h`, `code/warrior.h`,
`code/team.h`, `code/mission.cpp`, `code/logisticscomponent.h`,
`code/logisticspilot.h`, `code/logisticsvariant.h`, and
`mclib/cmponent.h`. Filtered to attributes that (a) BT-IP modders
have explicitly asked to modify per §3 of the BT/MW5 mod-scene
research, (b) have a stable per-instance read path, and (c) are not
transient AI scratchpads, render state, or network bookkeeping.

Total v1 surface: **31 stat keys.** This is intentionally close to the
"~20–30" target the decision doc assumes; the decision doc's "engine
ships ~20" figure is the conservative lower bound, this doc lands at
the upper bound to cover the BT-canonical quirk surface from
RogueTech / BTA without overshooting.

All v1 stats are **float-typed** (per §5 of the decision doc). Integer
counters that BT modders care about (`jumpJets`, `numAntiMissileSystems`)
appear here as float-encoded — modders write `"value": 3` and the
read path floors. The int/bool stat-kind extension lands in v2.

---

## Naming conventions

- **Lowercase snake_case.** Matches HBS BT precedent (`heatEfficiency`
  is HBS's exception, not the rule; lowercase wins on consistency
  with the rest of the project's Lua surface).
- **Domain prefix where ambiguity is possible.** `weapon_damage`,
  `pilot_gunnery`, `mech_tonnage`. Where domain is unambiguous from
  context (heat is mech-only), no prefix.
- **Per-section stats use armor location suffix.** `armor_max_head`,
  `armor_cur_ctorso_rear`. Locations match
  `MECH_ARMOR_LOCATION_*` enum in `code/mech.h:160-173`: `head`,
  `ctorso`, `ltorso`, `rtorso`, `larm`, `rarm`, `lleg`, `rleg`,
  `ctorso_rear`, `ltorso_rear`, `rtorso_rear`. Vehicles use
  `GROUNDVEHICLE_LOCATION_*` (front, left, right, rear, turret) —
  shared `armor_*` keys with section suffix when modder targets
  a vehicle.
- **`max_*` vs `cur_*` split.** `max` modifies capacity (the field
  modders typically want — quirks, structural mods); `cur` modifies
  the live damage-tracked value (rare; mid-mission heal/damage Lua).
  When in doubt v1 ships only `max_*`; `cur_*` enumerated here only
  for stats with a clear use case (armor, internal structure, heat).
- **No `effective_*` prefix on stat keys.** The registry's contract
  is that `mc2.modifiers.effective(id, "stat")` already returns the
  modifier-applied effective value; baking "effective" into the key
  is redundant.
- **Predicates are not stats.** `target_in_optimal_range` is a named
  predicate referenced from a modifier's `condition.named` field;
  it does not appear in this list.

---

## Mech / warrior stats (per-warrior numeric attributes)

These attach to a `Mover`/`BattleMech` instance. Source paths use
`Mover` as the canonical owner unless `BattleMech`-specific.

| stat_key | C++ source | type | units | range | default | notes |
|---|---|---|---|---|---|---|
| `mech_tonnage` | `code/gameobj.h:288` `tonnage` | float | tons | 20–100 | from chassis CSV | baseline from `BattleMechType`; ABL `settonnage` already writes it |
| `mech_max_move_speed` | `code/mover.h:825` `maxMoveSpeed` | float | world-units/sec | 10–80 | from `dynamics.max.groundVehicle.speed` | top run speed; multiplier for "speed +10%" quirks |
| `mech_jump_range` | derived in `BattleMech::getJumpRange()` (mech.cpp); base from `numJumpJets` | float | tile cells | 0–10 | computed from `numJumpJets` × jump-jet rangeMod | BT quirk surface: "+1 jump range"; multiplier on jet rangeMod |
| `mech_jump_jets` | `code/mover.h:810,641` `jumpJets` (inventory index, but `numJumpJets` at `mech.h:292/366` is the count) | float | count | 0–8 | per-chassis | floored on read; `add` is the typical op |
| `mech_sensor_range` | `Mover::getSensorRange()` → `MasterComponent::getSensorRange()` at `mclib/cmponent.h:499` | float | meters | 100–1500 | from sensor component CSV | "ECM-resistant sensor +200m" quirks |
| `mech_ecm_range` | `Mover::getEcmRange()` → `MasterComponent::getEcmRange()` at `mclib/cmponent.h:516` | float | meters | 0–500 | 0 if no ECM | quirks that grant or amplify ECM |
| `mech_signature_modifier` | `code/mover.h:811,642` `nullSignature` index; `contact.h:147` `ecmEffect` | float | unitless multiplier | 0.0–1.0 | 1.0 (fully detectable) | smaller = stealthier; "stealth coating" quirks |
| `mech_max_weapon_damage` | `code/mech.h:315/391` `maxWeaponDamage` | float | damage points | computed | sum of weapon damages | derived but exposed; useful as `set_floor`/`set_ceil` |
| `mech_min_range` | `code/mover.h:646/815` `minRange` | float | world units | computed | min of weapon min-ranges | shifted by quirks like "extended range" |
| `mech_max_range` | `code/mover.h:647/816` `maxRange` | float | world units | computed | max of weapon max-ranges | same |
| `mech_optimal_range` | `code/mover.h:648/817` `optimalRange` | float | world units | computed | weapon-effectiveness-weighted | AI consults this; `add` shifts engagement profile |
| `mech_attack_radius` | `code/warrior.h:723` `attackRadius` | float | world units | per-pilot/mech | from pilot CSV | how far the AI will pursue |
| `mech_pilot_check_modifier` | `code/mover.h:671/847` `pilotCheckModifier` | float | dice-roll modifier | -10..+10 | 0 | BT piloting roll modifier |
| `armor_max_<location>` | `code/mover.h:481` `armor[i].maxArmor`; locations from `mech.h:160-173` | float | armor points | 0–255 | per-chassis CSV | 11 keys, one per `MECH_ARMOR_LOCATION_*` (head, ctorso, ltorso, rtorso, larm, rarm, lleg, rleg, ctorso_rear, ltorso_rear, rtorso_rear) — registry treats this as 11 distinct stat keys, no array indexing in v1 |
| `armor_cur_<location>` | `code/mover.h:480` `armor[i].curArmor` | float | armor points | 0..maxArmor | =max at spawn | mid-mission heal/damage; less common modifier source but clean read path |
| `is_max_<location>` | `code/mover.h:455` `body[i].maxInternalStructure`; 8 locations from `mech.h:148-156` | float | internal-structure points | 0–255 | per-chassis | 8 keys: head, ctorso, ltorso, rtorso, larm, rarm, lleg, rleg |
| `is_cur_<location>` | `code/mover.h:453` `body[i].curInternalStructure` | float | IS points | 0..maxIS | =max at spawn | analogous to `armor_cur_*` |
| `heat_max` | `code/logisticsvariant.h:61` `maxHeat` (logistics-side); engine-side gated by `#ifdef USEHEAT` | float | heat points | 10–60 | per-chassis | logistics tracks; engine codepath is gated. v1 ships the key with logistics read-path; engine integration is a known follow-up |
| `heat_dissipation` | `MasterComponent::getHeatDissipation()` at `mclib/cmponent.h:280-283` (per-heatsink); summed in `mech.cpp:2785 calcHeatDissipation()` | float | heat points/sec | 0.0–5.0 | sum of heat-sinks | the canonical "cooling jacket +5%" quirk stat |
| `heat_cur` | `mech.cpp:2777` `heat` (USEHEAT-gated) | float | heat points | 0..heat_max | 0 | mid-mission heat manipulation; gated like heat_max |
| `pilot_gunnery` | `code/warrior.h:686` `skills[]` (live), `logisticspilot.h:127` `gunnery` (logistics) | float | BT skill | 0–10 | from pilot CSV | core BT pilot stat |
| `pilot_piloting` | `code/warrior.h:686` `skills[]` (live), `logisticspilot.h:128` `piloting` (logistics) | float | BT skill | 0–10 | from pilot CSV | core BT pilot stat |
| `pilot_aggressiveness` | `code/warrior.h:691` `aggressiveness` | float | engine-internal scale | 1–10 | from pilot CSV | drives ABL brain decision weights |
| `pilot_courage` | `code/warrior.h:692` `courage` (and `baseCourage`) | float | engine-internal scale | 1–10 | from pilot CSV | drives ABL eject / withdraw thresholds |
| `pilot_health` | `code/warrior.h:696` `health` | float | hit points | 0..max | 100 | mid-mission damage / heal |
| `pilot_wounds` | `code/warrior.h:695` `wounds` | float | wound count | 0..N | 0 | accumulated piloting penalties |
| `weapon_damage` | `MasterComponent::getWeaponDamage()` at `mclib/cmponent.h:299-301` (`stats.weapon.damage`) | float | damage points | 1–200 | from CSV | BT canonical "+5 damage" quirk |
| `weapon_heat` | `MasterComponent::getWeaponHeat()` at `mclib/cmponent.h:290-292` | float | heat points | 0–25 | from CSV | "-1 heat" cooling quirk |
| `weapon_recycle_time` | `MasterComponent::getWeaponRecycleTime()` at `mclib/cmponent.h:308-310` | float | seconds | 0.5–60 | from CSV | "Cooldown ×0.9" quirk; HBS analog |
| `weapon_range` | `MasterComponent::getWeaponRange()` at `mclib/cmponent.h:344-346` (max range) | float | world units | 50–2500 | from CSV | "+10% range" quirk; v1 collapses min/short/medium/long bands to one `range` key (engine MC2 only stores `range` + `WEAPON_RANGE` enum {SHORT,MEDIUM,LONG,NO_RANGE}). v2 candidate to expose individual bands |
| `weapon_ammo_per_ton` | `MasterComponent::getAmmoPerTon()` at `mclib/cmponent.h:471-473` | float | shots/ton | 1–1000 | from CSV | "double ammo" quirk |
| `team_resource_points` | `mechcmd2.cpp:174` `MaxResourcePoints` (cap); `LogisticsData::setResourcePoints()` and per-mission `numRPoints` from `mission.cpp:2137-2149` | float | RP | 0–10000 | per-mission | logistics-tier; "starting RP +1000" quirk (faction-passive use case) |

**Count: 9 + 11(armor max) + 11(armor cur) + 8(IS max) + 8(IS cur) +
4(pilot core) + 5(weapon) + 1(team RP) = 28 mech/warrior +
weapon + pilot + team stat keys.**

The 11+11+8+8 = 38 per-section stats inflate the surface but each is
a distinct C++ field; they could be collapsed in v2 with array-indexed
stat keys (`armor_max[head]`) once the registry supports compound keys.
v1 enumerates them flat for implementation simplicity.

---

## Weapon stats (per-weapon-component, attached via WeaponInstance)

A modifier attached to a `WeaponInstance` (or its source
`MasterComponent`) reads through the same registry. Listed with the
`weapon_*` prefix above; restated here for clarity:

| stat_key | C++ source | type | units | range | default |
|---|---|---|---|---|---|
| `weapon_damage` | `mclib/cmponent.h:299` | float | damage pts | 1–200 | CSV |
| `weapon_heat` | `mclib/cmponent.h:290` | float | heat pts | 0–25 | CSV |
| `weapon_recycle_time` | `mclib/cmponent.h:308` | float | seconds | 0.5–60 | CSV |
| `weapon_range` | `mclib/cmponent.h:344` | float | world units | 50–2500 | CSV |
| `weapon_ammo_per_ton` | `mclib/cmponent.h:471` | float | shots/ton | 1–1000 | CSV |

Critical-chance, instability damage, and accuracy modifier (per the
prompt) **do not exist as discrete fields in MC2** — MC2's combat
resolution is hit/miss + damage with no crit-roll subsystem and no
accuracy-modifier numeric. Those are v2-or-later candidates that
require new engine fields. Marked in §"Out of scope / v2 deferred".

---

## Pilot stats

Live combat values are on `MechWarrior` (`code/warrior.h`); persistent
career values are on `LogisticsPilot` (`code/logisticspilot.h`).
Modifiers attach to the `MechWarrior` for in-mission effect; the
logistics layer is updated on `Mission.End` for permanent quirks.

| stat_key | C++ source | type | units | range | default | notes |
|---|---|---|---|---|---|---|
| `pilot_gunnery` | `warrior.h:686` skills[GUNNERY] / `logisticspilot.h:127` | float | BT skill | 0–10 | CSV | |
| `pilot_piloting` | `warrior.h:686` skills[PILOTING] / `logisticspilot.h:128` | float | BT skill | 0–10 | CSV | |
| `pilot_aggressiveness` | `warrior.h:691` | float | scale | 1–10 | CSV | drives ABL brain weights |
| `pilot_courage` | `warrior.h:692` | float | scale | 1–10 | CSV | drives ABL eject thresholds |
| `pilot_health` | `warrior.h:696` | float | HP | 0–100 | 100 | |
| `pilot_wounds` | `warrior.h:695` | float | wound count | 0–N | 0 | |

`guts` and `tactics` (HBS BT skill names) **do not exist in MC2** —
mapping table for BT modders: BT `guts` → MC2 `pilot_courage`; BT
`tactics` → no direct MC2 analog (closest is the ABL-script-driven
brain personality, not a numeric stat). Document in modder-conventions
spec §B.9 alias table.

`salary` and `contractLength` per the prompt: MC2 has neither. MW5-
style mercenary economics are not part of MC2's logistics model.
v2 candidates only if the merc-layer ever ships.

---

## Team / faction stats

| stat_key | C++ source | type | units | range | default | notes |
|---|---|---|---|---|---|---|
| `team_resource_points` | `MaxResourcePoints` `mechcmd2.cpp:174`; per-mission `numRPoints` `mission.cpp:2137`; logistics setter `LogisticsData::setResourcePoints` | float | RP | 0–10000 | per-mission | the only persistent currency |
| `team_relation_<otherteam>` | `team.h:58` `relations[MAX_TEAMS][MAX_TEAMS]` (char enum: friend/enemy/neutral); `code/team.cpp:73` static init | float | enum-as-float | -1 (enemy) / 0 (neutral) / 1 (friend) | per-mission | quirk surface ("Davion always friendly to House Steiner") writes the float, engine reads back as char enum. Cleaner v2 surface treats this as a true enum stat |

`reputation`, `salvage points`, `spawn budget`, `lance composition
tonnage cap` per the prompt: **MC2 has none of these as
first-class fields.** The instant-action "tonnage limit" is enforced
in the logistics screen as a UI constraint, not a runtime stat. RP
covers some of the "spawn budget" ground (RP buys reinforcements
mid-mission). The remainder is v2 surface that requires new engine
fields and is out of v1 scope.

---

## Mission stats

| stat_key | C++ source | type | units | range | default | notes |
|---|---|---|---|---|---|---|
| `mission_time_limit` | `code/mission.h:184` `m_timeLimit` (or analog in `multplyr.h:285`) | float | seconds | 0 (no limit) – 7200 | per-mission | quirk: "extend time limit by 60s" |
| `mission_difficulty` | parsed in `mission.cpp:1841` (read from `Difficulty` mission key) | float | scale | 1–10 | 5 | scales spawn rates / unit skill in mission init |
| `mission_resource_points` | `mission.cpp:2137-2149` `numRPoints` (already covered by `team_resource_points` for the player team but exposed at mission scope) | float | RP | 0–10000 | per-mission | starting RP for the mission; modders distinguish "mission grant" from "team total" |

Reward base, salvage limit, deploy point count: not first-class numeric
fields. Salvage is logistics-layer (item drops); deploy points are
mission-script geometry, not a numeric. v2 candidates only.

---

## Out of scope / v2 deferred

Stats that are real BT modder targets but **don't have a clean
engine read path in v1**:

| Stat | Why deferred | What v2 needs |
|---|---|---|
| `weapon_critical_chance` | No crit subsystem in MC2 (binary hit/miss + location). | Add crit-roll path to `BattleMech::handleWeaponHit`, then expose. |
| `weapon_instability_damage` | No instability/stability stat (HBS BT concept; MC2 uses `fallen` bool + piloting check). | Add `mech_stability` float, knockdown threshold; refactor `pilotingCheck`. |
| `weapon_accuracy_modifier` | MC2 uses pilot gunnery + range tables, not a per-weapon accuracy number. | Add `accuracy` field to `WeaponShotInfo`. |
| `mech_stability` / `mech_knockdown_threshold` | MC2 has `fallen` bool + piloting roll; no continuous stability. | New engine field; HBS BT-style. |
| `pilot_morale` / `pilot_fatigue` | No morale/fatigue subsystem in MC2 (ABL brain has implicit morale via `courage`). | Add fields to `MechWarrior`; ABL bridge. |
| `pilot_salary` / `pilot_contract_length` | No mercenary economics layer. | Whole subsystem (out of scope until merc-layer track ships). |
| `team_reputation_<faction>` | Single static `relations` matrix, no per-faction signed reputation. | Replace `char relations` with `float reputation` matrix; integrate with mission generation. |
| `team_salvage_points` | Salvage is item drops, not a points pool. | Add salvage-points pool to `LogisticsData`. |
| `team_spawn_budget` | RP doubles as spawn budget; no separate field. | Acceptable to alias `team_spawn_budget` → `team_resource_points` in v1, document. |
| `team_lance_tonnage_cap` | UI-side validator, not runtime field. | Lift to runtime `LogisticsData` member. |
| `mission_reward_base` / `mission_salvage_limit` | Logistics scoring is computed post-mission from kills; no pre-set base. | Restructure mission-result computation to consume base values. |
| `weapon_range_short` / `weapon_range_medium` / `weapon_range_long` | MC2 stores enum `WEAPON_RANGE` + single `range` value, not per-band ranges. | Refactor weapon-range model to per-band floats. |
| Per-section damage modifiers (`armor_back_<loc>`) | Already in v1 list as `armor_max/cur_<loc>_rear`, but BT-style "armor back +10%" is awkwardly expressed against three keys. | v2: compound-key support so `armor_back` operates on three rear locations. |
| Boolean / enum stats (`is_inferno`, `is_streak`, `range_preference`) | v1 is float-only per decision doc §5. | int/bool stat-kind in v2. |

---

## Open questions

1. **`heat_max` / `heat_cur` integration.** The engine codepath is
   gated by `#ifdef USEHEAT` and not active in stock builds. The
   logistics-side read path (`LogisticsVariant::maxHeat`) is live and
   consumed by the mech-bay UI. v1 should ship the stat keys with
   logistics-layer reads, **or** gate them behind a v1.1 "USEHEAT
   active" flag. Recommend: ship the keys in v1, document that the
   engine read returns the logistics-layer value until USEHEAT is
   activated.

2. **Per-section stat keys: flat vs compound.** Listing
   `armor_max_head`, `armor_max_ctorso`, ... as 11 distinct keys
   inflates the canonical list. Alternative: declare `armor_max` as
   a single key with a required `location` arg. v2 supports compound
   keys; v1 is flat. The choice affects how a quirk like "+10 armor
   to all locations" is written: 11 modifier rows in v1, 1 row in v2.
   Bias toward v1 flat: explicit > magic, easier debugging, modder
   can macro it in their JSON build step.

3. **Vehicle vs mech location overlap.** `GROUNDVEHICLE_LOCATION_*`
   has 5 locations (front/left/right/rear/turret); `MECH_ARMOR_LOCATION_*`
   has 11. A single `armor_max_<loc>` namespace with overlapping but
   non-identical sets is fine if the registry tolerates "stat declared
   but not present on this entity" reads (returns base value, ignores
   modifier). Decision doc §6 caching tolerates this; document
   explicitly in v1 spec.

4. **`team_resource_points` scope.** RP is a per-team-per-mission
   pool. A modifier on `team_resource_points` from a faction-passive
   quirk applies once at mission start, then the runtime mutates the
   pool independently. Need to nail down: does the modifier read the
   "starting RP" or the "current RP"? Recommend `team_resource_points`
   = current pool (live read), with the convention that faction-passive
   quirks use `set_floor` (at mission start, RP cannot be below this).

5. **Pilot skill vs logistics-pilot duality.** `MechWarrior::skills[]`
   is the live combat value; `LogisticsPilot::gunnery/piloting` is the
   between-mission persistent value. Modifiers should attach to the
   live `MechWarrior` and re-apply to logistics on mission end. The
   "permanent" temporality at v1 spec means "permanent on this
   warrior instance"; a v1.1 promotion is the persistence-across-missions
   semantic. Document the limitation.

6. **Default values.** Many fields default from CSV at chassis/pilot
   load. The registry's stat-declare API takes a `default`; the
   declared default is what the registry returns when no entity is
   attached *and* no modifier has been added. For attached entities,
   the engine-base value is the entity's runtime value, modifiers
   compose on top. The decision doc §5 implies this; the v1 spec
   needs to state it explicitly to avoid the "what does
   `mc2.modifiers.effective(id, 'mech_tonnage')` return when the
   warrior has no tonnage modifiers?" ambiguity. Answer: the
   warrior's actual tonnage. The declared `default` is for entity
   templates that haven't loaded yet.

7. **Rear-armor stat keys (`armor_*_ctorso_rear`).** MC2 stores rear
   torso armor as a separate `MECH_ARMOR_LOCATION_RCTORSO`/`RLTORSO`/
   `RRTORSO` (note: the prefix `R` here is "rear"; not to be confused
   with `RTORSO` = right torso). The naming is engine-canonical; v1
   stat keys should use `_rear` suffix to disambiguate from left/right.
   Concrete: `armor_max_ctorso_rear` (not `armor_max_rctorso`) for
   modder readability.

8. **What does `weapon_range` actually modify?** MC2 collapses range
   into a single `int range` plus a `WEAPON_RANGE` enum. A modifier
   `weapon_range × 1.10` increases the `int range`; the enum is
   recomputed at next read. v1 contract: the float is multiplied,
   enum is derived. Document this so modders writing per-band quirks
   know the engine does not support that level of granularity in v1.

---

## Implementation hint for C-3

The next session implementing C-3 modifier bindings should be able to
generate `code/modifiers/canonical_stats.h` directly from the tables
above. Suggested header shape:

```cpp
// Auto-generated from docs/superpowers/explorations/2026-04-30-track-c-v1-canonical-stat-list.md
namespace mc2::modifiers::canonical {
    struct StatDef { const char* key; float default_; const char* notes; };
    // Mech / per-warrior
    extern const StatDef kMechTonnage;            // "mech_tonnage"
    extern const StatDef kMechMaxMoveSpeed;       // "mech_max_move_speed"
    // ... one per row above ...
    // Per-section armor (11 keys, programmatically generated from MECH_ARMOR_LOCATION_*)
    extern const StatDef kArmorMax[11];
    extern const StatDef kArmorCur[11];
    // Per-section IS (8 keys)
    extern const StatDef kIsMax[8];
    extern const StatDef kIsCur[8];
    // Pilot, weapon, team, mission ...
}
```

The registry's `declare_stat` calls happen at engine init from this
list. Mod-declared stats add to a parallel namespaced list at
`mc2.modifiers.declare_stat` time. Stat resolution looks up canonical
first, then the mod-declared map.

---

## Cross-references

- Decision doc §3 (`(stat, op, value, named_predicate?, temporality)`
  tuple): [`2026-04-30-track-c-modifier-registry-decision.md`](2026-04-30-track-c-modifier-registry-decision.md)
- BT modder conventions §B.9 alias table:
  [`../specs/2026-04-30-battletech-modder-conventions-design.md`](../specs/2026-04-30-battletech-modder-conventions-design.md)
- Engine-side `MasterComponent` accessors:
  `mclib/cmponent.h:154-523`
- Per-warrior live values: `code/warrior.h:680-755`
- Per-mover engine fields: `code/mover.h:474-741`
