# Water Material v1 - GPU-Driven Design (terrain-side thickness)

**Date:** 2026-05-17
**Branch:** `claude/gpu-driven-rendering` (worktree `.claude/worktrees/gpu-driven-rendering/`)
**Status:** READY FOR IMPLEMENTATION PLAN (rev 3 - 2 adversarial rounds cleared)
**Supersedes (for this branch):** `docs/superpowers/specs/2026-05-17-water-material-v1-design.md`
**North star:** eliminate per-frame CPU work; 100% GPU-resident draw; modern-when-touched

---

## 1. Goal and non-goals

**Goal.** Replace flat textured water on the armed MDI path with a stylized
material: depth-tinted absorption color, Fresnel rim, procedural sine-wave
normals (specular + normal variation), and a continuous depth-faded shoreline
replacing the 3-band per-vertex alpha staircase **on the base water layer
only**. Water thickness is derived entirely from terrain-side elevation data
already GPU-resident; no depth-buffer read.

**Non-goals (deferred; not this spec):** screen-space refraction; planar
reflection (v1 uses a sky/fog-color approximation for the reflective term);
flow-map / sampled normal maps; per-biome / mod-configurable water style
(this is the explicit trigger that later promotes the v1 compile-time
constants to a UBO - see Section 8).

## 2. Revision history (why rev 3)

Rev 1 (depth-buffer thickness) was BLOCKed by two adversarial reviews:
water/terrain projection mismatch made `gl_FragCoord.z` vs `sceneDepthTex.r`
meaningless, and sampling the bound depth attachment is an AMD feedback hazard
the in-repo shadow-unbind precedent forbids. Rev 2 replaced the mechanic with
terrain-side elevation; two further adversarial reviews **confirmed the core
mechanic sound** (`velev` is genuinely the terrain floor; recipe SSBO
mission-static and already bound) but BLOCKed on shared-program side effects
and understated infra. Rev 3 folds every rev-2 finding:

- C1 (both reviewers): `s_waterMdiProg` serves base AND detail draws with one
  uniform set; the rev-2 "drop base-path band uniforms" instruction would
  destroy the detail layer's alpha. Rev 3 keeps all band uniforms/function/C++
  pushes intact and changes ONLY the base VS branch's alpha source.
- C2 (Sonnet): no UBO-binding plumbing exists on the raw `s_waterMdiProg`.
  Rev 3 uses compile-time FS constants (decision: a UBO is infra ahead of
  need for a single non-shared static style; promote at per-biome).
- M1 (Opus): explicit floor-vs-surface Z invariant added (Section 3).
- M2 (Opus/Sonnet): land-quad guard understated + invisible-land FS overdraw;
  rev 3 adds an explicit `discard` and honest perf wording.
- MAJOR-1 (Sonnet): probe env collision; rev 3 uses a distinct env.
- MAJOR-3 (Sonnet): `setMVec4` needs `(const float*)&` cast.
- MAJOR-4 (Sonnet): C++/GLSL `WaterRecipe` offset-aliasing documented.

All cited symbols grep-verified on this worktree; line numbers are starting
points (Rule 0 - grep the symbol at implementation time).

## 3. The mechanic: terrain-side thickness

**Terrain-floor elevation** under each water corner: `velev =
cornerElev(rec, cornerIdx)` (`shaders/gos_terrain_water_fast_mdi.vert` ~191),
selecting one of the GLSL `WaterRecipe.elev.{x,y,z,w}` (struct ~22-27, SSBO
binding 5 `WaterRecipeBuf`). Populated **once at mission load** in
`WaterStream::Build` (`GameOS/gameos/gos_terrain_water_stream.cpp` ~285-288,
`r.v0e = p0.elevation` etc. from terrain heightmap
`PostcompVertex::elevation`). Mission-static SSBO; zero per-frame CPU.

> std430 layout note: the C++ `WaterRecipe` (`gos_terrain_water_stream.h`
> ~49-72) declares `float v0e,v1e,v2e,v3e` as four scalars; the GLSL struct
> aliases the same 16-byte offset as `vec4 elev`. They are NOT the same type -
> they are offset-identical under std430, locked by
> `static_assert(sizeof(WaterRecipe)==64)` (~73). v1 only READS `.elev`; no
> field add is permitted (would break the alias).

**Water-plane surface elevation:** `uniform float waterElevation`
(`gos_terrain_water_fast_mdi.vert` ~67), from `Terrain::waterElevation =
mapData->waterDepth` (`mclib/terrain.cpp` ~131, ~2072), already pushed to
`s_waterMdiProg` (`gameos_graphics.cpp` ~2304). Mission-static value.

