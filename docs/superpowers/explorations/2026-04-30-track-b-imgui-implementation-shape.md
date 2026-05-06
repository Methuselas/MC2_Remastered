# Track B — ImGui Implementation Shape (B0–B3 MVP)

**Date:** 2026-04-30
**Scope:** Implementation-shape document for Track B sub-slices B0 through B3, targeting the Viewer (Mechlopedia) MVP gate. Read-only research; no code edits.
**Predecessors:** [`2026-04-29-track-b-imgui-hotreload-status.md`](2026-04-29-track-b-imgui-hotreload-status.md), [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md), `memory/imgui_fit_ui_design.md`.

---

## Headline finding (B1 scope-mover) — FIT IS AN EXISTING FORMAT

**FIT files are not a new modder DSL. They are MC2's existing `FitIniFile` block format**, parsed by `mclib/inifile.{h,cpp}` (`class FitIniFile : public File`, `inifile.h:63`). Wolfman, Omnitech, and the stock install all already ship FIT files (e.g. `mcl_en.fit`, `mc_logistics.fit`); every existing screen — Mechlopedia (`code/mechlopedia.cpp:54-66`), Logistics, MechBay, MissionBriefing — already opens a `FitIniFile` and feeds it to `LogisticsScreen::init(file, "Static", "Text", "Rect", "Button")` (`gui/logisticsscreen.cpp:94`), which seeks the named blocks and instantiates the legacy widget grid.

**Implication for B1:** the FIT *parser* is reuse, not rewrite. Track B1 is "FIT → UI model IR" — a *projection* layer that walks the same `FitIniFile` block API the legacy `LogisticsScreen::init` walks, and emits a `UILayout` IR. The legacy parser stays — `FitIniFile::seekBlock`, `readIdString`, `readIdLong`, etc. are the API. The work is the **adapter**, not a tokenizer.

This collapses B1 from "weeks of parser work" to "days of an enumerator that mirrors what `LogisticsScreen::init` already does." Surface this loudly to Methuselas — it changes the slice's effort estimate by an order of magnitude.

---

## B0 — ImGui bridge

### Vendoring layout

Match the Tracy precedent (`3rdparty/tracy/TracyClient.cpp` + headers, single-CU build). Vendor under:

```
3rdparty/imgui/
  imgui.h
  imgui.cpp
  imgui_internal.h
  imgui_widgets.cpp
  imgui_tables.cpp
  imgui_draw.cpp
  imgui_demo.cpp           (kept; first ImGui::ShowDemoWindow() proves bridge)
  imconfig.h               (project-local; sets IM_ASSERT to gosASSERT)
  backends/
    imgui_impl_sdl2.h
    imgui_impl_sdl2.cpp
    imgui_impl_opengl3.h
    imgui_impl_opengl3.cpp
    imgui_impl_opengl3_loader.h   (only if not already provided by GLAD)
```

Pin to ImGui v1.90.x docking branch (matches what the editor track will eventually want; stable API). Source: `https://github.com/ocornut/imgui` tagged release tarball, copied verbatim, no submodule (matches the `3rdparty.zip` convention from the root tree and the Tracy verbatim copy).

CMake integration: append the seven `imgui*.cpp` + two backend `.cpp` files to the existing `gameos` library target (same target Tracy is folded into per `docs/plans/2026-04-11-tracy-profiler-impl.md`). No `add_subdirectory` — direct source list. Add `3rdparty/imgui/` and `3rdparty/imgui/backends/` to `target_include_directories(gameos PUBLIC …)`. Backends compile against the existing SDL2 (`3rdparty/cmake/sdl2-config.cmake`) and the existing GL loader.

### `mc2_imgui_bridge.{h,cpp}`

New TU under `GameOS/gameos/mc2_imgui_bridge.{h,cpp}`. Owns the global `ImGuiContext*` (one per process), the SDL2 backend init state, the OpenGL3 backend init state, and a single `bool s_imguiCapturedInput` flag exposed for the input gate (§ below).

