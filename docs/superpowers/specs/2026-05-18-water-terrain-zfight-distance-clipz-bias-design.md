# Water/Terrain Shoreline Z-Fight - Distance-Proportional Clip-Z Bias

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated worktree; user integrates separately)
**Owner:** assistant (designated water owner, 2026-05-18)
**Decided direction:** `memory/vulkan_aligned_depth_bias_ruling.md` (2026-05-15) -
vertex-stage distance-proportional clip-z bias; NOT `glPolygonOffset`, NOT
`gl_FragDepth`. This spec makes that ruling concrete.
**Status:** SUPERSEDED (2026-05-18) - RIGHT FIX DIRECTION, INCOMPLETE
SCOPE. The 3rd grounding proved the user's zoom-step jump IS the transient
face of THIS distance-nonlinearity (one root, regime-independent) - so this
spec's Sym2/Sym3 split is wrong (they are one root) and its scope is
incomplete: it WRONGLY kept CPU water raster constant (CPU water jumps too -
user-confirmed) and excluded decals (decals jump too, via
glPolygonOffset). To be replaced by ONE consolidated unified clip-z-bias
spec covering terrain + GPU water + CPU water (CPU-side formula mirror) +
decals (with glPolygonOffset(-1,-1) removed same-change), gated on a new
latched [DEPTH_TRANSITION v1] probe. The formula/constraints/canary
sections here remain technically sound and are inherited by the
consolidated spec. DO NOT implement this in isolation.

**(historical) Status:** DESIGN - pending user spec review.
**Grounding:** 2 code-grounded advisor passes this session (render-expert
recon + shader-expert exact-formula), Rule 0. All file:line grep-verified
2026-05-18; symbols stable, lines drift - Section 7 re-confirms at plan.

---

## 1. Problem - scoped to Sym2 ONLY (3-way disambiguated)

User visual obs + code grounding split the water depth artifacts into
THREE distinct symptoms with THREE roots (do NOT conflate):

- **Sym1 - constant sit-low at all zooms** (even stationary at default):
  the separate pre-existing "water sits slightly low" `waterElevation`/
  wave-baseline issue. **OUT OF SCOPE** - the clip-z bias does not address
  it. Tracked separately.
- **Sym2 - shoreline z-fight + progressive recede that SCALES with
  zoom-out:** ONE shared root = the constant screen-z depth-fudge being
  distance-nonlinear. **THIS SLICE.**
- **Sym3 - 1-frame ~0.1 world-unit "jump" on every discrete zoom step
  (in OR out), settles next frame:** a transient frame-anchor/MVP-
  consistency residual specific to discrete zoom transitions (distinct
  from the pan/motion MVP-lag already fixed 926/0). **OUT OF SCOPE** -
  steady-state geometry bias does not fix a 1-frame transient. Tracked
  separately, needs its own grounding.

**The slice's visual gate is judged on Sym2 ALONE.** Sym1's constant
sit-low and Sym3's zoom-step blink will still be present after this slice
and are NOT failures of it.

## 2. Root cause (grep-verified)

The water-vs-terrain depth offset is a CONSTANT post-perspective-divide
window-z fudge in the vertex stage, single-sourced lockstep:
- `mclib/terrain_depth_bias.h` ~:42-49: `TERRAIN_DEPTH_FUDGE=0.002f`,
  `WATER_DEPTH_FUDGE_FAST=0.003f` (=TERRAIN+0.001),
  `WATER_DEPTH_FUDGE_RASTER=0.0025f`. GLSL mirror
  `shaders/include/terrain_depth_bias.hglsl` ~:10-12.
- Apply sites (`screen.z = clip.z*rhw + <FUDGE>`, then `ndc =
  mvp*vec4(screen,1)`): terrain `shaders/gos_terrain_thin.vert` ~:219;
  water `shaders/gos_terrain_water_fast_mdi.vert` ~:291 and
  `shaders/gos_terrain_water_fast.vert` ~:365.

