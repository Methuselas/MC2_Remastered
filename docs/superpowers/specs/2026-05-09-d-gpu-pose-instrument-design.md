# D-gpu-pose Instrument Pass — Design (Slice D-gpu-pose-instrument)

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice D-shadow-state-strip shipped 2026-05-09; Slice D-body-shadow-skip rolled back (didn't deliver measurable perf)
**Origin:** D-gpu-pose decomposition slice 1. The post-D-shadow-state-strip mc2_10 idle Tracy histogram is **trimodal** (peaks ~17/30/70µs on `Mech3D.UpdateGeometry`); the trimodality reflects structural work I haven't identified yet. Before committing to GPU-compute architecture, instrument the per-stage cost so slice 3+ can target the actual cost center — and so we don't repeat the body-shadow-skip mistake of building infrastructure for work that turned out to be small.

## Decomposition context

This is slice 1 of 4 in the D-gpu-pose campaign:

1. **D-gpu-pose-instrument (this slice):** add Tracy sub-zones for per-stage attribution. Pure measurement. No behavior change.
2. **D-gpu-pose-skeleton-upload:** static skeleton SSBO + per-actor pose state SSBO. Data infrastructure only. Killswitch off; no compute.
3. **D-gpu-pose-compute:** new compute shader walks skeleton, writes bone matrices to SSBO; CPU mirror populated alongside (for `getNodePosition`); VS reads bone matrices.
4. **D-gpu-pose-cpu-strip (queued, decide after 3 ships):** strip the CPU hierarchy walk if compute path validates.

Mechs only. Vehicles (gvactor.cpp) and buildings/trees (already on Track B) deferred.

## Goal

Land 8 coarse Tracy zones across `mech3d.cpp` and `msl.cpp` so a 90s mc2_10 capture surfaces per-stage µs/call histograms. The user reads the result and decides what slice 3 targets — or whether D-gpu-pose pays for itself at all.

## Why instrument first

The prior slice (D-body-shadow-skip) was rolled back because the work it skipped (`MultiTransformShadows` on body shape) turned out to be small at this mech count + light count. Recon was correct (no consumer in modern + GPU mech mode); the cost-center estimate was wrong. **Measure before architect.** D-gpu-pose is a much bigger commitment than the prior slices (new compute shader, SSBO layout, CPU↔GPU sync), so a measurement gate before commitment is prudent.

The user's note that "all mechs have the lights and 'sensors'" and "all mechs have arms attached by default" contradicts my prior histogram attribution (I'd assumed selected-mech-only sensors and blown-arm-only upper peaks). The trimodality must come from somewhere else; instrumentation will surface it.

## Architecture

### Tracy zone insertions

All zones use `ZoneScopedN(...)` (CPU-side; no GPU timer queries needed). Default-on (no killswitch). Permanent instrumentation. Names follow the existing convention `<Subsystem>.<Function>.<Stage>` so Tracy filtering works.

| # | Site | Zone name | Stage description |
|---|---|---|---|
| 1 | `mech3d.cpp:Mech3DAppearance::updateGeometry` body+shadow state setter block (currently `if (mechShadowShape && !stripShadowState)` at line 3382 + the `mechType->setAnimation`/`SetFrameNum`/etc. block at 3360-3404) | `Mech3D.UpdateGeometry.AnimPose` | The state-setter prelude before the body transform call. Current cost mostly attributed to this is bundled in parent zone. |
| 2 | `mech3d.cpp:Mech3DAppearance::updateGeometry` body transform conditional at line 3430 (the `mechShape->TransformMultiShape*` 2-way conditional) | `Mech3D.UpdateGeometry.BodyXform` | The dominant per-mech work. Will internally also be measured by zones 6-8 inside TransformMultiShape. |
| 3 | `mech3d.cpp:Mech3DAppearance::updateGeometry` sensor block at line ~3618-3651 (`sensorTriangleShape->TransformMultiShape`, `sensorSquareShape->TransformMultiShape`) | `Mech3D.UpdateGeometry.Sensors` | Per user: ALL mechs have sensors. Need attribution. |
| 4 | `mech3d.cpp:Mech3DAppearance::updateGeometry` foot-poof / weapon-effect block (the `if (visible && (sensorLevel > 4) && !InEditor && useNonWeaponEffects)` block at line ~3437+, including `getNodePosition` calls for foot positions) | `Mech3D.UpdateGeometry.Effects` | `getNodePosition` callers; potential cost contributor. |
| 5 | `mech3d.cpp:Mech3DAppearance::updateGeometry` arms block (the `leftArm->TransformMultiShape` and `rightArm->TransformMultiShape` callsites at ~4498/4582). Per user: arms attached by default — both fire on most mechs. **NOTE:** these callsites are ~1000 lines below the parent function entry. They may live in a separate function. Recon-during-execution: confirm the actual scope; if separate function, wrap a parent zone there. | `Mech3D.UpdateGeometry.Arms` | All mechs have arms attached by default per user. |
| 6 | `mclib/msl.cpp:TG_MultiShape::TransformMultiShape` hierarchy walk (the loop at ~1500-1734 that computes `shapeToWorld` per shape via animation interpolation + matrix concat) | `TG.MultiShape.HierarchyWalk` | Future GPU-compute target. |
| 7 | `mclib/msl.cpp:TG_MultiShape::TransformMultiShape` per-leaf dispatch (the loop body at ~1745-1761 that calls `MultiTransformShape*` per leaf, plus its `s_buildRecipeOnly continue`) | `TG.MultiShape.PerLeaf` | The pool alloc + per-vertex projection + per-face cull. PerPolySelect contract lives here. |
| 8 | `mclib/msl.cpp:TG_MultiShape::TransformMultiShape` `MultiTransformShadows` dispatch (~1763-1766) | `TG.MultiShape.ShadowProj` | Confirm what D-body-shadow-skip would have skipped is actually small. |

