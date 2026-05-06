# Track C — ABL → Lua Reverse Direction

Date: 2026-04-30
Status: Design / exploration. No code yet.
Companion docs:
- `2026-04-30-track-c-lua-implementation-shape.md`
- `2026-04-30-track-c-blocking-questions-resolution.md` (the `*_impl` extraction pattern)
- `2026-04-30-track-c-lua-api-surface-catalog.md`
- `2026-04-30-track-c-lua-loading-lifecycle.md`

---

## TL;DR

The forward direction (Lua → engine) had to invent the `*_impl` C-extraction pattern because Lua-initiated calls cannot safely reenter ABL's `execute()` stack. The **reverse direction (ABL → Lua) is the natural shape**: ABL is already mid-execution, and a normal `execXxx`-style ABL extension primitive can pop args off the ABL stack, hand them to a Lua dispatch table, run the Lua handler synchronously, and push the return value back. This unlocks "mods on top of stock content": a modder can hook stock `corebrain.abx` decision points (or engine-emitted state events) without rewriting any mission script.

---

## 1. The Contract

### ABL side — one new primitive

Registered exactly like every other extension in `code/ablmc2.cpp` (~line 6693+ pattern, via `ABLi_addFunction`):

```cpp
// in InitABLEnvironment() / extension registration block:
ABLi_addFunction("mc2luadispatch", false, "c"           // key:charptr
                                          ,  "i",        // returns int
                                  execMC2LuaDispatch);
```

The signature spec uses ABL's existing param-list grammar (`i`/`r`/`c` for int/real/charptr). Because ABL extensions are fixed-arity, M0 ships **typed overloads** rather than varargs:

```cpp
ABLi_addFunction("mc2luadispatch",   false, "c",      "i", execMC2LuaDispatch0);
ABLi_addFunction("mc2luadispatch_i", false, "ci",     "i", execMC2LuaDispatch1i);
ABLi_addFunction("mc2luadispatch_ii",false, "cii",    "i", execMC2LuaDispatch2i);
ABLi_addFunction("mc2luadispatch_r", false, "cr",     "i", execMC2LuaDispatch1r);
ABLi_addFunction("mc2luadispatch_ir",false, "cir",    "i", execMC2LuaDispatch2ir);
ABLi_addFunction("mc2luadispatch_c", false, "cc",     "i", execMC2LuaDispatch1c);
```

Six variants cover ~95% of expected hooks (key + 0-2 typed args). M1 can add 3-arg variants. Each variant is two lines:

```cpp
void execMC2LuaDispatch2i (void) {
    // ABL stack is intact — we are inside an active brain->execute() frame.
    long arg2 = ABLi_popInteger();
    long arg1 = ABLi_popInteger();
    char* key = ABLi_popCharPtr();

    int result = 0;
    if (g_LuaVM)
        result = g_LuaVM->Dispatch(key, arg1, arg2);
    ABLi_pushInteger(result);
}
```

### Lua side — registration

```lua
-- in a mod's control.lua, run during Lua VM init (see loading-lifecycle doc):
mc2.events.on("MyMod.OnEnemyKilled", function(warriorId, killerTeam)
    log("enemy " .. warriorId .. " killed by team " .. killerTeam)
    return 1   -- non-zero short-circuits and is what ABL receives
end)
```

### ABL caller (mod-supplied mission script)

```
result = mc2luadispatch_ii("MyMod.OnEnemyKilled", warriorId, killerTeam);
if (result <> 0)
    -- mod handled it
endif;
```

The contract:
- **Key**: charptr (Lua string). ABL `mc2luadispatch_*` primitives never inspect it; the C++ side does the table lookup.
- **Args**: typed by primitive variant. `int → lua_Integer`, `real → lua_Number`, `charptr → const char*` (sol2 maps to Lua string).
- **Return**: int. If no handler: 0. If handler returns nil: 0. If handler returns boolean: 0/1. If handler returns number: truncated to int (ABL has no concept of double-precision return). If handler errors: 0 (caught by `pcall`).
- **Side effects**: handler may call any forward-direction `*_impl` binding (those are pure C, no ABL reentry). Handler may NOT call anything that re-enters ABL execution (see Reentrancy Guard).

---

## 2. Why This Is Now Clean

The blocking-questions doc resolved the forward-direction reentrancy problem by extracting `*_impl` C functions: when Lua calls into the engine, sol2 lands in `engineThing_impl(...)` which does the real work without ever touching the ABL stack or the `CurWarrior`/`CurFSM` globals that ABL execution relies on.

The reverse direction is the **symmetric, easier case**:

1. ABL is in mid-execution. `brain->execute()` is on the C stack. `CurWarrior`, `CurFSM`, the ABL operand stack, `CurModule` — all globals are valid.
2. ABL hits the `mc2luadispatch_ii` opcode. It's just another `StandardFunctionInfo` callback in `FunctionCallbackTable[]`, dispatched by `ablxstd.cpp:993` exactly like `objectStatus` or `getContactId`.
3. Inside `execMC2LuaDispatch2i`, the ABL stack is the only authority. `ABLi_popInteger`/`ABLi_popCharPtr` pull args in reverse push order — the **correct mechanism** that the forward-direction code couldn't use because there was no live ABL frame.
4. We call `g_LuaVM->Dispatch(key, arg1, arg2)`. sol2 invokes the Lua handler synchronously on its own VM stack. ABL stack is untouched.
5. Handler returns. We `ABLi_pushInteger(result)`. ABL's `executeChild` sees a normal int on its operand stack and resumes.

The `*_impl` pattern from forward-direction was a workaround. The reverse direction is the **shape ABL extensions were designed for** — `objectStatus`, `getContacts`, `damage` all follow this exact pattern. We're just adding one more, parameterized by a string key that vectors to Lua instead of into engine code.

---

## 3. Reentrancy Guard

The cardinal rule: **during a Lua dispatch handler, do not re-enter ABL execution.** ABL is not reentrant on a single warrior's brain (the per-warrior VM state isn't push/pop-safe), and the operand stack is shared across the call chain.

### Mechanism

```cpp
class LuaVM {
    bool in_abl_dispatch_ = false;

    int Dispatch(const char* key, /* args... */) {
        bool prev = in_abl_dispatch_;
        in_abl_dispatch_ = true;
        int result = 0;
        try {
            auto it = event_handlers_.find(key);
            if (it == event_handlers_.end()) return 0;
            for (auto& fn : it->second) {
                sol::protected_function_result r = fn(/* args... */);
                if (!r.valid()) {
                    sol::error e = r;
                    LOG_LUA_ERROR("dispatch handler '%s': %s", key, e.what());
                    continue;
                }
                int v = r.get_or<int>(0);
                if (v != 0) { result = v; break; }   // first non-zero short-circuits
            }
        } catch (...) { /* swallow, ABL must not see C++ exceptions */ }
        in_abl_dispatch_ = prev;
        return result;
    }
};
```

### What forward bindings must check

The vast majority of M0 `*_impl` bindings are pure read-side — `getWarriorPosition_impl`, `getMissionTime_impl`, `log_impl`. None of them re-enter ABL. They're allowed during dispatch.

A small set of bindings *would* re-enter ABL if naively implemented:
- Anything that calls `brain->execute()` directly (none in M0, but plausible in future).
- Any mission-control hook that triggers a state transition mid-tick (`gotoState`, `execScenario`).

Those bindings — when added — must `if (in_abl_dispatch_) { lua_error("not allowed during ABL dispatch"); }`. M0 has none. M1 will likely add 1-2; we'll annotate them at definition time.

Stage-gate: a unit test that registers a Lua handler which calls every forward binding and asserts each one either returns successfully or errors-cleanly with the reentrancy message. No segfaults, no ABL stack corruption.

---

## 4. Event Registration Mechanism

### Lua API

```lua
mc2.events.on("EventName", handler)         -- register; returns a token
mc2.events.off("EventName", handler)        -- deregister specific handler
mc2.events.off("EventName")                 -- deregister all for that key
mc2.events.emit("EventName", ...)           -- mod→mod synchronous call
mc2.events.list()                           -- debug: returns table of keys → count
```

### C++ registry shape

```cpp
class LuaVM {
    std::unordered_map<std::string, std::vector<sol::protected_function>>
        event_handlers_;

    void RegisterHandler(const std::string& key, sol::protected_function fn) {
        event_handlers_[key].push_back(std::move(fn));
    }
    void ClearAllHandlers() { event_handlers_.clear(); }
    void ClearHandlersFor(const std::string& key) { event_handlers_.erase(key); }
};
```

- **Multiple handlers per event**: yes. Mods can stack hooks (e.g., two mods both watching `WarriorNearDeath`).
- **Order**: registration order. Stable.
- **Short-circuit semantics**: first handler returning a non-zero/non-nil number wins. Subsequent handlers are skipped. This mirrors the "did anyone handle this?" pattern from event systems (Win32 `WM_*`, browser `event.preventDefault()`).
- **Mod-to-mod**: `mc2.events.emit` is the same dispatch path; mod B's handler runs synchronously inside mod A's `emit` call. No queuing, no deferred dispatch in M0 (avoids a whole frame-ordering problem).

