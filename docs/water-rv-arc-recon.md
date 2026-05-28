# Water R→V Arc Recon (WATER-ARC-RECON-0)

Pass-level analog to [`static-prop-rv-arc-audit.md`](static-prop-rv-arc-audit.md)
and [`terrain-rv-arc-recon.md`](terrain-rv-arc-recon.md). Maps the water
pipeline end-to-end so Track R substrate gaps are visible before any Track V
(visual) work. **Docs-only artifact**, no code changes.

Lane worktree: `claude/water-rv-lane` (off `claude/nifty-mendeleev`).
Recon HEAD: `f37a634d`. Water is called out as out-of-scope in the terrain
audit and handled here as its own lane.

## TL;DR

Water is **further along visually than terrain but messier in substrate** —
the opposite shape of the terrain lane:

- **Three coexisting draw paths** for the same water plane: (A) legacy CPU
  quad path, (B) legacy thin-record fast path, (C) GPU-driven MDI fast path.
  Plus one **dead** mask-water path (D). Which one runs depends on env gates
  (`MC2_RENDER_WATER_FASTPATH`) and `gpu_driven::IsWaterEnabled()`.
- **Two different fragment shaders** depending on path: `gos_tex_vertex.frag`
  (legacy sin-wave look) vs `gos_terrain_water_mdi.frag` (the modern
  Beer-Lambert + fBm "water-v1" model). The good-looking water lives ONLY in
  the MDI FS, and only when the GPU-driven gate is on.
- **ViewUniforms NOT consumed** — water is 100% legacy `u_worldToClipGL`. No
  EngineView, no binding=3 UBO, no fallback branch. (Same gap terrain has.)
- **Debug visibility is asymmetric:** the fast *vertex* shaders already have a
  6-mode `debugMode` uniform (geometry-space: magenta/corner/elevation), but
  the *fragment* shaders — where albedo/alpha/depth/shore/lighting live — have
  **no debug uniform at all**. A real water debug-views slice must land a FS
  uniform.
- Color/material is **baked shader constants** (no per-biome data, no normal
  map, no flow map, no shore-mask texture; fresnel/reflection are
  compile-time dead). Water authority reduces to **one global plane height +
  a per-vertex underwater/wave-bob flag byte**.
- **Zero capture / JSON coverage:** no water `TRACKED_FLAGS` key, no water
  preset, no water section in debug-state JSON. 21 `MC2_WATER*` envs exist
  in code; `docs/tier1_env_vars.md` documents **none**.

## 1. Water authority chain

