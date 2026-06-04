# Design: Foliage Depth-Prepass Overdraw Meta-Fix (Lane A)

**Date:** 2026-06-04
**Branch:** `claude/model-override-system-recon-1`
**Goal:** Collapse alpha-test foliage overdraw (~33× shading per pixel) to ~1× by
adding a camera depth-prepass for static props, so the expensive lighting/color
fragment shader runs once per visible pixel instead of once per overlapping
leaf-card layer.

---

## Problem

Static props (trees, buildings) render through `GpuStaticPropBatcher::flush()`
(`gos_static_prop_batcher.cpp:4795`) with a single shaded pass using
`shaders/static_prop.frag`. Foliage materials carry `ALPHA_TEST_BIT` and the
fragment shader does:

```glsl
// static_prop.frag:215
if (... (materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) discard;
```

`discard` defeats early-Z on the GPU: every one of the ~33 overlapping leaf-card
layers (measured depth-complexity `dc_mean=33.2`, per the foliage-impostor cook
metric) executes the **full lighting/color shader** — vertex-interpolated
lighting multiply, optional PBR Schlick-Fresnel lobe, optional SH-L2 ambient IBL,
fog, object-id write — before the discard throws the fragment away. The shading
cost scales with depth complexity, not with visible pixels. This was the
multi-second `Render.GpuStaticProps` bottleneck the impostor pivot worked around
for the FAR field; the prepass fixes it for the NEAR/MID field where real
geometry must render.

This is the standard UE/Unity masked-foliage cost, and the standard fix is a
depth-prepass — which this engine does **not** currently have for the camera
(only a shadow depth-prepass exists, `gos_BeginShadowPrePass`, `txmmgr.cpp:2153`).

---

## Approach

Two-pass render for static props:

1. **Depth prepass** — draw all static props depth-only (no lighting, no color).
   Fragment shader does *only* the alpha-test discard, then writes depth.
   State: `depthTest=GL_GEQUAL`, `depthWrite=ON`, no color attachment writes.

2. **Color pass** (existing `flush()`) — same geometry, but
   `depthTest=GL_EQUAL`, `depthWrite=OFF`. Early-Z now rejects every fragment
   whose depth does not exactly match the front-most depth laid down by the
   prepass → the lighting shader runs ~1× per pixel regardless of leaf layering.

### Why this is correct under reverse-Z

This engine uses **reverse-Z with `GL_GEQUAL`** (confirmed:
`gos_static_prop_batcher.cpp:4955-4957, 5016-5017` — "DepthFunc::GreaterEqual
encoded in the table row", "GL_GEQUAL (reverse-Z)"). Near plane = 1.0, far = 0.0;
the greatest depth value wins. The prepass lays down the nearest (greatest) depth
per pixel. The color pass with `GL_EQUAL` keeps only fragments whose depth equals
that stored value — i.e. exactly the front-most surviving (non-discarded)
fragment per pixel.

**The load-bearing invariant:** the prepass and the color pass must produce
**bit-identical `gl_Position`** and make the **identical alpha-test decision**
for every fragment. If depth differs by one ULP, `GL_EQUAL` rejects everything
and the props vanish. We guarantee this by:

- **Reusing the existing `static_prop.vert` verbatim** for the prepass (same UBO,
  same instance SSBO, same transform math) so depth is identical by
  construction. The wasted per-vertex lighting in the VS is acceptable — vertex
  cost is negligible against the fragment overdraw we are removing. (A leaner
  depth-only VS is a possible later optimization but is NOT in this spec — it
  would reintroduce the bit-identical-position risk.)
- **Sampling the same texture with the same UV and the same `< 0.5` threshold**
  in the prepass fragment shader, so the discard set is identical.
- Adding `invariant gl_Position;` to the shared vertex shader as belt-and-braces
  against compiler reordering across the two programs.

### Scope of the prepass draw set

The prepass and color pass MUST cover the **same draw set in symmetric fashion**
(every prop drawn in color must have its depth in the prepass, or `GL_EQUAL`
fails). Opaque static props (buildings) already early-Z fine in a single pass,
but including them in the prepass is harmless (cheap depth-only) and keeps the
two passes symmetric. **Decision: prepass ALL static props**, color pass switches
to `GL_EQUAL`/no-write for all. This avoids a fragile "opaque on GEQUAL, alpha on
EQUAL" split.

---

## Components

### New shader: `shaders/static_prop_depth.frag`
- Inputs: same texture binding (`u_texArr` coalesce / `u_tex` legacy), same
  `materialFlags` source (PerDrawEntry SSBO slot 4 coalesce / `u_materialFlags`
  legacy), same UVs as `static_prop.frag`.
- Body: sample alpha; `if ((materialFlags & ALPHA_TEST_BIT) != 0 && a < 0.5)
  discard;` — identical predicate to `static_prop.frag:215`. No color output, no
  lighting, no object-id write.
- Pairs with the existing `static_prop.vert` (unchanged, reused).

### Pipeline entry: `StaticPropDepth`
- New `RenderCore::PipelineId::StaticPropDepth` row: program =
  `static_prop.vert` + `static_prop_depth.frag`; depth `GL_GEQUAL`, depthWrite
  ON; color mask OFF (or no color attachment); cull as color pass.
- The color pass pipeline (`StaticPropOpaque`) gets a variant or runtime override
  to `GL_EQUAL` + depthWrite OFF when the prepass is active.

### New method: `GpuStaticPropBatcher::flushDepthPrepass()`
- Mirrors `flush()`'s draw dispatch (same VAO `s_sharedVao`, same IBO, same
  instance/PerDrawEntry SSBO bindings, same indirect buffer + GPU-computed counts
  from `compute_dispatch()`), but binds the `StaticPropDepth` pipeline and the
  depth-only program. Structurally closest to the existing `flushShadow()`
  (`gos_static_prop_batcher.cpp:6715`), which already does a depth-only static-prop
  draw — but for the CAMERA matrices/UBO, not the light-space matrix.

