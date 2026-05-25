# CONSOLIDATED: Terrain/GPU-Water/Decal Zoom Z-Fight - Distance-Proportional Clip-Z Bias

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated; user integrates separately)
**Owner:** assistant (designated water owner)
**Status:** DESIGN - pending user spec review, then 2 mandatory adversarials.
**Supersedes (BOTH):** `2026-05-18-water-terrain-zfight-distance-clipz-bias-design.md`
(`64f265b`, right-direction-but-incomplete) and
`2026-05-18-decal-zoomstep-mvp-source-desync-design.md` (`85d9d17`, DEAD/
3rd-falsified MVP-desync model). This is the single authoritative spec.
**Grounding:** 3 advisor passes + the wired `[DEPTH_TRANSITION v1]` probe
(`f886b6c`) giving NUMERIC pre-fix proof. file:line grep-verified
2026-05-18; Section 6 re-confirms at plan.

---

## 1. Numerically proven root (scoped: terrain + GPU water + decals)

The `[DEPTH_TRANSITION v1]` probe (env `MC2_DEPTH_TRANSITION_PROBE`,
committed `f886b6c`) dumps each consumer's ACTUAL screen-z for a fixed
mid-map water point. Armed smoke `2026-05-18T09-40-39`:

- `dz_gpuw = +0.0010000` **flat every frame** = `WATER_DEPTH_FUDGE_FAST
  (0.003) - TERRAIN_DEPTH_FUDGE (0.002)`.
- `dz_decal = -0.0020001` **flat every frame** = `0 (decal has no additive
  fudge, `terrain_overlay.vert` `px.z = clip.z/clip.w`) - TERRAIN_DEPTH_
  FUDGE (0.002)` (+ a tiny modeled `glPolygonOffset(-1,-1)` term).
- `z_terr` itself **drifts** (0.864 -> 0.856 ...) as camera distance
  changes.

The per-consumer offset is a **constant in screen-z**; `z_terr`'s absolute
window-z swings with camera distance. NDC window-z is nonlinear in eye
distance, so a constant screen-z delta = a **distance-varying world-depth
separation**. A discrete zoom STEP throws `z_terr` to a far different point
on that nonlinear curve in one frame -> the same constant delta now spans a
very different world gap that frame -> the visible ~0.1u jump + transient
z-fight; it re-stabilizes at the new steady zoom. Regime-independent across
GPU water + decals (both carry a different constant fudge than terrain).
`[WATER_DEPTHPROBE v2]` is structurally blind (hashes the MVP matrix; the
fudge is downstream in the VS) - that is how "926/0 equal" coexisted with a
visible jump. This is THE root for the armed-gameplay case.

## 2. Explicitly OUT of scope (distinct roots; not failures of this slice)

- **CPU water.** Same probe, un-armed smoke `2026-05-18T09-42-29`:
  `dz_cpuw` is **NOT constant** - it swings `+0.055 -> -0.006` and FLIPS
  SIGN across the run. CPU water projects via the legacy
  `eye->projectForTerrainAdmission`/`projectZ` Stuff pipeline
  (`quad.cpp:~1060/3345`) - a DIFFERENT projection chain than terrain's
  `g_dispatchMvp16` bake. Its divergence is a large (~+-0.05), distance-
  varying, sign-flipping PROJECTION-PATH mismatch (~50x the fudge deltas),
  NOT a constant-fudge issue. A CPU-side fudge-formula mirror will NOT fix
  it (prior-grounding assumption falsified by this data). CPU water is its
  own distinct sub-root (legacy-projection-vs-dispatch-MVP divergence),
  un-armed-only (intro/deploy) in practice. Tracked separately; this slice
  does NOT touch the CPU raster path.
- **Sym1** - constant water-sits-low at all zooms (`waterElevation`
  baseline). Separate, tracked.

## 3. The fix (shader-expert-grounded; terrain + GPU water + decals)

Replace the constant screen-z fudge with **base + distance-proportional,
clamped**, single-sourced via a helper in the lockstep header pair, applied
symmetrically. From the (still-sound) shader-expert grounding:

`shaders/include/terrain_depth_bias.hglsl` (additions):
```glsl
const float TERRAIN_DEPTH_BIAS_W_REF = 1024.0;  // clip.w @ default-zoom shoreline - CALIBRATE via [DEPTH_TRANSITION v1] pre-ship
const float TERRAIN_DEPTH_FUDGE_MAX  = 0.010;   // hard clamp (deep-water lakebed punch-through guard)

float terrainDepthBias(float baseFudge, float clipW) {
    float s = abs(clipW) / TERRAIN_DEPTH_BIAS_W_REF;   // 1.0 at default zoom
    return min(baseFudge * s, TERRAIN_DEPTH_FUDGE_MAX);
}
```
`mclib/terrain_depth_bias.h` (lockstep, byte-equal): the two `constexpr
float` mirrors. The three regime fudges STAY SEPARATE (per
`feedback_single_source_scattered_tuning_constants.md`) as the per-call
`base`.

