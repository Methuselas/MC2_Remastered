<!-- refreshed: 2026-05-14 -->
# Architecture

**Analysis Date:** 2026-05-14

## System Overview

MechCommander 2 / MC3 is the Microsoft-open-sourced **MechCommander 2** RTS
engine, ported from DirectX to OpenGL and modernized. The codebase is a
single-process desktop game (no client/server split for singleplayer; an
optional multiplayer subsystem exists). The architecture is a classic
late-90s C++ engine — three big libraries (`stuff`, `mlr`, `mclib`) under a
platform veneer (`GameOS`) with the game proper sitting on top in `code/`.

```text
┌──────────────────────────────────────────────────────────────────────┐
│  Game logic (singleplayer + multiplayer)            `code/`          │
│  Mission, ObjectManager, Mover/Mech/GVehicle/Turret/Artillery,       │
│  Logistics screens, MissionInterface (HUD), GameCam, Weather, ABL    │
│  bridge (ablmc2.cpp), saveload                                       │
└──────────────────────┬───────────────────────────────────────────────┘
                       │ uses
                       ▼
┌──────────────────────────────────────────────────────────────────────┐
│  MC engine core                                     `mclib/`         │
│  Terrain, MC_TextureManager (txmmgr), TG_Shape (3D/animated),        │
│  Mech3DAppearance, GVAppearance, BdgAppearance, ABL VM,              │
│  PathManager, SensorManager, FastFile/.fst archive, packet,          │
│  FitIniFile, sound system, TGL pools (vertex/color/face/shadow)      │
└─────────┬──────────────────┬─────────────────────┬───────────────────┘
          │                  │                     │
          ▼                  ▼                     ▼
┌────────────────┐  ┌────────────────┐  ┌──────────────────────────────┐
│ Foundation     │  │ Renderer       │  │ GUI widgets                  │
│ `mclib/stuff/` │  │ `mclib/mlr/`   │  │ `gui/`                       │
│ Vector/Matrix, │  │ MLR shape/mesh │  │ aButton/aListbox/aSystem/    │
│ memory pools,  │  │ classes, gos*  │  │ aEdit/aAnim                  │
│ filestream,    │  │ vertex/image   │  │                              │
│ chain/hash     │  │ pools          │  │                              │
└────────┬───────┘  └───────┬────────┘  └──────────────┬───────────────┘
         │                  │                          │
         │ (+ FX: `mclib/gosfx/` particles/clouds/cards)│
         ▼                  ▼                          ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Platform / Renderer back-end                       `GameOS/`        │
│  gosRenderer (gameos_graphics.cpp), gosPostProcess (FBO/bloom/       │
│  shadow/FXAA/tonemap), gos_render (SDL2 window/context),             │
│  gameosmain (main loop), gos_input, gos_sound, gos_font              │
└──────────────────────┬───────────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Third-party: SDL2, SDL2_mixer, SDL2_ttf, GLEW, OpenGL 4.3, ZLIB     │
│  (resolved by CMake — `3rdparty/3rdparty/`)                          │
└──────────────────────────────────────────────────────────────────────┘
```

## Component Responsibilities

