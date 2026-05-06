# Terrain Material System — Slice 0 Facts-Only Recon

Date: 2026-05-02
Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`

This document is a code-grounded fact base. Every claim is cited at file:line; load-bearing behavior is quoted verbatim. No design proposals, no recommendations, no implications. Read-only inventory.

---

## 1. Shader / program inventory

Three terrain-related shader programs are created during `gosRenderer::init()` in `GameOS/gameos/gameos_graphics.cpp`. A fourth (water-fast) is also created in the same init block; it is listed for completeness but is the water/overlay program, not the terrain-material program.

| # | Logical name | VS | TCS | TES | FS | C++ creation site | Variant prefix / `#version` | Notable `#define`s in scope |
|---|---|---|---|---|---|---|---|---|
| 1 | `gos_terrain` (forward terrain w/ tess) | `shaders/gos_terrain.vert` | `shaders/gos_terrain.tesc` | `shaders/gos_terrain.tese` | `shaders/gos_terrain.frag` | `gosRenderMaterial::load(...)` invoked at `gameos_graphics.cpp:2576` ("`terrain_material_ = gosRenderMaterial::load("gos_terrain", mvar);`"); the load fn at `gameos_graphics.cpp:261-298` dispatches to `glsl_program::makeProgram2(...)` at `gameos_graphics.cpp:279-280` because `strcmp(shader, "gos_terrain") == 0` matches at `gameos_graphics.cpp:274`. | Prefix string is built in `gosMaterialVariationHelper::getMaterialVariation(...)` at `gameos_graphics.cpp:200-238`: starts `"#version 430\n"` (line 202). | `#define ALPHA_TEST = 1` (added when `gos_State_AlphaTest` is set, see `gameos_graphics.cpp:3034,3052`); `#define MRT_ENABLED 1` (added at `gameos_graphics.cpp:217-219` when `gosPostProcess::getSceneNormalTexture()` is non-null). Both are set per-variation, so a single material can produce up to four compiled program variants. |
| 2 | `shadow_terrain` (depth-only shadow caster w/ tess) | `shaders/shadow_terrain.vert` | `shaders/shadow_terrain.tesc` | `shaders/shadow_terrain.tese` | `shaders/shadow_terrain.frag` | `gosRenderMaterial::load(...)` invoked at `gameos_graphics.cpp:2592` (`"shadow_terrain_material_ = gosRenderMaterial::load("shadow_terrain", mvar);"`). Same TCS/TES dispatch path as above (gated by the same `strcmp` in `:274`). | Same `"#version 430\n"` prefix (built by `getMaterialVariation`). | Same `MRT_ENABLED` / `ALPHA_TEST` define machinery, but `shadow_terrain.frag` uses neither. |
| 3 | `gos_terrain_thin` (no-tess, SSBO-driven) | `shaders/gos_terrain_thin.vert` | — | — | `shaders/gos_terrain.frag` (shared with #1) | `glsl_program::makeProgram(...)` invoked directly at `gameos_graphics.cpp:2615-2619`. | Prefix string `"#version 430\n"` declared at `gameos_graphics.cpp:2614` (`static const char* kThinPrefix = "#version 430\n";`). Bypasses the `gosMaterialVariationHelper` machinery. | No `MRT_ENABLED` injection (does not go through `getMaterialVariation`); shares the same `gos_terrain.frag` source as program #1, which references `#ifdef MRT_ENABLED` and `#ifdef ALPHA_TEST`. With this prefix the defines are not set, so `MRT_ENABLED` and `ALPHA_TEST` blocks compile out. |
| 4 (non-terrain, listed) | `gos_terrain_water_fast` | `shaders/gos_terrain_water_fast.vert` | — | — | `shaders/gos_tex_vertex.frag` | `glsl_program::makeProgram(...)` at `gameos_graphics.cpp:2635-2639`. | `"#version 430\n"` prefix at `gameos_graphics.cpp:2634`. | None for terrain material. |

Notes:
- The shader source files do **not** carry `#version` lines themselves. `shaders/gos_terrain.frag:1` is `//#version 400 (version provided by material prefix)`; same comment at `gos_terrain.vert:1`, `gos_terrain.tesc:1`, `gos_terrain.tese:1`, `gos_terrain_thin.vert:1` (this one says `//#version 430`), `shadow_terrain.frag:1`, `shadow_terrain.tesc:1`, `shadow_terrain.tese:1`, `shadow_terrain.vert:1`.
- Effective `#version` for all four programs above is `430` (set by the C++ prefix, regardless of the comment-stated number).
- `gos_terrain.frag` is the fragment program for **two** distinct GL programs: program #1 (with TCS/TES + legacy VS) and program #3 (thin VS, no tess). Both must match its `in` interface.

---

## 2. Frag classifier flow (`gos_terrain.frag`)

This table walks the per-fragment material-classification pipeline, in execution order.

| # | Step | File:line range | Inputs | Outputs / writes | Notes |
|---|---|---|---|---|---|
| 1 | Atlas-mode UV reconstruction for tex1 | `gos_terrain.frag:263-269` | `useAtlasColormap`, `WorldPos.xy`, `atlasMapTopLeftX/Y`, `atlasOneOverWorldUnits`, `Texcoord` | `colormapUV` (vec2) | When `useAtlasColormap != 0`: UV = `(WorldPos.x − topLeftX) * oneOverWorldUnits, (topLeftY − WorldPos.y) * oneOverWorldUnits`. Else `colormapUV = Texcoord`. |
| 2 | Sample colormap (single tap) | `gos_terrain.frag:270` | `tex1`, `colormapUV` | `texColor` (vec4) | `texColor = texture(tex1, colormapUV);` |
| 3 | Cement-catalog override | `gos_terrain.frag:278-291` | `useCementAtlas`, `thinRecsFrag[RecordIdx].control.w`, `atlasCementGridSide`, `atlasCementWorldUnitsPerTile`, `WorldPos`, `tex3` | overwrites `texColor` if validity bit set | Validity bit is `(cementWord & 0x80000000u) != 0u` at `:280`. Layer index is `cementWord & 0xFFFFu` at `:282`. Sample is `texColor = texture(tex3, cAtlasUV);` at `:289`. |
| 4 | Water flag + materialAlpha | `gos_terrain.frag:292-293` | `texColor.rgb` (post-cement) | `waterFlag`, `materialAlpha` | `PREC float waterFlag = smoothstep(0.35, 0.45, rgb2hsv(texColor.rgb).x);` `PREC float materialAlpha = mix(1.0, 0.25, waterFlag);` |
| 5 | (optional) Alpha test discard | `gos_terrain.frag:295-298` | `texColor.a`, `ALPHA_TEST` define | early `discard` | Only compiled when `ALPHA_TEST` is defined. |
| 6 | Blurred colormap classification | `gos_terrain.frag:330-362` | `tex1`, `colormapUV`, `blurRadius=0.18`, `lodMid`, `lodNear`, `uvMargin=0.005` | `colAvg` (vec3) | Far: 1-tap; Mid: 5-tap cross; Near: 9-tap disc. Diagonal radius `r2 = blurRadius * 0.707` at `:333`. |
| 7 | `getColorWeights(colAvg)` body | function at `gos_terrain.frag:151-175` | `vec3 color` | `vec4 w` (rock, grass, dirt, concrete) | Body quoted below. |
| 8 | Snow weight | `gos_terrain.frag:373-378` | `colAvg` HSV | `snowWeight` | `snowRaw = smoothstep(0.15, 0.03, hsvAvg.y) * smoothstep(0.42, 0.62, hsvAvg.z);` `snowWeight = smoothstep(0.25, 0.55, snowRaw);` |
| 9 | `pureConcrete` from `TerrainType` | `gos_terrain.frag:384-387` | `TerrainType` (varying) | `pureConcrete`, `concreteColorBlend` | `PREC float pureConcrete = smoothstep(2.0, 3.0, TerrainType);` `PREC float concreteColorBlend = sqrt(clamp(pureConcrete, 0.0, 1.0));` |
| 10 | Cement-bit override on weights | `gos_terrain.frag:389` | `matWeights`, `pureConcrete` | `matWeights` | `matWeights = mix(matWeights, vec4(0.0, 0.0, 0.0, 1.0), pureConcrete);` |
| 11 | Snow suppressed on cement; snow steals from others | `gos_terrain.frag:391-393` | `snowWeight`, `pureConcrete`, `matWeights` | `snowWeight`, `matWeights` | `snowWeight *= (1.0 - pureConcrete);` followed by `matWeights *= (1.0 - snowWeight);` |
| 12 | Final renormalization | `gos_terrain.frag:395-400` | `matWeights` (post-snow steal) | `matWeights` | If `total > 0.01` divide by total else fallback `vec4(1.0, 0.0, 0.0, 0.0)`. Snow is **not** included in this normalization sum. |
| 13 | Far-tier 2-strongest cull | `gos_terrain.frag:403-409` | `matWeights`, `lodMid` | `matWeights` (renormalized) | Only when `lodMid < 0.01`: `vec4 mask = step(maxW * 0.5, matWeights); matWeights *= mask;` then renormalize. |

### `getColorWeights` quoted verbatim (`gos_terrain.frag:151-175`)

```
PREC vec4 getColorWeights(PREC vec3 color) {
    PREC vec3 hsv = rgb2hsv(color);
    PREC float h = hsv.x;
    PREC float s = hsv.y;
    PREC float v = hsv.z;

    PREC vec4 w = vec4(0.0);

    // Green → grass, brown → dirt, everything else → rock.
    // Concrete weight comes only from TerrainType (cement vertices) later in main();
    // never from colormap, so snow/overlay-whitened tiles fall through to rock.
    w.y = smoothstep(0.10, 0.20, h) * smoothstep(0.10, 0.32, s);   // green
    w.z = smoothstep(0.17, 0.11, h) * smoothstep(0.10, 0.32, s);   // brown
    w.x = 1.0 - max(w.y, w.z);                                     // everything else → rock
    w.w = 0.0;

    PREC float isWater = smoothstep(0.35, 0.45, h);
    w.x += isWater;
    w.y *= (1.0 - isWater);
    w.z *= (1.0 - isWater);

    PREC float total = w.x + w.y + w.z + w.w;
    w = (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
    return w;
}
```

### Cement override math (verbatim, `gos_terrain.frag:278-291`)

```
if (useCementAtlas != 0) {
    uint cementWord  = thinRecsFrag[RecordIdx].control.w;
    bool cementValid = (cementWord & 0x80000000u) != 0u;
    if (cementValid) {
        uint layerIdx = cementWord & 0xFFFFu;  // V27: was 0xFFu pre-widening
        int  gridSide = atlasCementGridSide;
        if (gridSide < 1) gridSide = 1;
        int  cCol = int(layerIdx) % gridSide;
        int  cRow = int(layerIdx) / gridSide;
        PREC vec2 cTileUV = fract(vec2(WorldPos.x, -WorldPos.y) / atlasCementWorldUnitsPerTile);
        PREC vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(gridSide);
        texColor = texture(tex3, cAtlasUV);
    }
}
```

### `pureConcrete` + snow stealing (verbatim, `gos_terrain.frag:384-400`)

```
PREC float pureConcrete = smoothstep(2.0, 3.0, TerrainType);
// Use a stronger curve for color than for material/normal blending so boundary tiles
// keep the smooth transition shape but visually track the pure cement tone more closely.
PREC float concreteColorBlend = sqrt(clamp(pureConcrete, 0.0, 1.0));

matWeights = mix(matWeights, vec4(0.0, 0.0, 0.0, 1.0), pureConcrete);
// Snow is suppressed on cement tiles (pureConcrete dominates there).
snowWeight *= (1.0 - pureConcrete);
// Snow steals from the other weights proportionally so the total across all 5 = 1.
matWeights *= (1.0 - snowWeight);

PREC float totalWeights = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
if (totalWeights > 0.01) {
    matWeights /= totalWeights;
} else {
    matWeights = vec4(1.0, 0.0, 0.0, 0.0);
}
```

Observation: at step 12, `matWeights` is renormalized to sum to 1 ignoring `snowWeight`; `snowWeight` is kept as a separate scalar added later (e.g. used at `:497-500` when sampling `matNormal4`, and at `:526` when computing `materialTint`).

There is **also** a second classifier in the include header used by tese: see `shaders/include/terrain_common.hglsl:14-36` (`tc_getColorWeights`). It uses different smoothstep thresholds than the frag's `getColorWeights` (different `h`/`s`/`v` cutoffs and includes a `w.w` concrete/snow heuristic, the frag's does not). The frag's classifier and the include's classifier are textually distinct; only the include's is used for displacement classification (see Table 3).

