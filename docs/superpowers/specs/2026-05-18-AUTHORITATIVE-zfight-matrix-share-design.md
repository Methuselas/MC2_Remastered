# AUTHORITATIVE: Zoom Z-Fight Fix - Matrix-Share + Drop-Fudge (Fix B)

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated; user integrates separately)
**Owner:** assistant (designated water owner)
**Status:** PLAN-READY. Re-adversarial #2 PASSED (opus+sonnet both
APPROVE-WITH-REQUIRED-EDITS, no BLOCK, 2026-05-18); open questions GROUNDED;
all required edits FOLDED into Section 12 (authoritative over Sections 3, 4,
and the edit-site lists of 11). User approved the Fa scope expansion
2026-05-18. Next: plan -> subagent execute. **Section 12 is the operative
design; read it last and treat it as authoritative.**
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

---

## 10. DUAL ADVERSARIAL OUTCOME (opus + sonnet, 2026-05-18)

opus=BLOCK / sonnet=APPROVE-WITH-REQUIRED-EDITS. Reconciled: the APPROACH
is sound (core numerical premise CLEARED by sonnet's make-or-break check:
terrain's two-stage projector and water/decal's are structurally identical
chains -> same matrix => bit-identical depth, co-planar by construction
WHEN ARMED; S3-BLOCK not smuggled; CPU-water/Sym1 exclusion confirmed
genuinely proven-distinct, not a punt) - but the spec AS WRITTEN has
CRITICAL defects. Required revision (folded / tracked below):

- **C1 (CRITICAL, both): wrong decal edit-site.** `~:3262` is the
  mine-sprite / `drawDecalStaticBatch` setup block, NOT the decal MVP
  upload. The real shared site is `uploadOverlayUniforms_()`
  ~`gameos_graphics.cpp:7013-7014` (`getTerrainMVP()` -> overlay
  `terrainMVP`), called from THREE paths: `drawTerrainOverlays` (~:7066,
  live per-frame), `drawDecalStaticBatch` (~:7176, static bake),
  `drawDecals` (~:7240, live bomb-crater). The armed substitution must be
  inside `uploadOverlayUniforms_` with an armed-vs-LIVE split (or a
  per-caller MVP arg) - a naive swap regresses the two LIVE paths (the
  85d9d17 shared-helper trap, confirmed real). Plus verify
  `overlayLocs_.terrainMVP` vs `decalLocs_.terrainMVP` (two
  `OverlayUniformLocs_` instances; `drawDecalStaticBatch` uses
  `overlayLocs_`).
- **C2/C3 (CRITICAL opus / MINOR sonnet) - ESCALATED ARCHITECTURAL FORK:**
  the spec's R-a "mirror the proven 926/0 / reflOn arm-gate" is a
  MISIDENTIFICATION - the 926/0 fix (`gos_terrain_water_stream.cpp:
  ~1409-1416`) feeds the water CULL-COMPUTE `u_terrainMVP`, NOT the render
  VS; `reflOn` ~:2330 is a texture-bind gate, not an MVP-source gate.
  There is NO existing proven render-VS MVP arm-gate to mirror. An
  `IsFrameSolidArmed()?getDispatchMvp16():terrain_mvp_` at the render bind
  is NOVEL plumbing creating a NEW armed<->un-armed render-path transition
  discontinuity that `[WATER_DEPTHPROBE v2]` (cull-MVP hash) is
  structurally BLIND to = the 1-frame-lag class on an un-probed path.
  FORK (needs render/terrain-indirect grounding before revise): (Fa) add a
  render-path depth-consistency probe + explicit arming-transition
  analysis and keep the arm-switch; vs (Fb) make the water/decal RENDER VS
  ALWAYS read `getDispatchMvp16()` with a DEFINED un-armed value (no
  flip -> no transition discontinuity -> dodges the whole 1-frame-lag
  risk). Fb is likely the cleaner answer; grounding decides.
