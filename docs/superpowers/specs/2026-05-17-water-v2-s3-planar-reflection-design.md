# Water v2 - Slice S3: GPU-Driven Planar Terrain Reflection

**Date:** 2026-05-17
**Branch:** `claude/water-material-v1` (isolated; keep-as-is)
**Parent:** `2026-05-17-water-v2-scope-and-decomposition.md` (Section 3 ruling)
**Status:** BLOCKED by opus adversarial (2026-05-17). Spec premise false vs code: ComputeDispatch() mutates global ring/fence state (Fix-A/B), single shared g_indirectCmdBuffer already consumed by primary terrain MDI, reflected view needs own cull/window. Real S3 = dedicated reflection thin-SSBO+cmdbuf + parameterized compute (extracted from ring machinery) + decided cull + viewport/shadow reconcile. Needs terrain-indirect-expert + user scope decision. NOT plan-ready as written.
**Grounding:** code-grounded feasibility map (render-expert + terrain-indirect-expert,
2026-05-17) - all design choices below trace to grep-verified findings.

---

## 1. Goal / the one camera-dependent term

Add a planar reflection of the terrain in the armed MDI water, **GPU-driven**
(no CPU geometry re-traversal). Per the v2 camera-independence ruling, this
Fresnel-weighted terrain reflection is the **ONLY** camera-dependent term in
the entire water material; S1's surface stays `f(WorldPos,time)`.

**Non-goals (deferred):** reflecting props/mechs/craters; sky/cloud reflection;
wave-normal distortion of the reflection (later pure-FS add); dynamic
resolution; Option-B (eliminate the 2nd compute dispatch). S2 refraction stays
deprioritised. S5 UBO demand-gated.

## 2. Why the obvious approach does NOT work (load-bearing)

Post-Fix-B the terrain thin VS (`shaders/gos_terrain_thin.vert`) reads
**pre-projected** `clipPos[]` from the thin records; it has **no `terrainMVP`
uniform**. Clip positions are baked once per frame by the compute shader
`shaders/gpu_driven_terrain_solid.comp` from `uniform mat4 u_terrainMVP`
(uploaded in `gos_terrain_indirect::ComputeDispatch()`). So you cannot
"re-issue the terrain MDI with a different MVP uniform" - there is none. The
reflected view needs a **second set of clip positions** projected by a mirrored
MVP. That is still pure-GPU.

## 3. Design (Option A - provably correct, recommended)

Reflection pass, issued at the **top of `gosRenderer::renderWaterFastPath()`**
(`GameOS/gameos/gameos_graphics.cpp`, the armed water draw site, after
`renderLists()` so terrain data is ready), gated identically to the water
draw on `gos_terrain_indirect::IsFrameSolidArmed() && mdiValid`:

1. **Reflected MVP (CPU: one mat4 build+multiply - within budget).**
   `reflectedMVP = Mirror(elev = Terrain::waterElevation) * dispatchMVP`,
   where `dispatchMVP` is the **armed frame anchor**
   `gos_terrain_indirect_getDispatchMvp16()` - NOT a freshly sampled
   `terrain_mvp_` (see Risk R1). `Mirror` reflects the MC2 `elev` axis about
   the water plane (`elev' = 2*waterElevation - elev`), applied in MC2 world
   space (right factor, since `dispatchMVP` already carries the axis swap).
   Row-major / `GL_FALSE` upload convention (matches `terrainMVP`).

2. **Second compute dispatch** of `gpu_driven_terrain_solid.comp` with
   `u_terrainMVP = reflectedMVP`, writing clip positions into a **second
   thin-record SSBO region** (separate buffer or ring slot - must not clobber
   the primary records the on-screen terrain/water still need this frame).
   Pure GPU; reuses the existing compute path verbatim with one uniform changed.

