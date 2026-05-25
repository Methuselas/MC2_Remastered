# Track C — Modder Tooling Deep-Dive (Editor Experience)

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- [`2026-04-30-track-c-lua-api-surface-catalog.md`](2026-04-30-track-c-lua-api-surface-catalog.md) — `BindingSpec` registry, doc-gen pattern (§9)
- [`2026-04-30-track-c-lua-trampolines-and-tests.md`](2026-04-30-track-c-lua-trampolines-and-tests.md) — `BindingRegistry` + `MC2_LUA_REG` (§7)
- [`2026-04-30-track-c-blocking-questions-resolution.md`](2026-04-30-track-c-blocking-questions-resolution.md) — locked namespace tree (Q2)

This doc fixes the **modder editor experience**: what a modder sees the moment they open `mods/<modid>/scripts/control.lua` in VS Code (or Neovim, Sublime, JetBrains — anything with an LSP client). The deliverable below is concrete enough that the next session can build `tools/lua_api_doc_gen/` with no design questions left.

---

## 1. LuaLS integration — `.luarc.json`

[LuaLS](https://luals.github.io) (`lua-language-server`) is the de-facto Lua LSP. It reads a per-workspace `.luarc.json` to discover type stubs, library paths, runtime version, and globals. Every mod ships one.

### Per-mod `.luarc.json`

```json
{
  "$schema": "https://raw.githubusercontent.com/LuaLS/vscode-lua/master/setting/schema.json",
  "runtime.version": "Lua 5.4",
  "runtime.path": ["?.lua", "?/init.lua"],
  "workspace.library": [
    "${3rd}/mc2/library",
    "../../meta",
    "meta"
  ],
  "workspace.checkThirdParty": false,
  "diagnostics.globals": ["mc2"],
  "diagnostics.disable": ["lowercase-global"],
  "completion.callSnippet": "Replace",
  "completion.keywordSnippet": "Replace",
  "hint.enable": true,
  "hint.paramName": "All",
  "hint.paramType": true,
  "telemetry.enable": false
}
```

### Where `meta/` lives

Three legitimate locations, resolved in this priority order:

1. **Engine-distributed stubs (`<mc2-install>/tools/lua-meta/`).** Authoritative source of truth — regenerated from `BindingRegistry` every release. Ships in the install tree. Modders point at it via the `../../meta` relative path *if* their mod folder lives under `<mc2-install>/mods/<id>/`. The relative form keeps the workspace portable; nothing in the mod refers to an absolute install location.
2. **Per-mod copy (`mods/<id>/meta/mc2-api.lua`).** The `tools/new-mod` template (§6) drops a copy here so a mod folder is self-contained when zipped, shared on Discord, or version-controlled in isolation. Mod CI can lint against this copy.
3. **Cross-mod published stubs (`mods/<id>/meta/api.lua` for that mod's *own* exports).** When mod A wants other mods to autocomplete its public Lua surface, it ships a stub for itself. Workspace `library` lists every `mods/*/meta/` so all loaded mods' stubs resolve. (See §9.)

The `${3rd}` syntax is LuaLS's convention for shipped third-party libraries; we register MC2 as a `${3rd}/mc2` package via a small `config.json` next to the engine stub. That gives modders a "type the keyword `mc2` in any Lua project and LuaLS prompts to enable the MC2 library" experience identical to how Love2D or OpenResty users see their libraries.

---

## 2. Type stub format — `meta/mc2-api.lua`

The engine emits this file from `BindingRegistry`. Every entry uses LuaLS annotations. The first line is `---@meta` so LuaLS treats the file as definition-only (no runtime semantics, no double-definition warnings if the modder also requires it).

### Header

```lua
---@meta
--
-- mc2-api.lua — auto-generated from BindingRegistry. DO NOT EDIT.
-- Regenerate: ./tools/lua_api_doc_gen --emit=luals > tools/lua-meta/library/mc2-api.lua
-- mc2_api_version: 1
-- Generated: 2026-04-30T00:00:00Z
--

---@class mc2
mc2 = {}

---@class mc2.log_module
mc2.log = {}
---@class mc2.object
mc2.object = {}
---@class mc2.objective
mc2.objective = {}
---@class mc2.timer
mc2.timer = {}
---@class mc2.mission
mc2.mission = {}
---@class mc2.audio
mc2.audio = {}
---@class mc2.experimental
mc2.experimental = {}
-- ... one per locked subsystem in Q2 namespace tree ...
```

### Aliases for ID enums

```lua
---@alias mc2.ObjectId integer Opaque object handle.
---@alias mc2.SideIndex integer 0..7. 0=player.
---@alias mc2.AreaId integer Trigger-zone identifier returned by mc2.area.add.
---@alias mc2.SoundId integer
---@alias mc2.PrototypeId integer Object-type id from CSV.

---@enum mc2.ObjectStatus
mc2.ObjectStatus = {
    NORMAL    = 0,
    DESTROYED = 1,
    DISABLED  = 2,
}

---@enum mc2.ObjectiveStatus
mc2.ObjectiveStatus = {
    INACTIVE  = 0,
    ACTIVE    = 1,
    SUCCEEDED = 2,
    FAILED    = 3,
}

---@class mc2.Vec3
---@field x number East-axis world coord.
---@field y number Elevation.
---@field z number North-axis world coord.
```

### Representative bindings (full annotated forms)

```lua
--- Emit a line to the engine log channel.
--- @param msg string Body of the log line. Prefixed with `[LUA INFO]` automatically.
--- @return nil
function mc2.log(msg) end

--- Mission seconds since start.
--- @return number  # Seconds. Floating-point.
--- @nodiscard
function mc2.time.now() end

--- True if the engine still tracks an object with this id.
--- @param id mc2.ObjectId
--- @return boolean
--- @nodiscard
function mc2.object.exists(id) end

--- Status enum for an object.
--- @param id mc2.ObjectId
--- @return mc2.ObjectStatus
--- @nodiscard
function mc2.object.status(id) end

--- Apply damage to an object as if from a weapon shot.
--- See ABL counterpart `damageobject`. Returns -3 on out-of-range damage,
--- -1 on bad target id, -2 on bad attacker id, >0 on hit count.
--- @param target_id   mc2.ObjectId Valid object handle.
--- @param attacker_id mc2.ObjectId Damage source.
--- @param weapon_id   integer       Master component id of the weapon.
--- @param amount      number        Damage points, clamped to [0, 100000].
--- @param hit_loc     integer       Body-location enum.
--- @param hit_roll    number        Pre-rolled hit value, [0, 1].
--- @param entry_angle number        Radians.
--- @return integer result `1`=hit, `-1`=bad target, `-2`=bad attacker, `-3`=damage out of range.
function mc2.object.apply_damage(target_id, attacker_id, weapon_id, amount,
                                  hit_loc, hit_roll, entry_angle) end

--- Spawn an object from a prototype id.
--- @param prototype_id mc2.PrototypeId
--- @param pos          mc2.Vec3
--- @param side         mc2.SideIndex
--- @return mc2.ObjectId  # New object id, or `0` on failure.
function mc2.object.spawn(prototype_id, pos, side) end

--- Set objective status.
--- @param id     integer Objective slot 0..N.
--- @param status mc2.ObjectiveStatus
function mc2.objective.set_status(id, status) end

--- Schedule a one-shot Lua callback.
--- @param seconds number Delay in mission-seconds, [0, 36000].
--- @param cb fun(timer_id:integer):any  Invoked once at expiry.
--- @return integer timer_id Used to cancel the timer; `0` on validation failure.
function mc2.timer.create(seconds, cb) end

--- Play a sound effect by id.
--- @param sound_id mc2.SoundId
function mc2.audio.play_sound(sound_id) end

--- Play a video by short name.
--- @param name string Filename minus extension.
--- @overload fun(name: string, on_end: fun()): nil  # Optional end callback (M1).
function mc2.video.play(name, on_end) end

--- Hook an engine event. Control stage only.
--- @param name "mission_start"|"mission_end"|"warrior_tick"|"object_destroyed"|"objective_completed"
--- @param cb fun(payload:table):any
function mc2.on_event(name, cb) end

--- Register an ActionRegistry handler (FIT-button dispatch).
--- @param key string  PascalCase.PascalCase per FIT convention, e.g. "MyMod.OpenShop".
--- @param fn  fun(arg: any?): any
function mc2.register_action(key, fn) end

--- @deprecated Use mc2.log instead.
--- @param ... any
function mc2.print(...) end
```

### Edge-case patterns

- **Optional / defaulted params:** `@param name string?` (the `?` marks nilable). LuaLS shows the param in lighter colour and skips the missing-argument diagnostic.
- **Vararg:** `@param ... any` plus `function f(...)`. With LuaLS 3.7+ prefer typed vararg: `@param ... string`.
- **Callback type:** inline function type literal `fun(payload:table):any` — readable, no separate `@alias` needed unless reused.
- **Multiple returns:** `@return integer id`, `@return string name` on consecutive lines.
- **`@overload`:** for the second video.play signature with an end-callback. Renders as a separate completion entry.
- **`@nodiscard`:** read-only queries (`mc2.time.now`, `mc2.object.exists`) get this so LuaLS warns when the modder forgets the assignment (`mc2.object.exists(id)` with no surrounding `if`).
- **Stage gating:** the experimental tier is its own table; the deprecated marker decorates individual entries. Stage-gating (data vs control) is *runtime* enforcement; we mention it in the doc-comment but do not encode it as a static type.

---

## 3. Generation pipeline

```
                   [build-time, RelWithDebInfo]
                           │
   BindingRegistry::Instance().DumpJson() ──► lua-api-registry.json
                           │
                           ▼
              tools/lua_api_doc_gen (Python, ~300 LoC)
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
   tools/lua-meta/library/         docs/lua-api.md
       mc2-api.lua                 (modder reference,
   (LuaLS stubs, §2)                rendered from the same JSON)
```

### Engine side

`BindingRegistry` (trampolines doc §7) gains a sibling `DumpJson` method beside `DumpMarkdown`. Schema:

```json
{
  "mc2_api_version": 1,
  "generated_at": "2026-04-30T00:00:00Z",
  "bindings": [
    {
      "lua_path": "mc2.object.apply_damage",
      "tier": "STABLE",
      "stage": "control",
      "since": 1,
      "deprecated": false,
      "doc": "Apply damage to an object as if from a weapon shot...",
      "params": [
        {"name": "target_id",   "type": "mc2.ObjectId", "doc": "Valid object handle."},
        {"name": "amount",      "type": "number",       "doc": "Damage points, clamped to [0,100000]."}
      ],
      "returns": {"type": "integer", "doc": "1=hit, -1=bad target, ..."}
    }
  ],
  "enums": [
    {"name": "mc2.ObjectStatus", "values": [{"name":"NORMAL","value":0}, ...]}
  ],
  "aliases": [
    {"name": "mc2.ObjectId", "type": "integer", "doc": "Opaque object handle."}
  ],
  "events": [
    {"name": "mission_start", "payload": {"reason":"string"}}
  ]
}
```

The engine emits this file in two ways:
1. **At build time** — a `lua_api_registry_dump` post-build step runs the engine in a `--dump-lua-api lua-api-registry.json --exit` mode. CMake target depends on the engine binary; output is committed.
2. **CLI command** — `mc2.exe --dump-lua-api <path>` for manual regeneration without a full build.

### Tool side — `tools/lua_api_doc_gen/`

A small Python tool (Python is acceptable here; the engine doesn't depend on it, just the dev workflow does):

```
tools/lua_api_doc_gen/
├── __main__.py          # CLI: --emit=luals|markdown|both, --in, --out
├── emit_luals.py        # JSON → mc2-api.lua
├── emit_markdown.py     # JSON → lua-api.md
├── templates/
│   ├── luals_header.lua
│   ├── luals_binding.lua.j2     # Jinja2
│   └── markdown_binding.md.j2
└── tests/
    └── test_emit_luals.py        # Golden-file tests against a fixture JSON
```

`emit_luals.py` traverses the JSON, emits per-namespace `@class` tables, then per-binding annotated function declarations in stable sort order (alphabetical by `lua_path`). Stable order = clean diffs.

C++ over Python for the tool was considered. Python wins because: (a) the engine already requires Python for `run_smoke.py`, `pack_mat_normal.py`, `upscale_*.py`; (b) the tool is template-substitution + JSON walking, not performance-critical; (c) modder-facing tooling is easier to inspect and patch in Python.

### CI guard

```bash
# scripts/check-lua-api-stubs.sh
mc2.exe --dump-lua-api /tmp/lua-api-registry.json --exit
python tools/lua_api_doc_gen --emit=both \
    --in /tmp/lua-api-registry.json \
    --luals-out tools/lua-meta/library/mc2-api.lua \
    --md-out docs/lua-api.md
git diff --exit-code tools/lua-meta/library/ docs/lua-api.md
```

Pre-commit hook for any commit touching `code/ablmc2.cpp`, `modding/lua_bindings_*.cpp`, or anything emitting `MC2_LUA_REG`. Same shape as `scripts/check-claude-md-pointer.sh` and `scripts/check-asset-scale-callers.sh` (worktree CLAUDE.md "Tier-1 Instrumentation" section).

---

## 4. VS Code workspace settings — `.vscode/settings.json`

Drops in via `tools/new-mod` (§6):

```json
{
  "Lua.runtime.version": "Lua 5.4",
  "Lua.workspace.library": ["meta"],
  "Lua.diagnostics.globals": ["mc2"],
  "Lua.completion.callSnippet": "Replace",
  "Lua.hint.enable": true,
  "Lua.hint.paramType": true,
  "Lua.format.enable": true,
  "Lua.telemetry.enable": false,
  "files.associations": {
    "mod.json": "jsonc"
  },
  "editor.formatOnSave": true,
  "editor.tabSize": 2,
  "editor.insertSpaces": true
}
```

And `.vscode/extensions.json`:

```json
{
  "recommendations": [
    "sumneko.lua",
    "ms-azuretools.vscode-json"
  ]
}
```

VS Code prompts the modder to install LuaLS (`sumneko.lua`) when the workspace opens. Two clicks and they're in.

---

## 5. Snippets — `.vscode/mc2-mod.code-snippets`

```json
{
  "MC2 Mod Skeleton (data + control)": {
    "scope": "lua",
    "prefix": "mc2-mod-skeleton",
    "body": [
      "-- ${1:control.lua} for mod ${2:my_mod}",
      "mc2.log('${2:my_mod} loaded')",
      "",
      "mc2.on_event('mission_start', function(payload)",
      "    mc2.log('mission_start side=' .. tostring(payload.side))",
      "    $0",
      "end)"
    ]
  },
  "MC2 Prototype — Mech": {
    "prefix": "mc2-prototype-mech",
    "body": [
      "mc2.data.mech.${1:my_mech} = {",
      "    name        = '${2:My Mech}',",
      "    tonnage     = ${3:50},",
      "    max_armor   = ${4:200},",
      "    base_chassis = '${5:bushwacker}',",
      "    $0",
      "}"
    ]
  },
  "MC2 Event Handler": {
    "prefix": "mc2-event-handler",
    "body": [
      "mc2.on_event('${1|mission_start,mission_end,warrior_tick,object_destroyed,objective_completed|}', function(payload)",
      "    $0",
      "end)"
    ]
  },
  "MC2 Timer (one-shot)": {
    "prefix": "mc2-timer",
    "body": [
      "mc2.timer.create(${1:5.0}, function(timer_id)",
      "    $0",
      "end)"
    ]
  },
  "MC2 Mission Script Skeleton": {
    "prefix": "mc2-mission-script",
    "body": [
      "-- ${1:mission_name}.lua — control-stage mission script",
      "local M = {}",
      "",
      "function M.on_start()",
      "    mc2.objective.set_status(0, mc2.ObjectiveStatus.ACTIVE)",
      "    $0",
      "end",
      "",
      "function M.on_tick(dt)",
      "end",
      "",
      "mc2.on_event('mission_start', M.on_start)",
      "return M"
    ]
  },
  "MC2 Spawn Mech": {
    "prefix": "mc2-spawn-mech",
    "body": [
      "local id = mc2.object.spawn(${1:prototype_id}, { x=${2:0}, y=${3:0}, z=${4:0} }, ${5:0})",
      "if id == 0 then mc2.log('spawn failed') end",
      "$0"
    ]
  },
  "MC2 Apply Damage": {
    "prefix": "mc2-apply-damage",
    "body": [
      "mc2.object.apply_damage(${1:target_id}, ${2:attacker_id}, ${3:weapon_id}, ${4:25.0}, ${5:0}, ${6:0.5}, ${7:0.0})"
    ]
  },
  "MC2 Trigger Area": {
    "prefix": "mc2-trigger-area",
    "body": [
      "local area = mc2.area.add(${1:x0}, ${2:y0}, ${3:x1}, ${4:y1}, ${5:0}, ${6:0})",
      "mc2.on_event('warrior_tick', function(payload)",
      "    if mc2.area.is_hit(area) then",
      "        $0",
      "        mc2.area.reset(area)",
      "    end",
      "end)"
    ]
  },
  "MC2 Register Action (FIT button)": {
    "prefix": "mc2-register-action",
    "body": [
      "mc2.register_action('${1:MyMod.${2:OpenShop}}', function(arg)",
      "    $0",
      "end)"
    ]
  },
  "MC2 Audio Cue": {
    "prefix": "mc2-audio-cue",
    "body": [
      "mc2.audio.play_sound(${1:sound_id})",
      "mc2.audio.play_speech(${2:line_id}, ${3:speaker_id})"
    ]
  }
}
```

---

## 6. Mod template generator — `tools/new-mod`

A small script (Python — same rationale as §3) that scaffolds a mod folder.

```
tools/new-mod
├── __main__.py
└── template/
    ├── mod.json.j2
    ├── scripts/
    │   ├── data.lua.j2
    │   └── control.lua.j2
    ├── meta/
    │   └── README.txt          # "regenerate via tools/lua_api_doc_gen"
    ├── .luarc.json.j2
    ├── .vscode/
    │   ├── settings.json
    │   ├── extensions.json
    │   └── mc2-mod.code-snippets
    └── .gitignore
```

Invocation:

```bash
$ python tools/new-mod --id my_first_mod --name "My First Mod" --author "Modder Name"
✓ Created mods/my_first_mod/
✓ Symlinked mods/my_first_mod/meta -> ../../tools/lua-meta/library  (Linux/macOS)
  -or-
✓ Copied  mods/my_first_mod/meta/mc2-api.lua  (Windows; symlink fallback)
✓ Wrote   mods/my_first_mod/.luarc.json
✓ Wrote   mods/my_first_mod/.vscode/{settings.json,extensions.json,mc2-mod.code-snippets}
✓ Wrote   mods/my_first_mod/scripts/{data.lua,control.lua}
✓ Wrote   mods/my_first_mod/mod.json (mc2_api_version=1)

Open in VS Code:
    code mods/my_first_mod
```

`mod.json` defaults:

```jsonc
{
  "id": "my_first_mod",
  "name": "My First Mod",
  "author": "Modder Name",
  "version": "0.1.0",
  "mc2_api_version": 1,
  "dependencies": [],
  "scripts": {
    "data":    "scripts/data.lua",
    "control": "scripts/control.lua"
  }
}
```

`scripts/control.lua` ships with one working event handler (so the modder runs the mission and immediately sees `[LUA INFO] my_first_mod loaded` in the log — feedback loop closed in <60s):

```lua
mc2.log('my_first_mod loaded')

mc2.on_event('mission_start', function(payload)
    mc2.log('mission_start fired')
end)
```

Symlink-vs-copy: on Linux/macOS, `meta/` is a symlink to the engine stubs so a regeneration update is picked up automatically. On Windows (default modder OS) symlinks require admin — fall back to a copy and print a one-line note: "regenerate `meta/mc2-api.lua` after updating MC2 by re-running `tools/new-mod --refresh-meta`".

---

## 7. Editor experience walkthrough

The modder opens `mods/my_first_mod/scripts/control.lua` in VS Code (LuaLS already running):

1. **Type `mc2.`** → completion popup lists every subnamespace and top-level binding: `log`, `time`, `object`, `objective`, `timer`, `area`, `global`, `mission`, `audio`, `video`, `camera`, `team`, `economy`, `strike`, `ui`, `tutorial`, `gate`, `experimental`, `data`, `on_event`, `register_action`. Sorted alphabetically. Each carries an icon (table for namespaces, function for `on_event`).

2. **Type `mc2.obj`** → narrows to `object`, `objective`. Pressing Tab/Enter accepts `object` and a `.` is auto-inserted (call-snippet config).

3. **Type `mc2.object.apply_damage(`** → signature-help popup:
   ```
   mc2.object.apply_damage(
     target_id: mc2.ObjectId,
     attacker_id: mc2.ObjectId,
     weapon_id: integer,
     amount: number,
     hit_loc: integer,
     hit_roll: number,
     entry_angle: number
   ) → integer
   ```
   Active argument bolded as the modder types past each comma. Hovering the popup shows the docstring ("Apply damage to an object as if from a weapon shot...") plus the return-value reference table.

4. **Type `mc2.object.apply_damage("not_an_int", 5, 1, 25.0, 0, 0.5, 0.0)`** → red squiggle under `"not_an_int"` with hover text:
   ```
   Cannot assign `string` to parameter `target_id: mc2.ObjectId` (alias of integer).
   ```
   The diagnostic appears as the modder types — no save needed.

5. **Hover over `mc2.timer.create`** → tooltip with full signature, parameter descriptions, return type, and the rendered docstring. Includes a "Go to definition" link that opens `meta/mc2-api.lua` at the function declaration.

6. **`Ctrl+click` on `mc2.object.spawn`** → jumps to `meta/mc2-api.lua` line for `function mc2.object.spawn ... end`. From there a comment reference like `-- See docs/lua-api.md#mc2-object-spawn` opens the markdown reference in a preview tab.

7. **Type `mc2.objective.set_status(0, "active")`** → red squiggle under `"active"`:
   ```
   Cannot assign `string` to parameter `status: mc2.ObjectiveStatus`.
   Suggested values: mc2.ObjectiveStatus.INACTIVE, .ACTIVE, .SUCCEEDED, .FAILED.
   ```
   The enum suggestions come for free from `@enum`.

8. **Type `mc2.experimental.`** → completion lists all experimental bindings, but each has a yellow `experimental` badge in the popup and the docstring leads with `-- ⚠ EXPERIMENTAL — may change in future api versions.`

9. **Type `mc2.print("hello")`** → strikethrough on `mc2.print` with hover text "Deprecated. Use mc2.log instead." The `@deprecated` annotation drives both the visual and a `deprecated` diagnostic.

10. **Type `local t = mc2.object.exists(123)` then forget the `local t =`** → grey hint diagnostic on `mc2.object.exists(123)`: "Return value of `mc2.object.exists` is discarded." Driven by `@nodiscard`.

11. **Press `mc2-mod-skeleton` and Tab** → snippet expands to the full event-handler boilerplate, cursor lands inside the `function(payload) ... end` body.

12. **Open `data.lua`, type `mc2.spawn_mech(...)`** → diagnostic: "Field `spawn_mech` does not exist on type `mc2`." (Because `mc2.spawn` lives under `mc2.object.spawn` — the catalog locked the namespace; the stub only exposes the canonical name.)

This is the experience that turns "writing a mod" from a 2003-style trial-and-error grep through `.abx` files into a 2026-style typed-API authoring session.

---

## 8. Static documentation site

Two options:

- **Plain `docs/lua-api.md` in-repo.** Free, zero infrastructure, browseable on GitHub. Good enough for v1.
- **mdBook / Hugo / VitePress rendered to `lua-api.mc2-remastered.example`.** Search, anchor links per binding, version selector for `mc2_api_version`. Costs: a small static-host pipeline (GitHub Pages would suffice — push `book/` on tag).

**Recommendation: defer the static-site build to post-1.0.** The doc-gen pipeline already emits `docs/lua-api.md`; modders can read it in any markdown viewer, and the LuaLS hover docs are the *primary* discovery surface anyway. mdBook adds polish; it does not add capability.

If a static site does land, the cheapest rig is mdBook with a custom preprocessor that consumes `lua-api-registry.json` directly (skip the markdown intermediate; lets us add per-binding "see source: `code/ablmc2.cpp:1926`" links and tier badges with no manual upkeep).

---

## 9. Cross-mod API discovery

Pattern: any mod that exposes a Lua API to other mods ships a stub for it.

**Mod A** publishes:
```
mods/mod_a/
├── meta/
│   └── api.lua        # ---@meta — declares mod_a.shop.*
└── scripts/
    └── control.lua    # registers the global mod_a table
```

`mods/mod_a/meta/api.lua`:
```lua
---@meta
---@class mod_a.shop_module
mod_a = {}
mod_a.shop = {}

--- Open the shop UI for a given side.
--- @param side mc2.SideIndex
--- @param items table<integer, mc2.PrototypeId>
function mod_a.shop.open(side, items) end
```

**Mod B's** `.luarc.json` lists every loaded mod's `meta/`:

```json
{
  "workspace.library": [
    "meta",
    "../mod_a/meta",
    "../mod_c/meta"
  ],
  "diagnostics.globals": ["mc2", "mod_a", "mod_c"]
}
```

The `tools/new-mod` template can offer a `--depend mod_a,mod_c` flag that pre-fills these two arrays, plus the `dependencies` list in `mod.json`. It can also generate a workspace-level `<install>/mods/.luarc.json` that lists every installed mod's `meta/` for "modder editing several mods at once" workflows.

LuaLS resolves declarations from across the library list as if they were one project. Mod B sees `mod_a.shop.open(...)` autocomplete the same way it sees `mc2.object.spawn(...)`.

The convention that mods ship their own stubs is the same convention LuaJIT-ecosystem libraries (`busted`, `luassert`, `inspect`) follow. We document it; we do not enforce it. Mods without stubs work fine; they just don't get autocomplete from outside.

---

## 10. Tooling for non-VS-Code editors

LuaLS is editor-agnostic. The same `.luarc.json` + `meta/` setup drives:

- **Neovim:** `nvim-lspconfig` `lua_ls` setup. One-line config snippet shipped in `docs/modding/editors.md`.
- **Sublime Text:** `LSP-lua` package. Reads `.luarc.json` directly.
- **Emacs:** `lsp-mode` with `lsp-clients-lua-language-server-bin` pointing at LuaLS.
- **JetBrains (CLion / IntelliJ Rust / IDEA):** Sumneko Lua plugin (LSP-bridged). Picks up `.luarc.json`.
- **Helix:** `helix-editor` ships LuaLS support out-of-the-box.
- **Zed:** Lua extension (LSP-bridged).

The `.vscode/` directory is VS-Code-specific; everything else (`.luarc.json`, `meta/`, `mod.json`) is universal. `tools/new-mod` should accept `--editor=vscode|neovim|sublime|none` and skip the `.vscode/` scaffolding when the modder uses something else. (`none` is for the modder who `mod.json`-edits in vim and never wants LSP.)

---

## 11. Open questions

1. **`@async` annotations.** LuaLS supports `@async` for coroutine-yielding functions. Track C currently has no Lua coroutines (sandbox blocks `coroutine` — implementation-shape §6); however, `mc2.video.play(name, on_end)` with the optional callback could read as `@async fun(name): nil` if we eventually support `await`-style. Keep classical-callback shape for v1; revisit.

2. **Per-mod `_ENV` vs global `mc2`.** Implementation-shape proposes per-mod `sol::environment` so two mods don't share `_G`. LuaLS does not natively model "this file's `_G` is restricted." Options: emit a per-mod `meta/env.lua` enumerating the safe sandbox surface; or accept that the editor sees a slightly more permissive type than runtime allows (false negatives on diagnostics, no false positives). Defer; document.

3. **Live updates from the engine.** Some game engines ship a "watch mode" where a running engine streams the registry over a localhost socket and the LuaLS workspace picks up new bindings instantly. Way out of scope for v1; nice-to-have for hot-reload sessions later.

4. **Inline run / debug from VS Code.** Could provide a `.vscode/launch.json` that launches `mc2.exe` with the current mod set. Concrete value depends on engine-side debug-attach support (Lua debugger via DAP). Trickier than it sounds; defer.

5. **Snippet curation.** The 10 snippets in §5 are starter content. Real modder workflows will want more (mission templates, AI-brain shells, FIT-button registrations, save/load hooks). Solicit additions in M1 once two or three real mods exist.

6. **Stub output stability.** Trivial JSON ordering changes propagate to per-line stub diffs. Enforce stable sort by `lua_path` in the C++ `DumpJson` and Python emitters. CI guard `git diff --exit-code` is sensitive to this; document.

7. **`mc2_api_version` selector in stubs.** A modder targeting `mc2_api_version=1` while the engine is on `=2` should get type-checked against v1, not v2. Solution: the engine ships *all* historical stubs (`tools/lua-meta/library/v1/`, `v2/`, ...), and `.luarc.json` points at the version matching `mod.json`'s declared version. `tools/new-mod` can write the right path. M2 concern.

8. **Discoverability of `tools/new-mod` itself.** The first-time modder needs to find this script. Options: (a) a shortcut in the install (`Create New Mod.bat` on Windows wrapping the script); (b) a top-level `mc2.exe --new-mod` mode; (c) a CONTRIBUTING / Modders.md README in the install root. (b) is cleanest; ship in M1.

---

## 12. References

Predecessor docs:
- `docs/superpowers/explorations/2026-04-30-track-c-lua-api-surface-catalog.md` §9 (BindingSpec / `MC2_BIND`).
- `docs/superpowers/explorations/2026-04-30-track-c-lua-trampolines-and-tests.md` §7 (`BindingRegistry`, `MC2_LUA_REG`, doc-gen path).
- `docs/superpowers/explorations/2026-04-30-track-c-blocking-questions-resolution.md` Q2 (locked namespace tree).
- `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` §5.4 stable mod ABI, §5.5 ActionRegistry.

External (verified during this session):
- LuaLS annotations reference: <https://luals.github.io/wiki/annotations/> — `@class`, `@field`, `@param`, `@return`, `@overload`, `@generic`, `@alias`, `@enum`, `@type`, `@nodiscard`, `@deprecated`, `@vararg`, `@meta`, `@diagnostic`, `@see`.
- LuaLS configuration reference: <https://luals.github.io/wiki/configuration/> (workspace library, diagnostics.globals, runtime.version).
- Factorio's `runtime-api.json` + Lua API doc-gen pipeline (`https://github.com/wube/factorio-data`) — closest precedent: JSON registry → typed Lua stubs → static docs site.

Worktree memory:
- `memory/debug_instrumentation_rule.md` — env-gated trace pattern (mirrored by the doc-gen CI guard pattern).
- `memory/stock_install_must_remain_playable.md` — `tools/new-mod` must not require modifying stock files; mods are pure overlays.
- `memory/feedback_deploy_path.md` — the install tree (`mc2-win64-v0.2/`) is where `tools/lua-meta/library/` lives at runtime.
