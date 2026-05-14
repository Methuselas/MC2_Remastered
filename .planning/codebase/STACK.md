# Technology Stack

**Analysis Date:** 2026-05-14

> Scope: this document describes the stack as it stands on the active
> worktree branch `claude/nifty-mendeleev`
> (`.claude/worktrees/nifty-mendeleev/`). The root repo is the upstream
> `terrain-pbr-mod` legacy state; differences are called out where they
> matter. See `ARCHITECTURE.md` for the layered design and
> `STRUCTURE.md` for directory layout.

## Languages

**Primary:**
- C++ (C++0x / C++11 baseline) — the entire engine. The non-MSVC arm
  of `CMakeLists.txt:31` sets `-std=c++0x -ggdb`. MSVC inherits its
  default modern standard (no explicit `/std:` flag), so C++14+
  features compile in practice but the project is intentionally
  written to C++0x/C++11 conventions for cross-compiler safety.
- C — Tracy's vendored `libbacktrace` (`3rdparty/tracy/libbacktrace/`)
  plus a few `.h` shims and GLEW. Engine itself is essentially zero-C.
- GLSL 4.30 (core profile, `#version 430\n` injected as a string
  prefix by `makeProgram()` — shader files themselves carry NO
  `#version` directive; see `CLAUDE.md` "Critical Rules"). The root
  branch is on GLSL 4.20 with a `#version 420\n` prefix
  (`GameOS/gameos/gameos_graphics.cpp:170` on root).
- ABL — a domain-specific scripting language compiled and executed by
  a custom VM inside `mclib/` (`mclib/abl.h`,
  `mclib/ablscan.cpp/ablparse.h/ablexec.cpp/ablrtn.cpp/...`). Originated
  with the MS source release; AI brains for mechs/missions are written
  in `.abi`/`.abl` files consumed by `code/ablmc2.cpp`.
- Python 3 — out-of-band asset-pipeline scripts only
  (`upscale_*.py`, `pack_mat_normal.py`, `deploy_*.py`,
  `sd_terrain_upscale.py`). Not invoked by the build; not required
  for runtime. See `INTEGRATIONS.md`.

**Generated:**
- `cmake/mc2video_dlls.h.in` is configured to
  `${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h` — a C string array
  of FFmpeg DLL filenames consumed by the delay-load failure hook in
  `code/mc2video.cpp`.

## Runtime

**Environment:**
- Windows x64 (primary). The `CMakeLists.txt:16-21` check enforces
  a 64-bit pointer build path. Per `BUILD-WIN.md` and `README.md`,
  the supported toolchain is **Visual Studio 17 2022**.
- Linux x64 (secondary, upstream). The README notes "passed the game
  on my Linux box." The active worktree adds FFmpeg/Tracy/ASan/Win32
  link deps (`winmm ws2_32 dbghelp psapi user32 shell32`) that make
  the Linux build path effectively dormant on this branch — Linux
  build is unverified on `nifty-mendeleev`.

**Package Manager:**
- None at the language level. Third-party libraries are vendored in
  `3rdparty/3rdparty/` (zlib, GLEW, SDL2 family) and the worktree
  adds `3rdparty/ffmpeg-lgpl-win64/` and `3rdparty/tracy/`. A pinned
  `3rdparty.zip` is also checked in at repo root as a convenience.

## Frameworks

**Core:**
- **SDL2** (release-2.30.11 per `BUILD-WIN.md`) — window creation,
  GL context, input, timing. `SDL_GL_SetAttribute` configures the
  GL context at `GameOS/gameos/gos_render.cpp:92`+.
- **SDL2_mixer** (release-2.8.0) — audio playback
  (`GameOS/gameos/gameos_sound.cpp` calls `Mix_OpenAudio`,
  `Mix_LoadWAV`, `Mix_PlayMusic`).
- **SDL2_ttf** (release-2.24.0) — only consumed by the offline
  `text_tool/` executable, not by `mc2.exe`.