3. **Reflection FBO** (`gosRenderer`-owned, greenfield - no `reflection*`
   member exists today). Lazy-create / resize-guarded mirroring the
   `gosPostProcess::createFBOs`/`resize` idiom: quarter-res
   (`drawable/4`), color `GL_RGBA8` single attachment + **dedicated**
   `GL_DEPTH24_STENCIL8` depth renderbuffer (terrain self-occlusion in the
   mirrored view; depth not sampled). Bind it, set quarter-res viewport,
   `glClear` color+depth.

4. **Second terrain MDI** (`glMultiDrawArraysIndirect`, the thin terrain
   program) over the reflected thin records into `reflectionFbo_`. Save/restore
   exactly the GL state the existing terrain bridge saves
   (`gos_terrain_bridge_drawIndirect` save/restore set) PLUS the FBO binding
   and the dedicated depth - the net-new state (Risk R3).

5. **Restore** `sceneFBO_` (`getGosPostProcess()->getSceneFBO()`) + main
   viewport, then proceed into the existing water MDI draw unchanged - it now
   samples `reflectionColorTex_`.

### Water FS fold (`shaders/gos_terrain_water_mdi.frag`, `o_isWater==1` only)

Just before `FragColor = vec4(col, shore);`, add the SOLE camera-dependent
term:

```glsl
// S3: planar terrain reflection - the ONLY camera-dependent term (ruling).
vec4 rclip = reflectionMVP * vec4(WorldPos, 1.0);
vec2 ruv   = rclip.xy / rclip.w * 0.5 + 0.5;
vec3 refl  = texture(reflectionTex, clamp(ruv, 0.0, 1.0)).rgb;
vec3 vdir  = normalize(cameraPos.xyz - WorldPos);         // only cam-dep input
float fres = REFL_F0 + (1.0 - REFL_F0) * pow(1.0 - max(vdir.z, 0.0), 5.0);
col = mix(col, refl, fres * REFL_STRENGTH * waveLOD);     // S1 col is the base
```

`reflectionTex` on **texture unit 2** (units 0/1 = tex1/tex2 - confirmed
collision; unit 2 free; save/restore unit-2 sampler like the existing unit-1
save/restore). `reflectionMVP` uniform on `s_waterMdiProg` via the existing
`setMMat4*` lambda block. New consts `REFL_F0`/`REFL_STRENGTH` (hot-reloadable,
visual-tuned like S1). `GBuffer1` (MRT loc 1) untouched.

**Graceful fallback:** not-armed / FBO-not-ready -> bind a 1px black / cleared
`reflectionTex` (or skip the sample via a `reflectionOn` int uniform) ->
`mix(col, black, ~0)` ≈ S1 look. S3 never hard-fails the water draw.

## 4. Plan-stage verifications (MUST close before/within the plan - Rule 0)

- **V1 (A-vs-B fork):** read the thin-record struct (GLSL + C++) + the
  `gpu_driven_terrain_solid.comp` record writes. Spec assumes Option A
  (second dispatch). Confirm A is correct as written; only if world-space
  corners are persisted is the cheaper Option B even possible (kept deferred
  regardless for S3).
- **V2 (frame anchor):** confirm `gos_terrain_indirect_getDispatchMvp16()` /
  `g_dispatchMvp16` is populated unconditionally when armed (not behind
  `MC2_RING_TRACE`/a trace gate). Reflected MVP MUST use it.
- **V3:** confirm quarter-res depth **renderbuffer** (not texture) is
  acceptable for the reflection terrain pass; confirm the second thin-SSBO
  region does not alias the primary records still needed this frame.

## 5. Load-bearing constraints / risks (adversarial focus)

- **R1 MVP frame-anchor mismatch (the exact bug water already paid for).**
  Reflected MVP from `g_dispatchMvp16` (armed), never a separately-sampled
  `terrain_mvp_`, or the reflection lags terrain 1 frame under motion
  (`memory/water_fastpath_interim_fixes_and_residuals.md`). Inherit, don't
  re-derive.
