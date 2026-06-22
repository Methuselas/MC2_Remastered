# XFORM-CONVENTION-LEDGER-1 — per-subsystem transform/matrix ownership

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC · **Slice:** XFORM-CONVENTION-LEDGER-1 (docs-only, no code, no build) · **(2026-06-22)**
**Built against:** nifty-mendeleev HEAD `eab7924d`. All `file:line` citations are cheap to drift — re-grep before relying on a line number.

## Purpose

A single ledger of who owns the world / model / view / projection transform for each render path,
and which space the shader actually receives. This dimension was **PARTIAL** in
`render-contract-index-1.md` (no per-subsystem ownership doc). The Vulkan port needs it because
Vulkan has no fixed-function matrix stack, and clip-space conventions (Y-flip, depth range, winding,
reverse-Z) must be explicit per pass — there is no implicit GL default to inherit.

Cross-references:
- `docs/render-backend-seams/render-contract-index-1.md` — coverage matrix (this slice closes the xform row).
- `docs/engine-standalone-seams.md` §"Modern-spine vs legacy pass routing" — the authoritative ViewUniforms-consumer-per-pass table that this ledger refines.
- `docs/render-contract.md` — coordinate-space / submission-bucket contract.
- `RenderCore/ViewUniforms.h` + `shaders/include/view_uniforms.hglsl` — the shared 144B ABI.
- `shaders/include/terrain_depth_bias.hglsl` + `mclib/terrain_depth_bias.h` — reverse-Z co-planar bias lockstep.

## The one fact that ties the whole engine together

There is exactly **one canonical clip matrix** per frame: `terrain_mvp_`, set by
`gos_SetWorldToClipGL()` (`GameOS/gameos/gameos_graphics.cpp:8956`). Its value is
**`kAxisSwapMC2toGL * worldToClip`** (the MC2→GL axis swap and the R-clipw polarity fold are baked
into the matrix; `gameos_graphics.cpp:8946-8948`). It is stored **row-major** and uploaded with
**`GL_FALSE`** (no GL transpose) at ~10 CPU bind sites.

Every GPU-direct subsystem consumes that same matrix through a uniform/UBO member **named
`u_worldToClipGL`**. The ViewUniforms UBO (binding=3) is simply an *alternate delivery channel* for
the same matrix value — its `worldToClipGL` is byte-compared equal to the legacy upload every frame
(`code/gamecam.cpp:333-344`, F1-3C parity probe). So for Vulkan, **there is a single
world→clip matrix to standardize**, delivered today by two mechanisms (per-program glUniform vs UBO
binding=3) that the legacy / modern split chose between.

Note on which matrix the UBO carries: the ViewUniforms UBO is filled in `code/gamecam.cpp:287-300`
from `eye->worldToClipGL()` (transposed col-major→row-major). The legacy `terrain_mvp_` is filled
from `gos_SetWorldToClipGL` with the same axis-swap baked in; the F1-3C probe confirms the two are
identical, so the UBO path and the flat-uniform path are interchangeable in value.

## Ownership table

