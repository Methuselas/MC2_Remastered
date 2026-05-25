# Safer Release Defaults & Startup Diagnostics — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove avoidable performance overhead in shipping builds and add unmissable startup diagnostics so future external reports (starting with an i9 + RTX 5070 laptop reporting "extremely slow loading") can be triaged from a single console capture.

**Architecture:** Seven focused edits across three files (`gameosmain.cpp`, `gos_render.cpp`, `utils/shader_builder.cpp`). Config-shape changes (discrete-GPU exports, env-gated GL debug context + callback, env-gated vsync), diagnostics expansions (unconditional `[GPU]`/GL-capability logging, `[TIME]` phase markers), console cleanup (silence verbose defaults + legacy breadcrumbs), and one bug fix (`~glsl_program` double-detach). `TRACY_ON_DEMAND` is already in `CMakeLists.txt:59` — no build-system edit needed. All C++ only — no shader, asset, or data-file changes, so the release delta is a single `mc2.exe`.

**Tech Stack:** C++ / SDL2 / OpenGL 4.3 core / Tracy profiler / CMake / MSYS2 bash (for build/deploy skills).

**Verification pattern:** This codebase has no C++ unit-test framework. Each task is verified by building, deploying, launching the game, and inspecting stdout. The existing `/mc2-build-deploy` skill handles build+deploy; launch is manual (the task runner must visually confirm expected log lines appear or do not appear, per the test criteria in each task).

**Reference spec:** `docs/superpowers/specs/2026-04-22-safer-release-defaults-design.md`.

---

## File Structure

Files modified:

- `GameOS/gameos/gameosmain.cpp` — discrete-GPU symbol exports (top of file, `extern "C"`) and `[TIME]` phase markers inside `main()`.
- `GameOS/gameos/gos_render.cpp` — env-gated GL debug flag, env-gated vsync, unconditional `[GPU]` logging, expanded GL capability logging, `[MODE]` summary line.
- `CMakeLists.txt` — add `-DTRACY_ON_DEMAND`.

No new files. No files deleted. No test files (this codebase has none for the engine).

---

## Task 1: Force discrete GPU on hybrid-graphics laptops

**Why first:** Highest expected impact for the known external report. A laptop with an RTX 5070 currently defaults to the Intel iGPU because `mc2.exe` is not in NVIDIA's discrete-GPU application profile. Exporting two magic symbols flips that globally for every NVIDIA and AMD hybrid-graphics driver.

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (add block near top of file, after existing includes, outside any namespace)

- [ ] **Step 1: Add the discrete-GPU export block**

Open `GameOS/gameos/gameosmain.cpp`. Find the existing include block at the top of the file (the first 30-40 lines — should end somewhere between line 30 and line 60 before any function definitions). Immediately after the last `#include`, add:

```cpp
// Force discrete GPU selection on hybrid-graphics laptops (NVIDIA Optimus,
// AMD PowerXpress). Without these exports, an unknown OpenGL executable is
// routed to the Intel integrated GPU by default, which is catastrophic for
// our terrain/shadow/post-process workload. These symbols are looked up by
// the driver by exported name; they do not need to be referenced from code.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
```

- [ ] **Step 2: Build**

Run the `/mc2-build` skill (or `cmake --build build64 --config RelWithDebInfo` directly). Expected: build completes without errors. The symbols are `dllexport`ed from the exe; they do not need a `.lib` anywhere else.

- [ ] **Step 3: Verify the symbols are in the binary**

Run (from the worktree root, MSYS2 bash):

```bash
strings build64/RelWithDebInfo/mc2.exe | grep -E "NvOptimusEnablement|AmdPowerXpressRequestHighPerformance"
```

Expected output: both symbol names appear.

- [ ] **Step 4: Deploy and sanity launch**

Run `/mc2-deploy`. Launch the deployed `mc2.exe`. Game should start normally (no behavioural change visible on a desktop-only dev machine — the symbols are no-ops when there is no hybrid-graphics driver to read them).

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat: export NvOptimus/PowerXpress hints to force discrete GPU on laptops