- **GLEW** — OpenGL extension loader (`#include <GL/glew.h>` in
  `gos_render.cpp:9`).

**Testing:**
- No unit-test framework in tree. Regression is a Python-driven smoke
  harness (`scripts/run_smoke.py`, `tests/smoke/`) that boots
  `mc2.exe` against five canonical missions
  (`mc2_01, mc2_03, mc2_10, mc2_17, mc2_24`) and asserts on exit
  status and log invariants. See `CLAUDE.md` "Smoke Gate".

**Build/Dev:**
- CMake ≥ 3.10 (`cmake_minimum_required` at `CMakeLists.txt:1`).
  Uses `CMP0072 NEW` to prefer GLVND OpenGL.
- Visual Studio 17 2022 generator on Windows (per BUILD-WIN.md).
  Required CMake invocation:
  `cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=… -DCMAKE_LIBRARY_ARCHITECTURE=x64 ..`.
- Configurations: `Debug`, `RelWithDebInfo`, `Release`.
  **Operational rule (worktree only):** always build
  `--config RelWithDebInfo`. Release reportedly crashes with
  `GL_INVALID_ENUM` (see `CLAUDE.md`).
- Optional ASan dev build via `-DMC2_ASAN=ON` (worktree only). Strips
  `/MP`, `/RTC1`, `/INCREMENTAL`, adds `/fsanitize=address`,
  disables Tracy, and copies `clang_rt.asan_dynamic-x86_64.dll` next
  to `mc2.exe` at POST_BUILD (`CMakeLists.txt:51-108` and `:316-354`
  on the worktree).

## Graphics API

**OpenGL:**
- Context requested: **OpenGL 4.3 Core Profile** (worktree;
  `gos_render.cpp:184-188`). Root branch requests 4.0.
- Buffer config: 8/8/8/8 RGBA, 16-bit depth, double-buffered
  (`gos_render.cpp:92-97`). Multisample is requested-off in source
  (worktree's MSAA path is post-process / FBO-based, not default
  framebuffer MSAA).
- `SDL_GL_CONTEXT_DEBUG_FLAG` is enabled (`gos_render.cpp:188`),
  so `KHR_debug` callbacks are available at runtime.

**Required GL features (worktree):**
- Core 4.3: SSBOs (`GL_SHADER_STORAGE_BUFFER`), `std430`,
  `glBindBufferBase`/`glBindBufferRange`, `glMultiDrawArraysIndirect`,
  compute shaders (`glDispatchCompute`).
- Tessellation shaders (terrain uses `.tesc`/`.tese`, see
  `shaders/gos_terrain.tesc`).
- `KHR_parallel_shader_compile` is opportunistically used (see
  memory `gl_hygiene_dropins.md`).
- 4× anisotropic filtering applied to all major texture paths
  (terrain, water, buildings, mechs). Whether this is
  `GL_EXT_texture_filter_anisotropic` or core
  (`GL_ARB_texture_filter_anisotropic`, core in 4.6) is
  unverified — appears to be the extension form.
- Native clip-space `[0,1]` depth is assumed
  (`TERRAIN_DEPTH_FUDGE = 0.002f`, see memory
  `gpu_direct_depth_state_inheritance.md`); implies `glClipControl`
  is in use somewhere in init, although the call site was not
  pinned during this mapping.

**Required GL features (root branch):**
- Core 4.0 / GLSL 4.20. No SSBOs in the legacy renderer path.
  Static-prop / water / terrain SSBO fast paths are worktree-only.

## Key Dependencies