`glClipControl(GL_ZERO_TO_ONE)` is in effect (native [0,1] window-z).
Window-z is nonlinear in eye distance: a CONSTANT `screen.z` offset
realizes a WORLD-depth separation `proportional to clip.w` (shrinks toward
the camera, grows away). Zoomed out (large `clip.w`) the fixed 0.001
water-over-terrain delta both (a) falls below the depth LSB at the shore
=> z-fight (tie-break face), and (b) over-separates => water visibly
recedes (displacement face). The `vulkan_aligned_depth_bias_ruling` fix
(distance-proportional bias) is NOT applied at all today. The MVP-lag
recede-on-pan is ALREADY fixed (926/0, `getDispatchMvp16` when armed) -
not that. `gl_FragDepth` is NOT on the armed terrain path (thin VS pure
vertex-stage) - the bias correctly lands in the VS.

## 3. The fix (exact, shader-expert-grounded)

Replace the constant add with **base + distance-proportional, clamped**,
single-sourced via a helper in the lockstep header pair, applied
symmetrically (same scale `s`, regime-separate `base`).

`shaders/include/terrain_depth_bias.hglsl` (additions):
```glsl
const float TERRAIN_DEPTH_BIAS_W_REF = 1024.0;  // clip.w at default-zoom shoreline (CALIBRATE via canary pre-ship)
const float TERRAIN_DEPTH_FUDGE_MAX  = 0.010;   // hard clamp (deep-water lakebed punch-through guard)

// Distance-proportional window-z bias: scale the regime base by
// clip.w/W_REF so the realized WORLD-depth separation is ~invariant
// across zoom (cancels the 1/clip.w window-z nonlinearity to first
// order), then clamp so deep water can never punch the lakebed.
float terrainDepthBias(float baseFudge, float clipW) {
    float s = abs(clipW) / TERRAIN_DEPTH_BIAS_W_REF;   // 1.0 at default zoom
    return min(baseFudge * s, TERRAIN_DEPTH_FUDGE_MAX);
}
```
`mclib/terrain_depth_bias.h` (lockstep, byte-equal): add
`constexpr float TERRAIN_DEPTH_BIAS_W_REF = 1024.0f;` +
`constexpr float TERRAIN_DEPTH_FUDGE_MAX = 0.010f;`. The three existing
regime fudges STAY SEPARATE (per `feedback_single_source_scattered_tuning_
constants.md`) and become the per-call `base`.

Per-VS edit (terrain `gos_terrain_thin.vert` ~:219):
```glsl
screen.z = clip.z * rhw + terrainDepthBias(TERRAIN_DEPTH_FUDGE, clip.w);
```
Per-VS edit (both water FAST VS ~:291 / ~:365):
```glsl
screen.z = clip.z * rhw + terrainDepthBias(WATER_DEPTH_FUDGE_FAST, clip.w);
```
`clip.w` is grep-confirmed in scope at every site (`rhw = 1.0/clip.w`
computed just above). It is the MVP-consistent `terrainMVP` clip-w
(post-`getDispatchMvp16` when armed - the SAME matrix terrain uses). The
distance proxy MUST be this `clip.w`; NEVER a separately-sampled camera
pos (re-opens the 1-frame-lag class).

**Why zoom-invariant in world depth:** for the MC2 perspective matrix the
window-z increment realizing a fixed world offset is `proportional to
1/clip.w`; scaling the bias by `clip.w/W_REF` cancels it to first order ->
realized world separation constant. The per-regime delta is preserved:
`bias_w - bias_t = (WATER_DEPTH_FUDGE_FAST - TERRAIN_DEPTH_FUDGE)*s =
0.001*s > 0` at every zoom -> water still loses the shoreline `GL_LEQUAL`
tie to already-drawn terrain (correct: water sits just behind terrain at
the shore, wins on open water), and the clamp bounds the absolute water
bias so deep-water lakebed coverage cannot fail.

**SIGN (load-bearing, do not invert):** the bias *window-z value* grows
with `clip.w` so the *realized world separation* stays CONSTANT. This is
the mandated fix - NOT the rejected "grow the world separation with zoom"
(which punches the lakebed). The clamp is the only place world-invariance
intentionally breaks (extreme zoom-out), bounded below the proven deep-
water limit.