### Built-in events vs custom

Two namespaces by convention:
- `corebrain.*`, `mission.*`, `engine.*` — fired by stock content / engine. Documented contract.
- `<ModId>.*` — mod-defined. Mods own the contract.

No enforcement; just a doc convention.

---

## 5. Use Cases

### 5a. Stock corebrain extension — "second wind"

Stock `corebrain.abx` is patched (Option A in §6) so that when a warrior's HP drops below 25%, it emits:

```
secondWind = mc2luadispatch_i("corebrain.WarriorNearDeath", getId());
if (secondWind <> 0)
    -- mod healed me; reset combat state
    setRealMemory(MEM_LOWHP_REACTED, 0);
endif;
```

A mod hooks it:

```lua
mc2.events.on("corebrain.WarriorNearDeath", function(warriorId)
    if state.second_wind_used[warriorId] then return 0 end
    state.second_wind_used[warriorId] = true
    mc2.warrior.heal_impl(warriorId, 50)        -- forward binding
    mc2.fx.spawn_impl("HealAura", warriorId)
    return 1
end)
```

Stock missions become extensible without a per-mission rewrite.

### 5b. Cross-mod communication — market price ticker

Mod A maintains an in-game economy. When a price changes:

```lua
mc2.events.emit("ModA.MarketPriceChanged", "AC20_AMMO", new_price)
```

Mod B (a UI overlay) hooks it:

```lua
mc2.events.on("ModA.MarketPriceChanged", function(item, price)
    ui.refresh_price_panel(item, price)
    return 0   -- don't short-circuit; other listeners may want it too
end)
```

No coupling between the two mods at load time. They negotiate via the event key string.

### 5c. Generic mission hook — objective complete

If a mission (stock or modded) is patched (or natively written) to emit:

```
mc2luadispatch_i("Mission.ObjectiveComplete", objectiveIndex);
```

Multiple mods can react: an achievements mod, a stats tracker, a UI splash, a meta-campaign progression mod.

---

## 6. Stock Mission Compatibility — Option A vs Option B

The **magic-ABL-contamination memory rule** (load-bearing) forbids shipping modified `.abx` files in the stock distribution path. v0.2 hotfix shipped a 42786-byte `corebrain.abx` (vs stock 42206) and missions hung on enemy activation. So how does stock corebrain emit events without being modified?

### Option A — patch + override stock corebrain (data/abl/corebrain.abx)

- We DO patch stock `corebrain.abx` to insert `mc2luadispatch_*` calls at well-known decision points.
- Ship the patched binary as `data/abl/corebrain.abx`. MC2's loose-file override (memory: file override system, `data/` overrides FST) picks it up; the original FST stays untouched.
- The patched corebrain is **versioned in source control** (we maintain the `.abl` source, recompile on every release, diff against stock to confirm no behavioral drift in non-emitting paths).
- The contamination rule still applies: never ship the patched corebrain in the stock-only release branch. It rides only with the mod-loader package.
- Risk: if our patch introduces a behavioral change beyond just emitting events (e.g., a new local var collides with stock state), stock missions break. Mitigation: the patch is event-emission only, no state mutation, and we run the full Carver5 + mc2_01 + Wolfman regression matrix on the patched corebrain before each release.

### Option B — engine-side hooks (preferred for engine state)

For events that come from **engine state** (warrior took damage, AI reached waypoint, building destroyed, mission timer crossed threshold), don't go through ABL bytecode at all. Fire the event directly from C++:

```cpp
// in code/warrior.cpp damage handler:
if (g_LuaVM && hp_before > threshold && hp_after <= threshold) {
    g_LuaVM->Dispatch("warrior.HpThresholdCrossed", getId(), threshold);
}
```

- Pros: no `.abx` patching, no contamination risk, works for all missions (stock and modded).
- Pros: handler runs *outside* an ABL frame entirely — `in_abl_dispatch_` stays false, so even reentrant forward bindings would be safe (though we don't promise that to mod authors).
- Cons: the call site is hardcoded in our engine; modders can't add new emission points without us shipping a new build.

### Recommendation

- **Option B for engine-state events** (HP thresholds, kills, damage taken, building captured, mission start/end, objective completion if engine-tracked, AI alarm fired). Most expansion-style hooks fit here.
- **Option A only for ABL-decision-tree events** that can't be reconstructed from engine state alone (e.g., "corebrain decided to retreat" — that decision lives in `.abl` source and isn't visible from C++). Each Option A patch is documented, versioned, and regression-tested.

