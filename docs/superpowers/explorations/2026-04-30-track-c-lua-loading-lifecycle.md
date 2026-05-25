# Track C — Mod Loading + Lifecycle + Data/Control Split Mechanics

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- `2026-04-29-track-c-lua-scripting-status.md` (status snapshot)
- `2026-04-30-track-c-lua-implementation-shape.md` (VM class, lifecycle wiring, sandbox)
- `specs/2026-04-29-modders-paradise-roadmap-design.md` §6, §8.2, §10
**Predecessor decision (impl-shape §7):** single `sol::state` with stage-gated API views — NOT two VMs.

This doc fills in everything the impl-shape doc deferred: load order, prototype freeze mechanics, save/load behavior of Lua state, hot-reload semantics, error handling, and the interface to the collaborator's `mod-profile-launcher`.

---

## 1. Load order — dependency topological sort

### Manifest shape (per `mods/<modid>/mod.json`, roadmap §5.2)

```json
{
  "id":      "magic_corebrain",
  "version": "1.2.0",
  "depends": {
    "core":          ">=1.0.0",
    "common_assets": ">=2.1.0 <3.0.0"
  },
  "scripts": { "data": "scripts/data.lua", "control": "scripts/control.lua" }
}
```

`depends` is a `{modid: version_constraint}` map. Constraint syntax mirrors Factorio: `>=`, `<=`, `=`, `<`, `>`, plus the conjunction form `">=1.0.0 <2.0.0"`. Optional dependencies use a `?` prefix on the modid: `"?optional_mod": ">=0.5.0"`.

### Algorithm (Kahn's BFS variant — stable order)

```text
function resolveLoadOrder(modDir, profileFilter):
    # 1. Discover.
    manifests = {}
    for entry in scandir(modDir):
        m = parseJson(entry/"mod.json")
        if profileFilter && !profileFilter.contains(m.id): continue   # §7
        manifests[m.id] = m

    # 2. Validate dependency presence + version.
    errors = []
    for id, m in manifests:
        for depId, constraint in m.depends:
            optional = depId.startsWith("?")
            realId   = optional ? depId[1:] : depId
            if realId not in manifests:
                if optional: continue
                errors.append({mod: id, kind: "missing_dep", dep: realId})
                continue
            if !semverSatisfies(manifests[realId].version, constraint):
                errors.append({mod: id, kind: "version_mismatch",
                               dep: realId, have: manifests[realId].version,
                               want: constraint})
    if errors.nonempty(): return Err(errors)

    # 3. Build directed edges  dep -> mod  (dep loads BEFORE mod).
    indeg = {id: 0 for id in manifests}
    adj   = {id: []  for id in manifests}
    for id, m in manifests:
        for depId, _ in m.depends:
            realId = strip("?", depId)
            if realId in manifests:
                adj[realId].append(id)
                indeg[id] += 1

    # 4. Kahn — pull stable order by sorting the ready set by id.
    ready = sortedByModId([id for id, d in indeg if d == 0])
    order = []
    while ready.nonempty():
        n = ready.popFront()
        order.append(n)
        for next in adj[n]:
            indeg[next] -= 1
            if indeg[next] == 0:
                ready.insertSorted(next)

    # 5. Cycle detection.
    if len(order) != len(manifests):
        cycle = findCycle(adj, indeg)              # DFS color-walk on remaining nodes
        return Err([{kind: "cycle", path: cycle}])

    return Ok(order)
```

`semverSatisfies` is a tiny in-tree helper (no semver library — the constraint grammar above is small enough to parse by hand; nlohmann/json from the mod-profile-launcher already vendored).

`sortedByModId` makes the load order **deterministic** even when multiple mods sit at indegree zero — required for save/load and for multiplayer (whenever §10.2 lands).

`findCycle` returns the actual `[a -> b -> c -> a]` path so the user-facing error is actionable, not just "cycle exists."

### Error surface

All four error kinds (`missing_dep`, `version_mismatch`, `cycle`, plus `parse_error` from earlier in step 1) bubble up as **mod-fatal**, not engine-fatal. The dependent-cascade rule in §8 picks up the slack.

---

## 2. `data.lua` / `control.lua` stage mechanics

The single-VM-with-stage-gates approach (impl-shape §7) is implemented as follows.

### Stage state

```cpp
// In LuaVM::Impl
enum class Stage { None, Data, DataFrozen, Control, Mission };
Stage current_stage_ = Stage::None;
```

State transitions are linear and one-way per VM lifetime: `None → Data → DataFrozen → Control → Mission → (Shutdown)`.

