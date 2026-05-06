# Track C — Blocking Architectural Questions Resolution

**Date:** 2026-04-30
**Mode:** Research / design only. No code in this commit.
**Predecessors (read in order):**
- [`2026-04-30-track-c-lua-api-surface-catalog.md`](2026-04-30-track-c-lua-api-surface-catalog.md) — STABLE/EXPERIMENTAL/INTERNAL tier list
- [`2026-04-30-track-c-lua-trampolines-and-tests.md`](2026-04-30-track-c-lua-trampolines-and-tests.md) — Open Q #2 (reentrancy)
- [`2026-04-30-track-c-lua-loading-lifecycle.md`](2026-04-30-track-c-lua-loading-lifecycle.md) — stage gates
- [`2026-04-30-track-c-lua-implementation-shape.md`](2026-04-30-track-c-lua-implementation-shape.md) — note that §5 is superseded by trampolines doc §0

This doc resolves the three blockers for C-3 (Sol2 + Lua wiring). The Q1 finding is more severe than the trampolines doc anticipated and forces a substantial revision of the C-3 calling convention. Q2 and Q3 lock in respectively the namespace tree and the corebrain shadow policy.

---

## Q1 — ABL stack reentrancy (CRITICAL)

### Finding (one line)

**The trampoline pattern in `2026-04-30-track-c-lua-trampolines-and-tests.md` §1 cannot work as written.** `ABLi_pop*` does NOT pop a pre-pushed value — it advances the in-flight ABL bytecode pointer (`codeSegmentPtr`) and evaluates the next ABL expression. Calling `execXxx()` from a non-ABL context with synthetic pushes onto the global stack will misexecute whatever bytecode `codeSegmentPtr` happens to point at.

### Evidence (file:line)

1. **Stack and `codeSegmentPtr` are global singletons.**
   - `mclib/ablxstd.cpp:38-40` declares `extern StackItem* stack; extern StackItemPtr tos; extern StackItemPtr stackFrameBasePtr;` — single global VM, no per-VM/per-frame separation.
   - `mclib/ablexec.cpp:40` defines `char* codeSegmentPtr = NULL;` — likewise global.
   - `mclib/ablrtn.cpp:817` (in `ABLi_execute`) initializes the stack: `stackFrameBasePtr = tos = (stack + eternalOffset);`. Every ABL execution shares this same buffer.

2. **`ABLi_popInteger` is bytecode-driven, not stack-pop.**
   `mclib/ablxstd.cpp:84-92`:
   ```cpp
   int ABLi_popInteger (void) {
       getCodeToken();
       execExpression();
       int val = tos->integer;
       pop();
       return(val);
   }
   ```
   The pattern `getCodeToken(); execExpression();` reads the next token from `codeSegmentPtr`, recursively evaluates the argument expression (which may be a literal, a variable, or an arbitrary subexpression), pushes its result, then pops. `ABLi_popReal`, `ABLi_popBoolean`, `ABLi_popCharPtr`, `ABLi_peek*`, and `ABLi_popAnything` (`ablxstd.cpp:96-232`) all share this shape.

3. **`ABLi_pushInteger` is a real pre-pushed-arg push.**
   `mclib/ablxstd.cpp:247-254`:
   ```cpp
   void ABLi_pushInteger (int value) {
       StackItemPtr valuePtr = ++tos;
       if (valuePtr >= &stack[MAXSIZE_STACK])
           runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
       valuePtr->integer = value;
   }
   ```
   These are used by exec functions to **return** values to the caller, e.g. `execGetTime` (`code/ablmc2.cpp:434`): `ABLi_pushReal(mission->actualTime);`.

4. **Exec functions interleave reads and writes against the bytecode-driven stream.**
   `code/ablmc2.cpp:2268` `execObjectStatus`: `long objectId = ABLi_popInteger();` … then at the end `ABLi_pushInteger(result);`. If we synthesize the stack via `ABLi_pushInteger(id)` then call `execObjectStatus()`, the function will discard our pushed value and instead try to evaluate the *next ABL expression at `codeSegmentPtr`* — which on a non-ABL caller is uninitialized or pointing at unrelated bytecode.

5. **Exec dispatch always resets `codeSegmentPtr` from the calling routine.**
   `mclib/ablexec.cpp:542` (in `execRoutineCall`): `codeSegmentPtr = routineIdPtr->defn.info.routine.codeSegment;`.
   `mclib/ablxstd.cpp:990-994` `execStandardRoutineCall`:
   ```cpp
   if (FunctionInfoTable[key].numParams > 0)
       getCodeToken();
   SkipOrder = skipOrder;
   if (FunctionCallbackTable[key])
       (*FunctionCallbackTable[key])();
   ```
   The dispatch site advances `codeSegmentPtr` past the function-call token, then invokes the callback. The callback then walks `codeSegmentPtr` further by calling `ABLi_pop*`. This requires `codeSegmentPtr` to be pointing at a real ABL argument list belonging to the active execution.

### Reentrancy semantics

