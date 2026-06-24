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

---

## Slice 2 — EDITOR-BRIDGE-GPU-FIREWALL-1: editor GPU access is MANDATORY-bridged + firewalled

### Summary

Made the EditorBridge **mandatory** for GPU access: no editor TU includes the
GPU-internal batcher/registry/cull headers directly anymore. All static-prop
batcher, mech batcher, static-prop registry, and `gpu_cull` compute access from
`editor/` now routes through new `EditorBridge::*` lifecycle passthroughs. Then
added `editor/` to the include-firewall checker (header-only scope) so future
direct includes are blocked at check time.

### Leak inventory (verified — file:line:symbol, before this slice)

Direct GPU-internal **includes** removed from editor TUs:

| File:line | Header |
|---|---|
| `editor/EditorCamera.h:36` | `gos_static_prop_registry.h` |
| `editor/EditorCamera.h:37` | `gos_mech_batcher.h` |
| `editor/EditorData.cpp:25` | `gos_static_prop_batcher.h` |
| `editor/EditorData.cpp:29` | `gos_mech_batcher.h` |
| `editor/EditorData.cpp:30` | `gos_static_prop_registry.h` |
| `editor/EditorData.cpp:32` | `gpu_cull_compute.h` |
| `editor/EditorObjectMgr.cpp:49` | `gos_static_prop_batcher.h` |
| `editor/EditorObjectMgr.cpp:127` | `gos_static_prop_registry.h` |

Direct GPU symbol **call sites** these includes enabled (the actual usage surface):

| File:line | Symbol called | Wrapped by |
|---|---|---|
| `EditorCamera.h:270` | `GpuStaticPropRegistry::frameBegin()` | `staticPropFrameBegin()` |
| `EditorCamera.h:275` | `GpuMechBatcher::instance().finalizePending()` | `mechFinalizePending()` |
| `EditorData.cpp:181-186` | `gpu_cull::readback_shutdown/compute_shutdown/substrate_shutdown` + `GpuStaticPropBatcher::onMapUnload` + `GpuMechBatcher::onMapUnload` + `GpuStaticPropRegistry::destroy` (6-step teardown) | `endMissionRenderResources()` |
| `EditorData.cpp:456-458` | `GpuStaticPropBatcher::onMapLoad` + `GpuMechBatcher::onMapLoad` + `GpuStaticPropRegistry::init` | `beginMissionRenderResources()` |
| `EditorData.cpp:470` | `gpu_cull::compute_init()` | `cullComputeInit()` |
| `EditorData.cpp:536-539` | `GpuStaticPropBatcher::finalizeGeometry` + `GpuMechBatcher::finalizeGeometry` + `gpu_cull::compute_isEnabled` + `gpu_cull::compute_buildIndirectBuffer(batcher_getTypeCount())` | `finalizeMissionGeometry()` |
| `EditorData.cpp:1674-1720` | second copy of the begin/cull-init/finalize chain (FromTGA new-map path) | same wrappers |
| `EditorObjectMgr.cpp:2232-2233` | `GpuStaticPropBatcher::instance().registerMultiShape(TG_TypeMultiShape*)` | `registerStaticPropShape(void*)` |
| `EditorObjectMgr.cpp:3381` | `GpuStaticPropRegistry::isMissionLoadRegEnabled()` | `staticPropMissionLoadRegEnabled()` |

Notes:
- `gpu_cull_substrate.h` / `gpu_cull_readback.h` are **not** in scope for this
  slice (not GPU-internal batcher/registry headers); `substrate_init` /
  `readback_init` calls remain inline in editor and those includes stay. Only the
  four headers in the leak table are firewalled in `editor/`.
- `EditorInterface.cpp:786` (`gpu_cull::substrate_frameBegin`) is left as-is —
  it uses `gpu_cull_substrate.h`, out of this slice's scope.
- Comment-only references (`EditorCamera.h:205,324`, several `EditorObjectMgr`
  doc comments, `EditorInterface.cpp:4070`) are not violations — the checker
  strips comment lines and they were left intact.

### Bridge methods added (`EditorBridge/EditorRenderBridge.{h,cpp}`)

