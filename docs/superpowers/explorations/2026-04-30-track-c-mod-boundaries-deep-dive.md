# Track C — Mod Boundaries Deep Dive: The NO List for `mc2_api_version=1`

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- [`2026-04-30-track-c-lua-sandbox-and-errors.md`](2026-04-30-track-c-lua-sandbox-and-errors.md) — sandbox baseline
- [`2026-04-30-track-c-lua-api-surface-catalog.md`](2026-04-30-track-c-lua-api-surface-catalog.md) — the YES list
- [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §5.4 (ABI), §8.3 (sandboxing)
- Memory: `stock_install_must_remain_playable.md`, `modders_paradise_roadmap.md`

The catalog doc lists what mods CAN call. This doc lists what they CAN'T, why, what happens when they try, and what (if any) escape hatch exists. It is the document a modder reads before filing "why doesn't this work?" — the boundaries are deliberate, not omissions. It is also the document the engine reads to decide what to refuse when a mod gets clever.

---

## 1. The complete NO list (categorized)

Every entry below is blocked in `mc2_api_version=1`. "Failure mode" is what a mod actually observes; "log line" is what the engine prints.

### 1.1 Networking

- **What:** any host-to-network operation — TCP/UDP sockets, HTTP(S), DNS, named pipes, MQTT, IPC of any kind, WebSockets, raw bind/listen.
- **Why blocked:** privacy and exfiltration. A mod that calls home leaks player data; a mod that listens opens an attack surface. Determinism for a future MP track also requires no I/O nondeterminism. There is no benign use case worth that exposure in v1.
- **Failure mode:** `socket`, `http`, `luasocket`, `copas`, `lanes`, etc. are not modules in the sandbox; `require("socket")` returns a `require` error (resolver miss) caught by `xpcall`.
- **Log line:** `[LUA v1] event=error origin=<event> mod=<id> msg=...module 'socket' not found...`
- **Escape hatch (v1):** none.
- **Future opt-in (v2+ candidate):** a `mc2.net.fetch(url, callback)` allowlist-only async GET against domains the player added to `config/lua_net_allowlist.txt`. Player consent at profile-launcher level, not mod manifest. Even then, no listening sockets, no POST, no cookies.

### 1.2 Threading

- **What:** any creation of native OS threads — `lanes`, `effil`, `llthreads`, `pthread`-via-FFI. Parallel-foreach over a Lua table.
- **Why blocked:** the sandbox is single-state; `lua_State*` is not concurrency-safe and Sol2 does not lock. Two threads in one VM corrupt the GC.
- **Coroutines ARE allowed.** Lua's cooperative coroutines run on the same OS thread; they can't race. They show up as `coroutine.create/resume/yield/wrap/status/isyieldable` and are the supported primitive for mod state machines and fiber-style scripts.
- **Failure mode:** `lanes`, `effil`, etc. not present; `require` returns nothing.
- **Log line:** `[LUA v1] event=error ... module not found`
- **Escape hatch:** the engine may, in v2+, ship `mc2.spawn_task(fn)` that wraps a coroutine, schedules it on the main thread, and applies the per-tick instruction cap. Still single-thread under the hood.

### 1.3 Raw memory / FFI

- **What:** LuaJIT-style `ffi.cast`, `ffi.new`, `ffi.typeof`, `ffi.cdef`, struct layouts, pointer arithmetic, `lua_topointer` reflection, `string.dump`-then-`load` round trips that smuggle bytecode.
- **Why blocked:** FFI is a complete sandbox bypass — `ffi.cast("char*", 0xDEADBEEF)` is arbitrary memory access. We are running PUC-Rio Lua 5.4, not LuaJIT, so FFI is never linked in the first place. Bytecode loads are blocked by stripping `load`/`loadstring`/`loadfile` (sandbox doc §1).
- **Failure mode:** `ffi` is `nil`; bytecode-bearing strings rejected by our `require` shim (sandbox doc §8 open question, definite policy here: any chunk whose first byte is `0x1B` is rejected before `luaL_loadbuffer`).
- **Log line:** `[LUA v1] event=bytecode_reject mod=<id> path=<...>` for the bytecode case; standard `event=error msg=...nil value...` for FFI access.
- **Escape hatch:** none, ever. FFI in a mod sandbox is a category error.

### 1.4 OS shell and process

- **What:** `os.execute`, `os.remove`, `os.rename`, `os.exit`, `os.getenv`, `os.setlocale`, `os.tmpname`, `popen`, `io.popen`, any process spawn.
- **Why blocked:** `os.execute("rm -rf ~")` is the textbook example. Even read-only `os.getenv` leaks PII (paths, usernames). `os.exit` kills the host process from a mod, not just the VM.
- **Failure mode:** functions are absent from the rebuilt `os` table (sandbox doc §1). Calls hit `attempt to call a nil value`.
- **Log line:** `[LUA v1] event=error origin=<event> mod=<id> msg=...attempt to call a nil value (field 'execute')...`
- **Allowed:** `os.time`, `os.clock`, `os.date`, `os.difftime` (pure, read-only).

### 1.5 Filesystem outside the sandbox

- **What:** reading anything not under `<install>/mods/<modid>/` or `<install>/data/`; writing anywhere; following symlinks/junctions out of the roots; absolute paths; UNC paths; drive letters; `..` traversal.
- **Why blocked:** sandbox escape. Symlinks/junctions on Windows are the sneaky case (sandbox doc §8 open question 6).
- **Failure mode:** `mc2.io.read(path)` returns `nil` on rejection. `io.open` is not exposed at all. `package.loadlib` is not exposed.
- **Log line:** `[LUA v1] event=io_reject reason=<resolve|symlink|absolute|nul|drive> path=<rel>`
- **Escape hatch:** the modder ships the file inside their mod tree.

### 1.6 Other mods' internal state

- **What:** reading or writing another mod's `persist` table, upvalues, locals, registered handlers; injecting globals into a sibling mod's environment; iterating a sibling's loaded chunks.
- **Why blocked:** mod isolation. Two mods that compete for the same global break in mysterious ways; one mod can't be allowed to silently corrupt another. Communication MUST be explicit (events, registered actions) so dependencies are visible.
- **Failure mode:** `mc2.mods["other_mod"]` returns the read-only metadata proxy described in §5; attempting to index `.persist`, `.env`, or any non-whitelisted field returns `nil`.
- **Log line:** `[LUA v1] event=cross_mod_access_reject mod=<caller> target=<other_mod> field=<name>` (logged once per (caller, target, field) tuple to avoid spam).
- **Escape hatch:** events (`mc2.events.emit`/`on`) and actions (`ActionRegistry`).

### 1.7 Engine internals

- **What:** raw `GameObject*` pointers, GL state, FBO handles, shader objects, the txmmgr render lists, ABL VM internals, `gosFX` particle pool internals, the cull gates (`inView`/`canBeSeen`/`objBlockInfo`).
- **Why blocked:** the cull gates are load-bearing (per memory:`cull_gates_are_load_bearing.md`). Random pointer access from a script crashes the host. The supported surface is the BindingRegistry — what the catalog doc enumerates and nothing else.
- **Failure mode:** there's no symbol to call. Any attempt to "find" engine pointers via `string.find` on `tostring(obj)` returns userdata addresses but they can't be dereferenced — Lua has no pointer arithmetic without FFI (see §1.3).
- **Log line:** N/A — there is nothing to reject; the surface simply does not exist.

### 1.8 Save file format manipulation

- **What:** `mc2.save.write_raw_blob`, `mc2.save.poke_offset`, byte-level patches to the savegame file, `mc2.save.replace_signature`.
- **Why blocked:** saves are an engine concern. A mod that writes raw bytes corrupts saves the engine then refuses to load (or, worse, crashes mid-load). Persistence is via `mc2.persist.set/get` only — typed key/value, schema-versioned, scoped per-mod.
- **Failure mode:** no such API. `mc2.save` is not in the namespace tree.
- **Escape hatch:** `mc2.persist.*` (Track E, deferred); when it lands, schema versioning and migration is engine-managed, not mod-managed.

### 1.9 Reentry into the ABL VM from non-ABL context

- **What:** a forward-direction binding (mc2.* call → ABL primitive) that pushes onto the ABL stack while the engine is not currently in an ABL tick. Per blocking-questions §Q1: ABL has a single global VM with thread-local-like state assumed live during `update()`.
- **Why blocked:** ABL's stack frames are not reentrant. A Lua callback firing from a timer that pushes ABL state corrupts the next ABL tick.
- **Failure mode:** runtime guard at the binding boundary — every forward binding checks a `g_ablInTick` flag. If false, the binding errors.
- **Log line:** `[LUA v1] event=abl_reentry_reject mod=<id> primitive=<name>`
- **Escape hatch:** none. Mods that need to talk to ABL do so by firing engine events that ABL polls on its own tick.

### 1.10 Modifying other mods' Lua code or environment

- **What:** `debug.sethook`, `debug.getinfo` on foreign frames, `debug.getlocal`, `debug.setupvalue`, `debug.setmetatable`, monkey-patching by reaching into `mc2.mods["other"].env`.
- **Why blocked:** debugging is a great backdoor. A "logging" mod that hooks every other mod becomes a kernel.
- **Failure mode:** `debug` table contains only `traceback` (sandbox doc §1). All other names are `nil`.
- **Log line:** standard `attempt to call a nil value`.
- **Escape hatch:** `mc2.events.on("debug_trace", ...)` — engine-emitted, mod-consumed; no foreign code introspection.

### 1.11 Bypass profile-launcher / sandbox itself

- **What:** `mc2.dev.disable_security`, `mc2.dev.bypass_sandbox`, `mc2.engine.unsafe_eval`, environment-variable backdoors keyed on mod-supplied strings.
- **Why blocked:** there is no scenario where a script should disable its own sandbox. Even debug builds ship the gates — debug builds add MORE logging, not fewer guards.
- **Failure mode:** these names are not in the namespace.
- **Log line:** N/A.

### 1.12 Stock content modification in place

- **What:** writing to `<install>/data/`, modifying stock `.fst`, replacing `corebrain.abx`.
- **Why blocked:** memory:`stock_install_must_remain_playable.md`, memory:`magic_abl_contamination_rule.md` — overwrites of stock files cascade into save corruption and AI hangs.
- **Failure mode:** the FS shim is read-only (sandbox doc §2).
- **Log line:** `[LUA v1] event=io_reject reason=writes_disallowed`

---

## 2. Experimental opt-in mechanism

Some power-user features are unsafe to ship default-on but worth offering. A mod opts in per-feature in `mod.json`:

```json
{
  "id": "advanced_ai",
  "mc2_api_version": 1,
  "experimental_features": ["raw_metatables", "filesystem_external_read"]
}
```

At load, the engine logs every flag explicitly:
```
[LUA v1] event=experimental_opt_in mod=advanced_ai features=[raw_metatables,filesystem_external_read]
```
The launcher UI surfaces a yellow badge. The modder declares — in writing, in the manifest — that they accept instability and that the names may be renamed or removed without `mc2_api_version` bump.

**Candidate experimental flags for v1:**
- `raw_metatables` — re-enables `setmetatable`/`getmetatable` (sandbox doc §1 strips them in v1). Useful for OO-style mod code.
- `coroutine_unbounded` — disables the per-tick instruction cap inside coroutines, with a hard 50M opcode safety ceiling. For long-running mod state machines.
- `mc2.experimental.*` namespace — already covered by the catalog (`magicattack`, mech-lab manipulation, advanced UI primitives). Listing the flag is implicit when any binding from that namespace is referenced.

**Candidate v2 flags (future):**
- `filesystem_external_read` — opt-in for reading from a player-managed `<install>/mods/_shared_data/` directory.
- `net_fetch_allowlist` — see §1.1.
- `lua_jit_unsafe` — if we ever switch to LuaJIT and want FFI behind the strongest possible warning.
- `large_memory_cap` — override the 64 MiB per-VM ceiling (require player approval at launcher, not mod).

Engine policy: **a mod cannot consume an experimental flag that isn't declared in its manifest.** Attempting to call `mc2.experimental.foo` without `experimental_features` listing it errors at first call with `[LUA v1] event=experimental_undeclared mod=<id> feature=foo`.

---

## 3. Per-mod resource budgets

The sandbox doc gives one VM-wide cap. With multiple mods sharing a VM, fairness matters.

### 3.1 Memory

- VM-wide cap: 64 MiB (sandbox doc §4).
- Per-mod sub-cap: **soft** 16 MiB advisory, no hard enforcement in v1 — measuring per-mod allocations requires per-allocator tagging the v1 plan does not pay for. We log periodic `[LUA_BUDGET v1] event=mem_estimate mod=<id> usage_kib=<n>` lines from a sampling pass that tags allocations by `g_current_mod_id` at top-of-stack.
- When VM-wide passes 80% of cap: `[LUA_BUDGET v1] event=approaching budget=memory usage=82%` — once per crossing.
- When cap is hit: standard `out of memory` Lua error from the allocator.

### 3.2 CPU (opcodes per tick)

- VM-wide: 5,000,000 opcodes/tick (sandbox doc §4).
- Per-mod sub-budget: **round-robin slicing**. If five mods are loaded, each gets ~1M opcodes before the hook bumps execution to the next mod's pending callbacks. Implementation: the hook checks `g_current_mod_id` and a per-mod counter; when one mod exhausts its slice, the dispatcher yields control to the next mod's queued callbacks for this tick.
- At 80% of slice: `[LUA_BUDGET v1] event=approaching mod=<id> budget=cpu usage=80%`.
- At 100%: that mod's current callback errors with `instruction cap exceeded`; other mods on the same tick still run.

### 3.3 String / table allocation

- No separate budget. The memory cap subsumes both (sandbox doc §4).

### 3.4 Asset loading (textures, models)

- No hard cap in v1. Modders get warned via existing `[ASSET_SCALE v1]` and engine handle-cap logs (texture handle cap 3000 — memory:`texture_handle_cap.md`).
- When a mod registers its 100th texture: `[LUA_BUDGET v1] event=asset_count mod=<id> kind=texture count=100` (advisory).
- When the engine handle table fills: existing engine error path; the mod's load fails, mission boots without that mod.

### 3.5 Persistence

- Per-mod persist table soft-capped at 1 MiB serialized. Above that: warning. Above 4 MiB: refuse to write.

---

## 4. Sandbox escape tests

The sandbox-and-errors doc ships a 20-row battery. Extending it for boundary verification:

### 4.1 Bytecode and load-trickery
- **B1.** `string.dump(function() end)` then `load(<bytes>)` — `string.dump` is left in the sandbox? If yes, ensure `load` is gone (it is, sandbox §1). Test: error.
- **B2.** Construct a string starting with `\x1B` and pass to `require` — fail at the bytecode header check.
- **B3.** `require("scripts/../../../../../../etc/passwd")` — resolver rejects.

### 4.2 Recursion and stack
- **R1.** Mutual recursion 1M deep — Lua stack overflow, caught.
- **R2.** Hand-rolled deep table `{a={b={c=...}}}` of 100K levels — should hit memory cap on serialize, not crash on traversal.

### 4.3 Coroutine abuse
- **C1.** 10K coroutines created, never resumed — memory cap.
- **C2.** Coroutine that resumes itself indirectly — Lua refuses (cannot resume non-suspended); error caught.
- **C3.** `coroutine.wrap` over an infinite-loop fn, called from inside per-tick hook — instruction cap fires inside the coroutine (validates open-question 3 of sandbox doc).

### 4.4 Allocation bombs
- **A1.** `local t={} for i=1,1e9 do t[i]=i end` — memory cap.
- **A2.** `string.format(("%s"):rep(1e6), unpack(args))` — memory cap.
- **A3.** Self-referential table cycles forced through serializer — `mc2.persist.set("k", cycle)` errors with `cycle detected`.

### 4.5 Metatable circumvention
- **M1.** `setmetatable({}, ...)` — `setmetatable` is `nil` in v1.
- **M2.** Reach `getmetatable("")` (string metatable is shared) — returns `nil` (we override; or the string library's metatable is frozen).
- **M3.** Assign `__index` via `rawset` — `rawset` is `nil`.

### 4.6 FFI-via-string-trickery
- **F1.** `package.loadlib("kernel32", "MessageBoxA")` — `package` is `nil`.
- **F2.** `io.popen("calc")` — `io.popen` is `nil`; `io` is the replacement table.
- **F3.** Spelling games — `_G["os"]["execute"]` — `os.execute` is `nil` even via dynamic lookup.

### 4.7 Cross-mod
- **X1.** Mod A sets a global; Mod B reads it — Mod B sees `nil` (separate sandbox envs per mod).
- **X2.** Mod A calls `mc2.mods["B"].persist.foo = 1` — write rejected, log line emitted.
- **X3.** Mod A registers handler for event `B.private` — handler fires only if Mod B emits that event publicly via `mc2.events.emit`.

**CI integration:** all rows above run on every build via `--lua-sandbox-test` smoke flag. **Build fails** if any row produces unexpected behavior. Adding a binding to the engine that breaks a row blocks the merge.

---

## 5. Cross-mod state isolation

`mc2.mods[other_id]` returns a table with **only** these read-only fields:

```lua
{
  id        = "other_mod",     -- string
  version   = "1.2.0",         -- string, semver
  loaded_at = 1714435200,      -- os.time integer
  api       = 1,               -- declared mc2_api_version
  enabled   = true,            -- bool
}
```

That's it. No `persist`, no `env`, no `handlers`, no `actions`. Indexing any other field returns `nil`. The proxy is frozen via a metatable in C++ (the only place metatables exist in v1 outside `experimental_features=raw_metatables`).

Cross-mod communication is via two mechanisms only:

1. **Events.** `mc2.events.emit("namespace.event", payload)` from Mod A; `mc2.events.on("namespace.event", fn)` from Mod B. Payload is deep-copied across the boundary so neither mod can mutate the other's view.
2. **Actions.** `ActionRegistry.register("action_name", fn)` from Mod A; `ActionRegistry.invoke("action_name", args)` from Mod B. Same deep-copy semantics.

Naming convention: mods that emit public events SHOULD prefix with their mod id (`advanced_ai.threat_detected`), so other mods know who they're listening to.

---

## 6. Trust levels

**v1: one tier — Local.**

All mods come from `<install>/mods/<modid>/`. All Local mods get the identical sandbox. There is no signing, no reputation, no special-case escape. The decision to install a mod is the player's, and the launcher shows the manifest's declared `experimental_features` before enabling.

**v2+ candidates (deferred, listed for context):**
- **Verified** — signed by an MC2-team key. Could opt out of certain warnings; could (very carefully) be granted access to one or two extra `experimental_features` without the in-launcher consent prompt. The bar for verification is "we read the source and it's safe", not "the author is famous".
- **Workshop** — Steam Workshop or moddb subscription. Same sandbox as Local; metadata source differs.

The point of trust tiers is NOT to grant more capabilities. It is to *reduce friction* on already-allowed capabilities. Capabilities themselves are gated by the sandbox, full stop. A "verified" mod still cannot call `os.execute`.

---

## 7. The stock-playable rule applied to mods

Per memory:`stock_install_must_remain_playable.md`, the architectural constant is: **stock content always works, regardless of mods.**

Concrete rules a mod cannot violate:

- A mod **cannot disable** stock missions. The mission registry is append-only from a mod's perspective; mods may add missions but never remove or rename existing ones.
- A mod **cannot remove** stock content (units, voices, art). Removal would break the stock campaign.
- A mod **cannot modify** stock data files in place. Override is per-mod via the loose-file system (memory:`mc2_path_separator_linux_build.md`); the mod ships its own copy of any file it overrides, in its own tree, and the engine resolves to it only when the mod is enabled.
- A mod **cannot replace** `corebrain.abx`. Per memory:`magic_abl_contamination_rule.md`, this is enforced by the profile-launcher's `.abx` audit, not by Lua. Mod-supplied `.abx` files outside `corebrain.abx` are loaded into the per-profile ABL pool.
- The **stock profile** is always launchable from the mod-profile-launcher with zero mods enabled. This is a launcher invariant, not a mod-side concern, but worth listing here so modders understand: "no mods" is a first-class supported configuration.

If a mod's mission scripts assume an active mod is loaded, the launcher prompt is "this profile requires mods X, Y, Z" — the engine refuses to launch the mod-dependent profile without dependencies, but the stock profile is unaffected.

---

## 8. Failure modes table

| NO-list item | Engine response | Log line | Severity | Visible to modder? |
|---|---|---|---|---|
| `os.execute` etc. | nil call → Lua error | `event=error msg=...nil value...` | operation-fatal | yes, in mod log |
| `socket`/`http` | require fails | `event=error msg=module not found` | operation-fatal | yes |
| `ffi.*` | nil index | `event=error msg=...nil value...` | operation-fatal | yes |
| FS read outside roots | `mc2.io.read` returns nil | `event=io_reject reason=resolve path=...` | operation-fatal | yes |
| FS write | shim does not expose | `event=error msg=...nil value...` | operation-fatal | yes |
| Bytecode in `require` | rejected before load | `event=bytecode_reject mod=<id> path=<...>` | operation-fatal | yes |
| Cross-mod persist write | proxy returns nil | `event=cross_mod_access_reject mod=<a> target=<b> field=persist` (once per tuple) | operation-fatal | yes |
| `debug.sethook` etc. | nil | `event=error msg=...nil value...` | operation-fatal | yes |
| ABL reentry from non-tick | runtime guard | `event=abl_reentry_reject mod=<id> primitive=<name>` | operation-fatal | yes |
| Memory cap exceeded | allocator returns NULL | `event=error msg=...not enough memory...` | mod-fatal-this-tick (xpcall recovers VM) | yes |
| Instruction cap exceeded | hook errors | `event=error msg=...instruction cap exceeded...` | mod-fatal-this-tick | yes |
| Experimental feature not declared | first-call error | `event=experimental_undeclared mod=<id> feature=<n>` | operation-fatal | yes |
| Stock data write attempt | shim refuses | `event=io_reject reason=writes_disallowed` | operation-fatal | yes |
| Symlink escapes root | resolver rejects | `event=io_reject reason=symlink path=<...>` | operation-fatal | yes |
| API version too high | refuse to load mod | `event=manifest_api_mismatch mod=<id> declared=<n> engine=<m>` | mod-fatal-load | yes, in launcher |
| API version too low | warn, attempt load | same line, severity=warn | warning | yes |

**Severity legend:** `operation-fatal` = the offending call errors, the rest of the mod still runs. `mod-fatal-this-tick` = the current event handler aborts; the mod's other handlers fire next tick. `mod-fatal-load` = the mod is rejected at load; engine continues without it. **Engine never crashes** on any row.

---

## 9. Migration policy for boundary changes

When the boundary moves between API versions, the catalog doc's stability/versioning policy applies (catalog §8):

- **Adding a new namespace or function (e.g. `mc2.net.fetch` in v2):** additive. Old mods unaffected. `mc2_api_version` is bumped because the engine's new version may behave differently on previously-undefined names.
- **Promoting `mc2.experimental.foo` → `mc2.foo`:** additive *with* one-version alias retention. `mc2_api_version` not bumped on the promotion itself; bumped one version later when the alias is removed.
- **Removing something from the NO list (e.g. enabling `setmetatable` by default):** API-version bump required. Mods declaring the new version may rely on it; mods declaring the old version still see the v1 sandbox. The engine selects the per-mod sandbox shape based on the manifest's `mc2_api_version`, which means the engine ships *both* sandbox configurations once any boundary moves.
- **Adding to the NO list (tightening):** major version bump and a deprecation window. Mods declaring the old version get a warning but still load against the old sandbox; mods using the new version get the tighter sandbox.
- **Renaming a binding:** alias keeps old name working for one version; bump after deprecation window.

The launcher always shows declared-vs-engine API version with a yellow badge on mismatch, even when the engine accepts the load.

---

## 10. Open questions

1. **Per-mod memory accounting overhead.** Tagging every Lua allocation with `g_current_mod_id` requires either a per-mod allocator context swap on every callback boundary (cheap if done at the dispatch trampoline) or a sampled approximation. Which does v1 ship? Lean: sampled, with a 60s rolling estimate per mod, since the hard cap is VM-wide and per-mod is advisory only.

2. **Round-robin CPU slicing fairness.** If Mod A registers 10 timers and Mod B registers 1, A consumes 10× the dispatcher slots even before opcode counts. Do we round-robin by *handler-invocation* or by *opcodes-elapsed-per-mod*? Lean: opcodes, since handlers are unequal weight. Implementation cost: per-mod opcode counter alongside the existing per-tick counter.

3. **`mc2.events.emit` payload size.** Cross-mod events deep-copy. A mod that emits a 10 MB table 60 times per second is a denial-of-service against itself and listeners. Cap event payload at 64 KiB serialized? Larger payloads should go via `mc2.persist` and a "look at this key" event with the key name.

4. **Symlink resolution on Windows junctions.** Sandbox doc §8 open question 6 — `std::filesystem::is_symlink` does not reliably flag NTFS junction points. Need a test against a junction pointing into `C:\Windows` before declaring §1.5 watertight; if `is_symlink` misses, fall back to `GetFileAttributesW(... & FILE_ATTRIBUTE_REPARSE_POINT)`.

5. **Read-only proxy implementation cost.** `mc2.mods[other]` returns a frozen table proxy. The proxy needs `__index` returning a fixed whitelist plus `__newindex` rejecting writes. That requires metatables on engine-owned tables even when `setmetatable` is sandbox-stripped — fine, since the sandbox restriction is on Lua-side calls, not engine-pushed tables.

6. **Should `mc2.persist` be per-mod-per-mission or per-mod-global?** Catalog doc defers; for boundary purposes the relevant question is whether one mod's mission-A persist is visible from its own mission-B. Lean: per-mod-global with explicit `mc2.persist.scope("mission")` for mission-scoped sub-tables. Out of scope for v1 if Track E persistence isn't ready.

7. **Per-mod handler exception isolation.** When Mod A's handler errors, Mod B's handler for the same event still runs (the dispatcher iterates and `xpcall`s each separately — sandbox doc §3). Confirmed yes; documenting here so future refactoring doesn't regress.

8. **Determinism guarantees and `math.random`.** v1 ignores determinism. When MP lands, every mod's `math.random` must seed from a per-mission deterministic source. That's a sandbox amendment, not a boundary change; flagging here so the boundary doc owner stays aware.

---

**Tally:** 12 NO-list categories, 7 experimental-flag candidates (3 v1, 4 v2), 5 budget axes, 22+ sandbox escape tests, 5 read-only metadata fields per cross-mod query, 1 trust tier in v1, 5 stock-playable rules, 16 failure-mode rows, 5 migration-axis bumps, 8 open questions. The contract is small enough to defend, large enough to mod against.