- **Single global stack.** `stack[MAXSIZE_STACK]`, single `tos`, single `stackFrameBasePtr`. No save/restore in `execXxx` — they assume the caller's bytecode and stack frame are intact.
- **No save/restore on recursive ABL→native→ABL.** Native callbacks (`FunctionCallbackTable[key]`) run on the same stack, and they are expected not to corrupt it (each pops what it reads, pokes its return into the slot the dispatcher allocated).
- **No existing pattern calls `execXxx` from non-ABL context.** Searching the tree for callers of `execGetTime`, `execObjectStatus`, etc. outside `code/ablmc2.cpp` (where they are defined and registered) finds zero hits. They are exclusively dispatched through `execStandardRoutineCall` from inside `ABLi_execute`.
- **`ABLi_execute` re-initializes the stack** (`mclib/ablrtn.cpp:817`). A nested `ABLi_execute` would clobber the outer execution's `tos` — i.e. ABL itself is not designed to be called recursively from inside an in-flight `brain->execute()`.

### Concrete reentrancy scenario from C-3

`code/warrior.cpp:2155` calls `brain->execute()`. If a Lua event handler is registered for `on_warrior_tick` and fires inside that call (because some hook in the brain dispatch path drains queued Lua callbacks), and the handler calls `mc2.damage_object(...)` whose trampoline (per the trampolines doc §1) does:

```
ABLi_pushReal(entryAng); ABLi_pushReal(hitroll); ABLi_pushInteger(hitloc);
ABLi_pushReal(dmg); ABLi_pushInteger(weapon); ABLi_pushInteger(attacker);
ABLi_pushInteger(target);
execDamageObject();     // <-- broken
ABLi_popInteger();
```

Two failure modes occur:

- The seven `ABLi_pushInteger/Real` calls land 7 fresh slots above the in-flight ABL stack frame. The next time the in-flight bytecode interpreter pops a value (e.g. for the next ABL statement), it sees the topmost Lua-pushed value instead of what the bytecode expected. **Silent in-flight ABL stack corruption.**
- `execDamageObject` then reads `codeSegmentPtr` — which is positioned at *whatever ABL token the brain was on*, not at a damageobject argument list — and walks it as if it were an argument expression. **Either bogus argument values or a parser misstep that crashes inside `execExpression`.**

### The three options, evaluated

**(a) Forbid reentry — defer Lua handlers to a post-tick drain queue.**

Architecture: Lua handler invocations from inside `brain->execute()` are forbidden. Any event whose source is a bytecode interpreter (warrior tick, FSM transition, ABL `print`) enqueues onto a `std::vector<DeferredEvent>` owned by `LuaVM`. The drain happens at one well-defined site — the end of the per-frame `LuaVM::Tick()` (Implementation-Shape §4) — *after* the world tick but *before* render. Lua bindings that need ABL state read it via the trampoline at drain time, where there is no ABL execution in flight.

- Pros: tiny code surface; no ABL VM changes; stays inside the single-VM single-stack invariant; matches Factorio/Stellaris event-tick model that modders already understand.
- Cons: handlers see "almost-current" world state (one frame stale at most, since the tick→drain gap is sub-frame); the order of side effects is the drain order, not the trigger order, so a handler that wants to "interrupt" a brain decision cannot.
- Implementation cost: ~30 LoC for the queue plus the drain site. No ABL changes.

**(b) Stack save/restore around each Lua trampoline invocation.**

Architecture: trampoline saves `tos`, `stackFrameBasePtr`, and `codeSegmentPtr`, fakes a synthetic bytecode preamble in a private buffer that encodes the argument list as ABL expressions, points `codeSegmentPtr` at the buffer, calls `execXxx`, then restores all three. The synthetic bytecode encoding has to match the ABL token stream format — which is documented only by `mclib/ablexec.cpp` and `mclib/ablgen.cpp` (the code generator).

- Pros: bindings can fire from inside `brain->execute()` without deferral.
- Cons: requires reverse-engineering the ABL bytecode format for every supported argument shape (`i`, `r`, `b`, `c`, `*`, `?`, `R` reference parameters); the encoding is internal and subject to change between savefile-format versions; bytecode is `TokenCodeType` enum-byte-sequence with embedded `int`/`float`/symbol-pointer payloads (see `mclib/ablexec.cpp:282-362`). Each marshalled argument needs ~5-15 bytes of synthesized token stream. Very fragile, very high audit cost. Also: the in-flight `execExpression` may invoke `getSymTableNodePtr` against the active module's symbol table, which would not match a Lua-originated synthetic call.
- Implementation cost: easily 500+ LoC of fragile bytecode synthesis plus per-arg-shape tests. Future ABL bytecode changes silently break Lua bindings.

**(c) Separate call channel — bypass the ABL stack entirely; port each `execXxx` to a normal C arg list.**

Architecture: for each STABLE binding, write a sibling C function that takes its arguments as a normal C parameter list and returns its result by value. The ABL `execXxx` body is rewritten to pop its args, call the new C function, then push the result. Lua trampolines call the new C functions directly. Both paths converge.

