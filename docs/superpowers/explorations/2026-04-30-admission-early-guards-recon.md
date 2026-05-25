# quadSetupTextures admission / early guards — recon findings (2026-04-30)

> **Status:** Complete. Recommendation: **bundle as "Tracy hygiene + code clarity" micro-commit, NOT queue as a standalone performance slice.** The reported 0.68 ms is overwhelmingly Tracy zone-emission overhead, not genuine work. Actual recoverable CPU is ~40-160 µs.

---

## Tracy zone location

`mclib/quad.cpp:489-504`. The zone is opened by `ZoneScopedN("quadSetupTextures admission / early guards")` at `quad.cpp:490`, inside an anonymous block that spans lines 489-504. It closes at line 504, before the function's main body continues at line 506.

Code bracketed by the zone (exactly):

```cpp
// quad.cpp:491-503
if (mineTextureHandle == 0xffffffff)
{
    FullPathFileName mineTextureName;
    mineTextureName.init(texturePath, "defaults" PATH_SEPARATOR "mine_00", ".tga");
    mineTextureHandle = mcTextureManager->loadTexture(mineTextureName, gos_Texture_Alpha,
        gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
}
if (blownTextureHandle == 0xffffffff)
{
    FullPathFileName mineTextureName;
    mineTextureName.init(texturePath, "defaults" PATH_SEPARATOR "minescorch_00", ".tga");
    blownTextureHandle = mcTextureManager->loadTexture(mineTextureName, gos_Texture_Alpha,
        gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
}
```

Both `mineTextureHandle` and `blownTextureHandle` are `static DWORD` class members declared at `mclib/quad.h:82-83`. They are initialized to `0xffffffff` at `mclib/quad.cpp:136-137`. They are reset to `0xffffffff` only in `Terrain::destroy()` at `mclib/terrain.cpp:805-806` (mission teardown, not per-frame).

The zone does NOT bracket any clip-info checks, `isTerrainQuadVisible` calls, `terrainTextures2` null-test, recipe resolution, mine-state scanning, or water projection. All of those are outside the closing brace at line 504.

## setupTextures call sites

One call site in production code: `mclib/terrain.cpp:1705`.

```cpp
// terrain.cpp:1701-1707
TerrainQuadPtr currentQuad = quadList;
{
    ZoneScopedN("Terrain::geometry quadSetupTextures");
    for (i=0;i<numberQuads;i++)
    {
        currentQuad->setupTextures();
        currentQuad++;
    }
```

Single sequential loop. No nested calls. No call from any other `.cpp` file in the worktree — verified by grep across all `.cpp` files. Two definitions appear in `.codex_tmp_isolate/quad_head.cpp:120` and `.codex_tmp_isolate/quad_desired.cpp:120` but those are isolated analysis copies not part of the build.

`setupTextures` is not called from `draw()`, `drawWater()`, `drawLine()`, or `drawMine()`. The `TerrainQuad::draw` loop (terrain.cpp:959), `TerrainQuad::drawMine` loop (terrain.cpp:982), and `TerrainQuad::drawWater` / `renderWater` loop (terrain.cpp:993, 1064) are separate passes, none of which invoke `setupTextures`.

## Recursion analysis

`TerrainQuad::setupTextures` does not recurse. It does not call itself. It does not iterate sub-quads. Full call tree from within the function:

- `quad.cpp:669` — `isTerrainQuadVisible(*this)`: file-static helper at `quad.cpp:218`, reads four `clipInfo` fields only
- `quad.cpp:682` — `Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC)`: returns a cache entry pointer, no quad iteration
- `quad.cpp:686` — `Terrain::mapData->ensureTerrainFaceCacheEntryResident(...)`: cache residency bookkeeping
- `quad.cpp:699,701` — `buildTerrainRecipeInline` / `tryGetCachedTerrainRecipe`: file-static helpers at `quad.cpp:399` and `quad.cpp:428`, call `terrainTextures2->getTextureHandle()` and related — texture handle resolution only, no quad iteration
- `quad.cpp:716` — `addTerrainTriangles(recipe)`: file-static helper at `quad.cpp:447`, calls `mcTextureManager->addTriangleBulk(...)` — enqueues triangles, no quad iteration
- `quad.cpp:719-722` — `pz_emit_terrain_tris(...)`: projectZ overlay emit
- `quad.cpp:728` — `enqueueTerrainMineState(*this)`: file-static helper at `quad.cpp:232`, iterates `MAPCELL_DIM × MAPCELL_DIM` cells within the SINGLE quad, typically short-circuits via `GameMap->tileHasMines(tileR, tileC)` at `quad.cpp:245`

None of these functions call back into `setupTextures`. No recursion, no sub-quad dispatch.

## Early-out condition inventory

The admission zone brackets two conditions:

**Condition 1 — `mineTextureHandle` lazy load (quad.cpp:491-496)**
- Check: `mineTextureHandle == 0xffffffff`
- On true: construct `FullPathFileName` from global `texturePath` (extern at `mclib/paths.h:39`, defined at `mclib/paths.cpp:41` as `"data/textures/"`), call `mcTextureManager->loadTexture(...)`, assign result to static
- Input: `TerrainQuad::mineTextureHandle` — `static DWORD` shared across all `TerrainQuad` instances (quad.h:82)
- Stability: per-mission. Set once on first call after `Terrain::destroy()`. Stays set until next `Terrain::destroy()` call. Completely stable within a mission from frame 2 onward
- Classification: **per-mission stable after first load**

**Condition 2 — `blownTextureHandle` lazy load (quad.cpp:498-503)**
- Identical analysis to Condition 1 but for `TerrainQuad::blownTextureHandle` (quad.h:83)
- Classification: **per-mission stable after first load**

In steady state (all frames after frame 1 of a mission), both `if` branches are FALSE. The zone body executes as two integer-compare-with-immediate instructions and two predicted-not-taken branches.

Neither condition touches per-vertex data, per-quad mutable state, camera-derived values, cull flags, or object lifecycle state.

## 40K count explanation

The "40K calls per frame" is correct, expected, and does NOT represent 50× per quad per frame. The orchestrator description ("40,000 calls per frame on a map with ~14,000 quads = ~50× per quad per frame") conflates two distinct counts:

- **Map tile count**: `realVerticesMapSide^2`. Stock maps are 120, 100, 80, or 60 vertices per side (terrain.cpp:313-318). For a 120×120 map (mc2_01-class): 14,400 actual map tiles.
- **Visible window quad count** (`numberQuads`): `(visibleVerticesPerSide - 1)^2`. At `GameVisibleVertices = 200`, this is `199^2 = 39,601`.

`GameVisibleVertices` is hard-locked to 200 at `code/mechcmd2.cpp:1481`:

```cpp
// mechcmd2.cpp:1480-1481
GameVisibleVertices = 200;
GameVisibleVertices = 200;  // Override config — full visibility
```

Terrain allocates `200^2 = 40,000` TerrainQuad and Vertex slots at init (terrain.cpp:561-570). `MapData::makeLists` (mapdata.cpp:1080) populates `numQuads = (200-1)^2 = 39,601` entries per frame (rightmost/bottom vertex row has no associated quad; enforced by `maxX = maxY = Terrain::visibleVerticesPerSide - 1` at mapdata.cpp:1171).

The visible window at `GameVisibleVertices=200` extends substantially beyond any stock map's 120×120 tile grid. Off-map vertices receive `pVertex = blankVertex` and `vertexNum = -1` (mapdata.cpp:1107). These off-map quads still iterate through `setupTextures`, enter the admission zone, and execute the two comparisons.

**Conclusion**: 40K calls = 39,601 calls = 1× per visible quad per frame from a single loop. No amplification. No recursion. No multi-site inflation.

## Hoistability per condition

| Condition | Classification | Hoistable? | Form |
|---|---|---|---|
| `mineTextureHandle == 0xffffffff` load check | Per-mission static after first load | Yes — fully | Pre-loop check in terrain.cpp before the `setupTextures` loop, OR at mission init after `Terrain::init()` |
| `blownTextureHandle == 0xffffffff` load check | Per-mission static after first load | Yes — fully | Same as above |

Both conditions together can be hoisted to a 2-line pre-loop block in `Terrain::geometry()` (terrain.cpp:1703 area) or moved to `Terrain::init()`. After the first mission frame, all 39,601 per-quad calls are pure dead work — the statics are already loaded. The hoist eliminates the 39,601 calls to the check entirely in the steady state.

## Estimated win

**Genuine CPU work recovered: ~40-160 µs**

Steady-state cost of two integer comparisons × 39,601 calls: at ~1-4 ns/comparison pair (branch-predictor-hot, likely L1-resident static DWORD): 40-160 µs actual CPU time. This is below the σ noise floor from slice 2b (σ = 291 µs per `patchstream_shape_c.md`). Not measurable as a standalone delta via Tracy histogram.

**Tracy-reported delta from zone removal: ~0.68 ms disappears from self-time**

The reported 0.68 ms is primarily Tracy zone overhead from 39,601 zone-begin/zone-end event emissions, not genuine work. Per orchestrator working principle #2: "~30-100 ns per zone-pair." At 39,601 zones:

- 30 ns/zone-pair: 1.19 ms overhead
- 50 ns/zone-pair: 1.98 ms overhead
- 15 ns/zone-pair (optimistic): 0.59 ms overhead

The 0.68 ms measurement is consistent with ~17 ns/zone-pair effective overhead — plausible given Tracy's internal ring-buffer compression and the fact that the zone body does trivially little work.

**Realistic win**: 40-160 µs of actual CPU work recovered. The primary value of the fix is eliminating a misleading Tracy zone that attributes ~0.68 ms to work that essentially doesn't exist in the steady state.

## Slice vs refactor assessment

**Single-slice micro-fix. Zero architectural risk.**

