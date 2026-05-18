# Water v2 - Slice S1: Living Surface (continuous-space animated normal)
> SUPERSEDED BY AS-BUILT (2026-05-17): the rev2 sine+Fresnel design below was
> replaced via the user visual loop with a camera-independent dual-counter-
> scroll fBm surface. Authoritative as-built = commit 8ee5d12 + the v2-scope
> doc Section 3-4 (camera-independence ruling). Kept for review history.

**Date:** 2026-05-17
**Branch:** `claude/water-material-v1` (isolated; keep-as-is)
**Parent:** `docs/superpowers/specs/2026-05-17-water-v2-scope-and-decomposition.md`
**Status:** READY FOR ADVERSARIAL REVIEW (x2 opus|sonnet, adversarial-plan-review skill)

---

## 1. Goal / non-goals

**Goal.** Restore animated surface "life" that v1 deliberately dropped, WITHOUT
the tile-seam banding that forced v1 to ship flat. Derive the wave normal from
a **continuous world-space coordinate (`WorldPos.xy`)** instead of the
wrap-corrected discontinuous `Texcoord`. Feed the already-wired Fresnel +
specular. Preserve v1's hard-won camera-stability.

**Non-goals (still later slices):** screen-space refraction (S2), planar
reflection (S3), WaterStyle UBO (S5, demand-gated). No VS / C++ change, no new
uniform or varying, no new pass. Pure hot-reloadable `gos_terrain_water_mdi.frag`
edit (north-star: zero new per-frame CPU; `WorldPos` and `time` are already FS
inputs).

## 2. Why this is the right S1 (grounded)

v1's `gos_terrain_water_mdi.frag` base branch (`o_isWater == 1`) computes the
wave from `wuv = Texcoord * 6.2831853`. `Texcoord` carries the per-triangle
`MaxMinUV` wrap correction (VS `gos_terrain_water_fast_mdi.vert` ~lines
200-227): at tile/loop boundaries the UV is phase-discontinuous -> diagonal
bands "catching on the loops" (root-caused + visually confirmed in the v1
debug session). v1 shipped `NORMAL_STRENGTH = 0.0` (flat `wN`) and left the
exact reactivation seam with a `TODO(water-v2)` pointing here. `WorldPos` (FS
`in vec3`, the wave-displaced MC2-world surface position, set in VS at the
`WaterThickness`/`WorldPos` emit) is **globally continuous** - no `MaxMinUV`
wrap is applied to it - so a sine of `WorldPos.xy` has no seam by construction.

## 3. Design (FS-only, grounded against current code)

Current base-branch code (grep-verify exact lines at impl time - `wuv`, `w`,
`wN`, `NORMAL_STRENGTH`):

```glsl
const float NORMAL_STRENGTH    = 0.0;   // v1 flat (TODO water-v2 seam)
...
PREC vec2 wuv = Texcoord * 6.2831853;
PREC vec2 w = vec2(sin(wuv.y*3.0 + time*0.50) + sin(wuv.y*1.2 + time*0.355),
                   sin(wuv.x*3.0 + time*0.355) + sin(wuv.x*1.2 + time*0.50));
PREC vec3 wN = normalize(vec3(w * NORMAL_STRENGTH, 1.0));
```

Replace the phase source, re-enable strength, and add a **distance LOD fade**
(the rev-2 fix for both adversarial MAJORs: zoomed-out aliasing/shimmer and
specular fireflies). New consts (compile-time, hot-reloadable; principled
starts, then user visual tuning like v1):

```glsl
const float NORMAL_STRENGTH = 0.18;   // wave normal tilt (was 0.0 v1-flat). Low: camera-stable.
const float WAVE_FREQ       = 0.012;  // 1/world-u; wavelength ~520 (~4 terrain quads @128u/vtx):
                                      // macro swell, NOT sub-quad sparkle (adversarial F3).
const float WAVE_SPEED      = 0.6;    // world phase units/sec (calm drift; cf legacy frameCos pace)
const float SPEC_SCALE      = 0.5;    // specular intensity, DECOUPLED from NORMAL_STRENGTH (F2)
const float WAVE_FADE_NEAR  = 1500.0; // world-u: full wave life closer than this
const float WAVE_FADE_FAR   = 6000.0; // world-u: fully flat (v1 calm) beyond this
```

