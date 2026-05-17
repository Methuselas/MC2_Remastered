# Water Material v1 - GPU-Driven Design (terrain-side thickness)

**Date:** 2026-05-17
**Branch:** `claude/gpu-driven-rendering` (worktree `.claude/worktrees/gpu-driven-rendering/`)
**Status:** READY FOR ADVERSARIAL RE-REVIEW (rev 2 - depth-buffer mechanic replaced)
**Supersedes (for this branch):** `docs/superpowers/specs/2026-05-17-water-material-v1-design.md`
**North star:** eliminate per-frame CPU work; bake to UBO; 100% GPU-resident draw

---

## 1. Goal and non-goals

**Goal.** Replace flat textured water on the armed MDI path with a stylized
material: depth-tinted absorption color, Fresnel rim, procedural sine-wave
normals (specular + normal variation), and a continuous depth-faded shoreline
replacing the 3-band per-vertex alpha staircase. Water thickness is derived
**entirely from terrain-side elevation data already GPU-resident**, not from
the depth buffer.

**Non-goals (deferred; not this spec):** screen-space refraction; planar
reflection (v1 uses a sky/fog-color approximation for the reflective term);
flow-map / sampled normal maps; per-biome / mod-configurable water style.

## 2. Why rev 2 (the depth-buffer mechanic was killed in review)

Two independent code-grounded adversarial reviews BLOCKED rev 1's
depth-buffer thickness mechanic with convergent CRITICALs:

- The armed MDI water VS carries a screen-space-z double-projection + a
  constant `WATER_DEPTH_FUDGE_FAST = +0.003` (`gos_terrain_water_fast_mdi.vert`
  around lines 270-281). The opaque terrain that writes `sceneDepthTex` does
  not project depth through that same chain, so `gl_FragCoord.z(water)` vs
  `sceneDepthTex.r(terrain)` mixed two non-linear encodings - `depthDelta` was
  not a valid open-water thickness.
- Sampling `sceneDepthTex` (a depth attachment of the live `sceneFBO_`) while
  it is bound for depth-test is contradicted by the in-repo shadow-unbind
  precedent in `docs/amd-driver-rules.md` (requires unbind, not just
  depth-write-off).

Rev 2 removes the depth-buffer entirely. Both CRITICALs are **designed out**:
no `sceneDepthTex` sample exists, so neither the projection mismatch nor the
feedback hazard can occur.

## 3. The mechanic: terrain-side thickness (grounded)

All symbols below are grep-verified on this worktree; line numbers are
starting points (Rule 0 - grep the symbol at implementation time).

- **Terrain-floor elevation under each water corner:** `velev =
  cornerElev(rec, cornerIdx)` (`gos_terrain_water_fast_mdi.vert` ~line 191),
  selecting one of `WaterRecipe.elev.{x,y,z,w}` (struct ~line 25, SSBO binding
  5 `WaterRecipeBuf`). Populated **once at mission load** in `WaterStream::Build`
  (`GameOS/gameos/gos_terrain_water_stream.cpp` ~lines 285-288, `r.v0e =
  p0.elevation` etc. from the terrain heightmap `PostcompVertex::elevation`).
  Mission-static SSBO - zero per-frame CPU.
- **Water-plane surface elevation:** `uniform float waterElevation`
  (`gos_terrain_water_fast_mdi.vert` ~line 67), sourced from
  `Terrain::waterElevation = mapData->waterDepth` (`mclib/terrain.cpp` ~lines
  131, 2072), already pushed to `s_waterMdiProg` (`gameos_graphics.cpp` ~line
  2304 `setMF("waterElevation", ...)`). Mission-static value.

**Continuous thickness (computed in the VS, emitted as one new varying):**

```glsl
// VS, alongside the existing worldPos / velev computation:
float waterThickness = max(0.0, waterElevation - velev);  // world units, >=0
out float WaterThickness;   // new varying (see Section 6 for the full out list)
...
WaterThickness = waterThickness;
```

This is the SAME `velev` the existing `elevAlphaBandByte(velev)`
(`gos_terrain_water_fast_mdi.vert` ~lines 132-139) consumes - we replace the
3-band staircase with this continuous value. No depth buffer, no projection,
no near/far, no feedback loop. Physically: vertical water column at that
corner. The wave-bob term (`waveOurCos(...)`) is intentionally excluded from
thickness (thickness tracks the static plane, not the surface ripple) -
documented so it is not "fixed" later.

**Land-quad guard (real residual, not a blocker):** the recipe SSBO includes
land quads where `velev > waterElevation` (`gos_terrain_water_stream.cpp`
~lines 243-264). `max(0.0, ...)` clamps thickness to 0 there; the existing
per-triangle pz-gate (`WaterThinRecord.flags`,
`gos_terrain_water_stream.h` ~lines 93-104) already culls non-visible water at
draw time. v1 relies on both; no new cull logic.

