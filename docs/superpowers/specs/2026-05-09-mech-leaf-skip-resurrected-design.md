# Mech Leaf-Skip — Resurrected Design (Slice D-leaf-skip-v2)

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Origin:** Original D-leaf-skip slice (see `2026-05-09-mech-leaf-skip-design.SUPERSEDED.md`) was STOP-THE-LINE'd by a CRIT-1 claim that aggressive leaf-skip would break `Mech3DAppearance::PerPolySelect` mouse-pick. After the diversion through D-body-shadow-skip (rolled back), D-gpu-pose-instrument (shipped, abandoned the campaign), and mech-aabb-pick (recon error, scrapped), **second-pass recon proved the original CRIT-1 was a theoretical-only hazard that doesn't fire in practice.** Mech selection goes through `findMoverByMouse` (objmgr.cpp:2468-2566) which is rect-only — comment line 2506 `// Movers are NOT per poly!!`. PerPolySelect on mechs is reachable only via the fallback `findObjectByMouse` at objmgr.cpp:2620, which only fires when no mover rect-hit (i.e., click was geometrically NOT on any mech) — in that case PerPolySelect on mechs returns false either way (today and under leaf-skip), no observable behavior change.

**This slice ships D-leaf-skip directly**, the original ~143µs/frame perf target. No precursor needed.

## Goal

Strip the per-leaf body of `mechShape->TransformMultiShape*` (per-leaf pool alloc + per-vertex screen-space projection + per-face backface cull, plus `MultiTransformShadows` dispatch) when `MC2_GPU_MECH_LEAF_SKIP=1` AND `g_useGpuMechs` AND `gos_IsTerrainTessellationActive()`.

Recon basis: every per-leaf field on `mechShape` has zero consumer in modern + GPU mech mode:
- `mechShape->Render(true)` (mech3d.cpp:2583) gated by Slice A's `(!gpuMechSubmitted && !mechGpuCullSkip)` — when GPU mech submit succeeds, doesn't fire.
- `mechShape->RenderShadows(true)` (mech3d.cpp:3073) unreachable on tessellation (`Mech3DAppearance::renderShadows` early-returns at mech3d.cpp:3054).
- `Mech3DAppearance::PerPolySelect` (mech3d.cpp:1640) — **theoretical reachability via fallback findObjectByMouse path; practical reachability for mechs is zero per recon above.** Even in the fallback case, today's behavior returns false (cursor not on mech's silhouette); leaf-skip changes the path to "always returns false" — same observable outcome.
- `submitActor` (gos_mech_batcher.cpp:560-567) reads only `listOfShapes[i].shapeToWorld.entries` — hierarchy-level, populated by the OUTER hierarchy walk (preserved by leaf-skip).
- `getNodePosition` / `getNodeNamePosition` (msl.cpp:1153, 1190) read only `shapeToWorld.entries[3/7/11]` — same hierarchy-level data.

Expected delta (mc2_10 idle, ~19 visible mechs): **~7.5µs/call mean drop on `Mech3D.UpdateGeometry.BodyXform`** (per-leaf retirement of pool alloc + projection + cull from instrumentation data). Frame-time gate ≥100µs/frame additional savings (~143µs target).

## Architecture

Reuses existing `s_buildRecipeOnly` flag in `TG_MultiShape::TransformMultiShape` (msl.cpp:1745-1746 — `continue` skips per-leaf + MultiTransformShadows). Add a thin `TransformMultiShape_HierarchyOnly` wrapper that sets `s_buildRecipeOnly` (semantic alias for self-documenting use at the mech callsite vs. reusing `_BuildRecipe` named for static-prop registry init).

Body callsite at `mech3d.cpp:3430` extends from current 2-way (FAST_TRANSFORM `_PositionsOnly` vs legacy `TransformMultiShape`) to 3-way:

```cpp
if (g_useGpuMechs && g_useGpuMechLeafSkip && gos_IsTerrainTessellationActive()) {
    mechShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &qRotation);
} else if (g_useGpuMechs && g_useGpuMechFastTransform) {
    mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
} else {
    mechShape->TransformMultiShape(&xlatPosition, &qRotation);
}
```

Precedence: LEAF_SKIP > FAST_TRANSFORM > legacy. Killswitch `MC2_GPU_MECH_LEAF_SKIP=1`, default-off (matches prior slice convention).

## Why this ships now (vs the SUPERSEDED original)

Original spec was correct in design. Plan-time review's CRIT-1 was a verification miss — it grep'd PerPolySelect's preconditions (per-leaf state) but didn't grep PerPolySelect's caller chain to confirm reachability for mechs. Caller-chain grep (this slice's recon) shows `findMoverByMouse` is rect-only; PerPolySelect-on-mech is theoretically reachable but practically returns false-or-irrelevant either way.

Lesson learned (memory-pin candidate): negative claims about call-chain reachability need TWO grep passes — the symbol's preconditions AND the symbol's reachability from real entry points.

## Failure modes

Same as SUPERSEDED original. Notably:
- **`MC2_MECH_GPU_PARITY=1` debug mode** still incompatible (the parity FBO compares CPU `Render(true)` vs GPU draw; LEAF_SKIP eliminates the per-leaf state that CPU `Render` needs, so parity output is garbage). Killswitch comment block notes incompatibility.
- **Tessellation flips at runtime** — runtime gate `gos_IsTerrainTessellationActive()` re-engages legacy path; one frame of stale shadows on flip-off, visually imperceptible.
- **First-frame nullity** — `mechShape->listOfShapes[i].listOfVertices` etc. start NULL via `CreateFrom`; LEAF_SKIP from frame 1 leaves them NULL forever; no consumer in modern + GPU mech mode.

## Verification gate

1. **Smoke matrix mc2_10 30s only** (per established cadence):
   - A: LEAF_SKIP=0 sentinel (current shipped baseline at full-bore env).
   - B: LEAF_SKIP=1 target — PASS, +0 destroys.
2. **Tracy mc2_10 90s** (USER prompts when ready):
   - **Acceptance:** `Mech3D.UpdateGeometry.BodyXform` mean drops by ≥5µs/call from current ~15.6µs toward ~10µs or lower. `TG.MultiShape.PerLeaf` per-leaf cost drops to near-zero (or zone fires far less frequently if the dispatch is skipped entirely).
   - **Frame-time gate:** `Units.Mechs` ≥100µs/frame additional reduction.
   - **σ tightens** — per-leaf variable-cost retired.
3. **Mouse-pick canary:** during 90s capture, click on mechs to confirm selection works. Per recon, this should be unaffected (findMoverByMouse rect-only).
4. **Killswitch=0 regression sentinel:** LEAF_SKIP=0 byte-identical to current shipped state.

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/msl.h` | Declare `TransformMultiShape_HierarchyOnly` near existing variants |
| Modify | `mclib/msl.cpp` | Define wrapper (reuses `s_buildRecipeOnly`) |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def |
| Modify | `mclib/mech3d.cpp` | Extend body conditional at 3430 to 3-way |

No shader changes. No SSBO. No new files.

## Out of scope

- Vehicles, buildings/trees (separate paths).
- Sensor / arm fast-transform (sensors not measurable hot, arms ~22ns/mech per instrument data — not worth pursuing).
- Default-on flip — separate slice after combined soak.

## Spec ceremony level

Given:
- The design is identical to the SUPERSEDED original (~98% mechanical reuse).
- The CRIT-1 issue was the only blocker; this revision documents why CRIT-1 was theoretical.
- Implementation is small (5 files, ~25 lines).

**Recommend skipping formal adversarial review** for this slice — the prior plan-time review's grep evidence stands; the only delta is the corrected CRIT-1 reasoning which this spec documents directly. User direction "I don't care how we get there, just reduce per-mech cost" supports fast-tracking. If user wants formal review, dispatch happens before Task 1.
