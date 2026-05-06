# Terrain Shader Input Map — 2026-04-27

> Read-only exploration. No source files were modified.
> Covers the full 4-stage tessellation pipeline:
> `gos_terrain.vert → gos_terrain.tesc → gos_terrain.tese → gos_terrain.frag`
> Cross-referenced against C++: `gameos_graphics.cpp` + `mclib/txmmgr.cpp`.

---

## 1. Vertex Attributes (`layout(location = N) in`)

All attribute locations are resolved **dynamically** via `glGetAttribLocation()` at material compile time (not hardcoded integer literals in the C++). The shader-side `location =` values below are taken directly from the GLSL source.

| Loc | GLSL name | GLSL type | Stride | Offset | VBO | C++ source | Stage |
|-----|-----------|-----------|--------|--------|-----|------------|-------|
| 0 | `pos` | `vec4` | 36 B | 0 | `mesh->getVB()` | `gos_VERTEX.x/y/z/rhw` (4 floats) | vert |
| 1 | `color` | `vec4` | 36 B | 16 | `mesh->getVB()` | `gos_VERTEX.argb` (DWORD, UBYTE×4) | vert |
| 2 | `fog` | `vec4` | 36 B | 20 | `mesh->getVB()` | `gos_VERTEX.frgb` (DWORD, UBYTE×4) — **material index encoded in `.x * 255`; fog density in `.w`** | vert |
| 3 | `texcoord` | `vec2` | 36 B | 24 | `mesh->getVB()` | `gos_VERTEX.u/v` (2 floats) | vert |
| 4 | `worldPos` | `vec3` | 24 B | 0 | `terrain_extra_vb_` | `gos_TERRAIN_EXTRA.wx/wy/wz` | vert |
| 5 | `worldNorm` | `vec3` | 24 B | 12 | `terrain_extra_vb_` | `gos_TERRAIN_EXTRA.nx/ny/nz` | vert |

**Setup location in C++:** `gameos_graphics.cpp:2784–2807`
- Main VBO attributes (0–3): `gosRenderMaterial::applyVertexDeclaration()` → `gameos_graphics.cpp:340–359`
- Extras VBO attributes (4–5): inline in `terrainDrawIndexedPatches()` → `gameos_graphics.cpp:2800–2807`

---

## 2. `gos_TERRAIN_EXTRA` / Extras VBO

**C++ struct** (24 bytes, tightly packed):

```cpp
struct gos_TERRAIN_EXTRA {
    float wx, wy, wz;   // MC2 world position  — offset 0,  12 bytes
    float nx, ny, nz;   // MC2 world normal     — offset 12, 12 bytes
};
```

**Buffer lifecycle:**
- Created once as a dynamic GPU buffer (`terrain_extra_vb_`).
- Re-uploaded every frame from `terrain_batch_extras_` pointer via `updateBuffer(..., GL_DYNAMIC_DRAW)` at `gameos_graphics.cpp:2793–2794`.
- The pointer is set per terrain node by `gos_SetTerrainBatchExtras()` (line 4309), called from `txmmgr.cpp:1324` inside the per-node loop.
- Count must match the vertex count of `mesh->getVB()` exactly (1:1 correspondence assumed).

**Which stage reads it:**
- Vertex shader reads `worldPos` (loc 4) and `worldNorm` (loc 5), emits them as `vs_WorldPos` / `vs_WorldNorm` to TCS.
- TCS passes them through unchanged as `tcs_WorldPos` / `tcs_WorldNorm`.
- TES interpolates them barycentrically and further displaces `WorldPos` via heightmap sampling.
- Fragment shader receives the final interpolated `WorldPos` and `WorldNorm`.

---

## 3. Direct Uniforms

### gos_terrain.vert
| Name | Type | C++ upload site | Notes |
|------|------|-----------------|-------|
| `mvp` | `mat4` | `gosRenderMaterial::apply()` (standard pipeline uniform) | Screen-pixel NDC projection |

### gos_terrain.tesc
| Name | Type | C++ upload site | Notes |
|------|------|-----------------|-------|
| `tessLevel` | `vec4` | `gameos_graphics.cpp:2700` | `.x`=inner, `.y`=outer tess level |
| `tessDistanceRange` | `vec4` | `gameos_graphics.cpp:2701` | `.x`=near dist, `.y`=far dist |
| `cameraPos` | `vec4` | `gameos_graphics.cpp:2703` | Camera position in Stuff/MLR space |

