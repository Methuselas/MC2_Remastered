# Video Playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore cutscene video playback in the MC2 OpenGL port by replacing the stubbed `MC2Movie` class with an FFmpeg-based decoder that plays original `.bik` files and transparently prefers loose-file MP4/H.264 upscale replacements at any source resolution.

**Architecture:** FFmpeg LGPL DLLs (delay-loaded) are vendored under `3rdparty/ffmpeg-lgpl-win64/`. A new `code/mc2video.cpp` translation unit owns every FFmpeg symbol behind an availability gate; no FFmpeg header is included anywhere else. `MC2Movie` public API is preserved so the five existing callsites stay untouched. One NPOT RGBA8 texture sized to the source, one letterboxed screen-space quad computed at `init()`, main-thread decode per `update()`. Audio routes through a thin push-PCM adapter added to `SoundSystem`; A/V sync uses audio as master clock with wall-clock fallback.

**Tech Stack:** C++17 / FFmpeg 7.1.x LGPL (libavformat, libavcodec, libavutil, libswscale, libswresample) / OpenGL 4.3 core / SDL2 + SDL2_mixer / gosAudio / MSVC 2022 + CMake.

**Verification pattern:** This codebase has no C++ unit test framework. Each task is verified by building (`/mc2-build` skill), deploying (`/mc2-deploy`), launching `mc2.exe`, and inspecting stdout or visually confirming the cutscene plays. Where a pure-code invariant can be checked without the game, a minimal `ctest`-style runner is added in-task; otherwise rely on targeted console logging gated by `MC2_DEBUG_VIDEO`.

**Reference spec:** `docs/superpowers/specs/2026-04-23-video-playback-design.md` (v4, approved).

**Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`.

---

## File Structure

Files created:

- `3rdparty/ffmpeg-lgpl-win64/` — vendored FFmpeg: `include/`, `lib/`, `bin/`, `VERSION.txt`, `LICENSE.LGPL`.
- `cmake/FindFFmpegLGPL.cmake` — custom `find_package`-style module exposing an imported target per library.
- `code/mc2video.h` — declarations for the FFmpeg-backed playback engine (`VideoSession`, `resolveVideoCandidate`, `g_ffmpegAvailable`). No FFmpeg types exposed.
- `code/mc2video.cpp` — the ONLY translation unit that `#include`s FFmpeg headers. Owns delay-load failure hook, availability gate, resolver, decoder session.
- `release_assets/LICENSE.FFmpeg.txt` — LGPL notice + replacement instructions.

Files modified:

- `code/mc2movie.h` — new private members (opaque `VideoSession*` pointer). Public API unchanged.
- `code/mc2movie.cpp` — body rewritten against `mc2video.h`.
- `code/prefs.h` — add `bool UseUpscaledVideos`.
- `code/prefs.cpp` — default-init, load, save for new pref.
- `code/logistics.cpp` — remove 800×600 fossil at line 307.
- `code/mainmenu.cpp` — (audit) confirm full-window rect.
- `code/missionselectionscreen.cpp` — (audit) confirm rect is screen-space.
- `code/controlgui.cpp` / `code/forcegroupbar.cpp` — (audit) confirm pilot-cam rects use current resolution.
- `mclib/soundsys.h` / `mclib/soundsys.cpp` — add push-PCM adapter if P1 spike confirms the path; otherwise no change.
- `CMakeLists.txt` — `find_package(FFmpegLGPL REQUIRED)`, `target_link_libraries(mc2 ...)`, `/DELAYLOAD` linker flags, install rule for the five DLLs.

No files deleted.

---

## Task 1: P2 prerequisite — verify shipped Bink files are Bink1

**Why first:** If any shipped `.bik` is Bink2, `libavcodec` will fail to open it and the entire approach needs re-scoping. This is a 5-minute check that gates everything downstream.

**Files:**
- Read-only: `C:/Users/Joe/Downloads/MechCommander-2_Win_EN_ISO-Version/MechCommander 2 ISO/DATA/MOVIES/`

- [ ] **Step 1: Install standalone ffprobe (one-shot)**

Download `ffprobe.exe` from `https://github.com/BtbN/FFmpeg-Builds/releases/latest` (pick the `ffmpeg-master-latest-win64-lgpl-shared.zip` release or the `gpl` variant — ffprobe behavior is identical). Extract `ffprobe.exe` to a temp location (e.g., `C:/tmp/ffprobe.exe`). Do not yet place FFmpeg into the source tree — Task 3 does that with a pinned version.

- [ ] **Step 2: Probe every shipped .bik file for codec identity**

From MSYS2 bash:

```bash
MOVIE_DIR="/c/Users/Joe/Downloads/MechCommander-2_Win_EN_ISO-Version/MechCommander 2 ISO/DATA/MOVIES"
for f in "$MOVIE_DIR"/*.bik "$MOVIE_DIR"/*.BIK; do
  [ -e "$f" ] || continue
  codec=$(/c/tmp/ffprobe.exe -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$f" 2>&1)
  echo "$(basename "$f"): $codec"
done | tee /tmp/bink-probe.log
```

Expected: every line reports `binkvideo` (Bink1). No `bink2` or error lines.

- [ ] **Step 3: If any file is Bink2, STOP and escalate**

If any row reports anything other than `binkvideo`, halt the plan. Record the offending filenames in `docs/superpowers/specs/2026-04-23-video-playback-design.md` under a new "P2 findings" section and return to brainstorming to decide whether to: (a) re-demux those specific files offline and ship the re-demuxed version alongside the original, or (b) scope them out.

If every row reports `binkvideo`, commit the probe log as evidence:

```bash
mkdir -p docs/superpowers/plans/progress
cp /tmp/bink-probe.log docs/superpowers/plans/progress/2026-04-23-video-bink-probe.log
git add docs/superpowers/plans/progress/2026-04-23-video-bink-probe.log
git commit -m "chore(video): probe shipped .bik files — all Bink1, safe for libavcodec"
```

---

## Task 2: P1 prerequisite — audit gosAudio push-PCM + position-query capability

**Why:** The spec requires streaming PCM from the decoder into the mixer at the mixer's native format, plus a trustworthy playback position for audio-master A/V sync. `soundsys.cpp:779` shows `playDigitalStream` is file-backed via `gosAudio_CreateResource(gosAudio_StreamedFile, ...)` — there is no push-PCM call on `SoundSystem` today. Either gosAudio itself exposes a push mode, or we add a thin adapter, or we fall back to decode-to-temp-WAV. Deciding this now drives Task 12.

**Files:**
- Read-only audit: `GameOS/gameos/gosaudio.cpp`, `GameOS/gameos/gos.h`, `mclib/soundsys.cpp`

- [ ] **Step 1: Locate the gosAudio resource-type enum**

```bash
grep -n "gosAudio_StreamedFile\|gosAudio_UserMemory\|gosAudio_Streaming\|gosAudio_Memory\|enum.*gosAudio" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.h \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.cpp
```

Read the enum. Record whether any resource-type value exists for application-provided streaming PCM (names to watch for: `gosAudio_Streaming`, `gosAudio_UserStream`, `gosAudio_CallbackBuffer`).

- [ ] **Step 2: Check for a push or callback API**

```bash
grep -n "gosAudio_QueueBuffer\|gosAudio_FillBuffer\|gosAudio_SetCallback\|gosAudio_GetPosition\|gosAudio_GetSamplesPlayed" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.cpp \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.h
```

- [ ] **Step 3: Check the SDL2_mixer backend path**

gosAudio is implemented on top of SDL2_mixer in this port. Grep the gosAudio implementation for `Mix_HookMusic`, `Mix_PlayChannel`, `Mix_RegisterEffect`:

```bash
grep -n "Mix_\|SDL_Queue\|SDL_OpenAudio" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gosaudio.cpp
```

- [ ] **Step 4: Record the decision in `docs/superpowers/plans/progress/2026-04-23-video-p1-audio.md`**

Write one of three outcomes:

```
Path A (preferred): gosAudio exposes <function names>. We will call it
directly from mc2video.cpp. Position comes from <function>. No changes
to SoundSystem required.

Path B: gosAudio does not expose push-PCM, but SDL2_mixer does via
Mix_HookMusic (already linked). We will add a thin push adapter in a
new function SoundSystem::pushVideoPCM(int16_t* samples, int count, int
rate, int channels) backed by Mix_HookMusic. Position is tracked by
the adapter (sample counter, mixer rate).

Path C: Neither path exposes push-PCM. We will decode-to-temp-WAV at
init() (seek the file, decode audio stream to a scratch .wav under the
OS temp dir), then call the existing playDigitalStream on that path.
Audio-master clock is replaced by wall-clock; A/V sync is best-effort.
```

- [ ] **Step 5: Commit the decision**

```bash
git add docs/superpowers/plans/progress/2026-04-23-video-p1-audio.md
git commit -m "chore(video): P1 spike — audio push-PCM path decision"
```

Tasks 12 and 13 below branch on the recorded path. Paths are ordered A > B > C by preference.

---

## Task 3: Vendor FFmpeg 7.1.x LGPL shared Windows x64 build

**Files:**
- Create: `3rdparty/ffmpeg-lgpl-win64/include/` (copied from upstream)
- Create: `3rdparty/ffmpeg-lgpl-win64/lib/` (import libs `.lib`)
- Create: `3rdparty/ffmpeg-lgpl-win64/bin/` (runtime DLLs)
- Create: `3rdparty/ffmpeg-lgpl-win64/VERSION.txt`
- Create: `3rdparty/ffmpeg-lgpl-win64/LICENSE.LGPL`

- [ ] **Step 1: Download the pinned LGPL shared build**

Go to `https://github.com/BtbN/FFmpeg-Builds/releases` and pick the **most recent release whose version string starts with `n7.1`** (NOT the rolling `latest` tag). Download the asset matching:

```
ffmpeg-n7.1.X-YYYYMMDD-win64-lgpl-shared-7.1.zip
```

(`lgpl-shared`, not `gpl`, not `static`). Save the exact filename.