| Data | Owner (file:line) | Consumer |
|---|---|---|
| Per-vertex water flag (`PostcompVertex::water` byte; bit0=underwater, bits6/7=wave-bob marker) | [mclib/mapdata.cpp:588-595](../mclib/mapdata.cpp) `calcWater` (mirror `recalcWater` 629-635) | recipe build [gos_terrain_water_stream.cpp:303-307](../GameOS/gameos/gos_terrain_water_stream.cpp); legacy walk [terrain.cpp:2078](../mclib/terrain.cpp) |
| **Water plane height** (single global `waterElevation = wDepth + sDepth`) | [mclib/mapdata.cpp:611](../mclib/mapdata.cpp); stored [terrain.cpp:138](../mclib/terrain.cpp); persisted .fit [terrain.cpp:2345-2384](../mclib/terrain.cpp) | uniform [gos_terrain_water_fast_mdi.vert:67](../shaders/gos_terrain_water_fast_mdi.vert); bridge [terrain.cpp:1486](../mclib/terrain.cpp) |
| Per-quad geometry (`WaterRecipe` 64 B; mission-static, dense over map) | [gos_terrain_water_stream.cpp:281-307](../GameOS/gameos/gos_terrain_water_stream.cpp) `BuildDenseRecipe` | SSBO **binding 5**, [gos_terrain_water_fast.vert:31](../shaders/gos_terrain_water_fast.vert) |
| Per-frame state (`WaterThinRecord` 48 B: light/fog/pz) | [gos_terrain_water_stream.cpp:429/566-595](../GameOS/gameos/gos_terrain_water_stream.cpp) (CPU) or compute cull (GPU) | SSBO **binding 6**, [gos_terrain_water_fast.vert:49](../shaders/gos_terrain_water_fast.vert) |
| Per-draw cmd (`WaterPerCmd` 32 B; exactly 2 records: base+detail) | [gameos_graphics.cpp:2482-2486](../GameOS/gameos/gameos_graphics.cpp) (`static_assert sizeof==32` :2217) | SSBO **binding 7**, [gos_terrain_water_fast_mdi.vert:55](../shaders/gos_terrain_water_fast_mdi.vert), indexed by `gl_DrawIDARB` |
| Base albedo tga (`<colormap>.tga`, default fallback) | [terrtxm2.cpp:1624-1642](../mclib/terrtxm2.cpp) `init`; accessor `getWaterTextureHandle` | unit 0, [gameos_graphics.cpp:2538](../GameOS/gameos/gameos_graphics.cpp) |
| Detail/spray tgas (256 frames, animated) | [terrtxm2.cpp:1083-1125](../mclib/terrtxm2.cpp) (`MAX_WATER_DETAIL_TEXTURES=256`) | unit 1, [gameos_graphics.cpp:2539](../GameOS/gameos/gameos_graphics.cpp) |
| Tiling factor (.fit `WaterTextureTilingFactor`, default 48.0) | [terrtxm2.cpp:96,1686](../mclib/terrtxm2.cpp) | [terrain.cpp:1454-1458](../mclib/terrain.cpp) → uvScale |
| Color / tint / alpha / Beer-Lambert constants | [gos_terrain_water_mdi.frag:41-55](../shaders/gos_terrain_water_mdi.frag) **compile-time `const`** | FS :106-118,163 |
| Alpha-band DWORDs (edge/middle/deep) | `Terrain::alphaEdge/Middle/Deep`, `MapData::alphaDepth` | uniforms [terrain.cpp:1473-1521](../mclib/terrain.cpp); classifier [water_stream.cpp:699](../GameOS/gameos/gos_terrain_water_stream.cpp) |
| Shore ramp (`WaterThickness` vs `alphaDepth` → discard) | [gos_terrain_water_mdi.frag:113-115,163](../shaders/gos_terrain_water_mdi.frag) | in-FS |
| Shore foam (screen-space) | [shaders/shoreline.frag](../shaders/shoreline.frag); [gos_postprocess.cpp:773-799](../GameOS/gameos/gos_postprocess.cpp) | post-process pass |
| **Normal map** | — | **ABSENT** (procedural fBm only, mdi.frag:84-104) |
| **Flow / scroll map** | — | **ABSENT** (UV offset on detail tex only, gameos_graphics.cpp:2483) |
| **Fresnel / reflection** | mdi.frag:157 | **DEAD** (`const bool S3_REFLECTION_ENABLED=false`, mdi.frag:63) |
| **Baked shore-mask texture** | — | **ABSENT** (runtime-derived only) |

**Authority rule (preserve):** `waterElevation` (single global plane) and
`PostcompVertex::water` are the data ground truth. Gameplay (pathing, depth
checks) reads these on the CPU — any render-side change must not feed back.

## 2. Shader / pass inventory

