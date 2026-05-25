# quadSetupTextures — next-slice handoff (post M2d)

> Status: handoff for fresh session. M2d-overlay landed 2026-04-29 (`Terrain::render drawPass` 25→1.46 ms, `fast=14000 legacy=0`). The next CPU bucket worth attacking is `Terrain::geometry quadSetupTextures` (~1.17 ms/frame in the last Tracy snapshot, April 27). Substantial prior work exists; this doc is a pickup map, not a fresh design.

---

## What's already in the tree

This is **not** a greenfield slice. Three layers of infrastructure already exist:

1. **Shape A — TexResolveTable** — landed default-on (`87666c6`, `5f72d3e`). Per-frame lazy memoization of `MC_TextureNode` handles. Validated: the M2d Gate C log shows `[TEX_RESOLVE v1] event=shutdown total_frames=3777 total_resolves=815180 mismatches=0 oob=0`.

2. **Shape B — PatchStream M0a/b** — landed default-on. Persistent-mapped VBO; eliminated per-frame `glBufferData` upload churn for terrain solid path.

3. **Shape C — PatchTable scaffolding** — landed but **default-off**. Env-gated by `MC2_MODERN_TERRAIN_PATCHES=1`. The structure is:
   - `MapData::WorldQuadTerrainCacheEntry` (`mclib/mapdata.h:86-122`) — stable per-quad recipe (handles, UV atlas data, isCement/isAlpha)
   - `Terrain::primeMissionTerrainCache` (`mclib/terrain.cpp:561`) — warms the cache at mission load
   - `s_shapeCEnabled` flag + `tryGetCachedTerrainRecipe()` + `buildTerrainRecipeInline()` + parity check in `mclib/quad.cpp:58-389`
   - Validate mode: `MC2_SHAPE_C_PARITY_CHECK=1` runs both paths and prints `[SHAPE_C] MISMATCH ...` on divergence

Read these in order before writing code:
- `docs/superpowers/explorations/2026-04-27-modern-terrain-surface-findings.md` — the full call-graph trace, Tracy zone inventory, and per-cost decomposition
- `docs/superpowers/brainstorms/2026-04-27-modern-terrain-surface-seam-m0-shapes.md` — Shape A/B/C designs side by side, including what stays legacy vs becomes modern
- `docs/superpowers/specs/2026-04-28-patchstream-shape-c-design.md` — the specific Shape C wiring spec, including verified invalidation path (`setOverlay` → `setTerrain` → `invalidateTerrainFaceCache`)

---

## What I observed this session (M2d wrapup)

- Pre-M2d Tracy baseline (April 27): `Terrain::geometry quadSetupTextures` ~1.17 ms/frame (Self-only Tracy snapshot, ~3060 frames). That number is now ~1.5 days stale.
- Post-M2d: `Terrain::render drawPass` is at 1.46 ms (max zoom out, mc2_01). That's a different zone — `Terrain::geometry` is a sibling zone in the outer frame, not a child of `drawPass`. **The two numbers do not subtract.**
- Shape C scaffolding is live in code but the env flag is default-off — meaning the per-frame loop currently always falls into `buildTerrainRecipeInline()`. The cache read path has never been measured under load.

---

## The decision the next session must make

**Before writing any code, run two Tracy captures at Wolfman (`visibleVerticesPerSide=200`, ~40k visible verts) and compare:**

1. Default build (`MC2_MODERN_TERRAIN_PATCHES` unset). Capture `Terrain::geometry quadSetupTextures` zone.
2. Same build with `MC2_MODERN_TERRAIN_PATCHES=1`. Compare the same zone.

This is the **CR6 gate** from the brainstorm doc: "Insufficient win without Shape A or Shape B — Tracy says quadSetupTextures is 1.17 ms but a meaningful fraction is `get_gosTextureHandle` already (a Shape A target). Removing setupTextures may transfer cost rather than eliminate it."

Shape A landed since that brainstorm was written, so the residual is now the actual Shape C target. Measure it.

### Three outcomes

**A. Cache-read path drops the zone by ≥0.5 ms.**
Slice ships as: promote `MC2_MODERN_TERRAIN_PATCHES=1` to default-on (M0a-style flip), with the parity check + validate mode tools already in place. One commit; very low risk because the parity infrastructure has been sitting in the tree validated.

**B. Cache-read path drops the zone by 0.1-0.5 ms.**
The cache-read path itself is fine, but the bigger win is somewhere else — most likely `addTerrainTriangles()` (`mclib/quad.cpp:394`), which fires 2-8 `mcTextureManager->addTriangle` calls per quad per frame for capacity reservation. This is *triangle counter bumping*, not handle resolution; it can be lifted into the mission-load cache too, since the (texture-node, triangle-count) tuple is deterministic per quad.