| Subsystem | Owns which matrices | Where the transform lives | Space the shader receives | ViewUniforms (b=3) consumer? | file:line evidence | Vulkan note |
|---|---|---|---|---|---|---|
| **MECH** (GpuMechBatcher) | placement+pose folded into **per-bone** matrices (CPU FK bake); world→clip = shared `u_worldToClipGL` | bones in per-frame SSBO **binding 1** (`GpuMechBone` row0..row3); per-instance data SSBO **binding 0**; world→clip via **UBO binding=3** (default) or flat `glUniform mat4` (kill-switch) | model/local vertex `a_position`; shader does `boneT*v` → Stuff frame → in-shader **Stuff→GL axis swap** `(-x,z,y)` → `u_worldToClipGL` | **YES, default-ON** (kill-switch `MC2_VIEW_UNIFORMS=0`) — *not* opt-in | `shaders/mech.vert:48-68,127-165`; gate `gos_mech_batcher.cpp:137-140,548-550`; loc `:603` | bones already CPU-baked → push as SSBO unchanged. Axis swap is in-shader (fine for VK). No per-instance model matrix — placement IS the bones. |
| **STATIC PROPS** (GpuStaticPropBatcher) | explicit **per-instance MODEL matrix**; world→clip = shared `u_worldToClipGL` | `modelMatrix` in per-instance SSBO **binding 0** (`Instance.modelMatrix`, Stuff row-vec convention, `v*M`); world→clip via **UBO binding=3** (default) or flat uniform (kill-switch) | model vertex `a_position`; shader does `v*modelMatrix` → Stuff frame → in-shader **Stuff→MC2 axis swap** `(-x,z,y)` → `u_worldToClipGL` | **YES, default-ON** (F1-3D flip; kill-switch `MC2_VIEW_UNIFORMS=0`) | `shaders/static_prop.vert:33-45,56,70-74,240-284`; gate `gos_static_prop_batcher.cpp:1143-1156,608-617` | model matrix is `v*M` (row-vec). VK GLSL is column-vec by default — either keep the `v*M` order or transpose at upload. Normal uses `a_normal*mat3(M)` (no inverse-transpose; assumes uniform scale). |
| **TERRAIN** (chunk/indirect/thin) | **global** world→clip only (no per-object matrix) | `terrain_mvp_` cached in renderer; uploaded **directly via `glUniformMatrix4fv(loc, GL_FALSE, terrain_mvp_)`** to `u_worldToClipGL` at each bind site | MC2 world (x=east, y=north, z=elev) → `u_worldToClipGL` | **NO** — passive snapshot row only (does not read binding=3) | upload `gameos_graphics.cpp:6801-6809,6513-6514,9391-9395`; loc `:2371,:2340,:5064`; routing `engine-standalone-seams.md:294` | The reference matrix. Baked-clip vs live-projected-MVP consistency held by the shared `terrain_mvp_` + the reverse-Z co-planar bias (below), NOT by divergent per-pass projection. |
| **WATER** (renderWaterFastPath, Bucket B1) | global world→clip only | flat `glUniform mat4 u_worldToClipGL` (= the same axis-swapped MVP), set by `Terrain::renderWaterFastPath` | world (recipe corner XY + elev) → `u_worldToClipGL`; **+`WATER_DEPTH_FUDGE_FAST*clip.w` pre-divide** | NO | `shaders/gos_terrain_water_fast_mdi.vert:66,283-286`; bias `terrain_depth_bias.hglsl:44,47` | **CORRECTION:** does NOT use `g_dispatchMvp16` and does NOT carry a baked clip in the SSBO — it uses the **same `u_worldToClipGL`** as terrain, plus a co-planar depth epsilon. Intentional projected path (camera-dependent by design). |
| **PARTICLES / VFX** (gos_particle_bridge) | global world→clip + per-flush camera basis | flat `glUniform mat4 u_worldToClipGL` + `u_cameraRight`/`u_cameraUp` uniforms; particle data SSBO **binding 14** | particle center in Stuff/MC2 → in-shader axis swap `(-x,z,y)` → billboard expand in GL world → `u_worldToClipGL` | NO | `shaders/particle_billboard.vert:26-36,114-121` | GPU-projected (not CPU pre-projected). Camera basis is per-draw uniform → in VK a small push-constant or per-draw UBO. |
| **VEGETATION** (gos_vegetation cards) | global world→clip only | flat `glUniform mat4 u_worldToClipGL = gos_GetTerrainMVPMat4()` (same terrain MVP) | terrain-chunk space (east/north/elev) → `u_worldToClipGL`; **+1×`TERRAIN_DEPTH_FUDGE*clip.w`** | NO | `shaders/gos_vegetation_card.vert:8,27,165-167` | Shares the terrain matrix verbatim. Depth bias 1× (vs terrain's 2×) places veg between terrain and props under GL_GREATER. |
| **UI / HUD** (legacy 2D, gos_vertex.vert) | none — positions arrive **pre-projected (CPU screen-space)** | `uniform mat4 mvp` is **identity** for screen-space content; VS divides by `pos.w` | already-projected screen-space; fixed **800×600** HUD/GUI coordinate space | NO | `shaders/gos_vertex.vert:8,16-17`; `docs/recon/vehicles-render-path.md:120,132,287` | CPU pre-projection must move to a real ortho matrix for VK (no fixed-function, identity-MVP-with-screen-coords is a GL-ism). HUD is decoupled from the world viewport (fixed 800×600). |
| **POST-PROCESS** (gos_postprocess) | none — full-screen NDC quad | no model/view/proj; fragment samples FBO attachments | clip-space full-screen triangle/quad; NDC directly | NO | routing `engine-standalone-seams.md:300+`; `render-contract-index-1.md:84` | Trivial for VK (static quad). Poor counter/seam pilot per buffer-recon. |
| **SHADOW** (per-lane shadow programs) | light-space world→clip (separate matrix) | static/dynamic light-space matrices passed via the EngineView registry (`ShadowDirectional0-Static` / `ShadowDynamic` views) into each lane's own `u_worldToClipGL` upload | world → **light-space** clip | indirect: registered as **EngineViews** (`registerOrUpdateView`) but lanes upload per-program, not the binding-3 scene UBO | `gos_postprocess.cpp:3299-3304,3996-4001`; routing `engine-standalone-seams.md:293` | **FORWARD-Z** sub-pass (glClearDepth(1), GL_LESS) bolted onto the reverse-Z scene — see clip-space section. VK needs an explicit per-view matrix + its own depth convention; the GL state-leak guard (`gameos_graphics.cpp:5815-5897`) is a GL-ism that disappears under explicit VK pipelines. |

## GL clip-space conventions a Vulkan backend must translate

These apply to the **main scene** (terrain/mech/static-prop/water/particle/vegetation) unless noted.

1. **Reverse-Z, depth range [0,1].** `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` is asserted for
   the scene; `glClearDepth(0)`; depth compare is **`GL_GEQUAL`** (larger NDC z = nearer wins).
   Evidence: `gameos_graphics.cpp:5847-5850,5896,3145,3666,4616,8857` (every scene pass sets
   `GL_GEQUAL`); `terrain_depth_bias.hglsl:6-9`. **VK note:** VK is already [0,1] depth natively, so
   the GL `GL_ZERO_TO_ONE` adapter goes away, but the **reverse-Z convention (near→1, far→0) and the
   GEQUAL compare must be carried into the VK pipeline depth-compare-op + clear value** — they are a
   project design choice, not a GL default.
2. **Y / clip-origin.** GL is `GL_LOWER_LEFT` clip origin. **VK clip space is Y-down** (origin
   top-left) — the backend must apply a **Y-flip** (negate proj row 1, or flip viewport height) so
   the same `u_worldToClipGL` produces correct on-screen orientation. None of the shaders do a manual
   Y-flip today (they rely on the GL convention), so this is a backend-level fix, not per-shader.
3. **clip.w polarity / axis swap is baked into the matrix.** `kAxisSwapMC2toGL` folds the MC2→GL axis
   swap **and** an R-clipw polarity so in-front MC2 verts get `clip.w > 0`
   (`gameos_graphics.cpp:8946-8948`; `particle_billboard.vert:18-21`). Shaders deliberately do **no
   clip.w sign test** (`mech.vert:166-167`). VK can keep the baked matrix; just be aware the swap is
   pre-applied and some shaders additionally do an in-shader Stuff→MC2/GL swap on *position+normal*
   (mech, static-prop, particles) — those are model-space corrections, independent of the clip matrix.
4. **Matrix storage / upload convention.** `terrain_mvp_` is **row-major**, uploaded **`GL_FALSE`**
   (the D3D-derived chain math cancels the implicit transpose; `gameos_graphics.cpp:6618,3022`). The
   ViewUniforms UBO stores the same data row-major (`ViewUniforms.h:8-11,22-26`). GLSL reads `mat4`
   columns, so a VK port that switches to column-vector convention must transpose at upload (or keep
   the existing row-vec `v*M` multiply order used by static_prop). The **material cache uses `GL_TRUE`**
   — do not blanket-apply one transpose rule (per critical GL rules).
5. **Winding.** Default `GL_CCW` front face; particle billboards are explicitly authored CCW
   (`particle_billboard.vert:69-73`). VK default front-face differs by pipeline config — set
   explicitly to match.
6. **Co-planar depth bias (terrain/water/overlay/veg).** Reverse-Z flips the sign of every epsilon vs
   the old forward-Z/LEQUAL convention. Constants (lockstep C++/GLSL): `TERRAIN_DEPTH_FUDGE=-0.002`,
   `WATER_DEPTH_BIAS=-0.00175` (water loses the shoreline GEQUAL tie), `OVERLAY_DEPTH_BIAS=+0.00005`
   (decals win), `WATER_DEPTH_FUDGE_FAST=-0.0025`. Applied as `clip.z += bias*clip.w` pre-divide.
   `terrain_depth_bias.hglsl:43-47`. VK must preserve these (they encode draw-order correctness, not a
   GL quirk) and keep the reverse-Z sign convention.
7. **Forward-Z shadow sub-pass.** Shadow passes run **forward-Z** (glClearDepth(1), GL_LESS) inside
   the reverse-Z scene via a manual swap guarded by save/restore (`gameos_graphics.cpp:5815-5897`).
   VK should model shadow as a separate pipeline/render-pass with its own depth convention rather than
   a state swap.

## Corrections to the starting facts (what was wrong / stale)

1. **"Mechs route ViewUniforms (binding=3)" — TRUE and default-ON, not opt-in.** The mech gate
   `s_mechViewUniforms` is **DEFAULT-ON** with kill-switch `MC2_MECH_VIEWUNIFORMS=0`
   (`gos_mech_batcher.cpp:137-140`). The docstring inside `shaders/mech.vert:19-28` calling it "DEFAULT
   OFF" is itself stale relative to the C++ gate. Placement-folded-into-bones is **confirmed**
   (no per-instance model matrix).
2. **`shaders/include/view_uniforms.hglsl:10-11` comment "Not yet bound in runtime shaders" is STALE.**
   Both mech (default-on) and static-prop (default-on via the F1-3D flip,
   `gos_static_prop_batcher.cpp:1143-1156`) **do** consume the `ViewUniformsBlock` at binding=3 in
   shipping builds. The "contract-only fixture" framing in that header predates F1-3B/3D.
3. **WATER does NOT use `g_dispatchMvp16` or a baked clip in the SSBO.** It uses the **same
   `u_worldToClipGL`** as terrain (flat glUniform), plus `WATER_DEPTH_FUDGE_FAST` co-planar bias
   (`gos_terrain_water_fast_mdi.vert:66,283-286`). The "intentional projected (Bucket B1)" framing is
   correct, but the projection *source* is the shared axis-swapped MVP, not a separate dispatch matrix.
4. **Reverse-Z status is CONFIRMED (not merely "noted").** The scene is reverse-Z / [0,1] / GEQUAL
   today; there is no `gl_ClipControl`-pending TODO — it is the live convention
   (`gameos_graphics.cpp:5847-5850,5896`).
5. **Static-prop model matrix is `v*M` (row-vector Stuff convention), not `M*v`.** Load-bearing for any
   VK port that flips to column-vector GLSL (`static_prop.vert:257-260,350`).

## UNVERIFIED / out of scope

- Editor (`EditRel.exe`) preview/asset-viewer transform path (`ModelPreviewEngineShader.cpp`,
  `tools/asset_viewer/`) was not traced here — it is GPU-path-only and may differ; mark UNVERIFIED for
  this ledger.
- The exact `worldToViewGL` consumer set (binding=3 member 2) beyond mech specular's camera-position
  use was not exhaustively traced; `cameraWorldPos`/`worldToViewGL` are present in the ABI but most
  scene shaders only read `worldToClipGL`.
- The `gpu_driven_water.comp` / `gpu_driven_terrain_solid.comp` compute-side matrix handling
  (vs the raster water VS documented here) was not opened; if a VK port targets the compute path,
  re-verify which matrix those consume.
