# PR2 Stage 0 recon — item 3: terrainDetailHandle stability across mission lifetime

**Date:** 2026-05-07
**Status:** Recon. Closes brainstorm open recon item #3
([`brainstorms/2026-05-01-detail-overlay-consolidation-scope.md:823-826`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)).
**Scope:** Confirm whether `terrainDetailHandle` (and its underlying
texture slot) is mission-static, or if it mutates mid-mission via
`tex_resolve` or other paths. Determine whether PR2a's recipe needs
an invalidation hook beyond PR1's `InvalidateRecipeForVertexNum`
chain.

---

## TL;DR

- **Underlying texture slot is mission-stable.** `detailTextureNodeIndex`
  is set ONCE per mission (at `loadTexture` time) and reset only on
  construct / destroy. No mid-mission mutation.
- **Per-frame `tex_resolve` mutates the gosTextureHandle, NOT the slot
  index.** This matches PR1's cement-atlas pattern: store slot indices
  in the recipe, resolve at draw time. No new invalidation hook needed
  for `tex_resolve`-based mutation.
- **Per-quad `terrainDetailHandle = 0xffffffff` for cement quads** is
  driven by per-quad terrain-type classification, not a slot mutation.
  PR1's `setTerrain` invalidation chain already covers terrain-type
  changes that flip detail presence on/off per quad.
- **Net result for PR2a:** recipe stays correct as captured at
  `BuildDenseRecipe` time with the existing PR1 invalidation surface.
  No additional hook required.
- **(Reminder: per recon item 6, PR2a may be retirement-only — in
  which case this entire stability question is moot.)**

---

## `terrainDetailHandle` provenance (modern path)

```cpp
// mclib/quad.cpp:428,435 — recipe builder
r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
```

`TerrainColorMap::getDetailHandle` definition at
[`mclib/terrtxm2.h:149-153`](../../../mclib/terrtxm2.h):

```cpp
DWORD getDetailHandle (void)
{
    tex_resolve(detailTextureNodeIndex);
    return (detailTextureNodeIndex);
}
```

The function calls `tex_resolve` (which can mutate the underlying
`mcTextureNodeIndex` per-frame per
[`memory/mc2_texture_handle_is_live.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mc2_texture_handle_is_live.md))
but returns the **slot index** `detailTextureNodeIndex` itself, not a
resolved gosTextureHandle. The slot index is stable.

### `detailTextureNodeIndex` mutation surface

Grep `detailTextureNodeIndex\s*=` returns 5 sites in `mclib/terrtxm2.cpp`:

| Line | Context | When |
|---|---|---|
| 91 | `detailTextureNodeIndex = 0xffffffff;` | Constructor (initial state) |
| 1032 | `detailTextureNodeIndex = 0xffffffff;` | Destructor / mission unload |
| 1045 | `detailTextureNodeIndex = mcTextureManager->loadTexture(detailFile,...)` | Mission load |
| 1047 | `detailTextureNodeIndex = mcTextureManager->loadTexture(fileName,...)` | Mission load (alt path) |
| 1619 | `detailTextureNodeIndex = mcTextureManager->loadTexture(detailFile,...)` | Mission load (alt path) |

**No mid-mission mutation.** The slot index is set once when the mission
loads and remains stable until the mission unloads.

---

## `terrainDetailHandle` provenance (legacy path)

```cpp
// mclib/quad.cpp:545-549, :629-631 — legacy fallback
DWORD terrainDetailData = Terrain::terrainTextures->setDetail(1,0);
if (terrainDetailData != 0xfffffff)
    terrainDetailHandle = Terrain::terrainTextures->getTextureHandle(terrainDetailData);
else
    terrainDetailHandle = 0xffffffff;
```

`setDetail(type=1, frame=0)` is a per-call lookup into the
`terrainTextures` catalog, returning a slot identifier. `getTextureHandle`
maps that to the mcTextureManager slot index. Both functions are
read-only against mission-static data — no mutation.

---

## Per-quad detail-presence classification

Per the brainstorm appendix and grep verification, multiple sites set
`terrainDetailHandle = 0xffffffff` on a per-quad basis when the quad
shouldn't carry detail (e.g., cement). For example (excerpt from
prior `.codex_tmp_isolate/quad_head.cpp:334`):

```cpp
terrainDetailHandle = 0xffffffff;     // Cement has NO detail!!
```

These per-quad assignments happen during `setupTextures()` based on
**terrain type** and **base-texture classification**. Terrain-type
mutations ARE captured by `MapData::setTerrain` (mapdata.cpp:1308) —
which already triggers `gos_terrain_indirect::InvalidateRecipeForVertexNum`
at mapdata.cpp:1378-1381 (per recon item 4 investigation).

So PR2a's recipe for "is detail present on this quad" stays in sync
through the existing PR1 invalidation chain. No new hook needed.

---

## What PR2a's recipe extension would store (if PR2a re-introduces detail)

Conditional on recon item 6's open question:

| Field | Source | Mission-static? |
|---|---|---|
| Detail presence flag | per-quad detail-classification at recipe-build time | Stable post-build until terrain-type change → invalidates via `setTerrain` chain |
| Detail texture-array layer | mapping from `terrainTextures2->getDetailHandle()` slot index → array layer | Stable for mission lifetime (slot doesn't mutate) |

Both fit the PR1 cement-atlas pattern: per-mission build at
`BuildDenseRecipe`/`BuildCementCatalogAtlas` time, slot-keyed lookup
that survives `tex_resolve` mutation by virtue of using slot indices
not handles.

---

## Risk R3 from brainstorm — addressed

Brainstorm Q8 R3:

> "Texture-array layer assignment must survive `tex_resolve` mutation"

Mitigation per recon: PR2a (and PR2b, PR2c) build texture arrays
**from mcTextureManager slot indices**, not from gosTextureHandle.
Slot index → array layer mapping is fixed at array-build time. Per-frame
`tex_resolve` is a no-op for the indirect path (resolution happens at
array-build via `glGetTexImage` + `glTexSubImage3D`, not per-frame). 

This is the same pattern PR1's `BuildCementCatalogAtlas` uses
([`memory/indirect_terrain_solid_endpoint.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/indirect_terrain_solid_endpoint.md)
"Slot-keyed lookup ... NodeIdx is NOT a stable key").