| # | File | Stage | Status |
|---|---|---|---|
| 1 | [gos_terrain_water_fast.vert](../shaders/gos_terrain_water_fast.vert) | VS | LIVE — `water_fast_prog_` (legacy thin-record fast path B) |
| 2 | [gos_tex_vertex.frag](../shaders/gos_tex_vertex.frag) | FS | LIVE — paired FS for fast + mask water (legacy sin-wave look) |
| 3 | [gos_terrain_water_fast_mdi.vert](../shaders/gos_terrain_water_fast_mdi.vert) | VS | LIVE (gated) — `s_waterMdiProg`, only if `gpu_driven::IsWaterEnabled()` |
| 4 | [gos_terrain_water_mdi.frag](../shaders/gos_terrain_water_mdi.frag) | FS | LIVE (gated) — the modern "water-v1" model |
| 5 | [gos_terrain_mask_water.vert](../shaders/gos_terrain_mask_water.vert) | VS | LIVE-but-inert — `mask_water_prog_`, Stage 1c soak, writes masked off |
| 6 | [gpu_driven_water.comp](../shaders/gpu_driven_water.comp) | CS | LIVE (gated) — cull/pack → `WaterThinRecord[]` (not visual) |
| 7 | [shoreline.frag](../shaders/shoreline.frag) | FS (post) | LIVE — `shorelineProg_`, `runShoreline()`, default-on |
| 8 | [gos_terrain.frag](../shaders/gos_terrain.frag) | FS | water-ADJACENT only (zeros splat weight on water material; not water surface) |

No tesc/tese, no godray/caustic/ripple water shaders. Mask-water *draw*
(path D, `DrawMaskWater`/`gos_terrain_bridge_drawMaskWater`,
[gos_terrain_mask_dispatch.cpp:295](../GameOS/gameos/gos_terrain_mask_dispatch.cpp))
is fully implemented but **unreferenced — dead code** (sibling `DrawMaskSolid`
is live at [txmmgr.cpp:2192](../mclib/txmmgr.cpp)).

### Draw paths (C++)

| Path | Draw call | Site | When |
|---|---|---|---|
| A legacy CPU water | buffered quads → `gos_State_Water` drain | producer [quad.cpp:2514](../mclib/quad.cpp); loop [terrain.cpp:1337-1361](../mclib/terrain.cpp); drain [gameos_graphics.cpp:5753](../GameOS/gameos/gameos_graphics.cpp) | fallback when fast path not armed |
| B fast / thin-record | `glDrawArrays(GL_TRIANGLES)` ×1–2 | [gameos_graphics.cpp:2693/2719](../GameOS/gameos/gameos_graphics.cpp) | `MC2_RENDER_WATER_FASTPATH` set, MDI off |
| C fast / GPU-driven MDI | `glMultiDrawArraysIndirect` | [gameos_graphics.cpp:2605](../GameOS/gameos/gameos_graphics.cpp) | `gpu_driven::IsWaterEnabled()` + armed (`mdiValid` :2461) |
| D mask-water | `glDrawArraysIndirect` | [gameos_graphics.cpp:3643](../GameOS/gameos/gameos_graphics.cpp) | **DEAD — no caller** |

A vs B/C mutually exclusive: `WaterFastPathOwnsArmedDraw()` early-return
[terrain.cpp:1298-1302](../mclib/terrain.cpp).

### Frame position ([code/gamecam.cpp](../code/gamecam.cpp))

```
land->render() [terrain]            :287
craters                             :293
ObjectManager->render() [props/mechs]:302
land->renderWater() [LEGACY A only] :307   (early-returns if fast path armed)
renderShadows                       :313
renderLists() [drains terrain+legacy water quads] :334
land->renderWaterFastPath() [B/C]   :345   ← after renderLists so terrain depth written first
GPU particle flush                  :358
```

Water fast path is the **last opaque-scene composite before particles**.

## 3. Current visual model

| Path | FS | Look |
|---|---|---|
| A / B (legacy) | [gos_tex_vertex.frag](../shaders/gos_tex_vertex.frag) | `Color.bgra * tex1`; two sin-wave brightness perturbations (base vs detail/spray), faint specular glint, fog. The original MC2 sin-wave water. |
| C (MDI, "water-v1") | [gos_terrain_water_mdi.frag](../shaders/gos_terrain_water_mdi.frag) | Stylized **camera-independent** water: Beer-Lambert depth absorption `exp(-WaterThickness*ABSORPTION_DENSITY)` mixing `DEEP_COLOR`↔`SHALLOW_COLOR`; `WATER_MAX_ALPHA=0.87`; shore smoothstep + discard above waterline; dual counter-scrolling 3-octave fBm ripple brighten + crest glint with distance LOD fade; fog. `o_isWater==2` (detail) → discard; legacy sin block below (lines 168-221) is dead. |