The slice is then: extend `WorldQuadTerrainCacheEntry` to also hold a `triReservation[layer]` array; replace the four `addTriangle` calls in `addTerrainTriangles()` with a single `addTriangleBulk(node, count)` driven by the cached value. Bigger change, single fast-path-style commit.

**C. Cache-read path drops the zone by <0.1 ms.**
Surprise. Means the inline recipe build was already cheap. Pivot: the next CPU bucket is probably `Terrain::renderWater()` (still iterates all 39,601 quads) or the per-vertex `vertexProjectLoop` (~0.41 ms/frame in the April 27 snapshot). Different attack surface; spec from scratch.

---

## What NOT to do

- **Don't redesign Shape C.** The structure in `mclib/quad.cpp:58-389` is already shaped correctly for the M2-style "modern parallel layer" pattern (recipe-via-cache vs. recipe-inline, with parity validation). The next slice extends it; doesn't replace it.
- **Don't touch the cache-invalidation path** without re-reading `2026-04-28-patchstream-shape-c-design.md` §2 (verified facts from code reads). `setOverlay` already funnels through `setTerrain`; the invalidation contract is single-point.
- **Don't promote default-on without parity check.** Run `MC2_SHAPE_C_PARITY_CHECK=1` for at least one tier1 + one Carver5O + one Magic canary. Watch for `[SHAPE_C] MISMATCH` lines.
- **Don't try to also lift `enqueueTerrainMineState` or the water-detail block** in the same slice. They're per-frame-dynamic; the cache won't help. Sister slices.

---

## Validation gates (when a slice ships)

Mirror the M2d gate ladder:
- **A — visual parity** at Wolfman zoom: cement/concrete tile borders, mine/scorch decals, water transition zones. Pan + select units (per the M2d note about transient blending).
- **B — Tracy delta** at the `Terrain::geometry quadSetupTextures` zone, Wolfman. ≥0.3 ms/frame to count as a win.
- **C — `[SHAPE_C] MISMATCH` count = 0** for one full tier1 + Carver5O + Magic run with `MC2_SHAPE_C_PARITY_CHECK=1`. Per-mismatch line lists tile coordinates and the diverging field.
- **D — tier1 smoke** standard regression (5/5 PASS, +0 destroys delta). Note: smoke runner does NOT propagate `MC2_MODERN_TERRAIN_PATCHES` — see `scripts/run_smoke.py:232-237` env passthrough list. Either patch the runner to add it, or rely on tier1 testing the legacy path (which is fine since the slice keeps legacy intact behind the flag).

---

## Build / deploy / run reference

Standard worktree pattern:
- Build: `RelWithDebInfo`, target `mc2`, build dir `build64/`.
- Deploy: `cp -f` per file + `diff -q` to `A:/Games/mc2-opengl/mc2-win64-v0.2/`.
- Run with all PatchStream + Shape C env flags:
  ```
  set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& set MC2_MODERN_TERRAIN_PATCHES=1& set MC2_SHAPE_C_PARITY_CHECK=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"
  ```
- For Tracy capture: launch Tracy GUI before mc2 (it auto-connects on port 8086; vendored under `3rdparty/tracy/`).

---

## Pickup checklist

1. Read `docs/superpowers/explorations/2026-04-27-modern-terrain-surface-findings.md` §1.4, §2.2 (90 lines). The cost decomposition table is the key reference.
2. Read `mclib/quad.cpp:58-389` and `565-665`. That is all of Shape C's existing scaffolding plus the active `setupTextures` body that calls into it.
3. Run the two Tracy captures described above (Wolfman, mc2_01, ~30 s each, with and without `MC2_MODERN_TERRAIN_PATCHES=1`). Compare `Terrain::geometry quadSetupTextures` self-time. **Don't decide the slice until this number is in hand.**
4. Per outcome A/B/C above, write a single-commit slice with parity-validate runs in tier1 + Carver5O + Magic before promoting any default.

---

## What this session left behind

- M2d landed (`258e584`): `mclib/quad.cpp` overlay fast-path emit + spec status update.
- `memory/m2_thin_record_cpu_reduction_results.md` updated with the 1.46 ms result and the parity-log silent-on-success gotcha.
- M2d spec at `docs/superpowers/specs/2026-04-29-m2d-overlay-fast-path-design.md` marked SHIPPED with the OVERLAY_ELEV_OFFSET correction noted.
- This handoff doc.