| Package | Version (pin) | Role | Linkage |
|---------|---------------|------|---------|
| SDL2 | 2.30.11 | window/GL/input/timing | dynamic, vendored under `3rdparty/` |
| SDL2_mixer | 2.8.0 | audio mixing | dynamic |
| SDL2_ttf | 2.24.0 | text rasterization (tools only) | dynamic |
| GLEW | ~2.x prebuilt | GL extension loader | dynamic (`glew32.dll`) |
| zlib | gnuwin32 build | inflation for FastFile archives | dynamic (`zlib.dll`) |
| OpenGL | system | rendering | system (`OpenGL::GL` via CMake) |
| **FFmpeg LGPL shared** | **n7.1.3-45-g2d6ee37238** | **video playback (BIK→MP4 conversion target)** | **delay-loaded dynamic, worktree-only** |
| **Tracy** | vendored under `3rdparty/tracy/` | **CPU + GPU profiler client** | **static, worktree-only, on by default** |

FFmpeg DLLs in the worktree are pinned to specific major versions
(`avcodec-61`, `avformat-61`, `avutil-59`, `swresample-5`,
`swscale-8`) and are **delay-loaded** so `mc2.exe` launches even when
the DLLs are missing — the game falls back to skipping video playback
(`code/mc2video.cpp` provides the delay-load failure hook;
`CMakeLists.txt:284-292` configures `/DELAYLOAD` per DLL).

Tracy is compiled in with `-DTRACY_ENABLE -DTRACY_ON_DEMAND
-DTRACY_NO_SYSTEM_TRACING` (`CMakeLists.txt:113-122` worktree).
On-demand mode means events are only enqueued while a profiler is
actively connected. ASan and Tracy are mutually exclusive (the
`MC2_ASAN` branch disables Tracy).

**Infrastructure (Win32 link deps, worktree):**
- `winmm` — `timeGetTime` and similar.
- `ws2_32` — Winsock (linked but the multiplayer netlib is
  unimplemented at the socket level — see INTEGRATIONS.md).
- `dbghelp psapi` — symbolication for Tracy and crash dumps.
- `user32 shell32` — required by Tracy's Win32 backend and FFmpeg
  delay-load hook.

## Math Libraries

- **Custom**: `mclib/stuff/` (the "Stuff" library) provides the
  full math surface — `Vector3D`, `Vector4D`, `Matrix4D`, `Quaternion`,
  affine transforms, line-of-sight, etc. No GLM, no Eigen, no DirectX
  math. Appears to be the original Microsoft-released math layer
  preserved as-is.
- `mclib/mathfunc.cpp/.h` — additional helpers (trig tables, etc.).
- Shader-side math is plain GLSL; no spirv-cross / spirv-tools.

## Audio

- **SDL2_mixer** is the playback backend at the C++ layer
  (`GameOS/gameos/gameos_sound.cpp`):
  `Mix_OpenAudio`, `Mix_LoadWAV`, `Mix_PlayMusic`, plus a direct
  `SDL_LoadWAV` path at line 260.
- The engine still carries a higher-level abstraction in
  `mclib/soundsys.cpp` for SFX banks, Betty radio voice, music
  streaming, and `.pak` sound archives (lines 147-167, 733-835).
- 8-bit WAV trap: see memory `audio_8bit_wav_unsigned.md` —
  `gameos_sound.cpp:297` must use `AUDIO_U8`, not `AUDIO_S8`.

## Platform Target

**Development:**
- Windows 10/11, MSVC v143 (VS 2022), x64.
- The active build flow uses CMake-generated `.sln` opened in VS
  or built from CLI:
  `cmake.exe --build build64 --config RelWithDebInfo`.

**Production:**
- Windows x64. Deployed to a sibling install tree
  (`A:/Games/mc2-opengl/mc2-win64-v0.3/`) via the
  `/mc2-deploy` skill. The game runs out of that install root and
  reads game data from a parallel `mc2srcdata` checkout via FST
  archives + loose-file overrides.

## Compiler / Toolchain Flags