- Pros: clean per-binding interface; no reentrancy concerns at all (no shared mutable state); permanent decoupling; allows future replacements of ABL itself (specs §5.4 mentions Lua-as-replacement aspirations).
- Cons: requires touching `code/ablmc2.cpp` for every binding we expose. ~85 STABLE bindings + ~65 EXPERIMENTAL = ~150 functions to wrap. Each is a pure refactor (extract pop-args at top, push-return at bottom, body becomes a parameter-list call), but it is a lot of mechanical churn. Some bindings (variadic `*`/`?`, ref-out `R`-array) are awkward to express as a normal C signature.
- Implementation cost: ~30 LoC per binding × 85 STABLE = ~2500 LoC, plus a similar amount for EXPERIMENTAL. Spread across many files but conceptually trivial.

### Recommendation: **(a) — forbid reentry, defer to a post-tick drain queue.**

Rationale, ordered:

1. **Smallest correct slice.** The C-3 commit ships only ~10 starter bindings (Implementation-Shape §5; trampolines §1). Option (b) is too fragile to ship in any commit. Option (c) is a multi-week refactor spanning the whole binding surface — way out of scope for the M0 slice. Option (a) lands in C-3 with one queue, one drain site, and the existing trampoline pattern *adjusted* (see "Implications" below) to use a Lua-owned scratch stack only at a known-safe site.

2. **The trampoline still cannot use the ABL stack.** Even with deferral, calling `execXxx()` from `LuaVM::Tick()` via push-then-call doesn't work because of the bytecode-driven `pop` (see "Evidence" #2). The drain site is a non-ABL context just like any other. **What deferral buys us is the right to *not* call `execXxx` at all** — at drain time we are between ABL ticks, so we can call a small set of *direct C entry points* we add ourselves. This means option (a) implies a partial option (c): a tiny C-side entry-point shim for each of the M0 ten bindings, not a wholesale port.

3. **Modder mental model is already deferred.** Other modding ecosystems with similar architectures (Factorio's `on_event`, Bannerlord's `MissionBehavior.OnTick`) all run handlers in a tick-end drain. Modders writing `mc2.on_event("warrior_tick", fn)` will not be surprised that `fn` sees the current frame's resolved state, not interrupting state.

4. **Path forward to (c) is preserved.** Adding direct-C shims for the 10 M0 bindings is the first step of (c). Subsequent commits can extend the shim coverage as bindings are promoted from EXPERIMENTAL → STABLE, and eventually the ABL `execXxx` bodies are rewritten to call through the same shims. (a)+partial-(c) is monotone toward a full (c) without ever shipping bytecode-synthesis code.

### Implications for C-3 (corrects the trampolines doc)

The corrected M0 trampoline shape, illustrated for `mc2.damage_object`:

1. `code/ablmc2.cpp` — extract the body of `execDamageObject` into a new C function `mc2_damage_object_impl(int target, int attacker, int weapon, float dmg, int hitloc, float hitroll, float entryAngle) -> int`. Rewrite the original `execDamageObject` to: pop args via `ABLi_pop*`, call `mc2_damage_object_impl(...)`, `ABLi_pokeInteger(result)`. Net behavior change: zero. ABL still works exactly as before.

2. `modding/lua_abl_shim.h` — declares the impls (not the `execXxx`). One line per binding.

3. `modding/lua_bindings_mc2.cpp` — Lua trampoline calls `mc2_damage_object_impl(...)` directly. No ABL stack push/pop, no `execXxx` call.

4. `modding/lua_vm.cpp` — `LuaVM::Tick(deltaSec)` is the only site that fires Lua callbacks. Brain-tick or other ABL-driven event sources enqueue `DeferredEvent{kind, params}`; `Tick` drains them in FIFO order *after* `objMgr->update()` returns. No Lua handler ever runs while `codeSegmentPtr` or `tos` is mid-flight.

5. Trampoline doc §0 ("ABL exec-function calling convention") and §1 ("Reference binding") need a follow-up note pointing here for the corrected pattern. The §1 example must replace `ABLi_pushReal/pushInteger/execDamageObject/popInteger` with the direct `mc2_damage_object_impl(...)` call.

6. The 10 M0 bindings need their `execXxx` body extracted into a `*_impl` C function. Cost: ~10 × ~30 LoC = ~300 LoC of pure refactor in `code/ablmc2.cpp`. No ABL semantics change; no new tests needed beyond what the bindings already require.

---

## Q2 — Namespace lock-in

### Method

Walk the STABLE list from the catalog doc §3, decide each binding's subsystem, prefer noun-grouping over verb-grouping (catalog §7 rule 1), accept symmetric `set_/_` pairs, and resolve close-call placements by listing both candidates and picking with rationale.

### Final namespace tree