- [ ] **Step 2: Extract into the vendor directory**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
mkdir -p 3rdparty/ffmpeg-lgpl-win64
unzip -q ~/Downloads/ffmpeg-n7.1.X-YYYYMMDD-win64-lgpl-shared-7.1.zip -d /tmp/ffmpeg-raw
# The zip expands to a single top-level folder; move its contents up one level:
cp -r /tmp/ffmpeg-raw/*/include 3rdparty/ffmpeg-lgpl-win64/
cp -r /tmp/ffmpeg-raw/*/lib     3rdparty/ffmpeg-lgpl-win64/
cp -r /tmp/ffmpeg-raw/*/bin     3rdparty/ffmpeg-lgpl-win64/
```

Expected: `3rdparty/ffmpeg-lgpl-win64/{include,lib,bin}` exist.

- [ ] **Step 3: Trim unused components**

We only need avformat, avcodec, avutil, swscale, swresample. Delete the extras to keep the tree small:

```bash
cd 3rdparty/ffmpeg-lgpl-win64
rm -f bin/avfilter-*.dll bin/avdevice-*.dll bin/postproc-*.dll
rm -f bin/ffmpeg.exe bin/ffprobe.exe bin/ffplay.exe
rm -f lib/avfilter.lib lib/avdevice.lib lib/postproc.lib
rm -rf include/libavfilter include/libavdevice include/libpostproc
```

- [ ] **Step 4: Record exact DLL filenames in `VERSION.txt`**

```bash
cd 3rdparty/ffmpeg-lgpl-win64
ls bin/ > VERSION.txt.tmp
{
  echo "FFmpeg LGPL shared build — pinned"
  echo "Source: https://github.com/BtbN/FFmpeg-Builds/releases"
  echo "Upstream archive: ffmpeg-n7.1.X-YYYYMMDD-win64-lgpl-shared-7.1.zip"
  echo "Pinned: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo ""
  echo "Runtime DLLs (shipped alongside mc2.exe):"
  cat VERSION.txt.tmp | sed 's/^/  /'
  echo ""
  echo "Replace with same-major-version build only. If upstream renames a"
  echo "DLL (e.g. avcodec-61.dll -> avcodec-62.dll), cmake/FindFFmpegLGPL.cmake"
  echo "and the /DELAYLOAD linker flags in CMakeLists.txt must be updated to"
  echo "match."
} > VERSION.txt
rm VERSION.txt.tmp
```

Substitute real values: `n7.1.X` with the actual version number, `YYYYMMDD` with the actual date, and the DLL list will be the real ls output (typically `avcodec-61.dll`, `avformat-61.dll`, `avutil-59.dll`, `swresample-5.dll`, `swscale-8.dll` for n7.1).

- [ ] **Step 5: Place the LGPL notice**

Copy the `LICENSE.txt` that ships in the FFmpeg zip to `3rdparty/ffmpeg-lgpl-win64/LICENSE.LGPL`.

- [ ] **Step 6: Commit**

```bash
git add 3rdparty/ffmpeg-lgpl-win64/
git commit -m "chore(video): vendor FFmpeg 7.1.x LGPL shared build for Windows x64

Pinned release — not rolling. See 3rdparty/ffmpeg-lgpl-win64/VERSION.txt
for the exact upstream archive and DLL manifest. Unused components
(avfilter, avdevice, postproc, ffmpeg/ffprobe/ffplay exes) removed."
```

---

## Task 4: CMake integration — FindFFmpegLGPL module + delay-load linker flags

**Files:**
- Create: `cmake/FindFFmpegLGPL.cmake`
- Modify: `CMakeLists.txt` (around lines 63-70, add module path; around line 205, link libs and delay-load)

- [ ] **Step 1: Create the find-module**

Create `cmake/FindFFmpegLGPL.cmake` with:

```cmake
# Locates the vendored FFmpeg LGPL shared build under
# 3rdparty/ffmpeg-lgpl-win64/. Exposes one IMPORTED target per library:
#   FFmpegLGPL::avcodec
#   FFmpegLGPL::avformat
#   FFmpegLGPL::avutil
#   FFmpegLGPL::swscale
#   FFmpegLGPL::swresample
# Plus FFmpegLGPL_RUNTIME_DLLS (list of absolute DLL paths) for install().

set(FFMPEG_VENDOR_DIR "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg-lgpl-win64")

if(NOT EXISTS "${FFMPEG_VENDOR_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpegLGPL: vendored tree not found at ${FFMPEG_VENDOR_DIR}. "
        "Run the video-playback plan's Task 3 to populate it.")
endif()

set(FFmpegLGPL_INCLUDE_DIR "${FFMPEG_VENDOR_DIR}/include")

function(_ffmpeg_add_lib NAME)
    file(GLOB _imp_lib "${FFMPEG_VENDOR_DIR}/lib/${NAME}.lib")
    file(GLOB _dll "${FFMPEG_VENDOR_DIR}/bin/${NAME}-*.dll")
    if(NOT _imp_lib OR NOT _dll)
        message(FATAL_ERROR
            "FFmpegLGPL: could not find ${NAME} import lib or DLL in ${FFMPEG_VENDOR_DIR}")
    endif()
    add_library(FFmpegLGPL::${NAME} SHARED IMPORTED)
    set_target_properties(FFmpegLGPL::${NAME} PROPERTIES
        IMPORTED_LOCATION "${_dll}"
        IMPORTED_IMPLIB   "${_imp_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpegLGPL_INCLUDE_DIR}")
    get_filename_component(_dll_name "${_dll}" NAME)
    list(APPEND FFmpegLGPL_RUNTIME_DLLS "${_dll}")
    set(FFmpegLGPL_RUNTIME_DLLS "${FFmpegLGPL_RUNTIME_DLLS}" PARENT_SCOPE)
    set(FFmpegLGPL_${NAME}_DLL_NAME "${_dll_name}" PARENT_SCOPE)
endfunction()

set(FFmpegLGPL_RUNTIME_DLLS "")
_ffmpeg_add_lib(avcodec)
_ffmpeg_add_lib(avformat)
_ffmpeg_add_lib(avutil)
_ffmpeg_add_lib(swscale)
_ffmpeg_add_lib(swresample)

set(FFmpegLGPL_FOUND TRUE)
message(STATUS "FFmpegLGPL: ${FFMPEG_VENDOR_DIR}")
message(STATUS "FFmpegLGPL DLLs: ${FFmpegLGPL_RUNTIME_DLLS}")

# Generate a C header listing the runtime DLL filenames so
# ffmpegProbeAvailability() in mc2video.cpp can enumerate them
# without hardcoding. Single source of truth: the vendored tree.
set(_dll_names_c "")
foreach(_dll_path ${FFmpegLGPL_RUNTIME_DLLS})
    get_filename_component(_n "${_dll_path}" NAME)
    string(APPEND _dll_names_c "    \"${_n}\",\n")
endforeach()
set(FFMPEG_DLL_NAMES_CLIST "${_dll_names_c}")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/mc2video_dlls.h.in"
    "${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h"
    @ONLY)
```

- [ ] **Step 1b: Create the header template**

Create `cmake/mc2video_dlls.h.in`:

```cpp
// AUTO-GENERATED by cmake/FindFFmpegLGPL.cmake from the contents of
// 3rdparty/ffmpeg-lgpl-win64/bin/. Do NOT edit by hand. Re-run CMake
// configure after updating the vendored FFmpeg tree to regenerate.
#ifndef MC2_VIDEO_DLLS_H
#define MC2_VIDEO_DLLS_H
static const char* const MC2_VIDEO_DLL_LIST[] = {
@FFMPEG_DLL_NAMES_CLIST@
};
#endif
```

The `@FFMPEG_DLL_NAMES_CLIST@` placeholder is filled by `configure_file` with the DLL filenames discovered by the find-module — single source of truth.

- [ ] **Step 2: Wire into top-level `CMakeLists.txt`**

Open `CMakeLists.txt`. Find the block around lines 60-70 that does `find_package(SDL2 ...)`. After line 70 (after `find_package(OpenGL REQUIRED)`), add:

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(FFmpegLGPL REQUIRED)
# Exposes MC2_VIDEO_DLL_LIST (C string array) in ${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h
include_directories("${CMAKE_BINARY_DIR}/generated")
```

Then find line 205 (the `target_link_libraries(mc2 ...)` line). After it, add:

```cmake
target_link_libraries(mc2
    FFmpegLGPL::avformat
    FFmpegLGPL::avcodec
    FFmpegLGPL::avutil
    FFmpegLGPL::swscale
    FFmpegLGPL::swresample)

# Delay-load all five FFmpeg DLLs so mc2.exe launches even when the
# DLLs are missing (the game gracefully skips video playback). The
# delay-load failure hook lives in code/mc2video.cpp.
if(MSVC)
    target_link_options(mc2 PRIVATE
        "/DELAYLOAD:${FFmpegLGPL_avcodec_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_avformat_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_avutil_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_swscale_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_swresample_DLL_NAME}")
    target_link_libraries(mc2 delayimp.lib)
endif()
```

- [ ] **Step 3: Configure and build to confirm linker accepts the flags**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tee /tmp/build-task4.log
```

Expected: build succeeds. There are no callers of FFmpeg symbols yet, so the delay-load flags are inert but the linker validates the DLL names exist in the import libs.

- [ ] **Step 4: Commit**

```bash
git add cmake/FindFFmpegLGPL.cmake cmake/mc2video_dlls.h.in CMakeLists.txt
git commit -m "build(video): link vendored FFmpeg LGPL with delay-load hooks"
```

---

## Task 5: Create `code/mc2video.h` and `code/mc2video.cpp` skeleton with availability gate + delay-load failure hook

**Files:**
- Create: `code/mc2video.h`
- Create: `code/mc2video.cpp`
- Modify: `CMakeLists.txt` (add `code/mc2video.cpp` to `${SOURCES}`)

- [ ] **Step 1: Write the public header**

Create `code/mc2video.h`:

```cpp
//=============================================================================
// FFmpeg-backed video decoder facade for MC2Movie.
//
// Every FFmpeg symbol lives in mc2video.cpp. This header exposes only
// plain C / C++ types so no other translation unit needs FFmpeg headers.
//=============================================================================
#ifndef MC2VIDEO_H
#define MC2VIDEO_H

#include <cstdint>

struct VideoSession;          // opaque

// Availability of the FFmpeg DLLs at runtime. Flipped to false by
// ffmpegProbeAvailability() at startup if any delay-loaded DLL is
// missing. Readable from anywhere.
extern bool g_ffmpegAvailable;

// MUST be called once at engine startup, before any code path that
// could reach a MC2Movie::init. Probes every FFmpeg DLL inside an SEH
// wrapper and sets g_ffmpegAvailable accordingly. Calling it late (or
// not at all) means the first missing-DLL case will only be caught by
// the delay-load failure hook's defensive path, which is harder to
// reason about.
void ffmpegProbeAvailability();

// Enumerates the *viable* candidate override chain by index. index=0
// is the highest-priority candidate that actually exists on disk (or
// is the FST-fallback slot); index=1 is the next viable one; and so
// on. Loose-file candidates whose files are absent are skipped
// internally — the caller never sees them. Returns false when no
// further viable candidate exists.
//
// The underlying chain (before missing-file filtering):
//   a: data/movies/<name>.mp4        (loose, only if preferUpscaled)
//   b: data/movies/<name>.mkv        (loose, only if preferUpscaled)
//   c: data/movies/<name>.webm       (loose, only if preferUpscaled)
//   d: data/movies/<name>.bik        (loose)
//   e: <name>.bik                    (FST; File::open auto-falls-back,
//                                     so this slot is always viable)
//
// Example with preferUpscaled=true and only msft.mkv + msft.bik (loose)
// physically present:
//   resolveVideoCandidate(..., 0, ...) -> true, path ends in msft.mkv
//   resolveVideoCandidate(..., 1, ...) -> true, path ends in msft.bik
//   resolveVideoCandidate(..., 2, ...) -> true, FST fallback (same path)
//   resolveVideoCandidate(..., 3, ...) -> false (exhausted)
//
// The caller simply increments index until it receives false. Each
// attempted video_open() still fails cleanly if the file is corrupt,
// and the caller moves on to the next index.
bool resolveVideoCandidate(const char* shortName, bool preferUpscaled,
                           int index, char* outPath, unsigned outPathSize);

// Creates a playback session for the file at resolvedPath. Returns null
// on any failure (file open, decoder init, texture alloc). On success,
// fills out width/height/sar/hasAudio/hasAlpha.
struct VideoOpenParams {
    const char* resolvedPath;   // from resolveVideoCandidate
    int   destRectW;            // screen-space
    int   destRectH;
    bool  useWaveFile;          // caller's flag from MC2Movie::init
    const char* waveFileShortName;  // non-null iff useWaveFile==true
};

struct VideoOpenResult {
    int    srcW;
    int    srcH;
    double sar;                 // sample aspect ratio, 1.0 if unknown
    double fps;
    bool   hasAudio;
    bool   hasAlpha;
    unsigned glTextureId;       // 0 if creation failed
    float  quadX0, quadY0, quadX1, quadY1;  // destination quad in screen-space
};

VideoSession* video_open(const VideoOpenParams& p, VideoOpenResult* out);
void          video_close(VideoSession* s);

// Per-frame update. Returns true when EOF reached (caller deletes).
bool          video_update(VideoSession* s);

// Draws the current frame to the quad stored at open time.
void          video_render(VideoSession* s);

// Lifecycle controls (match MC2Movie public API).
void          video_pause(VideoSession* s, bool paused);
void          video_stop(VideoSession* s);
void          video_restart(VideoSession* s);
void          video_setRect(VideoSession* s, int x0, int y0, int w, int h);

#endif // MC2VIDEO_H
```

- [ ] **Step 2: Write the skeleton `.cpp` with delay-load hook + gate**

Create `code/mc2video.cpp`:

```cpp
//=============================================================================
// FFmpeg-backed video decoder — sole translation unit with FFmpeg symbols.
//=============================================================================
#include "mc2video.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)
#  include <windows.h>
#  include <delayimp.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

//-----------------------------------------------------------------------------
// Availability gate
//-----------------------------------------------------------------------------
bool g_ffmpegAvailable = true;

static const bool s_videoTrace = (getenv("MC2_DEBUG_VIDEO") != nullptr);
#define VIDEO_TRACE(fmt, ...) do { \
    if (s_videoTrace) { printf("[VIDEO] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } \
} while (0)

#define VIDEO_LOG(fmt, ...) do { \
    printf("[VIDEO] " fmt "\n", ##__VA_ARGS__); fflush(stdout); \
} while (0)

#if defined(_WIN32)
// Delay-load failure hook. Defense-in-depth: flags unavailability if a
// delayed import ever triggers post-probe. Returns a sentinel so the
// loader's internal dispatch does not terminate the process at the
// import site. The PRIMARY mechanism is ffmpegProbeAvailability()
// below — the hook is only reached if we slip a call past the gate.
static FARPROC WINAPI Mc2VideoDelayLoadFailureHook(unsigned dliNotify,
                                                    PDelayLoadInfo pdli)
{
    if (dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc) {
        if (g_ffmpegAvailable) {
            VIDEO_LOG("FFmpeg delay-load failed post-probe (dll=%s, notify=%u). "
                      "This should not happen if the gate is respected.",
                      pdli && pdli->szDll ? pdli->szDll : "<unknown>",
                      dliNotify);
            g_ffmpegAvailable = false;
        }
        return (FARPROC)0;
    }
    return 0;
}

extern "C" const PfnDliHook __pfnDliFailureHook2 = Mc2VideoDelayLoadFailureHook;

// Probe every delay-loaded DLL at startup, inside an SEH wrapper, so
// any failure (missing DLL, missing symbol) flips the availability
// gate *before* any ordinary FFmpeg call site runs. After probe, every
// wrapper in this TU checks g_ffmpegAvailable and short-circuits on
// false — the delay-load mechanism is never invoked again.
static HRESULT tryLoadOne(const char* dllName)
{
    __try {
        return __HrLoadAllImportsForDll(dllName);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

// mc2video_dlls.h is CMake-generated (see cmake/FindFFmpegLGPL.cmake
// and cmake/mc2video_dlls.h.in). It defines MC2_VIDEO_DLL_LIST as a
// static const char* const[] of the runtime DLL filenames. Single
// source of truth: the 3rdparty/ffmpeg-lgpl-win64/bin/ directory.
#include "mc2video_dlls.h"

void ffmpegProbeAvailability()
{
    // Bumping FFmpeg updates one place (the vendored tree); CMake
    // regenerates this header automatically.
    for (const char* d : MC2_VIDEO_DLL_LIST) {
        HRESULT hr = tryLoadOne(d);
        if (FAILED(hr)) {
            VIDEO_LOG("FFmpeg unavailable: probe of %s failed (hr=0x%08lx). "
                      "Video playback disabled for this session.", d, (long)hr);
            g_ffmpegAvailable = false;
            return;
        }
    }
    VIDEO_TRACE("FFmpeg availability probe passed");
}
#else
void ffmpegProbeAvailability() { /* non-Windows: link-loaded, always avail */ }
#endif