```cpp
// GameOS/gameos/mc2_imgui_bridge.h
#pragma once
#include <SDL2/SDL_events.h>

namespace mc2_imgui {

// Call once after SDL window + GL context exist, before first frame.
// glsl_version_string is the same "#version 430\n" prefix the shader system uses.
bool Init(struct SDL_Window* win, void* gl_ctx, const char* glsl_version_string);

// Call from process_events() BEFORE the existing case dispatch.
// Returns true iff ImGui consumed the event AND the legacy input
// system should skip it (typing in a text field, click on a window).
bool ProcessEvent(const SDL_Event* ev);

// Call once per frame, AFTER input::updateKeyboardState() and BEFORE any
// ImGui::Begin() calls in feature code. Wraps:
//   ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
void BeginFrame();

// Call once per frame, AFTER all ImGui::Begin/End() pairs, AFTER
// gosPostProcess::endScene() composited the scene to FB0, BEFORE
// gos_RendererFlushHUDBatch() replays HUD draws. Wraps:
//   ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
void EndFrame();

// Tear down on engine shutdown, before SDL_GL_DeleteContext.
void Shutdown();

// Cheap accessors (forwarded to ImGui::GetIO()) for the input gate.
bool WantCaptureKeyboard();
bool WantCaptureMouse();

}  // namespace mc2_imgui
```

State the bridge owns: the context pointer, the bool returned from `Init`, the `glsl_version_string` (so reload-on-context-loss can re-init with same version). No widget state — that lives in feature TUs.

### Frame-loop integration (the load-bearing question)

Existing call sites, all in `GameOS/gameos/gameosmain.cpp::draw_screen()`:

| Line | Existing call | ImGui slot |
|------|---------------|------------|
| 405 | `pp->beginScene()` (binds scene FBO) | — |
| 469 | `gos_RendererBeginFrame()` | — |
| 470 | `Environment.UpdateRenderers()` | — |
| 471 | `gos_RendererEndFrame()` | — |
| 478 | `pp->endScene()` (composite to FB0, with bloom + FXAA) | — |
| 484 | `projectz_overlay_render(...)` | — |
| 487 | `gos_RendererFlushHUDBatch()` | — |

ImGui slots in **between line 484 (or 478 if projectz is off) and line 487**:

```
pp->endScene();                          // line 478 — scene composited to FB0
projectz_overlay_render(...);            // line 484 — debug overlay
mc2_imgui::EndFrame();                   // <- NEW — UI on top of post-processed scene
gos_RendererFlushHUDBatch();             // line 487 — HUD on top of UI
```

Rationale (matches design v0.1 risk § "Render composite slot"): ImGui must render *after* `endScene()` so bloom/FXAA do not blur menu text. It must render *before* `gos_RendererFlushHUDBatch()` because in-mission HUD is its own contract (B7) and stays a separate buffered replay path — UI menus draw under the legacy HUD when both happen to be active.

`mc2_imgui::BeginFrame()` is called at the **top of `draw_screen()`** (before line 388 `pp = getGosPostProcess()`), so feature code in `Environment.UpdateRenderers()` can issue `ImGui::Begin()/End()` calls during scene update without coupling to the composite slot.

`mc2_imgui::Init()` is called once from `gameosmain.cpp::main()` after the GL context is created and after `gos_CreateRenderer()` succeeds, before the main loop starts. `Shutdown()` is symmetric, before `gos_DestroyRenderer()`.

This coordinates with the render-contract registry (`specs/2026-04-26-render-contract-registry-design.md`) by being a **named composite slot** ("ui-overlay") that the registry can validate orderings against. Don't inline-document the slot in `draw_screen()`; register it.

### Input gate

Existing dispatch: `gameosmain.cpp::process_events()` (line 329), the `while (SDL_PollEvent(&event))` loop at line 334. Cases dispatch to `handle_key_down()`, `input::handleKeyEvent()`, `input::handleMouseMotion()`, etc.

Slot in **at the very top of the loop body, before any case**:

```cpp
while (SDL_PollEvent(&event)) {
    if (g_focus_lost) { /* ... existing focus-lost early-out ... */ }

    // NEW: forward to ImGui first. Returns true if ImGui consumed it.
    bool imgui_consumed = mc2_imgui::ProcessEvent(&event);

    // For mouse + key events the legacy system MUST be skipped iff
    // ImGui's WantCapture* is set, otherwise typing in an inspector
    // field moves the camera (memory: gos_GetKey is non-consuming).
    if (imgui_consumed) {
        switch (event.type) {
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
            if (mc2_imgui::WantCaptureKeyboard()) continue;
            break;
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
            if (mc2_imgui::WantCaptureMouse()) continue;
            break;
        }
    }

    switch (event.type) { /* ... existing legacy dispatch ... */ }
}
```