### Wiring: `txmmgr.cpp` `Render.GpuStaticProps` block (~2490-2591)
Current order: `batcher_prepareBaseInstanceTable()` → `compute_dispatch()`
(writes indirect counts) → `flush()` (color).
New order (prepass active):
`prepareBaseInstanceTable()` → `compute_dispatch()` → **`flushDepthPrepass()`** →
`flush()` (color, now EQUAL/no-write).
The prepass runs AFTER `compute_dispatch()` because it consumes the same
GPU-authoritative indirect counts the color pass uses.

---

## Kill-switch & rollout

- Env gate `MC2_STATIC_PROP_DEPTH_PREPASS`, **default OFF** initially.
- Phase 1: implement, prove parity (color pass with EQUAL must render
  byte-comparable output to single-pass GEQUAL for a static frame — no vanished
  props, no Z-fighting). Validate on tier1 + mc2_24 (override trees).
- Phase 2: measure `Render.GpuStaticProps` GPU time with prepass on vs off
  (Tracy). Expect foliage-heavy frames to drop sharply; opaque-only frames to
  pay a small prepass tax (the symmetric-prepass cost). If opaque-only frames
  regress meaningfully, reconsider scoping the prepass to the alpha-ON bucket.
- Phase 3: flip default ON if parity holds and the win is real. `=0` remains the
  kill-switch.

## Measurement

- New Tracy zone `GpuSP.DepthPrepass` around `flushDepthPrepass()`.
- Headline metric: `Render.GpuStaticProps` GPU self-time on a near-foliage camera
  (the user's navigated mc2_24 view), prepass-on vs prepass-off.
- Correctness oracle: a static-camera frame screenshot diff (prepass-on vs
  prepass-off) — must be visually identical (lighting unchanged; only which
  fragments get *shaded* changes, not the result for the surviving fragment).

## Risks

1. **`GL_EQUAL` depth mismatch → invisible props.** Mitigated by reusing the
   exact vertex shader + `invariant gl_Position`. This is THE risk; parity test
   gates it.
2. **Prepass tax on opaque-only scenes.** Symmetric prepass costs one depth-only
   geometry pass even when overdraw is low. Measured in Phase 2; alpha-ON-only
   scoping is the fallback.
3. **Coalesce vs legacy draw paths.** `flush()` has three dispatch paths (v6
   per-packet default, v5, legacy multidraw — `gos_static_prop_batcher.cpp:5952/
   6128/6232`). The prepass must mirror whichever path is active so the draw set
   matches. Plan must handle the default (v6) path first; legacy as follow-up.
4. **Shadow pass interaction.** The shadow depth-prepass is independent
   (light-space matrices, separate FBO) and unchanged. No interaction expected.

## Out of scope (separate lanes/cycles)

- Lane B: low-poly solid-canopy tree assets + offline LOD chain bake.
- Lane C: camera-facing / octahedral impostor polish.
- Leaner depth-only vertex shader (position-only) — deferred; correctness-first
  reuses the full VS.
- HZB / GPU occlusion culling off the prepass depth (future: the prepass depth
  buffer is exactly the input an HZB cull would want — noted, not built here).