- **Terrain** `gos_terrain_thin.vert` ~:219: `screen.z = clip.z*rhw +
  terrainDepthBias(TERRAIN_DEPTH_FUDGE, clip.w)`.
- **GPU water** `gos_terrain_water_fast_mdi.vert` ~:291 + `gos_terrain_
  water_fast.vert` ~:365: `... + terrainDepthBias(WATER_DEPTH_FUDGE_FAST,
  clip.w)`.
- **Decals** `terrain_overlay.vert` ~:36 (`px.z = clip4.z*rhw;`): add
  `+ terrainDepthBias(DECAL_DEPTH_BIAS_BASE, clip4.w)` where
  `DECAL_DEPTH_BIAS_BASE` is a new lockstep regime const sized to keep the
  decal-under-terrain ordering it had via polygon-offset (decals currently
  draw at terrain depth with `glPolygonOffset(-1,-1)` pulling them
  forward). **REMOVE `glPolygonOffset(-1,-1)` (`gameos_graphics.cpp`
  ~:7062/7171/7236) in the SAME change** (double-stack hazard - the ruling
  forbids polygon-offset for this; decals must move fully onto the clip-z
  scheme). `decal_z_approx=1` in the probe becomes exact once decals use a
  shader additive bias instead of rasterizer slope-offset.

`clip.w` is the MVP-consistent value already in scope at each site
(post-`getDispatchMvp16` when armed for GPU water; the live consistent MVP
for terrain/decal - same matrix). NEVER a separately-sampled camera pos.

**SIGN (load-bearing, do not invert):** the screen-z bias *grows* with
`clip.w` so the realized *world* separation stays ~CONSTANT across zoom
(cancels the `1/clip.w` nonlinearity to first order). This is the mandated
fix - NOT "grow the world gap with zoom" (the previously-rejected sign that
punched the lakebed). The clamp is the only place world-invariance
intentionally breaks (extreme zoom-out), bounded below the proven deep-
water limit.

## 4. Load-bearing constraints / regression risks (adversarial focus)

- **THE clamp regime (mandated adversarial focus):** `TERRAIN_DEPTH_FUDGE_
  MAX` at extreme zoom-out trades directly against the protected,
  previously-falsified deep-water lakebed punch-through (`terrain_depth_
  bias.h` ~:36-38, the 0.004->0.003 scar). Verify the clamp never lets the
  water/decal absolute bias exceed the proven deep-water bound while still
  resolving the shoreline tie at default zoom. Calibrate `*_W_REF` from the
  probe's measured default-zoom `clip.w` before ship.
- **Shoreline LEQUAL ordering** (`gos_terrain_water_fast.vert` ~:333-364
  scar block) AND the **decal<terrain<water 3-tier** must hold at EVERY
  zoom under the symmetric scale. Decals moving from polygon-offset to
  clip-z bias is the riskiest part - the new `DECAL_DEPTH_BIAS_BASE` must
  reproduce "decals win over terrain at the same surface" at all zooms;
  this is the second mandated adversarial focus.
- **Do NOT touch `absW`/signed-w packaging** (`gos_terrain_water_fast_mdi.
  vert` ~:293-294; 3 falsified attempts). Bias added to `screen.z` BEFORE
  the `mvp*vec4(screen,1)` re-projection.
- **AMD/early-Z (`/mc2-amd-shader-review`):** all vertex-stage (no
  `gl_FragDepth`); `min()` branchless; re-run the review on the 4 post-edit
  `.vert` as the pre-build gate.
