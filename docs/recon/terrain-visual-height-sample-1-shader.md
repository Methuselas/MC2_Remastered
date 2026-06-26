# TERRAIN-VISUAL-HEIGHT-SAMPLE-1 — shader-side recon (read-only)

Worktree: nifty-mendeleev. Goal: displace LIVE terrain geometry to a 4x-finer
VISUAL heightfield; gameplay height stays authoritative; gate default-OFF
byte-identical. This doc covers the SHADER side (vertex displacement, normals,
shadow geometry, depth/clip, gate). All file:line grep-verified 2026-06-26.

## 0. Two distinct terrain renderers — DO NOT conflate

- **terrain_lod_chunk.vert** (LIVE solid terrain, default path; gate
  `mc2TerrainLodChunkEnabled`). Reads geometry height from **SSBO binding 23**
  (`TerrainHeightBuf`). THIS is where visual displacement enters.
- **gos_terrain.{vert,tesc,tese}.frag** (tessellated path) + the
  TERRAIN-NORMALS / TERRAIN-RESAMPLE stack — reads a separate **R32F height
  TEX (sampler unit 11)** for NORMALS only, never moves geometry.
- TERRAIN-RESAMPLE-1 (`gos_terrain_height_tex.cpp`) resamples the **tex**
  (normals), NOT the SSBO (geometry). They are independent buffers fed by
  independent uploads.

## 1. VERT HEIGHT READ + displacement entry  (terrain_lod_chunk.vert)

- SSBO decl: `layout(binding = 23, std430) readonly buffer TerrainHeightBuf { float heights[]; }` — :25-27
- Height fetch: `float h = heights[mapX + mapY * u_mapSide];` — :45
- (edge-stitch may overwrite `h` via mix() :52-75; skirt pull :78)
- World pos: `worldX = mapX*128 - halfMap`, `worldY = halfMap - mapY*128`,
  `v_worldPos = vec3(worldX, worldY, h)` — :80-82
- Clip: `clip = u_worldToClipGL * vec4(worldX,worldY,h,1.0)` — :85; depth fudge
  `clip.z += 2*TERRAIN_DEPTH_FUDGE*clip.w` (FUDGE = +0.002 here, forward-Z
  LEQUAL) — :16, :86; `gl_Position = clip` — :87.

**Displaced Z enters at `h` (:45).** To displace 4x: index a finer height
array. Grid is `u_mapSide` square; world step is hardcoded 128.0 (:80-81). A 4x
finer grid means u_mapSide*4 samples and a 128/4=32 world step — BOTH the SSBO
content AND the index math (`u_mapSide`, the `*128.0`) must change in lockstep,
OR keep coarse XY topology and only sub-sample `h` per coarse vertex (no new
verts = no extra detail). True 4x geometry detail requires 4x more emitted
vertices (CPU patch-gen side, gos_terrain_lod_chunk.cpp), not just a finer SSBO.

## 2. NORMALS  (terrain_height_normal.hglsl)

- `computeTerrainNormalFromHeight(worldXY)` central-difference from the R32F
  **tex unit 11**, span = `2*wuPerVertex` (:83-89). wuPerVertex already divides
  by the resample factor (`g_worldUnitsPerVertex = wuPerSource/factor`,
  gos_terrain_height_tex.cpp:198), so the normal tex CAN be 4x today.
- **YES normals must come from the 4x height too.** If geometry moves to a 4x
  shape but normals are computed from the coarse height, lighting mismatches the
  new silhouette (shading reads the old slope). Fix = feed the SAME 4x bake to
  BOTH consumers: SSBO@23 (geometry, terrain_lod_chunk) and R32F tex@11
  (normals). One resample (`resample_bilinear`, gos_terrain_height_tex.cpp:65)
  can produce one finer grid; upload it to both the tex and the SSBO. Note the
  LOD-chunk frag's own normal source — verify terrain_lod_chunk.frag uses the
  unit-11 helper (it includes the same hglsl) before relying on this.

## 3. SHADOW GEOMETRY — DESYNC RISK (the critical finding)

- Shadow caster terrain is a **SEPARATE CPU-built path**, NOT the SSBO.
  `MapData::renderStaticTerrainShadowFullMap` (mapdata.cpp:1294) walks
  `blocks[].elevation` (SOURCE MapData, coarse, un-resampled) into gos_VERTEX +
  gos_TERRAIN_EXTRA buffers — emit macro `_wz = _pv->elevation`, world step
  `WUPV` (mapdata.cpp:1316-1325). World XY identical formula to the main vert.
