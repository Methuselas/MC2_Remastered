# Remove Bad Default Overhead & Improve Startup Diagnostics — Design

**Date:** 2026-04-22
**Author:** brainstorm with user (rjosephmathews)
**Status:** draft, awaiting implementation

## Motivation

First external user report: "extremely slow loading, basically unplayable, animations really slow." Follow-up: the reporter is on **i9 laptop + RTX 5070** — capable hardware, not weak. That datapoint makes "defaults are too heavy for low-end GPUs" a much less likely primary explanation and elevates the following alternatives:

- Wrong GPU selected on a hybrid-graphics laptop (iGPU instead of discrete).
- First-run / shader compilation stall (no binary cache ships today).
- Laptop power mode / battery-saver interaction.
- Pathological startup or asset-loading path that never fires on the dev machine.
- Some NVIDIA-driver-specific behaviour the codebase has never been validated against.

All development has been on a single config (Ryzen 7 5800X3D + Radeon RX 7900 XTX), so any vendor- or laptop-specific bug has been invisible until now.

This spec covers two complementary things:

1. **Remove avoidable default overhead** from shipping builds (GL debug context, forced vsync, always-on Tracy collection, missing discrete-GPU hint). These are cheap, low-risk, and reduce noise in future reports whether or not they are the root cause of this one.
2. **Make startup diagnostics unmissable** — renderer/adapter identity, drawable/logical sizes, phase timings from process start through first rendered frame — so the next report (or a re-report from this user) tells us which of the competing hypotheses is actually happening.

Larger mitigations (render-scale factor, shader binary cache, quality preset) are deferred to follow-up specs.

Out of scope for this spec:
- HiDPI gating. The user (confirmed) is comfortable assuming 4K monitor owners have GPU headroom.
- Runtime GPU capability detection / auto-preset.
- Loose-file override scan optimization.
- Framerate-coupled animation audit.

## Goals

1. **Force discrete GPU selection on hybrid laptops** (the likely root cause of the first external report).
2. Stop paying avoidable perf costs in shipped builds (GL debug context, forced vsync, always-on Tracy collection).
3. Make any remaining overhead *visible* at startup so the user (or a future triage session) can tell at a glance what mode the build is running in.
4. Expand startup diagnostics so a single screenshot of the console window gives enough info to classify the user's hardware and capability — including which GPU OpenGL actually selected.

## Non-goals

- Changing how fast the game runs on the dev machine.
- Offering a user-facing settings UI. This is all env-var / CMake-flag driven for now.
- Backwards compatibility with existing `MC2_*` env vars — none of these names are in use yet.

## Design

### 1. GL debug context + debug callback become env-gated

**Files:** `GameOS/gameos/gos_render.cpp` (around line 117), `GameOS/gameos/gameosmain.cpp` (around lines 470-474).

Two changes under the same `MC2_GL_DEBUG` env var. Both default off in shipping builds.

**1a. Context flag** — `gos_render.cpp` line 117:
```cpp
const bool gl_debug = (getenv("MC2_GL_DEBUG") != nullptr);
if (gl_debug) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    printf("[GL_DEBUG] MC2_GL_DEBUG=1 -- GL debug context active. This reduces performance.\n");
}
```

**1b. Debug callback** — `gameosmain.cpp` lines 470-474 (`glEnable(GL_DEBUG_OUTPUT)` etc. and `glDebugMessageCallbackARB`). Wrap the entire block in the same `MC2_GL_DEBUG` env check. Without the callback installed, harmless AMD-driver warnings (e.g. `glDetachShader has failed because ... (GL_INVALID_OPERATION)`) no longer spam stdout with 100+ HIGH-severity lines per run. Rationale for keeping them gated rather than silencing unconditionally: some GL debug callback output is real and useful during development — the gate keeps it available on demand.

NDEBUG was rejected because `RelWithDebInfo` (the required build config) defines `NDEBUG`, so users triaging a shipped binary could not re-enable the debug context without a rebuild. Env var preserves that flexibility. A single loud startup line ensures anyone running with the flag knows why their build is slower.

### 2. Vsync becomes env-gated, default OFF

**File:** `GameOS/gameos/gos_render.cpp` (line 21 and lines 267-271).

Current:
```cpp
static bool ENABLE_VSYNC = true;
...
if (ENABLE_VSYNC) { SDL_GL_SetSwapInterval(1); } else { SDL_GL_SetSwapInterval(0); }
```

New:
```cpp
// MC2_VSYNC: "1" forces vsync on, "0" or unset leaves it off (default).
// Off by default so weak GPUs that miss 60Hz don't get snapped to 30/20/15 FPS.
const char* vsync_env = getenv("MC2_VSYNC");
const bool vsync_on = (vsync_env && vsync_env[0] == '1');
SDL_GL_SetSwapInterval(vsync_on ? 1 : 0);
printf("[VSYNC] MC2_VSYNC=%s -- vsync %s.\n",
       vsync_env ? vsync_env : "(unset, default 0)",
       vsync_on ? "ON" : "OFF");
```

