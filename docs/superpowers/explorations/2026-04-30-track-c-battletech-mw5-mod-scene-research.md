# Track C — BattleTech / MW5 / RogueTech Mod-Scene Research

**Date:** 2026-04-30
**Mode:** Research only. No code or schema changes proposed here; outputs feed Tracks C (Lua scripting), D (Assimp mech importer), and E (JSON manifests).
**Predecessors:**
- Spec: `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` §1–§6
- Catalog: `docs/superpowers/explorations/2026-04-30-track-c-lua-api-surface-catalog.md`
- Spec: `docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`

The framing question for this doc: **a modder who has spent five years building MW5 / HBS BattleTech mods sits down in front of MC2R. Does the platform we are building feel like a serious BT-IP mod target, or a hobby project?** The answer requires looking at what those modders actually ship, what their tooling expects, and which BT-IP-specific concepts (hardpoint topology, quirks, lance composition, pilot abilities, faction reputation, salvage, black market, paint schemes) are first-class in the established ecosystems. I worked from the public repos, wikis, and Nexus pages of the major mods; everything below is paraphrased from those sources with citations.

---

## §1 — Mod inventory

A representative cross-section, weighted toward mods with public source so the structure is inspectable. Categories follow the brief.

### Total conversions / overhauls

1. **RogueTech** (HBS BattleTech). Near-total conversion. Doubles vanilla content; adds Mech Engineer (deep mechlab rework), Custom Flashpoints, double-blind sensor rules, timeline coverage 3025–3130. Built on ModTek + a stack of dependent C# DLLs (`CustomComponents`, `CustomUnits`, `BattleTechPerformanceFix`, etc.). Repo: `github.com/BattletechModders/RogueTech`. Scale: thousands of JSON content files (mechs/weapons/gear), tens of thousands of LoC across the C# helper DLLs it bundles.
2. **BattleTech Advanced 3062 (BTA3062)**. Sibling overhaul to RogueTech, advancing the timeline to 3062. Wiki claims 50+ chassis and 500+ variants and 200+ tank platforms. Heat/ammo/explosion overhaul, salvage rework, Mechlab restored to TT-style construction. Repo: `github.com/Syrkres/BTA3062` (content) plus `github.com/BattleTech-Advanced-3062` org.
3. **MercTech** (MW5). Comprehensive mechlab + damage-model overhaul. FrankenMech limb-swap, three switchable damage models (MW5 / TT / MercTech), Ironman mode, ammo swap and AC firing modes. Nexus: `nexusmods.com/mechwarrior5mercenaries/mods/48`.
4. **YAML — Yet Another Mechlab** (MW5). Closer to a platform than a mod; many other mods depend on it. Adds the missing MWO variants, AMS hardpoints, equipment slot framework, save settings serializer, and the de-facto modding schema for mechs/weapons/quirks across the MW5 community. Nexus: `mods/459`. Schema repo: `github.com/mw5mercs-modding/yaml-docs`.

### Mech additions (variant packs)

5. **Yet Another IS Mech** / **Yet Another Clan Mech** / **Yet Another Legendary Mech** (MW5). Variant packs sitting on top of YAML; ship `.pak` content + JSON definitions for hardpoint layouts and quirks. Nexus: `mods/964`, `mods/966`. Each is a few dozen MDA/Loadout JSON files plus art.
6. **Lore-based Mech Variants — YAML Edition** (MW5). Pure-data mod — no DLL — that retunes hardpoints/quirks of stock chassis to match MWO/Sarna canon. Nexus: `mods/493`. This is the pattern MC2R should aspire to enable: data-only mod, no compiled code.

### Mission packs / procedural content

7. **Coyote's Mission Pack and Mission Overhaul** (MW5). Adds procedural mission types (Tower Defense, Multi-Objective, Convoy Escort, Patrol-with-encounter-table). Career-mode focused. Nexus: `mods/263`.
8. **vonBiomes** (MW5). Procedural-biome map generator pack used by Coyote's Mission Pack and others; modders depend on it for fresh terrain selection. Nexus: separate listing under vonBiomes.

### Balance / rules overhauls

9. **TTRulez_AIMod / TTRulez_DifficultyMod** (MW5). AI behavior + difficulty curve overhaul. Pure-data + small DLL. Nexus: `mods/269`.
10. **Skill Tree Rebuild** (HBS BT). Replaces vanilla pilot ability tree with TT-RPG-style branches. Nexus: `mods/647`. Built on ModTek + `Abilifier`.
11. **SkillBasedInit** (HBS BT). Initiative replacement; uses ModTek DLL hooks. Repo: `github.com/BattletechModders/SkillBasedInit`.

