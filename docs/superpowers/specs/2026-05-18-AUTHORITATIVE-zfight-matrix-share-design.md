# AUTHORITATIVE: Zoom Z-Fight Fix - Matrix-Share + Drop-Fudge (Fix B)

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated; user integrates separately)
**Owner:** assistant (designated water owner)
**Status:** DESIGN - pending user spec review, then 2 mandatory adversarials
(structural depth change = high-stakes trigger).
**Supersedes ALL prior z-fight specs:** `2026-05-18-water-terrain-zfight-
distance-clipz-bias-design.md` (`64f265b`), `2026-05-18-decal-zoomstep-mvp-
source-desync-design.md` (`85d9d17`, dead), `2026-05-18-CONSOLIDATED-zfight-
distance-clipz-bias-design.md` (`2d2cccb`). Fix A (distance-proportional
clip-z bias) is SHELVED - it compensated for a divergence this fix removes
outright. This is the single authoritative spec.
**Grounding:** 3 advisor passes + the `[DEPTH_TRANSITION v1]` probe
(`f886b6c`, numeric proof) + the bake-vs-bias fork grounding (user proposed
"bake them"; verdict: matrix-share, NOT bake-in-dispatch). Rule 0; Section 6
re-confirms at plan.

---

## 1. Root (numerically proven) and why Fix B eliminates it structurally

TERRAIN renders from PRE-BAKED clip `tr.clipPos[]` baked once/frame by the
terrain compute dispatch with `g_dispatchMvp16` (`gpu_driven_terrain_solid.
comp` ~:226/471; snapshot `gos_terrain_indirect.cpp` ~:2903; published
read-only `getDispatchMvp16()` ~:3366). GPU WATER (`gos_terrain_water_fast_
mdi.vert` ~:283-291) and DECALS (`terrain_overlay.vert` ~:31-42) project
LIVE from the **live `terrain_mvp_`** (host binds it: water `gameos_
graphics.cpp` ~:2308, decal ~:3262), plus a constant screen-z fudge that
differs from terrain's.

The pop has TWO additive components (probe `[DEPTH_TRANSITION v1]`,
`f886b6c`):
1. **live `terrain_mvp_` vs snapshot `g_dispatchMvp16` divergence** -
   ~0 in steady armed (probe `dz_gpuw` flat), spikes TRANSIENTLY on a
   discrete zoom STEP (the 1-frame jump).
2. **constant screen-z fudge is zoom-nonlinear** - a constant post-divide
   NDC offset = a distance-varying world-depth gap; the proven `dz_gpuw`
   flat `+0.0010000` / `dz_decal` flat `-0.0020001` while `z_terr` drifts.

**Fix B kills both at the source, no formula:** bind water+decal to
terrain's already-published baked matrix `getDispatchMvp16()` (eliminates
(1) bit-identically - they project through the exact matrix terrain baked)
AND drop the divergent constant fudge (eliminates (2)). With identical
matrix + identical (zero or one shared constant) screen-z handling, the
layers are co-planar in projected depth -> relative pop ZERO by
construction. Fix A's distance-proportional bias was only needed to make a
*diverging* live projection track a baked one; matrix-share removes the
divergence, so the formula/`W_REF`/clamp/sign-hazard are all moot.

## 2. Not the S3 BLOCK (load-bearing clearance)

The S3 dual-adversarial BLOCK was specifically: a SECOND `ComputeDispatch()`
with a DIFFERENT (mirrored) MVP for an OFF-SCREEN, quarter-res reflection
view (its 4 blocked elements: 2nd dispatch + ring/fence/`g_indirectCmdBuffer`
/`g_dispatchMvp16` mutation; primary-camera cull-window; two-stage thin-VS
pixel-grid reuse; reflected-shadow uniforms). Fix B uses the **SAME MVP,
same frame point, same on-screen primary-camera full-res view**, water/decal
keep **their own** VS/VBO/draw - it touches NONE of the four. It does NOT
bake water/decal clip inside terrain's dispatch (that phrasing IS the
unnecessary ring-mutation-adjacent version - explicitly NOT this design); it
only consumes terrain's already-immutably-published matrix VALUE. Cleared.

## 3. The fix - smallest correct form (3 edits, no new dispatch/buffer/ring)