```
mc2
├── log(msg)
│
├── time
│   ├── now()                          execGetTime
│   └── remaining()                    execGetTimeLeft
│
├── object
│   ├── exists(id)                     execObjectExists
│   ├── status(id)                     execObjectStatus
│   ├── position(id)                   execGetObjectPosition
│   ├── team(id)                       execObjectTeam (alias objectside)
│   ├── commander(id)                  execObjectCommander
│   ├── class(id)                      execObjectClass
│   ├── type_id(id)                    execObjectTypeID
│   ├── visible(viewer, target)        execObjectVisible
│   ├── distance_to(a, b)              execDistanceToObject
│   ├── distance_to_position(id, pos)  execDistanceToPosition
│   ├── is_dead_or_fled(id)            execIsDeadOrFled
│   ├── armor_pts(id)                  execGetArmorPts
│   ├── max_armor(id)                  execGetMaxArmor
│   ├── damage_state(id)               execGetObjectDamage
│   ├── damage_pts(id)                 execGetObjectDmgPts
│   ├── max_damage(id)                 execGetObjectMaxDmg
│   ├── set_damage(id, dmg)            execSetObjectDamage
│   ├── apply_damage(id, dmg, src,
│   │                hitloc, dtype,
│   │                dirX, dirY)        execDamageObject  *renamed*
│   ├── spawn(typeId, pos, side)       execObjectCreate
│   ├── suicide(id)                    execObjectSuicide
│   ├── remove(id)                     execObjectRemove
│   ├── change_sides(id, side)         execObjectChangeSides
│   ├── set_active(id, b)              execSetObjectActive
│   ├── is_active(id)                  execGetObjectActive
│   ├── teleport(id, pos)              execTeleportToPoint
│   ├── is_in_area(id, pos, r, areaId) execInArea
│   └── is_off_map(pos)                execIsOffMap
│
├── objective
│   ├── set_status(id, status)         execSetObjectiveStatus
│   ├── status(id)                     execCheckObjectiveStatus
│   ├── set_type(id, type)             execSetObjectiveType
│   ├── type(id)                       execCheckObjectiveType
│   └── set_position(id, pos)          execSetObjectivePos
│
├── timer
│   ├── set(id, secs)                  execSetTimer
│   ├── check(id)                      execCheckTimer
│   ├── end(id)                        execEndTimer
│   └── create(secs, cb)               (engine-side — see callback doc §3.1)
│
├── area
│   ├── add(x0, y0, x1, y1, kind, id)  execAddTriggerArea
│   ├── is_hit(id)                     execIsTriggerAreaHit
│   ├── reset(id)                      execResetTriggerArea
│   └── remove(id)                     execRemoveTriggerArea
│
├── global
│   ├── set(key, val)                  execSetGlobalValue
│   ├── get(key)                       execGetGlobalValue
│   ├── set_campaign(key, val)         execSetCampaignGlobalVar
│   └── get_campaign(key)              execGetCampaignGlobalVar
│
├── mission
│   ├── status()                       execGetMissionStatus
│   ├── is_won()                       execGetMissionWon
│   ├── is_lost()                      execGetMissionLost
│   ├── objective_success()            execGetObjectiveSuccess
│   ├── objective_failed()             execGetObjectiveFailed
│   ├── player_in_combat()             execPlayerInCombat
│   ├── enemy_destroyed()              execGetEnemyDestroyed
│   ├── friendly_destroyed()           execGetFriendlyDestroyed
│   ├── is_server()                    execIsServer
│   └── home_team()                    execGetHomeTeam
│
├── audio
│   ├── play_sound(id)                 execPlaySoundEffect
│   ├── play_music(id)                 execPlayDigitalMusic
│   ├── stop_music()                   execStopMusic
│   ├── play_speech(id, who)           execPlaySpeech
│   ├── play_betty(id)                 execPlayBetty
│   ├── play_wave(name, id)            execPlayWave
│   ├── set_radio(id, on)              execSetRadio
│   └── current_music()                execGetCurrentMusicId
│
├── video
│   ├── play(name)                     execPlayVideo
│   ├── fade_to_color(c, secs)         execFadeToColor
│   ├── set_movie_mode()               execSetMovieMode
│   ├── end_movie_mode()               execEndMovieMode
│   └── force_end()                    execForceMovieEnd
│
├── camera                             20 entries: position, goal_position,
│   │                                  rotation, goal_rotation, zoom,
│   │                                  goal_zoom, velocity, goal_velocity,
│   │                                  look_object, frame_length — each as
│   │                                  getter (e.g. mc2.camera.position()) +
│   │                                  setter (mc2.camera.set_position(v))
│   └── …
│
├── team
│   ├── is_targeting(team, target, who)   execIsTeamTargeting
│   ├── is_capturing(team, target, who)   execIsTeamCapturing
│   ├── print_status(team)                execPrintTeamStatus
│   ├── add_mover(side, id, ...)          execAddMoverToPlayer
│   ├── remove_mover(side, id, ...)       execRemoveMoverFromPlayer
│   └── set_force_group(id, group)        execSetUnitForceGroup
│
├── economy
│   ├── add_resource(n)                execAddResourcePoints
│   └── add_money(n)                   execAddMoney
│
├── strike
│   ├── toggle_air()                   execToggleAirStrike
│   ├── toggle_sensor()                execToggleSensorStrike
│   ├── toggle_artillery()             execToggleArtilleryPiece
│   ├── toggle_repair_truck()          execToggleRepairTruck
│   ├── toggle_salvage()               execToggleSalvageCraft
│   └── call(id, side, x, y, z, doFx)  execCallStrike
│
├── ui
│   ├── text_msg(id, val, dur)         execSetTextMsg
│   ├── large_msg(id, val, dur)        execSetLargeMsg
│   ├── tutorial_text(id)              execTutorialText
│   └── in_callout()                   execInCallout
│
├── tutorial
│   ├── animation_callout(...)         execAnimationCallout
│   ├── set_invulnerable(b)            execSetInvulnerable
│   ├── freeze_gui(b)                  execFreezeGUI
│   ├── is_voiceover_playing()         execIsPlayingVoiceOver
│   └── stop_voiceover()               execStopVoiceOver
│
├── gate
│   ├── lock_open(id)                  execLockGateOpen
│   ├── lock_closed(id)                execLockGateClosed
│   ├── release(id)                    execReleaseGateLock
│   └── is_open(id)                    execIsGateOpen
│
├── on_event(name, cb)                 (control stage only)
├── register_action(key, fn)           (Track B interop)
├── data                               (data stage only — frozen on transition)
└── experimental.*                     (mirror tree of EXPERIMENTAL bindings)
```

