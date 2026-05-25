# IMG-INSPECT-2 + IMG-INSPECT-3 Implementation Plan (v2 — review-patched)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add terrain fallback picking (Ctrl+Shift+click on background → shows terrain tile/world data) and per-frame debug highlight geometry (ring/crosshair drawn into the scene FBO before post-process) to the EditorInspector.

**Architecture:** Two cooperating features sharing the `InspectorSelection` state. **Inspector dispatch uses a local `InspectorPickKind` enum** (NOT `RenderWorld::RenderObjectKind::Terrain`) — keeps terrain UI/debug behavior independent of any RenderWorld semantic. `RenderWorld::LookupResult` + `RenderObjectHandle` remain valid only for StaticProp/Mech. Terrain world position comes from `Camera::inverseProject` (already cached in `missiongui.cpp`). Debug geometry is queued via `DebugRenderer::drawLineWorld/drawRingWorld` and flushed inside `gamecam.cpp` **immediately before** the existing `DebugRenderer::flushWorldPrims()` call at line 323 — same-frame, same-FBO.

**Tech Stack:** C++17, Dear ImGui 1.91.8, `DebugRenderer` (GameOS), `Stuff::Vector3D`, `Terrain::worldToTile/worldToCell`, MSVC build via CMake / MSBuild, MC2_IMGUI + MC2_DEBUG_RENDERER env gates.

---

## Review fixes applied (v2)

| ID | Fix |
|---|---|
| C1 | Introduce `InspectorPickKind { None, StaticProp, Mech, Terrain }` + `hasSelection`. drawImGui + flushDebugHighlight dispatch on `pickKind`, not `RenderObjectKind`. Existing `kind` field retained for valid prop/mech lookup compatibility. |
| M1 | `flushDebugHighlight()` called in `gamecam.cpp` immediately before `DebugRenderer::flushWorldPrims()` (line 323) — NOT in missiongui.cpp. Same-frame guarantee. |
| M2 | `setTerrainData()` sets `s_selection.hasSelection = true` and `s_selection.pickKind = Terrain`. All branches use `hasSelection`. |
| M3 | Fallback guard: `Terrain::IsGameSelectTerrainPosition(wPos)` is the project-canonical "world point is on selectable terrain" check (used elsewhere in same file at line 789). Document explicitly as terrain-validity, not viewport guard. Mouse coords already routed through GameOS client-area mapping; no separate screen rect available without new plumbing. |
| M4 | `pickedByInspector = true` ONLY when object-ID hit OR terrain fallback hit. Miss = pass-through. |
| M5 | StaticProp highlight color = white `0xFFFFFFFFu`. |
| M6 | Terrain highlight = flat X/Z crosshair (no vertical arm). |
| m1 | `<cstdlib>` already included (line 4 of cpp). No change. |
| m2 | Add `pickKindName()` helper for ImGui display. |
| m3 | Two pick handlers in missiongui (~1601 and ~1920) — keep byte-identical fallback insertion. |

---

## Codebase state going in (verified 2026-05-24)

| File | Key fact |
|---|---|
| `GuiRuntime/EditorInspector.h` | `InspectorSelection` has `valid` (bool), `kind` (RenderObjectKind), `handle`, `lookup`, `screenX`, `screenY`. No `hasSelection`/`pickKind` yet. |
| `GuiRuntime/EditorInspector.cpp` | Already includes `<cstdlib>`. Stub at line 253–257 ("Terrain pick reserved (M3)."). Early-exit guard at line 102: `if (!s_selection.valid)`. `s_kindNames[]` already has "Terrain" entry. |
| `code/gamecam.cpp:319-324` | `DebugRenderer::flushWorldPrims()` call inside `ZoneScopedN("GameCamera::render debugRendererFlushWorldPrims")`. Include path `../GameOS/gameos/debug_renderer.h`. |
| `code/missiongui.cpp:1543-1602` | First Ctrl+Shift+click handler. `pickedByInspector` declared line 1543, set unconditionally line 1601. |
| `code/missiongui.cpp:1862-1921` | Second identical handler. |
| `code/missiongui.cpp:760-781` | `wPos` (Stuff::Vector3D) computed via `eye->inverseProject` with delta-cache. `land` (Terrain*) in scope. |
| `code/missiongui.cpp:789` | `Terrain::IsGameSelectTerrainPosition(wPos)` already used as the canonical "is this world point on selectable map terrain" guard. |

---

## Task 1: Add `InspectorPickKind` + `TerrainInspectorData` + declarations to EditorInspector.h

**Files:**
- Modify: `GuiRuntime/EditorInspector.h`

- [ ] **Step 1: Add `InspectorPickKind` enum near top of namespace**

After `namespace EditorInspector {` and before `struct InspectorSelection`, insert:

```cpp
// Inspector's own pick-source taxonomy. Independent of RenderWorld::RenderObjectKind
// so terrain (which is not a RenderWorld object) can be a first-class pick result.
enum class InspectorPickKind : unsigned char {
    None       = 0,
    StaticProp = 1,
    Mech       = 2,
    Terrain    = 3,
};
```

- [ ] **Step 2: Extend `InspectorSelection`**

Add two fields (keep existing `valid`/`kind`/`handle`/`lookup` intact — required by setPickResult callers):

```cpp
struct InspectorSelection {
    bool valid = false;                                                  // (existing) RenderWorld lookup valid
    bool hasSelection = false;                                           // NEW: any inspector pick has data
    InspectorPickKind pickKind = InspectorPickKind::None;                // NEW
    RenderWorld::RenderObjectKind kind = RenderWorld::RenderObjectKind::StaticProp;  // (existing)
    RenderCore::RenderObjectHandle handle;                               // (existing)
    RenderWorld::LookupResult lookup;                                    // (existing)
    int screenX = 0;
    int screenY = 0;
};
```

- [ ] **Step 3: Add `TerrainInspectorData` after `MechInspectorData`**

```cpp
struct TerrainInspectorData {
    bool  populated  = false;
    float worldX = 0.f, worldY = 0.f, worldZ = 0.f;
    int   tileRow = -1, tileCol = -1;
    int   cellRow = -1, cellCol = -1;
    int   terrainType = -1;  // -1 = not exposed in v1
};
```

- [ ] **Step 4: Add declarations**

After `setMechData`:

```cpp
void setTerrainData(const TerrainInspectorData& td);  // called by missiongui.cpp terrain fallback
void flushDebugHighlight();                            // called by gamecam.cpp before flushWorldPrims
```

- [ ] **Step 5: Verify header parses**

```
cmake --build build64 --config RelWithDebInfo --target gui_runtime 2>&1 | tail -5
```

---

## Task 2: Wire `s_terrainData` + `hasSelection` into EditorInspector.cpp; update setPickResult

**Files:**
- Modify: `GuiRuntime/EditorInspector.cpp`

- [ ] **Step 1: Add static state**

After `static EditorInspector::MechInspectorData s_mechData;`:

```cpp
static EditorInspector::TerrainInspectorData s_terrainData;
```

- [ ] **Step 2: Update `setPickResult` to set `hasSelection`/`pickKind`**

Inside the existing `if (lookup.isValid)` block, also set:

```cpp
    s_selection.hasSelection = true;
    if (lookup.kind == RenderWorld::RenderObjectKind::StaticProp)
        s_selection.pickKind = InspectorPickKind::StaticProp;
    else if (lookup.kind == RenderWorld::RenderObjectKind::Mech)
        s_selection.pickKind = InspectorPickKind::Mech;
    else
        s_selection.pickKind = InspectorPickKind::None;
```

- [ ] **Step 3: Reset `s_terrainData` in `onCtrlShiftClick`, `setPickResult`, and `clear`**

Add `s_terrainData = TerrainInspectorData{};` next to the existing `s_mechData = MechInspectorData{};` resets in all three sites.

- [ ] **Step 4: Implement `setTerrainData`**

After `setMechData`:

```cpp
void EditorInspector::setTerrainData(const TerrainInspectorData& td) {
    if (!isEnabled()) return;
    s_terrainData = td;
    s_selection.hasSelection     = td.populated;
    s_selection.pickKind         = InspectorPickKind::Terrain;
    // Mirror world pos into lookup so the generic header's "World:" line renders.
    s_selection.lookup.worldX      = td.worldX;
    s_selection.lookup.worldY      = td.worldY;
    s_selection.lookup.worldZ      = td.worldZ;
    s_selection.lookup.worldPosValid = td.populated;
    // NB: s_selection.valid stays false (no RenderWorld lookup).
}
```

- [ ] **Step 5: Build + commit**

```
cmake --build build64 --config RelWithDebInfo --target gui_runtime 2>&1 | tail -5
git add GuiRuntime/EditorInspector.h GuiRuntime/EditorInspector.cpp
git commit -m "feat(inspector): InspectorPickKind + TerrainInspectorData state (IMG-INSPECT-2 state)"
```

---

## Task 3: Switch drawImGui dispatch to `pickKind` + fill Terrain panel

**Files:**
- Modify: `GuiRuntime/EditorInspector.cpp` (drawImGui)

- [ ] **Step 1: Replace early-exit guard**

Find the line 102 guard `if (!s_selection.valid) {`. Replace condition only:

```cpp
if (!s_selection.hasSelection) {
```

- [ ] **Step 2: Add a `pickKindName()` helper** (top of cpp, in anonymous namespace)