1. **`GameOS/gameos/gameos_graphics.cpp` ~:2308** (water MDI
   `setMMat4Direct("terrainMVP", &terrain_mvp_)`): bind from
   `gos_terrain_indirect_getDispatchMvp16()` when
   `gos_terrain_indirect::IsFrameSolidArmed()`, else fall back to
   `terrain_mvp_` (un-armed) - matching the existing reflOn-style arm gate
   pattern in the same function (~:2330).
2. **`GameOS/gameos/gameos_graphics.cpp` ~:3262** (decal bridge
   `getTerrainMVP()` -> overlay `terrainMVP`): SAME armed substitution.
3. **`shaders/gos_terrain_water_fast_mdi.vert` ~:291** (and
   `gos_terrain_water_fast.vert` ~:365 for the non-MDI armed path):
   remove `+ WATER_DEPTH_FUDGE_FAST`. **Decal** (`terrain_overlay.vert`
   ~:36, no fudge today) + the decal `glPolygonOffset(-1,-1)`
   (`gameos_graphics.cpp` ~:7062/7171/7236) - if a residual
   water/decal-above-terrain offset is needed to avoid co-planar z-fight,
   replace ALL of them with ONE shared small constant applied IDENTICALLY,
   defined once in the lockstep `shaders/include/terrain_depth_bias.hglsl`
   + `mclib/terrain_depth_bias.h`. The zero-vs-one-constant decision is the
   single open sub-question -> mc2-shader-expert + adversarial (Section 4).

`clip.w`/matrix consistency: water+decal now use the EXACT matrix terrain
baked with - the divergence Fix A chased is gone. `absW`/signed-w packaging
UNTOUCHED. No `gl_FragDepth`. Vertex-stage only.

## 4. Load-bearing constraints / risks (adversarial focus)

- **R-a (mandated focus): `getDispatchMvp16()` armed/stale contract.**
  Confirm it is populated unconditionally WHEN armed and that the un-armed
  fallback to `terrain_mvp_` is correct (the snapshot is stale un-armed -
  `gos_terrain_indirect.cpp` ~:3365 "otherwise stale"). This is S3's V2
  verbatim + the water 1-frame-lag memory (`water_fastpath_interim_fixes_
  and_residuals.md`) - getting the arm gate / fallback wrong re-opens the
  exact 1-frame-lag class the 926/0 fix closed. The water MDI already has a
  proven arm-gated pattern (reflOn ~:2330) to mirror.
- **R-b (mandated focus): decal frame-skew.** Decal draws AFTER terrain
  same frame (`txmmgr.cpp` ~:2080 via `renderLists()`); verify
  `getDispatchMvp16()` at decal-draw time is THIS frame's snapshot and
  there is no skew when terrain is armed but `RebuildDecalStaticVBOIfDirty()`
  / the static decal VBO rebuild runs.
- **R-c: residual coplanar offset (zero vs one shared constant).** If
  water/decal must sit visibly above terrain, the one shared constant must
  itself be zoom-safe (a single small constant applied identically to
  co-projected layers is constant in world-depth-ordering - it does NOT
  reintroduce Fix A's nonlinearity because there is no longer a
  *projection* divergence, only a uniform tiny epsilon). Decide zero vs
  constant with mc2-shader-expert; if a constant, single-source it in the
  lockstep header (per `feedback_single_source_scattered_tuning_constants`).
- **No regression:** shipped S1/S6/transparency water (depth-only, MVP
  source + fudge removal; the camera-independent surface untouched), the
  926/0 MVP-consistency invariant (this STRENGTHENS it - water now
  literally uses terrain's matrix; `[WATER_DEPTHPROBE v2]` stays equal=1
  and is now trivially-true), static decal bake correctness, terrain
  Fix-B, the live per-frame overlay path (the armed substitution must NOT
  change un-armed/live-overlay behaviour - same shared-helper trap class as
  the dead 85d9d17 noted), shadow paths.
- **CPU water EXPLICITLY OUT:** probe proved `dz_cpuw` non-constant
  (~+0.055 -> -0.006, sign-flipping) = a separate ~50x legacy-`eye->projectZ`
  -vs-dispatch-MVP path mismatch, un-armed-only (intro/deploy). NOT fixed
  here (it has no `terrainMVP` uniform to repoint). Separate sub-root,
  tracked. **Sym1** (constant `waterElevation` sit-low) OUT.

