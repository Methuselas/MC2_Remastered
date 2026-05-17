# Water Material v1 Design

**Date:** 2026-05-17
**Scope:** Phase A (dedicated water shader pair) + Phase B (depth absorption / refraction)
         + first-pass planar reflection (terrain-only, quarter-res)
**Status:** READY FOR IMPLEMENTATION (Opus handoff)
**Predecessor:** renderWater architectural slice (Stages 1-3 shipped; parity confirmed)

---

## Context and current state

The water fast path ships and works:
- VS: `shaders/gos_terrain_water_fast.vert` (SSBO-fed, double-projection chain, z +0.0025)
- FS: `shaders/gos_tex_vertex.frag` (simple `color * texture + fog`)
- C++ entry: `gosRenderer::renderWaterFastPath()` in `GameOS/gameos/gameos_graphics.cpp`
- Armed gate: `MC2_GPU_DRIVEN_WATER` default-ON + `IsFrameSolidArmed()`; un-armed pans use legacy water
- MVP fix shipped 2026-05-17 (`e925dfb`): water uses `gos_terrain_indirect_getDispatchMvp16()`
  when armed so projection is bit-consistent with terrain-solid (ring_slot_state_must_travel_with_slot)
- `[WATER_DEPTHPROBE v2]` (`MC2_WATER_DEPTHPROBE` env) is a retained regression instrument in
  `gos_terrain_water_stream.cpp` -- must not be disturbed

Mask-dispatch Stage 1c (`gos_terrain_mask_water.vert`) is in dual-run soak (writes disabled).
Stage 1d will retire `gos_terrain_water_fast.vert` and promote it as the active VS.

The G-buffer is fully live at water draw time (water draws post-`renderLists()`):
- `sceneColorTex_` (RGBA16F, color attachment 0)
- `sceneNormalTex_` (RGBA16F, color attachment 1)
- `sceneDepthTex_` (GL_DEPTH24_STENCIL8, sampleable)

Accessors confirmed at `gos_postprocess.cpp` lines 281/291/307 respectively.

---

## What does NOT change

- SSBO schema (`WaterRecipe`, `WaterThinRecord`) -- untouched
- Z-ordering invariant: terrain +0.002, water +0.0025 -- untouched
- Two-draw structure (base layer + detail/spray) via `detailMode` uniform -- preserved
- All existing uniforms and GL state setup in `renderWaterFastPath()` -- preserved
- `[WATER_DEPTHPROBE v2]` instrument -- untouched
- MVP-consistency fix (`gos_terrain_indirect_getDispatchMvp16()`) -- untouched
- Un-armed legacy water path -- untouched
- `mclib/terrain.cpp` call sites -- untouched

---

## Files changed or added

```
MODIFIED:  shaders/gos_terrain_water_fast.vert      -- add: out vec3 WorldPos
MODIFIED:  shaders/gos_terrain_mask_water.vert       -- add: out vec3 WorldPos
NEW:       shaders/gos_terrain_water.frag            -- dedicated water material shader
MODIFIED:  GameOS/gameos/gameos_graphics.cpp         -- swap FS, bind new samplers,
                                                        add renderWaterReflectionPass()
NEW:       memory/water_v2_upgrade_checklist.md      -- forward-compat deferred items
```

The reflection FBO and its depth renderbuffer live on `gosRenderer` (not `gos_postprocess`)
since they are water-specific, not screen-space effects.

---

## Section 1: VS changes (gos_terrain_water_fast.vert + gos_terrain_mask_water.vert)

Both VSes already compute `worldPos` internally -- the fast VS at line 208, the mask VS at
line 114. Add three lines to each:

```glsl
// alongside existing: out vec4 Color; out vec2 Texcoord; out float FogValue;
out vec3 WorldPos;

// in main(), just before the gl_Position assignment:
WorldPos = worldPos;
```