//-----------------------------------------------------------------------------
// Resolver — implemented in Task 6
//-----------------------------------------------------------------------------
bool resolveVideoCandidate(const char* /*shortName*/, bool /*preferUpscaled*/,
                           int /*index*/,
                           char* /*outPath*/, unsigned /*outPathSize*/)
{
    return false;  // Task 6
}

//-----------------------------------------------------------------------------
// Session open/close/update/render — implemented in Tasks 9-13
//-----------------------------------------------------------------------------
struct VideoSession {
    // fields added incrementally by later tasks
    bool dummy;
};

VideoSession* video_open(const VideoOpenParams& /*p*/, VideoOpenResult* out)
{
    if (!g_ffmpegAvailable) return nullptr;
    if (out) memset(out, 0, sizeof(*out));
    return nullptr;  // Task 9
}

void video_close(VideoSession* /*s*/) { /* Task 9 */ }
bool video_update(VideoSession* /*s*/) { return true; /* Task 10 */ }
void video_render(VideoSession* /*s*/) { /* Task 11 */ }
void video_pause(VideoSession* /*s*/, bool /*paused*/) { /* Task 15 */ }
void video_stop(VideoSession* /*s*/) { /* Task 15 */ }
void video_restart(VideoSession* /*s*/) { /* Task 15 */ }
void video_setRect(VideoSession* /*s*/, int /*x0*/, int /*y0*/,
                   int /*w*/, int /*h*/) { /* Task 15 */ }
```

Note: the `__pfnDliFailureHook2` symbol name is the MSVC contract — do not rename. `dliFailLoadLib` and `dliFailGetProc` come from `<delayimp.h>`.

- [ ] **Step 2b: Wire `ffmpegProbeAvailability()` into startup**

Open `code/mechcmd2.cpp`. Find the `InitializeGameEngine` function (search for `{ ZoneScopedN("InitializeGameEngine prefs.applyPrefs"); prefs.load();` — that line exists around line 1564). Immediately after `prefs.applyPrefs();`, add:

```cpp
#include "mc2video.h"      // at top of file if not already
// ...
ffmpegProbeAvailability();
```

This must run before any code path that constructs an `MC2Movie`. `MainMenu::init` is the earliest such caller (`mainmenu.cpp:156`), and it runs after `InitializeGameEngine` completes. Placing the probe inside `InitializeGameEngine` guarantees the gate is set before any video call site.

- [ ] **Step 3: Add the new file to `CMakeLists.txt`**

Find the `${SOURCES}` accumulator (it's built up across multiple `list(APPEND SOURCES ...)` calls). Add `code/mc2video.cpp` to whichever list contains `code/mc2movie.cpp`:

```bash
grep -n "mc2movie" CMakeLists.txt
```

In that same `list(APPEND ...)`, add `code/mc2video.cpp` on a new line, alphabetically adjacent to `mc2movie.cpp`.

- [ ] **Step 4: Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -40
```

Expected: success. `g_ffmpegAvailable` is a new symbol; the hook is registered but never called yet (no FFmpeg symbols are referenced from live code).

- [ ] **Step 5: Deploy and smoke-test with DLLs missing**

Deploy, then from the deploy dir rename `avcodec-*.dll` to `avcodec-*.dll.bak`. Launch `mc2.exe`. Expected: the game starts normally, and stdout shows one `[VIDEO] FFmpeg unavailable: probe of avcodec-61.dll failed` line from `ffmpegProbeAvailability()`. No crash. This confirms the probe path works end-to-end even though no FFmpeg symbol has any live caller yet. Restore the DLL before continuing:

```bash
cd /a/Games/mc2-opengl/mc2-win64-v0.1.1
mv avcodec-*.dll.bak avcodec-*.dll  # restore (use actual filename)
```

- [ ] **Step 6: Commit**

```bash
git add code/mc2video.h code/mc2video.cpp code/mechcmd2.cpp CMakeLists.txt
git commit -m "feat(video): skeleton mc2video.cpp with delay-load failure hook + availability gate"
```

---

## Task 6: Implement `resolveVideoCandidate()` — loose-file override chain

**Files:**
- Modify: `code/mc2video.cpp` (replace stub body of `resolveVideoCandidate`)
- Modify: `code/mc2video.cpp` (add includes for `fileExists`, `moviePath`)

- [ ] **Step 1: Add includes**

At the top of `code/mc2video.cpp`, after the existing system includes:

```cpp
#include "file.h"       // fileExists
#include "paths.h"      // moviePath (if such a header exists; else:
// extern "C" char moviePath[]; is also acceptable, match existing
// convention in mc2movie.cpp)
```

Check `grep -n '#include.*paths\.h' code/*.cpp` — if no callers include it, use the `extern "C" char moviePath[80];` declaration instead, since that is how `mc2movie.cpp` already refers to shared paths.

- [ ] **Step 2: Implement `resolveVideoCandidate`**

```cpp
bool resolveVideoCandidate(const char* shortName, bool preferUpscaled,
                           int index, char* outPath, unsigned outPathSize)
{
    if (!shortName || !outPath || outPathSize < 2 || index < 0) return false;

    // The raw chain, in priority order. Each entry is (extension,
    // requiresLooseFile). The last entry (FST .bik) has
    // requiresLooseFile=false: it is always considered viable because
    // File::open consults the FST archive if the loose path is missing.
    struct Slot { const char* ext; bool looseRequired; };
    static constexpr Slot kUpscaleSlots[] = {
        { ".mp4",  true  },
        { ".mkv",  true  },
        { ".webm", true  },
    };
    static constexpr Slot kOriginalSlots[] = {
        { ".bik",  true  },   // loose .bik
        { ".bik",  false },   // FST-fallback .bik (always viable)
    };

    // Walk the raw chain in order, skipping loose slots whose file is
    // absent. The caller's index selects the Nth *viable* slot. We
    // stop the scan as soon as we either return the requested viable
    // slot or exhaust the chain.
    int viableSoFar = 0;
    auto tryEmit = [&](const Slot& s) -> int {
        // 1 = returned this slot (requested viable index matched)
        // 0 = skipped (loose file missing)
        // other: not relevant here
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s%s%s", moviePath, shortName, s.ext);
        if (s.looseRequired && !fileExists(tmp)) return 0;
        if (viableSoFar == index) {
            snprintf(outPath, outPathSize, "%s", tmp);
            VIDEO_TRACE("resolver: viable idx=%d ext=%s path=%s%s",
                        index, s.ext,
                        s.looseRequired ? "" : "(FST) ", outPath);
            return 1;
        }
        ++viableSoFar;
        return 0;
    };

    if (preferUpscaled) {
        for (const Slot& s : kUpscaleSlots) {
            int r = tryEmit(s);
            if (r == 1) return true;
        }
    }
    for (const Slot& s : kOriginalSlots) {
        int r = tryEmit(s);
        if (r == 1) return true;
    }

    // Chain exhausted without reaching the requested viable index.
    return false;
}
```

**Behavioral contract (matching the header comment):**

- `index=0` always returns the highest-priority candidate whose file exists (or the FST slot if no loose file is present).
- Subsequent indices enumerate remaining viable candidates in order.
- Missing loose files are invisible to the caller — they never count against the index.
- Returns false only when truly exhausted. Callers iterate `for (int i = 0; resolveVideoCandidate(..., i, ...); ++i)`.
- Because the FST slot is always viable, the chain is guaranteed to produce at least one result (the FST `.bik`) for any movie name the engine knows — even if every loose candidate is missing. A caller sees `false` only when the resolver gave up, at which point MC2Movie has nothing to try. `video_open` may still fail on the FST slot if the archive does not contain that movie; that is the real "no such movie" terminal state.

- [ ] **Step 3: Inline unit-check (temporary)**

Temporarily in `main()` at an early point, add a diagnostic block gated by env var (remove before shipping; this is a one-task sanity check):

```cpp
if (getenv("MC2_VIDEO_RESOLVER_TEST")) {
    char p[1024];
    for (int i = 0; ; ++i) {
        if (!resolveVideoCandidate("msft", true, i, p, sizeof(p))) break;
        printf("resolver msft up idx=%d -> %s\n", i, p);
    }
    for (int i = 0; ; ++i) {
        if (!resolveVideoCandidate("msft", false, i, p, sizeof(p))) break;
        printf("resolver msft orig idx=%d -> %s\n", i, p);
    }
}
```

Run: `MC2_VIDEO_RESOLVER_TEST=1 ./mc2.exe`. Expected output shows the chain is enumerated in order, with loose-missing candidates silently skipped and the FST-fallback slot always reported. Remove this block after confirming.

- [ ] **Step 4: Build, deploy, confirm, remove diagnostic**

Per `/mc2-build-deploy`. Launch with `MC2_VIDEO_RESOLVER_TEST=1`, confirm output, remove the diagnostic block, rebuild, commit.

- [ ] **Step 5: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): resolveVideoCandidate — extension-agnostic override chain"
```

---

## Task 7: Add `prefs.UseUpscaledVideos` and wire through load/save

**Files:**
- Modify: `code/prefs.h` (add field)
- Modify: `code/prefs.cpp` (default, load, save)

- [ ] **Step 1: Add the field to `CPrefs`**

Open `code/prefs.h`. After line 54 (`bool pilotVideos;`), add:

```cpp
	bool	UseUpscaledVideos;
