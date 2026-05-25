# IMG-INSPECT-2 + IMG-INSPECT-3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add terrain fallback picking (Ctrl+Shift+click on background → shows terrain tile/world data) and per-frame debug highlight geometry (ring/crosshair drawn into the scene FBO before post-process) to the EditorInspector.

**Architecture:** Two cooperating features sharing the existing `InspectorSelection` state. `RenderObjectKind::Terrain` already exists in the enum (value=2); the plan fills in the stub. World position for terrain picks comes from `Camera::inverseProject` (already cached in `missiongui.cpp`). Debug geometry is queued via `DebugRenderer::drawRingWorld/drawLineWorld` and flushed by the existing `gamecam.cpp:323` `flushWorldPrims()` call that already runs before post-process — no new render-hook plumbing needed.

**Tech Stack:** C++17, Dear ImGui 1.91.8, `DebugRenderer` (GameOS), `Stuff::Vector3D`, `Terrain::worldToTile/worldToCell`, MSVC build via CMake / MSBuild, MC2_IMGUI + MC2_DEBUG_RENDERER env gates.

---

## Codebase state going in

| File | Key fact |
|---|---|
| `GuiRuntime/EditorInspector.h` | `InspectorSelection` has `valid`, `kind` (`RenderObjectKind`), `handle`, `lookup`, `screenX`, `screenY`. Terrain section already has a stub in drawImGui ("Terrain pick reserved (M3)."). |
| `GuiRuntime/EditorInspector.cpp` | `drawImGui()` bails early at `if (!s_selection.valid)` — Terrain picks will never reach the Terrain branch without a fix to this guard. `s_kindNames[]` likely has 3 entries (StaticProp=0, Mech=1, Terrain=2). |
| `RenderWorld/RenderWorld.h` | `LookupResult` already has `worldPosValid`, `worldX/Y/Z`. `RenderObjectKind::Terrain = 2`. |
| `GameOS/gameos/debug_renderer.h` | `drawLineWorld`, `drawAabbWorld`, `drawRingWorld(center,r,segs,rgba)`, `flushWorldPrims()`. Ring is in XZ plane at `center.y`. All calls are no-ops when `MC2_DEBUG_RENDERER` unset. |
| `code/gamecam.cpp:47,323` | Includes `"../GameOS/gameos/debug_renderer.h"` and calls `flushWorldPrims()` before post-process every frame. |
| `code/missiongui.cpp:1544` | Ctrl+Shift+click inspector pick handler (first copy). `wPos` (Stuff::Vector3D, terrain world pos) and `land` (Terrain*) are in scope. |
| `code/missiongui.cpp:1863` | Identical second copy of the same pick handler. Both need the terrain fallback. |

---

## Task 1: Add `TerrainInspectorData` + declarations to EditorInspector.h

**Files:**
- Modify: `GuiRuntime/EditorInspector.h`

- [ ] **Step 1: Add `TerrainInspectorData` struct after `MechInspectorData`**

Open `GuiRuntime/EditorInspector.h`. After the closing `};` of `MechInspectorData`, insert:

```cpp
struct TerrainInspectorData {
    bool  populated    = false;
    float worldX = 0.f, worldY = 0.f, worldZ = 0.f;
    int   tileRow = -1, tileCol = -1;
    int   cellRow = -1, cellCol = -1;
    int   terrainType = -1;          // -1 = not exposed in v1
    // Tile corner cache: reserved for future tile-outline highlight (IMG-INSPECT-3 v2).
    // hasTileCorners is always false in v1; flushDebugHighlight falls back to crosshair.
    bool  hasTileCorners = false;
    float cornerX[4]     = {};
    float cornerY[4]     = {};
    float cornerZ[4]     = {};
};
```

- [ ] **Step 2: Add `setTerrainData` and `flushDebugHighlight` declarations**

In the same file, in the function declaration block (after `setMechData`), add:

```cpp
void setTerrainData(const TerrainInspectorData& td);  // called by missiongui.cpp terrain fallback
void flushDebugHighlight();  // queue DebugRenderer prims each game-logic frame; called before flushWorldPrims
```

- [ ] **Step 3: Verify file compiles in isolation**

```
cmake --build build64 --config RelWithDebInfo --target gui_runtime 2>&1 | tail -5
```

Expected: `gui_runtime.vcxproj ->` line, no errors. (Header-only change; just validates it parses.)

---

## Task 2: Wire `s_terrainData` state into EditorInspector.cpp (setters + clear)

**Files:**
- Modify: `GuiRuntime/EditorInspector.cpp` (lines 14–65 region)

- [ ] **Step 1: Add static state variable**

