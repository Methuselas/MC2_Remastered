# Video Playback: Format- and Resolution-Agnostic Player

Date: 2026-04-23
Status: Approved v4 — plan-ready. Three polish edits from final
review applied (DLL wording, resolver fallback boundary, audio-clock
handoff).

## Problem

The OpenGL port ships with `code/mc2movie.cpp` fully stubbed: `init()`
just stores a filename, `update()` only flips flags, `BltMovieFrame()`
is empty, `render()` early-returns. Every video callsite
(`mainmenu.cpp` intro, `logistics.cpp` briefings/credits,
`missionselectionscreen.cpp` pre-mission clips, `controlgui.cpp`
in-mission `playMovie()`, `forcegroupbar.cpp` pilot cams) routes
through this class. **No videos play today** — the original Bink
(`.bik`) files are shipped on disk but the decoder was stripped when
RAD Bink was not relicensed for the OpenGL port.

## Central promise (scoped)

Play original Bink1 assets and transparently prefer loose-file
replacements. **H.264 in MP4 is the required upscale format.**
Additional FFmpeg-supported formats (MKV, WebM, HEVC, AV1) are
best-effort — they will usually work because we route everything
through `libavformat`/`libavcodec`, but they are not part of the
tested support matrix.

Goals:

1. Decode and display the original `.bik` files that ship in
   `DATA/MOVIES/` (Bink1, typically 320×240 or 640×480).
2. Transparently play loose-file replacements when present. **MP4 /
   H.264** is the contract; other containers/codecs work if FFmpeg
   can open them.
3. Never hard-code resolution. Source size is whatever the file says;
   destination size is whatever rect the caller passes (screen-space).
4. Keep the `MC2Movie` public API untouched so the five callsites stay
   as-is.

## Non-goals

- No in-game video editing, capture, or recording.
- No subtitle/caption rendering.
- No multi-angle / interactive video.
- No streaming from URL — local files only.
- No attempt to build FFmpeg from source. Use a pinned prebuilt LGPL
  Windows distribution.
- No worker-thread decode in MVP. See threading section.

## Prerequisites (must confirm before implementation plan begins)

**These are blockers, not open questions.** The design collapses if
either fails.

### P1. `soundSystem` streamed PCM capability

Need to verify that the existing `soundSystem` (MSS-derived) can:

- Accept a push of streamed raw PCM from application code (not just
  file-backed `playDigitalStream`).
- Report a trustworthy playback position (sample count or equivalent
  monotonic clock) for audio-master A/V sync.
- Accept the mixer's **native** sample rate and format, so we resample
  the decoder output to match the mixer — not the other way around.

If the mixer cannot expose a position clock, the sync model falls back
to external-clock (wall-time) sync, and we document drift tolerance.
If it cannot accept streamed push, we either extend it (in-scope for
this work) or fall back to decode-to-temp-WAV + file playback (lossy
on the audio side, probably acceptable given original `.bik` audio
quality).

**Verification task in the plan:** spike a 30-line prototype that
pushes a sine wave through the streaming path and reads back position.

### P2. Bink1 vs Bink2

`libavcodec` decodes Bink1 natively; Bink2 is not fully supported. MC2
ships 2001; Bink2 dates to ~2013. Verify by running `ffprobe` on a
sample shipped `.bik` before implementation starts. If any file is
Bink2, we either re-demux those files to a supported format as part
of the shipped assets or scope them out.

## Approach

### Decoder: FFmpeg linked as LGPL shared libraries, pinned version

- **Pinned:** `FFmpeg 7.1.x` LGPL shared Windows x64 build from
  `BtbN/FFmpeg-Builds` release `autobuild-<YYYY-MM-DD>` (exact tag
  selected at implementation start and recorded in
  `3rdparty/ffmpeg-lgpl-win64/VERSION.txt`). No nightly rolling.
- Link `libavformat`, `libavcodec`, `libavutil`, `libswscale`,
  `libswresample`. Exclude `avfilter`, `avdevice`, `postproc`.
- Ship the five runtime DLLs alongside `mc2.exe` under deploy path
  `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`. DLL filenames captured in
  the pinned version note to catch future naming drift.
- **Delay-load all five FFmpeg DLLs** via MSVC
  `/DELAYLOAD:avcodec-*.dll` (and equivalents for the other four).
  Install a process-wide `__pfnDliFailureHook2` that flips a global
  `g_ffmpegAvailable = false` flag on any DLL load or symbol
  resolution failure and returns a failure indicator consistent with
  the delay-load contract (so the loader does not terminate the
  process at that import site).