**Continuous thickness, computed in the VS, emitted as a new varying:**

```glsl
float waterThickness = max(0.0, waterElevation - velev);  // world units, >= 0
out float WaterThickness;
WaterThickness = waterThickness;
```

**LOAD-BEARING Z INVARIANT (rev-2 M1 - the exact rev-1 trap class).**
Two different Z values exist in this VS and MUST NOT be unified:
- `velev` = terrain FLOOR elevation (recipe `.elev`). Thickness numerator. ONLY input to `waterThickness`.
- `worldPos.z` = `waveOurCos(...) + waterElevation` = wave-displaced water SURFACE (`gos_terrain_water_fast_mdi.vert` ~194-195). Emitted as the `WorldPos` varying and used ONLY for the Fresnel view vector.
The wave-bob term is intentionally absent from thickness (thickness tracks the
static plane, not surface ripple). Do not "fix" this; do not derive thickness
from `worldPos.z`.

**Land-quad behavior (rev-2 M2 - stated honestly, not as a no-op).** The
recipe SSBO includes quads where the water plane projects on-screen even when
all corners are land (`gos_terrain_water_stream.cpp` ~243-264); the
per-triangle pz-gate (`WaterThinRecord.flags`) does NOT cull these. For any
corner with `velev >= waterElevation`, `waterThickness = 0`, so
`shore = smoothstep(0, shoreBlendDepth, 0) = 0` and the fragment is fully
transparent. This is a deliberate consequence of replacing the staircase:
the legacy faint `alphaEdgeByte` edge-water over slightly-raised shoreline
land is removed along with the staircase it was part of (intended - the
continuous fade is the replacement). To eliminate the now-invisible-fragment
FS overdraw, the base branch does `if (shore <= 0.0) discard;` (Section 5).

## 4. Shading (FS: gos_terrain_water_mdi.frag, base branch only)

`t = clamp(WaterThickness * ABSORPTION_DENSITY, 0.0, 1.0)` - world-unit
thickness scaled by a compile-time constant (Section 8); physically
interpretable units, unlike rev-1's clip-depth fudge.

- **Absorption color:** `waterCol = mix(SHALLOW_COLOR, DEEP_COLOR, t)`.
- **Shoreline alpha:** `shore = smoothstep(0.0, SHORE_BLEND_DEPTH, WaterThickness)` - the SOLE base-layer alpha (replaces the staircase; see Section 5).
- **Procedural normals:** two summed sine waves -> `wN = normalize(vec3(w * NORMAL_STRENGTH, 1.0))`, driven by `time` (already pushed per-frame for this program - reuse, do not remove).
- **Fresnel:** `viewDir = normalize(cameraPos - WorldPos); fres = FRESNEL_F0 + (1-FRESNEL_F0)*pow(1 - max(dot(wN,viewDir),0), 5)`. `cameraPos` is added as ONE new per-frame uniform (Section 7); `WorldPos` is the new surface-position varying.
- **Reflective term:** `reflCol = fog_color.rgb * 1.4` (sky/fog approx; `fog_color` already available). No reflection pass in v1.
- **Specular:** `pow(max(dot(wN, halfV),0),64) * fres`, `halfV = normalize(viewDir + LIGHT_DIR)`. `LIGHT_DIR` stays the FS's existing constant light vector for v1 (zero new cost; physical sun-color deferred).

## 5. Composite and the shared-program correction (rev-2 C1)

`s_waterMdiProg` is ONE program serving both the base draw
(`cmds[0].isWater=1`, `detailMode=0`) and the detail/spray draw
(`cmds[1].isWater=2`, `detailMode=1`) via a single `glMultiDrawArraysIndirect`
(`gameos_graphics.cpp` ~2264-2266, ~2345) with one uniform set.
`elevAlphaBandByte(velev)` (`gos_terrain_water_fast_mdi.vert` ~132-139) is
called unconditionally (~229) and the detail branch packs `elevAlphaByte`
directly as its alpha (~234-235).

**Therefore the band uniforms (`alphaEdge/Middle/DeepByte`, `alphaDepth`),
the `elevAlphaBandByte` function, and their C++ pushes
(`gameos_graphics.cpp` ~2306-2308) are ALL KEPT.** The ONLY VS change is the
base branch's alpha source:

```glsl
uint elevAlphaByte = elevAlphaBandByte(velev);   // UNCHANGED - detail path still needs it
uint argb;
if (detailMode == 0) {                            // BASE: alpha now owned by FS shore term
    uint lrgb = cornerLightRGB(trec, cornerIdx);
    argb = (lrgb & 0x00FFFFFFu) | 0xFF000000u;    // was (elevAlphaByte<<24); now opaque sentinel
} else {                                          // DETAIL/SPRAY: BYTE-FOR-BYTE UNCHANGED
    argb = (elevAlphaByte << 24) | 0x00FFFFFFu;
}
```

The detail/spray branch and every band uniform are preserved verbatim;
"detail preserved verbatim" is now literally true. The base layer's alpha
moves entirely to the FS `shore` term.

FS base branch (`o_isWater == 1`; detail `o_isWater == 2` branch unchanged -
the discriminator is `WaterPerCmd.isWater` via VS ~150, FS-visible flat int,
NOT the FS-invisible `detailMode` SSBO field):

```glsl
if (o_isWater != 1) { /* existing detail/spray branch, unchanged */ }
else {
    // ... wN, t, waterCol, fres, spec, reflCol as Section 4 ...
    float shore = smoothstep(0.0, SHORE_BLEND_DEPTH, WaterThickness);
    if (shore <= 0.0) discard;                         // kill invisible-land overdraw
    vec3 vertexLightRGB = Color.bgra.rgb;              // double-swizzle: VS packs .bgra (~241), FS un-swizzles
    vec3 col = mix(waterCol, reflCol, fres);           // absorption, NOT * tile diffuse
    col     *= vertexLightRGB;
    col     += SUN_INTENSITY * spec;
    col      = mix(fog_color.rgb, col, FogValue);      // exact existing fog order/operands
    FragColor = vec4(col, shore);
}
```

(Fog: the current FS guards `mix` with `if(fog_color.x>0||...)`; v1 keeps that
guard verbatim around the `mix` line - omitted above for brevity, retained in
code, so no-fog missions are unaffected.)

## 6. VS/FS interface change

Current MDI varyings (exact, grep-verified): VS `out vec4 Color; out vec2
Texcoord; out float FogValue; flat out int o_isWater;` <-> matching FS `in`.
Add exactly two, in BOTH shaders, ONE atomic edit (hot-reload fails silently
if only one side reloads - relink and check console):

```
out float WaterThickness;  /  in PREC float WaterThickness;
out vec3  WorldPos;         /  in PREC vec3  WorldPos;
```

6 varyings total, far under the GL 4.3 minimum - no link/limit risk.

## 7. Files changed

```
MODIFIED  shaders/gos_terrain_water_fast_mdi.vert  -- compute+emit WaterThickness; emit WorldPos
                                                      (= existing worldPos surface); base-branch argb
                                                      alpha -> 0xFF sentinel. Detail branch + band
                                                      function/uniforms UNTOUCHED.
MODIFIED  shaders/gos_terrain_water_mdi.frag       -- base-branch material/composite (Sec 4-5) +
                                                      compile-time const block (Sec 8); o_isWater!=1
                                                      detail branch UNTOUCHED.
MODIFIED  GameOS/gameos/gameos_graphics.cpp        -- one new per-frame
                                                      setMVec4("cameraPos", (const float*)&terrain_camera_pos_)
                                                      in the existing MDI lambda block (~2300-2316).
                                                      NO uniform-push removals.
MODIFIED  GameOS/gameos/gos_terrain_water_stream.cpp -- env-gated [WATER_MAT v1] probe in
                                                      WaterStream::Build (Sec 9).
```

No new files. No depth-texture binding. No UBO. Legacy `water_fast_prog_` /
`gos_terrain_water_fast.vert` / `gos_tex_vertex.frag` untouched (un-armed
users keep current flat water; band uniforms still pushed so the un-armed
and detail paths are wholly unaffected).

## 8. Material constants - compile-time (the "bake")

The ~8 static style constants are GLSL compile-time `const` in
`gos_terrain_water_mdi.frag` (zero per-frame cost, zero binding infra, tuned
via shader hot-reload):

```glsl
const vec3  SHALLOW_COLOR     = vec3(0.22, 0.45, 0.38);
const vec3  DEEP_COLOR        = vec3(0.02, 0.08, 0.10);
const float ABSORPTION_DENSITY= 0.15;   // 1/world-units (plan: tune w/ shader-expert)
const float SHORE_BLEND_DEPTH = 3.0;    // world-units to full opacity (plan: tune)
const float NORMAL_STRENGTH   = 0.30;
const float FRESNEL_F0        = 0.02;
const float SUN_INTENSITY     = 1.0;
// LIGHT_DIR: keep the FS's existing constant light vector (do not add a new const)
```