| Component | Responsibility | File |
|-----------|----------------|------|
| `main()` | SDL window/context init, GL bootstrap, frame loop | `GameOS/gameos/gameosmain.cpp` |
| `Environment` (gosEnvironment) | Holds callbacks `InitializeGameEngine` / `DoGameLogic` / `UpdateRenderers` / `TerminateGameEngine` | `GameOS/gameos/gameos.cpp:52`, declared in `GameOS/include/gameos.hpp` |
| `gosRenderer` | All OpenGL state, render-state stack, draw-call dispatch, texture/buffer/material objects | `GameOS/gameos/gameos_graphics.cpp:968` |
| `gosPostProcess` | HDR scene FBO, bloom ping-pong, shadow map FBO, FXAA, tonemap, procedural skybox | `GameOS/gameos/gos_postprocess.cpp`, header `GameOS/gameos/gos_postprocess.h` |
| `MC_TextureManager` | Owns all engine textures + per-frame "master vertex node" / "master shape node" lists; flushes them as `renderLists()` after game render. Singleton: `mcTextureManager`. | `mclib/txmmgr.cpp` (notably `renderLists` at `mclib/txmmgr.cpp:910`) |
| `Terrain` | Heightfield, tile texture atlas, `quadList` window of visible quads, water, craters | `mclib/terrain.cpp`, `mclib/terrtxm.cpp`, `mclib/quad.cpp` |
| `Mission` | One scenario: owns `land` (Terrain), `eye` (camera), `ObjectManager`, weather, mission interface, mission ABL brain | `code/mission.cpp` |
| `ObjectManager` | Master registry of `GameObject` instances (Mechs, vehicles, turrets, terrain objects, weapons fx) + LOS/sensor bookkeeping | `code/objmgr.cpp` |
| `Mech3DAppearance` | Articulated mech rendering, hit-locations, weapon-node attachments | `mclib/mech3d.cpp` (5139 lines — largest single file) |
| `TG_Shape` / `TG_MultiShape` / `TG_AnimateShape` | DX-era articulated mesh + animation primitives; backbone of every 3D actor | `mclib/mlr/` + `mclib/bdactor.cpp`, `mclib/gvactor.cpp`, `mclib/genactor.cpp` |
| `gosfx::*` | Particle FX (point clouds, debris, cards, tubes, perturbations) | `mclib/gosfx/` |
| ABL VM | Scripting language used for mission brains, AI warriors, triggers | `mclib/abl*.cpp`, bridged from game in `code/ablmc2.cpp` |
| `gui::aSystem` | Widget tree, focus, input dispatch, screen overlay | `gui/aSystem.cpp`, widgets `gui/a*.cpp` |

## Pattern Overview

**Overall:** Late-90s "subsystem singletons + global pointers" C++ engine,
wrapped in a SDL2/OpenGL platform layer. Modernized incrementally: SSBOs,
GLSL 4.3 shaders, FBOs, shadow maps, post-processing, tessellation control
for terrain — but the data flow into the renderer is still the original
"submit a list of `gos_VERTEX` arrays, deferred-flush at end of frame."

**Key Characteristics:**
- **Singletons everywhere.** `mcTextureManager`, `ObjectManager`, `eye`
  (Camera), `land` (Terrain), `userInput`, `soundSystem`, `PathManager`,
  `SensorManager`, `craterManager`, `weather` are file-scope globals.
- **Deferred / queued rendering at the engine level.** Game code calls
  `Terrain::render`, `currentQuad->draw()`, `Mech::render`, etc. but those
  do not draw — they append to the master vertex/shape node arrays in
  `MC_TextureManager`. `mcTextureManager->renderLists()` walks those arrays
  and issues the GL draws at the end of `draw_screen()`.
- **Hand-rolled memory pools.** `vertexPool`, `colorPool`, `facePool`,
  `shadowPool`, `trianglePool` — bumped to 500K entries (see
  `memory/tgl_pool_exhaustion_is_silent.md`). Reset every frame in
  `Mission::render`.
- **Active modernization seams.** GPU static-prop registry, GPU compute
  cull, indirect terrain draw, SSBO-backed light data, deferred uniform
  system. These coexist with the legacy CPU path — both are present in
  the tree; many gated by env vars (`MC2_GPU_CULL_*`,
  `MC2_STATIC_UPDATE_SKIP`, etc.).

## Layers

**Game layer (`code/`):**
- Purpose: Gameplay rules, mission flow, GUI screens (mainmenu, logistics,
  mechbay, options), multiplayer setup, save/load, ABL integration.
- Location: `code/`
- Contains: `Mission`, `Mech`, `Mover`, `GVehicle`, `Turret`, `Artillery`,
  `WeaponBolt`, `Carnage` (explosions/damage), `Team`, `Commander`,
  `Group`, `Warrior` (AI pilot), `Logistics*`, all `MP*` multiplayer
  setup screens, `MissionInterface` (HUD).
