# ModernTerrainSurface — Seam M0 Implementation Shapes

**Date:** 2026-04-27
**Branch:** `claude/nifty-mendeleev`
**Status:** brainstorm — three shapes, one leading preference, no spec.
**Cold-read input:** [`docs/superpowers/explorations/2026-04-27-modern-terrain-surface-findings.md`](../explorations/2026-04-27-modern-terrain-surface-findings.md).
**Successor:** spec author picks one shape, opens an `EnterPlanMode` session against that shape's open questions.

This document does not propose a spec. It produces three implementation shapes the operator can evaluate against the empirical Tracy baseline, the §5 load-bearing constraints, and the §9 risk register in the findings doc.

---

## Working thesis (recap)

Modernize CPU work *downstream* of terrain admission — the per-quad `setupTextures` reservation pass, the per-triangle `gos_VERTEX` pack, the per-batch GL upload churn — while preserving CPU `projectZ` admission, the `clipInfo` + `setObjBlockActive`/`setObjVertexActive` cull-cascade root, the `pz` gate, the 8 `projectFor*` wrappers, the static-shadow accumulation path, and campaign compatibility. The empirical Tracy snapshot supports this prioritization (`quadSetupTextures` 1.17 ms/frame ≈ 3× `vertexProjectLoop` 0.41 ms/frame; per-capture `get_gosTextureHandle` 96M calls; per-batch GL upload churn ~13 MB/frame). All three shapes below sit inside Seam M's envelope; none requires the Seam-H matrix-sign rework or the Seam-L "refactor-only" non-result.

## Shared constraints (recap, not duplicated)

All three shapes inherit the findings doc §5 constraints C1–C14 (cull cascade, TGL pool silence, ARGB packing, `terrainMVP` `GL_FALSE`, `abs(clip.w)`, `pz` gate, deferred-vs-direct uniforms, `rc_gbuffer1_*` registry, `projectFor*` wrappers, live texture handles, lightRGB swizzle, flag semantics, heightmap shape, mission-end TGL reset) and §9 risk register R1–R13. They inherit the AMD-driver rules at [`docs/amd-driver-rules.md`](../../amd-driver-rules.md), the smoke gate (tier1 + menu canary), and the debug-instrumentation rule (env-gated `[SUBSYS]` lifecycle prints land in the same commit). The `MC2_TGL_POOL_TRACE` / `MC2_GL_ERROR_DRAIN_SILENT` / `MC2_HEARTBEAT` envs are the canonical canary set for each shape's regression hunt.

The static-prop batcher at [`GameOS/gameos/gos_static_prop_batcher.cpp:664–838`](../../../GameOS/gameos/gos_static_prop_batcher.cpp) is the in-tree reference for "persistent SSBO + frame-fenced + GL state save/restore + slot-not-handle" — every shape that goes near GL state should be auditable against that file.

---

## Shape A — `TexResolveTable` (handle-resolution hoist)

### A.1 One-line thesis
Build a per-frame flat `int[]` mapping `(MC_TextureNode* → resolved gosTextureHandle)` once at the top of `GameCamera::render` so the 96M-call `MC_TextureNode::get_gosTextureHandle` hot path becomes a single indexed load.

### A.2 What stays legacy / what becomes modern

| Stays legacy | Becomes modern |
|---|---|
| All of `Terrain::geometry`, `vertexProjectLoop`, `quadSetupTextures` outer loop, `TerrainQuad::setupTextures`, `TerrainQuad::draw`, `addTriangle`/`addVertices` rings, `renderLists` dispatch, `terrainDrawIndexedPatches`, every shader, every GL buffer | A new `TerrainTexResolveTable` populated once per frame just before terrain submit (after `Terrain::geometry`, before `Terrain::render`). Callers swap `node->get_gosTextureHandle()` for `g_texResolveTable[node->slot]`. |

### A.3 What ships first (M0 slice)
A single commit:
1. Define `TerrainTexResolveTable` (POD vector of `int handles` indexed by node slot or by an int the node already carries).
2. Populate at the top of `GameCamera::render` step 3 (before `land->render()` at [`code/gamecam.cpp:199`](../../../code/gamecam.cpp)). Walk only nodes flagged `MC2_ISTERRAIN`.
3. Replace the read sites in the `setupTextures` cache-hit path ([`mclib/quad.cpp:433`](../../../mclib/quad.cpp)) and resolveFallback ([`mclib/quad.cpp:434`](../../../mclib/quad.cpp)) and any per-triangle handle reads in `TerrainQuad::draw` ([`mclib/quad.cpp:1515–2173`](../../../mclib/quad.cpp)).
4. Behind killswitch off → all sites fall back to `node->get_gosTextureHandle()` exactly.

