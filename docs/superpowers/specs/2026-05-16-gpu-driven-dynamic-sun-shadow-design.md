# GPU-Driven Dynamic Sun Shadow (Frustum-Fit) - Design

Date: 2026-05-16
Status: DESIGN (pre-plan). Slice for VPL-#10 artifact B and the
mech/building shadow-feed gap.
Branch: claude/gpu-driven-rendering

Symbols are authoritative; every file:line below is a starting point to
re-grep at plan/execute time (Rule 0). Line numbers drift.

## Problem

Under the default config (`g_useGpuObjects` on, `g_useGpuMechs` on),
GPU-batched buildings and mechs cast **no sun shadow at all**:
`GpuStaticPropBatcher::flushShadow()` (`gos_static_prop_batcher.cpp`
~:3589) and `GpuMechBatcher::flushShadow()` (`gos_mech_batcher.cpp`
~:341) are empty stubs with zero callers. The only feed of the dynamic
shadow set `g_shadowShapes[]` is the legacy `TG_Shape::Render` ->
`addShadowShape` path (`tgl.cpp` ~:3064), which runs only on GPU-submit
fallback. So the dynamic shadow FBO today contains only legacy-fallback
leftovers; the static FBO is terrain-only (`renderStaticTerrainShadowFullMap`,
`mapdata.cpp` ~:1284).

Separately, the dynamic light-space matrix is built from a legacy
CPU-era shim in `txmmgr.cpp` (~:1559-1574): a camera->ground raycast
(`eye->getCameraOrigin()`/`getLookVector()`, `t*=0.80f`) plus a hand
MC2<->Stuff axis/sign conversion feeding `buildDynamicLightMatrix`
(`gos_postprocess.cpp` ~:1334-1432) with a fixed `xyRadius=2400`. This
is VPL-#10 artifact B: footprint not square, bounces off map edges,
does not track the camera across the full 360deg azimuth / +10..90deg
elevation envelope.

The static/dynamic split itself was a CPU-terrain-cost workaround (user,
2026-05-16); terrain projection is GPU-side now so the split is no
longer load-bearing - but the static path was just fixed (commit
`0c421d1`, [0,1] ZERO_TO_ONE) and works, so unification is OUT OF SCOPE
here (regression risk against a known-good path).

## Goal

Crisp, camera-tracked sun shadows for **mechs/vehicles and
buildings/GPU-batched static props**, auto-scaling resolution with zoom
(sharp zoomed-in, naturally coarser zoomed-out), via the modern
single-sun-pass GPU-driven design. No CPU readback, no frame-ordering
hazard, no substrate sync-stall.

## Non-goals (explicit)

- Static terrain shadow map: untouched (separate FBO/matrix
  `staticLightSpaceMatrix_`, just fixed by `0c421d1`). Not regressed.
- Unifying static+dynamic into one pass: future consideration, not this
  slice.
- Trees / generic genactor props: gate-excluded from the shadow set
  (`alphaTestOn` / `isClamped`, `tgl.cpp` ~:3060-3062). Sharpening those
  is a separate caster-gate slice.
- The up-hint basis flip `fabsf(fz) > 0.9f` (`gos_postprocess.cpp`
  ~:1392): separable robustness follow-up, not this slice.
- GPU-cull-driven focus: rejected - cull dispatch (`txmmgr.cpp` ~:1737)
  runs AFTER the shadow prepass; reading bounds back hits the documented
  ~6 ms substrate sync-stall; no bounds product exists.

## Design

Two coupled components, ONE catastrophic-axis slice (soak waived ->
env-gated parity probes + TWO mandated adversarial reviews
(opus + sonnet) + code-proof).

### Component 1 - Frustum-fit sun ortho

Replace the `txmmgr.cpp` ~:1559-1574 shim and the fixed-extent body of
`buildDynamicLightMatrix`. Per frame, in raw-MC2 caster space (the space
mech `shapeToWorld` and scene `WorldPos` / `calcDynamicShadow` already
use - NO hand conversion):

1. Get the camera frustum via `Camera::extractFrustumPlanes()`
   (`camera.h` ~:649-652) - already swizzle-folded to RAW-MC2, so no
   matrix inverse and no axis-swap risk (chosen over `inverse(terrainMVP)`
   precisely to avoid reintroducing the artifact-B space bug).
2. Reconstruct the 8 frustum corners from the planes.
3. Intersect the frustum with a world **elevation slab** [terrain min,
   terrain max + caster-height margin] before taking bounds (tightest
   correct footprint; prevents the low-angle "frustum misses ground ->
   footprint explodes" failure). Terrain elevation range + map bounds
   sourced from the static path's constants (`mapHalfExtent_`,
   `gos_postprocess.cpp` ~:61; static clamp `r = mapHalfExtent *
   sqrt(2) * 1.05`, ~:1207).
4. Clamp the resulting footprint to map bounds (never exceed the static
   `r`-derived extent).
5. Transform the clamped footprint into sun/light space; take the AABB
   -> that AABB is the dynamic ortho's XY center + extent.
6. Build the ortho: sun-aligned view basis (unchanged), XY from the fit
   AABB, **[0,1] ZERO_TO_ONE z-row preserved verbatim from `0c421d1`**
   (`gos_postprocess.cpp` ~:1414-1423; reverting to [-1,1] silently
   breaks the `.xy`-only sampler remap in `shadow.hglsl` ~:97). Keep a
   fixed generous sun-depth envelope (existing `depthDist` form) - per
   the bounded caster height, per-frame near/far adds instability for
   no gain.