M0 ships **only Option B** (3-5 well-chosen engine hooks). Option A waits until M1 after we have a working patch-build pipeline for the corebrain `.abl` source.

---

## 7. Performance

Worst-case engine-emitted events at saturation: 30Hz tick × 40 warriors × ~5 events/warrior/tick ≈ **6000 events/sec**.

Per-event cost breakdown (back-of-envelope, sol2 numbers from prior measurements in the API-surface doc):
- Dispatch table lookup (`unordered_map<string,vector>::find`): ~50ns
- sol2 protected_function call (no args): ~100ns
- Per-arg push (int): ~20ns
- Lua interpreter overhead for an empty handler: ~200ns
- Total minimum: ~400-500ns per event with a registered handler

At 6000 events/sec: ~3ms/sec of overhead = **0.3% of a 30Hz tick**. Acceptable.

But the **dominant cost is when there's NO listener** and we still construct args. Mitigation:

```cpp
// fast path
if (!g_LuaVM || !g_LuaVM->HasHandlersFor("warrior.HpThresholdCrossed")) return;
g_LuaVM->Dispatch("warrior.HpThresholdCrossed", id, threshold);
```

`HasHandlersFor` is a single hashmap probe (~30ns). Engine-side emission sites all use this guard. Result: zero-listener cost is ~30ns per emission point per event.

For ABL-side emission via `mc2luadispatch_*`: the ABL primitive cost (~1µs per call, dominated by ABL stack ops) dwarfs the dispatch lookup, so no fast-path needed there.

Budget: 1ms/frame for engine-emitted events. Above that, profile and prune emission sites.

---

## 8. Lifecycle Interaction

(Cross-ref: `2026-04-30-track-c-lua-loading-lifecycle.md`.)

