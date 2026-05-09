# Mech Body Shadow-Projection Skip — Design (Slice D-body-shadow-skip)

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice D-shadow-state-strip shipped 2026-05-09
**Origin:** D-leaf-skip's plan-time adversarial review found CRIT-1 — `Mech3DAppearance::PerPolySelect` (mech3d.cpp:1640) is the canonical mover-selection path and requires per-leaf state populated. The aggressive "skip per-leaf entirely" approach would silently break mouse-pick on mechs. This slice retreats to a narrower scope: keep `_PositionsOnly` semantics (PerPolySelect contract preserved) but additionally skip the `MultiTransformShadows` dispatch on the body shape, which has no consumer in modern + GPU mech mode.

The original D-leaf-skip docs are kept as `*.SUPERSEDED.md` for historical record of the CRIT-1 finding.

## What this slice actually does

The body slice's `_PositionsOnly` (C3-revised, shipped) calls per-leaf `MultiTransformShape_PositionsOnly` (skips per-vertex lighting bake) but keeps:
- Per-leaf pool allocation (PerPolySelect needs pointers non-null)
- Per-vertex screen-space projection (PerPolySelect needs `listOfVertices[j].x/y/z` populated)
- Per-face backface cull (PerPolySelect needs `listOfVisibleFaces` populated)
- `lastTurnTransformed = turn` bump (PerPolySelect's staleness gate at tglpp.cpp:20)
- The unconditional `MultiTransformShadows` dispatch at msl.cpp:1763-1766

That last item — `MultiTransformShadows` on the BODY shape (not `mechShadowShape`) — does per-light × per-vertex shadow projection writing to the body's `listOfShadowTVertices`, `listOfVisibleShadows`, `numVisibleShadows`. **In modern + GPU mech mode, those outputs have no consumer:**
- `mechShape->RenderShadows(true)` (mech3d.cpp:3073) is unreachable on tessellation (mech3d.cpp:3054 early-returns).
- No other code reads `mechShape`'s shadow vertex state.

This slice introduces a new flag `s_skipMultiTransformShadows` checked at the dispatch site (msl.cpp:1763), and a new wrapper `TransformMultiShape_PositionsOnlyNoShadowProj` that sets both `s_multiShapePositionsOnly` AND `s_skipMultiTransformShadows`. Body callsite gates to use the new wrapper under `MC2_GPU_MECH_BODY_SHADOW_SKIP=1`.

## Recon (extends D-shadow-skip and D-leaf-skip's audit)

### Q1 — Consumers of `mechShape`'s shadow projection state in modern + GPU mech mode

Same audit as D-leaf-skip's superseded spec, except now scoped specifically to `MultiTransformShadows`-populated fields (`listOfShadowTVertices`, `listOfVisibleShadows`, `numVisibleShadows`, `shadowsVisible[]`):

| Consumer | Site | Reachable in modern + GPU mech mode? |
|---|---|---|
| `TG_Shape::RenderShadows` (only consumer of `listOfShadowTVertices`) | tgl.cpp:3577-3706 | Only fires via `mechShape->RenderShadows(true)` at mech3d.cpp:3073, which is unreachable due to `Mech3DAppearance::renderShadows` early-returning on tessellation at mech3d.cpp:3054 |
| `addShadowShape` (modern dynamic shadow registration) | tgl.cpp:3052 (inside `TG_Shape::Render`) | `mechShape->Render(true)` at mech3d.cpp:2583 is gated by Slice A's `(!gpuMechSubmitted && !mechGpuCullSkip)`. When GPU mech submit succeeds, doesn't fire. AND modern `g_shadowShapes[]` (txmmgr.cpp:130) doesn't index from `mechShape`'s shadow projection state. |

**Conclusion:** `mechShape`'s `MultiTransformShadows` outputs have zero consumer in this configuration.

### Q2 — Static flag race-safety

The new flag `s_skipMultiTransformShadows` is set+cleared inside the new wrapper (single-threaded actor update). It's independent of `s_multiShapePositionsOnly` and `s_buildRecipeOnly`. The wrapper composes the two flags by setting `s_multiShapePositionsOnly` AND `s_skipMultiTransformShadows` simultaneously:

```cpp
long TG_MultiShape::TransformMultiShape_PositionsOnlyNoShadowProj (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_multiShapePositionsOnly  = true;
    s_skipMultiTransformShadows = true;
    long result = TransformMultiShape(pos, rot);
    s_skipMultiTransformShadows = false;
    s_multiShapePositionsOnly  = false;
    return result;
}
```

No reentrancy. No concurrent setter. Sequential within a single mech's `updateGeometry`.

### Q3 — Why this preserves PerPolySelect

`Mech3DAppearance::PerPolySelect` (mech3d.cpp:1640) → `mechShape->PerPolySelect` → `TG_Shape::PerPolySelect` (tglpp.cpp:10-33). Reads:
- `listOfVertices[j].x/y/rhw` (screen-space projection) — populated by `_PositionsOnly`'s per-vertex projection ✓
- `listOfTriangles`, `listOfVisibleFaces` — populated by `_PositionsOnly`'s per-face backface cull ✓
- `lastTurnTransformed` — bumped by `_PositionsOnly` at tgl.cpp:2724 ✓
- Pool pointers non-null — `_PositionsOnly` allocates all 6 pools at tgl.cpp:2705-2713 ✓

Skipping ONLY `MultiTransformShadows` (which writes shadow projection state, not screen-projection state) leaves all of PerPolySelect's preconditions satisfied. **Mouse-pick contract preserved.**

**Load-bearing distinction (CRIT-1 reopen risk):** PerPolySelect's null-check gate at `tglpp.cpp:14-21` requires `listOfShadowTVertices` and `listOfVisibleShadows` to be **non-null** (pointer-level), even though it never indexes into their CONTENTS. These pointers are populated by `_PositionsOnly`'s **pool allocations** at `tgl.cpp:2708/2713` — independent of whether `MultiTransformShadows` runs. This slice skips only the per-light × per-vertex CONTENT writes, leaving the pointers valid. **A future "skip pool alloc on body" optimization would reopen CRIT-1.** The comment at `tgl.cpp:2651` ("PerPolySelect at tglpp.cpp:14-21 requires ALL pointers non-null") is the canonical warning.

### Q4 — Slice A fallback path safety

If `submitActor` returns false (late registration or shader init failure), Slice A falls back to `mechShape->Render(true)` at mech3d.cpp:2583. With `_PositionsOnlyNoShadowProj` the per-leaf state is fully populated (same as `_PositionsOnly`). `Render(true)` reads `listOfVertices[j]`, `listOfTriangles`, `listOfVisibleFaces`, `tri.aRGBLight[i]` — all populated.

The only thing different: `tri.aRGBLight[i]` per-face lighting. Body slice C3-revised already strips that under `_PositionsOnly` (the per-face lighting kernel at tgl.cpp:2807-2810 was stripped in `MultiTransformShape_PositionsOnly`). So Slice A fallback under `_PositionsOnlyNoShadowProj` would render with stripped per-face lighting — same as today's body-slice fallback. No new degradation introduced by this slice.

(The body slice's CRIT-1 review accepted this fallback degradation because falling back from GPU to CPU rendering is already a degraded scenario.)

### Q5 — Naming clarity

Two existing prior killswitches have similar names:
- `MC2_GPU_MECH_SHADOW_SKIP` (D-shadow-skip): skips `mechShadowShape->TransformMultiShape*` entirely (the dedicated shadow caster's whole transform).
- `MC2_GPU_MECH_BODY_SHADOW_SKIP` (this slice): skips only the `MultiTransformShadows` dispatch on the body shape (`mechShape`).

Distinct flags, distinct code paths, distinct shapes. Comment block on each must call out the disambiguation.

## Goal

Add `TransformMultiShape_PositionsOnlyNoShadowProj` wrapper + new flag `s_skipMultiTransformShadows`. Gate the body callsite to use it under `MC2_GPU_MECH_BODY_SHADOW_SKIP=1` AND `g_useGpuMechs` AND `gos_IsTerrainTessellationActive()`. Body callsite becomes a 3-way: BODY_SHADOW_SKIP > FAST_TRANSFORM > legacy.

Expected payoff (mc2_10 idle, ~18 visible mechs):
- Per-call mean: -3 to -8µs/call (per-light × per-vertex shadow projection on body retired)
- Frame time: ~50-150µs/frame additive on `Units.Mechs` outer zone
- σ tightens (per-light variable cost retired on body)

Smaller than D-leaf-skip's superseded estimate because the per-leaf body of `_PositionsOnly` (which dominates) is preserved.

## Architecture

### New flag in msl.cpp

Add a static at the file scope near the existing `s_buildRecipeOnly` and `s_multiShapePositionsOnly`:

```cpp
// Slice D-body-shadow-skip: gates the unconditional MultiTransformShadows
// dispatch in TransformMultiShape's per-shape loop (msl.cpp:1763-1766) so
// the body shape's per-light × per-vertex shadow projection can be skipped
// when no consumer exists (modern + GPU mech mode). Set/cleared inside the
// _PositionsOnlyNoShadowProj wrapper. Single-threaded.
static bool s_skipMultiTransformShadows = false;
```

### New wrapper in msl.cpp

After the existing `_BuildRecipe` definition:

```cpp
//-------------------------------------------------------------------------------
// Slice D-body-shadow-skip (2026-05-09): composes _PositionsOnly with skipping
// the unconditional MultiTransformShadows dispatch. The body's MultiTransformShadows
// outputs (listOfShadowTVertices etc.) are consumed only by RenderShadows, which
// is unreachable on tessellation (mech3d.cpp:3054 early-return). PerPolySelect's
// contract (per-leaf pool alloc + per-vertex screen projection + per-face cull +
// lastTurnTransformed bump) is preserved by the underlying _PositionsOnly mechanism.
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_PositionsOnlyNoShadowProj (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_multiShapePositionsOnly   = true;
    s_skipMultiTransformShadows = true;
    long result = TransformMultiShape(pos, rot);
    s_skipMultiTransformShadows = false;
    s_multiShapePositionsOnly   = false;
    return result;
}
```

### MultiTransformShadows dispatch gate in TransformMultiShape

In `msl.cpp:1763-1766`, change:

```cpp
if (useShadows && d_useShadows)
{
    listOfShapes[i].node->MultiTransformShadows(pos, &(listOfShapes[i].shapeToWorld), yawRotation);
}
```

To:

```cpp
if (useShadows && d_useShadows && !s_skipMultiTransformShadows)
{
    listOfShapes[i].node->MultiTransformShadows(pos, &(listOfShapes[i].shapeToWorld), yawRotation);
}
```

This gate is read INSIDE the per-shape loop, so the cost is one bool check per shape per call. Sub-100ns per check; negligible.

### Body callsite extension

Current at `mech3d.cpp:3430-3434`:
```cpp
if (g_useGpuMechs && g_useGpuMechFastTransform) {
    mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
} else {
    mechShape->TransformMultiShape(&xlatPosition, &qRotation);
}
```

Becomes:
```cpp
if (g_useGpuMechs && g_useGpuMechBodyShadowSkip && gos_IsTerrainTessellationActive()) {
    mechShape->TransformMultiShape_PositionsOnlyNoShadowProj(&xlatPosition, &qRotation);
} else if (g_useGpuMechs && g_useGpuMechFastTransform) {
    mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
} else {
    mechShape->TransformMultiShape(&xlatPosition, &qRotation);
}
```

Precedence: BODY_SHADOW_SKIP > FAST_TRANSFORM > legacy.

### Killswitch

```
MC2_GPU_MECH_BODY_SHADOW_SKIP=1
```

- Independent of all other mech killswitches for bisect granularity.
- Opt-in default off.
- Requires `g_useGpuMechs=true` AND `gos_IsTerrainTessellationActive()`.
- **Distinct from `MC2_GPU_MECH_SHADOW_SKIP`** (which targets `mechShadowShape`'s entire transform; this targets `mechShape`'s `MultiTransformShadows` dispatch).

### Killswitch reachability after this slice

```
BODY_SHADOW_SKIP=1   → MultiTransformShadows on mechShape skipped; per-leaf still runs (PerPolySelect preserved)
BODY_SHADOW_SKIP=0   → falls through to FAST_TRANSFORM (current shipped) or legacy
```

## Pixel-equivalence reasoning

When BODY_SHADOW_SKIP fires:
- `mechShape->listOfShapes[i].shapeToWorld` populated by hierarchy walk → submitActor + getNodePosition correct ✓
- Per-leaf state (listOfVertices etc.) populated by `_PositionsOnly` → PerPolySelect contract satisfied ✓
- `MultiTransformShadows` skipped → `mechShape`'s shadow projection state stale/null. **No reader exists** in modern + GPU mech mode (Q1) ✓

When BODY_SHADOW_SKIP off, behavior is byte-identical to current shipped C3-revised body slice.

**Pixel output identical.**

## Failure modes

- **Tessellation flips at runtime:** runtime gate prevents skip when tessellation off. Same belt-and-suspenders as D-shadow-skip.
- **First-frame nullity:** `mechShape`'s shadow vertex pools allocated by `_PositionsOnly` even in BODY_SHADOW_SKIP path; just unwritten. No reader. Safe.
- **Slice A fallback (Render path):** `Render(true)` reads `listOfVertices`, `listOfTriangles`, `listOfVisibleFaces`, `tri.aRGBLight[i]`. All populated by `_PositionsOnly`. `Render` does NOT read `listOfShadowTVertices` (that's `RenderShadows`-only). So fallback degrades identically to today's `_PositionsOnly` fallback — no new failure mode.
- **PerPolySelect:** preserved — Q3 above.
- **`s_skipMultiTransformShadows` race:** flag set+cleared inside wrapper; single-threaded actor update; no reentrancy.
- **Future code adding a reader of `mechShape`'s shadow projection state:** documentary risk only. Killswitch opt-in; default-off behavior unchanged.

## Verification gate

1. **Smoke matrix on mc2_10 30s only** (per user direction):
   - A: BODY_SHADOW_SKIP=0 sentinel.
   - B: BODY_SHADOW_SKIP=1 target — PASS, +0 destroys.
2. **Tracy mc2_10 90s** (USER prompts):
   - **Acceptance:** `Mech3D.UpdateGeometry` mean drops by ≥2µs/call from 22.52µs (D-shadow-state-strip baseline) toward ~20µs or lower. `Units.Mechs` outer zone: ≥30µs/frame additional reduction.
   - σ tightens.
3. **Operator visual A/B** on mc2_10: BODY_SHADOW_SKIP=0 vs =1, otherwise identical full-bore env. Mech body, arms, sensors, modern dynamic shadows must look pixel-identical.
4. **Mouse-pick canary:** during the 90s capture, click on a few friendly mechs. Selection must work — confirms PerPolySelect contract preserved (CRIT-1 mitigation).
5. **Killswitch=0 regression sentinel:** BODY_SHADOW_SKIP=0 byte-identical to current shipped state.
6. **Adversarial review post-implementation.**

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/msl.h` | declare `TransformMultiShape_PositionsOnlyNoShadowProj` |
| Modify | `mclib/msl.cpp` | add `s_skipMultiTransformShadows` static, gate at MultiTransformShadows dispatch (line 1763), define new wrapper |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def |
| Modify | `mclib/mech3d.cpp` | extend body callsite at 3430 to 3-way |

## Out of scope

- Aggressive leaf-skip (skip per-leaf entirely on body) — blocked by PerPolySelect; deferred to follow-up after "GPU-mech-aware mouse pick" precursor lands. Coordinate with cursor rework (see `cursor_rework_pending.md`).
- Sensor / arm fast-transform.
- GPU bone-hierarchy compute (D-gpu-pose).
- Default-on flip.

## Adversarial plan-review verdict (pre-execution)

READY FOR EXECUTION (2026-05-09, second pass). Reviewer confirmed the CRIT-1 mitigation holds: per-leaf pool allocations and per-vertex screen projection (PerPolySelect's hard preconditions) survive untouched; only the per-light × per-vertex shadow content writes are skipped. The new flag composes correctly with `s_multiShapePositionsOnly`, doesn't perturb `_BuildRecipe`'s upstream `continue`, and the body-callsite 3-way short-circuits cleanly. 5 MINOR documentary findings; the load-bearing one (pool-alloc vs content-write distinction) is incorporated above as the "Load-bearing distinction (CRIT-1 reopen risk)" note. No CRITICAL/MAJOR.

## Plan reference

`docs/superpowers/plans/2026-05-09-mech-body-shadow-skip.md` (next).
