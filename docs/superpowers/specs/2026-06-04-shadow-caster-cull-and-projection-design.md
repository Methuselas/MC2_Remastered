# Design: Dynamic Prop Shadow — Caster Cull + Focus-Center Projection (Lane D)

**Date:** 2026-06-04
**Branch:** `claude/model-override-system-recon-1`
**Codenames:** `SHADOW-CASTER-LIGHTBOX-CULL-1`, `SHADOW-FOCUS-CENTER-1`
**Deploy:** v0.3

## Problem

Close-up gameplay is GPU-bound (~46 ms/frame, GPU 100%). Tracy showed
`GpuSP.BatcherFlush` GPU self-time ≈ 37 ms — but that was **mis-attributed**.
The dynamic prop shadow caster `GpuStaticPropBatcher::drawDynamicPropShadows()`
(`gos_static_prop_batcher.cpp:7306`, called `txmmgr.cpp:2339`) had **no GPU
Tracy zone**, so its cost drained into the next GPU-timestamped zone
(`GpuSP.BatcherFlush`) — the same first-GPU-zone attribution trap documented in
the foliage-impostor handoff.

The caster is the real cost. `getDynamicPropShadowInstances()`
(`gos_static_prop_registry.cpp:1396`) walks the **entire ~14K-recipe registry**
and emits every non-building prop (only skipping `noShadow` impostors) into the
dynamic shadow map **every frame** — no light-box cull, no LOD, no distance cull.
Each caster draws its full geometry (e.g. all 27 leaf-card packets of the lush
tree) into the shadow map. The Lane A color depth-prepass does **not** touch the
shadow pass, which is why enabling it "looked the same and didn't help close up"
(color-pass parity held; the shadow pass is a different, dominant cost).

Two coupled defects:

1. **No caster cull** — thousands of off-map trees draw shadows that never land
   in the (camera-fit, often small) dynamic shadow map.
2. **Mis-centered light box** — `buildDynamicLightMatrix()`
   (`gos_postprocess.cpp:2477`) centers the shadow box on the **AABB centroid of
   the camera frustum corners**. For an oblique near-ground camera the far/horizon
   corners (far plane ~61,555 WU) drag the centroid off-map (measured box
   `worldCenter=(-17131,-35844,7526)` while the scene sits at ~`(±300,3349,635)`),
   and near map edges the center oscillates → "shadows reflect off the map edge."
   `SHADOW-UNIFIED-PROJECTION-1` fixed the *radius* fit; the *center* was never
   fixed. `MC2_SHADOW_BOUNDED_NEAR_FIT` caps radius, not center.

The cull culls *against* the light box, so a mis-centered box makes the cull wrong
too — the two fixes are coupled and ship together.

## Solution

### SHADOW-FOCUS-CENTER-1 (projection fix)
Center the shadow box on the camera's **near-ground focus point** instead of the
frustum-AABB centroid. In `buildDynamicLightMatrix`, after the light basis is
built and corners projected to light space:

- Identify near vs far corner sets **by spread** (perspective near corners are
  closer together; smaller max-corner-to-centroid distance = near). Do NOT assume
  index order. (Measured: nearSpread 61 WU vs farSpread 27983 WU — unambiguous.)
- `viewDir = normalize(farCenter − nearCenter)`;
  `focusWorld = nearCenter + viewDir · focusDist` (focusDist =
  `MC2_SHADOW_FOCUS_DIST`, default 1500 WU, clamped [256, 8000]).
- Override the light-space center `cxL/cyL` with the focus projection
  (`cxL = right·focusWorld`, `cyL = up·focusWorld`), BEFORE the texel-snap and
  camZ back-projection. **Keep the radius fit unchanged** (only center moves).

Gate `MC2_SHADOW_FOCUS_CENTER`, default OFF. **Proven:** box `worldCenter` moved
from Z=7526 (off-ground, off-map) to Z=84 (near ground, tracking camera focus);
with focus on, the cull keeps 5466/5674 casters (box now lands on the scene) vs
100%-cull when the box was off-map.