- Depends on: `mclib/`, `gui/`, `GameOS/include/`.
- Used by: nothing — `mc2.exe` entry.

**Engine layer (`mclib/`):**
- Purpose: Reusable engine subsystems — terrain, textures, animated 3D
  shapes (`TG_*`), ABL VM, pathfinding, sound, packet/FastFile I/O.
- Location: `mclib/` + the sub-libraries it builds: `mclib/mlr/`,
  `mclib/gosfx/`, `mclib/stuff/`.
- Contains: every `*.cpp` in `mclib/` (193 files).
- Depends on: `stuff` (math/containers), `gosfx` (particles), `mlr`
  (mesh primitives), `GameOS/include`.
- Used by: `code/`, `gui/`.

**GUI library (`gui/`):**
- Purpose: Immediate-mode-ish widget set (buttons, listboxes, edit, scroll,
  animations) used by all logistics/main-menu screens.
- Depends on: `mclib/` (for `gosFont`, file I/O), `GameOS/include`.
- Used by: `code/` screen classes.

**Platform / renderer back-end (`GameOS/`):**
- Purpose: All direct OpenGL / SDL2 contact. Implements the GOS API
  (`gos_*` C-style functions declared in `GameOS/include/gameos.hpp`)
  that the engine and game both call.
- Contains: `gosRenderer` (OpenGL render-state + draw dispatch),
  `gosPostProcess` (FBOs, bloom, shadow, FXAA, tonemap, skybox),
  `gos_render` (SDL2 window/context wrapper), `gos_input`, `gos_sound`,
  `gos_font`, `gameosmain` (the actual `main()`).
- Depends on: SDL2, GLEW, OpenGL ≥4.3, ZLIB.
- Used by: `mclib/`, `code/`, `gui/`.

**Platform shim (`GameOS/src/`):**
- Purpose: Cross-platform stand-ins for Win32 APIs (`platform_str.cpp`,
  `platform_winuser.cpp`, etc.) so the rest of the codebase can call
  `_stricmp`, `lstrcpy`, etc. on POSIX builds. Built unconditionally as
  the `windows` static lib (note: `LINUX_BUILD` is defined globally even
  on Windows — see `MEMORY.md`).
- Depends on: nothing.

**Foundation (`mclib/stuff/`):**
- Purpose: `Stuff::Vector3D`, `Stuff::Matrix4x4`, file streams, hash,
  notation file, motion/origin/quat math. Pre-STL. Modeled after a DX-era
  "Stuff" library.
- Depends on: nothing.

**Mesh renderer (`mclib/mlr/`):**
- Purpose: Multi-list mesh primitives — `MLR_I_C_DET_PMesh`,
  `MLR_I_L_DT_TMesh` and ~20 other variants. Templated on
  Indexed/Indirect, Colored, Lit, Detail-textured, Multi-textured,
  Polygon-mesh vs Triangle-mesh. Backs `gosImage`, `gosVertexPool`,
  `gosPoint`.
- Depends on: `stuff/`.

**Particle FX (`mclib/gosfx/`):**
- Purpose: Weapon/explosion/smoke particle systems. `Effect`,
  `EffectLibrary`, particle/shape/spinning/point/debris/pert clouds.
- Depends on: `mlr/`, `stuff/`.

## Data Flow

### Primary Frame Path

The main loop is in `GameOS/gameos/gameosmain.cpp:379–403`. One frame:

1. `Environment.DoGameLogic()` — installed by the game at startup, points
   to `__stdcall DoGameLogic()` in `code/mechcmd2.cpp:2024`. Updates input,
   sound, timers, logistics OR mission. The bulk of game work runs through
   `Mission::update()` at `code/mission.cpp:318`.
2. `process_events()` — SDL event pump, debug hotkeys (RAlt+F1 bloom toggle,
   RAlt+F3 shadow toggle, etc. — `gameosmain.cpp:37`).