External user report (i9 + RTX 5070 laptop) showed extremely slow loading,
which is consistent with the OpenGL driver routing an unknown exe to the
Intel iGPU by default. Exporting these symbols is the standard fix."
```

---

## Task 2: Env-gate GL debug context AND debug callback, default off

**Why:** `SDL_GL_CONTEXT_DEBUG_FLAG` is currently forced on in every shipping build, and the debug callback that receives driver messages is also installed unconditionally. Debug contexts cost perf on all drivers (especially NVIDIA), and the AMD driver in particular routes harmless "shader already detached" warnings through the callback at HIGH severity — spamming 100+ lines of `glDetachShader has failed because ... (GL_INVALID_OPERATION)` into stdout on every run. Both the context flag and the callback install belong behind the same env var so shipping users see neither the perf cost nor the noise, while developers can flip the one switch to get the full debug stream back.

**Files:**
- Modify: `GameOS/gameos/gos_render.cpp` (around line 117) — context flag
- Modify: `GameOS/gameos/gameosmain.cpp` (lines 470-474) — debug callback install

- [ ] **Step 1: Replace the unconditional context-flag set with an env-gated version**

Open `GameOS/gameos/gos_render.cpp`. Locate line 117 (the existing `SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);` call). Replace that one line and the comment on line 118 with:

```cpp
    // MC2_GL_DEBUG=1 enables the OpenGL debug context AND the debug-message
    // callback (installed in gameosmain.cpp). Debug contexts run driver-side
    // validation on every GL call and can cost 10-30% perf, especially on
    // NVIDIA; the callback also floods stdout with harmless AMD-driver
    // warnings in our workload. Off by default in shipped builds;
    // env-gated rather than NDEBUG-gated so it can be flipped on a
    // deployed binary without rebuilding.
    const bool gl_debug = (getenv("MC2_GL_DEBUG") != nullptr);
    if (gl_debug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
        printf("[GL_DEBUG] MC2_GL_DEBUG=1 -- GL debug context active. This reduces performance.\n");
    }
```

Add `#include <cstdlib>` near the top of `gos_render.cpp` if it is not already included (for `getenv`). If `<stdlib.h>` is already present via another header that is fine.

- [ ] **Step 2: Wrap the debug callback install in the same env check**

Open `GameOS/gameos/gameosmain.cpp`. Locate lines 470-474:

```cpp
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    //glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 76, 1, "My debug group");
    glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
    glDebugMessageCallbackARB((GLDEBUGPROC)&OpenGLDebugLog, NULL);
```

Replace with:

```cpp
    // Install GL debug callback only when MC2_GL_DEBUG is set. In shipping
    // builds this keeps stdout free of harmless driver warnings (esp. the
    // AMD ~glsl_program double-detach chatter) and saves the sync-debug
    // overhead. Paired with the context-flag gate in gos_render.cpp.
    if (getenv("MC2_GL_DEBUG") != nullptr) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        glDebugMessageCallbackARB((GLDEBUGPROC)&OpenGLDebugLog, NULL);
    }
```

`<cstdlib>` / `<stdlib.h>` is already included in `gameosmain.cpp` transitively; confirm via a quick grep and add if missing.

- [ ] **Step 3: Build**

`/mc2-build`. Expected: clean build.

- [ ] **Step 4: Deploy and launch with the flag OFF (default)**

`/mc2-deploy`. Launch `mc2.exe`. Inspect the console window.

Expected: no `[GL_DEBUG]` line. No `Type: DEBUG_TYPE_ERROR ...` / `Message : glDetachShader has failed ...` lines. Game runs normally.

- [ ] **Step 5: Launch with the flag ON**

From cmd.exe in the deploy dir:

```
set MC2_GL_DEBUG=1
mc2.exe
```

Expected: `[GL_DEBUG] ...` startup line appears. Debug callback output (including the double-detach warnings, until Task 4b fixes them) streams as before.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_render.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat: gate GL debug context + callback behind MC2_GL_DEBUG