### gos_terrain.tese
| Name | Type | C++ upload site | Notes |
|------|------|-----------------|-------|
| `tessDisplace` | `vec4` | `gameos_graphics.cpp:2702` | `.x`=phongAlpha, `.y`=displaceScale |
| `terrainMVP` | `mat4` | `gameos_graphics.cpp:2720` | axisSwap × worldToClip; uploaded `GL_FALSE` (row-major math cancels) |
| `terrainViewport` | `vec4` | `gameos_graphics.cpp:2715` | `(vmx, vmy, vax, vay)` perspective params |
| `mvp` | `mat4` | standard pipeline | screen-pixel → NDC |
| `detailNormalTiling` | `vec4` | `gameos_graphics.cpp:2726` | `.x`=base tiling multiplier |
| `matNormal0` | `sampler2D` | `gameos_graphics.cpp:2743` — unit 5 | Rock normal+displacement |
| `matNormal1` | `sampler2D` | `gameos_graphics.cpp:2743` — unit 6 | Grass normal+displacement |
| `matNormal2` | `sampler2D` | `gameos_graphics.cpp:2743` — unit 7 | Dirt normal+displacement |
| `matNormal3` | `sampler2D` | `gameos_graphics.cpp:2743` — unit 8 | Concrete normal+displacement |

### gos_terrain.frag
| Name | Type | C++ upload site | Notes |
|------|------|-----------------|-------|
| `tex1` | `sampler2D` | bound upstream in txmmgr before call; `glUniform1i→0` | Colormap / splat |
| `tex2` | `sampler2D` | — (legacy fallback, may be unset) | Detail normal engine fallback |
| `tex3` | `sampler2D` | — (legacy unused) | Detail displacement |
| `matNormal0`–`matNormal3` | `sampler2D` | `gameos_graphics.cpp:2743` — units 5–8 | Material normals (same as TES) |
| `matNormal4` | `sampler2D` | `gameos_graphics.cpp:2743` — unit 9 | Snow (⚠️ **collides with `shadowMap` on unit 9** — see §6) |
| `terrainLightDir` | `vec4` | `gameos_graphics.cpp:2724` | Light direction |
| `detailNormalTiling` | `vec4` | `gameos_graphics.cpp:2726` | |
| `detailNormalStrength` | `vec4` | `gameos_graphics.cpp:2728` | |
| `fog_color` | `vec4` | — | Fog blend color |
| `pomParams` | `vec4` | `gameos_graphics.cpp:2730` | `.x`=POM scale, `.y`=8 min steps, `.z`=32 max steps |
| `cameraPos` | `vec4` | `gameos_graphics.cpp:2703` | |
| `terrainWorldScale` | `vec4` | `gameos_graphics.cpp:2732` | |
| `terrainViewDir` | `vec4` | — | |
| `tessDebug` | `vec4` | `gameos_graphics.cpp:2705` | `.x`=debug mode (0–7) |
| `time` | `float` | `gameos_graphics.cpp:2737` | `(now_ms - start_ms) / 1000` |
| `mapHalfExtent` | `float` | `gameos_graphics.cpp:2711` | Half side of playable map |
| `cellBombParams` | `vec4` | `gameos_graphics.cpp:2734` | anti-tiling cell params |

### shadow.hglsl (included by frag)
| Name | Type | C++ upload site |
|------|------|-----------------|
| `shadowMap` | `sampler2DShadow` | `gameos_graphics.cpp:2758` → unit 9 |
| `lightSpaceMatrix` | `mat4` | `gameos_graphics.cpp:2754` |
| `enableShadows` | `int` | `gameos_graphics.cpp:2755` |
| `shadowSoftness` | `float` | `gameos_graphics.cpp:2756` |
| `dynamicShadowMap` | `sampler2DShadow` | `gameos_graphics.cpp:2769` → unit 10 |
| `dynamicLightSpaceMatrix` | `mat4` | `gameos_graphics.cpp:2766` |
| `enableDynamicShadows` | `int` | `gameos_graphics.cpp:2767` |

---

## 4. UBOs (`layout(std140) uniform`)

**None.** The terrain pipeline uses zero UBO blocks. All uniform data is uploaded via direct `glUniform*` calls.

---

## 5. SSBOs (`layout(std430) buffer`)

**None.** The terrain tessellation pipeline has no SSBO declarations in any stage (vert, tesc, tese, frag) or included headers.

---

## 6. Texture Samplers — Complete Unit Map