- **M1 (MAJOR, both): zero-fudge is UNSAFE - mandate a non-zero shared
  constant.** Identical MVP + zero epsilon + LEQUAL + draw-order = coplanar
  shoreline TIE = the v0.3 staircase regression the LEQUAL scar block
  (`gos_terrain_water_fast.vert:~328-364`) exists to prevent. The shared
  epsilon's RELATIVE-pop-cancellation math IS sound (a shared constant
  cancels in relative ordering, no zoom-nonlinearity reintroduced - the
  spec's load-bearing claim holds), but "zero" is NOT viable. Spec MUST
  mandate ONE non-zero shared small constant, applied identically to
  water AND `terrain_overlay.vert` (currently fudge=0; removing
  `glPolygonOffset` there without adding it strips the live overlay's ONLY
  depth ordering). Not optional.
- **M3/m-3: scope completeness.** All three `glPolygonOffset(-1,-1)` sites
  (~:7062/7171/7236) are decal-family (not shared w/ terrain/water/shadow
  - clears that hazard) but are 3 distinct passes; removal is all-or-
  nothing single-PR and REQUIRES the shared shader constant for all three.
  Missing host bind: the NON-MDI water path binds `terrain_mvp_` at
  ~`gameos_graphics.cpp:2153` (not just the MDI ~:2308) - it needs the
  same armed substitution + its `WATER_DEPTH_FUDGE_FAST` removal
  (`gos_terrain_water_fast.vert:~365`) or MDI/non-MDI water split.
- **Surface to user (m-2):** the user's "happens with CPU water too" will
  persist on the un-armed intro/deploy cinematic (CPU raster, no
  terrainMVP uniform - genuinely out of scope, probe-proven) - this is
  EXPECTED, not a fix failure; confirm acceptable at the visual gate.

Plan is NOT written until: the C2/C3 R-a fork is grounded (Fa vs Fb), and
the spec is revised with the corrected sites (C1), mandated non-zero
shared constant (M1), and full scope (M3/m-3). Then re-adversarial (the
revision is substantial).

---

## 11. REVISED DESIGN (post R-a fork grounding, 2026-05-18) - supersedes S3/S4-R-a

The dual-adversarial BLOCK is resolved: approach sound; the fictional R-a
is replaced by the grounded SYMMETRIC-MIRROR + a mandatory render-path
probe. This section is authoritative over Sections 3 and 4-R-a.

**STRIKE R-a.** There is NO existing render-VS MVP arm-gate to "mirror"
(the 926/0 gate `gos_terrain_water_stream.cpp:~1409-1416` feeds the water
CULL-COMPUTE `u_terrainMVP` ~:1344, not a render VS).

**Root design = symmetric mirror.** Grounded fact: when ARMED, terrain
draws from `g_dispatchMvp16` (snapshot of `terrain_mvp_` at compute-
dispatch); when UN-ARMED, `DrawIndirect` early-returns and terrain falls to
the LEGACY per-quad live path projecting from live `terrain_mvp_`
(`terrain.cpp:~1069-1089`). So the co-planarity target itself arm-switches
on `s_frameSolidArmed` the same frame. Water/decal must bind the SAME
expression terrain effectively uses:
`IsFrameSolidArmed() ? gos_terrain_indirect_getDispatchMvp16() :
gos_GetTerrainMVPMat4()` (with the `if(!mvp) mvp=gos_GetTerrainMVPMat4()`
nullptr-safety exactly as `gos_terrain_water_stream.cpp:~1413`). This is
INTENTIONAL and REQUIRED: because terrain itself flips source on the same
gate the same frame, the symmetric flip yields ZERO *relative*
water-vs-terrain discontinuity in both regimes AND across the transition
frame (the only thing z-fight/co-planarity cares about). A "no-flip pin to
one source" would CREATE a relative discontinuity - explicitly rejected.

**Corrected exact edit sites (4):**
1. `gameos_graphics.cpp:~2153` - non-MDI water `setMat4Direct("terrainMVP",
   &terrain_mvp_)`. NOT dead: it is the shared pre-amble bind in
   `renderWaterFastPath`, live whenever `mdiValid` is false. Bind the
   symmetric-mirror expression.
2. `gameos_graphics.cpp:~2308` - MDI water `setMMat4Direct("terrainMVP",
   &terrain_mvp_)`. Same symmetric-mirror.