Both the SDL context flag and the glDebugMessageCallback install were
forced on in every shipping build. Debug context costs perf; the
callback spams stdout with harmless AMD driver warnings (e.g. the
~glsl_program double-detach chatter). Env-gated rather than NDEBUG
so a shipped binary can still opt in for triage."
```

---

## Task 3: Env-gate vsync, default OFF

**Why:** `ENABLE_VSYNC = true` forces `SDL_GL_SetSwapInterval(1)` in every build. A GPU that misses 60 Hz gets rounded to 30/20/15 FPS, which visually looks like "really slow animations" even when the GPU could otherwise sustain 40-50 FPS uncapped. Default off lets weak hardware run at its natural rate; env var restores vsync for users who want tear-free play.

**Files:**
- Modify: `GameOS/gameos/gos_render.cpp` (line 21, and lines 267-271)

- [ ] **Step 1: Change the static default and the swap-interval logic**

Open `GameOS/gameos/gos_render.cpp`.

At line 21, change:
```cpp
static bool ENABLE_VSYNC = true;
```
to:
```cpp
static bool ENABLE_VSYNC = false;  // default off; overridden by MC2_VSYNC env var in init_render_context
```

At lines 267-271 (the existing `if (ENABLE_VSYNC) { ... } else { ... }` block inside `init_render_context`), replace the block with:
```cpp
    // MC2_VSYNC: "1" forces vsync on, "0" or unset leaves it off.
    // Off by default so a GPU that misses 60 Hz is not rounded down
    // to 30/20/15 FPS.
    const char* vsync_env = getenv("MC2_VSYNC");
    const bool vsync_on = (vsync_env && vsync_env[0] == '1');
    SDL_GL_SetSwapInterval(vsync_on ? 1 : 0);
    printf("[VSYNC] MC2_VSYNC=%s -- vsync %s.\n",
           vsync_env ? vsync_env : "(unset, default 0)",
           vsync_on ? "ON" : "OFF");
```

The static `ENABLE_VSYNC` variable is now effectively unused, but leave it in place — it is read by other translation units via the `set_verbose` sibling function nearby and removing it would be out of scope for this change.

- [ ] **Step 2: Build**

`/mc2-build`. Expected: clean build.

- [ ] **Step 3: Deploy and launch with default (vsync off)**

`/mc2-deploy`. Launch `mc2.exe`. Inspect console.

Expected: `[VSYNC] MC2_VSYNC=(unset, default 0) -- vsync OFF.` appears near top of output. In-game FPS may be uncapped; on the dev machine (RX 7900 XTX) this will show very high framerates and possibly visible tearing. Both are expected.

- [ ] **Step 4: Launch with vsync forced on**

From cmd.exe:
```
set MC2_VSYNC=1
mc2.exe
```

Expected: `[VSYNC] MC2_VSYNC=1 -- vsync ON.` appears. Framerate caps at monitor refresh. No tearing.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_render.cpp
git commit -m "feat: gate vsync behind MC2_VSYNC env var, default OFF

Forced vsync rounds missed frames to 30/20/15 FPS on weak GPUs, which
reads as 'really slow animations' in user reports. Default off lets
weak hardware run at its natural rate; MC2_VSYNC=1 restores the old
behaviour."
```

---

## Task 4: Unconditional `[GPU]` identity + `[MODE]` summary + GL capability logging

**Why:** Today, `glGetString(GL_VENDOR / RENDERER / VERSION)` is only printed when `VERBOSE_RENDER == true`. While that default is currently `true`, the line is buried deep in a large conditional block, printed without a distinctive prefix, and easy to miss in a log paste. On a laptop report where the single most important question is "which GPU did OpenGL actually pick?", this information needs a consistent, grep-friendly prefix and must print in every build. This task also adds GL capability limits, drawable/logical size, and a single-line `[MODE]` summary of the effective env-flag state from tasks 2-3. `TRACY_ON_DEMAND` is already set in CMakeLists.txt (no change needed), but we still advertise it in the startup log for triage clarity.

**Files:**
- Modify: `GameOS/gameos/gos_render.cpp` (inside `init_render_context`, around line 285 — after the existing `VERBOSE_RENDER` block)

- [ ] **Step 1: Move the GPU identity prints out of `VERBOSE_RENDER`**

Open `GameOS/gameos/gos_render.cpp`. Locate the block at lines 273-284 (the `if(VERBOSE_RENDER) { ... printf("Vendor ..."); printf("Renderer ..."); printf("Version ..."); ... }`). Copy the three `glGetString` print calls out of the conditional and place them immediately before the `if(VERBOSE_RENDER)` block, with a distinctive `[GPU]` prefix:

```cpp
    // Print GPU identity unconditionally with a distinctive prefix so it is
    // impossible to miss when a user pastes their console log for triage.
    // On hybrid-graphics laptops this line is the single most valuable
    // diagnostic: it says which GPU OpenGL actually selected.
    printf("[GPU] Vendor   : %s\n", glGetString(GL_VENDOR));
    printf("[GPU] Renderer : %s\n", glGetString(GL_RENDERER));
    printf("[GPU] Version  : %s\n", glGetString(GL_VERSION));
```

Leave the existing `if(VERBOSE_RENDER)` block intact — the duplicated prints inside it are harmless (`VERBOSE_RENDER` defaults to `true` but can be overridden to `false` by `set_verbose`, in which case we still want the `[GPU]` lines). If you prefer, delete the three duplicate `glGetString` prints inside the `VERBOSE_RENDER` block to avoid the repetition; either is acceptable.

- [ ] **Step 2: Add GL capability limits and drawable/logical sizes**

Immediately after the three `[GPU]` lines from Step 1, add:

```cpp
    // GL capability limits — useful to rule out SSBO / texture-size / unit
    // ceilings when a user reports rendering issues on unusual hardware.
    GLint maxTex = 0, maxSSBO = 0, maxTexUnits = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSSBO);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTexUnits);
    printf("[GL] max_texture_size=%d max_ssbo_block=%d max_combined_texture_units=%d\n",
           maxTex, maxSSBO, maxTexUnits);

    // Drawable (physical, post-HiDPI) vs logical (window coords) size. A
    // divergence indicates the backbuffer is larger than the window, which
    // multiplies fragment cost. rw->window_ is the SDL_Window* owned by
    // the caller; we have it in scope here via the rw pointer walked from
    // the RenderContext earlier in this function.
    {
        int draw_w = 0, draw_h = 0, logical_w = 0, logical_h = 0;
        SDL_GL_GetDrawableSize(rw->window_, &draw_w, &draw_h);
        SDL_GetWindowSize(rw->window_, &logical_w, &logical_h);
        printf("[WINDOW] drawable=%dx%d logical=%dx%d%s\n",
               draw_w, draw_h, logical_w, logical_h,
               (draw_w != logical_w || draw_h != logical_h) ? " (HiDPI)" : "");
    }
```

**Scope note on `rw`:** this block lives inside `init_render_context`, where `rw` is already in scope (it is walked from `render_window` at the top of the function around line 253). If the name differs in the actual code, use whatever variable points at the `RenderWindow`/`SDL_Window*` at this point.

- [ ] **Step 3: Add the `[MODE]` summary line**

After the `[WINDOW]` block, add a single summary line consolidating the effective state of all env-flag knobs from tasks 2-4:

```cpp
    // Single-line summary of effective runtime mode. Anchor for log pastes:
    // grep [MODE] and you see every toggle state at a glance.
    printf("[MODE] gl_debug=%d vsync=%d tracy=on-demand\n",
           gl_debug ? 1 : 0,
           vsync_on ? 1 : 0);
    printf("[TRACY] on-demand mode -- profiler listening on TCP 8086, no capture until a GUI attaches.\n");
```

**Scope note on `gl_debug` and `vsync_on`:** both are declared earlier in `init_render_context` by Tasks 2 and 3. If this task is executed before them (it should not be — tasks are ordered), the plan order must be followed or the references will not compile.

- [ ] **Step 4: Build**

`/mc2-build`. Expected: clean build. If the compiler complains about `SDL_GL_GetDrawableSize` not being declared, verify `#include <SDL.h>` (or the relevant SDL video header) is present in this file — it should already be, since `SDL_GL_SetAttribute` is used higher up in the same file.

- [ ] **Step 5: Deploy and launch, capture full stdout**

`/mc2-deploy`. From cmd.exe:
```
cd <deploy-dir>
mc2.exe > mc2_startup_log.txt 2>&1
```

Let the main menu appear, then quit. Open `mc2_startup_log.txt` and verify the following lines appear, in roughly this order, near the top:

- `[VSYNC] MC2_VSYNC=(unset, default 0) -- vsync OFF.`
- `[GPU] Vendor   : ATI Technologies Inc.` (or whatever the dev machine reports)
- `[GPU] Renderer : AMD Radeon RX 7900 XTX` (or similar)
- `[GPU] Version  : 4.6.xxxxx ...`
- `[GL] max_texture_size=... max_ssbo_block=... max_combined_texture_units=...`
- `[WINDOW] drawable=WxH logical=WxH` (should be equal on the dev machine, no `(HiDPI)` tag)
- `[MODE] gl_debug=0 vsync=0 tracy=on-demand`
- `[TRACY] on-demand mode ...`

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_render.cpp
git commit -m "feat: unconditional [GPU] identity log + [MODE] triage summary

