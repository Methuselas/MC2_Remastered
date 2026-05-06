# BattleTech Modder Conventions — Design Spec

**Date:** 2026-04-30
**Status:** Draft — collaborator-fillable stub
**Owner:** RogueTech-veteran collaborator (art-side primary, systems-side reviewer)
**Scope:** Authoritative reference for BattleTech-IP modding conventions to adopt in Tracks C / D / E. Supersedes any per-doc guesses about mech / weapon / mission file formats.

---

## How to use this document

This spec has **two zones with different ownership**:

- **Section A — Art / asset conventions (collaborator-authoritative).** The collaborator has direct RogueTech experience here. Sections are written as *prompts*; please fill in with your actual conventions, file shapes, and rationale. No external research seeds these — your knowledge is the source of truth.

- **Section B — Systems conventions (research-seeded, collaborator-validated).** Public research from ModTek / RogueTech / Coyote's / BTA / MW5 has produced initial proposals. Please mark each as one of:
  - ✅ **Adopt as-is** — matches your experience, fine to lock
  - 🔧 **Adopt with deltas** — conceptually right; specific changes noted
  - ❌ **Wrong context for MC2** — explain why and what's right instead
  - ❓ **Need more research** — flag for follow-up
  - 💡 **Bigger idea** — your experience says this is part of a larger pattern we're missing

Fill in deltas inline; don't worry about formatting prose. The synthesis will pull the answers into the relevant Track C / D / E specs.

This doc is intentionally not a final spec — it's a structured fill-in for things you have authoritative knowledge of. Once filled, it becomes the source of truth.

---

# Section A — Art / asset conventions

*(collaborator-authoritative; no proposals seeded)*

## A.1 — Mech variant naming and topology

**Question:** How should we name mech prototypes? RogueTech used what convention (`mech.madcat.prime` vs `mech_madcat_prime` vs `MAD-3R` style)? What's the relationship between chassis, variant, and loadout in your model?

**Your answer:**
> _(fill in)_

**Question:** What does the variant taxonomy look like for total conversions? E.g. how do you handle "MadCat Prime", "MadCat A", "MadCat-Pryde", "MadCat IIC" — same chassis, different variants, different IP eras?

**Your answer:**
> _(fill in)_

## A.2 — Hardpoint slot layouts (visual)

**Question:** RogueTech mechs ship with hardpoint visualizations — energy, ballistic, missile, AMS slots positioned per chassis. What file format encodes the visual hardpoint positions? Is it embedded in the model file (.glb extras dict?), a sidecar JSON, or generated from a slot manifest?

**Your answer:**
> _(fill in)_

**Question:** How does hardpoint visual size scale with weapon class (small laser vs LRM-20)? Does the model author bake hardpoint anchor data, or does the engine compute it from a class table?

**Your answer:**
> _(fill in)_

## A.3 — Paint scheme data format

**Question:** Per the BT research, paint schemes should be data, not asset replacement. RogueTech's pattern: how is a paint scheme defined? RGB triples per body part? Tintable mask textures + per-mech color slots? Per-faction color palettes referenced by ID?

**Your answer:**
> _(fill in)_

**Question:** What's the player-facing paint UI workflow? Pick from palette? Numeric RGB? Faction preset? Custom?

**Your answer:**
> _(fill in)_

## A.4 — Model export pipeline conventions

**Question:** From Blender (or 3ds Max) → .glb / .fbx → engine. What conventions did RogueTech rely on?
- Bone naming for animation retargeting
- Coordinate-system handedness
- Scale (cm vs m vs in)
- UV unwrap conventions (atlas? per-part?)
- Animation export — embedded? Sidecar?
- LOD tiers — exported as separate meshes? Generated?

**Your answer:**
> _(fill in)_

## A.5 — CAB (Community Asset Bundle) integration

**Question:** CAB is a community-shared asset library in BattleTech / RogueTech. Should MC2R have an equivalent? If so, what's the right shape — a public manifest with versioned bundles? A `mods/cab/` reserved directory? Per-asset attribution metadata?

**Your answer:**
> _(fill in)_

## A.6 — Texture / normal / material conventions

