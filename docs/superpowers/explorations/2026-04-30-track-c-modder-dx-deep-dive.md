# Track C — Modder Developer Experience (in-game side) Deep-Dive

**Date:** 2026-04-30
**Mode:** Design only. No code changes.
**Predecessors:**
- [`2026-04-30-track-c-lua-trampolines-and-tests.md`](2026-04-30-track-c-lua-trampolines-and-tests.md) — `MC2_LUA_TRACE`, `BindingRegistry`, protected-call envelope
- [`2026-04-30-track-c-lua-loading-lifecycle.md`](2026-04-30-track-c-lua-loading-lifecycle.md) — `LuaVM::reloadFromDisk()`, stage gates, persist
- [`2026-04-30-track-c-modder-tooling-deep-dive.md`](2026-04-30-track-c-modder-tooling-deep-dive.md) — editor side (LuaLS, stubs, snippets)
- [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §5.3 hot-reload contract, §5.5 ActionRegistry

The other tooling deep-dive covers the editor-side surface — autocomplete, type stubs, `tools/new-mod`. Once a modder has typed their script and pressed save, they hit the engine. This doc is everything that happens after that: the `/c` console, in-engine error surface, profiler hooks, hot-reload UX, state inspector, debug overlays, mod manager.

The frame throughout is the **midnight test:** I am tired, my mod just lost a unit reference somewhere between two events, the log has 4000 lines, and I want to find the bug in the next twenty minutes. Every design decision below is justified by that scenario or it is removed.

---

## 1. REPL design — `LuaConsole`

Borrowed wholesale from Factorio's `/c` chat command. It is the single most-used debugging tool in the entire RTS-modding ecosystem, and copying it costs us almost nothing.

### Trigger

Two entry surfaces, both feed the same `LuaConsole::Submit()`:
- **Hotkey:** `` ` `` (backtick) toggles the ImGui console window. Same key as Quake / Source / most engines; modders' muscle memory hits it instinctively.
- **Chat-style command:** if Track B's chat input ever lands, a leading `/c ` prefix routes the rest of the line to `Submit()`. Lower-priority; the hotkey is the primary path because Track C ships before any chat surface.

A third surface — programmatic `mc2.dev.run(src)` from another mod — is *not* exposed. Mods that want to run arbitrary Lua already can; they don't need a backdoor.

### Auth gate

Default OFF in shipping builds. Enabled by ANY of:
- `MC2_LUA_REPL=1` env var
- `--enable-repl` CLI flag (parsed in `main.cpp` next to the existing `--profile` / `--dump-lua-api` flags)
- `s_devModeFlag` (already present per loading-lifecycle §5; lit by `MC2_LUA_DEV=1`)

Resolution: `LuaConsole::Enabled()` returns the OR. The hotkey is silently inert when disabled — no "press ` to enable dev mode" hint. Discoverability for shipping users is intentionally zero; the hint lives in `docs/modding/repl.md`.

### UI shape — ImGui

ImGui-based, gated on Track B's bridge being live. Until B0 lands, a stdio fallback (the engine reads stdin in a worker thread) keeps the REPL functional from a console launched alongside the game window. The fallback is two pages of code; ship it day one so the REPL is not blocked on Track B.

```cpp
// modding/lua_console.h
namespace mc2lua {

struct LuaConsoleResult {
    std::string output;     // captured stdout from the submission
    bool        is_error;   // true if compilation or execution failed
};

class LuaConsole {
public:
    static LuaConsole& Instance();

    bool Enabled() const;             // gated by env / flag / s_devModeFlag
    void Toggle();                    // bound to backtick
    bool IsOpen() const;

    // Per-frame ImGui draw. Slot in Track B's overlay chain after post-process.
    void Draw();

    // Programmatic submission (used by hotkey, chat command, tests).
    LuaConsoleResult Submit(const std::string& code);

    // Sink hook for print() / mc2.log / [LUA ERROR] capture (§8).
    void AppendLine(const char* line, int severity);

    // History recall (up/down arrow); persisted across runs.
    void SaveHistory();
    void LoadHistory();
};

} // namespace mc2lua
```

### Multi-line input

Lua REPL idiom (Lua 5.4 `lua.c:loadline`): try to compile the buffer; if `lua_load` returns the magic incomplete-chunk error message ending in `<eof>`, the prompt switches to continuation mode (`>>` instead of `>`) and waits for more lines. We mirror this exactly:

```cpp
LuaConsoleResult LuaConsole::Submit(const std::string& src) {
    pending_ += src;
    pending_ += '\n';
    sol::load_result lr = state_.load("return " + pending_); // try expr first
    if (!lr.valid()) {
        lr = state_.load(pending_);                          // fall back to stmt
    }
    if (!lr.valid()) {
        sol::error e = lr;
        std::string msg = e.what();
        if (msg.find("<eof>") != std::string::npos) {
            return { "", false };  // incomplete; UI shows ">>" continuation
        }
        pending_.clear();
        return { msg, true };
    }
    pending_.clear();
    sol::protected_function pf = lr;
    sol::protected_function_result r = pf();
    return { CaptureSinkFlush(), !r.valid() };
}
```

The `"return " + buf` first-try is the Lua REPL convention that lets a modder type `1+2` and get `3` instead of "syntax error near '+'".

### History

Up/Down arrow recalls. Persisted to `<install>/mods/.lua_repl_history` (a sibling to `.bash_history`). Cap at 1000 entries, deduped consecutively. **Loaded at engine boot, saved at console close, autosaved every 5 entries.** History is per-install, not per-mod — a modder's recall doesn't reset when they start work on a new mod.

### Output rendering

Append-only scrollback buffer (`std::deque<Line>` capped at 8192 lines, FIFO trim). Each line tagged with severity → color:
- `INFO` (white) — `print()`, `mc2.log` no-tag
- `WARN` (yellow) — `mc2.log(WARN, ...)`, deprecation warnings
- `ERROR` (red) — `[LUA ERROR]` lines, `pcall` failures
- `TRACE` (grey) — `[LUA_TRACE v1]` when `MC2_LUA_TRACE=1`
- `INPUT` (cyan) — echo of submitted input, prefixed `> `

Auto-scroll-on-bottom (Discord/IRC pattern): if the user scrolls up to read history, new lines append silently; `Ctrl-End` or scrolling back to the bottom resumes auto-follow. Midnight test: scrolling up to read an error and getting yanked back down on the next log line is infuriating; this convention is non-negotiable.

### `serpent` table printer

Lua tables are unprintable by default (`tostring({})` → `"table: 0x..."`). Modders need pretty-printing. Two options: vendor [Pavel Kulchenko's `serpent`](https://github.com/pkulchenko/serpent) (single ~150-line Lua file, MIT license, the de-facto choice) or hand-roll a 40-line walker. **Vendor `serpent`** — it handles cycles, sparse arrays, key sorting, and a `block`/`line` mode switch that we'd otherwise reinvent. Drop it at `mods/.builtin/serpent.lua`, autoloaded into the REPL env.

Convenience: bind `?` as a one-character alias for `serpent.block` so `?global` pretty-prints the global state. (Implementation: REPL preprocesses input — if the trimmed line starts with `? `, rewrite to `print(serpent.block(...))`.)

### Tab-completion

Walk the `BindingRegistry` to enumerate `mc2.*` paths plus inspect `_G` for user-defined names. Naive prefix match on the cursor word; ImGui draws a popup. Skip generic-ranking heuristics in v1 — a modder typing `mc2.obj` and seeing `object`/`objective` listed alphabetically is sufficient.

### ImGui frame integration

`LuaConsole::Draw()` slots into Track B's draw chain **after** the post-process resolve and **before** the `ImGui::Render()` flush — same place the future inspector lives. Window is non-modal, default-pinned bottom-third of the viewport, resizable, dockable. Modder can drag it anywhere and the position persists via ImGui's own `imgui.ini`.

---

## 2. Profiler integration — `mc2.profiler`

Tracy is in-tree (`memory/tracy_profiler.md`, `3rdparty/tracy/tracy/TracyC.h`). Modders need first-class access.

### Lua API

```lua
mc2.profiler.zone("MyMod.UpdateAI", function()
    -- expensive work
    for i, w in ipairs(warriors) do compute_path(w) end
end)

-- Or scoped:
local z = mc2.profiler.zone_begin("MyMod.HotPath")
do_work()
mc2.profiler.zone_end(z)

-- Per-frame plot:
mc2.profiler.plot("MyMod.queue_size", #queue)
```

### C++ binding shape

```cpp
// modding/lua_profiler_bindings.cpp
#include "tracy/TracyC.h"

namespace mc2lua {

struct LuaTracyZone { ___tracy_c_zone_context ctx; bool live; };

static int lua_profiler_zone_begin(sol::this_state ts, sol::object name) {
    if (!name.is<std::string>()) return 0;
    const std::string& n = name.as<std::string>();
    // TracyAlloc'd source-loc; modders' zone names are dynamic.
    auto* loc = ___tracy_alloc_srcloc_name(0, "<lua>", 5, n.c_str(), n.size(), 0);
    auto ctx  = ___tracy_emit_zone_begin_alloc(loc, 1);
    int  id   = NextZoneId();
    g_LuaVM->RegisterZone(id, LuaTracyZone{ ctx, true });
    return id;
}

static void lua_profiler_zone_end(int id) {
    auto* z = g_LuaVM->LookupZone(id);
    if (!z || !z->live) return;
    ___tracy_emit_zone_end(z->ctx);
    z->live = false;
    g_LuaVM->ReleaseZone(id);
}

static void lua_profiler_zone(sol::this_state ts, sol::object n, sol::function fn) {
    int id = lua_profiler_zone_begin(ts, n);
    sol::protected_function_result r = fn();        // exceptions caught
    lua_profiler_zone_end(id);
    if (!r.valid()) {
        sol::error e = r;
        printf("[LUA ERROR] profiler.zone: %s\n", e.what());
    }
}
```

`mc2.profiler.plot("name", value)` wraps `___tracy_emit_plot`. Names are interned at first sight (`std::unordered_set<std::string>` keeping stable `c_str()` pointers — Tracy requires the name string to outlive the program).

### Per-frame Lua-call counter

`LuaVM` increments an atomic counter on every binding entry; per frame the value is plotted as `mc2.lua.calls_per_frame`. Visible in Tracy's plot viewer next to FPS. A spike from 200 → 50000 on one frame is a textbook "modder accidentally wrote `for _=1,100000 do mc2.x() end`" signature.

### Per-mod budget

`LuaVM` tracks a per-mod ms-per-frame counter (sum of `clock()` deltas across all bindings called during the frame, attributed by the calling environment's mod-id sentinel). At end-of-frame:

```cpp
for (auto& [modId, ms] : per_mod_ms_) {
    if (ms > 1.0f) {  // 1 ms/frame budget
        printf("[LUA_PERF v1] event=mod_over_budget mod=%s ms=%.2f\n",
               modId.c_str(), ms);
    }
}
```

The log is throttled to once per second per mod-id (avoid per-frame spam). Visible in the mod-manager inspector (§7) as a colored bar — green <0.5 ms, yellow 0.5–1.0 ms, red >1.0 ms.

Tracy plot mirror: `mc2.lua.<mod_id>.ms` so a modder can correlate spikes with engine zones.

### Borrow

Spring/BAR's [widget profiler](https://springrts.com/wiki/Lua:Widgets#Profiling) is the precedent — Spring's `widgetHandler:UpdateCallIns()` measures every widget every frame and surfaces ms/frame in a corner overlay. We don't need the corner overlay (Tracy is better) but we do borrow the per-mod accounting discipline.

---

## 3. Visible error surface

Three layers, escalating from "always" to "dev only":

### Layer 1 — log line (always)

`[LUA v1] event=error mod=<id> stage=<data|control|tick> msg="<one-line>"` already designed. Goes to stdout and the REPL output buffer (§8).

### Layer 2 — ImGui banner (dev only, default 5s)

```cpp
struct ErrorBanner {
    std::string mod_id;
    std::string short_msg;          // first line, ≤120 chars
    std::string full_traceback;
    float       remaining_seconds;
};
```

Top-of-viewport red strip, monospace, fixed-height (32 px). Stacks to a maximum of 3 simultaneous banners; older ones slide off as new ones arrive. Click to expand → modal popup with full Lua traceback (`debug.traceback()` captured at error site by the `protected_function` wrapper). Click outside → dismiss. Banners auto-fade after 5 s; this is configurable per-mod via `mc2.dev.banner_seconds`.

The banner is **not** modal — gameplay continues. A modder iterating on a mid-mission feature should not be locked out of the camera by their own error. Midnight test: a banner that pauses the game on every error makes me close the editor and ragequit.

### Layer 3 — persistent error log (dev only)

REPL's output buffer captures `[LUA v1] event=error` lines verbatim. Modder presses `` ` ``, scrolls back, reads the full chain. The persistent buffer survives mission-end → next-mission-start; a save/load doesn't clear it. Cleared only on engine shutdown or explicit `mc2.dev.clear_log()` from REPL.

### Severity classification

- **Data-stage error** → mod-fatal. The mod is added to the `skipped` set; cascade applies. Banner color: red. Survives until next `data.lua` reload.
- **Control-stage error during `LoadControlStage`** → mod-fatal. Same treatment.
- **Control-stage error during `Tick()` / event dispatch** → event-fatal. Mod stays loaded; the specific callback is logged and skipped. Banner color: yellow. After the third occurrence in 60 s, the engine auto-disables that handler with a `[LUA v1] event=handler_disabled reason=repeated_error` line and prompts (in the banner) "Click to re-enable." Modder fixes their code, hot-reloads, the handler re-registers with `mc2.on_event` and is live again.

The auto-disable saves the mission from a Lua handler that throws every tick — without it, the log fills with 60 errors/second and the banner queue never drains. This pattern is borrowed from Bevy's [error-resilient systems](https://bevy-cheatbook.github.io/programming/system-order.html) and from Spring's widget-watchdog.

---

## 4. Hot-reload UX

The §5.3 contract (`LuaVM::reloadFromDisk(path) -> bool`) is already designed. Four user-facing triggers feed it:

### Trigger A — auto-watch (default ON in dev)

Filesystem watcher on every loaded mod's `scripts/` directory. On any `.lua` file save (modify event), debounce 250 ms (Sublime Text saves twice in rapid succession; debouncing avoids redundant reloads), then call `reloadFromDisk()`.

Cross-platform shape:

```cpp
// modding/file_watcher.h
class FileWatcher {
public:
    using Callback = std::function<void(const std::string& abs_path)>;
    bool AddDirectory(const std::string& dir, bool recursive);
    void Poll();   // call once per frame from the main thread
    void SetCallback(Callback cb);
};
```

Implementation:
- **Windows:** one worker thread per watched root, calling `ReadDirectoryChangesW` with `FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME`. Events posted to a lock-free SPSC queue (Tracy's `tracy_SPSCQueue.h` is already in tree — reuse it).
- **Linux:** single `inotify_init1(IN_NONBLOCK)` fd + a `select`/`poll` in the main thread's `Poll()`. `IN_MODIFY | IN_MOVED_TO | IN_CREATE`.
- **macOS** (not currently a target, but the API gap is worth noting): `FSEvents` or kqueue; deferred.

SDL has no file-watcher wrapper, confirmed; we ship our own. The class is ~150 LoC each per platform; both implementations sit behind the same header. Gated by `s_devModeFlag` — production runs do not start the watcher thread.

### Trigger B — F5 hotkey

Bound through Track B's input system. `gos_GetKey` non-consumption (memory: `gos_getkey_non_consuming.md`) means we drain the F5 edge correctly via the existing `FIRST_PRESSED` pattern. F5 calls `g_LuaVM->reloadFromDisk()` on every loaded mod's `control.lua` in topological order. Status line in the banner: `[LUA v1] event=hot_reload_all status=ok mods=12 ms=43`.

### Trigger C — inspector button

Mod-manager UI (§7) has a per-mod "Reload" button. Same call.

### Trigger D — REPL command

`/c mc2.dev.reload("modid")` or `/c mc2.dev.reload_all()`. Same call.

### Reload UX details

- Reload preserves `mc2.persist[mod_id]`. The §5.3 doc establishes that `control.lua` re-run wipes user-defined functions and re-registers handlers; the persist table survives by being engine-owned.
- Pending timers are cleared on reload (otherwise an old callback ref outlives the function it pointed at — `sol::function` is fine but the modder's mental model is cleaner if reload starts fresh).
- A reload during a `Tick()` is queued: the watcher sets a `pending_reload_set`, and `LuaVM::Tick()` drains it at end-of-frame, **after** any in-flight callbacks finish. This prevents reload-while-callback-stack-is-live UB.
- Banner on completion: green-tinted info banner `Reloaded megamod (43 ms)`. Fade 2 s.

### Borrow

Factorio's `--instrument` doesn't expose hot-reload to modders directly; the borrow here is Spring's `/luarules reload` console command, plus Bevy's [hot-reload-via-asset-server](https://bevy-cheatbook.github.io/assets/hot-reload.html) pattern. Watch + manual + REPL trigger all funnel into one entry point.

---

## 5. In-game state inspector

`mc2.persist[mod_id]` is engine-readable. ImGui tree view, accessible from the mod manager (§7) per mod or as a standalone window via `Ctrl-` ` ` (Ctrl+backtick).

### Renderer

Recursive walk:

```cpp
void DrawLuaTable(sol::table t, const char* label) {
    if (ImGui::TreeNode(label)) {
        std::vector<sol::object> keys;
        for (auto& kv : t) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end(), LuaKeyLess);  // stable display
        for (auto& k : keys) {
            sol::object v = t[k];
            std::string ks = SerpentInline(k);
            switch (v.get_type()) {
                case sol::type::table:    DrawLuaTable(v.as<sol::table>(), ks.c_str()); break;
                case sol::type::number:   ImGui::Text("%s = %g", ks.c_str(), v.as<double>()); break;
                case sol::type::string:   ImGui::Text("%s = %s", ks.c_str(), v.as<std::string>().c_str()); break;
                case sol::type::boolean:  ImGui::Text("%s = %s", ks.c_str(), v.as<bool>()?"true":"false"); break;
                case sol::type::function: ImGui::TextDisabled("%s = <function>", ks.c_str()); break;
                default:                  ImGui::TextDisabled("%s = <%s>", ks.c_str(), TypeName(v)); break;
            }
        }
        ImGui::TreePop();
    }
}
```

Functions/userdata/threads render as disabled "non-serializable" entries. Numbers >7 digits get scientific notation. Strings >80 chars truncate with `...` and a hover tooltip showing full content.

### Read-only by default

Edit mode is a per-window toggle, default off. With edit on: each leaf gets an `ImGui::InputText` / `InputDouble` / `Checkbox`. Submit writes back via `t[key] = new_value`. **Tables cannot be added/removed via the inspector** (would require schema knowledge); only existing leaves are mutable.

### Search/filter

Top-of-window text input filters by substring match against keys (case-insensitive). Subtrees with no matches collapse; matches highlight yellow. Matches the Bevy egui inspector's filter pattern.

### Snapshot to JSON

Button "Snapshot to file" → walks the tree, emits `mc2.persist.<mod>.YYYYMMDD-HHMMSS.json` next to the savegame directory. Diffable with `git diff --no-index` between two snapshots; the workflow is "before vs after one event."

### Live update

Inspector polls the table every frame (cheap — ImGui only walks expanded nodes). No subscription or change-detection layer; the inspector being live-by-default means a modder can watch a counter increment as the event fires. Midnight test: opening the inspector and seeing nothing change because of a stale snapshot would lead to chasing a phantom bug.

### Borrow

Bevy's [`bevy-inspector-egui`](https://github.com/jakobhellermann/bevy-inspector-egui) — its recursive-component-browser pattern is what we copy. Its filter behavior, edit-mode toggle, and snapshot-to-JSON are all drop-in.

---

## 6. Debug overlays

Modders register in-world visualizations. Behind a per-mod debug toggle in the mod manager.

### Lua API

```lua
mc2.debug.draw_circle(world_x, world_y, radius, color)   -- color: 0xRRGGBBAA
mc2.debug.draw_line(x0, y0, z0, x1, y1, z1, color)
mc2.debug.draw_text(world_x, world_y, "patrol point", color)
mc2.debug.draw_box(min_xyz, max_xyz, color)
```

All calls are **per-frame, immediate-mode** — modder calls them inside `mc2.on_event("warrior_tick", ...)` or similar; they last for one frame. Persistent overlays are a layer-up convenience and not v1.

### C++ shape

The existing `TerrainQuad::drawLine` (mclib/quad.cpp:3351) is a terrain-grid line drawer in screen space — wrong layer. The needed primitive is a world-space immediate-mode batcher.

```cpp
// modding/debug_draw.h
namespace mc2dbg {
struct DebugLine { float p0[3], p1[3]; uint32_t color; };
struct DebugText { float pos[3]; uint32_t color; std::string s; };

class DebugDraw {
public:
    static DebugDraw& Instance();
    void AddLine(const float a[3], const float b[3], uint32_t c);
    void AddCircleXY(float cx, float cy, float r, uint32_t c, int seg=24);
    void AddText(const float pos[3], uint32_t c, std::string s);
    // Called once per frame after main scene, before HUD.
    void Flush();
    // Called at end-of-frame to clear immediate-mode buffers.
    void Reset();
    // Per-mod gate; calls drop on the floor when disabled.
    void SetModEnabled(const std::string& modId, bool on);
    bool ModEnabled(const std::string& modId) const;
};
}
```

`AddLine` writes into a single `std::vector<DebugLine>` (no per-mod separation in storage; the gate filters at API entry). `Flush()` issues one `glDrawArrays(GL_LINES, ...)` against a streamed VBO with a flat-color shader. `AddText` uses the existing GameOS bitmap-font path; render slot is after the world but before the HUD's gos_State_IsHUD buffering.

The Lua bindings are 30 lines of trampolines that read coords, look up the calling mod's gate state, and forward to `DebugDraw::Instance().AddLine(...)`.

### Per-mod toggle

Mod manager has a "Debug overlays" checkbox per mod. Default off. State persisted in `imgui.ini`-style or a small `.mod_debug_state` file. From REPL: `mc2.dev.debug_overlay("modid", true)`.

### Borrow

Spring's `Spring.MarkerAddPoint` and Source's `debugoverlay` console command are the precedents. Both are per-frame immediate-mode + a hidden persistent layer. We ship the immediate-mode half v1, defer persistent.

---

## 7. Mod manager UI

ImGui window, gated on Track B. Single window with three tabs: **Mods**, **REPL** (if not already a separate window), **Inspector**.

### Mods tab — list view

Per-mod row:

| Column           | Content                                                |
|------------------|--------------------------------------------------------|
| Status icon      | green-check / yellow-warn / red-error / grey-disabled  |
| Mod id + version | bold; click to expand row                              |
| Stage            | `Data` / `Control` / `Mission` / `Skipped`             |
| ms/frame         | colored number, per §2 budget                          |
| Actions          | `Reload` `Disable` `Open Folder` `View Log` `Inspect`  |

Expand-on-click reveals manifest fields, dependency chain, last error message, persist-table size in bytes.

### Action buttons

- **Reload** → `g_LuaVM->reloadFromDisk(controlLuaPath)`. Banner on result.
- **Disable** → adds modid to runtime-disabled set, calls `clearHandlersFor(modid)`. Re-enable next mission start.
- **Open Folder** → `ShellExecuteA(NULL, "open", path, ...)` (Windows) / `xdg-open` (Linux). Spawns the OS file explorer at the mod root.
- **View Log** → switches to a filtered REPL view (lines tagged `mod=<id>`).
- **Inspect** → opens the §5 state inspector window, scoped to `mc2.persist[modid]`.

### Reorder load order

Drag-and-drop within the list. Reorder is **subject to the §1 dependency-graph constraints** in the loading-lifecycle doc — the topological sort still wins. The drag is a *preference* used as the secondary tiebreaker (sorted-by-modid → sorted-by-user-preference). Validation: an invalid drag (would violate deps) snaps back with a one-line tooltip "blocked by: depends on `core`".

### Window state

Pinned position; persists via ImGui's `imgui.ini`. Default-closed in dev, hotkey-toggleable via `Ctrl-Shift-M`.

---

## 8. Console output capture

`print()`, `mc2.log()`, and `[LUA *]` log lines need to land in three places: stdout, the engine log file (already writes to `[LUA INFO]`-prefixed lines), and the REPL output buffer. Multi-sink writer:

```cpp
// modding/lua_log_sink.h
class LuaLogSink {
public:
    static LuaLogSink& Instance();
    void Write(int severity, const char* fmt, va_list args);
    void RegisterSink(std::function<void(int sev, const char* line)> sink);
};
```

Three sinks registered at boot:
1. `stdout` writer (always)
2. log file writer (always; matches existing engine log path)
3. REPL append (only when REPL is enabled)

`mc2.log` and the existing `[LUA *]` printf paths route through `LuaLogSink::Write`. The REPL append is **non-blocking**: it pushes to a lock-free SPSC queue (the same Tracy queue we reuse in §4); `LuaConsole::Draw()` drains the queue at frame start. Guarantee: a slow ImGui frame never blocks a Lua `print()`.

Volume cap: each frame's drain processes at most 256 lines. Excess is dropped with one summary line "[LUA WARN] log_sink dropped <N>". Without this cap a runaway loop logging at 50 kHz starves the inspector renderer.

---

## 9. Best-practice borrows

Each line is one design decision and the source we copied it from.

- **Backtick-toggled console window** — Quake / Source. Universally muscle-memoried.
- **`/c` chat-prefix command form** — [Factorio Console](https://wiki.factorio.com/Console). The two-letter prefix-as-shibboleth distinguishes "command" from "broadcast"; we keep `/c` even though we don't have multiplayer chat, for cross-game muscle memory.
- **`serpent` table printer** — [pkulchenko/serpent](https://github.com/pkulchenko/serpent). De-facto Lua table pretty-printer. MIT, vendor.
- **`?expr` shorthand for inspecting a value** — Ruby IRB / Pry, Lua REPL prior art. Saves four characters every line.
- **Continuation prompt `>>` for incomplete chunks** — standard Lua REPL (`lua.c:loadline`). Modders writing `function foo()` know to keep typing.
- **Per-widget ms/frame profiler** — Spring/BAR widget watchdog. We borrow the per-mod budget + auto-disable on repeated error.
- **Recursive component inspector with edit mode + filter** — [bevy-inspector-egui](https://github.com/jakobhellermann/bevy-inspector-egui).
- **Hot-reload contract: any of {watcher, hotkey, button, REPL} → one entry point** — Bevy's `AssetServer` reload + Spring's `/luarules reload`.
- **Persistent error banner with click-to-expand traceback** — Unity Console window, condensed.
- **Auto-disable handler after N errors in window** — Spring widget watchdog (`widgetHandler:RemoveWidget` on repeated callin failure).
- **`debug_draw` immediate-mode in-world overlay primitives** — Source `debugoverlay` console var, Spring `Spring.MarkerAddPoint`.
- **Mod manager with reorderable list + per-mod action buttons** — Factorio mod-manager UI. Drag-with-dep-validation snapback is theirs.
- **History persisted across sessions** — bash, every IDE REPL since 2005.

---

## 10. Open questions

1. **Multi-line input UX in ImGui.** ImGui's `InputTextMultiline` is bulky for a one-liner; the `>>` continuation pattern wants single-line input that *visually* shows pending lines above. Probably: a read-only buffer above the active `InputText`, both inside one ImGui group. Prototype before committing.
2. **Tab-completion ranking.** Pure prefix match is fine v1, but when a modder types `mc2.o` and gets `object`, `objective`, `on_event` they want frequency-weighted ranking. Defer; revisit if a modder complains.
3. **REPL sandbox parity.** The REPL evaluates submissions in the same `sol::state` as mods, but in *which* environment? Probably a special `_DEV_REPL` env that has all mods' globals visible, plus shortcuts to `mc2.persist`. Resolve in C-3.
4. **Reload-while-paused vs reload-while-running.** Pause-only is safer (no callbacks fire mid-reload); always-on is what modders actually want. Loading-lifecycle §5 dev gate covers `data.lua`; control.lua reload is currently unrestricted. Should it pause briefly during the swap? Probably: a one-frame `mission.pause()` around the reload, transparent to gameplay.
5. **Profiler zone-id leak.** A Lua mod that calls `zone_begin` without `zone_end` leaks a Tracy context. Mitigation: cap per-frame outstanding zones at 64; on overflow, force-end the oldest with a `[LUA WARN] profiler zone leaked` line. Track as M1 polish.
6. **Banner dismissal during fast retry.** Modder hot-reloads, reads banner, fixes mod, hot-reloads — the previous banner may still be on screen. Expected behavior: a successful reload that touches the same mod auto-dismisses any error banner for that mod. Implement.
7. **State inspector edit-mode and types.** A modder edits `score = 5` to `score = "five"` and the next event tick errors. Should the inspector type-check against the current value's type? Probably yes (warn-on-type-change, allow with confirm). Defer; v1 ships unchecked.
8. **Cross-platform clipboard for REPL.** Copy/paste of submitted lines and output should work on both Windows and Linux. ImGui's clipboard hooks via `SDL_SetClipboardText` cover this when SDL is the input backend; verify Track B's input plumbing wires it through. Already-known-good per ImGui docs but worth a smoke test.
9. **Overlay lifetime model.** §6 ships immediate-mode only; modders will quickly want `mc2.debug.draw_circle_persistent(id, ...)`/`clear(id)`. Add in M1 with an explicit handle-keyed store; not blocking v1.
10. **Hotkey conflict with editor.** Backtick is also used by some keyboard layouts (AZERTY) as a dead-key. Provide a `MC2_REPL_HOTKEY=F1` env override; make the default rebindable in the mod manager's settings tab.

---

## 11. References

Predecessor docs:
- `docs/superpowers/explorations/2026-04-30-track-c-lua-trampolines-and-tests.md` — `MC2_LUA_TRACE`, `BindingRegistry`, protected-function envelope (REPL borrows the same envelope).
- `docs/superpowers/explorations/2026-04-30-track-c-lua-loading-lifecycle.md` §5 — `LuaVM::reloadFromDisk()`, dev-mode gate.
- `docs/superpowers/explorations/2026-04-30-track-c-modder-tooling-deep-dive.md` — editor-side counterpart.
- `docs/superpowers/specs/2026-04-29-modders-paradise-roadmap-design.md` §5.3 hot-reload, §5.5 ActionRegistry, Track B sub-slice sequence.

Engine code:
- `3rdparty/tracy/tracy/TracyC.h` — `TracyCZoneN`, `___tracy_alloc_srcloc_name`, `___tracy_emit_zone_begin_alloc`, `___tracy_emit_plot`.
- `3rdparty/tracy/tracy/tracy_SPSCQueue.h` — lock-free queue reused for both file-watcher events and log-sink drain.
- `mclib/quad.cpp:3351` — existing `TerrainQuad::drawLine` (terrain-grid screen-space line; **not** the right layer for `mc2.debug.draw_*`, used as reference for the new world-space batcher).

Memory:
- `memory/tracy_profiler.md` — always-on Tracy compile, 18 zones already live; modder zones extend the same pipeline.
- `memory/debug_instrumentation_rule.md` — env-gated trace pattern; `MC2_LUA_REPL`, `MC2_LUA_DEV` follow the same convention.
- `memory/gos_getkey_non_consuming.md` — F5 hotkey edge handling.
- `memory/feedback_hotkeys.md` — never use Alt+F4; backtick chosen as primary hotkey to avoid conflict.

External:
- Factorio Console — <https://wiki.factorio.com/Console>
- pkulchenko/serpent — <https://github.com/pkulchenko/serpent>
- bevy-inspector-egui — <https://github.com/jakobhellermann/bevy-inspector-egui>
- Spring/BAR widget profiler — <https://springrts.com/wiki/Lua:Widgets#Profiling>
