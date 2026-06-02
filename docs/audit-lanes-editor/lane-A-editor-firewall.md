# Lane A — Editor Firewall Audit

**Auditor:** ED-AUDIT-FIREWALL (Sonnet lane)
**Date:** 2026-06-02
**Scope:** `EditRel` (editor/) + `mc2_asset_viewer` (tools/asset_viewer/)
**Worktree:** `A:/Games/mc2-trackv-ci-gate-restore`
**Mode:** READ-ONLY — no production code changed.

---

## 1. Firewall Violation Table

| ID | Sev | File:Line | Violation | Risk | Slice | Mitigating gates |
|----|-----|-----------|-----------|------|-------|-----------------|
| FW-01 | **P1** | `editor/EditorData.cpp:25-30` | Direct `#include` of GPU-batcher internals: `gos_static_prop_batcher.h`, `gos_mech_batcher.h`, `gos_static_prop_registry.h`, `gpu_cull_substrate.h`, `gpu_cull_compute.h`, `gpu_cull_readback.h` from editor source (not via GameAdapters) | Editor tightly couples to GPU pipeline lifecycle; any API change in those headers requires editor rebuild; lifecycle ordering must stay in sync with `mission.cpp` manually | M | firewall script does NOT watch `editor/` — only `RenderCore/`, `RenderWorld/`, `EditorBridge/`, etc. Documented in `EditorData.cpp` with "S2 mission-unload chain" comment. |
| FW-02 | **P1** | `editor/EditorInterface.cpp:3682-3720` | Direct raw GL calls (`glGetIntegerv`, `glBindFramebuffer`, `glReadBuffer`, `glReadPixels`) inside editor MFC pick handler — **outside** `EditorBridge` / `gameplay_pick.cpp` | Bypasses the `EditorBridge` abstraction the codebase explicitly designed; if FBO layout changes (attachment indices, format), editor pick silently misfires | S | Diagnostic code path (`s_pickDiagDone` gate; fires only until first successful hit). Partially mitigated: this is a scan *diagnostic* not the actual pick path. Labeled as `[EDITOR_PICK scan]`. |
| FW-03 | **P1** | `editor/EditorGameOS.cpp:490-504,548-567` | Raw GL (`glViewport`, `glEnable`, `glDepthFunc`, `glDepthMask`, `glClearColor`, `glClear`, `glBindFramebuffer`) in editor-owned frame loop | GL state managed outside the GameOS/gosRenderer state machine; can collide with gosRenderer state expectations across frame boundary | M | Comment at line 577 explicitly warns against `glUseProgram(0)` for this reason; partial discipline. `glBindFramebuffer(GL_FRAMEBUFFER,0)` restores default correctly. |
| FW-04 | **P2** | `editor/EditorGlobals.cpp:33-34` | `GameObjectManager* ObjectManager = nullptr;` defined via raw forward-decl (no include of `code/objmgr.h`). Null-stub is intentional, but the fwd-decl pattern means the actual class definition is never visible to the editor linker | ABI/layout mismatch risk if `GameObjectManager` size/vtable changes; editor would silently link against zero-byte stub | XS | Comment in file acknowledges this; `MechRenderAdapter` guards `ObjectManager == nullptr`. P2 because no current consumer exercises this path. |
| FW-05 | **P2** | `editor/EditorObjectMgr.cpp:119` | `#include "../GameOS/gameos/gos_static_prop_registry.h"` directly in `EditorObjectMgr.cpp` (not through an adapter) | Secondary coupling to registry internals; same issue as FW-01 but narrower (single header) | XS | Covered by same `check-include-firewall.allowlist` gap as FW-01 (firewall does not watch `editor/`). |

---

## 2. Acceptable Bridge Table

| Component | Coupling | Verdict |
|-----------|----------|---------|
| `EditorBridge/EditorRenderBridge.h` | Engine types only (`RenderCore/Handle.h`, `RenderWorld/VisibilityRequest.h`). No game headers. No GL. | CLEAN — narrow, one-way, typed API |
| `EditorBridge/EditorRenderBridge.cpp` | Carve-out; bridges game-side (`terrain.h`, `camera.h`) + engine-side (`RenderWorld.h`, `ScreenPick.h`) + GameOS | ACCEPTABLE — only permitted TU per allowlist |
| `editor/EditorInterface.cpp` pick path (non-diagnostic) | Uses `tryGameplayPick` (via `gameplay_pick.cpp` compiled into `EDITOR_BRIDGE_SOURCES`) + `gos_GetViewport` | ACCEPTABLE — `gameplay_pick.cpp` has no game-object deps per CMakeLists comment |
| `GameAdapters/StaticPropRenderAdapter`, `MechRenderAdapter` | Editor calls `GameAdapters::StaticProp::endMission()` / `GameAdapters::Mech::endMission()` — adapter layer used correctly | CLEAN |
| `EditorData.cpp` GPU lifecycle calls | Calls `GpuStaticPropBatcher::instance().onMapLoad/Unload()`, `GpuStaticPropRegistry::init/destroy()`, gpu_cull `*_shutdown()` | ARCH CONCERN — direct coupling (see FW-01), but functionally correct per documented "S2 mission-unload chain" |

---

## 3. `mc2_asset_viewer` Coupling Verdict

**CLEAN** — near-standalone render path, no engine/game internals breached.

