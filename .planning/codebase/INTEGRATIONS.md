# External Integrations

**Analysis Date:** 2026-05-14

> Scope: external systems and surface area for the active worktree
> `claude/nifty-mendeleev`. Companion to `STACK.md`.

## File Formats Consumed

### Game-data archives

- **`.fst` — FastFile archive.** The primary content container.
  zlib-compressed, hash-indexed bundles of game files. Implementation:
  `mclib/fastfile.cpp` (registry / find), `mclib/ffile.cpp` (read),
  `mclib/file.cpp` (file abstraction over FST + loose files).
  Examples: `tgl.fst` (mech mesh archive, opened from CD path at
  `file.cpp:311`), per-mission FSTs.
  - Lookup keys are forward-slash, lowercased, hashed with `elfHash`.
    `.fit`-embedded paths use backslash and must be normalized on
    read. See memory `fst_forward_slash_invariant.md`.
  - Packer: `data_tools/makefst.cpp` (also unpacks via `-d`).
- **`.fit` — FastFile Index Table.** Higher-level descriptor that
  bundles related files. Used for cameras
  (`mclib/color.cpp:40` reads `colors.fit`), paint schemata
  (`mclib/mech3d.cpp:1649` reads `paintSchemata.fit`),
  campaigns (`mc2srcdata/campaign/campaign.fit`,
  `mc2srcdata/campaign/tutorial.fit`).
- **`.pak` — sound pack.** Used by `mclib/soundsys.cpp:147-167` for
  voice/SFX/Betty banks (`Betty.pak`, `support.pak`).
- **`.rsp` / `.res`** — string-resource archives. `makersp` tool in
  `data_tools/makersp.cpp` builds these; the `res/` subdirectory
  builds `mc2res.dll` which carries strings and icons.

### Art/data

- **`.tga` — Truevision Targa.** Effectively the only image format.
  Used for: terrain colormap (`terrain.cpp:457`), terrain burn-in
  overlay (`.burnin.tga`, `terrain.cpp:460`), terrain tile mipmaps
  (`terrtxm.cpp`, `terrtxm2.cpp`), detail/water/height layers
  (`terrtxm2.cpp:1042-1100`), clouds (`clouds.cpp:62`),
  craters (`crater.cpp:155`), mech-bay images
  (`mlr/gosimagepool.cpp:134`), UI atlases, mine markers
  (`quad.cpp:677-684`). Reader: `mclib/tgainfo.cpp`,
  `mclib/txmconv.cpp`. The renderer expects ARGB layout in memory
  (which is BGRA on the wire — see memory `mc2_argb_packing.md`).
- **`.bmp`** — referenced sparingly; mostly historical.
- **`.png`** — output of the upscaler pipeline, but **not consumed
  at runtime** — the upscalers always finish by writing back to
  `.tga`. PNG is the working format inside `mc2srcdata/`.

### Scripting