## 5. Canary - `[DEPTH_TRANSITION v1]` (already wired, `f886b6c`)

Pre-fix (recorded): `dz_gpuw` flat `+0.0010000`, `dz_decal` flat
`-0.0020001`, `z_terr` drifting. **Post-fix gate:** with matrix-share +
fudge-drop, `dz_gpuw` and `dz_decal` must collapse to ~0 (or exactly the
one shared constant if chosen) AND STAY there across a zoom sweep INCLUDING
the transition frame (no flat-constant-with-drifting-`z_terr`, no transition
spike). `[WATER_DEPTHPROBE v2]` stays equal=1 (now trivially - water uses
terrain's matrix). No `world_sep` extension needed (Fix A artifact - drop
that requirement).

## 6. Plan-stage Rule-0 verifications

- **V1:** re-grep the 2 host bind sites (`gameos_graphics.cpp` ~:2308 water,
  ~:3262 decal) + the existing reflOn arm-gate pattern (~:2330) to mirror;
  `gos_terrain_indirect_getDispatchMvp16()` signature + `IsFrameSolidArmed`
  callability at both.
- **V2:** re-grep `getDispatchMvp16()` populated-when-armed + the
  "otherwise stale" contract (~:3365/2903); the un-armed fallback design.
- **V3:** re-grep the fudge removal sites (water `_mdi.vert` ~:291 /
  `_fast.vert` ~:365) + ALL decal `glPolygonOffset(-1,-1)` sites
  (~:7062/7171/7236) - all consistently handled.
- **V4:** confirm decal draw-order vs terrain (`txmmgr.cpp` ~:2080) +
  no static-VBO-rebuild frame-skew (R-b).
- **V5:** confirm the armed substitution does NOT alter the live per-frame
  overlay path (shared-bind-site trap); changed VS not in shadow programs.
- **V6:** `/mc2-amd-shader-review` on the changed `.vert` (fudge removal
  is trivial but run the gate).

## 7. Gates

Build (RelWithDebInfo, full relink) + deploy exe+shaders to isolated
`mc2-win64-water` ONLY. Kill-aware `mc2_01` smoke + `MC2_DEPTH_TRANSITION_
PROBE=1`: post-fix `dz_gpuw`/`dz_decal` ~0 (or the shared constant) and
zoom-invariant incl. transition + `[WATER_DEPTHPROBE v2]` equal=1 + no
`0(N): error`. Then USER visual: zoom-step jump + shoreline z-fight GONE for
GPU water AND decals across in/out zoom steps; decals still correctly over
terrain at all zooms (polygon-offset removal); no deep-water regression;
S1/S6/transparency unchanged. CPU-water (un-armed) jump + Sym1 expected to
persist - NOT failures.

## 8. Files (anticipated; grep-confirm at plan)

```
MODIFIED GameOS/gameos/gameos_graphics.cpp        -- water(~:2308)+decal(~:3262) bind getDispatchMvp16() when armed; REMOVE decal glPolygonOffset(-1,-1) (~:7062/7171/7236)
MODIFIED shaders/gos_terrain_water_fast_mdi.vert  -- remove WATER_DEPTH_FUDGE_FAST (~:291)
MODIFIED shaders/gos_terrain_water_fast.vert      -- remove WATER_DEPTH_FUDGE_FAST (~:365)
MODIFIED shaders/terrain_overlay.vert             -- (only if a shared residual offset is chosen) ~:36
MODIFIED shaders/include/terrain_depth_bias.hglsl + mclib/terrain_depth_bias.h -- (only if) ONE shared residual constant
(UNCHANGED) CPU raster quad.cpp (separate sub-root); Sym1 waterElevation; terrain VS (terrain already baked - it is the reference)
```

## 9. Discipline

This spec -> user spec review -> 2 adversarials (opus|sonnet, adversarial-
plan-review skill, code-grounded; mandated foci: R-a getDispatchMvp16
armed/stale + un-armed fallback re the 1-frame-lag class, R-b decal
frame-skew, R-c zero-vs-one-constant) -> fold -> plan -> subagent-driven
execute -> isolated build/deploy -> kill-aware mc2_01 smoke
(`[DEPTH_TRANSITION v1]` collapsed+zoom-invariant) -> USER visual gate
(GPU-water+decal; CPU water + Sym1 out). Branch isolated; user integrates
separately.
