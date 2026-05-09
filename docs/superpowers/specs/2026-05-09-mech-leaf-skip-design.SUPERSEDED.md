# Mech Leaf-Skip — Design (Slice D-leaf-skip)

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice D-shadow-state-strip shipped 2026-05-09
**Origin:** the body slice (C3-revised) used `_PositionsOnly`, which strips the per-vertex CPU lighting kernel but keeps per-leaf pool allocs + per-vertex screen-space projection + per-face backface cull. In modern + GPU mech mode, those per-leaf outputs have no consumer — Slice A bypasses `mechShape->Render(true)` (mech3d.cpp:2583) and `mechShape->RenderShadows(true)` is unreachable on tessellation (mech3d.cpp:3054).

## The problem

Tracy mc2_10 idle, full bore + state-strip: `Mech3D.UpdateGeometry` mean 22.52µs/call. Histogram is trimodal:
- Lower peak ~17µs (fast body + shadow elided)
- Middle peak ~30µs (likely body + sensor shapes for selected mechs)
- Upper peak ~70µs (mechs with arms still on full transform per body-slice CRIT-1)

The body slice's `_PositionsOnly` runs the OUTER hierarchy walk + per-leaf inner work. For ~25-50 leaves per mech × per-vertex projection × per-face cull, this is the dominant residual cost on the lower-peak population. Stripping the per-leaf entirely would compress the lower peak further.

The cure already exists: `s_buildRecipeOnly` flag in `TG_MultiShape::TransformMultiShape` (`mclib/msl.cpp:1745-1746`) makes the loop `continue` past both the per-leaf dispatch AND the `MultiTransformShadows` dispatch. The wrapper `TransformMultiShape_BuildRecipe` (`msl.cpp:1804-1810`) was built for Track B's static-prop registry init.

This slice adds a thin runtime wrapper `TransformMultiShape_HierarchyOnly` (reusing the same `s_buildRecipeOnly` flag) and gates the body callsite to use it under a new killswitch.

## Recon answers

### Q1 — Per-leaf state consumers on `mechShape` in modern + GPU mech mode

Comprehensive grep of `mechShape->Render`, `mechShape->RenderShadows`, and per-leaf field readers (`->listOfVertices`, `->listOfTriangles`, `->listOfVisibleFaces`, `->listOfShadowTVertices`, `->listOfVisibleShadows`, `lastTurnTransformed`):

| Consumer | Site | Reachable in modern + GPU mech mode? |
|---|---|---|
| `mechShape->Render(true)` | `mech3d.cpp:2583` | **No** — gated by Slice A's `(!gpuMechSubmitted && !mechGpuCullSkip)`. When GPU mech submit succeeds, doesn't fire. |
| `mechShape->RenderShadows(true)` | `mech3d.cpp:3073` | **No** — `Mech3DAppearance::renderShadows` early-returns on tessellation at `mech3d.cpp:3054`. |
| `TG_Shape::Render` | `tgl.cpp:2867-2982` | only via `mechShape->Render(true)` (above) |
| `TG_Shape::RenderShadows` | `tgl.cpp:3577` | only via `mechShape->RenderShadows(true)` (above) |
| `TG_Shape::PerPolySelect` (tglpp.cpp:14-21) | (mouse-pick path) | not invoked on mechShape per-frame in normal play; only via `Render`-path codepaths |
| MC2_MECH_GPU_PARITY=1 (debug-only opt-in) | shadow FBO comparison | guarded by env var; default off; can be guarded in slice predicate if needed |

Conclusion: no reader of `mechShape`'s per-leaf state in normal play. Stripping per-leaf is safe.

### Q2 — `lastTurnTransformed` gate

All readers of `lastTurnTransformed` live inside `TG_Shape::Render` (tgl.cpp:2881), `TG_Shape::RenderShadows` (tgl.cpp:3598), and `PerPolySelect` (tglpp.cpp:20, 31) — same consumer set as Q1. Skipping per-leaf means the gate fails and these functions early-return — but they're unreachable for `mechShape` anyway.