The hoist is mechanical:
1. Move the two `if` blocks (quad.cpp:491-503) out of `TerrainQuad::setupTextures` entirely
2. Place a 4-line equivalent check in `Terrain::geometry()` immediately before the `for (i=0;i<numberQuads;i++)` loop at terrain.cpp:1703
3. Remove the Tracy zone (the anonymous block wrapper at quad.cpp:489-504 goes away)

No structural change to either function. No new data paths. No SSBO schema changes. No new state variables. `Terrain::destroy()` already resets both statics at terrain.cpp:805-806; that reset behavior is unchanged by the hoist.

Re-arming works correctly: if a mission restart calls `Terrain::destroy()` then `Terrain::init()`, the statics become `0xffffffff` again, and the pre-loop check in the next `Terrain::geometry()` call will fire and re-load correctly.

No parity validation needed — no visual output changes, no quad ordering changes, no rendering path changes. Smoke run confirming stable FPS and no crash is the appropriate gate.

## Tracy overhead assessment

The 0.68 ms is **primarily Tracy zone overhead, not genuine work**.

This zone matches exactly the anti-pattern in orchestrator working principle #2: per-quad zone measuring per-quad work below ~200 ns. In steady state (frames 2+), the zone body contains ~2-4 ns of work (two well-predicted branches). The zone's enter/exit cost of ~15-100 ns overwhelms the work by a factor of 4-50×.

The zone was appropriate during initial diagnosis (to confirm whether the lazy-load was firing on every frame). It became misleading after confirming it fires only once per mission. It should be removed as part of the hoist commit, with a comment at the hoist site noting the one-time nature of the lazy load.

**Implication for past cost modeling**: any prior analysis of `quadSetupTextures` self-time that treated 0.68 ms as "genuine work in the admission / early guards zone" was working from an inflated figure. The real recoverable budget from this code path is ~40-160 µs, not ~680 µs.

## Load-bearing constraints checklist

- **gpu_direct_renderer_bringup_checklist.md**: Not relevant. No new GPU-direct renderer. No SSBO changes. No mcTextureManager bypass.
- **cull_gates_are_load_bearing.md**: Not relevant. The two conditions check a class-static DWORD texture handle sentinel; do not touch `inView`, `canBeSeen`, `objBlockInfo`, `clipInfo`, TGL pool state, per-object update logic, or any cull-gate-adjacent field.
- **mc2_texture_handle_is_live.md**: Informational context but not a constraint. `mineTextureHandle` and `blownTextureHandle` are `mcTextureManager` texture indices set once per mission via `loadTexture`; they are NOT `TG_TypeShape::listOfTextures[slot].gosTextureHandle` values rewritten each frame by `TransformMultiShape`. The per-frame mutation trap does not apply.
- **quadlist_is_camera_windowed.md**: Not relevant. No static SSBO indexing by quadList slot.
- **feedback_offload_scope_stock_only.md**: Applicable. Tier-1 five-mission smoke run is the appropriate gate.

## Open questions for brainstorm

1. **Was the zone intentionally kept as a diagnostic tool?** No comment in the code explains it. Before removal, verify there's no scenario where the load should NOT be hoisted (e.g., hot-reload of texture paths mid-session — not a stock feature, so not a constraint).

2. **Pre-loop hoist in terrain.cpp vs hoist to `Terrain::init()`**: Loop-pre-check in terrain.cpp:1703 is unambiguously safe. Mission-init hoist requires auditing that `mcTextureManager` is initialized before `Terrain::init()` completes. Simpler choice is terrain.cpp.

3. **Does removing the zone name break downstream tooling?** The orchestrator status board references this name. After removal, the reference at `cpu-to-gpu-offload-orchestrator.md` should be updated to reflect that the zone was a measurement artifact and was retired.

4. **Is there value in measuring without Tracy overhead?** rdtsc brackets around the two comparisons (not a Tracy zone) printing per-mission average. Useful if the brainstorm wants a precise number; not required.

## Recommendation

**Ready for brainstorm — with "Tracy hygiene micro-slice" framing, not "performance slice".**

The 40K count is genuine and structurally sound (one call per visible quad per frame; `numberQuads = (GameVisibleVertices-1)² = 39,601` from the hard-locked `GameVisibleVertices=200` override at `mechcmd2.cpp:1481`). The "50× per quad per frame" framing in the original problem statement was a misread of map-tile-count vs visible-window-quad-count.

The 0.68 ms Tracy measurement is overwhelmingly zone-emission overhead (~17-100 ns/zone-pair × 39,601 pairs), not genuine branching work. Actual recoverable CPU is ~40-160 µs — below the quadSetupTextures arc's noise floor (σ = 291 µs from slice 2b).

The hoist itself is a 4-line, zero-risk change. The Tracy zone should be removed in the same commit. Primary value is removing a misleading zone that inflates the apparent cost of this path by ~500 µs in every future Tracy capture.

**Do not queue as a standalone performance slice.** Bundle into a "code clarity + zone hygiene" commit alongside any other zone-removal or diagnostic cleanup work. The genuine wall-clock delta (40-160 µs) does not justify a plan/design/parity-gate cycle.
