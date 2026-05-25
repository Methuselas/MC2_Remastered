# MechCommander 2 — OpenGL Remastered v0.4 beta

A visual remaster of MechCommander 2: PBR terrain, real-time shadows, tessellation,
and modern post-processing. Original gameplay and missions are unchanged.

This is a **beta** release. Most rendering changes are stable; particles and a
small number of asset-side glitches remain. See **Known Issues** below.

## Requirements

- Windows 10 or 11 (64-bit)
- GPU with OpenGL 4.3 support (NVIDIA GTX 900 series or later, AMD RX 400 series or later, or Intel Iris Xe)
- ~6 GB free disk space

## Install

Create a new empty folder, then download all seven zips and extract each into that
folder. Folder contents merge — do not extract into subfolders.

| Zip | Contents |
|-----|----------|
| `mc2-remastered-engine.zip` | Executable, shaders, DLLs, Mission Editor, tools |
| `mc2-gamedata.zip` | Game archives, maps, sounds, configs |
| `mc2-movies.zip` | BIK movies |
| `mc2-burnins-4x-pt1.zip` | 4× upscaled mission lightmaps (campaign A + tutorials) |
| `mc2-burnins-4x-pt2.zip` | 4× upscaled mission lightmaps (campaign M + e3demo) |
| `mc2-art.zip` | 4× upscaled UI and art textures |
| `mc2-tgl.zip` | 4× upscaled 3D model textures |

The `burnins-4x` zips are technically optional — the game falls back to stock-resolution
lightmaps inside the FST archives — but terrain colormaps look noticeably blurry without
them.

No original MC2 install required.

**Optional:** Extract `mc2-load-points.zip` (53 KB) into
`%USERPROFILE%\.mechcommander2\savegame\` (create the folder if it doesn't exist).
Provides 24 pre-built save games — one per campaign mission — each with a full pilot
roster and 1,000,000 CBills. Load from the **Load Game** menu to jump to any mission.

## Run

Double-click `mc2.exe`, or run `run-with-log.bat` to capture a `stderr.log` next to
the executable if you hit a problem. The cmd window stays open in this beta to make
modder log capture easier — close it after `mc2.exe` exits.

## Mission Editor (beta)

`Mission Editor.exe` ships alongside `mc2.exe`. Launch with `run-editor.bat`. The
editor runs on the same GPU-driven rendering pipeline as the game and can open any
campaign mission. Save support and asset-import polish are still in progress —
treat as preview.

Bundled libraries (in the engine zip) for modder consumption:

- **Assimp** — model importer (`assimp::assimp` linked through `mclib`)
- **meshoptimizer** — mesh optimization (available, off by default)

These are present so external tooling and future cookers can be built against the
same headers the game/editor use. Not all consumer paths are wired in v0.4 beta.

## Toggles (in-game)

These work at any time during gameplay:

| Key | Effect |
|-----|--------|
| RAlt+F1 | Toggle bloom |
| RAlt+F2 | Shadow debug overlay |
| RAlt+F3 | Toggle shadows |
| RAlt+F5 | Terrain draw killswitch |
| RAlt+5 | Cycle HUD scale (1.0 → 0.90 → 0.85 → 0.80) |

## Known Issues (v0.4 beta)

- **Particle system not fully functional — placeholder animations.**
  Explosions, smoke, fire, and missile trails render but some curves and
  per-effect tuning are placeholder. Visible-but-imperfect. Fix targeted.
- **Some static props pop in and out.**
  Edge-case in the GPU visibility cull for small props near tile boundaries.
  Known, fix targeted, but non-trivial.
- **Shadow stutter** when the camera moves a large distance in one jump.
  Panning continuously is smooth; the re-render only triggers on large jumps.
- **Shadow banding** shifts slightly with camera rotation on terrain with steep
  geometry — a known limitation of view-dependent tessellation with a fixed shadow map.

### Tracked GitHub issues

Status of issues open at release time:

| # | Title | v0.4 beta status |
|---|-------|------------------|
| [#33](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/33) | Water elevation bug | **Fixed** — submerged-sand + shoreline + cliff-bleed |
| [#34](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/34) | Near clipping plane | Partial — HUD reverse-Z fixed; world near-plane untouched |
| [#24](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/24) | Paint scheme picker R/B swap | Partial — channels fixed; 3D preview still missing |
| [#10](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/10) | Enemies missing scripted moves | Open |
| [#12](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/12) | Power generator glow upside-down | Open |
| [#14](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/14) | Pop-up turrets visible while underground | Open |
| [#17](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/17) | Mechs reluctant to fire weapons | Open |
| [#23](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/23) | Mission planner background turns gray | Open |
| [#26](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/26) | Cursor glitches in bottom 1/3 of screen | Open |
| [#32](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/32) | Data files: patches applied? | Open (docs) |
| [#35](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/35) | Edge of map transparency | Open |
| [#37](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/37) | Expanded terrain | Open (feature request) |

v0.4 beta focused on the rendering pipeline and Mission Editor bring-up. Gameplay,
AI, asset, and HUD/UI issues largely remain for later passes.

## Troubleshooting

**Black screen / immediate crash:** your GPU may not support OpenGL 4.3. Check
`stderr.log` for a `[GPU]` line — if the version is below 4.3 the engine will not start.

**Wrong textures / scrambled icons:** do not replace `data/art/` files from third-party
mods that predate this release. Some icon atlas files are resolution-sensitive and must
stay at their original sizes.

**No sound:** SDL2_mixer requires audio to be available at startup. Check that your
default audio device is active before launching.

**Editor won't launch:** `Mission Editor.exe` must run with CWD set to the install
folder. Always launch via `run-editor.bat`.

## Credits

- **Original game**: Microsoft / FASA Interactive (2001)
- **OpenGL port, Linux support, and engine bug fixes**: [alariq/mc2](https://github.com/alariq/mc2) — without this there is no remaster
- **D3F font loader, multi-monitor mouse grab, SDL window-event dispatch fix, FMV reference**: [Alexbeav](https://github.com/Alexbeav) — code cherry-picked from [MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)
- **Visual remaster + Mission Editor bring-up**: ThranduilsRing
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