---

## 3. Tese displacement flow + alpha sampling (`gos_terrain.tese`)

The TES does Phong smoothing optionally, then samples a per-fragment colormap to produce material weights, then samples one normal-map alpha (matNormal2, dirt) to produce a per-fragment scalar offset along the interpolated normal. This is **not** a per-class scalar amplitude; the displacement amplitude is the alpha channel of the dirt normal map, sampled per-fragment, scaled by the dirt classifier weight and the global `tessDisplace.y`.

### Quoted verbatim (`gos_terrain.tese:96-118`)

```
    // --- Phong tessellation smoothing ---
    float alpha = tessDisplace.x;  // phongAlpha
    if (alpha > 0.0) {
        vec3 proj0 = worldPos - dot(worldPos - tcs_WorldPos[0], tcs_WorldNorm[0]) * tcs_WorldNorm[0];
        vec3 proj1 = worldPos - dot(worldPos - tcs_WorldPos[1], tcs_WorldNorm[1]) * tcs_WorldNorm[1];
        vec3 proj2 = worldPos - dot(worldPos - tcs_WorldPos[2], tcs_WorldNorm[2]) * tcs_WorldNorm[2];
        vec3 phongPos = bary.x * proj0 + bary.y * proj1 + bary.z * proj2;
        worldPos = mix(worldPos, phongPos, alpha);
    }

    // --- Texture-based displacement along normal (dirt only) ---
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        vec3 colSample = texture(tex1, Texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);
        float dirtWeight = matWeights.z;
        if (dirtWeight > 0.01) {
            float baseTiling = detailNormalTiling.x;
            vec2 dispUV = Texcoord * baseTiling * TC_MAT_TILING.z;
            float disp = 1.0 - texture(matNormal2, dispUV).a;
            worldPos += worldNorm * (disp - 0.5) * displaceScale * dirtWeight;
        }
    }
```

