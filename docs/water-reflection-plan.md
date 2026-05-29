# Water Reflection Plan (WATER-REFLECTION-RECON-0)

Scopes water reflections on the matured water MDI lane. **Recon/plan only — no
implementation in this slice.** Direction: get the right **broad sky/cloud
reflection shape** first (shader-only), then optionally a **low-res (~1/4)
terrain reflection** as a separate pass/resource arc.

Lane: `claude/water-reflection` (off `claude/nifty-mendeleev` `a1771f12`).
Target FS: [`shaders/gos_terrain_water_mdi.frag`](../shaders/gos_terrain_water_mdi.frag).
Pairs with [`water-rv-arc-audit.md`](water-rv-arc-audit.md),
[`water-lighting-plan.md`](water-lighting-plan.md).

## ⚠️ Ruling correction (load-bearing)

The "2026-05-17 camera-independence ruling" cited in the shader comments and the
earlier water docs is **SUPERSEDED**. Per the user 2026-05-18 (memory
`water_v2_s1_shipped_s3_blocked_reborn.md`): *"that decision was mostly due to
it being ugly as shit lmao, not some invariant principle."* **Camera-dependent
water reflection IS permitted if it looks good** — quality bar at the visual
gate, not a ban. This arc is the sanctioned revisit. The shader header comment
(`gos_terrain_water_mdi.frag:64-69`) is stale and should be updated when Phase A
lands.

## TL;DR — recommendation

1. **Phase A first (WATER-SKY-REFLECTION-1, shader-only):** revive the dormant
   S3 reflection scaffold but **swap the sample source from the terrain ground
   colormap to the SKY.** Use the **SH-L2 irradiance** set (`IblShCoeffs.h`,
   derived from the real loaded HDRI) evaluated along the world-space reflected
   ray for a smooth, **orbit-stable broad sky shape** — this directly fixes the
   prior failure. Gated `MC2_WATER_REFLECTION`, **default OFF**, strength
   tunable in Graphics Options > Water.
2. **Phase B (WATER-REFLECTION-RESOURCE-1):** only after Phase A proves useful —
   add a 1/4-res reflection render target (RenderResourceRegistry slot +
   EngineView descriptor + ImGui debug preview). No rendering into it yet.
3. **Phase C (WATER-TERRAIN-REFLECTION-1):** render terrain (+sky) into the
   1/4-res target via a mirrored camera, water samples it. Separate slice;
   MODERATE effort, water-height clip plane is the HARD sub-task.
4. **Phase D:** polish — wave-normal distortion, distance fade, sky+terrain
   blend, Fresnel tuning.

Sky-first because the **prior attempt's only real flaw was the source**: it
reflected the top-down terrain *ground colormap atlas*, so the water hue swung
as the camera orbited ("compass hue swing"). The reflect-vector + Fresnel +
waveLOD-mix math was fine. Sky is roughly uniform/directional → orbit-stable.

## 1. Existing water reflection scaffolding

| Item | State | file:line |
|---|---|---|
| S3 reflection block | **present, compile-time DEAD** (`const bool S3_REFLECTION_ENABLED=false`) | [mdi.frag:70, 144-173](../shaders/gos_terrain_water_mdi.frag) |
| Reflect-vector math | reusable: `vdir=normalize(cameraPos-WorldPos)`, `waveNormal` from fBm `dFdx/dFdy(nz)`, `rdir=reflect(-vdir,waveNormal)` | mdi.frag:151-154 |
| Sample source (PRIOR) | **terrain ground colormap atlas** `reflTex` (unit 2), reflected-ray march in world-XY → atlas UV | mdi.frag:157-167 |
| Fresnel + mix | Schlick `pow(1-max(vdir.z,0),5)`, `mix(col,refl,clamp(fres*REFL_STRENGTH*waveLOD,0,REFL_MAX))` | mdi.frag:169-172 |
| Uniforms/consts | `reflTex`, `reflectionOn`, `atlas*`, `REFL_F0=0.02/STEPS=5/STEP_LEN=96/STRENGTH=0.35/MAX=0.22/WAVE_SLOPE=0.05` | mdi.frag:27-33, 70-76 |
| C++ bind | unit-2 atlas bind, `reflOn` arm gate, save/restore, `MC2_WATER_REFL_TRACE` | [gameos_graphics.cpp:2617-2638, 2664-2689](../GameOS/gameos/gameos_graphics.cpp) |
| Sky-tint (NOT reflection) | `u_waterSkyTintColor`/`u_waterSkyTintStrength` — flat camera-indep tint, default 0 | mdi.frag:41-42, 140 |
| Water normal | **flat (0,0,1)** in GBuffer; scalar fBm `nz` in scope (perturbable) | mdi.frag:111, 190/195/252 |

**Previous failure mode:** SSPR rejected at design (oblique ~30° camera →
near-shore smear/holes). The chosen Option C (atlas-march) shipped, then
SHELVED `24190b98` — reflecting the ground colormap gave a compass-dependent hue
swing, "ugly as shit." Lesson: **wrong source, right machinery.**

