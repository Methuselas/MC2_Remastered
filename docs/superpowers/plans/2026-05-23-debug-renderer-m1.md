# DebugRenderer M1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement an env-gated world-space debug primitive renderer (lines, AABBs, rings) that draws depth-tested into the scene FBO before post-process, with zero pixel delta when disabled.

**Architecture:** New `DebugRenderer` namespace in `GameOS/gameos/debug_renderer.h/.cpp`. Public draw functions enqueue `DbgVert` structs into a static vector; `flushWorldPrims()` uploads them to a GPU line VBO and draws with a simple colored shader. Entire system gated behind `MC2_DEBUG_RENDERER=1`; disabled = no shader compile, no GL objects, no enqueued verts.

**Tech Stack:** OpenGL 4.5, GLSL 4.30, `glsl_program::makeProgram()` (GameOS shader loader), `gos_GetTerrainMVPMat4()` for world-to-clip matrix.

---

## File Map

| File | Status | Responsibility |
|---|---|---|
| `shaders/debug_prim.vert` | Create | World-space vertex → clip-space, pass color |
| `shaders/debug_prim.frag` | Create | Output vertex color |
| `GameOS/gameos/debug_renderer.h` | Create | Public API: `Vec3`, draw functions, flush stubs |
| `GameOS/gameos/debug_renderer.cpp` | Create | Init, enqueue, flush, GL state save/restore |
| `GameOS/gameos/CMakeLists.txt` | Modify | Add `debug_renderer.cpp` to SOURCES |
| `code/gamecam.cpp` | Modify | Call `flushWorldPrims()` after `weather->render()` |

---

## Task 1: Shader files

**Files:**
- Create: `shaders/debug_prim.vert`
- Create: `shaders/debug_prim.frag`

No `#version` directive in either file -- the version prefix is injected at `makeProgram()` call time (project rule).

- [ ] **Step 1: Create vertex shader**

Create `shaders/debug_prim.vert` with exactly this content:

```glsl
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

uniform mat4 uMVP;

out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPosition, 1.0);
    vColor = aColor;
}
```

- [ ] **Step 2: Create fragment shader**

Create `shaders/debug_prim.frag` with exactly this content:

```glsl
in vec4 vColor;
out vec4 fragColor;

void main() {
    fragColor = vColor;
}
```

- [ ] **Step 3: Commit**

```bash
git add shaders/debug_prim.vert shaders/debug_prim.frag
git commit -m "feat(debugrenderer): add debug_prim vert/frag shaders (M1)"
```

---

## Task 2: Header and CMakeLists

**Files:**
- Create: `GameOS/gameos/debug_renderer.h`
- Modify: `GameOS/gameos/CMakeLists.txt` (add one line after line 32 `gos_smoke.cpp`)

- [ ] **Step 1: Create header**

Create `GameOS/gameos/debug_renderer.h`:

```cpp
#pragma once
#include <cstdint>

namespace DebugRenderer {

struct Vec3 { float x, y, z; };

// Enqueue a world-space line segment. No-op when MC2_DEBUG_RENDERER is unset.
void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba);

// Enqueue a world-space AABB as 12 edges (24 verts).
// No-op when MC2_DEBUG_RENDERER is unset.
void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba);

// Enqueue a world-space horizontal ring (XZ plane at center.y).
// segments clamped to [3, 256]. No-op when MC2_DEBUG_RENDERER is unset.
void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba);

// Flush collected world-space primitives to the GPU and draw.
// Must be called while the scene FBO is still bound
// (inside Environment.UpdateRenderers, after weather->render()).
// No-op when MC2_DEBUG_RENDERER is unset.
void flushWorldPrims();

// Reserved: future crisp HUD/debug-text pass (post post-process). No-op in M1.
void flushScreenPrims();

} // namespace DebugRenderer
```

- [ ] **Step 2: Add to CMakeLists**

Open `GameOS/gameos/CMakeLists.txt`. After line 32 (`gos_smoke.cpp`), insert:

```cmake
    debug_renderer.cpp
```

The SOURCES block should look like:

```cmake
    gos_smoke.cpp
    debug_renderer.cpp
    gos_font.cpp
```

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/debug_renderer.h GameOS/gameos/CMakeLists.txt
git commit -m "feat(debugrenderer): add header + CMakeLists entry (M1)"
```

---

## Task 3: Implementation skeleton + env gate

**Files:**
- Create: `GameOS/gameos/debug_renderer.cpp`

This task creates the file with all state, the env helper, the `init_once()` function, and stubs for the public functions. No GL calls yet.

- [ ] **Step 1: Create debug_renderer.cpp skeleton**

Create `GameOS/gameos/debug_renderer.cpp`:

```cpp
#include "debug_renderer.h"

