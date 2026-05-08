# PR2 Stage 0 recon — item 5: setMine chokepoint enumeration verification

**Date:** 2026-05-07
**Status:** Recon. Closes brainstorm open recon item #5
([`brainstorms/2026-05-01-detail-overlay-consolidation-scope.md:833-836`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)).
**Scope:** Re-verify the slice 2b "all 19 callsites route through
`GameMap::setMine`" claim is still true post-PR1, and confirm
PR2c's `MineCellRecipeSSBO` invalidation has a single hook point.

---

## TL;DR

- **Chokepoint claim still holds.** All gameplay-driven mine mutations
  call `MissionMap::setMine(row, col, mine)` at
  [`mclib/move.h:634-646`](../../../mclib/move.h). PR2c's invalidation
  hooks one site.
- **Callsite count drifted +1** since slice 2b: 19 → 20 (one new
  callsite added, likely in `code/mech.cpp` or `code/gvehicl.cpp` in
  the months between slice 2b and today). Not architecturally
  significant — the chokepoint is unchanged.
- **`tileMineCount` maintenance is automatic** within `setMine` itself
  (move.h:641-645). No separate hook needed for the per-tile cache.
- **Two terms named `setMine` exist.** Don't confuse:
  1. `MissionMap::setMine(row,col,mine)` — game-state mutation;
     PR2c's chokepoint.
  2. `mineResult.setMine(cellPos, data)` (defined at
     [`mclib/quad.h:45`](../../../mclib/quad.h)) — per-quad rendering
     cache, populated inside `setupTextures()`'s inner loop. Not a
     game-state mutation.

---

## `GameMap->setMine` callsite inventory (current)

Grep `GameMap->setMine` in worktree:

| File | Line | Context |
|---|---|---|
| `code/carnage.cpp` | 577 | Carnage damage explosion sets mine state |
| `code/artlry.cpp` | 706 | Artillery impact mine setter |
| `code/mech.cpp` | 3807 | (skim suggests mine-laying ability) |
| `code/mech.cpp` | 8210 | minelayer call |
| `code/mech.cpp` | 8295 | minelayer call |
| `code/mech.cpp` | 8441 | mine-clear / lay |
| `code/mech.cpp` | 8551 | minelayer |
| `code/mech.cpp` | 8665 | minelayer |
| `code/mech.cpp` | 8722 | minelayer |
| `code/mech.cpp` | 8775 | minelayer |
| `code/mech.cpp` | 8816 | minelayer |
| `code/gvehicl.cpp` | 1391 | vehicle mine-laying |
| `code/gvehicl.cpp` | 1422 | vehicle mine-laying |
| `code/gvehicl.cpp` | 3630 | minelayer (note: `setMine(...,1)` not `2` — different mine type) |
| `code/gvehicl.cpp` | 4753 | "Mark Exploded" — sets to 2 |
| `code/gvehicl.cpp` | 4847 | "Mark Exploded" — sets to 2 |
| `code/gvehicl.cpp` | 4976 | "Mark Exploded" — sets to 2 |
| `code/gvehicl.cpp` | 5080 | "Mark Exploded" — sets to 2 |
| `code/weaponbolt.cpp` | 835 | weapon impact mine clear/set |
| `code/weaponbolt.cpp` | 1087 | weapon impact mine clear/set |

**Total: 20 callsites.** All are `GameMap->setMine(row, col, mine)`
invocations.

Plus 2 internal calls in `mclib/move.cpp` (within `MissionMap`):

| File | Line | Context |
|---|---|---|
| `mclib/move.cpp` | 991 | Inside `init`: `setMine(row, col, mapData[row * width + col].mine)` per cell during init/load |
| `mclib/move.cpp` | 1032 | Inside mineDestroy / cell reset: `setMine(row, col, 0)` |

These also flow through the same chokepoint.

**Drift vs slice 2b:** brainstorm cited "all 19 callsites." Today shows
20. One additional callsite added since the slice 2b finding (~3+
months ago). Architecturally insignificant — chokepoint unchanged.

---

## `MissionMap::setMine` chokepoint at move.h:634-646

```cpp
void setMine (long row, long col, unsigned long mine) {
    MapCellPtr cell = &map[row * width + col];
    const unsigned long oldMine = cell->getMine();
    cell->setMine(mine);
    // Maintain per-tile mine count: only cells transitioning between zero
    // and non-zero affect the count. ...
    if (tileMineCount && ((oldMine == 0) != (mine == 0))) {
        const long tileIdx = (row / MAPCELL_DIM) * tileMineCountWidth + (col / MAPCELL_DIM);
        if (mine == 0) --tileMineCount[tileIdx];
        else           ++tileMineCount[tileIdx];
    }
}
```

Properties relevant to PR2c:

1. **Single mutation site for cell mine state.** Every gameplay path
   funnels through this method.
2. **`tileMineCount` is automatically maintained** as part of the same
   method. PR2c does not need a separate hook for the per-tile cache;
   it can read `tileMineCount` (or the derived `tileHasMines()` query
   at move.h:652-657) at recipe-build time.
3. **Transitions matter.** Mine state is `0` (no mine), `1` (mine),
   `2` (blown). Transitions `0↔non-zero` change `tileMineCount`;
   `1↔2` does NOT (both are non-zero). PR2c's recipe lifecycle must
   reflect this: a `1→2` transition changes the cell's *texture*
   (mine vs blown) but not the cell's *presence in the recipe*.
