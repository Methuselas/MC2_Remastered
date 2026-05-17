# Water v2 - Slice S1: Living Surface (continuous-space animated normal)

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

Replace the phase source and re-enable strength. New consts (compile-time,
hot-reloadable, tuned via the user visual loop exactly like v1):

```glsl
const float NORMAL_STRENGTH = 0.18;   // tune; was 0.0 (v1 flat). Low: preserve camera-stability.
const float WAVE_FREQ       = 0.045;  // 1/world-units; wavelength ~2*PI/0.045 ~= 140 world-u
const float WAVE_SPEED      = 0.6;    // world phase units/sec
```

New wave (continuous; no `Texcoord`, no UV-wrap, hence no seam):

```glsl
// WorldPos.xy is global continuous MC2-world (no MaxMinUV wrap) -> seam-free.
PREC vec2 p  = WorldPos.xy * WAVE_FREQ;
PREC vec2 w  = vec2(sin(p.y          + time*WAVE_SPEED)        + 0.5*sin(p.y*2.17 - time*WAVE_SPEED*0.7),
                    sin(p.x*1.13     - time*WAVE_SPEED*0.85)   + 0.5*sin(p.x*2.31 + time*WAVE_SPEED*0.6));
PREC vec3 wN = normalize(vec3(w * NORMAL_STRENGTH, 1.0));
```

Two octaves per axis, irrational-ish frequency ratios (2.17 / 2.31 / 1.13) so
the pattern does not visibly repeat at view scale. Everything downstream
(`ct = dot(wN,viewDir)`, `fres`, `spec`) is unchanged and already wired.

## 4. Load-bearing constraints (do not regress - adversarial focus)

- **Camera-stability (v1's hard-won property).** The animated `wN` re-adds a
  camera-dependent term via `fres`. v1 fixed the "pale wash at grazing" with
  `FRESNEL_SKY_MAX = 0.12` (kept) + flat normal. S1 must keep `FRESNEL_SKY_MAX`
  low so the normal mostly drives the *specular sparkle* (localized, desirable
  life) not the broad sky-mix wash. `NORMAL_STRENGTH` starts low (0.18) and is
  the primary visual-tuning knob. If grazing wash returns, lower
  `NORMAL_STRENGTH` and/or `FRESNEL_SKY_MAX` - do NOT raise the sky mix.
- **Large-world-coord trig precision.** `WorldPos.xy` can reach ~1e4 (MC2
  world units). `p = WorldPos.xy * 0.045` -> arg ~ up to ~450; `highp` (`PREC`)
  `sin` has ~7 significant digits, ample at that magnitude (no fract/mod
  needed; `WAVE_FREQ` is intentionally small to keep the argument bounded).
  Adversarial must confirm the bound holds for mc2_01's map extent and that
  no shimmer/aliasing results; if it does, the mitigation is a lower
  `WAVE_FREQ` or a `mod(p, K)` domain wrap LARGER than the visible footprint
  (not a tile) - tuning, not redesign.
- **Seam-free by construction.** `WorldPos` must be the un-wrapped world
  position (grep-verify the VS emits `WorldPos = worldPos;` where `worldPos`
  is `vec3(vxy, wz)` BEFORE any `MaxMinUV` shift - it is; `MaxMinUV` only
  mutates `u`/`v`/`Texcoord`, never `worldPos`). If that ever changes, the
  seam returns - this is the named contract S1 depends on.
- **Detail/spray (S4) subsumed.** The `o_isWater == 2` legacy detail discard
  stays as-is (untouched). A living base surface removes the need v1 suppressed
  detail for; S4 needs no separate slice unless the user wants more than S1.
- **No regression of v1 invariants:** z-bias untouched (no depth math change),
  `[WATER_MAT v1]` / `[WATER_DEPTHPROBE v2]` probes untouched, base/detail
  shared-program structure untouched, `WaterThickness` absorption path
  untouched. S1 changes only the `wN` derivation + 3 consts.

## 5. Files changed

```
MODIFIED  shaders/gos_terrain_water_mdi.frag  -- replace wuv/w phase source with
                                                 WorldPos.xy continuous coord;
                                                 NORMAL_STRENGTH 0.0 -> 0.18;
                                                 add WAVE_FREQ, WAVE_SPEED consts
```

No new files. No VS, no C++, no uniform, no varying, no pass. Hot-reloadable
(`#version` stays prefixed by makeProgram; check console for silent compile
fail after redeploy - the v1 debug pitfall).

## 6. Gates (same isolated, kill-aware discipline as v1)

Deploy ONLY the `.frag` to `A:/Games/mc2-opengl/mc2-win64-water/` (never v0.4).
`mc2_01` only, `--duration 30 --keep-logs --exe <water>`, no `--kill-existing`
if another mc2 is running (priority session). Decide by markers in the engine
log, never exit code: log present + `[SMOKE v1] event=summary result=pass` =>
completed; require shader compiled (no `0(N): error`/link error - silent-fail
trap). Then USER visual tuning (the binary that matters):
- Surface has gentle animated life (sparkle/normal variation) - NOT flat-dead.
- NO diagonal tile-seam bands, NO "catch on the loops" at any camera angle.
- Camera-stable: deep teal-blue holds from all angles (no pale-wash return).
Tune `NORMAL_STRENGTH` / `WAVE_FREQ` / `WAVE_SPEED` via hot-reload to taste;
clean up after per the user's tune-later mandate.

## 7. Plan-stage advisor routing

`mc2-shader-expert`: confirm `WorldPos` is the un-wrapped world pos in the FS
(seam-free contract), the `highp` trig precision bound for mc2_01 extent, and
that re-enabling `wN` does not reintroduce the v1 grazing-wash given
`FRESNEL_SKY_MAX=0.12`. Cross: none (no pipeline/C++ surface touched).
