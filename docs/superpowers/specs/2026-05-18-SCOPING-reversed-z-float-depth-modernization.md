# SCOPING: Reversed-Z + Float-Depth Buffer (root-class depth modernization)

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated)
**Status:** SCOPING ONLY - greenlightable future slice, NOT yet planned.
User-approved to scope (2026-05-18); decision to greenlight deferred to
AFTER Fix B ships.
**Origin:** the greybeard-meta answer to the z-fight class - per-consumer
depth bias/matrix-share are symptom-treatment of a perspective-nonlinear,
low-precision (24-bit integer) depth buffer. Reversed-Z + float depth makes
precision ~uniform across range -> the whole distance-worsening z-fight
class disappears engine-wide. Aligns with the project Vulkan-prep +
`memory/vulkan_aligned_depth_bias_ruling.md` (this is that ruling's
natural completion).
**Grounding:** code-grounded scoping recon, Rule 0, 2026-05-18.

---

## Verdict: TRACTABLE-BUT-LARGE (greenlight as a standalone slice AFTER Fix B)

The killer prerequisite is ALREADY shipped: `glClipControl(GL_LOWER_LEFT,
GL_ZERO_TO_ONE)` (`gameosmain.cpp:~930`, fail-closed, load-bearing). The
engine already produces D3D-style `[0,1]` clip-z; reverse-Z is the delta on
top, not a ground-up depth rework.

## Blast radius (grounded; ~20+ C++ sites + ~10 shaders + 1 lockstep pair, shadows EXCLUDED v1)

- **Projection:** `mclib/camera.cpp:~2074-2092` (perspective `cameraToClip`),
  `~:2020-2038` (ortho/TacMap) - swap near/far. The terrain dispatch MVP
  follows automatically (CPU-supplied).
- **Depth state:** `glDepthFunc` LEQUAL/LESS -> GEQUAL/GREATER (~25 sites
  `gameos_graphics.cpp` + the 3929-3931 switch; `gos_mech_batcher.cpp:
  ~1063`; `gos_static_prop_batcher.cpp:~2872`; `gos_postprocess.cpp:
  ~1139/1163`); ADD explicit `glClearDepth(0)` at ~4 clear sites (engine
  currently relies on the GL default 1.0); scene depth format
  `GL_DEPTH24_STENCIL8` -> 32F (`gos_postprocess.cpp:~294`).
- **Depth-bias lockstep:** `mclib/terrain_depth_bias.h` +
  `shaders/include/terrain_depth_bias.hglsl` + ~13 consumers - reverse-Z
  FLIPS THE SIGN/meaning of every fudge (highest-coordination cluster;
  intersects the active Fix A/B work).
- **Shaders w/ explicit depth (14):** the heavy one is
  `shaders/gos_terrain.frag` (~12 `gl_FragDepth` POM/displacement writes
  incl. a hardcoded `+0.0005` and a `max(UndisplacedDepth,...)` that both
  flip) - a re-derivation, not find-replace. Plus probes
  (`[DEPTH_TRANSITION v1]`, `[WATER_DEPTHPROBE]`) re-baselined.
- **Post-process depth reconstruction:** `shadow_screen.frag`,
  `ssao.frag:~43` - re-derive for `near=1` (~2-3 shaders, mechanical).
- **EXCLUDE shadows from v1** (ortho - no perspective nonlinearity, zero
  precision benefit; including them doubles surface + risks acne sign-flip
  for nothing).

## What it SUBSUMES (project-direction update)

- **The shelved Fix A (distance-proportional clip-z bias) AND the
  `vulkan_aligned_depth_bias_ruling` distance-proportional unifier are
  LARGELY OBVIATED.** They exist solely to compensate for the post-divide
  depth nonlinearity; reverse-Z removes the nonlinearity, so a single small
  symmetric constant suffices and the distance-proportional formula is no
  longer needed. The `terrain_depth_bias` FAST/RASTER dual-regime
  fragility (lakebed-punch-through-at-zoom) dissolves. **This is the
  strongest argument for the slice and a real re-prioritisation: do NOT
  build the distance-proportional bias unifier - reverse-Z replaces it.**
- **Fix B (matrix-share): NOT obviated, independent.** Fix B fixes a
  TEMPORAL desync (water/terrain MVP frame-skew), orthogonal to precision.
  Reverse-Z changes the matrix CONTENT, not Fix B's sharing plumbing -
  Fix B survives reverse-Z cleanly and is a STEPPING STONE (reverse-Z
  verification depends on the water==terrain MVP invariant Fix B
  establishes). Sequence validated: **Fix B (temporal) -> reverse-Z
  (precision) -> the distance-bias slice is cancelled.**
- **Orthogonal / untouched:** CPU-water ~0.05 projection-path divergence
  (different-pipeline, not precision); Sym1 `waterElevation` baseline.

## The 2 design questions that gate the slice (answer BEFORE any code)

1. **`gos_terrain.frag` POM `gl_FragDepth` machinery** (~12 writes + the
   `+0.0005` + the `max(UndisplacedDepth,gl_FragCoord.z)`): must be
   re-derived for `near=1,far=0` (max->min, +const flips). Highest single
   line-item; get it wrong -> terrain self-occlusion / POM punch-through.
   Also: `gl_FragDepth` writes disable early-Z on the 7900 XTX (pre-
   existing wart; reverse-Z forces touching every site).
2. **Legacy CPU `Camera::inverseProjectZ` / raster-water `projectZ`**
   (`camera.cpp:~1941-1985`, `quad.cpp`): hand-rolled scalar forward-`[0,1]`
   math (no matrix seam), used by TacMap + CPU water. Decision: port it
   (risk) vs fence CPU-water/TacMap to a forward-Z compat transform (water
   is going full-GPU anyway -> fencing the dying path may be pragmatic).

## Recommendation

Greenlight as a **standalone slice AFTER Fix B ships**, scope shadows OUT
of v1, answer the 2 gating design questions first, then it is mechanical-
but-wide. On greenlight it becomes a full spec -> adversarials -> plan ->
subagent execute, with the `[DEPTH_TRANSITION v1]` probe re-baselined as
its canary (post-reverse-Z, the distance-nonlinearity it measures must be
GONE - dz zoom-invariant by precision, not by formula). Until greenlit:
tracked, not active; the active z-fight deliverable remains Fix B.
