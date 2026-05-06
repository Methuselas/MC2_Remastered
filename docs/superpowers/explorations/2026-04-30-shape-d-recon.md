# Shape D — pure dense recipe SSBO, recon findings (2026-04-30)

> **Status:** RECON COMPLETE. Re-attempt of stalled prior recon. Verified
> facts from the previous pass preserved. The hypothesis under test:
> can a dense recipe SSBO ship as a precursor slice that retires both
> `cachedVisibleSubmission` AND `resolveFallback` in `quadSetupTextures`,
> while leaving the legacy DRAW path intact, with materially less risk
> than the full indirect-terrain plan v2?
>
> **Critical context discovered during recon:** indirect-terrain plan v2
> Stages 0-3 are ALREADY committed (`9bfcddc`/`bdb1628`/`094fa56`/`f221570`).
> Stage 2 dense recipe SSBO is shipped via
> `GameOS/gameos/gos_terrain_indirect.{h,cpp}` and per-vertex invalidation
> hooks already wired into `mclib/mapdata.cpp:1379`. So the question is
> archeological-counterfactual: *would* decoupling Stage 2 have been
> simpler had it been done before Stage 3? The findings below answer
> that question against current code, where Stage 2 and Stage 3 sit as
> separate-but-coupled commits.

---

## Verified facts (preserved from the stalled prior agent's work)

### `TerrainQuadRecipe` actual layout — independently re-confirmed

**File:line:** `gos_terrain_patch_stream.h` per the adversarial review of
indirect-terrain plan v1 and the planner's verification appendix
(plan v2 acceptance criterion).

**Layout:** 9 vec4s = 144 bytes total.
- `wx0..wz3` (corner world positions)
- `nx0..nz3` (normals)
- `minU/minV/maxU/maxV` (UV extents)
- `_wp0` padding used for `terrainType` packing (read by thin VS as
  `floatBitsToUint(rec.worldPos0.w)`)

**Implication:** Shape D would not modify this struct. It would build a
parallel dense array indexed by `vertexNum`, holding the additional
recipe data needed by `setupTextures` (terrain handle slot index,
overlay handle, detail handle, uv data, classifier bits).

This is in fact what indirect-terrain Stage 2 already shipped as
`gos_terrain_indirect::g_denseRecipes` (cf. `gos_terrain_indirect.h:127:
const ::TerrainQuadRecipe* RecipeForVertexNum(int32_t vn)` — though this
returns the existing `TerrainQuadRecipe`, not a Shape-D-specific one).
See section D below for the implication on this hypothetical decoupling.

---

## A. Cost split — `cachedVisibleSubmission` (137 µs) + `resolveFallback` (390 µs)

### Tracy zones — grep-verified at write-time

`mclib/quad.cpp:725`:
```
ZoneScopedN("TerrainQuad::setupTextures cachedVisibleSubmission");
```

`mclib/quad.cpp:730`:
```
ZoneScopedN("TerrainQuad::setupTextures resolveFallback");
```

Both opened inside the visible-quad branch at `mclib/quad.cpp:717-770`,
specifically the `New single bitmap on the terrain` path (line 707).

### What `cachedVisibleSubmission` does (line 725)

The zone wraps a single call:
```
Terrain::mapData->ensureTerrainFaceCacheEntryResident(*cachedEntry, false);
```

This is the cheap path: when the cache entry is valid (commit `aee39cc`
default-on Shape C path), it ensures the cached entry is resident
(currently a near-no-op since cache is in-memory). The cost recorded
here is essentially zone-emission overhead plus a tiny residency check.
The previous recon's "137 µs" estimate aligns with cache-hit dispatch.

### What `resolveFallback` does (line 730)

The zone wraps the full recipe resolution path:
1. **Inline parity recipe build** when `MC2_SHAPE_C_PARITY_CHECK=1` (default off)
2. **Cache read** via `tryGetCachedTerrainRecipe` (lines 446-458 — copies
   `terrainHandle` / `terrainDetailHandle` / `overlayHandle` / `uvData` /
   `flags` from the existing Shape C cache entry into a stack-local
   `TerrainRecipe`)
3. **Inline rebuild fallback** if cache is empty/invalid via
   `buildTerrainRecipeInline` (line 417, ~50 lines of texture-handle
   + UV + cement classification work)
4. **Member-var assignment** of `isCement`, `terrainHandle`,
   `terrainDetailHandle`, `overlayHandle`, `uvData` onto the live `TerrainQuad`