Rationale: a GPU that can't sustain 60 FPS has its framerate rounded to the next vsync divisor (30, 20, 15). This exact pattern matches the "animations really slow" report — not because animations are slow, but because the frame presentation rate is.

### 3. Tracy on-demand collection — already in place

**File:** none (verified no change required).

`CMakeLists.txt:59` already defines `-DTRACY_ON_DEMAND`. The Tracy client in every existing shipping build already listens on TCP 8086 and captures nothing until a GUI connects. This was put in place with the previous RAM-leak fix; no engine rebuild is required to profile an existing deployed binary.

Operational consequence: the external user's *current* build already supports remote Tracy profiling. If their console paste is ambiguous, we can ask them to run Tracy GUI against their existing install without shipping anything new.

This spec still prints a `[TRACY]` line at startup as part of the `[MODE]` summary (item 5 below) so the log paste explicitly confirms on-demand mode is active.

### 4. Force discrete GPU on hybrid-graphics laptops

**File:** `GameOS/gameos/gameosmain.cpp` (near the top of the file, outside any namespace, `extern "C"` linkage).

Current: no `NvOptimusEnablement` or `AmdPowerXpressRequestHighPerformance` symbol is exported anywhere in the codebase.

New:
```cpp
// Force discrete GPU selection on hybrid-graphics laptops.
// Without these exports, NVIDIA Optimus and AMD PowerXpress route unknown
// OpenGL executables to the Intel integrated GPU by default, which is
// catastrophic for a terrain/shadow/post-process workload like ours.
// These symbols are looked up by the driver by name; they do not need to
// be referenced from code.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
```

Rationale: first external user (i9 laptop + RTX 5070) reported "extremely slow, basically unplayable." Laptops with hybrid graphics default to the Intel iGPU for any OpenGL app whose name is not on the driver's discrete-GPU whitelist. `mc2.exe` is not on that list. Without these exports, any user with a modern laptop is almost certainly running on integrated graphics regardless of how powerful their discrete GPU is. This is the single highest-value fix in this bundle.

The `[MODE]` summary line (item 5 below) should include the `GL_RENDERER` string prominently so a user looking at their own log can immediately see whether they are on discrete or integrated graphics — e.g. `NVIDIA GeForce RTX 5070 Laptop GPU/PCIe/SSE2` vs `Intel(R) Iris(R) Xe Graphics`.

### 4a. Silence legacy startup breadcrumbs and verbose defaults

**Files:** `GameOS/gameos/gos_render.cpp` (lines 19-20), and whichever files emit `GodRays SKIP:` and `STOPFailed to create texture` (to be located during implementation).

Three edits:

- Flip `VERBOSE_MODES` default from `true` to `false`. Current output: ~80 lines listing every fullscreen video mode of the user's monitor (every refresh rate, every dimension, repeated with red/green/blue masks). This buries real startup info.
- Flip `VERBOSE_RENDER` default from `true` to `false`. Current output: ~20 lines of SDL renderer drivers plus a dump of `GL_EXTENSIONS`. Safe to flip **only after** item 5 below moves the GPU identity lines (vendor / renderer / version) out of this gate; otherwise we lose the single most important triage line.
- Silence or gate the `GodRays SKIP:` per-frame spam and `STOPFailed to create texture` relic — either delete them outright (they look like leftover debug from a disabled feature) or gate behind `MC2_VERBOSE=1` env var for resurrection during triage. Judgement call left to implementation task.

Rationale: on the first external report we'd need to scroll past ~150 lines of irrelevant output before reaching anything useful. Silencing these defaults is the single biggest readability win.

### 4b. Fix `~glsl_program` double-detach bug

**File:** `GameOS/gameos/utils/shader_builder.cpp` (lines 741-747).

`glsl_program::makeProgram2` at line 710 already detaches shaders from the program after link — the correct pattern. The destructor at lines 741-747 then tries to detach them again, which fails with `GL_INVALID_OPERATION` because they are no longer attached. On the current deployed build this shows up as ~100 HIGH-severity lines of `glDetachShader has failed because the operation requires the referenced shader object '80' to be attached to the referenced program object '82'` in the debug callback output.

Fix: delete the redundant detach loop in the destructor. `glDeleteProgram` at line 748 handles cleanup of any shaders still attached automatically, so removing the manual detach does not leak.

The matching `reload()` path at lines 796-810 is correct: it re-attaches at 797 and detaches at 809, so those detaches do match real attach states.

### 5. Startup phase timings

**File:** `GameOS/gameos/gameosmain.cpp`.

The user report says "extremely slow loading." We currently have no breakdown of where startup time is spent, so a 30-second black window could be: window/context creation, FastFile mount, texture/sound asset load, shader compile/link, world init, or something else. Without phase timings, the next report will be just as opaque.

Add lightweight timing prints at these phases, using `SDL_GetPerformanceCounter` / `SDL_GetPerformanceFrequency`:

```
[TIME] t=0.00s  phase=process_start
[TIME] t=0.12s  phase=sdl_init_done
[TIME] t=0.28s  phase=gl_context_ready
[TIME] t=1.45s  phase=fastfile_mount_done  archives=N
[TIME] t=3.90s  phase=core_textures_loaded
[TIME] t=8.72s  phase=shaders_linked       programs=15
[TIME] t=9.10s  phase=first_frame_presented
```