---

## Open follow-up: animated detail (water-detail spray)

`Terrain::terrainTextures2->getWaterDetailHandle(frameNum)` at
[`mclib/terrtxm2.h:163-169`](../../../mclib/terrtxm2.h) returns
`waterDetailNodeIndex[frameNum]` — multiple slots per animated detail.
Frame index advances per-frame in `mapdata.cpp:1056-1076`.

This is the WATER-DETAIL path, not the base-detail path. Out of PR2a
scope (water indirect endpoint is a separate workstream). Noted here
for completeness: if any future water-indirect-endpoint slice extends
to detail, frame-index mutation IS mid-mission and would need a
per-frame layer-index update in the thin record (NOT the recipe).

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `getDetailHandle` calls `tex_resolve(detailTextureNodeIndex)` then returns slot | [`mclib/terrtxm2.h:149-153`](../../../mclib/terrtxm2.h) | M |
| 2 | `detailTextureNodeIndex` mutated at construct/destroy/load only | [`mclib/terrtxm2.cpp:91, :1032, :1045, :1047, :1619`](../../../mclib/terrtxm2.cpp) — 5 sites total, all init/teardown | M |
| 3 | No mid-mission mutation of `detailTextureNodeIndex` | grep `detailTextureNodeIndex\\s*=` returns exactly the 5 sites in (2); zero are inside per-frame or mid-mission code paths | M (negative claim defended via opposite-direction grep) |
| 4 | Per-quad `terrainDetailHandle = 0xffffffff` for cement | grep returns multiple sites (legacy + .codex_tmp_isolate); current `quad.cpp` has equivalent. Per-quad classification fires from `setupTextures` based on terrain type | M (load-bearing existence; line numbers may have drifted in current quad.cpp) |
| 5 | PR1's `setTerrain` invalidates 4 vertices' recipes | [`mclib/mapdata.cpp:1375-1381`](../../../mclib/mapdata.cpp) — `gos_terrain_indirect::InvalidateRecipeForVertexNum(vn)` per affected vertex | M |
| 6 | `tex_resolve` can mutate gosTextureHandle per-frame | [`memory/mc2_texture_handle_is_live.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/mc2_texture_handle_is_live.md) load-bearing rule | M |
| 7 | PR1 cement-atlas uses slot-keyed lookup not nodeIdx | [`memory/indirect_terrain_solid_endpoint.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/indirect_terrain_solid_endpoint.md) — "NodeIdx is NOT a stable key" | M |
| 8 | Legacy `setDetail(1,0)` is a read-only catalog query | [`mclib/quad.cpp:545,629`](../../../mclib/quad.cpp) — `Terrain::terrainTextures->setDetail(1,0)`; `setDetail` returns a precomputed catalog index, no mutation of mission state | M (function-name "setDetail" misleading — semantically it's a getter) |
| 9 | Water-detail (out-of-scope) frame-indexed slots exist | [`mclib/terrtxm2.h:163-169`](../../../mclib/terrtxm2.h) — `waterDetailNodeIndex[frameNum]` array | M |

**Status summary:** 9 entries; 9 M, 0 D, 0 NF. The negative claim
"`terrainDetailHandle` is mission-stable" holds for both modern and
legacy paths.

---

## Decision input for spec session

- **No new invalidation hook needed for `terrainDetailHandle`.** PR1's
  existing `InvalidateRecipeForVertexNum` chain via `setTerrain`
  covers per-quad detail-presence classification changes.
- **Recipe stores slot indices, not handles.** Mirror PR1 cement-atlas
  pattern. Texture-array layer mapping fixed at per-mission array
  build time.
- **Brainstorm R3 risk addressed by design** — the slot-vs-nodeIdx
  discipline shipped in PR1 generalizes cleanly to PR2a/b/c.
- **All recon items 2/3/4/5/6 are now closed.** Item 1 is the only
  open recon (perf re-baseline, deferred to another session).
