# slimReduce CULL-write de-inline (byte-identity restore)

**Date:** 2026-05-17
**Scope:** `mclib/terrain.cpp` `Terrain::geometry` slimReduce loop only. Phase-1 of the
slimReduce elimination campaign (the free/zero-risk bank; endpoint deferred).
**Stakes:** catastrophic cull axis (`cull_gates_are_load_bearing.md`) — mandated
adversarial review. Mitigated by byte-identity-by-construction.

## Problem

`[SLIMSPLIT v1]` attribution (user capture, instr commit `3519071`): the slimReduce
~584us is dominated by the **CULL bucket ~1.44M cyc/frame, stable and
camera-independent** (~60%). PROJ is gated == legacy (~45 cyc/call, cheap); RED is
small (dead consumer). The CULL regression vs the legacy `~475us` baseline is the
two deltas the terrain-indirect advisor named, of which this slice fixes the larger,
zero-risk one: the slim re-home calls the **non-inlined** `Terrain::setObjBlockActive`
/ `Terrain::setObjVertexActive` member functions per active vertex (~40k/frame),
where the legacy production *fast* path (`0c8e06b^` `s_vpFast`, lines 1633-1645)
deliberately **inlined** them as direct bounds-checked array writes — with an
explicit commit comment at `0c8e06b^:1520`: "cull-cascade setters are inlined as
direct array writes so the compiler doesn't gamble on inlining setObjBlockActive."
The re-home took the legacy *slow*-path accessor-call form instead.

## Change (byte-identical by construction)

Restore the legacy fast-path inline form. Hoist the two loop-invariant bounds
(`numObjBlocks`, `realVerticesMapSide*realVerticesMapSide` — fixed at mission load)
to pre-loop locals; replace the two member calls with the inlined bounds-checked
stores.

Accessor bodies being inlined (verbatim, `terrain.cpp:2042` / `:2056`):

```cpp
void Terrain::setObjBlockActive(long b, bool a){ if((b>=0)&&(b<numObjBlocks)) objBlockInfo[b].active=a; }
void Terrain::setObjVertexActive(long v,bool a){ if((v>=0)&&(v<(realVerticesMapSide*realVerticesMapSide))) objVertexActive[v]=a; }
```

In-loop replacement (the only edit; gate, `clipInfo` write, window-append, ordering,
the loose 768u/384u cull contract — all UNCHANGED):

```cpp
// before the for: const long ssNumObjB = numObjBlocks;
//                  const long ssNumActiveVerts = realVerticesMapSide * realVerticesMapSide;
if (rv->clipInfo) {
    const long blockNum = rv->getBlockNumber();
    if ((blockNum >= 0) && (blockNum < ssNumObjB))       objBlockInfo[blockNum].active = true;
    const long vertNum = rv->vertexNum;
    if ((vertNum >= 0) && (vertNum < ssNumActiveVerts))   objVertexActive[vertNum]   = true;
    if (s_solidNarrowOn) { /* unchanged window-append */ }
}
```

`Terrain::geometry` is a `Terrain` method → same access to the `Terrain` statics
`objBlockInfo` / `objVertexActive` / `numObjBlocks` / `realVerticesMapSide` the
accessors use. `active` is always `true` at this call site (constant-folded). The
predicates and stores are textually identical to the accessor bodies → the written
active-set is bit-identical for every input. The `s_solidNarrowOn` window-append
block is moved verbatim, unchanged.

## Why byte-identical (the review's burden)

1. Same bounds predicate (`>=0 && < N`) with `N` == the exact accessor expression
   (`numObjBlocks` / `realVerticesMapSide*realVerticesMapSide`); hoisting is legal —
   both are mission-load constants, never written in the frame loop (grep:
   set in `Terrain::init`/`initMapData`, not in `geometry`).
2. Same store (`objBlockInfo[i].active = true` / `objVertexActive[i] = true`).
3. Gate (`if (rv->clipInfo)`), `clipInfo` value, ordering vs the reduction
   `continue`, and the raster `px/pz` block are untouched → the CRIT-1
   loose-superset cull contract (`cull_cascade_wrap_and_reduce_pattern.md`) is
   preserved verbatim.
4. No cross-frame state added (stateless; not the incremental variant).

## Out of scope (queued, deferred)

RED-delete (reduction + `setInverseProject` + dead `inverseProjectZ`) — carries a
`[INVPROJZ v1]` zero-call tripwire across all 3 mission-setup paths + adversarial
review; not "free". slimReduce endpoint (granularity inversion). quadSetupTextures
(1.5ms) / Units.TerrainObjects (1.38ms) heavy sub-projects. Mission.Update — other
session.

## Gates

Mandated adversarial review (byte-identity proof) -> build RelWithDebInfo full
relink -> deploy v0.4 -> tier1 smoke 5/5 (`MC2_SLIM_COST_SPLIT=1` to confirm CULL
cyc/frame drops with active-set unchanged) -> commit `perf(terrain):`.