## 4. Shading (FS: gos_terrain_water_mdi.frag)

`t = clamp(WaterThickness * absorptionDensity, 0.0, 1.0)` (world-unit
thickness scaled by a tuned UBO constant - now in real world units, so the
constant is physically interpretable, unlike rev 1's clip-depth fudge).

- **Absorption color:** `waterCol = mix(shallowColor, deepColor, t)`.
- **Shoreline alpha:** `shore = smoothstep(0.0, shoreBlendDepth,
  WaterThickness)`. This REPLACES the elevation-band staircase as the sole
  alpha driver (see Section 5 for the composite - the staircase is removed,
  not multiplied in).
- **Procedural normals:** two summed sine waves ->
  `wN = normalize(vec3(w * normalStrength, 1.0))` driven by `time` (already
  pushed per-frame for this program - reuse, do not remove).
- **Fresnel:** `viewDir = normalize(cameraPos - WorldPos);
  fres = fresnelF0 + (1-fresnelF0)*pow(1 - max(dot(wN,viewDir),0), 5)`.
  `WorldPos` is a new VS->FS varying (the VS already computes `worldPos =
  vec3(vxy, wz)`); `cameraPos` is NOT currently visible to the water MDI
  program (confirmed: no shared SceneData UBO bound to `s_waterMdiProg`) - it
  is added as ONE new per-frame `setMVec4` (Section 7). Light direction stays
  the FS's existing constant for v1 specular (zero new cost; physical
  sun-color deferred).
- **Reflective term:** `reflCol = fog_color.rgb * 1.4` (sky/fog approx;
  `fog_color` already available). No reflection pass in v1.
- **Specular:** `pow(max(dot(wN, halfV),0),64) * fres`.

## 5. Composite (corrected per review CRITICAL-2)

Rev 1 multiplied the absorption color by the tile diffuse (undoing the
stylized look) and multiplied the smooth shore by the staircase alpha
(preserving the artifact). Corrected:

```glsl
// detail/spray pass is discriminated by the EXISTING flat varying o_isWater
// (1 = base water, 2 = detail/spray; gos_terrain_water_fast_mdi.vert ~line 150
//  from WaterPerCmd.isWater). It is NOT WaterPerCmd.detailMode - that SSBO
// field is not visible to the FS. Detail/spray keeps its current path:
if (o_isWater == 2) { /* unchanged existing detail/spray branch */ }
else {
    vec3  col = mix(waterCol, reflCol, fres);   // absorption, NOT * tile diffuse
    col      *= vertexLightRGB;                  // VS light tint (decode from Color.rgb)
    col      += sunIntensity * spec;
    col       = mix(fog_color.rgb, col, FogValue);   // exact existing fog order
    FragColor = vec4(col, shore);                // shore is the SOLE alpha; staircase removed
}
```

The per-vertex `elevAlphaBandByte` path and the `alphaEdge/Middle/DeepByte`
uniforms become dead for the base layer and are removed in the same commit
(no half-retired state). `vertexLightRGB` is the existing per-corner light
already packed into the `Color` varying - decoded, not a new input.

## 6. VS/FS interface change

Current MDI pair varyings (exact, grep-verified): VS `out vec4 Color; out
vec2 Texcoord; out float FogValue; flat out int o_isWater;` <-> FS matching
`in`. Rev 2 adds exactly two, in BOTH shaders in one atomic edit (hot-reload
fails silently if only one side reloads - relink and check console):

```
out float WaterThickness;  /  in PREC float WaterThickness;
out vec3  WorldPos;         /  in PREC vec3  WorldPos;
```

## 7. Files changed

```
MODIFIED  shaders/gos_terrain_water_fast_mdi.vert  -- compute WaterThickness; emit WaterThickness + WorldPos;
                                                      remove elevAlphaBandByte band logic from base path
MODIFIED  shaders/gos_terrain_water_mdi.frag       -- material/composite (Section 4-5);
                                                      o_isWater==2 detail branch preserved verbatim
MODIFIED  GameOS/gameos/gameos_graphics.cpp        -- WaterStyle UBO (write-once) bound to s_waterMdiProg;
                                                      one new per-frame setMVec4("cameraPos", terrain_camera_pos_)
                                                      in the existing MDI lambda block; drop now-dead
                                                      alphaEdge/Middle/Deep uniform pushes for the base path
```

No new files. No depth-texture binding. Legacy `water_fast_prog_` /
`gos_terrain_water_fast.vert` / `gos_tex_vertex.frag` untouched (un-armed
users keep current flat water).