### UI / HUD / quality-of-life

12. **Pilot Overhaul** (MW5). Pilot management revamp; replaces vanilla pilot with named/portraited roster, hiring market. Repos: `github.com/Wpnx330/PilotOverhaul`, `github.com/blastjack85/PilotOverhaul-Eternal`.
13. **Chassis Editor** (HBS BT). Standalone tool, not a mod; emits ModTek-format chassis JSON for tweaks. Nexus: `mods/578`.

### Asset packs

14. **MW5 Mod Compatibility Pack** (MW5). Aggregates compatibility .pak files between mods. Nexus: `mods/168`.
15. **MW5 CAB (Community Asset Bundle)**. Open art-asset library other mods depend on; ModDB hosts the source. Conceptually parallel to the upscaler/asset-pack lane MC2R already has.

Across all of these, three patterns repeat: **(a)** a JSON-merge mod loader (ModTek for HBS BT, YAML for MW5) is the substrate; **(b)** the substrate publishes a schema (manifest, hardpoint, quirk, equipment) that other mods extend; **(c)** the most popular mods are *data-only* — DLL-required mods are rarer and treated as a tax.

---

## §2 — Mod loader patterns

### ModTek (HBS BattleTech 2018)

ModTek is the de-facto loader; HBS shipped a stripped-down fork in the 1.7 patch but the community kept moving on the original. Repo: `github.com/BattletechModders/ModTek`. Format docs: `doc/MOD_JSON_FORMAT.md`, `doc/ADVANCED_JSON_MERGING.md`.

**Directory layout.** Each mod is a folder under `BATTLETECH/Mods/<ModName>/`, with a single `mod.json` at the root plus arbitrary subfolders pointed at by manifest entries.

**`mod.json` shape (paraphrased from `MOD_JSON_FORMAT.md`).** `Name` is the only required field; optionals include `Enabled`, `Version`, `Description`, `Author`, `Website`, `Contact`, `PackagedOn`. Compatibility is declared via `BattleTechVersion` / `BattleTechVersionMin` / `BattleTechVersionMax`. Dependencies are three arrays: `DependsOn` (hard), `OptionallyDependsOn` (load-after if present), `ConflictsWith`. Content is registered under `Manifest` — an array of `{Type, Path, Id?, ShouldMergeJSON?, AssetBundleName?}` rows. C# extensibility: `DLL` + `DLLEntryPoint` + `Settings` (free-form object passed to the DLL as JSON).

**JSON merging semantics (`ADVANCED_JSON_MERGING.md`).** Two flavors. (1) The default: a mod ships a JSON file with the same `Id` as a stock def and ModTek does a recursive merge over it (load-order wins on conflicts). (2) "Advanced" merges: instruction files using JSONPath + an `Action` selector. Operations are `ObjectMerge`, `ArrayAdd`, `ArrayConcat`, `ArrayAddBefore`, `ArrayAddAfter`, `Remove`, `Replace`. The example in the docs removes all heat-sink components matching a JSONPath filter, then concats new ones in.

This is the single most important pattern to internalize: **modders expect granular per-field merges, not full-file replacement.** Plain "drop a JSON, it overrides the stock JSON" is the floor. The ceiling — and what the BT community uses heavily — is JSONPath + ops to splice into existing arrays. Track E currently sketches a "register prototype" Lua call (`mc2.prototypes.register("weapon", "ppc", {...})`) which is full-replace by default. We should add an `inherits` / `patch` mode at the same time.

### MW5 — `.pak` + Resources + `mod.json`

MW5 is Unreal Engine 4. Mods ship as a top-level folder containing a `Paks/` subfolder (compiled UE assets via UnrealPak) and a `Resources/` subfolder (JSON config — quirks, equipment, mech mappings). A `mod.json` sits at the top. Knight Ravens' MW5 modding guide (`knightravens.com/2019/12/18/mechwarrior-5-modding-guide-1/`) and the official MW5 Mod Editor Guide PDF (`static.mw5mercs.com/docs/MW5Mercs_Mod_Editor_Guide_(v2.3).pdf`) describe the toolchain.