```

- [ ] **Step 2: Default-initialize in `CPrefs::CPrefs`/`load`**

Open `code/prefs.cpp`. Find the block near line 44 that defaults `DigitalMasterVolume = 255;` (this is the defaults block inside load() or the ctor — match whichever). Add near it:

```cpp
	UseUpscaledVideos = true;
```

Then find the `readIdLong`/`readIdBoolean` chain around line 112 and add:

```cpp
	{
		long tmp = UseUpscaledVideos ? 1 : 0;
		prefsFile->readIdLong("UseUpscaledVideos", tmp);
		UseUpscaledVideos = (tmp != 0);
	}
```

(Use `readIdLong` with a temporary long because that is the existing pattern; `readIdBoolean` may not exist.)

- [ ] **Step 3: Save in `CPrefs::save`**

Near line 313 (`writeIdLong("DigitalMasterVolume", ...)`), add:

```cpp
	prefsFile->writeIdLong("UseUpscaledVideos", UseUpscaledVideos ? 1 : 0);
```

- [ ] **Step 4: Build, launch, exit, inspect `options.pref` in deploy dir**

```bash
grep -i UseUpscaledVideos /a/Games/mc2-opengl/mc2-win64-v0.1.1/options.pref
```

Expected: a line `UseUpscaledVideos = 1` appears after first save.

- [ ] **Step 5: Commit**

```bash
git add code/prefs.h code/prefs.cpp
git commit -m "feat(video): add prefs.UseUpscaledVideos (default on)"
```

---

## Task 8: Rewrite `MC2Movie` to delegate to `mc2video.*`

**Files:**
- Modify: `code/mc2movie.h` (add opaque session pointer, remove tile members)
- Modify: `code/mc2movie.cpp` (rewrite methods to delegate)

- [ ] **Step 1: Add an opaque member to `MC2Movie`**

Open `code/mc2movie.h`. At the top, after the existing includes, add:

```cpp
struct VideoSession;  // forward decl from mc2video.h
```

In the `protected:` block, replace the tile-grid members:

```cpp
		DWORD*		MC2Surface;
		DWORD		mc2TextureNodeIndex[MAX_TEXTURES_NEEDED];
		RECT		MC2Rect;
		DWORD 		numWide;
		DWORD		numHigh;
		DWORD		totalTexturesUsed;
		bool		forceStop;
		DWORD		singleTextureSize;
		bool		stillPlaying;
		bool		separateWAVE;
		bool		soundStarted;
		char		*waveName;
		char		*m_MC2Name;
```

with:

```cpp
		RECT		MC2Rect;
		bool		forceStop;
		bool		stillPlaying;
		bool		separateWAVE;       // callsite intent: use sidecar WAV
		char		*waveName;
		char		*m_MC2Name;
		VideoSession*	m_session;      // owned; null iff not playing
		bool		m_sessionDone;      // EOF reached
```

Delete the `const DWORD MAX_TEXTURES_NEEDED = 6;` constant. Delete the `BltMovieFrame` declaration.

- [ ] **Step 2: Rewrite `mc2movie.cpp` `init()`**

Replace the entire body:

```cpp
void MC2Movie::init(const char* MC2Name, RECT mRect, bool useWaveFile)
{
    // Copy the short-name from the full path.
    char shortName[1024];
    _splitpath(MC2Name, NULL, NULL, shortName, NULL);

    m_MC2Name = new char[strlen(shortName) + 1];
    strcpy(m_MC2Name, shortName);

    MC2Rect = mRect;
    forceStop     = false;
    stillPlaying  = false;
    m_session     = nullptr;
    m_sessionDone = false;

    separateWAVE = useWaveFile && (prefs.DigitalMasterVolume != 0.0f);
    if (separateWAVE) {
        waveName = new char[strlen(shortName) + 1];
        strcpy(waveName, shortName);
    } else {
        waveName = nullptr;
    }

    if (!g_ffmpegAvailable) {
        VIDEO_LOG("init: FFmpeg unavailable, skipping movie '%s'", shortName);
        stillPlaying = false;   // update() will return true immediately
        m_sessionDone = true;
        return;
    }

    // Enumerate every candidate in the resolver's chain until one
    // opens successfully. Each failure fully tears down the failed
    // decoder state before the next attempt (spec: fallback happens
    // only during init, never by recovering a half-constructed
    // decoder mid-playback). The loop terminates when the resolver
    // reports the chain exhausted.
    const bool preferUpscaled = prefs.UseUpscaledVideos;
    for (int candidateIndex = 0; ; ++candidateIndex) {
        char resolvedPath[1024];
        if (!resolveVideoCandidate(shortName, preferUpscaled, candidateIndex,
                                    resolvedPath, sizeof(resolvedPath))) {
            break;  // chain exhausted
        }

        VideoOpenParams p = {};
        p.resolvedPath       = resolvedPath;
        p.destRectW          = MC2Rect.right  - MC2Rect.left;
        p.destRectH          = MC2Rect.bottom - MC2Rect.top;
        p.useWaveFile        = separateWAVE;
        p.waveFileShortName  = waveName;

        VideoOpenResult r = {};
        m_session = video_open(p, &r);
        if (m_session) {
            stillPlaying = true;
            VIDEO_TRACE("init: opened %s (%dx%d, sar=%.3f, fps=%.2f, audio=%d)",
                        resolvedPath, r.srcW, r.srcH, r.sar, r.fps, r.hasAudio);
            return;
        }

        VIDEO_LOG("init: open failed for '%s' (idx=%d), trying next candidate",
                  resolvedPath, candidateIndex);
        // video_open is responsible for tearing down its own partial
        // state on failure; nothing to clean up here.
    }

    // All candidates exhausted.
    VIDEO_LOG("init: no playable candidate for '%s'", shortName);
    stillPlaying  = false;
    m_sessionDone = true;
}
```

Add required includes at the top of `mc2movie.cpp`:

```cpp
#include "mc2video.h"
#include "prefs.h"
```

- [ ] **Step 3: Rewrite `update()`**

```cpp
bool MC2Movie::update(void)
{
    if (!stillPlaying || m_sessionDone) {
        return true;  // caller deletes us
    }

    if (forceStop) {
        if (m_session) video_stop(m_session);
        stillPlaying = false;
        m_sessionDone = true;
        return true;
    }

    if (m_session && video_update(m_session)) {
        // natural EOF
        stillPlaying = false;
        m_sessionDone = true;
        return true;
    }

    return false;
}
```

- [ ] **Step 4: Rewrite `render()`**

```cpp
void MC2Movie::render(void)
{
    if (m_session) video_render(m_session);
}
```

- [ ] **Step 5: Rewrite `setRect()`, `pause()`, `stop()`, `restart()`**

```cpp
void MC2Movie::setRect(RECT vRect)
{
    MC2Rect = vRect;
    if (m_session) {
        video_setRect(m_session,
                      vRect.left, vRect.top,
                      vRect.right  - vRect.left,
                      vRect.bottom - vRect.top);
    }
}

void MC2Movie::pause(bool pauseState)
{
    if (m_session) video_pause(m_session, pauseState);
    // forceStop = false; preserved from original stub
}

void MC2Movie::stop(void)
{
    forceStop = true;
    if (m_session) video_stop(m_session);
}

void MC2Movie::restart(void)
{
    if (m_session) video_restart(m_session);
    forceStop    = false;
    stillPlaying = true;
    m_sessionDone = false;
}
```

`pause()` and `setRect()` move from inline in `mc2movie.h` to out-of-line in `mc2movie.cpp`. Remove the inline bodies from the header.

- [ ] **Step 6: Destructor — release session**

Add to `mc2movie.h` destructor (replace inline body):

```cpp
~MC2Movie(void);
```

In `mc2movie.cpp`:

```cpp
MC2Movie::~MC2Movie(void)
{
    if (m_session) {
        video_close(m_session);
        m_session = nullptr;
    }
    if (waveName)   { delete[] waveName;   waveName   = nullptr; }
    if (m_MC2Name)  { delete[] m_MC2Name;  m_MC2Name  = nullptr; }
}
```

- [ ] **Step 7: Build**

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```

Expected: success. `video_open` is still a stub returning null, so at runtime every movie immediately reports done.

- [ ] **Step 8: Deploy and launch — confirm no crash**

Deploy, launch. Main-menu intro slot silently skips (one `[VIDEO] init: open failed` line in console). Main menu appears. No crash.

- [ ] **Step 9: Commit**

```bash
git add code/mc2movie.h code/mc2movie.cpp
git commit -m "refactor(video): delegate MC2Movie to mc2video opaque session"
```

---

## Task 8b: Texture-upload integration decision — gos-owned vs raw GL

**Why before Task 9:** Task 9 allocates the per-session texture. If we pick raw GL here but the draw path (`gos_DrawQuads`) insists on gos-owned texture handles, Tasks 9–11 all get rewritten. Decide once, up front, and the rest of the pipeline is linear.

**Files:** read-only audit.

- [ ] **Step 1: Find how UI currently draws screen-space textured quads**

```bash
grep -rn "gos_DrawQuads\|gos_DrawTriangle\|gos_SetRenderState.*Texture" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/*.cpp | head -20
```

Open two representative callsites (e.g., a UI bitmap blit in `mechicon.cpp` and a video-ish path in `forcegroupbar.cpp`). Read the vertex struct (`gos_VERTEX`), the texture binding API (`gos_SetRenderState(gos_State_Texture, handle)`), and trace `handle` back to its allocation site.

- [ ] **Step 2: Check whether gos exposes a lock/upload API for per-frame texture updates**

```bash
grep -n "gos_LockTexture\|gos_NewTextureFromMemory\|gos_NewEmptyTexture\|gos_NewTexture\b" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.cpp \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.h
```

- [ ] **Step 3: Check whether the GL texture ID inside a gos texture is retrievable**

```bash
grep -n "gos_GetTextureName\|gos_TextureToGL\|textureHandle.*GLuint" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/*.cpp
```