- **Strict containment:** every FFmpeg call lives behind an
  availability gate in `code/mc2video.cpp`. No FFmpeg header is
  included outside that translation unit, and no call into those
  wrappers happens before `g_ffmpegAvailable` has been checked.
  Eager imports of FFmpeg symbols anywhere else in the engine are
  disallowed — a failed-import hook only helps if nothing outside
  the gate can trigger it.
- When the gate is closed, `MC2Movie::init()` logs once and sets
  `stillPlaying = false` so `update()` returns `true` immediately.
  This is what makes DoD test #8 (missing DLLs) reachable — ordinary
  import linking would terminate the process before `main()` on a
  missing DLL.

### File resolver: extension-agnostic override chain

New helper `resolveVideoPath(const char* name)` in a new
`code/mc2video.cpp`:

```
candidates in order:
  if (prefs.UseUpscaledVideos):
    data/movies/<name>.mp4       // required upscale format
    data/movies/<name>.mkv       // best-effort
    data/movies/<name>.webm      // best-effort
  data/movies/<name>.bik         // loose original
  FST: movies/<name>.bik         // shipped original
```

Extension list is a `static constexpr` array; adding a new extension
is a one-line change. Mirrors the `data/art/` loose-file override
pattern used for texture upscales.

### Threading model: single-threaded MVP

All demux / decode / scale / upload runs on the **main render thread**
inside `MC2Movie::update()`. Justification:

- Original `.bik` files are 320×240 or 640×480. CPU decode cost is
  trivial on modern hardware.
- Cutscenes pause the game sim — we are not competing with combat AI
  for CPU at that moment. FFmpeg's H.264 decoder is itself
  multi-threaded internally, so "single-threaded from the game's
  perspective" still uses multiple cores.
- Avoids a large class of bugs around queue bounds, shutdown order,
  and PBO fences that a worker thread introduces.

**Supported hardware floor:** the only target currently validated is
the development workstation (RX 7900 XTX + modern 8+ core x64 CPU).
Performance on upscaled H.264 assets at 1920×1440 or 2560×1920 is
"tested on target hardware; frame drop acceptable under load." We
are not claiming a broader portability guarantee in this MVP. If
community play testing surfaces slower CPUs where upscaled playback
hitches, worker-thread decode is the planned remediation.

**Frame drop is allowed.** If decode + scale + upload exceeds the
per-frame budget, we present the latest-ready frame and skip
intermediates. Audio keeps playing uninterrupted.

**Worker-thread decode is an explicit follow-on** if profiling shows
hitching on the 4× upscaled assets. Adding it later is a contained
change (the `update()`-centric API does not leak onto callsites).

### Texture upload: `glTexSubImage2D`, synchronous

Single NPOT `GL_RGBA8` texture sized to the source. Per-frame
`glTexSubImage2D` from a system-memory RGBA buffer filled by
`sws_scale`. No PBO ping-pong in MVP.

**PBO upload is an explicit follow-on optimization.** Add only if
profiling on a 2560×1920 upscale shows upload stalling the draw.
The caller-visible design does not change when we add it.

### Resolution-agnostic geometry: decide once at `init()`

At `init()` the decoder reports `(srcW, srcH, sar, fps, pix_fmt,
color_range, color_primaries, has_audio, has_alpha)`. We:

- Allocate **one NPOT `GL_RGBA8` texture sized `srcW × srcH`**. No
  tile grid. The fossil `MAX_TEXTURE_WIDTH=256` /
  `MAX_TEXTURES_NEEDED=6` machinery is retired.
- Compute the draw quad by **uniform scale-to-fill-rect with
  letterbox/pillarbox**:
    - `scale = min(rectW / (srcW * sar), rectH / srcH)`
    - `quadW = srcW * sar * scale; quadH = srcH * scale`
    - centered inside `MC2Rect`, black fill outside.
- Cache the quad. Only recomputed on `setRect()` with resize.

Per frame, in `update()`:

1. Demux/decode packets at the master clock until the next video PTS
   ≤ now. Hold the latest-decoded frame if game is faster than video
   fps; drop frames if slower.
2. `sws_scale` the frame to RGBA8 into a stable system-memory buffer.
3. `glTexSubImage2D` from that buffer into the single texture.
4. Store "frame ready" flag so `render()` draws one screen-space
   quad.