Solid opaque terrain only — water / decals / overlays / shadow paths read through their own callsites untouched.

### A.4 Killswitch / bisect handle
`MC2_MODERN_TEX_RESOLVE` env (default ON once landed; OFF disables the table and reverts every callsite to a direct `get_gosTextureHandle()` call). No new hotkey — the perf delta is so localized that runtime toggling isn't useful. Bisectable in one launch.

### A.5 New buffers / structures
- `TerrainTexResolveTable` (CPU-only; ~few KB; lives in `Terrain` or beside `MapData::terrainFaceCache`).
- No new GPU buffers, no new shader entries, no GL state changes.

### A.6 Touched existing structures
- `MC_TextureNode` ([`mclib/txmmgr.h:79–110`](../../../mclib/txmmgr.h)) gains nothing structural; only its `get_gosTextureHandle` callsites are replaced at terrain-flagged readers.
- `gos_VERTEX` untouched. `gos_TERRAIN_EXTRA` untouched. `MC_VertexArrayNode` untouched.

### A.7 Static-shadow disposition
Unaffected. `Shadow.StaticAccum` ([`mclib/txmmgr.cpp:1184–1242`](../../../mclib/txmmgr.cpp)) still consumes the same nodes; if it reads handles at all, it can opt into the table or stay legacy. Shape A explicitly does **not** require shadow co-migration.

### A.8 Non-tess fallback disposition
Unchanged — the table is a read-side optimization on top of the existing `gos_VERTEX` pipeline.

### A.9 Risk register (shape-specific)

| # | Risk | Canary | Mitigation |
|---|---|---|---|
| AR1 | Live-handle violation per C10. If table is built once per mission instead of once per frame, slot mutates in `TransformMultiShape` and reads serve stale handles. | Stale or 0 handle → black or wrong-textured terrain patch. | Build per-frame, not per-mission. Add `[TEX_RESOLVE v1]` lifecycle print on (re)build with table size + sentinel handle to detect rebuild misses. |
| AR2 | Table coverage gap. Some node read-site missed in the conversion → mixed legacy + modern reads, masked perf win. | Tracy zone delta smaller than expected. | Add `MC2_TEX_RESOLVE_TRACE=1` per-call counter at every legacy `get_gosTextureHandle` site for one capture, gate on terrain flag, prove zero residuals. |
| AR3 | Crater/decal/water node share crosses table → out-of-domain read. | Decal or water texture goes black. | Only walk + populate `MC2_ISTERRAIN`-flagged nodes. Non-terrain reads stay direct. |

### A.10 Empirical hooks
- Tracy: zone deltas on `MC_TextureNode::get_gosTextureHandle`, `TerrainQuad::setupTextures cachedVisibleSubmission`, `resolveFallback`. Expected win: **up to ~0.4 ms/frame** in the captured scenario; actual win must be measured because table lookup still costs nonzero and some `get_gosTextureHandle` calls remain outside the terrain-solid path.
- Killswitch toggle for direct A/B in one session.
- `MC2_TEX_RESOLVE_TRACE=1` for residual-call audit.