```glsl
// TODO(water-v2): when per-biome / mod-configurable water style lands, promote
// this const block to a std140 (or GL4.3 std430) WaterStyle UBO bound to
// s_waterMdiProg. UBO infra (buffer alloc at program-compile site, glUniform-
// BlockBinding post-compile, glBindBufferBase at draw) is deferred until a
// second consumer or runtime config actually needs it - infra ahead of need
// is forbidden (vulkan_prep / minimal-touch). Binding-point assignment then
// must avoid the program's std430 SSBO bindings 5/6/7.
```

## 9. Instrumentation and gates

**Probe (distinct env - the retained `[WATER_DEPTHPROBE v2]` MVP instrument on
`MC2_WATER_DEPTHPROBE` must NOT be disturbed).** Add a CPU-side `[WATER_MAT
v1]` probe in `WaterStream::Build` after `g_recipes` is populated, gated on a
NEW env `MC2_WATER_MATERIAL_PROBE`: loop recipes, compute `thickness =
max(0.0f, waterElevation - min(v0e,v1e,v2e,v3e))`, track min/max, print once
per mission. PASS requires `max > 0` (proves the elevation path is live, not a
flat unbound read). Separate static gate bool, separate marker schema.

**Build/deploy:** `--config RelWithDebInfo`; full relink
(`gameos_graphics.cpp` + `gos_terrain_water_stream.cpp` change - rm
`build64/RelWithDebInfo/mc2.exe` + changed `.obj` or `--clean-first`); deploy
per-file `cp -f` + `diff -q` to `A:/Games/mc2-opengl/mc2-win64-v0.4/`. Smoke
runs the DEPLOYED exe - a PASS on a stale exe verifies nothing
(silent-fallback rule); deploy is load-bearing and ordered before smoke.

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\gpu-driven-rendering\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing
```

PASS = exit 0 on the freshly-deployed exe AND `[WATER_MAT v1] ... max=>0` in
artifacts.

**Manual visual (tier1 necessary, not sufficient):**
- Continuous shoreline fade; the 3-band staircase gone on the base layer; no waterline flicker.
- Deep water darker than shallow (absorption over real world-unit thickness).
- Cinematic low angle: Fresnel rim brighter at grazing incidence.
- Detail/spray (`o_isWater==2`) visually unchanged vs baseline.
- Un-armed intro pan: legacy flat water unchanged.
- `MC2_WATER_DEPTHPROBE=1`: equal=1 on motion frames (MVP-consistency intact, instrument undisturbed).
- **Zoomed-out big-map (known blind stress path):** the base FS is materially
  heavier (Fresnel pow5, 4 sin, specular pow64) and runs on every base-water
  fragment incl. clamped land before `discard`. Draw volume is unchanged but
  per-fragment cost is NOT - this is NOT "structurally cost-neutral". Gate:
  Tracy GPU water-zone at zoomed-out big-map must not exceed ~2x current MDI
  water FS; measure, do not assume.

## 10. Load-bearing constraints (do not regress)

- Z-bias invariant `WATER_DEPTH_FUDGE_FAST=+0.003` / terrain `+0.002` / delta `0.001` - untouched (rev 3 reads/writes no depth).
- `[WATER_DEPTHPROBE v2]` (`MC2_WATER_DEPTHPROBE`) - undisturbed; new probe on a separate env.
- MVP-consistency / un-armed guard / two-draw structure - untouched.
- All band uniforms + `elevAlphaBandByte` + their C++ pushes - KEPT (detail path consumer).
- `WaterRecipe` C++/GLSL std430 offset-alias + `static_assert(==64)` - not modified; no field add.
- `o_isWater` discriminator (1 base / 2 detail) - only the base branch composite changes.

## 11. Plan-stage advisor routing

- `mc2-shader-expert`: tune `ABSORPTION_DENSITY` / `SHORE_BLEND_DEPTH` in
  world units; confirm `Color.bgra.rgb` light-tint decode (VS double-swizzle
  ~241 + FS un-swizzle); confirm base-branch `argb` sentinel does not perturb
  the detail branch; verify the existing FS fog guard is preserved.
- `mc2-terrain-indirect-expert` (cross): confirm the base-branch VS edit and
  the new `cameraPos` uniform do not disturb the MDI dispatch / thin-record
  path or the `detailMode`-keyed argb logic.
