# TEXTURE-UNIT-OWNERSHIP-RECON-1 (banked)

Read-only recon, banked. Source-verified vs nifty `69a738b5`.

## Per-pass texture-unit map (condensed)

- **Terrain:** unit 0 colormap; 4 transition-mask-array (legacy); 5-8 matNormal0-3; 12 snow.
- **Mech (`gos_mech_batcher.cpp:2136+`):** 0 base; 1 PBR detail-normal; 2 PBR detail-ORM;
  3 paint-normal; 4 paint-ORM; 6 imported-AO; 7 imported-normal.
- **Static-prop:** 0 albedo 2D_ARRAY; 1 ORM 2D_ARRAY.
- **Post-fx (`gos_postprocess.cpp`):** depth/normal/scene-color/shadow rotate through units
  0-4 per pass (box-decal 0=depthCopy,1=normal; ssao 0=depth,1=normal; screenShadow
  0=depth,1=normal,2=staticShadow,3=dynArray,4=fullMap; composite 0=sceneColor,2=objectId).
- **Water (`gameos_graphics.cpp:3500+`):** 0 base; 1 detail; 2 reflRT; 3 HDRI (1-3 saved/restored).
- **Particle (`gos_particle_bridge.cpp`):** 0 tex; 1 depthCopy(soft); 2 sceneColorCopy(distort).

## What's already governed (the honest part)

~70% covered:
- **`check-sampler-bindings.py`** governs sampler NAME→unit (hard-anchors: `uSrc`/`sceneTex`/
  `ssaoTex`→0). Catches the hardest drift class.
- **`GlScopedTextureUnit`** (gl_state_guard.h) + manual prev-tex/sampler save/restore in
  mech/particle/water/composite cover unit-leak.
- Units 0-4 are intentionally **multiplexed** across sequential passes (re-bound each pass) —
  expected, not a conflict.

## The real gaps (lower-value than expected)

1. **2D-vs-2D_ARRAY target mixing on a unit** — unit 3 holds `GL_TEXTURE_2D_ARRAY` (CSM-on
   dynamic shadow) in one pass and `GL_TEXTURE_2D` (mech paint-normal / water HDRI) in
   another. Safe only via per-pass re-bind + the `glActiveTexture(0)`-at-exit convention; no
   static check. A target-type mismatch on a unit is a real GL hazard class.
2. **Unit 5 shared** between terrain matNormal0 and mine-sprite array (different shaders;
   safe iff passes don't co-occur — unverified).
3. No generated per-pass occupancy table (prose/grep only).

## What a ledger/checker would govern + verdict

A `scripts/map-texture-units.py` could emit a per-pass `unit → {texture, target, sampler}`
ledger and flag 2D/2D_ARRAY target mismatches on multiplexed units. **Verdict: LOWER value
than the other seams** — the hardest drift (name→unit) is already checked, leaks are
guarded, and multiplexing is by-design. Worth doing mainly for the target-type-mismatch
lint (gap #1) and to generate the Vulkan DescriptorSet occupancy map. Defer behind
shader-variant.