**Landmines** (memory `gos_getkey_non_consuming.md`):

1. `gos_GetKey` is non-consuming — pollers in the legacy code drain a buffer that ImGui never sees. ImGui must receive events via `ProcessEvent` *before* `input::handleKeyEvent()` populates that buffer, otherwise `WantCaptureKeyboard` is read after the fact.
2. `KeyboardFlush` is incomplete — leaves `first_pressed_` live. After an ImGui modal closes, an explicit `input::resetKeyboardState()` (or equivalent) must fire from B3 modal-close handlers to prevent a held-down key from re-triggering camera movement.
3. The `RAlt+*` debug hotkeys must be checked **after** the `WantCaptureKeyboard` gate so they don't fire while typing in an ImGui field.

Document these in the bridge `.cpp` as comments referencing the memory entries.

---

## B1 — FIT loader → UI model IR

### Reuse, not rewrite (see headline above)

`FitIniFile` (`mclib/inifile.h:63`) provides: `open(path)`, `seekBlock(blockId)`, `readIdLong(name, &out)`, `readIdString(name, buf, len)`, `readIdBoolean`, `readIdFloat`. Block-iteration is via `findNextBlockStart`. The existing `LogisticsScreen::init(file, "Static", "Text", "Rect", "Button")` is the pattern: walk all blocks named `Static0..N`, `Text0..N`, `Rect0..N`, `Button0..N`, instantiate one widget per block.

### IR struct outlines

New header `GameOS/gameos/ui/ui_model.h`:

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace mc2_ui {

enum class UIElementType : uint8_t {
    Static,        // bitmap / decoration
    Text,          // localized string lookup (StringCatalog key in B4)
    Rect,          // box / panel
    Button,        // dispatches an Action key (§5.5)
    ListBox,       // bound to a DataSource (§5.5)
    EditBox,       // text input (post-MVP)
    Image,         // dynamic image bound to a DataSource
};

struct UIRect { int left, top, width, height; };

struct UIElement {
    UIElementType    type;
    std::string      id;          // FIT block id ("Button12")
    UIRect           rect;
    uint32_t         color;       // ARGB
    std::string      text_key;    // for Text: StringCatalog key (B4)
    std::string      texture;     // for Static/Image
    std::string      action_key;  // for Button: ActionRegistry key (§5.5)
    std::string      data_source; // for ListBox/Image: DataSourceRegistry key
    int32_t          legacy_id;   // for legacy message dispatch (e.g. ENCYCLO_MECHS=130)
    // Subclass-specific fields stay in a small std::variant or sidecar map;
    // intentionally NOT a class hierarchy (keep IR POD-ish for hot-reload swap).
};

struct UILayout {
    std::string                source_path;  // "data/screens/mcl_en.fit"
    std::vector<UIElement>     elements;
    // Future: theme key, parent-page key for B-mod page injection.
};

}  // namespace mc2_ui
```

### Loader stub signature

```cpp
// GameOS/gameos/ui/fit_loader.h
namespace mc2_ui {

// Loads a .fit file via FitIniFile, walks Static/Text/Rect/Button (and
// future ListBox/EditBox/Image) blocks, returns a populated UILayout.
// On any parse error returns an empty UILayout with source_path set and
// logs [UI_FIT v1] event=load status=failed path=<...> reason=<...>.
// Never throws.
UILayout LoadFITLayout(const char* path);

// Hot-reload entry (§5.3 contract). Atomic swap into a shadow copy on
// success; old layout untouched on failure.
bool ReloadFITLayout(UILayout& target, const char* path);

}  // namespace mc2_ui
```

Called from B3 `MechlopediaImGui::init()` and (eventually) every screen-init that today calls `LogisticsScreen::init(file, ...)`. The legacy code path stays — B3 runs alongside, gated by a `MC2_UI_IMGUI_VIEWER=1` env flag for MVP, default off.

---

## B2 — ActionRegistry + DataSourceRegistry

New TU pair `GameOS/gameos/ui/action_registry.{h,cpp}` and `data_source_registry.{h,cpp}`. Singleton instances (`AccessActionRegistry()` / `AccessDataSourceRegistry()`) — same shape as `getGosPostProcess()` / `getGosRenderer()`.

### Dispatch table designed for two backends from day 1

```cpp
// GameOS/gameos/ui/action_registry.h
#pragma once
#include <functional>
#include <string>

