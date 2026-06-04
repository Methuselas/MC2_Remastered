# Design: Static-Prop Depth-Prepass Overdraw Meta-Fix (Lane A)

**Codename:** `FOLIAGE-STATICPROP-DEPTH-PREPASS-1`
**Date:** 2026-06-04 (rev 2 — incorporates review patches)
**Branch:** `claude/model-override-system-recon-1`
**Goal:** General static-prop/foliage **overdraw** mitigation. Add a camera
depth-prepass so the expensive lighting/color fragment shader runs ~once per
visible pixel instead of once per overlapping surface layer. Success is measured
as **reduced color-pass shader invocations / GPU time — NOT triangle count.**

---

## Problem (reframed)

Static props (trees, buildings) render through `GpuStaticPropBatcher::flush()`
(`gos_static_prop_batcher.cpp:4795`) in a single shaded pass using
`shaders/static_prop.frag`. The fragment shader runs the full lighting/color
computation (vertex-interpolated lighting multiply, optional PBR Schlick-Fresnel
lobe, optional SH-L2 ambient IBL, fog, object-id write) for **every rasterized
fragment**, then either keeps it or (for `ALPHA_TEST_BIT` foliage) discards it:

```glsl
// static_prop.frag:215
if (... (materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) discard;
```

Two overdraw regimes, both fixed by a depth-prepass:

1. **Alpha-test foliage (old lush / stock alpha-card trees).** `discard` defeats
   early-Z → all ~33 overlapping leaf-card layers (cook `dc_mean=33.2`,
   `dc_max=246`) execute the full lighting shader before being thrown away.
2. **Solid-canopy foliage (new trees9 LOD chain, Lane B).** Opaque leaf clusters,
   no alpha-test — but static props are **not sorted front-to-back**, so the
   GPU's in-pass early-Z still shades a large fraction of the ~16 overlapping
   opaque layers (`dc_mean≈16`, `dc_max≈67`) before they're depth-overwritten.

Lane B's asset fix (49× fewer triangles, impostor far-LOD) proves **mesh LOD
alone does not reduce canopy depth complexity** — dc_mean stays ~16 as triangles
drop. The depth-prepass is the **engine-side** complement that attacks the
overdraw multiplier directly, for the near/mid range where real geometry must
remain visible. This engine has **no camera depth-prepass** today (only a shadow
depth-prepass, `gos_BeginShadowPrePass`, `txmmgr.cpp:2153`).

---

## Approach

Two-pass static-prop render:

1. **Depth prepass** — draw static props depth-only (no lighting, no color, no
   object-id). Fragment shader does *only* the alpha-test discard, then writes
   depth. State: `depthTest=GL_GEQUAL`, `depthWrite=ON`, `glColorMask(0,0,0,0)`.
2. **Color pass** (existing `flush()`) — same geometry, `depthTest=GL_EQUAL`,
   `depthWrite=OFF`. Early-Z rejects every fragment whose depth ≠ the front-most
   depth from the prepass → the lighting shader runs ~1× per visible pixel.

### Why this is correct under reverse-Z

This engine uses **reverse-Z with `GL_GEQUAL`** (confirmed
`gos_static_prop_batcher.cpp:4955-4957, 5016-5017`). Near=1.0, far=0.0; greatest
depth wins. Prepass lays down the nearest (greatest) depth per pixel; the color
pass with `GL_EQUAL` keeps only the fragment whose depth equals that stored value
— exactly the front-most non-discarded fragment.

### The load-bearing invariant

Prepass and color pass MUST produce **bit-identical `gl_Position`** and the
**identical alpha-test decision** per fragment, or `GL_EQUAL` rejects everything
and props vanish. Guaranteed by:

- **Reusing `static_prop.vert` verbatim** for the prepass (same UBO, same
  instance SSBO, same transform) so depth is identical by construction. Wasted
  per-vertex lighting in the VS is acceptable (vertex cost ≪ removed fragment
  overdraw). A position-only depth VS is explicitly **out of scope** — it would
  reintroduce the bit-mismatch risk.
- Adding **`invariant gl_Position;`** to the shared vertex shader against
  cross-program compiler reordering.
- **Identical alpha predicate**: same texture, same UV, same `< 0.5` threshold,
  same `materialFlags` source.

### GL_EQUAL fragility checklist (must be identical across both passes)

`GL_EQUAL` is fragile even with an identical VS if **any** of these differ
between prepass and color pass. The plan must assert each is identical or
unchanged:

- clip control (depth range / reverse-Z convention)
- polygon offset (must be OFF or identical in both)
- cull mode (front/back/none)
- alpha predicate (threshold + which materials test)
- texture sampling state (filter, wrap, sampler bindings)
- mip / LOD selection (same LOD bias; same derivatives ⇒ same VS+UV)
- discard threshold (`0.5`)
- shader `#define`s / preprocessor variants
- debug addr mode (`u_debugAddrMode` — bypasses discard at value 8; must match)

### Draw-set scope — TWO MODES

The prepass and color pass must cover the **same visible draw set symmetrically**
(every prop shaded in the color pass must have its depth in the prepass, else
`GL_EQUAL` fails). Two implementable modes:

**Mode A — prepass ALL static props (correctness baseline, build first).**
Prepass every prop depth-only; color pass switches ALL props to
`GL_EQUAL`/no-write. Simplest to prove (one uniform state flip). Pays one
depth-only geometry pass even on opaque-only scenes.

**Mode B — prepass alpha-ON only (fallback, if Mode A regresses opaque scenes).**
```
prepass:  alpha-ON props only, depth-only, GEQUAL + write
color:    opaque props → GEQUAL + depthWrite ON (unchanged, single-pass)
          alpha  props → EQUAL  + depthWrite OFF (consumes prepass depth)
```
Mode B avoids the prepass tax on buildings but needs the color pass to split
state by alpha bucket (the draw lists already separate `s_alphaOffCmdCount` /
`s_alphaOnCmdCount`, `gos_static_prop_batcher.cpp:6232/6267/6288`).

**Decision:** implement Mode A first (provable), keep Mode B as a gated variant
selected only if Phase-2 measurement shows opaque-only regression.

### Pass order (unchanged from rev 1, confirmed correct)

```
batcher_prepareBaseInstanceTable()        // txmmgr.cpp:2511
gpu_cull::compute_dispatch()              // :2528  — writes GPU indirect counts
GpuStaticPropBatcher::flushDepthPrepass() // NEW    — consumes same counts
GpuStaticPropBatcher::flush()             // :2578  — color, EQUAL/no-write
```
The prepass runs AFTER `compute_dispatch()` so it draws the exact GPU-authoritative
visible set the color pass draws.

---

## Components

### New shader: `shaders/static_prop_depth.frag`
- Mirrors `static_prop.frag`'s **alpha predicate and BOTH texture paths** —
  coalesced (`u_texArr` + `PerDrawEntry.materialFlags` SSBO slot 4, indexed by
  `gl_DrawIDARB`) **and** legacy (`u_tex` + `u_materialFlags` uniform). It must
  handle whichever path the active dispatch uses (see Risk 3).
- Body: sample alpha; `if (u_debugAddrMode != 8 && (materialFlags &
  ALPHA_TEST_BIT) != 0 && a < 0.5) discard;` — byte-identical predicate to
  `static_prop.frag:215`. **No color outputs, no lighting, no object-id write.**
- Pairs with the unchanged `static_prop.vert`.

### Pipeline entry: `RenderCore::PipelineId::StaticPropDepth`
- Program = `static_prop.vert` (+ `invariant gl_Position`) + `static_prop_depth.frag`.
- Depth `GL_GEQUAL`, depthWrite **ON**, `glColorMask(GL_FALSE×4)`, cull = color pass.
- **Keep the same FBO/attachments bound** — do NOT alter framebuffer attachment
  setup. Depth-only is achieved with color mask, not attachment surgery, then the
  color mask is restored before `flush()`.

### Color-pass state override (gated)
- When the prepass is active, the color pass (`StaticPropOpaque` path) overrides
  to `depthFunc=GL_EQUAL`, `depthWrite=OFF` via the pipeline row/state override.
  **Explicitly validate both** (a color pass left on GEQUAL+write would silently
  "work" but isn't the intended state and defeats the early-Z benefit).
- **Object-ID write is PRESERVED in the color pass** — the surviving (front-most)
  fragment must still write its object id. Only the *prepass* skips object-id.

### New method: `GpuStaticPropBatcher::flushDepthPrepass()`
- Mirrors `flush()`'s **default v6/coalesced** dispatch first (same VAO
  `s_sharedVao`, IBO, instance/PerDrawEntry SSBO bindings, same indirect buffer +
  GPU counts), binding the `StaticPropDepth` pipeline. Structurally closest to
  `flushShadow()` (`gos_static_prop_batcher.cpp:6715`).
- **If a legacy/unsupported dispatch path is active and not mirrored, SKIP the
  prepass and log** (color pass stays single-pass GEQUAL — correct, just
  unoptimized). Never run a prepass that draws a different set than the color pass.

### Wiring: `txmmgr.cpp` `Render.GpuStaticProps` block (~2490-2591)
Insert `flushDepthPrepass()` between `compute_dispatch()` and `flush()` as above,
under the gate.

---

## Implementation shape (tasks)

1. **`static_prop_depth.frag`** — same alpha predicate, both texture paths, no
   color/lighting/object-id outputs.
2. **`StaticPropDepth` pipeline** — same VS + `invariant gl_Position`; GEQUAL;
   depthWrite ON; colorMask OFF; same FBO.
3. **`flushDepthPrepass()`** — mirror default v6/coalesced draw path first; skip
   + log if active path is legacy/unsupported.
4. **Color-pass state override under gate** — GL_EQUAL, depthWrite OFF; restore
   color mask; preserve object-id write.
5. **Gate** `MC2_STATIC_PROP_DEPTH_PREPASS=1`, **default OFF**. (Reserve a Mode-B
   selector env for the alpha-only variant if needed.)
6. **Instrument** Tracy zones `GpuSP.DepthPrepass` and `GpuSP.ColorAfterPrepass`.
7. **Validate** (gate below).