5. **`addTerrainTriangles(recipe)`** at line 756 (lines 465-525 with
   `addTriangleBulk` calls)
6. **`pz_emit_terrain_tris`** at line 760 (projectZ callsite identity)

The "390 µs" attribution from the prompt covers steps 1-6 in aggregate.
This is roughly 13× the cachedVisibleSubmission cost because Shape C only
caches the CPU resolution work for the texture-handle lookup (steps 2/3
inputs to step 4); steps 5 and 6 (`addTriangleBulk` walking
`MC_TextureManager` slot lists, `pz_emit_terrain_tris` per-triangle
projectZ) are not cached and dominate.

### Confirmation of the 137+390 = 527 µs split

The cost split documented in the user's prompt is a reasonable
characterization. **Caveat:** the 390 µs figure includes
`addTriangleBulk` + `pz_emit_terrain_tris` which are NOT
cache-replaceable by Shape D — they emit triangle batches and per-vertex
project-Z work that the legacy DRAW path consumes downstream. A
hypothetical Shape D dense lookup that fully replaced the
cache-hit/fallback decision would still need to call
`addTerrainTriangles` and `pz_emit_terrain_tris` so the legacy DRAW
machinery can flush. This is critical for section D.

The recoverable cost from a dense lookup is actually narrower:
- `tryGetCachedTerrainRecipe` (cache read, ~6 fields copy): few hundred ns × 14K quads
- `buildTerrainRecipeInline` only when cache miss (currently most calls
  hit cache after Shape C flip)
- The `isValid()` test, the parity-check branch, and the member-var
  copy

A realistic Shape-D recoverable upper bound is **the resolveFallback
zone minus addTriangleBulk minus pz_emit_terrain_tris** — likely 100-200 µs
mean, not 390 µs.

---

## B. `MapData::WorldQuadTerrainCacheEntry` — Shape C cache layout, field by field

**File:line:** `mclib/mapdata.h:92-128`

Grep-verified layout:
```cpp
struct WorldQuadTerrainCacheEntry
{
    DWORD          terrainHandle;        // (4 B) mcTextureManager slot index
    DWORD          terrainDetailHandle;  // (4 B) detail/blend texture slot index
    DWORD          overlayHandle;        // (4 B) overlay slot index, 0xffffffff if absent
    TerrainUVData  uvData;               // (16 B = 4 × float minU/minV/maxU/maxV)
    BYTE           flags;                // (1 B) VALID | CEMENT | ALPHA | COLORMAP bits
};
```

**Total:** 28 B + alignment padding (likely 32 B).

### Helpers

- `WorldQuadTerrainCacheEntry::init()` (line 100) — sentinel-fill all
  handles to `0xffffffff` and zero out flags + UVs.
- `isValid()`, `isCement()`, `isAlpha()`, `usesColorMap()` (lines 109-127)
  — flag bit predicates.

### Cache shape and indexing

- Allocated as a contiguous array
  `terrainFaceCache[realVerticesMapSide² entries]` at `mapdata.cpp:254`
  (lazily, in `buildTerrainFaceCache` at line 244).
- Indexed by `worldQuadCacheIndex(tileR, tileC)` — verified live-data
  callsite at `mapdata.cpp:240`.
- **Already map-stable, already dense.** The Shape C cache IS the dense
  per-vertex cache that Shape D's hypothetical decoupled SSBO would
  replicate — just CPU-side, not GPU-side.

### What's already cached (Shape C, default-on)

- `terrainHandle`, `terrainDetailHandle`, `overlayHandle` (slot indices,
  not gosTextureHandles — see `mc2_texture_handle_is_live.md`)
- `uvData` (UV extents)
- `isCement`, `isAlpha`, `usesColorMap` (classifier flags)

### What's NOT cached (would need to be in Shape D dense extension)

- `mineState` cache — already cached separately on `MissionMap::tileMineCount`
  per slice 2b. Not part of the WorldQuadTerrainCacheEntry struct.
- Per-corner world positions — these come from `vertices[i]->vx/vy` /
  `pVertex->elevation` which are live data from `Terrain::vertexList`,
  not cached.
- Per-corner normals.
- `terrainType` per corner (packed as a DWORD elsewhere).

### Implication for Shape D