- **No regression** of shipped S1/S6/transparency water (depth-only, VS-
  stage), the static decal bake correctness, terrain Fix-B, shadow paths
  (confirm changed VS not linked into shadow programs), `[WATER_MAT v1]`.
  `[WATER_DEPTHPROBE v2]` stays equal=1 (blind; not this slice's canary).
- **CPU water untouched** - its divergence is the separate sub-root; do NOT
  attempt a CPU fudge mirror here (proven insufficient).

## 5. Canary: `[DEPTH_TRANSITION v1]` (already wired, `f886b6c`)

It is the pre/post numeric proof. **Pre-fix (recorded):** `dz_gpuw` flat
+0.0010000, `dz_decal` flat -0.0020001, `z_terr` drifting -> constant
screen-z, distance-varying world. **Post-fix gate:** add a `world_sep`
readout (the bias's world-depth equivalent via the inverse-projection
jacobian, per the shader-expert canary design) and require `world_sep` for
GPU water and decal to stay within a tight band (e.g. +-10%) across a zoom
sweep INCLUDING the transition frame, while raw `dz_*` is no longer flat
(it now scales with `clip.w` to hold world-sep constant) - until the clamp
engages at extreme zoom-out. This converts the existing probe into the
substitutive correctness gate; it must be extended with the `world_sep`
column in this slice.

## 6. Plan-stage Rule-0 verifications (close in the plan)

- **V1:** re-grep the 4 apply-site lines (3 existing water/terrain +
  `terrain_overlay.vert`) + `clip.w`/`clip4.w` in scope at each; the
  `absW` repack lines (untouched).
- **V2:** re-grep `terrain_depth_bias.h` + `.hglsl` constants + lockstep
  contract; the helper-insertion seam; add `DECAL_DEPTH_BIAS_BASE`.
- **V3:** grep ALL `glPolygonOffset(-1,-1)` decal sites
  (`gameos_graphics.cpp` ~:7062/7171/7236) - ALL must be removed
  same-change; confirm none is shared with a non-decal pass.
- **V4:** confirm the 4 changed `.vert` not linked into shadow programs.
- **V5:** confirm `[DEPTH_TRANSITION v1]` `world_sep` extension point +
  that `[WATER_DEPTHPROBE v2]` stays equal=1 (blind).
- **V6:** re-run `/mc2-amd-shader-review` on the 4 post-edit `.vert`.

## 7. Gates

Build (RelWithDebInfo, full relink) + deploy exe+shaders to isolated
`mc2-win64-water` ONLY. Kill-aware `mc2_01` smoke + `MC2_DEPTH_TRANSITION_
PROBE=1`: post-fix `world_sep` for GPU water + decal zoom-invariant within
band (incl. transition) + `[WATER_DEPTHPROBE v2]` equal=1 + no `0(N):
error`. Then USER visual: shoreline z-fight + the zoom-step JUMP gone for
GPU water AND decals across in/out zoom steps; no new deep-water lakebed
punch-through; no shoreline regression at default zoom; decals still
correctly over terrain at all zooms (the polygon-offset->clip-z migration);
S1/S6/transparency unchanged. CPU water (un-armed intro/deploy) jump + Sym1
expected to persist - NOT failures of this slice. `*_W_REF`/`DECAL_DEPTH_
BIAS_BASE` tuned in the visual loop via the probe (hot-reload `.hglsl`;
sync `.h` once converged - C++ const change needs relink).

## 8. Files (anticipated; grep-confirm at plan)

```
MODIFIED shaders/include/terrain_depth_bias.hglsl  -- terrainDepthBias() + W_REF/FUDGE_MAX + DECAL_DEPTH_BIAS_BASE
MODIFIED mclib/terrain_depth_bias.h                -- lockstep consts
MODIFIED shaders/gos_terrain_thin.vert             -- ~:219 helper (TERRAIN base)
MODIFIED shaders/gos_terrain_water_fast_mdi.vert   -- ~:291 helper (WATER_FAST base)
MODIFIED shaders/gos_terrain_water_fast.vert       -- ~:365 helper (WATER_FAST base)
MODIFIED shaders/terrain_overlay.vert              -- ~:36 helper (DECAL base)
MODIFIED GameOS/gameos/gameos_graphics.cpp         -- REMOVE glPolygonOffset(-1,-1) decal sites; [DEPTH_TRANSITION v1] world_sep extension
(UNCHANGED, separate sub-root) CPU raster quad.cpp; (UNCHANGED) Sym1 waterElevation
```

## 9. Discipline

This spec -> user spec review -> 2 adversarials (opus|sonnet, adversarial-
plan-review skill, code-grounded, CRITICAL/MAJOR/MINOR; mandated foci: (a)
the clamp-vs-lakebed regime, (b) the decal polygon-offset->clip-z migration
preserving decal-over-terrain at all zooms) -> fold -> plan -> subagent-
driven execute (per-task spec+quality review) -> isolated build/deploy ->
kill-aware mc2_01 smoke (`[DEPTH_TRANSITION v1]` world_sep zoom-invariant +
`[WATER_DEPTHPROBE v2]` equal=1) -> USER visual gate (terrain+GPU-water+
decal; CPU water + Sym1 out) -> `W_REF` calibration loop. Branch isolated;
user integrates separately.
