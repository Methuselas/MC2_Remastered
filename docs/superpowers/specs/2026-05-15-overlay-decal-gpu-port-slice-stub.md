# Overlay/decal GPU port - slice stub (NOT in the VPL retirement 10-step plan)

Status: TRACKED, not yet planned. Created 2026-05-15 from user direction during VPL-retirement Wave 3.

## Why this exists

The VPL retirement plan (`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`)
retires VPL's per-vertex *projection*. It deliberately does NOT touch the M2d
overlay/decal producer. Step 1b (`cpu-pack-retirement` plan, commits
`37702a1`/`b7be3af`) explicitly kept `gos_PushTerrainOverlay` UNCONDITIONAL -
gating it would have caused the `9964d5a` conflated-overlay regression class
(missing cement-transition / runway / road decals). The terrain-indirect
advisor flagged this HIGH on 2026-05-14 and the mc2_24-road decal canary in
Step 1b smoke confirmed the producer must stay CPU-side for now.

The user (2026-05-15) confirmed the overlay producer should eventually move to
GPU as part of the overarching CPU->GPU offload (`mc3_modernization_philosophy`:
"CPU or GPU? -> GPU at every fork"). It is a smaller CPU-time chunk than the
thin-record path but still in scope for full GPU-driven terrain.

## Scope (to be expanded into a real plan-doc when scheduled)

- **Producer:** `gos_PushTerrainOverlay()` calls at `mclib/quad.cpp` (gated by
  `pzTri1`/`pzTri2`; advisor-verified call sites ~`:2249/2256/2265/2272`,
  gate predicates ~`:2244/2251/2260/2267`). The M2d overlay block runs
  ~`quad.cpp:2220`-`:2276` (grep-verify at plan time; numbers drift).
- **What it produces:** cement-transition tiles, runway/road decals -
  world-space overlay quads distinct from the SOLID base-quad thin records.
- **Why it survived Fix B:** Fix B (`005ebc7`) moved SOLID base-quad clip
  projection to `gpu_driven_terrain_solid.comp`. The overlay emit is a
  SEPARATE decal pipeline the compute path does not replace. The Stage 2b
  "indirect overlay packer" was vapor (`indirect_overlay_packed_quads`
  never increments - confirmed by terrain-indirect advisor 2026-05-14).

## Likely shape (hypothesis, not committed)

Analogous to Fix B's pattern: a GPU compute pass that admits + projects
overlay decal quads into an indirect-draw ring, replacing the CPU
`gos_PushTerrainOverlay` walk. Needs:
- Recon of the M2d overlay pipeline (how overlays are keyed, textured,
  z-ordered against base terrain + the depth-fudge interaction).
- A dedicated advisor pass (`mc2-terrain-indirect-expert` +
  `mc2-shader-expert`) before any design - the overlay/base-quad
  z-fighting + the existing `TERRAIN_DEPTH_FUDGE` (0.002) interaction is
  the load-bearing risk surface.
- Its own adversarial-plan-review (architectural endpoint + decal-pipeline
  retirement = high-stakes per worktree CLAUDE.md review discipline).
- Decal canary in smoke (mc2_24 road, mc2_01 apron) as the regression gate -
  the exact canary that validated Step 1b.

## Sequencing

Sibling to VPL retirement, NOT a dependency of it. Can be scheduled after
the VPL 10-step plan completes, or interleaved once the GPU-driven terrain
substrate is stable. Pre-req recon should wait until VPL retirement Step 8
(body deletion) lands so the overlay port isn't fighting a moving target.

## Memory cross-refs

- `cull_cascade_wrap_and_reduce_pattern.md` - the wrap-and-reduce discipline
  that kept the overlay producer alive through Step 1b.
- `mc3_modernization_philosophy.md` - GPU-at-every-fork rationale.
- `stock_install_must_remain_playable.md` - the port must degrade to
  stock-compatible decal generation if GPU data is missing.