## 2. Skybox / cloud source

| Source | Where | Access | Verdict |
|---|---|---|---|
| **SH-L2 irradiance (9×RGB)** from the HDRI | [`RenderCore/IblShCoeffs.h:35`](../RenderCore/IblShCoeffs.h); eval pattern `static_prop.vert::evalShL2` | header-only constexpr → inline a `const vec3[9]` + `evalShL2(dir_Yup)`; no texture, no TU, no mips | **BEST for broad shape** — smooth, orbit-stable, cheap, real-HDRI-derived |
| **HDRI equirect texture** | `data/hdr/DaySkyHDRI063B_4K.exr` → `hdriTex_` (private) [gos_postprocess.cpp:189] | needs a 1-line `getHdriTexture()` accessor + bind to a free TU; equirect map from `hdri_skybox.frag:30-33` | sharper, but **no mips** → roughness-blur needs prefilter; Phase D option |
| Analytic sky gradient | `shaders/skybox.frag:13-28` (DEAD path) | copy zenith/horizon/sun math inline; re-supply colors | fallback when `MC2_HDRI_SKY=0` / no HDRI |
| `u_waterSkyTintColor` | mdi.frag:42 (already wired) | reuse as **off-map/horizon fallback color** | crudest; pairs with any of the above |
| Sun dir | `gos_SetTerrainLightDir` → `terrainLightDir` [gamecam.cpp:262] | not bound to water MDI yet; add `glUniform4fv` | only if a sun-glow/specular term wanted |
| **Real clouds** | — | **ABSENT** — no cloud texture/procedural layer. (water "cloudOffset" is detail-UV scroll, not sky.) | clouds would be procedural-in-shader or deferred |

**Coordinate space (load-bearing):** the water reflect vector is **MC2 Z-up**;
HDRI and SH are **Y-up Stuff space**. Apply the MC2(Z-up)→Stuff(Y-up) axis swap
to `rdir` before `evalShL2`/HDRI sample (swap Y/Z), matching the convention the
sky/IBL already use.

## 3. Camera / view data

Readiness for building a reflection vector **today** (no new view plumbing):

| Input | Status |
|---|---|
| Camera pos (MC2 world) | ✅ `cameraPos` uniform (mdi.frag:36; from `terrain_camera_pos_`) |
| Surface pos | ✅ `WorldPos` (mdi.frag:22) — same space |
| View dir | ✅ constructible: `normalize(cameraPos-WorldPos)` (already at mdi.frag:102) |
| Surface normal | ⚠️ flat in GBuffer, but fBm `nz` in scope → buildable wave normal |
| View/inv-proj matrices | ❌ water still on legacy `u_worldToClipGL` (no ViewUniforms UBO) |

- **ViewUniforms is NOT a prerequisite for sky reflection** — `reflect(viewDir,
  normal)` + `evalShL2`/cubemap needs only world-space viewDir+normal, both
  present. Phase A needs zero matrix work.
- **ViewUniforms / a mirror matrix IS needed for Phase C** (true planar terrain
  RT reproject). Note the terrain draw consumes the **global
  `gos_GetTerrainMVPMat4()`**, not the EngineView UBO — that global is the
  mirror lever.
- **Normal quality:** the current `dFdx/dFdy(nz)` screen-space gradient is
  cheap but noisy/aliased at grazing angles. Recommend an **analytic gradient
  of the existing fBm** (2-4 extra `fbm3` taps at small WorldPos.xy offsets;
  seam-free, precision-safe, reuses `WAVE_FREQ`/`WAVE_SPEED`) for a believable
  wave normal — no new texture/asset.

## 4. Reflection target architecture (Phases B/C)

| Registry | Extension point | Verdict |
|---|---|---|
| RenderResourceRegistry | add `WaterReflectionColor`(+`Depth`) before `Count` ([.h:10-19](../RenderCore/RenderResourceRegistry.h)); add `toString` case; register as `Texture2D` (descriptive only — owner keeps GL lifetime) | **CLEAN** |
| EngineView | add `kWaterReflectionViewId=4` + `ViewKind::WaterReflection` ([.h:8-21](../RenderCore/EngineView.h)); `registerOrUpdateView` (cap 8, 4 used). NOTE: terrain ignores the View UBO → mirror via the MVP global, not `setCurrentView` | **CLEAN descriptor; matrix math new** |
| RenderDebugView preview | add `getWaterReflectionTexture()` accessor + one `ImGui::Image` block in `drawGBufferPreview()` ([GraphicsOptionsWindow.cpp:1156](../GuiRuntime/GraphicsOptionsWindow.cpp)); enum needs no new value | **CLEAN** |
| FBO create/resize | mirror the bloom **half-res** block at **w/4,h/4** + add depth attachment (like `sceneDepthTex_`) in `createFBOs/destroyFBOs/resize` ([gos_postprocess.cpp:334-449](../GameOS/gameos/gos_postprocess.cpp)) | **CLEAN** |

