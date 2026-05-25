# Mission Editor RenderBridge v0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `EditorBridge::*` v0 — the single API seam through which the mission editor calls into the nifty 0.4 rendering spine (RenderWorld pick, visibility, and debug-draw), without touching game-side frame state.

**Architecture:** New `EditorBridge/` static library sits alongside `GameAdapters/` as the editor-side carve-out; only `EditorRenderBridge.cpp` may bridge both sides. `RenderWorld/ScreenPick.h` provides a **pure-math** coord-transform struct and inline function (no GameOS dependency); each consumer (`gameplay_pick.cpp`, `EditorRenderBridge.cpp`) owns its own private runtime wrapper that calls `gos_GetViewport`. Four public functions are implemented: `pickAt`, `queryVisibility`, `drawSelectionBounds`, `drawTerrainTileOutline`.

**Tech Stack:** C++17, OpenGL 4.5 (via GameOS), doctest (unit tests), MSVC RelWithDebInfo.

**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`

**Spec:** `docs/superpowers/specs/mission-editor-render-bridge-v0-spec.md`

---

## File Map

**Create:**
- `RenderWorld/ScreenPick.h` — `ScreenPickContext` struct + inline `screenPickCompute`. Pure math only — NO `gos_GetViewport`, NO `screenToFboPixel` declaration, NO `.cpp` companion.
- `tests/unit/test_screen_pick.cpp` — doctest cases for `screenPickCompute`
- `EditorBridge/CMakeLists.txt` — `editorbridge` static lib
- `EditorBridge/EditorRenderBridge.h` — public API header (engine types only)
- `EditorBridge/EditorRenderBridge.cpp` — all four functions + private runtime wrapper (the one carve-out file)

**Modify:**
- `tests/unit/CMakeLists.txt` — add `test_screen_pick.cpp`; add `RenderWorld/` to include dirs
- `code/gameplay_pick.h` — remove public `screenToFboPixel` declaration
- `code/gameplay_pick.cpp` — add `#include "../RenderWorld/ScreenPick.h"`; update local `screenToFboPixel` to delegate math to `RenderWorld::screenPickCompute` (keeps its own `gos_GetViewport` call)
- `scripts/check-include-firewall.sh` — add `EditorBridge` to `SCOPE_DIRS`
- `scripts/check-include-firewall.allowlist` — add `EditorRenderBridge.cpp` entry
- `CMakeLists.txt` (root) — `add_subdirectory` for `EditorBridge`; link `editorbridge` into `mc2`
- `docs/tier1_env_vars.md` — document `MC2_EDITOR_MODE`

**NOT modified** (firewall discipline):
- `RenderWorld/CMakeLists.txt` — ScreenPick is header-only; no new source file
- `RenderWorld/RenderWorld.h/.cpp` — untouched

---

## Task 1: Extract pure `screenPickCompute` to `RenderWorld/ScreenPick.h`

**Firewall discipline:** This header is pure math. It must include only `<cstdint>`. No `gameos.hpp`, no `Environment`, no `gos_GetViewport`. The runtime wrapper that reads viewport state stays private to each consumer TU.

**Files:**
- Create: `RenderWorld/ScreenPick.h`
- Create: `tests/unit/test_screen_pick.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1.1: Create `RenderWorld/ScreenPick.h`**

```cpp
// RenderWorld/ScreenPick.h
//
// Pure screen-to-FBO-pixel coordinate math, extracted from
// code/gameplay_pick.cpp. Consumed by:
//   - code/gameplay_pick.cpp    (gameplay pick spine; owns its own gos_GetViewport wrapper)
//   - EditorBridge/EditorRenderBridge.cpp  (editor pick; owns its own gos_GetViewport wrapper)
//
// Firewall: includes only <cstdint>. No game, GL, or GameOS headers.
// No screenToFboPixel declaration here -- that function lives in each consumer
// as a private static, because it calls gos_GetViewport (GameOS runtime).
// Keeping GameOS out of RenderWorld is a load-bearing firewall invariant.

#pragma once
#include <cstdint>

namespace RenderWorld {

// All fields. Populated by the caller's local screenToFboPixel wrapper,
// or set manually for unit testing (no GL context required).
struct ScreenPickContext {
    int   mouseX = 0, mouseY = 0;    // Win32 origin top-left (input)
    int   glX = 0,    glY = 0;       // GL origin bottom-left (output)
    int   fboX = 0,   fboY = 0;      // FBO pixel, top-left origin (output)
    int   drawableWidth  = 0;
    int   drawableHeight = 0;
    float vMulX = 0.f, vMulY = 0.f;  // gos_GetViewport scale (FBO-pixel units)
    float vAddX = 0.f, vAddY = 0.f;  // gos_GetViewport offset
};

// Pure coord computation. Caller populates mouseX/Y and all viewport fields
// before calling. Fills fboX/Y and glX/Y. No-op if vMulX/Y <= 0.
//
// Derivation: fboX = vAddX + mouseX * (drawableWidth / vMulX)
//             glY  = drawableHeight - 1 - fboY  (GL y-flip)
// Identical math to the former gameplay_pick.cpp screenToFboPixel.
inline void screenPickCompute(ScreenPickContext* ctx) {
    if (!ctx || ctx->vMulX <= 0.0f || ctx->vMulY <= 0.0f) return;
    const float scaleX = static_cast<float>(ctx->drawableWidth)  / ctx->vMulX;
    const float scaleY = static_cast<float>(ctx->drawableHeight) / ctx->vMulY;
    ctx->fboX = static_cast<int>(ctx->vAddX + static_cast<float>(ctx->mouseX) * scaleX);
    ctx->fboY = static_cast<int>(ctx->vAddY + static_cast<float>(ctx->mouseY) * scaleY);
    ctx->glX  = ctx->fboX;
    ctx->glY  = ctx->drawableHeight - 1 - ctx->fboY;
}

} // namespace RenderWorld
```

- [ ] **Step 1.2: Write unit tests for `screenPickCompute`**

Create `tests/unit/test_screen_pick.cpp`:

```cpp
// tests/unit/test_screen_pick.cpp
// Unit tests for the screenPickCompute pure coord transform.
// Does NOT test any runtime wrapper (those call gos_GetViewport / require a GL context).