Moves glGetString output out of VERBOSE_RENDER gate so the most
important triage line (which GPU OpenGL selected) always prints
with a grep-friendly prefix. Adds GL capability limits, drawable
vs logical window size, and a single [MODE] line summarising the
effective state of MC2_GL_DEBUG, MC2_VSYNC, and Tracy."
```

---

## Task 4a: Silence legacy startup breadcrumbs and verbose defaults

**Why:** The current baseline deployed build dumps ~150 lines of irrelevant output before anything useful appears — every display mode of the monitor, every SDL renderer driver, a `GL_EXTENSIONS` dump, repeated `GodRays SKIP:` lines per frame, and a `STOPFailed to create texture` relic (note the missing space, classic leftover debug). For an external user pasting a log for triage, this is pure noise. This task flips the two verbose defaults off and silences the two legacy breadcrumbs.

**Dependency:** must land **after** Task 4, because Task 4 moves the GPU identity (`glGetString` vendor/renderer/version) out of the `VERBOSE_RENDER` gate. Flipping `VERBOSE_RENDER` off before Task 4 would lose the single most important triage line.

**Files:**
- Modify: `GameOS/gameos/gos_render.cpp` (lines 19-20)
- Modify: whichever file(s) contain `GodRays SKIP:` and `STOPFailed to create texture` — grep to locate during implementation.

- [ ] **Step 1: Flip the verbose defaults**

Open `GameOS/gameos/gos_render.cpp`. Locate lines 19-20:

```cpp
static bool VERBOSE_RENDER = true;
static bool VERBOSE_MODES = true;
```

Change both to `false`:

```cpp
static bool VERBOSE_RENDER = false;
static bool VERBOSE_MODES = false;
```

(`ENABLE_VSYNC` at line 21 was already handled in Task 3 — do not touch it again.)

- [ ] **Step 2: Locate and silence `GodRays SKIP:`**

From the worktree root:

```
grep -rn "GodRays SKIP" --include="*.cpp" --include="*.h" .
```

Expected hit: one or two `printf` / `log_` calls. The feature is currently disabled (per memory: "God rays: Infrastructure built, DISABLED"). Delete the print calls outright — they are per-frame breadcrumbs from a disabled subsystem. Do not gate behind an env var; if god rays are ever re-enabled, the author of that change can add new instrumentation.

- [ ] **Step 3: Locate and silence `STOPFailed to create texture`**

```
grep -rn "STOPFailed" --include="*.cpp" --include="*.h" .
```

Expected hit: a single `printf` call with a missing space (copy-paste tag left by a previous author). Delete the print outright — a real texture creation failure already produces a more informative error via the `log_error` / `SPEW` channel elsewhere.

If grep surfaces anything unexpected (e.g. the string appears in unrelated code) report back as `DONE_WITH_CONCERNS` before committing.

- [ ] **Step 4: Build**

`/mc2-build`. Expected: clean build.

- [ ] **Step 5: Deploy and verify silence**

`/mc2-deploy`. Launch `mc2.exe` from cmd.exe with stdout redirected:

```
mc2.exe > mc2_log.txt 2>&1
```

Let the main menu appear, then quit. `mc2_log.txt` should:
- No longer contain the `Number of displays:` / `Display 0:` / `Mode N:` enumeration.
- No longer contain the `Built-in render drivers:` block.
- No longer contain a `GL_EXTENSIONS` dump.
- No longer contain any `GodRays SKIP:` lines.
- No longer contain any `STOPFailed to create texture` line.
- STILL contain the Task 4 `[GPU]`, `[GL]`, `[WINDOW]`, `[MODE]`, `[TRACY]` lines and Task 3 `[VSYNC]` line.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_render.cpp <other file(s) from grep>
git commit -m "chore: silence verbose startup defaults and legacy breadcrumbs

Flip VERBOSE_RENDER and VERBOSE_MODES defaults to false -- the SDL
mode/driver enumeration and GL_EXTENSIONS dump buried every useful
line of output. Delete per-frame GodRays SKIP and STOPFailed
breadcrumbs from a disabled feature / copy-paste relic.

Task 4's unconditional [GPU] prints make VERBOSE_RENDER=false safe
without losing triage signal."
```