#include <cstdlib>   // getenv
#include <cstdio>    // fprintf, stderr
#include <cstddef>   // offsetof
#include <vector>
#include <cmath>     // sinf, cosf

#include <GL/glew.h>

#include "utils/shader_builder.h"  // glsl_program::makeProgram

// Forward-declare the scene viewproj getter (defined in gameos_graphics.cpp).
// Upload with the same transpose convention as the existing terrain MVP upload sites.
extern const float* gos_GetTerrainMVPMat4();

namespace DebugRenderer {

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct DbgVert { float x, y, z, r, g, b, a; };

static constexpr int kVertCapacity = 65536;

// ---------------------------------------------------------------------------
// Env helper (default OFF -- absent or "0" = disabled)
// MUST be defined before the static bools that call it.
// ---------------------------------------------------------------------------

static bool envFlagDefaultOff(const char* name) {
    const char* v = getenv(name);
    return v && !(v[0] == '0' && v[1] == '\0');
}

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static bool              s_enabled       = envFlagDefaultOff("MC2_DEBUG_RENDERER");
static bool              s_testCanary    = envFlagDefaultOff("MC2_DEBUG_RENDERER_TEST");
static bool              s_initialized   = false;
static bool              s_capWarnedOnce = false;

static GLuint            s_vao = 0;
static GLuint            s_vbo = 0;
static glsl_program*     s_program = nullptr;
static GLuint            s_programId = 0;  // cached for glProgramUniformMatrix4fv
static GLint             s_mvpLoc = -1;

static std::vector<DbgVert> s_verts;

// ---------------------------------------------------------------------------
// Internal: init on first flush
// ---------------------------------------------------------------------------

static void init_once() {
    if (s_initialized) return;
    s_initialized = true;

    fprintf(stderr, "[DEBUGDRAW v1] event=init enabled=1\n");

    // Load shader (no #version in files; prefix injected here per project rule)
    s_program = glsl_program::makeProgram(
        "debug_prim",
        "shaders/debug_prim.vert",
        "shaders/debug_prim.frag",
        "#version 430\n"
    );
    if (!s_program) {
        fprintf(stderr, "[DEBUGDRAW v1] event=shader_fail -- debug draw disabled\n");
        s_enabled = false;
        return;
    }

    // Cache program ID for explicit-program uniform upload.
    // glsl_program::apply() uses GL_TRUE for matrix uniforms (shader_builder.cpp:781)
    // which would transpose our already-row-major MVP -- we must NOT use setMat4+apply.
    // Instead: bind once here to get the GL program ID, then use glProgramUniformMatrix4fv.
    s_program->apply();
    {
        GLint pid = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &pid);
        s_programId = (GLuint)pid;
    }
    glUseProgram(0);  // unbind after init query

    s_mvpLoc = glGetUniformLocation(s_programId, "uMVP");
    if (s_mvpLoc < 0) {
        fprintf(stderr, "[DEBUGDRAW v1] event=uniform_missing name=uMVP -- debug draw disabled\n");
        s_enabled = false;
        return;
    }

    // VAO + VBO
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    // Allocate buffer at max capacity once
    glBufferData(GL_ARRAY_BUFFER,
                 kVertCapacity * (GLsizeiptr)sizeof(DbgVert),
                 nullptr, GL_DYNAMIC_DRAW);

    // layout(location=0) aPosition: vec3 at offset 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(DbgVert),
                          (const void*)offsetof(DbgVert, x));

    // layout(location=1) aColor: vec4 at offset 12
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(DbgVert),
                          (const void*)offsetof(DbgVert, r));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    s_verts.reserve(4096);
}

// ---------------------------------------------------------------------------
// Color unpack helper
// ---------------------------------------------------------------------------

static DbgVert makeVert(float x, float y, float z, uint32_t rgba) {
    DbgVert v;
    v.x = x; v.y = y; v.z = z;
    v.r = ((rgba >> 24) & 0xFF) / 255.0f;
    v.g = ((rgba >> 16) & 0xFF) / 255.0f;
    v.b = ((rgba >>  8) & 0xFF) / 255.0f;
    v.a = ((rgba >>  0) & 0xFF) / 255.0f;
    return v;
}

// ---------------------------------------------------------------------------
// Public API stubs (implementation in next tasks)
// ---------------------------------------------------------------------------

void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba) {
    if (!s_enabled) return;
    s_verts.push_back(makeVert(a.x, a.y, a.z, rgba));
    s_verts.push_back(makeVert(b.x, b.y, b.z, rgba));
}