Wave geometry (both VS): per-corner `waterBits` bit7 → ±`frameCos` bob added to
`waterElevation`; world-derived UV with `MaxMinUV` wrap correction. Depth bias
`clip.z += WATER_DEPTH_FUDGE_FAST * clip.w`.

**The visible quality difference is entirely the FS, and the good one (MDI) is
gated.** Reflection/fresnel scaffolding exists in mdi.frag but is compile-time
dead (`S3_REFLECTION_ENABLED=false`).

## 4. Current uniforms / textures

### Fast VS (`gos_terrain_water_fast(_mdi).vert`)
`mat4 u_worldToClipGL` (LEGACY, not ViewUniforms), `float waterElevation`,
`float alphaDepth`, `vec2 mapTopLeft`, `float frameCos`, `float frameCosAlpha`,
`float uvScale`, `vec2 uvOffset`, `int alphaEdge/Middle/DeepByte`,
`int detailMode`, **`int debugMode`** (modes 0-6), `float maxMinUV`.
(MDI variant drops uvScale/uvOffset/detailMode → reads from SSBO-7.)

### MDI FS (`gos_terrain_water_mdi.frag`)
samplers `tex1` (base, unit 0), `tex2` (detail, unit 1), `reflTex` (unit 2,
dormant); `int reflectionOn`, `float atlasMapTopLeftX/Y`,
`float atlasOneOverWorldUnits`, `vec4 fog_color`, `float time`,
`vec4 cameraPos`, `float alphaDepth`. Color/tint constants are
compile-time `const` (mdi.frag:41-55). **No debug uniform.**

### Legacy FS (`gos_tex_vertex.frag`)
sampler `tex1`; `vec4 fog_color`, `float time`, `int isWater`. **No debug uniform.**

### SSBO binding map (water)
`5`=WaterRecipe (64B, geometry), `6`=WaterThinRecord (48B, per-frame),
`7`=WaterPerCmd (32B, per-draw, 2 records), `18`=water visibility mask.
Compute pass uses a separate local binding scheme (0/1/2/3/6).

## 5. View / resource / debug / inspector / capture gaps

| Area | State | Evidence |
|---|---|---|
| **ViewUniforms** | GAP — water 100% legacy `u_worldToClipGL`; no binding=3 UBO, no `MC2_VIEW_UNIFORMS` gate, no fallback | VS [gos_terrain_water_fast.vert:61](../shaders/gos_terrain_water_fast.vert); bind [gameos_graphics.cpp:2364,2523](../GameOS/gameos/gameos_graphics.cpp) |
| **EngineView** | ABSENT — no water reference in [EngineView.h](../RenderCore/EngineView.h) or view_uniforms_gl.cpp | — |
| **RenderResourceRegistry** | GAP — registers nothing for water. Candidates: recipe/thin/window/bucket-header/indirect SSBOs (would need new `RenderResourceId` enum values; `Buffer` kind already exists) | [RenderResourceRegistry.h:10-19](../RenderCore/RenderResourceRegistry.h); buffers [gos_terrain_water_stream.cpp:1352-1615](../GameOS/gameos/gos_terrain_water_stream.cpp) |
| **Water-owned FBO/RT** | ABSENT — shoreline post reads/writes SHARED scene targets only | [gos_postprocess.cpp:780-814](../GameOS/gameos/gos_postprocess.cpp) |
| **RenderDebugView mask** | GAP — no `kDebugViewMask_Water`; placeholder pattern (`_Terrain/_Shadow/_Vfx = 0u`) is the clean slot | [RenderDebugView.h:23,36,43-45](../RenderCore/RenderDebugView.h) |
| **Water debug uniform** | PARTIAL — fast VS has `int debugMode` 0-6 (geometry-space) via `MC2_RENDER_WATER_FASTPATH_DEBUG`; **FS has none** (where material/depth/shore/lighting live) | VS [gos_terrain_water_fast.vert:82,283-318](../shaders/gos_terrain_water_fast.vert); env [gameos_graphics.cpp:2369-2374](../GameOS/gameos/gameos_graphics.cpp) |
| **ImGui controls** | MINIMAL — one env toggle (`MC2_RENDER_WATER_FASTPATH`) + one read-only program-id line. No tuning sliders, no debug-mode selector | [GraphicsOptionsWindow.cpp:101](../GuiRuntime/GraphicsOptionsWindow.cpp); [EditorInspector.cpp:983](../GuiRuntime/EditorInspector.cpp) |
| **Capture preset / TRACKED_FLAGS** | GAP — no water key in [capture_baseline.py](../scripts/capture_baseline.py) `TRACKED_FLAGS`, no water preset in [presets.json](../tests/visual/baselines/presets.json). Standalone `water_visual_diff.py` exists, not integrated | — |
| **Debug-state JSON** | GAP — zero water matches in [debug_state_dump.cpp](../GameOS/gameos/debug_state_dump.cpp); no water section (cf. staticPropOpaque/mech) | — |
| **Env registration** | PARTIAL — 21 `MC2_WATER*`/`MC2_RENDER_WATER*` envs in code; subset in `check-env-registry.sh`; **zero in `docs/tier1_env_vars.md`** | [check-env-registry.sh:125,187-189,236,262-269](../scripts/check-env-registry.sh) |

