# IMG-INSPECT-2 + IMG-INSPECT-3 Spec
## Terrain Pick Fallback + Inspector Debug Highlight

**Date:** 2026-05-24
**Branch:** claude/nifty-mendeleev
**Status:** Approved

---

## Scope

Two composable inspector extensions, independent enough to implement sequentially:

| Tag | Feature | Prerequisite |
|---|---|---|
| IMG-INSPECT-2 | Terrain CPU fallback when object-ID misses | M3 phase 2 (shipped) |
| IMG-INSPECT-3 | World-space debug highlight for inspected object | IMG-INSPECT-2 |

### Explicit non-scope

- No terrain object-ID writer
- No gameplay selection mutation (`selectedObject` or mission-GUI selection state untouched)
- No VFX / decal / overlay picking
- No editor placement mutation
- No default-framebuffer world overlay

---

## IMG-INSPECT-2 — Terrain Pick Fallback

### Pick priority order (invariant)

```
1. ImGui window/widget capture
2. Ctrl+Shift+LMB object-ID hit (existing path)
3. Ctrl+Shift+LMB terrain CPU fallback  ← new, fires only on step-2 miss
4. Normal gameplay input
```

### Terrain fallback trigger condition

Fallback fires **only when both conditions are true:**

1. `lookupAtPixel()` returned `!isValid` (any reason: BACKGROUND_PIXEL,
   OID_BUFFER_DISABLED, INDEX_OUT_OF_RANGE, etc.)
2. The screen point is **inside the viewport** (already guarded upstream in
   `tryGameplayPick` — explicit re-check in the fallback path for clarity)

Do **not** fire on every miss. Out-of-bounds clicks or invalid FBO coords must
produce `pickKind = None`, not a bogus terrain entry.

### World position source

Use the **existing missiongui.cpp terrain-pick world position path** (the
`Camera::inverseProject` + terrain-pick spine already used around line 775 of
`missiongui.cpp`, cached per mouse+camera change). Do not assume
`Camera::inverseProject` alone returns an exact terrain-surface point; use
whatever existing machinery (terrain AABB walk / terrain intersection) that
path already performs.

The cached world position is also stored in `InspectorSelection.worldX/Y/Z` for
use by IMG-INSPECT-3.

### Data model additions (EditorInspector.h)

```cpp
// -- InspectorPickKind -------------------------------------------------
// Local enum; must NOT be added to RenderObjectKind or any RenderCore type.
enum class InspectorPickKind { None, StaticProp, Mech, Terrain };

// -- InspectorSelection (replaces existing struct) ---------------------
struct InspectorSelection {
    bool              hasSelection = false;
    InspectorPickKind pickKind     = InspectorPickKind::None;

    // Valid for StaticProp / Mech only:
    RenderCore::RenderObjectHandle handle;
    RenderWorld::LookupResult      lookup;

    // Valid for all kinds when hasSelection == true:
    float worldX = 0.f, worldY = 0.f, worldZ = 0.f;

    int screenX = 0, screenY = 0;
};

// Invariant:
//   hasSelection == false  ->  pickKind == None
//   hasSelection == true   ->  pickKind in { StaticProp, Mech, Terrain }

// -- TerrainInspectorData ----------------------------------------------
struct TerrainInspectorData {
    bool populated  = false;
    float worldX    = 0.f, worldY = 0.f, worldZ = 0.f;
    int   tileRow   = -1,  tileCol = -1;
    int   cellRow   = -1,  cellCol = -1;
    int   terrainType = -1;           // -1 if not trivially accessible in v1

    // Tile outline corners for IMG-INSPECT-3 highlight.
    // hasTileCorners == false in v1 if tileCellToWorld lookup is non-trivial.
    // IMG-INSPECT-3 falls back to hit-point crosshair when false.
    bool  hasTileCorners = false;
    float cornerX[4] = {};
    float cornerY[4] = {};
    float cornerZ[4] = {};
};
```

`TerrainInspectorData` is a peer of the existing `StaticPropInspectorData` and
`MechInspectorData` structs. Same store/clear pattern.

### New EditorInspector API

```cpp
void setTerrainData(const TerrainInspectorData& td);
```

Same call pattern as the existing `setStaticPropData` / `setMechData`.

### missiongui.cpp changes (IMG-INSPECT-2)

After `lookupAtPixel()` returns miss AND viewport guard passes:

```
call existing terrain world-position path -> wPos
worldToTile(wPos, tileR, tileC)
worldToCell(wPos, cellR, cellC)
attempt tileCellToWorld for 4 corners -> set hasTileCorners if successful
terrainType lookup if available -> leave -1 otherwise

populate TerrainInspectorData
populate InspectorSelection { hasSelection=true, pickKind=Terrain, worldXYZ=wPos }
call EditorInspector::setTerrainData(td)
```

No GameAdapter layer needed: missiongui.cpp is in `code/` and has direct
access to terrain and camera.