The interesting half is the **Resources/ JSON layer** — that's where the modder community lives. MW5's stock data uses Unreal `UDataTable` assets; YAML-the-mod added a JSON layer on top so mods don't all have to recompile UE assets. MW5 modders rarely touch `.pak` content for pure stat tweaks; almost everything is JSON in `Resources/`.

### YAML (MW5) — schemas as a community standard

YAML is technically a single mod, but functionally it is the MW5 modding platform. Schema repo: `github.com/mw5mercs-modding/yaml-docs`. The two key data files most mods author:

- **`quirks.json`** — defines a map of quirk-id → `{name, description, optional color, properties}`. Properties cover stat modifiers, weapon-group rules, incoming-damage modifiers.
- **`mechs.json`** — maps mech chassis IDs → list of quirk IDs. Multi-mod merges are load-order based; later mods override earlier entries by chassis ID.
- **`equipment-properties.json`** — extends YAML's equipment slot framework; one entry per custom equipment or per gameplay-tag path.

Each MW5 mod can ship its own `quirks.json` / `mechs.json` and YAML merges all of them at startup. This is the same JSON-merge pattern as ModTek, with shallower semantics (per-key override, not JSONPath).

**Hardpoint files (per Mechwarrior Foundry Wiki, `mechwarrior5modding.fandom.com/wiki/Hardpoints,_Mech_Data,_and_Loadouts`).** Three layered files per chassis: a Hardpoint file (HPS) listing each hardpoint with `{name, SlotType: ballistic|energy|missile|ams, size: small|medium|large, models[]}`; an MDA (Mech Data) file with movement, health, slot list, name, icon, hero flag; and a Loadout file with the actual weapons + armor + paint scheme + hero status. Modded chassis live at `MyMod/StreamingAssets/data/chassis/chassisdef_atlas_AS7-D.json`-style paths.

### What both ecosystems agree on (load-bearing)

- One root manifest file per mod, JSON.
- Dependency declarations with version ranges.
- JSON content under named subfolders, registered (or auto-discovered) per type.
- Default merge is by ID; advanced merges target field paths.
- Optional native-code extension (DLL) but the community treats this as a tax — pure-data mods are the norm.
- Load order determines override winners.