---

## Task 4b: Fix `~glsl_program` double-detach bug

**Why:** The GL debug callback output captured during noise-investigation contained ~100 lines of:

```
Type: DEBUG_TYPE_ERROR; Source: DEBUG_SOURCE_API; ID: 1204; Severity : DEBUG_SEVERITY_HIGH
Message : glDetachShader has failed because the operation requires the referenced shader object '80' to be attached to the referenced program object '82' (GL_INVALID_OPERATION)
```

Root cause is a real bug: `glsl_program::makeProgram2` at `shader_builder.cpp:710` correctly detaches shaders from the program after link (standard GL pattern). The `~glsl_program()` destructor at lines 741-747 then tries to detach them **again**. They are no longer attached, so every detach returns `GL_INVALID_OPERATION`. The matching `reload()` path at lines 796-810 is already correct (it re-attaches at line 797 and detaches at 809, so those detaches match real attach states — do not touch it).

`glDeleteProgram` at line 748 handles cleanup of any still-attached shaders automatically, so removing the manual detach loop does not leak.

**Files:**
- Modify: `GameOS/gameos/utils/shader_builder.cpp` (lines 737-750)

- [ ] **Step 1: Remove the redundant detach loop**

Open `GameOS/gameos/utils/shader_builder.cpp`. Locate the destructor:

```cpp
glsl_program::~glsl_program()
{
    if(shp_)
    {
        glsl_shader* pipeline[] = { vsh_, hsh_, dsh_, gsh_, fsh_ };
        for(uint32_t i=0; i< sizeof(pipeline)/sizeof(pipeline[0]); ++i)
        {
            if(!pipeline[i]) continue;

            glDetachShader(shp_, pipeline[i]->shader_);
        }
        glDeleteProgram(shp_);
    }
}
```

Replace with:

```cpp
glsl_program::~glsl_program()
{
    if(shp_)
    {
        // Shaders were detached after link in makeProgram2 (line 710) /
        // reload (line 809). Re-detaching here returns GL_INVALID_OPERATION
        // and spams the debug callback. glDeleteProgram handles cleanup of
        // any shaders still attached automatically.
        glDeleteProgram(shp_);
    }
}
```

- [ ] **Step 2: Build**

`/mc2-build`. Expected: clean build.

- [ ] **Step 3: Verify (requires Task 2 to have landed first)**

Deploy and launch with `MC2_GL_DEBUG=1` set:

```
set MC2_GL_DEBUG=1
mc2.exe > mc2_log.txt 2>&1
```

Quit normally (via the main menu Quit option, not Alt-F4 — the destructor has to run). `mc2_log.txt` should contain zero `glDetachShader has failed` lines on shutdown. Before the fix, ~100 such lines appeared at program teardown as each cached `glsl_program` was destroyed.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/utils/shader_builder.cpp
git commit -m "fix: remove redundant shader detach in ~glsl_program

makeProgram2 / reload already detach shaders after link (standard GL
pattern). The destructor's additional detach loop was racing already-
detached state, producing ~100 HIGH-severity GL_INVALID_OPERATION
lines in the debug callback on every shutdown. glDeleteProgram
handles any still-attached shaders automatically."
```

---

## Task 5: Startup phase timings

**Why:** "Extremely slow loading" is currently un-attributable. Could be window/context creation, FastFile mount, initial texture upload, shader link, or engine init. Six cheap `printf` markers from process start through first presented frame turn "slow loading" into "phase X took Y seconds" on any future report.

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (inside `main()` and inside the main frame loop)

- [ ] **Step 1: Add the timing helper**

Open `GameOS/gameos/gameosmain.cpp`. Near the top of the file (below the discrete-GPU export block from Task 1 and any existing static/anonymous-namespace variables, but above `main()`), add:

```cpp
namespace {
    // Startup phase timing. Anchor at the top of main(). Cheap printfs —
    // total cost is microseconds, but the signal for triage is high.
    static Uint64 g_startup_t0 = 0;
    static double startup_elapsed() {
        const Uint64 now = SDL_GetPerformanceCounter();
        const double freq = (double)SDL_GetPerformanceFrequency();
        return (double)(now - g_startup_t0) / freq;
    }
    static void startup_phase(const char* name) {
        printf("[TIME] t=%6.2fs  phase=%s\n", startup_elapsed(), name);
    }
}
```

If `SDL.h` is not already included in `gameosmain.cpp` (it should be, indirectly via other headers — search for `#include` at the top to confirm), add it.