**Question:** What texture-set conventions did RogueTech use? Diffuse / normal / specular naming? PBR vs blinn-phong? Per-faction palette swaps as separate maps or shader-uniform tints?

**Your answer:**
> _(fill in)_

## A.7 — Mech bay portrait + iconography

**Question:** Per the existing `Do Not Upscale These Art Assets` rule (CLAUDE.md), mech icons are tightly coupled to atlas math. RogueTech's portrait/icon conventions — single PNG per mech? Atlas? Procedural composite from chassis + variant tags?

**Your answer:**
> _(fill in)_

## A.8 — Anything else art-side that you know we'll trip on

> _(fill in — this is the "what would I want a future me to know on day 1" section)_

---

# Section B — Systems conventions

*(research-seeded from ModTek / RogueTech / Coyote's / BTA / MW5; mark each ✅ / 🔧 / ❌ / ❓ / 💡)*

## B.1 — Mission file format

**Research finding (Coyote's Mission Pack pattern):**
Mission files are JSON with a top-level `name`, `description`, `Length`, `Difficulty`, `MapID`, `Biomes`, `EncounterLayerName`, plus arrays of `objectives`, `optional_objectives`, `dialog`, and `event_triggers`. Procedural variation is via referenced template tables; specific spawns can be declared inline or pulled from `mech_groups`.

**Proposed MC2R adaptation:** mission files at `mods/<id>/data/missions/<id>.json` (or `.yaml`). Each registers via `mc2.prototypes.register("mission", id, table)` in `data.lua`. Per-deployment-point spawns hooked via `control.lua` event handlers on `Mission.Begin`.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.2 — Weapon JSON schema

**Research finding (ModTek weapon pattern):**
Standard fields: `Description.Id`, `Description.Name`, `Damage`, `Heat`, `MinRange`, `ShortRange`, `MediumRange`, `LongRange`, `MaxRange`, `RefireModifier`, `BonusValueA/B`, `Tonnage`, `InventorySize`, `WeaponSubType`, `Type`, `WeaponEffectID`, `ComponentTags.items[]`, `statusEffects[]`. Patches via merge operators (`ObjectMerge`, `ArrayAdd`).

**Proposed MC2R adaptation:** weapon files at `mods/<id>/data/weapons/<id>.json`. Auto-discovered by the loader (no manifest entry required for the common case). Override stock weapons via same-id registration with merge mode (per the JSON-merge-operators free win below).

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.3 — Mech chassis / variant / loadout split

**Research finding (MW5 + ModTek pattern):**
Three-file split per mech instance:
- `chassis_<id>.json` — physical/visual (tonnage, slots, hitboxes, model ref)
- `mech_<id>.json` — variant-level (chassis ref, role tag, default armor distribution)
- `mechdef_<id>.json` (or loadout) — instance-level (variant ref, equipped weapons/heatsinks, paint)

This split lets a single chassis support many variants which in turn support many loadouts.

**Proposed MC2R adaptation:** same three-file split under `mods/<id>/data/{chassis,variants,loadouts}/`. Cross-references via `Inherits:` field (per OpenRA's pattern in the borrowing doc).

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.4 — Quirks / affinities

**Research finding (YAML quirks pattern):**
Quirks are first-class data: `quirk_<id>.json` files with `id`, `name`, `description`, `properties{}`. Properties reference a registered modifier list (`heat_efficiency_+5pct`, `armor_back_+10`, etc.). Modder ships their own quirk JSON; engine reads, applies modifier dynamically at mech-init.

**Proposed MC2R adaptation:** quirks at `mods/<id>/data/quirks/<id>.json`. Registered via `mc2.prototypes.register("quirk", id, table)`. A modifier registry exposed at `mc2.modifiers.register(name, fn)` so quirk effects are themselves moddable.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.5 — Lance composition rules

**Research finding (BattleTech career mode / RogueTech pattern):**
Lance composition rules drive contract generation, opfor selection, salvage caps. Defined via `lance_def_<id>.json` with `units[]` (each can be a specific mech or a tag-based query like `tonnage:80-100,role:assault`). Tonnage caps per faction or per difficulty tier.

**Proposed MC2R adaptation:** lance defs at `mods/<id>/data/lances/<id>.json`. Selection at runtime via Lua: `mc2.lance.generate(faction_id, difficulty, tonnage_cap)` returns a unit list.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.6 — Faction reputation / contracts

**Research finding (RogueTech / BTA flashpoint pattern):**
Factions are first-class prototypes with id / name / colors / mech-roster / weapon-preferences. Reputation is per-savegame state in `mc2.persist[mod_id].faction_rep`. Contracts reference factions by id; reward/penalty deltas update reputation.

**Proposed MC2R adaptation:** `mods/<id>/data/factions/<id>.json` for static data; `mc2.faction.reputation(faction_id)` API for runtime queries; reputation persisted in `mc2.persist`.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.7 — Salvage tables / loot economy

**Research finding (RogueTech `itemCollection_*` pattern):**
Indirection: a contract references an `itemCollection_id`, which references a list of `(item_id, weight, condition)`. Modders extend by adding new collections, not by editing contracts. This is RogueTech's indirection trick that lets economy mods compose.

**Proposed MC2R adaptation:** `mods/<id>/data/loot_tables/<id>.json` keyed by collection id. Contracts (B.1) reference collection ids by string. Engine-side roll API: `mc2.loot.roll(collection_id, count)`.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.8 — Career mode events / flashpoints

**Research finding (BattleTech flashpoint pattern):**
Flashpoints are narrative event trees: triggered by `(faction_rep, time, mission_count, custom_flag)` predicates; present player a branching dialog; outcomes mutate game state. Defined as `flashpoint_<id>.json` with `triggers{}`, `dialog[]`, `branches[]`, `outcomes[]`.

**Proposed MC2R adaptation:** flashpoints at `mods/<id>/data/flashpoints/<id>.json`. `control.lua` hooks `mc2.events.on("Career.Tick", ...)` to evaluate triggers; engine surfaces dialog UI when a flashpoint fires.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.9 — Pilot abilities / leveling

**Research finding (BattleTech 2018 + RogueTech pattern):**
Pilots have core stats (Gunnery, Piloting, Guts, Tactics) plus an ability tree of unlockable perks. Abilities are data-driven (modders add new abilities via JSON). Per-pilot persistence in savegame.

**Proposed MC2R adaptation:** `mods/<id>/data/pilot_abilities/<id>.json`. Pilot state in `mc2.persist[mod_id].pilots[pilot_id]`. Ability effects via the modifier registry (B.4).

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.10 — JSON merge operators (CRITICAL — table stakes per BT research)

**Research finding (ModTek standard):**
ModTek defines a set of JSON-Patch-style merge operators: `ObjectMerge`, `ArrayAdd`, `ArrayConcat`, `ArrayReplace`, `Remove`. Modders override stock data by writing patches, not full replacements. This is **table-stakes** for BattleTech-IP modders — full-replace would be a downgrade from ModTek.

**Proposed MC2R adaptation:** Add a `$patch` field to mod data files. Modes:
- `$patch: "replace"` (default for new prototypes)
- `$patch: "merge"` (object-level deep merge)
- `$patch: "ops"` with explicit op list (`{op: "add", path: "$.weapons[+]", value: ...}`)

Implement via `nlohmann/json-schema-validator` or hand-rolled patch applier.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta. **Especially important to validate this one** — modder ergonomics depends on it.)_

## B.11 — Per-type folder auto-discovery (CRITICAL — table stakes)

**Research finding (ModTek + YAML standard):**
Loader auto-discovers all `*.json` (and `*.yaml`) files in `mods/<id>/data/<type>/` and registers them by `id` field. Modders never write manifest entries for the common case.

**Proposed MC2R adaptation:** Loader walks `mods/<id>/data/{mechs,weapons,quirks,...}/` and ingests every file. Manifest entries (per the catalog doc's earlier mention) are an *override fallback* for non-standard layouts, not the default.

**Status:** ❓ **Pending review** — your validation needed.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.12 — `OptionallyDependsOn` (free win from ModTek)

**Research finding:** ModTek distinguishes `dependsOn` (hard required) from `OptionallyDependsOn` (load after if present, no error if absent). This enables soft integration patterns.

**Proposed MC2R adaptation:** Add `?modid` already supported; ensure semantics match ModTek's "load-after-if-present" not just "loadable-without."

**Status:** Recommend ✅ **adopt verbatim**. Cheap, valuable.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

## B.13 — Settings stage (Factorio + ModTek pattern)

**Research finding:** Factorio has a third lifecycle stage — `settings`. Mods declare user-configurable options (`mod-runtime`, `mod-startup`); engine surfaces a settings UI. ModTek's `Settings` blob serves the same purpose.

**Proposed MC2R adaptation:** `mod.json` carries a `settings: {}` object declaring options. Engine renders settings UI via Track B's ImGui. Lua reads via `mc2.mod.settings(id)`.

**Status:** ❓ **Pending review** — needs collaborator input on whether RogueTech players actually use this or it's a footgun.

**Your assessment:**
> _(✅/🔧/❌/❓/💡 + delta)_

---

## Section C — Decisions you can make right now

A few small questions whose answers unblock immediate work:

### C.1 — JSON vs YAML default

Engine will support both (cost is low — see chat synthesis). But **examples and `tools/new-mod` template ship in one format.**

- RogueTech / ModTek / BTA modders are JSON-native
- MW5 / Coyote's modders are JSON-native (despite MW5 using YAML internally — the BT-IP mod scene around it standardized on JSON via tooling)
- YAML Mech Engineer is one of the few YAML-first BT tools

**Your call:**
- [ ] JSON default (recommended unless you disagree)
- [ ] YAML default
- [ ] Other

### C.2 — Lua mod-API namespace style

Per the API surface catalog: `mc2.<subsystem>.<verb>` (e.g. `mc2.object.apply_damage`). Locked in blocking-questions doc Q2.

The 5 close-calls (apply_damage, is_in_area, is_dead_or_fled, teleport, video.*) — any pushback?

**Your call:**
> _(any disagreements?)_

### C.3 — Track F (AI replacement) priority

The BT research surfaced that 3025-era total conversions are blocked on AI replacement. RogueTech / BTA solved this with C# DLLs; our equivalent is a Lua `corebrain` replacement, currently unscoped.

**Your call:**
- [ ] Defer Track F until after C-1..C-5 (recommended; ship Lua first, AI replacement second)
- [ ] Scope Track F now alongside Track C
- [ ] Other

### C.4 — Quirks / abilities / modifier registry — first-class or deferred?

Several BT-specific patterns (quirks, pilot abilities, weapon component tags) all want a shared "modifier registry" layer. Should this be a v1 feature or v2?

**Your call:**
- [ ] V1 (lock the modifier registry shape now)
- [ ] V2 (ship without; modders add via control.lua manually until then)
- [ ] Hybrid (basic registry v1, expand v2)

---

## Sources for the systems-side research

These are the public artifacts the proposals are seeded from. If something looks wrong, check the source first:

- ModTek: <https://github.com/BattletechModders/ModTek>
- RogueTech: <https://github.com/BattletechModders/RogueTech>
- BTA 3062: <https://www.bta3062.com/>
- Coyote's Mission Pack: search NexusMods for the latest version
- YAML weapons/quirks (Yet Another *): NexusMods MW5 / search
- Factorio modding docs: <https://lua-api.factorio.com/>

Full research findings: [`../explorations/2026-04-30-track-c-battletech-mw5-mod-scene-research.md`](../explorations/2026-04-30-track-c-battletech-mw5-mod-scene-research.md).

---

## When this document is filled

Section A becomes the **art-pipeline reference** for Track D (Assimp importer) and Track E (manifests). Section B becomes the **schema reference** for Track E. Section C answers unblock immediate work on Tracks C and E.

Cross-reference back to the Track-specific specs (modders paradise roadmap §6 D and §6 E) once landed.

The synthesis pass after this is filled converts your answers into normative spec language in the relevant Track docs. You don't have to write spec prose — your answers + deltas are the input; structured prose is the output.