- [ ] **Step 4: Decide one of three paths and record**

Write the decision to `docs/superpowers/plans/progress/2026-04-23-video-gl-integration.md`:

```
Path GL-A (preferred): gos_NewEmptyTexture(RGBA, w, h) + gos_LockTexture
per-frame exists. Use it. Tasks 9/10/11 store a DWORD gos handle, not
a GLuint; upload via gos_LockTexture/sws_scale-into-locked-ptr/
gos_UnlockTexture; draw via gos_DrawQuads. No raw GL in this feature.

Path GL-B: gos stores textures on the GPU but does not expose a lock
API. Allocate a raw GL texture, wrap it in a gos handle via the
existing back-channel (e.g. gosTexture::setGLName), upload with
glTexSubImage2D, draw through gos_DrawQuads. Tasks 9–11 keep the raw
GL texture code but additionally register a gos handle wrapping it.

Path GL-C: gos draw path is fully locked — no way to present our
texture through gos_DrawQuads. Drop into raw GL for the video layer:
set up a tiny dedicated VBO + shader program in Task 9, render in
Task 11 using glUseProgram/glDrawArrays, making sure to save/restore
any gos-side state that shares the fixed-function pipeline.
```

- [ ] **Step 5: Commit the decision**

```bash
git add docs/superpowers/plans/progress/2026-04-23-video-gl-integration.md
git commit -m "chore(video): render integration decision (GL-A / GL-B / GL-C)"
```

Tasks 9, 10, 11 below assume Path GL-A as the default phrasing; the decision record tells the implementer which parts to substitute if GL-B or GL-C was chosen. Specifically:

- **Task 9 Step 4** ("GL texture alloc"): replace with `gos_NewEmptyTexture` + storing `DWORD gosHandle` if GL-A; extend with a gos-handle wrap step if GL-B; keep raw GL + also create VBO/shader if GL-C.
- **Task 10 Step 3** (`uploadFrameToTexture`): replace `glTexSubImage2D` with `gos_LockTexture`/copy/`gos_UnlockTexture` if GL-A; keep raw-GL if GL-B or GL-C.
- **Task 11** (`video_render`): use `gos_DrawQuads` for GL-A and GL-B; use raw VBO draw for GL-C.

---

## Task 9: Implement `video_open` / `video_close` — FFmpeg demuxer + video decoder + texture alloc + quad math

**Files:**
- Modify: `code/mc2video.cpp`

- [ ] **Step 1: Add GL + session struct**

At top of `mc2video.cpp` after FFmpeg includes:

```cpp
#include <GL/glew.h>
```

Replace the placeholder `VideoSession` struct with:

```cpp
struct VideoSession {
    // Demuxer / video decoder
    AVFormatContext* fmt      = nullptr;
    int              vStream  = -1;
    AVCodecContext*  vCodec   = nullptr;
    SwsContext*      sws      = nullptr;
    AVFrame*         vFrame   = nullptr;
    AVFrame*         rgbFrame = nullptr;
    uint8_t*         rgbBuffer = nullptr;     // stable RGBA scratch
    int              srcW = 0, srcH = 0;
    double           sar = 1.0;
    double           fps = 30.0;
    double           timeBase = 0.0;          // seconds per PTS unit
    AVPacket*        pkt      = nullptr;

    // GL
    GLuint           tex      = 0;
    float            quadX0 = 0, quadY0 = 0, quadX1 = 0, quadY1 = 0;
    int              rectX = 0, rectY = 0, rectW = 0, rectH = 0;
    bool             frameReady = false;

    // Clock
    double           clockStart = 0.0;   // wall-clock at start/resume
    double           presentedPTS = -1.0;

    // Audio — Task 12 fills these in
    int              aStream = -1;
    AVCodecContext*  aCodec  = nullptr;
    SwrContext*      swr     = nullptr;

    // Lifecycle
    bool             paused = false;
    bool             eof    = false;
};
```

- [ ] **Step 2: Quad math helper**

```cpp
static void computeLetterboxQuad(int srcW, int srcH, double sar,
                                 int rectX, int rectY, int rectW, int rectH,
                                 float& x0, float& y0,
                                 float& x1, float& y1)
{
    if (rectW <= 0 || rectH <= 0 || srcW <= 0 || srcH <= 0) {
        x0 = (float)rectX; y0 = (float)rectY;
        x1 = (float)(rectX + rectW); y1 = (float)(rectY + rectH);
        return;
    }
    double srcAspectW = (double)srcW * sar;
    double scale = std::min((double)rectW / srcAspectW, (double)rectH / srcH);
    double quadW = srcAspectW * scale;
    double quadH = srcH * scale;
    double cx = rectX + rectW * 0.5;
    double cy = rectY + rectH * 0.5;
    x0 = (float)(cx - quadW * 0.5);
    y0 = (float)(cy - quadH * 0.5);
    x1 = (float)(cx + quadW * 0.5);
    y1 = (float)(cy + quadH * 0.5);
}
```

Add `#include <algorithm>` for `std::min`.

- [ ] **Step 3: `video_open` body**

```cpp
VideoSession* video_open(const VideoOpenParams& p, VideoOpenResult* out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!g_ffmpegAvailable || !p.resolvedPath) return nullptr;

    VideoSession* s = new VideoSession();

    // 1. Demux
    if (avformat_open_input(&s->fmt, p.resolvedPath, nullptr, nullptr) < 0) {
        VIDEO_LOG("avformat_open_input failed for '%s'", p.resolvedPath);
        video_close(s); return nullptr;
    }
    if (avformat_find_stream_info(s->fmt, nullptr) < 0) {
        VIDEO_LOG("avformat_find_stream_info failed for '%s'", p.resolvedPath);
        video_close(s); return nullptr;
    }

    // 2. Video stream
    s->vStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO,
                                      -1, -1, nullptr, 0);
    if (s->vStream < 0) {
        VIDEO_LOG("no video stream in '%s'", p.resolvedPath);
        video_close(s); return nullptr;
    }
    AVStream* vst = s->fmt->streams[s->vStream];
    const AVCodec* vcodec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (!vcodec) { video_close(s); return nullptr; }
    s->vCodec = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(s->vCodec, vst->codecpar);
    if (avcodec_open2(s->vCodec, vcodec, nullptr) < 0) {
        VIDEO_LOG("video decoder open failed"); video_close(s); return nullptr;
    }

    s->srcW = s->vCodec->width;
    s->srcH = s->vCodec->height;
    s->sar  = (vst->sample_aspect_ratio.num && vst->sample_aspect_ratio.den)
              ? av_q2d(vst->sample_aspect_ratio) : 1.0;
    s->fps  = (vst->avg_frame_rate.num && vst->avg_frame_rate.den)
              ? av_q2d(vst->avg_frame_rate) : 30.0;
    s->timeBase = av_q2d(vst->time_base);

    // 3. Sws
    s->sws = sws_getContext(s->srcW, s->srcH, s->vCodec->pix_fmt,
                             s->srcW, s->srcH, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!s->sws) { video_close(s); return nullptr; }

    s->vFrame   = av_frame_alloc();
    s->rgbFrame = av_frame_alloc();
    s->pkt      = av_packet_alloc();

    int rgbBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA,
                                             s->srcW, s->srcH, 1);
    s->rgbBuffer = (uint8_t*)av_malloc(rgbBytes);
    av_image_fill_arrays(s->rgbFrame->data, s->rgbFrame->linesize,
                         s->rgbBuffer, AV_PIX_FMT_RGBA,
                         s->srcW, s->srcH, 1);

    // 4. GL texture (NPOT RGBA8, sized to source)
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 s->srcW, s->srcH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 5. Quad geometry
    s->rectX = 0; s->rectY = 0;  // set by caller via video_setRect shortly
    s->rectW = p.destRectW;
    s->rectH = p.destRectH;
    computeLetterboxQuad(s->srcW, s->srcH, s->sar,
                         s->rectX, s->rectY, s->rectW, s->rectH,
                         s->quadX0, s->quadY0, s->quadX1, s->quadY1);

    // 6. Audio (Task 12) — for now just detect presence
    s->aStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO,
                                      -1, -1, nullptr, 0);

    // 7. Fill out
    if (out) {
        out->srcW = s->srcW; out->srcH = s->srcH;
        out->sar  = s->sar;  out->fps  = s->fps;
        out->hasAudio = (s->aStream >= 0);
        out->hasAlpha = (s->vCodec->pix_fmt == AV_PIX_FMT_YUVA420P ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_ARGB ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_RGBA);
        out->glTextureId = s->tex;
        out->quadX0 = s->quadX0; out->quadY0 = s->quadY0;
        out->quadX1 = s->quadX1; out->quadY1 = s->quadY1;
    }

    // 8. Clock
    s->clockStart = (double)av_gettime() / 1000000.0;
    s->presentedPTS = -1.0;

    return s;
}
```

- [ ] **Step 4: `video_close` body**

```cpp
void video_close(VideoSession* s)
{
    if (!s) return;
    if (s->tex)       { glDeleteTextures(1, &s->tex); s->tex = 0; }
    if (s->rgbBuffer) { av_free(s->rgbBuffer); s->rgbBuffer = nullptr; }
    if (s->rgbFrame)  { av_frame_free(&s->rgbFrame); }
    if (s->vFrame)    { av_frame_free(&s->vFrame); }
    if (s->pkt)       { av_packet_free(&s->pkt); }
    if (s->sws)       { sws_freeContext(s->sws); s->sws = nullptr; }
    if (s->vCodec)    { avcodec_free_context(&s->vCodec); }
    if (s->aCodec)    { avcodec_free_context(&s->aCodec); }
    if (s->swr)       { swr_free(&s->swr); }
    if (s->fmt)       { avformat_close_input(&s->fmt); }
    delete s;
}
```

- [ ] **Step 5: Build + deploy + launch**

Expected: main-menu intro opens successfully (`[VIDEO] init: opened` line with dimensions). Screen stays black where the movie would be — no frames decoded yet, next task.

- [ ] **Step 6: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): video_open/close — demux, decode init, texture alloc, letterbox quad"
```

---

## Task 10: Implement `video_update` — decode loop + sws_scale + texture upload

**Files:**
- Modify: `code/mc2video.cpp`

- [ ] **Step 1: Master-clock helper**

```cpp
static double nowSeconds() {
    return (double)av_gettime() / 1000000.0;
}

// Returns the presentation time for the next frame we want to show.
// Wall-clock for now; Task 13 replaces with audio-master when available.
static double videoMasterClock(const VideoSession* s) {
    return nowSeconds() - s->clockStart;
}
```

- [ ] **Step 2: Decode-one-frame helper**

```cpp
// Pulls packets and decodes until one video frame is produced or EOF.
// Returns: 1 = frame produced, 0 = more packets needed (caller retries
// on next update), -1 = EOF.
static int decodeNextVideoFrame(VideoSession* s)
{
    for (;;) {
        int ret = avcodec_receive_frame(s->vCodec, s->vFrame);
        if (ret == 0) return 1;
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return -1;

        // Need more input
        ret = av_read_frame(s->fmt, s->pkt);
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(s->vCodec, nullptr);  // flush
            ret = avcodec_receive_frame(s->vCodec, s->vFrame);
            return (ret == 0) ? 1 : -1;
        }
        if (ret < 0) return -1;

        if (s->pkt->stream_index == s->vStream) {
            avcodec_send_packet(s->vCodec, s->pkt);
        }
        // audio packets handled in Task 12; for now discard
        av_packet_unref(s->pkt);
    }
}
```

- [ ] **Step 3: Upload helper**

```cpp
static void uploadFrameToTexture(VideoSession* s)
{
    sws_scale(s->sws,
              s->vFrame->data, s->vFrame->linesize,
              0, s->srcH,
              s->rgbFrame->data, s->rgbFrame->linesize);

    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    s->srcW, s->srcH,
                    GL_RGBA, GL_UNSIGNED_BYTE, s->rgbBuffer);
    glBindTexture(GL_TEXTURE_2D, 0);
    s->frameReady = true;
}
```

- [ ] **Step 4: Extend `VideoSession` with a pending-frame slot**

Add these fields alongside the existing video-decoder members:

```cpp
    bool     pendingFrameValid = false;   // vFrame holds a decoded-but-not-yet-due frame
    double   pendingFramePTS   = 0.0;     // PTS of that frame in seconds