3. `draw_screen()` — `gameosmain.cpp:141`:
   - `pp->updateLightMatrix(...)` — light-space matrix for shadow map.
   - `pp->beginScene()` — bind HDR scene FBO.
   - `pp->renderSkybox(...)` — procedural skybox fragment shader.
   - `gos_RendererBeginFrame()` → `Environment.UpdateRenderers()` —
     installed by the game (`code/mechcmd2.cpp:676`); calls
     `mission->render()` which calls `eye->render()`,
     `missionInterface->render()`, etc. Most of these enqueue draws into
     `MC_TextureManager`'s master arrays. **They do not actually draw.**
   - `gos_RendererEndFrame()` — internally calls
     `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:910`) which
     uploads light/scene UBOs, then walks `masterHardwareVertexNodes` (for
     `TG_Shape`-style draws using `ShapeRenderer`), the shadow depth pass,
     and `masterVertexNodes` (for the legacy `gos_VERTEX` flat stream:
     terrain, water, craters, alpha-test passes split into states 0/1).
   - `pp->endScene()` — composite HDR → default FBO; bloom; FXAA; tonemap.
4. `graphics::swap_window(win)` — SDL swap buffers.

### Mission::update path

`Mission::update` (`code/mission.cpp:318`) runs in a specific order each
turn (the comment "Lastly, process the terrain geometry" calls this out):

1. `mcTextureManager->clearArrays()` — reset master vertex/shape nodes
2. `missionInterface->update()` (HUD inputs)
3. `eye->update()` — camera
4. `weather->update()`
5. `PathManager->update()`
6. `land->clearObjBlocksActive(); land->clearObjVerticesActive();`
7. `land->terrainTextures->update()` — texture LOD / streaming
8. `land->geometry()` — terrain CPU project + cull, populate visible-quad
   window (`Terrain::quadList`)
9. `ObjectManager->update(true,true,true)` (or `updateAppearancesOnly` if
   paused) — calls `update()` on every active GameObject, gated by the
   cull infrastructure (`inView`, `objBlockInfo.active`)
10. `craterManager->update()`
11. `mcTextureManager->update()` — texture-handle re-cache for visible
    shapes. **Critical:** skipped when paused; this is the root of the
    "PAUSE diagnostic for static-render bugs" memory.
12. `SensorManager->update()` (optional)
13. `ObjectManager->updateCollisions()` (optional)
14. `missionBrain->execute()` — ABL VM, may end the mission

### Mission::render path

`Mission::render` (`code/mission.cpp:756`):

1. `eye->render()` — sets up camera matrices on the renderer
2. `GameMap->clearCellDebugs(0)`
3. `FloatHelp::renderAll()` — world-space floaters
4. `missionInterface->render()` — HUD widgets; chains into terrain quad
   draw, mech draw, etc. (each ultimately appending to MC_TextureManager
   master arrays)
5. **Pool reset** at end: `colorPool->reset(); vertexPool->reset();
   facePool->reset(); shadowPool->reset(); trianglePool->reset();`

### renderLists() / GPU draw path

`MC_TextureManager::renderLists` (`mclib/txmmgr.cpp:910`) is where CPU
queues finally become GL calls. Order matters:

1. Upload `lightDataBuffer_` (UBO) and `sceneDataBuffer_` (TG_HWSceneData
   UBO with fog, camera pos, base vertex color).
2. **Shape pass:** iterate `masterHardwareVertexNodes` (modern path,
   `TG_Shape`-style). For each: `ShapeRenderer::setup(world, view, wvp,
   viewport)` + `render(vb, ib, vdecl, texture, light_data_buffer_index)`.
   These shapes are the mechs, vehicles, buildings — articulated 3D.
3. **Shadow depth pass:** if `gosPostProcess::shadowsEnabled_`, bind shadow
   FBO and re-render terrain master vertex nodes flagged `MC2_ISTERRAIN`
   into the shadow depth texture (`shadow_terrain.vert/frag`).
