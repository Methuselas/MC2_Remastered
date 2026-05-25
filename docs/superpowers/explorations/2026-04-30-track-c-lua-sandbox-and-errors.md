# Track C — Lua Sandbox & Error Handling: Design Spec

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- [`2026-04-29-track-c-lua-scripting-status.md`](2026-04-29-track-c-lua-scripting-status.md)
- [`2026-04-30-track-c-lua-implementation-shape.md`](2026-04-30-track-c-lua-implementation-shape.md) §6
**Spec:** [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §8.3

The implementation-shape doc carries a 30-line sketch of `LuaVM::Init`'s sandbox setup. This document fixes the complete behavior: which standard libraries are loaded, what file I/O is reachable, where Lua errors get caught, what resource caps apply, what the threading constraint is, how a corrupt VM recovers, and the test-case battery the sandbox must defeat. Everything below is binding on the C-3 commit; nothing here is negotiable per-mod.

---

## 1. Stdlib whitelist

Lua 5.4 ships ten standard libraries. The first-slice policy is:

| Library            | Status   | Reason |
|--------------------|----------|--------|
| `base`             | partial  | Need `assert`, `error`, `pcall`, `xpcall`, `print`, `pairs`, `ipairs`, `select`, `tonumber`, `tostring`, `type`, `next`, `unpack`/`table.unpack`. Strip the loaders and reflection escape hatches. |
| `math`             | full     | Pure functions, no escape vectors. |
| `string`           | full     | Pure. `string.rep` capped indirectly via memory cap (§4). |
| `table`            | full     | Pure. |
| `utf8`             | full     | Pure. |
| `coroutine`        | full     | Cooperative; can't escape sandbox. Useful for mod state machines. (Status doc §4 deferred this; we admit it now — coroutines do not breach memory or I/O isolation.) |
| `io`               | replaced | Original `io.*` not loaded. A path-validating shim `mc2.io.read(path)` (§2) is exposed instead. |
| `os`               | replaced | Original `os.*` not loaded. Re-export `os.time`, `os.clock`, `os.date`, `os.difftime` only — pure, read-only. No `os.execute`, `os.remove`, `os.rename`, `os.exit`, `os.getenv`, `os.tmpname`, `os.setlocale`. |
| `package`          | none     | Not loaded. `require` is replaced by a sandboxed loader (§2). `package.loadlib` would let a mod open a DLL — never. |
| `debug`            | partial  | Not loaded via `open_libraries`. We expose only `debug.traceback` as a plain function in the sandbox env, since `xpcall` needs it for error reports (§3). Everything else (`debug.getinfo`, `debug.getlocal`, `debug.sethook`, `debug.setupvalue`, etc.) stays unreachable — they would let a mod read host upvalues, install conflicting hooks, or rewrite C frames. |

### Concrete `sol::state` setup

```cpp
state.open_libraries(
    sol::lib::base,
    sol::lib::math,
    sol::lib::string,
    sol::lib::table,
    sol::lib::utf8,
    sol::lib::coroutine
);
// io, os, package, debug — NOT opened.

// --- base library cleanup ---
// Code-execution loaders: gone. No mod is allowed to load arbitrary
// Lua source or, especially, untrusted bytecode (load() accepts both
// and bytecode loads bypass all syntactic safety).
state["dofile"]         = sol::nil;
state["loadfile"]       = sol::nil;
state["load"]           = sol::nil;
state["loadstring"]     = sol::nil;   // 5.1-compat shim if LUA_COMPAT_5_3 exposed it
// VM owns GC pacing; mod scripts must not stall the GC or force
// massive collections.
state["collectgarbage"] = sol::nil;
// Raw access bypasses metatables we use to freeze prototype tables
// (impl-shape §7) and to make mc2.* readonly.
state["rawequal"]       = sol::nil;
state["rawget"]         = sol::nil;
state["rawset"]         = sol::nil;
state["rawlen"]         = sol::nil;
// Metatable manipulation: gone in M0 to keep the freeze guarantees
// trivially sound. Re-enable per-table metatables if mods ask for OOP.
state["setmetatable"]   = sol::nil;
state["getmetatable"]   = sol::nil;

// --- partial os surface ---
sol::table os_tbl = state.create_named_table("os");
os_tbl["time"]       = []() { return std::time(nullptr); };
os_tbl["clock"]      = []() { return (double)std::clock() / CLOCKS_PER_SEC; };
os_tbl["date"]       = sol::overload(/* ... date variants ... */);
os_tbl["difftime"]   = [](double a, double b) { return a - b; };
// os.execute / .remove / .rename / .exit / .getenv / .tmpname / .setlocale
// deliberately absent.

// --- replacement io (see §2) ---
sol::table io_tbl = state.create_named_table("io");
io_tbl["read"]  = &mc2lua::SafeIoRead;   // path-validating shim
// no io.open, io.write, io.popen, io.lines, io.input, io.output, io.tmpfile.

// --- expose debug.traceback only ---
sol::table dbg_tbl = state.create_named_table("debug");
dbg_tbl["traceback"] = state["_internal_traceback"];
// _internal_traceback is registered by us as a thin wrapper around
// luaL_traceback so it survives even though sol::lib::debug isn't open.

// --- sandbox env (impl-shape §6) ---
sandbox = sol::environment(state, sol::create, state.globals());
```

The sandbox env is what every loaded chunk runs against. After Init, we rebind `state["_G"]` inside the sandbox to the sandbox itself so `_G.foo = bar` from mod code stays in the mod's view rather than leaking into the host state.

---

## 2. File I/O sandbox

### Reachable roots, in order of precedence

1. `<install_root>/mods/<modid>/` — read-only. The mod's own tree. Lua chunks loaded via `require` resolve here first.
2. `<install_root>/data/` — read-only. Game data (tgl/, art/, objects/, sound/). Mods may want to read JSON tables that ship with the engine.

### Forbidden

- Any path resolving above `<install_root>` (i.e. containing `..` after canonicalization, or starting with a drive letter / UNC / `/` absolute).
- Any write. No `io.open(path, "w")`, no `io.write`, no `os.remove`, no `os.rename`. Mods cannot persist data via the FS shim. (Persistence between missions, when we eventually need it, will go through an explicit `mc2.save_state(table)` binding that writes into the savegame, not the FS.)
- Any access to other mods' directories. `mods/foo` cannot read `mods/bar`. The active mod's id is recorded on the `LuaVM::Impl` and consulted by the resolver.

### Path-resolution shim

```cpp
// modding/lua_fs_sandbox.cpp
namespace mc2lua {

static std::string g_install_root;        // canonicalized, set at Init
static std::string g_current_mod_root;    // canonicalized "<install>/mods/<id>"
static std::string g_data_root;           // canonicalized "<install>/data"

// Returns canonicalized abs path on success, empty string on rejection.
// Logs [LUA v1] event=io_reject reason=... path=... when it rejects.
static std::string ResolveModRead(const std::string& rel) {
    // 1. Reject empty, absolute, drive-letter, UNC, or NUL-bearing input.
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\') return {};
    if (rel.size() >= 2 && rel[1] == ':') return {};        // "C:..."
    if (rel.find('\0') != std::string::npos) return {};

    // 2. Try mod-root, then data-root (in that order — mods can shadow data
    //    only by carrying their own copy explicitly).
    for (const auto& root : { g_current_mod_root, g_data_root }) {
        std::string joined  = root + "/" + rel;
        std::string canon   = filesystem::weakly_canonical(joined).generic_string();

        // 3. Reject if canonicalization escaped the root (".." traversal).
        if (canon.rfind(root + "/", 0) != 0 && canon != root) continue;

        // 4. Reject symlinks pointing outside the root (Windows: junctions).
        if (filesystem::is_symlink(canon)) {
            auto target = filesystem::read_symlink(canon).generic_string();
            if (filesystem::weakly_canonical(target).generic_string()
                    .rfind(root + "/", 0) != 0) continue;
        }

        if (filesystem::exists(canon) && filesystem::is_regular_file(canon))
            return canon;
    }
    return {};
}

// Bound as mc2.io.read(path) -> string|nil
sol::object SafeIoRead(const std::string& rel, sol::this_state ts) {
    std::string canon = ResolveModRead(rel);
    if (canon.empty()) {
        printf("[LUA v1] event=io_reject reason=resolve path=%s\n", rel.c_str());
        return sol::nil;
    }
    // Use MC2's File class to honor the loose-overrides-FST chain
    // (memory/stock_install_must_remain_playable.md). 64 MiB hard cap
    // per read so a malicious 4 GB symlink target can't OOM us.
    File f;
    if (f.open(canon.c_str()) != NO_ERR) return sol::nil;
    constexpr size_t kReadCap = 64 * 1024 * 1024;
    long len = std::min<long>(f.fileSize(), (long)kReadCap);
    std::string buf(len, '\0');
    f.read((BYTE*)buf.data(), len);
    return sol::object(ts, sol::in_place, std::move(buf));
}

} // namespace mc2lua
```

`require` reuses the same resolver: `require("foo.bar")` becomes `ResolveModRead("scripts/foo/bar.lua")` against the *mod root only* (never `data/` — game data isn't Lua source). The chunk is compiled in the sandbox env via `state.script(buf, sandbox)`. ABL's `ablFileOpenCB` (`code/ablmc2.cpp:7507`) is intentionally laxer — Lua does not inherit that surface.

---

## 3. pcall / xpcall error catching

### Per-event wrapping (canonical)

The C++→Lua dispatch trampolines (`LuaVM::CallEvent`, `LuaVM::DispatchAction`, timer expiry inside `Tick`) wrap the Lua-side function in `xpcall` with `debug.traceback` as the message handler. **Every** Lua entry from the engine routes through this wrapper. Per-binding wrapping is unnecessary: a Lua error inside a binding-call propagates up to the enclosing event invocation, where the `xpcall` catches it.

```cpp
void LuaVM::CallEvent(const char* eventName) {
    auto it = pimpl_->event_handlers.find(eventName);
    if (it == pimpl_->event_handlers.end()) return;
    for (sol::function& cb : it->second) {
        sol::protected_function pcb = cb;
        pcb.error_handler = pimpl_->state["_internal_traceback"];
        sol::protected_function_result r = pcb();
        if (!r.valid()) {
            sol::error err = r;
            LogLuaError(eventName, err.what());
        }
    }
}

void LogLuaError(const char* event, const char* msg) {
    // Schema: [LUA v1] event=error origin=<event-name> mod=<id> msg=<...>
    // The traceback is in msg (xpcall replaced the original error with
    // traceback-prepended text); we don't try to parse out file/line —
    // Lua's traceback already formatted them.
    printf("[LUA v1] event=error origin=%s mod=%s msg=%s\n",
           event,
           g_current_mod_id.c_str(),
           msg);
    fflush(stdout);
}
```

### Per-tick wrapping

`LuaVM::Tick(deltaSec)` already iterates expired timers; each timer fires through the same `xpcall` wrapper. A throwing timer logs an error and continues to the next timer. The Lua VM survives.

### Crash-isolation invariant

A Lua error during *any* event invocation **must not** propagate to engine state. Concretely:

- No `lua_error`/`lua_pcall` longjmp can escape into MC2 C++ frames. Sol2's `sol::protected_function` enforces this (it issues `lua_pcall` internally). Plain `state.script(...)` is forbidden in dispatch paths — only `state.script(..., sandbox)` returning `sol::protected_function_result` is admitted.
- After an error, the VM is left in a coherent state: timers/event-handlers tables are unchanged, `_G` is unchanged. Lua's exception model rolls back automatically; we don't try to undo partial side-effects in C++ bindings (which is impossible anyway), but we do guarantee the *VM* itself is safe to keep using.

### Stack trace format

`debug.traceback` is exposed in the sandbox even though `sol::lib::debug` isn't loaded — registering a single C wrapper around `luaL_traceback` and binding it in the sandbox env is sufficient. The shipped traceback strings start with `stack traceback:` and list every Lua frame with file/line; that goes verbatim into the `msg=<...>` field of the `[LUA v1] event=error` line.

---

## 4. Resource caps

### Memory cap per VM

```cpp
// 64 MiB upper bound per VM. Factorio's default is ~1 GB
// (configurable), but Factorio mods are heavier. 64 MiB is generous
// for mission-script and AI-table workloads and small enough to fail
// fast on runaway allocators.
static constexpr size_t kMemoryCapBytes = 64 * 1024 * 1024;

struct LuaAllocCtx { size_t used = 0; size_t cap = kMemoryCapBytes; };

static void* LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* ctx = static_cast<LuaAllocCtx*>(ud);
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        ctx->used -= old;
        free(ptr);
        return nullptr;
    }
    if (ctx->used + nsize - old > ctx->cap) return nullptr;  // OOM ⇒ Lua error
    void* np = realloc(ptr, nsize);
    if (np) ctx->used = ctx->used + nsize - old;
    return np;
}

// At Init, instead of sol::state state_, use:
//   ctx_ = new LuaAllocCtx;
//   lua_State* L = lua_newstate(LuaAlloc, ctx_);
//   sol::state_view sv(L); state_ takes ownership via sol::state(L).
```

When the cap is exceeded, `LuaAlloc` returns `NULL`; Lua converts this to a regular Lua error which the surrounding `xpcall` catches and logs.

### Per-tick instruction cap

```cpp
// Fire after N opcodes; abort runaway loops with lua_error.
static constexpr int kInstructionCap     = 5'000'000;  // ~5 ms wall on this hardware
static constexpr int kHookEverryNOpcodes = 1000;       // hook resolution

static void LuaInstructionHook(lua_State* L, lua_Debug*) {
    auto* ctx = /* fetch from extraspace */;
    if (++ctx->opcount > kInstructionCap) {
        luaL_error(L, "instruction cap exceeded (%d opcodes)", kInstructionCap);
    }
}

// In LuaVM::Tick, BEFORE dispatching events for this frame:
ctx_->opcount = 0;
lua_sethook(L, LuaInstructionHook, LUA_MASKCOUNT, kHookEverryNOpcodes);
// dispatch...
lua_sethook(L, nullptr, 0, 0);  // disable between frames
```

Counter resets per tick — a mod can use ~5M opcodes per frame, not per session. Combined with the memory cap and the `xpcall` wrapper, a `while true do end` aborts cleanly mid-frame, logs an error, and the engine continues.

### String / table allocation budget

There is no separate string-table budget. The memory cap subsumes it: `string.rep("x", 1e9)` allocates 1 GB which trips the 64 MiB cap before Lua finishes the call. Same for unbounded `table.insert` loops.

---

## 5. Threading model

The Lua VM is **main-thread-only**. The engine's gameplay logic runs single-threaded, and all engine sites that call `LuaVM::CallEvent` / `Tick` / `DispatchAction` are on that thread. There is no plan to add Lua-from-worker dispatch in M0–M2.

Concretely: `lua_State*` is not `Send` / cross-thread safe; `sol::state` does not protect against concurrent access. Any future audio/render thread that needs to *trigger* a Lua callback must enqueue an event onto a main-thread queue rather than calling Lua directly.

Multiplayer (roadmap §10, deferred) introduces a determinism requirement: every Lua callback must produce the same effects on every host given the same engine inputs. The current sandbox already disallows the obvious nondeterminism sources — `os.execute`, `os.tmpname`, `io.open`-for-write, `package.loadlib`, `debug.*` — so the determinism gap when MP lands is mostly about clamping `math.random` to a deterministic seed and gating `os.time` / `os.clock` behind a determinism toggle. That work belongs to the MP track, not C-3.

---

## 6. Crash recovery

### What constitutes "corrupt VM"

- A Lua error escaped into C++ (should be impossible given §3 — but defense in depth).
- The memory allocator returned NULL during a critical bind (e.g. `mc2.on_event` couldn't push to `event_handlers`).
- An assertion fired inside a native binding (engine bug, not a mod bug).

### Recovery flow

1. `LuaVM::Shutdown()` is called. This flushes timers, clears handler tables, calls `pimpl_->state.collect_garbage()`, and destroys `lua_State` via `sol::state` dtor.
2. A new `LuaVM` is constructed; `Init()` re-runs sandbox setup; `LoadDataStage`/`LoadControlStage` re-run for each enabled mod.
3. If any stage re-load fails, the engine logs `[LUA v1] event=recovery_failed mod=<id> stage=<data|control>` and proceeds with **no Lua** for the rest of the mission. The legacy `.abx` path is untouched, so the mission is still playable.

### User-facing surface

For M0: log file only. The smoke runner already greps for `event=error` lines and surfaces them in `tests/smoke/artifacts/<ts>/`. An in-engine console message for a mod-author-facing reload UI is Track E (mod-discovery) territory.

### Mission-level vs engine-level

VM teardown/reload is *mission-scoped*, mirroring `closeABL`/`initABL`. We never recreate the VM mid-mission — that would orphan registered timers and event handlers in surprising ways. If `Init` fails at engine boot, the engine boots without Lua and logs that fact; `g_LuaVM` stays NULL. Every C++ dispatch site is already NULL-tolerant (it has to be, since unmodded installs run with no LuaVM).

---

## 7. Sandbox test cases

Each item is a Lua snippet, the expected sandbox response, and the expected log line. These become the contents of `mods/test/scripts/tests/sandbox_*.lua`, dispatched by a `--lua-sandbox-test` smoke flag.

| #  | Snippet                                                             | Expected response                                | Log line keyword          |
|----|---------------------------------------------------------------------|--------------------------------------------------|---------------------------|
| 1  | `os.execute("rm -rf /")`                                            | `nil` index → "attempt to call a nil value"      | `event=error origin=...`  |
| 2  | `os.remove("data/foo")`                                             | nil call                                         | `event=error`             |
| 3  | `os.exit(0)`                                                        | nil call                                         | `event=error`             |
| 4  | `os.getenv("PATH")`                                                 | nil call                                         | `event=error`             |
| 5  | `io.open("../../etc/passwd", "r")`                                  | `mc2.io.read` rejects on canonicalize            | `event=io_reject reason=resolve` |
| 6  | `mc2.io.read("/etc/passwd")`                                        | rejects on absolute prefix                       | `event=io_reject reason=resolve` |
| 7  | `mc2.io.read("..\\..\\Windows\\System32\\drivers\\etc\\hosts")`     | rejects (`..` after canonicalize)                | `event=io_reject reason=resolve` |
| 8  | `mc2.io.read("../../mods/other/secret.lua")`                        | rejects (escapes mod root)                       | `event=io_reject reason=resolve` |
| 9  | `package.loadlib("kernel32", "ExitProcess")`                        | `package` is nil                                 | `event=error`             |
| 10 | `debug.getinfo(1)`                                                  | `debug.getinfo` is nil (only `traceback` exposed)| `event=error`             |
| 11 | `load("return os.execute('calc')")()`                               | `load` is nil                                    | `event=error`             |
| 12 | `dofile("ahem.lua")`                                                | `dofile` is nil                                  | `event=error`             |
| 13 | `while true do end`                                                 | instruction cap fires                            | `event=error msg=...instruction cap...` |
| 14 | `local t={} while true do table.insert(t,0) end`                    | memory cap fires before instruction cap          | `event=error msg=...not enough memory...` |
| 15 | `string.rep("x", 1e9)`                                              | memory cap fires inside `string.rep`             | `event=error msg=...not enough memory...` |
| 16 | `error("crash")`                                                    | `xpcall` catches, traceback logged               | `event=error msg=...crash\nstack traceback:...` |
| 17 | `local function f() f() end f()`                                    | Lua stack overflow → error                       | `event=error msg=...stack overflow...` |
| 18 | `setmetatable({}, {__index=function() while true do end end})`      | `setmetatable` is nil                            | `event=error`             |
| 19 | `coroutine.create(function() while true do end end)` then `resume`  | resume runs until instruction cap fires inside co| `event=error`             |
| 20 | `mc2.io.read("scripts/data.lua")` *(legitimate, mod-local)*         | succeeds, returns chunk text                     | no error                  |

The sandbox is "passing" iff every row produces the indicated response in the smoke artifacts — never a crash, never silent success on a forbidden case, and never an `event=error` on row 20.

---

## 8. Open questions

1. **Sol2 panic vs exception handler.** Sol2 supports both `sol::state::set_panic` (called when an unprotected Lua error reaches C) and `sol::state::set_exception_handler` (for C++ exceptions thrown from bindings). Both should be installed at Init: panic logs `[LUA v1] event=panic msg=...` then aborts the VM (not the process); exception handler converts to `lua_error` so the surrounding `xpcall` catches it. Confirm the exact Sol2 4.x API when C-3 lands — function signatures changed between Sol2 3.x and 4.x.

2. **Memory cap interaction with `debug.traceback`.** When the cap is hit, allocating the traceback string itself can fail. Check whether `luaL_traceback` allocates; if so, reserve a small dedicated buffer outside the cap so the error message survives even when the VM's heap is exhausted.

3. **Coroutine instruction cap accounting.** Does `lua_sethook` apply across coroutine boundaries, or do we need to re-arm the hook after every `coroutine.resume`? The Lua manual is ambiguous; needs a test before C-3 ships coroutine support. If re-arming is needed, expose only a `mc2.spawn_task(fn)` shim that handles it for the modder.

4. **`require` cache semantics.** Do we want the standard Lua `package.loaded` deduplication? If yes, we need our own `package.loaded` table inside the sandbox env. If no, `require("foo")` recompiles `foo.lua` every call — fine for hot-reload, wasteful otherwise. M0 default: yes-with-cache, cleared on hot-reload.

5. **Save/load and Lua state.** A mod's `control.lua`-installed event handlers don't currently survive a savegame round-trip (the VM is recreated from scratch on load — status doc §3, impl-shape §4). Verifying this is correct: any mod that *needs* persistent state must use a future `mc2.save_state` binding that round-trips through the savegame blob, never via FS. Confirm this is the intended Track E surface, not something we need to engineer in C-3.

6. **Symlink rejection on Windows.** `std::filesystem::is_symlink` recognizes NTFS symbolic links and reparse points; it does *not* by default flag directory junctions reliably. Test the resolver against a junction pointing into `C:\Windows` before declaring §2 watertight.

7. **Bytecode rejection.** Stripping `load`/`loadstring` is necessary but possibly not sufficient if `require`'s loader (our own implementation) ever calls `luaL_loadbuffer` on a buffer whose first byte is `0x1B` (Lua bytecode header). Our `require` must explicitly check and reject bytecode chunks — text-source-only.

---

## 9. References

- Implementation shape: `2026-04-30-track-c-lua-implementation-shape.md` §6 (`LuaVM::Init` skeleton).
- Status snapshot: `2026-04-29-track-c-lua-scripting-status.md` §4 (sandbox baseline vs ABL).
- Roadmap: `specs/2026-04-29-modders-paradise-roadmap-design.md` §8.3.
- Source precedent: `code/ablmc2.cpp:7492-7520` — ABL file callbacks (intentionally **wider** than the Lua surface).
- Memory: `stock_install_must_remain_playable.md` — Lua failure must degrade to legacy `.abx`, never block boot.
- Lua 5.4 manual: §6.10 `debug` library (why we expose only `traceback`); §4.7 hook API (`lua_sethook`, `LUA_MASKCOUNT`).
- Sol2 docs: `sol::state::open_libraries`, `sol::protected_function`, `sol::state::set_exception_handler`, `sol::state::set_panic`.
