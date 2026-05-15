# Render Performance Snapshot

**Last refreshed:** 2026-05-14 (raster-triangle Fix A + Fix B shipped; VertexProjectLoop retirement plan drafted).

**Refresh when:** a load-bearing slice ships (Track D, substrate-coalesce, Lifecycle gate, PR2/3 milestones), the wolfman bucket map drifts noticeably in fresh Tracy captures, or a major env-var default flips.

**How to refresh:** re-measure with user-driven Tracy at wolfman zoom (smoke FPS is deceptive - the harness sends no input). Update the bucket map, dependency graph, and recent-decisions sections below. Note the new "Last refreshed" date. Archive significantly outdated versions to `docs/render-perf-snapshot-history/` if you want a record.

`mc2-render-perf-expert` reads this file as the first item in its `<load_first>` block.

## Frame budget (2026-05-08 session end)

- **~6.76 ms zoom-in** (frame-cap-limited at ~150 fps effective from 165 fps target)
- **~9.5 ms zoom-out (wolfman)** - the real CPU floor; this is the working number for slice ROI math

Smoke FPS is NOT comparable to live Tracy at wolfman zoom because the smoke harness sends no input. Always re-baseline with user-driven Tracy.

## Bucket map at wolfman zoom (~9.5ms baseline)

Approximate, 2026-05-08. Re-grep against current Tracy on next refresh.

| Zone | Cost (ms) | Notes |
|---|---:|---|
| `Terrain::render drawPass` | ~1.7 | post-PR2c MINE retire |
| `GameLogic.Units.TerrainObjects` | ~1.27 | render-prep + game logic mix |
| `quadSetupTextures` | ~1.1 | Phase 1 GPU compute endpoint candidate per `mc2-cpu-gpu-offload-expert` |
| `GameCamera::render` objects | ~0.9 | Track D scope |
| `Vehicles AppearanceUpdate` | ~0.3 - 0.4 | per-actor render-prep, gateable |
| `vertexProjectLoop` | ~0.475 | retirement plan drafted 2026-05-14 (`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`); D1 hoist closed at compiler ceiling per `memory/vertexproject_loop_asymptotic.md`; lever is elimination, not speedup |
| `Terrain::SolidComputeDispatch` (CPU submission) | ~0.95 mean / 0.59 median | post-Fix-B baseline; ~80-170 µs recoverable via cmd-patch retirement, ~500 µs floor target via bind-once + persistent-mapped + dirty-flag skip (sequenced in plan) |
| `Mech3D.UpdateGeometry` x6 | ~0.3 | Track D scope |
| `Render.GpuStaticProps` | ~2 | WHEN substrate-on (currently default-off due to 323-bucket regression) |

## In-flight slices and dependencies

