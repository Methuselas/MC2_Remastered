# GPU-Driven Dynamic Sun Shadow (Frustum-Fit) - Design (rev 3)

Date: 2026-05-16
Status: DESIGN (plan-stage gates closed), rev 3. Slice for VPL-#10
artifact B and the mech/building shadow-feed gap.
Branch: claude/gpu-driven-rendering

## rev 3 plan-stage gate resolution (supersedes conflicting rev-2 text)

- GATE 1 (off-frustum casters) = PRE-WINDOWED for BOTH batchers
  (grep-proven: mech submit gated `inView`+`mechGpuCullSkip`
  `mech3d.cpp:2453/2505`; building gated `objBlockInfo/objVertexActive`
  `objmgr.cpp:1703` + `inView` `bdactor.cpp:1648`; buckets cleared+rebuilt
  per frame from camera-admitted submissions only). The GPU 0.08
  frustum dilation is DOWNSTREAM + default-off (`MC2_GPU_CULL_LIFECYCLE`
  unset), does NOT widen the caster buckets. No low-blast-radius lever
  exists. **RESOLUTION (user): PHASED.** This slice is **Phase 1** =
  flushShadow over the camera-visible (`inView`) caster set + frustum-
  fit. Off-screen-caster shadows at low sun are a REAL documented gap
  (a +10deg-sun 100u building throws ~570u of shadow that can enter
  view from just off-screen). **Phase 2** = light-volume caster cull
  that admits casters whose shadow volume intersects the view WITHOUT
  touching the load-bearing `inView` cull cascade - a genuine,
  non-deferrable follow-up tracked separately, NOT an edge-case note.
- `Camera::clipToWorld` yields **Stuff space, not raw-MC2** (canonical
  `camera.cpp:1982-1984` applies `(-x, z, y)` AFTER). Component 1 MUST
  apply the `vec3(-s.x, s.z, s.y)` swizzle post-`clipToWorld` (same
  swizzle as `shadow_object.vert:14`) or it re-introduces artifact B.
  Multiply is row-vector: `xform.Multiply(ndcCoords, eye->clipToWorld)`,
  divide by `xform.w` (negate-if-w<0 per `camera.cpp:1979-1980`), then
  swizzle. `eye->clipToWorld` is a public `Stuff::Matrix4D` member
  directly readable at the `txmmgr.cpp` site.