## 4. Load-bearing constraints / regression risks (adversarial focus)

- **THE clamp regime (the one the adversarial MUST hammer):**
  `TERRAIN_DEPTH_FUDGE_MAX` at extreme zoom-out is the single point where
  world-invariance breaks and it trades directly against the protected,
  previously-falsified deep-water lakebed-punch-through residual
  (`terrain_depth_bias.h` ~:36-38, the 0.004->0.003 scar). Verify the
  clamp never lets `bias_w` exceed the proven `< ~0.002 absolute over
  terrain` deep-water bound while still resolving the shoreline tie at
  default zoom. `W_REF` MUST be calibrated to the measured default-zoom
  shoreline `clip.w` via the Section 6 canary before ship (1024.0 is a
  placeholder anchor, not measured).
- **Shoreline LEQUAL ordering** (`gos_terrain_water_fast.vert` ~:333-364,
  the scar comment block): water > terrain at shore, bounded delta in deep
  water - must hold at EVERY zoom. The decal<terrain<water 3-tier
  (`~:344-347`; the `MC2_TERRAIN_INDIRECT_OVERLAY` decal bake) must keep
  decals winning over terrain under the symmetric scale.
- **Do NOT touch `absW`/signed-w packaging** (`gos_terrain_water_fast_mdi.
  vert` ~:293-294; 3 prior falsified attempts). Bias is added to
  `screen.z` BEFORE the `mvp*vec4(screen,1)` re-projection - that line and
  the `absW` repack are byte-untouched.
- **AMD/early-Z (`/mc2-amd-shader-review`):** stays entirely vertex-stage
  (no `gl_FragDepth` - kills early-Z, AMD rule violation). No new sampler/
  UBO/SSBO/`#version`/transpose - none of the 6 AMD rules engaged; `min()`
  is a branchless select (RDNA3-safe). Re-run `/mc2-amd-shader-review` on
  all 3 `.vert` post-edit as the pre-build gate.
- **MVP-consistency contract** (`gos_terrain_water_stream.cpp` ~:1400-1430,
  the 926/0 invariant): distance term from the already-consistent `clip.w`
  only. `[WATER_DEPTHPROBE v2]` stays `equal=1` (it hashes the matrix,
  blind to this change - NOT the canary for this fix).
- **No regression** of shipped S1/S6/transparency water (camera-
  independent surface untouched - this is depth-only, VS-stage),
  shadow-depth paths (separate FBO/program - confirm the 3 changed VS are
  not linked into the shadow pass), `[WATER_MAT v1]` probe.
- **Sym1/Sym3 out of scope** - not regressions of this slice (Section 1).

## 5. CPU raster regime (un-armed)

`mclib/quad.cpp` ~:2511-3792 (`WATER_DEPTH_FUDGE_RASTER`/`TERRAIN_DEPTH_
FUDGE` constant adds; `pz`/`wz`/`pw` available). **Decision: keep the CPU
raster path CONSTANT** with a documented armed/un-armed shoreline-pop
divergence (un-armed = brief intro/deploy pan, no zoom-stress - the
failure regime is absent there; porting widens blast radius across ~16
legacy emit sites; `minimal_touch_modern_when_touched` governs). Document
the divergence in BOTH header comment blocks.

## 6. Bias-correctness canary (new; the existing probes are blind to this)

`MC2_WATER_BIASPROBE` (env-gated, silent default, demote-not-delete), in
`GameOS/gameos/gos_terrain_water_stream.cpp` in the same per-frame block
as `[WATER_DEPTHPROBE v2]` (~:1431), AFTER the armed MVP is known. Once
per N frames, for a fixed canonical water/terrain-coincident shoreline
world point at `waterElevation`: CPU-recompute `clip.w`, mirror the GLSL
helper to get `bias_t`/`bias_w`, and the realized world separation
`dWorld` (via the inverse-projection jacobian). Emit:
`[WATER_BIASPROBE v1] f=<frame> clipw=<f> s=<f> bias_t=<f> bias_w=<f>
dWorld=<f> clamped=<0|1>`. **Pass:** across a zoom sweep `dWorld` stays
within a tight band (e.g. +-10%) until `clamped=1` at extreme zoom-out,
while raw `bias_w` visibly grows with `clipw`. Pre-fix the same probe
shows `dWorld` collapsing ~linearly as `clipw` shrinks. Use it to
CALIBRATE `W_REF` (= measured default-zoom shoreline `clip.w`) before ship.