### Q3 — GPU mech batcher's `submitActor`

Reads only `desc.mechShape->listOfShapes[i].shapeToWorld.entries` (gos_mech_batcher.cpp:560-567) for bone matrices. **Hierarchy-level data**, populated by the OUTER hierarchy walk that this slice preserves. Not affected by per-leaf strip.

### Q4 — Sub-actor / animation / `getNodePosition` dependencies

- **Arms** (`leftArm`, `rightArm`) are **separate `TG_MultiShape` instances** with their own per-leaf state. This slice operates only on `mechShape`; arm callsites at `mech3d.cpp:4498/4582` unchanged.
- **Sensor shapes** (`sensorTriangleShape`, `sensorSquareShape`) are also separate instances. Unchanged.
- **`getNodePosition` / `getNodeNamePosition`**: read only `listOfShapes[i].shapeToWorld.entries[3/7/11]` (msl.cpp:1153, 1190) — hierarchy-level data only. Author comment "DO NOT UPDATE THE HEIRARCHY!!!! This thing may not have updated itself this turn yet!!!" (msl.cpp:1175) confirms intentional independence from per-leaf state.
- **Mech destruction**: when status changes, mechShape is swapped to a destruction shape and re-runs through the same TransformMultiShape* path. No special per-leaf reader.
- **Animation pose**: handled by state setters (`setAnimation`, `SetFrameNum`, `SetNodeRotation`) that mutate node-level state, not per-leaf state. Unaffected.
- **Foot poofs / weapon node positions**: all go through `getNodePosition` (Q4 above).

### Q5 — Reuse `_BuildRecipe` vs new wrapper

`TransformMultiShape_BuildRecipe` (msl.cpp:1804-1810) sets `s_buildRecipeOnly = true`, which the per-leaf dispatch loop (`msl.cpp:1745-1746`) reads to `continue` past per-leaf + `MultiTransformShadows` dispatch. **Functionally identical to what we want.**

But the symbol name `BuildRecipe` was chosen for static-prop registry init use; a future reader seeing `mechShape->TransformMultiShape_BuildRecipe(...)` at the mech runtime callsite would be confused. Add a thin alias `TransformMultiShape_HierarchyOnly` that reuses the same flag — five new lines in msl.cpp, no functional change, semantically self-documenting.

## Goal

Add `TransformMultiShape_HierarchyOnly` as a thin wrapper sharing `s_buildRecipeOnly`'s mechanism. Gate the body callsite to use it under `MC2_GPU_MECH_LEAF_SKIP=1` AND `g_useGpuMechs` AND `gos_IsTerrainTessellationActive()`. Body callsite becomes a 3-way: LEAF_SKIP > FAST_TRANSFORM > legacy.

Expected payoff (mc2_10 idle, ~18 visible mechs):
- Per-call mean: -10 to -15µs/call (per-leaf pool alloc + per-vertex projection + per-face cull retired)
- Frame time: ~180-270µs/frame additive
- Lower bimodal peak should compress further toward ~5-7µs/call (residual hierarchy walk + animation pose). Approaches the structural floor short of GPU pose compute.

## Architecture

### New wrapper in msl.cpp

After the existing `TransformMultiShape_BuildRecipe` definition at `msl.cpp:1804-1810`, add:

```cpp
//-------------------------------------------------------------------------------
// Slice D-leaf-skip (2026-05-09): thin runtime wrapper that reuses the
// s_buildRecipeOnly mechanism for the GPU mech body callsite. The flag's
// semantic effect (skip per-leaf dispatch + MultiTransformShadows) is exactly
// what the GPU mech path needs in modern + tessellation mode: Slice A bypasses
// mechShape->Render(true), and mechShape->RenderShadows(true) is unreachable
// on tessellation (mech3d.cpp:3054). All per-leaf state (listOfVertices,
// listOfTriangles, listOfVisibleFaces, listOfShadowTVertices, etc.) has zero
// reader in this configuration. The shared flag is single-threaded-safe (set/
// clear inside the wrapper); same lifetime discipline as _PositionsOnly and
// _BuildRecipe. Aliased here for self-documenting use at the mech callsite
// rather than reusing the BuildRecipe name (which is static-prop-registry-
// scoped semantically).
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_HierarchyOnly (Stuff::Point3D *pos, Stuff::UnitQuaternion *rot)
{
    s_buildRecipeOnly = true;
    long result = TransformMultiShape(pos, rot);
    s_buildRecipeOnly = false;
    return result;
}
```

Add corresponding declaration in `msl.h` near the existing `TransformMultiShape_PositionsOnly` and `TransformMultiShape_BuildRecipe` declarations.

### Body callsite extension

Current body callsite at `mech3d.cpp:3416-3420`:
```cpp
if (g_useGpuMechs && g_useGpuMechFastTransform) {
    mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
} else {
    mechShape->TransformMultiShape(&xlatPosition, &qRotation);
}
```

Becomes 3-way:
```cpp
if (g_useGpuMechs && g_useGpuMechLeafSkip && gos_IsTerrainTessellationActive()) {
    mechShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &qRotation);
} else if (g_useGpuMechs && g_useGpuMechFastTransform) {
    mechShape->TransformMultiShape_PositionsOnly(&xlatPosition, &qRotation);
} else {
    mechShape->TransformMultiShape(&xlatPosition, &qRotation);
}
```

Precedence: LEAF_SKIP > FAST_TRANSFORM > legacy.

### Killswitch

```
MC2_GPU_MECH_LEAF_SKIP=1
```

- Independent of all other mech killswitches (bisect granularity).
- Opt-in default off.
- Requires `g_useGpuMechs=true` AND `gos_IsTerrainTessellationActive()` to take effect. Belt-and-suspenders gate on tessellation: if a user disables tessellation, `mechShape->RenderShadows(true)` becomes reachable and would need per-leaf state.

### Killswitch reachability after this slice

```
LEAF_SKIP=1   → per-leaf dispatch + MultiTransformShadows dispatch on body skipped (hierarchy walk + animation pose preserved)
LEAF_SKIP=0   → falls through to FAST_TRANSFORM (_PositionsOnly) or legacy (_TransformMultiShape) per existing C3-revised body-slice gating
```

## Pixel-equivalence reasoning

When LEAF_SKIP fires:
- `mechShape->listOfShapes[i].shapeToWorld` populated by hierarchy walk → submitActor (gos_mech_batcher.cpp:560-567) reads correctly.
- Animation pose state (`currentAnimation`, `frameNum`, `baseRotation`) advanced by setAnimation/SetFrameNum/SetNodeRotation calls preceding the conditional → hierarchy walk uses it for matrix concat.
- Per-leaf `listOfVertices` etc. NOT populated → no consumer reads them (Q1).
- `MultiTransformShadows` NOT dispatched → `listOfShadowTVertices` etc. NOT populated → no consumer reads them (Q1, Q2).

When LEAF_SKIP off, behavior is byte-identical to current shipped C3-revised body slice.

**Pixel output identical.**

## Failure modes