Implementation: one `double phase_elapsed()` helper in `gameosmain.cpp`, and one `printf` call at each phase boundary. Anchor t=0 at the top of `main()`. Keep the prints in the release build — total cost is microseconds, and the signal-to-noise for triage is high.

Phase boundaries to instrument:
- Top of `main()`.
- After `SDL_Init`.
- After `init_render_context` completes.
- After the initial FastFile mount.
- After the first batch of texture uploads.
- After the shader program link pass completes (wherever `makeProgram()` calls are batched).
- At the first `SDL_GL_SwapWindow` of the first real rendered frame.

Exact call sites to be pinned down in the implementation plan.

### 6. Expanded startup diagnostics

**File:** `GameOS/gameos/gos_render.cpp` (after the existing `VERBOSE_RENDER` block around line 285).

Today the build already prints `GL_VENDOR / GL_RENDERER / GL_VERSION` when `VERBOSE_RENDER=true` (which is the default). Move those three lines **out of the `VERBOSE_RENDER` gate** so they print unconditionally — on a laptop report this is the single most valuable line of output, because it tells us whether OpenGL landed on the discrete GPU or the Intel iGPU regardless of every other fix in this spec. Mark them clearly with a `[GPU]` prefix so the user can find them easily when skimming the console.

Then extend the block to also print unconditionally:

- Backbuffer physical size (`SDL_GL_GetDrawableSize`).
- Logical window size (`SDL_GetWindowSize`) — divergence from backbuffer indicates HiDPI scaling.
- `GL_MAX_TEXTURE_SIZE`.
- `GL_MAX_SHADER_STORAGE_BLOCK_SIZE`.
- `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS`.
- GPU memory if the extension is present:
  - On NVIDIA, query `GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX` (from `GL_NVX_gpu_memory_info`).
  - On AMD, query `WGL_GPU_RAM_AMD` via `wglGetGPUIDsAMD` / `wglGetGPUInfoAMD` if available. Skip silently on non-Windows or when the extension is absent.
- A single `[MODE]` summary line listing current effective settings:
  ```
  [MODE] gl_debug=0 vsync=0 tracy=on-demand render_scale=1.00
  ```
  (render_scale fixed at 1.00 for now — placeholder for a future spec.)

The log is printed once, at the end of `init_render_context`, after the GL context is current.

### Error handling

- `getenv` returning `nullptr` is the default path; all branches treat it as "flag unset".
- Missing `GL_NVX_gpu_memory_info` / AMD extension: print `vram=unknown`. Do not fail.
- Existing `VERBOSE_RENDER=true` remains the default; the new diagnostics print whether it is on or off, so a terse user who captures only a screenshot still shows the new data.

### Testing

Manual test matrix on the dev machine:

1. Default run (no env vars set): confirm `[VSYNC] ... OFF`, `[TRACY] on-demand`, no `[GL_DEBUG]` line, framerate uncapped, startup diagnostics printed once.
2. `MC2_GL_DEBUG=1`: confirm `[GL_DEBUG]` line appears and GL debug messages flow (existing debug callback).
3. `MC2_VSYNC=1`: confirm `[VSYNC] ... ON` and framerate caps at monitor refresh.
4. Attach Tracy GUI to the running build: confirm capture begins, flame chart populates, disconnect leaves the game running without a stall.
5. Missing `GL_NVX_gpu_memory_info` path can't be tested on the dev AMD card; verify the code compiles and behaves by temporarily forcing the "extension missing" branch.

Laptop validation (this afternoon):
- Capture the full startup stdout for later reference.
- Compare framerate before/after the vsync default flip. Expect weak GPU to show a visible FPS improvement if it was previously floor-rounded.

### Deployment

Standard `/mc2-build-deploy`. No shader changes, so only `mc2.exe` redeploys. Verify the stdout console on first launch shows the new `[MODE]` line.

## Risks

- **User who relied on forced vsync for tear-free play** now sees tearing. Mitigation: the `[VSYNC]` startup line explains how to set it back. Acceptable tradeoff; the safer default protects far more users than it inconveniences.
- **TCP port 8086 listening on all builds** could be flagged by corporate AV / firewalls. Unchanged from today (Tracy already listens), so not a regression. If this becomes a complaint, a follow-up change can add a `MC2_TRACY=off` kill switch that compiles the whole thing out.
- **New diagnostics rely on a valid GL context being current.** Placement at the end of `init_render_context` ensures that. A context-creation failure would have aborted earlier anyway.

## Follow-up specs (deferred, not part of this one)

- Render-scale factor (draw internal FBO smaller, upscale to window).
- Program binary cache to kill first-launch NVIDIA shader compile stall.
- Quality preset env var / ini (shadow map size, POM, FXAA, bloom, cloud shadows, grass).
- Loose-file override probe short-circuit when `data/` overrides are empty.
- Runtime GPU capability check writing a first-run config file.