All existing debug-mode branches leave WorldPos set -- it carries position, not a
correctness-critical value for debug views.

```glsl
// TODO(water-v2): when mask-dispatch Stage 1d ships and gos_terrain_water_fast.vert
// retires, gos_terrain_mask_water.vert becomes the sole active VS. WorldPos is
// already emitted there -- no FS change needed at that transition.
```

---

## Section 2: New fragment shader (shaders/gos_terrain_water.frag)

Replaces `gos_tex_vertex.frag` as the water program's FS.
`water_fast_prog_` in `gameos_graphics.cpp` pairs with this file instead.

### Varyings in (must match VS out exactly)

```glsl
in vec4  Color;       // lightRGB + alpha band (already BGRA pre-swizzled by VS)
in vec2  Texcoord;    // world-derived UV with wrap correction
in float FogValue;
in vec3  WorldPos;    // new
```

### Samplers

```glsl
uniform sampler2D tex1;          // unit 0 -- animated water diffuse (existing, unchanged)
uniform sampler2D sceneColorTex; // unit 1 -- RGBA16F opaque scene
uniform sampler2D sceneDepthTex; // unit 2 -- GL_DEPTH24_STENCIL8, sample .r channel
uniform sampler2D reflectionTex; // unit 3 -- quarter-res planar reflection FBO
```

### Uniforms (new additions to existing set)

```glsl
// Projection for depth reconstruction (glClipControl ZERO_TO_ONE in effect)
uniform vec2  inverseScreenSize;  // 1.0 / vec2(width, height)
uniform float nearPlane;
uniform float farPlane;

// Water material
uniform vec3  shallowColor;        // default (0.22, 0.45, 0.38)
uniform vec3  deepColor;           // default (0.02, 0.08, 0.10)
uniform float absorptionDensity;   // default 0.15
uniform float refractionStrength;  // default 0.015
uniform float normalStrength;      // default 0.30
uniform float fresnelF0;           // default 0.02 (water at normal incidence)
uniform float shoreBlendDepth;     // default 3.0 (world-units depth for full opacity)

// Reflection
uniform mat4 reflectionMVP;        // reflection camera's full MVP
uniform int  enableReflection;     // 0=off, 1=on

// Existing uniforms already set by renderWaterFastPath() -- no change needed:
//   time, fog_color, terrainLightDir (via cameraPos + terrainLightDir additions below)
```

### Procedural wave normals

Two sine waves at different frequencies, blended. Zero asset dependency; upgrade
path is adding `sampler2D waterNormalA/B` samplers and swapping the computation.

```glsl
vec2 waveN(vec2 uv, float freq, float speed) {
    return vec2(sin(uv.y * freq + time * speed),
                sin(uv.x * freq + time * speed * 0.71));
}
vec3 waveNormal(vec2 uv) {
    vec2 w = waveN(uv, 3.0, 0.50) + waveN(uv * 0.4, 7.0, 0.30);
    return normalize(vec3(w * normalStrength, 1.0));
}
// TODO(water-v2): replace with dual sampler2D waterNormalA/B scrolled via uvOffset
// pairs (Portal 2 flow-map pattern). Procedural stays as fallback when no map bound.
```

### Depth reconstruction and water thickness

With `glClipControl(ZERO_TO_ONE)` (already active), depth buffer values are in [0,1]
with 0=near. Standard linearization:

```glsl
vec2 screenUV    = gl_FragCoord.xy * inverseScreenSize;
float rawScene   = texture(sceneDepthTex, screenUV).r;
float linScene   = (nearPlane * farPlane) / (farPlane - rawScene  * (farPlane - nearPlane));
float linWater   = (nearPlane * farPlane) / (farPlane - gl_FragCoord.z * (farPlane - nearPlane));
float waterThick = max(0.0, linScene - linWater);
```

### Shoreline alpha (replaces per-vertex alpha-band system)

