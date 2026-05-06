# PatchStream Shape B M0b — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land an env-gated, default-off `TerrainPatchStream` that replaces the per-batch `glBufferData(GL_DYNAMIC_DRAW)` color-VBO upload churn inside `terrainDrawIndexedPatches` with persistent-mapped VBO rings, while keeping legacy ring writes (`addVertices`, `fillTerrainExtra`) and shaders untouched. Headline target: eliminate ~7.6 MB/frame color-VBO upload + collapse N per-batch extras-VBO uploads into one per-frame consolidated upload.

**Architecture:** New file-local class `TerrainPatchStream` in `GameOS/gameos/`. Two triple-buffered persistent-mapped VBOs (`gos_VERTEX` 32 B/vert + `gos_TERRAIN_EXTRA` 24 B/vert), bound as `GL_ARRAY_BUFFER` with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` per the `gos_static_prop_batcher.cpp:163-164` precedent. Fence-rotation, optimistic-no-wait. Same shaders, same attribute layout, same draw count (one per material bucket via `glDrawArrays(GL_PATCHES,…)`). Killswitch `MC2_MODERN_TERRAIN_SURFACE` (default 0). Whole-frame fallback to legacy on overflow or init failure.

**Tech Stack:** C++17, OpenGL 4.3 core (Windows MSYS2 + MSVC RelWithDebInfo build), GLSL `#version 430`, Tracy for CPU/GPU profiling, existing smoke runner (`scripts/run_smoke.py`), existing instrumentation (`[SUBSYS v1]` envelope per `memory/debug_instrumentation_rule.md`).

**Spec:** `docs/superpowers/specs/2026-04-27-patchstream-shape-b-design.md` (rev 5).
All numbered §-references below point at that file.

> **⚠️ Plan-overrides-spec note (rev 2 of this plan, 2026-04-27).**
> The spec contains intentional decision-justifying mentions of SSBO
> in its rev banners, the "Why VBO not SSBO" subsection, §5.1, and
> §7.1's Q-B1 answer — those contrast the chosen mechanism against
> the rejected alternative and are correct as written. **All other
> operational language is persistent-mapped VBO bound as
> `GL_ARRAY_BUFFER`.** If you find any other operational SSBO/
> `glBindBufferRange`/`GL_SHADER_STORAGE_BUFFER` reference in the
> spec, treat it as stale and trust this plan: M0b is **persistent
> VBO only, no shader changes, no SSBO bind, no `glBindBufferRange`**.

> **⚠️ Bucket-grouping note (rev 2 of this plan, 2026-04-27).**
> Quad traversal in `mclib/quad.cpp` does NOT visit triangles in
> texture-grouped order — `mcTextureManager` does that grouping
> *after* traversal. A naive "current-run + flip on textureIndex
> change" bucket policy would create dozens of tiny run-buckets when
> mixed-biome terrain alternates textureIndex per quad. M0b uses a
> two-phase staging approach instead: appends go into per-texture
> CPU staging vectors keyed by `textureIndex`; `flush()` concatenates
> them into the persistent ring before drawing. Result: one bucket
> per distinct `textureIndex` (~5-40 buckets/frame, matching today's
> draw count), independent of traversal order. See Task 3.