### Close calls (5)

1. **`execInArea` → `mc2.object.is_in_area(id, pos, r, areaId)` vs. `mc2.area.contains_object(areaId, id)`.**
   *Picked:* `mc2.object.is_in_area`. Rationale: the binding takes an `id` and decides whether *that object* is in the queried zone — it's a per-object predicate, not a per-area enumeration. `mc2.area` is reserved for trigger-rect lifecycle (`add`, `is_hit`, `reset`, `remove`), and adding an `is_in_area` lookup that takes `(areaId, id)` would conflate "trigger area was hit by anyone" with "this object is currently inside this area." Modders writing `if mc2.object.is_in_area(myMech, base_pos, 200) then …` reads naturally; the alternative `mc2.area.contains_object` reads as if the area's hit-flag were being queried.

2. **`execDamageObject` → `mc2.object.apply_damage(...)` vs. catalog's proposed `mc2.object.damage(...)`.**
   *Picked:* renamed to `apply_damage`. Rationale: the catalog already has `mc2.object.damage_state` (the read query) and `mc2.object.damage_pts` (the int). Reusing `damage` for the apply-action collides — `mc2.object.damage(id)` would be ambiguous between "what is the damage state of id" and "apply damage to id". `apply_damage` is unambiguous and matches the engine's verb. (Catalog §3 wrote both "damage" and "set_damage"; this pick resolves it.)

3. **`execIsDeadOrFled` → `mc2.object.is_dead_or_fled(id)` vs. `mc2.pilot.is_dead_or_fled(id)`.**
   *Picked:* `mc2.object`. Rationale: the predicate takes the object id and asks the engine. The pilot is a read-through. Modders working with object ids in trigger code call `if mc2.object.is_dead_or_fled(scout) …` without first resolving a pilot. `mc2.pilot.*` stays in EXPERIMENTAL and is for direct pilot-table queries.

4. **`execTeleportToPoint` → `mc2.object.teleport(id, pos)` vs. `mc2.world.teleport(id, pos)`.**
   *Picked:* `mc2.object.teleport`. Rationale: there is no `mc2.world` namespace and no other binding that would justify creating one. The action is per-object (move *this* object), not a world-state mutation. `mc2.object.teleport` parallels `mc2.object.set_position` in shape.

5. **`execSetMovieMode` / `execEndMovieMode` / `execForceMovieEnd` → `mc2.video.*` vs. `mc2.movie.*`.**
   *Picked:* `mc2.video`. Rationale: `mc2.video.play(name)` (cinematic) and the movie-mode triplet share the same subsystem (the movie/cinematic letterbox is part of the video pipeline). A separate `mc2.movie` would split a 4-binding subsystem two ways. Movie-mode is, semantically, "make the video pipeline draw fullscreen letterboxed and stop accepting gameplay input."

### Notes on policy not in the tree above

- All bindings live under `mc2`. No top-level convenience aliases (e.g. `damage_object` at root). Catalog §1 already mandates this.
- `experimental.*` is a *mirror* of the regular tree shape (catalog §2): `mc2.experimental.weapon.ranges`, `mc2.experimental.pilot.id`. Promotion to STABLE copies the entry to the parent table; the experimental name remains as alias for one release per the version policy (catalog §8).
- `mc2.camera.*` setter symmetry: every getter `mc2.camera.X()` has a setter `mc2.camera.set_X(v)`. 10 properties × 2 = 20 bindings (catalog §3). No overload tricks; explicit names only.
- `mc2.data` is a table (control stage frozen) and `mc2.on_event` is a function — they don't fit subsystem-tree; documented as such in catalog §2.