- **`.abl` / `.abi`** — ABL (Andy's Battle Language?) source files
  for mission AI, mech brains, and triggers. Parsed and executed by
  the in-tree VM (`mclib/ablscan.cpp`, `mclib/ablparse.h`,
  `mclib/ablexec.cpp`, `mclib/ablrtn.cpp`, `mclib/ablexpr.cpp`,
  `mclib/ablsymt.cpp`, etc.). Bridged to game state from
  `code/ablmc2.cpp` (6000+ lines).

### Audio

- **`.wav`** — uncompressed PCM. Loaded via SDL2_mixer
  (`Mix_LoadWAV`) and direct `SDL_LoadWAV`. Digital music streams
  also use `.wav` (`mclib/soundsys.cpp:733, 835`).
- **`.pak`** — packed sound banks (see above).

### Mesh / animation (3D)

- **`.ase`** — Autodesk ASCII Scene Export. Consumed offline by
  `data_tools/aseconv.cpp` (uses the Microsoft XNA "ARM" library
  headers under `ARM/Microsoft.Xna.Arm.h`) to convert into the
  engine's runtime mesh form.
- **`.tgl`** — runtime mesh shapes, consumed by
  `mclib/mlr/` and `mclib/tgl.cpp`. Always packed inside `.fst`.
- Runtime mesh classes live in `mclib/mlr/` (templated multi-shape
  pipeline) and `mclib/genactor.cpp`, `mclib/mech3d.cpp`.

### Configuration / data

- **`.cfg`** — INI-style key/value (`mclib/inifile.cpp` reader).
  Files: `system.cfg`, `options.cfg`, `prefs.cfg`, per-camera `.cfg`.
- **`.csv`** — used for tabular game-data (`mclib/csvfile.cpp`
  reader).
- **`.fnt`** — font definition. Builder is `text_tool/main.cpp`
  (depends on SDL2_ttf).

### Movies

- **BIK (legacy)** — original game shipped Bink video. Modern path:
  remuxed/transcoded to MP4 (H.264 or similar) and played back via
  FFmpeg. See `code/mc2video.cpp` and the FFmpeg integration below.

## The `mc2srcdata` Submodule

Tracked separately as a sibling git checkout (not a submodule in the
strict `git submodule` sense — checked out beside the engine source
per `BUILD-WIN.md`). The engine itself is a treeshown gitlink
entry `mc2srcdata` at repo root.

Contents (top level):
- `art/` — raw / authored TGA art assets (UI, splash, icons).
- `art_4x_gpu/` — 4× ESRGAN-upscaled variants. Loaded as **optional
  loose-file overrides** alongside the stock FST contents.
- `campaign/` — `campaign.fit`, `tutorial.fit` (and `arm/`).
- `cameras/` — camera definitions (`.cfg` + `.fit`).
- `effects/`, `fonts/` — supporting data.
- `build_scripts/` — GNU `make` driven build pipeline (`makefile`
  + `*.mk` shards: `fst.mk`, `tgl.mk`, `sound.mk`, `movie.mk`,
  `object.mk`, `other.mk`, `config.mk`, `fonts.mk`). Invokes
  `aseconv`, `makefst`, `makersp`, `mpak`, `text_tool` from
  `data_tools/` + `text_tool/`.

The engine consumes `mc2srcdata` at install time: the
`build_scripts/make all` flow produces the FSTs and PAKs that get
copied next to `mc2.exe`. The repo also serves as the source for
loose-file overrides at runtime (4× art assets are loaded directly
from disk, bypassing FST).

**Architectural rule:** stock install must remain playable; loose-file
overrides are strictly optional. See memory
`stock_install_must_remain_playable.md`.

## External Build-Time Tools

These are built from this repo into `mc2.exe`'s sibling executables
or required externally:
- `data_tools/aseconv.cpp` → `aseconv` (ASE → engine mesh).
- `data_tools/makefst.cpp` → `makefst` (FST pack / unpack).
- `data_tools/makersp.cpp` → `makersp` (string-resource builder).
- `data_tools/pak.cpp` → `mpak` (PAK builder).
- `text_tool/main.cpp` → `text_tool` (font generation; SDL2_ttf).
- `res/` subdir builds `mc2res.dll` (icons, string tables).
- `Viewer/` builds a separate mesh viewer executable.

External to this repo, **GNU make** is required to drive the
`mc2srcdata/build_scripts/makefile` pipeline.

## Telemetry / Profiling Integrations

- **Tracy** — vendored at `3rdparty/tracy/` (TracyClient.cpp linked
  in). Always compiled in on the worktree (`-DTRACY_ENABLE
  -DTRACY_ON_DEMAND`). The profiler runs on a TCP socket and is
  driven by the external Tracy GUI. CPU zones, GPU zones (via GL
  timer queries), and frame markers are sprinkled across
  `GameOS/gameos/gameos_graphics.cpp`, `gos_postprocess.cpp`,
  `mclib/txmmgr.cpp`, etc. Helper macros in `GameOS/gameos/gos_profiler.h`.
- **AMD RGP / Radeon Developer Panel** — supported externally for
  GPU shader-level analysis (no in-tree hook; works via standard
  Radeon driver instrumentation).
- **In-process logging** — env-gated `[SUBSYS vN]` printf streams
  (heartbeat, TGL pool, destroy trace, GL error drain, asset-scale).
  See `STACK.md` "Environment knobs" and `CLAUDE.md`
  "Tier-1 Instrumentation Env Vars".
- **ASan** — opt-in `-DMC2_ASAN=ON` dev build; produces stack traces
  for heap errors at runtime. Mutually exclusive with Tracy.

## Networking / Multiplayer

**Status: stub on this branch.** Per `README.md`:
"all functionality (except networking) is implemented." The original
MC2 used DirectPlay; the OSS port has not reimplemented the network
transport.

- UI shell exists: `code/multplyr.cpp`, `code/mpconnectiontype.cpp`,
  `code/mphostgame.cpp`, `code/mpgamebrowser.cpp`,
  `code/mpsetuparea.cpp`, `code/mpparameterscreen.cpp`,
  `code/mploadmap.cpp`, `code/mpstats.cpp`, `code/mpprefs.cpp`,
  `code/chatwindow.cpp`. Selection between connection types is
  handled at `mpconnectiontype.cpp:147-272` (4 modes including
  null / unset).
- **No transport implementation found.** No `WSAStartup`, no `socket(`,
  no `connect(`/`listen(`/`bind(` calls in the `code/` tree
  (sole `TracySocket.cpp` hits are inside the Tracy vendor tree,
  unrelated to gameplay).
- `ws2_32` is linked on the worktree (`CMakeLists.txt:267`) — appears
  to be a forward-compatibility hook plus Tracy's Windows socket
  backend.
- `GameOS/include/net.hpp`, `network.hpp`, `netplayer.hpp`,
  `networklobby.hpp`, `networkmessages.hpp` are header stubs from
  the original Microsoft API surface; not wired to any active
  implementation.

## Save Game Format

- Custom binary format authored by `code/saveload.cpp`. Touches
  `Mission`, `MoveMgr`, `ObjectManager`, `Mover`, `Mech`, `Collsn`,
  `Cmponent`, plus sound/UI subsystems. No external format
  dependency.
- Memory rule (`stock_install_must_remain_playable.md`): no save
  game depends on generated render caches or modern visual sidecars.

## Video Playback (worktree-only)

- **FFmpeg LGPL shared 7.1.x** vendored at
  `3rdparty/ffmpeg-lgpl-win64/` (pin: `n7.1.3-45-g2d6ee37238`,
  autobuild `2026-04-23`).
- 5 DLLs delay-loaded: `avcodec-61`, `avformat-61`, `avutil-59`,
  `swresample-5`, `swscale-8` (CMakeLists.txt:284-292). Major
  versions are hard-coded in the link flags and in the generated
  `mc2video_dlls.h`; bumps require updating `cmake/FindFFmpegLGPL.cmake`.
- Failure mode: if any FFmpeg DLL is missing, `mc2.exe` still
  launches and the game skips video playback (delay-load failure
  hook in `code/mc2video.cpp`). Install-time copy is enforced by a
  POST_BUILD `copy_if_different` per DLL (`CMakeLists.txt:296-300`).
- LGPL compliance: license text is preserved at
  `3rdparty/ffmpeg-lgpl-win64/LICENSE.LGPL`; the build is shared,
  not static; FFmpeg sources are not modified.

## Asset Pipeline (offline Python)

Peripheral scripts in repo root, **not invoked by CMake**, **not
required for runtime**:
- `upscale_ui_textures.py` — 4× ESRGAN upscale of UI/HUD TGAs
  (`mc2srcdata/art/*.tga` → `mc2srcdata/art_4x/`). RGB upscaled with
  ESRGAN, alpha with LANCZOS. Skips < 32×32.
- `upscale_textures.py`, `upscale_gpu.py`, `upscale_pytorch.py`,
  `upscale_stablesr.py` — variants for different model / backend
  combos.
- `sd_terrain_upscale.py` — Stable-Diffusion-style terrain upscale.
- `esrgan_upscale.py` — generic ESRGAN driver.
- `pack_mat_normal.py` — packs an OpenGL normal map + displacement
  map into a single 32-bit RGBA TGA for terrain materials
  (RGB = tangent-space normal, A = displacement / POM input).
  Accepts EXR (via OpenCV) / PNG / JPG input.
- `deploy_256.py`, `deploy_colormaps.py` — deployment helpers.
- ESRGAN binaries / weights staged at `realesrgan-ncnn-vulkan/` and
  `esrgan_models/` (NOT part of build; gitignored or untracked).

**Excluded from this map by request:** the realesrgan binaries and
ESRGAN model weights themselves (`.pth`, `.safetensors`) are external
inference artifacts, not engine inputs.

**Caveat:** these scripts assume hardcoded paths
(`A:/Games/mc2-opengl-src/mc2srcdata/...`) — they are operator tools
on the developer's machine, not portable.

## Webhooks, APIs, Cloud Services

- None. The engine is fully offline. No telemetry uploads. No
  cloud sync. No update checks. Tracy connects only over loopback
  (or LAN) when the GUI is actively probing.

## Environment Configuration

**No `.env` files in tree** (verified — only the env vars
documented in `CLAUDE.md` "Tier-1 Instrumentation Env Vars" affect
behaviour).

**Required env vars to run:** none. The game looks for `system.cfg`
and `options.cfg` next to the executable; missing options.cfg is
recreated with defaults.

**Optional dev env vars:** see `STACK.md` "Environment knobs"
section.

## CI / CD

- **No checked-in CI configuration.** No `.github/workflows/`,
  no `.gitlab-ci.yml`, no Azure pipeline. The development workflow
  is local CMake + manual smoke gate
  (`scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing`,
  per `CLAUDE.md`).
- Skill files in `.claude/skills/` provide canonical recipes:
  `mc2-build`, `mc2-deploy`, `mc2-build-deploy`, `mc2-check`.

## Inbound / Outbound Surface

**Inbound:**
- TCP loopback when Tracy GUI attaches (worktree only,
  `TRACY_ON_DEMAND`).
- Keyboard/mouse via SDL2 (`GameOS/gameos/gos_input.cpp`).
- Filesystem reads against the install root + `mc2srcdata` overrides.

**Outbound:**
- Filesystem writes: save games, `options.cfg`, log files in install
  root.
- No network, no IPC, no clipboard, no external process spawning
  in the game runtime.

---

*Integration audit: 2026-05-14*