**Out of scope** (track as follow-on lanes — do NOT do these here):
- Shape B-array (`sampler2DArray` colormap collapse — gated on Canary B)
- `glGetAttribLocation` per-draw stall fix
- `matNormal0-4` redundant per-node rebind hoist
- Unit-9 collision (matNormal4 ↔ shadowMap)
- Migrating Shadow.StaticAccum or grass off the legacy rings (Shape B' lane)
- Live `RAlt+Shift+T` toggle

---

## File Structure

| Path | New / Modify | Responsibility |
|---|---|---|
| `GameOS/gameos/gos_terrain_patch_stream.h` | New | Public API + declarations for `TerrainPatchStream`, lifecycle event names |
| `GameOS/gameos/gos_terrain_patch_stream.cpp` | New | Allocation, slot rotation, fence sync, append, flush, issueDraws, state save/restore, lifecycle prints |
| `GameOS/gameos/gameos_graphics.cpp` | Modify | Wire `TerrainPatchStream::init/destroy` into `gosRenderer::init/destroy`. **No** changes to `terrainDrawIndexedPatches` itself — that path stays as the killswitch=0 / overflow-fallback target |
| `mclib/quad.cpp` | Modify | Mirror-write into `TerrainPatchStream` from the four SOLID-branch callsites at lines 1608, 1752, 1911, 2053, gated by killswitch, inside the existing `pz` gate brace. Legacy `addVertices` + `fillTerrainExtra` calls stay |
| `mclib/txmmgr.cpp` | Modify | `Render.TerrainSolid` arm at lines 1296–1358: when killswitch=1 and no overflow, dispatch `TerrainPatchStream::flush()` instead of the per-node legacy loop. On overflow, fall through to legacy for the whole frame |
| `docs/amd-driver-rules.md` | Modify (already done in pre-plan cleanup) | Soften sampler2DArray rule post-Canary A — no further edit in this plan |
| `tests/smoke/baselines.json` | Modify | After successful killswitch=0 parity run, baseline-update if any drift surfaced; otherwise untouched |
| `memory/patchstream_m0b.md` | New (after merge) | Durable memory entry pointing at the spec + plan + first commit hash |

The above are the only files M0b touches. The spec's "no shader changes" and "shadow path untouched" constraints mean `shaders/`, `mclib/txmmgr.cpp:1210–1252` (Shadow.StaticAccum), and `gosRenderer::drawGrassPass` are read-only references for this plan.

---

## Definitions used across tasks

These types and names are referenced consistently from Task 1 onward. Any task that violates them is a planning bug.

```c++
// In GameOS/gameos/gos_terrain_patch_stream.h

constexpr uint32_t kPatchStreamRingFrames    = 3;
constexpr uint32_t kPatchStreamMaxBuckets    = 64;     // far above observed 5–40 draws

// Per-Wolfman-frame worst case: 237,606 verts × 32 B = 7.6 MB color, 5.7 MB extras.
// Plus 25 % headroom (§5.4 of the spec).
constexpr uint32_t kPatchStreamColorBytesPerSlot  = 10u * 1024 * 1024;  // 10 MB ≈ 9.5 MB + slack
constexpr uint32_t kPatchStreamExtrasBytesPerSlot = 8u  * 1024 * 1024;  //  8 MB ≈ 7.1 MB + slack

struct PatchStreamBucket {
    DWORD textureIndex;     // resolved at draw time via tex_resolve()
    uint32_t firstVertex;   // offset within the slot, in vertices (color & extras share the same range)
    uint32_t vertexCount;   // multiple of 3 (one triangle per 3 verts; GL_PATCHES with 3 patch verts)
};

class TerrainPatchStream {
public:
    static bool init();         // returns false on glBufferStorage / glMapBufferRange failure
    static void destroy();
    static bool isReady();      // killswitch + init-success gate
    static bool isOverflowed(); // set during this frame's appends; cleared by beginFrame()

    // Called from quad.cpp SOLID branches, mirroring legacy addVertices/fillTerrainExtra:
    //   - vColor: 3 contiguous gos_VERTEX (one triangle) — same bytes the legacy ring writes
    //   - vExtra: 3 contiguous gos_TERRAIN_EXTRA (matching worldPos/worldNorm)
    //   - textureIndex: same value passed to legacy addVertices(terrainHandle, …)
    static void appendTriangle(DWORD textureIndex,
                               const gos_VERTEX* vColor,        // [3]
                               const gos_TERRAIN_EXTRA* vExtra); // [3]

    // Called once per frame from Render.TerrainSolid arm (mclib/txmmgr.cpp).
    // Issues per-bucket glDrawArrays(GL_PATCHES, …). Also issues ONE
    // updateBuffer(terrain_extra_vb_, consolidated extras) for grass.
    // Returns false if overflow occurred this frame OR not ready —
    // caller falls back to legacy in either case.
    static bool flush();

    // Called from gosRenderer::beginFrame() AFTER the gVAO bind.
    // Rotates to the next slot (waits on the slot's fence if not yet signaled),
    // resets bucket table, clears overflow flag.
    static void beginFrame();
};

// Lifecycle event names (always-on prints unless noted).
// Format: "[PATCH_STREAM v1] event=<name> key=val key=val ..."
//   init           — at end of TerrainPatchStream::init() on success
//   init_fail      — when glBufferStorage / glMapBufferRange returns failure;
//                    forces killswitch=0 for remainder of process
//   first_flush    — emitted on the first successful flush() per process
//   draw_count     — emitted at end of flush() if MC2_PATCH_STREAM_TRACE=1
//   bucket_full    — when a triangle would land in bucket #kPatchStreamMaxBuckets+ (overflow path)
//   overflow       — when per-slot byte budget would be exceeded
//   shutdown       — at start of destroy()
```

---

## Task 0: Add `gos_terrain_bridge.h` — C-style accessors for the patch stream

**Files:**
- Create: `GameOS/gameos/gos_terrain_bridge.h`
- Modify: `GameOS/gameos/gameos_graphics.cpp`

**Goal:** The `gosRenderer` class is defined inside `gameos_graphics.cpp` and
is not visible from other translation units. `TerrainPatchStream` cannot
call `g_gos_renderer->method(…)` from its own `.cpp` because that requires
a complete type. Solve this with a tiny C-style bridge header that
declares free functions which `TerrainPatchStream` calls and which
`gameos_graphics.cpp` defines (where the full `gosRenderer` type is
visible). This pattern is small surface area, no class hierarchy
gymnastics, and matches how `gos_static_prop_batcher` already integrates.

- [ ] **Step 1: Create the bridge header**

```c++
// GameOS/gameos/gos_terrain_bridge.h
//
// Tiny C-style accessor bridge so TerrainPatchStream (defined in its own
// .cpp) can reach gosRenderer state without #including gameos_graphics.cpp
// internals. Each function is implemented in gameos_graphics.cpp where the
// full gosRenderer type is visible.

#pragma once

// Forward decls for opaque pointer types used in signatures.
class gosRenderMaterial;

// Returns the active terrain material (the one terrainDrawIndexedPatches
// uses). Live pointer — do NOT cache across frames.
gosRenderMaterial* gos_terrain_bridge_getMaterial();

// Returns the legacy terrain_extra_vb_ GL buffer ID. Used by
// TerrainPatchStream::flush() to issue the single consolidated per-frame
// updateBuffer for grass + any legacy extras reader (§7.5 Option A).
unsigned int gos_terrain_bridge_getExtraVB();

// Returns the GL program ID of the terrain material's currently-applied
// shader. Used by flush() to look up worldPos / worldNorm attribute
// locations once and cache them, avoiding the per-draw glGetAttribLocation
// stall noted in spec §12.5 #1. Returns 0 if no terrain material is
// resident yet (e.g. before first mission load).
unsigned int gos_terrain_bridge_getShaderProgram();

// Sets every direct uniform + texture bind that terrainDrawIndexedPatches
// sets EXCEPT the per-batch VBO upload. Call once per flush() before
// issuing per-bucket glDrawArrays. The function internally calls
// material->apply() (which calls glUseProgram), so direct glUniform*
// calls inside it are AFTER apply() per AMD rule line 10.
void gos_terrain_bridge_bindUniforms(gosRenderMaterial* material);
```

- [ ] **Step 2: Implement the bridge stubs in `gameos_graphics.cpp`**

In `GameOS/gameos/gameos_graphics.cpp`, near the top with the other
includes:

```c++
#include "gos_terrain_bridge.h"
```

Then, just below the `gosRenderer` class definition (search for the
closing `};` of that class, then add a new section right after) and
*above* `gosRenderer::init()`, add:

```c++
// ─── gos_terrain_bridge implementation ────────────────────────────────────
// Defined here because the full gosRenderer type is visible in this TU.
// Declarations live in gos_terrain_bridge.h.

extern gosRenderer* g_gos_renderer;  // existing global

gosRenderMaterial* gos_terrain_bridge_getMaterial() {
    return g_gos_renderer ? g_gos_renderer->getTerrainMaterial() : nullptr;
}

unsigned int gos_terrain_bridge_getExtraVB() {
    return g_gos_renderer ? g_gos_renderer->getTerrainExtraVB() : 0;
}

unsigned int gos_terrain_bridge_getShaderProgram() {
    if (!g_gos_renderer) return 0;
    gosRenderMaterial* mat = g_gos_renderer->getTerrainMaterial();
    if (!mat || !mat->getShader()) return 0;
    return (unsigned int)mat->getShader()->shp_;
}

void gos_terrain_bridge_bindUniforms(gosRenderMaterial* material) {
    if (g_gos_renderer && material)
        g_gos_renderer->terrainBindUniformsForPatchStream(material);
}
// ──────────────────────────────────────────────────────────────────────────
```

- [ ] **Step 3: Add the three methods to the `gosRenderer` class**

Find the `gosRenderer` class definition in `gameos_graphics.cpp`. In
the public section, add:

```c++
    gosRenderMaterial* getTerrainMaterial() const { return terrain_material_; }
    unsigned int       getTerrainExtraVB()  const { return terrain_extra_vb_; }
    void               terrainBindUniformsForPatchStream(gosRenderMaterial* material);
```

(`unsigned int` rather than `GLuint` so the bridge header doesn't need
to pull `<gl/glew.h>`.)

The body of `terrainBindUniformsForPatchStream` will be added in
**Task 6** — for Task 0 it can be an empty stub that just calls
`material->apply()`:

```c++
void gosRenderer::terrainBindUniformsForPatchStream(gosRenderMaterial* material) {
    if (material) material->apply();   // Task 6 fills in the rest.
}
```

This stub is intentionally insufficient for correct rendering. Task 6
fleshes it out. We add it now so Task 6's revisions stay localized.

- [ ] **Step 4: Build**

Add the new header to the build (most CMake setups auto-pick up `.h`
files in the source dir; if not, add to `GameOS/gameos/CMakeLists.txt`).

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```
Expected: clean build. The bridge functions exist and link, but
nothing calls them yet.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_terrain_bridge.h GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(patchstream): add gos_terrain_bridge C-style accessor header

Tiny free-function bridge so TerrainPatchStream (separate TU) can call
into gosRenderer without seeing the class definition. Three accessors:
material, extra VB id, uniform-bind helper. terrainBindUniformsForPatchStream
is currently a stub (just material->apply); Task 6 fills it in. Same
integration pattern as gos_static_prop_batcher."
```

---

## Task 1: Add the `TerrainPatchStream` skeleton header + cpp + lifecycle prints

**Files:**
- Create: `GameOS/gameos/gos_terrain_patch_stream.h`
- Create: `GameOS/gameos/gos_terrain_patch_stream.cpp`
- Modify: `GameOS/gameos/gameos_graphics.cpp` — `gosRenderer::init` (~line 1884) and `gosRenderer::destroy` (~line 1888)

**Goal of this task:** the class exists, links, prints `init` and `shutdown` on game launch and exit. No GL allocation yet.

- [ ] **Step 1: Create the header**

```c++
// GameOS/gameos/gos_terrain_patch_stream.h
#pragma once

#include <cstdint>

// Forward-declare engine vertex types so this header doesn't pull large includes.
struct gos_VERTEX;
struct gos_TERRAIN_EXTRA;

constexpr uint32_t kPatchStreamRingFrames        = 3;
constexpr uint32_t kPatchStreamMaxBuckets        = 64;
constexpr uint32_t kPatchStreamColorBytesPerSlot  = 10u * 1024 * 1024;
constexpr uint32_t kPatchStreamExtrasBytesPerSlot = 8u  * 1024 * 1024;

struct PatchStreamBucket {
    unsigned long textureIndex;  // matches DWORD in the engine; avoids dragging windows.h here
    uint32_t firstVertex;
    uint32_t vertexCount;
};

class TerrainPatchStream {
public:
    static bool init();
    static void destroy();
    static bool isReady();
    static bool isOverflowed();

    static void appendTriangle(unsigned long textureIndex,
                               const gos_VERTEX* vColor,
                               const gos_TERRAIN_EXTRA* vExtra);

    static bool flush();
    static void beginFrame();
};
```

- [ ] **Step 2: Create the cpp with lifecycle prints only (no GL yet)**

```c++
// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"

#include <cstdio>
#include <cstdlib>

namespace {
    bool s_killswitch = false;   // resolved at init() from MC2_MODERN_TERRAIN_SURFACE
    bool s_initOk     = false;   // true once init() succeeded
    bool s_traceOn    = false;   // MC2_PATCH_STREAM_TRACE=1
}

bool TerrainPatchStream::init()
{
    s_killswitch = (getenv("MC2_MODERN_TERRAIN_SURFACE") != nullptr) &&
                   (getenv("MC2_MODERN_TERRAIN_SURFACE")[0] == '1');
    s_traceOn    = (getenv("MC2_PATCH_STREAM_TRACE") != nullptr);

    if (!s_killswitch) {
        // Default-off path. Do nothing (no GL, no allocations).
        // Lifecycle: do not print 'init' when disabled — matches stock behavior.
        return true;
    }

    // GL allocation arrives in Task 2. For now mark init OK so subsequent
    // tasks can wire isReady() correctly even when the ring isn't real yet.
    s_initOk = true;
    fprintf(stderr,
        "[PATCH_STREAM v1] event=init slots=%u colorBytes=%u extrasBytes=%u trace=%d\n",
        kPatchStreamRingFrames,
        kPatchStreamColorBytesPerSlot,
        kPatchStreamExtrasBytesPerSlot,
        (int)s_traceOn);
    fflush(stderr);
    return true;
}

void TerrainPatchStream::destroy()
{
    if (!s_killswitch || !s_initOk) return;
    fprintf(stderr, "[PATCH_STREAM v1] event=shutdown\n");
    fflush(stderr);
    s_initOk = false;
}

bool TerrainPatchStream::isReady()       { return s_killswitch && s_initOk; }
bool TerrainPatchStream::isOverflowed()  { return false; }
void TerrainPatchStream::beginFrame()    { /* Task 4 */ }
void TerrainPatchStream::appendTriangle(unsigned long, const gos_VERTEX*, const gos_TERRAIN_EXTRA*) { /* Task 3 */ }
bool TerrainPatchStream::flush()         { return false; /* Task 6 */ }
```

- [ ] **Step 3: Register init / destroy with the engine**

In `GameOS/gameos/gameos_graphics.cpp`, near the top with the other includes:

```c++
#include "gos_terrain_patch_stream.h"
```

At the bottom of `gosRenderer::init()` (immediately before the closing `}` — line ~1884 today):

```c++
    { ZoneScopedN("gosRenderer::init overlayVAOs"); makeOverlayVAO(terrainOverlayBatch_); makeOverlayVAO(decalBatch_); }

    if (!TerrainPatchStream::init()) {
        // init_fail path lives in Task 2; for now this is dead code.
        fprintf(stderr, "[PATCH_STREAM v1] event=init_fail reason=task1_skeleton_returned_false\n");
        fflush(stderr);
    }
}
```

At the start of `gosRenderer::destroy()` (the function begins at ~line 1886):

```c++
void gosRenderer::destroy() {
    TerrainPatchStream::destroy();
    gosMesh::destroy(quads_);
    // ...
```

- [ ] **Step 4: Add the new file to the build**

Open `GameOS/gameos/CMakeLists.txt` (the file that compiles `gameos.lib`). Find the source list (a `set()` or `add_library(gameos ...)` block) and add:

```cmake
gos_terrain_patch_stream.h
gos_terrain_patch_stream.cpp
```

Run:
```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -8
```
Expected: `mc2.exe` linked, no compile errors, the new files mentioned in the gameos.vcxproj line.

- [ ] **Step 5: Smoke that lifecycle prints fire when killswitch=1**

Deploy:
```bash
DEPLOY=A:/Games/mc2-opengl/mc2-win64-v0.2; SRC=A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
cp -f "$SRC/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
diff -q "$SRC/build64/RelWithDebInfo/mc2.exe" "$DEPLOY/mc2.exe"
```

Run via PowerShell with killswitch on:
```powershell
$env:MC2_MODERN_TERRAIN_SURFACE="1"
$deploy = "A:/Games/mc2-opengl/mc2-win64-v0.2"
$log = "$deploy/ps_smoke.log"; Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$deploy/mc2.exe" -RedirectStandardError $log -PassThru -WorkingDirectory $deploy
Start-Sleep -Seconds 10
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Content $log | Select-String "PATCH_STREAM"
```

Expected output:
```
[PATCH_STREAM v1] event=init slots=3 colorBytes=10485760 extrasBytes=8388608 trace=0
[PATCH_STREAM v1] event=shutdown
```

**Note on `event=shutdown`:** force-killing via `Stop-Process -Force`
skips the engine's destructor chain entirely, so `event=shutdown` will
NOT appear with this Step-5 launch. That's expected. The clean-exit
verification of `event=shutdown` is in **Task 11 Step 3** below — it
exits via the in-game menu so the destructor runs.

- [ ] **Step 6: Smoke that no prints fire when killswitch=0**

Same launch with `$env:MC2_MODERN_TERRAIN_SURFACE = $null`:
```powershell
Remove-Item Env:MC2_MODERN_TERRAIN_SURFACE -ErrorAction SilentlyContinue
$log = "A:/Games/mc2-opengl/mc2-win64-v0.2/ps_off.log"; Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe" -RedirectStandardError $log -PassThru -WorkingDirectory "A:/Games/mc2-opengl/mc2-win64-v0.2"
Start-Sleep -Seconds 10
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Content $log | Select-String "PATCH_STREAM"
```
Expected: zero matches. Default-off is silent.

- [ ] **Step 7: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.h \
        GameOS/gameos/gos_terrain_patch_stream.cpp \
        GameOS/gameos/gameos_graphics.cpp \
        GameOS/gameos/CMakeLists.txt
git commit -m "feat(patchstream): skeleton class with init/shutdown lifecycle prints

Empty TerrainPatchStream wired into gosRenderer::init/destroy. Gated
behind MC2_MODERN_TERRAIN_SURFACE=1 (default off — silent). No GL
allocation yet. Spec: docs/superpowers/specs/2026-04-27-patchstream-shape-b-design.md"
```

---

## Task 2: Allocate persistent-mapped color + extras VBO rings (init-fail path included)

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

**Goal:** `init()` allocates the two rings via `glBufferStorage` with persistent+coherent flags, maps them once. On any GL failure, log `event=init_fail`, force `s_killswitch=false`, return `true` so the engine continues on the legacy path. State save/restore mirror `gos_static_prop_batcher.cpp:692–712, 817–834`.

- [ ] **Step 1: Add GL bookkeeping to the cpp**

Replace the entire content of `gos_terrain_patch_stream.cpp` with:

```c++
// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gameos.hpp"   // gos_VERTEX, gos_TERRAIN_EXTRA
#include "gl/glew.h"    // OpenGL — same include the static-prop batcher uses

namespace {
    bool s_killswitch = false;
    bool s_initOk     = false;
    bool s_traceOn    = false;

    // GL handles. Two separate buffers — one for color, one for extras.
    GLuint s_colorBuf  = 0;
    GLuint s_extrasBuf = 0;

    // Persistent-mapped CPU pointers. Indexed by [slot * bytesPerSlot + offset].
    void* s_colorMap   = nullptr;
    void* s_extrasMap  = nullptr;

    // Fences per slot. Created at end of flush(); consumed at beginFrame() before
    // re-using the slot. NULL means "no fence yet" (first N frames).
    GLsync s_fence[kPatchStreamRingFrames] = { 0, 0, 0 };

    uint32_t s_slot = 0;  // index of the slot currently being written
}

// Drop GL state we touched, mirroring gos_static_prop_batcher's save/restore.
struct SavedGLState {
    GLint  arrayBuf      = 0;
    GLint  vao           = 0;
    GLboolean blend      = GL_FALSE;
    GLboolean depthTest  = GL_FALSE;
};

static void saveGLState(SavedGLState& s) {
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,  &s.arrayBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,  &s.vao);
    s.blend     = glIsEnabled(GL_BLEND);
    s.depthTest = glIsEnabled(GL_DEPTH_TEST);
}

