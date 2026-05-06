# Track C — What We Can Borrow From OpenRA, Factorio, and Spring/BAR

**Date:** 2026-04-30
**Mode:** Research only. No code changes. Companion to:
- `specs/2026-04-29-modders-paradise-roadmap-design.md` §3 (reference stack)
- `explorations/2026-04-30-track-c-lua-loading-lifecycle.md` (mechanics we are mapping onto)

This doc walks the actual source of the three reference projects and pins down what each one does, where the code lives, and how it maps onto our Track C design. Excerpts are short; paraphrase carries most of the weight. Citations are GitHub paths.

---

## Q1 — How does OpenRA implement `Inherits:`?

### Pattern: depth-first inheritance with immutable visited-set cycle detection

**Source:** [OpenRA/OpenRA `OpenRA.Game/MiniYaml.cs`](https://github.com/OpenRA/OpenRA/blob/bleed/OpenRA.Game/MiniYaml.cs), bleed branch.

The relevant entry points are three nested functions:
- `Merge()` (~lines 483–514) — top-level orchestrator.
- `ResolveInherits()` (~lines 548–588) — depth-first walk that resolves a node's inheritance chain.
- `MergeIntoResolved()` (~lines 516–535) — in-place merge of overrides into an already-resolved parent.

**Algorithm.** When `ResolveInherits` sees a child node whose key is `Inherits` or `Inherits@<suffix>`, it looks the parent up in `tree` (a flat dictionary of all top-level definitions parsed from the yaml fileset), recursively resolves that parent first, then merges the child's own nodes on top. The merge is **last-write-wins per field**, with the child winning over the parent. `Inherits@<suffix>` is the OpenRA mechanism for inheriting from multiple parents — each `Inherits@X` line is a separate edge, processed in source order, so later parents override earlier parents and the child overrides all of them.

**Conflict resolution.** Field-level. There is no diamond-resolution magic; the file-order semantics of `Inherits@` lines provide explicit ordering when a modder needs it. From `MergePartial` (~603–621): override values supersede existing ones via a null-coalescing read.

**Cycle detection.** `ResolveInherits` carries an `ImmutableDictionary<string, MiniYamlLocation> inherited` parameter that tracks every parent already on the resolution stack. Each recursive descent does:

> tracked via `inherited.Add(parent, location)` — `ArgumentException` on duplicate key triggers the cycle error.

The catch site re-throws as a `YamlException` mentioning the offending parent and source location. Because `ImmutableDictionary` is structural, sibling branches can re-visit the same parent legitimately (it's only the stack that detects re-entry).

**Code shape (paraphrased — under 15 words verbatim per excerpt):**

```csharp
// MiniYaml.cs:548 — ResolveInherits, abbreviated
foreach (var n in node.Nodes) {
    if (n.Key == "Inherits" || n.Key.StartsWith("Inherits@")) {
        if (!tree.TryGetValue(n.Value.Value, out var parent))
            throw new YamlException(...not found...);
        try { inherited = inherited.Add(n.Value.Value, n.Location); }
        catch (ArgumentException) { throw new YamlException(...already inherited...); }
        ResolveInherits(parent, tree, inherited);   // recurse
        MergeIntoResolved(node, parent, ...);
    }
}
```

**What we can borrow.** The Track C lifecycle doc (§1) already specifies Kahn's algorithm for *mod-level* dependencies. OpenRA's `Inherits:` is a *prototype-level* primitive — orthogonal but compatible. If we want a `mc2.prototypes.inherit("mech", "annihilator", "atlas", { hp = 12000 })` call (open question §9.1 in lifecycle doc — the override-policy decision), the algorithm shape transplants cleanly: do parent prototypes first, last-write-wins on field merge, walk-stack visited-set for cycles. The roadmap §5.2 manifest already has top-level `"inherits": ["stock"]` for whole-mod composition. We can layer prototype-level inheritance on top later without revisiting the manifest format.

**What's different about MC2.** Our prototypes are JSON-shaped (Factorio-style `data:extend{}`), not YAML trees. OpenRA parses an entire mod's worth of `*.yaml` into a single dictionary then resolves; we'd be merging *Lua tables in a single VM*, where field-merge is just `setmetatable(__index = parent)` for read-time inheritance — much cheaper than pre-resolving. The cycle-detection trick (immutable visited-set on the recursion stack) still ports.

---

## Q2 — How does Factorio implement the data.lua / control.lua sandbox?

### Pattern: stage-discarded shared Lua state + global `data` table + library-stripped data stage

**Source:**
- [`wube/factorio-data` `core/lualib/dataloader.lua`](https://github.com/wube/factorio-data/blob/master/core/lualib/dataloader.lua) — the `data:extend{}` API and `data.raw` storage.
- [`wube/factorio-data` `core/lualib/util.lua`](https://github.com/wube/factorio-data/blob/master/core/lualib/util.lua) — utility functions; reveals what's available in the data stage.
- [Factorio docs: data lifecycle](https://lua-api.factorio.com/latest/auxiliary/data-lifecycle.html) — official statement that the data stage's standard Lua API is restricted and the shared state is **discarded** between stages.

**What `data:extend{}` actually does.** From `dataloader.lua`: it's a method on the global `data` table that takes an array of prototypes, validates each has `type` and `name` fields, and stores them in `data.raw[type][name]`. If a `data.raw[type]` bucket doesn't exist yet, it's created on the fly. Duplicate detection uses a `data_duplicate_checker` helper. Both `data:extend(t)` and `data.extend(t)` calling conventions work because of an argument-shifting check at the top.

**Excerpt (under 15 words verbatim):**

```lua
-- dataloader.lua, paraphrased
function data:extend(otherdata)
  if type(otherdata) ~= "table" then error(...) end
  for _, e in ipairs(otherdata) do
    if not e.type or not e.name then error(...) end
    local t = self.raw[e.type]
    if t == nil then t = {}; self.raw[e.type] = t end
    -- duplicate-check + assign
    t[e.name] = e
  end
end
```

**Library subset at data stage.** The official lifecycle doc states the standard Lua API is restricted at data stage. Empirically, `util.lua` uses `pairs`, `ipairs`, `string.gmatch`, `math.*`, `table.*`, metatables, and `tostring`. So: most of pure-Lua is there. What's *missing* (and load-bearing for sandboxing): no `io`, no `os.execute`, no `require` of arbitrary paths (mods `require` is rerouted into the mod's own filesystem), no `loadfile`/`load`/`loadstring`, no `debug` library functions besides `traceback`. (Confirmed by absence — the wiki and modder community have documented these constraints for years.)

**Freeze mechanics.** The lifecycle doc is explicit: "After a stage's three rounds have finished, the shared Lua state is discarded." Factorio doesn't freeze the data table mid-flight via metatables; it throws away the whole VM. Control stage gets a *fresh* Lua state where the only carry-over is the prototype data (now exposed as runtime `LuaPrototype` objects, not the raw `data.raw` tables).

**What we can borrow verbatim.**
- `data:extend{}` API shape — already in our roadmap §5 directly.
- Validation pass: type+name required, indexed by type then by id. Track C lifecycle §3 already says `mc2.prototypes.register("mech", "madcat", {...})` — same primitive, slightly different surface (we name the kind explicitly rather than putting it in a `type=` field, because we have a closed set of kinds and it's clearer to error early).
- The "first-write wins" duplicate policy with a logged collision event.

**What we adapt.**
- Factorio destroys the data-stage VM and re-creates the world as runtime objects in a fresh VM. We use a **single VM with stage gates** (impl-shape §7, lifecycle §2). Reasoning: our control-stage scripts will want to *read* the prototypes as Lua tables, not as opaque engine handles, because our spawn primitives copy fields from the prototype into engine-side structs. Keeping the same Lua tables alive (just `__newindex`-locked) is simpler than reifying every prototype as a userdata wrapper.
- We never plan to ship multiplayer (open question §9.5), so the determinism arguments that drove Factorio's discard-and-restart mechanism don't apply with the same force. Our freeze-via-metatable approach is acceptable.

**What we reject.**
- The three-round (`data.lua` → `data-updates.lua` → `data-final-fixes.lua`) machinery. It exists because Factorio mods routinely *patch other mods' prototypes*, and that requires a deterministic ordering of "everyone declares, then everyone updates, then everyone fixes." We don't have that scale yet. M0 is single-pass. If the override-policy open question (lifecycle §9.1) resolves toward deep prototype-patching, we revisit.

---

## Q3 — How is Lua sandboxed in Spring/BAR?

### Pattern: synced/unsynced context split with stripped debug lib + restricted environments

**Source:** [`beyond-all-reason/RecoilEngine` `rts/Lua/LuaHandle.cpp`](https://github.com/beyond-all-reason/RecoilEngine/blob/master/rts/Lua/LuaHandle.cpp), master branch. The widget-handler counterpart lives in `rts/Lua/LuaHandleSynced.cpp` and the userspace dispatcher is `cont/LuaUI/widgets.lua`.

**Sandbox boundary.** Spring runs *two classes of Lua*:
- **Gadgets** — synced game-logic code. All clients must execute identical Lua to keep multiplayer in sync. Trusted; sees full game state. Lives in `LuaRules/`.
- **Widgets** — unsynced UI code. Per-client; can crash without desyncing the game. Filtered: cannot see information the local player shouldn't have (e.g. enemy units outside line-of-sight).

The split is enforced at the C++ binding layer: separate sets `SYNCED_LUAHANDLE_CONTEXTS` and `UNSYNCED_LUAHANDLE_CONTEXTS` track which `lua_State*` is which, and read functions like `LuaUnsyncedRead` are *only* registered into the unsynced contexts. Synced gadgets get the full callout API; widgets get the censored one.

**Excerpt (paraphrased):**

```cpp
// LuaHandle.cpp, abbreviated
static spring::unsynced_set<const luaContextData*> SYNCED_LUAHANDLE_CONTEXTS;
static spring::unsynced_set<const luaContextData*> UNSYNCED_LUAHANDLE_CONTEXTS;
// ...later, when registering callouts:
//   if (synced) push into LuaSyncedCtrl/Read; else push into LuaUnsyncedCtrl/Read.
```

**Debug library handling.** From the LuaHandle source comments: the debug library is opened into the registry but only `debug.traceback` is preserved. Per the docstring, the rest is "not sync safe" and stripped. This is a stronger claim than ours — they care about determinism; we mostly care about not leaking C-stack-introspection to mod code.

**Filesystem access.** Replaced wholesale by Spring's `LuaIO` / `LuaVFS` shim, which sandboxes file reads to the mod's archive (and explicitly enumerated paths). `io.open` is not the standard one. `os.execute`, `os.remove`, etc. are removed.

**Widget dispatch.** `cont/LuaUI/widgets.lua`. Engine-side callins (`GameStart`, `Update`, `UnitDamaged`, etc.) are routed through global functions installed by `widgetHandler:UpdateCallIn()`:

> per `widgets.lua:1057-1074`, an entry like `_G[name] = function(...) return selffunc(self, ...) end` makes the engine's call into the user-space dispatcher.

Each widget that defines a function with the matching name gets registered into a per-callin list (`widgetHandler.GameStartList`, etc.). On engine call, the handler walks the list and pcalls each widget in turn. This is exactly the dispatch fan-out shape our Track C `mc2.on_event(name, fn)` will need — many subscribers per event, pcall each, log failures, don't unwind on a single bad listener.

**What we borrow.**
- The pcall-each-listener dispatch shape (Q5).
- The "strip the debug lib down to just `traceback`" pattern. Our impl-shape §6 already lists which sol2 libs to open (`sol::lib::base, math, string, table`); we add a careful `debug.traceback`-only opening for error reporting.
- The "open a custom file-IO shim, don't open Lua's `io` library" pattern. We don't need full Spring-style VFS; a single `mc2.read_data(path_relative_to_mod)` is enough for M0.

**What we adapt.**
- We don't need synced/unsynced. There's no multiplayer ambition. The bifurcation collapses to the data/control stage gates we already have.

**What we reject.**
- Spring's `LuaVFS` archive-mounting layer. We have FST + loose-file overrides already (`memory/mc2_path_separator_linux_build.md`); a Lua-side VFS would duplicate it.

---

## Q4 — How does Factorio (and OpenRA) implement mod load order?

### Pattern: dependency-chain depth as primary sort key, mod ID as tiebreaker

**Factorio source.** Closed engine, but the algorithm is documented in the [data lifecycle docs](https://lua-api.factorio.com/latest/auxiliary/data-lifecycle.html) and the [mod structure docs](https://lua-api.factorio.com/latest/auxiliary/mod-structure.html). Paraphrased: each mod gets a "dependency chain length" computed from its `info.json` `dependencies` list, then mods are loaded shortest-chain to longest, with same-length mods sorted by internal name. This is functionally equivalent to Kahn's algorithm with alphabetical tiebreak — exactly what our lifecycle doc §1 specifies.

**Dependency operators (info.json):**
- `?` — optional dependency
- `(?)` — hidden optional (UI-suppressed)
- `!` — incompatibility (refuses to load alongside)
- `~` — dependency that does NOT affect load order (must exist, but no edge in the topo graph)
- no prefix — required, load-order-affecting
- Version operators: `<`, `<=`, `=`, `>=`, `>`

**OpenRA source.** OpenRA does *not* have arbitrary dep graphs — it has a single linear `Inherits:` chain in the mod manifest. Far simpler, no topo sort needed. The richer feature is per-class `Inherits:` (Q1), not whole-mod chaining. OpenRA's `mods/cnc/mod.yaml` and `mods/d2k/mod.yaml` show the manifest shape — a flat key-value file, no version constraints.

**What we borrow.**
- Factorio's prefix operator set, *minus the ones we don't need yet*. Our lifecycle §1 already has `?` (optional). We should add `!` (incompatibility) before any "this mod replaces stock content" mod ships — otherwise users will silently double-load conflicting overrides. `~` is a refinement we can defer until someone asks.
- The chain-length-then-name ordering rule. Lifecycle §1 already says "sortedByModId tiebreaker" — same idea.

**What we adapt.**
- Our `depends` is an object (`{modid: constraint}`) rather than Factorio's array of prefixed strings. Easier to parse with our existing nlohmann/json. The semantics are identical.

**What we reject.**
- Factorio's three-round-per-stage execution model (see Q2). It exists because their dep graph is large (literally thousands of mods on the portal). Our scale is "campaign mod, balance mod, asset pack" — three mods, maybe ten. Single-pass is sufficient.

---

## Q5 — Event hooks: how does the engine call into Lua?

### Pattern: handler-list-per-event, pcall-each-with-traceback, log-and-continue on failure

**OpenRA source:** [`OpenRA.Mods.Common/Scripting/Global/TriggerGlobal.cs`](https://github.com/OpenRA/OpenRA/blob/bleed/OpenRA.Mods.Common/Scripting/Global/TriggerGlobal.cs).

The reflection model is: any C# class tagged with `[ScriptGlobal("Name")]` extending `ScriptGlobal` becomes a Lua-accessible namespace; every public method on it becomes a Lua function under that namespace. The `[Desc(...)]` attribute supplies docstrings for tool-tip generation. The `[ScriptEmmyTypeOverride("fun()")]` attribute hints LuaLS-compatible types for mod editors.

**Concrete dispatch example (paraphrased from `TriggerGlobal.AfterDelay`):**

```csharp
[Desc("Call a function after a specified delay...")]
public void AfterDelay(int delay, [ScriptEmmyTypeOverride("fun()")] LuaFunction func) {
    var f = (LuaFunction)func.CopyReference();
    void DoCall() {
        try { using (f) f.Call().Dispose(); }
        catch (Exception e) { Context.FatalError(e); }
    }
    Context.World.AddFrameEndTask(w => w.Add(new DelayedAction(delay, DoCall)));
}
```

The pattern: copy the Lua function reference (so it survives across frames), wrap the invocation in a try/catch, schedule via the engine's frame-end queue, and let `Context.FatalError` route exceptions to a single sink. `World.AddFrameEndTask` is the load-bearing primitive — it's how the engine ensures Lua callbacks don't fire mid-tick where they could mutate state under iteration.

**Spring source:** `cont/LuaUI/widgets.lua` (path mirrors in BAR/Recoil). Engine calls into widget dispatcher via `_G[CallInName] = function(...) return selffunc(self, ...) end` (lines ~1057–1074 in current master). The dispatcher walks `widgetHandler[CallInName.."List"]` and pcalls each entry. Failed widgets get auto-disabled to prevent every-frame crash spam.

**Factorio source:** Closed engine, but the API is `script.on_event(defines.events.on_player_died, handler)`. Each event has a single handler per mod (mods that need fan-out wrap their own dispatcher). The C-side dispatch is a switch on event-id into a per-mod table; cross-mod fan-out is the mod's problem.

**What we borrow.**
- OpenRA's "schedule the Lua call to a frame-end queue" pattern. Our equivalent: the existing per-mission tick site (`Mission::update`, lifecycle §6) is the natural deferred-fire point. `mc2.on_event` should *enqueue* deliveries rather than calling synchronously inside whatever ABL primitive triggered the event.
- Spring's "auto-disable a repeatedly-failing widget" pattern. Useful for `brain_tick` (M1) where a buggy mod brain could crash 60×/s.
- OpenRA's reflection model is elegant but C#-specific. Sol2's `usertype` is the C++ equivalent — same mental model, different mechanics.

**What we adapt.**
- Factorio's one-handler-per-mod-per-event is ergonomic for control.lua but limiting. We follow Spring's many-handlers-per-event model: `mc2.on_event("mech_destroyed", fn)` appends to a list. This matches §1 lifecycle's existing language.

---

## Summary table

| Concern | Borrow verbatim | Adapt | Reject |
|---|---|---|---|
| Prototype API | `data:extend`-shaped registration | Single VM w/ stage gates instead of VM-per-stage | Three-round (data/updates/final-fixes) machinery |
| Inheritance | DFS w/ visited-set cycle detect (OpenRA) | JSON+Lua tables w/ `__index` instead of YAML pre-resolve | `Inherits@<suffix>` multi-parent grammar (deferred) |
| Sandbox | Open only `base/string/table/math` + `debug.traceback` | No synced/unsynced split (no MP) | Spring's LuaVFS (FST already does this) |
| Load order | Dep operators `?` `!`; chain-depth then name sort | Object-shaped `depends` not prefix-string array | Restrictive (`~`) operator (defer) |
| Event hooks | Many-handlers-per-event + pcall-each + auto-disable on repeat-fail | Frame-end-queue dispatch via existing Mission::update tick | One-handler-per-mod (Factorio) |
| Reflection model | OpenRA `[ScriptGlobal]` mental model | sol2 `usertype` (C++ equivalent) | C# attribute machinery (no .NET runtime) |

---

## Open questions that surfaced

1. **Multi-parent prototype inheritance.** OpenRA's `Inherits@<suffix>` is genuinely useful for "this unit is a Tank-with-Veterancy" composition. Worth pulling into our prototype model when override-policy (lifecycle §9.1) resolves? Or YAGNI for the campaign/balance/asset-pack scale we expect?
2. **Frame-end-queue vs synchronous Lua dispatch.** OpenRA defers; Spring (mostly) doesn't. The right answer depends on which ABL primitives can safely accept a deferred reply. Probably needs a separate exploration looking at the event sites in `warrior.cpp:2160` and the brain-tick path.
3. **Auto-disable threshold.** Spring auto-disables widgets after N consecutive errors. What's our N? Probably 3 errors in 60 frames for `brain_tick`-class events; never for `mission_start`-class one-shots. Defer to M1.
4. **Incompatibility (`!`) operator.** Worth adding to lifecycle §1's grammar before the first override-shipping mod lands. Cheap to add, expensive to retrofit semantics for after mods exist.
5. **`debug.traceback`-only opening.** Sol2's `lib::debug` is all-or-nothing; we'd need a manual two-step (open then strip). Add to impl-shape §6's sandbox spec when we touch it.

---

## References (GitHub paths)

- OpenRA `Inherits:` resolver: `OpenRA.Game/MiniYaml.cs` lines ~483–621 (functions `Merge`, `ResolveInherits`, `MergeIntoResolved`, `MergePartial`).
- OpenRA scripting globals: `OpenRA.Mods.Common/Scripting/Global/*.cs` (16 files, one per namespace). `TriggerGlobal.cs` is the canonical event-hook example.
- OpenRA script context: `OpenRA.Mods.Common/Scripting/LuaScript.cs` (trait shell) + `ScriptContext.cs` (VM owner).
- Factorio data API: `wube/factorio-data` `core/lualib/dataloader.lua` (`data:extend`), `core/lualib/util.lua` (table.deepcopy, color helpers).
- Factorio lifecycle docs: `https://lua-api.factorio.com/latest/auxiliary/data-lifecycle.html`, `mod-structure.html`.
- Spring/Recoil sandbox: `beyond-all-reason/RecoilEngine` `rts/Lua/LuaHandle.cpp` (synced/unsynced sets), `rts/Lua/LuaHandleSynced.cpp`, `cont/LuaUI/widgets.lua` (widget dispatcher).

All citations are read-only; no code in those repos was modified by this exploration.