## 6. Classification

| Property | Verdict | Evidence |
|---|---|---|
| Pass | Standalone, after terrain renderLists flush; shares terrain MVP, own program/state | [gamecam.cpp:345](../code/gamecam.cpp) |
| Blend | Alpha `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | [gameos_graphics.cpp:2429-2430](../GameOS/gameos/gameos_graphics.cpp) |
| Depth write | OFF (`glDepthMask(GL_FALSE)`) | :2431 |
| Depth test | ON, `GL_GEQUAL` (reverse-Z) | :2427-2428 |
| Shadows | NO — neither FS samples shadow map / `calcShadow` | gos_tex_vertex.frag, mdi.frag (no token) |
| GBuffer | Forward; MRT loc1 GBuffer1 = flat-up normal + shadow-eligible mask. No deferred | mdi.frag:164 |
| Fog | YES — `fog_color` uniform → FS mix | bind :2398/:2536; FS mdi.frag:129-130 |
| Reflection | Dead-stripped | mdi.frag:63 |

## 7. Recommended next slices

1. **WATER-DEBUG-VIEWS-1** (next; code). The fast VS debug modes are
   geometry-space only and do not cover the material/depth/shore terms that
   matter visually. Land a **FS** debug uniform. Decision required up front:
   target the **MDI FS** (`gos_terrain_water_mdi.frag`, the modern path) since
   that is the one worth improving — but it is gated by
   `gpu_driven::IsWaterEnabled()`, so debug captures must run with that gate
   on. Backed-by-real-data modes for the MDI FS:
   - 0 Final (default)
   - 1 Base/tint (DEEP↔SHALLOW mix pre-ripple — real)
   - 2 Alpha (WATER_MAX_ALPHA × shore ramp — real)
   - 3 Normal — **flat-up only** (GBuffer1 is constant `(0,0,1)`; honest but
     uninformative; document as "no real surface normal")
   - 4 Depth/Beer-Lambert factor (`WaterThickness`, `exp(-density·d)` — real)
   - 5 Shoreline mask (shore smoothstep term — real)
   - 6 Lighting/ripple term (fBm brighten + glint — real; reflection ABSENT)
   Prefer wiring through `kDebugViewMask_Water` (new placeholder slot) +
   env `MC2_WATER_DEBUG_MODE` + ImGui selector. Requires shader_reflect golden
   refresh (new FS uniform) + env_registry entry + `tier1_env_vars.md` entry.

2. **WATER-BASELINE-0** (after debug views). Add a water `TRACKED_FLAGS` key
   (`MC2_WATER_DEBUG_MODE`, `MC2_RENDER_WATER_FASTPATH`, `MC2_GPU_DRIVEN_WATER`)
   and a water-heavy preset to `presets.json`. Need to pick the missions
   (see open questions). Capture default + each real debug mode.

3. **WATER-TUNING-UI-1** (Batch 2). Real existing-but-baked values that could
   be promoted to uniforms safely: the mdi.frag color/absorption/alpha
   consts (spec comment already says "promote to UBO at water-v2"), alpha-band
   DWORDs, detail UV scroll speed. **Caution:** today these are compile-time
   `const` — exposing them is a small shader change (FS uniform), not a pure
   C++ slice. Scope carefully.

4. **WATER-LIGHTING-PLAN-0** (Batch 2). Likely recommendation: first visual
   improvement is fog/sky-tint coherence or fresnel-tint (cheap, gated), NOT
   SSR/cubemap. Water already has the dead fresnel scaffolding to revive
   behind a gate. ViewUniforms migration is a prerequisite consideration
   (water has `cameraPos` but no view matrix from the UBO).

## 8. Risks / what's blunt today

1. **Three live paths, two FS, one gate matrix.** A change must state which
   path it targets. The modern look (MDI FS) is gated; the default-armed path
   in many configs is the legacy sin-wave FS. Easy to "improve water" and have
   it not show because the wrong path is active.
2. **No FS debug uniform.** Cannot isolate albedo/alpha/depth/shore/lighting
   on the path that owns them. Blocks honest debug captures.
3. **Shoreline z-fight (known issue).** Quoted [known_issues.md:34](known_issues.md):
   *"Water shoreline z-fight on zoom/elevation-change (NOT pan); water sits
   slightly low (pre-existing). Interim fast-path fixes shipped 2026-05-17."*
   Mitigation is the GEQUAL depth-test block (gameos_graphics.cpp:2416-2428).
   Any depth-bias touch risks reopening this.
4. **ViewUniforms skew.** Water on legacy `u_worldToClipGL` while static-prop
   on UBO binding=3 — two view pipelines; risk if camera basis ever desyncs.
5. **Baked material constants.** No per-mission/biome water color; all missions
   share the same DEEP/SHALLOW palette. A tuning slice changes ALL water.
6. **No capture/JSON coverage.** A captured water frame is not reproducible
   (no TRACKED_FLAGS), and water has no inventory in debug-state JSON — cannot
   diff a regression structurally the way static-prop/mech can.
7. **Dead code adjacency.** `DrawMaskWater` path, mdi.frag detail/legacy/refl
   blocks are dead but live in the same files — edit risk of touching the
   wrong branch.
8. **Alpha blend + depth-write-off.** Overdraw on water-heavy missions;
   no sorting beyond GEQUAL occlusion. Perf risk if geometry density grows.
9. **Detail textures = 256 tgas.** Animated spray is a 256-frame flipbook;
   any texture-cook concern is out of scope but worth noting for perf/VRAM.

## 9. Open questions for the user

- **Which path is the V-lane target?** Recommend the **MDI FS** (modern
  water-v1) with debug captures run under `MC2_GPU_DRIVEN_WATER=1` /
  `gpu_driven::IsWaterEnabled()`. Confirm before WATER-DEBUG-VIEWS-1 picks a
  shader.
- **Env name:** `MC2_WATER_DEBUG_MODE` (matches the orchestrator brief) vs
  `MC2_WATER_DEBUG_VIEW` (matches static-prop family). The existing fast-VS
  control is `MC2_RENDER_WATER_FASTPATH_DEBUG` — do we fold into that or add a
  distinct FS-mode env?
- **Water-heavy missions** for baseline: needs a pick (tier1 = mc2_01/03/10/
  17/24; which have the most visible water?).

---

**Status:** docs-only artifact. No code touched. No build required.
Slice 1 of approved Batch 1 (WATER-ARC-RECON-0). Next slice
(WATER-DEBUG-VIEWS-1) is code+shader and needs the path-target decision in §9
resolved first.