3. `uploadOverlayUniforms_()` `gameos_graphics.cpp:~7011-7014` - add an
   explicit `const float* terrainMvpOverride = nullptr` param (nullptr =>
   current live `getTerrainMVP()`, unchanged). ONLY `drawDecalStaticBatch`
   (`~:7176`, the armed static-bake decal path co-planar with armed
   terrain-solid) passes the symmetric-mirror expression. The two LIVE
   callers `drawTerrainOverlays` (`~:7066`) and `drawDecals` (`~:7240`)
   pass nothing => unchanged live behaviour => NO live-overlay regression.
   **Correction to C1's locs note:** `drawDecalStaticBatch` uses
   `overlayLocs_`/`overlayProg_` (SHARED with the live `drawTerrainOverlays`),
   NOT `decalLocs_` - so per-locs-struct dispatch is IMPOSSIBLE; the
   per-caller argument is mandatory.
4. Fudge removal: `gos_terrain_water_fast_mdi.vert:~291` +
   `gos_terrain_water_fast.vert:~365` remove `WATER_DEPTH_FUDGE_FAST`;
   `terrain_overlay.vert:~36` + the 3 `glPolygonOffset(-1,-1)` sites
   (`gameos_graphics.cpp:~7062/7171/7236`, all decal-family, none shared
   w/ terrain/water/shadow) - replace ALL with the SAME single non-zero
   shared small constant (M1: zero is UNSAFE - regresses the shoreline
   LEQUAL scar `gos_terrain_water_fast.vert:~328-364`; the live overlay at
   :7062 has NO other ordering once polygon-offset is gone, so the shared
   constant in `terrain_overlay.vert` is MANDATORY not optional).
   Single-sourced in the lockstep `terrain_depth_bias.hglsl`/`.h`.

**Mandatory render-path probe (new spec deliverable, same commit):**
`MC2_WATER_RENDERPROBE` (env-gated, silent default, demote-not-delete,
FNV idiom of `[WATER_DEPTHPROBE v1]` `gos_terrain_water_stream.cpp:
~1431-1437`). Invariant A: FNV(matrix actually uploaded at :2153/:2308/
:7014-static) == FNV(the cull-feed matrix `gos_terrain_water_stream.cpp:
~1416`) every armed frame - the canary for the ONE real residual hazard
(`terrain_mvp_` mutated between the early cull-feed read and the late
render-bind read within one armed frame). Invariant B: latch the
arming-transition frame; assert water-render-bind FP == terrain's
this-frame source FP (dispatch FP if armed, live FP if un-armed). Passing
Invariant B on a captured arming-transition frame is a RELEASE GATE (it
substitutes for the un-probed-path risk; RenderDoc cannot catch the
1-frame transient). If Invariant A ever trips, the fix is to snapshot at a
single frame point - NOT to abandon the symmetric switch.

Plan-ready. Re-adversarial (the revision is substantial) -> plan ->
subagent execute. Mandated re-adversarial foci: (a) the symmetric-mirror
relative-co-planarity argument (prove water==terrain MVP in BOTH regimes +
across the transition frame), (b) Invariant A intra-frame-mutation hazard,
(c) the `uploadOverlayUniforms_` per-caller-arg leaving the 2 live callers
byte-unchanged, (d) M1 non-zero shared constant preserving the shoreline
LEQUAL invariant at all zooms.

---

## 12. RE-ADVERSARIAL #2 OUTCOME + GROUNDED FOLD (2026-05-18) - AUTHORITATIVE

Re-adversarial #2 (opus + sonnet, adversarial-plan-review skill,
code-grounded, independent). **Both APPROVE-WITH-REQUIRED-EDITS; neither
BLOCK.** Foci verdicts converged: (a) armed-regime co-planarity PROVEN
bit-identical (terrain armed reads `g_dispatchMvp16` baked from
`gos_GetTerrainMVPMat4()` at `gos_terrain_indirect.cpp:~2887/2903`; armed
thin VS reads `tr.clipPos[]`, no `terrainMVP`); the ternary's two arms
correspond bit-identically to terrain's two regimes; "no-flip pin creates
the discontinuity" reasoning correct. (b) Invariant A hazard NOT real under
current single-per-frame writer. (c) per-caller-arg mechanically sound,
exactly 3 callers, no 4th. (d) zero-fudge UNSAFE confirmed, BUT the spec's
"one shared constant" is internally impossible. **This section is
authoritative over Sections 3, 4, and the edit-site/probe text of 11.**
Folded required edits + the grounded resolutions of the 2 open questions:

### 12.1 CRITICAL (both reviewers, independently, same math): TWO single-sourced oppositely-signed constants - NOT one

With matrix-share the layers are bit-identically co-planar in `clip.z*rhw`;
the added screen-z constant is the ONLY depth separator. Grounded ordering
invariants (LEQUAL, forward-z `glClipControl(ZERO_TO_ONE)`, draw order
terrain -> decals/overlays -> water):
- **Water must lose the shoreline tie to terrain** (occluded by terrain) ->
  water constant **> `TERRAIN_DEPTH_FUDGE` (0.002)**. Enforced by the v0.3
  staircase LEQUAL-scar block `gos_terrain_water_fast.vert:~328-364`
  (verified real; zero/coplanar water = the staircase regression).
- **Decals/overlays must win the tie over terrain** (render on top,
  replacing today's `glPolygonOffset(-1,-1)` = toward-camera) ->
  decal/overlay constant **< `TERRAIN_DEPTH_FUDGE` (0.002)** (candidate
  `0.0`, or a small epsilon `< 0.002`; exact value is the open shader
  sub-question -> `/mc2-amd-shader-review` + mc2-shader-expert at plan
  time, Section 6 V6).
- `K > 0.002` AND `K < 0.002` cannot both hold -> **a single shared
  constant is mathematically impossible.** Section 11 edit-site-4 and the M1
  mandate are CORRECTED: define **TWO** constants, single-sourced in the
  lockstep `shaders/include/terrain_depth_bias.hglsl` + `mclib/terrain_
  depth_bias.h`, e.g. `WATER_DEPTH_BIAS` (> terrain ref) applied at
  `gos_terrain_water_fast_mdi.vert:~291` + `gos_terrain_water_fast.vert:
  ~365`, and `OVERLAY_DEPTH_BIAS` (< terrain ref) applied at
  `terrain_overlay.vert:~36`, with `TERRAIN_DEPTH_FUDGE` (0.002, terrain
  VS UNCHANGED) as the shared reference. This is NOT Fix A's
  zoom-nonlinearity: with a bit-identical shared projection a constant
  post-divide offset is invariant in relative depth ordering at every zoom;
  the relative-pop-cancellation math holds *per signed pair*. This is
  precisely `feedback_single_source_scattered_tuning_constants.md`:
  single-source the MECHANISM (the lockstep header), keep regime-distinct
  VALUES as separate named constants - the header already documents this
  same lesson for FAST-vs-RASTER. All three `glPolygonOffset(-1,-1)`
  (`gameos_graphics.cpp:~7062/7171/7236`, confirmed all decal-family, none
  shared w/ terrain/water/shadow) removed all-or-nothing same-PR, replaced
  by `OVERLAY_DEPTH_BIAS`.

### 12.2 GROUNDED Q1 (un-armed co-planarity - spec wording corrected, conclusion HOLDS)

The reviewers contradicted each other; grounding resolved it. In the
DEFAULT smoke env the fast-path env trio is default-OFF, so the thin-VS /
patch-stream tessellation paths are DEAD and the thin VS declares no
`terrainMVP` uniform anyway (`gos_terrain_thin.vert:~57-64`, reads
`tr.clipPos[]`). Un-armed terrain runs the **legacy per-quad raster**
(`terrain.cpp:~1069-1089` -> `quad.cpp:~2508+`) submitting CPU
pre-projected `vertices[].px/py/pz` from `eye->projectForTerrainAdmission`
/ `Camera::projectZ` (`terrain.cpp:~1741-1753`) - NOT a shader projecting
`terrain_mvp_`. **Section 11's "terrain projects from live `terrain_mvp_`
un-armed" wording is imprecise and is corrected here.** Conclusion still
HOLDS: `gos_GetTerrainMVPMat4()` (`= &terrain_mvp_`,
`gameos_graphics.cpp:~7319/1419`) is constructed to be the matrix-EXACT
equivalent of that same `projectZ` - documented load-bearing invariant
`gamecam.cpp:~167-172` ("AW^T*(vx,vy,elev,1) = projectZ(...) exactly,
Stuff row-vector convention"). So Fix B's un-armed ternary arm IS
co-planar with un-armed terrain **by construction**. Caveat (carry to plan
verification): this equivalence is by construction/comment, not a runtime
parity probe - the `[DEPTH_TRANSITION v1]` / `MC2_WATER_RENDERPROBE`
Invariant B on a captured arming-transition frame is the empirical check.
CPU-water's separate `dz_cpuw` sign-flip residual is a DIFFERENT
`eye->projectForTerrainAdmission`-vs-dispatch divergence, still OUT.

### 12.3 GROUNDED Q2/Q3 -> Fa (USER-APPROVED 2026-05-18): extend the symmetric-mirror to ALL THREE overlay/decal callers; supersedes Section 11 edit-site-3 and the prior-adversarial C1 "byte-unchanged" constraint

Grounding (Q2): in the DEFAULT smoke env the user's cement + bomb-crater
zoom-step pop flows through the **LIVE** path - `gos_DrawTerrainOverlays()`
UNCONDITIONAL (`txmmgr.cpp:~2064-2067`) -> `drawTerrainOverlays`
(`gameos_graphics.cpp:~7066`) and `gos_DrawDecals()` UNCONDITIONAL
(`txmmgr.cpp:~2095-2098`) -> `drawDecals` (`~:7240`). The static bake
`drawDecalStaticBatch` (`~:7176`) is `IsFrameOverlayArmed()`-gated
(`txmmgr.cpp:~2079-2081` -> `gos_terrain_indirect.cpp:~204-230`,
`MC2_TERRAIN_INDIRECT_OVERLAY` default OFF) -> **DEAD in default smoke.**
Section 11's static-bake-only edit-site-3 therefore does NOT touch the
matrix source for the decals the user actually sees - it would leave the
reported symptom unfixed.

Grounding (Q3) -> **Fa, USER-APPROVED:** all three callers
(`drawTerrainOverlays` `~:7066`, `drawDecalStaticBatch` `~:7176`,
`drawDecals` `~:7240`) pass the symmetric-mirror expression
`IsFrameSolidArmed() ? gos_terrain_indirect_getDispatchMvp16() :
gos_GetTerrainMVPMat4()` via the new per-caller `const float*
terrainMvpOverride` argument to `uploadOverlayUniforms_()`
(`gameos_graphics.cpp:~7011-7014`). The `nullptr` default is retained ONLY
as a safety no-op fallback, NOT used by any of the 3 callers. Grounded
safety rationale (supersedes the prior-adversarial C1 "leave the 2 live
callers byte-unchanged", which rested on the now-DEAD 85d9d17
MVP-desync model):
- **Un-armed: byte-identical.** Live callers today upload `getTerrainMVP()`
  == `&terrain_mvp_`; the mirror's un-armed arm `gos_GetTerrainMVPMat4()`
  returns the SAME `&terrain_mvp_` (`gameos_graphics.cpp:~7319/1419`). No
  un-armed live-overlay regression.
- **Armed: strictly MORE correct.** Live callers run on armed frames the
  same frame terrain-solid draws from `g_dispatchMvp16`; today they use
  divergent live `terrain_mvp_` (= the bug). The mirror makes them read
  `getDispatchMvp16()` - the exact matrix terrain baked from
  (`gos_terrain_indirect.cpp:~3366`) - co-planar by construction.
- **No shared-helper trap, no double-draw.** The trap was specific to the
  dead asymmetric 85d9d17 model. Under the symmetric mirror the correct
  projection for every decal/cement consumer on a frame is uniformly
  "whatever terrain used that frame"; grounding found NO armed case where a
  live decal should intentionally diverge from terrain. Live (unconditional)
  vs static (`IsFrameOverlayArmed`-gated, default OFF) are mutually
  exclusive in default play; in the non-default config the M2d cement
  producer is gate-suppressed (`quad.cpp:~2457`) so they never both emit
  the same frame - all-3-mirrored stays consistent there too.
- **Mechanical:** `drawTerrainOverlays` + `drawDecalStaticBatch` use
  `overlayLocs_`/`overlayProg_`; `drawDecals` uses `decalLocs_`/`decalProg_`
  - but all three share `terrain_overlay.vert`. Per-locs dispatch inside
  the helper is impossible (confirms Section 11 C1); the policy lives at
  the 3 call sites via the per-caller arg, helper stays neutral.

**Net edit-site-3 (REPLACES Section 11 site 3):** add `const float*
terrainMvpOverride = nullptr` to `uploadOverlayUniforms_`; the 3 callers
each pass the symmetric-mirror expression (with the
`if(!mvp) mvp=gos_GetTerrainMVPMat4()` nullptr-safety as
`gos_terrain_water_stream.cpp:~1413`). Sites 1 (`~:2153` non-MDI water,
config-dependent-live but harmless+correct for the `!mdiValid` fallback -
apply anyway), 2 (`~:2308` MDI water) unchanged from Section 11.

### 12.4 MINOR (both): Invariant A re-characterized; Invariant B is the release gate

`MC2_WATER_RENDERPROBE` Invariant A (FNV render-bind matrix == FNV cull-feed
matrix) is TRIVIALLY-TRUE under the current architecture: sole per-frame
`terrain_mvp_` writer is `gos_SetTerrainMVP` @ `gamecam.cpp:~176` (only
caller of the only writer, before all consumers); `g_dispatchMvp16` written
once/armed-frame @ `gos_terrain_indirect.cpp:~2903`, never mutated
intra-frame. It is a valid FUTURE-REGRESSION TRIPWIRE (fires if someone
later adds a mid-frame MVP mutation) but is NOT "the canary for the one
real residual hazard" - that hazard does not exist today. Re-document
accordingly; do NOT read a green Invariant A as a correctness proof.
**Invariant B (latched arming-transition frame: water/decal render-bind FP
== terrain's this-frame source FP) is the genuine release gate** -
RenderDoc cannot catch the 1-frame transient; Invariant B on a captured
transition frame substitutes for it. Probe stays env-gated, demote-not-
delete.

### 12.5 What re-adversarial #2 confirmed RIGHT (do not regress in plan)

Symmetric-mirror approach + armed bit-identical co-planarity; mirrors the
shipped 926/0 ternary (`gos_terrain_water_stream.cpp:~1409-1416`), not
novel plumbing; the C2/C3 STRIKE-R-a fold correct (no render-VS arm-gate
to mirror; 926/0 feeds cull-compute `u_terrainMVP` `~:1344`); S3-BLOCK
non-violation (same MVP/frame/on-screen view, none of the 4 blocked
elements); zero-fudge-UNSAFE premise; all 3 `glPolygonOffset` decal-family;
`getDispatchMvp16()` armed/stale contract + un-armed fallback choice;
CPU-water + Sym1 exclusion genuinely proven-distinct (`dz_cpuw`
non-constant sign-flipping ~+0.055->-0.006 vs flat `dz_gpuw=+0.001`/
`dz_decal=-0.002`), not a punt.

### 12.6 Plan-stage additions (fold into Section 6 Rule-0 verifications)

- **V7:** re-grep all 3 `uploadOverlayUniforms_` callers + the helper
  signature; confirm still exactly 3, no 4th (opposite-direction grep);
  confirm `drawDecalStaticBatch` uses `overlayLocs_` (not `decalLocs_`).
- **V8:** confirm the two-constant lockstep header pair compiles in both
  the `.hglsl` (all consuming `.vert`) and `.h` (any C++ consumer) sides;
  `OVERLAY_DEPTH_BIAS` < `TERRAIN_DEPTH_FUDGE` < `WATER_DEPTH_BIAS`
  ordering asserted at the single-source site.
- **V9:** mc2-shader-expert + `/mc2-amd-shader-review` decide
  `OVERLAY_DEPTH_BIAS` exact value (0.0 vs small epsilon) in the
  `ZERO_TO_ONE` regime; confirm `WATER_DEPTH_BIAS` preserves the
  `gos_terrain_water_fast.vert:~328-364` scar invariant at all zooms.
- **V10:** re-confirm un-armed `terrain_mvp_` == `gos_GetTerrainMVPMat4()`
  pointer identity and the `gamecam.cpp:~167-172` projectZ-equivalence
  comment still present (Q1 conclusion depends on it).

Re-adversarial #2 satisfied; no third dual-adversarial (the fold is
convergent + grounded, not a substantial re-design - a 3rd would be the
over-caution anti-pattern). Per-task spec+quality reviews in
subagent-execute + the plan-checker carry residual risk. -> plan.