This namespace is **locked** for `mc2_api_version=1`. Subsequent additions are additive (catalog §8: "Adding a new STABLE binding — No version bump").

---

## Q3 — magicpatrol / magicguard / magicescort shadow rule

### Sub-Q1: Are these three primitives independent of corebrain?

**Yes — verified.**

- `code/ablmc2.cpp:7040-7065` (in nifty-mendeleev worktree) defines `execMagicPatrol`, `execMagicGuard`, `execMagicEscort` as no-op stubs (each just pops args via `ABLi_popAnything`/`ABLi_popInteger` and traces).
- `code/ablmc2.cpp:8057-8059` registers them via `ABLi_addFunction`.
- `docs/observations/2026-04-25-abl-library-shadow-rule.md` is the authoritative observation. The four names that *did* shadow `corebrain.abx` — `magicAttack`, `coreGuard`, `corePatrol`, `coreWait` — were gated under `#if 0` after they caused the v0.2 "passive enemies / inert turrets / broken HQ convoy" regression (commit `db8c00a`, reverted in scope by the shadow-rule fix).
- The audit script in that observation enumerates `corebrain.abx` / `orders.abx` / `miscfunc.abx` token strings; for stock retail (md5 `75f9bbdf…`, 42786 bytes) `magicpatrol`/`magicguard`/`magicescort` produce **zero** matches. They exist nowhere in stock content.
- The only callers of these names in any deployed `.abx` are Omnitech / Carver5O FSM `.abx` files (`memory/omnitech_abl_stubs_session.md`).

So a Lua mod calling `mc2.ai.magic_patrol(...)` on a stock-mission warrior touches a no-op C stub and has zero effect on the warrior's corebrain-driven AI. No interference.

### Sub-Q2: `replaces_corebrain` in `mod.json` — right shape?

**No. Use per-mission content profile gating, not a per-mod opt-in flag.**

The catalog doc (§11 open question 1) proposed a `replaces_corebrain: ["magicpatrol", ...]` array in `mod.json`. The shadow-rule observation reaches a different conclusion: **the gate that matters is the deployed `.abx` library set, not the loaded Lua mod set.**

Reasoning:

1. The shadow hazard fires at `ABLi_addFunction` time, not at Lua call time. Whether or not Lua exposes `mc2.ai.magic_patrol` does not change the C stub's registration. The native registration happens unconditionally during `initABL()`; the Lua exposure is just a thin trampoline atop it.

2. Stock retail `corebrain.abx` does not define magicpatrol/guard/escort. So registering the C stub for these three names is *always safe* against stock content. No opt-in is needed.

3. The shadow hazard for those names only materializes if a future Omnitech-or-similar content pack ships an `.abx` library that defines `magicpatrol`/`magicguard`/`magicescort` as real ABL routines. In that case, the C stub registration would silently shadow it — but the rule from the observation is: "A native ABL function may only be registered via `ABLi_addFunction` if the deployed `.abx` libraries for the active content profile do NOT define a function with the same name." That gate lives at the **C registration site**, not at the Lua opt-in.

4. The right place for the gate is `initABL()` calling into the audit machinery the observation describes. The mod profile launcher (`docs/plans/2026-04-25-mod-profile-launcher-scope-additions.md` line 148) already plans this flow. C-3 inherits whatever the launcher decides; Lua side has no opinion.

### Sub-Q3: Failure-mode policy for Lua-side use

Three layered behaviors:

1. **Stock profile, no Omnitech content pack loaded.** `mc2.ai.magic_patrol(state, path)` — no-op. The C stub pops args, traces to `[ABL] magicPatrol stub` if `MC2_ABL_TRACE=1`, returns. Lua-side: trace via `MC2_LUA_TRACE` shows `magic_patrol call state=… -> 0`. **User-visible effect: nothing happens.** This is the documented behavior (catalog §11 open question 1: "Lua mod registers a same-named action … per-mission, gated on `mod.json`"). We add a one-time per-binding warning the first time a stock-profile mission calls it: `[LUA WARN] mc2.ai.magic_patrol called in stock profile — Omnitech FSM primitives have no effect; see docs/lua-api.md#ai-magic`.

2. **Omnitech profile loaded with the matching `.abx` library.** `mc2.ai.magic_patrol(...)` calls into the (eventually real) C implementation, which is the same path Omnitech ABL content uses. Behavior: matches Omnitech AI semantics.

3. **Magic'sUnofficialExpansion or an unknown content pack ships a `corebrain.abx` that defines these names** (hypothetical — none we know of does today). The C registration would shadow it. The mod-profile-launcher's pre-launch audit catches this and refuses the registration; the C stub is *not* registered; the library implementation runs. Lua's `mc2.ai.magic_patrol` resolves through `execStandardRoutineCall` to the library routine via the standard ABL dispatch. **This requires the C-3 trampoline to handle the "no native registration" case** — it falls back to calling the library via a synthesized `ABLi_execute` against the named module routine, which works because at that point we are not interleaving against an in-flight ABL execution. (Per Q1 resolution, all Lua → ABL calls happen at drain time outside any active brain tick.)