#include "doctest.h"
#include "../../RenderWorld/ScreenPick.h"

TEST_CASE("screenPickCompute full-screen identity: mouse pixel == FBO pixel") {
    // When vMulX == drawableWidth (full-screen, no sub-viewport offset),
    // screen pixel == FBO pixel. The only transform is the GL y-flip.
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 100; ctx.mouseY = 50;
    ctx.vMulX  = 800.f; ctx.vMulY  = 600.f;
    ctx.vAddX  = 0.f;   ctx.vAddY  = 0.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboX == 100);
    CHECK(ctx.fboY == 50);
    CHECK(ctx.glX  == 100);
    CHECK(ctx.glY  == 549);  // 600 - 1 - 50
}

TEST_CASE("screenPickCompute GL y-flip: top of screen maps to glY == drawableHeight-1") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 0; ctx.mouseY = 0;
    ctx.vMulX  = 800.f; ctx.vMulY  = 600.f;
    ctx.vAddX  = 0.f;   ctx.vAddY  = 0.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboY == 0);
    CHECK(ctx.glY  == 599);  // GL origin is bottom-left
}

TEST_CASE("screenPickCompute bottom of screen maps to glY == 0") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 0; ctx.mouseY = 599;
    ctx.vMulX  = 800.f; ctx.vMulY  = 600.f;
    ctx.vAddX  = 0.f;   ctx.vAddY  = 0.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboY == 599);
    CHECK(ctx.glY  == 0);
}

TEST_CASE("screenPickCompute sub-viewport with offset") {
    // vMulX=640, drawableWidth=800 -> scaleX=800/640=1.25
    // vAddX=80 -> fboX = 80 + 100*1.25 = 205
    // vAddY=60 -> fboY = 60 + 0*1.25   = 60
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 100; ctx.mouseY = 0;
    ctx.vMulX  = 640.f; ctx.vMulY  = 480.f;
    ctx.vAddX  = 80.f;  ctx.vAddY  = 60.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboX == 205);
    CHECK(ctx.fboY == 60);
    CHECK(ctx.glX  == 205);
    CHECK(ctx.glY  == 539);  // 600 - 1 - 60
}

TEST_CASE("screenPickCompute degenerate viewport (vMulX==0): no-op") {
    RenderWorld::ScreenPickContext ctx{};
    ctx.mouseX = 100; ctx.mouseY = 50;
    ctx.vMulX  = 0.f;   ctx.vMulY = 600.f;
    ctx.drawableWidth = 800; ctx.drawableHeight = 600;
    ctx.fboX = 999; ctx.glX = 999;  // sentinel
    RenderWorld::screenPickCompute(&ctx);
    CHECK(ctx.fboX == 999);  // unchanged
    CHECK(ctx.glX  == 999);  // unchanged
}
```

- [ ] **Step 1.3: Update `tests/unit/CMakeLists.txt` to wire the test**

Add `test_screen_pick.cpp` to the executable sources and `RenderWorld/` to the include path:

```cmake
add_executable(mc2_tests
    test_main.cpp
    test_hashing.cpp
    test_projection.cpp
    test_argb_pack.cpp
    test_screen_pick.cpp          # NEW

    ${MC2_WORKTREE_ROOT}/mclib/fst_hash.cpp
)

target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}/mclib
    ${MC2_WORKTREE_ROOT}/GameOS
    ${MC2_WORKTREE_ROOT}/RenderWorld    # NEW -- for ScreenPick.h
    ${CMAKE_CURRENT_SOURCE_DIR}
)
```

(All other lines unchanged.)

- [ ] **Step 1.4: Build unit tests and verify they pass**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64-tests --config RelWithDebInfo --target mc2_tests 2>&1 | Select-Object -Last 5
A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64-tests/RelWithDebInfo/mc2_tests.exe
```

Expected output includes: `[doctest] test cases: 5 | 5 passed | 0 failed` (the 5 new screen-pick cases).

- [ ] **Step 1.5: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add `
    RenderWorld/ScreenPick.h `
    tests/unit/test_screen_pick.cpp `
    tests/unit/CMakeLists.txt
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
feat: extract screenPickCompute to RenderWorld/ScreenPick.h (pure math, header-only)

ScreenPickContext struct + inline screenPickCompute factored out of
gameplay_pick.cpp. Header-only (no .cpp) -- gos_GetViewport stays in each
consumer as a private static. RenderWorld stays GameOS-free.
5 doctest cases verify the pure math transform.

Prerequisite for EditorBridge v0 (mission-editor-render-bridge-v0-spec.md).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Update `gameplay_pick.cpp` — local runtime wrapper + pure compute

**Goal:** `gameplay_pick.cpp` keeps owning `gos_GetViewport` (its private concern). The local `screenToFboPixel` static now delegates the pure math to `RenderWorld::screenPickCompute` instead of duplicating it. The public declaration in `gameplay_pick.h` is removed (function becomes private to the TU).

**Files:**
- Modify: `code/gameplay_pick.h` — remove public `screenToFboPixel` declaration
- Modify: `code/gameplay_pick.cpp` — add `ScreenPick.h` include; update local wrapper to call `screenPickCompute`

- [ ] **Step 2.1: Confirm `screenToFboPixel` location in `gameplay_pick`**

```powershell
Select-String -Path "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gameplay_pick.h" -Pattern "screenToFboPixel" -Context 1,1
Select-String -Path "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gameplay_pick.cpp" -Pattern "screenToFboPixel" -Context 2,2
```

Expected: declaration in `.h`, full definition + call site in `.cpp`. Note exact line numbers for the next steps.

- [ ] **Step 2.2: Remove `screenToFboPixel` declaration from `code/gameplay_pick.h`**

Remove lines 80–95 (the `screenToFboPixel` declaration block). The declaration is being removed because the function becomes a `static` internal to `gameplay_pick.cpp` — not part of the public contract.

After the edit, the end of the file should look like:

```cpp
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req);

