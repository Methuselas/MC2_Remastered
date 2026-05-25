# MC2 OpenGL Remaster — Editor / Contributor Handoff
*Audience: Methuselas, working on MFC Editor fork off 0.2.*
*Read-only audit of the main repo (nifty-mendeleev worktree). Citations relative to source root.*

---

## Task A — Debug and logging conventions

### A1. Runtime log line

**The "engine log" is the Windows debugger stream, not a file.**

`SPEW(x)` writes through `InternalFunctionSpew` → `OutputDebugString()` on Windows, or `fprintf(stderr, ...)` on Linux. However, **`SPEW` compiles to `((void)0)` unless `_ARMOR` is defined**, and `_ARMOR` is only injected for Debug builds:

```cmake
# CMakeLists.txt:43
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_ARMOR -D_DEBUG -DBUGLOG -DLAB_ONLY")`
```

The project **always** ships as `RelWithDebInfo` (Release without `_ARMOR` crashes on GL_INVALID_ENUM). `SPEW` is therefore silent in every distributed build.

**Use `SPEWALWAYS` for output that must survive RelWithDebInfo:**

```cpp
#include <gameos.hpp>                              // GameOS/include/gameos.hpp:3340
SPEWALWAYS(("MY_SYSTEM", "value=%d\n", myValue)); // always fires; same sink as SPEW
```

Or use bare `fprintf(stderr, ...)` / `printf(...)` for one-off diagnostics.

| Symbol | Header | Active when | Output sink |
|---|---|---|---|
| `SPEW(("GRP","fmt",...))` | `GameOS/include/gameos.hpp:134` | `_ARMOR` only (Debug) | `OutputDebugString` / stderr |
| `SPEWALWAYS(("GRP","fmt",...))` | `GameOS/include/gameos.hpp:3340` | always | same |
| `fprintf(stderr,...)` | `<cstdio>` | always | stderr / console |

`OutputDebugString` output is visible in **Sysinternals DebugView** or the **Visual Studio Output window**. Not visible in a console unless you call `AllocConsole()` + `freopen("CONOUT$","w",stderr)`.

`InternalFunctionSpew` implementation: `GameOS/gameos/gameos_debugging.cpp:81`.

---

### A2. Tracy profiler zones

Include the thin wrapper that guards against Tracy being absent:

```cpp
#include "gos_profiler.h"               // GameOS/gameos/gos_profiler.h
```

This header conditionally includes `<tracy/Tracy.hpp>` and `<tracy/TracyOpenGL.hpp>` via `__has_include`. If Tracy headers are present it defines `MC2_HAS_TRACY`; if not, it provides no-op stubs, so the include is always safe.

**Note:** `TRACY_ENABLE` is not unconditionally set at the preprocessor level. The CLAUDE.md "always compiled in" means the Tracy source is vendored in `3rdparty/tracy/` in deployed builds, so `MC2_HAS_TRACY` will normally resolve. The GPU zone macro (`TracyGpuZone`) has no no-op stub in `gos_profiler.h`; if you need it in code that might build without Tracy, add one yourself.

```cpp
// CPU zone (stack-scoped, auto-closes at scope exit):
ZoneScopedN("MySystem.Step");           // gos_profiler.h:23 (stub if no Tracy)

// GPU zone (GL timer query, wraps one or more draw calls):
TracyGpuZone("MySystem.DrawPass");      // TracyOpenGL.hpp macro
```

Real usage pattern: `GameOS/gameos/gameos_graphics.cpp:2865–2866`.

---

### A3. Debug hotkey toggle

All runtime debug toggles live in **`GameOS/gameos/gameosmain.cpp::handle_key_down()`** (`gameosmain.cpp:163`). It is the sole SDL key dispatcher. Three-part pattern:

**1 — Flag.** Either a field in `gosPostProcess` (for visual features) or a `bool` global in the relevant `.cpp`, `extern`-declared in `gameosmain.cpp`:

```cpp
// In your_feature.cpp:
bool g_myFeatureEnabled = false;

// In gameosmain.cpp (add near line 158 with the other externs):
extern bool g_myFeatureEnabled;
```

**2 — Key handler.** Add a `case` inside `handle_key_down()`:

```cpp
case SDLK_F9:                                    // gameosmain.cpp:163
    if (alt_debug) {                             // RAlt must be held
        g_myFeatureEnabled = !g_myFeatureEnabled;
        fprintf(stderr, "My Feature: %s\n", g_myFeatureEnabled ? "ON" : "OFF");
    }
    break;