New wave (continuous; no `Texcoord`/UV-wrap = seam-free; distance-faded so the
zoomed-out big-map stress path stays the v1 calm/camera-stable look and only
near water is animated -> kills aliasing crawl AND broadens specular at range):

```glsl
// WorldPos.xy is global continuous MC2-world (no MaxMinUV wrap) -> seam-free.
// length(viewVec) is ALREADY computed at the viewDir guard line - reuse it.
PREC float waveLOD = 1.0 - smoothstep(WAVE_FADE_NEAR, WAVE_FADE_FAR, length(viewVec));
PREC vec2  p  = WorldPos.xy * WAVE_FREQ;
PREC vec2  w  = vec2(sin(p.y       + time*WAVE_SPEED)      + 0.5*sin(p.y*2.17 - time*WAVE_SPEED*0.7),
                     sin(p.x*1.13  - time*WAVE_SPEED*0.85) + 0.5*sin(p.x*2.31 + time*WAVE_SPEED*0.6));
PREC vec3  wN = normalize(vec3(w * (NORMAL_STRENGTH * waveLOD), 1.0));
```

And the specular term (already `spec = pow(max(dot(wN,halfV),0),64)*fres`)
gains the decoupled scale + the SAME LOD fade so far-water specular broadens
to nothing instead of per-pixel firefly flashing:

```glsl
// existing: PREC float spec = pow(max(dot(wN, halfV), 0.0), 64.0) * fres;
spec *= SPEC_SCALE * waveLOD;   // F2: decoupled intensity + distance fade (anti-firefly)
```

`waveLOD` reuses `length(viewVec)` already computed at the `viewDir` NaN-guard
(no new cost). At distance `waveLOD->0`: `wN->flat`, `spec->0` => provably
reduces to v1's user-approved flat calm water at the zoomed-out stress path
(camera-stability cannot regress there by construction). Near water gets the
animated life. Two octaves, irrational ratios (2.17/2.31/1.13) so no visible
repeat at view scale.

## 4. Load-bearing constraints (do not regress - adversarial focus)