// Validation-gate self-test.
void RunGameplayPickSelfTest();

#endif // GAMEPLAY_PICK_H
```

- [ ] **Step 2.3: Update `code/gameplay_pick.cpp` — three surgical changes only**

Do NOT rewrite `tryGameplayPick`. M1.6 has coord-scaling fixes and diagnostic fields that must not be clobbered. Make exactly three changes:

**Change A — add include** (after the existing includes block, before any function definitions):
```cpp
#include "../RenderWorld/ScreenPick.h"  // ScreenPickContext, screenPickCompute
```

**Change B — update the local `screenToFboPixel` math body only.**

Read the existing `screenToFboPixel` static in `gameplay_pick.cpp` first. It contains the inline coord math (4–6 lines). Replace ONLY that math with a call to `RenderWorld::screenPickCompute`. Keep the function signature, the `gos_GetViewport` call, the `Environment.drawableWidth/Height` reads, and the return value logic exactly as they are. The only change is:

Before (the math lines — exact content varies; read the file):
```cpp
    // ... existing fboX/fboY/glX/glY computation (4-6 lines) ...
```

After:
```cpp
    RenderWorld::screenPickCompute(out);
```

The struct type of `out` may need changing from a local struct to `RenderWorld::ScreenPickContext*` — adjust the parameter type to match. Keep the field names (`mouseX`, `mouseY`, `vMulX`, `vMulY`, `vAddX`, `vAddY`, `drawableWidth`, `drawableHeight`, `fboX`, `fboY`, `glX`, `glY`) — they are identical between the old local struct and `ScreenPickContext`.

**Change C — update call site in `tryGameplayPick`.**

The call site currently declares a local struct and passes `&ctx` (or similar). Change the type to `RenderWorld::ScreenPickContext` so the compiler can verify the field names. Do not change any other logic in `tryGameplayPick`. If `r.ctx` is a different struct type than `RenderWorld::ScreenPickContext`, mirror the fields it needs one by one (as the original plan did); do not introduce new fields.

**Verification before committing:** run a `git diff code/gameplay_pick.cpp` and confirm the diff shows only the include addition, the struct type rename, and the math-body replacement. If the diff is larger than ~30 lines, STOP and raise a deviation — do not commit a silent wholesale rewrite.

- [ ] **Step 2.4: Build main project**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo 2>&1 | Select-Object -Last 10
```

Expected: `mc2.exe` with 0 errors.

- [ ] **Step 2.5: Run firewall script**

```powershell
sh A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
```

Expected: `clean`. (`code/gameplay_pick.cpp` is not in `SCOPE_DIRS`; `RenderWorld/ScreenPick.h` is clean — no forbidden includes.)

- [ ] **Step 2.5b: Verify RenderWorld stays GameOS-free**

```powershell
Select-String -Path "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/RenderWorld/*" -Pattern "gos_GetViewport|gameos\.hpp|Environment\." -Recurse
```

Expected: **no hits**. Any match here means `gos_GetViewport` or a GameOS header leaked into `RenderWorld/` — STOP and raise a deviation before committing.

- [ ] **Step 2.6: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add `
    code/gameplay_pick.h `
    code/gameplay_pick.cpp
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
refactor: gameplay_pick screenToFboPixel delegates math to RenderWorld::screenPickCompute

Local static wrapper keeps gos_GetViewport ownership in gameplay_pick.cpp
(RenderWorld stays GameOS-free). Pure math factored to screenPickCompute.
Public declaration removed from gameplay_pick.h (function is now TU-private).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Firewall Enforcement for EditorBridge

**Files:**
- Modify: `scripts/check-include-firewall.sh`
- Modify: `scripts/check-include-firewall.allowlist`

- [ ] **Step 3.1: Add `EditorBridge` to SCOPE_DIRS in `check-include-firewall.sh`**

Change the `SCOPE_DIRS` line from:
```sh
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL"
```
to:
```sh
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL EditorBridge"
```

- [ ] **Step 3.2: Add `EditorRenderBridge.cpp` to the allowlist**

Append to `scripts/check-include-firewall.allowlist`:

```
# EditorBridge/EditorRenderBridge.cpp -- sole carve-out file in EditorBridge/.
# Bridges game-side (terrain.h, camera.h, gameos.hpp) and engine-side
# (RenderWorld.h, ScreenPick.h) headers. All other EditorBridge/ files are
# engine-only and fully watched by the firewall.
EditorBridge/EditorRenderBridge.cpp
```