4. **Terrain main pass:** iterate `masterVertexNodes` flagged
   `MC2_DRAWSOLID`. Terrain nodes set `gos_State_Terrain=1` to enable
   the splat/POM shader (`gos_terrain.frag`). Solid alpha-off objects
   then drawn.
5. **Alpha pass:** terrain nodes flagged `MC2_DRAWALPHA` (water, decals)
   drawn in two sub-passes (`AlphaTest` 0 then 1) so test-discard and
   blended alpha don't interleave.
6. **Crater pass:** `MC2_ISCRATERS` nodes drawn last (separate ground-decal
   path, no alpha).

**State chunking matters:** each batch of ≤`MAX_SENDDOWN` vertices is
issued via `gos_RenderIndexedArray(gos_VERTEX*, count, indexArray, count)`
to avoid driver issues observed historically at 20–30k verts.

### Texture / appearance lifecycle

Textures live in `MC_TextureManager::masterTextureNodes` with stable
indices. **Handles mutate per-frame**: `gos_getTextureHandle(slot)` must
be re-resolved each draw — see `memory/mc2_texture_handle_is_live.md`.
The cache is evicted in `mcTextureManager->update()` (called inside
`Mission::update`, gated on not-paused) and re-populated when
`ObjectManager->update` re-touches shapes.

## Key Abstractions

**`TG_Shape` family (Microsoft DX-era):**
- Purpose: Articulated 3D shape with sub-shapes ("nodes") for animated
  parts (mech torso/arms/legs, building turret popups, weapon hardpoints).
- Variants: `TG_Shape` (base), `TG_MultiShape` (composite),
  `TG_AnimateShape` (keyframed). `TransformMultiShape` is the per-frame
  hot path that flattens hierarchies into world matrices.
- Examples: `mclib/mech3d.cpp:4170` (`Mech3DAppearance::update` calls into
  `TransformMultiShape`), `mclib/bdactor.cpp` (buildings),
  `mclib/gvactor.cpp` (ground vehicles).
- Pattern: per-type static state (`s_listOfLights`, `s_numLights`,
  `s_worldToCamera`) is reset by per-instance `init()`. Mismatched
  static-state reset is a latent-bug pattern (Stage 2.C regression).

**Appearance class hierarchy:**
- `ObjectAppearance` (abstract) → `Mech3DAppearance` (mechs) /
  `GVAppearance` (ground vehicles) / `BdgAppearance` (buildings) /
  `GenAppearance` (generic terrain objects).
- Each owns a `TG_MultiShape` + texture handles + LOD selection.
- Headers: `mclib/objectappearance.h`, `mclib/appear.h`,
  `mclib/apprtype.h`.

**Master vertex / shape nodes (deferred render queue):**
- `MC_TextureManager::masterVertexNodes[]` — legacy `gos_VERTEX*` per-frame
  vertex streams keyed by texture. Used by terrain quads, water, craters.
- `MC_TextureManager::masterHardwareVertexNodes[]` — modern
  `TG_RenderShape` entries (VB + IB + vdecl + MVP + viewport + light
  index). Used by all `TG_Shape` draws.
- Cleared each frame in `Mission::update` via `clearArrays()`.

**Camera (`eye`):**
- Singleton `Camera* eye` (class in `mclib/camera.cpp`); subclassed as
  `GameCamera` (`code/gamecam.cpp`, 626 lines) for in-mission, and
  `SimpleCamera` (`code/simplecamera.cpp`) for logistics screens.
- Holds projection, view, fog parameters, shadow-relevant axes. Oblique
  RTS-style perspective (~30° pitch, 360° yaw, cinematic-capable —
  see `memory/camera_model_oblique_cinematic.md`).

**FastFile / .fst archive:**
- `mclib/fastfile.cpp`, `mclib/ffile.cpp` — Microsoft's pak format.
  Keys use forward slashes; embedded paths use backslash (load-bearing
  invariant in `File::open`).

## Entry Points