namespace mc2_ui {

// Backend tag — distinguishes a C++-handler from a (future) Lua-handler.
// Lua dispatch path is a stub returning false in B2; lights up in Track C.
enum class ActionBackend : uint8_t { Cpp, Lua };

struct ActionEntry {
    ActionBackend                  backend;
    std::function<void()>          cpp_handler;   // valid iff backend==Cpp
    std::string                    lua_module;    // valid iff backend==Lua
    std::string                    lua_function;
};

class ActionRegistry {
public:
    // C++-side registration (B2).
    void RegisterCpp(const char* key, std::function<void()> handler);

    // Lua-side registration (Track C). Stub in B2: stores entry but
    // Dispatch() logs "[ACTION v1] event=dispatch status=lua_unwired key=<...>"
    // and returns false until Track C wires the VM.
    void RegisterLua(const char* key, const char* lua_module, const char* lua_function);

    // Returns true iff the action was dispatched. False on unknown key
    // or unwired Lua backend.
    bool Dispatch(const char* key);

    // For inspector / docs: enumerate all registered keys.
    void ForEachKey(std::function<void(const char* key, ActionBackend)> fn) const;

private:
    // std::unordered_map<std::string, ActionEntry> entries_;
};

ActionRegistry& AccessActionRegistry();

}  // namespace mc2_ui
```

`DataSourceRegistry` is the read-only mirror: `RegisterCpp(const char* key, std::function<DataSourceView()>)` returns a small view struct (vector of rows, column metadata) consumed by ListBox/Image elements. Same `RegisterLua` stub for Track C.

The two-backend dispatch table is the §5.5 commitment: **strings as the modder-stable contract; code on the other side of the lookup table.** Adding Lua dispatch in Track C is a one-function change to `Dispatch()` (call the Sol2 VM), not an interface revision.

Initial registrations for B3 Mechlopedia (all C++-backed):

- `Action: "Mechlopedia.SelectTab.Mechs"` → wraps existing `Mechlopedia::handleMessage(0, ENCYCLO_MECHS)`.
- `Action: "Mechlopedia.SelectTab.Vehicles"` → ditto for ENCYCLO_VEHIC.
- (… one Action per existing tab button.)
- `DataSource: "Mechlopedia.MechList"` → returns the existing `MechlopediaListItem` array.
- `DataSource: "Mechlopedia.SelectedMechDetail"` → wraps the current sub-screen state.

---

## B3 — Viewer (Mechlopedia) MVP

### Existing surface

- `code/mechlopedia.h` / `code/mechlopedia.cpp` — class `Mechlopedia : LogisticsScreen` with six sub-screens (`MechScreen`, `BuildingScreen`, `WeaponScreen`, vehicle-`MechScreen`, `PersonalityScreen`, history-`PersonalityScreen`).
- Loaded `mcl_en.fit` (`mechlopedia.cpp:54`); button IDs 130–136 (`ENCYCLO_MECHS` … `ENCYCLO_MM`).
- Render path: `Mechlopedia::render()` (line 161) draws a black backdrop, defers to current sub-screen's `render()`, then `LogisticsScreen::render()` for the legacy widget grid.

### ImGui replacement plug-in shape

New TU `code/mechlopedia_imgui.{h,cpp}`. New class `MechlopediaImGui` replicates the legacy class signature (`init/handleMessage/update/render/destroy`) but its render path:

1. Calls `mc2_imgui::BeginFrame()` is **not** here — that's the bridge's job at top of `draw_screen()`. This class only emits `ImGui::Begin("Mechlopedia") … ImGui::End()` calls.
2. Reads its `UILayout` (loaded once via `LoadFITLayout("data/screens/mcl_en.fit")` in `init()`).
3. Walks `layout.elements` and emits ImGui draw calls per `UIElementType`. Static → `ImGui::Image`. Text → `ImGui::TextUnformatted`. Button → `ImGui::Button` whose click dispatches `AccessActionRegistry().Dispatch(elem.action_key)`. ListBox → reads `AccessDataSourceRegistry().Get(elem.data_source)` and emits a `ImGui::ListBox` over the rows.

### Plug-in point

`code/missionselectionscreen.cpp` and the main-menu state machine instantiate `Mechlopedia` via the legacy screen-stack. Add an env-gated branch (`MC2_UI_IMGUI_VIEWER=1`) at the call site that instantiates `MechlopediaImGui` instead. **Both paths coexist** through the MVP gate — same shape as `g_useGpuStaticProps` killswitch.

### MVP completion criteria

The B3 outcome gate (per roadmap §6 Track B): "Viewer (Mechlopedia) renders fully via ImGui+FIT pipeline, with vanilla and at least one mod UI pack switching cleanly." For B3-as-MVP, "at least one mod UI pack" is loosened to "the env flag toggles between legacy and ImGui paths cleanly with `mcl_en.fit` parsed by both." The pack-switcher is B-pack.

### Files to inspect for the port

- `code/mechlopedia.{h,cpp}` — class to replicate.
- `gui/logisticsscreen.{h,cpp}` — base class; `LogisticsScreen::init` shows the FIT-block walking pattern (the pattern B1 mimics).
- `gui/abutton.{h,cpp}`, `gui/aanim.{h,cpp}` — legacy widget classes; their public API is what `ActionRegistry` actions wrap.
- `code/logisticsdata.{h,cpp}` — backing data; sources for `DataSource: "Mechlopedia.*"`.
- `data/screens/mcl_en.fit` (deployed install) — sample FIT layout to validate the loader against.

---

## §5.3 hot-reload contract — C++ shape

### Interface (no abstract base — just a convention)

```cpp
// Convention, applied per-subsystem. NOT a virtual base class — the
// signature is uniform but the call-site is per-subsystem (inspector
// button, file-watcher, Lua console). No vtable cost.
//
//   bool <Subsystem>::reloadFromDisk(const char* path);
//
// Returns true iff the new state is live; false iff the old state
// is preserved unchanged (no half-loaded data — atomicity rule).
//
// Logs ON COMPLETION (success or failure):
//   [<SUBSYS> v1] event=reload status=<ok|failed> path=<...>
//   [<SUBSYS> v1] event=reload status=failed path=<...> reason=<short>
//
// Schema-version grep pattern: \[SUBSYS v[0-9]+\]
// (matches the existing Tier-1 instrumentation env-var convention).
```

### First adopter — `glsl_program::reload()` wrapper

Existing `shader_builder.cpp:774-825` already implements the atomicity rule (compile + link into a temporary program; atomic swap of `shp_` only on link success; print on failure, keep old program). The contract wrapper is one log-line change:

```cpp
// Wrapper signature (replacing the bare bool reload()):
bool glsl_program::reloadFromDisk() {
    bool ok = reload();   // existing impl
    printf("[SHADER v1] event=reload status=%s path=%s\n",
           ok ? "ok" : "failed",
           vsh_ ? vsh_->fname_.c_str() : "?");
    fflush(stdout);
    return ok;
}
```

The legacy `reload()` keeps its existing two `printf("[SHADER] reload failed (compile|link); ...")` lines (they're useful detail); the new wrapper adds the schema-versioned status line on top. This is the cheapest possible precedent — disarms any bikeshed about API shape ("the contract is what shader reload does, with one log-line added").

### Second adopter — `AssetScale::reloadFromDisk(const char*)`

`asset_scale.cpp` today loads `data/art/asset_sizes.csv` once at startup (no reload path). Wrapper:

```cpp
// GameOS/gameos/asset_scale.h
namespace AssetScale {
    bool reloadFromDisk(const char* path);   // path defaults to manifest path
}

