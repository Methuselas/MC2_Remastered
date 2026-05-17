# Water Material v1 - GPU-Driven Design

**Date:** 2026-05-17
**Branch:** `claude/gpu-driven-rendering` (worktree `.claude/worktrees/gpu-driven-rendering/`)
**Status:** READY FOR ADVERSARIAL REVIEW
**Supersedes (for this branch):** `docs/superpowers/specs/2026-05-17-water-material-v1-design.md`
(written for the nifty-mendeleev single-program water path; invalid here - see "Why this supersedes")
**North star:** eliminate per-frame CPU work; bake to UBO/SSBO; 100% GPU-resident draw

---

## 1. Goal and non-goals

**Goal.** Replace flat textured water on the armed MDI path with a stylized
material: Beer-Lambert depth absorption, Fresnel rim, procedural sine-wave
normals driving specular and normal variation. Depth-faded shoreline alpha
replacing the per-vertex elevation-band staircase. **Zero new per-frame CPU
work and zero new per-frame GPU ops beyond the water shader itself.**

**Non-goals (deferred to later GPU-driven slices, not this spec):**
- Screen-space refraction (the only feature that would force a per-frame
  scene-color copy / FBO feedback-loop mitigation - cut by design).
- Planar reflection (the spec predecessor's `renderWaterReflectionPass()` was a
  per-frame CPU re-render of terrain - cut entirely; v1 uses a sky/fog-color
  approximation for the reflective term).
- Flow-map / sampled normal maps (procedural normals only in v1).
- Per-biome / mod-configurable water style (UBO is single global in v1; the UBO
  boundary makes per-biome a clean later extension).

## 2. Why this supersedes the predecessor spec on this branch

A code-grounded audit of the predecessor spec against this worktree
(`@5bffaf3`) found two CRITICAL invalidations:

1. **Dual-program water path.** `renderWaterFastPath()` lazily compiles a second
   MDI program (`s_waterMdiProg` from `shaders/gos_terrain_water_fast_mdi.vert`
   + `shaders/gos_terrain_water_mdi.frag`, built in `gameos_graphics.cpp`
   around lines 2081-2085). When `gpuArmed && mdiValid` (around lines
   2251-2254) the MDI program is bound and issues the draw via
   `glMultiDrawArraysIndirect` (around line 2345). `water_fast_prog_`
   (`gos_terrain_water_fast.vert` + `gos_tex_vertex.frag`, built around line
   3566) is only the un-armed / unsupported-GPU fallback. The predecessor spec
   swapped the FS on `water_fast_prog_`, which would not touch the live armed
   water at all. **v1 targets the MDI shaders only.**

2. **No `near_plane_` / `far_plane_`; non-perspective double-projection.**
   No such members exist on `gosRenderer`. `Camera::setOrthogonal()`
   (`mclib/camera.cpp` around line 1995) builds a custom `cameraToClip`, and
   the water VS uses a double-projection chain (`terrainMVP -> screen -> mvp`,
   `gos_terrain_water_fast.vert` around lines 323-368), so the predecessor's
   `(n*f)/(f - raw*(f-n))` linearization has no valid `(near,far)` and cannot
   be ported. **v1 uses a depth-delta mechanic that needs neither (Section 4).**

(Audit also confirmed, in this worktree's favor: scene depth/color/normal
textures already exist with class accessors - `gos_postprocess.h` around lines
30-32, `getSceneDepthTexture()` etc.; `glClipControl(GL_LOWER_LEFT,
GL_ZERO_TO_ONE)` is active - `gameosmain.cpp` around line 930; the
post-renderLists hook is at `gamecam.cpp` around line 256.)

All cited line numbers are starting points - grep the symbol to find the
current line at implementation time (Rule 0).

## 3. What does NOT change (load-bearing - do not regress)

- **MDI water draw structure:** `WaterStream::ComputeDispatchAndBindThinRecords`
  arming, `s_waterMdiProg` bind, `glMultiDrawArraysIndirect`, the per-cmd SSBO
  (`WaterPerCmd`, including its `detailMode` field used for base/detail/spray -
  this is an SSBO field, NOT a uniform).
- **Z-bias invariant:** terrain `+0.002`, water `WATER_DEPTH_FUDGE_FAST =
  +0.003` (`shaders/include/terrain_depth_bias.hglsl`). The load-bearing rule
  is delta = water - terrain = `0.001`. Do NOT "fix" to `0.0025` (that is
  `WATER_DEPTH_FUDGE_RASTER`, the mask/CPU-raster regime).
- **`[WATER_DEPTHPROBE v2]`** retention instrument in
  `gos_terrain_water_stream.cpp` (env `MC2_WATER_DEPTHPROBE`) - untouched.
- **MVP-consistency:** armed water continues to project with the terrain-solid
  dispatch MVP via the existing `WaterStream` path - untouched.