static void restoreGLState(const SavedGLState& s) {
    glBindBuffer(GL_ARRAY_BUFFER, s.arrayBuf);
    glBindVertexArray(s.vao);
    if (s.blend)     glEnable (GL_BLEND);     else glDisable(GL_BLEND);
    if (s.depthTest) glEnable (GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

static GLuint allocPersistentBuffer(GLsizeiptr totalBytes, void** outMappedPtr) {
    const GLbitfield flags = GL_MAP_WRITE_BIT
                           | GL_MAP_PERSISTENT_BIT
                           | GL_MAP_COHERENT_BIT;

    GLuint id = 0;
    glGenBuffers(1, &id);
    if (!id) return 0;

    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferStorage(GL_ARRAY_BUFFER, totalBytes, nullptr, flags);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &id);
        return 0;
    }

    void* p = glMapBufferRange(GL_ARRAY_BUFFER, 0, totalBytes, flags);
    if (!p) {
        glDeleteBuffers(1, &id);
        return 0;
    }
    *outMappedPtr = p;
    return id;
}

bool TerrainPatchStream::init()
{
    const char* env = getenv("MC2_MODERN_TERRAIN_SURFACE");
    s_killswitch = (env != nullptr) && (env[0] == '1');
    s_traceOn    = (getenv("MC2_PATCH_STREAM_TRACE") != nullptr);

    if (!s_killswitch) return true;

    SavedGLState saved;
    saveGLState(saved);

    const GLsizeiptr colorTotal  = (GLsizeiptr)kPatchStreamColorBytesPerSlot  * kPatchStreamRingFrames;
    const GLsizeiptr extrasTotal = (GLsizeiptr)kPatchStreamExtrasBytesPerSlot * kPatchStreamRingFrames;

    s_colorBuf  = allocPersistentBuffer(colorTotal,  &s_colorMap);
    s_extrasBuf = allocPersistentBuffer(extrasTotal, &s_extrasMap);

    restoreGLState(saved);

    if (!s_colorBuf || !s_extrasBuf) {
        // Init-fail path. Force killswitch off for the rest of the process.
        fprintf(stderr,
            "[PATCH_STREAM v1] event=init_fail reason=glBufferStorage_or_map "
            "colorBuf=%u extrasBuf=%u\n", s_colorBuf, s_extrasBuf);
        fflush(stderr);
        if (s_colorBuf)  { glDeleteBuffers(1, &s_colorBuf);  s_colorBuf  = 0; }
        if (s_extrasBuf) { glDeleteBuffers(1, &s_extrasBuf); s_extrasBuf = 0; }
        s_colorMap = s_extrasMap = nullptr;
        s_killswitch = false;
        return true;  // engine continues on legacy path
    }

    s_initOk = true;
    fprintf(stderr,
        "[PATCH_STREAM v1] event=init slots=%u colorBytes=%u extrasBytes=%u "
        "colorBuf=%u extrasBuf=%u trace=%d\n",
        kPatchStreamRingFrames,
        kPatchStreamColorBytesPerSlot,
        kPatchStreamExtrasBytesPerSlot,
        s_colorBuf, s_extrasBuf, (int)s_traceOn);
    fflush(stderr);
    return true;
}

void TerrainPatchStream::destroy()
{
    if (!s_initOk) return;
    fprintf(stderr, "[PATCH_STREAM v1] event=shutdown\n");
    fflush(stderr);

    for (uint32_t i = 0; i < kPatchStreamRingFrames; ++i) {
        if (s_fence[i]) { glDeleteSync(s_fence[i]); s_fence[i] = 0; }
    }
    if (s_colorBuf) {
        glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &s_colorBuf);
        s_colorBuf = 0; s_colorMap = nullptr;
    }
    if (s_extrasBuf) {
        glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &s_extrasBuf);
        s_extrasBuf = 0; s_extrasMap = nullptr;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_initOk = false;
}

bool TerrainPatchStream::isReady()       { return s_killswitch && s_initOk; }
bool TerrainPatchStream::isOverflowed()  { return false; }   // Task 3
void TerrainPatchStream::beginFrame()    { /* Task 4 */ }
void TerrainPatchStream::appendTriangle(unsigned long, const gos_VERTEX*, const gos_TERRAIN_EXTRA*) { /* Task 3 */ }
bool TerrainPatchStream::flush()         { return false; /* Task 6 */ }
```

- [ ] **Step 2: Build**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```
Expected: clean compile + link.

- [ ] **Step 3: Smoke that real allocation succeeds when killswitch=1**

Deploy and run as in Task 1 Step 5. Expected log lines:
```
[PATCH_STREAM v1] event=init slots=3 colorBytes=10485760 extrasBytes=8388608 colorBuf=<nonzero> extrasBuf=<nonzero> trace=0
... game runs ...
[PATCH_STREAM v1] event=shutdown   (on clean exit)
```

`colorBuf` / `extrasBuf` must both be non-zero. If either is `0`, init-fail fired and the legacy path took over — investigate before proceeding.

- [ ] **Step 4: Smoke that init-fail path is benign — no driver stress**

Don't request a 16 GB allocation just to force the failure path; that
risks driver instability. Add a dedicated env-gated bailout instead.

In `init()`, just before the `allocPersistentBuffer` calls, add:

```c++
if (getenv("MC2_PATCH_STREAM_FORCE_INIT_FAIL")) {
    fprintf(stderr,
        "[PATCH_STREAM v1] event=init_fail reason=force_env\n");
    fflush(stderr);
    s_killswitch = false;
    return true;  // engine continues on legacy
}
```

This is intentional debug instrumentation — leave it in the tree
(env-gated, default off) per `memory/debug_instrumentation_rule.md`.
It pairs with `MC2_PATCH_STREAM_SHRINK_BYTES` (Task 12) for overflow
testing.

Build, then run:

```powershell
$env:MC2_MODERN_TERRAIN_SURFACE="1"
$env:MC2_PATCH_STREAM_FORCE_INIT_FAIL="1"
$deploy = "A:/Games/mc2-opengl/mc2-win64-v0.2"
$log = "$deploy/initfail.log"; Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$deploy/mc2.exe" -RedirectStandardError $log -PassThru -WorkingDirectory $deploy
Start-Sleep -Seconds 10
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Content $log | Select-String "PATCH_STREAM"
Remove-Item Env:MC2_PATCH_STREAM_FORCE_INIT_FAIL
```

Expected:
```
[PATCH_STREAM v1] event=init_fail reason=force_env
```
Game must launch to menu without crash. The legacy path takes over for
the rest of the process.

**Bonus check:** also run a real glBufferStorage failure path by
manually editing `kPatchStreamColorBytesPerSlot` to a moderately
large but driver-rejectable value (e.g., 4 GB on a 6 GB card while
holding mission textures resident). Only do this if you're confident
the dev box won't lock up; the env-gated path above is the
primary test.

- [ ] **Step 5: Pool-headroom regression check**