**`main()`** (`GameOS/gameos/gameosmain.cpp:282`):
- Builds SDL window + GL 3.0+ context, calls `glewInit`, installs OpenGL
  debug callback, calls `gos_CreateRenderer`, `gos_CreateAudio`, then
  `Environment.InitializeGameEngine()` — that's the hand-off to game code.

**`InitializeGameEngine`** (`code/mechcmd2.cpp:825`):
- Loads resource DLL (`mc2res_64.dll` on Win64), checks swap-file space,
  shows EULA, loads game/system .fit files, brings up subsystems
  (heaps, fastfiles, paths, sounds, prefs, widgets, then logistics or
  splash). One-shot.

**`DoGameLogic`** (`code/mechcmd2.cpp:2024`):
- Called every frame. Runs MPlayer update, then either `logistics->update`
  (menus) OR `mission->update` (in-mission). Handles options dialog,
  save/load triggers, quit.

**`UpdateRenderers`** (`code/mechcmd2.cpp:676`):
- Called every frame from `draw_screen` via `Environment.UpdateRenderers()`.
  Sets render states, calls `mission->render()` (which queues into
  MC_TextureManager). The actual GL submission happens later in
  `gos_RendererEndFrame` → `renderLists`.

## Architectural Constraints

- **Threading:** single-threaded engine loop. Tracy GPU zones use timer
  queries on the same context; SDL audio mixer runs on its own thread but
  is opaque. No worker pool.
- **Global state:** pervasive. `eye`, `land`, `ObjectManager`,
  `mcTextureManager`, `userInput`, `soundSystem`, `craterManager`,
  `weather`, `PathManager`, `SensorManager`, `MPlayer`, `Commander::home`,
  `Team::home`, plus class-level `static` state on `TG_Shape`
  (`s_listOfLights`, `s_numLights`, `s_worldToCamera`).
- **Pool exhaustion is silent.** `vertexPool->getVerticesFromPool()`
  returning NULL → `TG_Shape::Render` early-out → shape vanishes with no
  error. Pools sized at 500K.
- **Cull gates are load-bearing.** `inView`/`canBeSeen`/
  `objBlockInfo.active`/`objVertexActive` chain gates not only
  visibility but also `update()` calls, TGL pool allocation, and object
  lifecycle (`update()` false return → `setExists(false)` → destruction).
  Bypassing the cull cascades.
- **`LINUX_BUILD` is defined globally**, even on Windows
  (`CMakeLists.txt:55`). Code branching on `_WIN32` for path separators is
  wrong — `PATH_SEPARATOR` is `"/"`.
- **Texture handle is live.** Cache the slot index, never the handle.
- **Shader `#version`** is injected as a string prefix (`"#version 430\n"`)
  via `makeProgram()` — never put it in the GLSL files themselves.

## Anti-Patterns

### "Bypass the broken cull"

**What happens:** A subsystem disables `inView` or `canBeSeen` checks to
fix a vanishing object.
**Why it's wrong:** Those checks also gate `update()`, pool allocation,
and lifecycle. Bypass causes streak artifacts (stale matrices), permanent
destruction (`update()` returns false on stale state), or silent shape
drop-outs from pool exhaustion.
**Do this instead:** Fix the cull predicate at its source, or stamp
per-frame markers (`cachedFrame_` on `TG_MultiShape`) to skip stale state
in the registry flush (see `memory/cull_gates_are_load_bearing.md` and the
black-tree-bug resolution).

### "It works in CPU mode, the GPU path is just an alt"

**What happens:** A modernization slice adds a parallel GPU path
(`g_useGpuStaticProps`, GPU compute cull) but the CPU path keeps doing
work; the GPU path becomes a half-rewritten fork.
**Why it's wrong:** State diverges silently — substrate=OFF on
nifty-mendeleev currently renders zero static props
(`memory/substrate_off_renders_no_static_props.md`). A "passing"
visual smoke under one path can be a false-positive.
**Do this instead:** Run parity from the start of any CPU→GPU port — see
`memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`. Treat
the GPU path as the authority once it ships default-on; remove the legacy
path or gate it to a clear killswitch.

