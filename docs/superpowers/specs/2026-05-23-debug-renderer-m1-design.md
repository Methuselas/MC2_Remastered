# DebugRenderer M1 -- World Primitive Overlay

**Date:** 2026-05-23
**Status:** Approved for implementation

---

## Scope

Independent, env-gated debug primitive renderer that draws world-space lines,
AABBs, and rings into the scene FBO with depth testing, before post-process.
No selection semantics, no object-ID dependency, no editor behavior.
Screen-space / HUD / text path is stubbed; not implemented in M1.

---

## Env gates

| Variable | Default | Effect |
|---|---|---|
| `MC2_DEBUG_RENDERER` | off | Master enable. Absent or `"0"` = disabled. |
| `MC2_DEBUG_RENDERER_TEST` | off | Draws world axes or one ring near origin when master gate on. Remove or leave opt-in after smoke. |

When `MC2_DEBUG_RENDERER` is unset:
- No shader compilation
- No GL object allocation (VAO/VBO)
- No draw calls
- Zero pixel delta

---

## Files

| File | Action |
|---|---|
| `GameOS/gameos/debug_renderer.h` | New -- public API |
| `GameOS/gameos/debug_renderer.cpp` | New -- implementation |
| `shaders/debug_prim.vert` | New -- colored vertex shader |
| `shaders/debug_prim.frag` | New -- pass-through frag |
| `GameOS/gameos/gameos_graphics.cpp` | No change -- `gos_GetTerrainMVPMat4()` already present at :7681 |
| `code/gamecam.cpp` | Add `flushWorldPrims()` call after `weather->render()` |
| `CMakeLists.txt` (GameOS) | Add `debug_renderer.cpp` to build |

---

## Public API

```cpp
// GameOS/gameos/debug_renderer.h
namespace DebugRenderer {

struct Vec3 { float x, y, z; };

void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba);
void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba);
void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba);

// Flush world-space primitives collected this frame.
// Must be called while the scene FBO is still bound (inside
// Environment.UpdateRenderers, after weather->render()).
// No-op when MC2_DEBUG_RENDERER is unset.
void flushWorldPrims();

// Reserved: future crisp HUD/debug-text pass (post post-process).
// No-op in M1.
void flushScreenPrims();

} // namespace DebugRenderer
```

`rgba` encoding: `0xRRGGBBAA` (red in high byte, alpha in low byte).

---

## Internal structure

```cpp
// debug_renderer.cpp (internal)
struct DbgVert { float x, y, z, r, g, b, a; };

static bool              s_enabled;      // set once from envFlagDefaultOff("MC2_DEBUG_RENDERER")
static bool              s_initialized;  // VAO/VBO/program allocated
static GLuint            s_vao, s_vbo;
static GLuint            s_program;
static GLint             s_mvpLoc;
static std::vector<DbgVert> s_verts;

static constexpr int kVertCapacity = 65536;
```

Initialization is lazy: happens on first `flushWorldPrims()` when `s_enabled && !s_initialized`.
If `gos_GetCurrentSceneViewProjGL()` returns false (matrix not yet set), flush no-ops silently.

---

## Matrix getter

`gos_GetTerrainMVPMat4()` already exists in `gameos_graphics.cpp:7681` and does exactly
what is needed:

```cpp
const float* gos_GetTerrainMVPMat4();
// Returns row-major float[16] pointer into the cached terrain_mvp_.
// Returns nullptr when g_gos_renderer is null or isTerrainMVPValid() is false.
```

No new getter is added. `flushWorldPrims()` calls `gos_GetTerrainMVPMat4()` directly;
the terrain-specific naming is an implementation detail hidden inside `debug_renderer.cpp`
and invisible to debug-renderer callers.

The `gameos_graphics.cpp` file column in the Files table is updated accordingly: add
`gos_GetTerrainMVPMat4` extern forward declaration to `debug_renderer.cpp` only
(it is already declared via the existing `extern` at `gameos_graphics.cpp:61`).

---

## Vertex expansion helpers

```
drawLineWorld(a, b)
    append 2 verts (a, b) to s_verts

drawAabbWorld(mn, mx)
    expand to 12 edges = 24 verts (all 8 corners, 12 unique edges)

drawRingWorld(center, radius, segments)
    generate `segments` point pairs in XZ plane at center.y
    append 2*segments verts
    clamp segments to [3, 256]
```

AABB edge list (corner indices 0-7, bit 0=X, bit 1=Y, bit 2=Z):
edges that differ by exactly 1 bit = 12 pairs.

---

## Shader

**`shaders/debug_prim.vert`** (no `#version` directive -- injected at makeProgram):
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

**`shaders/debug_prim.frag`**:
```glsl
in vec4 vColor;
out vec4 fragColor;

void main() {
    fragColor = vColor;
}
```

Prefix passed to `makeProgram()`: `"#version 430\n"`.
MVP uniform uploaded via `glProgramUniformMatrix4fv` (explicit-program rule).

---

## VAO layout