```

The decoded frame lives in `s->vFrame` (already allocated). When `pendingFrameValid` is true we keep it until its PTS is due, and only then upload+present. That is the critical rule: **never upload a frame before its PTS has arrived on the master clock**, or playback will run visually early at low fps.

- [ ] **Step 5: `video_update` body — hold future frames, drop late ones**

```cpp
bool video_update(VideoSession* s)
{
    if (!s || s->eof) return true;
    if (s->paused) return false;

    const double masterNow = videoMasterClock(s);

    // Step A: if we have a pending frame, check whether it is now due.
    if (s->pendingFrameValid) {
        if (s->pendingFramePTS > masterNow) {
            // Still in the future — hold, do not upload, do not present.
            return false;
        }
        // Due. Promote to the display texture.
        uploadFrameToTexture(s);
        s->presentedPTS = s->pendingFramePTS;
        s->pendingFrameValid = false;
    }

    // Step B: pull frames from the decoder. If we fall behind, we will
    // repeatedly find late frames — discard them without uploading
    // until we reach one that is due or in the future. Only the final
    // "due" frame uploads; the final "future" frame parks in the
    // pending slot.
    for (;;) {
        int r = decodeNextVideoFrame(s);
        if (r < 0) { s->eof = true; return true; }

        double pts = (s->vFrame->pts == AV_NOPTS_VALUE)
                     ? (s->presentedPTS + 1.0 / s->fps)
                     : s->vFrame->pts * s->timeBase;

        if (pts > masterNow) {
            // Future frame — park and return. Do NOT upload yet.
            s->pendingFrameValid = true;
            s->pendingFramePTS   = pts;
            return false;
        }

        // Frame is due or late. Check whether the NEXT decoded frame
        // is also late — if so, drop this one without uploading and
        // keep pulling. Otherwise upload and look for the future
        // frame to park.
        //
        // Cheap heuristic: if pts is more than (2 / fps) behind master,
        // assume the next frame is also late and drop without upload.
        const double lateThreshold = 2.0 / (s->fps > 0 ? s->fps : 30.0);
        if (masterNow - pts > lateThreshold) {
            // drop silently; vFrame gets overwritten on next decodeNextVideoFrame
            continue;
        }

        // Approximately on-time: upload and continue to look for the
        // future frame to park.
        uploadFrameToTexture(s);
        s->presentedPTS = pts;
        // loop; next iteration will either park a future frame or
        // continue catching up
    }
}
```

This path guarantees: (1) a frame is never uploaded before its PTS is due on the master clock, (2) severely late frames are dropped without GPU cost, (3) on-time frames are always presented, (4) the future frame always parks so we return promptly from `update()`.

- [ ] **Step 6: Build, deploy, launch — first playing frames!**

Expected: when main menu starts, the MSFT intro plays. Audio is absent (Task 12) but video should animate at ~15–30 fps. The video is drawn via `video_render` which is still a stub — so strictly speaking you won't see it yet. Proceed directly to Task 11 before visual confirmation.

- [ ] **Step 7: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): decode loop with frame drop + RGBA upload"
```

---

## Task 11: Implement `video_render` — draw the screen-space quad

**Files:**
- Modify: `code/mc2video.cpp`

Integration path was already chosen in Task 8b. This task writes `video_render` against that path. The signature of the draw call is known at task start; no mid-task architecture fork.

- [ ] **Step 1: Write `video_render` against the chosen path**

```cpp
void video_render(VideoSession* s)
{
    if (!s || !s->frameReady || s->tex == 0) return;

    // Draw a textured quad at (quadX0,quadY0)-(quadX1,quadY1) in
    // screen-space coordinates. Match the existing UI draw path.
    //
    // Pseudocode — replace with the concrete gos_DrawQuads call
    // discovered in Step 1:
    //
    //   gos_VERTEX v[4] = { ... };
    //   v[0].x = s->quadX0; v[0].y = s->quadY0; v[0].u = 0; v[0].v = 0;
    //   v[1].x = s->quadX1; v[1].y = s->quadY0; v[1].u = 1; v[1].v = 0;
    //   v[2].x = s->quadX1; v[2].y = s->quadY1; v[2].u = 1; v[2].v = 1;
    //   v[3].x = s->quadX0; v[3].y = s->quadY1; v[3].u = 0; v[3].v = 1;
    //   gos_SetRenderState(gos_State_Texture, /* gos handle */);
    //   gos_DrawQuads(v, 4);

    // Letterbox fill: before drawing the video quad, draw the
    // full MC2Rect in black, then the video quad on top. Only
    // needed if the source aspect does not fill the destination.
    if (s->quadX0 > s->rectX || s->quadY0 > s->rectY ||
        s->quadX1 < s->rectX + s->rectW || s->quadY1 < s->rectY + s->rectH) {
        // draw black MC2Rect quad (see UI path)
    }

    // draw textured video quad using s->tex
}
```

The fallback rectangle fill (black letterbox bars) uses the same draw API as the video quad — one black-untextured quad covering `MC2Rect` before the video quad is drawn on top.

- [ ] **Step 2: Build, deploy, launch**

Expected: MSFT intro plays visibly. Black pillarboxes on 16:9 display for the 4:3 source. No audio yet.

- [ ] **Step 3: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): video_render — letterboxed screen-space quad via gos draw path"
```

---

## Task 12: Audio pipeline — decode + resample + push to mixer (branched by P1 outcome)

**Files:** varies by path chosen in Task 2.

### If Task 2 selected Path A (gosAudio has push-PCM + position):

- [ ] **Step 1: Extend `VideoSession` with audio members**

```cpp
    int              aChannels    = 0;
    int              aSampleRate  = 0;
    AVSampleFormat   aOutFormat   = AV_SAMPLE_FMT_S16;  // or mixer native
    double           aStartClock  = 0.0;    // master clock at audio start
    int64_t          aSamplesFed  = 0;
```

- [ ] **Step 2: Open audio decoder in `video_open`**

After the video stream block:

```cpp
if (s->aStream >= 0 && !p.useWaveFile) {
    AVStream* ast = s->fmt->streams[s->aStream];
    const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
    if (ac) {
        s->aCodec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(s->aCodec, ast->codecpar);
        if (avcodec_open2(s->aCodec, ac, nullptr) == 0) {
            // Query mixer native format
            int rate = 0, channels = 0;
            AVSampleFormat fmt = AV_SAMPLE_FMT_S16;
            gosAudio_QueryFormat(&rate, &channels, &fmt);  // real name TBD from P1 spike
            s->aSampleRate = rate;
            s->aChannels   = channels;
            s->aOutFormat  = fmt;

            s->swr = swr_alloc();
            av_opt_set_int(s->swr, "in_channel_count",  s->aCodec->ch_layout.nb_channels, 0);
            av_opt_set_int(s->swr, "out_channel_count", channels, 0);
            av_opt_set_int(s->swr, "in_sample_rate",    s->aCodec->sample_rate, 0);
            av_opt_set_int(s->swr, "out_sample_rate",   rate, 0);
            av_opt_set_sample_fmt(s->swr, "in_sample_fmt",  s->aCodec->sample_fmt, 0);
            av_opt_set_sample_fmt(s->swr, "out_sample_fmt", fmt, 0);
            swr_init(s->swr);
        } else {
            avcodec_free_context(&s->aCodec);
        }
    }
}
```

- [ ] **Step 3: In `decodeNextVideoFrame`, route audio packets to audio decoder + resampler + push**

Replace the "audio packets discarded" comment with:

```cpp
if (s->pkt->stream_index == s->aStream && s->aCodec && s->swr) {
    avcodec_send_packet(s->aCodec, s->pkt);
    AVFrame* af = av_frame_alloc();
    while (avcodec_receive_frame(s->aCodec, af) == 0) {
        // Resample
        int outSamplesMax = (int)av_rescale_rnd(
            swr_get_delay(s->swr, s->aCodec->sample_rate) + af->nb_samples,
            s->aSampleRate, s->aCodec->sample_rate, AV_ROUND_UP);
        int outBytes = av_samples_get_buffer_size(
            nullptr, s->aChannels, outSamplesMax, s->aOutFormat, 1);
        uint8_t* outBuf = (uint8_t*)av_malloc(outBytes);
        uint8_t* outPtr = outBuf;
        int outSamples = swr_convert(s->swr, &outPtr, outSamplesMax,
                                     (const uint8_t**)af->data, af->nb_samples);
        if (outSamples > 0) {
            gosAudio_PushPCM(outBuf, outSamples, s->aSampleRate,
                             s->aChannels, s->aOutFormat);  // P1-discovered name
            s->aSamplesFed += outSamples;
        }
        av_free(outBuf);
    }
    av_frame_free(&af);
}
```

- [ ] **Step 4: Build, deploy, confirm audio plays**

Expected: intro plays with audio. Slight A/V drift until Task 13 wires audio-master clock.

### If Task 2 selected Path B (Mix_HookMusic adapter):

- [ ] **Step 1: Add `SoundSystem::pushVideoPCM` adapter**

Open `mclib/soundsys.h`, add to `public:`:

```cpp
    // Starts or continues pushing PCM for video playback. Exclusive
    // with background music while active. rate/channels/fmt match
    // the mixer's native spec (query via queryNativeFormat).
    bool beginVideoPCMStream(int rate, int channels /* 1 or 2 */);
    void pushVideoPCMSamples(const int16_t* samples, int frameCount);
    void endVideoPCMStream();
    int  videoPCMSamplesConsumed() const;   // monotonic for sync
    bool queryNativeFormat(int* rate, int* channels) const;
```

In `mclib/soundsys.cpp` implement:

```cpp
#include <SDL_mixer.h>
#include <SDL_atomic.h>
#include <vector>

namespace {
    struct VideoAudioState {
        std::vector<int16_t>  ring;          // interleaved samples
        size_t                cap = 0;
        SDL_SpinLock          lock = 0;
        size_t                head = 0;      // producer index
        size_t                tail = 0;      // consumer index
        SDL_atomic_t          consumedFrames;// updated in callback
        int                   rate = 0;
        int                   channels = 0;
        bool                  active = false;
    } g_va;

    static void videoMixHook(void* /*udata*/, Uint8* stream, int len)
    {
        int16_t* out = (int16_t*)stream;
        int outSamples = len / sizeof(int16_t);
        SDL_AtomicLock(&g_va.lock);
        for (int i = 0; i < outSamples; ++i) {
            if (g_va.tail == g_va.head) {
                out[i] = 0;    // underrun = silence
            } else {
                out[i] = g_va.ring[g_va.tail];
                g_va.tail = (g_va.tail + 1) % g_va.cap;
            }
        }
        SDL_AtomicUnlock(&g_va.lock);
        int framesWritten = outSamples / (g_va.channels ? g_va.channels : 1);
        SDL_AtomicAdd(&g_va.consumedFrames, framesWritten);
    }
}