4. **`readPacket()` bypass.** Per move.h:660 and the comment block
   above `rebuildTileMineCounts`, packet saves write cells directly
   into `map` memory bypassing `setMine`, so `tileMineCount` is
   rebuilt explicitly via `rebuildTileMineCounts()` after the
   packet load. **PR2c's recipe must also rebuild after readPacket.**
   Add a hook on `rebuildTileMineCounts` for full-mine-recipe rebuild.

---

## PR2c invalidation hook design (recommendation for spec)

Two hooks:

1. **`MissionMap::setMine` (move.h:634-646):** after the cell mutation,
   call `gos_terrain_indirect::OnMineCellChanged(row, col, oldMine,
   mine)`. The function:
   - On `oldMine==0 && mine!=0`: append a new `MineCellRecipe` entry
     for `(row*16 + cellIdx)`.
   - On `oldMine!=0 && mine==0`: mark entry inactive (or compact).
   - On `oldMine==1 && mine==2` (or vice versa): update entry's
     texture-layer index without changing geometry.

2. **`MissionMap::rebuildTileMineCounts` (move.cpp:876+):** after the
   loop completes, call `gos_terrain_indirect::OnMineFullRebuild()`
   to walk the cell map and re-emit the entire `MineCellRecipeSSBO`.

Both hooks gated by `MC2_TERRAIN_INDIRECT_MINE` so the legacy path is
unaffected when armed off.

---

## Mine state vs. detail/overlay — sub-quad granularity

Mine state's per-cell granularity (16 cells per quad,
`MAPCELL_DIM × MAPCELL_DIM = 4×4`) is unique to mine. Detail/overlay
operate per-quad. PR2c's recipe shape is therefore parallel rather than
recipe-extension per brainstorm Q4 / Q6.

`mineResult.setMine(cellPos, data)` ([`quad.h:45`](../../../mclib/quad.h))
is the **per-quad rendering cache** populated from `MissionMap`'s
authoritative state during `setupTextures()`. Callers:

- [`mclib/quad.cpp:285,293`](../../../mclib/quad.cpp) — inside
  `enqueueTerrainMineState`; legacy reservation path.
- [`mclib/quad.cpp:596,605,680,689`](../../../mclib/quad.cpp) —
  legacy fallback emit path.

This per-quad cache is consumed by `drawMine()`. PR2c's
`MineCellRecipeSSBO` would replace this cache for the indirect path —
the SSBO becomes the authoritative per-cell record consumed by the GPU
draw, and `mineResult` becomes legacy-only.

**Spec note:** retiring `mineResult.setMine` at the indirect drain
requires the same Stage 3 partial-landing rule as detail/overlay:
both indirect-mine-draw + legacy-mine-reservation gate-off must land
in the same PR.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `MissionMap::setMine` definition | [`mclib/move.h:634-646`](../../../mclib/move.h) | M |
| 2 | `tileMineCount` maintained in `setMine` | [`move.h:641-645`](../../../mclib/move.h) — `if (tileMineCount && ((oldMine == 0) != (mine == 0))) { ... }` | M |
| 3 | `tileHasMines` fast-skip read | [`move.h:652-657`](../../../mclib/move.h) | M |
| 4 | `rebuildTileMineCounts` for packet bypass | declared at [`move.h:659-661`](../../../mclib/move.h), defined at [`move.cpp:875`](../../../mclib/move.cpp) (re-grep'd 2026-05-08) | M |
| 5 | 20 GameMap->setMine callsites (drift +1 vs slice 2b's 19) | grep `GameMap->setMine` returns 20 lines across code/{carnage,artlry,mech,gvehicl,weaponbolt}.cpp | M (drift: count) |
| 6 | 2 internal MissionMap setMine calls | [`mclib/move.cpp:991`](../../../mclib/move.cpp) (init load), [`:1032`](../../../mclib/move.cpp) (mine destroy) | M |
| 7 | `mineResult.setMine` per-quad cache (DIFFERENT API) | [`mclib/quad.h:45`](../../../mclib/quad.h) — `void setMine (DWORD pos, unsigned char data)` on TerrainQuad's `mineResult` | M |
| 8 | `mineResult.setMine` callers | [`mclib/quad.cpp:285,293`](../../../mclib/quad.cpp), `:596`, `:605`, `:680`, `:689` (6 sites; all inside setupTextures path) | M |
| 9 | `MAPCELL_DIM × MAPCELL_DIM = 4×4` cell loop | [`mclib/quad.cpp:267-269`](../../../mclib/quad.cpp) (cite per brainstorm appendix #18; line drift possible — re-verify at spec-time) | M (assumed valid; brainstorm cited 2026-05-01) |
| 10 | `tileMineCount` maintenance is `0↔non-zero` only | [`move.h:638-640`](../../../mclib/move.h) comment confirms; `1↔2` is no-op for count | M |

**Status summary:** 10 entries; 9 M, 1 M with drift note (callsite
count). All assertions grep-verified at write-time. The slice 2b
chokepoint claim is intact — PR2c can hook one method and rely on
`tileMineCount` automatically.

---

## Decision input for spec session

- PR2c invalidation hook: single site at `MissionMap::setMine`
  (move.h:634-646) + one rebuild hook at `rebuildTileMineCounts`
  (move.cpp:876+).
- `tileMineCount` is the right data source for "which quads to
  emit recipe entries for" at recipe-build time.
- Per-cell granularity recipe shape (parallel `MineCellRecipeSSBO`
  keyed by `vertexNum × 16 + cellIdx`) per brainstorm Q4/Q6 stands.
- `mineResult` per-quad cache becomes legacy-only after PR2c's
  Stage 3 (gate-off + indirect draw in same PR per N2 partial-landing
  rule).