- **`MC2_MECH_GPU_PARITY=1` debug mode:** the parity FBO compares CPU `mechShape->Render(true)` vs GPU draw. With LEAF_SKIP=1, the CPU path can't render correctly (no per-leaf state). Mitigation: the predicate is a separate killswitch from PARITY; an operator using both gets garbage parity output, which is documentable. Slice ships with a comment at the predicate: "incompatible with `MC2_MECH_GPU_PARITY=1` — disable LEAF_SKIP if running parity diagnostic."
- **Tessellation flips at runtime:** if a mod toggles tessellation off mid-mission with LEAF_SKIP=1, `mechShape->RenderShadows(true)` becomes reachable on the next frame; its `lastTurnTransformed != turn` gate at tgl.cpp:3598 fails → silent early-return. At most one frame of stale shadows on tessellation flip-off; visually imperceptible. Same mitigation as D-shadow-skip.
- **First-frame nullity:** `mechShape->listOfShapes[i].listOfVertices` etc. start NULL via `CreateFrom` (`tgl.cpp:231/393/399`). LEAF_SKIP from frame 1 leaves them NULL forever. No consumer dereferences them in this configuration. Safe.
- **Static flag race-safety:** `s_buildRecipeOnly` is single-threaded; wrapper sets+clears synchronously. No interaction with other actors' updateGeometry calls (serial in mech update loop). No interaction with `s_multiShapePositionsOnly` (the body slice's wrapper sets/clears its own flag at line 3417, and we're in the LEAF_SKIP branch which uses the BuildRecipe flag — the two flags are independent).
- **`s_listOfLights` dependency:** `setAnimation`/`SetFrameNum`/`SetNodeRotation`/`SetLightList` calls preceding the body conditional at line 3407 still fire (NOT gated by LEAF_SKIP). The body's hierarchy walk reads `s_listOfLights` for per-shape light state — but only if per-leaf dispatch fires. With LEAF_SKIP, per-leaf is skipped; light state is unread. Safe.
- **Future code adding a reader of mechShape per-leaf state:** documentary risk only. Killswitch is opt-in; default-off behavior unchanged.

## Verification gate

1. **Smoke matrix on mc2_10 30s only** (per user direction):
   - A: STATE_STRIP=1 LEAF_SKIP=0 (current shipped baseline) — sentinel.
   - B: STATE_STRIP=1 LEAF_SKIP=1 (this slice's target) — PASS, +0 destroys, fallback_total=0.
2. **Tracy mc2_10 90s** (USER prompts when ready):
   - **Acceptance:** `Mech3D.UpdateGeometry` mean drops by ≥6µs/call from 22.52µs (D-shadow-state-strip baseline) toward ~16µs or lower. Lower bimodal peak compresses (mode shifts down further from 16.4µs).
   - **Frame-time gate:** ≥100µs/frame on `Units.Mechs` outer zone.
   - **σ should tighten further** as the variable-cost per-leaf work retires.
3. **Operator visual A/B** on mc2_10: LEAF_SKIP=0 vs =1, otherwise identical full-bore env. Mech body, arms, sensors, and shadows must look pixel-identical.
4. **Killswitch=0 regression sentinel:** with LEAF_SKIP=0 (and STATE_STRIP=0 for full regression), behavior is bit-for-bit identical to current shipped state.
5. **Adversarial review post-implementation.**

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/msl.h` | add decl `TransformMultiShape_HierarchyOnly` near existing variants |
| Modify | `mclib/msl.cpp` | add wrapper after `TransformMultiShape_BuildRecipe` definition (5 lines, reuses `s_buildRecipeOnly`) |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool `g_useGpuMechLeafSkip` decl |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def `MC2_GPU_MECH_LEAF_SKIP` |
| Modify | `mclib/mech3d.cpp` | extend body conditional to 3-way at the existing C3-revised callsite |

## Out of scope

- Sensor / arm fast-transform — separate slices; arms still call `Render(true)` unconditionally per body-slice CRIT-1.
- GPU bone-hierarchy compute (D-gpu-pose) — separate slice; the residual hierarchy walk + animation pose is the floor for this slice's approach.
- Default-on flip for LEAF_SKIP — separate slice after combined soak.
- Renaming `s_buildRecipeOnly` to reflect dual use (mech runtime + static-prop init) — bikeshed; the new wrapper makes the use site self-documenting.
- Eventual removal of the body slice's `_PositionsOnly` path once LEAF_SKIP is default-on — premature; staged retirement after soak.

## Adversarial plan-review verdict (pre-execution)

To be filled after Task 0 dispatch.

## Plan reference

`docs/superpowers/plans/2026-05-09-mech-leaf-skip.md` (next).