- [ ] **Step 3.3: Run firewall script — verify clean (EditorBridge/ dir doesn't exist yet)**

```powershell
sh A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
```

Expected: `clean` (the `[ -d ] || continue` guard skips missing dirs).

- [ ] **Step 3.4: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add `
    scripts/check-include-firewall.sh `
    scripts/check-include-firewall.allowlist
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
build: add EditorBridge to include-firewall scope + allowlist entry

EditorBridge/ added to SCOPE_DIRS so all headers there are watched.
EditorRenderBridge.cpp allowlisted as the one permitted carve-out TU
(bridges both game-side and engine-side headers by design).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: EditorBridge Skeleton (header, CMake, init/shutdown, env var docs)

**Initialization contract (M3 fix — load-bearing):**
`EditorBridge::init()` is NOT called automatically by the mc2 game startup. The mission editor integration is responsible for calling `EditorBridge::init()` before using any API function. If `init()` is never called, `s_enabled` stays `false` and all functions are no-ops. This is intentional — the bridge is invisible to the game path when not explicitly activated.

**Files:**
- Create: `EditorBridge/EditorRenderBridge.h`
- Create: `EditorBridge/CMakeLists.txt`
- Create: `EditorBridge/EditorRenderBridge.cpp` (skeleton — lifecycle + 4 stubs)
- Modify: `CMakeLists.txt` (root)
- Modify: `docs/tier1_env_vars.md`

- [ ] **Step 4.1: Create `EditorBridge/EditorRenderBridge.h`**

```cpp
// EditorBridge/EditorRenderBridge.h
//
// EditorBridge v0: stable API seam for the mission editor.
// Editor code calls ONLY this namespace -- never RenderWorld/GameAdapters/
// gameplay_pick/gos_postprocess directly.
//
// Initialization: the mission editor MUST call EditorBridge::init() before
// using any other function. mc2 game startup does NOT auto-call init().
// If init() is never called, all functions are no-ops (s_enabled=false).
//
// Firewall: this header includes engine types only.
//   - RenderCore/Handle.h              (RenderObjectHandle)
//   - RenderWorld/VisibilityRequest.h  (VisibilityRequest, VisibilityResult)
// No game headers. No GL. No GameOS. No Stuff/.

#pragma once

#include <cstdint>
#include "../RenderCore/Handle.h"
#include "../RenderWorld/VisibilityRequest.h"

namespace EditorBridge {

// ---- Lifecycle ----

// Call once at editor startup. Reads MC2_EDITOR_MODE (process-lifetime cached).
// All other functions are no-ops until this has been called with MC2_EDITOR_MODE=1.
void init();

// Call once at editor shutdown.
void shutdown();

// True iff MC2_EDITOR_MODE=1 was set at init() time.
bool isEnabled();

// ---- Types ----

struct EditorPickResult {
    enum class Kind : uint8_t {
        Miss       = 0,  // no hit (background, env-OFF, out-of-bounds)
        StaticProp = 1,  // building / tree / prop -- handle valid
        Mech       = 2,  // unit -- handle valid
        Terrain    = 3,  // terrain surface -- tile fields valid; handle invalid
    };

    Kind kind = Kind::Miss;

    // Valid when kind == StaticProp or kind == Mech.
    RenderCore::RenderObjectHandle handle =
        RenderCore::RenderObjectHandle::invalid();

    // Valid when kind == Terrain (-1 otherwise).
    int32_t terrainTileRow   = -1;
    int32_t terrainTileCol   = -1;
    int32_t terrainType      = -1;   // short->int from Terrain::getTerrainType; -1 if unavailable
    float   terrainElevation = 0.f;  // world-space Z from Terrain::getTerrainElevation

    // World-space surface point (all kinds; Miss = all zero).
    float worldX = 0.f;
    float worldY = 0.f;
    float worldZ = 0.f;
};

// AABB in world space. Caller provides -- v0 does not query from handle.
struct EditorAabb {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct SelectionBoundsStyle {
    uint32_t colorRGBA   = 0xFFFFFF80u;  // RGBA: R=bits31-24 .. A=bits7-0
    float    lineWidthPx = 2.0f;         // ignored in v0 (gos_DrawLines has no width)
};

struct TerrainTileOverlayDesc {
    int32_t  tileRow;
    int32_t  tileCol;
    uint32_t colorRGBA;  // e.g. 0xFF000040 (red 25% alpha) for erase preview
};

// ---- API ----

// Pick at screen position (Win32 convention: origin top-left, Y grows down).
//
// Env behavior:
//   MC2_EDITOR_MODE=0          -> Kind::Miss immediately; no side effects
//   MC2_OBJECT_ID_BUFFER=0     -> Path A (StaticProp/Mech) skipped;
//                                 terrain (Path B) still works; not an error
//   both flags off, terrain miss -> Kind::Miss
EditorPickResult pickAt(int screenX, int screenY);

// Object counts for editor UI panels. Thin wrapper over RenderWorld::queryVisibility.
// Returns zeroed result if MC2_EDITOR_MODE=0.
RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest req = {});

// Draw 12-edge AABB wireframe outline around a selected object.
// Caller provides world-space bounds. No-op if bounds degenerate or env-OFF.
//
// Overlay draw discipline (v0): overlay state is SET on entry and RESTORED
// to hardcoded defaults on exit. Does NOT preserve arbitrary prior render state.
// Must be called at a known overlay/debug phase of the frame.
void drawSelectionBounds(const EditorAabb& bounds, SelectionBoundsStyle style = {});

// Draw 4-edge wireframe outline of one terrain tile (brush preview).
// No-op if tile out of bounds or env-OFF.
//
// Overlay draw discipline: same as drawSelectionBounds.
void drawTerrainTileOutline(const TerrainTileOverlayDesc& desc);

} // namespace EditorBridge
```

- [ ] **Step 4.2: Create `EditorBridge/CMakeLists.txt`**

```cmake
# EditorBridge/CMakeLists.txt
#
# CARVE-OUT library: bridges game-side (mclib, GameOS) and engine-side
# (RenderCore, RenderWorld) headers. Only EditorRenderBridge.cpp is the
# carve-out TU per scripts/check-include-firewall.allowlist.

add_library(editorbridge STATIC
    EditorRenderBridge.cpp
)

target_link_libraries(editorbridge PUBLIC rendercore renderworld)

# Carve-out .cpp reaches game-side and GameOS headers.
target_include_directories(editorbridge PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/mclib
    ${CMAKE_SOURCE_DIR}/mclib/stuff
    ${CMAKE_SOURCE_DIR}/GameOS
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/GameOS/gameos
)

target_include_directories(editorbridge PRIVATE
    ${THIRDPARTY_INCLUDE_DIRS}
)

# Public include: EditorBridge/ itself (for EditorRenderBridge.h).
target_include_directories(editorbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(editorbridge PUBLIC cxx_std_17)
```

- [ ] **Step 4.3: Create `EditorBridge/EditorRenderBridge.cpp` — skeleton only**

```cpp
// EditorBridge/EditorRenderBridge.cpp
//
// CARVE-OUT: this is the ONLY file in EditorBridge/ permitted to include
// both game-side and engine-side headers.
// See: scripts/check-include-firewall.allowlist
//
// EditorBridge v0.
// Spec: docs/superpowers/specs/mission-editor-render-bridge-v0-spec.md

#include "EditorRenderBridge.h"

#include <cstdlib>   // std::getenv
#include <cstring>   // std::strcmp

// Engine side
#include "../RenderWorld/RenderWorld.h"
#include "../RenderWorld/ScreenPick.h"   // ScreenPickContext, screenPickCompute

// Game side
#include "../mclib/terrain.h"   // Terrain, land
#include "../mclib/camera.h"    // Camera::inverseProject, projectZ, CameraPtr

// GameOS (gos_VERTEX, gos_DrawLines, gos_SetRenderState, gos_GetViewport, Environment)
#include "../GameOS/include/gameos.hpp"

// ---- game-side globals wired by the editor at startup ----
extern Terrain*   land;   // defined in mclib/terrain.cpp
extern CameraPtr  eye;    // defined in mclib/camera.cpp

namespace EditorBridge {

// ---- internal state ----

static bool s_enabled = false;

// ---- helpers (defined before the public functions that call them) ----

// Convert spec RGBA (R=bits31-24, G=23-16, B=15-8, A=7-0) to gos_VERTEX::argb
// (A=bits31-24, R=23-16, G=15-8, B=7-0).
// See memory/mc2_argb_packing.md for packing rationale.
static uint32_t rgbaToArgb(uint32_t rgba) {
    const uint8_t r = (rgba >> 24) & 0xFFu;
    const uint8_t g = (rgba >> 16) & 0xFFu;
    const uint8_t b = (rgba >>  8) & 0xFFu;
    const uint8_t a = (rgba       ) & 0xFFu;
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// Fill a gos_VERTEX for a 2D overlay line endpoint (no Z test, no texture).
static gos_VERTEX makeVertex(float sx, float sy, uint32_t argb) {
    gos_VERTEX v{};
    v.x    = sx;
    v.y    = sy;
    v.z    = 0.0f;
    v.rhw  = 1.0f;
    v.argb = argb;
    v.frgb = 0;
    v.u    = 0.f;
    v.v    = 0.f;
    return v;
}

// Draw one line segment between two screen-space points.
// gos_DrawLines(pts, NumVertices) -- second arg is vertex count, not line count.
// 2 vertices = 1 line. Confirmed from gameos.hpp:2234 signature.
static void drawLine(float x0, float y0, float x1, float y1, uint32_t argb) {
    gos_VERTEX pts[2];
    pts[0] = makeVertex(x0, y0, argb);
    pts[1] = makeVertex(x1, y1, argb);
    gos_DrawLines(pts, 2);   // 2 vertices = 1 line segment
}

// Local runtime wrapper for screen->FBO-pixel coord transform.
// Mirrors the one in code/gameplay_pick.cpp. Both own their gos_GetViewport
// call -- it must NOT live in RenderWorld (GameOS is not a RenderWorld dep).
// Math delegated to RenderWorld::screenPickCompute (pure, no GameOS).
static bool editorScreenToFboPixel(int screenX, int screenY,
                                    RenderWorld::ScreenPickContext* out) {
    if (!out) return false;
    out->mouseX = screenX;
    out->mouseY = screenY;
    gos_GetViewport(&out->vMulX, &out->vMulY, &out->vAddX, &out->vAddY);
    out->drawableWidth  = Environment.drawableWidth;
    out->drawableHeight = Environment.drawableHeight;
    if (out->vMulX <= 0.0f || out->vMulY <= 0.0f) return false;
    RenderWorld::screenPickCompute(out);
    return true;
}

// Project a world-space point to screen space via Camera::projectZ.
// Returns false if the point is behind the camera.
static bool projectToScreen(const Stuff::Vector3D& worldPt,
                            float* outSx, float* outSy) {
    Stuff::Vector3D pt = worldPt;  // projectZ takes non-const ref
    Stuff::Vector4D screen{};
    if (!eye->projectZ(pt, screen)) return false;
    *outSx = screen.x;
    *outSy = screen.y;
    return true;
}

// Push overlay render state (no Z test, alpha blend, HUD layer).
// v0 discipline: does NOT preserve prior state. Call only at a known overlay
// phase of the frame. Restore via popOverlayState().
static void pushOverlayState() {
    gos_SetRenderState(gos_State_ZCompare,  0);
    gos_SetRenderState(gos_State_ZWrite,    0);
    gos_SetRenderState(gos_State_Texture,   0);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
    gos_SetRenderState(gos_State_IsHUD,     1);
}

// Restore hardcoded defaults. See header comment -- does NOT restore arbitrary
// prior state, only returns to well-known defaults.
static void popOverlayState() {
    gos_SetRenderState(gos_State_IsHUD,     0);
    gos_SetRenderState(gos_State_ZCompare,  1);
    gos_SetRenderState(gos_State_ZWrite,    1);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
}

// ---- lifecycle ----

void init() {
    const char* val = std::getenv("MC2_EDITOR_MODE");
    s_enabled = (val && std::strcmp(val, "1") == 0);
}

void shutdown() {
    s_enabled = false;
}

bool isEnabled() { return s_enabled; }

// ---- stub implementations (replaced in Tasks 5-7) ----

EditorPickResult pickAt(int /*screenX*/, int /*screenY*/) {
    return {};  // Task 5
}

RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest /*req*/) {
    return {};  // Task 6
}

void drawSelectionBounds(const EditorAabb& /*bounds*/, SelectionBoundsStyle /*style*/) {
    // Task 6
}

void drawTerrainTileOutline(const TerrainTileOverlayDesc& /*desc*/) {
    // Task 7
}

} // namespace EditorBridge
```

- [ ] **Step 4.4: Wire `EditorBridge` into root `CMakeLists.txt`**

After the line `add_subdirectory("./GameAdapters" ...)`, add:

```cmake
add_subdirectory("./EditorBridge" "./out/EditorBridge")
```

On the `target_link_libraries(mc2 ...)` line, add `editorbridge` after `gameadapters`:

```cmake
target_link_libraries(mc2 mclib fx_trace particles gosfx mlr stuff gui gameos gameos_main windows ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} OpenGL::GL renderworld gameadapters editorbridge)
```

- [ ] **Step 4.5: Document `MC2_EDITOR_MODE` in `docs/tier1_env_vars.md`**

Locate the existing env var table and add a row:

```markdown
| `MC2_EDITOR_MODE` | `0` | Set to `1` to activate the `EditorBridge` API surface. All `EditorBridge::*` functions are no-ops when `0`. Must be combined with `MC2_OBJECT_ID_BUFFER=1` for full GPU pick support (terrain raycast still works without it). Process-lifetime cached at `EditorBridge::init()`, which the mission editor must call explicitly — mc2 game startup does NOT auto-call it. |
```

- [ ] **Step 4.6: Build to verify the skeleton compiles**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo 2>&1 | Select-Object -Last 10
```

Expected: `mc2.exe` with 0 errors. (All four API stubs link cleanly.)

- [ ] **Step 4.7: Run firewall script — verify `EditorRenderBridge.h` is clean**

```powershell
sh A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
```

Expected: `clean (scope: ... EditorBridge)`. (`EditorRenderBridge.h` includes only `RenderCore/Handle.h` and `RenderWorld/VisibilityRequest.h` — both engine-side.)

- [ ] **Step 4.8: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add `
    EditorBridge/EditorRenderBridge.h `
    EditorBridge/EditorRenderBridge.cpp `
    EditorBridge/CMakeLists.txt `
    CMakeLists.txt `
    docs/tier1_env_vars.md
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
feat: add EditorBridge skeleton (v0 API surface, stub implementations)

EditorRenderBridge.h: engine-only public header with all types + API.
EditorRenderBridge.cpp: lifecycle (init/shutdown/isEnabled) + 4 stubs;
  editorScreenToFboPixel local wrapper (gos_GetViewport stays in carve-out).
CMakeLists.txt: editorbridge static lib wired into mc2 target.
MC2_EDITOR_MODE documented in docs/tier1_env_vars.md.
All stubs link cleanly; firewall passes.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Implement `pickAt`

**Files:**
- Modify: `EditorBridge/EditorRenderBridge.cpp` (replace `pickAt` stub)

- [ ] **Step 5.1: Replace the `pickAt` stub**

Replace:
```cpp
EditorPickResult pickAt(int /*screenX*/, int /*screenY*/) {
    return {};  // Task 5
}
```

With:

```cpp
EditorPickResult pickAt(int screenX, int screenY) {
    EditorPickResult result{};
    if (!s_enabled) return result;

    // ---- Path A: GPU object-ID readback (StaticProp + Mech) ----
    // Requires MC2_OBJECT_ID_BUFFER=1. Silently skipped if off -- not an error.
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        RenderWorld::ScreenPickContext spCtx;
        if (editorScreenToFboPixel(screenX, screenY, &spCtx)) {
            if (screenX >= 0 && screenY >= 0
                && screenX < static_cast<int>(spCtx.vMulX)
                && screenY < static_cast<int>(spCtx.vMulY))
            {
                const RenderWorld::LookupResult lr =
                    RenderWorld::lookupAtPixel(spCtx.glX, spCtx.glY);
                if (lr.isValid) {
                    result.handle = lr.handle;
                    // Explicit switch: unknown kinds do NOT silently become
                    // StaticProp. Each kind handled by name; new kinds will
                    // generate a compiler warning when the switch is extended.
                    switch (lr.kind) {
                    case RenderWorld::RenderObjectKind::StaticProp:
                        result.kind = EditorPickResult::Kind::StaticProp;
                        return result;
                    case RenderWorld::RenderObjectKind::Mech:
                        result.kind = EditorPickResult::Kind::Mech;
                        return result;
                    default:
                        // Terrain (reserved, no writer), Vfx (prohibited writers),
                        // or any future kind: fall through to terrain raycast.
                        break;
                    }
                }
            }
        }
    }

    // ---- Path B: CPU terrain raycast ----
    // Always fires when MC2_EDITOR_MODE=1, regardless of object-ID state.
    // This is the canonical ground-click path per M3 arc decision.
    if (!land || !eye) return result;

    Stuff::Vector2DOf<long> screenPt;
    screenPt.x = static_cast<long>(screenX);
    screenPt.y = static_cast<long>(screenY);
    Stuff::Vector3D worldPt{};
    const unsigned long hitFlags = eye->inverseProject(screenPt, worldPt);
    if (hitFlags != 0) return result;  // missed terrain (off-map; 0=success per camera.cpp)

    int tileR = -1, tileC = -1;
    land->worldToTile(worldPt, tileR, tileC);

    result.kind             = EditorPickResult::Kind::Terrain;
    result.terrainTileRow   = tileR;
    result.terrainTileCol   = tileC;
    result.terrainType      = static_cast<int32_t>(land->getTerrainType(worldPt));
    result.terrainElevation = land->getTerrainElevation(worldPt);
    result.worldX           = worldPt.x;
    result.worldY           = worldPt.y;
    result.worldZ           = worldPt.z;
    return result;
}
```

- [ ] **Step 5.2: Build**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo 2>&1 | Select-Object -Last 10
```

Expected: 0 errors.

- [ ] **Step 5.3: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add EditorBridge/EditorRenderBridge.cpp
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
feat: implement EditorBridge::pickAt (Path A GPU + Path B terrain)

Path A: MC2_OBJECT_ID_BUFFER=1 -> editorScreenToFboPixel -> lookupAtPixel.
Path B: Camera::inverseProject -> worldToTile + getTerrainType + getTerrainElevation.
Kind classified via explicit switch (no silent StaticProp fallback for future kinds).
Path A fires first; terrain is the fallback on miss, env-OFF, or unknown kind.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Implement `queryVisibility` and `drawSelectionBounds`

**Preflight — verify `gos_DrawLines` parameter semantics:**

- [ ] **Step 6.1: Grep `gos_DrawLines` signature**

```powershell
Select-String -Path "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/include/gameos.hpp" -Pattern "gos_DrawLines" -Context 0,2
```

Expected output (from greybeard recon): the second parameter is named `NumVertices` (not `NumLines`). Confirm this. If the parameter is named `NumVertices`, then `gos_DrawLines(pts, 2)` draws 1 line from 2 vertices — which is correct. If the signature says `NumLines`, change `gos_DrawLines(pts, 2)` to `gos_DrawLines(pts, 1)` in the `drawLine` helper (Task 4.3) before continuing.

**Files:**
- Modify: `EditorBridge/EditorRenderBridge.cpp` (replace two stubs)

- [ ] **Step 6.2: Replace the `queryVisibility` stub**

Replace:
```cpp
RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest /*req*/) {
    return {};  // Task 6
}
```

With:

```cpp
RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest req) {
    if (!s_enabled) return {};
    return RenderWorld::queryVisibility(req);
}
```

- [ ] **Step 6.3: Replace the `drawSelectionBounds` stub**

Replace:
```cpp
void drawSelectionBounds(const EditorAabb& /*bounds*/, SelectionBoundsStyle /*style*/) {
    // Task 6
}
```

With:

```cpp
void drawSelectionBounds(const EditorAabb& b, SelectionBoundsStyle style) {
    if (!s_enabled || !eye) return;
    // Degenerate AABB guard.
    if (b.minX >= b.maxX || b.minY >= b.maxY || b.minZ >= b.maxZ) return;

    // 8 world-space corners: bottom face (z=minZ) + top face (z=maxZ).
    const Stuff::Vector3D corners[8] = {
        { b.minX, b.minY, b.minZ }, { b.maxX, b.minY, b.minZ },
        { b.maxX, b.maxY, b.minZ }, { b.minX, b.maxY, b.minZ },
        { b.minX, b.minY, b.maxZ }, { b.maxX, b.minY, b.maxZ },
        { b.maxX, b.maxY, b.maxZ }, { b.minX, b.maxY, b.maxZ },
    };

    float sx[8], sy[8];
    for (int i = 0; i < 8; ++i) {
        if (!projectToScreen(corners[i], &sx[i], &sy[i])) return;
    }

    // 12 edges: 4 bottom, 4 top, 4 vertical pillars.
    constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   // bottom face
        {4,5},{5,6},{6,7},{7,4},   // top face
        {0,4},{1,5},{2,6},{3,7},   // pillars
    };

    const uint32_t argb = rgbaToArgb(style.colorRGBA);
    pushOverlayState();
    for (int e = 0; e < 12; ++e) {
        drawLine(sx[edges[e][0]], sy[edges[e][0]],
                 sx[edges[e][1]], sy[edges[e][1]], argb);
    }
    popOverlayState();
}
```

- [ ] **Step 6.4: Build**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo 2>&1 | Select-Object -Last 10
```

