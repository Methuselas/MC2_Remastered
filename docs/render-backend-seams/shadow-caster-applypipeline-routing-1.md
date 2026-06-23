# SHADOW-CASTER-APPLYPIPELINE-ROUTING-1

**Status:** SHIPPED. Behavior-bearing. Routes the shadow-caster fixed-function
state through `pipeline_binder::applyPipeline`, driven by the registered
`ShadowTerrain` / `ShadowMech` / `ShadowStaticProp` PipelineDesc rows. Option A:
the per-caster `polygonOffsetEnable` axis is now owned by the pipeline row, while
the offset MAGNITUDE (factor/units) and teardown stay exactly where they were.

## What changed (Option A)

- **`pipeline_binder.{h,cpp}`** — `applyPipeline` gains:
  - polygon-offset ENABLE/DISABLE from `desc.polygonOffsetEnable`
    (`glEnable/glDisable(GL_POLYGON_OFFSET_FILL)`). **Magnitude is NOT set here.**
  - an optional `dbgName` param + the `MC2_PIPELINE_BIND_TRACE` (default OFF)
    `[PIPELINE_BIND] <name> depth=… cull=… frontFace=… polygonOffset=…` line.
- **`gameos_graphics.cpp`** — the two LIVE shadow brackets replace their hand-set
  depth/cull/depthMask lines with `applyPipeline`:
  - `beginShadowPrePass` (static map: terrain + buildings) → `applyPipeline(ShadowTerrain)`
    (placed before the depth clear so `depthMask=GL_TRUE` precedes it).
  - `beginDynamicShadowPass` (dynamic map: props + mech) → `applyPipeline(ShadowMech)`.
- **`gos_static_prop_batcher.cpp`** — the two prop shadow draw-sites replace the
  bare `glEnable(GL_POLYGON_OFFSET_FILL)` with `applyPipeline(ShadowStaticProp)`
  (`polygonOffsetEnable=true`). The existing `glPolygonOffset(factor,units)` and
  the teardown `glDisable(GL_POLYGON_OFFSET_FILL)` are UNTOUCHED — the offset
  lifetime pairing (enable → magnitude → draw → disable) is preserved.
- **`scripts/run_smoke.py`** — added `MC2_PIPELINE_BIND_TRACE` to the subprocess
  env whitelist (Popen replaces env; vars not listed are dropped).

`glProgramName=0` on the shadow rows → `applyPipeline` skips `glUseProgram`, so
the bound shadow program is untouched. `glColorMask`, FBO/viewport/clear, and
`lightSpaceMatrix` upload all stay hand-set (not modeled by PipelineDesc).

The dormant `gos_postprocess.cpp` shadow brackets (no callers) were left alone.

## Why it is a no-op in effect
Every state `applyPipeline` now sets in the shadow path equals what the brackets
set by hand (depth test+write, `GL_LESS`, cull none). The extra calls are no-ops:
`glFrontFace(GL_CCW)` (global already CCW), `glDisable(GL_BLEND)`+`glBlendFunc`
(irrelevant under depth-only/colorMask-off), and `glDisable(GL_POLYGON_OFFSET_FILL)`
for the Terrain/Mech brackets (offset already off there). Post-pass
`restoreShadowGLState` is unchanged and leak-neutral; the prop teardown still
disables offset, so it ends OFF after the pass.

## Verification (gated as a render-risk slice)

- **Full build green** (mc2 + launcher, isolated worktree).
- **Trace** (`MC2_PIPELINE_BIND_TRACE=1`, mc2_24+mc2_01) shows exactly:
  - `ShadowTerrain depth=Less cull=None frontFace=Ccw polygonOffset=false`
  - `ShadowMech depth=Less cull=None frontFace=Ccw polygonOffset=false`
  - `ShadowStaticProp depth=Less cull=None frontFace=Ccw polygonOffset=true`
- **No GL errors** in the smoke logs.
- **Smoke PASS 2/2** (mc2_24 + mc2_01); shadows actively render
  (`[SHADOW_CULL] casters 598→162`, shadow shaders loaded, "rendering static
  shadows"); no crash.
- **Visual before/after (byte-hash, fixed-clock deterministic), mc2_24:**
  all 3 bookmarks **PIXEL-IDENTICAL** before (pre-routing) vs after (routed) —
  `overview_center b11ff22a`, `ridge_lowangle e0fb9cc8` (**shadow_cascade**),
  `highangle_wide c6df715e` (**static_prop_pbr**). Byte-identical ⇒ no missing
  shadows, no acne/peter-panning change, no caster winding/cull disappearance,
  static-prop + terrain shadows unchanged.
- **Polygon-offset leak proof:** the prop teardown `glDisable` (intact at
  gos_static_prop_batcher.cpp 7686 / 7850) keeps offset disabled after the prop
  section; `restoreShadowGLState` does not re-enable it; the bracket
  `applyPipeline` also disables it at bracket entry. Offset ends OFF; the
  byte-identical capture confirms no acne/offset bleed.

## Explicit exclusions (held)
No factor/units in PipelineDesc · no dormant postprocess bracket changes · no
legacy CPU shadow path · no colorMask/FBO/viewport/clear changes · no program
bind changes · no shadow-bias changes.

## Next
The rasterState axis is now fully authoritative AND applied for the registered
set. Future: SPIR-V for the shadow programs (depth-parity gate); or onboard the
remaining ad-hoc passes (water/vfx) to PipelineDesc.
