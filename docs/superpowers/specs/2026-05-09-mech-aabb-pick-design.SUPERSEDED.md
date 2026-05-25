# Mech AABB Pick — Design (Slice mech-aabb-pick)

**Date:** 2026-05-09
**Branch:** `claude/gpu-mech-batcher`
**Predecessor:** Slice D-gpu-pose-instrument shipped 2026-05-09; D-gpu-pose campaign abandoned (instrumentation revealed hierarchy walk is only ~60µs/frame at 19 mechs — not enough to justify GPU compute infrastructure).
**Origin:** Decoupling slice for the queued aggressive D-leaf-skip. The D-leaf-skip slice's plan-time review found CRIT-1: `Mech3DAppearance::PerPolySelect` (mech3d.cpp:1640) requires per-leaf state populated (`listOfVertices` non-null + screen-space projected + `lastTurnTransformed == turn-1`). Without decoupling, aggressive leaf-skip silently breaks mech mouse-pick. This slice replaces per-poly mouse-pick with AABB hit-test using only hierarchy-level data — unblocks the leaf-skip slice's ~143µs/frame PerLeaf retirement target.

## Goal

Replace `Mech3DAppearance::PerPolySelect`'s per-poly hit-test with a screen-space AABB hit-test computed from `TG_TypeMultiShape::maxBox/minBox` projected through the actor's transform + camera MVP. Default-on; legacy per-poly fallback gated by `MC2_GPU_MECH_AABB_PICK=0` for bisect/regression triage.

This is **not a perf slice** (mouse-pick fires on click events, not per-frame). It's a **structural-decoupling slice** that removes the per-leaf state contract from mech mouse-pick, unblocking aggressive leaf-skip. Side benefit: bigger hit zone (slightly conservative AABB) makes mech selection feel easier — partially addresses the "hard to select stuff" UX symptom user flagged 2026-05-09.

## Recon

### Existing call chain
- `findObjectByMouse` (`code/objmgr.cpp:2444, 2453`) iterates active actors. For each, runs the bounding-rect prefilter at objmgr.cpp:2425 (`upperLeft`/`lowerRight` 2D rect from prior frame's screen-position cache), then calls `objAppearance->PerPolySelect(mouseX, mouseY)` to confirm. Bool result decides whether to return that actor as the pick winner.
- `Mech3DAppearance::PerPolySelect` (mech3d.cpp:1640-1643) calls `mechShape->PerPolySelect(mouseX, mouseY)`.
- `TG_MultiShape::PerPolySelect` (msl.h:626-635) loops `listOfShapes[i].node->PerPolySelect(...)` over every leaf. Returns true on first hit.
- `TG_Shape::PerPolySelect` (tglpp.cpp:10-95) early-returns false if any of `listOfVertices`, `listOfColors`, `listOfShadowTVertices`, `listOfTriangles`, `listOfVisibleFaces`, `listOfVisibleShadows` is null OR `lastTurnTransformed != (turn-1)`. Otherwise iterates `listOfVisibleFaces`, reads `listOfVertices[triType.Vertices[k]].x/y` (screen coords), tests if mouse is inside any triangle.

### Why per-leaf state is required
PerPolySelect's screen-space coords come from per-vertex projection populated by `MultiTransformShape*` per-leaf dispatch. Aggressive leaf-skip skips that dispatch; pointers stay null or stale; PerPolySelect's null-check gate fails; mouse-pick returns false; mech is unselectable.

### Bounding box source
`TG_TypeMultiShape::maxBox`/`minBox` (msl.h:84-85) are public `Stuff::Vector3D` members storing the model-space AABB. Populated during type construction (presumably during ASE load — to confirm during execution); represent the rest-pose extent of all sub-shapes combined.

### Coord transforms available

**Existing precedent (correct citation):** `msl.cpp:1740` builds the per-leaf `shapeToClip = shapeToWorld * TG_Shape::s_worldToClip` matrix that `tgl.cpp:2740` then uses as `xformCoords.Multiply(pos, shapeToClip)` for per-vertex projection. The viewport mapping at `tgl.cpp:2750-2762` then does `screen.x = (xformCoords.x / xformCoords.w) * viewMulX + viewAddX` (and similarly y/z/w).

**For AABB pick we don't have a per-leaf `shapeToWorld`** — the bbox is in `TG_TypeMultiShape` model space (relative to the type's root). So the projection chain we need is:

1. **Build actor world transform.** Construct a `Stuff::LinearMatrix4D actorXform` from `xlatPosition` + `qRotation` (the same local-var pattern that mech3d.cpp uses at multiple sites — e.g., near line 765, 807, 849 — for `getNodePosition` work: `xlatPosition.x = -position.x; .y = position.z; .z = position.y; qRotation = EulerAngles(0, rotation*DEGREES_TO_RADS, 0)`). Then `actorXform.BuildRotation(qRotation); actorXform.BuildTranslation(xlatPosition);` (the same construction `MultiTransformShape` uses at msl.cpp:1395-1396 for the root).
2. **Project each of 8 model-space corners.** For corner `m = (min/max.x, min/max.y, min/max.z)` (8 combinations):
   - `worldCorner = m * actorXform` — model→world.
   - `clipCorner = worldCorner * TG_Shape::s_worldToClip` — world→clip. (Note column-vector convention: this is `s_worldToClip` applied AFTER `actorXform`, NOT before.)
   - If `clipCorner.w <= 0`: corner is behind camera; skip per Q6.
   - Else: `rhw = 1.0 / clipCorner.w; screenX = (clipCorner.x * rhw) * TG_Shape::viewMulX + TG_Shape::viewAddX; screenY = (clipCorner.y * rhw) * TG_Shape::viewMulY + TG_Shape::viewAddY`.
3. **From in-front corners**, take min/max of the screen coords to form the screen-space rect. Hit-test `mouseX, mouseY` against that rect.

`TG_Shape::s_worldToClip` is a class-static, populated each frame at tgl.cpp:1596 (`s_worldToClip.Multiply(s_worldToCamera, *s_cameraToClip)`). `TG_Shape::viewMulX/viewMulY/viewAddX/viewAddY` are also class-statics, populated by camera setup. All accessible from `Mech3DAppearance::aabbPick` via `TG_Shape::*` qualifier.

### Bbox scan exclusions (informational)
`LoadTGMultiShapeFromASE` at msl.cpp:867-922 (and the binary-cache path at msl.cpp:282-323) **excludes nodes whose `nodeId` starts with `_PAB`, `LOS_`, or `SpotLight`** when computing maxBox/minBox. So the bbox is geometry-only — auxiliary nodes (line-of-sight markers, spotlight cones, particle attach points) don't inflate the bounds. This matches Q5's "body bbox only" intent without explicit gating.

### `findObjectByMouse` tie-break — REVISED to depth-based (B-ii)

**Original Q4 decision was "inherit existing iteration order" (first-hit-wins).** Plan-time review surfaced a STOP-THE-LINE: `findObjectByMouse` iterates by raw `objList` index (objmgr.cpp:2413, `for (long objIndex = 0; objIndex < listSize; objIndex++)`). NOT z-sorted. Today's per-poly works mostly because tight silhouettes rarely overlap, so first-hit-wins is "the silhouette under cursor wins." **AABB rectangles overlap MUCH more often** in clustered mechs → first-hit-wins becomes "lowest objList index wins" regardless of which mech is visually in front. **This regresses the user's "hard to select stuff" UX** for the very cluster cases the slice is supposed to improve.

**Revised Q4 decision (B-ii from spec re-review):** include depth-based tie-break in this slice. Convert `findObjectByMouse` from "early-exit on first hit" to "iterate all, track closest hit, return at end."

**Implementation:** in `findObjectByMouse` (objmgr.cpp:2404-2464):
- Replace the `return(obj)` early-exits at lines 2445 and 2454 with: track `GameObjectPtr bestHit = NULL; float bestDistSq = FLT_MAX;`, compute `distSq` from camera position to `obj->getPosition()`, update `bestHit/bestDistSq` if `distSq < bestDistSq`.
- Replace the final `return(NULL)` at line 2463 with `return bestHit;`.
- Distance metric: simple squared Euclidean from camera to actor world-space position. Adequate for tie-break (relative ordering only; not pixel-precise depth).