### Recommendation: do not expose magicpatrol/guard/escort in v1

The catalog doc currently classifies them as INTERNAL (catalog §5). Keep that. Reasoning:

- They are no-ops in the supported (stock) profile. Exposing them as STABLE Lua API would document semantics that don't hold for most users.
- They are real verbs only in Omnitech content, which is itself not a v1 supported content pack (still has open AI-brain crashes per `memory/mco_omnitech_integration_attempt.md`).
- Proper exposure shape is `mc2.experimental.ai.magic_patrol(...)` once Omnitech is supported AND the `.abx` audit gate (Q3 sub-Q2) is wired through the launcher. That is a Track E concern, not Track C / C-3.

So: **C-3 does NOT bind magicpatrol/magicguard/magicescort at all.** Catalog §11 open question 1 is closed: the answer is "don't expose; revisit at Track E with proper profile-gating."

---

## Implications for C-3 (consolidated)

The changes to the C-3 implementation plan from these resolutions:

1. **Trampoline pattern is rewritten.** Drop "push args onto ABL stack, call `execXxx`, pop return". Use direct C-side `*_impl` functions extracted from the existing `execXxx` bodies. This is a refactor in `code/ablmc2.cpp` for each of the 10 M0 bindings — extract the body between the `ABLi_pop*` block and the `ABLi_pushXxx`/`ABLi_pokeXxx` line into a sibling function with a normal C signature; the original `execXxx` becomes a 3-line wrapper.

2. **LuaVM gains a deferred-event queue.** `LuaVM::Tick(deltaSec)` drains it before returning. Any Lua callback registered via `mc2.on_event(...)` runs in the drain, not at the source. Source sites enqueue an `ev::Event{kind, params}` struct; no Lua callback fires inside `brain->execute()`, ABL exec stack, or any nested ABL state.

3. **Trampolines doc §1 reference binding example needs editing.** Replace the `ABLi_pushReal/.../execDamageObject/.../ABLi_popInteger` block with a direct call to `mc2_damage_object_impl(target, attacker, weapon, dmg, hitloc, hitroll, entryAngle)`. Trampolines doc §0 already flags §5 of the implementation-shape doc as wrong; that flag now extends to trampolines §1 itself for the *same reason* — `ABLi_pop*` is bytecode-driven, not stack-driven.

4. **Trampolines doc Open Q #2 (reentrancy) is closed.** Resolution: never reentrant; never on ABL stack; deferred drain.

5. **Catalog doc §11 open Qs 1, 2, 7 are closed.** Q1 (magicpatrol/guard/escort) → not exposed in v1. Q2 (`requesthelp`/`requestshelter` `*` signatures) → resolved by the same direct-C-impl strategy: extract to a normal C signature that takes a typed list, do per-arg type-check on the Lua side, drop the `*` ambiguity. Q7 (`mcprint`) → folded into `mc2.log` per recommendation already in the catalog.

6. **C-3 binding count is unchanged for M0 but each binding requires one engine-side `_impl` extraction commit.** The engine-side commit sequence becomes: (i) extract `*_impl` for the 10 M0 bindings (no behavior change to ABL callers), (ii) vendor Sol2/Lua, (iii) wire `LuaVM` with deferred queue, (iv) write the Lua trampolines that call `*_impl` directly.

7. **Namespace is locked.** Binding registration code emits `mc2.<sub>.<verb>` exactly per the tree in Q2. Adding a binding outside that tree is a code-review reject.

---

## Open follow-ups

These survive unresolved after this doc:

1. **C-side variadic shape for `*` and `?` ABL signatures.** When Q1's option-(c) extraction is done for an EXPERIMENTAL binding using `*` (e.g. `requesthelp`, `objectstatus_count`'s `R`-array out-param) or `?` (e.g. `mcprint`, `magicpatrol` if ever revived), the `_impl` C signature has to model the variadic shape. Options: `std::variant<int,float>` per slot; an `ABLStackItem` struct passed through verbatim; a Lua-side type-tagged table. Decide per binding when promoting to STABLE. Not blocking C-3 (no STABLE M0 binding is variadic).

2. **`ABLi_execute`-based fallback for library-defined routines.** Q3 sub-Q3 case 3 (mod profile ships an `.abx` that defines a verb the engine would otherwise stub) requires Lua trampolines to call into the library via `ABLi_execute(moduleIdPtr, functionIdPtr, paramList, returnVal)`. This path is well-defined in `mclib/ablrtn.cpp:784` but never used from non-ABL context today. Need to verify `paramList` / `returnVal` work as documented and that `ABLi_execute` reset of the stack at line 817 is safe at drain time. M2 / Track E concern.

3. **Save/load Lua state with deferred-event queue contents.** If a save fires while events are queued, do we serialize the queue? Trampolines doc §8 already flags `sol::function` lifetime across save/load as M2. Deferred events can be modeled as the same problem (queue entries are data + `sol::function` callback ref). M2 design pass.