- [ ] **Step 2: Anchor t=0 at the top of main**

Inside `main()`, immediately after the two `setvbuf` calls (currently at lines 422-423):

```cpp
    g_startup_t0 = SDL_GetPerformanceCounter();
    startup_phase("process_start");
```

- [ ] **Step 3: Mark window creation**

After the successful window creation at line 455 (`if(!win) return 1;`), add:
```cpp
    startup_phase("window_created");
```

- [ ] **Step 4: Mark GL context ready**

After the successful GL version check at line 498 (`if(glsl_maj < 3 ...) return 1;`, or the line after `SPEW(("GRAPHICS", "Using %s shader version\n", version));` at line 502), add:
```cpp
    startup_phase("gl_context_ready");
```

- [ ] **Step 5: Mark renderer created**

After `gos_CreateRenderer(ctx, win, w, h);` (line 504), add:
```cpp
    startup_phase("renderer_created");
```

- [ ] **Step 6: Mark engine init done**

After `Environment.InitializeGameEngine();` (line 510), add:
```cpp
    startup_phase("engine_init_done");
```

- [ ] **Step 7: Mark first presented frame inside the main loop**

Inside the existing main loop (around line 559, inside the `ZoneScopedN("SwapWindow")` block, immediately after `graphics::swap_window(win);`), add a one-shot marker:

```cpp
            static bool s_first_frame_logged = false;
            if (!s_first_frame_logged) {
                s_first_frame_logged = true;
                startup_phase("first_frame_presented");
            }
```

- [ ] **Step 8: Build**

`/mc2-build`. Expected: clean build. If the compiler complains about `Uint64` not being declared, verify `#include <SDL.h>` at the top of `gameosmain.cpp`.

- [ ] **Step 9: Deploy, launch, and verify phase markers**

`/mc2-deploy`. Run `mc2.exe > mc2_startup_log.txt 2>&1` from cmd.exe. Let the main menu appear, then quit.

Expected lines in the log, monotonically increasing timestamps:
```
[TIME] t=  0.00s  phase=process_start
[TIME] t=  ....s  phase=window_created
[TIME] t=  ....s  phase=gl_context_ready
[TIME] t=  ....s  phase=renderer_created
[TIME] t=  ....s  phase=engine_init_done
[TIME] t=  ....s  phase=first_frame_presented
```

On the dev machine the whole sequence should complete in under a few seconds. The absolute numbers are not the thing we care about on the dev box — we care that each marker prints exactly once and in order.

- [ ] **Step 10: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat: add [TIME] startup phase markers

Six cheap printfs from process start through first presented frame.
Turns 'extremely slow loading' reports into 'phase X took Y seconds'
without requiring a profiler attach."
```

---

## Task 6: Full-pipeline verification + release packaging

**Why:** Tasks 1-5 each verified in isolation. This task confirms they interact correctly, the log is clean end to end, and packages a new `mc2-remastered-engine.zip` ready for the external user.

**Files:**
- No code changes. This is a verification + packaging task only.

- [ ] **Step 1: Full clean build**

```bash
rm -rf build64
cmake -S . -B build64 -G "Visual Studio 17 2022" -A x64 -T host=x64
cmake --build build64 --config RelWithDebInfo --target mc2
```

Expected: clean build from scratch.

- [ ] **Step 2: Deploy**

`/mc2-deploy` (or manual: `cp -f build64/RelWithDebInfo/mc2.exe <deploy-dir>/mc2.exe`, then `diff -q`).

- [ ] **Step 3: Full startup log capture**

From cmd.exe in deploy dir:
```
mc2.exe > mc2_final_log.txt 2>&1
```

Let the main menu fully load. Start a skirmish or mission load, play ~30 seconds, quit normally.

- [ ] **Step 4: Review the log**

Open `mc2_final_log.txt`. Confirm the following:

- `[TIME] t=  0.00s  phase=process_start` appears first among the new logs.
- All six `[TIME]` markers appear in order, each exactly once.
- `[GPU] Vendor`, `[GPU] Renderer`, `[GPU] Version` each appear with content.
- `[GL] max_texture_size=...` appears with plausible values (dev RX 7900 XTX: max_texture_size ≥ 16384).
- `[WINDOW] drawable=... logical=...` appears. On the dev machine these should match.
- `[MODE] gl_debug=0 vsync=0 tracy=on-demand` appears.
- `[TRACY] on-demand mode ...` appears.
- `[VSYNC] MC2_VSYNC=(unset, default 0) -- vsync OFF.` appears.
- No `[GL_DEBUG]` line appears (default-off path).

If any of the above is missing or out of order, return to the relevant task and fix the call-site ordering. Do not proceed to packaging.

- [ ] **Step 5: Package the release zip**

Package the single-file engine update:

```bash
cd <deploy-dir>
zip -j release_assets/mc2-remastered-engine.zip mc2.exe
```

(Adjust paths for the actual `release_assets/` location in the source tree — there is already a `release_assets/` folder at the repo root; use the same convention as the existing `mc2-remastered-engine.zip` in `release_assets/` at the source root. If unsure, run `ls release_assets/` first and match the existing file set.)

- [ ] **Step 6: Commit the packaged release**

```bash
git add release_assets/mc2-remastered-engine.zip
git commit -m "release: engine build with hybrid-GPU fix + startup diagnostics