**Common (all configurations):**
- `-DPLATFORM_WINDOWS` (when WIN32)
- `-D_CRT_SECURE_NO_WARNINGS` (MSVC)
- `-DUSE_ASSEMBLER_CODE=0`
- `-DLINUX_BUILD` — **deliberately defined globally even on
  Windows builds** so the engine's path handling treats
  `PATH_SEPARATOR` as `"/"` everywhere. See memory
  `mc2_path_separator_linux_build.md`; hardcoding `"\\"` against
  `_WIN32` crashes mission load.
- MSVC: `/MP` (parallel compile; stripped under MC2_ASAN).

**Debug:**
- `-D_ARMOR -D_DEBUG -DBUGLOG -DLAB_ONLY` plus `/MTd /MP` on MSVC.

**Non-MSVC (Linux/clang):**
- `-std=c++0x -ggdb` plus a wide warning-suppression set
  (`-Wno-unused-local-typedefs -Werror=array-bounds -Wno-switch
  -Wwrite-strings -fpermissive` …).

**Worktree-only:**
- `-DTRACY_ENABLE -DTRACY_ON_DEMAND -DTRACY_NO_SYSTEM_TRACING`
- `-DMC2_ASAN` when ASan dev build is selected.

## C++ Standard Library Discipline

- The engine ships its own intrusive containers in
  `mclib/llist.cpp`, `mclib/sortlist.cpp`, `mclib/elist.h`,
  `mclib/pqueue.cpp` and the original MS "Stuff" containers under
  `mclib/stuff/`. STL is used freely in newer code paths and tools
  (e.g., `<queue>` in `data_tools/makefst.cpp`).
- Heap discipline: `gos_Malloc` / `gos_Free` are plain `malloc`/`free`
  wrappers (per `CLAUDE.md` MC2_ASAN section). The remaining custom
  slab allocator is `TG_GOSVertexPool` in `mclib/tgl.cpp` (see
  memory `tgl_pool_exhaustion_is_silent.md`).
- No `std::filesystem` — all file I/O goes through GameOS file
  primitives and the FastFile archive layer.

## Configuration

**Build config:**
- `CMakeLists.txt` is monolithic at the root, with `add_subdirectory`
  calls for `mclib/`, `mclib/mlr/`, `mclib/gosfx/`, `mclib/stuff/`,
  `gui/`, `GameOS/gameos/`, `GameOS/src/`, `res/`, `data_tools/`,
  `text_tool/`, `Viewer/`.
- `compile_commands.json` is exported (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

**Runtime config:**
- `system.cfg` (engine constants, loaded at startup).
- `options.cfg` in the install root (user preferences;
  HUD/resolution; the options dialog has a known bug that writes
  bad `ResolutionX/Y` — see `CLAUDE.md` Known Issues).
- `prefs.cfg` (multiplayer prefs via `code/prefs.cpp` /
  `code/mpprefs.cpp`).

**Environment knobs (worktree, opt-in instrumentation):**
- `MC2_TGL_POOL_TRACE`, `MC2_DESTROY_TRACE`,
  `MC2_GL_ERROR_DRAIN_SILENT`, `MC2_ASSET_SCALE_TRACE`,
  `MC2_ASSET_SCALE_SELFTEST`, `MC2_HEARTBEAT`,
  `MC2_WALL_SHADOW_TRACE`, `MC2_STATIC_UPDATE_SKIP`,
  `MC2_GPU_CULL_LIFECYCLE`, `MC2_GPU_CULL_SUBSTRATE`,
  `MC2_DEBUG_SHADOW_COLLECT`. Documented in `CLAUDE.md`
  "Tier-1 Instrumentation Env Vars".

## Platform Requirements

**Hardware:** discrete GPU with OpenGL 4.3 Core + SSBO support.
Development primary target is AMD RX 7900 XTX; AMD driver quirks are
documented at `docs/amd-driver-rules.md` (sampler2DArray softening,
attribute-0 hazards, `gl_FragDepth` interactions, feedback loops).

**OS:** Windows 10 1809 or newer (UCRT dependency; see
`ucrt-list.txt` for the runtime DLLs).

---

*Stack analysis: 2026-05-14*