bool SoundSystem::queryNativeFormat(int* rate, int* channels) const {
    Uint16 fmt; int ch;
    if (Mix_QuerySpec(rate, &fmt, &ch) == 0) return false;
    if (channels) *channels = ch;
    return true;
}

bool SoundSystem::beginVideoPCMStream(int rate, int channels)
{
    if (g_va.active) return false;
    g_va.rate = rate; g_va.channels = channels;
    g_va.cap  = (size_t)rate * channels * 2;   // 2 s buffer
    g_va.ring.assign(g_va.cap, 0);
    g_va.head = g_va.tail = 0;
    SDL_AtomicSet(&g_va.consumedFrames, 0);
    Mix_HookMusic(videoMixHook, nullptr);
    g_va.active = true;
    return true;
}

void SoundSystem::pushVideoPCMSamples(const int16_t* samples, int frameCount)
{
    if (!g_va.active || !samples) return;
    int interleaved = frameCount * g_va.channels;
    SDL_AtomicLock(&g_va.lock);
    for (int i = 0; i < interleaved; ++i) {
        size_t next = (g_va.head + 1) % g_va.cap;
        if (next == g_va.tail) break;        // full — drop
        g_va.ring[g_va.head] = samples[i];
        g_va.head = next;
    }
    SDL_AtomicUnlock(&g_va.lock);
}

void SoundSystem::endVideoPCMStream()
{
    if (!g_va.active) return;
    Mix_HookMusic(nullptr, nullptr);
    g_va.active = false;
    g_va.ring.clear();
}

int SoundSystem::videoPCMSamplesConsumed() const {
    return SDL_AtomicGet((SDL_atomic_t*)&g_va.consumedFrames);
}
```

Critical notes:

- `Mix_HookMusic` takes over the music channel — any `Mix_PlayMusic` call while active is ignored by the mixer. That's acceptable because cutscenes are the only audio during playback in MC2.
- `queryNativeFormat` returns PCM-S16 because that's what `Mix_HookMusic` receives. If `Mix_QuerySpec` reports a different format, this plan does NOT convert — it assumes S16. Fail fast in `beginVideoPCMStream` with an error log if the queried format is not `AUDIO_S16SYS`.
- The ring buffer capacity (2 seconds) is generous; shrink to 0.5 s if memory matters.
- `SDL_SpinLock` is audio-thread-safe and lighter than a mutex. Contention is minimal (producer = main thread at ~60 Hz, consumer = audio thread at ~40 Hz).

- [ ] **Step 2: In `video_open`, call `soundSystem->queryNativeFormat` and `beginVideoPCMStream`; in `decodeNextVideoFrame` call `pushVideoPCMSamples`; in `video_close` call `endVideoPCMStream`.**

Same shape as Path A but with the adapter names.

- [ ] **Step 3: Build, deploy, confirm audio plays**

### If Task 2 selected Path C (decode-to-temp-WAV):

- [ ] **Step 1: In `video_open`, if `aStream >= 0 && !useWaveFile`, decode the entire audio stream into a temp WAV**

Use Windows `GetTempPathA`+`GetTempFileNameA` for the path. Write a standard 16-bit PCM WAV. Close it, then pass its path to `soundSystem->playDigitalStream`.

- [ ] **Step 2: Mark the temp file for deletion in `video_close` via `DeleteFileA`.**

- [ ] **Step 3: Note the A/V drift tolerance.** Path C uses wall-clock sync only. Document in the spec's "P1 findings" appendix that audio-master sync is not achievable on this build.

- [ ] **Commit (regardless of path)**

```bash
git add code/mc2video.cpp mclib/soundsys.h mclib/soundsys.cpp
git commit -m "feat(video): audio pipeline (resample + push to mixer, path $PATH_LETTER from P1)"
```

Substitute `$PATH_LETTER` with A/B/C.

---

## Task 13: A/V sync — audio master clock with wall-clock fallback

**Files:**
- Modify: `code/mc2video.cpp`

Applies only if Task 12 Path is A or B. Path C skips (already wall-clock).

- [ ] **Step 1: Implement `audioMasterClock`**

```cpp
static double audioMasterClock(const VideoSession* s)
{
    // Path A: gosAudio_GetSamplesPlayed(s->audioChannel)
    // Path B: soundSystem->videoPCMSamplesConsumed()
    int64_t consumed = /* chosen accessor */;
    return (double)consumed / (double)s->aSampleRate;
}
```

- [ ] **Step 2: Replace `videoMasterClock` body with audio-master + fallback**

```cpp
static double videoMasterClock(const VideoSession* s) {
    if (s->aCodec && s->aStream >= 0 && !s->audioStallLogged) {
        return audioMasterClock(s);
    }
    return nowSeconds() - s->clockStart;
}
```

Add `bool audioStallLogged = false;` to `VideoSession`. The handoff flip is one-shot — set `audioStallLogged = true` the first time the audio decoder returns a fatal error or the consumed-sample counter stalls for more than 2× the buffer duration. Do not reset it even if audio recovers (spec clock-handoff rule).

- [ ] **Step 3: Audio stall detection**

In `decodeNextVideoFrame`, when the audio side fails (swr_convert < 0, push returns false, or audio-decode error), call:

```cpp
static void markAudioStalled(VideoSession* s, const char* reason) {
    if (!s->audioStallLogged) {
        VIDEO_LOG("audio stalled (%s); master clock fallback to wall-clock", reason);
        // Freeze the wall-clock baseline at the current audio PTS so
        // we do NOT retroactively correct backward.
        double lastAudioSec = (s->aSampleRate > 0)
                              ? (double)s->aSamplesFed / s->aSampleRate
                              : 0.0;
        s->clockStart = nowSeconds() - lastAudioSec;
        s->audioStallLogged = true;
    }
}
```

- [ ] **Step 4: Build, deploy, confirm A/V stays in sync across a full intro play**

- [ ] **Step 5: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): audio-master A/V clock with one-shot wall-clock fallback"
```

---

## Task 14: Audio-source precedence — `useWaveFile` sidecar vs embedded

**Files:**
- Modify: `code/mc2video.cpp` (`video_open`)

- [ ] **Step 1: Concretize the precedence ladder inside `video_open`**

Replace the existing audio-init block in `video_open` (added in Task 12) with an explicit two-stage ladder. Track the outcome of each attempt with a local bool so the second stage only runs if the first failed:

```cpp
// Returns true if a WAV sidecar was successfully started. Wraps the
// existing SoundSystem::playDigitalStream (returns NO_ERR / -1).
static bool tryStartSidecarWAV(const char* waveShortName) {
    if (!soundSystem || !waveShortName) return false;
    if (prefs.DigitalMasterVolume == 0)  return false;   // muted → treat as n/a
    long r = soundSystem->playDigitalStream(waveShortName);
    return (r == NO_ERR);
}

// Returns true if an embedded audio stream was found AND its decoder
// opened. Fills in s->aStream/aCodec/swr as Task 12 described.
static bool tryOpenEmbeddedAudio(VideoSession* s) {
    s->aStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO,
                                      -1, -1, nullptr, 0);
    if (s->aStream < 0) return false;
    // ... Task 12's audio-decoder-open code goes here ...
    // return true only after swr_init succeeds; on any failure, tear
    // down s->aCodec/s->swr and reset s->aStream = -1 before returning
    // false.
    return /* true iff decoder + swr fully opened */;
}

// Precedence ladder (per spec §Audio-source precedence):
//   1. useWaveFile=true  -> prefer sidecar WAV
//   2. useWaveFile=false -> prefer embedded audio
//   3. if preferred fails, fall back to the other source
//   4. if both fail, play silent video
bool sidecarOK = false;
bool embeddedOK = false;
if (p.useWaveFile) {
    sidecarOK = tryStartSidecarWAV(p.waveFileShortName);
    if (!sidecarOK) {
        embeddedOK = tryOpenEmbeddedAudio(s);
        if (!embeddedOK) {
            VIDEO_LOG("audio: sidecar and embedded both unavailable; silent video");
        }
    }
} else {
    embeddedOK = tryOpenEmbeddedAudio(s);
    if (!embeddedOK) {
        sidecarOK = tryStartSidecarWAV(p.waveFileShortName);
        if (!sidecarOK) {
            VIDEO_LOG("audio: embedded and sidecar both unavailable; silent video");
        }
    }
}
```

Fallback success detection is now concrete in both directions:

| Preferred | Detection of "preferred failed" |
|---|---|
| sidecar WAV (`useWaveFile=true`) | `playDigitalStream` returned non-`NO_ERR`, or `DigitalMasterVolume==0`, or `soundSystem` null |
| embedded audio (`useWaveFile=false`) | `av_find_best_stream < 0` OR decoder/swr open failed |

No placeholder status API is needed. `playDigitalStream` already returns `long` (`NO_ERR=0`/`-1`) per `soundsys.cpp:779` — we treat `NO_ERR` as success and any other value as failure.

**Parameter naming:** this block lives entirely in `video_open(const VideoOpenParams& p, ...)` — use `p.useWaveFile` and `p.waveFileShortName`. The `waveName` member on `MC2Movie` is irrelevant here; `MC2Movie::init` already passed its value through `VideoOpenParams` in Task 8.

- [ ] **Step 2: Remove any Task 12 code that opened embedded audio unconditionally**

The Task 12 audio-open block unconditionally called `av_find_best_stream`/`avcodec_open2`. Now that ladder-gated, delete the standalone open block and rely on `tryOpenEmbeddedAudio()` being called from the ladder.

- [ ] **Step 3: Test with a callsite that passes `useWaveFile=true`**

Pick one of the `forcegroupbar.cpp` pilot-cam clips whose companion `.wav` exists. Launch, confirm sidecar audio plays.