```cpp
static const char* pickKindName(EditorInspector::InspectorPickKind k) {
    switch (k) {
        case EditorInspector::InspectorPickKind::StaticProp: return "StaticProp";
        case EditorInspector::InspectorPickKind::Mech:       return "Mech";
        case EditorInspector::InspectorPickKind::Terrain:    return "Terrain";
        default:                                              return "None";
    }
}
```

- [ ] **Step 3: Update generic header Kind label**

Replace the lines that compute `kindName` via `s_kindNames[kindIdx]` with:

```cpp
const char* kindName = pickKindName(s_selection.pickKind);
```

(Remove the now-unused `kindIdx` line and `s_kindNames[]` array — or leave `s_kindNames[]` if other code references it; grep first.)

- [ ] **Step 4: Switch kind-specific branches to `pickKind`**

Change the StaticProp / Mech / Terrain `if`/`else if` chain to dispatch on `s_selection.pickKind` instead of `s_selection.kind`. The existing data field access (`s_staticPropData`, `s_mechData`) stays the same.

- [ ] **Step 5: Replace the Terrain stub**

```cpp
} else if (s_selection.pickKind == InspectorPickKind::Terrain) {
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
        static int s_drEnabled = -1;
        if (s_drEnabled < 0)
            s_drEnabled = (std::getenv("MC2_DEBUG_RENDERER") != nullptr) ? 1 : 0;
        if (!s_drEnabled)
            ImGui::TextDisabled("Highlight: off (set MC2_DEBUG_RENDERER=1 to enable)");
    }
}
```

- [ ] **Step 6: Build + commit**

```
cmake --build build64 --config RelWithDebInfo --target gui_runtime 2>&1 | tail -5
git add GuiRuntime/EditorInspector.cpp
git commit -m "feat(inspector): drawImGui dispatch on pickKind + Terrain panel (IMG-INSPECT-2 display)"
```

---

## Task 4: Terrain CPU fallback in missiongui.cpp (both pick handlers); guard pickedByInspector

**Files:**
- Modify: `code/missiongui.cpp`

The handler exists twice (line ~1543 and ~1862). Apply identical changes to both.

- [ ] **Step 1: Replace unconditional `pickedByInspector = true` with success-gated set**

In the FIRST handler, replace:

```cpp
            }
            pickedByInspector = true;
        }
#endif
```

with:

```cpp
            }
            // IMG-INSPECT-2: terrain CPU fallback when object-ID misses.
            // Terrain::IsGameSelectTerrainPosition(wPos) is the canonical
            // "world point is on selectable map terrain" guard (also used
            // at line 789 for LOS/cell lookup). If OID buffer is disabled
            // (MC2_OBJECT_ID_BUFFER=0), inspResult.lookup.isValid is always
            // false here so terrain wins on any in-map click — expected.
            bool inspectorConsumed = inspResult.lookup.isValid;
            if (!inspResult.lookup.isValid && land &&
                    Terrain::IsGameSelectTerrainPosition(wPos)) {
                EditorInspector::TerrainInspectorData td;
                td.populated = true;
                td.worldX    = static_cast<float>(wPos.x);
                td.worldY    = static_cast<float>(wPos.y);
                td.worldZ    = static_cast<float>(wPos.z);
                land->worldToTile(wPos, td.tileRow, td.tileCol);
                land->worldToCell(wPos, td.cellRow, td.cellCol);
                EditorInspector::setTerrainData(td);
                inspectorConsumed = true;
            }
            if (inspectorConsumed)
                pickedByInspector = true;
        }
#endif
```

- [ ] **Step 2: Apply IDENTICAL block to SECOND handler (~line 1920)**

Find the second `pickedByInspector = true;` followed by `}\n#endif` and apply the same replacement.

- [ ] **Step 3: Build full mc2 target**

