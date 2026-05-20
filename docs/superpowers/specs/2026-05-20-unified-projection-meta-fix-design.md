# Unified projection META-FIX — clean GL-convention `worldToClip`

Date: 2026-05-20. Worktree: `claude/nifty-mendeleev` @ `ced9e40`
(post-alpha-Stage-1-Stage-1 ship; post-Stage-0-probe-fix; post-sticky-bit-ship).
Status: **DESIGN v1.** Cited symbols grep-verified at write-time.

This spec retires the MC2 D3D-pixel-homogeneous projection chain in
favor of a clean GL-convention `worldToClip` where `clip.w == z_eye`
monotonically, the GPU clipper rejects behind-camera vertices via
standard `-w ≤ {x,y,z} ≤ w` homogeneous clipping, and no per-shader
`abs(clip.w)` packaging or CPU `pz ∈ [0,1)` pre-rejection is required.

## Provenance

- Documented 25+ days as deferred META-FIX per `terrain_tes_projection.md`
  path (a).
- 2 prior failed attempts: `ddc173f` (D1a removal) reverted by `6c6e872`
  (giant tris + atrocious fps under tessellation).
- alpha-Stage 1 §5 Stage 0 measurement on mc2_10 worst-case:
  - 60.09% sustained per-actor-frame conflation (render vs sim widened)
  - 89.13% sustained mover-vs-readback disagreement
  - 25.80% minimum-window disagreement (every window crossed the META-FIX
    threshold)
- User-reported empirical symptoms believed to trace here:
  - Screen-spanning raster triangles when mechs are below+behind camera
    (transient single-frame; matches clip.w sign-flip via skinned bones)
  - Trees popping in/out at specific camera positions (matches angular-cull
    threshold instability from broken math)
  - Black building textures (separate gate-pair invariant class; partly
    addressed by alpha-Stage 1 unconflation)
  - LOD-swap-to-blank at ~5000 units (separate animation-shape-cache
    issue per `bldg_animation_lod_swap_unsafe.md`; not this spec)
- User stated preference: "unified projection seems like the actual fix."

## Convergence from 4 advisors (2026-05-20)

| Advisor | Key recommendation |
|---|---|
| **mc2-shader-expert** | Path (a) META-FIX is correct AND smaller than memory files suggest. `cameraToClip` is an honest perspective matrix already; the **screen→pixel→NDC round-trip** is what destroys clip.w sign information. Build new GL-convention MVP at `code/gamecam.cpp:151-191` (single edit site); upload via existing `terrainMVP` slot; do NOT rewrite Stuff library. Per-shader migration: 13 shaders simplify to `gl_Position = mvp * vec4(world,1)` (~6 lines each). |
| **mc2-render-expert** | Census: 10 pixel-homog/abs/no-reject shaders need bulk migration; 2 partially-protected (terrain_overlay + gos_terrain_surface have `pz` reject); 1 with CPU `pz` gate (TES + quad.cpp); 2 half-clean trap renderers (`gos_terrain_mine_static.vert`, `gos_tex_vertex_lighted.vert`); 3 clean GL (shadow_*.vert — leave alone). Migration order: overlay/surface (safe) → statics → TES+quad.cpp atomic → mechs LAST → grass. |
| **Greybeard** | **META-FIX** verdict. Steel-mans of "preserve" / "fix recalcBounds instead" / "ddc173f says it's unknown" / "Stuff is too integrated" all defeated by grep. The load-bearing planning constraint is **sequencing**: matrix swap + all consumer edits + pz gate removal must land in ONE atomic slice. `ddc173f`'s failure was deletion-without-matrix-swap; right deletion, wrong order. M2 (retire Stuff from camera path entirely; route Camera through glm) is the deeper destination — filed as debt after M1. |
| **Explore** | Composition sites at `tgl.cpp:1556` + `camera.cpp:2144`. `Camera::projectZ` at `camera.h:386-425` with `fabs(rhw)` smoking gun at `:409`. 13 shaders + ~5 CPU sites in scope. `ddc173f`/`6c6e872` not found by direct hash; related revert pattern at `d7ff1c1` for an M2d overlay-pz-gate repoint. |