### Audio: embedded via FFmpeg, push to mixer at its native rate

- Pull audio from the same container via `libavcodec`. Resample with
  `libswresample` to the **mixer's native sample rate and format**
  (queried from `soundSystem` at `init()`), not a hardcoded
  44.1/S16/stereo.
- Push through the streaming-PCM path identified in prerequisite P1.
- A/V sync: **audio is the master clock** (pending P1). Video PTS is
  compared to audio playback position. If P1 fails, fall back to
  monotonic wall clock.
- Keep the existing `separateWAVE` branch for legacy callsites that
  pass `useWaveFile=true` and for files with no audio stream.

**Audio-source precedence (explicit):**

1. If `useWaveFile=true`, prefer the sidecar WAV. Embedded audio in
   the container is ignored for this callsite even if present.
2. If `useWaveFile=false`, prefer embedded audio from the container.
3. If the preferred source is missing or fails to open, fall back to
   the other source; if both fail, play silent video.

This rule is deterministic regardless of container or callsite.

### Color handling

`libswscale` is configured from the codec context's color space,
primaries, and range. Originals (`.bik`, BT.601 full-range YUV420) and
modern upscales (typically BT.709 limited-range YUV420) both land as
correct sRGB RGBA8. Bink's optional alpha channel is preserved
through RGBA.

## Lifecycle & failure semantics

### `pause(true)`
- Stops audio stream (or pauses it if the mixer supports pause).
- Freezes the master clock. Decode does not advance. Current video
  frame stays on screen.
- `pause(false)` resumes audio and unfreezes the clock. Resume from
  the paused presentation position, allowing **up to one video
  frame of catch-up** to reabsorb audio-buffer slack on the mixer's
  resume path. Sample-exact resume is not guaranteed.

### `stop()`
- Forces `stillPlaying = false` on next `update()`.
- Flushes audio stream. Releases decoder state. Retains the texture
  until destruction (so `render()` showing an old frame for one
  post-stop frame is harmless).
- Idempotent.

### `restart()`
- Seeks container to start. Resets the master clock. Re-arms audio.
- If the decoder state is unhealthy, this also rebuilds it.

### `setRect()` during playback
- **Supported, not just tolerated.** Recomputes the draw quad.
  Texture is untouched (source size did not change).
- The old `STOP()` on resize is removed.

