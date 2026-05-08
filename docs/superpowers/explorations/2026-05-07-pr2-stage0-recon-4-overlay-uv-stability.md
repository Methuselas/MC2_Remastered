# PR2 Stage 0 recon — item 4: overlay UV mission-stability via setOverlay walk

**Date:** 2026-05-07
**Status:** Recon. Closes brainstorm open recon item #4
([`brainstorms/2026-05-01-detail-overlay-consolidation-scope.md:828-832`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)).
**Scope:** Confirm `oldminU/oldmaxU/oldminV/oldmaxV` are mission-stable
and walk `MapData::setOverlay` to confirm chokepoint identity for
parallel `OverlayRecipeSSBO` invalidation.

---

## TL;DR — corrects a brainstorm assumption

The brainstorm's Q4-overlay assertion was **wrong**:

> "Overlay UV anchors are `oldminU/oldmaxU/oldminV/oldmaxV` (NOT the
> same as detail's `minU/maxU/minV/maxV`). The mapping is per-quad,
> not per-frame (stable post-load)..."

In current code, `oldminU/oldmaxU/oldminV/oldmaxV` are **literal
constants** — not per-quad, not per-mission, not per-overlay:

```cpp
// mclib/quad.cpp:1772-1775
float oldminU = 0.0078125f;   // 1/128
float oldmaxU = 0.9921875f;   // 127/128
float oldminV = 0.0078125f;
float oldmaxV = 0.9921875f;
```

These are local-scope constants representing the standard "1/128 texel
inset" used to avoid sampling overlay-texture edges (anti-bleed). They
are NOT mutated anywhere (verified: no assignment to these names other
than the initialization).

**Spec implication for PR2b:**

- `OverlayRecipeSSBO` does NOT need per-quad UV anchors.
- The 4 inset constants (1/128, 127/128) become a shader-side `const
  vec4` — zero per-quad/per-mission storage.
- Brainstorm Q4 overlay's "parallel SSBO with per-quad
  `(oldminU,oldminV,oldmaxU,oldmaxV)` as vec4" is over-engineered.
  Recipe shape is much simpler: a presence flag + overlay layer
  index, period.

---

## What `MapData::setOverlay` actually mutates

```cpp
// mclib/mapdata.cpp:1274-1283
void MapData::setOverlay( long indexY, long indexX, Overlays type, DWORD offset )
{
    long index = indexX + indexY * Terrain::realVerticesMapSide;

    PostcompVertexPtr ourBlock = &blocks[index];
    ourBlock->textureData &= 0x0000ffff;
    ourBlock->textureData |= Terrain::terrainTextures->getOverlayHandle( type, offset );

    setTerrain(indexY,indexX,-1);
}
```

**The only mutable per-overlay data is the overlay slot handle** —
upper 16 bits of `blocks[index].textureData`, ORed in via
`Terrain::terrainTextures->getOverlayHandle(type, offset)` defined at
[`mclib/terrtxm.cpp:797`](../../../mclib/terrtxm.cpp).

The trailing `setTerrain(indexY, indexX, -1)` is the chokepoint that
invalidates the surrounding 4 vertices' face cache. Per
[`mclib/mapdata.cpp:1374-1381`](../../../mclib/mapdata.cpp), `setTerrain`
already calls `gos_terrain_indirect::InvalidateRecipeForVertexNum(vn)`
for each of the 4 affected vertices. **PR1's invalidation hook
already covers this path** — overlay mutations (via `setOverlay` or
direct `setTerrain` reuse) trigger recipe rebuild.

---

## PR2b's `OverlayRecipeSSBO` invalidation surface

**Single chokepoint.** PR2b's invalidation does NOT need a new hook:

- `MapData::setOverlay` is the only public mutation entry, and it
  ends in `setTerrain` which is already PR1-instrumented.
- `MapData::setTerrain` (called directly from terrain-typing paths)
  also triggers `InvalidateRecipeForVertexNum` for all 4 surrounding
  vertices.
- Both routes drop into the same `InvalidateRecipeForVertexNum`
  chokepoint at the indirect-terrain TU — PR2b's `OverlayRecipeSSBO`
  rebuild can piggyback on that single function with a parallel
  per-vertex invalidation hook.

**Recipe content per vertex (proposed):**

| Field | Source | Per-quad/mission? |
|---|---|---|
| Overlay presence flag | `(textureData >> 16) != 0xffff` | Per-quad, mutates on `setOverlay` |
| Overlay slot handle / layer | `textureData >> 16` → texture-array layer index | Per-quad, mutates on `setOverlay` |

**Recipe content NOT needed:**

- UV inset constants (`oldminU/etc`) — moved to shader as `const`
- `OVERLAY_ELEV_OFFSET` (0.15f) — also a shader constant
- Per-vertex world position — already shipped via PR1 SOLID recipe
  (PR2b can either re-use `worldPos0..3` from the SOLID recipe via
  back-reference index, or duplicate them in the parallel SSBO with
  a +144 B per-overlay-quad cost; spec session decides)

---

## Detail's `minU/maxU/minV/maxV` — separate behavior

`minU/maxU/minV/maxV` at quad.cpp:1768-1771 are also locally-initialized
constants based on `TERRAIN_TXM_SIZE` (a build-time constant), but
they're **conditionally overwritten** at quad.cpp:1779-1782 from
`uvData` (which IS per-quad data sourced from the recipe at
`r.uvData` per quad.cpp:427/433).

So:

- `minU/maxU/minV/maxV` — per-quad data, comes from `uvData` for
  base terrain texture sampling. Already shipped via PR1 SOLID recipe.
- `oldminU/oldmaxU/oldminV/oldmaxV` — fixed constants for overlay UV
  inset. PR2b shader-side `const`.