**Convergent insight (load-bearing):** the matrix is fine; the round-trip
chain is the bug. `cameraToClip(FORWARD_AXIS, 3) = 1.0f` means `clip.w =
z_eye` literally — positive for in-front, negative for behind-camera.
The `clip_w_sign_trap.md` memory's "mixed signs for visible verts"
wording is slightly misleading: visible verts uniformly have `clip.w >
0`; the trap is that the screen→pixel→NDC round-trip destroys the sign
information, leaving the GPU clipper unable to reject behind-camera
verts. Path (a) removes the round-trip entirely.

## 1. The load-bearing constraint (greybeard pin)

The current pipeline reaches the GPU rasterizer with behind-camera
vertices already finitized through `abs(clip.w)` packaging into the
legitimate NDC volume. The GPU clipper cannot distinguish them from
in-front verts because the sign information was destroyed by the
round-trip. Two consequences:

1. **CPU `pz ∈ [0,1)` gate in `mclib/quad.cpp` is load-bearing** —
   removes it without matrix change = giant garbage triangles per
   `ddc173f`/`terrain_tes_projection.md` attempt #2.
2. **Every shader needs `abs(clip.w)` packaging** to keep behind-camera
   verts from producing NaN gl_Position.

Both are workarounds for the same root cause: **the matrix's `clip.w`
sign is destroyed before the GPU's standard homogeneous clipping can
use it.** A clean GL-convention MVP eliminates the workarounds because
`clip.w == z_eye > 0` for in-front verts means the GPU clipper's
default `w > 0 && -w ≤ xyz ≤ w` correctly rejects behind-camera.

## 2. The migration shape

Two-stage approach per greybeard's sequencing rule:

**Stage 0 (PREREQUISITE — mandatory before any matrix change):**
Instrument `gos_terrain.tese` and `static_prop.vert` to log per-frame
`clip.w` sign distribution + near-zero count. Run on stock tier1.
Confirm the model: in-front verts have `clip.w > 0`, behind-camera
have `clip.w < 0`. `terrain_tes_projection.md` claims "mixed signs for
visible verts" — this MAY be slightly wrong per shader-expert's read
of `cameraToClip(FF,3)=1`. Empirically resolve before designing the
fix.

**Stage 1+ (the atomic slice, env-gated):**
Add new `worldToClipGL` matrix as PARALLEL uniform alongside legacy
`terrainMVP`. Env-gated `MC2_UNIFIED_PROJECTION=1`. Migrate consumers
in order. Final stage: delete legacy uniform + CPU pz gate atomically.

### 2.1 Stage 0 — clip.w distribution probe (PREREQUISITE)

Env: `MC2_PROJ_TRACE=1`. Per-frame TES + static_prop vertex-shader-side
counter:
- `n_visible_in_front` (clip.w > 0)
- `n_visible_behind` (clip.w < 0)
- `n_near_zero` (|clip.w| < 1e-3)
- `n_nan_or_inf` (clip.w not finite)

120-frame summary roll matching `[TOBJPARITY v1]` cadence. Per stock
tier1 mission (mc2_01, _03, _10, _17, _24) plus user-driven mc2_10
worst-case zoomed-out. User-driven canary 120s.

Output gate:
- If `n_visible_behind == 0` across all runs → shader-expert's model
  confirmed → matrix is fine, only round-trip needs replacing.
- If `n_visible_behind > 0` → memory's "mixed signs" warning is correct
  → matrix itself produces non-monotonic clip.w → spec scope expands
  to include matrix rebuild, not just chain replacement.