### EOF
- `update()` returns `true` (the existing API contract for "movie
  done"). Caller is responsible for `delete`.
- Natural EOF is distinguished from `forceStop` only for logging.

### Failure handling

| Failure | Behavior |
|---|---|
| FFmpeg DLLs missing at startup | Detect in `MC2Movie::init()`, log once, set `stillPlaying=false` so `update()` returns `true` immediately. Caller advances past the movie. Game does not crash. |
| Upscaled MP4 present but fails to open or decode | Log once, fully tear down the failed decoder state, and restart `init()` from the next resolver candidate (the `.bik` fallback). Fallback happens **only during initialization**, never by recovering a half-constructed decoder mid-playback. Once a candidate successfully reaches the playing state, a later decode error is handled per the audio-stall / EOF rules, not by re-resolving. |
| Original `.bik` also fails | Log once, return EOF. Caller advances. Matches current stub behavior from the caller's perspective. |
| Container has no audio stream | Silent video. Not an error. Existing `separateWAVE` path remains available for callers that want a `.wav` sidecar. |
| Audio decode stalls mid-playback | Continue video on wall-clock fallback; drop audio. Log once per file. **Clock handoff is one-shot:** master clock switches from audio to wall-clock at the moment of the stall, drift accumulated before the handoff is NOT corrected backward (no backward seeks, no visible jumps), and playback continues best-effort to EOF. We do not attempt to re-sync to audio if the audio stream recovers. |
| `setRect()` to a degenerate rect (zero area) | Ignore; keep prior quad. Log once. |

### Logging policy

One log line per failure class per `MC2Movie` instance. No per-frame
spam. Gated behind the existing `MC2_DEBUG_*` env-var pattern for
verbose tracing, using the subsystem tag `[VIDEO]`.

## Callsite cleanup (scoped to what serves this change)

- [logistics.cpp:307](code/logistics.cpp:307) hardcodes
  `movieRect.bottom = 600` for the credits roll (800×600 fossil).
  Change to `Environment.screenHeight`.
- Audit the other four callsites' rects for analogous fossils; fix
  any found. Do NOT refactor layout code beyond that.

## Build / CMake

- Add vendored `3rdparty/ffmpeg-lgpl-win64/` containing headers,
  import libs, runtime DLLs, `VERSION.txt`, `LICENSE.LGPL`.
- `find_package(FFmpeg ...)` custom module; link the five libs into
  `mc2.exe`.
- CMake install step copies the runtime DLLs next to `mc2.exe` on
  deploy.

## Deploy-side config

- New pref `prefs.UseUpscaledVideos` (bool, default `true`). Lets
  users compare originals against upscales without moving files.
  Exposed via the existing prefs file; no UI screen required.
- Deploy target: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`. FFmpeg
  runtime DLLs at the same level as `mc2.exe`. Upscaled MP4s in
  `data/movies/` (loose-file override).
- `release_assets/` gets `LICENSE.FFmpeg.txt` (LGPL notice) and a
  note describing the replacement mechanism (swap DLLs of same
  version).

## Upscale pipeline (out-of-band, separate track)

Not part of this spec's implementation — listed so the code design
accounts for the expected output.

- Input: `.bik` files from MC2 ISO `DATA/MOVIES/`.
- Stage 1: demux to lossless intermediate (FFV1/MKV) + per-file WAV.
- Stage 2: run chosen upscaler (first candidate: SeedVR2 v2.5; see
  separate investigation note on ROCm/ZLUDA/WSL2 feasibility on
  RDNA3).
- Stage 3: re-mux upscaled frames + original audio to **MP4 / H.264
  High / CRF 16 / yuv420p**, preserving source fps. Filename matches
  original base (e.g. `BUBBA1.bik` → `BUBBA1.mp4`).
- Final: drop MP4s in `data/movies/`.

## Risks

- **LGPL compliance.** Dynamic linking + LGPL notice + DLL
  replacement instructions. Add `LICENSE.FFmpeg.txt` to release
  assets.
- **P1 / P2 failure.** Addressed as prerequisites, not risks.
- **MVP single-thread decode may hitch on 4× upscales.** Mitigation:
  worker-thread decode is a contained follow-on.
- **AMD driver quirks** around streaming texture updates on NPOT
  RGBA8 textures. No known issues from
  [docs/amd-driver-rules.md](docs/amd-driver-rules.md), but verify
  during implementation.

## Definition of done

Positive tests:

1. Original MC2 intro (`msft.bik`) plays from main menu on 4K
   fullscreen with correct pillarboxing and audio.
2. Logistics briefings (`cinema*.bik`) and credits (`credits.bik`)
   play with correct aspect and audio.
3. An MP4 dropped in `data/movies/` with the same base name
   transparently replaces the `.bik` (verified by swapping one file).
4. Toggling `prefs.UseUpscaledVideos=false` forces `.bik` playback
   even when MP4 is present.
5. Videos at any source resolution (confirmed: 320×240, 640×480,
   2560×1920) render correctly without code changes.
6. `setRect()` during playback with a new-aspect rect re-fits the
   quad without crashing or tearing.
7. Pause/resume with audio-master sync resumes without audible
   discontinuity.

Negative tests:

8. With all five FFmpeg DLLs removed from the deploy directory, the
   game starts, the intro is skipped with one `[VIDEO]` log line, and
   no subsequent crash or hang occurs.
9. With a corrupted `data/movies/msft.mp4` present and the original
   `msft.bik` also present, the resolver falls back and plays the
   `.bik`.
10. With both replacement and original missing/corrupt, `update()`
    returns `true` promptly and the caller advances the UI.

Resource hygiene:

11. No per-frame `new`/`delete`. Single allocation at `init()`,
    single free at destructor.
12. ASan-clean in the supported dev build (MSVC `/fsanitize=address`
    RelWithDebInfo) across three full playback cycles (init →
    update-to-EOF → destruct). No net allocation growth under the
    project's standard Windows leak diagnostics.

Deployment:

13. Deploy package loads on a clean Win10 install (no dev toolchain)
    without missing-dependency errors.

## What this spec does NOT cover

- Implementation plan (comes next via writing-plans skill).
- SeedVR2 feasibility investigation (separate track).
- UI for per-cutscene playback control.
- Video capture / replay-to-file.
- Worker-thread decode (deferred; contained follow-on).
- PBO upload ping-pong (deferred; contained follow-on).