```
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

- [ ] **Step 4: Commit**

```
git add code/missiongui.cpp
git commit -m "feat(inspector): terrain CPU fallback pick + success-gated pickedByInspector (IMG-INSPECT-2)"
```

---

## Task 5: Implement `flushDebugHighlight()` and wire call from gamecam.cpp

**Files:**
- Modify: `GuiRuntime/EditorInspector.cpp`
- Modify: `code/gamecam.cpp`

**Background:** `DebugRenderer::drawRingWorld` draws in the XZ plane at `center.y`. `uint32_t rgba` packed `0xRRGGBBAA`. All draws are no-ops when `MC2_DEBUG_RENDERER` unset. `flushDebugHighlight` only QUEUES; the actual flush happens in `gamecam.cpp::flushWorldPrims()` which already manages GL state.

- [ ] **Step 1: Add include**

Top of `GuiRuntime/EditorInspector.cpp` after existing includes:

```cpp
#include "../GameOS/gameos/debug_renderer.h"  // IMG-INSPECT-3
```

- [ ] **Step 2: Implement `flushDebugHighlight`**

Add at end of file:

```cpp
void EditorInspector::flushDebugHighlight() {
    if (!isEnabled()) return;
    if (!s_open)      return;
    if (!s_selection.hasSelection) return;

    // 0xRRGGBBAA, opaque.
    constexpr uint32_t kStaticPropCol = 0xFFFFFFFFu;  // white
    constexpr uint32_t kMechCol       = 0xFFFF00FFu;  // yellow
    constexpr uint32_t kTerrainCol    = 0x44FF88FFu;  // green

    const auto pk = s_selection.pickKind;

    if ((pk == InspectorPickKind::StaticProp || pk == InspectorPickKind::Mech)
            && s_selection.valid) {
        const float radius   = (pk == InspectorPickKind::Mech) ? 4.f : 2.f;
        const uint32_t col   = (pk == InspectorPickKind::Mech) ? kMechCol : kStaticPropCol;
        DebugRenderer::Vec3 c{
            s_selection.lookup.worldX,
            s_selection.lookup.worldY,
            s_selection.lookup.worldZ
        };
        DebugRenderer::drawRingWorld(c, radius, 16, col);

    } else if (pk == InspectorPickKind::Terrain && s_terrainData.populated) {
        // Flat X/Z crosshair at terrain hit. No vertical arm — terrain
        // marker should look flush with the ground, not like an object bracket.
        const float x   = s_terrainData.worldX;
        const float y   = s_terrainData.worldY;
        const float z   = s_terrainData.worldZ;
        const float arm = 5.f;
        DebugRenderer::drawLineWorld({ x-arm, y, z     }, { x+arm, y, z     }, kTerrainCol);
        DebugRenderer::drawLineWorld({ x,     y, z-arm }, { x,     y, z+arm }, kTerrainCol);
    }
}
```

- [ ] **Step 3: Wire call in gamecam.cpp**

In `code/gamecam.cpp` near line 319, modify the existing DebugRenderer block. Add include at top of file (if not already present):

```cpp
#include "../GuiRuntime/EditorInspector.h"  // IMG-INSPECT-3 flushDebugHighlight
```

Then replace lines ~319–324:

```cpp
        // DebugRenderer world primitives -- depth-tested, before post-process.
        // No-op when MC2_DEBUG_RENDERER is unset.
        {
            ZoneScopedN("GameCamera::render debugRendererFlushWorldPrims");
#ifdef MC2_IMGUI
            EditorInspector::flushDebugHighlight();  // IMG-INSPECT-3: queue highlight prims same-frame
#endif
            DebugRenderer::flushWorldPrims();
        }
```

- [ ] **Step 4: Build full mc2 target**

```
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
```

If `EditorInspector.h` not found from `code/`, check `code/CMakeLists.txt` for `GuiRuntime` in include dirs; add if absent.

If `debug_renderer.h` not found from `GuiRuntime/`, add include dir to `GuiRuntime/CMakeLists.txt`:

```cmake
target_include_directories(gui_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/GameOS
)
```

- [ ] **Step 5: Deploy + smoke**

```
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

Launch with `MC2_IMGUI=1 MC2_IMGUI_INSPECTOR=1 MC2_DEBUG_RENDERER=1`. Verify:
1. Ctrl+Shift+click on static prop → StaticProp panel, white ring on ground.
2. Ctrl+Shift+click on empty ground → Terrain panel with World/Tile/Cell, green flat X/Z crosshair.
3. Ctrl+Shift+click outside map (sky) → no inspector consume, normal click pass-through (no panel change).
4. Without `MC2_DEBUG_RENDERER` → panel shows "Highlight: off" hint, no GL errors.

- [ ] **Step 6: Commit**

```
git add GuiRuntime/EditorInspector.cpp code/gamecam.cpp
git commit -m "feat(inspector): flushDebugHighlight wired in gamecam pre-flushWorldPrims (IMG-INSPECT-3)"
```

---

## Guard rails (v2)

| Guard | Where enforced |
|---|---|
| Terrain pick is inspector-local, NOT a RenderObjectKind | `InspectorPickKind` enum; `s_selection.valid` stays false for terrain |
| Same-frame highlight rendering | `flushDebugHighlight` called immediately before `flushWorldPrims` in gamecam.cpp |
| Fallback only fires on selectable terrain | `Terrain::IsGameSelectTerrainPosition(wPos)` |
| Click is consumed only on inspector success | `inspectorConsumed` gate around `pickedByInspector = true` |
| No GL state mutation in flushDebugHighlight | Only queue calls; flush owned by gamecam.cpp |
| StaticProp color matches spec | `kStaticPropCol = 0xFFFFFFFFu` (white) |
| Terrain marker is flat | X/Z crosshair only, no vertical arm |
| Two-handler symmetry | Identical block applied to both ~1601 and ~1920 sites |