### Inputs / state read

| Source | Location | Used for |
|---|---|---|
| `uniform vec4 tessDisplace` | `gos_terrain.tese:22` | `.x` = `phongAlpha`; `.y` = `displaceScale`. |
| `uniform sampler2D tex1` | `gos_terrain.tese:29` | Single colormap sample at the interpolated `Texcoord`. |
| `uniform sampler2D matNormal2` | `gos_terrain.tese:32` | The dirt normal+disp atlas; only the alpha channel is read. |
| `uniform vec4 detailNormalTiling` | `gos_terrain.tese:34` | `.x` = base tiling multiplier. |
| `#include <include/terrain_common.hglsl>` | `gos_terrain.tese:36` | Provides `tc_getColorWeights` and `TC_MAT_TILING` (`vec4(1.0, 12.0, 1.0, 6.0)` declared at `terrain_common.hglsl:39`). |
| `tcs_WorldPos[]`, `tcs_WorldNorm[]`, etc. | `gos_terrain.tese:5-9` | Phong smoothing inputs and barycentric interpolation. |

### Displacement amplitude characterisation

- **Direction:** along `worldNorm` (the barycentric-interpolated, then re-normalized vertex normal — `gos_terrain.tese:62-65`).
- **Per-fragment scalar amplitude:** `(disp - 0.5) * displaceScale * dirtWeight` where `disp = 1.0 - texture(matNormal2, dispUV).a`. So:
  - The texture sample is per-fragment (`texture(matNormal2, dispUV).a` at `gos_terrain.tese:115`).
  - The amplitude is centered around 0 by `(disp - 0.5)` (range −0.5 to +0.5 of inverted alpha).
  - Multiplied by per-fragment `dirtWeight` (the `.z` channel of the classifier output from `tc_getColorWeights(texture(tex1, Texcoord).rgb)`).
  - Multiplied by global `displaceScale = tessDisplace.y` (a per-frame uniform).
- **Slots whose textures are sampled in this path:** only `tex1` (colormap) and `matNormal2` (dirt). `matNormal0`, `matNormal1`, `matNormal3` are declared in the TES uniform block at `gos_terrain.tese:30-33` but **not sampled** in the body.
- **UV math:** `vec2 dispUV = Texcoord * baseTiling * TC_MAT_TILING.z;` (at `gos_terrain.tese:114`). With `TC_MAT_TILING.z = 1.0` (`terrain_common.hglsl:39`), `dispUV = Texcoord * detailNormalTiling.x`.

### Final projection (verbatim, `gos_terrain.tese:120-135`)

```
    WorldNorm = worldNorm;
    WorldPos = worldPos;

    // --- Projection of DISPLACED position (visual rendering) ---
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    // Match legacy CPU emit's TERRAIN_DEPTH_FUDGE=0.001 (mclib/quad.cpp:2004 etc.)
    // so decals/GpuStaticProps/water-on-terrain at coincident depth win the
    // GL_LEQUAL tie. Precedent: gos_terrain_water_fast.vert:332.
    screen.z = clip.z * rhw + 0.001;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position = vec4(ndc.xyz * absW, absW);
```

`UndisplacedDepth` is computed from the undisplaced (pre-Phong, pre-displacement) world position at `gos_terrain.tese:90-94`:

```
vec3 undisplacedWorldPos = bary.x * tcs_WorldPos[0]
                         + bary.y * tcs_WorldPos[1]
                         + bary.z * tcs_WorldPos[2];
vec4 uclip = terrainMVP * vec4(undisplacedWorldPos, 1.0);
UndisplacedDepth = (uclip.z / uclip.w) * 0.5 + 0.5;
```

