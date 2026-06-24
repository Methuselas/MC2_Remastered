# Editor / Game Runtime Parity Arc

Branch: `claude/editor-game-runtime-parity`.

Goal: bring the mission editor's render/pick paths onto the same modern
(`RenderWorld` / `EditorBridge`) seams the game runtime uses, retiring the
legacy CPU-only editor paths to documented fallbacks.

---

## Slice 3 — EDITOR-OBJECTID-PICK-BRIDGE-1: GPU ObjectID pick is PRIMARY

### Summary

The editor's primary single-click object selection now resolves through the GPU
ObjectID buffer via `EditorBridge::pickAt()` first, and only falls back to the
legacy CPU forward-projection picker (`EditorObjectMgr::getObjectAtScreenPosition`)
on a terrain hit, a miss, or when the killswitch disables the GPU path.

### Path

```
EditorInterface::OnLButtonUp (currentBrushID == IDS_SELECT, true non-drag click)
  -> EditorInterface_SelectObjectAtScreenPoint
    -> EditorInterface_PickObjectAtScreenPoint            <-- REWIRED (Slice 3)
         1. EditorBridge::pickAt(screenX, screenY)        (GPU ObjectID readback)
              kind == StaticProp -> reverse-lookup -> EditorObject  (return)
              kind == Mech       -> findUnitByMechHandle -> Unit     (return)
              kind == Terrain/Miss / unresolved -> fall through
         2. EditorInterface_PickObjectAtScreenPoint_CPU   (legacy fallback)
              -> EditorObjectMgr::getObjectAtScreenPosition (CPU forward-project)
```

The press-time drag-grab picker (`OnLButtonDown`, line ~1606) intentionally still
calls the CPU picker directly — it only grabs an object for a potential drag and
does not own selection, so it is left unchanged.

### Gate

- **`MC2_EDITOR_GPU_PICK`** — default **ON** (`envFlagDefaultOn` convention:
  unset -> on, exactly `"0"` -> off, any other value -> on).
- Killswitch: `MC2_EDITOR_GPU_PICK=0` reverts the primary path to the CPU
  forward-projection picker (full prior behaviour).
- The GPU path additionally requires `EditorBridge::isEnabled()` (i.e.
  `MC2_EDITOR_MODE=1`) and, inside `pickAt`, `MC2_OBJECT_ID_BUFFER=1`
  (default ON). If the OID buffer is off, `pickAt` returns Terrain/Miss and the
  CPU fallback runs — not an error.

### Reverse-lookup contract (the core of this slice)

`EditorBridge::pickAt()` returns an `EditorPickResult` carrying a
`RenderCore::RenderObjectHandle` for StaticProp/Mech hits. We must map that
handle back to the `EditorObject` the user clicked.

**StaticProp reversal** (`EditorObjectMgr::findObjectByStaticRecipeIndex`):
- `handle.index()` **equals** the `GpuStaticPropRegistry` `recipeIndex`. This is
  an identity mapping documented at `RenderWorld.h` (`handleToRecipeIndex`,
  `StaticPropRecordView::recipeIndex` comment: "== handle.index() == slot index").
- Each editor object's appearance carries that same `recipeIndex` via the
  `Appearance::getStaticRecipeIndex()` virtual (overridden by `BldgAppearance`
  and `TreeAppearance` to return `staticReg.recipeIndex`; base returns -1).
- Reversal = linear scan over `buildings` + `units` for the object whose
  `appearance()->getStaticRecipeIndex() == recipeIndex`. O(props) per click,
  click-rate only. `recipeIndex < 0` is guarded (the "unregistered" sentinel
  must never match).
- This mirrors the **M2.6 recon Option D** philosophy (resolve a GPU-side stable
  id back to a game object at pick time, in the adapter/editor layer) but uses
  the already-existing `recipeIndex` as the stable id rather than adding a new
  `gameObjectId` cookie — no RenderWorld or registry change required, and it
  stays entirely editor-side (no include-firewall impact).

**Mech reversal** (`EditorObjectMgr::findUnitByMechHandle`, pre-existing):
- Compares `Mech3DAppearance::getRenderWorldHandle().bits == handle.bits`.
- Best-effort for the editor: mechs are rarely placed in editor scenes, and
  mech `gameObjectId` is still 0 (`MechRenderAdapter.cpp`), but the **handle
  bits are the stable identity** here (per M2.6 CRITICAL-1: handle IS the
  identity for mechs), so this resolves cleanly when an editor mech exists.
  Unresolved -> CPU fallback (documented skip).

### Fallback semantics

- Terrain hit / background miss / GPU path disabled / static prop with no owning
  EditorObject (e.g. terrain-baked vegetation forests) / unresolved mech ->
  CPU forward-projection picker runs, preserving the legacy "nearest projected
  object" behaviour exactly. The GPU path is strictly additive on the
  object-hit case.

### Inspector path (Ctrl+Shift+LMB) — deliberately NOT rerouted

The Ctrl+Shift+LMB inspector pick (`OnLButtonUp`, `tryGameplayPick`) still calls
`RenderWorld::lookupAtPixel` directly (via `code/gameplay_pick.cpp`), NOT
`EditorBridge::pickAt`. Reason: the inspector consumes the **full
`RenderWorld::LookupResult`** (`rawObjectId`, `lookupFailReason`, mesh/material/
pipeline/drawPacket fields) via `EditorInspector::setPickResult`. `pickAt`
intentionally returns the **reduced** `EditorPickResult` and does not surface
`LookupResult` (bridge firewall: editor code sees a minimal struct). Unifying
onto a single seam would require widening `EditorPickResult` to carry the full
`LookupResult`, breaking the bridge's reduction/firewall intent — deferred to a
later slice as it is a debug-only consumer, not the primary selection path.

### Files changed

- `editor/EditorInterface.cpp`
  - include `../EditorBridge/EditorRenderBridge.h`
  - `EditorInterface_GpuPickEnabled()` (new, env gate)
  - `EditorInterface_PickObjectAtScreenPoint_CPU()` (renamed legacy body)
  - `EditorInterface_PickObjectAtScreenPoint()` (rewired: GPU first, CPU fallback)
- `editor/EditorObjectMgr.h` / `editor/EditorObjectMgr.cpp`
  - `EditorObjectMgr::findObjectByStaticRecipeIndex(int32_t recipeIndex)` (new
    reversal)

### Needs build-time verification

- Full editor (`EditRel.exe`) compile/link: bridge header reachable from
  `editor/` TU; `EditorPickResult` / `RenderObjectHandle::index()` usage.
- Coordinate parity: `pickAt` does its own `gos_GetViewport` +
  `editorScreenToFboPixel` transform; confirm it agrees with the viewport-local
  coords the call sites already pass (`EditorRttClientToViewport` applied before
  `EditorInterface_SelectObjectAtScreenPoint`). RTT vs non-RTT both need a click
  test.
- Live click test: select a building/tree via GPU path; confirm the same object
  the CPU path would have picked; confirm `MC2_EDITOR_GPU_PICK=0` reverts.