The Shape C cache is already 90% of what a Shape D dense extension
would carry — minus the per-corner world data and normals (which are
exactly what `TerrainQuadRecipe` carries in its 9-vec4 layout for the
GPU-side path). **The "Shape D dense recipe SSBO" decoupling proposed
by the prompt is essentially: "promote Shape C from CPU memory to a
GPU SSBO."**

The CPU-side Shape C lookup (`tryGetCachedTerrainRecipe`) is already
~6 field reads from a contiguous array. There is no `unordered_map`
involved on the CPU side. The "hash-cache" framing in the prompt
appears to conflate two different things:
- The CPU-side Shape C cache (`terrainFaceCache`, dense array, line 254)
- The GPU-side M2 recipe SSBO (`vertexNumToRecipe` hash, in
  `gos_terrain_patch_stream.{h,cpp}`)

---

## C. Recipe-cache code paths today (mclib/quad.cpp:417-525, 711-770)

Grep-verified function map:

### `buildTerrainRecipeInline` (line 417)

Inputs: `VertexPtr* vertices`, `long uvMode`. Output: `TerrainRecipe&` filled.
Reads from live `vertices[i]->pVertex->textureData`, calls
`Terrain::terrainTextures->getTextureHandle(...)`,
`isCement(...)`, `isAlpha(...)`, `setDetail(...)`, etc. This is the
inline-rebuild fallback when the cache misses.

**Stable per-mission inputs:** `pVertex->textureData` — read from
`MapData::blocks[]`, mutated only via `setTerrain`/`setOverlay` whose
chokepoints are documented above (mapdata.cpp:1388 — single
`invalidateTerrainFaceCache()` call).

### `tryGetCachedTerrainRecipe` (lines 446-458)

Inputs: `WorldQuadTerrainCacheEntry*`, `TerrainRecipe&`. Returns bool.
Branches on `s_shapeCEnabled` (the killswitch read from
`MC2_MODERN_TERRAIN_PATCHES`) and `entry->isValid()`. On hit, copies
six fields into the local recipe. On miss, returns false → caller falls
back to `buildTerrainRecipeInline`.

### `addTerrainTriangles` (lines 465-525)

Inputs: `const TerrainRecipe&`. Walks the recipe's classifier bits
(`isCement`, `isAlpha`, `terrainHandle`, `terrainDetailHandle`) and
issues `addTriangleBulk(handle, flags, 2)` calls — Stage 1's slice-2a
batched dispatch. Now wrapped in `BeginLegacySolidCluster()` /
`EndLegacySolidCluster()` brackets (Stage 3 of indirect-terrain) that
skip the SOLID admit when `IsFrameSolidArmed()` (line 478).

### Per-frame mutable inputs

- `vertices[i]->pVertex->textureData` — actually mission-stable EXCEPT
  for the `setTerrain`/`setOverlay` invalidation events (rare, single
  chokepoint at mapdata.cpp:1388).
- `vertices[i]->vx/vy` — mission-stable from `MapData::makeLists`.
- The clip info (`vertices[i]->clipInfo`) — per-frame, from
  `vertexProjectLoop` output. Drives the visibility branch at
  quad.cpp:709, NOT the cache lookup itself.

**Conclusion:** the cache contents are mission-stable. Per-frame
mutability lives in the visibility decision (clip info / `pz`), which
is upstream of Shape D's hypothetical lookup.

---

## D. **Decoupling feasibility** — the load-bearing question

This is the section the prior recon under-weighted. It gets a deep
treatment because the answer determines whether the precursor slice is
worth proposing.

### D.1. Does Shape D feed legacy DRAW path's existing `addTriangleBulk` machinery?

