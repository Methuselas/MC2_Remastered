# Render Performance Snapshot

**Last refreshed:** 2026-05-08 (carried over from `mc2-render-perf-expert.md` core_knowledge during the methodology/snapshot split on 2026-05-14).

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
| `vertexProjectLoop` | ~0.4 | vestigial post C3 routing |
| `Mech3D.UpdateGeometry` x6 | ~0.3 | Track D scope |
| `Render.GpuStaticProps` | ~2 | WHEN substrate-on (currently default-off due to 323-bucket regression) |

## In-flight slices and dependencies

| Slice | State (2026-05-08) | Blocked by | Blocks |
|---|---|---|---|
| Substrate (`MC2_GPU_CULL_SUBSTRATE=1`) | BROKEN at default-on (323-bucket `glDrawElementsIndirect` serialization, ~2 ms regression) | substrate multi-draw coalesce redesign (`docs/superpowers/plans/progress/2026-05-08-substrate-coalesce-redesign-prompt.md`) | Lifecycle gate (which consumes readback ring) |
| Lifecycle gate (`MC2_GPU_CULL_LIFECYCLE=1`) | PAUSED (strategic decision 2026-05-08) | substrate fixed | nothing |
| Track D (GPU mech batcher) | PARITY SIGN-OFF 2026-05-08 (Slice A); independent | nothing | nothing (per-mech lifecycle gating obsoleted by Track D) |
| PR2 detail/overlay/mine retirement | LARGELY SHIPPED (PR2a delete, PR2b overlay scaffold, PR2c MINE static-bake default-on) | nothing | nothing |
| Job-system parallel-for | QUEUED (post substrate-coalesce, per 2026-05-08 brainstorm) | substrate coalesce ship | parallel `vertexProjectLoop` slice |
| Cull-dilation + conservative-OR | SHIPPED 89e35ac 2026-05-08; makes Lifecycle motion-safe | nothing | (unblocks Lifecycle's motion-safety dimension) |

## Recent sign-offs and decisions

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

- `docs/superpowers/plans/progress/2026-05-08-track-d-execution-prompt.md` - Track D Slice A (parity sign-off received)
- `docs/superpowers/plans/progress/2026-05-08-substrate-coalesce-redesign-prompt.md` - substrate multi-draw coalesce redesign (Option 3 / 2 glBindBufferRange + 2 glMultiDrawElementsIndirect)
- `docs/superpowers/plans/progress/2026-05-08-next-orchestrator-9p5-to-4p5.md` - next-session orchestrator targeting wolfman 9.5 -> 4.5 ms
- `docs/superpowers/brainstorms/2026-05-08-job-system-parallel-for-scope.md` - vendor Taskflow + vertexProjectLoop first-slice