## 8. WaterStyle UBO (the "bake" target)

std140, single global, populated **once** at init (not per frame), bound to
`s_waterMdiProg`:

```glsl
layout(std140) uniform WaterStyle {
    vec3  shallowColor;      float absorptionDensity;  // 16B slot
    vec3  deepColor;         float shoreBlendDepth;     // 16B slot (world units)
    float normalStrength;    float fresnelF0;
    float sunIntensity;      float _pad;
};
```

The vec3+float interleave is a valid std140 16-byte packing; the C++ struct
MUST be built from `float[3]+float` (not `glm::vec3`) to match offsets - a
lockstep note for the plan. Binding-point index is assigned at plan stage to
avoid collision with the water program's existing SSBO bindings 5/6/7 and
UBO binding 1 (`SceneData`); routed to `mc2-terrain-indirect-expert`
(Section 12).

## 9. Per-frame budget

| Item | Rev 1 claim | Rev 2 actual |
|---|---|---|
| Material constants | UBO write-once | UBO write-once |
| Thickness input | `sceneDepthTex` fetch/frag | static recipe SSBO (already bound) |
| `cameraPos` (Fresnel) | "reuse" (was false) | 1 new `setMVec4`/frame (honest) |
| light dir | - | FS constant (zero cost, v1) |
| `inverseScreenSize` | textureSize | none (no screen-space term at all) |
| reflection / refraction | cut | cut |
| **Net new per-frame CPU** | claimed zero | **one `glUniform4fv`** |
| **Net new per-frame GPU ops** | 1 depth fetch/frag | **zero** (no new texture fetch) |

The single `glUniform4fv("cameraPos")` is the honest, irreducible
per-frame cost of a view-dependent Fresnel; it is one uniform write, no CPU
compute, no bake-able alternative without baking the camera (impossible). The
north-star intent (no per-frame CPU *work* / no re-derivation / bake what is
static) is met; this is acknowledged explicitly rather than hidden.

## 10. Load-bearing constraints (do not regress)

- Z-bias invariant: water `WATER_DEPTH_FUDGE_FAST = +0.003`, terrain `+0.002`,
  delta `0.001` (`shaders/include/terrain_depth_bias.hglsl`). Untouched (rev 2
  does not read or alter depth).
- `[WATER_DEPTHPROBE v2]` (`MC2_WATER_DEPTHPROBE`) - untouched.
- MVP-consistency / un-armed guard / two-draw structure - untouched.
- `o_isWater` discriminator (1 base / 2 detail) - the detail/spray branch is
  preserved verbatim; only the base branch composite changes.
- Recipe SSBO C++/GLSL std430 lockstep (`WaterRecipe` static_assert 64 B,
  `gos_terrain_water_stream.h` ~line 73) - not modified, but any field add is
  forbidden in v1 (we only READ existing `.elev`).

## 11. Gates

Build `--config RelWithDebInfo`, full relink (gameos_graphics.cpp changes),
deploy per-file `cp -f` + `diff -q`, then:

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing
```

Absolute path (CLAUDE.md). Smoke PASS alone does not verify a shader change
(silent-fallback rule): require BOTH the run on the freshly-deployed exe AND
a positive marker. Add an env-gated `[WATER v1]` probe logging the
per-session `WaterThickness` min/max once (gated on `MC2_WATER_DEPTHPROBE`,
reusing the existing instrument's env); a PASS requires `max > 0` (proves the
recipe elevation path is live, not a flat unbound read).

Manual visual (tier1 necessary, not sufficient):
- Continuous shoreline fade, no 3-band staircase, no waterline flicker.
- Deep water darker than shallow (absorption over real thickness).
- Cinematic low angle: Fresnel rim brighter at grazing incidence.
- Detail/spray (`o_isWater==2`) visually unchanged.
- Un-armed intro pan: legacy flat water unchanged.
- `MC2_WATER_DEPTHPROBE=1`: equal=1 on motion frames (MVP-consistency).
- Zoomed-out big-map (known blind stress path): Tracy GPU water-zone cost
  not > 2x current MDI water FS; no draw/cull/LOD change so draw volume
  structurally cannot regress, but the FS is heavier per fragment - measure,
  do not assume.

## 12. Plan-stage advisor routing

- `mc2-shader-expert`: WaterStyle std140 packing + C++ lockstep struct;
  `absorptionDensity` / `shoreBlendDepth` tuning in world units; `vertexLightRGB`
  decode from the existing `Color` varying; confirm removing the base-path
  band uniforms does not break the `o_isWater==2` detail branch.
- `mc2-terrain-indirect-expert` (cross): WaterStyle UBO binding-point index
  vs. the water MDI program's existing SSBO 5/6/7 + UBO 1 bindings.