4. **Hot-reload semantics with deferred queue.** If `control.lua` is hot-reloaded while events are queued, callbacks may point at functions in the old environment. Likely answer: drop the queue at hot-reload (matches Implementation-Shape's tentative "drop user functions" rule). Confirm at M1.

5. **Per-binding trace volume.** Trampolines §5 mandates `MC2_LUA_TRACE` is gated and lifecycle-only, but the deferred-drain site adds a new lifecycle event (`event=drain count=N`) that should be added to the trace schema. Trivial; surface in C-3 commit.

6. **The 4 corebrain shadow names (`magicAttack`, `coreGuard`, `corePatrol`, `coreWait`) under `#if 0`.** They are documented as Magic-of-the-Game-Crashing in `memory/magic_abl_contamination_rule.md` and the shadow-rule observation. C-3 must not accidentally re-enable them while extracting `_impl` functions for other bindings. Add a `git grep -n` smoke check: `if [[ $(grep -c '^void execMagicAttack' code/ablmc2.cpp) -gt 0 ]]; then fail; fi` to the pre-commit hook for C-3-touched files.

7. **`replaces_corebrain` removed from `mod.json` schema.** The catalog doc §11 open question 1 mentioned it as a possibility. This doc closes that: do not add the field. The launcher does the gating. Update the modders-paradise spec §5.4 schema accordingly when next touched.

---

## Citations

ABL VM internals:
- `mclib/ablxstd.cpp:38-40` — global `stack`, `tos`, `stackFrameBasePtr`.
- `mclib/ablxstd.cpp:84-92` — `ABLi_popInteger` (bytecode-driven `getCodeToken; execExpression; pop`).
- `mclib/ablxstd.cpp:96-232` — full pop/peek family showing the same pattern.
- `mclib/ablxstd.cpp:236-275` — pure-stack push family (`ABLi_pushInteger/Real/Boolean/Char`).
- `mclib/ablxstd.cpp:336-360` — pokes (`ABLi_pokeInteger/Real/Boolean/Char`) writing to `tos`.
- `mclib/ablxstd.cpp:972-1017` — `execStandardRoutineCall` dispatch.
- `mclib/ablexec.cpp:40` — `codeSegmentPtr` global.
- `mclib/ablexec.cpp:282-362` — bytecode token reader (`getCodeToken` and friends).
- `mclib/ablexec.cpp:542` — `execRoutineCall` resets `codeSegmentPtr` from the called routine.
- `mclib/ablrtn.cpp:784-880` — `ABLi_execute` initializes the stack and pushes return/static-link/dynamic-link/return-address frame.
- `mclib/ablrtn.cpp:817` — stack reinit `stackFrameBasePtr = tos = (stack + eternalOffset);`.
- `mclib/abl.h:122-149` — `ABLi_pop*` / `ABLi_push*` / `ABLi_peek*` / `ABLi_poke*` declarations.

Exec function bodies (sample):
- `code/ablmc2.cpp:420-435` — `execGetTime` (no args, just pushes result).
- `code/ablmc2.cpp:2230-2256` — `execObjectExists` (peek + poke pattern).
- `code/ablmc2.cpp:2260-2300` — `execObjectStatus` (`ABLi_popInteger` + `ABLi_pushInteger`).
- `code/ablmc2.cpp:2304-2316` — `execObjectStatusCount` (`ABLi_popIntegerPtr` for `R` ref-array out-param).

Brain-tick reentrancy site:
- `code/warrior.cpp:2155` — `brain->execute()` (per-warrior tick; the candidate reentrancy point).

Magic-FSM primitives:
- `code/ablmc2.cpp:7040-7065` — `execMagicPatrol`/`execMagicGuard`/`execMagicEscort` no-op stub bodies.
- `code/ablmc2.cpp:8057-8059` — `ABLi_addFunction("magicpatrol"/"magicguard"/"magicescort", …)`.
- `docs/observations/2026-04-25-abl-library-shadow-rule.md` — full shadow-rule analysis, audit-script, name table.
- `memory/magic_abl_contamination_rule.md` — Magic v0.2 hotfix shadow regression.
- `memory/omnitech_abl_stubs_session.md` — Omnitech tier-2 stub registration session.
- `memory/omnitech_abl_missing_names.md` — the 11-name tier-2 list including magicpatrol/guard/escort.

Predecessor design docs (consumed):
- `docs/superpowers/explorations/2026-04-30-track-c-lua-api-surface-catalog.md` §3 (STABLE list) §11 (open questions).
- `docs/superpowers/explorations/2026-04-30-track-c-lua-trampolines-and-tests.md` §0 §1 §8 (Open Q #2).
- `docs/superpowers/explorations/2026-04-30-track-c-lua-loading-lifecycle.md` (stage gates).
- `docs/superpowers/explorations/2026-04-30-track-c-lua-implementation-shape.md` §4 (lifecycle), §7 (single-VM stage gating), supersedes-§5 banner at top.
- `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` §5.4, §5.5, §6 Track C.