- **Un-armed fallback:** `water_fast_prog_` + `gos_terrain_water_fast.vert` +
  `gos_tex_vertex.frag` - untouched. Un-armed / unsupported-GPU users keep
  current flat textured water (acceptable; armed is the default and the
  GPU-driven north-star path).
- **Existing `time` uniform** already pushed for the current water path -
  reused, not re-pushed.

## 4. Depth mechanic (resolves CRITICAL-2 with no near/far, no inversion)

The water surface's own post-projection depth is `gl_FragCoord.z` - already
interpolated, free, no custom varying. The opaque floor depth is
`sceneDepthTex` sampled at `gl_FragCoord.xy / vec2(textureSize(sceneDepthTex,
0))` - screen size from `textureSize`, so **no `inverseScreenSize` uniform
exists**.

Both values live in the same `ZERO_TO_ONE`, non-reversed, non-linear depth
space produced by the same projection (water and terrain differ only by the
known `0.001` z-bias delta). Therefore:

```
float waterDepth  = gl_FragCoord.z;
float sceneDepth  = texture(sceneDepthTex, gl_FragCoord.xy /
                            vec2(textureSize(sceneDepthTex, 0))).r;
float depthDelta  = max(0.0, sceneDepth - waterDepth);   // monotonic in true depth
```

`depthDelta` is monotonic in true distance but not linear. v1 deliberately
folds the non-linearity into artist-tuned UBO constants (consistent with the
pre-existing per-vertex alpha bands, which were already non-physical):

```
float thick   = clamp(depthDelta * absorptionDensityClip, 0.0, 1.0);
vec3  waterCol = mix(shallowColor, deepColor, thick);          // Beer-Lambert-ish
float shore    = smoothstep(0.0, shoreBlendDepthClip, depthDelta);
```

This needs no near/far, no inversion of the double-projection, no CPU. At the
shoreline `depthDelta -> 0` so `shore -> 0` fades the edge out, eliminating the
tile-aligned staircase and making residual waterline z-fighting invisible
(pixels fade rather than flicker) without touching the z-bias invariant.

**Plan-stage open item (close with `mc2-shader-expert`):** exact values of
`absorptionDensityClip` and `shoreBlendDepthClip` in this compressed clip-depth
space (the useful `depthDelta` range is small and non-linear; constants will be
tuned, not guessed), and confirmation that the water base pass runs with
**depth-writes disabled** (required for the `sceneDepthTex` sample to be the
benign half of the AMD feedback rule - see Section 7).

## 5. Shading

**Procedural normals** (zero asset dependency; later upgrade path = sampled
flow-map normals):

```
vec2 waveN(vec2 uv, float f, float s) {
    return vec2(sin(uv.y*f + time*s), sin(uv.x*f + time*s*0.71));
}
vec3 waveNormal(vec2 uv) {
    vec2 w = waveN(uv,3.0,0.5) + waveN(uv*0.4,7.0,0.3);
    return normalize(vec3(w * normalStrength, 1.0));
}
```

**Fresnel** (needs world position -> the one new VS varying):

```
vec3  viewDir = normalize(cameraPos - WorldPos);
float ct      = max(dot(wN, viewDir), 0.0);
float fres    = fresnelF0 + (1.0 - fresnelF0) * pow(1.0 - ct, 5.0);
```

`cameraPos` and the light direction are read from the data **already
GPU-resident for the terrain MDI program** (same source the terrain MDI
shaders use - no new per-frame `setVec4`). At MC2's oblique ~30 deg camera
`ct ~ 0.5-0.7` (Fresnel ~0.05-0.25); cinematic low angles `ct ~ 0.1-0.2`
(Fresnel ~0.5-0.7) - the large visual win at cinematic angles.

**Reflective term (no reflection pass in v1):** `reflCol = fog_color.rgb *
1.4` (sky/fog approximation; `fog_color` already available on the water path).

**Specular:** `pow(max(dot(wN, halfV),0.0), 64.0) * fres`, `halfV =
normalize(viewDir + lightDir)`.

**Composite:**

```
vec3 col = mix(waterCol, reflCol, fres);
col     *= existingDiffuseSample.rgb * vertexLightTint;   // preserve current tint
col     += sunIntensity * spec;
col      = applyFog(col);                                  // existing fog path
float a  = shore * existingVertexAlpha;                    // shore now primary
```

The detail/spray pass (`WaterPerCmd.detailMode == 1`, SSBO field) keeps its
current lightweight path via an early-out at the top of the FS - it does NOT
run the depth/absorption math.

## 6. Files changed