Run a tier1 mission with killswitch=1 (env var set in the smoke runner's parent shell):
```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing --keep-logs
grep "TGL_POOL v1.*event=mission_unload" tests/smoke/artifacts/*/mc2_01.log | tail -1
```
Expected: peaks within the documented headroom from the spec §2 table (vertex / color / shadow ≤ 25,000; triangle ≤ 30,000; face ≤ 60,000). If anything is > 50 % higher than today's baseline, stop and investigate — there may be a CPU mirror leak.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): allocate persistent-mapped color + extras VBO rings

GL_MAP_WRITE | PERSISTENT | COHERENT, 3 slots each, sized for Wolfman
worst case + 25%% headroom. Init-fail path forces killswitch off for
the rest of the process — engine continues on legacy. State save/
restore mirrors gos_static_prop_batcher.cpp:692-712. Spec §5.3 / §5.4."
```

---

## Task 3: Append API — real per-texture bucketing via CPU staging, flush-time consolidation

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

**Goal:** `appendTriangle()` accumulates 3 `gos_VERTEX` + 3 `gos_TERRAIN_EXTRA` into a per-texture CPU staging vector keyed by `textureIndex`. The persistent ring is *not* written at append time — that happens in `flush()`, which walks the staging buckets in deterministic order, copies each one into a contiguous slot region, and records `firstVertex` / `vertexCount` for the per-bucket draw.

**Why two-phase staging:** quad traversal in `mclib/quad.cpp` does NOT visit triangles in texture-grouped order. `mcTextureManager` does that grouping *after* traversal. A "current run, flip on textureIndex change" policy creates run-buckets, not real material buckets — at Wolfman with mixed biomes you'd see 50–500 tiny buckets instead of the 5–40 that match today's draw shape. CPU-side staging is cheap (a few MB/frame of memcpy into `std::vector`s; no GL involvement) and produces correct one-bucket-per-`textureIndex` grouping at flush time.

**Why this doesn't break the perf model:** the win is eliminating per-batch `glBufferData` orphan + driver upload, not eliminating CPU memcpy. Section §9.2 of the spec already counts ~7.6 MB/frame of CPU staging in the duplication window — staging into `std::vector` first instead of straight into the mapped pointer is the same byte volume, just with one extra hop.

- [ ] **Step 1: Add per-frame staging state**

In the anonymous namespace of `gos_terrain_patch_stream.cpp`, after the existing `s_slot` declaration, add:

```c++
namespace {
    // Per-texture CPU staging. Fixed-size array of buckets — capacity is
    // retained across frames (clear() empties contents but keeps each
    // bucket's std::vector backing storage). beginFrame() resets
    // s_stagingCount and clear()s the live buckets only; the bucket
    // vectors never get destroyed during normal operation.
    //
    // Linear scan lookup over s_stagingCount entries (count is small —
    // ~5-40 distinct textures per frame), cheaper than unordered_map +
    // zero per-frame heap churn after warmup.
    struct PatchStagingBucket {
        unsigned long                  textureIndex = 0;
        std::vector<gos_VERTEX>        color;
        std::vector<gos_TERRAIN_EXTRA> extras;
    };

    PatchStagingBucket s_staging[kPatchStreamMaxBuckets];
    uint32_t           s_stagingCount = 0;
    uint32_t           s_totalVerts   = 0;
    bool               s_overflow     = false;

    // Filled at flush() time from the staging buckets — this is what
    // issueDraws walks for per-bucket glDrawArrays.
    PatchStreamBucket s_drawBuckets[kPatchStreamMaxBuckets];
    uint32_t          s_drawBucketCount = 0;

    // Telemetry
    bool s_firstFlushSeen = false;
}
```

Also add `#include <vector>` near the top of the cpp.

- [ ] **Step 2: Pre-reserve per-bucket vector capacity once at init**

At the bottom of `TerrainPatchStream::init()` (just before the success
`fprintf(stderr, "[PATCH_STREAM v1] event=init ...")`), add:

```c++
    // One-shot reserve so each bucket's std::vector never reallocates
    // during steady-state frames. Total CPU staging RAM at full capacity:
    //   kPatchStreamMaxBuckets * 32 K verts * (sizeof(gos_VERTEX) + sizeof(gos_TERRAIN_EXTRA))
    //   = 64 * 32768 * (32 + 24) bytes ≈ 117 MB worst case if every
    //   bucket maxes out. Typical Wolfman: ~10 active buckets × ~24 K
    //   verts × 56 B ≈ 13 MB resident.
    for (auto& b : s_staging) {
        b.color.reserve(32 * 1024);
        b.extras.reserve(32 * 1024);
    }
```

- [ ] **Step 3: Implement `isOverflowed()` + bucket lookup helper**

Replace the `isOverflowed()` body:

```c++
bool TerrainPatchStream::isOverflowed() { return s_overflow; }
```

Add a file-local helper above `appendTriangle`:

```c++
namespace {
    // Linear-scan lookup over the active prefix of s_staging.
    // Returns nullptr on overflow.
    PatchStagingBucket* findOrCreateStagingBucket(unsigned long textureIndex) {
        for (uint32_t i = 0; i < s_stagingCount; ++i) {
            if (s_staging[i].textureIndex == textureIndex) return &s_staging[i];
        }
        if (s_stagingCount >= kPatchStreamMaxBuckets) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=overflow slot=%u kind=bucket_count count=%u cap=%u\n",
                s_slot, s_stagingCount, kPatchStreamMaxBuckets);
            fflush(stderr);
            s_overflow = true;
            return nullptr;
        }
        PatchStagingBucket& nb = s_staging[s_stagingCount++];
        nb.textureIndex = textureIndex;
        // nb.color / nb.extras are cleared (size 0) but their reserved
        // capacity from init() is intact. No allocation here.
        return &nb;
    }
}
```

- [ ] **Step 4: Implement `appendTriangle()`**

Replace the placeholder body with:

```c++
void TerrainPatchStream::appendTriangle(unsigned long textureIndex,
                                        const gos_VERTEX* vColor,
                                        const gos_TERRAIN_EXTRA* vExtra)
{
    if (!s_initOk || !s_killswitch) return;
    if (s_overflow) return;  // sticky for the whole frame

    constexpr uint32_t vertsPerTri = 3;

    // Per-slot capacity in *vertices* — same as how flush() will copy out.
    const uint32_t maxVertsThisSlot =
        kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX);

    if (s_totalVerts + vertsPerTri > maxVertsThisSlot) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=overflow slot=%u kind=byte_budget cursor=%u cap=%u\n",
            s_slot, s_totalVerts, maxVertsThisSlot);
        fflush(stderr);
        s_overflow = true;
        return;
    }

    PatchStagingBucket* bk = findOrCreateStagingBucket(textureIndex);
    if (!bk) return;  // overflow already logged

    bk->color.insert(bk->color.end(),  vColor, vColor + vertsPerTri);
    bk->extras.insert(bk->extras.end(), vExtra, vExtra + vertsPerTri);
    s_totalVerts += vertsPerTri;
}
```

This does NOT write to the persistent map — that happens in `flush()`.
Append-time work is bounded by a linear scan over the staging vector
(small N) plus two `std::vector::insert` calls (amortized O(1) given
the upfront `reserve`).

- [ ] **Step 5: Note the beginFrame() integration (deferred to Task 4)**

The staging-clear logic that consumes the state added above lives in
`beginFrame()`, which is implemented in **Task 4**. Task 4's Step 1
includes the staging-clear lines — do not add them here. The append
path will not function correctly between Task 3 and Task 4 landing
(staging would grow without bound across frames), so do not run a
multi-frame smoke between Task 3 and Task 4. The compile-only check
in Step 6 below is sufficient.

- [ ] **Step 6: Build + check that the change compiles cleanly**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```

The append API has no callers yet (Task 5 wires those), so the smoke
output should be unchanged from Task 2. Confirm.

**Critical invariant — pointer-delta legacy compatibility (BR-pointer-delta from spec rev 3):** This append does NOT touch `MC_VertexArrayNode::vertices` / `currentVertex` / `extras` / `currentExtra`. The legacy `addVertices` and `addTerrainExtra` calls in `quad.cpp` continue to advance those pointers — that is what feeds Shadow.StaticAccum and the grass extras bind. **Do not move pointer-advance logic into `appendTriangle`. Do not call `addVertices` from `appendTriangle`.**

- [ ] **Step 7: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): per-texture CPU staging buckets + overflow detection

Two-phase: appendTriangle stages per-texture in std::vectors keyed by
textureIndex (linear-scan lookup, pre-reserved). flush() (Task 6)
consolidates staging into the persistent ring slot in deterministic
order. Avoids the run-bucket pathology of grouping by traversal order
when mcTextureManager actually groups after-the-fact. Sticky per-frame
overflow flag, whole-frame fallback in caller. Pointer-delta invariant
(txmmgr.h:72-100) preserved. Spec §5 / §6.5."
```

---

## Task 4: Per-frame slot rotation + fence sync

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`
- Modify: `GameOS/gameos/gameos_graphics.cpp` — `gosRenderer::beginFrame` (~line 2328)

**Goal:** `beginFrame()` rotates `s_slot` to `(s_slot + 1) % kPatchStreamRingFrames`, waits on that slot's fence (only if it hasn't signaled), resets per-frame counters and overflow flag. Called from `gosRenderer::beginFrame()` after the global VAO bind.

- [ ] **Step 1: Implement `beginFrame()` in the cpp**

Replace the placeholder body:

```c++
void TerrainPatchStream::beginFrame()
{
    if (!s_initOk || !s_killswitch) return;

    s_slot = (s_slot + 1) % kPatchStreamRingFrames;

    // Wait on the slot's fence (only the second time we visit a slot,
    // when it has been signaled by an earlier flush). With 3 slots the
    // GPU has typically finished with slot N by the time the CPU comes
    // back around, so this is normally a near-instant signal check —
    // but `GL_TIMEOUT_IGNORED` does mean an indefinite block if the GPU
    // is genuinely behind. We accept the blocking wait for safety in M0b
    // (better to stall the CPU than to write into a slot the GPU is
    // still reading), and log when the wait actually takes nontrivial
    // time so we can spot stalls in profiling.
    if (s_fence[s_slot]) {
        const uint64_t t0 = timing::get_wall_time_ms();
        glClientWaitSync(s_fence[s_slot], GL_SYNC_FLUSH_COMMANDS_BIT,
                         GL_TIMEOUT_IGNORED);
        const uint64_t waitedMs = timing::get_wall_time_ms() - t0;
        glDeleteSync(s_fence[s_slot]);
        s_fence[s_slot] = nullptr;
        if (waitedMs >= 1) {
            fprintf(stderr,
                "[PATCH_STREAM v1] event=fence_stall slot=%u waited_ms=%llu\n",
                s_slot, (unsigned long long)waitedMs);
            fflush(stderr);
        }
    }

    // Reset per-frame state. Buckets are clear()ed (contents emptied)
    // but their reserved capacity from init() is retained — no
    // allocator churn after warmup. s_stagingCount goes to 0 so the
    // linear-scan lookup in findOrCreateStagingBucket only walks
    // currently-active buckets.
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        s_staging[i].color.clear();
        s_staging[i].extras.clear();
    }
    s_stagingCount    = 0;
    s_totalVerts      = 0;
    s_drawBucketCount = 0;
    s_overflow        = false;
}
```

- [ ] **Step 2: Wire into `gosRenderer::beginFrame`**

In `GameOS/gameos/gameos_graphics.cpp`, find the `gosRenderer::beginFrame` body (currently around line 2328). After `glBindVertexArray(gVAO);` add:

```c++
    glBindVertexArray(gVAO);
    num_draw_calls_ = 0;

    TerrainPatchStream::beginFrame();
}
```

- [ ] **Step 3: Build**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```

- [ ] **Step 4: Smoke that nothing regressed**

Run a 10-second tier1 mission at killswitch=0 and at killswitch=1. Both must PASS. We don't expect any visible behavior change yet — `appendTriangle` still has no callers, so `flush()` is moot, and the only on-by-default work is the slot rotation + fence wait, which is a no-op until fences exist.

```bash
MC2_MODERN_TERRAIN_SURFACE=0 py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing
MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing
```

Both runs must show `result=PASS`.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(patchstream): per-frame slot rotation + fence wait

beginFrame() rotates slot, waits non-blockingly on the slot's stale
fence (deleted after wait), resets per-frame cursors / overflow flag.
Optimistic-no-wait pattern from gos_static_prop_batcher.cpp:148-154,
813. Wired into gosRenderer::beginFrame after the global VAO bind."
```

---

## Task 5: Wire append callsites in `mclib/quad.cpp` (4 SOLID branches)

**Files:**
- Modify: `mclib/quad.cpp` — lines 1608, 1752, 1911, 2053 (the four diagonal-mode branches)

**Goal:** Mirror-write to `TerrainPatchStream` from each of the four SOLID-branch sites where the legacy path calls `addVertices` + `fillTerrainExtra`. The append must happen **inside the existing `pz` gate brace** so the modern path can never bypass cull (BR4). The legacy calls **stay** — the duplication caveat from spec §9 is intentional.

- [ ] **Step 1: Read the existing four sites for shape**

The four sites are nearly identical: each is inside a per-quad-diagonal block wrapped by a check on all three vertices' `pz ∈ [0, 1)`. Each currently reads:

```c++
if(terrainHandle!=0) {
    mcTextureManager->addVertices(terrainHandle, gVertex, MC2_ISTERRAIN | MC2_DRAWSOLID);
    fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
}
```

`fillTerrainExtra` builds a `gos_TERRAIN_EXTRA[3]` from the three `MC2Vertex*` and calls `mcTextureManager->addTerrainExtra(...)`. We need that same array to feed the modern stream.

- [ ] **Step 2: Add a small helper above the SOLID branches**

In `mclib/quad.cpp`, add an include near the top:

```c++
#include "GameOS/gameos/gos_terrain_patch_stream.h"
```

Just below the existing `static void fillTerrainExtra(...)` definition (around line 93), add:

```c++
// Builds the same 3-element gos_TERRAIN_EXTRA[3] that fillTerrainExtra
// would push to addTerrainExtra, but returns it by value so the modern
// patch stream can mirror-write the bytes without re-reading the verts.
// Keeping the math identical to fillTerrainExtra is the BR-byte-parity
// guarantee for shadow / grass behavior.
static inline void buildTerrainExtraTriple(MC2Vertex* v0, MC2Vertex* v1, MC2Vertex* v2,
                                           gos_TERRAIN_EXTRA out[3])
{
    MC2Vertex* vs[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; ++k) {
        out[k].wx = vs[k]->vx;
        out[k].wy = vs[k]->vy;
        out[k].wz = vs[k]->elevation;
        out[k].nx = vs[k]->pVertex->vertexNormal.x;
        out[k].ny = vs[k]->pVertex->vertexNormal.y;
        out[k].nz = vs[k]->pVertex->vertexNormal.z;
    }
}
```

If `fillTerrainExtra` already builds the same array internally, refactor it to call `buildTerrainExtraTriple` first and then `addTerrainExtra` — but only if that refactor is trivial. If not, accept the duplicated math; the modern path's correctness depends on byte parity with `fillTerrainExtra`, not on code reuse.

- [ ] **Step 3: Add the modern-mirror call at site #1 (line ~1608)**

Replace:
```c++
if(terrainHandle!=0) {
    mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
    fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
}
```
with:
```c++
if(terrainHandle!=0) {
    mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
    fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);

    // Modern mirror — gated by killswitch via TerrainPatchStream::isReady().
    // INSIDE the pz gate brace by construction. BR4.
    if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
        gos_TERRAIN_EXTRA tx3[3];
        buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx3);
        TerrainPatchStream::appendTriangle(terrainHandle, gVertex, tx3);
    }
}
```

- [ ] **Step 4: Repeat the same insertion at sites #2, #3, #4**

Find sites at approximately lines 1752, 1911, 2053. Each is the same `if(terrainHandle!=0)` block — apply the same edit. The `vertices[0..2]` and `gVertex[0..2]` local names are used at all four sites by convention.

After all four edits, verify by `grep`:
```bash
grep -n "TerrainPatchStream::appendTriangle" mclib/quad.cpp
```
Expected: exactly 4 hits.

- [ ] **Step 5: Build**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```

- [ ] **Step 6: Verify the append is firing — add MC2_PATCH_STREAM_TRACE one-shot**

Temporarily add to `appendTriangle` (in `gos_terrain_patch_stream.cpp`):

```c++
{
    // Temporary one-shot for first-call verification.
    static bool s_firstAppend = false;
    if (!s_firstAppend && s_traceOn) {
        s_firstAppend = true;
        fprintf(stderr, "[PATCH_STREAM v1] event=first_append textureIndex=%lu slot=%u\n",
            textureIndex, s_slot);
        fflush(stderr);
    }
}
```
Place this at the top of `appendTriangle`, after the `s_initOk / s_killswitch / s_overflow` early-out guards.

Build, deploy, and run:
```bash
MC2_MODERN_TERRAIN_SURFACE=1 MC2_PATCH_STREAM_TRACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing --keep-logs
grep "PATCH_STREAM v1" tests/smoke/artifacts/*/mc2_01.log | tail -10
```

Expected: at least one `event=first_append textureIndex=<nonzero>` line during the first frame inside the mission. If not present, the gating logic broke — investigate before proceeding.

**Remove** the one-shot block after verification.

- [ ] **Step 7: Smoke that legacy parity holds**

Run mc2_01 with killswitch=0 and confirm `result=PASS` with FPS within 5 % of pre-Task-5 baseline. Append code at killswitch=0 must compile to `if (false) {…}` short-circuit and have zero observable effect.