### A.11 Open questions
- Q-A1. Is the slot index already on `MC_TextureNode`, or does the table need a separate (node\*→idx) registration step?
- Q-A2. Does `Shadow.StaticAccum` benefit from the same table, or is its handle-read volume already small (Tracy says it isn't a hotspot)?
- Q-A3. Is `TerrainColorMap::getTextureHandle realizeTexture` (Tracy: 15.7M calls, 0.15 ms/frame) the *same* hot path or a sister we should fold in?
- Q-A4. Are `MC_TextureNode` flags stable and queryable at the point the table is built, or are flags only known on `MC_VertexArrayNode` submissions? "Texture node" and "vertex array node" may not have the same terrain flag semantics — confirm before pinning the table-population walk on `MC2_ISTERRAIN`.

### A.12 NOT this shape
- Not a buffer change.
- Not a shader change.
- Not a submission consolidation. The 13 MB/frame upload churn is untouched.

---

## Shape B — `PatchStream` (persistent terrain SSBO + consolidated submit)

### B.1 One-line thesis
Replace the per-batch `glBufferData` orphan + upload of `indexed_tris` VB/IB and `terrain_extra_vb_` with a triple-buffered persistent-mapped SSBO holding the per-frame patch stream, drawn as one (or a small fixed number of) `glDrawElements(GL_PATCHES, …)` calls. The `gos_VERTEX` color-pass stream is **dropped** for tessellated terrain (per P2); `gos_TERRAIN_EXTRA` (worldPos/worldNorm) becomes the canonical per-vertex input, indexed by `gl_VertexID` from the SSBO.

### B.2 What stays legacy / what becomes modern

| Stays legacy | Becomes modern |
|---|---|
| `Terrain::geometry` admission incl. `vertexProjectLoop`, `setObjBlockActive`/`setObjVertexActive`, `clipInfo`, the 8 `projectFor*` wrappers, the `pz` gate at [`mclib/quad.cpp:1597–1602`](../../../mclib/quad.cpp) + sisters, all 4 cluster sites. `TerrainQuad::setupTextures` reservation pass (still drives flag-resolved per-quad records). Crater/decal/water/shadow paths. `MC_VertexArrayNode` rings for non-terrain nodes and for the non-tess fallback. | A new `TerrainPatchStream` class owning a triple-buffered persistent-mapped SSBO (~16 MB/slot at Wolfman; sized from `visibleVerticesPerSide²` × triangles-per-quad × per-vertex record). `TerrainQuad::draw` writes per-triangle records (`worldPos`, `worldNorm`, packed material slot, packed lightRGB) directly into the active slot via an `append()` helper instead of memcpy'ing into the per-node ring. `Render.TerrainSolid` ([`mclib/txmmgr.cpp:1281–1343`](../../../mclib/txmmgr.cpp)) dispatches `TerrainPatchStream::flush()` instead of iterating `masterVertexNodes` for `MC2_ISTERRAIN \| MC2_DRAWSOLID`. `terrainDrawIndexedPatches` ([`GameOS/gameos/gameos_graphics.cpp:2677–2844`](../../../GameOS/gameos/gameos_graphics.cpp)) is replaced by a slimmer `TerrainPatchStream::issueDraws()` that issues 1–N draws against the SSBO with per-material binds. |

### B.3 What ships first (M0 slice)
1. `TerrainPatchStream` allocates persistent SSBO + index buffer, with frame-fence + slot rotation copied from [`gos_static_prop_batcher.cpp:664–838`](../../../GameOS/gameos/gos_static_prop_batcher.cpp).
2. New `gos_terrain_modern.tese` (or a compile-time branch in the existing TES) reads `worldPos`/`worldNorm` indexed by `gl_VertexID` from the SSBO instead of a vertex-attribute pull. **GBuffer1 output uses the same `rc_gbuffer1_*` helper that `gos_terrain.frag` currently uses for TerrainBase** — verify by reading the active terrain FS before the spec, not by assuming `screenShadowEligible`. The current registry inventory (per F3 closing report) shows terrain/overlays/decals/grass typically opt into the terrain/self-shadow-handled path (likely `rc_gbuffer1_shadowHandled(...)` per [`shaders/include/render_contract.hglsl:20`](../../../shaders/include/render_contract.hglsl)), **not** `screenShadowEligible`. Do not flip terrain shadow semantics by accident.
3. `TerrainQuad::draw` adds a modern-mode branch: when `MC2_MODERN_TERRAIN_SURFACE=1` *and* tess is on, append to `TerrainPatchStream` and skip `addVertices`/`fillTerrainExtra` for the SOLID path. `pz` gate runs unchanged before append.
4. `Render.TerrainSolid` dispatches `TerrainPatchStream::flush()`. Other phases of `renderLists` untouched.
5. Static-shadow re-feed: in this slice, `Shadow.StaticAccum` keeps its current path (still reads `MC_VertexArrayNode::extras`). To preserve shadow correctness, the legacy `fillTerrainExtra` writes still happen (cheap; ~5.7 MB/frame memcpy is *not* the cost we're attacking). Only the `gos_VERTEX` pack and the `addVertices` ring write for SOLID are dropped. **Net per-frame upload reduction targets the ~7.7 MB `gos_VERTEX` memcpy and the ~13 MB GL upload churn.**

> ⚠️ **Duplication caveat.** M0 may temporarily duplicate world-position data: legacy `extras` for static shadows **plus** modern SSBO for color. The first slice must measure net CPU time and net upload bytes, not assume a win from the gross-memcpy reduction alone. If duplication outweighs savings, gate `fillTerrainExtra` modern-mode-off (deferred to B' shadow co-migration) becomes the gating decision — don't rationalize through it.

Solid opaque terrain only. Water / decal / overlay / non-tess fallback / dynamic-mech-shadow paths untouched.

### B.4 Killswitch / bisect handle
`MC2_MODERN_TERRAIN_SURFACE` env (0/1, default 0 until shadow coexistence is confirmed across tier1 + Wolfman). Plus a `RAlt+Shift+T` runtime toggle that flips the same flag mid-frame for live A/B (fence rotation makes this safe between frames). When OFF: `TerrainPatchStream::flush()` is a no-op, `addVertices` runs as before, `Render.TerrainSolid` iterates `masterVertexNodes` as today. Shadow path runs identically in both modes.

### B.5 New buffers / structures
- `TerrainPatchStream` (CPU-side append wrapper + GL persistent-mapped SSBO + 16-bit IBO + per-frame fences).
- New shader entry: modern TES path indexed by `gl_VertexID`. Same TCS, same FS.
- New GL state save/restore block matching the static-prop batcher's lines [692–712, 817–834](../../../GameOS/gameos/gos_static_prop_batcher.cpp).

### B.6 Touched existing structures
- `MC_VertexArrayNode::vertices` for `MC2_ISTERRAIN \| MC2_DRAWSOLID` is left unwritten in modern mode (still allocated, still walked by shadow). The `extras` ring is kept hot for `Shadow.StaticAccum` until a follow-on slice unifies it.
- `gos_VERTEX` color-pass writes in `TerrainQuad::draw` SOLID branches are gated under modern mode.
- `terrainDrawIndexedPatches` body is preserved as `terrainDrawIndexedPatches_legacy` and becomes a fallback when modern mode is OFF.

### B.7 Static-shadow disposition
**Unchanged in M0.** `Shadow.StaticAccum` continues to read `MC_VertexArrayNode::extras`, which the legacy `fillTerrainExtra` still populates. This shape's M0 slice deliberately accepts a small CPU-pack duplication (modern stream + legacy extras) to keep the shadow path bisect-safe. A follow-on shape (Shape B') would unify shadow against the same SSBO once the color path is proven across tier1 + Wolfman + Magic.

### B.8 Non-tess fallback disposition
Explicitly gated: modern mode requires `gos_State_Terrain && tessellation enabled`. With either condition false, the legacy `gos_VERTEX` path runs as today. Non-tess fallback callers (the `else` branch at [`gameos_graphics.cpp:3005`](../../../GameOS/gameos/gameos_graphics.cpp)) are not modernized in this shape.

### B.9 Risk register (shape-specific)

| # | Risk | Canary | Mitigation |
|---|---|---|---|
| BR1 | AMD GL state regression — SSBO bind, persistent-mapping flags, `glMemoryBarrier` placement, fence wait on the wrong slot. | Black/empty terrain on AMD; `[GL_ERROR v1]` first-error print fires. | Save/restore ALL touched GL state per static-prop batcher pattern. `MC2_GL_ERROR_DRAIN_SILENT=0` (default-on) catches regressions. AMD smoke run before merge. |
| BR2 | F3 GBuffer1 contract violated (R11) — modern TES/FS routes to a different `rc_gbuffer1_*` helper than the legacy terrain path uses, OR drops the registry call entirely. | shadow_screen mis-classification: terrain double-shadowed, terrain self-shadow flips on/off, or props shadowed onto sky. | Modern FS keeps the **same** `rc_gbuffer1_*` helper that current `gos_terrain.frag` uses for TerrainBase (read it; do not guess). Pre-commit check via `scripts/check-render-contract-gbuffer1.sh`. |
| BR3 | Shadow re-feed mismatch (R8) — `Shadow.StaticAccum` reads stale or wrong-shape `extras` because legacy `fillTerrainExtra` was conditionally elided. | Static terrain shadows pop / wrong-camera-frame / missing patches. | M0 slice keeps `fillTerrainExtra` always-on (do NOT gate it on modern flag). Co-migration deferred to B'. F3 RAlt+F3 shadow A/B audit in smoke. |
| BR4 | `pz` gate (C6) accidentally bypassed when migrating the SOLID branch. | Behind-camera garbage triangles span screen; FPS collapse. | The append callsite must be inside the existing `pz` gate brace, not in front of it. Pre-commit grep on the four cluster sites. |
| BR5 | Cull cascade (C1) inadvertently disturbed if modern path skips `addTriangle` capacity reservation that some other consumer reads. | Mech / building disappearance. | `addTriangle` is a side-effect-free counter — verify by grep before the slice lands. Tier1 + `mc2_03` mech-heavy canary. |
| BR6 | Wolfman SSBO size insufficient. 200²=40000 visible verts × 80k triangles × per-vert record. Sizing miscalc → ring overflow at Wolfman zoom. | `[PATCH_STREAM v1] event=overflow` (instrument from day 1); silent corruption if missing. | Size from worst-case Wolfman + 25% headroom; add overflow-throw with `[PATCH_STREAM v1]` lifecycle print + Tracy alert. |
| BR7 | Grass pass (R9) breaks if it reaches into the legacy extras VBO and we changed its content shape. | Grass float-up or invisible. | Modern slice does **not** rewrite `terrain_extra_vb_`; it adds an SSBO alongside. Grass keeps reading the legacy buffer. RAlt+5 toggle in smoke. |
| BR8 | Stale-shader-cache mimic per `stale_shader_cache_symptom.md`. | Frozen-cloud + over-darkened terrain after deploy that bisects to a non-shader commit. | Force shader cache clear during smoke before bisect. |

### B.10 Empirical hooks
- Tracy: `Render.TerrainSolid` self time (expect drop), `Terrain.DrawPatches` (expect drop), `Terrain.TessDraw`. Per-frame GL upload byte-count (add a Tracy plot).
- Killswitch live A/B: `RAlt+Shift+T`.
- `[PATCH_STREAM v1]` lifecycle prints: init / first-flush / overflow / fence-stall / shutdown.
- `MC2_TGL_POOL_TRACE=1` always — modernization that doesn't disturb the pool should produce identical 600-frame summaries.

### B.11 Open questions
- Q-B1. Persistent-mapped + coherent vs persistent-mapped + explicit `glFlushMappedBufferRange`? Static-prop batcher uses the explicit-flush route; AMD-quirks doc may have a recommendation.
- Q-B2. Is one big draw per material *better* than the current ~6–10 batches, or do we want to keep batches small to stay under SSBO bind-range limits and avoid material-switch overhead? **Note:** "one or a small fixed number of draws" presumes per-material grouping; PatchStream still groups by material/texture set unless material data moves to bindless or a sampler array. Do not let the spec assume "one draw" without also solving material binding.
- Q-B3. Can `Shadow.StaticAccum`'s 100-unit move trigger be served by the same SSBO with a depth-only TES + a fenced "shadow slot," or does it need its own buffer?
- Q-B4. Does the modern TES need any deferred-uniform plumbing, or are direct `glUniform*` calls (per C7) sufficient given the static-prop precedent?
- Q-B5. Is the grass pass an obvious follow-on consumer, or a permanent legacy reader?

### B.12 NOT this shape
- Not a `quadSetupTextures` rewrite. Quad setup stays in CPU, still reserves capacity, still resolves handles (Shape A is the right partner for the handle-resolution win).
- Not a shadow co-migration. Shape B' or a separate session.
- Not a non-tess fallback rewrite. The legacy fallback stays exactly as today.
- Not a water / decal / overlay change.

---

## Shape C — `PatchTable` (mission-load hoist of `quadSetupTextures`)

### C.1 One-line thesis
Pre-compute the *complete* per-quad submission record (per-layer texture-handle slot, UVs, layer mask, triangle indices) once at mission load into a `MapData::patchTable[numberQuads]`, primed by `Terrain::primeMissionTerrainCache` ([`mclib/terrain.cpp:561`](../../../mclib/terrain.cpp)). Per frame, `Terrain::geometry` produces a compact "visible patch index list" from the existing `clipInfo`/`objVertexActive` outputs; submission walks the index list and reads pre-built records — `TerrainQuad::setupTextures` and the per-quad `addTriangle` reservation pass disappear from the hot path.

### C.2 What stays legacy / what becomes modern

| Stays legacy | Becomes modern |
|---|---|
| All of `vertexProjectLoop`, `clipInfo`/`setObjBlockActive`/`setObjVertexActive`, `pz` gate, `projectFor*` wrappers. `TerrainQuad::draw` per-triangle worldPos/worldNorm pack into `gos_TERRAIN_EXTRA`. The 8 wrappers, the `terrainMVP` upload, the entire shader stack. Crater/decal/water/shadow paths. Non-tess fallback (it keeps using the legacy resolver). | `MapData::patchTable` populated at mission load. `Terrain::geometry quadSetupTextures` outer loop replaced by a lightweight `emitVisiblePatches()` that walks `quadList` and pushes `(patchIdx, layerMask)` records to a transient per-frame buffer. `TerrainQuad::setupTextures` body becomes a thin "modern path: skip; legacy path: resolve" switch. The per-quad **layer/triangle recipe** is hoisted to mission load; per-frame capacity reservation is replaced by either append into pre-sized worst-case buffers or a `visiblePatchList` count — `addTriangle`'s per-frame counter role is *replaced*, not "hoisted to mission load" (visibility is per-frame, so worst-case sizing is the actual mechanism). |

### C.3 What ships first (M0 slice)
1. New `MapData::patchTable` populated by extending `primeMissionTerrainCache` ([`mclib/terrain.cpp:561–577`](../../../mclib/terrain.cpp)). Records the **solid terrain layer recipe first** (`matSlot[]`, `uv[]`, `layerFlags`) per quad — slot indices, **never** resolved handles (per C10). `mineFlag` / `overlayFlag` / `waterFlag` may be present only as opaque metadata for parity/validation, **not consumed in M0**. Avoids inviting scope creep into water/overlay/mine while keeping the record extensible for follow-on slices.
2. Per-frame `Terrain::geometry` `quadSetupTextures` zone, under modern flag, becomes the cheap `emitVisiblePatches()` walk; legacy under `else`.
3. Submission consumer: cleanest pairing is **on top of Shape B** — `TerrainPatchStream::flush()` reads the visible-patch index list and the patch table to drive draws. As a standalone slice (without B), the consumer is still `TerrainQuad::draw` reading from the patch table instead of recomputing per-quad UV/handle, which preserves the existing submission ring.
4. Cache-invalidation paths for `Terrain::setOverlay` ([`mclib/terrain.cpp:808`](../../../mclib/terrain.cpp)), `Terrain::setOverlayTile` ([`mclib/terrain.cpp:802`](../../../mclib/terrain.cpp)), `Terrain::setTerrain` ([`mclib/terrain.cpp:814`](../../../mclib/terrain.cpp)), `Terrain::setVertexHeight` ([`mclib/terrain.cpp:844`](../../../mclib/terrain.cpp)), and `craterManager` updates must mark affected `patchTable` entries dirty and re-resolve them.

Solid opaque terrain only. Water / decals / overlay paths still call legacy `setupTextures`-equivalent code in M0; they are followed up in a sister slice.

### C.4 Killswitch / bisect handle
`MC2_MODERN_TERRAIN_PATCHES` env (0/1, default 0). Plus a `MC2_MODERN_TERRAIN_PATCHES_VALIDATE=1` mode that runs **both** paths in the same frame (legacy `setupTextures` + modern table read), compares per-quad records, and prints `[PATCH_TABLE v1] event=mismatch quad=<n> field=<x> legacy=<a> modern=<b>` on divergence. Validate-mode for the first week of bake; off in production.

### C.5 New buffers / structures
- `MapData::patchTable` (CPU-side; `numberQuads` × ~64 bytes ≈ a few MB at standard map size).
- Per-frame `visiblePatchList` (CPU; 4 bytes × visible quads).
- No new GPU buffers (those come from Shape B if composed).
- No shader changes in M0.

### C.6 Touched existing structures
- `MapData::WorldQuadTerrainCacheEntry` ([`mclib/mapdata.h:69–122`](../../../mclib/mapdata.h)) becomes the seed data for `patchTable` — likely subsumed into it.
- `TerrainQuad::setupTextures` body becomes a thin switch.
- `Terrain::primeMissionTerrainCache` extends to also populate `patchTable`.

### C.7 Static-shadow disposition
Unaffected — `patchTable` does not change what `Shadow.StaticAccum` reads (the per-vertex extras stream is still produced by `TerrainQuad::draw`). If composed with Shape B, shadow continues per B.7.

### C.8 Non-tess fallback disposition
Untouched. Legacy `setupTextures` resolution stays available behind the killswitch and is the only path the non-tess fallback takes.

### C.9 Risk register (shape-specific)

| # | Risk | Canary | Mitigation |
|---|---|---|---|
| CR1 | Cache-invalidation miss — `setOverlay`/`setOverlayTile`/`setTerrain` mutate but `patchTable` entry not marked dirty → stale UVs / wrong material on a cratered or footprinted patch. | Visible footprint or scorch in wrong place; or original material persists where overlay should appear. | Validate mode (C.4) catches; explicit dirty-mark in every mutator; `[PATCH_TABLE v1] event=invalidate` on each mark. |
| CR2 | Live texture handle (C10) regression — table accidentally caches resolved handle instead of slot index. | Stale 0 handle → black or wrong texture mid-mission. | Table stores slot only; handle resolved at submission time (Shape A's `TexResolveTable` is the natural pairing). Add static_assert / sanity probe on field types. |
| CR3 | Wolfman zoom rebuild — `resetVisibleVertices` reallocates `vertexList`/`quadList` ([`mclib/terrain.cpp:535`](../../../mclib/terrain.cpp)) but `patchTable` is sized to `numberQuads` (map total), not visible quads, so zoom change is safe. **But:** the visible-patch index list buffer must resize. | Wolfman zoom toggle stutter or buffer overrun. | Allocate `visiblePatchList` for worst-case Wolfman size at mission load, never per-zoom. |
| CR4 | Mod-content compatibility — Magic / Carver5O / MCO Omnitech missions exercise edge cases in heightmap sizes / unusual overlay sequences that hit code paths not covered by tier1. | Mod-only crash or wrong texture. | Run Carver5O and Magic canary missions in addition to tier1 before merge. R6/R7/R13 in §9. |
| CR5 | Mission-load time regression — `primeMissionTerrainCache` already exists; extending it for `patchTable` should not be a step function, but a 100-side map has 14400 quads and a 120-side has 14400 also — record-by-record resolution may add noticeable load latency. | Mission-load time regression in `[INSTR v1]` startup banner; `MC2_HEARTBEAT=1` shows pre-mission-render gap widening. | Time the `primeMissionTerrainCache` zone in Tracy; budget ≤ +200ms for a standard map; abort to legacy path on timeout. |
| CR6 | Insufficient win without Shape A or Shape B — Tracy says quadSetupTextures is 1.17 ms but a meaningful fraction is `get_gosTextureHandle` already (a Shape A target). Removing setupTextures may transfer cost rather than eliminate it. | Tracy delta < 0.5 ms/frame at Wolfman. | Land Shape A first; then C measures the residual. Decision gate before committing to C. |
| CR7 | Validate-mode performance cost masking real wins. | Validate-mode FPS unrepresentative. | Validate-mode prints on mismatch only; per-quad record memcmp is cheap; validate is a debug build mode, not a perf measurement mode. |

### C.10 Empirical hooks
- Tracy: `Terrain::geometry quadSetupTextures` (expect → near-zero), `Terrain::primeMissionTerrainCache` (expect modest growth), `MC_TextureNode::get_gosTextureHandle` (unchanged unless Shape A also lands).
- Validate-mode env for divergence detection.
- Lifecycle prints on table populate, on invalidation, on per-frame visible-patch count.

### C.11 Open questions
- Q-C1. What is the actual call-graph of `Terrain::setOverlay` vs `Terrain::setOverlayTile` vs runtime crater spawning? Need full mutator inventory before signing up to the cache-invalidation contract.
- Q-C2. Is `MapData::WorldQuadTerrainCacheEntry` already most of `patchTable`'s shape? If yes, this is a much smaller delta than it sounds.
- Q-C3. Does Wolfman's `visibleVerticesPerSide`=200 imply a quad-count change beyond what `numberQuads` (a per-mission constant) covers? (No — per [`mclib/terrain.cpp:535`](../../../mclib/terrain.cpp) only the *visible* slice resizes; the map quad-count is fixed. But verify.)
- Q-C4. Shape C has sharper cache-invalidation correctness risk than Shape B. Should it be paired with a heavier instrumentation budget (validate mode for two weeks instead of one)?

### C.12 NOT this shape
- Not a buffer or shader change.
- Not a submission consolidation. The 13 MB/frame upload churn is untouched (Shape B's job).
- Not an admission change. `vertexProjectLoop` runs unchanged.
- Not a shadow change.

---

## Shape composition (M0 → M1 → M2 path, not a separate shape)

The shapes are intentionally additive. A natural sequencing:

| Slice | Lands | Validates | Cumulative win (estimated from Tracy) |
|---|---|---|---|
| M0a | Shape A `TexResolveTable` | tier1 + Wolfman + Magic canary | ~0.2–0.5 ms/frame at Wolfman (must be measured; some calls outside terrain-solid path remain); structurally invisible; almost no risk |
| M0b | Shape B `PatchStream` (color path only; shadow stays legacy) | tier1 + Wolfman + AMD smoke + RAlt+5 grass + RAlt+F3 shadow | ~0.5–1.0 ms/frame, ~13 MB/frame upload churn elimination |
| M0c | Shape C `PatchTable` (composed on B + A) | tier1 + Carver5O + Magic + validate mode | up to ~1.0 ms/frame; the largest individual zone (`quadSetupTextures`) goes to near-zero |
| M1 | Shape B' (shadow co-migration) | shadow-specific A/B | freed CPU memcpy ~5.7 MB/frame; correctness gate unblocks future shadow LOD |
| M2 | Non-tess fallback retirement (out of scope this brainstorm) | — | enables deletion of `gos_VERTEX` color-pass packing |

This composition is mentioned only because the operator brief asks "what stays legacy vs modern" and "what ships first" — answering those independently per shape would imply the shapes are mutually exclusive. They aren't.

---

## If forced to pick today

**Lead with Shape A.** It is the smallest commit, closest to no-risk, has the lowest-cost killswitch, and produces a measurable Tracy delta in one capture. It also de-risks Shape C by isolating the `get_gosTextureHandle` win from the structural patch-table win — Shape C's CR6 ("insufficient win without Shape A or B") becomes answerable rather than speculative. Shape B is the right *second* slice because it attacks the upload churn the static-prop batcher already proved we can fix; Shape C is the right *third* slice because by then the validate-mode comparison is meaningful and the cache-invalidation contract has a Shape-B-shaped consumer to feed.

**The single piece of evidence that would flip my preference to Shape B-first:** a follow-up Tracy capture (CSV-exported, two missions including Wolfman) showing that `Terrain.DrawPatches` self-time is materially higher than the snapshot's 0.83 ms/frame — say, ≥1.5 ms — *and* that `MC_TextureNode::get_gosTextureHandle` is already cache-warm enough that Shape A's win is <0.2 ms. That would mean per-batch GL submission, not handle resolution, is the real cost center, and Shape B should ship first.

**The single piece of evidence that would flip my preference to Shape C-first:** the §"Open questions" §Q-C2 turns out true (`MapData::WorldQuadTerrainCacheEntry` is *already* essentially `patchTable`'s shape), in which case Shape C reduces to a thin promotion of an existing data structure plus the cache-invalidation discipline, and the perf win + structural simplification together outweigh Shape A's lower-risk profile.

---

## Cross-shape open questions the brainstorm couldn't resolve

These belong on the spec author's intake, not in any one shape:

- **OQ-1.** What does a CSV-exported Tracy capture (two missions, including Wolfman, ≥10k frames) show for the relative weights of `quadSetupTextures` self vs `Terrain.DrawPatches` self vs `get_gosTextureHandle` self? The current snapshot is screenshot-extracted and ~3060 frames; Shapes A vs B priority depends on the answer.
- **OQ-2.** Is the static-shadow accumulation path's 100-unit camera-move heuristic robust enough to keep using the legacy `extras` ring through M0a + M0b + M0c, or does retaining `fillTerrainExtra` start to feel like a tax we should pay down inside Shape B's M0?
- **OQ-3.** AMD-driver-rules compliance: does `glBufferStorage` with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT` work on the RX 7900 XTX driver currently in use, or does the static-prop batcher use the explicit-flush variant for a reason?
- **OQ-4.** Mod content (Carver5O, Magic, MCO Omnitech) exercises terrain via standard heightmap packets. Is there any mod-only mutator (e.g. ABL stub doing terrain edits we haven't audited) that would surprise Shape C's cache-invalidation contract?
- **OQ-5.** F3 follow-up: any in-flight registry change that would re-shape `rc_gbuffer1_screenShadowEligible`'s signature would force Shape B's modern FS to follow. Worth confirming F3 is at rest before B lands.

---

## References

- Findings doc (cold-read input): [`docs/superpowers/explorations/2026-04-27-modern-terrain-surface-findings.md`](../explorations/2026-04-27-modern-terrain-surface-findings.md)
- Render contract registry design: [`docs/superpowers/specs/2026-04-26-render-contract-registry-design.md`](../specs/2026-04-26-render-contract-registry-design.md)
- Static-prop batcher (the in-tree pattern): [`GameOS/gameos/gos_static_prop_batcher.cpp:664–838`](../../../GameOS/gameos/gos_static_prop_batcher.cpp)
- Worktree CLAUDE.md (critical rules, smoke gate, debug instrumentation rule)
- AMD driver rules: [`docs/amd-driver-rules.md`](../../amd-driver-rules.md)
- Memory (load-bearing): `cull_gates_are_load_bearing.md`, `tgl_pool_exhaustion_is_silent.md`, `terrain_tes_projection.md`, `terrain_mvp_gl_false.md`, `mc2_texture_handle_is_live.md`, `deferred_vs_direct_uniforms.md`, `static_prop_projection.md`, `stale_shader_cache_symptom.md`, `debug_instrumentation_rule.md`