**Behavior change for non-mech actors:** the same depth tie-break applies to BldgAppearance / GVAppearance / etc. (their PerPolySelect goes unchanged, but the iteration becomes "exhaustive then closest"). For BUILDINGS and VEHICLES with tight per-poly silhouettes, overlap is rare so the iteration cost stays similar to today, and when overlap does occur (e.g., vehicle parked behind building), depth-based pick is generally an improvement vs lowest-objList-index. Documented as out-of-scope-acceptance: this slice's tie-break is uniform across object types.

**Per-click cost:** today exits on first hit (~1-3 PerPolySelect calls). Revised version always iterates the full searchList. searchList sizes: ~20-100 actors typical. Per-click cost: ~20-100 PerPolySelect dispatches × ~µs each = ≤100µs per click. Click events are rare (multiple per second at most); cost is negligible.

## Architecture

### New `Mech3DAppearance::PerPolySelect` body

Replace the body to dispatch on the killswitch:

```cpp
bool Mech3DAppearance::PerPolySelect(long mouseX, long mouseY)
{
    if (g_useGpuMechAABBPick) {
        return aabbPick(mouseX, mouseY);
    }
    // Legacy fallback (opt-out via MC2_GPU_MECH_AABB_PICK=0).
    return mechShape->PerPolySelect(mouseX, mouseY);
}
```

Where `aabbPick` is a new private method that:
1. Builds the 8 corners of `mechType->maxBox`/`mechType->minBox` in model space.
2. For each corner, applies actor's world transform (`xlatPosition` + `qRotation`-derived matrix), then `TG_Shape::s_worldToClip`, then perspective-divide + viewport.
3. Skips corners with `clip.w <= 0` (Q6 — behind-camera handling). If all 8 are behind camera, returns false.
4. From in-front corners, computes screen-space rect (min/max of `screenX`/`screenY`).
5. Returns `mouseX >= minScreenX && mouseX <= maxScreenX && mouseY >= minScreenY && mouseY <= maxScreenY`.

### Killswitch

```
MC2_GPU_MECH_AABB_PICK=1   (default behavior — implicit when env var unset OR set to non-empty)
MC2_GPU_MECH_AABB_PICK=0   (opt-out → legacy per-poly path)
```

Important: this killswitch's default is INVERTED relative to all prior mech slices. Prior pattern was `extern bool g_useGpuMech<Foo> = (getenv("MC2_GPU_MECH_<FOO>") != nullptr);` (default false). For this slice, the default needs to be true — flipping a strict-improvement onto operators by default. Possible patterns:
- (a) `bool g_useGpuMechAABBPick = (getenv("MC2_GPU_MECH_AABB_PICK") == nullptr || strcmp(getenv("MC2_GPU_MECH_AABB_PICK"), "0") != 0);` — default true; only "0" opts out. **My lean.** Self-documenting "set to 0 to disable" pattern.
- (b) Default true; any non-empty env var opts out. Cleaner code but reverses the existing convention semantically.

Comment block in `gos_mech_killswitch.h` must explicitly call out the inverted default to avoid future confusion.

### What the killswitch does NOT change
- Other actor types' PerPolySelect (BldgAppearance, GVAppearance, etc.) are untouched.
- The bounding-rect prefilter at objmgr.cpp:2425 still runs — AABB pick is the SECOND-stage confirmation, replacing per-poly. The first-stage rect prefilter (using `upperLeft`/`lowerRight` from screenPos cache) handles the broad reject and keeps the existing iteration cost shape.
- Sensor shapes and arm shapes are independent `TG_MultiShape` instances with their own PerPolySelect callsites elsewhere — unaffected.

## Pixel-equivalence reasoning