| Slice | State (2026-05-08) | Blocked by | Blocks |
|---|---|---|---|
| Substrate (`MC2_GPU_CULL_SUBSTRATE=1`) | BROKEN at default-on (323-bucket `glDrawElementsIndirect` serialization, ~2 ms regression) | substrate multi-draw coalesce redesign (`docs/superpowers/plans/progress/2026-05-08-substrate-coalesce-redesign-prompt.md`) | Lifecycle gate (which consumes readback ring) |
| Lifecycle gate (`MC2_GPU_CULL_LIFECYCLE=1`) | PAUSED (strategic decision 2026-05-08) | substrate fixed | nothing |
| Track D (GPU mech batcher) | PARITY SIGN-OFF 2026-05-08 (Slice A); independent | nothing | nothing (per-mech lifecycle gating obsoleted by Track D) |
| PR2 detail/overlay/mine retirement | LARGELY SHIPPED (PR2a delete, PR2b overlay scaffold, PR2c MINE static-bake default-on) | nothing | nothing |
| Job-system parallel-for | QUEUED (post substrate-coalesce, per 2026-05-08 brainstorm) | substrate coalesce ship | parallel `vertexProjectLoop` slice (now SUPERSEDED — VPL retired, not parallelized; see plan 2026-05-14) |
| Cull-dilation + conservative-OR | SHIPPED 89e35ac 2026-05-08; makes Lifecycle motion-safe | nothing | (unblocks Lifecycle's motion-safety dimension) |
| Raster-triangle Fix A | SHIPPED `a373691` 2026-05-14 — per-ring-slot MVP snapshot, defense-in-depth | nothing | Fix B + retirement chain |
| Raster-triangle Fix B | SHIPPED `005ebc7` 2026-05-14 — pre-projected clipPos in thin record; GPU compute now sole projection authority for terrain quads in indirect path; +25 fps mc2_01 tier1 | Fix A | CPU pack retirement, cmd-patch retirement, VPL retirement |
| CPU pack path retirement | DEMOTED (not deleted) — Step 1 `18a4c36` (`PackThinRecordsForFrame` body behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK`, default-off); Step 1b-1 `4374794` (quad.cpp narrowed thin-emit gate-split, decal producer preserved); Step 1b-2 (parity infra `s_packParityMask`/`gos_terrain_indirect_getPackParityMask`/`ComputeDispatchParity_Check` DEMOTED, self-gating, zero stock cost — full delete deferred to VPL Step 4 per OQ-2). Re-measure bucket after smoke. | Fix B (shipped) | VPL retirement step 1, cmd-patch retirement |
| Cmd-patch dispatch retirement | QUEUED — design drafted; atomicAdd into `cmd.count` directly; eliminates second compute dispatch | CPU pack retirement (frees bucket header dependency) | ~80-170 µs CPU/frame |
| `VertexProjectLoop` retirement | PLAN DRAFTED 2026-05-14 (`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`) — 9-step interleaved sequence; needs adversarial review before step 1 | CPU pack retirement (step 1), picking repoint (step 3), cull-cascade audit (step 5) | object/prop iteration GPU port, GPU AABB mouse pick (`memory/gpu_mech_aware_mouse_pick_queued.md`) |

## Recent sign-offs and decisions

- **Raster-triangle Fix A + Fix B shipped (2026-05-14, `a373691` + `005ebc7`):** root cause was temporal MVP misalignment under the intentional one-frame compute->bridge lag in the indirect terrain pipeline. Fix A patched it at the binder (per-ring-slot MVP snapshot + override re-upload — defense-in-depth, currently inert post-Fix-B). Fix B moved per-corner projection from `gos_terrain_thin.vert` into `gpu_driven_terrain_solid.comp` and stored clipPos in the thin record (96 B/record, up from 32 B). The thin VS no longer touches `terrainMVP`. mc2_01 tier1 +25 fps from removing one `mat4*vec4` per VS invocation. Full handoff: `docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md`. Pattern memory: `memory/ring_slot_state_must_travel_with_slot.md`.

- **VertexProjectLoop retirement plan drafted (2026-05-14):** Path A (consumer retirement) confirmed by `mc2-cpu-gpu-offload-expert`; Path B (GPU port + readback) rejected — would reintroduce the 1-frame-lag bug class Fix A/B just killed. Sole production CPU survivor is `Camera::inverseProject` (mouse picking), repointed via the existing `projectForSelectionPicking` wrapper from the 2026-04-13 projectZ peeling arc. 9-step interleaved sequence in `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`. Adversarial review required before step 1 implementation (architectural endpoint + legacy retirement = high-stakes triggers). Downstream unlocks: object/prop iteration GPU port, GPU AABB mouse pick.

- **Cull-dilation + conservative-OR shipped 89e35ac (2026-05-08):** `MC2_GPU_CULL_FRUSTUM_DILATION` default 0.08 per plane; conservative-OR over N-1/N-2/current readback slots (`MC2_GPU_CULL_CONSERVATIVE_OR` default on). Does NOT fix substrate regression - just makes visibility decision tolerant to readback staleness during camera motion. Counter at `[GPU_CULL v1] event=motion_tolerance` emits every 600 frames showing `dilated_admits` and `conservative_or_admits`.

- **Track D Slice A parity gate (2026-05-08):** operator visual + `[MECHBATCHER v1] event=summary fallback_total=0` accepted as gate. Dual-FBO parity deferred to Slice A+ unless operator visual misses a regression during 7-day soak. Same precedent as renderWater Stage 2+3, indirect-terrain SOLID PR1, PR2c mine static-bake.

- **Lifecycle gate paused (2026-05-08):** strategic decision. RTS play patterns don't favor "stationary camera" gating (users pan/rotate constantly). Cull-dilation+OR made it motion-safe but value is fundamentally wolfman-zoom-only and Track D obsoletes per-mech lifecycle gating entirely. Pause until either (a) substrate regression fixed (lifecycle net-positive at wolfman with no substrate cost), or (b) Track D ships and shifts the bucket the gate would target.

- **applyRenderStates state-equality cache shipped (2026-05-08 74ce55b + review fixes f8c8f2f / 010550f).** 6 invalidation sites initially; adversarial review added 4 caller-side gaps (`gos_static_prop_batcher::flush` wrap-state mutation, `drawTerrainOverlays`, `drawDecals`, doc-of-contract gap). Canonical declaration site in `gameos_graphics.cpp`; grep `gos_InvalidateRenderStateCache` for current.

- **Strip commit `fdc47bc` (2026-05-07)** removed per-element Tracy zones from hot loops. See methodology advisor `<core_knowledge>` for the list of removed zones and the rule.

## Stale-flags

Items that need re-measurement on the next snapshot refresh:

- Bucket map costs above are pre-Track-D-default-on. If Track D flips default-on, `Mech3D.UpdateGeometry`, `GameCamera::render`, and possibly `Vehicles AppearanceUpdate` shift.
- Substrate cost (~2 ms when on) will change when multi-draw coalesce ships.
- `applyRenderStates` skip rate workload measurements (~47% normal-zoom, ~17% wolfman) - workload-dependent, re-measure on each tier1 baseline.
- Frustum-dilation default 0.08 - chosen heuristically; the `motion_tolerance` summary counter is the source for measuring `dilated_admits / strict_admits` ratio.

## Execution prompts referenced from this snapshot

- `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` - VPL retirement (Path A, 9-step interleaved); adversarial-review-pending
- `docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md` - Fix A diagnosis + Fix A/B context for the retirement chain
- `docs/superpowers/plans/progress/2026-05-08-track-d-execution-prompt.md` - Track D Slice A (parity sign-off received)
- `docs/superpowers/plans/progress/2026-05-08-substrate-coalesce-redesign-prompt.md` - substrate multi-draw coalesce redesign (Option 3 / 2 glBindBufferRange + 2 glMultiDrawElementsIndirect)
- `docs/superpowers/plans/progress/2026-05-08-next-orchestrator-9p5-to-4p5.md` - next-session orchestrator targeting wolfman 9.5 -> 4.5 ms
- `docs/superpowers/brainstorms/2026-05-08-job-system-parallel-for-scope.md` - SUPERSEDED by VPL retirement plan; VPL is retired, not parallelized