---

## Validation gate (before default-ON)

- **GL_EQUAL parity (THE gate):** static-camera frame, prepass-ON vs prepass-OFF.
  Prefer exact byte screenshot diff; if temporal effects make exact diff too
  strict, use visual diff **plus an explicit missing-prop / vanished-geometry
  check** (no prop may disappear).
- **mc2_24 near foliage** (override trees) — both old alpha-card AND new trees9
  solid-canopy foliage in view.
- **mc2_01 baseline.**
- **Opaque-heavy / low-AlphaOn scene** (if available) — Mode A's prepass tax. If
  it regresses meaningfully, keep default-OFF or ship Mode B (alpha-only).
- **Object picking / object-ID** validation if the object-id buffer is active
  (surviving fragment still writes correct id).
- **tier1 +0 destroys, GL-clean** (0 GL errors).
- **Tracy GPU comparison:** `Render.GpuStaticProps` color-pass GPU self-time and
  shader-invocation proxy, prepass-ON vs OFF, on a near-foliage camera. Headline
  success = fewer color-pass invocations / lower GPU time (NOT triangle count).

## Rollout

Default-OFF → prove GL_EQUAL parity → measure (Phase 2) → flip default-ON if
parity holds and the win is real and no opaque regression (else ship Mode B or
stay alpha-only). `=0` remains the kill-switch.

## Validation results (2026-06-04)

**Implemented + code-reviewed.** Commits `a5461d80..98a39c77`. Mode A (prepass all
static props). Gate `MC2_STATIC_PROP_DEPTH_PREPASS`, default OFF.

- **GL_EQUAL parity: HOLDS.** Build clean; gate OFF and ON both smoke-PASS,
  GL-clean, +0 destroys on mc2_24 + mc2_01. User confirmed navigated-camera visual
  parity — trees render identically ON vs OFF, nothing vanished. The depth program
  is built in the active **coalesce** variant; `invariant gl_Position` + identical
  alpha predicate (materialGpuSample/slot-5 SSBO/debugAddrMode all uploaded to the
  depth program — code review caught 2 CRITICAL parity bugs here that would have
  vanished override-textured trees in a navigated view; fixed).
- **Performance: prepass was NOT the close-up bottleneck.** User Tracy on a
  near-foliage navigated view: enabling the prepass "looked the same and didn't
  help close up." Root cause — the dominant close-up GPU cost is the **dynamic prop
  SHADOW caster** (`drawDynamicPropShadows`), not the color pass. The shadow caster
  had no GPU Tracy zone, so its cost was mis-attributed to `GpuSP.BatcherFlush`
  (first-GPU-zone attribution). The color depth-prepass is correct and parity-sound
  but addresses the wrong pass for this scene. See the Lane-D design
  (`2026-06-04-shadow-caster-cull-and-projection-design.md`) for the shadow fix.
- **Disposition:** keep `MC2_STATIC_PROP_DEPTH_PREPASS` **default-OFF**. It remains
  a correct tool for genuinely color-overdraw-bound scenes (dense alpha foliage
  without a dominating shadow pass), but it is not the current limiter. Re-measure
  once the Lane-D shadow caster is culled (then the color pass may become the next
  visible cost and the prepass may pay off).

## Risks

1. **`GL_EQUAL` depth/state mismatch → invisible props.** THE risk. Mitigated by
   verbatim VS reuse + `invariant gl_Position` + the fragility checklist; gated by
   the parity test.
2. **Prepass tax on opaque-only scenes** (Mode A). Measured Phase 2; Mode B is the
   fallback.
3. **Coalesce vs legacy draw paths.** `flush()` has three dispatch paths (v6
   per-packet default, v5, legacy multidraw — `gos_static_prop_batcher.cpp:5952/
   6128/6232`). The prepass MUST mirror the active path or skip+log. Implement v6
   (default) first.
4. **Color mask / FBO state.** Use `glColorMask` (or pipeline equivalent) and
   restore it; do NOT change FBO attachments in this slice.
5. **Object-ID buffer.** Prepass must not write it; color EQUAL pass must still
   write it for the surviving fragment. Validate if active.
6. **Shadow pass interaction.** Shadow depth-prepass is independent (light-space
   matrices, separate FBO) — unchanged, no interaction expected.

## Strategic note — complements, does not replace

```
far foliage:      impostor / billboard / HLOD        (Lane B + Lane C)
near-mid foliage: depth prepass for masked/solid overdraw   (THIS, Lane A)
asset cook:       tighter cards / better alpha coverage     (Lane B)
```
The prepass is the engine-side fix for the "real geometry must remain visible"
range. Impostors remain the representation fix for far trees. Lane B's solid
canopy + impostor chain and Lane A's prepass stack — they do not substitute.

## Out of scope (separate lanes/cycles)

- Lane B: low-poly solid-canopy tree assets + offline LOD chain bake (prototyped).
- Lane C: camera-facing / octahedral impostor polish.
- Position-only depth VS (correctness-first reuses the full VS).
- HZB / GPU occlusion culling off the prepass depth (the prepass depth buffer is
  exactly an HZB cull's input — noted, not built here).