| Unit | Sampler name | Sampler type | Contents | Bound at |
|------|-------------|--------------|----------|----------|
| 0 | `tex1` | `sampler2D` | Terrain colormap / splat | txmmgr.cpp:1331 (`gos_State_Texture`) — **before** `terrainDrawIndexedPatches` |
| 5 | `matNormal0` | `sampler2D` | Rock normal + displacement | `gameos_graphics.cpp:2744–2745` |
| 6 | `matNormal1` | `sampler2D` | Grass normal + displacement | `gameos_graphics.cpp:2744–2745` |
| 7 | `matNormal2` | `sampler2D` | Dirt normal + displacement | `gameos_graphics.cpp:2744–2745` |
| 8 | `matNormal3` | `sampler2D` | Concrete normal + displacement | `gameos_graphics.cpp:2744–2745` |
| 9 | `matNormal4` | `sampler2D` | Snow normal | `gameos_graphics.cpp:2744–2745` |
| **9** | **`shadowMap`** | **`sampler2DShadow`** | **Static shadow depth map** | **`gameos_graphics.cpp:2759–2760`** — ⚠️ **overwrites unit 9** |
| 10 | `dynamicShadowMap` | `sampler2DShadow` | Dynamic object shadow depth | `gameos_graphics.cpp:2770–2771` |

> **⚠️ Unit 9 collision:** `matNormal4` (snow) and `shadowMap` both target unit 9. The shadow map bind at line 2759 executes *after* the material normal bind loop at line 2744, so the shadow map wins. Any terrain fragment that samples `matNormal4` will read shadow depth data instead of the snow normal texture. Snow biome rendering is currently broken when shadows are enabled.

---

## 7. Material / Texture Selection Per Draw

Terrain is drawn **one draw call per texture node** (`masterVertexNodes[i]`). Each node corresponds to one colormap texture region. The loop is in `txmmgr.cpp:1300–1357` ("Render.TerrainSolid" zone).

- Filter: node must have flags `MC2_ISTERRAIN | MC2_DRAWSOLID`
- Each node binds its own colormap to unit 0 via `gos_State_Texture` before calling `terrainDrawIndexedPatches`
- Material normal textures (units 5–9) are **shared across all nodes** — they represent biome types (rock/grass/dirt/concrete/snow), not per-chunk maps
- The per-fragment material blend is derived from the colormap (`tex1`) RGB at runtime — not from a per-draw state change
- Draw call: `glDrawElements(GL_PATCHES, ni, GL_UNSIGNED_SHORT|INT, NULL)` at `gameos_graphics.cpp:2822–2823`

---

## 8. GBuffer Outputs

**Fragment shader outputs:**
| Location | Variable | Written when |
|----------|----------|-------------|
| 0 | `FragColor` (vec4) | Always |
| 1 | `GBuffer1` (vec4) | Only when `MRT_ENABLED` is defined |

`GBuffer1` is written via helpers from `shaders/include/render_contract.hglsl`. **Exact call sites in `gos_terrain.frag`:**

| Line | Call | When reached | What it writes to GBuffer1 |
|------|------|--------------|---------------------------|
| 206 | `rc_gbuffer1_shadowHandled_flatUp()` | Debug mode path | `(0.5, 0.5, 1.0, 1.0)` — pre-encoded up-vector, shadow=handled |
| 245 | `rc_gbuffer1_shadowHandled_flatUp()` | Debug mode path | same |
| 287 | `rc_gbuffer1_shadowHandled_flatUp()` | Debug mode path | same |
| 335 | `rc_gbuffer1_shadowHandled_flatUp()` | Debug mode path | same |
| 500 | `rc_gbuffer1_shadowHandled(N)` | Debug mode 5 | `(N * 0.5 + 0.5, 1.0)` — perturbed normal, shadow=handled |
| 546 | `rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha)` | Debug mode 7 (shoreline) | `(N * 0.5 + 0.5, materialAlpha)` — alpha encodes water flag |
| 568 | `rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha)` | Debug mode 6 (shoreline) | same |
| **604** | **`rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha)`** | **Main path (all production frames)** | **`(N * 0.5 + 0.5, materialAlpha)` — real terrain normal + shoreline/water alpha** |

**Helper definitions in `render_contract.hglsl`:**