| Attrib | Location | Type | Offset | Stride |
|---|---|---|---|---|
| aPosition | 0 | vec3 (float) | 0 | 28 |
| aColor | 1 | vec4 (float) | 12 | 28 |

Stride = `sizeof(DbgVert)` = 28 bytes.

---

## flushWorldPrims() sequence

```
1. if !s_enabled → return
2. if s_verts.empty() → return
3. if !s_initialized → init_once()
4. const float* vp = gos_GetTerrainMVPMat4()
   if !vp → s_verts.clear(); return
5. if (int)s_verts.size() > kVertCapacity → truncate, warn once
6. -- save GL state --
   GLint prevProg, prevVAO, prevVBO
   GLboolean depthTestWas, blendWas, cullWas, depthMaskWas
   GLint depthFuncWas
   GLfloat lineWidthWas
7. -- set known state --
   glUseProgram(s_program)
   glBindVertexArray(s_vao)
   glBindBuffer(GL_ARRAY_BUFFER, s_vbo)
   glEnable(GL_DEPTH_TEST)
   glDepthFunc(GL_LEQUAL)
   glDepthMask(GL_FALSE)
   glEnable(GL_BLEND)
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
   glDisable(GL_CULL_FACE)
   glLineWidth(1.0f)
8. upload MVP
   glProgramUniformMatrix4fv(s_program, s_mvpLoc, 1, GL_FALSE, vp)
   (GL_FALSE: row-major, same convention as terrain upload)
9. orphan + upload
   glBufferData(GL_ARRAY_BUFFER, byteSize, nullptr, GL_DYNAMIC_DRAW)
   glBufferData(GL_ARRAY_BUFFER, byteSize, s_verts.data(), GL_DYNAMIC_DRAW)
10. glDrawArrays(GL_LINES, 0, (GLsizei)s_verts.size())
11. -- restore GL state --
    glDepthMask(depthMaskWas)
    restore depthTest, blend, cull, depthFunc, lineWidth
    glUseProgram(prevProg)
    glBindVertexArray(prevVAO)
    glBindBuffer(GL_ARRAY_BUFFER, prevVBO)
12. s_verts.clear()
```

---

## GL state save/restore

State saved before and restored after flush:

| State | Save/restore method |
|---|---|
| Current program | `glGetIntegerv(GL_CURRENT_PROGRAM, ...)` |
| VAO binding | `glGetIntegerv(GL_VERTEX_ARRAY_BINDING, ...)` |
| Array buffer binding | `glGetIntegerv(GL_ARRAY_BUFFER_BINDING, ...)` |
| `GL_BLEND` enabled | `glIsEnabled(GL_BLEND)` |
| `GL_DEPTH_TEST` enabled | `glIsEnabled(GL_DEPTH_TEST)` |
| `GL_DEPTH_FUNC` | `glGetIntegerv(GL_DEPTH_FUNC, ...)` |
| `GL_DEPTH_WRITEMASK` | `glGetBooleanv(GL_DEPTH_WRITEMASK, ...)` |
| `GL_CULL_FACE` enabled | `glIsEnabled(GL_CULL_FACE)` |
| Line width | `glGetFloatv(GL_LINE_WIDTH, ...)` |

---

## Frame hook location

In `code/gamecam.cpp`, inside the `if (!drawOldWay)` block, after `weather->render()`:

```cpp
// DebugRenderer world primitives -- depth-tested, before post-process.
// No-op when MC2_DEBUG_RENDERER is unset.
DebugRenderer::flushWorldPrims();
```

`flushScreenPrims()` is NOT called in M1. Reserved for a future post-pp pass
alongside `projectz_overlay_render()` in `gameosmain.cpp`.

---

## Test canary

When `MC2_DEBUG_RENDERER_TEST=1` (checked inside `flushWorldPrims`):
draw world-axis tripod at origin (X=red, Y=green, Z=blue, 50-unit arms)
and one white ring at `(0, 50, 0)` radius 30, 64 segments.

This gives smoke a visible artifact without coupling to any gameplay system.
Remove or leave opt-in after initial validation.

---

## Logging

```
[DEBUGDRAW v1] event=init enabled=1
[DEBUGDRAW v1] event=vert_cap_exceeded cap=65536 requested=N (printed once)
```

Schema: `[DEBUGDRAW v\d+]` -- grep-stable per instrumentation convention.

---

## Render contract

Uses `PassIdentity::DebugOverlay` semantics (diagnostics-only, no shadow, no MRT).
`flushWorldPrims` does not call `render_contract::assertPassContract()` in M1 --
that integration is an M2 follow-up once pass-contract enforcement is production-ready.

---

## Out of scope for M1

- Text / label rendering
- Screen-space lines
- Selection-semantic tie-ins
- Object-ID dependency
- Editor / picker integration
- Multiple colors per primitive segment
- Depth-off (X-ray) mode
- Persistent (multi-frame) primitives

---

## CMakeLists change

In `GameOS/CMakeLists.txt` (or wherever `gos_mech_batcher.cpp` is listed),
add `gameos/debug_renderer.cpp` to the same target.