```cpp
void beginMissionRenderResources();        // onMapLoad x2 + registry::init
void endMissionRenderResources();          // 6-step locked teardown (cull/batcher/registry)
void finalizeMissionGeometry();            // batcher finalize x2 + compute indirect (if enabled)
void staticPropFrameBegin();               // registry::frameBegin()
void mechFinalizePending();                // mech batcher finalizePending()
void cullComputeInit();                    // gpu_cull::compute_init()
void registerStaticPropShape(void* multiShape);   // batcher registerMultiShape; void* keeps header GPU-free
bool staticPropMissionLoadRegEnabled();    // registry::isMissionLoadRegEnabled()
```

All return plain `int`/`bool`/`void` and take `void*` for the one shape handle —
**no GPU/Stuff struct crosses the bridge header**. These lifecycle methods are
intentionally NOT gated on `s_enabled` (`MC2_EDITOR_MODE`): the batcher lifecycle
must run on every editor map load regardless of the pick-bridge flag, mirroring
the game runtime. The carve-out TU `EditorRenderBridge.cpp` gains the four
GPU-internal includes (plus `gpu_cull_substrate.h`/`gpu_cull_readback.h` for the
teardown), which is already allowlisted.

### Files de-leaked

- `editor/EditorCamera.h` — frame-loop registry/mech calls + 2 includes
- `editor/EditorData.cpp` — both mission init chains + teardown + 4 includes
- `editor/EditorObjectMgr.cpp` — registerMultiShape + isMissionLoadRegEnabled + 2 includes

### Checker change

`scripts/check-include-firewall.sh`:
- Added a **header-only** scope `EDITOR_HEADER_ONLY_DIRS="editor"` checked against
  `EDITOR_FORBIDDEN_HEADERS` (the four GPU-internal headers ONLY).
- `editor/` is deliberately **NOT** subjected to `FORBIDDEN_SYMBOLS`
  (`Appearance`/`ObjectManager`/`Mission`/...): the mission editor legitimately
  references those game-logic symbols through its own headers. Firewalling them
  would produce thousands of false positives and is not the goal of this slice —
  the goal is the GPU batcher/registry/cull headers.

### Residual allowlist debt

**None.** Every in-scope symbol was wrapped cleanly. The one GPU-typed argument
(`TG_TypeMultiShape*` to `registerMultiShape`) is passed as `void*` through the
bridge and cast inside the carve-out TU, so no editor TU needed a residual
allowlist carve-out.

### Negative test

Reintroducing a direct include in any `editor/` TU now fails the checker, e.g.
`#include "../GameOS/gameos/gos_mech_batcher.h"` in `editor/EditorData.cpp`
triggers:

```
VIOLATION: editor/ must route GPU access through EditorBridge -- forbidden include of gos_mech_batcher.h in editor/EditorData.cpp:NN:#include "../GameOS/gameos/gos_mech_batcher.h"
```

(verified by temporary reintroduction, then reverted.)

### Checker output (this slice's tree)

`editor/` scope is **clean** (0 editor violations). The checker still reports 2
**pre-existing, unrelated** violations in `RenderCore/RendererFeatureRegistry.h`
(symbol grep matching `TreeAppearance`/`MechWarrior` inside multi-line string
literals — present on the base commit before this slice, confirmed via stash).
Those are outside this slice's scope (a separate symbol-grep-vs-string-literal
checker bug) and are not introduced here.

### Needs build-time verification

- Full editor (`EditRel.exe`) compile/link: confirm `EditorRenderBridge.h` is
  reachable from `editor/EditorCamera.h` (a header — pulled into multiple TUs)
  and the two editor `.cpp`s; confirm no ODR / include-order issue from moving
  the batcher includes behind the bridge.
- Confirm `TG_TypeMultiShape*` → `void*` round-trip in `registerStaticPropShape`
  compiles cleanly from `EditorObjectMgr.cpp` (the editor passes
  `bat->bldgShape[lod]` / `bat->bldgDmgShape`, whose static type is
  `TG_TypeMultiShape*` from the appearance headers, not the batcher header).
- Behavioural parity: editor map load/unload (both PCV-load and TGA-new-map
  paths) must still finalize batcher geometry and tear down in the locked order.
  The lifecycle wrappers preserve the exact call order; a smoke-equivalent editor
  load/unload is the real check (no `mc2` smoke tier covers `EditRel.exe`).
