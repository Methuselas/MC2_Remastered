# Track C — Sol2 + Lua Wiring: Status Snapshot

**Date:** 2026-04-29
**Roadmap:** [`2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §6 Track C
**Mode:** Read-only research; no code changes.

---

## 1. Current Lua / Sol2 state

**None.** Greenfield.

- `3rdparty/3rdparty/include/` contains only `GL/`, `SDL2/`, `unistd.h`, `zconf.h`, `zlib.h`. No `lua.h`, no `sol/`, no `sol.hpp`.
- Project-wide grep for `lua|sol2|Sol2` returns hits only in **documentation** (this roadmap, smoke runner naming coincidences in `scripts/smoke_lib/runner.py`/`logparse.py`, and the existing `docs/modding-guide.md`). Zero hits in `.cpp`/`.h`/`CMakeLists.txt` source code.
- No `LuaVM`, `LuaState`, `mc2_lua_*` symbols exist in the worktree.
- The roadmap's Track C is genuinely day-zero work.

---

## 2. ABL extension surface map

### Single canonical registration site

All ABL extension functions are registered in **one file**: [`code/ablmc2.cpp`](../../../code/ablmc2.cpp), inside the function `void initABL(void)` (starts at **line 7736**, ends ~line 8127).

The pattern is uniform:

```cpp
ABLi_addFunction("getid",         false, NULL,   "i", execGetId);
ABLi_addFunction("gettime",       false, NULL,   "r", execGetTime);
ABLi_addFunction("selectobject",  false, "i",    "i", execSelectObject);
ABLi_addFunction("orderattackobject", false, "iiiib", "i", execOrderAttackObject);
ABLi_addFunction("damageobject",  false, "iiirirr","i", execDamageObject);
// ...
```

Signature: `name, isOrder, paramTypes, returnType, execCallback`. Type letters: `i`=int, `r`=real, `b`=bool, `c`=char, `C`=string. Uppercase = output/by-ref. `*` = wildcard ANYTHING (per memory `carver5_mission_playable.md`).

### Counts (current `nifty-mendeleev` HEAD)

- **290** active `ABLi_addFunction` calls in `code/ablmc2.cpp` (a few extra are commented-out or dormant variants — `selectunit`, `setobjectivetimer`).
- The "51 stubs added for Carver5O/Omnitech" referenced in the roadmap correspond to the bulk added by commits `7c852e2` (40 Omnitech stubs) and `db8c00a` (11 FSM-primitive stubs), per `memory/omnitech_abl_stubs_session.md` and `memory/omnitech_abl_missing_names.md`. They are interleaved with the stock surface in `initABL()`.
- The **omnitech-abl worktree** (the historical home of this work) shows 283 calls, indicating nifty-mendeleev now carries the merged superset.

### Capacity

`mclib/ablsymt.h:166` — `#define MAX_STANDARD_FUNCTIONS 512` (raised from 256; the prior 256 OOB-wrote adjacent BSS, see comment 163-164). `FunctionCallbackTable` is a fixed `void(*)(void)` array indexed by registration order. Logged via `logAblFunctionTableCapacity()` at end of `initABL()`.

### Implementation pattern (per-extension)

Each function pairs an `execXxxx(long *params, long *resultType)` C function with the `ABLi_addFunction` registration line. The exec functions reach into MC2 globals — `mission`, `objMgr`, `commanderList[]`, `team[]` — and return ints/reals via the `params`/`returnType` channels. Roughly 7,700 lines of `ablmc2.cpp` are exec implementations followed by the registration block.

---

## 3. ABL VM lifecycle today

The Lua VM lifecycle should mirror this exactly.

### Init — once per mission load

`initABL()` is called from three sites:
- `code/mission.cpp:1705` — main `Mission::init()` path (gameplay missions).
- `code/missionbegin.cpp:122` — splash/begin sequencing.
- `code/saveload.cpp:704` — savegame load path.

`initABL()` does:
1. Allocates three heaps (`AblSymbolHeap`, `AblStackHeap`, `AblCodeHeap`) via `UserHeap::init` — sized at 6.1 MB / 4.1 MB / 2.5 MB respectively (heaps were 8x'd from stock to fit Omnitech/Carver5O scripts).
2. Calls `ABLi_init(stack, codeBlock, modules, statics, ...callbacks...)` — runtime stack 65535, code block 1 MB, max registered modules 1024, max statics 512.
3. Registers global callbacks (`setDebugPrintCallback`, `setRandomCallbacks`, `setEndlessStateCallback`).
4. Registers all 290 standard functions via `ABLi_addFunction`.

### Execution — per warrior brain tick

ABL is **per-warrior**, not per-mission. Each `Warrior` owns an `ABLModule* brain` (instantiated at `code/warrior.cpp:2107` via `brain = new ABLModule;` and resolved by handle at line 8392 via `ABLi_getModule`). The brain runs via `brain->execute()` (warrior.cpp:2160) on the warrior's update tick, plus alarm callbacks (lines 4675, 4832, 4838).

So: ~one ABL VM execution context per active warrior, all sharing the global function table and heaps.

### Teardown — once per mission end

`closeABL()` at `code/ablmc2.cpp:8131` is called from `mission.cpp:3336` and `missionbegin.cpp:450`. It calls `ABLi_close()` and deletes the three heaps.

### Implication for Lua

The Factorio-style two-stage split (data.lua at load, control.lua per mission) maps cleanly:
- **`data.lua`** runs once at engine boot, before mission load — equivalent to `ABLi_addFunction` registration time. Registers prototypes into engine tables.
- **`control.lua`** runs at mission init alongside `initABL()`. The Lua `lua_State` is created, sandboxed, and bound. Per-mission events (mission start, mech destroyed, timer tick) dispatch into Lua callbacks here.
- **Mirrors `closeABL()`** — `lua_close()` at mission teardown; never carries state across missions.

One Lua VM per mission load (matching the spec) is correct; do not attempt one-Lua-VM-per-warrior — the ABL scaling assumption (one shared function table, many module instances) doesn't translate, and a per-warrior Lua VM is overkill cost-wise.

---

## 4. Sandboxing baseline (what ABL has access to today)

ABL is **not sandboxed**. It has direct C-function bridges to:

- **Filesystem read/write** via `ablFileOpenCB` / `ablFileCreateCB` / `ablFileWriteCB` / `ablFileReadCB` (`code/ablmc2.cpp:7492-7520`). These wrap MC2's `File` class — which honors the loose-file-overrides-FST mechanism. ABL `.abx` scripts can `read_file`/`write_file` against any path that resolves through MC2's `File::open()`.
- **All engine internals** — every exec function reaches into `mission`, `objMgr`, `team[]`, `commanderList[]`, `weaponSpec[]`, etc. ABL can spawn movers, damage objects, set global values, change sides, play videos, end the mission.
- **Full game-state mutation** — `execObjectSuicide`, `execDamageObject`, `execObjectChangeSides`, `execSetGlobalValue`, `execSetObjectivePos`, `execSetCaptured`.
- **No networking.** Neither ABL nor the engine has a generic socket API. Multiplayer goes through the dedicated `multplyr.h` path, not ABL.
- **No shell.** No `os.execute` analogue.

**Lua sandbox should be tighter than ABL, not looser.** Practical baseline for first slice:

- Strip `os.execute`, `os.remove`, `os.rename`, `package.loadlib`, `debug.*`.
- `io.*` — whitelist read-only access scoped to the mod's own directory. No write. (ABL has write; we do not need to inherit that hazard.)
- `require` — sandboxed loader that only resolves from `mods/<modid>/scripts/`.
- All engine bindings go through a single `mc2.*` table installed into a fresh `_ENV`.

Roadmap §8.3 already states this. Sol2's `sol::state.set_function` + a clean environment table makes it ~30 lines of C++.

---

## 5. Proposed first-slice scope for Sol2 wiring

### Vendoring location

Sol2 is header-only. Drop into `3rdparty/3rdparty/include/sol/` (alongside `GL/`, `SDL2/`). Add Lua 5.4 sources to `3rdparty/3rdparty/src/lua/` (Lua's ~16k LoC builds clean as a static lib via a small CMake target). One new line in top-level `CMakeLists.txt` to build `lua_static` and link it against `mc2.exe`.

### Where the LuaVM class lives

New top-level directory per roadmap §8.5 ("new code in new directories"): `modding/`. Concretely:

- `modding/lua_vm.h` — `class LuaVM { sol::state state_; ... };`
- `modding/lua_vm.cpp` — sandbox setup, binding registration.
- `modding/lua_bindings_mc2.cpp` — the `mc2.*` API surface.
- `modding/mod_registry.h/.cpp` — placeholder; populated when Track E lands.

The LuaVM is created/destroyed in lockstep with ABL: `Mission::init()` constructs it right after `initABL()`; `Mission::~Mission()` (or whatever site invokes `closeABL()` in `mission.cpp:3336`) destructs it.

### Highest-priority Lua bindings (first 10)

Pick from the ABL surface for two reasons: (a) modders already understand the semantics from existing `.abx` scripts, (b) the exec functions are already implemented — bindings are 1-line trampolines.

| # | Lua call | ABL counterpart | Why |
|---|----------|-----------------|-----|
| 1 | `mc2.log(msg)` | `ABLi_setDebugPrintCallback` channel | Console output is the first thing every modder needs. |
| 2 | `mc2.get_time()` | `execGetTime` | Mission-time queries appear in nearly every brain. |
| 3 | `mc2.set_timer(id, secs)` / `mc2.check_timer(id)` | `execSetTimer` / `execCheckTimer` | Timed events are the most-used scripting primitive. |
| 4 | `mc2.spawn_object(typeId, x, y, side)` | `execObjectCreate` | Mission-script spawn is the headline modder verb. |
| 5 | `mc2.damage_object(id, dmg, ...)` | `execDamageObject` | Scripted destruction triggers. |
| 6 | `mc2.object_status(id)` / `mc2.object_exists(id)` | `execObjectStatus` / `execObjectExists` | Most condition checks. |
| 7 | `mc2.set_objective_status(id, status)` / `mc2.check_objective_status(id)` | `execSetObjectiveStatus` / `execCheckObjectiveStatus` | Drives mission win/lose. |
| 8 | `mc2.in_area(id, x, y, r)` | `execInArea` | Trigger-zone primitive. |
| 9 | `mc2.play_video(name)` / `mc2.play_sound(id)` | `execPlayVideo` / `execPlaySoundEffect` | Cinematic beats. |
| 10 | `mc2.set_global(key, val)` / `mc2.get_global(key)` | `execSetGlobalValue` / `execGetGlobalValue` | Cross-script state. |

These ten cover the vast majority of mission-script verbs in stock content. Adding the remaining ~280 ABL exec functions becomes mechanical once the binding pattern is established (Sol2 lambdas wrapping the existing `execXxx` functions; the `ABLi_addFunction` type-letter strings give us the type signatures for free).

### Demo mission shape

`mods/test/scripts/missions/demo.lua` (per roadmap §6 outcome gate):

```lua
-- demo.lua
mc2.log("Demo Lua mission booting")
local start_time = mc2.get_time()

mc2.on_event("mission_start", function()
  mc2.log("Mission has started")
  mc2.set_timer(1, 30.0)  -- 30-second delay
end)

mc2.on_event("timer_expired", function(id)
  if id == 1 then
    mc2.log("Timer 1 expired, spawning a mech")
    local id = mc2.spawn_object(MECH_BUSHWACKER, 100, 100, SIDE_PLAYER)
    mc2.log("Spawned mech id=" .. tostring(id))
  end
end)
```

This boots alongside (not replacing) an `.abx` mission. The Lua VM hooks the same mission tick that ABL brains tick on. `mc2.on_event` is implemented by registering callbacks in the LuaVM and invoking them from the existing C++ event sites (mission start, warrior alarm callbacks, the existing `brainAlarmCallback` array at `warrior.cpp:4675`).

### Outcome gate (roadmap §6)

A `.lua` file in `mods/test/scripts/missions/demo.lua` boots a mission, spawns a mech, prints to console, runs to completion. Tier-1 smoke (`run_smoke.py --tier tier1`) still passes because the legacy `.abx` path is untouched; Lua executes alongside it.

---

## 6. References

**Source files cited (all paths under `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`):**
- `code/ablmc2.cpp:7491-7520` — file callbacks (sandbox surface).
- `code/ablmc2.cpp:7736-8127` — `initABL()` body, all `ABLi_addFunction` calls.
- `code/ablmc2.cpp:8131-8147` — `closeABL()`.
- `code/mission.cpp:1705`, `code/missionbegin.cpp:122`, `code/saveload.cpp:704` — `initABL()` callers.
- `code/mission.cpp:3336`, `code/missionbegin.cpp:450` — `closeABL()` callers.
- `code/warrior.cpp:2107` — per-warrior `brain = new ABLModule`.
- `code/warrior.cpp:2160`, `4675`, `4832`, `4838` — `brain->execute()` sites.
- `mclib/ablsymt.h:166` — `MAX_STANDARD_FUNCTIONS 512`.
- `mclib/ablxstd.cpp:989-1007` — `FunctionCallbackTable` lookup + uninit-slot guard.

**Memory entries:**
- `memory/omnitech_abl_stubs_session.md` — origin of the 40 Omnitech stubs, registration mechanics.
- `memory/omnitech_abl_missing_names.md` — 11 additional FSM-primitive stubs (`db8c00a`).
- `memory/carver5_mission_playable.md` — `MAX_STANDARD_FUNCTIONS 256→512` raise rationale; `*` = ANYTHING wildcard.
- `memory/mco_omnitech_integration_attempt.md` — ABL gap class.
- `memory/stock_install_must_remain_playable.md` — sidecar architectural rule that Track C must obey.

**Spec context:**
- `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` §6 Track C, §5.3 hot-reload contract, §8.2/§8.3 two-stage Lua + sandbox.
