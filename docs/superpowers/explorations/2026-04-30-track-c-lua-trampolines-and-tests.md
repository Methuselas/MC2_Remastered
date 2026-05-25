# Track C — Lua Trampoline Pattern, Performance & Test Plan

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- [`2026-04-29-track-c-lua-scripting-status.md`](2026-04-29-track-c-lua-scripting-status.md) — status snapshot, ABL surface map.
- [`2026-04-30-track-c-lua-implementation-shape.md`](2026-04-30-track-c-lua-implementation-shape.md) — vendoring, VM class, 10 starter binding sketches, sequencing.
- [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §6 Track C.

This doc fixes the **complete trampoline pattern** (not the one-line sketches in the implementation-shape doc), the **callback-taking case**, the **performance budget**, the **trace/debug instrumentation**, the **test mission checklist**, and the **doc auto-generation pattern**.

> **⚠️ §1 reference example and §2 worked bindings are SUPERSEDED.** The "push args, call execXxx, pop return" pattern documented in this doc was based on a misread of the ABL VM. Subsequent investigation in [`2026-04-30-track-c-blocking-questions-resolution.md`](2026-04-30-track-c-blocking-questions-resolution.md) §Q1 found that `ABLi_popInteger` (`mclib/ablxstd.cpp:84-92`) is **bytecode-driven** — it walks `codeSegmentPtr` rather than popping a pre-pushed value. Calling `execXxx` from outside an active `ABLi_execute` therefore misexecutes whatever bytecode is at the current code pointer. **The corrected pattern is `*_impl` C function extraction**: each binding's engine logic is hoisted into a normal `extern "C"` function in `code/ablmc2.cpp`; ABL's `execXxx` becomes a thin pop/push wrapper around `*_impl`; Lua's trampoline calls `*_impl` directly, never touching the ABL stack. See blocking-questions doc §Q1 for the full pattern, evidence, and revised C-3 plan. Treat §1 and §2 of THIS doc as historical context only. The performance budget (§4), trace instrumentation (§5), test checklist (§6), and doc-gen pattern (§7) are unaffected and remain authoritative.

---

## 0. ABL exec-function calling convention (correction to implementation-shape §5)

The §5 sketches in `2026-04-30-track-c-lua-implementation-shape.md` assumed `execXxx(long* params, long* resultType)`. That is **wrong**. Verified from `code/ablmc2.cpp`:

```cpp
void execGetTime (void);          // ablmc2.cpp:422
void execDamageObject (void);     // ablmc2.cpp:1926
void execObjectCreate (void);     // ablmc2.cpp:2208
void execObjectExists (void);     // ablmc2.cpp:2232
void execObjectStatus (void);     // ablmc2.cpp:2262
void execSetTimer (void);         // ablmc2.cpp:2444
void execCheckTimer (void);       // ablmc2.cpp:2468
void execSetObjectiveStatus(void);// ablmc2.cpp:2546
void execCheckObjectiveStatus(void);// ablmc2.cpp:2562
void execPlaySoundEffect(void);   // ablmc2.cpp:2640
void execPlayVideo(void);         // ablmc2.cpp:2657
void execInArea (void);           // ablmc2.cpp:2922
void execGetGlobalValue (void);   // ablmc2.cpp:3404
void execSetGlobalValue (void);   // ablmc2.cpp:3423
```

All are `void(void)`. They reach an ABL VM stack via the helpers declared in `mclib/abl.h:123-148`:

```cpp
int   ABLi_popInteger();    void  ABLi_pushInteger(int);    int   ABLi_peekInteger();
float ABLi_popReal();       void  ABLi_pushReal(float);     float ABLi_peekReal();
char* ABLi_popCharPtr();    void  ABLi_pokeInteger(int);    void  ABLi_pokeReal(float);
```

The pattern in `execDamageObject` (ablmc2.cpp:1957-1967):

```cpp
long  targetId       = ABLi_popInteger();   // pop in declaration order
long  attackerId     = ABLi_popInteger();
long  weaponMasterId = ABLi_popInteger();
float damage         = ABLi_popReal();
long  hitLocation    = ABLi_popInteger();
float hitRoll        = ABLi_popReal();
float entryAngle     = ABLi_peekReal();     // peek the LAST one (it's the result slot)
ABLi_pokeInteger(-1);                       // overwrite peeked slot with return value
```

This is a **caller-pushed stack ABI**. To invoke an exec function from Lua we must **synthesize the same stack state**: push every argument, call `execXxx()`, then pop the return value. The cleanest way is to expose a set of `ABLi_push*` helpers (already extern-visible per `abl.h`) and wrap each binding in a templated `call_abl` helper that pushes args, calls the exec, and pops the result. The exec functions themselves are **not** declared in any header (they are file-local-but-non-static in `ablmc2.cpp`); we add a tiny extern shim header `modding/lua_abl_shim.h` that forward-declares the 10 we bind, with one-line inline wrappers per binding.

The §5 sketches' `execGetTime(params, &rtype)` and `*reinterpret_cast<float*>(&params[0])` are unreachable — discard them. The corrected pattern is in §1 below.

This also resolves Implementation-Shape doc §11 open question 2 (real marshalling): irrelevant, since we never touch ABL's internal `long*` cells from Lua.

---

## 1. Reference binding — `mc2.damage_object(...)` worked example

Goal: full Sol2 trampoline, validation, error reporting, trace, return-value handling. About 50 lines of real C++.

`modding/lua_abl_shim.h` (forward declarations + push helpers):

```cpp
// modding/lua_abl_shim.h — extern shims so binding code doesn't need
// to extern-declare each exec function inline. One-line block.
#pragma once
extern "C" {
    // ABL VM stack helpers (from mclib/abl.h, already extern-visible):
    int   ABLi_popInteger();    void  ABLi_pushInteger(int);
    float ABLi_popReal();       void  ABLi_pushReal(float);
    char* ABLi_popCharPtr();    void  ABLi_pushUserPtr(void*); // for "C" strings, see playvideo
}
// Exec entry points — non-static in ablmc2.cpp, just need declarations.
extern void execGetTime(void);
extern void execDamageObject(void);
extern void execObjectCreate(void);
extern void execObjectStatus(void);
extern void execObjectExists(void);
extern void execSetTimer(void);
extern void execCheckTimer(void);
extern void execSetObjectiveStatus(void);
extern void execCheckObjectiveStatus(void);
extern void execPlaySoundEffect(void);
extern void execPlayVideo(void);
extern void execInArea(void);
extern void execGetGlobalValue(void);
extern void execSetGlobalValue(void);
```

`modding/lua_bindings_mc2.cpp` — the reference binding:

```cpp
// modding/lua_bindings_mc2.cpp
#include <sol/sol.hpp>
#include "lua_vm.h"
#include "lua_abl_shim.h"
#include "lua_trace.h"   // MC2_LUA_TRACE macro, see §5
#include "objmgr.h"      // ObjectManager + GameObjectPtr, for sanity-validation
extern ObjectManager* objMgr;

namespace mc2lua {

// === Reference binding: mc2.damage_object ===
//
// ABL counterpart: damageobject  (ablmc2.cpp:7842, exec at 1926)
// ABL signature:   "iiirirr" -> "i"
//   targetId, attackerId, weaponMasterId, damage, hitLocation, hitRoll, entryAngle
//   returns: int (-1 bad target, -2 bad attacker, >0 = hit count)
//
// Lua signature:   mc2.damage_object(targetId, attackerId, weaponId, dmg,
//                                    hitLoc, hitRoll, entryAngle)
//                  -> integer  (or nil + error string on validation failure)
//
static int lua_damage_object(sol::this_state ts,
                             sol::object   v_target,
                             sol::object   v_attacker,
                             sol::object   v_weapon,
                             sol::object   v_dmg,
                             sol::object   v_hitloc,
                             sol::object   v_hitroll,
                             sol::object   v_entryAngle)
{
    sol::state_view L(ts);

    // ---- Validation (cheap, fail loud, never throw across C boundary) ----
    auto need_int = [&](sol::object o, const char* name) -> std::pair<bool,int> {
        if (!o.is<int>()) { L.script(std::string(
            "error('mc2.damage_object: ") + name + " must be integer', 2)"); return {false,0}; }
        return {true, o.as<int>()};
    };
    auto need_num = [&](sol::object o, const char* name) -> std::pair<bool,float> {
        if (!o.is<double>()) { L.script(std::string(
            "error('mc2.damage_object: ") + name + " must be number', 2)"); return {false,0}; }
        return {true, (float)o.as<double>()};
    };

    auto [ok1,target ]   = need_int(v_target,    "targetId");      if(!ok1) return -1;
    auto [ok2,attacker]  = need_int(v_attacker,  "attackerId");    if(!ok2) return -1;
    auto [ok3,weapon  ]  = need_int(v_weapon,    "weaponId");      if(!ok3) return -1;
    auto [ok4,dmg     ]  = need_num(v_dmg,       "damage");        if(!ok4) return -1;
    auto [ok5,hitloc  ]  = need_int(v_hitloc,    "hitLocation");   if(!ok5) return -1;
    auto [ok6,hitroll ]  = need_num(v_hitroll,   "hitRoll");       if(!ok6) return -1;
    auto [ok7,entryAng]  = need_num(v_entryAngle,"entryAngle");    if(!ok7) return -1;

    // ---- Range / handle validation (fail soft: log + return error code) ----
    if (dmg < 0.0f || dmg > 100000.0f) {
        MC2_LUA_TRACE("damage_object reject reason=damage_out_of_range val=%.1f", dmg);
        return -3;  // distinct error code; doesn't crash mod
    }
    // GameObjectPtr lookup uses MC2's standard getObject(). NULL == bad handle.
    extern GameObjectPtr getObject(long, bool=false);
    if (!getObject(target)) {
        MC2_LUA_TRACE("damage_object reject reason=bad_target id=%d", target);
        return -1;
    }
    if (!getObject(attacker)) {
        MC2_LUA_TRACE("damage_object reject reason=bad_attacker id=%d", attacker);
        return -2;
    }

    // ---- Push args onto the ABL stack in the order execDamageObject pops them.
    // Pop order in execDamageObject(): target, attacker, weapon, damage,
    //                                  hitLoc, hitRoll, entryAngle (peek).
    // Stack is LIFO -> push in REVERSE pop order.
    ABLi_pushReal   (entryAng);
    ABLi_pushReal   (hitroll);
    ABLi_pushInteger(hitloc);
    ABLi_pushReal   (dmg);
    ABLi_pushInteger(weapon);
    ABLi_pushInteger(attacker);
    ABLi_pushInteger(target);

    // ---- Call exec. It will pop 6 args, peek+poke the 7th as its return slot.
    execDamageObject();

    // ---- Pop the result the exec poked into the last-peeked slot.
    int result = ABLi_popInteger();

    MC2_LUA_TRACE("damage_object call target=%d attacker=%d dmg=%.1f -> %d",
                  target, attacker, dmg, result);
    return result;
}

} // namespace mc2lua

// In registerMc2Bindings:
//   mc2.set_function("damage_object", &mc2lua::lua_damage_object);
```

Pattern points worth highlighting:

1. **Validation lives in two tiers.** Tier-1 type-check (`o.is<int>()`) raises a Lua error via `error(..., 2)` — the `2` makes the line number point at the *caller* in mod code, not at the binding. Tier-2 semantic validation (range, handle existence) returns an error code and traces; never raises. Modders learn to check the return value.
2. **No exceptions cross the C boundary.** Sol2 can throw on type mismatch when using strongly-typed signatures (`int targetId` directly). We accept `sol::object` and check by hand to keep error messages informative and to avoid `lua_error` longjmping past stack-allocated C++ destructors. (Sol2 documents this hazard at length; the safety toggle is `SOL_SAFE_*` macros, but explicit validation is clearer.)
3. **Return-value pop is mandatory.** The exec function poked an integer into the ABL stack top; if we don't pop it, the next ABL call sees a corrupted stack. A leaked stack slot per Lua call would silently break ABL brains within a mission.
4. **Trace is unconditional inside `MC2_LUA_TRACE` — the macro itself gates** (see §5).
5. **Sol2 lambda capture: nothing captured.** Bindings are pure functions over the global ABL stack and `objMgr`. This keeps `sol::function` storage cheap (no per-binding closure) and makes the bindings hot-reloadable later.

---

## 2. Templates for the other 9 bindings

### 2.1 `mc2.log(level, msg)` — string-only (full)

```cpp
static void lua_log(sol::this_state ts, sol::object v_level, sol::object v_msg) {
    sol::state_view L(ts);
    if (!v_msg.is<std::string>()) {
        L.script("error('mc2.log: msg must be string', 2)"); return;
    }
    int level = v_level.is<int>() ? v_level.as<int>() : 0;
    const std::string& msg = v_msg.as<std::string>();
    static const char* lvl[4] = {"INFO","WARN","ERROR","DEBUG"};
    const char* tag = (level >= 0 && level < 4) ? lvl[level] : "INFO";
    printf("[LUA %s] %s\n", tag, msg.c_str());
    MC2_LUA_TRACE("log level=%s msg=\"%s\"", tag, msg.c_str());
}
```

No ABL trampoline — log goes straight to stdout / `gosASSERT_print`. No validation beyond type check; level is forgiving (out-of-range falls back to INFO). String-only path is the simplest possible pattern.

### 2.2 `mc2.timer.create(seconds, callback)` — callback-taking (full, see §3 for full callback discussion)

```cpp
static int lua_timer_create(sol::this_state ts, sol::object v_secs, sol::object v_cb) {
    sol::state_view L(ts);
    if (!v_secs.is<double>()) { L.script("error('mc2.timer.create: secs must be number',2)"); return 0; }
    if (!v_cb.is<sol::function>()) { L.script("error('mc2.timer.create: cb must be function',2)"); return 0; }
    if (!g_LuaVM) return 0;

    float secs = (float)v_secs.as<double>();
    if (secs < 0.0f || secs > 36000.0f) {  // 10 hours sanity ceiling
        MC2_LUA_TRACE("timer.create reject reason=secs_out_of_range val=%.2f", secs);
        return 0;
    }

    sol::function cb = v_cb.as<sol::function>();
    int id = g_LuaVM->RegisterTimer(secs, std::move(cb));   // §3.1
    MC2_LUA_TRACE("timer.create id=%d secs=%.2f", id, secs);
    return id;
}
```

The Lua function reference is moved into `LuaVM`-owned storage. See §3 for the full lifecycle.

### 2.3 `mc2.spawn_mech(prototype_id, x, y, team)` — prototype lookup + spawn (full)

```cpp
static int lua_spawn_mech(sol::this_state ts,
                          sol::object v_proto, sol::object v_x,
                          sol::object v_y,     sol::object v_team)
{
    sol::state_view L(ts);
    auto need_int = [&](sol::object o, const char* n)->std::pair<bool,int>{
        if(!o.is<int>()){L.script(std::string("error('mc2.spawn_mech: ")+n+" must be int',2)");return{false,0};}
        return {true, o.as<int>()};
    };
    auto need_num = [&](sol::object o, const char* n)->std::pair<bool,float>{
        if(!o.is<double>()){L.script(std::string("error('mc2.spawn_mech: ")+n+" must be number',2)");return{false,0};}
        return {true, (float)o.as<double>()};
    };

    auto [okp,proto] = need_int(v_proto,"prototype_id"); if(!okp) return 0;
    auto [okx,x]     = need_num(v_x,    "x");            if(!okx) return 0;
    auto [oky,y]     = need_num(v_y,    "y");            if(!oky) return 0;
    auto [okt,team]  = need_int(v_team, "team");         if(!okt) return 0;

    if (team < 0 || team >= 8) {
        MC2_LUA_TRACE("spawn_mech reject reason=bad_team val=%d", team);
        return 0;
    }
    // Prototype lookup: ObjectManager has type tables (mc2x integration uses these).
    extern bool ObjectManager_PrototypeExists(long protoId);  // shim added if needed
    if (!ObjectManager_PrototypeExists(proto)) {
        MC2_LUA_TRACE("spawn_mech reject reason=no_prototype id=%d", proto);
        return 0;
    }

    // Note: existing execObjectCreate(ablmc2.cpp:2208) only flips an EXISTING
    // object's exists-bit; it does NOT instantiate from a prototype. For a
    // real "spawn" we need ObjectManager::createObject(...). That call lives
    // in code/objmgr.cpp; the wrapper goes alongside this binding in M1.
    extern long ObjectManager_CreateMech(int proto, float x, float y, int team);
    long newId = ObjectManager_CreateMech(proto, x, y, team);
    MC2_LUA_TRACE("spawn_mech proto=%d pos=(%.1f,%.1f) team=%d -> id=%ld",
                  proto, x, y, team, newId);
    return (int)newId;
}
```

The other six bindings — `mc2.object.status(id)`, `mc2.mission.objective(id, status)`, `mc2.audio.play_sound(id)`, `mc2.get_time()`, `mc2.set_timer(id, secs)` (numeric ABL timer, distinct from `mc2.timer.create`), `mc2.in_area(id,x,y,r)` — **follow the reference pattern**. Each pushes its args onto the ABL stack in reverse pop order, calls `execXxx`, pops the result, and traces. Nothing about them is novel after §1.

---

## 3. Callback-taking bindings (the hard case)

`mc2.timer.create(seconds, fn)` and `mc2.on_event(name, fn)` pass Lua functions *into* engine state. Three failure modes have to be handled cleanly.

### 3.1 Reference holding

`sol::function` is a thin handle that stores a Lua registry reference (`luaL_ref` under the hood). Storing one in a C++ container (`std::unordered_map<int, sol::function>`) keeps the corresponding Lua function alive against GC for as long as the `sol::function` object lives.

```cpp
// modding/lua_vm.cpp — addition to Impl
struct Timer {
    float          expires_at;
    sol::function  cb;             // owns the Lua-side ref while in here
    bool           one_shot = true;
    bool           active   = true;
};
std::unordered_map<int, Timer> timers;
int next_timer_id = 1;

int LuaVM::RegisterTimer(float secs, sol::function cb) {
    int id = pimpl_->next_timer_id++;
    pimpl_->timers.emplace(id, Timer{ pimpl_->now_sec + secs, std::move(cb), true, true });
    return id;
}
```

Use `sol::function` (not `sol::main_protected_function`) **only because** there are no coroutines in our sandbox — `sol::lib::coroutine` is not opened (Implementation-Shape §6). If we ever open it, switch to `sol::main_protected_function` so callbacks resolve against the main thread state, not the coroutine thread that registered them.

### 3.2 Tick-time dispatch

```cpp
void LuaVM::Tick(float deltaSec) {
    pimpl_->now_sec += deltaSec;
    for (auto& [id, t] : pimpl_->timers) {
        if (!t.active || t.expires_at > pimpl_->now_sec) continue;
        // sol::protected_function lets us catch Lua errors instead of longjmping.
        sol::protected_function pf(t.cb);
        sol::protected_function_result r = pf(id);
        if (!r.valid()) {
            sol::error err = r;
            printf("[LUA ERROR] timer id=%d: %s\n", id, err.what());
            MC2_LUA_TRACE("timer.fire id=%d status=error msg=\"%s\"", id, err.what());
        } else {
            MC2_LUA_TRACE("timer.fire id=%d status=ok", id);
        }
        t.active = false;            // one-shot
    }
    // Reap dead timers in a second pass to avoid mutating during iteration.
    std::erase_if(pimpl_->timers, [](auto& kv){ return !kv.second.active; });
}
```

Three behaviors are load-bearing:
- **`sol::protected_function`** — wraps the callback so a Lua-side `error()` returns a result rather than longjmping out of `Tick()` (which would skip C++ destructors).
- **Reap after iteration** — the callback might call `mc2.timer.create(...)` to chain a timer, mutating the map. Mutate-during-iterate would be UB.
- **One-shot by default** — repeating timers are opt-in via a third arg `repeat=true`. Default-one-shot prevents runaway ticks if a modder forgets to cancel.

### 3.3 VM teardown safety

`LuaVM::Shutdown()` clears `timers` and `event_handlers` **before** destroying `pimpl_->state`. Each `sol::function` destructor runs while the `lua_State*` is still live, releasing its registry slot cleanly. Reverse order would access a freed `lua_State`.

The lifecycle wiring (Implementation-Shape §4) tears Lua *before* ABL — so even if a timer fires inside the final `Tick()` and the callback calls `mc2.damage_object`, ABL is still alive. Once `closeLuaVM()` returns, no more callbacks can fire; the order is a hard invariant.

### 3.4 Engine teardown during a fired callback

Concrete scenario: `mc2.timer.create(0.5, fn)`, where `fn` calls `mc2.spawn_mech` after the player has already exited the mission. Mitigation: every binding lambda checks `g_LuaVM != nullptr && g_LuaVM->initialized()` at the top. If the VM is mid-shutdown the binding returns the error sentinel (-1 or 0) without touching engine state. Defense in depth: between `closeLuaVM()` and `closeABL()` the `g_LuaVM` pointer is null, so any Lua call into engine bindings is a no-op — but no Lua call should be in flight anyway, since `lua_close()` already ran.

---

## 4. Performance considerations

### 4.1 Per-call overhead

Sol2 with no allocation and direct lambda binding is in the **20-80 ns range** per Lua-to-C call on x86_64. (Public benchmarks consistently show this; we match them in spirit.) Allocation-free type checks on `sol::object` add ~5 ns each. The reference `damage_object` binding in §1 has 7 type checks → ~35 ns of validation overhead — negligible against the engine work that follows.

### 4.2 When to disable type-checking

Sol2's `SOL_SAFE_USERTYPE`, `SOL_SAFE_REFERENCES`, `SOL_SAFE_FUNCTION_CALLS` are on by default in safe builds and add bounds-checking on userdata access. **Do not disable them** for the M0 slice. The 2-3× speedup they cost is invisible at our call rates (see §4.3); the safety they buy (no segfault on a misused handle) is the entire point of running mods sandboxed.

If profiling later shows a hot Lua-driven path consuming >0.5 ms/frame and bottlenecked on type checks, we can opt that path into a `sol::protected_function` that pre-binds known types — but only with a measurement to justify it.

### 4.3 Per-frame callback policy

ABL's per-warrior brain tick (`code/warrior.cpp:2160` — `brain->execute()`) runs at the warrior's update frequency, which is **~30 Hz, not 60 Hz** — warrior update is gated by the cull-active block tick. Stock missions ship with 12-24 active warriors during gameplay. Carver5O with Omnitech tops out around 40.

A Lua callback at the same cadence is comparable: 40 warriors × 30 Hz × ~80 ns Lua-call overhead ≈ 0.1 ms/frame. **Affordable.** A Lua callback per warrior per frame is fine. A Lua callback per object (warriors + buildings + mines + props ≈ several hundred) is also fine for the simple cases. A Lua callback per object **per pixel-y operation** (shadow caster eval, particle spawn) is not.

The line: if the C++ side iterates over an array of N objects and calls Lua N times, N>500 is the moment to start batching.

### 4.4 Batching

`mc2.object.status(id)` returning a single int has a ~80 ns Lua-call cost dominating the trivial C++ work. A batched form `mc2.object.status_many({id1, id2, ...}) -> {st1, st2, ...}` is one Lua call and fills a Lua table directly:

```cpp
static sol::table lua_status_many(sol::this_state ts, sol::table ids) {
    sol::state_view L(ts);
    sol::table out = L.create_table();
    int n = (int)ids.size();
    for (int i = 1; i <= n; ++i) {
        long id = ids[i].get<long>();
        ABLi_pushInteger((int)id);
        execObjectStatus();
        out[i] = ABLi_popInteger();
    }
    return out;
}
```

For 100 ids, this collapses 100×80 ns (8 µs) into one call + 100 cheap loop iterations (~3 µs total). Worth adding for any binding modders are likely to call in a tight loop. Candidates for batched variants: `object.status_many`, `object.exists_many`, `in_area_many`. Defer until a mod actually wants them.

### 4.5 String allocation in hot paths

`std::string` arguments allocate on the C++ side (sol2 marshals via `std::string` by default). Avoid in hot paths. For named timers / named events, prefer **integer handles**:

- Bad: `mc2.timer.create_named("escape_warning", 5.0, fn)` — the name string allocates per call.
- Good: `local TIMER_ESCAPE_WARNING = 1; mc2.timer.create(TIMER_ESCAPE_WARNING, 5.0, fn)`.

Mod code should hoist string-keyed lookups to module-load time (mod-script equivalent of a constexpr table). Document this in the modding guide.

### 4.6 GC pacing

Lua's incremental GC defaults are fine for our workload. Forcing `lua_gc(L, LUA_GCSTOP, 0)` plus periodic `LUA_GCSTEP` calls during the per-frame `Tick()` would let us cap GC pause budget — overkill for M0. Revisit only if a profiling spike tracks back to GC.

---

## 5. Trace and debugging — `MC2_LUA_TRACE`

Per the worktree's debug-instrumentation rule (`memory/debug_instrumentation_rule.md`): env-gated, default-off, lifecycle/event-only — not per-frame.

```cpp
// modding/lua_trace.h
#pragma once
#include <stdio.h>
#include <stdlib.h>
namespace mc2lua {
inline bool TraceEnabled() {
    static const bool s = (getenv("MC2_LUA_TRACE") != nullptr);
    return s;
}
}
#define MC2_LUA_TRACE(fmt, ...) \
    do { if (::mc2lua::TraceEnabled()) { \
        printf("[LUA_TRACE v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); \
    } } while (0)
```

Convention enforced by reference-binding example:

```
[LUA_TRACE v1] damage_object call target=123 attacker=45 dmg=5.0 -> 1
[LUA_TRACE v1] damage_object reject reason=bad_target id=99999
[LUA_TRACE v1] timer.create id=7 secs=5.00
[LUA_TRACE v1] timer.fire id=7 status=ok
[LUA_TRACE v1] timer.fire id=7 status=error msg="attempt to index nil value 'foo'"
```

Key properties:
1. **Trampoline-level placement.** Macro lives at the bottom of every binding lambda — hits automatically for all 10 bindings without per-binding boilerplate beyond the one call.
2. **Default off.** `MC2_LUA_TRACE=1` opt-in, mirrors `MC2_TGL_POOL_TRACE`, `MC2_DESTROY_TRACE`, `MC2_ASSET_SCALE_TRACE` from the worktree CLAUDE.md instrumentation list. Add `MC2_LUA_TRACE` to the startup `[INSTR v1] enabled:` banner emitted in main.cpp.
3. **Schema-versioned.** The `[LUA_TRACE v1]` prefix matches the `\[SUBSYS v[0-9]+\]` grep convention; if we later restructure the format we bump to v2.
4. **Grep-friendly.** Single-line, key=value-ish.

Independently of trace, **errors are always logged**. `[LUA ERROR]` lines from `protected_function` failures fire even with trace off — they're rare and load-bearing.

---

## 6. Test mission checklist (~20 items)

Comprehensive starter mission `mods/test/scripts/missions/demo.lua` plus a `mods/test/scripts/data.lua` and a small set of error-injection mods. The smoke test runs the mission for 30s and grepps logs for these markers.

| # | Check | What it proves |
|---|-------|----------------|
| 1 | `[LUA v1] event=init status=ok` appears once at mission start | VM init wiring (Implementation-Shape §4) hits |
| 2 | `[LUA v1] event=close status=ok` appears once at mission end | VM teardown wiring hits |
| 3 | data.lua line `mc2.data.mech.MyBushwacker = {...}` runs without error | Data-stage API view exposes `mc2.data` |
| 4 | data.lua attempting `mc2.spawn_mech(...)` errors with "unavailable in data stage" | Two-stage gate (Impl-Shape §7) is enforced |
| 5 | control.lua reads back `mc2.data.mech.MyBushwacker` and logs a field | Data table survives stage transition (frozen, readable) |
| 6 | control.lua attempting `mc2.data.mech.New = {...}` errors with "data stage frozen" | Freeze metatable installed |
| 7 | `[LUA INFO] demo.lua loaded — control stage` appears at startup | Demo file loaded successfully |
| 8 | `mc2.spawn_mech(0x100, 100, 100, 0)` returns a non-zero id | Spawn succeeds, prototype lookup works |
| 9 | The spawned mech is visible in-engine (mover count +1 after 1s) | Spawn actually instantiates a `Mover`, not just allocates an id |
| 10 | `mc2.timer.create(0.2, fn)` — fn runs ≤300ms later | Timer dispatch via `Tick()` works |
| 11 | Timer callback executes `mc2.audio.play_sound(...)` and the sound plays | Callback can re-enter bindings |
| 12 | `mc2.spawn_mech(99999, 0, 0, 0)` returns 0, no crash, `[LUA_TRACE]` logs `no_prototype` | Missing-prototype graceful failure |
| 13 | `mc2.damage_object("not a number", ...)` raises a Lua error caught by `pcall`, mission continues | Type-check tier-1 reaches the modder |
| 14 | A callback that does `error("boom")` logs `[LUA ERROR] timer ... boom`, mission continues | `protected_function` catches |
| 15 | A callback with `while true do end` is interrupted by an instruction-count hook (`debug.sethook` set up in `LuaVM::Init` with `LUA_MASKCOUNT, 1<<20`) | Infinite-loop containment (M1 if not in M0) |
| 16 | Saving the game inside `demo.lua`, exiting, reloading: the persistent table survives | Save/load Lua state (M2; punt with explicit "not yet" check in M0) |
| 17 | `mc2.object.status(spawnedId)` returns `OBJECT_STATUS_NORMAL` (≡ 0) for a fresh mech | Status binding's stack push/pop matches `execObjectStatus` |
| 18 | A second mod whose `data.lua` calls `error(...)` does not prevent first mod loading | Per-mod isolation (caught by surrounding `pcall`) |
| 19 | `MC2_LUA_TRACE=1` produces ≥1 line per binding called, none with trace off | Trace gating works |
| 20 | Tier-1 smoke (`run_smoke.py --tier tier1`) passes with `mods/test` enabled and disabled | `.abx` path untouched per `stock_install_must_remain_playable.md` |

Items 15-16 are M1+M2; the M0 commit only needs 1-14 + 17-20. Item 18 specifically requires the mod-loop in `initLuaVM()` to wrap each mod's load in `pcall`.

---

## 7. Documentation auto-generation pattern

Each binding registers metadata alongside its function. Doc generator walks the registry table and emits markdown — never hand-edit `docs/lua-api.md`.

```cpp
// modding/lua_binding_registry.h
struct Param { const char* name; const char* type; const char* doc; };
struct BindingMeta {
    const char* lua_path;        // "mc2.damage_object"
    const char* cpp_target;      // "execDamageObject"
    std::vector<Param> params;
    Param returns;
    const char* tier;            // "STABLE" | "EXPERIMENTAL" | "DEPRECATED"
    int api_version_since;
    const char* docstring;
};
class BindingRegistry {
public:
    static BindingRegistry& Instance();
    void Add(BindingMeta m);
    void DumpMarkdown(FILE* out) const;
    const std::vector<BindingMeta>& All() const;
};
```

Registration site in `lua_bindings_mc2.cpp`:

```cpp
#define MC2_LUA_REG(meta, fn) \
    do { BindingRegistry::Instance().Add(meta); \
         mc2.set_function(meta.lua_path + 4 /* skip "mc2." */, fn); } while(0)

MC2_LUA_REG(BindingMeta{
    "mc2.damage_object", "execDamageObject",
    {
        {"targetId",    "integer", "Valid object handle"},
        {"attackerId",  "integer", "Valid object handle for the damage source"},
        {"weaponId",    "integer", "Master component id of the weapon"},
        {"damage",      "number",  "Damage points (0..100000)"},
        {"hitLocation", "integer", "Body location enum"},
        {"hitRoll",     "number",  "Pre-rolled hit value 0..1"},
        {"entryAngle",  "number",  "Radians"},
    },
    {"result", "integer", "1=hit, -1=bad target, -2=bad attacker, -3=damage out of range"},
    "STABLE", /*api_version_since*/ 1,
    "Apply damage to an object as if from a weapon shot. "
    "Used for scripted destruction triggers; ABL counterpart: damageobject."
}, &lua_damage_object);
```

Toolchain: `tools/lua_api_doc_gen.cpp` is a tiny CLI that links against `mc2_modding.lib`, calls `BindingRegistry::Instance().DumpMarkdown(stdout)`, and exits. Build target `lua_api_doc_gen` runs in CI or by hand. Output goes to `docs/lua-api.md`. The docs/spec README points users at the generated file; the file's first line is `<!-- AUTO-GENERATED — do not edit. Regenerate with: ./lua_api_doc_gen > docs/lua-api.md -->`.

Doc generator output shape (markdown):

```markdown
## mc2.damage_object  (STABLE, since API v1)

C++ target: `execDamageObject` (code/ablmc2.cpp:1926).

Apply damage to an object as if from a weapon shot...

| Param | Type | Description |
|-------|------|-------------|
| targetId | integer | Valid object handle |
| ... | ... | ... |

**Returns:** integer — 1=hit, -1=bad target, -2=bad attacker, -3=damage out of range
```

Smoke test: a CI step regenerates `docs/lua-api.md`, `git diff --exit-code` to fail if a binding was added without committing the doc update. Same pattern as `scripts/check-claude-md-pointer.sh`.

---

## 8. Open questions

1. **`error(..., level)` line attribution in nested `require`s.** A binding-side `error('mc2.damage_object: targetId must be integer', 2)` reports the *caller's* line. If the caller is itself a thin Lua wrapper (as is likely once mods grow), the user sees the wrapper line, not their code. Consider a deeper `error(..., 3)` heuristic, or a `traceback()`-augmented error that prints the full Lua chain. M1 polish.

2. **`ABLi_push*` reentrancy during a Lua callback fired from inside an ABL execute.** The ABL stack is currently single-threaded (no concurrency); is it re-entrant? If `brain->execute()` calls back into Lua via an event hook, and Lua then calls `mc2.damage_object`, our trampoline pushes 7 args onto the ABL stack. Does the in-flight ABL execution see those? Worth a careful read of `mclib/ablxstd.cpp` before C-3. If the stack is shared, we may need a scratch stack for Lua-originated calls.

3. **Prototype-lookup C-side surface.** `mc2.spawn_mech` needs `ObjectManager_PrototypeExists` and `ObjectManager_CreateMech` shims that don't yet exist. They live in `code/objmgr.cpp` and need to be added in the binding commit. Track for C-3.

4. **Instruction-count hook performance.** `debug.sethook(L, hook, "", 1<<20)` to catch infinite loops costs a hook-fire per million instructions. Probably fine, but measure on Carver5O before committing — if the call rate is in the millions/frame the hook overhead matters.

5. **`sol::function` lifetime across save/load.** Save serializes mod state (a Lua table) to disk, load deserializes. Lua *functions* in the timer table are not serializable — on load, the timer table must be reconstructed from a "deferred event" list expressed in mod data, not in C++-held `sol::function`s. Needs design pass at M2.

6. **Per-mod sandbox isolation.** Two mods loaded in the same `sol::state` share `_G` despite `sol::environment`. If mod A overrides `string.format`, mod B sees the override. Fix: each mod gets its own `sol::environment` carrying its own `string`/`table`/`math` table copies. Costs ~16 KB per mod; affordable, do in M1.

7. **Hot-reload of `control.lua`.** Implementation-Shape says it's a goal; mechanism not specified. Tentative: paused-mission-only, drop the env's user-defined functions, re-execute `control.lua`. Anything bound to a still-active timer keeps running on the old function reference (which is fine — `sol::function` is copy-on-store). Open whether timers are reset at hot-reload or preserved.

8. **Sol2 exception model.** Sol2 throws on some unexpected paths even with `SOL_SAFE_*`. We need a top-level `try/catch(sol::error&)` around every binding entry, or compile with `-fno-exceptions` and rely on `sol::optional`. The codebase doesn't use exceptions today; check whether Sol2 amalgamation works with `-fno-exceptions`. If not, accept Sol2 as the only exception-using subsystem and keep `try/catch` at the binding boundary.

---

## 9. References

Source (verified during this session):
- `code/ablmc2.cpp:422`  — `execGetTime`.
- `code/ablmc2.cpp:1926` — `execDamageObject` (full `ABLi_pop*` pattern reference).
- `code/ablmc2.cpp:2208,2232,2262` — `execObjectCreate/Exists/Status`.
- `code/ablmc2.cpp:2444,2468` — `execSetTimer/CheckTimer`.
- `code/ablmc2.cpp:2546,2562` — `execSetObjectiveStatus/CheckObjectiveStatus`.
- `code/ablmc2.cpp:2640,2657` — `execPlaySoundEffect/PlayVideo`.
- `code/ablmc2.cpp:2922` — `execInArea`.
- `code/ablmc2.cpp:3404,3423` — `execGetGlobalValue/SetGlobalValue`.
- `code/ablmc2.cpp:7792,7842,7848-7892` — `ABLi_addFunction` registrations (signature strings).
- `mclib/abl.h:123-148` — `ABLi_push*` / `ABLi_pop*` / `ABLi_peek*` / `ABLi_poke*` declarations.
- `code/warrior.cpp:2160` — per-warrior `brain->execute()` site (Lua-callback cadence reference).

Predecessor design docs:
- `docs/superpowers/explorations/2026-04-29-track-c-lua-scripting-status.md`.
- `docs/superpowers/explorations/2026-04-30-track-c-lua-implementation-shape.md` (this doc supersedes its §5 sketches re: ABL exec calling convention).
- `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` §6.

Memory:
- `memory/debug_instrumentation_rule.md` — env-gated trace convention used in §5.
- `memory/stock_install_must_remain_playable.md` — checklist item 20 enforces.
- `memory/carver5_mission_playable.md` — `MAX_STANDARD_FUNCTIONS 512` (Lua VM doesn't add to that count; orthogonal).
