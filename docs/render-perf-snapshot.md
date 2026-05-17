# Render Performance Snapshot

**Last refreshed:** 2026-05-17 (LightsData UBO->SSBO ceiling removed `b41baec`; static-actor mission-load lighting bake shipped behind `MC2_LIGHTBAKE` kill-switch `2db2a04`; persistent static light table shipped behind `MC2_LIGHTBAKE` `38d8720` — substitutive proof PENDING user capture; drawPass-retirement Slice A+B shipped — substitutive proof confirmed; raster-triangle Fix A + Fix B shipped; VertexProjectLoop retirement plan drafted).

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
| `Terrain::render drawPass` | ~1.7 OFF / **~0.02 armed** | drawPass-retirement Slice A+B SHIPPED 2026-05-17 (`MC2_TERRAIN_INDIRECT_OVERLAY=1`): per-quad `currentQuad->draw()` loop skipped via the conjunction gate, decals from the static bake. Self-time ~1.7ms -> ~20µs armed. SUBSTITUTIVE (not displaced): user-driven non-COST_SPLIT Tracy, total frame dropped ON vs OFF (~11ms, with TRACE still on so true number is at-least-as-good) |
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
| drawPass-retirement Slice A+B | **SHIPPED 2026-05-17** (`0ee3c16` bake + clip-safe overlay VS, `3056f0e` call-site wire, `66f1ad5` probe demote). `MC2_TERRAIN_INDIRECT_OVERLAY` default-OFF = zero behavior change; conjunction gate `!(IsFrameSolidArmed() && IsFrameOverlayArmed())` is the master switch. Substitutive proof confirmed (drawPass ~0 armed AND total frame dropped, user-driven Tracy). First campaign slice after minePass to achieve genuine substitutive offload | nothing | nothing |
| Job-system parallel-for | QUEUED (post substrate-coalesce, per 2026-05-08 brainstorm) | substrate coalesce ship | parallel `vertexProjectLoop` slice (now SUPERSEDED — VPL retired, not parallelized; see plan 2026-05-14) |
| Cull-dilation + conservative-OR | SHIPPED 89e35ac 2026-05-08; makes Lifecycle motion-safe | nothing | (unblocks Lifecycle's motion-safety dimension) |
| Raster-triangle Fix A | SHIPPED `a373691` 2026-05-14 — per-ring-slot MVP snapshot, defense-in-depth | nothing | Fix B + retirement chain |
| Raster-triangle Fix B | SHIPPED `005ebc7` 2026-05-14 — pre-projected clipPos in thin record; GPU compute now sole projection authority for terrain quads in indirect path; +25 fps mc2_01 tier1 | Fix A | CPU pack retirement, cmd-patch retirement, VPL retirement |
| CPU pack path retirement | DEMOTED (not deleted) — Step 1 `18a4c36` (`PackThinRecordsForFrame` body behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK`, default-off); Step 1b-1 `4374794` (quad.cpp narrowed thin-emit gate-split, decal producer preserved); parity infra (`s_packParityMask`/`kParityMaskWords`/`gos_terrain_indirect_getPackParityMask`/`ComputeDispatchParity_Check` + the `txmmgr.cpp:1668` call) **RETIRED** 2026-05-15 — full delete, both OQ-2 gates clear (Step 4 `2e11617` retired the mask-dispatch consumer; soak-waiver; txmmgr coupling resolved by removing the call). CPU-pack body itself remains DEMOTED (`MC2_TERRAIN_INDIRECT_CPU_FALLBACK`, default-off) — full body deletion still the deferred §3 follow-up. Re-measure bucket after smoke. | Fix B (shipped) | VPL retirement step 1, cmd-patch retirement |
| Cmd-patch dispatch retirement | RETIRED 2026-05-15 in VPL-retirement Step 2b (NOT a separate queued slice) — program no longer compiled (`gos_terrain_indirect.cpp:2388`), uniform caches removed (`:1538-1539`), dispatch + parity probe retired (`:2289`/`:2953`); primary compute is sole authority (`:1485`) | (done) | (realized) |
| LightsData UBO->SSBO | **SHIPPED 2026-05-17 (`b41baec`)** — `LightsData` UBO converted to unbounded SSBO; the 64-slot light-data ceiling is removed and the pre-existing latent mc2_17 57/64 near-overflow fragility is resolved. Enabling infra only — no CPU zone retired by itself; it de-risks and unblocks the static-lighting bake (which writes more distinct slots) | nothing | static-lighting mission-load bake (`2db2a04`) |
| Static-actor mission-load lighting bake | **SHIPPED behind kill-switch 2026-05-17 (`2db2a04`)** — `MC2_LIGHTBAKE` default ON, `=0` bit-exact legacy D2 path. Retires the per-frame static (bdactor bldg/tree) `GatherLights`+decompose+template recompute (the static-populate body the D2 ON/OFF capture measured). Adversarial-clean + tier1-clean (5/5, +0 destroys, GL-clean, FPS 140-143 >= baseline). **Substitutive populate->0 + visual-parity proof is the armed USER-DRIVEN post-commit step and is NOT yet confirmed — pending `[LIGHTBRIDGE v1]` capture.** D2 stays the mech per-frame floor (genuinely-dynamic remainder) | `b41baec` (SSBO ceiling) | persistent static light table (`38d8720`) |
| Persistent static light table | **SHIPPED behind kill-switch 2026-05-17 (`38d8720`)** — `MC2_LIGHTBAKE` default ON, `=0` legacy `CacheGpuLightData` path. Retires the *survivor* the bake left: the per-frame `addLightDataStructure` 1792B FNV+memcmp for the static class (was ~1840 calls / ~0.97ms/frame bake-ON, Tracy-measured). Each static recipe now owns a permanent CPU-mirrored slot == recipeIndex (registry-keyed, never relocated, not GPU-moved), written once and re-shipped idempotently by the unchanged whole-buffer upload; `EmitBakedGpuLightData` is now a pure pointer assignment. Validated: adversarial recon-review STOP->fixes->simplified, implementation-review PROCEED (0 CRITICAL/MAJOR), tier1 5/5 +0 destroys GL-clean ~141.5 FPS neutral-to-faster. **Substitutive proof (`addLightDataStructure scan` calls/frame ~1840 -> dynamic-only + visual parity) is PENDING the user-driven post-commit capture — NOT yet confirmed.** | `2db2a04` (mission-load bake) | nothing |
| `VertexProjectLoop` retirement | PLAN DRAFTED 2026-05-14 (`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`) — 9-step interleaved sequence; needs adversarial review before step 1 | CPU pack retirement (step 1), picking repoint (step 3), cull-cascade audit (step 5) | object/prop iteration GPU port, GPU AABB mouse pick (`memory/gpu_mech_aware_mouse_pick_queued.md`) |

## Recent sign-offs and decisions

- **LightsData UBO->SSBO ceiling removed (2026-05-17, `b41baec`):** the `LightsData` block was a 64-slot UBO; converting it to an unbounded SSBO removes the hard light-data ceiling and resolves the pre-existing latent fragility where mc2_17 sat at 57/64 slots (a near-overflow that would have silently corrupted lighting once the static-bake widened the distinct-slot population). This is enabling infrastructure: no CPU zone is retired by it alone. Its purpose is to de-risk and unblock the static-lighting mission-load bake (which writes more distinct per-recipe slots than the dedup'd per-frame path did).

- **Static-actor mission-load lighting bake shipped behind kill-switch (2026-05-17, `2db2a04`):** the per-frame static (bdactor building/tree) `GatherLights`+`decomposeFirstActiveLightColor`+template-hash recompute is replaced by a lazy mission-load bake keyed on the monotonic-never-reused registry recipeIndex, re-emitting the post-decompose constant into a per-frame slot without the gather/decompose/hash. Gated by `MC2_LIGHTBAKE` (default ON; `=0` is the bit-exact legacy D2 path). This retires the static-populate body the D2 ON/OFF capture measured. D2 stays the per-frame floor for the genuinely-dynamic remainder (mechs/vehicles whose terrain-scaled per-actor color tracks position). **HONESTY CONSTRAINT — this is NOT yet a confirmed substitutive win.** Validated so far: adversarial-clean, tier1-clean (5/5, +0 destroys, GL-clean, FPS 140-143 >= baseline). The load-bearing substitutive proof (static-population zone -> ~0 in a fresh capture + visual parity, per `memory/feedback_offload_must_be_substitutive_not_additive.md`) is the armed USER-DRIVEN post-commit step and is PENDING the `[LIGHTBRIDGE v1]` capture. Do not count this among confirmed substitutive wins (minePass, drawPass) until that capture lands.

- **Persistent static light table shipped behind kill-switch (2026-05-17, `38d8720`):** the static-lighting bake (`2db2a04`) retired the gather/decompose/hash recompute but left a survivor — the per-frame `addLightDataStructure` 1792B FNV+memcmp dedup for the static class (~1840 calls / ~0.97ms/frame bake-ON, Tracy-measured). This slice designs it out: each static recipe now owns a permanent CPU-mirrored slot whose index == the monotonic registry recipeIndex (never relocated, never GPU-moved), written once at bake and re-shipped idempotently by the unchanged whole-buffer upload. `EmitBakedGpuLightData` collapses to a pure pointer assignment. Gated by `MC2_LIGHTBAKE` (default ON; `=0` is the legacy `CacheGpuLightData` path). Prior chain: `b41baec` (SSBO ceiling) -> `2db2a04` (bake) -> `8c1c491` (trace demote) -> `f3e3d8a` (snapshot) -> `38d8720` (this). **HONESTY CONSTRAINT — this is NOT yet a confirmed/measured win.** Validated: adversarial recon-review STOP->fixes->simplified, implementation-review PROCEED (0 CRITICAL/MAJOR), tier1 5/5 (+0 destroys, GL-clean, ~141.5 FPS neutral-to-faster). The substitutive proof (`addLightDataStructure scan` calls/frame ~1840 -> dynamic-only + visual parity) is the armed USER-DRIVEN post-commit capture and is PENDING. Do not count this among confirmed substitutive wins (minePass, drawPass) until that capture lands.

- **drawPass-retirement Slice A+B shipped (2026-05-17, `0ee3c16` / `3056f0e` / `66f1ad5`):** the `Terrain::render drawPass` per-quad `currentQuad->draw()` loop is retired behind `MC2_TERRAIN_INDIRECT_OVERLAY` (default-OFF; conjunction gate `!(IsFrameSolidArmed() && IsFrameOverlayArmed())` so unset = `draw()` runs = zero behavior change). Slice A bakes cement/road decals to a static VBO drawn via `DrawDecalStatic` (`Render.TerrainOverlaysStatic` hook in `renderLists()`); the earlier screen-spanning raster-sheet was root-caused (unconditional all-map draw vs non-clip-safe `terrain_overlay.vert`) and fixed with a `px.z in [0,1)` clip-safe guard (no-op for the CPU-pz-culled live path). **Substitutive proof (the campaign's load-bearing bar): user-driven non-COST_SPLIT total-frame Tracy showed drawPass self-time ~1.7ms -> ~20µs armed AND the total frame dropped ON vs OFF** (~11ms observed, captured with `MC2_TERRAIN_INDIRECT_TRACE=1` still set so the true number is at-least-as-good — the untraced `mission.update` zone the user saw is the TRACE `printf`/`fflush`). This is the first slice after minePass to clear the substitutive bar (`memory/feedback_offload_must_be_substitutive_not_additive.md`). Armed tier1 5/5, `GL_INVALID_OPERATION`=0, 4668-vert bake parity, user visual canary clean. The call site was partial-staged out of shared `txmmgr.cpp` to avoid absorbing a concurrent session's Phase-1 dynamic-sun-shadow WIP. Full state: `memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md`.

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
- Post-drawPass-retirement (2026-05-17) the new top terrain-side costs the user observed at ~11ms total: `terrain:geometry` still heavy and `drawScreen` ~3.3ms — next ROI candidates, not yet bucketed. Re-grep both on the next clean (TRACE-OFF, non-COST_SPLIT) Tracy refresh; the ~11ms armed figure itself is TRACE-poisoned (`mission.update` untraced `printf` zone) — re-baseline with `MC2_TERRAIN_INDIRECT_TRACE` unset.

## Execution prompts referenced from this snapshot

- `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` - VPL retirement (Path A, 9-step interleaved); adversarial-review-pending
- `docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md` - Fix A diagnosis + Fix A/B context for the retirement chain
- `docs/superpowers/plans/progress/2026-05-08-track-d-execution-prompt.md` - Track D Slice A (parity sign-off received)
- `docs/superpowers/plans/progress/2026-05-08-substrate-coalesce-redesign-prompt.md` - substrate multi-draw coalesce redesign (Option 3 / 2 glBindBufferRange + 2 glMultiDrawElementsIndirect)
- `docs/superpowers/plans/progress/2026-05-08-next-orchestrator-9p5-to-4p5.md` - next-session orchestrator targeting wolfman 9.5 -> 4.5 ms
- `docs/superpowers/brainstorms/2026-05-08-job-system-parallel-for-scope.md` - SUPERSEDED by VPL retirement plan; VPL is retired, not parallelized