### "Cache the texture handle"

**What happens:** Store `gos_TextureHandle` in an actor at construction
time and reuse it next frame.
**Why it's wrong:** `MC_TextureManager` mutates handles per frame as
slots are reassigned. Cached handle → wrong texture or invalid.
**Do this instead:** Store the slot/node index; call `gos_getTextureHandle`
at draw time. See `memory/mc2_texture_handle_is_live.md`.

### "Modern visuals are required for the game to look right"

**What happens:** A new render feature (PBR splat, normal-mapped
buildings, modern lights) hard-fails if the modern asset sidecar is
missing.
**Why it's wrong:** Stock install must remain playable. Savegames and
campaign files must not depend on the modern path.
**Do this instead:** Generate sidecar data at cache time from stock
assets; degrade gracefully if absent. See
`memory/stock_install_must_remain_playable.md`.

## Error Handling

**Strategy:** Hybrid. Engine layer uses C-style return codes (`NO_ERR`,
positive integers, mission status codes like `mis_PLAYING`,
`mis_PLAYER_WIN_BIG`) and assertions (`gosASSERT`, `STOP`, `Fatal`).
Modern OpenGL is monitored via the `glDebugMessageCallbackARB` callback
in `gameosmain.cpp:264` plus `[GL_ERROR v1]` drain loop instrumentation
(default print-on).

**Patterns:**
- `gosASSERT(cond)` for invariants — fires in debug.
- `STOP(("msg"))` / `Fatal(...)` for unrecoverable.
- Mission and ABL return integer status; the main loop interprets.
- Tracy zones bracket subsystems for performance regressions.
- Env-gated `[SUBSYSTEM]` lifecycle prints (the "Debug Instrumentation
  Rule" in the worktree CLAUDE.md).

## Cross-Cutting Concerns

**Logging:** `SPEW(("CATEGORY","fmt", ...))` macro throughout — prints to
stdout. The "[INSTR v1]" startup banner reports which env-gated
instrumentation is active.

**Validation:** GameOS adds `gos_validate.cpp/.h` (untracked in current
status) for new debug checks. `CHECK_GL_ERROR` macro after risky calls.

**Authentication / multiplayer:** `MPlayer` singleton (`code/multplyr.cpp`)
wraps direct TCP/IP session setup; not hooked into any modern auth.

**Configuration:**
- `options.cfg` in user config directory — runtime options
  (display, audio, controls).
- `system.cfg` at install root — engine constants.
- `prefs` global (`code/prefs.cpp`) wraps both at runtime.
- Env vars gate modernization paths: `MC2_GPU_CULL_*`,
  `MC2_STATIC_UPDATE_SKIP`, `MC2_TGL_POOL_TRACE`,
  `MC2_DESTROY_TRACE`, `MC2_HEARTBEAT`, `MC2_WALL_SHADOW_TRACE`, etc.

**Modernization fault lines (where CPU and GPU paths visibly coexist):**
- **Static props:** legacy CPU path vs `GpuStaticPropRegistry` /
  `TransformMultiShape_BuildRecipe`. Default mode depends on branch.
- **Cull:** legacy `inView` chain vs GPU compute cull
  (`MC2_GPU_CULL_LIFECYCLE`, `MC2_GPU_CULL_SUBSTRATE`).
- **Terrain water:** legacy `Terrain::renderWater` (per-quad CPU loop) vs
  SSBO recipe + single-draw fast path
  (`memory/water_ssbo_pattern.md`).
- **Render queue:** the two parallel master-node arrays
  (`masterVertexNodes` flat `gos_VERTEX` stream vs
  `masterHardwareVertexNodes` modern `TG_RenderShape` with UBO bindings)
  are the literal seam between the eras.

These are honest seams — both halves are in the tree, both ship in some
configuration. Treat any "X retires Y" claim with skepticism and verify
against `MEMORY.md` and the worktree CLAUDE.md before assuming a path is
dead code.

---

*Architecture analysis: 2026-05-14*