---

## 4. Shadow tese behavior (`shadow_terrain.tese`)

The shadow TES runs the same dirt-only normal-alpha displacement as the forward TES, but skips Phong smoothing entirely. It writes a light-space `gl_Position` instead of a screen-space one.

### Quoted verbatim (`shadow_terrain.tese` body)

```
void main()
{
    vec3 bary = gl_TessCoord;

    vec3 worldPos = bary.x * tcs_WorldPos[0]
                  + bary.y * tcs_WorldPos[1]
                  + bary.z * tcs_WorldPos[2];

    vec3 worldNorm = normalize(
        bary.x * tcs_WorldNorm[0]
      + bary.y * tcs_WorldNorm[1]
      + bary.z * tcs_WorldNorm[2]);

    vec2 texcoord = bary.x * tcs_Texcoord[0]
                  + bary.y * tcs_Texcoord[1]
                  + bary.z * tcs_Texcoord[2];

    // Texture-based displacement along normal (dirt only) — matches main TES
    float displaceScale = tessDisplace.y;
    if (displaceScale > 0.0) {
        vec3 colSample = texture(tex1, texcoord).rgb;
        vec4 matWeights = tc_getColorWeights(colSample);

        float dirtWeight = matWeights.z;
        if (dirtWeight > 0.01) {
            float baseTiling = detailNormalTiling.x;
            vec2 dispUV = texcoord * baseTiling * TC_MAT_TILING.z;
            float disp = 1.0 - texture(matNormal2, dispUV).a;
            worldPos += worldNorm * (disp - 0.5) * displaceScale * dirtWeight;
        }
    }

    // Simple orthographic projection into light space
    gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);
}
```

### Comparison to forward TES

| Aspect | Forward `gos_terrain.tese` | `shadow_terrain.tese` | Reference |
|---|---|---|---|
| Phong smoothing | Yes, gated on `tessDisplace.x > 0.0` | No (no Phong block at all) | `gos_terrain.tese:97-104` vs `shadow_terrain.tese` (none) |
| Displacement texture | `matNormal2` (dirt), alpha channel | Same | `gos_terrain.tese:115`, `shadow_terrain.tese:46` |
| Classifier function | `tc_getColorWeights` (from include) | Same | `gos_terrain.tese:110`, `shadow_terrain.tese:40` |
| UV transform for disp | `Texcoord * baseTiling * TC_MAT_TILING.z` | `texcoord * baseTiling * TC_MAT_TILING.z` | `gos_terrain.tese:114`, `shadow_terrain.tese:45` |
| `dirtWeight` gate | `if (dirtWeight > 0.01)` | Same | `gos_terrain.tese:112`, `shadow_terrain.tese:43` |
| Global gate | `if (displaceScale > 0.0)` | Same | `gos_terrain.tese:108`, `shadow_terrain.tese:38` |
| Final projection | `terrainMVP` + viewport + per-pixel `mvp` chain | `lightSpaceMatrix * vec4(worldPos, 1.0)` | `gos_terrain.tese:124-135` vs `shadow_terrain.tese:52` |
| `UndisplacedDepth` | Written as varying | Not present (no varying outputs) | `gos_terrain.tese:94` vs `shadow_terrain.tese` (no out vars) |

Forward and shadow TES read the same `matNormal2` texture and alpha channel; whether they bind the same GL texture-object at draw time is documented in Table 6.

---

## 5. Thin terrain path (`gos_terrain_thin.vert + gos_terrain.frag`)

| Question | Answer |
|---|---|
| Where is the program created? | `gameos_graphics.cpp:2615-2619` via `glsl_program::makeProgram("gos_terrain_thin", "shaders/gos_terrain_thin.vert", "shaders/gos_terrain.frag", kThinPrefix);` |
| Prefix string | `"#version 430\n"` from `gameos_graphics.cpp:2614`. No `MRT_ENABLED` injected (path bypasses `gosMaterialVariationHelper::getMaterialVariation`). |
| Where is it bound at draw time? | `glUseProgram(shp);` at `gameos_graphics.cpp:3616`, inside `gosRenderer::terrainBindThinUniformsForPatchStream()` (declared at `:3612`). |
| Same FS as program #1? | Yes, file-identical: `shaders/gos_terrain.frag` (the same source). However the compiled FS variant is different because the variant prefix is different (no `MRT_ENABLED`, no `ALPHA_TEST`), producing a separate compiled shader object linked into a separate program. |
| Classifier flow | Identical to Table 2 (same `gos_terrain.frag`). The thin path differs only in the VS / TCS+TES stages, the linked FS code is identical. |
| Cement-atlas branch availability | Yes — `gos_terrain.frag:84` declares `flat in uint RecordIdx;` which the thin VS writes at `gos_terrain_thin.vert:86`. The legacy chain VS does not write `RecordIdx`; the legacy chain TES sets it to constant `0u` at `gos_terrain.tese:41`. |
| `MRT_ENABLED` block compilation | Compiles **out** in the thin program (no `#define MRT_ENABLED`); compiles **in** in program #1 if the post-process FBO has the normal attachment (per `:217-219`). Net effect: the `layout(location=1) out` declaration at `gos_terrain.frag:30` is omitted in the thin program. |
| `ALPHA_TEST` block compilation | Same logic — does not appear unless the variation helper injects it; for the thin path, never. |

The thin VS does its own perspective + viewport projection (verbatim `gos_terrain_thin.vert:166-180`):

```
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    // Match legacy CPU emit's TERRAIN_DEPTH_FUDGE=0.001 (mclib/quad.cpp:2004 etc.)
    // so decals/GpuStaticProps/water-on-terrain at coincident depth win the
    // GL_LEQUAL tie. Precedent: gos_terrain_water_fast.vert:332.
    screen.z = clip.z * rhw + 0.001;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position      = vec4(ndc.xyz * absW, absW);
    UndisplacedDepth = screen.z * 0.5 + 0.5;
```