```

**3 — Render path reads the flag** from `gosPostProcess` or the global, before the relevant draw call.

Existing full examples: `SDLK_F1` → bloom (`gameosmain.cpp:174`), `SDLK_F3` → shadows (`gameosmain.cpp:201`), `SDLK_0` → GPU static props (`gameosmain.cpp:303`).

---

### A4. `--validate` harness

**Entry point:** `GameOS/gameos/gos_validate.cpp::validateParseArgs(int argc, char** argv)` (`gos_validate.cpp:14`), called once at startup in `main()` before the render loop.

**CLI flags:**

| Flag | Arg | Effect |
|---|---|---|
| `--validate` | — | Enables validate mode |
| `--frames N` | int | Exit after N frames (default: 60) |
| `--screenshot path.tga` | path | Write TGA screenshot on exit |
| `--log path.json` | path | Write result JSON (default: `validate.json`) |
| `--enable bloom\|shadows\|fxaa\|grass` | name | Force feature ON for the run |
| `--disable bloom\|shadows\|fxaa\|grass` | name | Force feature OFF for the run |

**`validate.json` output fields:**

```json
{
  "frames": 60,
  "avg_frame_ms": 16.7,
  "max_frame_ms": 22.1,
  "gl_errors": [],
  "shader_errors": [],
  "screenshot": "out.tga",
  "exit_code": 0
}
```

`exit_code` is 0 (pass) or 1 (any GL error or shader compile/link error). Shader errors are captured automatically: `GameOS/gameos/utils/shader_builder.cpp::get_shader_error_status()` calls `validateRecordShaderError()` at `shader_builder.cpp:81,111`.

**Header for adding new telemetry:** `GameOS/gameos/gos_validate.h`.

---

### A5. Editor-specific debug paths

**No `EditorInterface` or `EditorObjectMgr` source files exist in this repository.** Search across all worktrees returned zero results. What does exist:

- **`Viewer/View.cpp`** (visible in older `quirky-blackwell` worktree snapshot) — a Mechlopedia viewer, not a terrain editor. Its `UpdateRenderers()` calls `pMechlopedia->render()` only; no terrain draw path at all.
- **`GameOS/include/mfcplatform.hpp`** — the FASA/MSVC MFC DLL shim. Declares `InitGameOS(HINSTANCE, HWND, char*)`, `RunGameOSLogic()`, `ExitGameOS()`. An MFC-hosted Editor DLL calls these instead of the SDL `main()` in `gameosmain.cpp`.

**Logging in an MFC-hosted build:**

- `SPEWALWAYS` / `SPEW` → `InternalFunctionSpew` → `OutputDebugString()`. Visible in DebugView or VS Output. Not a console.
- `logging::logmsg(eLogCategory, file, line, fmt, ...)` (`GameOS/gameos/utils/logging.cpp:17`) also → `OutputDebugString()` on Windows. Separate utility; both sinks ultimately call `OutputDebugString`.
- **`[SHADER ERROR]` lines go to `printf()`** (`shader_builder.cpp:79,109`). In a GUI-only MFC app, `printf` output is discarded. To recover it: call `AllocConsole()` early in `InitGameOS()` then `freopen("CONOUT$", "w", stdout)` and `freopen("CONOUT$", "w", stderr)`.
- MFC's own `AfxTrace` → `OutputDebugString`. No separate routing needed.

---

## Task B — Black/no terrain in the Editor: hypotheses

The game renders fine on main because `gameosmain.cpp::draw_screen()` runs a strict setup sequence before `Environment.UpdateRenderers()`. The Editor bypasses this sequence entirely. Each hypothesis below is a specific setup step the Editor is likely skipping.

---

### H1 — Static shadow pre-pass never ran: shadow texture all-zeros → black (PROBABILITY: HIGH)

**Inspect first:** `GameOS/gameos/gameos_graphics.cpp:2835`

The terrain fragment shader does (line-by-line, `shaders/gos_terrain.frag:513–517`):

```glsl
float staticShadow = calcShadow(WorldPos, shadowN, terrainLightDir.xyz, shadowTaps);
float dynShadow    = calcDynamicShadow(...);
float shadow       = staticShadow * dynShadow;
c.rgb *= shadow;    // shadow=0 → pure black
```

`calcShadow()` samples `uniform sampler2DShadow shadowMap` (`shaders/include/shadow.hglsl:4`). At `gameos_graphics.cpp:2842`, the engine binds `pp->getShadowTexture()` on texture unit 9. This texture handle is allocated at `gosPostProcess` init, but **its depth values are only written by the static terrain shadow pre-pass**, which runs inside the game's `renderLists()` → `Environment.UpdateRenderers()` chain.

If the Editor never triggers this pre-pass, the depth texture remains uninitialised. AMD treats an uninitialised `GL_DEPTH_COMPONENT` texture sampled via `sampler2DShadow` as depth=0 everywhere. For every terrain fragment, `currentDepth > 0 > storedDepth` → comparison fails → `texture(shadowMap,...)` returns 0.0 → `staticShadow=0` → `c.rgb *= 0` → **pure black**.

**One-sentence test:** Temporarily force `glUniform1i(tl.enableShadows, 0)` unconditionally before the terrain draw (short-circuiting `gameos_graphics.cpp:2837`). If terrain appears normally lit, this is the cause.

---

### H2 — GL context version < 4.1: tessellation shader fails to compile (PROBABILITY: HIGH)

**Inspect first:** `GameOS/gameos/gameos_graphics.cpp:1867`

The `gos_terrain` material uses TCS and TES stages, which require GL ≥ 4.0. The shader is compiled with the `#version 430` prefix. If the MFC Editor creates a context with the legacy `wglCreateContext()` (no `WGL_ARB_create_context`), the context is GL 1.1 compatible profile; TCS/TES stages fail to compile or link and `terrain_material_` is set to `nullptr`:

```cpp
terrain_material_ = gosRenderMaterial::load("gos_terrain", mvar);
if (!terrain_material_)
    printf("[TESS] WARNING: Terrain material failed to load — tessellation disabled\n");
// gameos_graphics.cpp:1871–1873
```

With `terrain_material_ = nullptr`, the terrain draw function returns immediately — no geometry is submitted. The framebuffer shows whatever the clear color is. In an Editor context without `gosPostProcess::beginScene()` setting a sky clear, that is black.

These messages go to `printf()` — **silent in a GUI MFC app without a console** (see A5).

**One-sentence test:** Print `glGetString(GL_VERSION)` at Editor startup. If anything less than 4.3 is reported, recreate the context with `wglCreateContextAttribsARB` specifying version 4.3 core profile.

---

### H3 — Colormap texture (`tex1`) not loaded: default sampler returns black (PROBABILITY: MEDIUM)

**Inspect first:** `mclib/terrtxm.cpp`, `mclib/terrtxm2.cpp`

The terrain frag shader's base color comes from `uniform sampler2D tex1` (per-tile colormap, `gos_terrain.frag:34`). If the Editor does not run the terrain texture manager initialisation, `tex1` is bound to GL texture object 0. Sampling object 0 returns `(0,0,0,1)` → black terrain.

Distinguishing signature: this produces **uniformly flat black** with no shadow gradient or normal-map variation. H1 produces slightly varied dark output because normal-map lighting is applied before the shadow multiply. Flat = H3, varied dark = H1.

**One-sentence test:** Temporarily bind any white 1×1 texture on unit 0 before the terrain draw. If terrain turns white/grey, the texture manager was never initialised.

---

### H4 — `terrainLightDir` uniform not uploaded: zero diffuse contribution (PROBABILITY: MEDIUM)

**Inspect first:** `GameOS/gameos/gameos_graphics.cpp` (terrain uniform upload block, near line 2800)

`terrainLightDir` drives both the normal-map diffuse (`float diffuse = dot(N, terrainLightDir.xyz)`) and is passed into `calcShadow()`. If the uniform is never uploaded it defaults to `vec4(0,0,0,0)`. With `lightDir=(0,0,0)`: `diffuse=0` → `normalLight = mix(0.35, 1.20, 0.0) = 0.35`. On its own this gives 35% brightness — dim but not black. Combined with H1 (`shadow=0`): `c.rgb = original * 0.35 * 0 = black`.

H4 alone manifests as flat, dim terrain with no directional shading but not pure black.

**One-sentence test:** Upload a test value `terrainLightDir = vec4(0.57, 0.57, 0.57, 0.0)` (normalised diagonal) via a direct `glUniform4f` call before the draw and check whether brightness variation appears.

---

### H5 — MRT_ENABLED compiled in, FBO has no attachment 1: AMD driver rejects draw (PROBABILITY: MEDIUM)

**Inspect first:** `GameOS/gameos/gameos_graphics.cpp:211–218`

`MRT_ENABLED` is injected into the shader prefix at material-load time:

```cpp
gosPostProcess* pp = getGosPostProcess();
if (pp && pp->getSceneNormalTexture()) {
    defines_str.append("#define MRT_ENABLED 1\n");
}
// gameos_graphics.cpp:214–216
```