Expected: 0 errors.

- [ ] **Step 6.5: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add EditorBridge/EditorRenderBridge.cpp
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
feat: implement EditorBridge::queryVisibility + drawSelectionBounds

queryVisibility: thin wrapper over RenderWorld::queryVisibility.
drawSelectionBounds: 12-edge AABB wireframe via gos_DrawLines + pushOverlayState.
Caller provides EditorAabb (v0: no handle->bounds query).
gos_DrawLines(pts, 2) confirmed correct -- second arg is NumVertices.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Implement `drawTerrainTileOutline`

**Files:**
- Modify: `EditorBridge/EditorRenderBridge.cpp` (replace stub)

- [ ] **Step 7.1: Preflight — find terrain map-size dimension symbol**

```powershell
Select-String -Path "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.h" -Pattern "realVerticesMapSide|numVertices|mapSize|tileCount|numTiles" -Context 0,1
```

Note the exact symbol name and whether it is a `static` member, a `const`, or an instance field. The bounds check in the next step must use this symbol to guard `tileR+1` and `tileC+1` before indexing the coord arrays. (Expected: `realVerticesMapSide` is a public member of the `Terrain` instance, accessible as `land->realVerticesMapSide`.)

- [ ] **Step 7.2: Replace the `drawTerrainTileOutline` stub**

