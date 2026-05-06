# Track C — Sol2 + Lua: Implementation-Shape Document

**Date:** 2026-04-30
**Mode:** Design only. No code changes in this commit.
**Predecessor:** [`2026-04-29-track-c-lua-scripting-status.md`](2026-04-29-track-c-lua-scripting-status.md)
**Spec:** [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §6 Track C, §8.2, §8.3, §5.5

> **⚠️ §5 trampoline pattern is superseded — TWICE.** Two corrections compound:
>
> **First correction (impl-shape doc was wrong):** The original §5 pseudocode treated `execXxx` functions as `(long* params, long* return)` taking C-style argument lists. **This is wrong.** All `execXxx` functions are `void(void)` and operate on a caller-pushed LIFO stack via `ABLi_popInteger / popReal / popCharPtr / pushInteger / pushReal / peekReal / pokeInteger` (declared in [`mclib/abl.h:123-148`](../../../mclib/abl.h)). Verified against 13 representative `execXxx` definitions at `code/ablmc2.cpp:422, 1926, 2208, 2232, 2262, 2444, 2468, 2546, 2562, 2640, 2657, 2922, 3404, 3423`.
>
> **Second correction (the trampolines doc itself was structurally broken):** Even the corrected push/call/pop pattern from the trampolines doc is wrong — `ABLi_popInteger` (`mclib/ablxstd.cpp:84-92`) is **bytecode-driven**, walking `codeSegmentPtr` rather than popping a pre-pushed value. Calling `execXxx` from outside an active `ABLi_execute` misexecutes whatever bytecode is at the current code pointer.
>
> **Authoritative pattern: `*_impl` C function extraction.** Each binding's engine logic is hoisted into a normal `extern "C"` function in `code/ablmc2.cpp`; ABL's `execXxx` becomes a thin pop/push wrapper around `*_impl`; Lua's trampoline calls `*_impl` directly, never touching the ABL stack. Full evidence + revised C-3 plan in [`2026-04-30-track-c-blocking-questions-resolution.md`](2026-04-30-track-c-blocking-questions-resolution.md) §Q1.
>
> **What to follow when implementing C-3:** blocking-questions doc §Q1 for the trampoline pattern; trampolines doc §4–§7 for performance budget, trace instrumentation, test checklist, and doc-gen pattern (those sections remain valid). The §5 wiring/lifecycle sections (§4) and bindings list (the ten names) of THIS doc remain valid; only the *call shape* is replaced.

This document fixes the *form* of the first Track C slice so the next session can start coding the vendoring step with no design questions remaining.

---

## 1. Vendoring layout

The actual on-disk convention in this worktree is `3rdparty/<libname>/` (one level), not `3rdparty/3rdparty/<libname>/` as the status doc shorthand suggests. Confirmed:

```
3rdparty/include/GL/, 3rdparty/include/SDL2/      (header packs)
3rdparty/tracy/Tracy.hpp + tracy/client/, common/  (vendored source)
3rdparty/ffmpeg-lgpl-win64/                        (prebuilt)
```

Tracy is the closest precedent (header + source vendored in-tree, built as part of mc2.exe), so Lua follows the Tracy shape and Sol2 follows the GL/SDL shape.

### Final layout

```
3rdparty/sol/
    sol.hpp                      # Single-file Sol2 amalgamation (~30k LoC)
    LICENSE.txt                  # Sol2 (MIT)
3rdparty/lua/
    src/                         # Lua 5.4.x sources, unmodified upstream
        lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c
        lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c
        lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
        lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c
        loadlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c linit.c
        lua.h luaconf.h lualib.h lauxlib.h lua.hpp
    LICENSE                      # Lua (MIT)
    README                       # Upstream README for provenance
```

### CMake integration (top-level `CMakeLists.txt`)

Insert after the Tracy include line at `CMakeLists.txt:154`:

```cmake
# Lua 5.4 — vendored static lib, no external dependency.
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/*.c")
list(REMOVE_ITEM LUA_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/lua.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/luac.c")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src")
target_compile_definitions(lua_static PUBLIC LUA_COMPAT_5_3)
if(MSVC)
    target_compile_options(lua_static PRIVATE /wd4334 /wd4146)
endif()

# Sol2 — header-only.
list(APPEND THIRDPARTY_INCLUDE_DIRS
     "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/sol"
     "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src")
```

Then add a new subdirectory for the modding code (after the `add_subdirectory("./gui" ...)` line at `CMakeLists.txt:160`):

```cmake
add_subdirectory("./modding" "./out/modding")
```

The `mc2` executable target picks up `modding/` source files explicitly (mirroring the `SOURCES` list pattern at line 165) and links `lua_static` plus the new `modding` lib.

### License placement

`3rdparty/sol/LICENSE.txt` and `3rdparty/lua/LICENSE` ship verbatim from upstream. Add a one-line entry to (or create) `THIRD_PARTY_LICENSES.md` at the repo root referencing both. Per `memory/public_fork_and_release.md` the public release flow already accommodates third-party MIT.

---

## 2. `modding/lua_vm.h` — full header

```cpp
// modding/lua_vm.h
// Per-mission Lua VM. Mirrors initABL/closeABL lifecycle.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>

// Forward-declare so this header doesn't drag sol/sol.hpp into the world.
// sol::state is fwd-declarable via its underlying lua_State*; we hold a
// pimpl to keep ablmc2.cpp / mission.cpp compile times unchanged.
namespace sol { class state; }

namespace mc2lua {

enum class Stage { Data, Control };

struct ModEntry {
    std::string id;
    std::string root_dir;          // e.g. "mods/test"
    std::string data_entrypoint;   // e.g. "scripts/data.lua"
    std::string control_entrypoint;// e.g. "scripts/control.lua"
};

class LuaVM {
public:
    LuaVM();
    ~LuaVM();

    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    // Lifecycle — called from mission init/teardown sites.
    bool Init();         // Creates state_, opens whitelisted libs, installs mc2.* table, sandbox env.
    void Shutdown();     // Releases state_; safe to call multiple times.

    // Stage dispatch.
    bool LoadDataStage(const ModEntry& mod);     // data.lua — prototypes only, no game state.
    bool LoadControlStage(const ModEntry& mod);  // control.lua — installs event handlers.

    // Per-frame tick — drains expired Lua timers and dispatches them.
    void Tick(float deltaSec);

    // Event dispatch hooks. Called from existing C++ event sites
    // (mission_start, warrior alarm callbacks, timer expiry, etc.).
    void CallEvent(const char* eventName);
    void CallEvent(const char* eventName, int arg);
    void CallEvent(const char* eventName, int objectId, float x, float y);

    // ActionRegistry-side hook (see §8). Lua-installed handlers
    // dispatch through this when an "Action.<key>" string lands.
    bool DispatchAction(const char* key);

    // Diagnostics.
    bool HasError() const { return last_error_.size() != 0; }
    const std::string& LastError() const { return last_error_; }

    // Access to the underlying sol::state for binding registration only.
    sol::state& State();

private:
    struct Impl;
    Impl* pimpl_;            // sol::state + handler tables.
    std::string last_error_;
    Stage current_stage_ = Stage::Data;
    bool initialized_ = false;
};

// Single per-mission VM, owned by Mission/MissionBegin/SaveLoad lifetime.
// Created by initLuaVM(), torn down by closeLuaVM(). NULL between missions.
extern LuaVM* g_LuaVM;

void initLuaVM();    // mirrors initABL()
void closeLuaVM();   // mirrors closeABL()

} // namespace mc2lua
```

State the VM owns: a `sol::state` (which owns `lua_State*`), a `std::unordered_map<std::string, sol::function>` of event-name → Lua callback list, a similar map for action-key → Lua function, a small timer table (id → {expires_at, callback}), and the current sandbox env table (`sol::environment`).

---

## 3. `modding/lua_vm.cpp` — skeleton

```cpp
// modding/lua_vm.cpp
#include "lua_vm.h"
#include <sol/sol.hpp>
#include <stdio.h>

namespace mc2lua {

LuaVM* g_LuaVM = nullptr;

struct LuaVM::Impl {
    sol::state state;
    sol::environment sandbox;
    std::unordered_map<std::string, std::vector<sol::function>> event_handlers;
    std::unordered_map<std::string, sol::function> action_handlers;
    struct Timer { float expires; sol::function cb; bool active; };
    std::unordered_map<int, Timer> timers;
    float now_sec = 0.0f;
};

LuaVM::LuaVM() : pimpl_(new Impl) {}
LuaVM::~LuaVM() { Shutdown(); delete pimpl_; pimpl_ = nullptr; }

bool LuaVM::Init() {
    // TODO: open whitelisted stdlibs (base, math, string, table, utf8).
    // TODO: build sandbox env via sol::environment(state, sol::create).
    // TODO: install mc2.* binding table (calls registerMc2Bindings()).
    // TODO: register sandbox-friendly require() that resolves under mods/<id>/scripts.
    // TODO: install __index from sandbox -> safe_globals to keep _G clean.
    initialized_ = true;
    return true;
}

void LuaVM::Shutdown() {
    if (!initialized_) return;
    // TODO: clear timers, event_handlers, action_handlers.
    // TODO: pimpl_->state.collect_garbage(); — explicit; sol::state dtor finishes it.
    initialized_ = false;
}

bool LuaVM::LoadDataStage(const ModEntry& mod) {
    current_stage_ = Stage::Data;
    // TODO: install "data-stage only" API view (prototype tables, no engine queries).
    // TODO: pcall(loadfile(mod.root_dir + "/" + mod.data_entrypoint)) inside sandbox.
    // TODO: capture errors into last_error_ and log [LUA v1] event=data_load status=...
    return true;
}

bool LuaVM::LoadControlStage(const ModEntry& mod) {
    current_stage_ = Stage::Control;
    // TODO: swap exposed API to "control-stage" view (event hooks, mc2.spawn_object, etc.).
    // TODO: pcall(loadfile(mod.root_dir + "/" + mod.control_entrypoint)) inside sandbox.
    return true;
}

void LuaVM::Tick(float deltaSec) {
    pimpl_->now_sec += deltaSec;
    // TODO: walk timers, fire expired callbacks via pcall, deactivate.
}

void LuaVM::CallEvent(const char* eventName) {
    // TODO: look up event_handlers[eventName], pcall each, log+swallow errors.
}
void LuaVM::CallEvent(const char* eventName, int arg) { /* TODO */ }
void LuaVM::CallEvent(const char* eventName, int objectId, float x, float y) { /* TODO */ }

bool LuaVM::DispatchAction(const char* key) {
    // TODO: look up action_handlers[key]; if found, pcall, return true; else false.
    return false;
}

sol::state& LuaVM::State() { return pimpl_->state; }

void initLuaVM() {
    if (g_LuaVM) return;
    g_LuaVM = new LuaVM();
    g_LuaVM->Init();
    // TODO: enumerate enabled mods (placeholder = single hardcoded "mods/test"),
    //       call LoadDataStage then LoadControlStage for each.
    printf("[LUA v1] event=init status=ok\n");
}

void closeLuaVM() {
    if (!g_LuaVM) return;
    g_LuaVM->Shutdown();
    delete g_LuaVM;
    g_LuaVM = nullptr;
    printf("[LUA v1] event=close status=ok\n");
}

} // namespace mc2lua
```

Implementation note: `sol::state` is heavy (~64 KB). One per mission is correct; never per-warrior.

---

## 4. VM lifecycle wiring (mirrors initABL/closeABL exactly)

Three init sites, two teardown sites — match status doc §3 / source citations verified above.

### `code/mission.cpp:1705`

```cpp
   //-----------------------
   // Init the ABL system...
   initABL();
+  mc2lua::initLuaVM();   // Track C: mirror of initABL
```

### `code/mission.cpp:3336`

```cpp
+  mc2lua::closeLuaVM();  // Track C: mirror of closeABL — must run BEFORE closeABL
   closeABL();
```

(Order matters: Lua callbacks may reference ABL handles; tear Lua down first so any final pcall can't call back into a half-destroyed ABL.)

### `code/missionbegin.cpp:122`

```cpp
   { ZoneScopedN("MissionBegin::begin initABL");
   initABL();
+  mc2lua::initLuaVM();
   }
```

### `code/missionbegin.cpp:450`

```cpp
   logisticsBrain = NULL;
+  mc2lua::closeLuaVM();
   closeABL();
```

### `code/saveload.cpp:704`

```cpp
   //-----------------------
   // Init the ABL system...
   initABL();
+  mc2lua::initLuaVM();
```

(Saveload has no explicit `closeABL()` partner here; teardown rides the matching `Mission::~Mission` / `mission.cpp:3336` path. No additional saveload edit needed.)

Per-frame `Tick()` slots into the existing mission tick (`Mission::update` near where `objMgr->update` runs). Exact line picked when wiring lands — not load-bearing for this design.

---

## 5. `modding/lua_bindings_mc2.cpp` — 10 starter bindings

Each binding is a Sol2 lambda trampoline that calls the existing `execXxx` function (or its underlying engine call directly when that's cleaner than building a fake ABL `params` array). The ABL exec functions take `long* params, long* resultType` — for Lua we want plain typed parameters, so most bindings call the deeper engine helper rather than `execXxx` itself. The ABL signature column gives the type hint.

```cpp
// modding/lua_bindings_mc2.cpp
#include <sol/sol.hpp>
#include "lua_vm.h"

// Forward-decls into engine / ABL exec layer.
extern long execGetTime(long* params, long* resultType);
extern long execObjectCreate(long* params, long* resultType);
// ...etc — all 290 in code/ablmc2.cpp.

namespace mc2lua {

void registerMc2Bindings(sol::state& lua) {
    sol::table mc2 = lua.create_named_table("mc2");

    // 1. mc2.log(msg)  ← ABL debug-print channel (ABLi_setDebugPrintCallback).
    mc2["log"] = [](const std::string& msg) {
        printf("[LUA] %s\n", msg.c_str());  // routes through ablDebugPrintCallback infra
    };

    // 2. mc2.get_time() -> number  ← execGetTime
    mc2["get_time"] = []() -> double {
        long params[1] = {0}, rtype = 0;
        execGetTime(params, &rtype);
        return *reinterpret_cast<float*>(&params[0]);  // ABL real return convention
    };

    // 3a. mc2.set_timer(id, secs)  ← execSetTimer (ABL sig "i*", ret "i")
    mc2["set_timer"] = [](int id, double secs) -> int {
        long params[2] = {id, 0};
        *reinterpret_cast<float*>(&params[1]) = (float)secs;
        long rtype = 0;
        return (int)execSetTimer(params, &rtype);
    };
    // 3b. mc2.check_timer(id) -> seconds remaining  ← execCheckTimer
    mc2["check_timer"] = [](int id) -> double {
        long params[1] = {id}, rtype = 0;
        execCheckTimer(params, &rtype);
        return *reinterpret_cast<float*>(&params[0]);
    };

    // 4. mc2.spawn_object(typeId, x, y, side) -> objectId  ← execObjectCreate
    //    ABL exec is "i" -> "i"; richer wrapper goes through ObjectManager directly.
    mc2["spawn_object"] = [](int typeId, double x, double y, int side) -> int {
        // TODO: bypass execObjectCreate and call ObjectManager::createObject(typeId, {x,y,0}, side)
        return -1;
    };

    // 5. mc2.damage_object(id, dmg, sourceId, hitLocation, dirX, dirY)  ← execDamageObject
    //    ABL sig "iiirirr" -> "i"
    mc2["damage_object"] = [](int id, int dmg, int srcId, double hitLoc, int dmgType, double dx, double dy) -> int {
        // TODO: marshal into long params[7], call execDamageObject.
        return 0;
    };

    // 6a. mc2.object_status(id) -> int  ← execObjectStatus
    mc2["object_status"] = [](int id) -> int {
        long params[1] = {id}, rtype = 0;
        return (int)execObjectStatus(params, &rtype);
    };
    // 6b. mc2.object_exists(id) -> bool  ← execObjectExists
    mc2["object_exists"] = [](int id) -> bool {
        long params[1] = {id}, rtype = 0;
        return execObjectExists(params, &rtype) != 0;
    };

    // 7a. mc2.set_objective_status(id, status) -> int  ← execSetObjectiveStatus
    mc2["set_objective_status"] = [](int id, int status) -> int {
        long params[2] = {id, status}, rtype = 0;
        return (int)execSetObjectiveStatus(params, &rtype);
    };
    // 7b. mc2.check_objective_status(id) -> int  ← execCheckObjectiveStatus
    mc2["check_objective_status"] = [](int id) -> int {
        long params[1] = {id}, rtype = 0;
        return (int)execCheckObjectiveStatus(params, &rtype);
    };

    // 8. mc2.in_area(id, x, y, r) -> bool  ← execInArea (sig "iRri" -> "b")
    mc2["in_area"] = [](int id, double x, double y, double r) -> bool {
        // TODO: marshal Stuff::Vector3D R into params; call execInArea.
        return false;
    };

    // 9a. mc2.play_video(name) -> int  ← execPlayVideo (sig "C" -> "i")
    mc2["play_video"] = [](const std::string& name) -> int {
        // TODO: pack name pointer into params[0]; call execPlayVideo.
        return 0;
    };
    // 9b. mc2.play_sound(id) -> int  ← execPlaySoundEffect
    mc2["play_sound"] = [](int sfxId) -> int {
        long params[1] = {sfxId}, rtype = 0;
        return (int)execPlaySoundEffect(params, &rtype);
    };

    // 10a. mc2.set_global(key, val)  ← execSetGlobalValue (sig "i*")
    mc2["set_global"] = [](int key, double val) {
        long params[2] = {key, 0};
        *reinterpret_cast<float*>(&params[1]) = (float)val;
        long rtype = 0;
        execSetGlobalValue(params, &rtype);
    };
    // 10b. mc2.get_global(key) -> number  ← execGetGlobalValue
    mc2["get_global"] = [](int key) -> double {
        long params[1] = {key}, rtype = 0;
        execGetGlobalValue(params, &rtype);
        return *reinterpret_cast<float*>(&params[0]);
    };

    // Event-registration entrypoint.
    mc2["on_event"] = [](const std::string& name, sol::function cb) {
        if (g_LuaVM) {
            // TODO: g_LuaVM->RegisterEventHandler(name, cb);
        }
    };
}

} // namespace mc2lua
```

The ABL-`reinterpret_cast`-from-`long*` pattern is ugly but it's how `execGetTime` et al. return reals (status doc §2 type letters: `r` packs into the same `long*` cell). Code lives in one file, so the ugliness stays contained — Track C M1 will refactor toward a `mc2lua::call_abl<>(...)` template, but the M0 slice is fine being literal.

---

## 6. Sandbox setup

Sol2 makes the sandbox cheap. The entire setup is ~30 lines inside `LuaVM::Init`:

```cpp
// Whitelisted standard libraries.
state.open_libraries(
    sol::lib::base,     // assert, error, ipairs, pairs, pcall, print, tostring, tonumber, type
    sol::lib::math,
    sol::lib::string,
    sol::lib::table,
    sol::lib::utf8
);
// NOT opened: io, os, package, debug, coroutine (re-enable coroutine in M1 if mods need it).

// Strip the few hazards that sol::lib::base leaves behind.
state["dofile"]      = sol::nil;
state["loadfile"]    = sol::nil;
state["load"]        = sol::nil;     // bytecode loader — never let mods inject opcodes.
state["loadstring"]  = sol::nil;
state["collectgarbage"] = sol::nil;  // VM owns GC pacing.
state["rawequal"]    = sol::nil;
state["rawget"]      = sol::nil;
state["rawset"]      = sol::nil;
state["setmetatable"]= sol::nil;     // re-enable scoped if mods need OOP.
state["getmetatable"]= sol::nil;

// Whitelisted require: only resolves under mods/<modid>/scripts/.
state["require"] = [](const std::string& path) -> sol::object {
    // TODO: validate path has no .. or absolute prefix; resolve via File::open
    // restricted to current_mod_root_; return loaded chunk.
    return sol::nil;
};

// Replacement io: read-only, scoped to mod directory.
sol::table safe_io = state.create_named_table("io");
safe_io["open"] = [](const std::string& path, sol::optional<std::string> mode) -> sol::object {
    // TODO: mode must be "r" or "rb"; path must resolve under current_mod_root_.
    return sol::nil;
};

// Sandbox env — every loaded chunk runs against this, not _G.
sandbox = sol::environment(state, sol::create, state.globals());
```

Notes vs status doc §4:
- ABL has `ablFileWriteCB` / write access; Lua does **not** inherit this. Read-only `io.open`, no `io.write`, no `io.popen`.
- `os.execute` is removed by *not opening* `sol::lib::os` at all; this is stronger than nilling `os.execute` (since `os.getenv`, `os.remove`, `os.rename` go too).
- `package.loadlib` gone for the same reason — `sol::lib::package` is not opened.
- `debug.*` gone — `sol::lib::debug` not opened. Mods can't introspect host frames.

---

## 7. Two-stage data/control split mechanism

**Single `sol::state`, two exposed-API views.** Not two separate VMs.

Rationale: separate VMs (one for data, one for control) double the memory footprint and force serialize-deserialize plumbing for prototype tables passed between them. Factorio itself runs both stages in one Lua state — the discipline is *which API is reachable when*, not *which interpreter runs*.

Mechanism:

1. At `Init()`, install the union of bindings into the `mc2` table but record each binding's stage requirement in a sibling table `mc2._stage_required[name] = "data"|"control"|"any"`.
2. `LoadDataStage(mod)`:
   - Set `current_stage_ = Stage::Data`.
   - Replace `mc2` with a *view table* whose `__index` only exposes bindings tagged `data` or `any`. Game-state mutators (`spawn_object`, `damage_object`, `set_objective_status`) appear as `nil`; calling them raises a clear error.
   - Run `data.lua` inside the sandbox env. Mods register prototypes into `mc2.data.<entity_kind>[id] = {...}`.
3. After all mods' data stages complete, **freeze** the prototype tables: `setmetatable(mc2.data, {__newindex = function() error("data stage frozen") end})`. Status `Stage::Data` flips to `Stage::Control`.
4. `LoadControlStage(mod)`:
   - View swaps to expose `data`+`control`+`any`. Prototype tables are read-only (frozen). `mc2.spawn_object`, `mc2.on_event`, etc. now resolve.
   - Run `control.lua`. Mods install handlers via `mc2.on_event(...)`.

`current_stage_` plus a one-line gate at the top of any binding lambda gives us defense-in-depth in case the view-table swap is bypassed.

---

## 8. ActionRegistry interop hook (Lua side)

The C++ `ActionRegistry` (Track B §5.5 — owned by Track B's spec, this doc only describes the Lua side it must admit) has the dispatch interface:

```cpp
// In Track B's modding/action_registry.h (sketch, not authoritative):
class ActionRegistry {
public:
    void Register(const char* key, std::function<void()> handler);           // C++ handlers
    void RegisterLua(const char* key, /*opaque*/ void* lua_handler_handle);  // Lua handlers
    bool Dispatch(const char* key);
};
```

Lua side, exposed via the `mc2` table:

```cpp
// Inside registerMc2Bindings:
mc2["register_action"] = [](const std::string& key, sol::function fn) {
    if (!g_LuaVM) return;
    // Store the sol::function in LuaVM::Impl::action_handlers[key].
    // Then notify the C++ ActionRegistry that key is now Lua-backed:
    //   ActionRegistry::instance().RegisterLua(key.c_str(), &g_LuaVM /* opaque */);
    // C++ side, when Dispatch(key) hits a Lua-backed slot, calls back:
    //   g_LuaVM->DispatchAction(key);
};
```

`LuaVM::DispatchAction(key)` is the C++-callable trampoline (already in the header §2): looks up `action_handlers[key]`, pcalls it, returns `true` on hit. The opaque handle `RegisterLua` stores is just `g_LuaVM` itself — there is one VM per mission, so no per-handler bookkeeping is needed.

This admits both backends from day 1. When Track B lands FIT button dispatch, a button declared `Action="MyMod.OpenMarket"` reaches `ActionRegistry::Dispatch("MyMod.OpenMarket")` which sees the Lua-backed entry and calls `g_LuaVM->DispatchAction(...)`, which calls the registered Lua function. No FIT-aware code lives in Track C; no Lua-aware code lives in Track B.

---

## 9. Demo mission file

Path: `mods/test/scripts/missions/demo.lua` (mirrors roadmap §6 outcome gate).

```lua
-- mods/test/scripts/missions/demo.lua
-- Track C smoke: boots alongside an .abx mission, prints, spawns one mech.

mc2.log("demo.lua loaded — control stage")

local boot_t = mc2.get_time()
mc2.log(string.format("boot time = %.2f", boot_t))

mc2.on_event("mission_start", function()
    mc2.log("mission_start fired in Lua")
    mc2.set_timer(101, 5.0)
end)

mc2.on_event("timer_expired", function(id)
    if id == 101 then
        local oid = mc2.spawn_object(0x0100, 100.0, 100.0, 0)
        mc2.log("spawned object id=" .. tostring(oid))
    end
end)
```

The companion `mods/test/mod.json` and `mods/test/scripts/control.lua` (which `require`s the mission file) land in the same commit as the demo, but their content is dictated by §5.1/§5.2 of the roadmap — not part of the implementation shape.

---

## 10. Sequencing

Five commits land Track C M0. Each is independently buildable and reviewable; nothing in commit N+1 is needed for commit N to be useful.

1. **C-1 / vendoring** — drop `3rdparty/sol/sol.hpp` + LICENSE, drop `3rdparty/lua/src/*` + LICENSE, add the 14-line CMake block to `CMakeLists.txt:154`. Add empty `modding/CMakeLists.txt` declaring the future static lib. Verify: `cmake --build` produces `lua_static.lib` with no warnings beyond `/wd4334 /wd4146`. No engine behavior change.

2. **C-2 / VM class skeleton** — `modding/lua_vm.{h,cpp}` with the §2 / §3 contents, no bindings yet, no wiring. `g_LuaVM` exists but is never set. Verify: build clean, no link change to `mc2.exe` yet.

3. **C-3 / bindings (10 stubs)** — `modding/lua_bindings_mc2.cpp` with the §5 bindings. Wire `registerMc2Bindings()` into `LuaVM::Init`. Verify: a unit-test-shaped `lua_dofile_test()` (gated behind `MC2_LUA_TEST=1`) loads a one-line Lua chunk that calls `mc2.log` and `mc2.get_time`.

4. **C-4 / lifecycle wiring** — the five `+ mc2lua::initLuaVM();` / `closeLuaVM();` lines in §4. Now `g_LuaVM` actually exists during a mission. Verify: tier-1 smoke (`run_smoke.py --tier tier1`) still passes — no Lua content shipped, but VM init/teardown runs every mission. `[LUA v1] event=init|close` lines visible in artifacts.

5. **C-5 / demo mission** — drop `mods/test/mod.json`, `mods/test/scripts/data.lua`, `mods/test/scripts/control.lua`, `mods/test/scripts/missions/demo.lua` (§9). Hardcode `mods/test` as the only loaded mod for now; mod-discovery is Track E. Verify: launch any tier-1 mission, observe `[LUA] demo.lua loaded`, observe `[LUA] mission_start fired`, observe spawn or its TODO log line. Tier-1 still green.

Each commit message ends with `[CLAUDE.md§Memory & Discipline]`-style sidecar note per worktree convention.

---

## 11. Open questions (resolve before C-3)

1. **`execXxx` linkage.** Are the 290 `execXxx` symbols in `code/ablmc2.cpp` already extern-visible, or are they file-static? Quick `grep -n '^static.*exec' code/ablmc2.cpp` answers this. If static, either (a) remove the `static` qualifier on the 10 we bind, or (b) add a thin `mc2lua_call_abl_<name>` extern shim in `ablmc2.cpp`. Option (b) is cleaner and is what the bindings file should assume.

2. **ABL `params` real-return marshalling.** The `*reinterpret_cast<float*>(&params[0])` pattern in §5 assumes `sizeof(long) == sizeof(float) == 4`. True on Win64 (LLP64: `long` is 32-bit). Confirm before C-3; if any ABL slot ever stores doubles, the marshalling helper template is non-trivial.

3. **Mod discovery.** C-5 hardcodes `mods/test`. Mod enumeration (read `mods/*/mod.json`, build load order) is Track E §6. C-5 punts cleanly; nothing in this design depends on the answer.

4. **Lua-VM-during-saveload.** `code/saveload.cpp:704` calls `initABL()`. Should the LuaVM init there too, and run `data.lua` against the saved game's mod list? Decision: yes (status doc §3 says ABL inits at all three sites; Lua mirrors). The data-stage prototypes are deterministic, so re-running them on save load is safe and required for any save-resilient Lua state.

5. **Per-frame `Tick()` site.** Pick during C-4. Candidates: `Mission::update`, the same `objMgr->update` tick the cull chain runs on. Must be after game state advances (so timers fire on the right frame) but before render (so spawned objects appear same frame). Not load-bearing for the design.

---

## 12. References

- Status snapshot: `2026-04-29-track-c-lua-scripting-status.md` (this doc's predecessor).
- Roadmap: `specs/2026-04-29-modders-paradise-roadmap-design.md` §6, §8.2, §8.3, §5.5.
- Source (read-only, citations verified during this session):
  - `code/ablmc2.cpp:7491-7520` — file callbacks, sandbox precedent.
  - `code/ablmc2.cpp:7736-7785` — `initABL()` body (heaps, `ABLi_init`).
  - `code/ablmc2.cpp:7791-8127` — 290 `ABLi_addFunction` calls; the 10 bindings' `execXxx` pairs all live in this block.
  - `code/mission.cpp:1705`, `:3336` — initABL/closeABL site pair.
  - `code/missionbegin.cpp:122`, `:450` — initABL/closeABL site pair.
  - `code/saveload.cpp:704` — initABL site (no co-located closeABL).
  - `CMakeLists.txt:148-164` — THIRDPARTY_INCLUDE_DIRS pattern, `add_subdirectory` style.
  - `3rdparty/tracy/` — vendoring shape precedent.
- Memory: `stock_install_must_remain_playable.md` — Lua additions are sidecar; legacy `.abx` path untouched.