```glsl
float shoreAlpha = smoothstep(0.0, shoreBlendDepth, waterThick);
// This drives opacity. The VS Color.a (per-vertex elevation bands: alphaEdge/Middle/Deep)
// becomes a secondary multiplier and is no longer the primary shoreline driver.
// This eliminates the tile-aligned staircase artifact at the shoreline.
//
// TODO(water-v2): remove alphaEdgeByte/alphaMiddleByte/alphaDeepByte uniforms and
// the VS alpha-band logic when flow-map shoreline ships.
```

This also makes most z-fighting at the exact waterline invisible: pixels where
waterThick is near-zero fade out rather than flicker. The existing z-bias invariant
(terrain +0.002, water +0.0025) stays unchanged.

### Absorption and refraction

```glsl
vec3 wN = waveNormal(Texcoord);

// Beer-Lambert absorption
float absorb      = exp(-waterThick * absorptionDensity);
vec3  waterBase   = mix(deepColor, shallowColor, absorb);

// Screen-space refraction: perturb scene color lookup by wave normal.
// Fade offset near shore to avoid sampling above-water terrain.
vec2  refrOff    = wN.xy * refractionStrength * clamp(waterThick, 0.0, 1.0);
vec3  refractCol = texture(sceneColorTex, screenUV + refrOff).rgb;

// Blend: deep water shows base color; shallow shows terrain through refraction.
vec3  underCol   = mix(waterBase, refractCol, absorb);
```

### Fresnel

```glsl
// cameraPos is already a vec4 uniform set by renderWaterFastPath() (see Section 3).
vec3  viewDir  = normalize(cameraPos.xyz - WorldPos);
float cosTheta = max(dot(wN, viewDir), 0.0);
float fresnel  = fresnelF0 + (1.0 - fresnelF0) * pow(1.0 - cosTheta, 5.0);
```

At MC2's oblique 30 deg camera cosTheta is typically 0.5-0.7 (Fresnel ~0.05-0.25).
Low cinematic angles hit cosTheta ~0.1-0.2 (Fresnel ~0.5-0.7), which is the
"huge visual win at cinematic angles" the user noted.

### Reflection

```glsl
vec3 reflectCol = vec3(0.0);
if (enableReflection > 0) {
    // Project WorldPos through reflection camera to texcoord in reflectionTex.
    vec4 reflClip = reflectionMVP * vec4(WorldPos, 1.0);
    vec2 reflUV   = (reflClip.xy / reflClip.w) * 0.5 + 0.5;
    reflUV       += wN.xy * 0.02;          // wave-normal perturbation for non-mirror look
    reflUV        = clamp(reflUV, 0.0, 1.0);
    reflectCol    = texture(reflectionTex, reflUV).rgb;
} else {
    reflectCol = fog_color.rgb * 1.4;      // sky approximation fallback
}
// TODO(water-v2): SSR-lite fallback for mechs/buildings not in planar pass
// (ray march sceneDepthTex from reflected view direction, blend on confidence).
```

### Specular

```glsl
// terrainLightDir is already a vec4 uniform set by renderWaterFastPath() (see Section 3).
vec3  halfV = normalize(viewDir + terrainLightDir.xyz);
float spec  = pow(max(dot(wN, halfV), 0.0), 64.0) * fresnel;
```

### Final composite and two-pass branch