This slice **changes mouse-pick behavior**. AABB and per-poly produce different results in two cases:
- **AABB false positive:** mouse inside the mech's bounding rect but outside the visible silhouette (e.g., the gap between the legs of a bipedal mech). AABB returns true, per-poly returns false. Operator perception: "I clicked-empty-air and the mech got selected."
- **AABB false negative:** mouse on the visible silhouette but the mech is in an animation pose extending beyond the rest-pose bounds (e.g., extended arm during weapon fire). AABB returns false, per-poly returns true. Operator perception: "I clicked the mech and nothing happened."

Both edge cases are minor and biased TOWARD "easier selection" because:
- Most idle/walking mechs stay within their rest-pose AABB (false negatives rare).
- The gap-between-legs false positive is small relative to mech body size.

Net UX expectation: **clicking is easier and more forgiving.** This matches the user's "easier to select stuff" goal.

**Operator opt-out for parity-critical workflows:** `MC2_GPU_MECH_AABB_PICK=0` restores per-poly. Available for any session that needs strict pixel-perfect selection (e.g., regression bisect, tournament play).

## Failure modes

- **`maxBox`/`minBox` not populated for some mech types.** TG_TypeMultiShape's bbox is set during ASE load (msl.cpp:867-922 FOURTH PASS for ASE path; msl.cpp:282-323 for binary-cache path). All mech types load via these paths. If a future code path constructs a `TG_TypeMultiShape` without going through one of these loaders, bounds stay at `maxBox = -100000, minBox = +100000` per the constructor at msl.h:107-113, and the projected screen rect would be enormous or empty. **Mitigation:** in `aabbPick`, sanity-check that `maxBox.x >= minBox.x` (and y, z). If invalid, fall through to legacy per-poly path. Log once per type via env-gated `MC2_MECH_AABB_PICK_TRACE`. Damage shapes (`mechForwardDmgShape`, `mechBackwardDmgShape`) load through the same ASE path and have populated bounds — but `Mech3DAppearance::PerPolySelect` operates on `mechShape` (the live render shape) which is one of {default mech shape, damage variant}. Either way the path the bbox came from has populated bounds.
- **Camera matrices not yet initialized.** `TG_Shape::s_worldToClip` is set during camera setup before any mech update. By the time a mouse-click fires, this is always populated. Sanity-check `s_worldToClip` is non-zero matrix; if zero, fall through to legacy.
- **All 8 corners behind camera (clip.w ≤ 0).** Per Q6: return false. Mech is fully off-screen behind camera; not pickable.
- **Mech is in extreme animation pose with arms way out (e.g., weapon fire animation extending beyond rest-pose bounds).** False negative — mouse on extended arm misses. Acceptable tradeoff per Q5 (body bbox only).
- **Operator running with leaf-skip ON but AABB pick OFF (MC2_GPU_MECH_LEAF_SKIP=1 MC2_GPU_MECH_AABB_PICK=0).** This combo would silently break mouse-pick (leaf-skip removes the per-leaf state PerPolySelect needs). Killswitch comment block flags this incompatibility. Future leaf-skip slice's gate could explicitly require AABB_PICK to be on; defer that decision to leaf-skip's own design.
- **Future actor types add their own PerPolySelect overrides expecting per-leaf state.** This slice only touches Mech3DAppearance; vehicles/buildings/trees still use per-poly. If a future actor wants the same treatment, a parallel slice handles it.

## Verification gate

1. **Build clean.** `--config RelWithDebInfo` + `--target mc2`.
2. **Tier1 5/5 PASS** at default config (with AABB_PICK=1 implicit). +0 destroys.
3. **mc2_10 30s smoke** at default config. PASS, +0 destroys, no new GL errors.
4. **Mouse-pick canary (functional gate):** with the deployed exe at default-on, click on:
   - 3+ visible mechs in mc2_10 (any mission). All must select correctly.
   - A mech partially clipped at screen edge. Should select correctly (via Q6 partial-corners-in-front handling).
   - The gap between a bipedal mech's legs. Expected: false positive (mech selects). **This is acceptable per design.**