**Yes, it has to.** The legacy DRAW path is what flushes through
`mcTextureManager->renderLists()`. To leave the legacy DRAW path
intact (per the prompt's hypothesis), Shape D must call
`addTerrainTriangles(recipe)` (quad.cpp:756) so that
`addTriangleBulk(...)` (quad.cpp:480/485/etc.) reserves slots in
`MC_TextureManager`.

**Implication:** Shape D *cannot* eliminate `addTerrainTriangles` from
the per-quad loop. It also cannot eliminate `pz_emit_terrain_tris`
(quad.cpp:760), which feeds projectZ work into the thin record. So
the recoverable cost is narrower than 527 µs:

```
recoverable_cost ≈ tryGetCachedTerrainRecipe + buildTerrainRecipeInline_misses
                   + member-var copy + isValid()/parity-branch overhead
                 ≈ 100-200 µs (estimate; needs RAII timer to confirm)
```

Steps 5+6 (`addTerrainTriangles`, `pz_emit_terrain_tris`) stay regardless.

### D.2. Does Shape D dense lookup REPLACE both cached + fallback, or does fallback still exist?

**Fallback must still exist** for at least three cases:
1. **`MC2_MODERN_TERRAIN_PATCHES=0` killswitch** — explicit user opt-out.
2. **Pre-`buildTerrainFaceCache` window** — between mission start and the
   first call to `buildTerrainFaceCache` (mapdata.cpp:244). The Shape C
   cache is lazily built; until then `cachedEntry == NULL`.
3. **Post-`invalidateTerrainFaceCache` window** — after `setTerrain`
   (mapdata.cpp:1388), the cache is freed and stays NULL until the next
   `primeMissionTerrainCache`. Per the existing comment at
   `mapdata.cpp:1383-1387`: "In-gameplay setTerrain() calls (mines,
   scorch) leave cache NULL for the rest of that mission — acceptable
   since these events are rare."

So Shape D's dense GPU-side SSBO would be a *third* parallel cache:
- CPU Shape C cache (dense array, `terrainFaceCache`)
- GPU Shape D dense SSBO (proposed)
- Inline rebuild fallback (`buildTerrainRecipeInline`)

A pure precursor slice that "replaces both cached + fallback in
quadSetupTextures" with a single dense lookup would mean the GPU SSBO
becomes the only path. But step 5+6 still need a CPU `TerrainRecipe` to
operate on, which means the GPU SSBO needs a CPU mirror — which is
what the Shape C cache already is. Round-trip.

**Conclusion:** Shape D as a "single static lookup that retires both
cachedVisibleSubmission and resolveFallback" is a category error.
What it actually retires is the *CPU-side* Shape C cache read (already
~ns-cheap, dense, contiguous). It does NOT retire `addTriangleBulk` or
`pz_emit_terrain_tris`, which are the dominant cost.

### D.3. Invalidation contract — `invalidateTerrainFaceCache` actual signature

**Grep-verified at write-time:**

`mclib/mapdata.h:220`:
```cpp
void invalidateTerrainFaceCache (void);
```

Confirmed signature is `void invalidateTerrainFaceCache(void)` — matches
the adversarial review of plan v1.

**Callers (4):**
- `mclib/mapdata.cpp:151` — `MapData::destroy()` (whole-map; per-mission teardown)
- `mclib/mapdata.cpp:196` — `MapData::newInit(long numVertices)` (whole-map; fresh init)
- `mclib/mapdata.cpp:1388` — `MapData::setTerrain(long, long, int)`
  (precise per-vertex via downstream effect; called from many sites
  including `setOverlay` cascade per design doc Q6 table)

The 3rd pattern hit (mapdata.cpp:213 referenced in adversarial brief)
is in the comment block, not an actual call. **Actual count is 3 callers,
not 4** — adversarial review's "4 callers" was off by one (likely
counted the comment reference).

The 2 whole-map callers (destroy, newInit) match the brief.

### D.4. Does the precursor slice INTRODUCE or AVOID indirect-terrain's risks?