- [ ] **Step 8: Commit**

```bash
git add mclib/quad.cpp GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): mirror-append from quad.cpp SOLID branches

Four append sites at lines ~1608/1752/1911/2053 — each one INSIDE the
existing pz gate brace (BR4) — mirror-write the same gos_VERTEX[3] +
gos_TERRAIN_EXTRA[3] that the legacy addVertices/fillTerrainExtra
calls produce. Legacy calls remain (shadow + grass consumers — spec §3
/ §7.3 / §7.5). Killswitch-gated via isReady()."
```

---

## Task 6: `flush()` — consolidated extras upload + per-bucket draws

**Files:**
- Modify: `GameOS/gameos/gos_terrain_patch_stream.cpp`

**Goal:** End-of-`Render.TerrainSolid` dispatch. Issues:
1. **One** `glBufferData(terrain_extra_vb_, …)` of the consolidated extras for the active slot, so the grass pass and any caller that reads `terrain_extra_vb_` sees coherent data (§7.5 Option A).
2. Per-bucket: `gos_SetRenderState(gos_State_Texture, tex_resolve(textureIndex))`, `material->apply()`, direct `glUniform*` set (matching `terrainDrawIndexedPatches:2700-2780`), `glDrawArrays(GL_PATCHES, firstVertex, vertexCount)`.
3. Fence at end: `s_fence[s_slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)`.
4. Returns `false` if `s_overflow` (caller falls back to legacy whole-frame), `true` on success.

This task is the bulk of the engine integration. It needs read access to the engine's existing terrain material + uniform-cache machinery, which lives inside `gosRenderer::terrainDrawIndexedPatches` (`gameos_graphics.cpp:2677-2844`). The cleanest M0b approach is to expose those side-effects via a small "TerrainDrawContext" helper in `gameos_graphics.cpp` and have `flush()` call into it. But adding a new public engine API is more invasive than necessary; instead we reuse `terrainDrawIndexedPatches` for its uniform-set side and bypass its `glBufferData` upload — see Step 2.

- [ ] **Step 1: Flesh out `gosRenderer::terrainBindUniformsForPatchStream`**

The empty stub from Task 0 needs the real uniform-set body. In
`GameOS/gameos/gameos_graphics.cpp`, replace the stub:

```c++
void gosRenderer::terrainBindUniformsForPatchStream(gosRenderMaterial* material) {
    if (material) material->apply();   // Task 6 fills in the rest.
}
```

with the full implementation (a near-verbatim copy of
`terrainDrawIndexedPatches:2688-2782`, with the per-batch VBO upload
and `applyVertexDeclaration` removed — `flush()` does its own bind):

```c++
void gosRenderer::terrainBindUniformsForPatchStream(gosRenderMaterial* material)
{
    material->apply();
    material->setSamplerUnit(gosMesh::s_tex1, 0);

    GLuint shp = material->getShader()->shp_;
    cacheTerrainUniformLocations(shp);
    const auto& tl = terrainLocs_;

    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4]   = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4]   = { terrain_phong_alpha_, terrain_displace_scale_, 0.0f, 0.0f };
    if (tl.tessLevel >= 0)         glUniform4fv(tl.tessLevel, 1, tessParams);
    if (tl.tessDistanceRange >= 0) glUniform4fv(tl.tessDistanceRange, 1, tessDist);
    if (tl.tessDisplace >= 0)      glUniform4fv(tl.tessDisplace, 1, tessDisp);
    if (tl.cameraPos >= 0)         glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    float tessDebugVec[4] = { terrain_debug_mode_, 0.0f, 0.0f, 0.0f };
    if (tl.tessDebug >= 0)         glUniform4fv(tl.tessDebug, 1, tessDebugVec);

    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);
    }
    if (tl.terrainViewport >= 0)
        glUniform4fv(tl.terrainViewport, 1, (const float*)&terrain_viewport_);
    if (terrain_mvp_valid_ && tl.terrainMVP >= 0)
        glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);

    if (tl.terrainLightDir >= 0)        glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    float tiling[4]   = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    float strength[4] = { terrain_detail_strength_, 0.0f, 0.0f, 0.0f };
    float pomP[4]     = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    float cellP[4]    = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.detailNormalTiling >= 0)   glUniform4fv(tl.detailNormalTiling, 1, tiling);
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    if (tl.pomParams >= 0)            glUniform4fv(tl.pomParams, 1, pomP);
    if (tl.terrainWorldScale >= 0)    glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    if (tl.cellBombParams >= 0)       glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.time >= 0) {
        float elapsed = (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }

    for (int i = 0; i < 5; i++) {
        if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
            glUniform1i(tl.matNormal[i], 5 + i);
            glActiveTexture(GL_TEXTURE5 + i);
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    gosPostProcess* pp = getGosPostProcess();
    if (pp && pp->shadowsEnabled_) {
        if (tl.lightSpaceMatrix >= 0) glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
        if (tl.enableShadows >= 0)    glUniform1i(tl.enableShadows, 1);
        if (tl.shadowSoftness >= 0)   glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
        if (tl.shadowMap >= 0) {
            glUniform1i(tl.shadowMap, 9);
            glActiveTexture(GL_TEXTURE9);
            glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        if (pp->getDynamicShadowFBO()) {
            if (tl.dynamicLightSpaceMatrix >= 0)
                glUniformMatrix4fv(tl.dynamicLightSpaceMatrix, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
            if (tl.dynamicShadowMap >= 0) {
                glUniform1i(tl.dynamicShadowMap, 10);
                glActiveTexture(GL_TEXTURE10);
                glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
                glActiveTexture(GL_TEXTURE0);
            }
        } else {
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    } else {
        if (tl.enableShadows >= 0)         glUniform1i(tl.enableShadows, 0);
        if (tl.enableDynamicShadows >= 0)  glUniform1i(tl.enableDynamicShadows, 0);
    }
}
```

This is a near-verbatim copy of the uniform-set block at `terrainDrawIndexedPatches:2688-2782`, with the `glBindBuffer / mesh->uploadBuffers / glBindBuffer(EBO) / applyVertexDeclaration / extras-VBO logic` REMOVED. `flush()` does its own bind.

The accessor methods `getTerrainMaterial()` / `getTerrainExtraVB()`
were already added to the `gosRenderer` class in **Task 0 Step 3** — no
need to add them again. The `gos_terrain_bridge_*` free functions
(also added in Task 0) are what `flush()` calls; they delegate to
those accessors internally.

- [ ] **Step 2: Implement `flush()` via the bridge functions**

Replace the placeholder body in `gos_terrain_patch_stream.cpp`. Note:
this calls `gos_terrain_bridge_*` free functions (Task 0), not
`g_gos_renderer->method(…)` — the gosRenderer type is not visible in
this TU.

```c++
#include "gos_terrain_bridge.h"   // gos_terrain_bridge_* free functions

// gosRenderMaterial is a public engine type — include whatever header
// `gos_static_prop_batcher.cpp` uses to call material->apply() /
// applyVertexDeclaration() / end(). At time of writing this is the
// gameos materials header (search the static-prop batcher's include
// list for the right path; common candidates are
// "gameos_graphics.h" or "materials/gosRenderMaterial.h").
//
// gosRenderer (the renderer class) is NOT a public type — it's defined
// inside gameos_graphics.cpp. That's the one we cannot reach directly,
// hence the C-bridge in Task 0. gosRenderMaterial is publicly exposed
// and methods on it can be called from this TU.
//
// Other engine APIs the cpp depends on:
//   tex_resolve(textureIndex)        — TexResolveTable accessor;
//                                      include the header that provides
//                                      it (search the static-prop batcher
//                                      or terrainDrawIndexedPatches for
//                                      the include line).
//   gos_SetRenderState(...)          — declared in gameos.hpp (already
//                                      pulled in for gos_VERTEX above).
//   timing::get_wall_time_ms()       — declared in the engine timing
//                                      header used by gosRenderer; check
//                                      gameos_graphics.cpp's include
//                                      list and mirror it.
// Pull all three in alongside the gameos.hpp include from Task 2 Step 1.

bool TerrainPatchStream::flush()
{
    if (!s_initOk || !s_killswitch) return false;
    if (s_overflow) {
        // Caller falls through to legacy. No fence emitted — no draws
        // were issued, so the slot is unchanged.
        return false;
    }
    if (s_stagingCount == 0 || s_totalVerts == 0) {
        // Nothing to draw — treat as success so caller skips legacy too.
        return true;
    }

    SavedGLState saved;
    saveGLState(saved);

    // 1. Consolidate staging into the persistent ring at the active slot.
    //    Walk staging buckets in deterministic order (insertion order =
    //    first-append-per-texture order), copy each bucket's color +
    //    extras into contiguous regions, record firstVertex / vertexCount
    //    for issueDraws.
    const uint32_t slotFirstVert =
        s_slot * (kPatchStreamColorBytesPerSlot / (uint32_t)sizeof(gos_VERTEX));
    gos_VERTEX*        colorSlot  = (gos_VERTEX*)s_colorMap  + slotFirstVert;
    gos_TERRAIN_EXTRA* extrasSlot = (gos_TERRAIN_EXTRA*)s_extrasMap + slotFirstVert;

    uint32_t cursor = 0;
    s_drawBucketCount = 0;
    for (uint32_t i = 0; i < s_stagingCount; ++i) {
        const PatchStagingBucket& sb = s_staging[i];
        if (sb.color.empty()) continue;
        const uint32_t n = (uint32_t)sb.color.size();   // == sb.extras.size()

        memcpy(colorSlot  + cursor, sb.color.data(),  n * sizeof(gos_VERTEX));
        memcpy(extrasSlot + cursor, sb.extras.data(), n * sizeof(gos_TERRAIN_EXTRA));

        PatchStreamBucket& db = s_drawBuckets[s_drawBucketCount++];
        db.textureIndex = sb.textureIndex;
        db.firstVertex  = cursor;
        db.vertexCount  = n;
        cursor += n;
    }

    // 2. Consolidated per-frame upload of terrain_extra_vb_ for grass + any
    //    legacy reader (§7.5 Option A). ONE glBufferData call regardless of
    //    bucket count, sourced from the contiguous extras region we just
    //    wrote into the persistent slot.
    const GLsizeiptr extrasBytes = (GLsizeiptr)cursor * sizeof(gos_TERRAIN_EXTRA);
    GLuint legacyExtraVB = (GLuint)gos_terrain_bridge_getExtraVB();
    if (legacyExtraVB && extrasBytes > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, legacyExtraVB);
        glBufferData(GL_ARRAY_BUFFER, extrasBytes, extrasSlot, GL_DYNAMIC_DRAW);
    }

    // 3. Bind uniforms via the engine bridge — sets the program, samplers,
    //    terrainMVP GL_FALSE, all tess + splatting + shadow uniforms.
    //    apply() inside the bridge calls glUseProgram, after which the
    //    direct glUniform* calls land on the right program (AMD rule line 10).
    gosRenderMaterial* mat = gos_terrain_bridge_getMaterial();
    if (!mat) {
        // No terrain material — abort modern path. Caller falls back to
        // legacy. Emit fence-free, no draws happened.
        restoreGLState(saved);
        return false;
    }
    gos_terrain_bridge_bindUniforms(mat);

    // 4. Bind our persistent color ring as GL_ARRAY_BUFFER and issue
    //    applyVertexDeclaration so locations 0–3 read from it.
    glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
    mat->applyVertexDeclaration();

    // 5. Bind our persistent extras ring at locations 4–5 (worldPos / worldNorm).
    //    Cache attrib locations on first use via gos_terrain_bridge_getShaderProgram()
    //    (Task 0) to avoid the per-draw glGetAttribLocation stall noted in
    //    spec §12.5 #1 (cheap inline fix).
    static GLint locWorldPos  = -1;
    static GLint locWorldNorm = -1;
    if (locWorldPos < 0 || locWorldNorm < 0) {
        GLuint shp = (GLuint)gos_terrain_bridge_getShaderProgram();
        if (shp) {
            locWorldPos  = glGetAttribLocation(shp, "worldPos");
            locWorldNorm = glGetAttribLocation(shp, "worldNorm");
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
    // CRITICAL: attribute pointer offset is 0 (just the field offset
    // within gos_TERRAIN_EXTRA). The slot offset is applied EXACTLY ONCE
    // via the `first` argument of glDrawArrays below. If we also baked
    // it into the pointer offset, the GPU would read from
    // (slotFirstVert + firstVertex) for color but
    // (2*slotFirstVert + firstVertex) for extras — desyncing worldPos /
    // worldNorm from screen-space color data. Earlier draft of this plan
    // had that bug; the comment is here to make sure no one re-introduces
    // it during a refactor.
    if (locWorldPos >= 0) {
        glEnableVertexAttribArray(locWorldPos);
        glVertexAttribPointer(locWorldPos, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA),
            (void*)0);
    }
    if (locWorldNorm >= 0) {
        glEnableVertexAttribArray(locWorldNorm);
        glVertexAttribPointer(locWorldNorm, 3, GL_FLOAT, GL_FALSE,
            sizeof(gos_TERRAIN_EXTRA),
            (void*)(3 * sizeof(float)));
    }

    glPatchParameteri(GL_PATCH_VERTICES, 3);

    // 6. Per-bucket draws. Each bucket = one material change. firstVertex
    //    is bucket-local within the slot. The slot offset is added to
    //    the `first` argument of glDrawArrays — this is the ONE place
    //    the slot offset is applied, for both the color VBO and the
    //    extras VBO simultaneously (since glDrawArrays' `first` is
    //    passed to every bound vertex attribute, both rings advance in
    //    lockstep). See the comment block above the worldPos pointer
    //    set for why the pointer offsets must NOT also include
    //    slotFirstVert.
    for (uint32_t b = 0; b < s_drawBucketCount; ++b) {
        const PatchStreamBucket& bk = s_drawBuckets[b];
        gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
        gos_SetRenderState(gos_State_Terrain, 1);
        gos_SetRenderState(gos_State_Texture, tex_resolve(bk.textureIndex));

        glDrawArrays(GL_PATCHES,
                     (GLint)(slotFirstVert + bk.firstVertex),
                     (GLsizei)bk.vertexCount);
    }

    if (locWorldPos  >= 0) glDisableVertexAttribArray(locWorldPos);
    if (locWorldNorm >= 0) glDisableVertexAttribArray(locWorldNorm);
    mat->endVertexDeclaration();
    mat->end();

    // 7. Fence the slot.
    if (s_fence[s_slot]) glDeleteSync(s_fence[s_slot]);
    s_fence[s_slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    restoreGLState(saved);

    if (!s_firstFlushSeen) {
        s_firstFlushSeen = true;
        fprintf(stderr,
            "[PATCH_STREAM v1] event=first_flush slot=%u verts=%u buckets=%u\n",
            s_slot, cursor, s_drawBucketCount);
        fflush(stderr);
    }
    if (s_traceOn) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=draw_count slot=%u verts=%u buckets=%u\n",
            s_slot, cursor, s_drawBucketCount);
        fflush(stderr);
    }

    return true;
}
```