### Per-binding gate (defense-in-depth)

Every binding lambda starts with the same one-liner:

```cpp
mc2["spawn_object"] = [](int typeId, double x, double y, int side) -> int {
    if (g_LuaVM->current_stage_ < Stage::Control)
        return luaL_error(L, "mc2.spawn_object: engine state not available in data stage");
    // ...real impl
};
```

Three buckets (already mentioned in impl-shape §7 as `"data" | "control" | "any"`):

- **data** — `mc2.prototypes.register(...)`, `mc2.log`. Allowed in `Data`.
- **control** — `mc2.spawn_object`, `mc2.damage_object`, `mc2.on_event`, `mc2.set_objective_status`, `mc2.set_timer`. Allowed in `Control`+`Mission`.
- **any** — `mc2.log`, `mc2.get_time` (read-only), `string.*`, `math.*`. Always allowed.

### Load loop

```cpp
void initLuaVM() {
    g_LuaVM = new LuaVM();
    g_LuaVM->Init();
    auto orderResult = resolveLoadOrder("mods/", profileFilter());
    if (orderResult.isErr()) { logErrors(...); return; }

    g_LuaVM->current_stage_ = Stage::Data;
    for (modId in orderResult.value) {
        bool ok = g_LuaVM->LoadDataStage(manifests[modId]);
        if (!ok) markSkipped(modId);                 // §8 cascade
    }
    freezePrototypes();                              // §3
    g_LuaVM->current_stage_ = Stage::DataFrozen;

    g_LuaVM->current_stage_ = Stage::Control;
    for (modId in orderResult.value) {
        if (skipped(modId)) continue;
        g_LuaVM->LoadControlStage(manifests[modId]);
    }
    g_LuaVM->current_stage_ = Stage::Mission;
}
```

`Stage::DataFrozen` is briefly distinct from `Stage::Control` so the freeze step is observable in instrumentation.

---

## 3. Prototype tables

### Shape

```lua
mc2.prototypes = {
    mech = {
        madcat = { tonnage=75, hp=10000, weapons={"ppc","lrm20"}, ... },
        atlas  = { tonnage=100, ... },
    },
    weapon = {
        ppc        = { damage=10, heat=15, range=1800, ... },
        lrm20      = { damage=20, heat=18, range=2400, ... },
    },
    pilot      = { ... },
    objective  = { ... },
    sound      = { ... },
}
```

Top-level keys are entity *kinds* (closed set, defined by the engine — adding a new kind is an engine change, not a mod change). Inner keys are mod-supplied unique IDs. Values are arbitrary Lua tables; engine consumers (`mc2_loadFromPrototypes("mech", "madcat")`) read fields by name.

### Registration API (data stage only)

```lua
-- mods/megamod/scripts/data.lua
mc2.prototypes.register("mech", "madcat", {
    tonnage = 75, hp = 10000,
    weapons = { "ppc", "lrm20" },
})
```

`register` is the *only* legal write path. Direct `mc2.prototypes.mech.madcat = {...}` is rejected by a sentinel `__newindex` on the kind tables — assigning by indexing skips ID-collision checks.

`register` enforces:
- `kind` is one of the closed set (else error)
- `id` is non-empty, ASCII, no slashes
- ID collision policy: first-write-wins by load order; second writer logs `[LUA v1] event=proto_collision kind=mech id=madcat owner=megamod prev_owner=core` and returns false. (Override policy comes later — needs roadmap §10 conflict resolution decision.)

### Freeze

After all `data.lua` chunks complete:

```cpp
// In freezePrototypes(), C++-side via sol2:
sol::table P = state["mc2"]["prototypes"];
for (auto& [k,v] : P) {
    if (v.is<sol::table>()) {
        sol::table kindTbl = v;
        sol::table mt = state.create_table();
        mt["__newindex"] = [](sol::object, sol::object, sol::object) {
            return luaL_error(L, "mc2.prototypes is frozen after data stage");
        };
        mt["__metatable"] = "locked";    // hide MT from getmetatable
        kindTbl[sol::metatable_key] = mt;
    }
}
```

`control.lua` reads `mc2.prototypes.mech.madcat.tonnage` freely — only writes raise. The `__metatable = "locked"` line prevents a clever mod from `setmetatable(mc2.prototypes.mech, nil)` to unfreeze.

---

## 4. Save / load interaction

`saveload.cpp:704` re-runs `initABL()` on save load. Lua mirrors per impl-shape §11.4: re-run `data.lua` at the same site (prototypes are deterministic by design, so re-running them produces an identical frozen table set).

