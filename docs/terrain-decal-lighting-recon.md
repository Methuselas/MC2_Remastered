# Terrain Decal/Overlay Lighting Recon (TERRAIN-DECAL-LIGHTING-RECON-0)

Recon-only artifact. **No implementation in this slice.** Determines
whether the legacy terrain "decal" / transition-tile overlay layer can
be made to share the geometry-fixed terrain lighting stack
(`MC2_TERRAIN_NORMALS_FROM_HEIGHT` + `MC2_TERRAIN_LIGHTING_V1` +
`MC2_TERRAIN_LIGHTING_V2`).

Motivation: with the new terrain lighting visible, the cement/road
transition overlays still read with flat-up normals + no
hemisphere fill, so the seam between lit terrain and unlit overlay
gets more obvious as V1/V2 strength rises.

Recon target shaders (all in `shaders/`):

| Shader | Purpose | Candidate for sharing? |
|---|---|---|
| `terrain_overlay.frag` | Cement perimeter / transition / road tiles | **YES — high confidence** |
| `decal.frag` | Bomb craters, mech footprints | NO — different lighting intent (defer) |
| `terrain_overlay.vert` | Vertex shader shared by both overlay batches | Reuse without edit (already passes WorldPos) |
| `overlay_alpha_clear.frag` | GBuffer1 cleanup fullscreen post-pass | Out of scope — no lighting |

## 1. Authority chain

### Overlay (cement transitions)
- **CPU bind site:** `gosRenderer::drawTerrainOverlays()` —
  `GameOS/gameos/gameos_graphics.cpp:7761-7816`
- **Program:** `overlayProg_` compiled from `terrain_overlay.vert` +
  `terrain_overlay.frag` (gameos_graphics.cpp:7782)
- **Uniform setup:** `uploadOverlayUniforms_()` (line 7789) →
  `setupOverlayShadowsForShp()` (line 7754) currently uploads:
  `terrainLightDir`, `lightSpaceMatrix`, `enableShadows`,
  `shadowSoftness`, `dynamicLightSpaceMatrix`, `enableDynamicShadows`
  (uniforms looked up dynamically per call at lines 7679-7727).
- **GL state:** depth-test ON / `GL_GEQUAL` reverse-Z, depth-write
  ON, blend DISABLED, cull OFF.
- **Draw order** (`mclib/txmmgr.cpp:2195-2237`): after main terrain,
  before decals + before legacy overlays:
  ```
  1. Render.Terrain (tessellated patches; gos_terrain.frag)
  2. Render.TerrainOverlays                ← terrain_overlay.frag
  3. Render.TerrainOverlaysStatic
  4. Render.TerrainMines
  5. Render.Decals                          ← decal.frag
  6. Render.Overlays (legacy VFX/markers)
  ```

### Decal (craters / footprints)
- **CPU bind site:** `gosRenderer::drawDecals()` —
  `gameos_graphics.cpp:7939-7995`
- **Program:** `decalProg_` from `terrain_overlay.vert` (shared)
  + `decal.frag` (gameos_graphics.cpp:7960)
- **Uniform setup:** Same `uploadOverlayUniforms_()` +
  `setupOverlayShadowsForShp()`.
- **GL state:** depth-test ON `GEQUAL`, depth-write **OFF**, blend
  `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` (alpha-blended on top of terrain),
  cull OFF.
- **Lighting model:** intentionally narrow shadow + cloud range
  (0.88..1.0) so authored crater darkening stays visible.

## 2. Available shader inputs (today)

For both `terrain_overlay.frag` and `decal.frag`:

| Input | Present | Notes |
|---|---|---|
| `WorldPos.xy` (worldspace) | ✓ | Set by `terrain_overlay.vert`; needed to sample `terrainHeightTex` |
| Surface normal | ✗ | Both hardcode `vec3(0,0,1)` flat-up at shadow sampling |
| Terrain UV | ✗ | Both use their own per-overlay `Texcoord`; not the colormap UV |
| Shadow factor | ✓ | `calcShadow` + `calcDynamicShadow` (existing PCF) |
| Fog | ✓ | `FogValue` + `fog_color` uniform |
| Tile/layer ID | ✗ | Not exposed |

**Key:** `WorldPos.xy` is the only field needed to feed
`computeTerrainNormalFromHeight()`. No vertex shader changes
required.

## 3. Lighting uniforms currently MISSING from overlay/decal bind

The bind site `setupOverlayShadowsForShp()` uploads sun + shadow
uniforms but NOT the new terrain lighting family:

- `terrainHeightTex` (sampler unit 11)
- `terrainHeightParams` (vec4)
- `useTerrainNormalsFromHeight` (int)
- `terrainNormalsFromHeightStrength` (float)
- `terrainLightingV1Strength` (float)
- `terrainLightingV2ShadowFillFloor` (float)

These are uploaded by the shared helper `bindTerrainHeightTexUniforms()`
(`gameos_graphics.cpp` declared static, called from all 3 terrain
upload sites). The helper already handles env-gate force-zero
semantics — a single additional call from `uploadOverlayUniforms_()`
would extend the same authority to the overlay program with zero
new env logic.

## 4. Sharing feasibility per shader

### A. `terrain_overlay.frag` (cement transitions) — RECOMMENDED

Classification: **A — terrain transition overlay → share terrain lighting**

- Has WorldPos.xy → height-tex sampling works.
- Currently uses hardcoded `vec3(0,0,1)` for shadow sampling
  (lines 91-92). Substitute height-derived normal when gated.