5. **Legacy fallback canary:** restart with `MC2_GPU_MECH_AABB_PICK=0`. Click on the same mechs. Behavior matches today's per-poly behavior (regression sentinel).
6. **No perf gate.** This slice is structural-decoupling, not perf-positive. Tracy can be optionally captured to confirm no new hot path; not required for ship.
7. **Adversarial review** post-implementation. Scrutiny vectors:
   - `maxBox`/`minBox` validity check covers the uninitialized-type case.
   - Behind-camera handling matches Q6 (skip corners with `clip.w <= 0`; if all 8 behind, return false).
   - Killswitch default-on inversion correctly implemented and clearly documented.
   - Other actor types' PerPolySelect untouched.
   - Iteration-order tie-break preserved (no behavior delta in `findObjectByMouse`'s caller-side logic).

## Implementation file map

| Action | File | Responsibility |
|---|---|---|
| Modify | `mclib/mech3d.cpp` | Replace `Mech3DAppearance::PerPolySelect` body with killswitch dispatch + new `aabbPick` private method |
| Modify | `mclib/mech3d.h` | Declare new private `aabbPick(long mouseX, long mouseY) const` |
| Modify | `code/objmgr.cpp` | Convert `findObjectByMouse` early-exit to depth-tie-break (B-ii). Track `bestHit`/`bestDistSq`, replace `return(obj)` with conditional update, return `bestHit` after full iteration. |
| Modify | `GameOS/gameos/gos_mech_killswitch.h` | extern bool `g_useGpuMechAABBPick` decl with explicit default-on documentation |
| Modify | `GameOS/gameos/gos_mech_batcher.cpp` | env-var def with default-on logic |
| (Optional, MINOR-3 from review) | `mclib/mech3d.cpp` | env-gated `[MECH_PICK]` trace via `MC2_MECH_AABB_PICK_TRACE` for forensic logging when "hard to select" UX complaints surface |

No shader changes. No SSBO. No new files.

## Out of scope

- Vehicles (gvactor.cpp) and buildings/trees (bdactor.cpp) keeping their own PerPolySelect — separate slices if/when desired.
- Cursor UX investigation (drag thresholds, polling, frame-cap interaction) — separate slice (B from earlier scope discussion). User explicitly deferred to a future investigation.
- ~~Depth-based tie-break for overlapping AABBs — Q4 deferred to follow-up.~~ **Promoted into this slice (B-ii)** after plan-time review surfaced that iteration-order tie-break would regress the cluster-mech UX.
- Sensor/arm AABB inclusion — Q5 deferred. Body bbox only.
- Pre-cached AABB rects — Q7 deferred. Compute fresh per pick.
- Aggressive leaf-skip slice — separate slice that lands AFTER this ships and the AABB pick is validated functionally. Will reference `MC2_GPU_MECH_AABB_PICK=1` as a precondition.

## Adversarial plan-review verdict (pre-execution)

**First pass (2026-05-09): STOP-THE-LINE.** Reviewer found:
- MAJOR-1: matrix citation pointed to wrong tgl.cpp line (per-leaf `shapeToClip`, not `s_worldToClip`). **Fixed inline:** §"Coord transforms available" rewritten to cite msl.cpp:1740 + walk through the model→world→clip math explicitly.
- MAJOR-2: damage-shape concern was a red herring (PerPolySelect operates on `mechShape`, not damage shapes). **Fixed inline:** failure-modes entry re-scoped to "if a future TG_TypeMultiShape construction path skips the standard ASE/binary loaders, bounds stay defaulted."
- MAJOR-3 (architectural): iteration-order tie-break would regress cluster-mech UX — `findObjectByMouse` is NOT z-sorted (objmgr.cpp:2413 raw index iteration). **Resolved by user direction (B-ii):** depth tie-break promoted into this slice. §`findObjectByMouse` tie-break section rewritten to spec the implementation.
- MINOR findings on bbox scan exclusions, killswitch inversion correctness, instrumentation, line-number drift — folded into spec where load-bearing.

**Second pass:** to be re-dispatched after this revision (the architectural promotion of B-ii materially changes the slice's blast radius — touches objmgr.cpp now, not just mech3d.cpp/.h + killswitch).

## Plan reference

`docs/superpowers/plans/2026-05-09-mech-aabb-pick.md` (next).