**`control.lua` state is the harder question.** Lua-side mod-introduced variables (counters, flags, pending timer IDs, AI sub-states) do not survive a re-run of `control.lua`. Solution: a single mod-declared persistence table.

### `mc2.persist`

Each mod gets `mc2.persist[mod_id]` — a Lua table the mod owns:

```lua
-- in control.lua
mc2.persist.megamod = mc2.persist.megamod or { score=0, kills=0, last_event_t=0 }

mc2.on_event("mech_destroyed", function(id)
    mc2.persist.megamod.kills = mc2.persist.megamod.kills + 1
end)
```

Save path:
1. Engine reaches existing save site.
2. Lua serializer walks `mc2.persist`, emits a deterministic JSON blob (sort keys, no Lua functions/threads/userdata — those error and skip the field with a warning).
3. Blob lands in the savegame stream as a versioned chunk (`LUA1` + uint32 length + bytes), tucked alongside the existing ABL state blob written near `saveload.cpp:704`.

Load path:
1. After `initLuaVM()` runs `data.lua` and `control.lua`, the engine reads the `LUA1` chunk.
2. Deserializer parses JSON into a Lua table, assigns to `mc2.persist`.
3. **Backward compat:** missing `LUA1` chunk in old saves → `mc2.persist = {}`. Per-mod missing keys → mod's `or {}` initializer (line 1 above) fills in defaults. **Mods MUST tolerate empty persist on load.** Engine documents this as a hard contract; a mod that crashes on empty persist is mod-buggy.

### What does *not* survive