```glsl
void main() {
    // Detail / spray pass: lightweight additive layer, skip all depth math.
    if (detailMode == 1) {
        FragColor = texture(tex1, Texcoord) * Color.bgra;
        return;
    }

    // --- Base pass ---
    // [full computation: wN, waterThick, shoreAlpha, underCol, fresnel,
    //  reflectCol, spec as above]

    vec3 waterCol  = mix(underCol, reflectCol, fresnel);
    waterCol      *= texture(tex1, Texcoord).rgb * Color.bgr; // light tint from VS
    waterCol      += terrainLightDir.w * spec;                // sun color approximation

    if (fog_color.x > 0.0 || fog_color.y > 0.0 ||
        fog_color.z > 0.0 || fog_color.w > 0.0)
        waterCol = mix(fog_color.rgb, waterCol, FogValue);

    float alpha = shoreAlpha * Color.bgra.a;
    FragColor = vec4(waterCol, alpha);

    // Debug modes (controlled by existing MC2_RENDER_WATER_FASTPATH_DEBUG env var,
    // mapped to `debugMode` uniform already set by renderWaterFastPath()):
    //   1 = waterThick / 10.0 (grayscale)
    //   2 = wave normal (RGB)
    //   3 = Fresnel value (grayscale)
    //   4 = reflection contribution only
    //   5 = refraction contribution only
    //   6 = shore alpha mask
}
```

Note: `terrainLightDir.w` is used as a sun-color proxy (sun intensity). If a
dedicated `sunColor` uniform is cleaner, add it -- but reusing `terrainLightDir.w`
avoids adding a new uniform at the cost of some physical accuracy.

---

## Section 3: C++ changes (GameOS/gameos/gameos_graphics.cpp)

### Program swap

```cpp
// In gosRenderer init where water_fast_prog_ is built:
water_fast_prog_ = makeProgram(
    "gos_terrain_water_fast.vert",
    "gos_terrain_water.frag"   // was: gos_tex_vertex.frag
);
```

### New uniforms in renderWaterFastPath()

Add alongside the existing `setMat4Direct` / `setVec4` / `setF` / `setI` calls.
`terrain_camera_pos_` and `terrain_light_dir_` are already cached `gosRenderer`
members used by the terrain program -- zero new infrastructure:

```cpp
// Already available in gosRenderer; already set for terrain program.
setVec4("cameraPos",       (const float*)&terrain_camera_pos_);
setVec4("terrainLightDir", (const float*)&terrain_light_dir_);

// New scene inputs -- bind before draw.
const int screenW = (int)screen_width_;
const int screenH = (int)screen_height_;
setVec2("inverseScreenSize", 1.0f / screenW, 1.0f / screenH);
setF("nearPlane", near_plane_);    // gosRenderer member, confirm name by grep
setF("farPlane",  far_plane_);     // gosRenderer member, confirm name by grep

// Reflection MVP (computed by renderWaterReflectionPass(), stored as member)
setMat4Std("reflectionMVP", (const float*)&reflection_mvp_);
setI("enableReflection", reflectionFbo_ ? 1 : 0);

// New scene texture bindings.
glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, getSceneColorTex());   // from gos_postprocess
glActiveTexture(GL_TEXTURE2);
glBindTexture(GL_TEXTURE_2D, getSceneDepthTex());   // from gos_postprocess
glActiveTexture(GL_TEXTURE3);
glBindTexture(GL_TEXTURE_2D, reflectionColorTex_);
setI("sceneColorTex",  1);
setI("sceneDepthTex",  2);
setI("reflectionTex",  3);
// Restore active texture to 0 before the existing baseTex bind below
glActiveTexture(GL_TEXTURE0);

// Water material defaults (hardcoded v1; UBO-ize in v2)
// Note: existing renderWaterFastPath() only uses setVec4/setVec2/setF/setI lambdas.
// Add a setVec3 lambda mirroring setVec4, or pass as setVec4 with w=0.
setVec3("shallowColor",       0.22f, 0.45f, 0.38f);
setVec3("deepColor",          0.02f, 0.08f, 0.10f);
setF("absorptionDensity",     0.15f);
setF("refractionStrength",    0.015f);
setF("normalStrength",        0.30f);
setF("fresnelF0",             0.02f);
setF("shoreBlendDepth",       3.0f);
// TODO(water-v2): replace individual material uniform setters with WaterStyle UBO
// for per-biome / mod configuration (TOML/INI, mission record).
```