This distinction was conflated by the brainstorm. The clarification:
brainstorm's Q4 was correct that "overlay UV math is different" — but
that difference is structural (overlay uses fixed inset; base uses
content-derived `uvData`), not data-volume.

---

## Complete `MapData::setOverlay` chokepoint chain

| Layer | Function | Purpose |
|---|---|---|
| Public API | `Terrain::setOverlay(tileR, tileC, type, offset)` ([`mclib/terrain.cpp:909`](../../../mclib/terrain.cpp)) | Wraps mapData->setOverlay |
| Wrapper | `MapData::setOverlay(indexY, indexX, type, offset)` ([`mclib/mapdata.cpp:1274`](../../../mclib/mapdata.cpp)) | Mutates `textureData`; calls `setTerrain` |
| Cascade | `MapData::setTerrain(indexY, indexX, -1)` ([`mclib/mapdata.cpp:1308`](../../../mclib/mapdata.cpp)) | Recomputes 4 surrounding vertices; **calls `gos_terrain_indirect::InvalidateRecipeForVertexNum` per vertex at `:1378-1381`** |

Callers of `Terrain::setOverlay`/`MapData::setOverlay` (mid-mission):

- [`mclib/move.cpp:984`](../../../mclib/move.cpp) — `setOverlay(row, col, mapData[row * width + col].overlay)` during init/load
- [`mclib/move.cpp:1023`](../../../mclib/move.cpp) — `setOverlay(row, col, 0)` reset

Plus initial mission-load population (terrain data parser writes
overlay assignments to `blocks[].textureData` directly during
`MapData::init` — not through `setOverlay`). For full mission rebuild
(the equivalent of `rebuildTileMineCounts` for mines), PR2b should
have a `OnOverlayFullRebuild()` hook called at mission init AFTER
`MapData::init` populates blocks.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `oldminU` etc are local literal constants | [`mclib/quad.cpp:1772-1775`](../../../mclib/quad.cpp) — `float oldminU = 0.0078125f; ...` (no mutation in function body; verified by grep within file) | M |
| 2 | `oldminU` referenced ONLY for read (uv assignment) | [`mclib/quad.cpp:2053-2079`](../../../mclib/quad.cpp) M2d block + legacy fallback at `:2160-2308`, `:2573-2719` (all read sites; no writes) | M |
| 3 | `MapData::setOverlay` definition | [`mclib/mapdata.cpp:1274-1283`](../../../mclib/mapdata.cpp) | M |
| 4 | Overlay slot handle stored in `textureData >> 16` | [`mclib/mapdata.cpp:1280`](../../../mclib/mapdata.cpp) — `ourBlock->textureData \|= Terrain::terrainTextures->getOverlayHandle( type, offset );` (the `getOverlayHandle` returns a value already shifted up 16 bits per [`terrtxm.cpp:805`](../../../mclib/terrtxm.cpp) `txmHandle <<= 16`) | M |
| 5 | `setOverlay` ends with `setTerrain(indexY, indexX, -1)` | [`mclib/mapdata.cpp:1282`](../../../mclib/mapdata.cpp) | M |
| 6 | `setTerrain` already calls `InvalidateRecipeForVertexNum` for 4 vertices | [`mclib/mapdata.cpp:1375-1381`](../../../mclib/mapdata.cpp) | M |
| 7 | `Terrain::setOverlay` wraps `mapData->setOverlay` | [`mclib/terrain.cpp:909-911`](../../../mclib/terrain.cpp) | M |
| 8 | `move.cpp:984,1023` call `setOverlay` mid-mission | [`mclib/move.cpp:984`](../../../mclib/move.cpp), `:1023` | M |
| 9 | `getOverlayHandle` definition | [`mclib/terrtxm.cpp:797-808`](../../../mclib/terrtxm.cpp) | M |
| 10 | Detail's `minU/maxU/minV/maxV` per-quad via `uvData` | [`mclib/quad.cpp:1779-1782`](../../../mclib/quad.cpp) — `if (Terrain::terrainTextures2 && !(overlayHandle == 0xffffffff && isCement)) { minU = uvData.minU; ... }` | M |
| 11 | `OVERLAY_ELEV_OFFSET = 0.15f` | grep cite per brainstorm appendix #19 — [`quad.cpp:2050`](../../../mclib/quad.cpp) (re-grep'd at write-time; constant unchanged) | M |
| 12 | NOT mutated: `oldminU` etc — opposite-direction grep | grep `oldminU\\s*=` returns 1 line (initialization only); no other writes anywhere in worktree | M (negative claim defended via opposite-direction grep) |

**Status summary:** 12 entries; 12 M, 0 D, 0 NF. The negative claim
"`oldminU/etc` are mission-static" is upgraded to "literal constants"
via verification. Brainstorm's Q4 framing about per-overlay UV mapping
is **divergent from current code** and the spec must correct it.

---

## Decision input for spec session

- **PR2b recipe simplifies.** Brainstorm's "parallel sparse SSBO keyed
  by `vertexNum` with per-corner overlay UV pair" reduces to "parallel
  sparse SSBO keyed by `vertexNum` with overlay layer index +
  presence flag." The UV inset constants move to the overlay shader.
- **No new invalidation hook needed.** PR1's
  `InvalidateRecipeForVertexNum` chain already fires on `setOverlay`
  via `setTerrain`. PR2b adds a parallel-SSBO update path on the same
  chokepoint.
- **Brainstorm Q4 overlay decision needs spec-time correction.**
  Mention this in the spec preamble's "deltas vs brainstorm" section.
- **Mission-init full rebuild hook.** Recipe rebuild after
  `MapData::init` populates `blocks[]`, mirroring the
  `rebuildTileMineCounts`/`OnMineFullRebuild` pattern from item 5.