- [ ] **Step 4: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): audio-source precedence — sidecar vs embedded with fallback"
```

---

## Task 15: Lifecycle — pause / stop / restart / setRect

**Files:**
- Modify: `code/mc2video.cpp`

- [ ] **Step 1: `video_pause`**

```cpp
void video_pause(VideoSession* s, bool paused) {
    if (!s || s->paused == paused) return;
    s->paused = paused;
    if (paused) {
        // Freeze wall-clock and audio
        s->pausedAt = nowSeconds();   // add field to VideoSession
        if (s->aCodec) { /* pause audio stream per chosen path */ }
    } else {
        // Resume: shift clock origin forward by pause duration
        double delta = nowSeconds() - s->pausedAt;
        s->clockStart += delta;
        // Spec: up to one video frame of catch-up permitted
        if (s->aCodec) { /* resume audio stream per chosen path */ }
    }
}
```

- [ ] **Step 2: `video_stop`**

```cpp
void video_stop(VideoSession* s) {
    if (!s) return;
    s->eof = true;
    if (s->aCodec) { /* flush audio stream */ }
    // Texture is retained until video_close (destructor).
}
```

- [ ] **Step 3: `video_restart`**

```cpp
void video_restart(VideoSession* s) {
    if (!s) return;
    // Seek container to start
    av_seek_frame(s->fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(s->vCodec);
    if (s->aCodec) avcodec_flush_buffers(s->aCodec);
    s->eof = false;
    s->paused = false;
    s->frameReady = false;
    s->presentedPTS = -1.0;
    s->aSamplesFed = 0;
    s->audioStallLogged = false;
    s->clockStart = nowSeconds();
    // Re-arm audio push (Path A/B/C specific).
}
```

- [ ] **Step 4: `video_setRect`**

```cpp
void video_setRect(VideoSession* s, int x0, int y0, int w, int h)
{
    if (!s) return;
    if (w <= 0 || h <= 0) {
        VIDEO_LOG("setRect: degenerate rect ignored (w=%d h=%d)", w, h);
        return;
    }
    s->rectX = x0; s->rectY = y0; s->rectW = w; s->rectH = h;
    computeLetterboxQuad(s->srcW, s->srcH, s->sar,
                         s->rectX, s->rectY, s->rectW, s->rectH,
                         s->quadX0, s->quadY0, s->quadX1, s->quadY1);
}
```

Remove the `STOP(("Tried to change MC2 Movie Rect size..."))` call from `MC2Movie::setRect` in `mc2movie.cpp` — the resize-is-fatal rule is obsolete.

- [ ] **Step 5: Build, deploy**

Manual test: start intro, alt-tab the window to trigger pause (if MC2 pauses on focus-loss — check existing behavior). Resume. Confirm no discontinuity beyond one-frame catch-up.

- [ ] **Step 6: Commit**

```bash
git add code/mc2video.cpp code/mc2movie.cpp
git commit -m "feat(video): lifecycle — pause/stop/restart/setRect with resize-allowed"
```

---

## Task 16: Failure handling polish + one-log-per-class policy

**Files:**
- Modify: `code/mc2video.cpp`

- [ ] **Step 1: Add log-once flags to `VideoSession`**

```cpp
    // audioStallLogged already added in Task 13 — reuse it
    bool loggedDegenerateRect = false;
    bool loggedVideoDecodeError = false;
```

- [ ] **Step 2: Gate `setRect` degenerate log**

```cpp
if (w <= 0 || h <= 0) {
    if (!s->loggedDegenerateRect) {
        VIDEO_LOG("setRect: degenerate rect ignored (w=%d h=%d)", w, h);
        s->loggedDegenerateRect = true;
    }
    return;
}
```

- [ ] **Step 3: Gate video-decode-error logging in `decodeNextVideoFrame`**

Replace raw `return -1` with a log-once emission that includes `av_strerror()`.

- [ ] **Step 4: Confirm no per-frame logging on a 30-second play**

```bash
MC2_DEBUG_VIDEO=1 ./mc2.exe  2>&1 | grep "\[VIDEO\]" | wc -l
```

Expected: fewer than ~10 lines for a clean run (open, close, resolver hits, clock handoff only if it occurs).

- [ ] **Step 5: Commit**

```bash
git add code/mc2video.cpp
git commit -m "feat(video): log-once failure policy per session instance"
```

---

## Task 17: Callsite fossil audit — fix hardcoded rects

**Files:**
- Modify: `code/logistics.cpp` (line 307)
- Audit-only (no expected change): `code/mainmenu.cpp`, `code/missionselectionscreen.cpp`, `code/controlgui.cpp`, `code/forcegroupbar.cpp`

- [ ] **Step 1: Fix `code/logistics.cpp:307`**

Change:

```cpp
movieRect.bottom = 600;
```

to:

```cpp
movieRect.bottom = Environment.screenHeight;
```

Add `#include "platform_windows.h"` or confirm `Environment` is already in scope via existing includes at the top of the file.

- [ ] **Step 2: Audit the other four callsites**

```bash
grep -n "movieRect\|videoRect\|aviPath\|MC2Movie" \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/mainmenu.cpp \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/missionselectionscreen.cpp \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/controlgui.cpp \
  A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/forcegroupbar.cpp
```

For each rect, check whether width/height comes from `Environment.screen*` (good) or a literal (fossil). If a fossil is found, change it to `Environment.screen*` using the same pattern as `mainmenu.cpp:150-154`. Do NOT refactor layout math beyond that.

- [ ] **Step 3: Build, deploy, quick-launch each callsite**

Main menu intro, logistics credits (start and complete any mission to reach credits), one briefing cinema, one pilot-cam pop-up (in-mission).

- [ ] **Step 4: Commit**

```bash
git add code/logistics.cpp code/mainmenu.cpp code/missionselectionscreen.cpp code/controlgui.cpp code/forcegroupbar.cpp
git commit -m "fix(video): replace hardcoded movie-rect fossils with screen-space dimensions"
```

Only include files actually modified.

---

## Task 18: Deploy integration — install FFmpeg DLLs next to `mc2.exe`

**Files:**
- Modify: `CMakeLists.txt` (install rule)
- Modify: `.claude/skills/mc2-deploy.md` (skill sees the new DLLs)

- [ ] **Step 1: Add a CMake install rule**

After the `target_link_libraries(mc2 ...)` block in `CMakeLists.txt`, add:

```cmake
# FFmpeg runtime DLLs — must sit next to mc2.exe (delay-load pattern
# does not support subdirectories without SetDllDirectory calls).
foreach(_dll ${FFmpegLGPL_RUNTIME_DLLS})
    add_custom_command(TARGET mc2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:mc2>")
endforeach()
install(FILES ${FFmpegLGPL_RUNTIME_DLLS} DESTINATION ".")
```

- [ ] **Step 2: Update the deploy skill**

Open `.claude/skills/mc2-deploy.md`. Find the section that copies build artifacts and add (near the `mc2.exe` copy line):

```
Copy FFmpeg DLLs next to mc2.exe:
  for dll in avcodec-*.dll avformat-*.dll avutil-*.dll swscale-*.dll swresample-*.dll; do
    cp -f "build64/RelWithDebInfo/$dll" "$DEPLOY_DIR/"
    diff -q "build64/RelWithDebInfo/$dll" "$DEPLOY_DIR/$dll"
  done
```

(Per CLAUDE.md critical rule: per-file `cp -f` + `diff -q`, never `cp -r`.)

- [ ] **Step 3: Clean build + deploy, verify DLLs land in deploy dir**

```bash
ls /a/Games/mc2-opengl/mc2-win64-v0.1.1/avcodec-*.dll
ls /a/Games/mc2-opengl/mc2-win64-v0.1.1/avformat-*.dll
ls /a/Games/mc2-opengl/mc2-win64-v0.1.1/avutil-*.dll
ls /a/Games/mc2-opengl/mc2-win64-v0.1.1/swscale-*.dll
ls /a/Games/mc2-opengl/mc2-win64-v0.1.1/swresample-*.dll
```

Expected: all five present.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt .claude/skills/mc2-deploy.md
git commit -m "build(video): install FFmpeg DLLs next to mc2.exe; deploy skill copies them"
```

---

## Task 19: LGPL notice + release asset

**Files:**
- Create: `release_assets/LICENSE.FFmpeg.txt`

- [ ] **Step 1: Write the notice**

```
This distribution uses FFmpeg LGPL-licensed shared libraries
(avcodec, avformat, avutil, swscale, swresample) for video playback.

Version pinned in 3rdparty/ffmpeg-lgpl-win64/VERSION.txt of the
source distribution.

FFmpeg is free software licensed under the GNU Lesser General Public
License v2.1 or (at your option) any later version. The full license
text accompanies the FFmpeg source distribution available at:

    https://github.com/FFmpeg/FFmpeg

LGPL replacement mechanism: the FFmpeg DLLs shipped alongside mc2.exe
(avcodec-*.dll, avformat-*.dll, avutil-*.dll, swscale-*.dll,
swresample-*.dll) may be replaced with any API-compatible build of
the same major versions. No rebuild of mc2.exe is required. See
3rdparty/ffmpeg-lgpl-win64/VERSION.txt for the expected filenames.

Source for the exact binary build shipped here:
https://github.com/BtbN/FFmpeg-Builds
```

- [ ] **Step 2: Commit**

```bash
git add release_assets/LICENSE.FFmpeg.txt
git commit -m "docs(video): LGPL replacement notice for shipped FFmpeg DLLs"
```

---

## Task 20: Definition-of-done verification walkthrough

No code changes in this task. Work through every DoD item and record results in `docs/superpowers/plans/progress/2026-04-23-video-dod.md`.

- [ ] **DoD 1: MSFT intro at 4K fullscreen**

Launch at 3840×2160 fullscreen. Watch the intro. Expected: plays, pillarboxed (4:3 source centered on 16:9 screen), audio present, no stutter beyond occasional GPU-caused frame drop.

- [ ] **DoD 2: Logistics briefings + credits**

Start a campaign, complete first mission to reach a briefing cinema. Finish the campaign (or load the end state) to trigger credits. Expected: both play with correct aspect and audio.

- [ ] **DoD 3: MP4 override swap**

Place a hand-made MP4 at `data/movies/msft.mp4` (produced offline via `ffmpeg -i msft.bik -c:v libx264 -crf 16 data/movies/msft.mp4`). Launch. Expected: intro plays the MP4 (verify by recompressing at a visibly different CRF or different aspect).

- [ ] **DoD 4: `UseUpscaledVideos=false` forces originals**

Edit `options.pref`, set `UseUpscaledVideos = 0`. Launch. Expected: `.bik` plays even though `data/movies/msft.mp4` is present.

- [ ] **DoD 5: Multi-resolution source**

Hand-produce three variants of `msft.mp4` at 320×240, 640×480, 2560×1920. For each, launch, confirm it plays correctly (aspect preserved, filling the screen vertically on 16:9 for 4:3 source).

- [ ] **DoD 6: `setRect` during playback**

Locate the one callsite that resizes: none known. If none exists, manually test via a temporary debug hotkey that calls `setRect` on an active movie with a rect of different aspect. Expected: quad re-fits, no crash.

- [ ] **DoD 7: Pause/resume**

Alt-tab during intro (or trigger whatever pauses MC2). Resume. Expected: audio resumes cleanly, video catches up within ≤1 frame.

- [ ] **DoD 8: Missing DLLs**

Rename all five FFmpeg DLLs in the deploy dir to `.dll.bak`. Launch. Expected: game starts, main menu appears, one `[VIDEO]` log line on first movie attempt, no crash. Restore DLLs.

- [ ] **DoD 9: Corrupted MP4 fallback**

Place a 1-byte file at `data/movies/msft.mp4`. Launch. Expected: one `[VIDEO] init: open failed` line, intro plays from `.bik`.

- [ ] **DoD 10: All sources missing**

Remove `msft.bik` from FST (back it up first), remove `data/movies/msft.*`. Launch. Expected: no crash, main menu appears immediately.

- [ ] **DoD 11: Per-frame alloc check**

Run a 60-second playback with console logging on. Inspect with Tracy or just verify no `malloc`/`new`/`av_malloc` calls appear per-frame in the session struct's `update` path (code review). Pass if `video_update` + `video_render` call no allocators.

- [ ] **DoD 12: ASan playback stress**

Build with `/fsanitize=address`, run three full plays (init → EOF → destruct), check stderr. Expected: zero ASan reports.

- [ ] **DoD 13: Clean-Win10 install**

Copy the deploy dir to a fresh Win10 VM with no VS toolchain installed. Launch. Expected: no missing-DLL errors, game starts, intro plays.

- [ ] **Step: Record results, commit, close**

```bash
git add docs/superpowers/plans/progress/2026-04-23-video-dod.md
git commit -m "chore(video): DoD walkthrough results — all green"
```

---

## Notes for the implementer

- **Shader hot-reload does not apply** — no shader changes in this plan.
- **RelWithDebInfo only** — per CLAUDE.md, Release crashes with GL_INVALID_ENUM.
- **Deploy: per-file `cp -f` + `diff -q`** — never `cp -r`. See `.claude/skills/mc2-deploy.md`.
- **Test by running the game** — there is no C++ unit test framework here. Most tasks end with build → deploy → launch → visual/audio confirmation.
- **If a task's Path A approach fails mid-implementation**, fall back to B or C per the P1 spike outcome without revisiting the spec.
- **Worker-thread decode and PBO upload are explicit non-goals.** If profiling shows hitching on 4× upscales, file a follow-on plan — do not inline.
