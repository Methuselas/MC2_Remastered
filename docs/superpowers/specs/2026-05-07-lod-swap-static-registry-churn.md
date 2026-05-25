# LOD-Swap-Driven Static Registry Churn — Findings + Fix Path

> **Status:** Findings doc, post-Tracy-capture-3 diagnostic. Author: 2026-05-07.
> **Predecessor:** `docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md` (H4 fix in mission-load `registerStatic`).

## Summary

Three captures with progressive instrumentation revealed that the per-actor cost we've been chasing has multiple compounding mechanisms, not one. The H4 fix landed yesterday addresses mission-load-registered actors, but a second mechanism — **LOD-swap-driven invalidate+re-register churn on tree shapes** — is responsible for both the residual perf regression AND the black-tree visual symptom under `MC2_STATIC_UPDATE_SKIP=1`.

## What capture 3 showed

`MC2_STATIC_UPDATE_SKIP=1 MC2_TEX_LIFECYCLE_TRACE=1 MC2_STATIC_PROP_TRACE=1` produced sustained per-frame churn:

```
[STATIC_PROP] invalidate regIdx=450 (was count=1)
[STATIC_PROP] register regIdx=1375 first=1388 count=1
[TEX_LIFECYCLE v1] event=pin nodeIdx=1469 refcount=196 regIdx=1375
[TEX_LIFECYCLE v1] event=unpin nodeIdx=1373 refcount=405
[TEX_LIFECYCLE v1] event=unpin nodeIdx=1374 refcount=405
[STATIC_PROP] invalidate regIdx=451 (was count=1)
[STATIC_PROP] register regIdx=1376 first=1389 count=1
... (continues at high rate; regIdx grows monotonically)
```

Pattern characteristics:
- Per-frame invalidate+re-register pairs at high rate (dozens per frame).
- `regIdx` grows monotonically — never reuses tombstoned slots.
- Pin counts on shared textures (e.g., `nodeIdx=1373 refcount=405-396` over time) showing constant churn around the same texture.
- `[TEX_LIFECYCLE v1] event=recache_multi` floods the console with per-frame texture-handle re-cache events.

## Root cause chain

`TreeAppearance::render()` at `mclib/bdactor.cpp:4199-4244` performs LOD swap inline:

```cpp
if (selectLOD != currentLOD)
{
    currentLOD = selectLOD;
    treeShape->ClearAnimation();
    delete treeShape;                                          // (1) destroy old multi
    treeShape = NULL;
    treeShape = appearType->treeShape[currentLOD]->CreateFrom(); // (2) fresh new multi (cachedFrame_ = UINT32_MAX, lightData_ = default)

    // Load textures + SetTextureHandle for each (line 4210+)
    for (long j=0; j<treeShape->GetNumTextures(); j++) {
        ...
        treeShape->SetTextureHandle(j, gosTextureHandle);     // (3) emits recache_multi log per call
    }
}
```

Trees at the LOD distance boundary hover and swap LOD every frame. Each swap:

1. Deletes the old `TG_MultiShape` (instance allocation).
2. Allocates a new `TG_MultiShape` via `CreateFrom()` (instance allocation).
3. Calls `SetTextureHandle()` once per texture (typically 1-3 per tree multi-shape).
4. **Then `BldgAppearance::render` / `TreeAppearance::render` see the shape pointer change and trigger:**
   - `invalidateStaticRegistration()` (registry tombstones the old recipe slot — never reclaimed).
   - `submitMultiShape()` (full CPU bake of the new shape's vertices/colors).
   - `registerRecipe()` (registry appends a new recipe slot — `s_recipes` and `s_recipeRanges` grow unboundedly).
   - `pinNode()` per texture on the new recipe.
   - `unpinNode()` per texture from the released old recipe.

## Three compounding cost amplifiers

1. **`registerRecipe` always appends, never reuses tombstones.** `gos_static_prop_registry.cpp:209-217`. The vectors `s_recipes` and `s_recipeRanges` grow monotonically. After ~1000 LOD swaps, the registry is carrying ~1000 dead slots in addition to live ones.

2. **`submitMultiShape` runs full CPU bake on every LOD swap.** Even if the new LOD's data was previously baked (the same actor was at this LOD a few frames ago), the bake re-runs because the new TG_MultiShape instance's `listOfVertices`/`listOfColors`/`lightData_` are all default-state.

3. **Texture handle re-cache cascade.** Each `SetTextureHandle` call emits a `recache_multi` log line (when trace is on); when trace is off, it still does the underlying state mutation. With 600+ static actors and many at LOD boundaries, the call rate is per-frame × per-tree × per-texture.

## Why the H4 fix doesn't close this

The H4 fix at `bdactor.cpp:2727,4920` sets `needsFullBakeNextFrame = true` in `registerStatic()` (the mission-load path). It does NOT cover the per-frame re-registration site at `bdactor.cpp:4389` (the LOD-swap-driven path).

Even if we extended H4 to also set the flag at the per-frame re-registration site, we'd be paying the full bake cost EVERY frame — which is exactly what the registry exists to amortize.

## Black trees under UPDATE_SKIP — same mechanism

Under `MC2_STATIC_UPDATE_SKIP=1`:
- `update()` is skipped for static-eligible actors → `touch()` runs.
- `touch()` calls `ResubmitCachedGpuLightData()` which uses the multi's existing `lightData_`.
- After LOD swap, the new TG_MultiShape's `lightData_` is default-zero (never had a full update).
- `addLightDataStructure(&zeros)` returns the slot for the all-zero entry.
- Registry flush emits the actor with all-zero lighting → **black tree**.

The H4 fix, by setting `needsFullBakeNextFrame` at registration time, handled mission-load case. LOD-swap case is symmetric but at a different code path.

## Fix paths (in order of scope)

### Fix 1 — Slot reuse in `registerRecipe` (small, immediate)

Modify `registerRecipe` to scan `s_recipeRanges` for a tombstoned slot (count==0 AND multi==nullptr) before appending. Reuse if found. Bounds the registry's memory growth.

**Estimated:** ~15 lines + tests. No semantics change to callers.

### Fix 2 — Set `needsFullBakeNextFrame` at all `registerRecipe` callsites (small, immediate)

Mirror the H4 mission-load fix at the per-frame re-register site. After `staticReg.recipeIndex = registerRecipe(...)`, set `needsFullBakeNextFrame = true`. Forces one full update on the very next frame so `lightData_` is populated before any `touch()` fires.

**Cost:** one extra `update()` per LOD swap. For a tree hovering at LOD boundary, that's one full bake per frame instead of zero — same as pre-Track-B behavior. **Closes the black-tree symptom.**

**Estimated:** ~10 lines.

### Fix 3 — Per-LOD pre-registration (architectural, multi-week)

Track B + 3.C/3.D current model: each actor has ONE `staticReg` entry, keyed on its current `treeShape`/`bldgShape` pointer. LOD swap invalidates this.

Better model: each actor has up to N `staticReg` entries (one per LOD level), pre-registered at mission load. LOD swap is a registration-state lookup instead of a re-register cycle. The registry knows which registration is "active" for this frame via the actor's `currentLOD` field.

**Cost:** schema change to `staticReg` (array of recipeIndex per LOD). All registration code paths updated. New invalidation semantics (only invalidate-all on damage state change, not on LOD swap).

**Win:** zero per-frame LOD-swap-driven CPU bake. The registry's substrate fully amortizes.

**Estimated:** 2-4 weeks. Architecturally similar to Track B's mission-load enumeration scope.

### Fix 4 — Per-LOD shared TG_MultiShape pool (architectural, large)

Currently, every actor instantiates its own per-instance `TG_MultiShape` for each LOD level it reaches. That's wasteful — the multi-shape has identical structure for all instances of the same type at the same LOD.

Better model: per-type-per-LOD shared `TG_MultiShape` pool. Each actor references the shared pool entry; per-instance state (transform, color highlight) goes elsewhere.

**Cost:** Major TG_MultiShape lifecycle refactor. Touches `msl.cpp::CreateFrom`, the entire `bldgShape`/`treeShape` storage convention, every render path.

**Win:** zero allocation/free on LOD swap. LOD swap becomes a pointer reassignment.

**Estimated:** 4-8 weeks. Likely defer until after Track C lands.

## Recommendation

**Land Fix 1 + Fix 2 together as a single small slice.** Together they close:
- Memory growth (Fix 1 reclaims tombstones)
- Black trees under UPDATE_SKIP (Fix 2 forces full bake on re-register)
- Pin-count churn (Fix 1 reuses pins via slot reuse)

What remains uncovered by Fix 1+2:
- The full CPU bake on LOD swap is still happening (just deduplicated to one per swap, not multiple). For a tree hovering at LOD boundary, this is the same per-frame work as pre-3.C.

Fix 3 is the architectural endpoint that fully eliminates per-frame LOD churn cost. It's a significant slice — recommend brainstorm + plan after Fix 1+2 ship.

## Diagnostic improvements landed in this session

To make future captures usable:

1. **Tracy zone `TreeAppr LOD_swap_reregister`** at `bdactor.cpp:4365`. Per-frame count of LOD-swap-driven invalidate calls. Visible in Tracy's call counts.
2. **Tracy zone `BldgAppr shape_swap_reregister`** at `bdactor.cpp:1629`. Same for buildings (lower expected rate — damage state swaps only).
3. **Rate-limited `recache_multi` log** at `msl.cpp:1006`. First 32 events emit verbatim; thereafter 1-in-256 sampling. Set `MC2_TEX_LIFECYCLE_TRACE_VERBOSE=1` to restore unfiltered output for narrow-window debug.
4. **`addLightDataStructure scan` zone** at `txmmgr.cpp:891`. Visible cost of the linear-scan dedup.
5. **`[LIGHT_DEDUP v1]` table-growth log** every 256 new entries.

## Cross-references

- H4 fix mission-load registration: `mclib/bdactor.cpp:2727, 4920`
- LOD swap site (the source): `mclib/bdactor.cpp:4199-4244`
- Per-frame re-registration site: `mclib/bdactor.cpp:4365-4389`
- `registerRecipe` (always-append): `GameOS/gameos/gos_static_prop_registry.cpp:205-250`
- `invalidate` (tombstone-only): `GameOS/gameos/gos_static_prop_registry.cpp:259-269`
- `MC2_STATIC_UPDATE_SKIP` mechanism debug strategy: `docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md`
