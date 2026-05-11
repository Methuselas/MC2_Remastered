# Mech Fast-Transform — Design (Slice C3-revised)

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice C C1+C2 shipped (commit `7bc49e2`)
**Origin:** adversarial-review finding on Slice A's unrealized CPU savings

## The problem

Slice A's stated scope offloaded the mech vertex SUBMIT (`mechShape->Render(true)`) but explicitly preserved the mech vertex TRANSFORM (`mechShape->TransformMultiShape`). Empirical: tier1 mc2_10 with full B+C bore shows `mech3d.updateGeometry` at 19 calls × ~71µs = **~1.35ms/frame** still spent on per-actor mech CPU work. That's ~80% of the per-actor mech CPU cost, sitting unoffloaded by design.

Adversarial-review root cause: `TG_Shape::MultiTransformShape` (`mclib/tgl.cpp:1745-1900`) runs a per-vertex CPU lighting kernel (per-vert × per-light × saturating-add) inside `TransformMultiShape`. For a ~5K-vert mech with 1 active light, that's ~65-70µs/mech.

The cure already exists: `TG_MultiShape::TransformMultiShape_PositionsOnly` (`mclib/msl.cpp:1789`) was built for slice 2 (buildings / trees) and skips the per-vertex lighting bake. It was never wired for mechs.

## Goal

Wire `TransformMultiShape_PositionsOnly` for the GPU mech batcher path, gated by a new opt-in killswitch `MC2_GPU_MECH_FAST_TRANSFORM`. Expected payoff: ~1.0–1.2 ms/frame mainloop CPU savings on tier1 missions with ~20 mechs. Pixel-equivalent output when `MC2_GPU_MECH_LIGHTING=1` is on, because the GPU shader's `calc_light()` produces the same final color the now-stripped CPU bake would have produced.

## Architecture

### Body-only swap, all other callsites stay full

| File:line | Symbol | Decision | Rationale |
|---|---|---|---|
| `mech3d.cpp:3381` | `mechShape->TransformMultiShape` | **Swap** to `_PositionsOnly` when both env flags are on | Mech body. Slice A's `mechShape->Render(true)` bypass at mech3d.cpp:2582 is gated by `(!gpuMechSubmitted && !mechGpuCullSkip)` — when GPU mech path is on, the legacy `Render(true)` does NOT run for the body, so `listOfVertices[*].argb` has no consumer in that frame. Safe to strip. |
| `mech3d.cpp:4459` | `leftArm->TransformMultiShape` | **Leave as full** | **CRITICAL: arms `leftArm->Render(true)` at mech3d.cpp:3009 runs unconditionally — NOT gated by g_useGpuMechs.** Stripping the arm lighting bake would leave `listOfVertices[*].argb` unwritten and `Render(true)` would read pool-stale garbage. Out of scope for this slice; needs a separate slice that adds an arm GPU draw path or an arm `Render(true)` skip-when-stripped guard. |
| `mech3d.cpp:4543` | `rightArm->TransformMultiShape` | **Leave as full** | Same as left arm: `rightArm->Render(true)` at mech3d.cpp:2967 runs unconditionally; out of scope. |
| `mech3d.cpp:3377` | `mechShadowShape->TransformMultiShape` | **Leave as full** | Shadow path: `MultiTransformShadows` is dispatched alongside `MultiTransformShape` (`msl.cpp:1763-1766`); whether `_PositionsOnly` preserves shadow caster registration needs separate recon — out of scope for this slice |
| `mech3d.cpp:3579, 3585` | `sensorTriangleShape`, `sensorSquareShape->TransformMultiShape` | **Leave as full** | HUD overlays for selected mechs; small geometry, not on the perf hot path; no benefit to stripping their lighting |

### Killswitch

```
MC2_GPU_MECH_FAST_TRANSFORM=1
```

- Independent of `MC2_GPU_MECHS` and `MC2_GPU_MECH_LIGHTING` for bisect granularity (matches established Slice B1 pattern).
- Opt-in default off; flip default-on only after soak passes.
- Must be entered alongside `MC2_GPU_MECHS=1` to take effect — when GPU mech rendering is off, the legacy CPU `Render(true)` path NEEDS the per-vertex lighting bake's output (`listOfVertices[j].argb`), so we must keep the full TransformMultiShape.