**Pass ordering** ([gamecam.cpp:284-378](../code/gamecam.cpp)): sky → terrain
queue → objects → shadows → `renderLists()` (terrain GPU draws fire) →
`renderWaterFastPath()` (water samples reflection here) → particles. A
reflection pass must run **before** `renderWaterFastPath()` and restore all
global state before the main `renderLists()`.

**Which objects into the target:** Phase C = **sky + terrain SOLID only**
(broad shape + near-shore ground). Static props/mechs **deferred** (more draws,
more state to mirror) — Phase D+ if ever.

**Terrain-into-mirror feasibility:** MODERATE. Model on the shadow pre-pass
[`beginDynamicShadowPass`](../GameOS/gameos/gameos_graphics.cpp) (gameos_graphics.cpp:5186):
save FBO/viewport + `gos_GetTerrainMVPMat4` backing matrix → set
`mirror(waterPlane)*worldToClip` → bind reflection FBO → re-dispatch terrain
SOLID compute + `DrawIndirect` + sky → restore → `invalidateRenderStateCache()`.
**Blockers:** (a) the MVP global is shared by ~8 consumers — save/restore
strictly before the main draw; (b) the SOLID indirect/thin ring buffers assume
one dispatch/frame — a second dispatch needs its own ring slot or re-pack;
(c) **no `gl_ClipDistance` infra exists** anywhere — clipping terrain at the
water height (so below-waterline geometry doesn't pollute the reflection) is the
**HARD** sub-task (projection is baked in the SOLID *compute* shader, not a
draw-time VS uniform). Mitigations: ship Phase C without clipping first
(accept minor artifacts), or use an oblique-near-plane trick on the mirror
matrix, or add a clip-distance emit to the compute/thin path (largest change).
- **Recursion:** trivially avoided — the reflection pass calls only sky +
  terrain, never `renderWater*`. Do NOT re-call `GpuStaticPropRegistry::frameBegin()`.

## 5. Terrain reflection feasibility (summary)

Possible **without a massive refactor** at 1/4 res via the mirrored-MVP +
second-dispatch approach (precedent: shadow pre-pass). Water-plane clip is the
only HARD piece and is **optional for a first cut**. No recursion risk. Fog:
reuse the existing `fog_color` blend so the reflected terrain fogs consistently;
shoreline foam (`shoreline.frag`) is screen-space and untouched.

## 6. Phased plan

### Phase A — WATER-SKY-REFLECTION-1 (shader-only; recommended first)
- Revive S3 scaffold; **swap source to sky** via `evalShL2(swizzle(rdir))` from
  inlined `IblShCoeffs` (+ `u_waterSkyTintColor` as horizon fallback).
- Reflect vector from existing cameraPos/WorldPos; wave normal from **analytic
  fBm gradient** (upgrade from the dFdx hack).
- Reuse Fresnel + `waveLOD` mix; new strength uniform.
- Gate `MC2_WATER_REFLECTION` **default OFF**; strength/Fresnel tunable in
  Graphics Options > Water; add a debug mode (7 Reflection) to the FS debug enum.
- Validate: build, shader_reflect, env_registry, tier1 5/5 gate-OFF
  (byte-identical), mc2_24 gate-ON `gl_errors=0`, user visual A/B. **No --kill-existing.**

### Phase B — WATER-REFLECTION-RESOURCE-1 (substrate; after A proves useful)
- 1/4-res FBO+color+depth in `gosPostProcess`; `RenderResourceId::WaterReflectionColor/Depth`;
  `kWaterReflectionViewId`/`ViewKind::WaterReflection`; ImGui preview (black until C).
- No visual change; default-inert.

### Phase C — WATER-TERRAIN-REFLECTION-1 (mirrored pass; separate slice)
- Mirrored-MVP terrain+sky into the 1/4-res target (model on shadow pre-pass);
  water samples it, blended under the Phase-A sky term by Fresnel/distance.
- Gate default OFF. Clip plane deferred/optional.

### Phase D — polish
- Wave-normal reflection-UV distortion, distance fade, sky↔terrain blend,
  Fresnel/roughness tuning, optional HDRI-texture (with prefilter) for sharper sky.

## Out of scope (whole arc, first implementations)
- SSR; full scene reflection (props/mechs) in the first RT; cubemap
  prefilter/PBR dependency; water physics/gameplay/pathing/collision; terrain/
  static-prop/mech/shadow/VFX behavior changes; default visual flips. Existing
  water MDI path + tuning preserved.

---

**Status:** docs-only recon/plan. Recommends Phase A (shader-only sky reflection
via SH-L2, gated default-OFF) as the next slice; RT/terrain reflection as
later, separately-approved slices. The prior camera-independence ban is
superseded — reflections judged on looks at the visual gate.
