# VIEWPORT-CLASS-OWNERSHIP-RECON-1 (banked)

Read-only recon, banked. Source-verified vs nifty `69a738b5`.

## Findings

- **33 `glViewport` call sites; ZERO `glScissor`/`GL_SCISSOR_TEST`** anywhere in GameOS/
  + mclib/. All clipping is via the viewport rect — simpler state model.
- **~12 viewport classes**, each clearly owned (gosPostProcess / gosRenderer / gosTerrainRender /
  util): `FullResScene` (sceneFBO_, reverse-Z), `FullResBackbuffer` (FBO 0), `Pillarbox43`
  (MC2_FORCE_43, `gos_Compute43Box`), `ShadowMapStatic/Dynamic/PostProcess`,
  `ShadowMapFullCascade`/`ArrayCascade` (CSM, forward-Z), `HalfResSSAO` (width/2),
  `HzbPyramidLevel_N` (halving), `DebugHzbQuad` (256²), `QuarterResWaterReflection`
  (width/4, `gos_terrain_indirect.cpp:3750`), `Custom2D` (`gl_utils.cpp:386`).
- **Every sub-res pass saves+restores viewport** (HZB `gos_postprocess.cpp:1464/1523`, SSAO,
  shadow prepass `gameos_graphics.cpp:6039/6379` full-state-struct, CSM `3886/3940`, water
  reflection `3734/3758`). Restore is paired.

## Leak risk (low but real)

- Sub-res restore relies on **paired begin/end** + flags (csmStateSaved_, shadow_prepass_active_).
  An early-return/throw between begin and end would leak a sub-res viewport (e.g. 4096² shadow)
  into the next full-res pass → geometry clipped to a corner. No RAII guarantee.
- **No FBO↔viewport dimension assert** — a viewport larger than the bound FBO renders silently
  clipped.
- Recon-1's "no shared-viewport assert between full/sub-res" is CONFIRMED: mitigated by
  explicit save/restore, not asserted.

## What a ledger/checker would govern + cleanest first target

Govern: viewport↔FBO dimension match; depth-clear↔Z-convention (forward-Z shadow must not
render into reverse-Z scene FBO and vice-versa); sub-res scope bracketing; pillarbox remap
consistency (endScene + input use the same `gos_Compute43Box`).

**Cleanest first hardening (≤50 lines, zero perf):**
1. A `ViewportGuard` RAII (save in ctor, restore in dtor) adopted by every sub-res pass —
   makes a skipped-restore impossible. Highest impact.
2. A gated `MC2_DEBUG_VIEWPORT` assert that the bound FBO ≥ viewport size (catches silent
   clip).
3. (optional) viewport-class trace via the existing `render_frame_plan` framework.

## Verdict
Well-segregated already; the value is the RAII leak-guard + the FBO-dim assert (defensive,
gated, byte-identical). Safe but modest — defer behind shader-variant; pairs naturally with
a future GL-state-guard adoption pass.