### Conditional logic

```cpp
// mech3d.cpp:3381 (and arms at :4459, :4543) become:
if (g_useGpuMechs && g_useGpuMechFastTransform) {
    mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
} else {
    mechShape->TransformMultiShape(&xlatPosition, &qRotation);
}
```

## Pixel-equivalence reasoning

Stripping the per-vertex CPU lighting kernel removes writes to `listOfVertices[j].argb`. That field is consumed by:

1. **`TG_Shape::Render` per-face submit** (`tgl.cpp:2867-2950`) — Slice A skips this entire function for the GPU mech path.
2. **GPU mech batcher's `submitActor`** — reads `listOfShapes[i].shapeToWorld` for bone matrices but does NOT read `listOfVertices[j].argb`. The vertex `argb` is reconstructed in mech.vert from per-actor `inst.aRGBHighlight` and per-light `calc_light()`, NOT from the CPU-baked vertex argb.
3. **Light cache gather** (`msl.cpp:1596-1732`) — runs in the hierarchy walk before the per-leaf dispatch; `_PositionsOnly` does NOT skip the hierarchy walk, only the per-leaf vertex inner loop. Light cache state preserved.

So when `MC2_GPU_MECHS=1 && MC2_GPU_MECH_LIGHTING=1`, the stripped CPU lighting bake's output had no consumer to begin with — pixel output is identical.

When `MC2_GPU_MECHS=1 && MC2_GPU_MECH_LIGHTING=0` (Slice A flat-white shader), the shader hardcodes `baseLight=vec3(1.0)` and ignores any per-vertex lit color. Also pixel-identical.

When `MC2_GPU_MECHS=0` (legacy CPU path), the killswitch gate on this slice's conditional ensures we keep using full `TransformMultiShape`. Legacy behavior preserved.

## Failure modes

- **Shadow caster registration if shadow callsite is accidentally swapped:** explicitly out-of-scope for this slice; the conditional is wired at the body site only. Shadow callsite `3377` left alone with full `TransformMultiShape`.
- **Arm rendering with stripped lighting bake (CRITICAL hazard caught at plan review):** `leftArm->Render(true)` (`mech3d.cpp:3009`) and `rightArm->Render(true)` (`mech3d.cpp:2967`) run UNCONDITIONALLY — they are NOT inside the `(!gpuMechSubmitted && !mechGpuCullSkip)` guard that gates `mechShape->Render(true)` for the body. Therefore the arm `Render` always reads `listOfVertices[*].argb`. If we stripped the arm lighting bake, those reads would return pool-stale garbage. **Mitigation:** arms stay on full `TransformMultiShape` in this slice. Future slice that adds an arm GPU draw path OR an arm `Render(true)` skip-when-stripped guard can revisit.
- **Consumers of `listOfVertices[j].argb` (full enumeration via grep):**
  - `tgl.cpp:2506` — inside `MultiTransformShape` per-leaf path itself. Bypassed by construction when `_PositionsOnly` is used (the per-leaf dispatch picks `_PositionsOnly` instead, which does not enter this path).
  - `tgl.cpp:Render`, `RenderShadows`, legacy-debug paths — only runs via `mechShape->Render(true)` which Slice A bypasses for the body.
  - `gos_static_prop_batcher.cpp:914` — runs only for static-prop actors (buildings/trees) when `g_useGpuObjects` is set; mechs do NOT route through static_prop_batcher, so no interference.
  - No other reads exist in the worktree (verified by `grep -rn 'listOfVertices\[.*\]\.argb' --include="*.cpp" --include="*.h"`).