Replace:
```cpp
void drawTerrainTileOutline(const TerrainTileOverlayDesc& /*desc*/) {
    // Task 7
}
```

With:

```cpp
void drawTerrainTileOutline(const TerrainTileOverlayDesc& desc) {
    if (!s_enabled || !land || !eye) return;
    const int tileR = desc.tileRow;
    const int tileC = desc.tileCol;

    // Internal bounds check -- EditorBridge is a public API and must not
    // allow caller-supplied tile indices to run off the static coord arrays.
    //
    // coordLimit is the coord-array boundary count confirmed in Step 7.1.
    // The tile valid range is [0, coordLimit-2] because tile tileR needs
    // tileRowToWorldCoord[tileR] AND [tileR+1] to both be valid.
    //
    // If Step 7.1 reveals realVerticesMapSide is tile count (not boundary count),
    // change the condition to: tileR + 1 >= coordLimit + 1 (i.e., tileR >= coordLimit).
    // Document which interpretation applies in the commit message.
    const int coordLimit = land->realVerticesMapSide; // replace symbol if Step 7.1 differs
    if (tileR < 0 || tileC < 0) return;
    if (tileR + 1 >= coordLimit) return;
    if (tileC + 1 >= coordLimit) return;

    // Tile world-space extent from Terrain's public static coord arrays.
    // tileColToWorldCoord[tileC]   = left X boundary of tile
    // tileColToWorldCoord[tileC+1] = right X boundary
    // tileRowToWorldCoord[tileR]   = near Y boundary (larger = further from camera)
    // tileRowToWorldCoord[tileR+1] = far Y boundary
    const float x0 = Terrain::tileColToWorldCoord[tileC];
    const float x1 = Terrain::tileColToWorldCoord[tileC + 1];
    const float y0 = Terrain::tileRowToWorldCoord[tileR];
    const float y1 = Terrain::tileRowToWorldCoord[tileR + 1];

    // Sample elevation at the four world-space corners (CPU heightmap read).
    auto elevAt = [&](float x, float y) -> float {
        Stuff::Vector3D pos{ x, y, 0.f };
        return land->getTerrainElevation(pos);
    };

    const Stuff::Vector3D corners[4] = {
        { x0, y0, elevAt(x0, y0) },  // NW
        { x1, y0, elevAt(x1, y0) },  // NE
        { x1, y1, elevAt(x1, y1) },  // SE
        { x0, y1, elevAt(x0, y1) },  // SW
    };

    float sx[4], sy[4];
    for (int i = 0; i < 4; ++i) {
        if (!projectToScreen(corners[i], &sx[i], &sy[i])) return;
    }

    const uint32_t argb = rgbaToArgb(desc.colorRGBA);
    pushOverlayState();
    drawLine(sx[0], sy[0], sx[1], sy[1], argb);  // N edge
    drawLine(sx[1], sy[1], sx[2], sy[2], argb);  // E edge
    drawLine(sx[2], sy[2], sx[3], sy[3], argb);  // S edge
    drawLine(sx[3], sy[3], sx[0], sy[0], argb);  // W edge
    popOverlayState();
}
```