| Helper | Returns | GBuffer1.rgb | GBuffer1.a |
|--------|---------|--------------|------------|
| `rc_gbuffer1_shadowHandled(vec3 N)` | vec4 | `N * 0.5 + 0.5` | `1.0` (shadow self-handled) |
| `rc_gbuffer1_screenShadowEligible(vec3 N)` | vec4 | `N * 0.5 + 0.5` | `0.0` (eligible for screen-shadow pass) |
| `rc_gbuffer1_shadowHandled_flatUp()` | vec4 | `(0.5, 0.5, 1.0)` | `1.0` |
| `rc_gbuffer1_legacyTerrainMaterialAlpha(vec3 N, float a)` | vec4 | `N * 0.5 + 0.5` | `a` (water / shoreline flag) |
| `rc_pixelHandlesOwnShadow(vec4 g1)` | bool | — | `g1.a > 0.5` |

---

## 9. TCS/TES-Specific Data Flow

### Per-vertex interface (vert → tesc → tese)

All 6 varyings are passed through TCS unchanged (simple `out[gl_InvocationID] = in[gl_InvocationID]` passthrough):

```
vs_Color       (vec4)   → tcs_Color       → Color      (vec4,  smooth, → frag)
vs_FogValue    (float)  → tcs_FogValue    → FogValue   (float, smooth, → frag)
vs_Texcoord    (vec2)   → tcs_Texcoord    → Texcoord   (vec2,  smooth, → frag)
vs_TerrainType (float)  → tcs_TerrainType → TerrainType(float, smooth, → frag)
vs_WorldPos    (vec3)   → tcs_WorldPos    → WorldPos   (vec3,  smooth, → frag)
vs_WorldNorm   (vec3)   → tcs_WorldNorm   → WorldNorm  (vec3,  smooth, → frag)
```

TES also emits one additional output not in TCS:
- `UndisplacedDepth` (float, smooth) — depth of the pre-displacement surface, used by frag to avoid z-fighting.

### Tessellation built-ins

| Built-in | Read/Write | Stage | Purpose |
|----------|-----------|-------|---------|
| `gl_InvocationID` | Read | TCS | Index into per-vertex arrays (0..2) |
| `gl_in[].gl_Position` | Read | TCS | Input clip positions from VS |
| `gl_out[].gl_Position` | Write | TCS | Passed through unchanged |
| `gl_TessLevelOuter[0..2]` | Write | TCS | Set from `tessLevel.y` uniform |
| `gl_TessLevelInner[0]` | Write | TCS | Set from `tessLevel.x` uniform |
| `gl_TessCoord` | Read | TES | Barycentric coords for interpolation |
| `gl_Position` | Write | TES | Final clip-space position after Phong tess + displacement |

### SSBO/UBO reads in TES
**None.** The TES reads only from its own per-vertex input arrays (interpolated by barycentric coordinate) and from the declared sampler uniforms (`tex1`, `matNormal0–3`). It does **not** index any buffer object by `gl_VertexID` or any other index.

---

## Shape B Implications

Shape B = a proposed new terrain patch stream that needs to supply per-patch or per-vertex data beyond what the current `gos_TERRAIN_EXTRA` carries.

- **SSBO indexed by `gl_VertexID` in the TES is not straightforwardly usable.** After tessellation, `gl_VertexID` in the TES refers to the tessellated vertex index within a patch subdivision, not the original control-point index. To index an SSBO by original patch vertex, you'd need to pass control-point indices as a per-patch `patch out` from TCS, then use those in TES — this requires new shader declarations in both TCS and TES. The current pipeline has no infrastructure for this.

- **Extending the extras VBO is the path of least resistance.** The existing `terrain_extra_vb_` / `gos_TERRAIN_EXTRA` pattern already proves the mechanism: add new fields to the struct, increase the stride in `glVertexAttribPointer`, declare new `layout(location = N) in` attributes in `gos_terrain.vert`, and emit them as new varyings. TCS is a trivial passthrough; TES can then interpolate them barycentrically like everything else. No TES SSBO indexing complexity.

- **A texture buffer (`samplerBuffer`) could work from TES with less friction than an SSBO,** because it doesn't require knowing tessellated-vs-control-point indices — you would pass a flat control-point index as a flat `patch out` from TCS and index the TBO by that in TES. But it still requires new sampler declarations and TCS patch-out plumbing.

- **Persistent-mapped VBO via new attribute slots is the safest option.** It extends an already-proven path (attributes 4–5 are exactly this), requires changes only in the vertex shader (one new `layout(location)` per field) and in the C++ extras struct, and gives the TES barycentrically interpolated values at zero extra cost.

- **Fix the unit 9 collision before adding snow/5th-biome terrain data.** Any Shape B work that touches material normal sampling should first relocate `shadowMap` to unit 11 (or higher) and shift `matNormal4` to a free unit — otherwise snow biome fragments will continue silently sampling the shadow depth buffer as their normal map.