Our spec §5.2 already aligns on the manifest shape (Fabric-flavored). The two pieces missing from our plan that both BT ecosystems have are: **JSONPath-style patching** and **per-mod subfolder convention with type→folder auto-discovery** (so a modder doesn't have to manually list every JSON in the manifest).

---

## §3 — IP-specific feature requests

For each BT-IP concept, where it lives in MW5/HBS today and where it would land in our plan.

### Hardpoint topology (energy / ballistic / missile / AMS slots per variant)

**Today (MW5).** Per-chassis HPS files; per-variant MDA references. Tooling: hand-edited JSON with the YAML schema; some modders use the MW5 Mod Editor (UE4 plugin) to author visual placement. **Today (HBS BT).** Per-`MechDef` JSON; component slot lists per location (left arm, head, etc.) with allowed types.

**MC2R coverage.** Track D's Assimp importer ingests mesh + bone hierarchy; hardpoint topology is *not* the mesh, it's a sibling JSON manifest. Our plan covers hardpoints under Track E (`mods/<id>/data/mechs/<chassis>.json`), but the spec hasn't named the schema yet. We should publish a `mech.schema.json` early so modders can target it. The MW5 HPS shape (`{name, SlotType, size, models[]}`) is a fine reference — though MC2 has historically modeled this as weapon nodes baked into the `.ase` file rather than data. Track D needs to expose Assimp's named bones / locator nodes as candidate hardpoints and let the JSON manifest claim them.

### Quirks / chassis affinities

**Today (MW5).** YAML's `quirks.json` map; `mechs.json` assigns. A quirk can carry stat modifiers (e.g. `Cooldown=-0.10`) and tagged properties (`AppliesToWeaponGroup="Energy"`).

**MC2R coverage.** Not in current spec. Lua API has `mc2.object.*` mutators but no notion of *passive per-chassis modifier table consulted at runtime*. Plausible landing spot: Track E adds `data/quirks/*.json` and a `data/mechs/<id>.json` `quirks: ["jenner-aff", "ams-stable"]` field; Track C exposes `mc2.quirk.has(objId, "jenner-aff")` and the modifier table is consulted by combat code at damage-time. This is a *new* engine seam — not a simple sidecar — but is small.

### Lance composition rules (tonnage, role mix)

**Today.** Both BT ecosystems encode lance generators in DLL code (HBS) or in YAML mission scripts (MW5). RogueTech does it via C# in its Custom Lance generator. There's no canonical JSON schema for lance composition — every mod re-rolls.

**MC2R coverage.** Track C's `mc2.team.*` namespace, plus lance-builder Lua helpers, is the natural home. We don't need a JSON schema; modders will write generators in Lua.

### Faction reputation / contracts

**Today (HBS).** Stock has a `Faction` enum; RogueTech extends with rep-tab UI, alliances, priority contract gating, contested-planet shifting (`steamcommunity.com/app/637090/discussions/3/5086242673973346167/` and the RogueTech wiki). Storage is per-faction integers in the campaign save.

**MC2R coverage.** The spec's `mc2.global.set_campaign(key, val)` already covers cross-mission persistent values, but a modder building a reputation system today would have to roll their own UI. Track B (FIT-driven ImGui) is the right place for the reputation-tab affordance; Track C is the right place for the data store. **Gap:** we don't have a planned namespace for "factions" as first-class entities. Worth a small `mc2.faction.*` namespace even if it's mostly a thin wrapper over `mc2.global.*` initially.

### Salvage tables / loot economy

**Today (HBS).** ModTek `ItemCollection` JSONs; shops / mission rewards reference collection IDs. RogueTech has dozens of named collections (`itemCollection_Ammo_all`, etc.) and a "Faction Store / Black Market" gated on rep ≥ 100. **Today (MW5).** Hand-coded loot tables in Resources/ JSONs.

**MC2R coverage.** Not in spec. This is a **Track E content type** (`data/loot/*.json`) plus a Lua API to roll against tables (`mc2.loot.roll("salvage_clan_heavy")`).

### Career mode (hiring, training, maintenance, market)

**Today.** RogueTech and BTA3062 both rebuild this entirely. Mountain of JSON + DLL.

**MC2R coverage.** The spec doesn't touch career mode. MC2 stock is a campaign with linear logistics, not a career sim. This is a *total-conversion* surface, not a Track-C-bindings concern; the question is whether our scripting surface lets a modder *implement* one. Answer: probably yes via `mc2.global.set_campaign` + `mc2.on_event("Mission.End", …)` + custom UI through the FIT/ImGui registry. Worth a worked example in our docs once Track C lands.

### Black market / event trees / flashpoints

**Today (HBS).** Vanilla "Flashpoints" are JSON event chains with conditional branches; RogueTech adds Custom Flashpoints. Format: `EventDef` JSON with `Options[]`, each with `RequirementList[]` and `ResultSets[]`.

**MC2R coverage.** Not directly mapped; closest analog is Track C events on the mission lifecycle. A flashpoint-style branching event is reasonable as a **data-driven feature** layered on top of Lua: `data/events/*.json` + a generic event-runner Lua script. Worth flagging for a later spec.

### Pilot abilities / leveling

**Today (HBS).** Vanilla has a pilot ability tree; ModTek + `Abilifier` + `AbilityRealizer` let modders add custom abilities and rearrange the tree. JSON file: `AbilityDef`. Tree nodes: `data/abilities/Traits/`. **Today (MW5).** PilotOverhaul replaces with hireable named pilots.

**MC2R coverage.** `mc2.pilot.*` namespace exists. MC2's pilot model is much simpler (gunnery + piloting + a few perks). Modders restoring a TT-RPG-style ability tree would do it in `data/pilots/abilities/*.json` + Lua hooks at pilot-XP-up time. **Gap:** we don't expose a "pilot levels up" event yet — adding `mc2.on_event("Pilot.LevelUp", cb)` covers the use case.

### Heat / TT rules

**Today.** MercTech and BTA both restore TT heat — ammo explosions on shutdown, crit-chance modifiers. Implementation is C#/DLL, not data.

**MC2R coverage.** Modders can override stock weapon JSON via Track E to retune heat numbers, but the *mechanic* of "ammo can explode on shutdown" requires an engine event a modder can hook (`mc2.on_event("Mech.Shutdown", cb)`). Worth adding to the event catalog.

### Paint schemes as data

**Today (MW5).** Loadout JSON includes a `PaintScheme` field referencing a paint asset. **Today (HBS).** `MechDef.json` includes hero flag + paint asset references; the texture itself is a separate art asset.

**MC2R coverage.** MC2's mech paint is a data-driven dominant-channel classifier already (`memory/mech_paint_and_mipmap_system.md`). Track D's Assimp importer + Track E's `data/mechs/<id>.json` should expose `paint: "wolfs-dragoons"` and a `data/paint/wolfs-dragoons.json` defining the channel mapping + tint colors. **Mostly already covered**; just needs the schema to be named.

---

## §4 — End-to-end user stories

### Story 1: "I built a new mech in Blender. How do I get it into MC2R?"

Modder creates `Catapult-K2` in Blender, exports `.glb` with named locator empties for each weapon hardpoint (`HP_LA_E1`, `HP_RA_E1`, `HP_LT_M1`, `HP_RT_M1`). Folder layout:

```
mods/wolfs-mechs/
  mod.json
  data/
    mechs/catapult-k2.json
    paint/wolfs-dragoons.json
  assets/
    mechs/catapult-k2.glb
    textures/catapult-k2-diffuse.png
    textures/catapult-k2-icon.tga
  scripts/
    data.lua
```

`mod.json`:

```json
{
  "id": "wolfs-mechs",
  "name": "Wolf's Dragoons Mech Pack",
  "version": "0.1.0",
  "mc2_api_version": 1,
  "depends": { "mc2": ">=0.2.0" },
  "inherits": ["stock"],
  "entrypoints": { "data": "scripts/data.lua" },
  "authors": ["Modder"]
}
```

`data/mechs/catapult-k2.json`:

```json
{
  "id": "catapult-k2",
  "chassis": "catapult",
  "tonnage": 65,
  "max_speed": 64.8,
  "armor": { "ct": 56, "lt": 42, "rt": 42, "la": 32, "ra": 32, "head": 18, "ll": 42, "rl": 42 },
  "mesh": "assets/mechs/catapult-k2.glb",
  "icon": "assets/textures/catapult-k2-icon.tga",
  "paint": "wolfs-dragoons",
  "hardpoints": [
    { "node": "HP_LA_E1", "type": "energy", "size": "large" },
    { "node": "HP_RA_E1", "type": "energy", "size": "large" },
    { "node": "HP_LT_M1", "type": "missile", "size": "medium" },
    { "node": "HP_RT_M1", "type": "missile", "size": "medium" }
  ],
  "quirks": ["jump-capable", "command-mech"]
}
```

`scripts/data.lua` (data-stage; runs at mod load before mission start):

```lua
-- All JSON in data/ is auto-loaded; this file is for programmatic registration
-- or patching of stock prototypes.
mc2.prototypes.patch("mech", "catapult", {
  variants = mc2.prototypes.get("mech", "catapult").variants .. { "catapult-k2" }
})
```

`data/paint/wolfs-dragoons.json`:

```json
{
  "id": "wolfs-dragoons",
  "primary": [0.62, 0.20, 0.20],
  "secondary": [0.10, 0.10, 0.10],
  "trim": [0.85, 0.75, 0.20]
}
```

Engine ingestion: Track D Assimp importer reads `.glb`, builds `TG_TypeMultiShape`; Track E manifest loader reads `data/mechs/catapult-k2.json`, registers as a new mech variant; Track C's `data.lua` runs once at mod load to patch the chassis variant list. To spawn from a mission, a Lua mission script calls `mc2.object.spawn("catapult-k2", {x=0, y=0, z=0}, side=1)`. Stock missions don't see the new mech unless their `.abx` references its type ID (so it's effectively a sandbox / new-mission asset; this is a known limitation, see §5).

### Story 2: "I want procedural missions"

Modder ships a procedural mission generator. Two patterns are viable:

**Template-driven** (preferred for stock-like missions). Mod ships `data/mission_templates/skirmish.json` with deploy points / objective slots / spawn weights, plus a Lua generator that picks weights and emits a concrete mission descriptor at deploy time:

```lua
-- scripts/control.lua
mc2.on_event("Career.GenerateContract", function(ctx)
  local rng = mc2.rng.new(ctx.seed)  -- seeded for determinism
  local template = mc2.data.get("mission_template", "skirmish")
  local enemy_tonnage = rng:int(180, 260)
  local lance = mc2.lance.compose({
    tonnage_max = enemy_tonnage,
    role_mix = { brawler = 2, fire_support = 1, scout = 1 },
    faction = ctx.faction,
  })
  return {
    template = "skirmish",
    deploys = template.deploys,
    spawns = lance,
    objectives = { { type = "destroy_all_enemies" } },
  }
end)
```

**Pure-Lua (Coyote-style).** Mod hooks `Mission.Begin` and dynamically adds objectives/spawns mid-mission via `mc2.object.spawn` + `mc2.objective.set_*`. Less "structured", more flexible.

Determinism is enforced by the seeded RNG (`mc2.rng.new(seed)`) — same seed produces the same mission, important for save-reload parity and for mission-share workflows. Procedural missions live alongside stock missions; the engine's mission registry is augmented by the Lua-emitted descriptors at career step time.

### Story 3: "Edit a weapon's stats"

Stock weapon: shipped JSON inside the engine's content tree (or generated from CSV at engine boot). Modder doesn't touch it.

Modder ships `mods/my-balance/data/weapons/ppc.json`:

```json
{
  "id": "ppc",
  "$patch": "merge",
  "damage": 12,
  "heat": 9,
  "range_max": 600,
  "min_range": 90
}
```

Override semantics (proposed, drawing on ModTek): default is **field-level merge by `id`**, recursive. `$patch: "replace"` does full-file replacement. `$patch: "jsonpath"` opens an "operations" mode equivalent to ModTek's advanced merging (`{op: "ArrayAdd", path: "$.alt_modes[*]", value: ...}`).

Hot-reload: editor pressed-button calls `WeaponDb::reloadFromDisk()` per spec §5.3; in-flight projectiles unaffected, next-fired use the new stats. Lua side: `mc2.prototypes.register("weapon", "ppc", {…})` is the equivalent programmatic path for mods that need to compute their values (e.g. derived from a quirk).

### Story 4: "Add a new faction with custom mech rosters and behaviors"

```
mods/draconis-combine-elite/
  mod.json
  data/
    factions/draconis-combine.json     # name, palette, default lance archetypes
    factions/draconis-combine-rosters.json  # tonnage tiers → mech variant lists
  scripts/
    control.lua                        # AI hooks for faction-specific behavior
```

`data/factions/draconis-combine.json`:

```json
{
  "id": "draconis-combine",
  "name": "Draconis Combine",
  "color": [0.78, 0.16, 0.16],
  "starting_rep": 0,
  "ally_threshold": 100,
  "hostile_threshold": -50
}
```

`scripts/control.lua`:

```lua
mc2.on_event("Lance.Generate", function(req)
  if req.faction ~= "draconis-combine" then return end
  -- Combine prefers honor-duel brawler picks at high tonnage tiers
  if req.tonnage_max > 240 then
    return mc2.lance.compose_from_roster("draconis-combine", "elite_assault")
  end
end)

mc2.on_event("AI.PickStance", function(unit)
  if mc2.object.team(unit) == "draconis-combine" and mc2.object.armor_pts(unit) > 0.6 then
    return "advance_aggressive"  -- override default cautious
  end
end)
```

Reputation system: `mc2.faction.rep_get/set("draconis-combine")` thin wrapper over `mc2.global.set_campaign("rep.draconis-combine", n)`. UI registration via Track B's FIT/ImGui registry: mod ships a `data/ui/rep-tab.fit` and registers a DataSource keyed `"FactionReputation"`.

### Story 5: "Full 3025-era total-conversion BattleTech mod"

Workload estimate, scaled from RogueTech and BTA3062 footprints:

- **Mech roster**: 60–100 chassis × 3–6 variants each ≈ 250–600 JSON files in `data/mechs/`. ~50–100 `.glb` mesh imports via Track D.
- **Weapons**: 30–80 weapon JSONs (PPC, AC/2 through AC/20, LRM-5/10/15/20, MG, lasers small/med/large, SRM-2/4/6, etc.).
- **Pilots / abilities**: 20–50 ability JSONs + portrait art.
- **Missions / flashpoints**: 30–100 mission scripts (Lua). RogueTech ships hundreds.
- **Faction set**: 6–10 factions (FedSuns, Lyran, Kurita, Davion, Liao, FRR, mercs, periphery, ComStar).
- **Lua control LoC**: 2,000–5,000 LoC across `data.lua`, `control.lua`, mission scripts, lance generators.

Maps onto our roadmap as: Track D delivers the mesh import; Track E delivers the JSON manifest layer; Track C delivers the Lua scripting and event hooks. The unsolved residual is **AI / FSM behaviors that stock corebrain.abx can't express** — the `magic*` family of FSM primitives. Per memory `magic_abl_contamination_rule.md` and the lua-api-surface-catalog §Q3, these primitives are stock-incompatible. A real total conversion needs a *replacement corebrain* shipped as a Lua-or-equivalent AI module, not as `.abx` patches. That AI replacement is currently outside the spec — it would be the natural Track F.

---

## §5 — Gaps in our plan

Concrete capabilities a BT-IP modder coming from MW5 / HBS expects, that our roadmap does **not** yet cover:

1. **JSONPath / advanced merge operators.** §5.2 manifest is fine; §Track E currently implies field-level merge by ID. ModTek modders routinely splice into arrays via JSONPath. Adding a `$patch` mode (`merge` | `replace` | `ops`) closes this. Lands in Track E spec.
2. **Per-type folder auto-discovery.** Both ModTek and YAML auto-load all JSON in known subfolders by *type* (`data/mechs/*.json` → mech defs). Spec §5.1 already implies this directory structure but the loader behavior — "anything in `data/mechs/` is a mech def, no manifest entry needed" — needs to be explicit. Otherwise modders write painful manifest blocks.
3. **First-class faction concept.** No `mc2.faction.*` namespace planned. Add a thin one in Track C M1.
4. **Quirk system.** No engine seam for "passive per-chassis modifier table consulted at damage time / movement time / heat time". Open question: do we add a generic modifier registry, or punt to mod-side Lua hooks on every relevant event? The latter is simpler but less perf-friendly. Worth a design doc.
5. **Pilot.LevelUp / Mech.Shutdown / Lance.Generate / Career.GenerateContract events.** Several of the user stories assume these events; the lua-api-surface-catalog enumerates ~290 ABL functions but the *event taxonomy* is sparser. Need an event catalog spec parallel to the function catalog.
6. **Loot table / item collection schema.** `data/loot/*.json` + `mc2.loot.roll(id)` — not in spec. Modest add.
7. **Flashpoint / event-tree schema.** `data/events/*.json` with `Options[]` + `Requirements[]` + `Results[]`. Bigger add; can wait for a real total-conversion to drive the design.
8. **Replacement AI / corebrain hook (Track F).** Per §Q3 of blocking-questions, `magic*` FSM primitives are gated on a replacement AI brain that's currently not on the roadmap. A serious total conversion blocks here.
9. **In-game mod browser / load-order UI.** Both ModTek and MW5 have one. Track B (ImGui) is the right home. Not blocking but expected.
10. **Modder-facing schema docs / JSON schema files.** Modders use VSCode + JSON Schema for completion. We should publish `mech.schema.json`, `weapon.schema.json`, `quirk.schema.json` alongside the API version.

---

## §6 — Free wins to adopt

Patterns we should copy verbatim with attribution:

1. **ModTek's `Manifest[]` array of `{Type, Path, Id?, ShouldMergeJSON?}`** as the *fallback* path when a modder needs to register content outside the standard folders. Our auto-discovery covers 95%; this covers the rest. (Source: `BattletechModders/ModTek/doc/MOD_JSON_FORMAT.md`.)
2. **ModTek's advanced-merge JSONPath operators** (`ObjectMerge`, `ArrayAdd`, `ArrayConcat`, `Remove`, `Replace`). One spec, multiple ecosystems converged on it — it's the right granularity. (Source: `ADVANCED_JSON_MERGING.md`.)
3. **YAML's `quirks.json` shape** — `{id, name, description, color?, properties{}}` — for our quirk schema. (Source: `mw5mercs-modding/yaml-docs`.)
4. **MW5's three-file split per chassis (HPS / MDA / Loadout)** for hardpoints / chassis stats / variant loadouts. We can collapse to two files (chassis-with-hardpoints, variant-with-loadout) since MC2 has fewer slot types, but the *separation of variant from chassis* is right. (Source: `mechwarrior5modding.fandom.com/wiki/Hardpoints,_Mech_Data,_and_Loadouts`.)
5. **RogueTech's `itemCollection_*` naming convention** for loot tables + the "shop references collection IDs" indirection. Lets a modder add an item to all stores by editing one collection. (Source: RogueTech wiki "Shops and Stores".)
6. **MW5's `Resources/` vs `Paks/` split** — JSON config separate from compiled assets. Our `data/` vs `assets/` split is the same idea; emphasize that loose JSON is the modder norm.
7. **`OptionallyDependsOn` (load-after-if-present)**, distinct from `DependsOn` (hard). Compatibility patches use this. We currently only have `depends`. (Source: `MOD_JSON_FORMAT.md`.)
8. **Skipping `_DEBUG` builds entirely as a modder-test path**; instead, ModTek has a `Settings` blob passed to the DLL as JSON for runtime config. We should mirror this in `mod.json` — a `settings: {}` blob accessible from Lua via `mc2.mod.settings("my-mod")`.

---

## §7 — Open questions

1. **Patch operator surface area.** ModTek's `ObjectMerge` / `ArrayAdd` / `Remove` / `Replace` is one shape; JSON Patch (RFC 6902) is another (`add` / `remove` / `replace` / `move` / `copy` / `test`). Pick one and stick. JSON Patch has the stronger spec but ModTek's flavor is what BT modders know.
2. **Auto-discovery vs manifest registration default.** If we auto-discover everything in `data/mechs/*.json`, what happens when a mod ships a partial JSON intended only as a patch? We probably need a `_patch.json` filename suffix or a manifest-level marker. Not blocking but needs a decision before Track E ships.
3. **Where does the quirk modifier table actually get consulted?** Damage path, movement path, heat path are all hot. A generic modifier registry consulted on every relevant event has a perf cost. Worth a perf budget spike.
4. **AI replacement (Track F) sequencing.** A 3025-era total conversion blocks on it. Is it on the roadmap or explicitly punted? Spec should state.
5. **Mission-template schema.** §4 Story 2 hand-waves a `data/mission_templates/skirmish.json`. Real schema needs deploy-point semantics, spawn-point semantics, objective slots. Could borrow from Coyote's Mission Pack source if public.
6. **Modder-facing documentation site.** Both BT ecosystems have wikis (BTAWiki, RogueTech Fandom, mw5mercs.com docs). Whose responsibility is ours? Track B / docs / community?
7. **Hot-reload during mission play.** Spec §5.3 covers per-subsystem `reloadFromDisk()`. What's reloadable mid-mission vs only between missions? Weapon stats: probably mid-mission OK. Mech chassis: probably between-mission only. Needs an explicit table.

---

## Sources

- ModTek repo and docs: `github.com/BattletechModders/ModTek` (`doc/MOD_JSON_FORMAT.md`, `doc/ADVANCED_JSON_MERGING.md`, `doc/MOD_JSON.md`).
- RogueTech: `github.com/BattletechModders/RogueTech`, `roguetech.fandom.com/wiki/`.
- BTA3062: `github.com/Syrkres/BTA3062`, `github.com/BattleTech-Advanced-3062`, `bta3062.com`.
- YAML / MW5 modding docs: `github.com/mw5mercs-modding/yaml-docs`, `github.com/mw5mercs-modding/harjel`, `mechwarrior5modding.fandom.com`.
- MW5 first-party docs: `static.mw5mercs.com/docs/MW5Mercs_Mod_Editor_Guide_(v2.3).pdf`, Knight Ravens guide `knightravens.com/2019/12/18/mechwarrior-5-modding-guide-1/`.
- Coyote's Mission Pack: `nexusmods.com/mechwarrior5mercenaries/mods/263`.
- Yet Another Mechlab: `nexusmods.com/mechwarrior5mercenaries/mods/459`.
- Yet Another IS / Clan / Legendary Mech: `nexusmods.com/mechwarrior5mercenaries/mods/964`, `mods/966`.
- Lore-based Mech Variants — YAML Edition: `nexusmods.com/mechwarrior5mercenaries/mods/493`.
- MercTech: `nexusmods.com/mechwarrior5mercenaries/mods/48`, `mechwarrior5modding.fandom.com/wiki/MercTech`.
- TTRulez_AIMod3: `nexusmods.com/mechwarrior5mercenaries/mods/269`.
- Skill Tree Rebuild: `nexusmods.com/battletech/mods/647`. SkillBasedInit: `github.com/BattletechModders/SkillBasedInit`. Abilifier: `github.com/BattletechModders/Abilifier`. AbilityRealizer: `github.com/BattletechModders/AbilityRealizer`.
- Pilot Overhaul: `github.com/Wpnx330/PilotOverhaul`, `github.com/blastjack85/PilotOverhaul-Eternal`.
- MW5 Mod Compatibility Pack: `nexusmods.com/mechwarrior5mercenaries/mods/168`.
- Chassis Editor: `nexusmods.com/battletech/mods/578`.
