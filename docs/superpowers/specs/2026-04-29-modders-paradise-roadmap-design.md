# Modder's Paradise Roadmap — Design Spec

**Date:** 2026-04-29
**Status:** Draft
**Scope:** Post–Phase-B/C strategic roadmap for converting MC2 from a closed legacy engine into a modern moddable RTS, keyed off the OpenRA + Factorio + Fabric reference stack. Defines conventions, sequencing, ownership, and explicit rejections. Does **not** define M-slice implementation plans — those are written per-track when their preconditions are met.

---

## 1. Framing

### Problem

A modder who shows up to MC2 today has no viable path:

- **Mission scripts** are `.abx` — proprietary compiled binaries with no public toolchain. ABL extension stubs (Carver5O / Omnitech work) make legacy ports possible but do not give a new modder a way to author a new mission from scratch.
- **Mech models** are `.ase` — exported from 25-year-old 3DS Max plugins that no one has anymore. The Assimp importer ([`docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`](2026-04-27-assimp-mech-importer-design.md)) is the only viable answer.
- **Stats and content data** live in a mix of CSVs, FST archives, and compiled headers. Some are loose-file-overridable (the `data/art/`, `data/tgl/`, `data/objects/` system); others are not. There is no consistent convention.
- **Assets** are 1999-era TGAs at low resolutions, with the upscaler workflow as a sidecar.

Every one of these is an onboarding wall. The modder community we want to invite is not going to climb them.

### Goal

A modder downloads MC2, downloads a `.zip` mod from a forum, drops it in `mods/`, launches the game, and the new content shows up. They want to make their own content: they edit a JSON, write a Lua script, drop a `.glb` file, hit a "reload" button in an in-game inspector, and see the result in seconds.

This is not far-fetched. OpenRA modders do exactly this today. Factorio modders have done it for 12 years. The conventions are converged and we can adopt them.

### Non-goal

This roadmap does **not** propose an engine rewrite. The ABL parser stays, the FST reader stays, the GameObject hierarchy stays, the cull-gate chain stays. Modder-paradise capabilities **layer over** the legacy engine via env-gated, sidecar-shaped additions. This invariant is non-negotiable and the rejection list (§9) is mostly about enforcing it.

---

## 2. Architectural principle: sidecar layers, never engine rewrite

The **stock install must remain playable** rule (worktree CLAUDE.md, [`memory/stock_install_must_remain_playable.md`](../../../memory/stock_install_must_remain_playable.md)) generalizes here:

- Stock content (.abx missions, .ase mechs, FST archives) keeps loading via legacy paths, unmodified.
- Modder content (.lua scripts, .glb mechs, .json manifests) loads via new sidecar paths.
- Both paths coexist permanently. There is no "transition." Stock missions will never be rewritten in Lua.
- Modder content can override stock assets via the existing loose-file-overrides-FST mechanism. It can never *replace* the legacy loaders themselves.

This is the same shape as the GPU-thinning render path: legacy CPU vertex building stays, GPU thin records run alongside, env gates pick the path. The discipline that's working for the renderer applies cleanly to the content pipeline.

---

## 3. Reference stack

The OSS modding-conventions space has converged. We adopt three references:

| Reference | What we steal | Why |
|-----------|---------------|-----|
| **[OpenRA](https://www.openra.net/)** | Mod directory layout, `mod.yaml`-style manifest, YAML/JSON for entity definitions, Lua for missions, mod inheritance (`Inherits:`) | Genre-correct. RTS modders coming from OpenRA recognize the shape. |
| **[Factorio](https://wiki.factorio.com/Tutorial:Modding_tutorial)** | Two-stage `data.lua` / `control.lua` split, `info.json` manifest with version-constrained dependencies, sandboxed Lua, in-game error console | Best-in-class modder UX in the OSS space. The data/control split is the conceptual primitive. |
| **[Fabric (Minecraft)](https://fabricmc.net/)** | `fabric.mod.json` shape (id, version, depends, entrypoints) | Most-copied mod manifest format in the last 5 years; modders from any genre recognize it. |

**Tier-2 references, study but don't copy verbatim:** Spring/BAR (widget/gadget split), 0 A.D. (JSON entity templates), Bevy (asset hot-reload via reflection).

**Aware of, not adopting:** Paradox script DSL (wrong genre), Bethesda ESP/BSA (engine-welded), Wasm component model (north-star but ergonomically premature in 2026).

---

## 4. Layer diagram

```
Modder authoring                  Engine ingestion                 Runtime
─────────────────                 ────────────────                 ───────

mods/<modid>/                                                      
  mod.json          ──────────►  ModRegistry                       
  data/*.json       ──────────►  Schema-validated loader  ──────►  Existing internal
  scripts/*.lua     ──────────►  Sol2 Lua VM                       structures
                                 (sandboxed)            ──────►    (mech stats,
  assets/                                                           weapon tables,
    mechs/*.glb     ──────────►  Assimp importer        ──────►    TG_TypeMultiShape,
    textures/*.png  ──────────►  Loose-file FST override            mission state,
    audio/*.ogg     ──────────►  Loose-file FST override            ABL VM)
                                                                   
  overrides/        ──────────►  Loose-file FST override
  
                                     ▲
                                     │
                                In-engine ImGui inspector
                                + hot-reload trigger
                                (collaborator track)
```

Every arrow into the engine terminates at an existing internal structure. Nothing in the engine learns "this came from a mod" — once ingested, modder content is indistinguishable from stock content.

---

## 5. Conventions to lock in early

These are cheap to get right now and expensive to fix later. All four belong in a single foundational spec before any track lands code.

### 5.1 Mod directory layout

```
mods/
  <modid>/
    mod.json              # Manifest (§5.2)
    data/
      mechs/*.json        # Stats — schema-validated
      weapons/*.json
      pilots/*.json
      missions/*.json     # Mission metadata (briefing, deploy, etc.)
    scripts/
      data.lua            # Load-time prototype registration (Factorio-style)
      control.lua         # Runtime event handlers (Factorio-style)
      missions/<id>.lua   # Per-mission scripts (OpenRA-style)
    assets/
      mechs/*.glb         # Assimp-imported
      textures/*.png      # Or .ktx2 once VRAM matters
      audio/*.ogg
    overrides/            # Mirrors stock data/ tree; uses existing FST override
```

The `data/` vs `assets/` vs `scripts/` split mirrors OpenRA. The `data.lua` / `control.lua` split mirrors Factorio. The `overrides/` directory plugs into the existing loose-file-overrides-FST mechanism.

### 5.2 Manifest format (`mod.json`)

Fabric-shaped JSON, OpenRA-flavored fields:

```json
{
  "id": "magic-unofficial-expansion",
  "name": "Magic's Unofficial Expansion",
  "version": "1.2.0",
  "mc2_api_version": 1,
  "depends": {
    "mc2": ">=0.2.0"
  },
  "inherits": ["stock"],
  "entrypoints": {
    "data":    "scripts/data.lua",
    "control": "scripts/control.lua"
  },
  "authors": ["Magic"],
  "description": "Restores cut content from MC2 development build."
}
```

`mc2_api_version` is the stable-mod-ABI declaration (§5.4). `depends` uses SemVer-style version ranges. `inherits` enables OpenRA-style mod composition (a patch mod inherits stock; a total conversion does not). `entrypoints` are paths into the mod's own tree.

### 5.3 Hot-reload contract

Every subsystem that ingests modder-editable content must expose a `reloadFromDisk()` entry point. The contract:

- Returns `bool` indicating success.
- Logs `[<SUBSYS> v1] event=reload status=<ok|failed> path=<...>` on completion.
- On failure, leaves the previous state intact (no half-loaded data).
- Is callable from the ImGui inspector and from a Lua console command.

This is the seam between the engine work (you) and the inspector work (collaborator). Without the contract, hot-reload becomes a per-subsystem ad-hoc retrofit. With the contract, every new manifest gets reload for free.

### 5.4 Stable mod ABI

`mc2_api_version` in the manifest declares which API surface the mod expects. The engine maintains a single integer (start at `1`), bumps it when a breaking change lands in the modder-facing API, and emits a `manifest_api_mismatch` warning at mod load time when versions disagree.

Modders tolerate breaking changes if they're announced. They do not tolerate silent ones. The integer + a single `docs/mod-api-changelog.md` is enough discipline.

### 5.5 Action and DataSource registries (string-keyed dispatch)

Modder-authored UI definitions (FIT files) and modder-authored mission scripts (Lua) reference engine functionality by **string key**, never by direct code pointer. Two registries enforce this:

- **`ActionRegistry`** — maps action keys (e.g. `"MechBay.BuySelected"`) to engine-side handler functions. FIT button declarations carry `Action="MechBay.BuySelected"` strings; the registry resolves them at click time.
- **`DataSourceRegistry`** — maps data-source keys (e.g. `"MechInventory"`) to read-only data providers. FIT element declarations carry `DataSource="MechInventory"` strings; the registry resolves them at render time.

This is the UI-side analog to the §5.4 stable mod ABI. The benefits are the same:

- **No arbitrary code execution.** Modder content can only invoke registered actions; it cannot inject C++ or run unrestricted logic through the UI layer.
- **Versionable surface.** Adding/removing/renaming actions is visible in one place, easy to mark deprecated, easy to ship a mod-api-changelog entry for.
- **Lua dispatch interop.** The same registry can route `"Action.MyMod.OpenMarket"` to a *Lua*-side handler when Track C lands, making FIT files able to dispatch into mod scripts. Design the dispatch table to admit both backends from day 1, even if Lua dispatch isn't wired until Track C.

Same conceptual primitive applied at a different layer: **strings as the modder-stable contract; code on the other side of the lookup table.** This pattern recurs throughout — manifest fields, action keys, data-source keys, Lua binding names.

---

## 6. Track sequence

Five tracks, each with its own preconditions. Tracks A–C run in parallel; D–E gate on the others.

### Track A — Render headroom (you, in flight)

**Status:** in flight. M2 chain landed (`Terrain::render drawPass` 25 ms → 1.46 ms; FPS 50–60 → 100–145; 14,000/14,000 quads on the fast path). Two pending sub-slices before Phase B can start, both surfaced by [`explorations/2026-04-29-track-a-render-headroom-status.md`](../explorations/2026-04-29-track-a-render-headroom-status.md):

1. **`quadSetupTextures` next slice** — still ~1.17 ms, has its own handoff doc at [`specs/2026-04-29-quadsetuptextures-next-slice-handoff.md`](2026-04-29-quadsetuptextures-next-slice-handoff.md).
2. **Smoke runner fast-path env-var passthrough** — `scripts/run_smoke.py:232–237` doesn't propagate FASTPATH env vars, so tier1 covers only the legacy path. Five-line patch; closes a real gate-validation gap.

**Phase B / Phase C:** GPU lighting (drops `lightRGBs` from 32B thin record → 16B) and CPU-only-does-visibility-and-admission. No standalone spec yet — proposed first slice (B0: VS-side diffuse with no struct change; B1: drop `lightRGBs`, carry selection mask in `flags`) lives in the Track A status doc.

**Why this is a precondition:** every modder-facing capability is gated on having CPU headroom. If the engine is CPU-pinned at 60 mechs today, no amount of Lua scripting buys SupCom-scale battles. Phase B + C is what makes 200+ mechs renderable. Don't context-switch off this until it lands.

**Outcome gate:** `quadSetupTextures` < 1 ms at max Wolfman zoom on tier1 missions, fast-path covered by smoke. Recipe SSBO + 16B thin record default-on.

### Track B — UI subsystem replacement: FIT-driven ImGui + Editor migration

**Status:** in flight, two converging surfaces. Collaborator (Methuselas) is shipping editor restoration under its own Editor SemVer (`0.14.0-editor` as of writing) and has authored a v0.1 design doc for the in-game UI replacement: *MC2R ImGui + FIT UI Integration Design* (collaborator-owned; summary in [`memory/imgui_fit_ui_design.md`](../../../memory/imgui_fit_ui_design.md)).

**Scope.** ImGui becomes the only UI renderer. **FIT remains the modder-facing UI definition format** — Wolfman, Omnitech, and future TC packs continue to ship FIT files unchanged. UI Pack system (`data/defs/ui/<pack>/`) supports per-mod theming and full UI replacement. mc2res.dll deprecates in favor of `data/defs/text/` (Linux portability win).

**Sub-slice sequence:**
- **B0** — ImGui bridge: vendor + render-loop integration (composite slot in post-process chain) + input gate (`WantCapture*` interleaved with existing input system)
- **B1** — FIT loader → UI model intermediate representation
- **B2** — `ActionRegistry` + `DataSourceRegistry` stub (per §5.5; no Lua dispatcher yet)
- **B3** — **Viewer (Mechlopedia) port** — MVP gate. Proves the pipeline end-to-end (list + detail + image + input).
- **B4** — `StringCatalog` + `data/defs/text/` + `mc2res.dll` extraction tool
- **B5** — Simple screens (Options, Main Menu)
- **B6** — Logistics + MechBay
- **B7** — In-mission HUD *(separate render-state contract — HUD has tighter timing and post-process interactions than menus; see Risks)*
- **B8** — Legacy GUI removal
- **B-mod** — `UIRegistry` page-injection (lights up modder-side; `[Page:Market] Parent=MechBay`)
- **B-pack** — UI Pack switcher, `[UI] Pack=<name>` config

**Editor track (parallel, Methuselas-owned):** the existing MFC-shell-with-embedded-SDL/GL editor is being restored independently — selection, camera, tacmap, FIT-backed resource catalog (rule already in force at editor 0.3.0: *new Editor resource names belong in FIT data, not new mc2res.dll entries*). Editor SemVer is intentionally independent of engine SemVer. Editor migration to the ImGui pipeline is a deliberate *later* milestone — MFC-in-MFC ImGui integration is a different shape than ImGui-in-SDL and shouldn't be conflated with B0–B8.

**Risks (from the design v0.1 review):**
- **Menus vs in-mission HUD.** ImGui composites well as menu/screen overlay; HUD has tighter render-state requirements (post-process interactions tracked in [`memory/`](../../../memory/) — `gos_State_IsHUD` buffering). Treat B7 as separate.
- **Input event coexistence.** `gos_GetKey` is non-consuming + `KeyboardFlush` incomplete (memory). `WantCaptureKeyboard/Mouse` gate must interleave correctly or typing in fields will move the camera.
- **FIT loader scope.** Reuse-vs-rewrite of any existing FIT parser materially changes B1 effort. To confirm with collaborator.
- **Render slot for ImGui composite.** Likely after post-process resolve so bloom/FXAA don't hit menu text. Coordinate with Track A's render-contract registry ([`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md)).
- **Background-art-heavy legacy menus.** ImGui can do bitmap backgrounds via `ImageBackground` but the style is against the grain. Visual prototype before B5.

**Outcome gate (MVP):** Viewer (Mechlopedia) renders fully via ImGui+FIT pipeline, with vanilla and at least one mod UI pack switching cleanly. Each subsystem ingesting FIT or text files exposes the §5.3 hot-reload contract. The full B-chain (B5–B8 + legacy GUI removal) is months of work; **the right gate to evaluate the design is B3, not B8.**

### Track C — Sol2 + Lua wiring (~3–5 days, parallel)

**Preconditions:** none. Sol2 is header-only; Lua 5.4 is C source vendored alongside (Tracy is the precedent for in-tree-built dependencies).

**Design completeness:** Track C is the most thoroughly designed track in the roadmap as of 2026-04-30. Six artifacts:

| Doc | Coverage |
|-----|---------|
| [Status snapshot](../explorations/2026-04-29-track-c-lua-scripting-status.md) | ABL surface inventory, VM lifecycle today, sandbox baseline |
| [Implementation shape](../explorations/2026-04-30-track-c-lua-implementation-shape.md) | Vendoring layout, `LuaVM` class, lifecycle wiring, 5-commit sequence |
| [API surface catalog](../explorations/2026-04-30-track-c-lua-api-surface-catalog.md) | All ~289 ABL functions classified; ~85-binding STABLE v1 tier; namespace tree |
| [Sandbox + errors](../explorations/2026-04-30-track-c-lua-sandbox-and-errors.md) | Stdlib whitelist, file I/O sandbox, 20-case test battery, 64 MiB cap, 5M-opcode/tick |
| [Loading + lifecycle](../explorations/2026-04-30-track-c-lua-loading-lifecycle.md) | Kahn's BFS load order, stage gates, prototype freeze, save/load via `mc2.persist`, profile-launcher hook |
| [Trampolines + tests](../explorations/2026-04-30-track-c-lua-trampolines-and-tests.md) | **Authoritative trampoline pattern** (corrected ABL ABI: caller-pushed LIFO stack, not `(long*, long*)`); 4 worked bindings; perf budget; doc-gen pattern |

**Minimum viable scope (per implementation-shape §10):**
- Vendor Sol2 (header-only) + Lua 5.4 (C source) under `3rdparty/sol/` and `3rdparty/lua/`
- New top-level `modding/` directory with `lua_vm.{h,cpp}` and `lua_bindings_mc2.cpp`
- Single sandboxed `sol::state` per mission load, stage-gated views for `data.lua` vs `control.lua`
- ~85 STABLE-tier bindings as Sol2 trampolines following the corrected pattern (most are ~3-line wrappers around existing `execXxx` ABL primitives)
- Demo `mods/test/scripts/missions/demo.lua` running alongside an `.abx` mission

**Why now:** this is the seam through which all future modder-facing gameplay logic flows. Once the VM exists and the binding pattern is established, every new ABL extension stub gets a Lua mirror trivially. The ~289 active ABL extension primitives become ~85 immediate Lua API entries (the rest classified `EXPERIMENTAL/INTERNAL/DEPRECATE` per the catalog).

**Blocking architectural questions** (resolved before C-3 implementation): see [`2026-04-30-track-c-blocking-questions-resolution.md`](../explorations/2026-04-30-track-c-blocking-questions-resolution.md). The three blockers are: (1) ABL stack reentrancy when Lua handlers fire inside `brain->execute()`, (2) namespace tree lock-in, (3) magicpatrol/guard/escort shadow rule with stock `corebrain.abx`.

**Outcome gate:** a `.lua` file in `mods/test/scripts/missions/demo.lua` boots a mission, spawns a mech, prints to console, fires a timer callback at +200ms, runs to completion. Tier-1 smoke clean. `[LUA v1] event=error` log lines absent.

### Track D — Assimp mech importer (2–4 days, gated on Track A)

**Status:** spec exists at [`2026-04-27-assimp-mech-importer-design.md`](2026-04-27-assimp-mech-importer-design.md).

**Preconditions:** none structurally, but pace-wise wait for Phase B to land so the rendering side has headroom for slightly heavier mech meshes than `.ase` produces.

**Outcome gate:** a `.glb` exported from Blender drops into `mods/<id>/assets/mechs/`, gets ingested by the Assimp pipeline, terminates at `TG_TypeMultiShape`, and renders in-game indistinguishably from a stock `.ase` mech.

### Track E — JSON-manifest path for one new subsystem (1–2 days, gated on Track C)

**Preconditions:** Track C wiring exists, so `data.lua` can register prototypes and the JSON loader can call into Lua-registered factories.

**Scope:** pick one subsystem (suggested: weapons or pilots — high modder-edit-frequency, low engine-internal-coupling). Migrate its loader to the JSON path. Existing CSV path stays as fallback for stock content.

**Why one subsystem:** establish the pattern. Don't migrate everything. The split between engine-CSV (internal indexes — `asset_sizes.csv`, etc.) and modder-JSON (modder-edited stats) is by *audience*, not by *novelty*.

**Outcome gate:** a `mods/<id>/data/weapons/my_ppc.json` adds a new weapon to the game, hot-reloadable, with schema validation errors surfaced in the inspector.

---

## 7. Track ownership

| Track | Owner | Parallelism |
|-------|-------|-------------|
| A — Render headroom | rjm | In flight; quadSetupTextures slice + smoke env-var passthrough still pending |
| B — UI subsystem (FIT + ImGui) | Methuselas | In flight; v0.1 design doc authored |
| B-editor — MFC editor restoration | Methuselas | In flight; independent SemVer, 14 versions shipped |
| C — Sol2 + Lua wiring | Whoever has bandwidth | Parallel; ~1–2 days |
| D — Assimp importer | Either | Plan ready (1547 lines), 2-gap MVP scoped; gated on Phase B |
| E — JSON-manifest pilot | Either | Gated on Track C |

**Coordination seams:**
- §5.3 hot-reload contract — interface between rjm (engine subsystems) and Methuselas (ImGui inspector). Lock this in a short follow-up doc before B0 lands code.
- §5.5 ActionRegistry — interface between Track B (UI dispatch) and Track C (Lua handlers). Design the dispatch table to admit both backends from day 1, even if Lua wiring lands later.
- Render composite slot — interface between Track A (render contract / post-process chain) and Track B0 (ImGui FBO target). Coordinate before B0 to avoid bloom/FXAA hitting UI text.

**On Editor independence:** the editor maintains its own SemVer (`X.Y.Z-editor`) deliberately separate from engine SemVer. This is the same engine-vs-distribution naming principle that applies to the future open-source rename question — borrow it rather than fight it.

---

## 8. Conventions, principles, discipline

### 8.1 Engine-CSV vs modder-JSON

Engine-internal indexes (`asset_sizes.csv`, FST manifests, etc.) stay CSV — they're for engineering use, never edited by modders. Modder-facing data (mech stats, weapon tables, pilot rosters, mission metadata) starts JSON. **Do not migrate existing engine CSV to JSON unless a modder has a concrete reason to edit it.**

### 8.2 Two-stage Lua: data vs control

Borrow Factorio's split verbatim:

- `data.lua` runs at mod load. Registers prototypes (mechs, weapons, missions) into the engine's tables. **No game-state access** — game isn't running yet.
- `control.lua` runs once the game loop is live. Hooks events (mission start, mech destroyed, timer tick). **No prototype registration** — those are frozen.

This separation prevents whole classes of "mod broke after savegame load" bugs and makes mod loading deterministic. It is the single most-imitated piece of Factorio's architecture for a reason.

### 8.3 Sandboxed Lua

Strip `os.execute`, `os.remove`, `io.open` (whitelist read-only access to mod's own directory), `package.loadlib`, `debug.*`. Sol2 makes this trivial — start with a clean environment table and explicitly allow safe primitives.

Modders will write malicious mods. We will not give them shell access by accident.

### 8.4 Mod-as-directory, not mod-as-archive (initially)

Keep mods as plain directories during early adoption. Zip-as-mod can come later when distribution matters. Iteration speed for modders matters more than distribution polish in year 1.

### 8.5 New code in new directories

Borrow Gemini's narrow-version `LegacyNamespace` idea: don't reorganize `mclib/`, `gameos/`, the ABL parser. Don't add to them either. New modder-facing code lives in new top-level directories (`mods/`, `modding/`, etc.). This is a discipline, not a refactor.

### 8.6 ECS shape for new subsystems only

Component-array layout (SoA) is the right pattern for any new subsystem we add (projectile manager, particle table, GPU static-prop registry, recipe SSBO). The renderer is already moving in this direction implicitly via thin records.

**Do not migrate `GameObject` to ECS.** The cull-gate chain ([`memory/cull_gates_are_load_bearing.md`](../../../memory/cull_gates_are_load_bearing.md)) is fused to it. Migration is an engine rewrite, not a refactor, and breaks every save game.

---

## 9. Explicit rejections

These have come up in roadmap discussions and will come up again. Documenting the rejection rationale here so it doesn't have to be re-litigated every quarter.

| Proposed addition | Status | Rationale |
|-------------------|--------|-----------|
| **EnTT / full ECS migration** | **Rejected** | Cull-gate chain fused to GameObject; migration is multi-month engine rewrite breaking all saves. Use SoA *for new subsystems only* — soft no on conversion, firm yes on ECS-shape for greenfield code. |
| **SDL3 migration** | **Rejected** | Zero modder-facing benefit. Window/input layer works. Steam Deck / macOS support is downstream of gameplay shipping. |
| **Vulkan / MoltenVK** | **Rejected** | Pure aspiration. Current GPU wins on OpenGL 4.3. Nothing about current bottleneck is API-level. Revisit only if AMD driver wall forces it. |
| **PhysFS** | **Rejected** | Existing loose-file-overrides-FST system works. Replacement is gratuitous plumbing. |
| **spdlog** | **Rejected** | Existing `[SUBSYS v1]` pattern works. Migration is plumbing for zero modder benefit. |
| **Taskflow / job system** | **Deferred** | Single-threaded CPU bottleneck is being deleted by GPU migration; orchestrating disappearing work is wasted. Revisit *only* when AI brain becomes measurably the wall (post-Phase-C, scaling toward 200+ units). |
| **Wasm component model for mod code** | **Deferred** | Is the genuine north-star direction in 2026+. Not yet ergonomic enough to choose over Lua. Revisit in 2 years; don't be surprised by the term in the meantime. |
| **KTX2 / Basis Universal textures** | **Deferred** | Real win when VRAM matters. Today's bottleneck is CPU. Revisit when modders ship 8K mech textures and VRAM pressure becomes measurable. |
| **Total ABL replacement** | **Rejected permanently** | `.abx` stock missions stay supported via the existing parser. Lua sits next to ABL, not on top of it. Stock-install-playable rule is non-negotiable. |
| **"Wrap legacy in `LegacyNamespace`"** | **Soft no** | Touch-every-file refactor with no payoff. Adopt the *narrow* version (§8.5): don't reorganize legacy, just stop adding to it. |

---

## 10. Open design questions (resolve in follow-up specs)

These are acknowledged as unresolved; each becomes its own short spec when the relevant track is ready to start.

1. **Mod load order and conflict resolution.** OpenRA uses mod inheritance + load order; Factorio uses dependency-graph topological sort. Probably adopt Factorio's model since it scales better with many mods, but unresolved until Track E.
2. **Multiplayer determinism with Lua.** Lua is non-deterministic across implementations under some conditions (table iteration order, floating-point reordering). If multiplayer ever ships, mods need a deterministic execution mode. Spring/BAR's widget/gadget split is the reference. Defer.
3. **Mod distribution.** Steam Workshop, ModDB, GitHub releases, custom in-engine browser? Probably defer until there's a critical mass of mods — distribution polish before content is a known anti-pattern.
4. **Asset cooking pipeline.** Should mods ship raw `.glb` and have the engine cook on first load (Assimp importer's current shape) or pre-cook into a binary cache? Initial answer: raw `.glb` + on-load cooking, with the binary cache as a transparent optimization. Revisit if load times become an issue.
5. **JSON Schema publication.** Once the JSON-manifest path lands, publish JSON Schema files so VS Code gives modders autocomplete. Cheap once one schema exists; defer until §6 Track E concrete.

---

## 11. Success criteria

**Year-1 goal:** a modder can install MC2, drop a mod into `mods/`, see it load, and use ImGui to inspect/reload. The mod can ship: new mechs (.glb), new weapons (.json), one new mission (.lua), and asset overrides (existing FST mechanism). Stock missions still play.

**Anti-goal:** a project that looks like a modern engine on paper but doesn't make modders' lives any better than the legacy state. If we ship Vulkan + ECS + SDL3 + Wasm and a modder still can't author a mech without 3DS Max, we have failed.

The right concrete progress signal is **a modder shipping a non-trivial mod** — not a rewrite count. Optimize for that.

---

## 12. References

**Sister specs in this worktree:**
- [`2026-04-27-assimp-mech-importer-design.md`](2026-04-27-assimp-mech-importer-design.md) — Track D
- [`2026-04-23-asset-scale-aware-rendering-design.md`](2026-04-23-asset-scale-aware-rendering-design.md) — texture pipeline scale handling
- [`2026-04-22-safer-release-defaults-design.md`](2026-04-22-safer-release-defaults-design.md) — sidecar release pattern precedent
- [`2026-04-26-render-contract-registry-design.md`](2026-04-26-render-contract-registry-design.md) — render-state registry the ImGui composite slot plugs into

**Track-status explorations (2026-04-29):**
- [`../explorations/2026-04-29-track-a-render-headroom-status.md`](../explorations/2026-04-29-track-a-render-headroom-status.md)
- [`../explorations/2026-04-29-track-b-imgui-hotreload-status.md`](../explorations/2026-04-29-track-b-imgui-hotreload-status.md)
- [`../explorations/2026-04-29-track-c-lua-scripting-status.md`](../explorations/2026-04-29-track-c-lua-scripting-status.md)
- [`../explorations/2026-04-29-track-d-assimp-importer-status.md`](../explorations/2026-04-29-track-d-assimp-importer-status.md)

**Implementation-shape docs (2026-04-30):**
- [`../explorations/2026-04-30-track-a-phase-b-implementation-shape.md`](../explorations/2026-04-30-track-a-phase-b-implementation-shape.md) — Phase B byte-level layout + S0/S1 sequencing
- [`../explorations/2026-04-30-track-b-imgui-implementation-shape.md`](../explorations/2026-04-30-track-b-imgui-implementation-shape.md) — frame-loop slot, FIT-as-existing-parser finding, hot-reload contract C++ shape
- [`../explorations/2026-04-30-track-c-lua-implementation-shape.md`](../explorations/2026-04-30-track-c-lua-implementation-shape.md) — vendoring, LuaVM class, 5-commit sequence (ABI section superseded — see trampolines doc)

**Track C deep-dives (2026-04-30):**
- [`../explorations/2026-04-30-track-c-lua-api-surface-catalog.md`](../explorations/2026-04-30-track-c-lua-api-surface-catalog.md) — full classification of ~289 ABL functions, ~85 STABLE v1 bindings
- [`../explorations/2026-04-30-track-c-lua-sandbox-and-errors.md`](../explorations/2026-04-30-track-c-lua-sandbox-and-errors.md) — stdlib whitelist, file I/O sandbox, 20-case test battery, resource caps
- [`../explorations/2026-04-30-track-c-lua-loading-lifecycle.md`](../explorations/2026-04-30-track-c-lua-loading-lifecycle.md) — Kahn's BFS load order, stage mechanics, save/load
- [`../explorations/2026-04-30-track-c-lua-trampolines-and-tests.md`](../explorations/2026-04-30-track-c-lua-trampolines-and-tests.md) — corrected ABL ABI, worked binding examples, perf budget
- [`../explorations/2026-04-30-track-c-blocking-questions-resolution.md`](../explorations/2026-04-30-track-c-blocking-questions-resolution.md) — reentrancy, namespace lock, magicpatrol shadow rule

**Collaborator-owned design artifacts (Methuselas):**
- *MC2R ImGui + FIT UI Integration Design (v0.1)* — summarized in [`memory/imgui_fit_ui_design.md`](../../../memory/imgui_fit_ui_design.md)
- *MC2R Editor Changelog* — Editor SemVer history through `0.14.0-editor` (see `Editor/EditorVersion.h` for current)

**External references:**
- OpenRA mod structure: <https://github.com/OpenRA/OpenRA/tree/bleed/mods>
- Factorio modding tutorial: <https://wiki.factorio.com/Tutorial:Modding_tutorial>
- Fabric mod manifest spec: <https://fabricmc.net/wiki/documentation:fabric_mod_json>
- Spring/BAR modding: <https://github.com/beyond-all-reason/Beyond-All-Reason>
- 0 A.D. modding: <https://trac.wildfiregames.com/wiki/Modding_Guide>

**Memory entries:**
- [`memory/stock_install_must_remain_playable.md`](../../../memory/stock_install_must_remain_playable.md) — sidecar architectural rule
- [`memory/cull_gates_are_load_bearing.md`](../../../memory/cull_gates_are_load_bearing.md) — why no GameObject ECS migration
- [`memory/public_fork_and_release.md`](../../../memory/public_fork_and_release.md) — existing release/fork conventions
- [`memory/mco_mech_csv_format.md`](../../../memory/mco_mech_csv_format.md) — existing loose-CSV mech precedent
- [`memory/modders_paradise_roadmap.md`](../../../memory/modders_paradise_roadmap.md) — short-form hook for this spec
- [`memory/imgui_fit_ui_design.md`](../../../memory/imgui_fit_ui_design.md) — Track B design summary + editor context