void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba) {
    if (!s_enabled) return;
    // 8 corners: bit0=X, bit1=Y, bit2=Z
    float cx[8], cy[8], cz[8];
    for (int i = 0; i < 8; ++i) {
        cx[i] = (i & 1) ? mx.x : mn.x;
        cy[i] = (i & 2) ? mx.y : mn.y;
        cz[i] = (i & 4) ? mx.z : mn.z;
    }
    // 12 edges: explicit list, no __builtin_popcount (MSVC portability)
    static constexpr int kEdges[12][2] = {
        {0,1}, {0,2}, {0,4},
        {1,3}, {1,5},
        {2,3}, {2,6},
        {3,7},
        {4,5}, {4,6},
        {5,7},
        {6,7},
    };
    for (const auto& e : kEdges) {
        int a = e[0], b = e[1];
        s_verts.push_back(makeVert(cx[a], cy[a], cz[a], rgba));
        s_verts.push_back(makeVert(cx[b], cy[b], cz[b], rgba));
    }
}

void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba) {
    if (!s_enabled) return;
    if (segments < 3)   segments = 3;
    if (segments > 256) segments = 256;
    const float step = 6.2831853f / (float)segments;  // 2*PI
    for (int i = 0; i < segments; ++i) {
        float a0 = step * i;
        float a1 = step * (i + 1);
        s_verts.push_back(makeVert(
            center.x + radius * cosf(a0),
            center.y,
            center.z + radius * sinf(a0), rgba));
        s_verts.push_back(makeVert(
            center.x + radius * cosf(a1),
            center.y,
            center.z + radius * sinf(a1), rgba));
    }
}

void flushWorldPrims() {
    // full implementation in Task 4
}

void flushScreenPrims() {
    // reserved for future crisp HUD/debug-text pass
}

} // namespace DebugRenderer
```

- [ ] **Step 2: Build to confirm no compile errors**

```
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target gameos 2>&1 | tail -20
# (Git Bash/MSYS2; in PowerShell replace `tail -20` with `Select-Object -Last 20`)
```

Expected: build succeeds (0 errors). Warnings about unused variables in stub `flushWorldPrims` are OK for now.

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/debug_renderer.cpp
git commit -m "feat(debugrenderer): skeleton + env gate + enqueue helpers (M1)"
```

---

## Task 4: flushWorldPrims() -- full implementation

**Files:**
- Modify: `GameOS/gameos/debug_renderer.cpp` (replace the stub `flushWorldPrims`)

- [ ] **Step 1: Replace flushWorldPrims stub**

Replace the stub `flushWorldPrims()` body in `debug_renderer.cpp` with:

```cpp
void flushWorldPrims() {
    if (!s_enabled) return;

    // Test canary: enqueue BEFORE empty check so MC2_DEBUG_RENDERER_TEST=1
    // produces output even when no other callers have queued verts.
    if (s_testCanary) {
        // World axis tripod at origin: X=red, Y=green, Z=blue, 50-unit arms
        drawLineWorld({0,0,0}, {50,0,0},  0xFF0000FFu);  // X red
        drawLineWorld({0,0,0}, {0,50,0},  0x00FF00FFu);  // Y green
        drawLineWorld({0,0,0}, {0,0,50},  0x0000FFFFu);  // Z blue
        // White ring at (0, 50, 0) radius 30, 64 segments
        drawRingWorld({0, 50, 0}, 30.0f, 64, 0xFFFFFFFFu);
    }

    if (s_verts.empty()) return;

    if (!s_initialized) init_once();
    if (!s_enabled) { s_verts.clear(); return; }  // init_once() may disable on shader fail

    const float* vp = gos_GetTerrainMVPMat4();
    if (!vp) {
        s_verts.clear();
        return;
    }

    // Truncate to capacity
    if ((int)s_verts.size() > kVertCapacity) {
        if (!s_capWarnedOnce) {
            fprintf(stderr, "[DEBUGDRAW v1] event=vert_cap_exceeded cap=%d requested=%d\n",
                    kVertCapacity, (int)s_verts.size());
            s_capWarnedOnce = true;
        }
        s_verts.resize(kVertCapacity);
        // Ensure even count for GL_LINES (pairs)
        if (s_verts.size() % 2 != 0) s_verts.pop_back();
    }

    const GLsizeiptr byteSize = (GLsizeiptr)(s_verts.size() * sizeof(DbgVert));

    // --- Save GL state ---
    GLint  prevProg = 0, prevVAO = 0, prevVBO = 0;
    GLint  depthFuncWas = 0;
    GLint  blendSrcRGB = 0, blendDstRGB = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
    GLint  blendEqRGB = 0, blendEqAlpha = 0;
    GLboolean depthTestWas, blendWas, cullWas, depthMaskWas;
    GLfloat lineWidthWas = 1.0f;

    glGetIntegerv(GL_CURRENT_PROGRAM,      &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevVBO);
    depthTestWas = glIsEnabled(GL_DEPTH_TEST);
    blendWas     = glIsEnabled(GL_BLEND);
    cullWas      = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK,      &depthMaskWas);
    glGetIntegerv(GL_DEPTH_FUNC,           &depthFuncWas);
    glGetIntegerv(GL_BLEND_SRC_RGB,        &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB,        &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,      &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA,      &blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,   &blendEqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
    glGetFloatv(GL_LINE_WIDTH,             &lineWidthWas);

    // --- Set known state ---
    // Use glUseProgram directly (not s_program->apply()) because apply() would
    // flush dirty matrix uniforms with GL_TRUE -- wrong for our GL_FALSE MVP.
    glUseProgram(s_programId);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glLineWidth(1.0f);

    // Upload MVP. GL_FALSE: matrix from gos_GetTerrainMVPMat4 is already
    // row-major (converted at gos_SetWorldToClipGL). All terrain upload
    // sites use GL_FALSE with the same matrix (gameos_graphics.cpp:5012,
    // 5091, 5202, 5245, 7376).
    glProgramUniformMatrix4fv(s_programId, s_mvpLoc, 1, GL_FALSE, vp);

    // Orphan + subdata upload
    glBufferData(GL_ARRAY_BUFFER, byteSize, nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, byteSize, s_verts.data());

    glDrawArrays(GL_LINES, 0, (GLsizei)s_verts.size());

    // --- Restore GL state ---
    glDepthMask(depthMaskWas);
    if (depthTestWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWas)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (cullWas)      glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    glDepthFunc(depthFuncWas);
    glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    glBlendEquationSeparate(blendEqRGB, blendEqAlpha);
    glLineWidth(lineWidthWas);
    glUseProgram(prevProg);
    glBindVertexArray(prevVAO);
    glBindBuffer(GL_ARRAY_BUFFER, prevVBO);

    s_verts.clear();
}
```

- [ ] **Step 2: Build**

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target gameos 2>&1 | tail -20
# (Git Bash/MSYS2; in PowerShell replace `tail -20` with `Select-Object -Last 20`)
```

Expected: 0 errors.

- [ ] **Step 3: Commit**

```bash
git add GameOS/gameos/debug_renderer.cpp
git commit -m "feat(debugrenderer): implement flushWorldPrims with GL state save/restore (M1)"
```

---

## Task 5: Frame hook in gamecam.cpp

**Files:**
- Modify: `code/gamecam.cpp`

Wire `flushWorldPrims()` into the render loop after `weather->render()` (line 315).

- [ ] **Step 1: Add include**

In `code/gamecam.cpp`, add after the existing includes (after line 46 `particles/batcher.h`):

```cpp
#include "../GameOS/gameos/debug_renderer.h"  // M1 world debug primitives
```

- [ ] **Step 2: Add flush call**

In `code/gamecam.cpp`, after line 315 (`weather->render()`), insert:

```cpp
        // DebugRenderer world primitives -- depth-tested, before post-process.
        // No-op when MC2_DEBUG_RENDERER is unset.
        {
            ZoneScopedN("GameCamera::render debugRendererFlushWorldPrims");
            DebugRenderer::flushWorldPrims();
        }
```

The surrounding context should look like:

```cpp
        if (useNonWeaponEffects)
        {
            ZoneScopedN("GameCamera::render weather");
            weather->render();              //Draw the weather
        }

        // DebugRenderer world primitives -- depth-tested, before post-process.
        // No-op when MC2_DEBUG_RENDERER is unset.
        {
            ZoneScopedN("GameCamera::render debugRendererFlushWorldPrims");
            DebugRenderer::flushWorldPrims();
        }
    }