There is no displacement in the thin VS; `UndisplacedDepth = screen.z * 0.5 + 0.5;` (line 179) — the comment at `:166` notes "No displacement => UndisplacedDepth == actual depth."

---

## 6. Sampler unit map by program / bind site

Sampler unit numbers are bound by `glUniform1i(loc, unit)` calls; texture-object binding is by `glActiveTexture(GL_TEXTUREn) + glBindTexture(GL_TEXTURE_2D, ...)`.

Each program is bound at draw time. The unit assignments below are the values written into the program-state at the cited bind site.

### Program #1 — `gos_terrain` (forward terrain)

| Sampler uniform | Unit | Bind site (file:line) |
|---|---|---|
| `tex1` | 0 | `gameos_graphics.cpp:3727` (`material->setSamplerUnit(gosMesh::s_tex1, 0);`). `s_tex1` is `"tex1"` per `:562`. |
| `matNormal0..4` | 5..9 (i.e. `5+i`) | Loop `for (int i = 0; i < 5; i++)` at `gameos_graphics.cpp:3779-3785`: `glUniform1i(tl.matNormal[i], 5 + i);` |
| `shadowMap` | 9 | `gameos_graphics.cpp:3796` (`glUniform1i(tl.shadowMap, 9);`) |
| `dynamicShadowMap` | 10 | `gameos_graphics.cpp:3807` (`glUniform1i(tl.dynamicShadowMap, 10);`) |
| `tex2` | not bound | No `glUniform1i` for `tex2` in this program in `gameos_graphics.cpp`. |
| `tex3` | unset on this draw path | The uniform exists in the FS (`gos_terrain.frag:35`); for program #1 it is bound only on the indirect path (Table below). |

**Collision note:** on program #1 if the loop above completes for `i=0..4`, `matNormal4` is written to **unit 9**. The shadow-map binding then writes `shadowMap` to **unit 9** as well. Both call `glUniform1i(loc, 9)` against different sampler uniforms within the same program, the actual binding for `matNormal4` versus `shadowMap` therefore depends on order. The matNormal loop writes first (`:3779-3785`), then `shadowMap` writes (`:3796`). After the second write, `shadowMap` references unit 9, and `matNormal4` continues to reference unit 9 too. The texture object actually bound to `GL_TEXTURE9` immediately before draw is the depth texture (`pp->getShadowTexture()` at `:3798`), because it is bound after the matNormal-array activation (`:3577`) and the `glActiveTexture(GL_TEXTURE0);` at `:3786` resets active state but not bindings.

### Program #2 — `shadow_terrain`

| Sampler uniform | Unit | Bind site (file:line) |
|---|---|---|
| `tex1` | 0 | `gameos_graphics.cpp:3349` (`glUniform1i(sl.tex1, 0);`) |
| `matNormal2` | 7 | `gameos_graphics.cpp:3338-3343` (`glUniform1i(sl.matNormal2, 7);` then `glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[2]);`) |
| Other matNormal slots | not declared in shaders | Only `matNormal2` is declared in `shadow_terrain.tese:14`; FS is `shadow_terrain.frag` which declares no samplers. |

### Program #3 — `gos_terrain_thin`

| Sampler uniform | Unit | Bind site (file:line) |
|---|---|---|
| `tex1` | 0 | `gameos_graphics.cpp:3671` (`if (tl.tex1 >= 0) glUniform1i(tl.tex1, 0);`) |
| `matNormal0..4` | 5..9 | Loop at `gameos_graphics.cpp:3672-3677`: `glUniform1i(tl.matNormal[i], 5 + i);` |
| `shadowMap` | 9 | `gameos_graphics.cpp:3688` (`glUniform1i(tl.shadowMap, 9);`) |
| `dynamicShadowMap` | 10 | `gameos_graphics.cpp:3699` (`glUniform1i(tl.dynamicShadowMap, 10);`) |
| `tex3` (cement atlas) | 3 | `gameos_graphics.cpp:2366` (`if (locTex3 >= 0) glUniform1i(locTex3, 3);`), inside `gos_terrain_bridge_drawIndirect` (`:2219`). The texture is bound via `glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());` at `:2339-2343`. The unit 3 sampler-object is cleared with `glBindSampler(3, 0);` at `:2342`. |
| `tex2` | not bound | Same as program #1 — declared but no `glUniform1i` site. |

**Same matNormal4↔shadowMap collision on unit 9** (`gameos_graphics.cpp:3673-3676` vs `:3687-3691`).

### Cross-program unit summary

| Unit | Program 1 (`gos_terrain`) | Program 2 (`shadow_terrain`) | Program 3 (`gos_terrain_thin`) | Shared / unique |
|---|---|---|---|---|
| 0 | `tex1` | `tex1` | `tex1` | All three programs use unit 0 for the colormap. |
| 3 | (not bound here) | — | `tex3` (cement atlas, indirect path only) | Owned by program 3 indirect path. |
| 5 | `matNormal0` | — | `matNormal0` | Shared with program 1 / 3. |
| 6 | `matNormal1` | — | `matNormal1` | Shared. |
| 7 | `matNormal2` | `matNormal2` | `matNormal2` | All three. |
| 8 | `matNormal3` | — | `matNormal3` | Shared. |
| 9 | `matNormal4` **and** `shadowMap` (same program, same draw) | — | `matNormal4` **and** `shadowMap` | Within-program collision in programs 1 and 3 (see above). |
| 10 | `dynamicShadowMap` | — | `dynamicShadowMap` | Shared. |

The shadow-map programs in non-terrain pipelines (e.g. mech / static-prop FS) also bind `shadowMap` and `dynamicShadowMap` at units 9 and 10 — see `gameos_graphics.cpp:5374-5395` (an unrelated material) and `:5702-5722`. Those programs do not also bind matNormal4, so their unit-9 use is exclusive.

---

## 7. `surfaceDebugMode` values in use

`surfaceDebugMode` is the integer cast of `tessDebug.x` (`gos_terrain.frag:300`: `int surfaceDebugMode = int(floor(tessDebug.x + 0.5));`). The variable is set in C++ as `terrain_debug_mode_` and uploaded to `tessDebug` (`gameos_graphics.cpp:3742-3743`).