- Forces discrete GPU on NVIDIA Optimus / AMD PowerXpress laptops
- Removes forced GL debug context from shipping builds
- Default vsync off (MC2_VSYNC=1 restores)
- Tracy switched to on-demand collection
- Unconditional [GPU]/[MODE]/[TIME] startup diagnostics

First external user (i9 + RTX 5070 laptop) reported extremely slow
loading; hybrid-GPU hint is the most likely fix, new diagnostics
let the next report self-classify on a single console paste."
```

- [ ] **Step 7: Write user-facing instructions**

Create a short message to send the external user. Save as `release_assets/USER_INSTRUCTIONS.txt` (do not commit yet — user will review):

```
New version attached. Please:

1. Close the current game.
2. Replace mc2.exe in your install folder with the one in this zip.
3. Launch normally (double-click mc2.exe).
4. When the main menu appears, Alt-Tab to the small black console window
   that opened alongside the game.
5. Right-click the console title bar -> Edit -> Select All, then
   Edit -> Copy.
6. Paste into a reply message or a pastebin (e.g. gist.github.com) and
   send the link back.

The first 20 lines or so of that console log are what we need. They
contain which GPU the game selected and how long each startup phase
took.
```

---

## Self-Review Results

**Spec coverage:**
- Spec §1 discrete GPU exports → Task 1 ✓
- Spec §2 GL debug env-gate → Task 2 ✓
- Spec §3 vsync env-gate default off → Task 3 ✓
- Spec §4 Tracy on-demand → already in CMakeLists.txt:59, no task needed; `[TRACY]` log line included in Task 4 ✓
- Spec §4a Silence legacy breadcrumbs + verbose defaults → Task 4a ✓
- Spec §4b `~glsl_program` double-detach fix → Task 4b ✓
- Spec §5 Phase timings → Task 5 ✓
- Spec §6 expanded diagnostics + [MODE] summary → Task 4 ✓
- Spec Testing checklist → Task 6 ✓

All six spec items mapped to tasks. No gaps.

**Placeholder scan:** No TBD / TODO / "similar to Task N" / "add error handling" patterns present. Phase boundary call sites were pinned to concrete line numbers from the live source read.

**Type / naming consistency:**
- `gl_debug` (Task 2) referenced in Task 5 `[MODE]` line ✓
- `vsync_on` (Task 3) referenced in Task 5 `[MODE]` line ✓
- `startup_phase()` / `g_startup_t0` used consistently in Task 6 ✓
- Env var names `MC2_GL_DEBUG`, `MC2_VSYNC` consistent across spec + plan ✓
- Log prefixes `[GPU]`, `[GL]`, `[WINDOW]`, `[MODE]`, `[TRACY]`, `[VSYNC]`, `[GL_DEBUG]`, `[TIME]` are all distinct and grep-friendly.

**Scope:** Seven small tasks, single-file edits where possible, each independently commit-able. No subsystem coupling that would force a multi-task rollback.