The pseudocode in spec §6.3 maps directly to this implementation. The deviations from the rev-1 spec language are documented in the spec rev-2 / rev-5 banners; key ones:
- `glDrawArrays(GL_PATCHES, …)` instead of `glDrawElements` (§6.5).
- `GL_ARRAY_BUFFER` bind (NOT `glBindBufferRange(GL_SHADER_STORAGE_BUFFER)` — the rev-1 SSBO leftover that was scrubbed in spec rev-5).
- Bridge calls instead of forward-declared method calls (Task 0 introduces the bridge to make this compile in a separate TU).

All four bridge accessors (`getMaterial`, `getExtraVB`, `getShaderProgram`, `bindUniforms`) are added in Task 0. Task 6 has no plan-mid corrections to apply.

- [ ] **Step 4: Build**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```

`flush()` has no callers yet — Task 7 wires it. Smoke output should still be unchanged.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(patchstream): flush() — consolidated extras upload + per-bucket draws

Phase 1: walk per-texture staging buckets, memcpy each into contiguous
slot region, record draw bucket. Phase 2: ONE glBufferData of the
consolidated extras to terrain_extra_vb_ for grass (§7.5 Option A).
Phase 3: bind uniforms via gos_terrain_bridge_bindUniforms (Task 0
bridge — flushes the same direct uniform set as terrainDrawIndexedPatches:
2700-2780, GL_FALSE matrix, after-apply order). Phase 4: per-bucket
glDrawArrays(GL_PATCHES, slotFirstVert + bucket.firstVertex, bucket.vertexCount).
Phase 5: fence the slot. Lifecycle: first_flush + (trace) draw_count."
```

---

## Task 7: `Render.TerrainSolid` arm — dispatch flush() with whole-frame fallback

**Files:**
- Modify: `mclib/txmmgr.cpp` — lines 1296–1358

**Goal:** When killswitch is on AND the patch stream has not overflowed, dispatch `TerrainPatchStream::flush()` instead of the per-node legacy loop. On overflow OR if `flush()` returns false, fall through to the legacy loop **for the whole frame**, using the legacy ring data which is still being written by `addVertices`/`fillTerrainExtra` (per §3, §9). Killswitch=0 path is bit-identical to today.

- [ ] **Step 1: Add the dispatch wrapper**

In `mclib/txmmgr.cpp`, add an include near the top:

```c++
#include "GameOS/gameos/gos_terrain_patch_stream.h"
```

Replace the `Render.TerrainSolid` block (the existing block at lines ~1296-1358):

```c++
    {
        ZoneScopedN("Render.TerrainSolid");
        TracyGpuZone("Render.TerrainSolid");

        // Modern path. flush() returns true on success and false on overflow
        // / not-ready / not-killswitched. On false we fall through to the
        // legacy loop for the WHOLE FRAME — never partial-frame. The legacy
        // ring data has been kept in sync by addVertices/fillTerrainExtra
        // running unconditionally in quad.cpp.
        bool modernHandled = false;
        if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
            modernHandled = TerrainPatchStream::flush();
        }

        bool bSkip_DRAWSOLID = false;
        if (!modernHandled) {
            for (long i = 0; i < nextAvailableVertexNode && !bSkip_DRAWSOLID; i++)
            {
                if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
                    (masterVertexNodes[i].vertices))
                {
                    if (masterVertexNodes[i].flags & MC2_ISTERRAIN) {
                        gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
                        gos_SetRenderState( gos_State_Terrain, 1 );
                    } else {
                        gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
                        gos_SetRenderState( gos_State_Terrain, 0 );
                    }

                    DWORD totalVertices = masterVertexNodes[i].numVertices;
                    if (masterVertexNodes[i].currentVertex !=
                        (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                    {
                        totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                    }

                    if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) && masterVertexNodes[i].extras) {
                        int extraCount = masterVertexNodes[i].currentExtra
                            ? (int)(masterVertexNodes[i].currentExtra - masterVertexNodes[i].extras)
                            : 0;
                        gos_SetTerrainBatchExtras(masterVertexNodes[i].extras, extraCount);
                    } else {
                        gos_SetTerrainBatchExtras(NULL, 0);
                    }

                    if (totalVertices && (totalVertices < MAX_SENDDOWN))
                    {
                        gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
                        gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                    }
                    else if (totalVertices > MAX_SENDDOWN)
                    {
                        gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
                        long currentVertices = 0;
                        while (currentVertices < totalVertices)
                        {
                            gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                            long tVertices = totalVertices - currentVertices;
                            if (tVertices > MAX_SENDDOWN)
                                tVertices = MAX_SENDDOWN;
                            gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
                            currentVertices += tVertices;
                        }
                    }
                    masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
                }
            }
        } else {
            // Modern handled — still need to reset the legacy ring's
            // currentVertex pointers so next frame's appends start fresh.
            // This loop is the legacy reset that the if-branch did
            // implicitly via the draw-then-reset pattern.
            for (long i = 0; i < nextAvailableVertexNode; i++) {
                if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
                    (masterVertexNodes[i].vertices))
                {
                    masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
                }
            }
        }
    }   // end Render.TerrainSolid zone
```

The `else` branch's reset loop is critical: the legacy ring's `currentVertex` pointer is reset every frame at the bottom of the legacy loop. If we skip the legacy loop, we must still reset that pointer; otherwise subsequent frames see stale `currentVertex - vertices` lengths and Shadow.StaticAccum reads garbage.

**Critical: do NOT touch `currentExtra`.** That pointer's lifecycle is owned by `addTerrainExtra` / `clearArrays` — frame-reset is via `clearArrays()` per spec §3 (which `free()`s and `memset`s the node). Adding a `currentExtra` reset here would be a double-reset bug.