| Value | File:line of conditional | Effect (as written in code) |
|---|---|---|
| < −2.5 | `gos_terrain.tese:43-55` | TES early-out — outputs a fixed equilateral triangle in NDC, sets all varyings to safe defaults. |
| < −1.5 (and ≥ −2.5) | `gos_terrain.tese:79-87` | TES early-out — sets varyings, position is a barycentric blend of `gl_in[*].gl_Position`. |
| < −0.5 (and ≥ −1.5) | `gos_terrain.frag:248-255` | FS outputs solid red `vec4(1.0, 0.0, 0.0, 1.0)`; if `MRT_ENABLED`, writes `rc_gbuffer1_shadowHandled_flatUp()`. |
| 1 | `gos_terrain.frag:307-318` | Depth diagnostic — R = `gl_FragCoord.z` mapped from [0.85,1.0], G = `UndisplacedDepth` mapped same range. `gl_FragDepth = actual;` and an early `return`. |
| 2 | `gos_terrain.frag:321-328` | Outputs raw `vec4(texColor.rgb, 1.0)` — the post-cement, atlas-or-tile colormap sample. |
| 3 | `gos_terrain.frag:363-370` | Outputs `vec4(colAvg, 1.0)` — the post-blur classified colormap sample. |
| 4 | `gos_terrain.frag:411-418` | Outputs `vec4(matWeights.x, matWeights.y, matWeights.z, 1.0)` — the per-fragment classifier weights for rock/grass/dirt (concrete and snow not visualized). |
| 5 | `gos_terrain.frag:576-583` | Outputs `vec4(vec3(normalLight), 1.0)` — the post-mix normal-lighting scalar (range 0.35..1.20 mixed by `diffuse`). |
| 6 | `gos_terrain.frag:644-651` | Outputs `vec4(vec3(shadow), 1.0)` — combined static × dynamic shadow factor. |
| 7 | `gos_terrain.frag:622-629` | Outputs `vec4(vec3(mix(0.92, 1.0, cloudShadow)), 1.0)` — animated FBM cloud-shadow visualization. |

There is **no** `surfaceDebugMode == 0` branch. Mode 0 is the default — the function falls through every guarded block.

`shadow_terrain.tese` reads `tessDebug` indirectly: `shadow_terrain.tese:9` declares `uniform vec4 tessDisplace;` only; there is no `tessDebug` in the shadow chain.

The `terrain_overlay.frag` (separate program, not part of the terrain material program list above) also reads `surfaceDebugMode` at `terrain_overlay.frag:42` and branches on values 1, 2, 3, 4, 6 (`:62-110`). Reported here only to note the namespace overlap; this is not the same shader.

---

## 8. UBO / SSBO binding points in use

A repo-wide `grep` over `shaders/` for `layout(std140` returned no matches; the codebase does not use std140 UBOs in any shader file. SSBO bindings only.

### `layout(std430, binding = N) readonly buffer ...` declarations

| Binding | Buffer name | Struct | Declaring shader file | Used by which shaders |
|---|---|---|---|---|
| 0 | `QuadRecordBuf` | `TerrainQuadRecord records[]` (12×vec4 / 192 B) | `shaders/gos_terrain.tesc:34-36` | `gos_terrain.tesc` (program 1) only. |
| 1 | `RecipeBuf` | `TerrainQuadRecipe recipes[]` | `shaders/gos_terrain_thin.vert:18-20` | `gos_terrain_thin.vert` (program 3). |
| 2 | `ThinRecordBuf` | `TerrainQuadThinRecord thinRecs[]` | `shaders/gos_terrain_thin.vert:9-11` | `gos_terrain_thin.vert`. |
| 2 | `ThinRecordBufFrag` | `TerrainQuadThinRecord_Frag thinRecsFrag[]` | `shaders/gos_terrain.frag:90-92` | `gos_terrain.frag` (linked into both program 1 and program 3). |
| 5 | `WaterRecipeBuf` | `WaterRecipe recipes[]` | `shaders/gos_terrain_water_fast.vert:30` | `gos_terrain_water_fast.vert` (water program). |
| 6 | `WaterThinBuf` | `WaterThin thins[]` | `shaders/gos_terrain_water_fast.vert:48` | Same. |
| 0 | `Instances` (`Instance i[]`) | static-prop instance buffer | `shaders/static_prop.vert:17` | `static_prop.vert` only — **NOT** a terrain program; the binding-0 namespace is shared across the GL context but not within a single terrain program's link unit. |
| 1 | `Colors` (`uint c[]`) | static-prop color buffer | `shaders/static_prop.vert:18` | Same — not terrain. |

Within the terrain set (programs 1 / 2 / 3):
- Binding 0: terrain `QuadRecordBuf` (program 1 only).
- Binding 1: `RecipeBuf` (program 3 only).
- Binding 2: shared between VS (`ThinRecordBuf`) and FS (`ThinRecordBufFrag`) when the thin program is linked. The VS struct (`gos_terrain_thin.vert:4-8`) has `uvec4 control; uvec4 lightRGBs;` — the FS struct (`gos_terrain.frag:86-89`) declares the same: `uvec4 control; uvec4 lightRGBs;`. Different types in source (`TerrainQuadThinRecord` vs `TerrainQuadThinRecord_Frag`) but identical std430 layout.

### UBO / std140

None present in `shaders/`. Confirmed by `grep -rn "layout(std140"` returning empty.

(UBO and SSBO bindings are independent namespaces; this row exists only to make absence explicit.)

---

## 9. `TerrainColorMap` init / reload / destroy lifecycle

Class declared at `mclib/terrtxm2.h:49-257`. Singleton-like pointer is `Terrain::terrainTextures2` (declared at `mclib/terrain.cpp:85`).

### Construction

- `TerrainColorMap::TerrainColorMap()` at `terrtxm2.h:109-112` calls `init()` (the no-arg version) immediately.
- `TerrainColorMap::init(void)` at `terrtxm2.cpp:76-119` — zeroes all members, including `colorMapStarted = false;` (`:88`).