**NOTE:** If Step 7.1 reveals that the map-size symbol has a different name (e.g., `numVerticesMapSide`, `mapVertexCount`, etc.) or is a `static` rather than an instance field, adjust the bounds check lines accordingly. The intent is: no array access past the valid index range.

- [ ] **Step 7.3: Build**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64 --config RelWithDebInfo 2>&1 | Select-Object -Last 10
```

Expected: 0 errors.

- [ ] **Step 7.4: Commit**

```powershell
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev add EditorBridge/EditorRenderBridge.cpp
git -C A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev commit -m "$(cat <<'EOF'
feat: implement EditorBridge::drawTerrainTileOutline

4-edge wireframe tile outline via Terrain static coord arrays
(tileColToWorldCoord / tileRowToWorldCoord) + getTerrainElevation.
Internal bounds check guards tileR/tileC+1 against realVerticesMapSide.
Corners projected via Camera::projectZ; drawn via gos_DrawLines.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Final Verification

**Files:** none — build + script + smoke only

- [ ] **Step 8.1: Run the full unit test suite**

```powershell
cmake --build A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64-tests --config RelWithDebInfo --target mc2_tests 2>&1 | Select-Object -Last 5
A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64-tests/RelWithDebInfo/mc2_tests.exe
```

Expected: all doctest cases pass. `test_screen_pick` contributes 5 cases.