- Mech ring-slot arithmetic: advance at `gos_mech_batcher.cpp:820`,
  write at `:827`, fence at `:1097`. At the new-region time (before
  this frame's mech `flush()`), `s_frameSlot` holds last frame's
  post-advance value and last frame's fenced data is at `s_frameSlot`
  itself. The rev-2 `(s_frameSlot-1+N)%N` formula is OFF; the correct
  read slot is `s_frameSlot` (re-derive + assert at execute, this is a
  plan-stage precision item, not an assumption).
- Elevation slab: no global terrain min/max constant exists; use a
  fixed conservative slab `[-512, 4096]` + caster margin `+512` world
  units; the REAL safety net is the map-bounds clamp `r =
  mapHalfExtent*sqrt(2)*1.05` (`gos_postprocess.cpp:1207`).
- No `gos_ShadowsEnabled()` wrapper: rely on `gos_BeginDynamicShadowPass()`
  internal `!pp->shadowsEnabled_` early-return (existing precedent,
  lower blast radius) rather than adding an accessor.
- New SSBO-reading shadow shaders use the `glsl_program::makeProgram`
  + explicit `"#version 430\n"` prefix path (like the color batcher
  programs), NOT `gosRenderMaterial::load` (exact loader confirmed
  with mc2-shader-expert at task time).

Symbols are authoritative; every file:line is a starting point to
re-grep at plan/execute time (Rule 0). Line numbers drift.

## Problem

Under the default config (`g_useGpuObjects` on, `g_useGpuMechs` on),
GPU-batched buildings and mechs cast **no sun shadow at all**:
`GpuStaticPropBatcher::flushShadow()` (`gos_static_prop_batcher.cpp`
~:3589) and `GpuMechBatcher::flushShadow()` (`gos_mech_batcher.cpp`
~:341) are empty stubs with zero callers. `g_shadowShapes[]` is fed only
by the legacy `TG_Shape::Render`->`addShadowShape` path (`tgl.cpp`
~:3064), which runs only on GPU-submit fallback - so it is empty in the
target config. Static FBO is terrain-only.

The dynamic light matrix is built from a legacy CPU-era shim
(`txmmgr.cpp` ~:1559-1574: camera->ground raycast + hand MC2/Stuff
conversion) feeding `buildDynamicLightMatrix` (`gos_postprocess.cpp`
~:1334-1432) with a fixed `xyRadius=2400`. That is VPL-#10 artifact B
(footprint not square, bounces off map edges, no camera tracking across
360deg az / +10..90deg el).

The static/dynamic split was a CPU-terrain-cost workaround now obsolete,
but the static path was just fixed (commit `0c421d1`, [0,1]
ZERO_TO_ONE) and works, so unification is OUT OF SCOPE (regression risk).

## Goal

Crisp, camera-tracked sun shadows for mechs/vehicles and
buildings/GPU-batched static props, auto-scaling resolution with zoom,
via the modern single-sun-pass GPU-driven design. No CPU readback, no
frame-ordering hazard, no substrate sync-stall.

## Non-goals (explicit)

- Static terrain shadow map: untouched (separate `staticLightSpaceMatrix_`
  / FBO, fixed by `0c421d1`).
- Unifying static+dynamic: future, not this slice.
- Trees / generic genactor props: gate-excluded (`tgl.cpp` ~:3060-3062);
  separate caster-gate slice.
- "Fixing" the up-hint basis flip to a continuous up vector: separable
  follow-up. BUT the existing `fabsf(fz) > 0.9f` guard
  (`gos_postprocess.cpp` ~:1392) is LOAD-BEARING at high sun elevation
  (fz->1 near +90deg el) and MUST be preserved verbatim in the rebuilt
  builder (sonnet MINOR-5).
- GPU-cull-driven focus: rejected (cull dispatch is after the shadow
  prepass; readback hits the ~6 ms substrate sync-stall).

## Design

Two coupled components, ONE catastrophic-axis slice, internally staged
as ordered commits (shaders -> matrix -> wiring/flushShadow -> probe) so
the new shaders are reviewable before integration. Soak waived ->
env-gated parity probes + the two completed mandated reviews +
code-proof.

### Component 1 - Frustum-fit sun ortho

Replace the `txmmgr.cpp` ~:1559-1574 shim and the fixed-extent body of
`buildDynamicLightMatrix`. Per frame, in raw-MC2 caster space (the space
mech `shapeToWorld` `tgl.cpp` ~:3064 and `calcDynamicShadow` worldPos
`shadow.hglsl` ~:95 use - NO hand conversion):

1. Unproject the 8 NDC frustum corners `{(-1,+/-1, zN/zF),
   (+1,+/-1, zN/zF)}` (zN=0, zF=1 for ZERO_TO_ONE depth) through
   **`Camera::clipToWorld`** (`camera.h` ~:139, set `camera.cpp` ~:2291)
   - the precomputed inverse of `worldToClip`, swizzle already folded,
   yields raw-MC2 world corners. (Chosen over 6-plane corner
   reconstruction per sonnet MAJOR-1: simpler, existing matrix, same
   space guarantee, no ill-posed plane-triple solve. NOT
   `inverse(terrainMVP)` - `clipToWorld` is `worldToClip.inverse()`
   with the swizzle folded, the artifact-B-safe path.)
2. Clamp corner elevations to a fixed conservative world elevation slab
   (MC2 map elevation range; concrete bounds determined at plan stage by
   grepping the block/elevation data range, fallback fixed bounds e.g.
   [-200, 1200] world units) - this caps the low-sun "far corners miss
   ground -> footprint explodes" degeneracy. NO new Terrain elevation
   API (sonnet/opus M2: no global min/max accessor exists; do not add a
   cross-module dependency on a catastrophic slice).
3. Take the AABB of the clamped corners; clamp it to map bounds using
   the static path's proven constant `r = mapHalfExtent * sqrt(2) *
   1.05` (`gos_postprocess.cpp` ~:1207; value via `setMapHalfExtent`
   `gos_postprocess.h` ~:64 - NOT the ctor `mapHalfExtent_(0.0f)`). This
   map-bounds clamp is the REAL safety net; the elevation slab is an
   optimization (both reviews).
4. Transform the clamped footprint into sun/light space; AABB -> the
   dynamic ortho XY center+extent.
5. Build the ortho: sun-aligned view basis (unchanged, INCLUDING the
   `fabsf(fz) > 0.9f` up-hint guard - preserve verbatim), XY from the
   fit AABB, **[0,1] ZERO_TO_ONE z-row preserved verbatim from
   `0c421d1`** (`gos_postprocess.cpp` ~:1414-1423; `[SHADOWZRANGE v1]`
   probe ~:1438 still asserts it). Fixed generous sun-depth envelope
   (existing `depthDist` form).
6. Anti-shimmer, in this order: map-bounds clamp (step 3) FIRST, THEN
   round the fit extent UP to a power-of-two world step, recompute
   `worldUnitsPerTexel` from the snapped extent (`dynShadowMapSize_` is
   4096 post-realloc `gos_postprocess.cpp` ~:1312; ctor default 2048 at
   ~:65 - read the live post-realloc value), then snap the fit center
   to that texel grid (extends the existing `floorf` block ~:1383-1384).

Output remains `dynamicLightSpaceMatrix_`, consumed UNCHANGED as a
`GL_FALSE` CPU uniform. EXHAUSTIVE consumer list (grep-completed; both
reviews flagged the rev-1 list as incomplete - re-verify at plan stage):
`gos_postprocess.cpp` ~:650; `gameos_graphics.cpp` ~:4510-4512 (inside
`beginDynamicShadowPass`), ~:4619, ~:4736, ~:4858, ~:6440-6442,
~:6782-6783; shaders `shadow.hglsl` `calcDynamicShadow` ~:88/95,
`shadow_screen.frag` ~:68/148/189, `terrain_overlay.frag` ~:92,
`decal.frag`, `gos_grass.frag`, `gos_terrain.frag` ~:675. All
`GL_FALSE`. Static `staticLightSpaceMatrix_` is a separate uniform/FBO -
isolated (verified both passes).

### Component 2 - GPU-batcher shadow feed (flushShadow)

NOT "reuse indirect + mirror shadow_object" (rev-1 was factually wrong
per both passes). Concrete contract:

- **New deliverable `shaders/shadow_mech.vert`**: instanced + skinned,
  reads the mech instance SSBO (binding 0) + bone SSBO (binding 1) +
  `u_instanceBase` exactly as `mech.vert` (`gos_mech_batcher.cpp` ~:183
  program), applies the same bone skinning to position only, emits
  `gl_Position = lightSpaceMatrix * vec4(mc2Pos,1)`, no color/fragment
  work. Loaded as a `shadow_mech` material in renderer init.
- **New deliverable `shaders/shadow_static_prop.vert`**: instanced,
  reads `instances_.i[gl_InstanceID + u_instanceBase].modelMatrix` as
  `static_prop.vert`, applies the Stuff->MC2 swizzle matching
  `shadow_object.vert` ~:11-14, emits `lightSpaceMatrix * mc2Pos`,
  depth-only. Loaded as `shadow_static_prop` material.
- `GpuMechBatcher::flushShadow()`: replay the per-bucket
  `glDrawElementsInstancedBaseVertex` loop from `flush()`
  (`gos_mech_batcher.cpp` ~:733/1068) with the `shadow_mech` program and
  the fit `dynamicLightSpaceMatrix_` as MVP. **Read the PREVIOUS frame's
  ring slot** `(s_frameSlot - 1 + MECH_RING_FRAMES) % MECH_RING_FRAMES`
  - already fence-waited and populated by last frame's `flush()`. Do NOT
  advance the ring or write the SSBO in `flushShadow` (sonnet
  CRITICAL-4): one-frame shadow-position lag (~16 ms, imperceptible),
  zero ring restructuring, no fence hazard. Draw the FULL bucket set
  (the shadow-cull rule below).
- `GpuStaticPropBatcher::flushShadow()`: instanced draw over the FULL
  per-type SSBO ranges with the `shadow_static_prop` program -
  explicitly **NON-indirect**, NOT `compute_getIndirectCmdBuf()` (that
  indirect buffer's counts ARE the camera-frustum cull output
  `gos_static_prop_batcher.cpp` ~:2947-2961; using it violates the
  shadow-cull rule). Reuse the existing instance SSBO upload
  (`s_lastUploadedSlot`/`uploadAllBucketsIfNeeded` ~:2600-2607 already
  fence-safe).
- **Shadow-cull rule (rev 3, gate-1 resolved = PHASED):** the batcher
  buckets ARE the camera-visible (`inView`) set (pre-windowed; proven).
  Phase 1 flushShadow draws that existing batched set as-is - it must
  still NOT use the camera-frustum `compute_getIndirectCmdBuf()` counts
  (that is a further GPU-cull narrowing of the already-windowed set and
  the static-prop indirect path; use the non-indirect full per-type
  bucket ranges so Phase 1 covers the whole camera-visible set, not the
  GPU-cull-narrowed subset). Off-screen casters are NOT in the buckets;
  their low-sun shadows are the documented Phase-1 gap. Phase 2
  (separate slice) adds a light-volume caster set.
- FBO/state: flushShadow uses `gos_BeginDynamicShadowPass()` for FBO
  bind + the load-bearing AMD feedback-loop/comparison/viewport/
  depth-state setup (`gameos_graphics.cpp` ~:4488-4512), then switches
  to its own program. Accept the `shadow_terrain_material_->apply()`
  overhead inside Begin as cheap vs reimplementing the safety state
  (sonnet MAJOR-5).

### Frame ordering (rev: the rev-1 region is DEAD - both passes CRITICAL-1)

The `txmmgr.cpp` ~:1549 region is gated `gos_IsTerrainTessellationActive()
&& g_numShadowShapes > 0`; `g_numShadowShapes` is 0 in the target GPU
config (sole producer `tgl.cpp` ~:3064 never runs). Add a NEW region,
gated on `gos_IsTerrainTessellationActive() && (g_useGpuObjects ||
g_useGpuMechs) && shadowsEnabled`, placed BEFORE
`gpu_cull::compute_dispatch()` (`txmmgr.cpp` ~:1737): build the
frustum-fit matrix, `gos_BeginDynamicShadowPass()`,
`GpuStaticPropBatcher::flushShadow()`, `GpuMechBatcher::flushShadow()`,
`gos_EndDynamicShadowPass()`. Before the cull dispatch so the
non-indirect static-prop shadow draw uses pre-cull full ranges and the
shadow pass consumes no cull output. Exact placement vs the
`Render.GpuStaticProps` zone (~:1711-1753) confirmed at plan stage.

## Instrumentation (same slice)

Env-gated `[SHADOWFIT v1]` (env `MC2_DEBUG_SHADOW_FIT`), unconditional
`fprintf(stderr)` not `assert`: emit the 8 unprojected corners, the
elevation-clamped + map-clamped fit AABB, the power-of-two snapped
extent+center, worst-case zoomed-out one-shot. Plus a flushShadow
instance-count emit per batcher (program, types, instances, ring slot
read, FBO) so "casters absent" vs "present but mis-projected" is a
one-log read. Gated-off default; demote-not-delete.

## Verification

- Two mandated adversarial passes: DONE (opus + sonnet, convergent,
  findings incorporated in this rev).
- Build RelWithDebInfo full relink; deploy v0.4 (per-file cp -f +
  diff -q).
- User visual: mech AND building shadows present, sharp, camera-tracked
  across 360deg / +10..90deg; Alt+F2 dynamic atlas full/tight; no
  edge-crawl beyond the quantized step; static terrain shadow NOT
  regressed; `[SHADOWZRANGE v1]` still inRange.
- `[SHADOWFIT v1]` AABB/corners sane zoomed-out worst-case;
  instance-count probe shows mech+prop casters drawn.
- tier1 smoke gate (`--tier tier1 --duration 30 --kill-existing`).

## Risk register

- Dead-region (compile-passes-renders-nothing): new unconditional region
  gated on GPU flags, not `g_numShadowShapes`.
- Wrong space (re-introduce artifact B): `clipToWorld` (swizzle-folded
  existing inverse) + parity probe.
- Wrong shader (garbage/crash): two new depth-only instanced(/skinned)
  shaders authored + reviewed before wiring.
- Ring fence hazard: read previous fenced slot, no advance in
  flushShadow.
- Shadow-cull using camera-visible set: forbidden; draw all; plan-stage
  off-frustum-presence grep gate (escalate if pre-windowed).
- Static-prop indirect = cull output: non-indirect full-range draw.
- [0,1] z-row reverted: preserved verbatim; `[SHADOWZRANGE v1]` asserts.
- Up-hint guard dropped at high sun: preserved verbatim.
- Static path regression: separate matrix/FBO; both passes confirmed
  isolation.
- Texel shimmer: map-clamp -> pow2 extent -> center snap (ordered).

## Open plan-stage gates (must close before execute)

1. Off-frustum caster presence in mech `s_pendingSubmits` and
   static-prop per-type SSBO ranges (escalate scope if pre-windowed).
2. Concrete conservative elevation-slab bounds from the map block data.
3. Exact new-region placement vs `Render.GpuStaticProps` zone.
4. Re-grep the exhaustive `dynamicLightSpaceMatrix_` consumer list.