In the anonymous namespace (immediately after `static EditorInspector::MechInspectorData s_mechData;`), add:

```cpp
static EditorInspector::TerrainInspectorData s_terrainData;
```

- [ ] **Step 2: Reset `s_terrainData` in `onCtrlShiftClick`**

`onCtrlShiftClick` currently resets `s_selection`, `s_staticPropData`, `s_mechData`. Add the terrain reset:

```cpp
void EditorInspector::onCtrlShiftClick(int mouseX, int mouseY) {
    if (!isEnabled()) return;
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};   // ← add this line
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    s_open = true;
}
```

- [ ] **Step 3: Reset `s_terrainData` in `clear`**

```cpp
void EditorInspector::clear() {
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};   // ← add this line
    s_open = false;
}
```

- [ ] **Step 4: Implement `setTerrainData`**

Add after `setMechData`:

```cpp
void EditorInspector::setTerrainData(const TerrainInspectorData& td) {
    if (!isEnabled()) return;
    s_terrainData = td;
    // Mark kind as Terrain so drawImGui renders the Terrain panel.
    s_selection.kind = RenderWorld::RenderObjectKind::Terrain;
    // Propagate world position into s_selection.lookup so the generic
    // header's "World:" line renders even though lookup.isValid is false.
    s_selection.lookup.worldX      = td.worldX;
    s_selection.lookup.worldY      = td.worldY;
    s_selection.lookup.worldZ      = td.worldZ;
    s_selection.lookup.worldPosValid = td.populated;
}
```

- [ ] **Step 5: Build to confirm no compile errors**

```
cmake --build build64 --config RelWithDebInfo --target gui_runtime 2>&1 | tail -5
```

Expected: `gui_runtime.vcxproj ->` with no errors.

- [ ] **Step 6: Commit**

```bash
git add GuiRuntime/EditorInspector.h GuiRuntime/EditorInspector.cpp
git commit -m "feat(inspector): TerrainInspectorData struct + setTerrainData wiring (IMG-INSPECT-2 state)"
```

---

## Task 3: Fix early-exit guard + fill in Terrain ImGui panel

**Files:**
- Modify: `GuiRuntime/EditorInspector.cpp` (drawImGui section)

- [ ] **Step 1: Fix the early-exit guard to allow Terrain selections through**

Find the block at approximately line 102 that reads:

```cpp
if (!s_selection.valid) {
    ImGui::Spacing();
    ImGui::TextUnformatted("No valid object selected.");
    ImGui::TextUnformatted("Ctrl+Shift+LMB to pick.");
    ImGui::Text("Screen: (%d, %d)", s_selection.screenX, s_selection.screenY);
    ImGui::End();
    return;
}
```

Replace the condition only (keep the body unchanged):

```cpp
const bool hasTerrainHit = (s_selection.kind == RenderWorld::RenderObjectKind::Terrain)
                            && s_terrainData.populated;
if (!s_selection.valid && !hasTerrainHit) {
    ImGui::Spacing();
    ImGui::TextUnformatted("No valid object selected.");
    ImGui::TextUnformatted("Ctrl+Shift+LMB to pick.");
    ImGui::Text("Screen: (%d, %d)", s_selection.screenX, s_selection.screenY);
    ImGui::End();
    return;
}
```

- [ ] **Step 2: Fill in the Terrain panel stub**

Find the stub that reads:

```cpp
    } else if (s_selection.kind == RenderWorld::RenderObjectKind::Terrain) {
        if (ImGui::CollapsingHeader("Terrain")) {
            ImGui::TextUnformatted("Terrain pick reserved (M3).");
        }
    }
```

Replace entirely with:

```cpp
    } else if (s_selection.kind == RenderWorld::RenderObjectKind::Terrain) {
        if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s_terrainData.populated) {
                ImGui::TextUnformatted("(terrain pick not resolved)");
            } else {
                ImGui::Text("World:  (%.1f, %.1f, %.1f)",
                    s_terrainData.worldX, s_terrainData.worldY, s_terrainData.worldZ);
                ImGui::Text("Tile:   row %-4d  col %d",
                    s_terrainData.tileRow, s_terrainData.tileCol);
                ImGui::Text("Cell:   row %-4d  col %d",
                    s_terrainData.cellRow, s_terrainData.cellCol);
                if (s_terrainData.terrainType >= 0)
                    ImGui::Text("Type:   %d", s_terrainData.terrainType);
                else
                    ImGui::TextDisabled("Type:   (n/a v1)");
            }
            // Hint shown when the DebugRenderer highlight is silent.
            static int s_drEnabled = -1;
            if (s_drEnabled < 0)
                s_drEnabled = (std::getenv("MC2_DEBUG_RENDERER") != nullptr) ? 1 : 0;
            if (!s_drEnabled)
                ImGui::TextDisabled("Highlight: off (set MC2_DEBUG_RENDERER=1 to enable)");
        }
    }
```