- **Bounds / cull data dependencies:** `TransformMultiShape_PositionsOnly` writes `shapeToWorld` and the cached lastTransformed turn (`tgl.cpp:2724`); bounds extraction at `recalcBounds` reads `shapeToWorld`, not `listOfVertices`. Cull preserved.
- **TGL pool allocation:** the per-leaf `MultiTransformShape` path calls into `TG_VertexPool::getVerticesFromPool` for the rendered output. `_PositionsOnly` skips this — TGL pool consumption drops by per-mech-vertex-count. Net positive (already-tight pool budget gets relief).
- **Hierarchy walk static flag race (`s_multiShapePositionsOnly`):** `msl.cpp:1370-1376` documents the flag is single-threaded-safe (no static reentrancy possible because TransformMultiShape is called serially per actor in the update loop). Reviewer-confirmed.

## Verification gate

This slice's smoke gate is stricter than B1's because the cure deliberately changes a load-bearing path's per-vertex behavior, and the user has already noted Slice B's verification was incomplete.

1. **Tier1 5/5 PASS** at `MC2_GPU_MECHS=1 MC2_GPU_MECH_LIGHTING=1 MC2_GPU_MECH_FAST_TRANSFORM=1` — +0 destroys, fallback_total=0.
2. **Tracy comparison:** before-vs-after `GameLogic.Mech3D.UpdateGeometry` zone average (verified zone name at `mech3d.cpp:3184`). The zone wraps the WHOLE function — body + shadow + arms (if blown) + sensors (if selected). Body-only swap retires only ~half of the zone's cost.
   - **Acceptance:** Tracy zone delta ≥30µs/call (body's per-vertex lighting bake removed). Mainloop frame time drop ≥0.5ms on mc2_10's ~19 mechs. **Original ~1.0–1.2ms estimate was for a body+arms slice that was de-scoped at plan review per the CRITICAL arm hazard.**
3. **Operator visual A/B:** `MC2_GPU_MECH_FAST_TRANSFORM=0` vs `=1` with the rest of the bore identical, both at `MC2_GPU_MECH_LIGHTING=1`. Mechs must look pixel-identical to operator-visual confidence (any drift signals a missed `listOfVertices[j].argb` consumer).
4. **mc2_24 stress test:** different mech mix, larger active population. Confirm tier1 PASS at full bore; tracy delta visible.
5. **Killswitch=0 regression sentinel:** `MC2_GPU_MECHS=0` (CPU baseline) must remain bit-for-bit identical to pre-slice — i.e. when `g_useGpuMechs=false` the conditional at the three callsites takes the else branch and the slice's gate change is a no-op.
6. **Adversarial review** post-implementation, scrutiny vectors:
   - Conditional placement matches the spec exactly (no accidental swap of shadow callsite).
   - `_PositionsOnly` actually populates `shapeToWorld` for the GPU mech batcher's bone SSBO upload to read.
   - File-static flag race-safety holds.
   - No code path reads `listOfVertices[j].argb` while `g_useGpuMechs && g_useGpuMechFastTransform` is set.

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool `g_useGpuMechFastTransform` decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def `MC2_GPU_MECH_FAST_TRANSFORM` |
| Modify | `mclib/mech3d.cpp` | conditional swap at 3381 ONLY (NOT 3377, 3579, 3585, 4459, 4543) |

No shader changes. No new files. Smallest possible patch.

## Out of scope

- Shadow path (`mech3d.cpp:3377`) — defer to its own slice once `_PositionsOnly` impact on `MultiTransformShadows` dispatch is recon'd.
- Arm sub-actors (`mech3d.cpp:4459, :4543`) — defer to its own slice. Arms `Render(true)` runs unconditionally (mech3d.cpp:2967, 3009), so stripping the arm lighting bake would render garbage. Arm fast-transform requires either an arm GPU draw path through the GpuMechBatcher or an arm `Render(true)` skip-when-stripped guard.
- Sensor / HUD shapes — leave as full; not on the hot path.
- GPU bone-hierarchy compute (the originally-deferred Slice C3) — still deferred; the bone walk is ~5µs/mech and not worth the complexity.
- Animation pose interpolation — separate slice.
- Tracy zone instrumentation tightening — pre-existing instrumentation should suffice; if not, add as a separate diagnostic slice.

## Plan reference

`docs/superpowers/plans/2026-05-09-mech-fast-transform.md` (next).