**Orthogonality:** focus-center fixes *position*, bounded-near-fit fixes *size*,
the cull drops *off-box* casters. All three compose; the full close-up win needs
all three:
```
MC2_SHADOW_FOCUS_CENTER=1  MC2_SHADOW_BOUNDED_NEAR_FIT=1  MC2_SHADOW_CASTER_LIGHTBOX_CULL=1
```

### SHADOW-CASTER-LIGHTBOX-CULL-1 (caster cull)
Per-frame in `txmmgr.cpp`, between the caster-set build (`getDynamicPropShadow-
Instances`, ~2335) and the draw (~2339): filter `s_dynPropInsts` to only casters
whose shadow lands in the dynamic shadow map, pass the culled list to the draw.

Cull test (bulletproof — uses the exact shadow projection): transform each
caster's world center through `getDynamicLightSpaceMatrix()` → clip → NDC; keep if
`clip.w > 0` and `NDC.xy ∈ [−1−margin, +1+margin]` (margin =
`MC2_SHADOW_CASTER_CULL_MARGIN`, default 0.25). Caster world position
= `(−M[3], M[11], M[7])` column-major (3× cross-verified vs `shadow_static_prop.vert`,
the GPU-cull `worldCenter` extraction, and the `SHADOWZRANGE` probe). Reuses a
static culled vector (no per-frame heap churn). `Shadow.CasterCull` CPU zone;
`MC2_SHADOW_CULL_DEBUG=1` logs `[SHADOW_CULL] casters N→M`.

Gate `MC2_SHADOW_CASTER_LIGHTBOX_CULL`, default OFF. Margin generous (err toward
keeping casters — a missing shadow is worse than a wasted draw).

### Instrumentation
`GpuSP.DynShadowDraw` GPU Tracy zone around the caster draw (`txmmgr.cpp:2339`)
deconflates the shadow-caster GPU cost from `GpuSP.BatcherFlush`.

## Validation

- **Smoke (mc2_24):** all gates off, and each gate on — PASS, +0 destroys,
  GL-clean.
- **Projection proof:** `MC2_SHADOW_FRUSTUM_DIAG` worldCenter moved off-map→on-ground.
- **Caveat — smoke camera is degenerate** for this: the fixed start camera looks
  across the map, so even with focus-center the absolute box can sit off the
  battlefield. The *relative* move (toward camera focus, onto the scene) is the
  proof. **Real validation needs a gameplay battlefield camera** (user, or
  desktop-driven): confirm shadows track the camera (no edge-reflection), the box
  centers on the visible trees, and `N→M` drops with the near trees kept.

## Rollout

All three gates default-OFF. After gameplay A/B confirms (shadows sensible + GPU
win + no near-shadow loss): flip `MC2_SHADOW_FOCUS_CENTER`,
`MC2_SHADOW_BOUNDED_NEAR_FIT`, `MC2_SHADOW_CASTER_LIGHTBOX_CULL` default-ON. Tune
`MC2_SHADOW_FOCUS_DIST` for the dense-shadow region distance.

## Risks / follow-ups

1. **Cull frame correctness** — wrong world-pos frame would keep the wrong trees
   (near shadows vanish). Mitigated by 3× cross-verification + `MC2_SHADOW_CULL_DEBUG`.
2. **Focus-center radius still full-frustum** — focus-center alone re-centers but
   leaves a large box; pair with bounded-near-fit for dense near shadows.
3. **Cheap-LOD casters (follow-up):** in-box casters still draw full geometry into
   the shadow map. Feeding reduced-LOD geometry would cut shadow-map fill further,
   but is entangled with the per-LOD registration / lighting-ownership work — defer.
4. **Proper stable CSM (follow-up):** if focus-center centering proves insufficient
   on fast camera motion, a texel-snapped cascaded fit is the next step.

## Commits
- `b519f242` — `GpuSP.DynShadowDraw` GPU zone (measure-first instrument)
- `19f3a08f` — light-box caster cull (gated)
- `647334cb` — focus-center projection fix (gated)