- [ ] **Step 3: Build**

```
cmake --build build64 --config RelWithDebInfo --target gui_runtime 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add GuiRuntime/EditorInspector.cpp
git commit -m "feat(inspector): fill Terrain ImGui panel + fix early-exit guard (IMG-INSPECT-2 display)"
```

---

## Task 4: Terrain fallback + per-frame highlight queue in missiongui.cpp

**Files:**
- Modify: `code/missiongui.cpp`

The Ctrl+Shift+click inspector block exists **twice** in `missiongui.cpp` (lines ~1544 and ~1863). Both are structurally identical. Apply **both** changes described below.

- [ ] **Step 1: Add terrain fallback after the object-ID dispatch in FIRST pick handler (line ~1600)**

The first handler ends with:
```cpp
        }
        pickedByInspector = true;
    }
#endif
```

The `if (inspResult.lookup.isValid)` block closes at the `}` just before `pickedByInspector = true;`. Add the terrain fallback between them:

```cpp
        // IMG-INSPECT-2: terrain CPU fallback when object-ID misses.
        // Fires only if: OID gave no valid object AND wPos is inside the playable map.
        // When MC2_OBJECT_ID_BUFFER is off, OID always misses, so terrain wins by default;
        // that is expected (and documented in the Object-ID panel as OID_BUFFER_DISABLED).
        if (!inspResult.lookup.isValid && land &&
                Terrain::IsGameSelectTerrainPosition(wPos)) {
            EditorInspector::TerrainInspectorData td;
            td.populated = true;
            td.worldX    = static_cast<float>(wPos.x);
            td.worldY    = static_cast<float>(wPos.y);
            td.worldZ    = static_cast<float>(wPos.z);
            land->worldToTile(wPos, td.tileRow, td.tileCol);
            land->worldToCell(wPos, td.cellRow, td.cellCol);
            // terrainType left as -1 (v1 — accessor not wired yet).
            EditorInspector::setTerrainData(td);
        }
        pickedByInspector = true;
```

- [ ] **Step 2: Apply the identical fallback to the SECOND pick handler (line ~1920)**

Search for the second `pickedByInspector = true;` inside an `#ifdef MC2_IMGUI` block (around line 1920). Apply the exact same block as Step 1 immediately above it.

- [ ] **Step 3: Add per-frame `flushDebugHighlight` call**

`flushDebugHighlight()` queues DebugRenderer draws — it must run every frame (not just on click) so the highlight persists. `DebugRenderer::flushWorldPrims()` is already called in `gamecam.cpp:323` during scene rendering; our queued draws will be consumed there.

Find the `wPos` computation block (lines ~760–793). Immediately after the block that calls `eye->inverseProject` (or reuses the cache), inside an `#ifdef MC2_IMGUI` guard, add:

```cpp
#ifdef MC2_IMGUI
    EditorInspector::flushDebugHighlight();  // IMG-INSPECT-3: queue world-space highlight prims
#endif
```

If there is already a nearby `#ifdef MC2_IMGUI` block, fold it in rather than nesting.

- [ ] **Step 4: Build full mc2 target**

```
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: clean link, `mc2.exe` produced.

- [ ] **Step 5: Commit**

```bash
git add code/missiongui.cpp
git commit -m "feat(inspector): terrain CPU fallback pick + per-frame flushDebugHighlight (IMG-INSPECT-2/3 wiring)"
```

---

## Task 5: Implement `flushDebugHighlight()` in EditorInspector.cpp

**Files:**
- Modify: `GuiRuntime/EditorInspector.cpp`

**Background:** `DebugRenderer::drawRingWorld` draws a ring in the **XZ plane** at `center.y`. In DebugRenderer's color convention the `uint32_t rgba` parameter is packed as 0xRRGGBBAA. All draw calls are no-ops when `MC2_DEBUG_RENDERER` is unset.

**Draw-buffer contract:** `flushDebugHighlight` only calls `draw*World` (which queue to an internal CPU buffer). It does NOT call `flushWorldPrims()`. The actual GL draw happens inside `gamecam.cpp::flushWorldPrims()` which already manages its own GL state. `flushDebugHighlight` therefore touches no GL state — the contract is satisfied automatically.

- [ ] **Step 1: Add `debug_renderer.h` include**

At the top of `GuiRuntime/EditorInspector.cpp`, after the existing includes, add:

```cpp
#include "../GameOS/gameos/debug_renderer.h"  // IMG-INSPECT-3: DebugRenderer highlight
```

(Same relative path used by `code/gamecam.cpp`.)

- [ ] **Step 2: Implement `flushDebugHighlight`**

Add at the end of the file, before the final `}  // namespace EditorInspector`:

```cpp
void EditorInspector::flushDebugHighlight() {
    if (!isEnabled()) return;
    if (!s_open)      return;

    // Colors: 0xRRGGBBAA, opaque.
    constexpr uint32_t kCyan   = 0x00FFFFFFu;  // StaticProp
    constexpr uint32_t kYellow = 0xFFFF00FFu;  // Mech
    constexpr uint32_t kGreen  = 0x44FF88FFu;  // Terrain

    const auto kind = s_selection.kind;

    if ((kind == RenderWorld::RenderObjectKind::StaticProp ||
         kind == RenderWorld::RenderObjectKind::Mech)
            && s_selection.valid) {
        // Ring in XZ plane at the world hit point.
        const float radius = (kind == RenderWorld::RenderObjectKind::Mech) ? 4.f : 2.f;
        const uint32_t col = (kind == RenderWorld::RenderObjectKind::Mech) ? kYellow : kCyan;
        DebugRenderer::Vec3 c{
            s_selection.lookup.worldX,
            s_selection.lookup.worldY,
            s_selection.lookup.worldZ
        };
        DebugRenderer::drawRingWorld(c, radius, 16, col);

    } else if (kind == RenderWorld::RenderObjectKind::Terrain
                && s_terrainData.populated) {
        // 3-axis crosshair at terrain hit point (v1 — tile corners not computed).
        // Uses all 3 axes so it is visible regardless of which axis is "up" in-world.
        const float x   = s_terrainData.worldX;
        const float y   = s_terrainData.worldY;
        const float z   = s_terrainData.worldZ;
        const float arm = 5.f;
        DebugRenderer::drawLineWorld({ x-arm, y,    z    }, { x+arm, y,    z    }, kGreen);
        DebugRenderer::drawLineWorld({ x,    y-arm, z    }, { x,    y+arm, z    }, kGreen);
        DebugRenderer::drawLineWorld({ x,    y,    z-arm }, { x,    y,    z+arm }, kGreen);
    }
}
```

- [ ] **Step 3: Build full mc2 target**

```
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

Expected: clean build and link, no errors about `debug_renderer.h` not found.

If build fails with `'../GameOS/gameos/debug_renderer.h': No such file or directory`, add the include path to `GuiRuntime/CMakeLists.txt`:

```cmake
target_include_directories(gui_runtime PRIVATE
    ${THIRDPARTY_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/GameOS           # ← add if needed
)
```

Then rebuild.

- [ ] **Step 4: Deploy**

```bash
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

- [ ] **Step 5: Smoke check**

Launch `run-editor.bat` (or the game) with `MC2_IMGUI=1 MC2_IMGUI_INSPECTOR=1`.

Verify:
1. Ctrl+Shift+click on a static prop → inspector shows StaticProp panel (regression check).
2. Ctrl+Shift+click on empty ground → inspector shows "Terrain" panel with World/Tile/Cell data.
3. The terrain pick does NOT interfere with normal gameplay clicks (no `pickedByInspector` side-effect on non-Ctrl paths).
4. With `MC2_DEBUG_RENDERER=1`: a cyan ring appears on static props, a green crosshair on terrain.
5. Without `MC2_DEBUG_RENDERER`: panel shows the "Highlight: off" hint line; no GL errors.

- [ ] **Step 6: Commit**

```bash
git add GuiRuntime/EditorInspector.cpp
git commit -m "feat(inspector): flushDebugHighlight — ring (prop/mech) + crosshair (terrain) IMG-INSPECT-3"
```

---

## Guard rails

These were identified in the spec review and are enforced by this plan:

| Guard | Where enforced |
|---|---|
| Terrain fallback only fires inside viewport | `Terrain::IsGameSelectTerrainPosition(wPos)` guard in Task 4 Step 1 |
| OID-off behavior documented in panel | Object-ID section already shows `OID_BUFFER_DISABLED` reason; comment in Task 4 Step 1 explains the fallback-wins behaviour |
| No draw-buffer mutation | `flushDebugHighlight` only calls queue functions, never `flushWorldPrims`; `gamecam.cpp` owns the flush |
| DebugRenderer line-only fallback | Not needed — `drawRingWorld` already exists in debug_renderer.h |
| State restore | No GL state touched by `flushDebugHighlight`; contract satisfied by design |
| No RenderObjectKind::Terrain added | Already exists (value=2); plan uses it, does not re-add |
| No gameplay selection mutation | `pickedByInspector = true` was already set before terrain fallback; no other selection state touched |