- **R2 Option-A correctness vs unaudited Option-B.** Spec commits to A
  (second full compute dispatch) precisely because it reuses the proven
  compute->MDI path with one uniform changed; B is unverified and deferred.
- **R3 Feedback-loop + FBO/depth restore (AMD bring-up trap,
  `docs/amd-driver-rules.md`, `gpu_direct_renderer_bringup_checklist.md`).**
  `reflectionColorTex_` must be unbound from its FBO and `sceneFBO_` rebound
  BEFORE the water samples it; the reflection pass needs its own depth and
  must not write `sceneDepthTex_`. The existing bridge save/restore covers
  pipeline state but NOT the FBO binding or separate depth - net-new, the
  most likely "black screen / nothing reflects" source.
- **Camera-independence named contract:** the reflection sample + its Fresnel
  weight is the ONLY `cameraPos`-dependent code added; S1's wave/glint/color
  stay `f(WorldPos,time)`. `viewVec`/`waveLOD` (distance, not angle) is reused
  only as the existing LOD anti-alias factor on the reflection mix.
- **No regression:** z-bias untouched (reflection terrain is offscreen, does
  not interact with the on-screen water/terrain z-fight residual - do NOT try
  to fix that here); `[WATER_MAT v1]`/`[WATER_DEPTHPROBE v2]` probes untouched;
  GBuffer1/MRT untouched; no readback / no `glClientWaitSync`-after-write in
  the reflection path (sync-stall lesson). Per-frame CPU budget: 1 mat4 +
  1 bind + 1 dispatch + 1 MDI + clears + a few uniforms - no CPU geometry.

## 6. Files (anticipated; grep-confirm at plan/impl)

```
MODIFIED  GameOS/gameos/gameos_graphics.cpp        -- gosRenderer reflectionFbo_/
                                                      reflectionColorTex_/depthRbo_
                                                      members + lazy create/resize;
                                                      renderWaterReflectionPass()
                                                      (2nd compute dispatch w/ mirrored
                                                      MVP + 2nd terrain MDI into FBO);
                                                      call it at top of renderWaterFastPath
                                                      (armed gate); bind reflectionTex
                                                      unit 2 + reflectionMVP on s_waterMdiProg
MODIFIED  shaders/gos_terrain_water_mdi.frag       -- o_isWater==1: reflectionTex/
                                                      reflectionMVP sample, Fresnel-
                                                      weighted mix (sole camera-dep term);
                                                      REFL_F0/REFL_STRENGTH consts
(REUSED, unmodified)  gpu_driven_terrain_solid.comp, gos_terrain_thin.vert,
                      the indirect cmd/recipe/thin SSBOs - re-bound, not changed.
```

NOT pure-FS: this slice needs a **full relink** (gameos_graphics.cpp) +
deploy (exe + .frag) to the isolated `mc2-win64-water`. Build discipline
per the v1 plan's configure/relink steps.

## 7. Gates

Build (RelWithDebInfo, full relink) + deploy exe+shaders to
`A:/Games/mc2-opengl/mc2-win64-water` (never v0.4). Kill-aware `mc2_01`
smoke (`--keep-logs --exe`, marker-gated, exit-code-agnostic). Add an
env-gated `[WATER_REFL v1]` probe (reflection FBO created, armed, dispatch
fired) so a smoke can prove the path is live (not silent-fallback). Then USER
visual: terrain visibly reflected in water; reflection tracks under camera
motion with NO 1-frame lag/smear (R1); degrades to clean S1 look when
un-armed / zoomed (waveLOD); no black screen / no feedback artifacts (R3);
S1 surface + camera-independence of everything-but-this unchanged.

## 8. Plan-stage advisor routing

`mc2-terrain-indirect-expert` (V1/V2 - thin-record struct, compute dispatch
re-invocation, dispatch-MVP anchor) + `mc2-render-expert` (FBO/feedback-loop,
state save-restore, hook ordering). Cross: `mc2-shader-expert` (FS reflection
projection + Fresnel as the sole camera-dependent term).