- [ ] **Step 2: Build**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -5
```

- [ ] **Step 3: Smoke killswitch=0 — bit-identical legacy parity**

```bash
MC2_MODERN_TERRAIN_SURFACE=0 py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing --keep-logs
```
Expected: `result=PASS`, FPS within 2 % of pre-PR baseline. The added env-check should compile to a single branch and have no measurable cost.

- [ ] **Step 4: Smoke killswitch=1 — first end-to-end modern run**

```bash
MC2_MODERN_TERRAIN_SURFACE=1 MC2_PATCH_STREAM_TRACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing --keep-logs
grep "PATCH_STREAM v1" tests/smoke/artifacts/*/mc2_01.log | head -20
```
Expected:
- `event=init slots=3 ...`
- `event=first_flush slot=N verts=>0 buckets=>=1`
- Multiple `event=draw_count` lines (one per frame)
- No `event=overflow`, no `event=init_fail`
- `result=PASS`
- Visual: terrain rendered correctly. **Take a screenshot at fixed camera and diff against killswitch=0 screenshot from Step 3.** Visual diff should be zero or limited to subpixel timing artifacts.

If terrain is missing, has holes, or shows tearing: the most likely cause is the `currentVertex` reset loop in the `else` branch interacting badly with the modern path's read; the second most likely is a uniform missing from `terrainBindUniformsForPatchStream` (Task 6 Step 1 should have copied them all — verify against the original `terrainDrawIndexedPatches:2688-2782`).

- [ ] **Step 5: Commit**

```bash
git add mclib/txmmgr.cpp
git commit -m "feat(patchstream): Render.TerrainSolid dispatches flush() with legacy fallback

Killswitch + non-overflow → TerrainPatchStream::flush(). flush()=false
or overflow or killswitch=0 → original per-node legacy loop. Modern
branch still resets MC_VertexArrayNode::currentVertex so Shadow.StaticAccum
sees current-frame data on the next pass. currentExtra untouched
(owned by clearArrays). Spec §4 step 4, §8.3 overflow, §9 duplication."
```

---

## Task 8: Verification gate — killswitch=0 legacy parity (full tier1)

**Goal:** Confirm zero behavior change on the legacy path with the M0b code merged.

- [ ] **Step 1: Capture pre-M0b baseline**

If a clean baseline doesn't exist, `git stash` the local M0b changes, build, run:
```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs
```
Save the artifact dir as `tests/smoke/artifacts/baseline-pre-m0b/` for comparison.

`git stash pop`, rebuild.

- [ ] **Step 2: Run tier1 with killswitch=0**

```bash
MC2_MODERN_TERRAIN_SURFACE=0 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs
```

Expected:
- All 5 missions PASS
- `Δ destroys` = 0 for every mission
- Avg FPS within 2 % of baseline
- p1% within 5 % of baseline
- mission_ready_ms within 200 ms of baseline

- [ ] **Step 3: Diff `report.json` against the baseline**

```bash
diff <(jq '.rows[] | {stem, result, avg_fps, p1low_fps, mission_ready_ms, destroys_delta}' \
       tests/smoke/artifacts/baseline-pre-m0b/report.json) \
     <(jq '.rows[] | {stem, result, avg_fps, p1low_fps, mission_ready_ms, destroys_delta}' \
       tests/smoke/artifacts/<latest>/report.json)
```

Expected: no `destroys_delta` change, FPS / load-time within tolerance bands above.

- [ ] **Step 4: Commit (no code change — just a checkpoint)**

If everything passes, no commit is needed for this task; otherwise STOP and investigate.

---

## Task 9: Verification gate — killswitch=1 stock-install playable + Magic mod canary

**Goal:** Modern path runs cleanly on stock content and the Magic mod (the project's known mod-content-stress canary).

- [ ] **Step 1: Stock tier1 with killswitch=1**

```bash
MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs
grep -c "event=overflow" tests/smoke/artifacts/<latest>/*.log
grep -c "event=init_fail" tests/smoke/artifacts/<latest>/*.log
```
Expected:
- All 5 missions PASS
- 0 overflow events (Wolfman is stock-tier; if it overflows, sizing in §5.4 needs revision)
- 0 init_fail events
- Visual: zero diff vs. killswitch=0 screenshots from Task 8

- [ ] **Step 2: Magic mod canary**

The project's `magic_abl_contamination_rule.md` documents a Magic-content stress mission. Run it explicitly:

```bash
# Replace mc2_magic with the actual stem from tests/smoke/smoke_missions.txt
# (look for tier=tier1 entry tagged 'magic' or the documented Magic canary path).
MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py --mission <magic_canary_stem> --kill-existing --keep-logs
```
Expected: PASS with zero overflow/init_fail.

- [ ] **Step 3: Stock-install rule sanity (use a separate clean install)**

The spec §10 stock-install constraint requires that modern data is
generated at runtime from stock assets. The patch stream is purely
runtime — no FST/sidecar dependency. **Don't `mv` directories out of
the live deploy** (destructive; one slip can wipe upscaled art). Use
the existing clean stock install at `A:/Games/mc2-opengl/mc2-fresh-install/`
instead. Deploy the new exe + bridge header to that directory and run
from there:

```bash
FRESH=A:/Games/mc2-opengl/mc2-fresh-install
SRC=A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev

# Verify fresh install is genuinely stock — no data/art/ overrides.
ls "$FRESH/data/art" 2>/dev/null && echo "WARNING: fresh install has data/art overrides" || echo "OK: stock"

cp -f "$SRC/build64/RelWithDebInfo/mc2.exe" "$FRESH/mc2.exe"
diff -q "$SRC/build64/RelWithDebInfo/mc2.exe" "$FRESH/mc2.exe"

# Run mc2_01 against the fresh install. Use --exe to point the smoke
# runner at the right binary.
MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py \
    --mission mc2_01 --duration 10 --kill-existing --keep-logs \
    --exe "$FRESH/mc2.exe"
```
Expected: PASS. The patch stream does not depend on any asset-side
change. If `mc2-fresh-install/` does not exist or is not actually stock,
**stop and create one** before running this step — the value of this
gate depends on the install being known-clean.

---

## Task 10: Verification gate — static-shadow A/B (RAlt+F3) + grass A/B (RAlt+5)

**Goal:** Confirm that legacy ring writes still feed Shadow.StaticAccum (RAlt+F3) and grass (RAlt+5) correctly under killswitch=1. This is the key BR3/BR7 verification.

- [ ] **Step 1: Static-shadow A/B**

The smoke runner doesn't expose the keyboard, so this is a manual test. Procedure:

1. Launch killswitch=1 directly:
   ```powershell
   $env:MC2_MODERN_TERRAIN_SURFACE="1"
   Start-Process -FilePath "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe" -WorkingDirectory "A:/Games/mc2-opengl/mc2-win64-v0.2"
   ```
2. Skip into mc2_01.
3. Pause at a known camera position (use a vista with prominent terrain shadows).
4. Press `RAlt+F3` to toggle shadows off → on. Compare side by side.
5. Take screenshots; restart with killswitch=0 and repeat.
6. Diff the two killswitch=1 screenshots against the two killswitch=0 screenshots.

Expected: shadow shape, size, and contrast are visually indistinguishable across killswitch states. Any visible drift is a Shape B' regression — STOP and investigate.

- [ ] **Step 2: Grass visibility A/B**

Same procedure but toggle `RAlt+5` (grass) at a known grass-rich vista (one of the early grass-biome missions — mc2_01 opening if applicable, otherwise the documented grass-test mission).

Expected: grass density, placement, and worldPos all match between killswitch states. The most likely failure mode is the consolidated `updateBuffer(terrain_extra_vb_)` upload in `flush()` writing wrong bytes — if grass is wrong but shadows are right, suspect the consolidated extras layout.

- [ ] **Step 3: Document results**

Append A/B verification screenshots + outcome to `tests/smoke/artifacts/<latest>/m0b-shadow-grass-ab.md`:

```markdown
# M0b shadow + grass A/B verification

Date: <date>
Build: <commit hash>
Hardware: <GPU + driver>

## Static shadow (RAlt+F3)
- killswitch=0: <screenshot>
- killswitch=1: <screenshot>
- visual diff: <pass/fail + notes>

## Grass (RAlt+5)
- killswitch=0: <screenshot>
- killswitch=1: <screenshot>
- visual diff: <pass/fail + notes>
```

---

## Task 11: Verification gate — pool-headroom + lifecycle prints

**Goal:** TGL pool peaks unchanged; required `[PATCH_STREAM v1]` events all present.

- [ ] **Step 1: Pool-headroom**

```bash
MC2_MODERN_TERRAIN_SURFACE=1 MC2_TGL_POOL_TRACE=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing --keep-logs
grep "TGL_POOL v1.*event=mission_unload" tests/smoke/artifacts/<latest>/*.log
```
Expected: peaks within the spec §2 envelope. If anything is > 25 % higher than the baseline, suspect a CPU-side mirror leak (the patch stream's CPU writes are mapped pointers, NOT TGL pool draws — the only path that should affect TGL is the legacy `addVertices` calls, which are unchanged).

- [ ] **Step 2: Lifecycle print presence (smoke run, force-kill expected)**

```bash
LOG=tests/smoke/artifacts/<latest>/mc2_01.log
echo "init: $(grep -c 'event=init ' $LOG)"
echo "first_flush: $(grep -c 'event=first_flush' $LOG)"
echo "init_fail: $(grep -c 'event=init_fail' $LOG)"
echo "overflow: $(grep -c 'event=overflow' $LOG)"
```
Expected:
- `init`: 1
- `first_flush`: 1 (one per process)
- `init_fail`: 0
- `overflow`: 0

The smoke runner force-kills mc2.exe, so `event=shutdown` is **not**
expected here — Step 3 below verifies it via a clean exit. If any
other required event is missing, the lifecycle wiring is wrong — fix
before proceeding.

- [ ] **Step 3: Clean-exit `event=shutdown` verification (manual)**

Launch the game with the canary env vars set, navigate to the main
menu, choose "Quit" so the engine runs its destructor chain:

```powershell
$env:MC2_MODERN_TERRAIN_SURFACE="1"
$deploy = "A:/Games/mc2-opengl/mc2-win64-v0.2"
$log = "$deploy/clean_exit.log"; Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$deploy/mc2.exe" -RedirectStandardError $log -PassThru -WorkingDirectory $deploy
# Manually: click through to main menu, choose Quit, wait for process to exit.
$p.WaitForExit()
"exit code=$($p.ExitCode)"
Get-Content $log | Select-String "PATCH_STREAM"
```
Expected:
- `event=init` line at top
- `event=shutdown` line near the bottom
- Process exit code 0

If `event=shutdown` is missing on clean exit, the destructor wiring
in `gosRenderer::destroy()` (Task 1 Step 3) is wrong.

---

## Task 12: Verification gate — overflow-fallback (debug build with shrunken slot)

**Goal:** Force an overflow at runtime and confirm the whole-frame fallback to legacy renders correctly with no holes.

- [ ] **Step 1: Add a debug shrink env var**

In `gos_terrain_patch_stream.cpp`, replace the literal `kPatchStreamColorBytesPerSlot` and `kPatchStreamExtrasBytesPerSlot` reads inside `init()` with values that respect an env override:

```c++
// At top of init(), after reading s_killswitch and s_traceOn:
uint32_t colorBytes = kPatchStreamColorBytesPerSlot;
uint32_t extrasBytes = kPatchStreamExtrasBytesPerSlot;
if (const char* shrink = getenv("MC2_PATCH_STREAM_SHRINK_BYTES")) {
    uint32_t v = (uint32_t)atoi(shrink);
    if (v > 0 && v < colorBytes) {
        colorBytes  = v;
        extrasBytes = (v * 24) / 32;  // keep extras proportional
    }
}
// ... use colorBytes / extrasBytes when computing colorTotal / extrasTotal ...
```

This is a targeted instrumentation knob — leave it in the tree (env-gated, default 0/no shrink), so future overflow regression tests reuse it. Per `memory/debug_instrumentation_rule.md`: demote, don't delete.

- [ ] **Step 2: Build, deploy**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -3
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe
```

- [ ] **Step 3: Force overflow with a tiny slot**

```bash
MC2_MODERN_TERRAIN_SURFACE=1 MC2_PATCH_STREAM_SHRINK_BYTES=4096 \
    py -3 scripts/run_smoke.py --mission mc2_01 --duration 10 --kill-existing --keep-logs
grep -E "event=overflow|event=first_flush" tests/smoke/artifacts/<latest>/mc2_01.log | head -5
```
Expected:
- `event=overflow` fires within the first second of mission render (4 KB / 32 B = 128 verts before exhaustion)
- `result=PASS` (legacy fallback handled the frame correctly)
- Visual: terrain rendered correctly throughout — no holes, no flicker

If terrain shows holes when overflow fires, the `else` branch's `currentVertex` reset (Task 7 Step 1) is wrong — most likely it's running on the wrong nodes.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_terrain_patch_stream.cpp
git commit -m "feat(patchstream): MC2_PATCH_STREAM_SHRINK_BYTES debug knob

Env-gated slot-size override for overflow regression testing. Default
0 = no override. Kept in-tree per debug_instrumentation_rule (demote,
don't delete). Verified: 4 KB slot triggers overflow within ~1s of
gameplay, whole-frame fallback to legacy renders correctly."
```

---

## Task 13: Verification gate — AMD canary RX 7900 XTX end-to-end + final §11 success thresholds

**Goal:** Final integration gate. AMD-specific verification + the spec §11 success thresholds for Tracy delta, GL upload delta, visual A/B.

- [ ] **Step 1: AMD canary**

Run a longer mc2_01 + a known mech-heavy mission (`mc2_03` per `tests/smoke/smoke_missions.txt`) on the RX 7900 XTX hardware:

```bash
MC2_MODERN_TERRAIN_SURFACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_03 --duration 30 --kill-existing --keep-logs
grep "GL_ERROR" tests/smoke/artifacts/<latest>/*.log | head
```
Expected:
- Both missions PASS
- Zero `[GL_ERROR v1]` lines
- Zero overflow events
- Tracy GPU zone `Render.TerrainSolid` shows no spikes vs. baseline

- [ ] **Step 2: Tracy delta — `Terrain.DrawPatches`**

Capture a Tracy trace of mc2_01 at killswitch=0 (baseline) and killswitch=1. Compare:
- `Terrain.DrawPatches` zone mean per-frame ms
- `Render.TerrainSolid` zone mean per-frame ms
- `Terrain::geometry` zone mean per-frame ms (must NOT regress)

Expected (per spec §11):
- `Terrain.DrawPatches` reduced by ≥ 0.20 ms/frame at Wolfman (target 0.30 ms/frame or lower; today's residual is 0.51 ms/frame).
- `Render.TerrainSolid` overall ≤ today.
- No regression in `Terrain::geometry`.

If the Tracy delta is negative or null, the most likely cause is the per-bucket `material->apply()` call paying the same uniform-upload cost as today (which is by design — AMD rule line 11 forbids skipping). The savings come from eliminating `glBufferData` uploads, which Tracy may not show explicitly. Drop to `apitrace` or RenderDoc if needed to confirm `glBufferData` call count went from N×2 per frame (color + extras per batch) to 1 per frame (consolidated extras only).

- [ ] **Step 3: GL upload delta**

Counted via apitrace or RenderDoc:
- Pre-M0b: per-frame `glBufferData` calls inside `terrainDrawIndexedPatches` = ~6–10 (one per material) × 2 (color + extras) = ~12–20 calls, ~13 MB total.
- Post-M0b at killswitch=1: per-frame `glBufferData` calls in flush() = 1 (consolidated extras only) = ~5.7 MB.

Expected: matches the spec §9.2 prediction.

- [ ] **Step 4: Visual A/B at three vistas**

Take fixed-camera screenshots at killswitch=0 and killswitch=1 at:
- Wolfman (`visibleVerticesPerSide=200`) — RAlt-modified zoom
- Default RTS zoom
- mc2_01 stock first mission, opening cinematic camera

Pixel-diff via ImageMagick:
```bash
magick compare -metric AE shot_off.png shot_on.png diff.png
```
Expected: AE (absolute pixel error) ≤ 0.1 % of total pixels, all differences explainable by sub-frame timing (e.g. moving particle effects).

- [ ] **Step 5: Document the success-threshold results**

Add to `tests/smoke/artifacts/<latest>/m0b-success-threshold.md`:

```markdown
# M0b §11 success-threshold results

Date: <date>
Build: <commit hash>

| Metric                             | Target            | Observed       | Pass? |
|------------------------------------|-------------------|----------------|-------|
| Terrain.DrawPatches reduction      | ≥ 0.20 ms/frame   | <X> ms/frame   | <Y/N> |
| Render.TerrainSolid                | ≤ today           | <X> ms/frame   | <Y/N> |
| Terrain::geometry                  | no regression     | <X> ms/frame   | <Y/N> |
| glBufferData calls / frame         | from ~13 MB → 5.7 | <X> MB         | <Y/N> |
| Visual A/B (3 vistas)              | AE ≤ 0.1 %        | <X> %          | <Y/N> |
| [GL_ERROR v1] count                | 0                 | <N>            | <Y/N> |
| event=overflow count               | 0                 | <N>            | <Y/N> |
```

If all rows pass: M0b is mergeable. If any row fails: stop and triage.

---

## Task 14: Memory entry + final commit

**Files:**
- Create: `~/.claude/projects/A--Games-mc2-opengl-src/memory/patchstream_m0b.md`
- Modify: `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

**Goal:** Durable memory entry so future sessions see the M0b shape, the killswitch state, and the Shape B' / B-array follow-on lanes.

- [ ] **Step 1: Create the memory file**

```markdown
---
name: PatchStream M0b — terrain solid color path
description: Persistent-mapped VBO seam for Render.TerrainSolid; killswitch MC2_MODERN_TERRAIN_SURFACE; default off through validation; eliminates ~13 MB/frame glBufferData churn at Wolfman; legacy rings still feed shadow + grass
type: project
---

PatchStream M0b landed at <commit-hash>. Goal: replace per-batch glBufferData
churn inside terrainDrawIndexedPatches with persistent-mapped VBO rings.

**Why:** §9 spec — eliminate ~7.6 MB/frame color VBO uploads + collapse N
per-batch extras uploads to 1 consolidated upload. CPU memcpy goes UP in M0b
(legacy rings stay populated for shadow + grass); the win is GL upload
elimination, not CPU memcpy reduction. CPU-memcpy win is Shape B' lane.

**How to apply:**
- Killswitch is `MC2_MODERN_TERRAIN_SURFACE`, default 0. Default flips to 1
  only after a release cycle running default-off cleanly across tier1.
- Overflow → whole-frame fallback to legacy. Safe in M0b because legacy
  rings are still populated.
- DO NOT redesign M0b around sampler2DArray. AMD ban is softened (Canary
  A passed, see exploration doc) but production sampler2DArray is gated
  on Canary B and is a separate Shape B-array lane.
- Pointer-delta invariant (txmmgr.h:72-100): `currentVertex - vertices`
  and `currentExtra - extras` are the implicit fill counts. Modern
  flush() does NOT touch these. Legacy addVertices/fillTerrainExtra
  continue to advance them.
- Spec: docs/superpowers/specs/2026-04-27-patchstream-shape-b-design.md
- Plan: docs/superpowers/plans/2026-04-27-patchstream-m0b-plan.md

**Follow-on lanes (out of M0b scope):**
- Shape B-array — sampler2DArray draw collapse, gated on Canary B
- Shape B' — migrate Shadow.StaticAccum + grass off legacy rings
- glGetAttribLocation per-draw stall fix
- matNormal0-4 per-node rebind hoist
- Unit-9 collision (matNormal4 ↔ shadowMap)
```

- [ ] **Step 2: Add the index entry**

In `MEMORY.md` under the "Rendering / shaders" or "Features / subsystems" section, add:

```markdown
- [PatchStream M0b — terrain solid color path](patchstream_m0b.md) — env-gated MC2_MODERN_TERRAIN_SURFACE persistent-mapped VBO seam; default off; legacy rings still feed shadow + grass; sampler2DArray ban softened post Canary A
```

- [ ] **Step 3: Final commit**

```bash
git add docs/superpowers/plans/2026-04-27-patchstream-m0b-plan.md
git commit -m "docs(patchstream): M0b implementation plan

TDD-style task list covering 13 tasks: skeleton class through final
success-threshold verification. References spec rev 4, all four audit
explorations, and the canary report. Out-of-scope lanes (B-array,
B', stall fixes) explicitly tracked as follow-ups."
```

(Memory files are written outside the worktree — they don't get committed there.)

---

## Plan revision history

- **Rev 3 (2026-04-27)** — second-pass advisor blockers fixed:
  - **`getShaderProgram` bridge promoted** to Task 0 — header decl +
    impl land in Task 0 Steps 1–2 instead of an inline note in Task 6.
    Task 6 has zero plan-mid corrections.
  - **Retained staging buckets** — Task 3 changed from
    `std::vector<PatchStagingBucket> s_staging` (which destroys + frees
    bucket vectors on every `clear()`) to a fixed `s_staging[kPatchStreamMaxBuckets]`
    + `s_stagingCount`. Bucket vector capacity is reserved once at init
    and retained across frames; only contents are cleared each frame.
    Zero allocator churn after warmup.
  - **Critical bug fix — extras attribute pointer offset**: rev 2 had
    `glVertexAttribPointer` with offset `slotFirstVert * sizeof(...)`
    AND `glDrawArrays` with `slotFirstVert + firstVertex`. OpenGL adds
    both, so extras would have read from `2 * slotFirstVert + firstVertex`
    — desyncing worldPos/worldNorm from screen color. Fix: extras
    attribute pointer offset is 0; slot offset is applied exactly once
    via `glDrawArrays`'s `first` parameter (which advances both color
    and extras attributes in lockstep). Inline comment added so future
    refactors don't reintroduce the bug.
  - **Fence-wait wording corrected** — `GL_TIMEOUT_IGNORED` is blocking,
    not "non-blocking." Comment updated; added
    `[PATCH_STREAM v1] event=fence_stall` log when wait time ≥ 1 ms so
    real stalls show up in profiling.
  - **Engine API includes spelled out** — `tex_resolve`,
    `gos_SetRenderState`, `timing::get_wall_time_ms` — Task 6 now lists
    them as dependencies the implementer must pull in alongside
    `gameos.hpp`.
- **Rev 2 (2026-04-27)** — advisor-flagged blockers fixed:
  - **Bridge architecture** (advisor #2): Task 0 added — C-style bridge
    in `gos_terrain_bridge.{h,cpp-block}` so `TerrainPatchStream`
    (separate TU) can reach `gosRenderer` (private to gameos_graphics.cpp)
    without forward-declaring methods on an incomplete type.
  - **Bucket logic** (advisor #3): Task 3 reworked from run-bucket to
    real per-texture bucketing via CPU staging. `appendTriangle` now
    accumulates into `std::vector` keyed by `textureIndex`; `flush()`
    consolidates into the persistent ring at flush time. Result: one
    bucket per distinct `textureIndex`, independent of quad traversal
    order.
  - **Spec SSBO drift** (advisor #4): scrubbed in spec rev 5;
    operational `glBindBufferRange` reference at §7.2 rewritten to
    `glDrawArrays`. Plan-overrides-spec note added at the top of this
    plan covering any remaining ambiguity.
  - **Init-fail driver stress** (advisor #5): Task 2 Step 4 replaced
    16 GB allocation hack with `MC2_PATCH_STREAM_FORCE_INIT_FAIL=1`
    env var that takes the fail path without touching GL.
  - **Destructive `mv data/art`** (advisor non-blocking #1): Task 9
    Step 3 now uses `A:/Games/mc2-opengl/mc2-fresh-install/` (a
    separate clean install) instead of moving directories around in
    the live deploy.
  - **Force-kill shutdown verification** (advisor non-blocking #2):
    Task 1 Step 5 clarified to expect missing `event=shutdown` under
    force-kill; Task 11 Step 3 added for the clean-exit verification.
  - **Recommended subagent review checkpoints**: after Task 3, Task 6,
    Task 7. Each is a non-trivial integration touchpoint where a
    fresh agent's review catches drift before it cascades.
- **Rev 1 (2026-04-27)** — initial plan, 14 tasks.

## Self-Review

**1. Spec coverage:**
- Spec §1 thesis → Tasks 0–7 (the entire build-out)
- Spec §2 pool framing → Task 11 Step 1
- Spec §3 stays-legacy / becomes-modern → Tasks 1–4 (modern), Task 5 (legacy preserved)
- Spec §4 M0b slice steps 1–7 → Tasks 1–6 (skeleton/alloc/append/rotate/quad-hooks/flush) + Task 7 (Render.TerrainSolid arm)
- Spec §5 buffer layout / sizing / flags / slot rotation → Task 2 (alloc + flags), §5.4 sizing constants in the Definitions block
- Spec §6 draw architecture (count / triggers / material / AMD rules / index strategy) → Task 6 Steps 1–3 + the §6.5 glDrawArrays preference
- Spec §7 open questions resolved → already resolved, plan inherits
- Spec §8 killswitch (env / default / failure modes) → Tasks 1, 2, 7
- Spec §9 duplication caveat → Task 5 (legacy stays) + Task 11 (pool headroom)
- Spec §10 risk register BR1–BR8 → BR1 (state save/restore Task 2/6); BR2/BR8 dropped (no shaders); BR3 (Task 10 shadow A/B); BR4 (Task 5 inside-pz-brace constraint); BR5 (legacy `addTriangle` keeps running, no removal); BR6 (Tasks 3, 12 overflow); BR7 (Task 10 grass A/B)
- Spec §11 success threshold → Task 13
- Spec §12 verification gates → Tasks 8 (parity), 9 (stock + Magic), 10 (shadow + grass A/B), 11 (pool + lifecycle), 12 (overflow), 13 (AMD + thresholds)
- Spec §12.5 out-of-scope wins → listed in plan front-matter; not implemented
- Spec §13 readiness verdict → satisfied by plan execution

**2. Placeholder scan:** No `TBD`/`TODO`/`implement later`/`add appropriate error handling` strings. All steps either show concrete code blocks or concrete commands with expected output. The "compare to baseline" verification steps name the exact `jq` invocation and the metric tolerance bands.

**3. Type consistency check:**
- `TerrainPatchStream::init / destroy / isReady / isOverflowed / appendTriangle / flush / beginFrame` — same names from Definitions block through Task 7.
- `PatchStreamBucket { textureIndex, firstVertex, vertexCount }` — used identically in Task 3 (write) and Task 6 (read).
- `kPatchStreamRingFrames`, `kPatchStreamMaxBuckets`, `kPatchStreamColorBytesPerSlot`, `kPatchStreamExtrasBytesPerSlot` — declared once in the header, referenced by literal name throughout.
- `gosRenderer::terrainBindUniformsForPatchStream` / `getTerrainMaterial` / `getTerrainExtraVB` — defined in Task 6, used only in Task 6. `g_gos_renderer` is the existing engine global referenced from the cpp.
- `buildTerrainExtraTriple` — defined in Task 5 Step 2, called only from Task 5 Steps 3–4.
- Lifecycle event names — used identically in `init()`, `destroy()`, `flush()`, `appendTriangle()`, and Task 11 Step 2 grep counts.

**4. Found issues:** None during self-review. Plan is internally consistent.

---

## Execution Handoff

Plan complete and saved (rev 2) to `docs/superpowers/plans/2026-04-27-patchstream-m0b-plan.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. **Mandatory review checkpoints after Tasks 3, 6, and 7** (per rev-2 advisor feedback — these are the non-trivial integration touchpoints). Good fit here: tasks 0–7 are sequential implementation; tasks 8–13 are independent verification gates that can run in parallel after task 7.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints for review.

Which approach?