### What we deliberately skip

- **Shadow-shape state setters** (the `if (mechShadowShape && !stripShadowState)` block at mech3d.cpp:3382). D-shadow-skip + D-shadow-state-strip already retired or attributed this work. Adding more zones there would clutter without new info.
- **Per-vertex / per-leaf-iteration zones.** Violates the worktree CLAUDE.md 100ns floor rule.
- **GPU timer queries.** This slice is CPU-only; no rendering changes.
- **MC2_GPU_POSE_INSTRUMENT killswitch.** Default-on permanent instrumentation; Tracy overhead (~50ns × 8 zones × per-mech-per-frame) is well below the work being measured.

### Naming convention

`<Subsystem>.<Function>.<Stage>` — matches existing zones like `GameLogic.Units.Mechs`, `GameLogic.Mech3D.UpdateGeometry`. Stage names are short noun phrases that describe the work, not the function. Future Tracy filters (`TG.MultiShape.*` to see TG_MultiShape's internals) work cleanly.

### Sub-zone nesting in Tracy

When `Mech3D.UpdateGeometry` parent zone fires, child zones 1-5 nest inside it; when zone 2 (`BodyXform`) fires, child zones 6-8 (inside the called `TransformMultiShape`) nest inside zone 2. Tracy auto-handles the visualization. Per-call sample counts will differ between parent and children (children fire less often if their stage has early-return paths).

## Pixel-equivalence reasoning

Tracy zones are pure observers — `ZoneScopedN` constructs a stack-allocated profiler scope with no behavioral side effects. **Trivially pixel-identical.** No need for visual A/B; smoke pass + tier1 5/5 confirms no regression.

## Failure modes

- **Tracy zone overhead skews the measurement we're taking.** Per the worktree rule, zone overhead is ~50ns. With 8 zones × ~18 mechs × ~60 FPS ≈ 8.6K zones/sec × 50ns = 0.4ms/sec ≈ 0.04% — negligible.
- **Zone insertion site doesn't match function flow.** E.g., the arms block (zone 5) may be in a separate function ~1000 lines from `updateGeometry` entry. Mitigation: recon during execution; insert wrapping zone in whatever function actually owns the arms callsite.
- **Tracy capture window misses transient peaks.** Mitigated by 90s capture window and the user's instinct (already validated on prior slices) to scroll Tracy to a representative section.
- **Existing `Mech3D.UpdateGeometry` parent zone semantics change.** The parent zone stays exactly as today. Sub-zones add observability without altering the parent's measurement.

## Verification gate

1. **Build clean** at `RelWithDebInfo` with `--target mc2`.
2. **Tier1 5/5 PASS** at the current full-bore config (`MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_CULL=1 MC2_GPU_MECH_SKIN=1 MC2_GPU_MECH_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_FAST_TRANSFORM=1 MC2_GPU_MECH_SHADOW_SKIP=1 MC2_GPU_MECH_SHADOW_STATE_STRIP=1`). +0 destroys.
3. **mc2_10 30s smoke** — same env. PASS, +0 destroys.
4. **mc2_10 90s Tracy capture** (USER prompts) — verify 8 zones visible in Tracy GUI; per-stage histograms readable. **No accept-gate on µs/call values; the OUTPUT of this slice is the measurement itself.**
5. **No new GL errors. No fallback events. No `[TGL_POOL]` regressions.**

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/mech3d.cpp` | Insert 5 sub-zones inside `Mech3DAppearance::updateGeometry` (zones 1-5) |
| Modify | `mclib/msl.cpp` | Insert 3 sub-zones inside `TG_MultiShape::TransformMultiShape` (zones 6-8) |

No header includes (Tracy is already in scope via `gos_profiler.h`, included transitively). No new files. No killswitch infrastructure. Smallest-possible patch.

## Out of scope

- The 4 follow-on slices (skeleton-upload, compute, cpu-strip, default-on flip) — separate slices.
- Any architectural change.
- Vehicles (gvactor.cpp).
- Sub-bone-iteration zones (would violate 100ns floor).

## Adversarial plan-review verdict (pre-execution)

To be filled after Task 0 dispatch.

## Plan reference

`docs/superpowers/plans/2026-05-09-d-gpu-pose-instrument.md` (next).