Dependency chain:
- `main.cpp` / `AssetViewerApp.cpp`: SDL2, GLEW, ImGui only
- `TexturePreview2D.cpp`: uses `UiEditorImageCache` (ui_editor/)
- `UiEditorImageCache.cpp`: SDL (file I/O only via `SDL_RWops`), raw GL (for GL texture upload), `GameOS/gameos/utils/Image.cpp` for image decode
- No includes of `RenderWorld`, `RenderCore`, `mclib`, `GameAdapters`, `GameOS/include/gameos.hpp`
- No access to engine globals, game state, prop registries, or renderer internals
- Links only: `imgui`, `SDL2`, `OpenGL::GL`, `GLEW`, `ole32`, `windowscodecs`, `uuid`

The raw GL calls in `UiEditorImageCache.cpp` (glGenTextures, glBindTexture, glTexImage2D, glDeleteTextures) are **appropriate** — this is a standalone tool that owns its own GL context. No shared GL state with engine.

One note: `UiEditorImageCache_Get()` uses a `static UiEditorImageTexture result` local — caller must not cache the returned pointer across calls. Not a firewall issue, but a thread/re-entrancy footgun (P3/known debt).

---

## 4. Shared Mutable Global State

| State | Shared? | Risk |
|-------|---------|------|
| `ObjectManager` (EditorGlobals.cpp) | Editor defines null stub; game binary never loads this TU | No collision — different processes |
| `land` / `eye` externs (EditorRenderBridge.cpp) | Editor owns; game binary supplies from its own TUs | No collision — single-process single-binary; editor does not link game's `code/` |
| `g_cache` in UiEditorImageCache.cpp | asset_viewer-private (static within anon namespace) | No collision |
| `GpuStaticPropRegistry` / `GpuStaticPropBatcher` singletons | Editor calls directly (FW-01); no game process running simultaneously in editor mode | Low collision risk in practice; P1 arch concern remains |

---

## 5. Editor Inspect/Picking Ownership

- **Fast path** (normal pick): `tryGameplayPick` via `gameplay_pick.cpp` compiled as `EDITOR_BRIDGE_SOURCES` — no game-object deps; goes through `ScreenPick` in RenderWorld; clean.
- **Slow/diagnostic path**: Raw `glReadPixels` grid scan inside `EditorInterface.cpp:3682-3720` — **firewall breach** (FW-02). Gated by `!s_pickDiagDone`, fires only until first successful hit. Should be extracted to `EditorBridge` or removed when pick stabilizes.
- **Terrain path**: Via `EditorBridge::pickAt()` → `eye->inverseProject` + `land->worldToTile` — properly bridged.
- **Selection bounds drawing**: Via `EditorBridge::drawSelectionBounds` / `drawTerrainTileOutline` — clean, uses gos_DrawLines through GameOS, no raw GL.

---

## 6. Resource Ownership

No duplicate ownership found between `mc2_asset_viewer` and the engine:
- asset_viewer creates its own SDL/GL context; engine runs in a separate process or is not running.
- `UiEditorImageCache` manages its own `GLuint` texture names and deletes them on `Shutdown()`.
- No shared texture atlas, shader program, or FBO between asset_viewer and engine.

---

## 7. Firewall Script Coverage Gap

`scripts/check-include-firewall.sh` watches:
```
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL EditorBridge"
```

**`editor/` is NOT in SCOPE_DIRS.** Violations FW-01, FW-03, FW-05 are undetectable by the current pre-commit gate. The firewall script covers engine boundaries, not editor→engine boundaries.

**Recommended:** Add `editor` to `SCOPE_DIRS` with a targeted allowlist covering the `EditorData.cpp` and `EditorGameOS.cpp` grandfathered direct GPU calls, or wrap those calls behind `EditorBridge`.

---

## 8. Recommended Fixes

| ID | Slice | Action |
|----|-------|--------|
| FW-01 | M (Opus) | Introduce `EditorBridge::initMapGpu()` / `EditorBridge::shutdownMapGpu()` thin wrappers over `GpuStaticPropBatcher`, `GpuStaticPropRegistry`, `gpu_cull_*`. Editor calls bridge, not internal headers. Add those headers to firewall allowlist only for `EditorBridge/EditorRenderBridge.cpp`. |
| FW-02 | S | Extract the OID scan diagnostic block (lines 3682–3734 of `EditorInterface.cpp`) into a helper in `EditorBridge` (e.g. `EditorBridge::diagScanObjectIdBuffer()`). Remove raw GL from `EditorInterface.cpp`. Once pick is stable, delete the diagnostic entirely. |
| FW-03 | M | Move per-frame raw GL setup in `EditorGameOS.cpp` into a `gosRenderer_EditorBeginFrame()` / `gosRenderer_EditorEndFrame()` shim that goes through GameOS render API. Belt-and-suspenders `glBindFramebuffer(0)` before ImGui pass can stay as guard. |
| FW-04 | XS | Replace fwd-decl stub `class GameObjectManager; GameObjectManager* ObjectManager = nullptr;` with a proper `#include "code/objmgr.h"` under a compile-guard, or formalize as a typed null-sentinel in a mclib header. |
| FW-05 | XS | Add `editor/EditorObjectMgr.cpp` to `check-include-firewall.allowlist` or route `GpuStaticPropRegistry` access through `EditorBridge`. |
| Coverage | S | Add `editor` to `SCOPE_DIRS` in `scripts/check-include-firewall.sh` with appropriate allowlist entries for grandfathered direct GPU calls. |