```

The Tracy zone ensures the flush shows up in profiling if ever enabled.

- [ ] **Step 3: Build full project**

```
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -30
# (Git Bash/MSYS2; in PowerShell replace `tail -30` with `Select-Object -Last 30`)
```

Expected: 0 errors. This builds the full project (mc2.exe + all libs).

- [ ] **Step 4: Commit**

```bash
git add code/gamecam.cpp
git commit -m "feat(debugrenderer): wire flushWorldPrims after weather in gamecam.cpp (M1)"
```

---

## Task 6: Smoke test -- canary visible

This task verifies the renderer works end-to-end. No automated test exists; validation is visual + log.

- [ ] **Step 1: Deploy**

Follow the mc2-deploy skill (`/mc2-deploy`) to copy the freshly built exe + updated shaders to the deploy directory. Shaders changed (new files added), so the full shader tree must deploy in lockstep with mc2.exe.

Key deploy steps (from skill). Run from worktree root in Git Bash/MSYS2:

```bash
DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.4"
WORKTREE="A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"

cp -f "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"

# New shader files -- must deploy in lockstep with exe (project rule)
cp -f "$WORKTREE/shaders/debug_prim.vert" "$DEPLOY/shaders/debug_prim.vert"
cp -f "$WORKTREE/shaders/debug_prim.frag" "$DEPLOY/shaders/debug_prim.frag"

# Verify
diff -q "$WORKTREE/shaders/debug_prim.vert" "$DEPLOY/shaders/debug_prim.vert"
diff -q "$WORKTREE/shaders/debug_prim.frag" "$DEPLOY/shaders/debug_prim.frag"
diff -q "$WORKTREE/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
```

Expected: all three `diff -q` print nothing (files match).

- [ ] **Step 2: Run with canary env**

Launch with both env vars set:

```
set MC2_DEBUG_RENDERER=1
set MC2_DEBUG_RENDERER_TEST=1
mc2.exe
```

Start any mission. Look at terrain/ground level. Expected:
- World axis tripod visible at or near world origin: red line (X), green line (Y), blue line (Z), each 50 units long
- White ring at Y=50 near origin

If origin is underground or far from camera: the canary lines may still be visible near the horizon. Check stderr for `[DEBUGDRAW v1] event=init enabled=1`.

- [ ] **Step 3: Verify env-off zero delta**

Run WITHOUT the env vars. Verify:
- No `[DEBUGDRAW v1]` lines in stderr
- Visually identical to baseline (no extra primitives)

- [ ] **Step 4: Commit canary test note**

No code change needed. Add a one-line note to the spec:

In `docs/superpowers/specs/2026-05-23-debug-renderer-m1-design.md`, update the Test Canary section to add at the end:

```
Canary validated 2026-05-23: world axes + ring visible at origin with MC2_DEBUG_RENDERER_TEST=1.
```

```bash
git add docs/superpowers/specs/2026-05-23-debug-renderer-m1-design.md
git commit -m "docs(debugrenderer): mark canary smoke validated (M1)"
```

---

## Self-Review Checklist

### Spec coverage

| Spec section | Covered by |
|---|---|
| Env gate default-off | Task 3 (static init with `envFlagDefaultOff`) |
| Draw functions early-return when disabled | Task 3 (all three enqueue fns check `!s_enabled`) |
| Test canary before empty check | Task 4 (`s_testCanary` block before `s_verts.empty()`) |
| Lazy init (no GL objects when disabled) | Task 3 (`init_once()` called from flush only) |
| VBO orphan+subdata | Task 4 (`glBufferData nullptr` + `glBufferSubData`) |
| GL state save/restore | Task 4 (full list including blend func/eq) |
| GL_FALSE transpose rationale | Task 4 (comment cites 5 upload sites) |
| Frame hook after weather | Task 5 |
| Tracy zone | Task 5 |
| `flushScreenPrims` stub | Task 3 (stub, no-op body with comment) |
| `[DEBUGDRAW v1]` logging schema | Task 3 (init), Task 4 (cap exceeded) |
| CMakeLists | Task 2 |
| Shaders (no #version in files) | Task 1 |
| `#version 430\n` prefix at makeProgram | Task 3 |
| VAO attrib layout (location 0+1, stride 28) | Task 3 |
| Shader deploy lockstep | Task 6 |

### Placeholder scan

None found.

### Type consistency

- `DbgVert` defined in Task 3, used in Task 4 (same struct, `{x,y,z,r,g,b,a}`)
- `makeVert()` defined in Task 3, called in Tasks 3+4
- `s_programId` (`GLuint`) cached at init in Task 3, used for `glProgramUniformMatrix4fv` in Task 4
- `s_mvpLoc` cached via `glGetUniformLocation(s_programId, "uMVP")` in Task 3 init
- `glsl_program::apply()` exists (`shader_builder.h:116`) -- used only in init to bind+query program ID, NOT at flush time
- `glsl_program::setMat4` intentionally NOT used -- it calls `GL_TRUE` transpose internally (`shader_builder.cpp:781`)

All method names verified against `shader_builder.h` before plan was written.