// asset_scale.cpp impl shape:
bool reloadFromDisk(const char* path) {
    State shadow;                                // parse into a copy
    if (!parseManifestInto(shadow, path)) {
        printf("[ASSET_SCALE v1] event=reload status=failed path=%s\n", path);
        fflush(stdout);
        return false;                            // old state untouched
    }
    state().manifest = std::move(shadow.manifest); // atomic swap
    // counters intentionally preserved across reload; only manifest swaps
    printf("[ASSET_SCALE v1] event=reload status=ok path=%s\n", path);
    fflush(stdout);
    return true;
}
```

This is the cleanest CSV adopter (existing `[ASSET_SCALE v1]` schema-versioned logging, existing self-test mode). Together, shader reload (renderer-side, timestamp-driven, frequent) and AssetScale reload (content-side, manual trigger, rare) are the precedent every later subsystem follows — weapon CSVs, pilot rosters, mod manifests, FIT layouts (`ReloadFITLayout` from B1 itself adopts this contract).

---

## Sequencing

| Slice | Depends on | Effort | Notes |
|-------|-----------|--------|-------|
| B0 | Vendor ImGui + bridge + frame-loop slot + input gate | 1–2 days | Independently testable via `ImGui::ShowDemoWindow()` toggled by RAlt+I. |
| §5.3 contract adopters | B0 (so the inspector can call them) — but contract doc + shader/AssetScale wrappers are independent | 0.5 day | Land before B1, so B1's `ReloadFITLayout` adopts the contract from day 1. |
| B1 | B0 + the FIT-reuse finding above | 1–2 days (was: weeks) | Adapter, not a parser. |
| B2 | independent of B0/B1 | 0.5 day | Stub; Track C wires Lua dispatch later. |
| B3 | B0 + B1 + B2 | 2–3 days | MVP gate. |

Total MVP path: ~5–7 days. The legacy Mechlopedia path stays live; B3 ships behind `MC2_UI_IMGUI_VIEWER=1` until the gate is met.

---

## Open questions for Methuselas

1. **FIT-parser reuse confirmation.** The status doc flagged "reuse-vs-rewrite of any existing FIT parser materially changes B1 effort." This document asserts reuse. Confirm `FitIniFile` is the format you mean — and that future modder-authored FIT files keep the same block syntax — before B1 lands code.
2. **Render slot naming.** The composite slot is "between `pp->endScene()` and `gos_RendererFlushHUDBatch()`." Render-contract registry name proposal: `"ui-overlay"`. Acceptable, or do you prefer a different name to avoid colliding with HUD ("HUD" is the legacy buffered path; "ui-overlay" is the ImGui menu path)?
3. **`ImGui::ShowDemoWindow()` survival.** Keeping `imgui_demo.cpp` in the build adds ~150 KB and is invaluable for first-cut inspector debugging. Strip post-B3 or keep behind RAlt+I for the duration of Track B?
4. **B-mod page injection IR.** The IR above does NOT yet model `[Page:Market] Parent=MechBay Slot=Tabs Order=50` (B-mod scope). Worth pre-baking a `parent_page` / `slot` / `order` triple into `UILayout` now to avoid an IR migration at B-mod, or defer?
5. **mc2res.dll string lookups for B3.** Mechlopedia uses `IDS_*` resource IDs today via `loadString()`. For B3 MVP, do we (a) keep `loadString()` as the resolver behind the IR's `text_key`, or (b) push a stub `StringCatalog` ahead of B4 so `text_key` is always a key, never an ID? Option (b) is cleaner but pulls B4 forward by half a slice.
6. **RAlt+I or different chord** for the bridge demo / inspector toggle? RAlt+0..9 are heavily used; a free chord that doesn't collide with shadow/grass/godrays toggles is needed.

---

## References

- Status: [`2026-04-29-track-b-imgui-hotreload-status.md`](2026-04-29-track-b-imgui-hotreload-status.md)
- Roadmap §5.3, §5.5, §6 Track B: [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md)
- Render contract registry: [`specs/2026-04-26-render-contract-registry-design.md`](../specs/2026-04-26-render-contract-registry-design.md)
- Tracy vendoring precedent: [`docs/plans/2026-04-11-tracy-profiler-impl.md`](../../plans/2026-04-11-tracy-profiler-impl.md)
- FIT parser: `mclib/inifile.h:63` (class `FitIniFile`), `mclib/inifile.cpp`
- Frame loop: `GameOS/gameos/gameosmain.cpp:467-490`
- Begin/End frame: `GameOS/gameos/gameos_graphics.cpp:3825-3833`
- SDL event dispatch: `GameOS/gameos/gameosmain.cpp:329-380`
- Shader reload precedent: `GameOS/gameos/utils/shader_builder.cpp:774-829`
- AssetScale: `GameOS/gameos/asset_scale.cpp`
- Mechlopedia (B3 target): `code/mechlopedia.cpp:32-170`
- Logistics screen base (FIT-walk pattern): `gui/logisticsscreen.cpp:94-170`
- Input non-consume landmine: `memory/gos_getkey_non_consuming.md`
- HUD render-state contract: `memory/` (`gos_State_IsHUD` buffering)