- **Registration**: handlers registered during `control.lua` execution at mission start.
- **Survival**: handlers live for the entire mission. No GC pressure issues (sol2 holds strong refs in `event_handlers_`).
- **Mid-mission registration**: legal. A handler can call `mc2.events.on(...)` to register a new handler. The new handler is visible on the *next* dispatch (we don't mutate the vector mid-iteration; iterate a copy of the vector ref).
- **Mid-mission deregistration**: legal but careful. `mc2.events.off` while iterating the same key's vector: we iterate by index against a captured `size()` snapshot, so deregistration during dispatch only affects future dispatches.
- **Teardown**: on mission end / `closeABL` / Lua VM destruction, `event_handlers_.clear()` runs. sol2 drops all `protected_function` refs. No leaks.
- **Save/load**: handlers are NOT serialized. On load, `control.lua` re-runs (it's part of the mod's init contract) and re-registers. Mod authors must keep `mc2.events.on` calls **at file scope** in `control.lua`, not gated behind state.

---

## 9. Hot-Reload Interaction

When `control.lua` hot-reloads (dev workflow: edit script, press hotkey, see changes without restarting the mission):

1. **Clear stale handlers**: `g_LuaVM->ClearAllEventHandlers()` runs *before* the new script executes. Otherwise old handlers from the previous version stack on top of new ones, and you get double-fires.
2. **Re-execute control.lua**: the file scope `mc2.events.on(...)` calls re-register fresh handlers.
3. **State preservation**: any `state` table at module scope is implicitly preserved if we re-run the script in the same Lua state, *unless* the script itself wipes it. Convention: mods do `state = state or {}` at top of `control.lua` so reload is idempotent.

API:
```cpp
void LuaVM::HotReloadControlScript(const char* path) {
    ClearAllEventHandlers();
    sol::protected_function_result r = lua_state_.script_file(path);
    if (!r.valid()) { /* log */ }
}
```

Hotkey: TBD (one of the unbound debug slots; not Alt+F4).

---

## 10. Test Cases

A starter mission ships with M0 to prove this end-to-end. The mission's `mission.abl` and a sidecar `control.lua` together exercise:

1. **Round-trip**: handler returns 42; ABL receives 42. Assert with `mc2luadispatch_i("Test.Echo", 42)` → expect 42.
2. **No handler returns 0**: `mc2luadispatch("Test.NotRegistered")` → expect 0.
3. **Stacked handlers, short-circuit**: register two handlers for `"Test.Stack"`. First returns 0, second returns 7. ABL gets 7. Both fired. Reorder: first returns 5, second returns 99 → ABL gets 5, second never runs.
4. **Handler error caught**: handler does `error("boom")`. `pcall` catches. ABL gets 0. Log shows the traceback. ABL execution continues normally on the *next* tick.
5. **Reentrancy rejected**: handler tries to call a (synthetic, M0-test-only) forward binding flagged `requires_no_dispatch`. Lua errors cleanly. Dispatch returns 0.
6. **Hot-reload clears**: register handler v1 returning 1. Edit `control.lua` so handler v2 returns 2. Hot-reload. ABL now sees 2, never 3 (would mean both fired). Test passes only if `ClearAllEventHandlers` ran.
7. **Engine-emitted event reaches Lua**: damage a warrior past 25% HP. Handler logs the event. Test scaffolding asserts the log line appeared.
8. **Cross-mod emit**: Lua A calls `mc2.events.emit("A.Ping")`. Lua B's handler increments a counter. Counter == 1 after one emit.
9. **Missing handler signature mismatch**: ABL calls `mc2luadispatch_ii(...)` but handler only takes one arg. Lua silently drops the extra (sol2 default). No crash. (M1: consider stricter mode.)
10. **Performance smoke**: 1000 emissions/frame for 60 frames against a no-op handler. Frame time stays under budget.

These are integration tests run by the mission-script harness, not unit tests on `LuaVM` in isolation.

---

## 11. Open Questions

1. **String interning for keys**: should the C++ side `std::string` keys be replaced by interned IDs to avoid hash+memcmp on hot dispatch? Probably not for M0 (lookup is already <100ns), revisit if profiling shows hash dominating.
2. **Async / queued dispatch**: should `mc2.events.emit` have a "post" variant that defers to the next tick? Simplifies cycle avoidance but adds a frame-ordering contract. M0 says no; M1 maybe.
3. **Typed return convention**: should handlers be able to return `real` to ABL? Adds `mc2luadispatch_*_r` variants that `pushReal` instead of `pushInteger`. Defer to M1 if no real demand.
4. **Multiple return values**: ABL extensions can write into out-params (`Iii` syntax — pointer to int array). Could a Lua handler populate an out-array? Probably not worth it; mod authors can store state in Lua and read it back via separate forward bindings.
5. **Wildcards / namespacing**: `mc2.events.on("corebrain.*", handler)` to subscribe to a whole namespace. Cute, adds matcher complexity. Defer.
6. **Per-warrior context**: do we automatically pass `CurWarrior->getId()` as an implicit first arg when dispatching from inside a brain? Or do mod authors always pass `getId()` explicitly in the ABL emit? Explicit is clearer; recommend explicit. But this is the kind of API ergonomics call we should revisit after M0 mods exist.
7. **Option A patch tooling**: who maintains the corebrain `.abl` source patch? What's the build pipeline? (Not blocking M0 since M0 ships only Option B.)
8. **Handler priority / ordering control**: registration order is stable but invisible. If two mods both want to be "first," they fight. Add `priority` arg to `on()`? Defer until two real mods conflict in the wild.
9. **Diagnostic introspection**: should `mc2.events.list()` return per-key dispatch counts and total time? Useful for mod profiling. Cheap to add. M1.
10. **`emit` with a registered handler that errors** — does `emit`'s caller get 0, or get the value from the *next* handler past the erroring one? Current design: error → skip → continue iterating. Caller gets the first non-zero from the survivors. Document this.

---

## Files / Sites Touched (when implementation lands)

- `code/ablmc2.cpp`: add `execMC2LuaDispatch*` functions and their `ABLi_addFunction` registrations alongside the existing block ~line 6693+.
- `mclib/ablsymt.h`: `MAX_STANDARD_FUNCTIONS` already at 512 in some worktrees; verify nifty-mendeleev has the bump (carver5 stability session bumped 256→512). 6 new primitives won't push the limit.
- New: `GameOS/lua/lua_vm.h/cpp` — `Dispatch`, `RegisterHandler`, `ClearAllEventHandlers`, `in_abl_dispatch_`, `event_handlers_`. Forward-direction `*_impl` bindings already lived here per the implementation-shape doc.
- `code/warrior.cpp`: 1-2 Option B emit sites near damage handler / brain execute (~line 2155).
- `code/objmgr.cpp`: 1-2 Option B emit sites at game-state transitions (object destroyed, team eliminated).
- `code/mission.cpp` near line 2257: no change for M0 (we are not patching corebrain.abx in M0).

The next session should be able to start with this doc, the implementation-shape doc, and the loading-lifecycle doc, and produce a concrete M0 PR that adds `mc2luadispatch_*` primitives and 3-5 engine-side `Dispatch` call sites with zero ambiguity.