- **Camera-stability (v1's hard-won property).** The animated `wN` re-adds a
  camera-dependent term via `fres` (which feeds BOTH the sky-mix and `spec` -
  adversarial F4: `FRESNEL_SKY_MAX` alone bounds only the sky wash, not the
  specular; `SPEC_SCALE`+`waveLOD` bound the specular). `FRESNEL_SKY_MAX=0.12`
  kept. The `waveLOD` distance fade makes camera-stability **un-regressable at
  the zoomed-out stress path by construction** (`waveLOD->0` => exact v1 flat
  state there). Near-camera, if grazing wash appears, lower `NORMAL_STRENGTH`
  and/or `FRESNEL_SKY_MAX` - never raise the sky mix.
- **Large-world-coord trig precision (corrected per adversarial).** MC2 map
  extent is bounded: `worldUnitsPerVertex=128`, max `realVerticesMapSide=120`
  (`mclib/terrain.cpp` ~95/318/395), so `|WorldPos.xy| <= 15360/2 = 7680`
  (NOT ~1e4). With `WAVE_FREQ=0.012`: max trig arg `7680*0.012 ~= 92` - well
  within `highp` fp32 (~7 sig digits); ULP ~ `92*2^-23 ~= 1e-5` rad, negligible
  jitter, no precision shimmer. No `fract`/`mod` needed. (Aliasing/shimmer
  from screen-space wave footprint at zoom is handled by `waveLOD`, NOT by
  this precision bound - separate concern, now mitigated by design.)
- **Seam-free by construction.** `WorldPos` must be the un-wrapped world
  position (grep-verify the VS emits `WorldPos = worldPos;` where `worldPos`
  is `vec3(vxy, wz)` BEFORE any `MaxMinUV` shift - it is; `MaxMinUV` only
  mutates `u`/`v`/`Texcoord`, never `worldPos`). If that ever changes, the
  seam returns - this is the named contract S1 depends on.
- **Detail/spray (S4) subsumed.** The `o_isWater == 2` legacy detail discard
  stays as-is (untouched). A living base surface removes the need v1 suppressed
  detail for; S4 needs no separate slice unless the user wants more than S1.
- **Dead-block awareness (adversarial F4/F5 - do NOT touch).** The base layer
  `o_isWater==1` returns early; the SECOND `o_isWater==1` wave sub-block in the
  legacy fallthrough (`gos_terrain_water_mdi.frag` ~lines 101-115) is therefore
  unreachable dead code. v1 deliberately preserved the fallthrough verbatim
  (minimal-touch). S1 keeps that decision: edit ONLY the live `o_isWater==1`
  branch; do NOT modify or "clean up" the dead block (out of scope; its cleanup
  is a separate retire-legacy slice, not S1). Executor must not be confused by
  the duplicate `wuv` at the dead block.
- **No regression of v1 invariants:** z-bias untouched (no depth math change),
  `[WATER_MAT v1]` / `[WATER_DEPTHPROBE v2]` probes untouched (CPU-side, other
  TUs), base/detail shared-program structure untouched, `WaterThickness`
  absorption path untouched, `o_isWater==2` discard untouched. S1 changes only
  the live-branch `wN`/`spec` derivation + the consts in Section 3.

## 5. Files changed

```
MODIFIED  shaders/gos_terrain_water_mdi.frag  -- in the LIVE o_isWater==1 branch
                                                 only: replace wuv/w phase with
                                                 WorldPos.xy + waveLOD distance
                                                 fade; spec *= SPEC_SCALE*waveLOD;
                                                 NORMAL_STRENGTH 0.0 -> 0.18;
                                                 add WAVE_FREQ/SPEED/SPEC_SCALE/
                                                 WAVE_FADE_NEAR/FAR consts.
                                                 Dead 2nd o_isWater==1 block + the
                                                 o_isWater==2 discard: UNTOUCHED.
```

(Recipe-population reference for grounding: `GameOS/gameos/gos_terrain_water_stream.cpp`
~266-273; map-extent: `mclib/terrain.cpp` ~95/318/395.) No new files. No VS,
no C++, no uniform, no varying, no pass. Hot-reloadable (`#version` stays
prefixed by makeProgram; check console for silent compile fail after redeploy
- the v1 debug pitfall).

## 6. Gates (same isolated, kill-aware discipline as v1)

Deploy ONLY the `.frag` to `A:/Games/mc2-opengl/mc2-win64-water/` (never v0.4).
`mc2_01` only, `--duration 30 --keep-logs --exe <water>`, no `--kill-existing`
if another mc2 is running (priority session). Decide by markers in the engine
log, never exit code: log present + `[SMOKE v1] event=summary result=pass` =>
completed; require shader compiled (no `0(N): error`/link error - silent-fail
trap). Then USER visual tuning - **two camera regimes are MANDATORY** (the
zoomed-out big-map is the documented stress path that hides full-map
regressions - `memory/zoomed_out_big_map_is_an_unexercised_stress_path...`;
omitting it repeats a known lesson):
- **Near/default camera:** surface has gentle animated life (subtle normal
  variation + soft moving sheen) - NOT flat-dead, NOT noisy fireflies.
- **Zoomed-out big-map AND grazing/low angle (MANDATORY):** must reduce to the
  v1 calm/camera-stable look - NO crawling/aliasing shimmer, NO per-pixel
  specular flashing, NO pale-wash return. `waveLOD` should make far water
  visually identical to shipped v1 there.
- NO diagonal tile-seam bands / "catch on the loops" at ANY angle (the S1
  premise; if present, the seam-free contract broke - escalate, do not tune).
Tuning knobs via hot-reload (clean up after, per the user tune-later mandate):
`NORMAL_STRENGTH` (wave tilt), `WAVE_FREQ` (swell scale - lower=bigger swell),
`WAVE_SPEED` (drift pace), `SPEC_SCALE` (sparkle intensity, decoupled),
`WAVE_FADE_NEAR/FAR` (distance at which it returns to v1 calm).

## 7. Plan-stage advisor routing

`mc2-shader-expert`: confirm `WorldPos` is the un-wrapped world pos in the FS
(seam-free contract), the `highp` trig precision bound for mc2_01 extent, and
that re-enabling `wN` does not reintroduce the v1 grazing-wash given
`FRESNEL_SKY_MAX=0.12`. Cross: none (no pipeline/C++ surface touched).