```
MODIFIED  shaders/gos_terrain_water_fast_mdi.vert  -- add: out vec3 WorldPos (set from existing world pos)
MODIFIED  shaders/gos_terrain_water_mdi.frag       -- material logic; detailMode early-out preserved
MODIFIED  GameOS/gameos/gameos_graphics.cpp        -- WaterStyle UBO (write-once) + bind to MDI prog;
                                                      bind getSceneDepthTexture() on a unit for MDI prog;
                                                      delete predecessor per-frame material/cameraPos/
                                                      inverseScreenSize/reflection uniform block
```

No new files. No accessor additions (existing
`getGosPostProcess()->getSceneDepthTexture()` is used). Legacy
`gos_terrain_water_fast.vert` / `gos_tex_vertex.frag` / `water_fast_prog_`
untouched.

## 7. WaterStyle UBO

std140, single global, populated **once** at init (or on explicit style
change - not per frame), bound to `s_waterMdiProg` at a fixed binding point
alongside the program's existing UBO/SSBO bindings:

```
layout(std140) uniform WaterStyle {
    vec3  shallowColor;          // ~ (0.22, 0.45, 0.38)
    float absorptionDensityClip; // tuned in clip-depth space (plan-stage)
    vec3  deepColor;             // ~ (0.02, 0.08, 0.10)
    float shoreBlendDepthClip;   // tuned in clip-depth space (plan-stage)
    float normalStrength;        // ~ 0.30
    float fresnelF0;             // ~ 0.02
};
```

(std140 layout to be finalized with `mc2-shader-expert` for correct vec3/float
packing; values above are starting points, tuned in the plan.)

## 8. Per-frame budget (the point of this spec)

| Item | Predecessor (per frame) | v1 |
|---|---|---|
| Material constants | ~7 `setF`/`setVec3` | UBO, write-once |
| `cameraPos` / light dir | 2 `setVec4` | reuse GPU-resident terrain MDI data |
| `inverseScreenSize` | 1 `setVec2` | none (`textureSize` in FS) |
| Reflection MVP + toggle | 1 `setMat4` + 1 `setI` | cut (no reflection in v1) |
| Reflection terrain re-render | full CPU pass | cut |
| Scene-color feedback blit | 1 `glBlitFramebuffer` | none (no refraction) |
| **Net new per-frame CPU/GPU** | several | **zero** |

## 9. AMD / Vulkan-prep constraints

- **Feedback rule:** the water draw must NOT sample `sceneColorTex` (it does
  not - no refraction). It DOES sample `sceneDepthTex`, which is an attachment
  of the bound scene FBO; this is defined/benign **only with depth-writes off
  on the water base pass**. The plan MUST verify and, if necessary, explicitly
  set the water base pass depth-mask to GL_FALSE (water is alpha-blended; this
  is almost certainly already its state, but it is load-bearing - confirm, do
  not assume).
- **No `glTextureBarrier`** (GL 4.5; context is 4.3) - not needed given the
  above.
- **Vulkan-prep:** UBO is explicit device-mediated binding; no implicit
  cross-call GL state assumed. Reuse of GPU-resident camera/light data must go
  through the same explicit binding the terrain MDI program uses, not a hidden
  global.
- **Shader `#version`:** never in the shader file; `"#version 430\n"` prefix
  via the existing `makeProgram` path.
- **Hot-reload fails silent:** check console after editing the MDI shaders (bad
  compile = old shader stays active).

## 10. Smoke and visual gates

```
py -3 .claude/worktrees/gpu-driven-rendering/scripts/run_smoke.py \
   --tier tier1 --duration 30 --kill-existing
```

(Run against the rebuilt-and-deployed exe - a smoke PASS on a stale deployed
exe does not verify a shader change. Full relink before deploy since
`gameos_graphics.cpp` changes.)

Manual visual checks (tier1 necessary, not sufficient):
- Deep water visibly darker (absorption) vs shallow near-shore.
- Shoreline: smooth depth fade, no tile-aligned staircase, no waterline
  flicker under camera motion.
- Cinematic low angle: Fresnel rim visibly brighter at grazing incidence.
- Detail/spray pass still renders (detailMode SSBO early-out preserved).
- Un-armed intro pan: legacy flat water still draws (no fallback regression).
- `MC2_WATER_DEPTHPROBE=1`: equal=1 on all motion frames (MVP-consistency
  intact).
- Zoomed-out big-map: no perf regression (depth sample is one texture fetch;
  no new draw / cull / LOD change - structurally cannot regress draw volume,
  but verify per the zoomed-out stress-path rule).

## 11. Plan-stage advisor routing (per project advisor discipline)

- `mc2-shader-expert`: std140 packing; `absorptionDensityClip` /
  `shoreBlendDepthClip` tuning in clip-depth space; depth-write state of the
  water base pass; reuse mechanism for GPU-resident camera/light in the MDI
  program.
- `mc2-terrain-indirect-expert` (cross): confirm the MDI water program's
  binding-point layout so the WaterStyle UBO binding does not collide with
  existing terrain-indirect UBO/SSBO bindings.