### File-loading `init(char* fileName)` — callsites

The file-loading `init(char*)` is declared at `terrtxm2.h:121` and defined at `terrtxm2.cpp:1593`. Callsites:

| File:line | Context |
|---|---|
| `mclib/terrain.cpp:541` | First load during `Terrain::init`, gated by `if ( terrainTextures2  && !(terrainTextures2->colorMapStarted))` at `:538`. Called as `terrainTextures2->init(colorMapName);` if `colorMapName` is non-null, else `terrainTextures2->init(terrainName);` at `:543`. |
| `mclib/terrain.cpp:820` | Inside `Terrain::update`, same `colorMapStarted` guard (`:816`). Called as `terrainTextures2->init(colorMapName);` (`:820`) or `terrainTextures2->init(terrainName);` (`:822`). |

### `colorMapStarted` semantics

- Initialized `false` in the no-arg `init` at `terrtxm2.cpp:88`.
- Both the `Terrain::init` (`:538`) and `Terrain::update` (`:816`) callsites are guarded `if (... && !(terrainTextures2->colorMapStarted))`. So `init(char*)` is normally only entered when this flag is false.
- Inside `init(char*)`, the body's outermost work is `if (!colorMapStarted) { ... }` at `terrtxm2.cpp:1597` — the entire heavyweight load (colormap, normal map, material arrays) is inside this branch.
- The flag is set `true` at two points: `terrtxm2.cpp:1774` (JPG branch, currently unreachable due to `&& false` in the `if` at `:1719`) and `terrtxm2.cpp:1910` (TGA branch).
- After the `!colorMapStarted` block ends at `:1927`, the function continues with cleanup, normal-map load, detail-normal load, displacement load, and material-array load **unconditionally** — i.e., these run on every call to `init(char*)`, including subsequent calls. However, since the callers gate on `!colorMapStarted` before invoking, in practice the function runs at most once per mission until `colorMapStarted` is reset (which only happens in `init(void)` — the constructor / no-arg form).

### `destroy()`

Defined at `terrtxm2.cpp:121-165`. Frees `textures`, `txmRAM` array (per-tile), `ColorMap`, `colorMapHeap`, `colorMapRAMHeap`, `cpuDispAlpha`, `cpuColorMap`. Sets `numTextures = 0;` (`:159`). Does **not** reset `colorMapStarted`. Does **not** free or null `normalMapTextures` (CPU-side handle list allocated via `malloc` at `:2022`) or detail-normal / displacement / material-array GL textures.

Callsites of `destroy`:
- `Terrain::destroy()` at `mclib/terrain.cpp:691` (inside `if (terrainTextures2) { ... terrainTextures2->destroy(); delete terrainTextures2; terrainTextures2 = NULL; }`, the if at `:689`).
- `~TerrainColorMap()` at `terrtxm2.h:116-119` calls `destroy()`.

### Per-mission vs engine-lifetime

- `Terrain::destroy` is the only path that `delete`s `terrainTextures2` (set to `NULL` at `terrain.cpp:693`).
- A new `TerrainColorMap` is allocated at `terrain.cpp:461` (`terrainTextures2 = new TerrainColorMap;`), inside the colormap-availability check at `:459-462`, which is itself inside `Terrain::init` (the surrounding function).
- Therefore the instance is **per-Terrain**, and Terrain is rebuilt across missions (per `Terrain::destroy` followed by `Terrain::init`). The class instance does **not** survive mission boundaries; `colorMapStarted` is also fresh on each new instance because the constructor calls the no-arg `init`.

### Reload-from-disk paths

`resetBaseTexture(const char*)`, `resetDetailTexture`, `resetWaterTexture`, `resetWaterDetailTextures` are declared at `terrtxm2.h:230-233`. These are reload entry points used from the editor (per the comment at `terrtxm2.h:227-229`). They are not invoked from `Terrain::init` / `Terrain::update`.

### `recalcLight` callsite

`terrainTextures2->recalcLight(colorMapName);` at `mclib/terrain.cpp:2146`.

---

## 10. Engine-default normal/displacement texture loader (`mclib/terrtxm2.cpp:2161+`)

This block runs unconditionally inside `init(char*)` (after the `!colorMapStarted` cold-path block). It loads the engine-default per-class normal and displacement maps from `texturePath`.

### Quoted name arrays (`terrtxm2.cpp:2161-2166`)

```
const char* normalNames[5] = {
    "mat0_normal", "mat1_normal", "mat2_normal", "mat3_normal", "mat4_normal"
};
const char* dispNames[5] = {
    "mat0_displacement", "mat1_displacement", "mat2_displacement", "mat3_displacement", "mat4_displacement"
};
```

### Loop bound (verbatim, `terrtxm2.cpp:2172`)

```
for (int mat = 0; mat < 5; mat++)
```

(Same loop bound is repeated for the displacement layers at `terrtxm2.cpp:2217`.)

### Conditional skip-if-missing logic (verbatim, `terrtxm2.cpp:2177-2185`)

```
if (!fileExists(nmPath)) {
    printf("[SPLATTING] NOT FOUND: %s%s\n", (const char*)nmPath, mat >= 4 ? " (optional slot skipped)" : "");
    // mat4+ are optional — missing file leaves the slot empty but doesn't fail the load.
    if (mat >= 4) continue;
    allLoaded = false; break;
}

File nmFile;
if (nmFile.open(nmPath) != NO_ERR) { if (mat >= 4) continue; allLoaded = false; break; }
```

The displacement loop has identical opt-out at `:2221`: `if (!fileExists(dispPath)) { if (mat >= 4) continue; allLoaded = false; break; }` and `:2224`: `if (dispFile.open(dispPath) != NO_ERR) { if (mat >= 4) continue; allLoaded = false; break; }`.

### All-same-width constraint (verbatim, `terrtxm2.cpp:2191-2196`)

```
if (nmInfo.image_type != UNC_TRUE || nmInfo.width != nmInfo.height)
{ free(nmData); allLoaded = false; break; }

if (arrayWidth == 0) arrayWidth = nmInfo.width;
else if (nmInfo.width != arrayWidth)
{ free(nmData); allLoaded = false; break; }
```