- Add V1 hemisphere additive + V2 shadow-aware floor with the
  same expression copied from `gos_terrain.frag:780-846`. Snow
  damping not relevant (cement doesn't snow); set snowWeight=0
  inline.
- Same gate semantics — when env gates OFF, the upload site
  force-zeroes the strength/factor uniforms so the shader branches
  short-circuit → byte-identical legacy output.
- Confidence: **HIGH**. The overlay surface IS terrain semantically;
  this just removes the lighting-model seam that V1/V2 introduced.

### B. `decal.frag` (craters / footprints) — DEFER

Classification: **B — independent decal pass → only approximate lighting**

- WorldPos.xy is present so it COULD share, technically.
- But the lighting model is intentionally narrow: craters use a
  preserved-darkness 0.88..1.0 shadow range to keep authored crater
  shading visible. Stacking hemisphere fill or height-derived
  normals on top risks brightening crater interiors and losing the
  depth cue.
- Recommend: leave `decal.frag` as-is. Revisit only if post-ship
  feedback specifically calls out flat crater shading under V1/V2.
- Confidence: HIGH that deferral is the right call.

### C. `overlay_alpha_clear.frag` — out of scope

GBuffer1 cleanup post-pass only; no shading. No changes warranted.

## 5. Recommended implementation slice: TERRAIN-DECAL-LIGHTING-1

If/when authorized, the slice would:

**Files to touch (≤ 3):**
1. `shaders/terrain_overlay.frag` — add 6 uniform decls, port the
   `computeTerrainNormalFromHeight()` helper inline (or as a shared
   include), replace `vec3(0,0,1)` normal with gated height-derived
   normal at the existing shadow sampling site, add the V1
   hemisphere additive block + V2 floor modulation (copy from
   `gos_terrain.frag`).
2. `GameOS/gameos/gameos_graphics.cpp` — `uploadOverlayUniforms_()`
   gains a call to `bindTerrainHeightTexUniforms()` after the
   existing `setupOverlayShadowsForShp()` call. The helper's
   GLint params point at new overlay-locs members cached at compile
   time alongside the existing overlay shadow locs.
3. `tools/shader_reflect/expected/shaders__terrain_overlay.frag__default.json`
   — refresh after the uniform additions.

**No new env var.** Slice reuses the existing 3-gate family.
**No new C-API.** Reuses `bindTerrainHeightTexUniforms()`.
**No new ImGui slider.** The 3 sliders in Graphics Options > Terrain
Tuning automatically affect the overlay path once the uniforms are
bound to its program.

**shader_reflect goldens WILL drift** — refresh
`tools/shader_reflect/expected/shaders__terrain_overlay.frag__default.json`
as part of the slice. ~6 new entries in the default-UBO members
list at offsets immediately after the existing overlay uniforms.

**Helper extension question:** factor the height-normal compute
out of `gos_terrain.frag` into `shaders/include/terrain_height_normal.hglsl`
so both terrain.frag and terrain_overlay.frag can `#include` it.
Avoids drift between the two implementations. Adds 1 file. Optional
but recommended for the slice.

## 6. Validation / capture plan

Per-slice gates:

| Check | Detail |
|---|---|
| Build | RelWithDebInfo |
| shader_reflect | `--shader shaders/terrain_overlay.frag --update`; verify 77/77 after |
| env_registry | No changes (no new env vars) |
| Tier1 default | 5/5 PASS with no terrain env vars set (byte-equivalence) |
| Single-mission gate ON | mc2_03 (cement-overlay heavy) with NFH+V1+V2 |
| Capture matrix | terrain_salvage_03 with: default / V1-only / V1+V2; same env-gated as terrain main path |
| Visual check | Cement edges should no longer show a lighting seam against adjacent terrain when V1+V2 are ON |
| Frame-hash regression | None expected at default-OFF — no draws added |

Optional but valuable:

- **A/B comparison capture** at mc2_03 with overlay-only debug
  (no terrain) vs terrain-only — confirms the overlay now matches
  the terrain shading model.
- **Sun-angle sweep** (mid + late time-of-day) since the
  height-derived normal interacts with `terrainLightDir`; mismatch
  would be most visible at low sun angles.

## 7. Risks

1. **Lighting double-count:** the existing
   `setupOverlayShadowsForShp()` already applies sun+shadow on the
   overlay. Adding V1 hemi additive on top is the same architecture
   as the terrain path uses, so no double-count risk if the same
   expression is copied verbatim. **Confidence: HIGH** (mirrors
   gos_terrain.frag, which already does this correctly post-V2).
2. **Edge-of-map sampling:** `terrainHeightParams.x>0.5` guard +
   CLAMP_TO_EDGE on the height texture handle this safely (proven
   by the terrain main path soak).
3. **Performance:** 4 texelFetches per overlay fragment is
   negligible; the overlay batch is small fragment count compared
   to terrain.
4. **GBuffer1 path:** overlay already writes
   `rc_gbuffer1_shadowHandled_*` flags; no MRT changes needed.

## 8. Recommendation (action: defer to user)

- **TERRAIN-DECAL-LIGHTING-1a** (cement transition overlay) —
  recommend AUTHORIZE. Small surgical slice (≤ 3 files, no new
  uniforms in the env-gate family, no new C-API, no new ImGui).
  Closes the visible seam the V1/V2 work introduced.
  Confidence: **HIGH**.
- **TERRAIN-DECAL-LIGHTING-1b** (decal/craters) — recommend
  DEFER. Different lighting intent; risk of flattening crater
  depth cue without clear benefit. Revisit only if post-ship
  feedback identifies it as a concrete problem.

Neither shipped this slice — recon only per spec. If you authorize
1a, the next slice is one diff-ready → reviewer → commit cycle
following the same pattern as TERRAIN-LIGHTING-1 / -2.