- `n_near_zero > 0` count tells us how close to the failure boundary
  current pipeline runs (the screen-spanning-triangle mechanism per
  advisor #1 = clip.w near-zero singularity in `1/clip.w`).

Cost: ~30 LOC probe; one user-driven canary.

### 2.2 Stage 1 — Parallel `worldToClipGL` build (no consumer migration)

Add a new CPU-side matrix composition at `code/gamecam.cpp:151-191`
alongside the existing axisSwap·worldToClip. The new matrix:

```
worldToClipGL_rowmajor = axisSwap_GLhanded * worldToCamera * P_GL
```

where:
- `axisSwap_GLhanded`: maps MC2 world `(x, y=ground, z=elevation)` to
  GL camera `(x, y=up, -z=forward)` (one-line permutation matrix).
- `worldToCamera`: existing MC2 view matrix.
- `P_GL`: standard GL perspective matrix (glm::perspectiveRH_ZO with
  reverse-Z, OR hand-built from `near_clip`, `far_clip`,
  `horizontal_fov`, `screen aspect`). Inputs already available on
  Camera class.

Upload via NEW uniform slot (e.g., `gos_SetTerrainMVP_GL`), or
reuse the existing `terrainMVP` slot conditionally based on
`MC2_UNIFIED_PROJECTION` env. Shader-expert recommends reusing the
slot with a runtime branch in `gos_SetTerrainMVP` — simpler than a
new slot.

No consumer changes in this stage. Just produce the matrix and
prove it composes without crashing. Tier1 5/5 sanity gate.

### 2.3 Stage 2 — Migrate the 2 already-rejecting renderers

`terrain_overlay.vert` and `gos_terrain_surface.vert` both already
carry a `px.z ∈ [0,1)` reject. They are the safest first migration:
their bug surface is decals + solid terrain (visible without
taking down mechs).

Per-shader edit (under `#ifdef MC2_UNIFIED_PROJECTION`):

```glsl
// REPLACE the entire clip→pixel→NDC chain with:
vec4 clip4 = u_worldToClipGL * vec4(world, 1.0);
gl_Position = clip4;
// (The pz reject becomes dead code under unified projection
//  because GPU clipper handles behind-camera. Leave it in for
//  insurance; remove in cleanup stage.)
```

Visual canary on mc2_10. If clean, proceed.

### 2.4 Stage 3 — Migrate statics + water + mines + grass

`static_prop.vert`, `gos_terrain_thin.vert`, `gos_terrain_mask_solid.vert`,
`gos_terrain_mask_water.vert`, `gos_terrain_water_fast.vert`,
`gos_terrain_water_fast_mdi.vert`, `gos_grass.geom`,
`gos_terrain_mine_static.vert`, `gos_tex_vertex_lighted.vert`.

Same edit pattern as Stage 2. Mines + tex_vertex_lighted are the
half-clean trap renderers per render-expert's audit — they CURRENTLY
work by luck (geometry happens to land in the D3D pixel-homog volume);
new matrix WILL break them visibly. Budget for it; visual canary
specifically targets mines and lit overlays.

### 2.5 Stage 4 — TES + quad.cpp pz gate (ATOMIC, HIGH-STAKES)

`gos_terrain.tese` migration is coupled to the 4 `MC2_ISTERRAIN |
MC2_DRAWSOLID` pz-gate clusters in `mclib/quad.cpp:~1495, ~1660,
~1799, ~1961` (per `terrain_tes_projection.md`). Must land in ONE
atomic commit because:
- Removing TES `abs(clip.w)` packaging without matrix swap = NaN
  gl_Position for behind-camera verts → giant garbage triangles
- Removing quad.cpp pz gate without TES swap = same failure mode at
  CPU producer level (this is `ddc173f` specifically)
- Removing pz gate WITH TES swap and matrix swap = the META-FIX
  delivered; GPU clipper takes over rejection cleanly

Mandatory parity probe before the atomic land:
- Side-by-side compute old and new `gl_Position` in TES via
  `#ifdef LEGACY_PROJ` branch
- Write per-vertex delta to debug SSBO
- Gate the slice on max-delta < 1e-3 NDC across stock tier1

After parity passes: atomic slice deletes legacy uniforms,
legacy chain blocks, and the 4 pz-gate clusters together.

### 2.6 Stage 5 — Mechs (last)

`mech.vert`. Per render-expert "dynamics LAST" rule and
`cull_gates_are_load_bearing.md` "mechs are the canary." This is also
the slice that should retire the user's screen-spanning-triangle
symptom (per shader-expert: triangles trace to clip.w near-zero
singularity during bone-skin pose transitions; new matrix eliminates
the singularity because GPU clipper rejects behind-camera bones
before rasterization).

Visual canary: user-driven mc2_10 wolfman zoom with mechs in
worst-case pose transitions. If triangle bug disappears, that
empirically confirms the unified-projection META-FIX retired the
class.

### 2.7 Stage 6 — Cleanup

Delete `terrainMVP`, `u_terrainViewport`, `u_mvp` uniform slots and
their composition sites in `code/gamecam.cpp`. Delete the `pz` gates
in `mclib/quad.cpp` (already done in Stage 4; verify). Deprecate
`clip_w_sign_trap.md` (update to RETIRED status). Update
`terrain_tes_projection.md`.

`Camera::projectZ` at `camera.h:386-425` keeps its public API but
body re-derived from new GL matrix: `screen.xyz = (mvp_GL * world).xyz
/ w; screen.w = 1.0/w_eye`. `fabs(rhw)` becomes plain `rhw`. The 7
policy-split wrappers at `camera.h:514-617` keep working unchanged.

## 3. Negative space — explicit list of what we are NOT doing

| NOT touching | Why |
|---|---|
| Stuff library (`mclib/stuff/`) internals | Multi-quarter retirement; M1 stays narrow. Render-expert + shader-expert both warn against. M2 (retire Stuff from camera) is filed as future debt. |
| MLR projection path (`mclib/mlr/`) | May have its own matrix path; flagged by render-expert as audit-needed-before-spec-freeze but out of scope to MIGRATE. If audit shows it consumes `worldToClip`, migrate in a follow-on slice. |
| gosFX particle Stuff matrix path | Render-expert flagged as easily-forgotten consumer; out of scope. Will silently shift if `worldToClip` semantics change. **Audit required before Stage 1 lands.** |
| Editor's forked render loop | `feedback_editor_must_converge_with_runtime_paths.md` debt; editor builds its own terrainMVP at `EditorGameOS.cpp`. Audit-required-before-spec-freeze per render-expert. Editor migration is a separate worktree's slice. |
| recalcBounds angular cull (5 overrides) | Already partially retired (Tasks 2/3 deleted projection body at `bdactor.cpp:1218, 3981`). Finishing it is a separate cleanup slice per `clip_w_sign_trap.md`'s recommended `projectZ`-OBB pattern. NOT bundled here. |
| HUD/GUI/screen-space shaders | (`gos_text.vert`, `postprocess.vert`, etc.) — projection-immune; skybox + screen-space verts are written directly. |
| Shadow shaders (`shadow_*.vert`, `shadow_terrain.tese`) | Already use clean GL convention (`lightSpaceMatrix * vec4(pos, 1.0)` direct to `gl_Position`). LEAVE ALONE. |
| `Camera::inverseProject` deprecation | Per `inverseproject_reduction_feeds_only_deprecated_tacmap.md` — already deprecated, fed only by deprecated tacmap. Out of scope. |
| alpha-Stage 1 unconflation (Stages 2-7) | Separate spec. The 60% conflation + 89% mover-vs-readback Stage 0 telemetry is partly producer-broken-math (which this spec retires) AND partly legitimate consumer concern split (which the unconflation handles). Both specs needed; this one is a deeper foundation. |

## 4. Risk surface

**R1 — `ddc173f` failure mode recurrence.** The 2 prior attempts
failed because they deleted the pz gate WITHOUT swapping the matrix.
This spec mandates atomic slicing (Stage 4 is "delete + swap + edit
all shaders in ONE commit"). Any partial application reproduces
`ddc173f`. Adversarial review of the Stage 4 commit MUST verify
atomicity.

**R2 — Mixed-state during staged migration is hazardous.** Once
some shaders are on `u_worldToClipGL` and others on legacy
`terrainMVP`, ANY shader that's mid-migration will be inconsistent
with surrounding draws if state leaks. Mitigation: env-gate
`MC2_UNIFIED_PROJECTION` globally; either ALL non-shadow renderers
use new matrix or NONE do.

**R3 — Parity probe is load-bearing.** Stage 4 (the atomic slice)
gates on per-vertex delta < 1e-3 NDC between old and new chain.
This probe MUST land first (Stage 0.5?) and prove parity on stock
tier1. Skipping = blind ship = `ddc173f`.

**R4 — gosFX silent shift.** If gosFX particles consume Stuff's
`worldToClip` and we change it, particles silently move on screen.
Mitigation: gosFX audit before Stage 1; if it consumes, migrate
in lockstep or carve-out.

**R5 — MLR projection path may differ from `terrainMVP`.** Render-
expert flagged this as high-risk negative space. Audit required.

**R6 — Editor breakage.** Per `feedback_editor_must_converge_with_runtime_paths.md`,
editor builds its own forked terrainMVP. Editor will need parallel
migration or the LEAVE-site pattern from `alpha-stage-1` spec §4.3.

**R7 — Reverse-Z interaction.** Per shader-expert: existing
`cameraToClip` already wires reverse-Z. New GL MVP must preserve
this (`glm::perspectiveRH_ZO`). `gl_FragDepth` and `gl_ClipControl`
defaults can interact unpredictably; AMD shader review (`/mc2-amd-shader-review`)
mandatory before any deploy.

**R8 — Stage 0 model resolution may be the wrong premise.** If
TES probe shows visible verts DO have mixed clip.w signs (memory's
warning correct, shader-expert's reading wrong), the matrix itself
is broken, not just the round-trip. Scope expands to actual matrix
rebuild. Stage 0 GATES Stage 1+.

## 5. Test / gate strategy

| Stage | Smoke gate | Visual canary | Adversarial review | --clean-first? |
|---|---|---|---|---|
| 0 (probe) | tier1 5/5 30s | user-driven mc2_10 120s with MC2_PROJ_TRACE=1 (collect clip.w distribution) | optional | no |
| 1 (parallel matrix) | tier1 5/5 30s | none (no consumer migration) | yes (matrix construction correctness) | no |
| 2 (overlay+surface) | tier1 5/5 + mc2_10 60s | yes (decal + solid terrain at wolfman zoom) | yes | no |
| 3 (statics) | tier1 5/5 + mc2_10 60s | yes (static-prop + water + mines + grass; the trap renderers) | yes | no |
| 4 (TES + pz gate ATOMIC) | tier1 5/5 + parity probe < 1e-3 NDC | YES (this is the load-bearing slice; mc2_10 worst-case zoomed-out 120s) | YES (atomicity verification, `ddc173f` failure-mode check) | YES (TES touches many TUs) |
| 5 (mechs) | tier1 5/5 + mc2_10 60s | YES (the user's screen-spanning-triangle test; mc2_10 mechs in worst-case pose transitions) | yes | no |
| 6 (cleanup) | tier1 5/5 + grep audit | none | yes | no |

`MC2_GL_DEBUG_FATAL=1` mandatory for all stages per worktree CLAUDE.md
Tier 1.2. `/mc2-amd-shader-review` skill MANDATORY before each shader-
touching stage per AMD-specific gotchas (reverse-Z, ClipControl,
FragDepth interactions).

## 6. Open questions for next planner

1. **Stage 0 model resolution.** Greybeard called this out as RED-on-grounding
   — without the TES probe data, we don't know if the matrix itself is
   broken or just the round-trip. THIS IS BLOCKING. Cannot ship Stage 1
   until Stage 0 lands.

2. **gosFX audit.** Render-expert flagged. Need to grep `mclib/gosfx/`
   for `worldToClip` / `terrainMVP` consumers and decide migration.

3. **MLR audit.** Same. Need to grep `mclib/mlr/`.

4. **Editor audit.** Need to grep `EditorGameOS.cpp` (in editor worktree)
   for terrainMVP build site.

5. **Atomic-slice scope.** Stage 4 mandates "matrix + TES + quad.cpp pz
   gate" atomic. Does that also include Stages 2-3-5 shaders? Greybeard
   says YES (one slice gated by `MC2_UNIFIED_PROJECTION=1`). Shader-
   expert says incremental per-stage is OK if env-gated. Resolve before
   Stage 1 plan.

6. **Reuse `terrainMVP` slot vs add new slot.** Shader-expert prefers
   reuse with runtime branch (simpler). Render-expert prefers parallel
   slot (cleaner A/B). Resolve.

7. **`Camera::projectZ` body migration timing.** Render-expert says
   keep public API stable, swap body to use new matrix in Stage 6
   (after Stage 5 mechs land). Shader-expert says CPU `projectZ`
   migration is opportunistic later, not blocking. Resolve.

8. **M2 deferred destination.** Greybeard names M2 (retire Stuff
   matrix from camera path, route through glm) as the deeper META-FIX
   below M1. After M1 ships, file M2 spec? Or accept M1 as the
   end-state?

## 7. References

- alpha-Stage 1 spec (sibling, parallel campaign):
  `docs/superpowers/specs/2026-05-20-appearance-inview-unconflation-design.md`
- The trap memory: `memory/clip_w_sign_trap.md`
- The projection chain documentation: `memory/terrain_tes_projection.md`
- Static-prop projection rules: `memory/static_prop_projection.md`
- Matrix convention warnings: `memory/matrix_index_convention_verify_before_trusting_cited_index.md`
- Editor convergence debt: `memory/feedback_editor_must_converge_with_runtime_paths.md`
- Stage 0 alpha-Stage 1 corrected probe data (mc2_10, 60.09% sustained):
  `tests/smoke/artifacts/2026-05-20T14-30-56/mc2_10.log`
- Failed-attempt commits: `ddc173f` (D1a removal) reverted by `6c6e872`
  (not found by direct hash by Explore agent; verify in
  `git log --all --grep="D1a"`)
- Related revert: `d7ff1c1` (M2d overlay-pz-gate repoint revert per
  Explore agent)
- Composition sites (grep-verified per Explore agent):
  `mclib/tgl.cpp:1556`, `mclib/camera.cpp:2144`
- `Camera::projectZ`: `mclib/camera.h:386-425` with `fabs(rhw)` at `:409`
- `cameraToClip` reverse-Z wiring: `mclib/camera.cpp:2025-2047`
- 13 shaders in scope per render-expert census + shader-expert grep
- Mandatory before Stage 1: `/mc2-amd-shader-review` skill + adversarial
  review

## 8. What this spec is NOT

- Not the alpha-Stage 1 unconflation. That's separate; both ship in parallel.
- Not a Stuff library retirement (M2 deeper destination filed as debt).
- Not a recalcBounds angular cull fix (separate cleanup slice after this spec).
- Not a single-session implementation. Stages 0-6 are independent
  shippable slices, each with its own gate.
- Not a CPU perf slice. This is correctness; perf wins (deletion of
  abs(clip.w), pz gate, round-trip) are second-order benefits.

This spec retires the COORD-SPACE BUG CLASS. The alpha-Stage 1 spec
retires the CONFLATION. Both are META-FIXes at different layers; both
needed.