Note: `time` (line 2113) and `fog_color` (line 2114) are already set. `detailMode`
(line ~2218) is already set per-draw. No change needed for those.

### New reflection FBO members on gosRenderer

```cpp
// In gosRenderer class declaration (alongside water_fast_prog_):
GLuint reflectionFbo_       = 0;
GLuint reflectionColorTex_  = 0;
GLuint reflectionDepthRbo_  = 0;   // renderbuffer, not texture -- not sampled later
glm::mat4 reflection_mvp_   = {};  // updated each frame by renderWaterReflectionPass()
```

FBO created / resized at resolution-change time. Size: `screen_width_ / 4` x
`screen_height_ / 4`. Color attachment: GL_RGBA8 (sufficient at quarter-res).
Depth: GL_DEPTH_COMPONENT24 renderbuffer (needed for terrain depth test during
reflection render, but not sampled after).

### renderWaterReflectionPass() (new method)

Called from the post-renderLists() hook immediately before `renderWaterFastPath()`.

```
1. Bind reflectionFbo_. Set viewport to quarter-res.
2. Clear color + depth.
3. Compute reflected camera:
     MC2 coordinate convention (architecture.md):
       terrain.x = -camera.x, terrain.y = camera.z, terrain.z = camera.y
     Water plane in terrain space: z = waterElevation (terrain Z = world elevation).
     Reflected camera:
       reflPos.xy  = camPos.xy  (lateral unchanged)
       reflPos.z   = 2 * waterElevation - camPos.z   (flip elevation)
       reflDir.xy  = lookDir.xy
       reflDir.z   = -lookDir.z                      (invert elevation component)
4. Build reflectionMVP from reflected camera pos + direction.
   Store as reflection_mvp_ for the water FS uniform.
5. Draw terrain from reflected camera using existing terrain program + thin VS.
   Use the SAME terrainMVP (or dispatch MVP when armed) but with the reflected
   camera's view transform folded into the matrix.
6. (Optional trees) Skip for v1 -- see TODO below.
7. Restore main FBO and viewport.
```

```cpp
// TODO(water-v2): add GL_CLIP_DISTANCE0 clip plane at z=waterElevation so
// underwater terrain doesn't bleed into reflection at extreme low angles.

// TODO(water-v2): draw static props (trees) in reflection pass.
// The global instance pool SSBO (coalesce merge 2026-05-12) means a simplified
// prop draw needs to account for per-type range layout in s_globalPool.
// Terrain-only reflection is still a large visual win without this.

// TODO(water-v2): low-rate reflection update for distant / calm water bodies.
// Hero-plane selection: only render reflection for the largest visible water plane.
// Every-frame update is correct for v1.
```

### getSceneColorTex() / getSceneDepthTex() accessors

These expose `sceneColorTex_` and `sceneDepthTex_` from `gos_postprocess.cpp`.
Simple getter pair -- either add to the `GosPostProcess` class or expose as
standalone functions mirroring the existing pattern for `gos_getBloomTex()`.

---

## Section 4: Forward-compatibility table

All deferred items are documented in `memory/water_v2_upgrade_checklist.md` and
marked with `// TODO(water-v2):` at the call site.

| Code location | Deferred item | Notes |
|---|---|---|
| `gos_terrain_water.frag` procedural normals | Dual `sampler2D waterNormalA/B`, flow-map advection | Portal 2 flow-map pattern |
| `gos_terrain_water.frag` reflection UV | SSR-lite ray march for mechs/buildings not in planar pass | McGuire/Mara 2014 baseline |
| `renderWaterFastPath()` material uniforms | WaterStyle UBO, per-biome loose-file config | TOML/INI, mission record |
| `renderWaterReflectionPass()` | GL_CLIP_DISTANCE0 clip plane at waterElevation | Prevents underwater bleed at low camera angles |
| `renderWaterReflectionPass()` | Tree draw via global pool SSBO range layout | s_globalPool coalesce |
| `renderWaterReflectionPass()` | Low-rate update + hero-plane selection | Currently every-frame |
| VS `alphaEdge/Middle/Deep` uniforms + logic | Remove when flow-map shoreline ships | smoothstep replaces them now |
| `gos_terrain_mask_water.vert` promotion | Already has WorldPos -- no FS change at Stage 1d | Mask-dispatch Stage 1d |
| `renderWaterFastPath()` two-draw structure | When Stage 1d arms mask-water writes, thin-record SSBO (binding 6) retires | base/detail UV move to per-pass uniforms on mask VS |