- Pending timers — `mc2.set_timer` IDs are runtime; mods re-arm in the `mission_start` event (which fires after load).
- Registered event handlers — re-bound by `control.lua` re-run.
- Coroutines — explicitly not supported in M0 (sandbox doesn't open `sol::lib::coroutine`).

This matches Factorio's contract and is the minimum viable surface.

---

## 5. Hot-reload semantics

Per roadmap §5.3 contract.

### `LuaVM::reloadFromDisk(const char* path)`

```cpp
bool LuaVM::reloadFromDisk(const char* path) {
    bool isDataLua    = endsWith(path, "/data.lua");
    bool isControlLua = endsWith(path, "/control.lua");

    if (isDataLua) {
        if (!s_devModeFlag) {                      // env: MC2_LUA_DEV=1
            log("[LUA v1] event=hot_reload_blocked path=%s reason=data_lua_runtime", path);
            return false;                          // policy: blocked in normal play
        }
        // Dev-only: nuke prototypes, re-run all data.lua, re-freeze.
        // NOTE: mid-mission references into prototypes (cached HP from earlier
        // mc2_loadFromPrototypes) keep the OLD values until the next load —
        // documented as a known caveat, not a bug.
        unfreezePrototypes();
        rerunAllDataStages();
        freezePrototypes();
        log("[LUA v1] event=hot_reload status=ok stage=data path=%s", path);
        return true;
    }

    if (isControlLua) {
        // SAFE path. Drop event handlers/action handlers for owning mod, re-run.
        ModEntry* mod = findOwningMod(path);
        if (!mod) return false;
        clearHandlersFor(mod->id);
        bool ok = LoadControlStage(*mod);          // re-runs file in same VM
        log("[LUA v1] event=hot_reload status=%s stage=control mod=%s",
            ok ? "ok" : "fail", mod->id.c_str());
        return ok;
    }

    // Mod-internal `require`d module: treat as control if the data stage froze.
    return reloadModule(path);
}
```

The dev gate is a single env check. Production builds set `s_devModeFlag = false` unconditionally; `MC2_LUA_DEV=1` opts in for mod authors.

The `control.lua` reload path is the "good" hot-reload: handler tables are owned by `LuaVM`, so re-running `control.lua` after clearing the prior bindings is idempotent.

---

## 6. Full lifecycle diagram (every entry/exit point)

```
ENGINE START
  └─ no Lua VM yet (sol::state is per-mission)

(time passes — main menu, logistics, mech bay)
  └─ no Lua VM (consciously: prototypes only valid mid-mission for M0)

MISSION START — three init sites (impl-shape §4):
  mission.cpp:1705       initABL();     →  mc2lua::initLuaVM();
  missionbegin.cpp:122   initABL();     →  mc2lua::initLuaVM();
  saveload.cpp:704       initABL();     →  mc2lua::initLuaVM();
                                              │
                                              ├─ scanMods("mods/", profileFilter)
                                              ├─ resolveLoadOrder()           (§1)
                                              ├─ Stage::Data
                                              │    for each mod: LoadDataStage()
                                              ├─ freezePrototypes()           (§3)
                                              ├─ Stage::Control
                                              │    for each mod: LoadControlStage()
                                              ├─ deserialize mc2.persist      (§4, only at saveload site)
                                              └─ Stage::Mission

MISSION TICK
  warrior.cpp:2160 brain execute  →  (M1+) mc2lua::g_LuaVM->CallEvent("brain_tick", ...)
  Mission::update                 →  g_LuaVM->Tick(deltaSec)   // drains timers
  ABL events / engine signals     →  g_LuaVM->CallEvent(name, args)
  ActionRegistry::Dispatch(key)   →  g_LuaVM->DispatchAction(key)  (impl-shape §8)

SAVE
  → engine reaches save chunk site near saveload.cpp save path
  → serialize mc2.persist into LUA1 chunk                          (§4)

MISSION END — two teardown sites (impl-shape §4):
  mission.cpp:3336       mc2lua::closeLuaVM(); → closeABL();   // Lua first
  missionbegin.cpp:450   mc2lua::closeLuaVM(); → closeABL();   // Lua first

ENGINE SHUTDOWN
  └─ no VM at this point (already torn down at last mission end)
```

Order rule: **Lua tears down before ABL** at every site — final pcalls during teardown can reference ABL state safely; the reverse order risks pcalling into a half-destroyed ABL heap.

---

## 7. Mod-profile-launcher integration

The collaborator's `mod-profile-launcher` worktree (`docs/plans/2026-04-25-mod-profile-launcher-plan-1-foundation.md`) introduces `--profile <id>` CLI parsing, `profile.json` with a `base` chain, and `profile_manager::bindActive()` rewriting eleven path globals before any subsystem reads them.

### Integration shape

`profile.json` already accepts a `base` chain. Add (or re-use) a `mods` field listing Lua-mod IDs:

```json
{
  "id": "campaign_megamod",
  "base": "stock",
  "mods": ["core_lib", "rebalance_pack", "megamod"],
  "campaigns": { ... },
  "art_overrides": { ... }
}
```

- Plan-1 `profile_manager` already parses unknown fields without behavior. Add `getActiveModList()` accessor returning the resolved chain (base profile's `mods` ⊕ this profile's `mods`, dedup, order preserved).
- `mc2lua::initLuaVM()` calls `profileFilter = profile_manager::getActiveModList()` (returns `optional<set<string>>`). If unset (stock boot, no `--profile`): scan everything in `mods/`. If set: scan only listed mods.
- The list is **filtering**, not reordering — topological sort still runs (§1) so a profile that lists `[megamod, core_lib]` still loads `core_lib` before `megamod` because the dep graph wins.

### Integration point (single function)

```cpp
// In modding/mod_loader.cpp:
std::vector<ModEntry> scanMods(const std::string& root) {
    auto filter = profile_manager::getActiveModList(); // optional
    std::vector<ModEntry> out;
    for (entry in scandir(root)) {
        ModEntry m = parseManifest(entry);
        if (filter && filter->find(m.id) == filter->end()) continue;
        out.push_back(m);
    }
    return out;
}
```

That single function call is the only Track C ↔ profile-launcher coupling. If profile-launcher is absent (older builds), `getActiveModList()` returns `nullopt` and the filter is a no-op — Track C still works standalone.

---

## 8. Error semantics during load

### Default policy: mod-fatal, not engine-fatal

A `data.lua` syntax error or runtime error in mod `X` skips mod `X` and continues with other mods. The legacy `.abx` mission still loads, the engine still boots. This matches the "stock install must remain playable" rule (memory: `stock_install_must_remain_playable.md`) — a broken mod cannot brick the game.

### Cascade

If mod `X` fails to load, every mod that has `X` (or any descendant) in its `depends` chain is also skipped — it would error worse trying to call APIs that didn't register. Implementation in step 4 of the load loop §2:

```text
skipped = set()
for mod in topo_order:
    if any(dep in skipped for dep in mod.depends): skipped.add(mod.id); continue
    if !LoadDataStage(mod): skipped.add(mod.id)
```

### User-facing surface

Three layers:

1. **Per-event log line** at the moment of failure:
   `[LUA v1] event=mod_load_fail mod=megamod stage=data error="scripts/data.lua:42: attempt to call nil value 'wat'"`
2. **Cascade log line**: `[LUA v1] event=mod_load_skip mod=megamod_addon reason=dep_failed dep=megamod`
3. **Summary line at mission start** (always emitted, even on full success):
   `[LUA v1] event=mods_loaded total=12 ok=10 skipped=2 reasons=[data_error:1, dep_failed:1]`

A future ImGui inspector (Track B) reads the same `[LUA v1]` event stream and surfaces it in a Modder Console pane. No new log channel needs introducing for that.

### Engine-fatal exceptions (rare)

Two cases are engine-fatal because they indicate a broken installation, not a broken mod:

- The `mods/` root is unreadable (permissions). User error, but at boot we want to crash visibly so the user knows.
- `mc2.prototypes` table itself fails to install (host-side bug in `Init()`). Indicates Sol2/Lua corruption; abort.

Everything else degrades.

---

## 9. Open questions

1. **Prototype override policy.** First-write-wins (§3) is the safe default but undermines `rebalance_pack`-style mods that want to *replace* a stock mech's stats. Three plausible answers: (a) explicit `mc2.prototypes.replace(kind, id, tbl)` API gated by a `"replaces": ["core"]` declaration in `mod.json` — Factorio model; (b) layered prototype tables with the topmost mod winning by load order — simpler but harder to debug; (c) leave first-write-wins and force explicit deletion + re-register. Resolve before the first override-needing mod ships.
2. **Reload-on-savegame-load mod set drift.** A savegame written with mods `[A,B,C]` may be loaded with mods `[A,B,D]` active. Today's plan re-runs whatever's currently configured. `mc2.persist.A` survives, `mc2.persist.C` is silently dropped, `D` starts empty. Should the engine warn/refuse on mismatch? Probably warn-only in M0; refuse-by-default with a `--force` opt-in is a v1.1 hardening.
3. **Persist serializer scope.** JSON-only (numbers, strings, booleans, arrays, tables) is the M0 scope. Lua functions/userdata/threads error-and-skip with a warning. Is that strict enough? Some mods will want to serialize references to engine objects (e.g. "the mech the player is escorting"). Probably need a `mc2.ref(objectId)` boxing primitive that survives serialization as a stable engine ID — but that's M1.
4. **Mid-mission mod enable/disable.** Out of scope for M0 (only profile-driven boot-time mod selection). Track E may need it if a launcher UI exposes per-mission mod toggles.
5. **Determinism for multiplayer.** Lua's `pairs()` iteration order is not deterministic across implementations. Even on a single Lua build, table-rehash on insertion can scramble order. If multiplayer ships, mods need a deterministic-iteration discipline (`ipairs` only, or sorted-keys helper). Defer until multiplayer is on the roadmap.
6. **Module reload graph.** Section §5 handles `data.lua`/`control.lua` reload; what about a `require`d sub-module like `mods/X/scripts/util.lua`? Likely "reload anything under a mod's tree triggers a `control.lua` re-run for that mod" — but mod authors may want finer control. Resolve when the second hot-reload bug report arrives.
7. **Crash-during-data.lua recovery.** A `data.lua` that hard-crashes the host process (Lua C-API misuse, stack overflow, OOM) is currently fatal. We could wrap in SEH on Windows / signal handler on POSIX to convert to mod-fatal — but this is fraught (corrupt Lua state survives the catch). Leave fatal for M0; revisit only if a real crash appears in field testing.

---

## 10. References

- Predecessor: `2026-04-30-track-c-lua-implementation-shape.md` (VM class, sandbox, lifecycle wiring, ActionRegistry hook).
- Roadmap: `specs/2026-04-29-modders-paradise-roadmap-design.md` §6 (Track C), §8.2 (two-stage), §10 (open questions).
- Code sites (read-only, citations from impl-shape):
  - `code/mission.cpp:1705`, `:3336` — initABL/closeABL pair.
  - `code/missionbegin.cpp:122`, `:450` — initABL/closeABL pair.
  - `code/saveload.cpp:704` — initABL on save load.
  - `code/ablmc2.cpp:7491-7520`, `:7736-7785` — ABL file callbacks + init body precedent.
  - `code/warrior.cpp:2160` — brain execute (future Lua brain dispatch site).
- Collaborator: `.claude/worktrees/mod-profile-launcher/docs/plans/2026-04-25-mod-profile-launcher-plan-1-foundation.md` — `--profile` CLI, `profile_manager::bindActive()`, vendored `nlohmann/json`.
- Memory: `stock_install_must_remain_playable.md` (architectural rule), `public_fork_and_release.md` (release framing), `mco_mech_csv_format.md` (loose-CSV precedent).
- External: Factorio `data.lua`/`control.lua` contract — <https://wiki.factorio.com/Tutorial:Modding_tutorial>; Factorio dependency syntax — <https://wiki.factorio.com/Tutorial:Mod_structure#dependencies>.