## 7. Plan-stage Rule-0 verifications (close in the plan)

- **V1:** re-grep the exact 3 apply-site lines + that `clip.w`/`rhw` is in
  scope at each; the `absW` repack lines (must stay untouched).
- **V2:** re-grep `terrain_depth_bias.h` + `.hglsl` current constants +
  the lockstep contract comment; confirm the helper-insertion seam.
- **V3:** re-grep the shoreline LEQUAL scar block + the decal/terrain/water
  3-tier; confirm the symmetric-scale delta preserves ordering.
- **V4:** confirm the 3 changed `.vert` are NOT linked into any shadow
  program (negative-claim grep of the shadow pass VS list).
- **V5:** confirm `[WATER_DEPTHPROBE v2]` is matrix-hash only (will stay
  equal=1; not this fix's canary) and the `gos_terrain_water_stream.cpp`
  per-frame probe block location for the new `[WATER_BIASPROBE v1]`.
- **V6:** re-run `/mc2-amd-shader-review` on the 3 post-edit `.vert`.

## 8. Gates

Build (RelWithDebInfo, full relink - shader+header change but the consts
are compile-time in C++ too; deploy exe+shaders) to isolated
`mc2-win64-water` ONLY. Kill-aware `mc2_01` smoke + `MC2_WATER_BIASPROBE=1`:
`[WATER_BIASPROBE v1]` shows `dWorld` zoom-invariant (within band) +
`[WATER_DEPTHPROBE v2]` stays `equal=1` + no `0(N): error`. Then the REAL
gate = USER visual: shoreline z-fight gone AND the zoom-scaling recede
gone (water holds its shore relationship across a zoom sweep), with NO new
lakebed punch-through in deep water, NO shoreline regression at default
zoom, S1/S6/transparency unchanged. Sym1 (constant low) + Sym3 (zoom-step
blink) expected to persist - NOT failures. `W_REF` tuned in the visual
loop via the canary (hot-reload the `.hglsl`; C++ const change needs
relink - tune shader-side first, sync the .h once converged).

## 9. Files (anticipated; grep-confirm at plan)

```
MODIFIED  shaders/include/terrain_depth_bias.hglsl  -- terrainDepthBias() helper + 2 consts
MODIFIED  mclib/terrain_depth_bias.h                -- lockstep 2 consts (byte-equal)
MODIFIED  shaders/gos_terrain_thin.vert             -- ~:219 use helper (TERRAIN base)
MODIFIED  shaders/gos_terrain_water_fast_mdi.vert   -- ~:291 use helper (WATER_FAST base)
MODIFIED  shaders/gos_terrain_water_fast.vert       -- ~:365 use helper (WATER_FAST base)
MODIFIED  GameOS/gameos/gos_terrain_water_stream.cpp-- [WATER_BIASPROBE v1] canary
(UNCHANGED, documented divergence) mclib/quad.cpp CPU raster constant regime
```

## 10. Discipline

This spec -> user spec review -> 2 adversarials (opus|sonnet,
adversarial-plan-review skill, code-grounded, CRITICAL/MAJOR/MINOR; the
clamp-vs-lakebed regime is the mandated focus) -> fold -> plan ->
subagent-driven execute (per-task spec+quality review) -> isolated build/
deploy -> kill-aware mc2_01 smoke (`[WATER_BIASPROBE v1]` zoom-invariant +
`[WATER_DEPTHPROBE v2]` equal=1) -> USER visual gate (Sym2 only) -> W_REF
calibration loop. Branch isolated; user integrates separately.