The displacement loop has the corresponding check at `:2230`: `if (dispInfo.image_type != UNC_TRUE || dispInfo.width != arrayWidth) { free(dispData); allLoaded = false; break; }`.

### Where the resulting handles are stored / consumed

After the load loops complete, if `allLoaded` is true, GL textures are created and registered (verbatim, `terrtxm2.cpp:2251-2261`):

```
if (allLoaded) {
    printf("[SPLATTING] all loaded OK, width=%d, creating individual textures\n", arrayWidth);
    for (int i = 0; i < 5; i++) {
        if (!normalLayers[i]) continue;  // optional slot absent
        unsigned int nmId = gos_CreateTerrainNormalTexture(normalLayers[i], arrayWidth);
        printf("[SPLATTING] matNormal%d GL id=%u\n", i, nmId);
        gos_SetTerrainMaterialNormal(i, nmId);
    }
} else {
    printf("[SPLATTING] FAILED to load all material textures\n");
}
```

`gos_SetTerrainMaterialNormal(i, glTexId)` is the registration entry point. The handles are then consumed at draw time via the C++ array `terrain_mat_normal_[5]` referenced at `gameos_graphics.cpp:3577` (`glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);`), `:3676`, and `:3783`. The loop in those bind sites runs `for (int i = 0; i < 5; i++) { if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) { ... } }`, so a slot with `terrain_mat_normal_[i] == 0` (e.g. mat4 missing) is silently skipped on the bind side as well.

The displacement (`dispLayers[]`) data is populated but **not** uploaded as a texture or stored in any GL handle by this block — the only consumer of `dispLayers[i]` after the loop is the cleanup `free` at `terrtxm2.cpp:2279`. The TES displacement instead reads the alpha channel of `matNormal2` (see Tables 3 and 4).

### CPU mirror of matNormal2 alpha (verbatim, `terrtxm2.cpp:2263-2275`)

```
// Retain CPU copy of matNormal2 alpha for terrain elevation displacement
if (normalLayers[2]) {
    if (cpuDispAlpha) { free(cpuDispAlpha); cpuDispAlpha = NULL; }
    long pixels = (long)arrayWidth * (long)arrayWidth;
    cpuDispAlpha = (unsigned char*)malloc(pixels);
    gosASSERT(cpuDispAlpha != NULL);
    cpuDispAlphaSize = arrayWidth;
    const unsigned char* src = normalLayers[2];
    for (long i = 0; i < pixels; i++) {
        cpuDispAlpha[i] = src[i * 4 + 3]; // alpha channel (RGBA layout)
    }
    printf("[SPLATTING] retained matNormal2 alpha on CPU (%dx%d)\n", arrayWidth, arrayWidth);
}
```

`cpuDispAlpha` and `cpuDispAlphaSize` are declared as public members of `TerrainColorMap` at `terrtxm2.h:91-92`.

---

## Verification methodology

Each table was constructed by directly reading the cited source files, not by paraphrasing prior memory or specs. Specifically:

- **Table 1:** Located the program-creation block by `grep`'ing `gameos_graphics.cpp` for `terrain_material_`, `shadow_terrain_material_`, `thin_terrain_prog_`, and `water_fast_prog_`, then read the surrounding 100-line region around `:2576-2647`. Confirmed the `makeProgram` vs `makeProgram2` dispatch at `:261-298`. Confirmed the prefix-string source at `:200-238`.
- **Table 2:** Read `gos_terrain.frag` end-to-end. Quoted `getColorWeights` (`:151-175`), the cement override (`:278-291`), `pureConcrete` and snow stealing (`:384-400`) verbatim. Confirmed the alternative classifier in `terrain_common.hglsl:14-36` is **not** used by the frag (frag uses its own `getColorWeights` defined inline).
- **Table 3:** Read `gos_terrain.tese` end-to-end. Quoted the displacement block (`:96-118`) verbatim. Cross-referenced `TC_MAT_TILING` at `terrain_common.hglsl:39`. Confirmed which samplers are sampled vs merely declared by re-grepping `texture(matNormal0` etc. through the file.
- **Table 4:** Read `shadow_terrain.tese` end-to-end. Quoted the body verbatim. Compared each line to the equivalent in `gos_terrain.tese`.
- **Table 5:** Located the thin-program creation site at `gameos_graphics.cpp:2615-2619` and the bind site at `:3616`. Read `gos_terrain_thin.vert` end-to-end, including the projection block (`:166-180`).
- **Table 6:** `grep`'ed `gameos_graphics.cpp` for `setSamplerUnit`, `glUniform1i`, `matNormal`, `shadowMap`, `dynamicShadowMap`, and `tex1/2/3`. Cross-checked each unit number against the surrounding `glActiveTexture(GL_TEXTUREn) + glBindTexture` calls. Confirmed the matNormal4 ↔ shadowMap unit-9 collision by reading the consecutive code in `:3779-3796` and `:3672-3691`.
- **Table 7:** `grep`'ed shader files for `surfaceDebugMode` and `tessDebug.x`. Confirmed mode-0 absence by reading the full conditional cascade in `gos_terrain.frag:248-651`.
- **Table 8:** `grep`'ed `shaders/` for `layout(std430` (8 hits) and `layout(std140` (0 hits). Read each hit in context to record the binding number and buffer name.
- **Table 9:** `grep`'ed `terrtxm2.cpp` for `colorMapStarted`, `TerrainColorMap::init`, `TerrainColorMap::destroy`. `grep`'ed `terrain.cpp` for `terrainTextures2`. Read the constructor at `terrtxm2.h:109-112` and the file-loading init at `terrtxm2.cpp:1593-1700+`.
- **Table 10:** Read `terrtxm2.cpp:2161-2281` end-to-end. Quoted the name arrays, loop bound, skip-if-missing branches, all-same-width check, GL-creation block, and CPU mirror block verbatim. Cross-referenced `terrain_mat_normal_[i]` consumers via `grep` over `gameos_graphics.cpp`.