- [ ] **Step 8.2: Run the firewall script**

```powershell
sh A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-include-firewall.sh
```

Expected: `clean (scope: RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL EditorBridge)`.

- [ ] **Step 8.3: Run the no-raw-GL script**

```powershell
sh A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/check-no-raw-gl-from-game.sh
```

Expected: clean. `EditorRenderBridge.cpp` is NOT in `code/` or `mclib/` — outside the script's scope.

- [ ] **Step 8.3b: Verify `EditorBridge::init` is not called from the game path**

```powershell
Select-String -Path "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/*.cpp", `
              "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/*.cpp", `
              "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.cpp" `
              -Pattern "EditorBridge::init"
```

Expected: **no hits**. `EditorBridge::init()` must only be called by the mission editor integration — not from any game-startup or frame-loop TU.

- [ ] **Step 8.4: Run the tier1 smoke gate**

```powershell
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0. `MC2_EDITOR_MODE` is unset by default; bridge is a pure no-op in the game path — zero behavioral change to the 5 smoke missions.

---

## Self-Review

**Spec coverage:**

| Spec requirement | Task covering it |
|-----------------|-----------------|
| `EditorBridge::init/shutdown/isEnabled` | Task 4 |
| `MC2_EDITOR_MODE` env gate | Task 4 (steps 4.3, 4.5) |
| Init must be called by editor explicitly (not auto from mc2) | Task 4 (header comment + env var doc) |
| `EditorRenderBridge.h` engine-types-only | Task 4 (step 4.1); verified by firewall in step 4.7 |
| Only `.cpp` is the carve-out | Task 3 (allowlist) + Task 4 (CMakeLists) |
| `screenPickCompute` promoted to pure-math `RenderWorld/ScreenPick.h` | Task 1 |
| No `gos_GetViewport` in `RenderWorld` | Task 1 (header-only; no .cpp); Task 2 (gameplay_pick keeps local wrapper) |
| `gameplay_pick.cpp` delegates math to `screenPickCompute` | Task 2 |
| `pickAt` Path A: GPU object-ID | Task 5 |
| `pickAt` Path B: terrain raycast | Task 5 |
| `pickAt` env behavior (obj-ID off = terrain still works) | Task 5 (code comments + logic) |
| `pickAt` explicit kind switch (no silent StaticProp fallback) | Task 5 |
| `queryVisibility` thin wrapper | Task 6 |
| `drawSelectionBounds` with `EditorAabb` (not handle) | Task 6 |
| Overlay state v0 discipline documented | Task 4 (header) + Task 6 (code comment) |
| `gos_DrawLines` semantics verified before use | Task 6 (step 6.1 preflight) |
| `drawTerrainTileOutline` (outline, not filled) | Task 7 |
| `drawTerrainTileOutline` internal bounds check | Task 7 |
| `terrainType` + `terrainElevation` in pick result | Task 5 |
| Smoke gate clean | Task 8 |
| Firewall clean | Tasks 3 + 8 |

**Ghost / multi-tile / handle-bounds / per-handle visibility:** confirmed absent (v1 deferred per spec §9).

**Type consistency:** `EditorAabb`, `SelectionBoundsStyle`, `TerrainTileOverlayDesc`, `EditorPickResult`, `EditorPickResult::Kind` — used consistently across header and implementation. `rgbaToArgb`, `makeVertex`, `drawLine`, `editorScreenToFboPixel`, `projectToScreen`, `pushOverlayState`, `popOverlayState` — all defined before use in `EditorRenderBridge.cpp`.

**Firewall discipline summary:**
- `RenderWorld/ScreenPick.h` — pure math, `<cstdint>` only. No runtime.
- `code/gameplay_pick.cpp` — owns its `gos_GetViewport` call; delegates math to `screenPickCompute`.
- `EditorBridge/EditorRenderBridge.cpp` — the one carve-out; owns its `gos_GetViewport` call via `editorScreenToFboPixel`; delegates math to `screenPickCompute`.
- No GameOS dependency in `RenderWorld/`. Invariant holds.