---

## Section 5: Load-bearing constraints (do not regress)

- **Z-ordering invariant:** terrain +0.002, water +0.0025. Water must lose LEQUAL
  ties to terrain at shoreline. Any depth-bias change needs a fresh smoke visual check.
- **`[WATER_DEPTHPROBE v2]`:** retention instrument in `gos_terrain_water_stream.cpp`.
  Must not be removed or silenced. Env: `MC2_WATER_DEPTHPROBE`.
- **MVP-consistency:** `renderWaterFastPath()` must continue to pass the dispatch MVP
  (from `gos_terrain_indirect_getDispatchMvp16()` when armed) as `terrainMVP`.
  The existing line 2079 already does this via the armed-gate logic in
  `gos_terrain_water_stream.cpp::UploadAndBindThinRecords()`. Do not change the MVP
  uniform source.
- **Un-armed guard:** `99a4c23` added an un-armed legacy fallback guard in
  `mclib/terrain.cpp`. The reflection pass must also gate on `IsFrameSolidArmed()` --
  no point rendering a reflection if the fast path is not drawing water.
- **Two-draw structure:** base (detailMode=0) then detail/spray (detailMode=1).
  The detail pass uses `tex1` unit 0 (spray texture). Do not accidentally sample
  sceneColorTex/sceneDepthTex during the detail pass -- the `detailMode == 1` early
  return in the FS handles this.
- **AMD driver rule (CRITICAL -- will work on NVIDIA, break on AMD):**
  Sampler units 1/2/3 are new for the water program. The feedback-loop rule:
  sceneColorTex (unit 1) must not be the current draw FBO's color attachment
  during the water draw. Water draws to `sceneFBO_` -- but sceneColorTex IS
  sceneFBO_'s color attachment 0. This IS a feedback loop. See docs/amd-driver-rules.md.
  Options:
    (a) Blit sceneColorTex to a separate `waterSceneColorCopy_` before the water draw
        and bind the copy on unit 1. Cost: one blit per frame.
    (b) Draw water to a separate `waterFbo_` and composite onto scene in post-process.
    (c) Use `glTextureBarrier()` (GL 4.5) to make the feedback loop defined.
  Recommended: option (a) -- simplest, most portable, no driver risk.
  Add `waterSceneColorCopy_` as a new RGBA16F texture + blit in the post-renderLists hook
  before the reflection pass.

---

## Section 6: Smoke and visual gates

Run after implementation:

```bash
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```

Manual visual checks (tier1 pass is necessary but not sufficient):
- Shoreline: no tile-aligned staircase; smooth depth fade at shore contact
- Deep water: dark absorption tint visible relative to shallow near-shore
- Cinematic low angle: Fresnel reflection visibly brighter at grazing incidence
- Reflection: terrain visible in water surface, perturbed by wave normal
- Z-fighting: no flickering at the exact waterline under camera motion
- Detail/spray pass: still renders (detailMode=1 early-return preserves it)
- Un-armed intro pan: legacy water still draws (no regression from 99a4c23)
- `MC2_WATER_DEPTHPROBE=1`: equal=1 on all motion frames (MVP-consistency intact)
- `MC2_RENDER_WATER_FASTPATH_DEBUG=1-6`: debug views render without GL errors