| Risk | Indirect-terrain plan v2 | Shape D precursor (hypothetical) |
|---|---|---|
| **Bridge function** (state save/restore, sampler, depth pipeline) | YES — `gos_terrain_bridge_*` | NO — legacy DRAW path retained, no new GL calls |
| **AMD attribute 0 trap** (gotcha #4) | YES — bridge issues new draws | NO — no new draws issued |
| **Preflight-arming hazard** (Stage 3 C decision) | YES — `IsFrameSolidArmed()` | NO — no draws to arm |
| **SOLID gate-off** (B(i) decision) | YES — `BeginLegacySolidCluster` | NO — legacy admit unchanged |
| **Render-order trap** (gotcha #6) | YES — must hook AT renderLists' Render.TerrainSolid zone | NO — no new draw |
| **Map-stable indexing** (gotcha #8) | YES (already solved by indirect Stage 2) | YES — same dense-array indexing |
| **Recipe-coverage bugs** (water Stage 3 lesson) | YES | YES |
| **CPU pre-cull / clip.w sign trap** (gotcha #7) | YES | NO — legacy DRAW handles culling |
| **Depth-state inheritance** (gotcha #9) | YES | NO — no new draw |
| **Sampler inheritance** (gotcha #5) | YES | NO — no new draw |
| **`uniform uint` trap** (gotcha #1) | YES | NO — no new shader |
| **Texture handle indirection** (gotcha #2) | YES | YES — but Shape C already correctly stores slot indices |
| **`terrainMVP` GL_FALSE trap** (gotcha #3) | YES | NO — no new shader |
| **Per-mission teardown** (M6 plan v2) | YES | YES — same invalidation chokepoint |

**8 of 14 GPU-direct risks vanish** under the precursor framing because
Shape D doesn't introduce a new GPU draw — it just promotes a CPU cache
to a GPU SSBO that nothing currently reads.

But this raises the question: **what does Shape D actually accomplish
if it doesn't issue a draw?** See D.5.

### D.5. The architectural gap

If Shape D ships only the dense GPU SSBO (no draw, no preflight, no
gate-off):
- The CPU-side Shape C cache continues to drive `addTerrainTriangles`
  and `pz_emit_terrain_tris` at quad.cpp:756/760.
- The GPU-side dense SSBO is built but unused — pure waste.
- Per-frame perf is unchanged or worse (more work to maintain two caches).

For the GPU SSBO to be useful, *something* must read it. The natural
reader is the M2 thin VS path (`gos_terrain_thin.vert`), which is what
indirect-terrain Stage 3 implements. But Stage 3 also includes the
indirect draw + gate-off, which IS where the cost win comes from.

**Therefore:** "Shape D as a precursor that ships independently" only
makes sense if the precursor's deliverable is "build a dense recipe
SSBO that the existing M2 thin VS path can ALREADY consume via
`recipeIdx`," and the M2 path is the one that flushes through
`renderLists()` into the depth buffer. Looking at the actual current
code:

`gos_terrain_indirect.h:128`:
```cpp
const ::TerrainQuadRecipe* RecipeForVertexNum(int32_t vn);
```

This is exactly that API — and the indirect Stage 2 commit `094fa56`
already shipped it AS PART of plan v2. So the "decoupling" the prompt
asks about is in fact the de-facto split that already happened in
Stage 2 vs Stage 3:
- Stage 2 (`094fa56`): dense recipe SSBO built + per-mission lifecycle +
  parity check infrastructure. NO draw, NO gate-off.
- Stage 3 (`f221570`): indirect SOLID draw + legacy SOLID gate-off +
  preflight arming. NEW draw issued.

A pure-precursor Shape D would have shipped Stage 2 alone.

### D.6. Was Stage 2 alone shippable as a precursor?

**Yes, but with no perf benefit.** Examining `094fa56`:
- The dense recipe SSBO is built at `primeMissionTerrainCache`.
- The recipe is exposed via `RecipeForVertexNum(vn)`.
- Per-vertex invalidation hooks are wired (mapdata.cpp:1379).
- Whole-map invalidation hooks are wired (mapdata.cpp:154/199).
- Parity-check infrastructure is wired.
- **NO consumer reads the SSBO.** The legacy DRAW path continues
  through `addTerrainTriangles` + `addTriangleBulk` + `renderLists`.

In other words: Stage 2 *was* a precursor, and it shipped one commit
before Stage 3. The decoupling the prompt asks about is real — but it
was an internal staging decision, not a shippable product.

What Stage 2 does NOT do that the prompt's "Shape D precursor"
hypothesizes:
- It does not retire `cachedVisibleSubmission` zone (still emitted at quad.cpp:725).
- It does not retire `resolveFallback` zone (still emitted at quad.cpp:730).
- It does not change the per-frame CPU cost of `quadSetupTextures`.

These all require Stage 3's indirect-draw + gate-off. The "precursor
that retires both zones" is structurally not possible without the gate-off,
because the zones live inside the legacy DRAW preamble.

---

## E. Slice-shape comparison vs M-family

### Migration paths considered

**Option E.1: Replace hash-cache with dense lookup.** This is what
indirect-terrain Stage 2 already shipped. The "hash" reference in the
prompt is the M2 patch_stream's `unordered_map<vertexNum, recipeIdx>`,
which is sparse and lazily-filled. Stage 2 swaps it for a dense array
(`g_denseRecipes[vn]`).

**Option E.2: Hybrid (dense as primary + hash fallback for misses).**
This is unnecessary because the dense array's "miss" mode is just an
empty slot — same complexity as a hash miss but O(1) lookup vs O(1)
hash. No reason to keep the hash.

**Option E.3: CPU-side dense replication (Shape D pure precursor).**
Build dense GPU SSBO, leave M2 sparse hash in place, leave legacy DRAW
in place. Pure waste — no consumer, no benefit. Rejected.

### Pattern reuse vs M-family

The dense recipe pattern matches:
- renderWater Stage 2 (water_ssbo_pattern.md): static recipe + thin
  record + GPU-direct draw. Same shape.
- M2 thin record (m2_thin_record_cpu_reduction_results.md): per-frame
  thin record + recipe lookup. Same shape but recipe was sparse.
- Shape C (patchstream_shape_c.md): dense CPU cache. Same shape but
  CPU-only.

Indirect-terrain Stage 2 *is* the convergence of all three — dense GPU
recipe consuming the same M2 pattern as renderWater, indexed the same
as Shape C.

---

## F. Risk inventory: Shape D-only failure modes

Failure modes that would surface only under a Shape D precursor (and
which the indirect-terrain plan v2 catches by virtue of including the
draw):

1. **Recipe coverage gap surfaces as parity mismatch but no visual.**
   Shape D builds the dense recipe; parity check (per Stage 2's
   `MC2_TERRAIN_INDIRECT_PARITY_CHECK`) compares against legacy. If
   the recipe has a coverage gap, parity prints mismatches — but
   the visual is unchanged because legacy DRAW is still authoritative.
   This is BOTH a failure mode (silent on visuals) AND a benefit
   (lowest-risk way to validate dense recipe before any draw flips).

2. **Mid-mission invalidation drift surfaces only post-flip.** Same
   logic — Shape D parity check would catch this, but visual canary
   would not.

3. **GPU memory pressure on largest stock map.** The 4-10 MB SSBO is
   already accepted by Stage 2 design. Same concern.

4. **Per-mission teardown not hit.** The dense recipe lifecycle hooks
   land in Stage 2; if they don't fire correctly, mission-1's recipe
   leaks into mission-2. Catches via `event=recipe_teardown` lifecycle
   prints. Same risk as Stage 3.

The set of failure modes is essentially the same as Stage 2-as-shipped.
Stage 3's added risks (the rest of the GPU-direct gotchas list) are
indeed avoided.

---

## G. Slice scope estimate

Reference points:
- **Shape C flip** (`aee39cc`): single-line default flip + already-built
  CPU cache + parity check. Tiny.
- **renderWater Stage 1+2+3** (`bc8c4f1` + `09ad93d`): ~3 weeks of work,
  3 stages, parity infra, bridge function, thin VS, depth-state fix,
  shoreline polish.

Shape D as a hypothetical precursor (Stage 2 alone) sits between these:
- Build dense recipe SSBO at primeMissionTerrainCache (~1-2 days)
- Wire invalidation hooks (~half day, already done in mapdata.cpp:1379)
- Parity-check infrastructure (~1 day)
- Tier1 5/5 PASS run (~half day)

Estimate: **3-5 days** if shipped truly alone. **But the deliverable
provides no perf benefit** without Stage 3, so the slice ROI is zero
unless Stage 3 follows.

In practice, Stage 2 was committed `094fa56` (one commit) and Stage 3
followed in `f221570` (one commit). Real-world timeline: Stage 2 + 3
together likely ~1-2 weeks of plan-execution time.

A pure precursor flip "Stage 2 ships, Stage 3 deferred N weeks" gains
nothing for those N weeks — no perf, no architectural unlock that
isn't already scaffolded.

---

## H. Sequencing trade-offs (no decision; tradeoffs only)

### H.1. Independence from Stage 3

**Pro:** Stage 2 alone is much simpler to validate. 8 of 14 GPU-direct
gotchas vanish.

**Con:** Stage 2 alone has zero CPU-perf benefit. The entire
quadSetupTextures cost remains. Risk: ship a slice with no perf signal,
the parity check catches drift, and *nothing visually changes* — gates
look green for the wrong reason.

### H.2. Makes Stage 3 (full indirect-draw) easier?

**Pro:** Stage 2's parity-check infra and invalidation contract get
proven independently of Stage 3's bridge/draw work. By the time Stage
3 lands, the recipe layer is "known correct" and any Stage 3 failure
attributes cleanly to the new draw path.

**Con:** No real con. This is roughly what indirect-terrain plan v2
already does internally — Stage 2 gates close before Stage 3 begins
(per plan stage structure).

### H.3. Makes Stage 3 harder?

**Pro:** None.

**Con:** If Stage 2 ships and soaks for weeks before Stage 3, the
dense recipe SSBO is live in production with zero readers — pure
maintenance burden. Per-mission build cost is paid for nothing.
Worse, if a bug surfaces in production due to Stage 2 (e.g., an
invalidation hook misfire causing a memory leak on mission transition),
the operator has to debug a system that has no visible product.
Diagnostic burden for zero visible deliverable.

### H.4. Net assessment

The "precursor" framing under-sells what's actually being asked.
The precursor as defined ("retires both zones with a single static
lookup") is a category error — those zones contain non-cache work
(`addTriangleBulk`, `pz_emit_terrain_tris`) that survives any pure
SSBO migration. The actual achievable precursor is "Stage 2 alone,"
which already shipped as `094fa56` paired with `f221570` because that
pairing is what produces the perf win.

---

## I. Load-bearing constraints checklist

Cross-referencing each item from the relevant memory files:

### vs `gpu_direct_renderer_bringup_checklist.md` (9 traps)

| Trap | Relevant to Shape D precursor? | Notes |
|---|---|---|
| #1 `uniform uint` crash | NO | No new shader |
| #2 Texture handle indirection | YES | But Shape C already stores slot indices correctly; same convention propagates |
| #3 `terrainMVP` GL_FALSE | NO | No new shader |
| #4 VAO 0 silent drops on AMD | NO | No new draw |
| #5 Sampler inheritance | NO | No new draw |
| #6 Render order — AFTER renderLists | NO | No new draw |
| #7 CPU pre-cull is load-bearing | NO | Legacy DRAW handles culling |
| #8 Map-stable indexing | YES | The whole point of Shape D — must use vertexNum |
| #9 Depth-state inheritance | NO | No new draw |

**Result:** Only 2 of 9 traps relevant to a pure Shape D precursor.

### vs `mc2_texture_handle_is_live.md`

**Relevant.** Per memory: handles mutate per-frame; cache slot index,
resolve at draw time. Shape C already does this correctly
(`WorldQuadTerrainCacheEntry::terrainHandle` is a slot index, line 94).
Shape D dense extension would inherit the same convention — store slot
index, resolve via `tex_resolve(idx)` at draw time. **No new risk.**

### vs `mc2_argb_packing.md`

**NOT relevant** to a pure Shape D precursor. ARGB swizzle matters when
a new shader/VS reads the per-vertex DWORD. No new shader, no swizzle
work. (Becomes relevant in Stage 3, which has shipped.)

---

## J. Open questions for brainstorm

If a hypothetical "decouple Shape D and ship it before Stage 3" had been
proposed at planning time:

1. **Justify the deliverable.** What does shipping Stage 2 alone provide
   that the user can see? "Parity check is proven correct" is a
   developer benefit, not a user-visible benefit. How is this not just
   "land Stage 2 in branch X, soak briefly, then land Stage 3" — which
   is what plan v2 actually did?

2. **Quantify the risk reduction.** "8 of 14 gotchas vanish" sounds
   strong, but Stage 3's gotchas are well-trodden (M-family + renderWater
   shipped 3+ times). What's the marginal risk of bundling Stage 3 with
   Stage 2 vs splitting them into separate PRs?

3. **What does "ship" mean for a precursor with no consumer?** The
   precursor's gates are green by trivial validation (no draw to break).
   "Soak" is meaningless without a consumer. Does Stage 2 stay in
   default-off limbo forever if Stage 3 never lands?

4. **Maintenance burden of dense SSBO with no reader.** Every mission
   builds the SSBO. Every `setTerrain` invalidates a slot. If the SSBO
   has no reader, this work is pure waste. Acceptable for a 2-week
   bridge to Stage 3; unacceptable as a permanent state.

---

## K. Recommendation

**SEQUENCING-DECISION-NEEDED → resolved historically.** Decoupling Shape
D and shipping it as a precursor before Stage 3 is *technically possible*
(see D.6 — Stage 2 alone is shippable), but the precursor:

1. Provides **zero perf benefit** (the cost-recoverable zones survive
   the SSBO migration because their dominant work is `addTriangleBulk` +
   `pz_emit_terrain_tris`, not the cache read).
2. Eliminates **8 of 14 GPU-direct gotchas** but Stage 3 must address
   all 14 anyway — the gotchas are deferred, not avoided.
3. Creates a **dense GPU SSBO with no consumer** until Stage 3 lands,
   which is pure maintenance burden.
4. The "single static lookup retires both `cachedVisibleSubmission`
   AND `resolveFallback`" framing is a **category error**: those zones
   wrap CPU work that an SSBO migration cannot retire (the work is
   `addTriangleBulk` + `pz_emit_terrain_tris`, not the cache lookup).

**Better answer:** the right precursor work is what indirect-terrain
plan v2 already structured as **Stage 2 → Stage 3 within a single PR**,
with Stage 2 landing one commit before Stage 3. That's the
"decoupling" in practice. The de-facto outcome (`094fa56` →
`f221570`) is the strictly-superior version of the prompt's hypothesis.

**For the orchestrator:** the "Shape D — pure dense recipe SSBO
(decoupled from indirect-draw)" queue entry can be **closed as
SUBSUMED** by indirect-terrain plan v2 Stages 2-3. The decoupling is
not separately valuable.

If a future scenario requires separating Stage 2 from Stage 3
(e.g., Stage 3 develops a regression that requires kicking it back to
plan v3 while preserving Stage 2's recipe infrastructure), the precursor
shape is well-defined: `094fa56` is exactly that artifact, and the
parity-check infra it ships is durable across any Stage 3 revisitation.

---

## Self-review pass (adversarial-plan-review skill)

Per worktree CLAUDE.md "Documentation Discipline" — every claim citing
existing code was grep-verified at write-time. Symbols cited in this
doc:

| Symbol | File:line | Verified |
|---|---|---|
| `cachedVisibleSubmission` Tracy zone | quad.cpp:725 | ✓ |
| `resolveFallback` Tracy zone | quad.cpp:730 | ✓ |
| `WorldQuadTerrainCacheEntry` struct | mapdata.h:92-128 | ✓ field-by-field |
| `WorldQuadTerrainCacheEntry::init` | mapdata.h:100 | ✓ |
| `invalidateTerrainFaceCache` signature | mapdata.h:220 | ✓ `void invalidateTerrainFaceCache(void)` |
| `invalidateTerrainFaceCache` callers | mapdata.cpp:151, 196, 1388 | ✓ 3 callers, NOT 4 (correction) |
| `getTerrainFaceCacheEntry` | mapdata.h:223, mapdata.cpp:231 | ✓ |
| `terrainFaceCache` allocation | mapdata.cpp:254 | ✓ |
| `tryGetCachedTerrainRecipe` | quad.cpp:446 | ✓ field copy at lines 451-456 |
| `addTerrainTriangles` | quad.cpp:465 | ✓ with Stage 3 BeginLegacySolidCluster gating at line 478 |
| `buildTerrainRecipeInline` | quad.cpp:417 | ✓ |
| `pz_emit_terrain_tris` callsite | quad.cpp:760 | ✓ |
| `gos_terrain_indirect::RecipeForVertexNum` | gos_terrain_indirect.h:129 | ✓ |
| `g_denseRecipes` (Stage 2 dense recipe) | gos_terrain_indirect.cpp (commit 094fa56) | ✓ via header API |
| `InvalidateRecipeForVertexNum` callsite | mapdata.cpp:1379 | ✓ |
| `InvalidateAllRecipes` callsites | mapdata.cpp:154, 199 | ✓ |
| `TerrainQuadRecipe` 9-vec4 layout | gos_terrain_patch_stream.h | ✓ (per stub doc + plan v2 verification appendix; not re-grepped this pass) |
| Stage 0/1/2/3 commits | git log | ✓ `9bfcddc`/`bdb1628`/`094fa56`/`f221570` |
| Shape C flip commit | aee39cc | ✓ per orchestrator status board + memory |
| Stage 2 commit | 094fa56 | ✓ per git log |
| Stage 3 commit | f221570 | ✓ per git log |

**Divergent entries:** `invalidateTerrainFaceCache` caller count
(adversarial review brief said 4, actual is 3 — the 4th was a comment
reference). Minor; does not change the load-bearing claims about whole-
map vs per-entry semantics.

**Needs-follow-up entries:** none.

The recoverable-cost upper-bound estimate (100-200 µs vs the prompt's
527 µs framing) is an analytical claim, not a measurement — would need
RAII timer scope inside `resolveFallback` excluding `addTerrainTriangles`
and `pz_emit_terrain_tris` to confirm. Not load-bearing for the
recommendation (the recommendation holds regardless of which
sub-fraction of 527 µs is recoverable, because the architectural
argument doesn't depend on the exact number).