7. Anti-shimmer: round the fit extent UP to a power-of-two world step;
   recompute `worldUnitsPerTexel` from the snapped extent
   (`dynShadowMapSize_` = 4096, ~:1312); snap the fit center to that
   texel grid (extends the existing `floorf` snap block ~:1383-1384).

Output remains `dynamicLightSpaceMatrix_`, consumed UNCHANGED as a
`GL_FALSE` CPU uniform by every consumer (`gos_postprocess.cpp` ~:650;
`gameos_graphics.cpp` ~:4619/4736/4858; shaders `shadow.hglsl`
`calcDynamicShadow`, `shadow_screen.frag`, `terrain_overlay.frag`).
Static path's `staticLightSpaceMatrix_` is a separate uniform -
untouched.

### Component 2 - GPU-batcher shadow feed (flushShadow)

Implement `GpuMechBatcher::flushShadow()` and
`GpuStaticPropBatcher::flushShadow()` to draw their already-batched
instance sets into the dynamic shadow FBO, depth-only, transformed by
the frustum-fit `dynamicLightSpaceMatrix_`. Design contract (exact
command-buffer/SSBO/VAO reuse to be grep-confirmed at plan stage,
mirroring each batcher's existing `flush()` indirect machinery):

- Reuse the batcher's existing instance SSBO + indirect command buffer
  (the same data the main `flush()` draws). Bind the dynamic shadow FBO
  (via `gos_BeginDynamicShadowPass` or its FBO), a depth-only shadow
  program (mirror the existing `shadow_object` / `shadow_depth` path),
  the frustum-fit light matrix as the MVP, no color attachment write.
- **Shadow-cull constraint (critical):** shadow casters can lie OUTSIDE
  the camera frustum (a building behind the camera casts into view).
  `flushShadow` MUST NOT reuse the camera-frustum-culled visible set
  (`VisibleIds`). Baseline: draw ALL batched instances of the type set
  (user has VRAM/compute to spare; simplest, correct). Optional later
  optimization: reject against the fit AABB expanded along the sun
  direction - explicitly deferred unless a probe shows a cost problem.
- Vulkan-prep: explicit device-mediated binding, no implicit cross-call
  GL state; no new readback; std430 lockstep if any new SSBO field is
  added (prefer reusing existing instance buffers to avoid a new
  schema).

### Frame ordering

`flushShadow` calls go in the dynamic shadow prepass region
(`txmmgr.cpp` ~:1548-1581), after `buildDynamicLightMatrix` produces the
fit matrix and the shadow FBO is bound, before it is resolved/unbound.
This is BEFORE the GPU cull dispatch (~:1737) - fine, the shadow pass
does not consume cull output. Confirm exact insertion point and FBO
lifetime at plan stage.

## Instrumentation (same commit)

Env-gated `[SHADOWFIT v1]` parity probe (env e.g.
`MC2_DEBUG_SHADOW_FIT`), unconditional `fprintf(stderr)` not `assert`
(per `assert_is_noop_in_relwithdebinfo`), mirroring the existing
`[SHADOWFRUSTUM v1]` / `[SHADOWZRANGE v1]` patterns: emit the
unprojected frustum corners, the slab-intersected + clamped fit AABB,
the snapped extent/center, and a worst-case zoomed-out one-shot. Plus a
`flushShadow` instance-count emit (types drawn, instances, FBO bound) so
"casters absent" vs "casters present but mis-projected" is a one-log
read. Stays gated-off by default; demote-not-delete.

## Verification (soak waived -> probes + reviews + code-proof)

- TWO mandated adversarial reviews of THIS spec (one opus, one sonnet),
  adversarial-plan-review skill, grep-closing every cited symbol and the
  shadow-cull-correctness, space-consistency, [0,1]-z-row-preservation,
  and flushShadow-mirrors-flush claims.
- Build RelWithDebInfo full relink; deploy v0.4 (per-file cp -f +
  diff -q).
- User-driven visual: mech AND building shadows present, sharp,
  camera-tracked across 360deg / +10..90deg; Alt+F2 dynamic atlas full
  and tight; no edge-crawl beyond the quantized step; static terrain
  shadow (artifact A) NOT regressed.
- `[SHADOWFIT v1]` parity probe inRange/aabb sane at zoomed-out
  worst-case.
- tier1 smoke gate (`--tier tier1 --duration 30 --kill-existing`).

## Risk register

- Wrong space (reintroduce artifact B): mitigated by using the
  swizzle-folded `extractFrustumPlanes` (no inverse), and the parity
  probe.
- Shadow-cull using camera-visible set -> casters pop in/out at frustum
  edges: explicitly forbidden; baseline draws all batched instances.
- [0,1] z-row reverted -> silent sampler break: preserve `0c421d1`
  z-row verbatim; `[SHADOWZRANGE v1]` still asserts it.
- `flushShadow` mis-mirrors `flush()` (wrong VBO/command buffer) ->
  garbage/no casters: plan-stage grep of each batcher's `flush()`;
  instance-count probe catches "absent".
- Static path regression: static matrix/FBO untouched; reviews confirm
  isolation.
- Texel shimmer on zoom/rotate: power-of-two extent quantization +
  center snap.