If `gosPostProcess` is initialised and `sceneNormalTex_` exists when `gosRenderer::init()` loads materials, the terrain shader is compiled **with** `layout(location=1) out vec4 GBuffer1` declared. In the game, `gosPostProcess::beginScene()` (`gos_postprocess.cpp:511`) binds the MRT FBO (both attachments) before any draw. If the Editor never calls `beginScene()`, the default framebuffer is active — it has only one color attachment. The GL spec says writes to declared outputs with no attachment are discarded, but **AMD may report `GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER` and skip the draw entirely**.

**One-sentence test:** Call `glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)` immediately before the first terrain draw and log the hex result. Anything other than `0x8CD5` (`GL_FRAMEBUFFER_COMPLETE`) means draws are being dropped; the fix is to call `gosPostProcess::beginScene()` before `Environment.UpdateRenderers()`.

---

### H6 — Shader errors silent in MFC app (PROBABILITY: MEDIUM — surface area, not direct cause)

Not a direct cause of black terrain, but it silently masks every shader-level failure listed above.

`shader_builder.cpp` reports errors via `printf("[SHADER ERROR] ...")` (`shader_builder.cpp:79,109`). In a GUI MFC app with no console, `printf` goes to the void. Every scenario in H2, H3, and the material load warning in H1 is therefore invisible by default.

**Fix:** Add to `InitGameOS()` or as early as possible after GL context creation:

```cpp
AllocConsole();
freopen("CONOUT$", "w", stdout);
freopen("CONOUT$", "w", stderr);
```

This is the single highest-leverage action for diagnosing the Editor — it makes the shader builder, the terrain material loader, and all `[TESS]` warnings immediately readable.

---

### H7 — `glDepthMask` state corruption via `gosRenderMaterial::apply()` (PROBABILITY: LOW)

**File:** `GameOS/gameos/gameos_graphics.cpp:4275`

The game's dynamic shadow pass works around `gosRenderMaterial::apply()` overriding `glDepthMask` by forcing `glDepthMask(GL_TRUE)` explicitly after `apply()`. If the Editor calls `terrain_material_->apply()` without this and the material spec sets `depthMask=false`, terrain fragments pass depth test but are not written to the depth buffer, producing z-fighting artifacts. **This is not black terrain.** Rule this out if the symptom is consistently solid black rather than z-fighting.

---

### H8 — AMD sampler2DArray / attribute 0 / feedback loop (PROBABILITY: LOW)

The terrain shader uses only `sampler2D` uniforms — no `sampler2DArray`. Vertex attribute location 0 is used for vertex position — correct. No feedback loop (shadow FBO is separate from scene FBO). The AMD-specific failure modes in `docs/amd-driver-rules.md` do not apply to the main terrain draw path.

---

## Next steps for Methuselas (priority order)

1. **Allocate a console window** as the very first action in `InitGameOS()`. Without this, H2 and all shader-compile failures are completely invisible. Every subsequent diagnosis step depends on readable `printf` output. (`AllocConsole()` + `freopen("CONOUT$","w",stdout)` + `freopen("CONOUT$","w",stderr)`)

2. **Print `glGetString(GL_VERSION)`** at context creation. If anything less than "4.3" is reported, the tessellation shader cannot compile. Fix: use `wglCreateContextAttribsARB` with `WGL_CONTEXT_MAJOR_VERSION_ARB=4`, `WGL_CONTEXT_MINOR_VERSION_ARB=3`, `WGL_CONTEXT_PROFILE_MASK_ARB=WGL_CONTEXT_CORE_PROFILE_BIT_ARB`. Watch for `[TESS] WARNING: Terrain material failed to load` in the new console.

3. **Force `enableShadows=0`** temporarily in the terrain uniform upload: at `gameos_graphics.cpp:2835`, comment out the `if (pp && pp->shadowsEnabled_)` branch and unconditionally execute the `else` branch (`glUniform1i(tl.enableShadows, 0)`). If terrain appears normally lit, the static shadow pre-pass is not running. Fix: ensure the same shadow pre-pass chain as the game's `renderLists()` executes before the first frame, or explicitly call `pp->shadowsEnabled_ = false` in Editor mode until the pre-pass is wired up.

4. **Call `gosPostProcess::beginScene()` before `Environment.UpdateRenderers()`**. The game calls this in `gameosmain.cpp::draw_screen()` before the renderer chain. If the Editor skips it, the MRT FBO is unbound and — on AMD — the draw may be discarded. Follow this with `glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)` and log the result.

5. **Verify terrain texture manager init**. Trace `InitializeGameEngine()` in the game (`code/mission.cpp`) to find where `TerrainTextureManager` / `terrtxm` is initialised and ensure the Editor's equivalent `InitializeGameEngine()` callback replicates that call chain. If terrain goes from black to white/grey after binding a dummy white texture on unit 0, this is the missing step.