- Vertex stage `shadow_terrain.vert` just passes `worldPos` attrib through
  (:14-23); `shadow_terrain.tese:62` writes `gl_Position = lightSpaceMatrix *
  vec4(worldPos,1.0)` (tese displacement at :44-58 is dirt-normal POM only,
  unrelated). So shadow Z = the CPU-emitted coarse `elevation`.
- **DESYNC: main terrain displaces 4x (SSBO@23) but shadow caster stays coarse
  (blocks[].elevation) → shadow shape != visible shape** (self-shadow seams,
  peak shadows offset, shoreline mismatch).
- **Lockstep fix:** feed the SAME 4x heightfield into the shadow emit. Two
  options: (a) CPU — make `renderStaticTerrainShadowFullMap` read the resampled
  grid (the cached `g_cachedSourceElev` resample, or a shared 4x array) and emit
  4x verts (mind the 59994 CHUNK cap / 16-bit index, mapdata.cpp:1306 — 4x verts
  = ~16x quads, will flush far more chunks). (b) Move the shadow caster onto the
  same SSBO@23 in a displacing VS. Either way the shadow geometry MUST consume
  the identical displaced height the main vert uses.

## 4. DEPTH / CLIP lockstep

- Main vert FUDGE: `TERRAIN_DEPTH_FUDGE = -0.002` const, applied PRE-divide
  `clip.z += 2*FUDGE*clip.w` (terrain_lod_chunk.vert:16,86). NB: the file
  comment/const here reads +0.002/forward-Z LEQUAL; the shared include
  `terrain_depth_bias.hglsl:43` is reverse-Z -0.002 — confirm which regime the
  live build compiles (reverse-Z is the current stack). Lockstep note at
  terrain_lod_chunk.vert:14-15 points to that include.
- Water/overlay co-planar biases (`WATER_DEPTH_BIAS -0.00175`,
  `OVERLAY_DEPTH_BIAS 0.00005`, terrain_depth_bias.hglsl:43-47) are epsilons off
  the SHARED terrain reference plane. They depend on terrain Z being where water
  expects it. **Displacing terrain Z by up to 4x-finer detail changes the
  terrain surface relative to the flat water plane.** The depth FUDGE itself is
  a constant NDC offset (independent of geometry), so it does NOT need to track
  displacement. BUT: if the 4x detail pushes terrain below the water plane at a
  shoreline that was previously above it, that is a WORLD-SPACE z relationship,
  not a bias issue — water z-fight/punch-through can reappear. Hard bound:
  |water abs| 0.0025 < 0.004 (terrain_depth_bias.hglsl:38). Keep gameplay/water
  height authoritative; displacement is visual-only so the water plane stays put
  — verify shoreline cells don't invert. No bias constant change required for
  default-OFF; revisit only if 4x detail visibly z-fights water.

## 5. GATE — default-OFF byte-identical

- Discipline: terrain_lod_chunk uses the DIRECT uniform path —
  `glUseProgram(s_terrainProgram)` then `glUniform1i/...` AFTER bind
  (gos_terrain_lod_chunk.cpp:625, 633-644). New uniform follows that (locate via
  `glGetUniformLocation` at program build, set after glUseProgram). NOT the
  deferred setFloat/apply() cache.
- Add `uniform float u_visualDisplace;` (default 0). Original path:
  `h` stays `heights[...]` (the coarse SSBO). When `u_visualDisplace == 0.0` the
  vert must take the EXACT original code path (same `h`, same index math) →
  byte-identical. Gate the finer-grid index/sample behind `if
  (u_visualDisplace > 0.0)`.
- CPU force-zero u_visualDisplace when env gate (e.g. MC2_TERRAIN_VISUAL_DISPLACE)
  unset — mirrors `useTerrainNormalsFromHeight` force-zero discipline
  (terrain_height_normal.hglsl:13-15). No `#version` in file; prefix via
  makeProgram. `uniform float` (NOT uniform uint).
- Same gate must zero the shadow-caster displacement (§3) and the normal-source
  factor (§2) so all three move together — a partial gate = desync.

## Caveats / verify-before-build
- Confirm depth-FUDGE regime (forward vs reverse-Z) the live build actually
  compiles; the lod_chunk file const text disagrees with the shared include.
- Confirm terrain_lod_chunk.frag's normal source (unit-11 helper vs vertex
  normal) before assuming §2 covers the LOD-chunk lighting.
- 4x geometry = 4x emitted verts on BOTH main patch-gen and shadow emit; the
  shadow 59994 CHUNK/16-bit cap is the real scaling constraint.