### ImGui panel: Terrain section

Collapsing header `"Terrain"`, `ImGuiTreeNodeFlags_DefaultOpen`:

```
Kind:       Terrain
World:      123.45  56.78  890.12
Tile:       row 42   col 17
Cell:       row 168  col 69
Terrain type: N      (omit row if terrainType == -1)
```

---

## IMG-INSPECT-3 — Debug Highlight

### Render hook: scene FBO, before post-process

```
EditorInspector::flushDebugHighlight()
```

Called **before `gosPostProcess::process()`** while the scene FBO is still
bound. This ensures debug geometry is depth-tested against scene geometry and
passes through the post-process/tone-map pipeline as part of the world image.

**State contract (non-negotiable):**
- Must restore depth test, blend, and line-width state to what it was before the call,
  or delegate entirely to `DebugRenderer`'s existing state-save/restore contract.
- Must **not** write to the ObjectID attachment (draw-buffer mask must exclude
  color attachment 2).
- Must **not** alter `GL_DRAW_BUFFER` state on return.
- Must **not** break depth/post-process correctness (see depth-reverse-Z fix,
  HEAD `330e665`).

### Gating

- Default-on whenever `MC2_IMGUI_INSPECTOR` is active and
  `s_selection.hasSelection == true`.
- `DebugRenderer` draw calls are already no-ops if `MC2_DEBUG_RENDERER` is
  unset — no additional env var needed.
- If `MC2_DEBUG_RENDERER` is unset, ImGui inspector panel shows one line:

  ```
  Highlight: disabled (set MC2_DEBUG_RENDERER=1 to enable)
  ```

### Color convention

All colors use `DebugRenderer`'s existing `0xRRGGBBAA` encoding (red in high
byte, alpha in low byte) as documented in
`2026-05-23-debug-renderer-m1-design.md`. No new encoding.

| Kind | Color | Packed |
|---|---|---|
| StaticProp | white | `0xFFFFFFFF` |
| Mech | yellow | `0xFFFF00FF` |
| Terrain | green | `0x44FF88FF` |

Full alpha (`AA = 0xFF`) on all.

### Geometry per kind

#### StaticProp

Primary (if AABB/footprint available from RenderObjectRecord or similar):
- `drawAabbWorld(mn, mx, color)` using world-space bounds

Fallback (if bounds not accessible):
- `drawRingWorld(worldXZ, 2.0f, 16, color)` — ring at hit point, radius 2 m
- Two `drawLineWorld` calls: vertical bracket ±3 m from worldY (bottom + top marker)

#### Mech

Primary (if AABB available):
- `drawAabbWorld(mn, mx, color)`

Fallback:
- `drawRingWorld(worldXZ, 4.0f, 16, color)` — ring at feet, radius 4 m

#### Terrain

Primary (if `hasTileCorners == true` in `TerrainInspectorData`):
- 4 `drawLineWorld` calls forming the tile outline (corner[0]→[1], [1]→[2],
  [2]→[3], [3]→[0])

Fallback (if `hasTileCorners == false`):
- 2 `drawLineWorld` calls: crosshair at hit point, ±1 m arms in X and Z

Hit-point cross (both primary and fallback):
- 2 `drawLineWorld` calls at worldY, ±0.5 m arms in X and Z (smaller inner cross)

### Persistence

Highlight draws every frame while `s_selection.hasSelection == true`. Clears
when `EditorInspector::clear()` is called (existing path, no change needed).

---

## File change summary

| File | Change |
|---|---|
| `GuiRuntime/EditorInspector.h` | New `InspectorPickKind` enum; updated `InspectorSelection`; new `TerrainInspectorData`; new `setTerrainData()` + `flushDebugHighlight()` declarations |
| `GuiRuntime/EditorInspector.cpp` | Implement `setTerrainData()`, terrain ImGui section, `flushDebugHighlight()` with DebugRenderer calls |
| `code/missiongui.cpp` | Terrain fallback branch in Ctrl+Shift+click handler; store worldXYZ in InspectorSelection for all pick kinds |
| Render loop caller | `EditorInspector::flushDebugHighlight()` call site before `gosPostProcess::process()` — exact file TBD during implementation |

No new CMake targets. No new headers outside `GuiRuntime/`. No terrain
object-ID writes.

---

## Open items for implementation

1. **Tile corners**: Determine if `Terrain::tileCellToWorld` (or equivalent) is
   accessible from missiongui.cpp cheaply enough to populate `cornerX/Y/Z[4]`.
   If so, set `hasTileCorners = true`. Otherwise accept crosshair-only for v1.

2. **Static prop bounds**: Check whether `RenderWorld::RenderObjectRecord` or a
   query function exposes world-space AABB for a given handle. If not available,
   use ring fallback.

3. **`flushDebugHighlight()` call site**: Identify the exact location in the
   frame where the scene FBO is still bound and the call can be safely inserted
   before `gosPostProcess::process()`. Document as part of implementation.
