# ModernTerrainSurface — Codebase-Mapping Findings

**Date:** 2026-04-27
**Branch:** `claude/nifty-mendeleev`
**HEAD:** `e0b34d8`
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Status:** research-and-mapping. **No design recommendation made.**
**Successor:** brainstorming agent reads this cold; cite file:line back from this doc.

This document describes what the legacy MC2 terrain submission pipeline actually does today, what is CPU-expensive, what is GPU-cheap, what GPU buffers exist, what is load-bearing, what beachheads exist, three plausible API seams (no recommendation), the dependency graph, and the risk register.

Every architectural claim cites file:line and was verified against current source. Memories are flagged where they were used as starting points.

---

## ⚠️ Premise-modifying findings (read first)

Three findings change the brief's framing.

### P1 — The "3.66 ms in MC2 terrain vertex building" memory is partially miscaptioned, and the measurable hot path is structurally different than suggested.

`memory/perf_profiling_results.md` is 2 days old and frames the bottleneck as "MC2 terrain vertex building." That phrasing collapses two distinct CPU costs that the current Tracy instrumentation already separates:

- **Per-vertex CPU projection + admission** (`Terrain::geometry vertexProjectLoop`, `mclib/terrain.cpp:1002–1162`). Iterates `numberVertices` (= `realVerticesMapSide² ≤ 14400` at 120 max, but only `visibleVerticesPerSide²` per frame — Wolfman bumps this to 200²=40000 per `mc2x_wolfman_reference.md`). Inner loop calls `eye->projectForTerrainAdmission` (which dispatches `projectZ` per `mclib/camera.h:525`) for every visible vertex, plus the angular-cull math at `mclib/terrain.cpp:1040–1053`.

- **Per-quad texture/UV resolution + per-triangle gos_VERTEX pack** (`Terrain::geometry quadSetupTextures` → `TerrainQuad::setupTextures`, `mclib/quad.cpp:250–545`; followed by `Camera::render` → `Terrain::render` → `TerrainQuad::draw`/`drawWater`/`drawMine`, `mclib/quad.cpp:1515,2174,3586`). This is where the per-vertex `gos_VERTEX` struct gets memcpy'd into the per-texture-node ring, six triangle calls per visible quad (`addTriangle` ×4 in `setupTextures` for solid+detail layers, then `addVertices` ×2 in `draw`).

Tracy already separates these zones (`Terrain::geometry vertexProjectLoop` vs `quadSetupTextures` vs the GameOS `Terrain.TessDraw` / `Terrain.DrawPatches` GPU zones). The brief's task §2 of "break the 3.66 ms into sub-costs" is largely already done by the existing zones — what is missing is per-zone numbers in the latest builds. The memory says nothing about which of these two CPU phases dominates, and the source has been heavily reorganized since the memory was written (the per-batch terrain extras VBO in `addTerrainExtra` at `mclib/txmmgr.h:1081`, the cached terrain-face cache in `MapData::WorldQuadTerrainCacheEntry` at `mclib/mapdata.h:69`, and the `primeMissionTerrainCache` warm-up at `mclib/terrain.cpp:561–577` were all added after the memory was captured).

A future brainstorm should not budget perf gains as if the memory's "3.66 ms self-time" number is intact. The actual current distribution is now partially measured by the operator-supplied Self-only Tracy snapshot (see "Tracy baseline snapshot" section). It supports quad/setup/submission as the larger near-term target, though a formal exported Tracy capture is still desirable before making final benchmark claims.

### P2 — Terrain admission has TWO independent CPU pipelines on the legacy path, not one.

The legacy terrain frame produces both:

1. **A submitted gos_VERTEX stream of pretransformed screen-space triangles** that `MC_TextureManager::renderLists()` (definition begins at `mclib/txmmgr.cpp:902`; the per-node `MC2_ISTERRAIN` solid-pass iteration that consumes the stream is at `mclib/txmmgr.cpp:995`) iterates as per-texture-node arrays of vertices. This is the DX6/MLR-era format.

2. **A parallel `gos_TERRAIN_EXTRA` stream of world-space (worldPos, worldNorm) per triangle**, accumulated alongside via `fillTerrainExtra` at `mclib/quad.cpp:92,1608,1752,1911,2053`, stored on `MC_VertexArrayNode::extras` (`mclib/txmmgr.h:82`), and fed to the GPU tessellation path through `gos_SetTerrainBatchExtras` at `mclib/txmmgr.cpp:1309` and `gosRenderer::terrainDrawIndexedPatches` at `GameOS/gameos/gameos_graphics.cpp:2790–2807`.

The CPU `gos_VERTEX` screen-space fields (`pz/pw/px/py`) appear vestigial *for the active tessellated terrain color pass* — the TES recomputes screen-space from `worldPos` (extras) using the `terrainMVP` chain (`shaders/gos_terrain.tese` projection at memory `terrain_tes_projection.md`), and `gosRenderer::drawIndexedTris` rewinds the indexed_tris mesh in the tess-active branch (`gameos_graphics.cpp:3003–3004`). However, the underlying `MC_TextureNode` / texture-manager node structures (the `MC_VertexArrayNode::vertices` rings populated by `addVertices`, the per-node iteration in `renderLists`) remain load-bearing for the non-tess fallback path (the `else` branch at `gameos_graphics.cpp:3005`, which still rasterizes the pretransformed `gos_VERTEX` stream through the legacy non-tess fragment shaders) and for static-shadow accumulation submission (`Shadow.StaticAccum` at `mclib/txmmgr.cpp:1184–1242`, which consumes the same node structures), until those consumers are independently audited or re-pointed. Do not interpret P2 as license to delete the node structures; only the **screen-space pretransform within the active color pass** is unambiguously dead.

The CPU `projectZ` work itself also retains a live consumer beyond the color pass: the `inView` admission bool drives `setObjBlockActive` / `setObjVertexActive` (`mclib/terrain.cpp:1131–1132`), which the cull-gate cascade depends on (see §5).

This is the single most surprising finding in this exploration. The CPU pipeline still pays the per-vertex projection cost but the GPU draw (under tessellation, color pass) doesn't consume the screen-space result — only the cull bool. A modernization that keeps the cull-active side effect, the fallback-path node structures, and the static-shadow submission, but drops the screen-space pretransform within the active color pass, is plausible. It still conflicts with §5 load-bearing constraints (the `pz` gate at `mclib/quad.cpp:1597–1602` and the `clipInfo` propagation through `setupTextures` cluster gating at `mclib/quad.cpp:270–273, 343–344`).

### P3 — Tessellation is already the production rendering path; the "CPU sets up triangles for GPU rasterization" mental model is inaccurate.

When `gos_State_Terrain` is set and tessellation is active, `gosRenderer::drawIndexedTris` (`GameOS/gameos/gameos_graphics.cpp:2987`) routes through `terrainDrawIndexedPatches` which issues `glDrawElements(GL_PATCHES, ...)` against the per-batch extras VBO. The `gos_VERTEX` data still gets uploaded to the indexed-tris mesh (`mesh->uploadBuffers()` at `gameos_graphics.cpp:2685`), but the TCS/TES use the extras (worldPos/worldNorm) and reconstruct screen-space. The `gos_VERTEX` SOLID terrain rasterization path is short-circuited at `gameos_graphics.cpp:3003–3004` (`indexed_tris_->rewind()`).

The non-tessellated terrain fallback (the `else` branch at line 3005) still rasterizes the pretransformed `gos_VERTEX` stream through `gos_tex_vertex_lighted.frag` / `gos_terrain.frag` non-tess shader. But under modern hardware + the engine's defaults, tessellation is on and that fallback rarely runs for terrain (only for non-tessellation-active modes).

Brainstorming should treat the "modernization" target as **moving the per-frame extras + per-triangle pack into something a GPU can consume directly**, not as "porting a software rasterizer to a GPU."

---

## §1 Legacy MC2 terrain admission pipeline — end-to-end trace

### 1.1 Frame entry

Hot path entry: `gameosmain.cpp:464,468–470` at `GameOS/gameos/gameosmain.cpp` —

```cpp
ZoneScopedN("Camera.UpdateRenderers");
Environment.UpdateRenderers();
```

`Environment.UpdateRenderers` is the function-pointer field declared at `GameOS/include/gameos.hpp:354`. The active production binding is `code/mechcmd2.cpp:2796` → `__stdcall UpdateRenderers()` at `code/mechcmd2.cpp:690`.

`UpdateRenderers()` body (`code/mechcmd2.cpp:690–782`):
- Sets up GL viewport + base render states (`gos_State_Filter`, `AlphaMode`, `Clipping`, `TextureAddress`, `Dither`).
- Calls `mission->render()` (`code/mission.cpp:754`) for the in-game scene.
- Calls `logistics->render()` for non-mission UI screens (mutually exclusive with mission).
- Calls `userInput->render()` and `DEBUGWINS_render()` for HUD/text (drawn after `endScene`, see F3 audit §1).

`mission->render()` (`code/mission.cpp:754–805`) calls `eye->render()` at line 767. `eye` is the `Camera*` (`GameCamera`-derived); the production binding is `GameCamera::render()` at `code/gamecam.cpp:140` (referenced via vtable). It also resets the TGL CPU pools (`vertexPool->reset()` etc.) at the END of mission-render, which is the per-frame "shapes vanish if you exhaust mid-frame" boundary.

### 1.2 GameCamera::render — terrain admission window

`code/gamecam.cpp:140–256`. The explicit per-frame ordered call list, tagged with the Tracy zones already in place:

| Step | Call | File:line | What it submits |
|---|---|---|---|
| 1 | `Camera.BuildMVP` | `code/gamecam.cpp:151–189` | Builds `terrainMVP` (axisSwap × worldToClip, see `terrain_mvp_gl_false.md`); uploads `terrainViewport`, `terrainCameraPos`, `terrainLightDir` to GL (no per-vertex work yet). |
| 2 | `theSky->render(1)` | `code/gamecam.cpp:194` | Sky background quad. |
| 3 | `land->render()` | `code/gamecam.cpp:199` | Per-quad submit: `Terrain::render` → for each `TerrainQuadPtr` in `quadList[0..numberQuads]`, calls `currentQuad->draw()` then `currentQuad->drawMine()`. **This is per-frame queue building, not GL drawing.** |
| 4 | `craterManager->render()` | `code/gamecam.cpp:205` | Decals/craters submit to per-texture-node lists. |
| 5 | `ObjectManager->render(true,true,true)` | `code/gamecam.cpp:210` | Mechs/buildings/vehicles immediate-draw via MLR. |
| 6 | `land->renderWater()` | `code/gamecam.cpp:215` | Per-quad water overlay submit. |
| 7 | `ObjectManager->renderShadows(...)` | `code/gamecam.cpp:221` | (drawOldWay) blob shadows. |
| 8 | `mission->missionInterface->drawVTOL()` | `code/gamecam.cpp:227` | VTOL HUD overlay. |
| 9 | `compass->render(-1)` | `code/gamecam.cpp:235` | Compass HUD. |
| 10 | `mcTextureManager->renderLists()` | `code/gamecam.cpp:242` | **Flush all queued terrain/overlay/water/decal draws to GL.** |

`mission->update()` runs *before* `UpdateRenderers` (in the Tracy `GameLogic.Mission.Terrain` and `GameLogic.Mission.TerrainGeometry` zones at `code/mission.cpp:474,499`). `land->update()` is in update; `land->geometry()` is the heavy CPU vertex pass — see §1.3.

### 1.3 The CPU vertex-projection pass — `Terrain::geometry`

`mclib/terrain.cpp:980–1193`. Tracy zone: `Terrain::geometry`. Inner zones: `vertexProjectLoop` (1002), `quadSetupTextures` (1169), `cloudUpdate` (1190).

The vertex loop:

```cpp
for (i=0; i<numberVertices; i++) {                        // mclib/terrain.cpp:1003
  // angular cull (mclib/terrain.cpp:1029-1054):
  Camera::cameraFrame.trans_to_frame(objectCenter);
  float distanceToEye = objectCenter.GetApproximateLength();
  if (distanceToClip > CLIP_THRESHOLD_DISTANCE) {
    float object_angle = fabs(objectCenter.z) * clip_distance;
    float extent_angle = VERTEX_EXTENT_RADIUS / distanceToEye;
    if (object_angle > (vClipConstant + extent_angle)) onScreen = false;
    else if (fabs(objectCenter.x) * clip_distance > (hClipConstant + extent_angle)) onScreen = false;
  }
  // haze/fog calc (1056-1069)
  // [PROJECTZ:BoolAdmission id=terrain_cpu_vert_admit]
  if (onScreen) {
    inView = eye->projectForTerrainAdmission(vertex3D, screenPos);   // mclib/terrain.cpp:1099
    currentVertex->px = screenPos.x; ... pw = screenPos.w;            // 1101-1104
  }
  currentVertex->clipInfo = onScreen;                               // 1124
  if (currentVertex->clipInfo) {
    setObjBlockActive(currentVertex->getBlockNumber(), true);       // 1131
    setObjVertexActive(currentVertex->vertexNum, true);             // 1132
    // updates leastZ/mostZ/leastW/mostW for inverseProject
  }
}
```

**`numberVertices` is `visibleVerticesPerSide²`**, default 120²=14400 in stock zoom and 200²=40000 in Wolfman zoom (per `MEMORY.md` Wolfman line, `mc2x_wolfman_reference.md`). At Wolfman zoom this loop runs 40k iterations per frame, each doing one matrix multiply (`projectZ` body at `mclib/camera.h:435–470`), and writes four floats + a bool per vertex.

**The projectZ work is partially vestigial** (see P2). The `clipInfo` bool is what gates the cull cascade (line 1131–1132); the `pz/pw/px/py` get written into the `Vertex` struct but for tessellated terrain the GPU doesn't consume them — only the legacy non-tess `gos_terrain.frag` path consumes `pz`.

After `vertexProjectLoop`, the `quadSetupTextures` loop iterates `numberQuads` (= `numberVertices` minus a strip) and calls `TerrainQuad::setupTextures()` per quad.

### 1.4 The per-quad texture/triangle reservation — `TerrainQuad::setupTextures`

`mclib/quad.cpp:250–545`. Each visible quad (gated on `clipInfo` of any of its vertices) reserves triangles in the per-texture node ring via `mcTextureManager->addTriangle(handle, MC2_ISTERRAIN | MC2_DRAWSOLID)` (×2 for the two terrain triangles, ×2 for the detail-layer triangles, ×0–4 for mines, ×0–2 for overlay).

This is **two passes**: `setupTextures` *reserves* (pre-counts capacity into `MC_VertexArrayNode::numVertices`), then `TerrainQuad::draw()` (called from `Terrain::render` in step 3 above) *fills* (memcpys actual `gos_VERTEX` into the reserved range). The split is for one-pass capacity planning so `addVertices` doesn't have to grow per call. Reservation is `addTriangle`, fill is `addVertices`. See `mclib/txmmgr.h:685` (`addTriangle`) vs `820` (`addVertices`).

There is a `MapData::WorldQuadTerrainCacheEntry` cache (`mclib/mapdata.h:86–122`) that allows `setupTextures` to skip per-quad UV/handle re-resolution for previously-resolved quads (`TerrainQuad::setupTextures cachedVisibleSubmission` zone at `mclib/quad.cpp:433`). This is the most recent CPU-side optimization in the path. The `Terrain::primeMissionTerrainCache` call at `mclib/terrain.cpp:561` warms this at mission load.

### 1.5 The per-quad triangle fill — `TerrainQuad::draw`

`mclib/quad.cpp:1515–2173`. For each triangle (4 quads × 2 triangles per quad × 2–3 layers), packs three `gos_VERTEX` structs (24 bytes × 3 = 72 bytes per triangle) and:

- Calls `mcTextureManager->addVertices(handle, gVertex, MC2_ISTERRAIN | MC2_DRAWSOLID)` (`mclib/quad.cpp:1607`) — this `memcpy`s 96 bytes (3 × 32 — `gos_VERTEX` is 32, not 24) into the per-node ring buffer (`mclib/txmmgr.h:861`).
- Calls `fillTerrainExtra(handle, flags, v0, v1, v2)` (`mclib/quad.cpp:1608`) — this writes the per-triangle `gos_TERRAIN_EXTRA` (worldPos[3] + worldNorm[3] = 24 bytes per vertex × 3 = 72 bytes per triangle) via `mcTextureManager->addTerrainExtra` (`mclib/txmmgr.h:1081`).

The `pz` gate at `mclib/quad.cpp:1597–1602` (`if all three z in [0,1) ...`) is the load-bearing CPU pre-rejection per `terrain_tes_projection.md`. Every triangle that fails this gate is silently dropped.

### 1.6 The flush — `MC_TextureManager::renderLists`

Function definition begins at `mclib/txmmgr.cpp:902`; the terrain-flush subsection (per-node iteration over `masterHardwareVertexNodes`, gated by `MC2_ISTERRAIN | MC2_DRAWSOLID`) starts at `mclib/txmmgr.cpp:995` and runs through end of function. **This is where actual GL submission happens.** Tracy phases:

- `Render.3DObjects` (1086–1146): iterates `masterHardwareVertexNodes` for opaque-object draws (mechs, buildings via `ShapeRenderer::render`).
- `Shadow.StaticAccum` (1184–1242): renders terrain SOLID-flag nodes through `gos_DrawShadowBatchTessellated` to the static shadow map. Cached: re-renders only when camera moves >100 units (line 1190).
- `Shadow.DynPass` (1245–1276): per-frame dynamic-mech shadow draws via `gos_DrawShadowObjectBatch`.
- `Render.TerrainSolid` (1281–1343): for each `masterVertexNodes[i]` with `MC2_ISTERRAIN | MC2_DRAWSOLID`, sets `gos_State_Terrain=1`, calls `gos_SetTerrainBatchExtras(extras, count)` (line 1309), then `gos_RenderIndexedArray(vertices, totalVertices, indexArray, totalVertices)` (line 1317). The `RenderIndexedArray` upcast lands in `gosRenderer::drawIndexedTris` → `terrainDrawIndexedPatches` (the registry-tagged terrain entry at `GameOS/gameos/gameos_graphics.cpp:2995`).
- `Render.GpuStaticProps` (1352–1358): `GpuStaticPropBatcher::instance().flush()` (gated by `g_useGpuStaticProps`).
- `Render.TerrainOverlays` / `Render.Decals` (1367, 1372): world-space overlay batches via `gos_DrawTerrainOverlays` / `gos_DrawDecals`.
- `Render.Overlays` (1378–end): per-flag re-iteration of `masterVertexNodes` for water (`MC2_ISWATER` at flag bit, line 1399–1456) and other alpha overlays. Water gets `gos_State_Water` 1 or 2 (line 1416–1418) which routes the legacy `gos_tex_vertex.frag` shader's `isWater` uniform — this is the Water rendering architecture per memory `water_rendering_architecture.md`.
- `Render.NoUnderlayer` (1463): `MC2_GPUOVERLAY`-flagged terrain — note `MC2_GPUOVERLAY` is defined at `mclib/txmmgr.h:65` but never set in production (per F3 report Task 4 finding); this loop is dead at runtime.

### 1.7 GPU draw — `terrainDrawIndexedPatches`

`GameOS/gameos/gameos_graphics.cpp:2677–2844`. Verified file:line.

Per-call (i.e., per per-texture-node × per-frame):
- `mesh->uploadBuffers()` (2685): `glBufferData` to re-upload the indexed_tris mesh's vb/ib (orphan + upload, GL_DYNAMIC_DRAW). The mesh capacity is 1024×60 vertices, 1024×60 indices (`gameos_graphics.cpp:1693`).
- `material->apply()` (2688): `glUseProgram` + flushes deferred uniforms.
- ~25 direct `glUniform*` calls (2700–2738) for tessellation params, MVP, viewport, light, POM, time, etc. Locations are *cached* in `terrainLocs_` (line 2693, `cacheTerrainUniformLocations`) — the per-frame string lookups that the perf memory called out have been eliminated.
- 4 `glActiveTexture` + `glBindTexture` calls for material normal maps (2742–2747).
- 2–3 shadow-map binds (2750–2780).
- Updates the `terrain_extra_vb_` with the batch's gos_TERRAIN_EXTRA stream via `updateBuffer` (2793–2795). This is the per-frame world-pos+world-norm upload for tessellation.
- Sets attribute pointers for `worldPos` (loc 4), `worldNorm` (loc 5) (2796–2808). Non-cached — calls `glGetAttribLocation` per draw (line 2797–2798).
- `glPatchParameteri(GL_PATCH_VERTICES, 3)` (2819).
- `glDrawElements(GL_PATCHES, ni, ...)` (2822) — the actual draw.

### 1.8 Cull pipeline interaction with terrain

Per `cull_gates_are_load_bearing.md` (memory) and verified at `mclib/terrain.cpp:1131–1132`:

- Terrain has its own per-vertex `clipInfo` set by the angular cull + projectZ admission. **It does NOT use `inView`/`canBeSeen` like props.**
- `clipInfo=true` triggers `setObjBlockActive(blockNum, true)` and `setObjVertexActive(vertexNum, true)`.
- These two flags then gate **prop iteration** in `GameObjectManager::update` and `render` (`code/objmgr.cpp:1731,1486` per `gpu-static-prop-cull-lessons.md`). Out-of-block props don't get `update()` called — see `cull_gates_are_load_bearing.md` for the five-way load-bearing chain.

**The terrain admission cull is therefore the cause-chain root for the prop visibility cascade.** A modernization that moves terrain admission to GPU clipping (and stops setting `objBlockActive` per-vertex) would cascade into prop disappearance, mech shape destruction, and TGL pool semantics. This is the single hardest constraint.

### 1.9 Wolfman vs standard zoom

Both frames trace through identical code paths. The only differences are:

- `Terrain::resetVisibleVertices(maxVisibleVertices)` (`mclib/terrain.cpp:535`) is called when zoom changes; reallocates `vertexList` and `quadList` to `visibleVerticesPerSide²` size.
- Wolfman bumps `visibleVerticesPerSide` from 120 to 200 → 40000 vs 14400 visible verts → ~2.78× more iterations of `vertexProjectLoop`, ~2.78× more `setupTextures` calls, ~2.78× more `TerrainQuad::draw` triangle packs.
- Altitude/clip params change `vClipConstant`/`hClipConstant` but the loop structure is identical.

There is no separate Wolfman code path. The cost difference is purely O(N²) on `visibleVerticesPerSide`.

---

## §2 What's actually CPU-expensive

### 2.1 Validation against current code

The `perf_profiling_results.md` claim "3.66ms self-time is MC2 terrain vertex building" was **plausible but unverified for current HEAD** at exploration start; subsequently partially verified by the Tracy baseline snapshot — see that section for current numbers. The relevant Tracy zones present and emitting timing data today (verified at `mclib/terrain.cpp:982,1002,1169,1190` and `mclib/quad.cpp:433,438`):

- `Terrain::geometry`
  - `Terrain::geometry vertexProjectLoop` — the per-vertex projection + cull loop (§1.3)
  - `Terrain::geometry quadSetupTextures` — the per-quad reservation pass (§1.4)
  - `Terrain::geometry cloudUpdate`
- `Terrain::primeMissionTerrainCache build` / `warm` — mission-load only
- `TerrainQuad::setupTextures cachedVisibleSubmission` / `resolveFallback` — cache hit/miss inside quad-loop

Plus the GPU-side Tracy zones in `gameos_graphics.cpp:2678` (`Terrain.DrawPatches`), `2988` (`Terrain.TessDraw`), `txmmgr.cpp:1282` (`Render.TerrainSolid`).

**A Self-only Tracy snapshot has now been taken** (see "Tracy baseline snapshot" section) and supports quad/setup/submission as the larger near-term target relative to the projection loop. A formal exported CSV/Tracy capture against tier1 + Wolfman is still desirable before publishing perf numbers, but seam selection is no longer perf-blind. This exploration did not run instrumentation beyond that snapshot per the brief's read-only constraint.

### 2.2 Sub-cost decomposition (structural, not measured)

Per-frame at Wolfman (40000 visible verts, ~80000 triangles, ~6–10 batches):

| Cost | Site | Magnitude estimate | Hoistable to mission load? |
|---|---|---|---|
| Per-vertex `projectZ` matrix multiply + `fabs(rhw)` divide | `mclib/camera.h:435–470` via `mclib/terrain.cpp:1099` | ~40k × ~20 FP ops ≈ 800k FP ops/frame; near-trivial individually but cache-hostile (writes into `Vertex` struct in the heap-allocated `vertexList`) | No — depends on per-frame camera matrix. Could move to GPU compute or fold into TES (which already does it). |
| Per-vertex angular cull (`Camera::cameraFrame.trans_to_frame` + `GetApproximateLength`) | `mclib/terrain.cpp:1024–1054` | ~40k × ~30 FP ops ≈ 1.2M FP ops/frame | No per-frame; could be replaced with frustum-AABB test per block (numberBlocks ≪ numberVertices). |
| Per-vertex haze/fog factor | `mclib/terrain.cpp:1056–1069` | ~40k mults | No, but trivially vectorizable; depends on per-frame `Camera::MaxClipDistance`. |
| Per-vertex `setObjBlockActive` / `setObjVertexActive` writes | `mclib/terrain.cpp:1131–1132` | Cache-line bouncing into `objBlockInfo[]` and `objVertexActive[]` arrays | No — load-bearing for prop cull (§5). |
| Per-quad `TerrainQuad::setupTextures` texture-handle resolution | `mclib/quad.cpp:430–540` | Hits the new `WorldQuadTerrainCacheEntry` cache (`mapdata.h:86`); fallback path is heavier | Already largely hoisted via the cache + `primeMissionTerrainCache`. |
| Per-quad `addTriangle` (capacity reservation) | `mclib/quad.cpp:284–540` (40+ callsites); body at `mclib/txmmgr.h:685` | ~40k quads × 4–8 calls = ~250k function calls/frame | Conceptually hoistable — capacity is roughly knowable per-mission given visible-vertex limit. |
| Per-triangle `gos_VERTEX` pack + `addVertices` memcpy | `mclib/quad.cpp:1567–1612, 1607` (and 4 sister blocks); body at `mclib/txmmgr.h:861` | ~80k triangles × 96-byte memcpy = ~7.7MB/frame memcpy traffic | Yes — if GPU consumes worldPos directly, the gos_VERTEX pack is dead weight. |
| Per-triangle `fillTerrainExtra` worldPos/worldNorm pack | `mclib/quad.cpp:92,1608` (and sisters) | ~80k triangles × 72-byte memcpy ≈ ~5.7MB/frame | No — the GPU consumes this stream today. |
| Per-batch `glBufferData` orphan + upload of indexed_tris VB/IB | `gameos_graphics.cpp:510–518` via `terrainDrawIndexedPatches:2685` | Per batch: 60k × 32 = 1.9MB VBO + 120KB IBO; ~6–10 batches/frame ≈ ~12–19MB/frame GL upload | Yes — could be persistent-mapped or single uploaded batch. |
| Per-batch `terrain_extra_vb_` upload | `gameos_graphics.cpp:2793` | Per batch: count × 24 bytes ≈ 0.5–2MB/frame | Yes — same as above. |
| Per-draw `glGetAttribLocation` for worldPos/worldNorm | `gameos_graphics.cpp:2797–2798` | Two string lookups per batch | Yes, trivially — uniform-loc cache pattern works here. |

The "per-vertex string-formatted uniform lookups" the perf memory called out **have already been fixed** for terrain — `cacheTerrainUniformLocations` at `gameos_graphics.cpp:2693` caches them. But two `glGetAttribLocation` calls per batch remain at lines 2797–2798 (mentioned above) and the grass pass at lines 2933–2934 / 2868–2870 (uncached `L = [&](name)` lambda for ~15 uniforms per draw — that one is a regression and fix candidate independent of any seam decision).

### 2.3 Per-frame allocations / memcpys

- **No per-frame `malloc`s** in the terrain pipeline — `MC_VertexArrayNode::extras` is `malloc`'d on first use per node (`mclib/txmmgr.h:1098`), then reused.
- **Heavy memcpy traffic**: ~13MB/frame across `addVertices` + `addTerrainExtra` + GL uploads (estimates above).
- **No per-vertex allocator hits**.

The CPU stalls if any are likely cache-miss / memcpy bandwidth, not malloc. This is a separate axis from the "per-string uniform lookup" hypothesis.

---

## §3 What's GPU-expensive

### 3.1 The "GPU has massive headroom" claim

`memory/perf_profiling_results.md` claims 11–15% GPU util at Wolfman with 46–53 FPS. **This was not re-measured as part of this exploration.** No attempt was made to refute it.

If true, the modernization direction implied is "fewer, larger draw calls" (MDI / GPU-driven cull / persistent buffers), not "smaller shaders." This is consistent with the brief.

### 3.2 Heavy GPU paths

Even at low overall util, two GPU passes consume a real fraction of the frame budget per the same memory and architecture doc:

- **Tessellation** (`shaders/gos_terrain.tcs/.tese`): runs at `tessLevel` × per-patch vertex factor. With `terrain_tess_level_` typically 8–32 (per `gameos_graphics.cpp:2696`), each input triangle becomes 64–1024 output triangles. Heavy at default settings; F6–F12 hotkeys tune it (per `MEMORY.md`).
- **POM (parallax occlusion mapping)** in `shaders/gos_terrain.frag`: `pomParams = (terrain_pom_scale_, 8.0, 32.0)` at `gameos_graphics.cpp:2729`. The 8/32 likely controls min/max ray-march steps. Disabling POM is already a known-cheap optimization.

Other notable GPU costs:
- 16-tap stratified Poisson PCF in `shaders/include/shadow.hglsl` (per architecture.md).
- Triplanar cliff sampling and FBM cloud shadow noise per `MEMORY.md` Phase 4 line.

No specific GPU costs were measured during this exploration. The "GPU is idle" framing remains the working hypothesis.

---

## §4 Catalog of terrain-related GPU buffers

### 4.1 Static (allocated once)

- **`terrain_extra_vb_`** GL buffer at `GameOS/gameos/gameos_graphics.cpp:1772`. Capacity `1024 × 120` × `sizeof(gos_TERRAIN_EXTRA)` = 122880 × 24 bytes = 2.95MB. `GL_DYNAMIC_DRAW`. **Allocated once at engine init** (line 1771) but **fully re-uploaded per batch** at `gameos_graphics.cpp:2793–2795`. So it's "static-handle, per-frame-content."
- **`indexArray`** in `mclib/txmmgr.cpp` (the canonical 0,1,2,3,... index array) — referenced at line 1317 etc. Static-content, 16-bit indices.
- **Material-normal textures** (units 5–8): `terrain_mat_normal_[i]` at `gameos_graphics.cpp:2742–2747`. Loaded at mission start, never updated.
- **Color map texture** (unit 0). Loaded from `terrain_colormap_<missionname>.tga` at mission load.
- **Shadow textures** (units 9–10). FBO-attached; content updated per frame, allocation static.
- **`gosMesh::indexed_tris_`** (`gameos_graphics.cpp:1693`) — the ring-buffer mesh that everything routes through. 60k vert / 60k idx capacity. CPU-side `pvertex_data_` + `pindex_data_` arrays plus matching GL VBO/IBO. **Allocated once.**

### 4.2 Per-mission

- **`Terrain::vertexList`** (`VertexPtr`) at `mclib/terrain.cpp:508`. Size = `sizeof(Vertex) × visibleVerticesPerSide²`. CPU-side; not on GPU. **One entry per visible vertex per frame is overwritten in `Terrain::geometry`.**
- **`Terrain::quadList`** at `mclib/terrain.cpp:515`. Same size scaling. CPU-side.
- **`MapData::blocks`** (`PostcompVertex *`) at `mclib/dmapdata.cpp:174`. Size = `realVerticesMapSide² × sizeof(PostcompVertex)`. The actual heightmap. Loaded from a `PacketFile` packet at `mclib/terrain.cpp:295`. CPU-side; never directly GPU-uploaded as a heightmap (the GPU only sees per-triangle interpolated worldPos/worldNorm).
- **`MapData::terrainFaceCache`** (`WorldQuadTerrainCacheEntry *`) at `mclib/mapdata.h:74`. Per-quad UV+handle cache, primed at mission load (`Terrain::primeMissionTerrainCache` at `mclib/terrain.cpp:561`).
- **TGL pool buffers** (color/vertex/face/shadow/triangle pools at `code/mission.cpp:3097–3110`, sized 500K/500K/200K/500K/200K per `tgl_pool_exhaustion_is_silent.md`). Used by mech/building TGL, not terrain, but co-resident in the same `tglHeap`.

### 4.3 Per-frame (rebuilt)

**This is the modernization target.** Every item below is re-uploaded or re-built every frame at the rate shown.

| Buffer | Rebuild rate | Size | Site |
|---|---|---|---|
| `gosMesh::indexed_tris_` VBO content | per batch (~6–10/frame) | up to 60k × 32 = 1.9MB | `gameos_graphics.cpp:512–518` from `pvertex_data_` |
| `gosMesh::indexed_tris_` IBO content | per batch | up to 60k × 2 = 120KB | same |
| `terrain_extra_vb_` content | per batch | count × 24 bytes (count = batch tri count × 3) | `gameos_graphics.cpp:2793–2795` |
| Per-`MC_VertexArrayNode::vertices` ring | per frame, per node | 32 bytes × `numVertices` (sized in `addTriangle`); typical batch ~30k–100k vertices | `mclib/txmmgr.h:861` (memcpy from `setupTextures`+`draw`) |
| Per-`MC_VertexArrayNode::extras` ring | per frame, per node | 24 bytes × `numVertices` | `mclib/txmmgr.h:1102` |
| `Vertex::px/py/pz/pw` (CPU only, but written every frame) | per frame, per vertex | 16 bytes × `visibleVerticesPerSide²` | `mclib/terrain.cpp:1101–1115` |
| `Camera.SceneDataUpload` UBO | once/frame | `sizeof(TG_HWSceneData)` | `mclib/txmmgr.cpp:1064–1081` |
| `lightDataBuffer_` UBO | once/frame | `lightDataStructuresCount × sizeof(TG_HWLightsData)` | `mclib/txmmgr.cpp:1049–1059` |
| Static shadow map content | every >100-unit camera move (or forced) | 8192×8192 depth buffer, accumulated | `mclib/txmmgr.cpp:1184–1242` |
| Dynamic shadow map content | per frame | 2048×2048 depth | `mclib/txmmgr.cpp:1244–1276` |
| `terrainMVP` upload | per frame | 64 bytes | `gameos_graphics.cpp:2718–2721` |
| `tessLevel/tessDistanceRange/tessDisplace/cameraPos/light/etc.` uniforms | per batch | ~20 vec4s | `gameos_graphics.cpp:2700–2738` |

### 4.4 Streaming

The `terrainFaceCache` has a residency notion via `MapData::ensureTerrainFaceCacheEntryResident` (`mclib/quad.cpp:434`) and `MapData::warmTerrainFaceCacheResidency` (`mclib/terrain.cpp:574`), but this is **CPU residency** for the pre-resolved UV/handle data, not GPU streaming. There is no LOD page-in/out for terrain on GPU.

---

## §5 Load-bearing constraints (must not break)

Each item names the consequence of breaking it (user-visible bug).

| # | Constraint | File:line | Consequence on break |
|---|---|---|---|
| C1 | **Cull-gate cascade.** `terrain.cpp:geometry` setting `setObjBlockActive`/`setObjVertexActive` from `clipInfo` is the cause-chain root for prop iteration in `GameObjectManager::update`/`render`. Memory: `cull_gates_are_load_bearing.md`. | `mclib/terrain.cpp:1131–1132` → `code/objmgr.cpp:1731,1486` (per memory) | Buildings disappear, mechs streak, props lose `update()` calls, objects get `setExists(false)`, TGL pool exhausts as iteration reaches mechs last. |
| C2 | **TGL pool exhaustion is silent.** `getVerticesFromPool` returns NULL → `TG_Shape::Render` early-out. | `mclib/tgl.h:1022`, `mclib/tgl.cpp:2536` (per memory `tgl_pool_exhaustion_is_silent.md`) | Random shapes (last-iterated → mechs first) silently vanish. No GL error, no assert. |
| C3 | **MC2 ARGB packing.** `gos_VERTEX.argb` is little-endian BGRA; GL attribute → `.bgra` swizzle in shader; SSBO uint → bit-decode. | `mclib/tgl.cpp:2119` (per memory `mc2_argb_packing.md`) | Black or color-channel-swapped buildings/mechs (the original 4af44f7 bug). |
| C4 | **`terrainMVP` uploaded with `GL_FALSE`.** Comment in `gamecam.cpp:165` says `GL_TRUE` and is wrong. | `code/gamecam.cpp:170–174`, upload at `gameos_graphics.cpp:2720` (per memory `terrain_mvp_gl_false.md`) | Terrain projects to wrong screen quadrant; full visible regression. |
| C5 | **TES projection chain — `abs(clip.w)` is load-bearing, NOT a DX7 leftover.** Three replacement attempts produced regressions (transparent terrain, garbage triangles, NaN gl_Position). | `shaders/gos_terrain.tese:100–108` (per memory `terrain_tes_projection.md`) | Most or all terrain disappears OR giant garbage triangles. |
| C6 | **CPU `pz` gate.** Four `MC2_ISTERRAIN \| MC2_DRAWSOLID` clusters in `quad.cpp` gate triangle submission on `pz ∈ [0,1)`. The TES has no clip-space signal that distinguishes front-of-camera from behind-camera verts. | `mclib/quad.cpp:1597–1602, 1660+, 1799+, 1961+` (per memory `terrain_tes_projection.md`) | Behind-camera verts produce giant garbage triangles in the rasterizer, framerate collapse. |
| C7 | **Deferred vs direct uniform discipline.** `setFloat`/`setInt` BEFORE `apply()`, `glUniform*` AFTER. | `glsl_program::apply` flow (per memory `deferred_vs_direct_uniforms.md` + worktree CLAUDE.md "Critical Rules") | Wrong uniforms reach shader → wrong material rendering or stale state. |
| C8 | **Render Contract Registry — `rc_gbuffer1_*` helpers.** Every modern terrain producer must continue to write `GBuffer1` via the typed helpers. Grep census enforces. | `shaders/include/render_contract.hglsl`, `mclib/render_contract.{h,cpp}`, F3 closing report | shadow_screen.frag mis-classifies pixels → terrain gets double-shadowed or mech shadows applied to terrain. The legacy `rc_gbuffer1_legacyTerrainMaterialAlpha` (water/shoreline) must remain the only continuous-alpha producer — F1 follow-up. |
| C9 | **The 8 `projectFor*` projectZ wrappers in `mclib/camera.h`.** `projectZ` itself is `[[deprecated]]`; all callsites must use the intent-specific wrappers per the projectZ Policy Split. | `mclib/camera.h:433–610`, projectz-callsite-inventory.md | Wrong policy applied at wrong site → wedge-class hazards (behind-camera verts admitted), regression in containment audit. |
| C10 | **Texture-handle live mutation.** MC2 texture handles mutate per-frame in `TransformMultiShape`; cache slot index, resolve handle at draw time. | per memory `mc2_texture_handle_is_live.md`; static prop batcher example at `gos_static_prop_batcher.cpp:790–793` | Stale (usually 0) texture handle → wrong texture or untextured mesh. |
| C11 | **ARGB swizzle from `lightRGB`.** The per-quad `gVertex[i].argb = vertices[i]->lightRGB` writes a DWORD that must be read with `.bgra` in CPU-path shaders. | `mclib/quad.cpp:1573,1583,1593,...` (per memory `mc2_argb_packing.md`) | Color channel swap on terrain lighting / selection highlight. |
| C12 | **`MC2_ISTERRAIN`, `MC2_DRAWSOLID`, `MC2_DRAWALPHA`, `MC2_ISWATER`, `MC2_ISWATERDETAIL`, `MC2_ISCRATERS`, `MC2_GPUOVERLAY` flag semantics.** These drive the per-flag iteration arms in `renderLists`. F2 is queued to clean up legacy "terrain flag" terminology. | `mclib/txmmgr.h:65` (`MC2_GPUOVERLAY` def), `mclib/txmmgr.cpp:1094,1290,1399,1473` (consumers) | Wrong pass picks up wrong nodes → water rendered as solid terrain, decals double-drawn, etc. |
| C13 | **MapData heightmap structure.** `MapData::blocks` is an `realVerticesMapSide²`-array of `PostcompVertex` (40 bytes each). Map sizes are 60/80/100/120/side per `mclib/terrain.cpp:296–303`. CPU `terrainElevation()` was patched (commits d504713 etc, per memory `cpu_displacement_done.md`) to match GPU TES displacement so units don't float. | `mclib/vertex.h:32–63`, `mclib/dmapdata.cpp:174,208`, `mclib/terrain.cpp:1196–1199` | Units float above terrain; pathing and AI broken. Or fall through terrain. |
| C14 | **Pre-draw mission TGL-pool reset.** Mission render resets pools at end, not start. | `code/mission.cpp:798–802` | Pool-state corruption across frames if reordered. |

---

## §6 Existing modernization beachheads

### 6.1 GPU static-prop batcher

`GameOS/gameos/gos_static_prop_batcher.cpp` (file size suggests ~850+ lines). Active under RAlt+0 killswitch (currently OFF by default — see CLAUDE.md L51–55). Techniques used:

- **Persistent geometry buffers per type** uploaded once (`onMapLoad` at line 199, `finalizeGeometry` at line 355).
- **Per-instance data in SSBOs** (`s_instanceSsbo`, `s_colorSsbo`) bound via `glBindBufferRange` per type (line 767–772). No CPU memcpy of vertex data per frame.
- **Instanced indexed draw with base vertex** via `glDrawElementsInstancedBaseVertex` at line 803–809. One draw per (type × packet/material).
- **GL state save/restore discipline** — explicit `glGet`s at flush start (lines 692–712), explicit restores at end (lines 817–834). Per memory `static_prop_projection.md` this is required under GL 4.3 core because MLR/HUD downstream paths break with mismatched inherited state.
- **Texture handle resolved at draw time** (`type.source->listOfTextures[pkt.textureSlot].gosTextureHandle` at line 791–793) to handle MC2's live-mutation pattern (C10).
- **GL fence per frame slot** (`s_fence[s_frameSlot]` at line 813) for triple-buffered persistent SSBOs.
- **Direct uniform uploads, not deferred** (line 738 `glUniformMatrix4fv` directly, not via the `setMat4` cache). This matches C7's rule for direct GL paths.

Could terrain borrow? Conceptually yes — instanced patches with per-patch SSBO data and indexed draw is the same shape. The complications are (a) terrain has no natural "type" (every patch is unique), and (b) the cull-gate cascade C1 makes the input-set decision non-local. The static_prop batcher gets to assume "if it's registered, it's drawn"; terrain modernization can't.

### 6.2 GPU mode killswitch — what the previous attempt got right and wrong

`docs/gpu-static-prop-cull-lessons.md` is the canonical post-mortem. Summary of relevant lessons:

- **Right.** Recognized that GPU clipping can replace CPU angular cull when the per-vertex bool isn't load-bearing for non-cull purposes.
- **Right.** Persistent buffers with frame-slot fencing.
- **Right.** Save/restore all touched GL state.
- **Wrong.** Treated the cull bypass as a local fix — cascaded into prop disappearance via #1–#4 of the lessons doc.
- **Wrong (early attempts).** Used `clip.w < 0.1` as a behind-camera guard. Worked for some camera angles, failed at others (per `clip_w_sign_trap.md`). Settled on the explicit-w guard per `terrain_tes_projection.md`.

The **single biggest reusable lesson** is "the cull is load-bearing for object lifecycle, not just visibility — any modernization that changes who sets `objBlockActive`/`objVertexActive` must understand the prop iteration that consumes those flags."

### 6.3 Tessellation pipeline — already GPU-friendly?

`shaders/gos_terrain.tcs` / `.tese` accept patches via `glDrawElements(GL_PATCHES, ...)`. The CPU side feeds:
- A `gos_VERTEX` stream that the TES *ignores* (per P2/P3) — vestigial.
- A `gos_TERRAIN_EXTRA` stream containing worldPos + worldNorm — this is what TES uses.
- `terrainMVP`, `terrainViewport`, `cameraPos`, light dir, tess params as uniforms.

Is this "CPU-side patch submission already GPU-friendly, or just dressing"? **Mostly dressing.** The CPU work to produce the extras stream is per-triangle (in `fillTerrainExtra`), and it duplicates worldPos that the heightmap already implies. A "GPU-friendly" version would feed the heightmap directly (texture sample of elevation in the TES) and let the GPU compute worldPos. The current path is "GPU does the projection work, CPU still does the geometry sourcing."

### 6.4 Tracy zones — sufficient?

The terrain pipeline has zones at every level of detail needed to attribute cost (see §2.1 list). What's *missing* for a modernization decision:

- No GPU zone on `Terrain::geometry` (it's a pure CPU pass; this is correct).
- No per-batch GPU zone inside `terrainDrawIndexedPatches` (the whole call is one zone). Could add per-pass zones for shadow-bind, extras-upload, draw.
- No allocation-tracking zones (Tracy supports memory tracking but it's not wired in).

For brainstorming purposes the existing zones are sufficient to validate or refute the perf memory's claims after one Tracy capture.

---

## §8 Dependency graph

### 8.1 What blocks ModernTerrainSurface

| Dependency | Status | Why it blocks |
|---|---|---|
| **F3 (MRT completeness)** | DONE 2026-04-27 | Without F3's registry, ModernTerrainSurface would re-introduce the implicit-overload problem the registry just eliminated. |
| **projectZ Policy Split** | DONE | The 8 `projectFor*` wrappers are the load-bearing contract C9; ModernTerrainSurface inherits them. |
| **F1 (water/shoreline material-alpha overload)** | OPEN | Only blocks if Seam H or Seam M choose to migrate water rendering. Seam L doesn't care. F1 must land before water moves into ModernTerrainSurface. |
| **F2 (legacy "terrain flag" terminology cleanup)** | OPEN | Doesn't strictly block — F2 is rename-only. But ModernTerrainSurface inherits whatever vocabulary exists at landing. Worth landing F2 first or co-landing. |
| **Tracy baseline snapshot** (per §2.1) | PARTIAL | Operator-supplied Self-only Tracy screenshot shows `quadSetupTextures` ~1.17 ms/frame vs `vertexProjectLoop` ~0.41 ms/frame — enough to support Seam M as the first brainstorm candidate. Formal CSV/exported capture still desirable before final benchmark claims. |

### 8.2 What ModernTerrainSurface blocks

| Dependent | Why |
|---|---|
| **F8 (overlay/decal unification)** | Crater/decal blending against terrain depth depends on which side owns terrain depth. If ModernTerrainSurface owns depth, F8 must integrate against its API. |
| **F6 (native-modern sidecars)** | "Sidecar" implies parallel modern impls of legacy systems. ModernTerrainSurface is the canonical example; F6 generalizes the pattern. |
| **Removing `enableMRT`/`disableMRT`** (deferred from F3 Task 8) | Could be done independently, but Seam L is the natural home for the cleanup. |
| **GPU-driven cull / MDI** | If H or M lands, MDI is the next perf step. Seam L doesn't unblock MDI. |
| **Long-term shadow stability** | Static-shadow accumulation and dynamic-shadow direct-draw paths in `txmmgr.cpp:1184–1276` cohabit with terrain submission. Modernization that owns terrain submission can also rationalize shadow setup. |

### 8.3 In-flight features that could conflict

| Feature | Conflict risk |
|---|---|
| **Texture upscaling pipeline** (`upscale_gpu.py`, `art_4x_gpu/`, `tgl_4x_gpu/`) | LOW. Loose-file overrides happen at texture-load time, not at terrain submission. ModernTerrainSurface is texture-handle-agnostic. |
| **AssetScale subsystem** (`GameOS/gameos/asset_scale.{h,cpp}`) | LOW. AssetScale is concerned with UI atlas blits, not terrain. |
| **GPU grass pass** (`drawGrassPass` at `gameos_graphics.cpp:2847`) | MEDIUM. Grass shares the terrain mesh's VBO/IBO and re-derives uniforms. If ModernTerrainSurface re-shapes those buffers, grass needs to follow. Already shares `terrain_mat_normal_`, `terrain_mvp_`, `terrain_batch_extras_`. |
| **Terrain shadow accumulation** (`gos_BeginShadowPrePass` / `gos_DrawShadowBatchTessellated` at `txmmgr.cpp:1210, 1230`) | HIGH. Static-shadow pre-pass directly consumes the same `MC_VertexArrayNode::vertices`/`extras` that legacy terrain uses. ModernTerrainSurface either re-feeds the shadow pre-pass with the same data, or both paths get re-shaped together. |
| **Phase 4 effects** (cloud shadows, height fog, triplanar cliff) | LOW. Pure shader-side; data flow unchanged. |
| **Wolfman mode** | MEDIUM. Modernization must remain valid at 200²=40000 visible verts. The bigger CPU-side `vertexProjectLoop` is exactly the Wolfman pain point. |
| **Mod-content paths** (Magic, Carver5O, MCO Omnitech per `MEMORY.md`) | LOW for terrain pipeline directly — mods touch ABL, mech CSVs, art, not terrain admission. But the smoke matrix runs these missions; regressions here surface mod-related crashes that are unrelated. Confounder, not conflict. |

---

## §9 Risk register

Ordered by severity (highest first). Each names the failure mode, the canary, and the regression test that catches it.

| # | Failure mode | Canary | Regression test |
|---|---|---|---|
| R1 | **Mech shadows break / trees vanish** — cull-gate cascade C1 disturbed; `setObjBlockActive` no longer set per active vertex; props lose iteration. | Mechs invisible from frame 1, or appear after 5–10s as `inView` recovers; trees flicker. | `tests/smoke/run_smoke.py --tier tier1`; menu canary; the two missions `mc2_03` (mech-heavy) and `mc2_24` will catch it. Per-vertex `setObjBlockActive` count vs baseline. |
| R2 | **Terrain seams reappear** — `pz` gate C6 violated, behind-camera triangles rasterize as garbage, or matrix sign convention changes silently. | Giant single triangles spanning the screen; framerate collapse; or visible seams between adjacent quads. | `mc2_01` start frame visual A/B per F3 protocol. Any visible-pixel diff vs baseline blocks the commit. |
| R3 | **Wolfman zoom stutter or crash** — the 200²=40000 visible-vertex inner loop pessimized; or buffer-size assumption broken. | Wolfman-mode FPS drop >20% vs baseline; OR `[TGL_POOL v1]` summary shows pool exhaustion at high vertex count. | `MC2_HEARTBEAT=1`-instrumented Wolfman session for 60s. Compare FPS vs F3 closing baseline (46–53 FPS). |
| R4 | **AMD breakage** — GBuffer1 declaration drift, sampler array, attribute 0, or stale shader cache regression. | Black or single-color terrain; GL_INVALID_ENUM or GL_INVALID_OPERATION in `[GL_ERROR v1]`. | `docs/amd-driver-rules.md` hot-points checked; `MC2_GL_ERROR_DRAIN_SILENT=0` (default-on) catches first-frame errors. |
| R5 | **TGL pool exhaustion silent regression** — bypassing or restructuring terrain admission to feed more shapes than the 500K vertex pool budgets. | Random buildings/mechs vanish; per-mission, last-iterated objects affected first. | `MC2_TGL_POOL_TRACE=1` per-frame trace; the always-on 600-frame summary line. Compare baseline NULL count. |
| R6 | **Fresh install or modded install boot break** — change interacts with mission-load init order, or the asset_scale/manifest path; ABL or Magic content regression. | First-mission load crash, or `[ASSET_SCALE v1] event=manifest_missing` + downstream `oob_blit`. | Tier1 5/5 missions, plus a Magic install canary. `MC2_HEARTBEAT=1` to detect freezes. |
| R7 | **Mod-content compatibility regression** — Carver5O, MCO Omnitech, Magic interactions. ModernTerrainSurface should be data-format-agnostic for terrain (heightmap is engine, not mod) but flag-handling regressions could break mod missions specifically. | Mission load crashes only on modded missions; tier1 stock passes. | Carver5O feasibility worktree run after stock smoke passes. Per `MEMORY.md` carver5_mission_playable. |
| R8 | **Static shadow accumulation breaks** — terrain SOLID-flag node iteration (`txmmgr.cpp:1211–1232`) consumes data ModernTerrainSurface no longer produces in that shape. | Terrain shadows missing or stuttering when camera moves >100 units. | F3 protocol shadow-on/off A/B (RAlt+F3); compare reflectance against `mc2_01` baseline screenshot. |
| R9 | **GPU grass pass breaks** — grass shares terrain mesh; if extras VBO content shape changes, grass either crashes or renders mis-positioned. | Grass float-up, mis-located, or invisible. | RAlt+5 toggle on `mc2_01` visual A/B. |
| R10 | **Water rendering regression** — Seam H/M scope creep eats `gos_State_Water` plumbing; per-quad water UV wrapping breaks (per `water_rendering_architecture.md` UV seam rule). | Water appears solid black, or shows simplex-noise-like seams every quad. | `mc2_03` (water-heavy) visual A/B. |
| R11 | **F3 GBuffer1 contract violated** — modernized terrain producer writes `GBuffer1` directly instead of through `rc_gbuffer1_*`. | shadow_screen.frag mis-classifies pixels; double-shadowing or shadow on wrong surfaces. | `scripts/check-render-contract-gbuffer1.sh` grep census (run pre-commit). |
| R12 | **Stale shader cache mimicking regression** — per `stale_shader_cache_symptom.md`. False positive during bisect. | Frozen clouds or over-darkened terrain after deploy that bisects to a non-shader commit. | Force shader reload by clearing cache; A/B before reverting. |
| R13 | **Mod-content perf cliff** — modernization fast on stock content, slow on modded missions with non-standard map sizes. | Carver5O mission slower than baseline by >20%. | Carver5O 60s heartbeat run. |

---

## Open questions for the brainstorm

These are not findings — they are unanswered questions that the brainstorm needs to address.

- **Q1.** Is `Terrain::geometry vertexProjectLoop` actually the dominant CPU cost for current HEAD? (See P1 / §2.1.) **Partially answered — see [Tracy baseline snapshot](#tracy-baseline-snapshot--self-only-3060-frames).** The operator-supplied screenshot capture (~3,060 frames, Self-only) shows `vertexProjectLoop` at ~0.41 ms/frame vs `quadSetupTextures` at ~1.17 ms/frame — i.e., the build step is **not** the dominant CPU cost; the downstream submit/setup work is. A formal CSV-exported Tracy capture is still desirable for final benchmark claims, but the seam-selection question is no longer perf-blind.
- **Q2.** What fraction of CPU time is in the per-vertex `projectZ` math vs the per-quad `setupTextures`/`addTriangle` overhead vs the per-triangle `gos_VERTEX` pack? Determines whether Seam M is sufficient.
- **Q3.** Can the `clipInfo` cull cascade (C1) be inverted so that `objBlockActive`/`objVertexActive` are computed from a coarser-granularity test (per-block frustum-AABB instead of per-vertex angular cull)? If yes, Seam H becomes feasible without rewriting `objmgr.cpp`.
- **Q4.** Is the legacy `gos_VERTEX` pretransformed-screen-space stream actually consumed anywhere meaningful for terrain, or is it pure dead weight under tessellation? (See P2.) If dead, the cleanup is independent of ModernTerrainSurface.
- **Q5.** What does `terrainMVP` look like as a clean GL-convention projection matrix (closing the matrix-sign question per `terrain_tes_projection.md`)? If solvable, the C6 `pz` gate could be eliminated and Seam H becomes much friendlier.
- **Q6.** Should Seam-decision wait for F1 (water material-alpha) to land? Seam H is meaningfully larger in scope if water stays on legacy path.

---

---

# Pipeline reference (operator-requested deliverables)

The four sections below are operator-named deliverables for the brainstorming agent. They use exact greppable headings ("1. Current terrain render pipeline map", "2. Hot-path inventory", "3. Data authority map", "4. Seam candidates") and either promote or refactor content from §1–§9 above. Cross-links point back to the prose trace; each section also stands alone.

## 1. Current terrain render pipeline map

Linear pipeline, MC2 mission data → rasterized pixels. Each stage names entry-point file:line, input, output, and the function(s) responsible. Cross-links into §1–§4 above.

```
[mission load]
   MapData::init (mclib/dmapdata.cpp:174)
   Terrain::init  (mclib/terrain.cpp:219)
   Terrain::initMapCellArrays (mclib/terrain.cpp:229)
   Terrain::primeMissionTerrainCache (mclib/terrain.cpp:561)
        in:  PostcompVertex[] heightmap from PacketFile
        out: MapData::blocks[], MapData::terrainFaceCache[]
                |
                v
[per-frame: GameLogic phase — runs in mission->update() before UpdateRenderers]
  Stage A.  CPU admission — produces clipInfo + cull-active flags
   Terrain::geometry (mclib/terrain.cpp:943)
     A1. vertexProjectLoop (mclib/terrain.cpp:1002–1162)
         in:  Vertex* vertexList[visibleVerticesPerSide^2], camera matrices
         out: per-Vertex {clipInfo, px,py,pz,pw, hazeValue}, plus
              setObjBlockActive / setObjVertexActive side-effects
              (terrain.cpp:1131–1132) — load-bearing for prop iteration (C1)
              ENTRY of [PROJECTZ:BoolAdmission id=terrain_cpu_vert_admit]
              via eye->projectForTerrainAdmission (mclib/terrain.cpp:1099,
              dispatch at mclib/camera.h:525)
                |
                v
  Stage B.  CPU quad setup — per-quad UV/handle resolution + capacity reservation
     A2. quadSetupTextures loop (mclib/terrain.cpp:1169 calling :1127)
         per quad: TerrainQuad::setupTextures (mclib/quad.cpp:108
         declaration; mclib/quad.cpp:250–545 body)
         in:  Vertex::clipInfo, MapData::terrainFaceCache entry
         out: per-quad texture handles + UVs; reserves triangle capacity via
              MC_TextureManager::addTriangle (mclib/txmmgr.h:685)
                |
                v
[per-frame: Render phase — UpdateRenderers]
  Stage C.  CPU vertex/triangle pack — gos_VERTEX + gos_TERRAIN_EXTRA
   Camera.UpdateRenderers (gameosmain.cpp:464–470)
   GameCamera::render (code/gamecam.cpp:140)
     C1. land->render() → Terrain::render (mclib/terrain.cpp:860)
         per quad: TerrainQuad::draw (mclib/quad.cpp:1515)
                   TerrainQuad::drawWater (mclib/quad.cpp:2174)
                   TerrainQuad::drawMine  (mclib/quad.cpp:3586)
         in:  per-Vertex projected px/py/pz/pw + worldPos + clipInfo
         out: gos_VERTEX triples → MC_TextureManager::addVertices
              (mclib/txmmgr.h:861); per-triangle gos_TERRAIN_EXTRA
              (worldPos+worldNorm) → addTerrainExtra (mclib/txmmgr.h:1081)
              via fillTerrainExtra (mclib/quad.cpp:92, 1608)
              GATE: pz∈[0,1) cluster (mclib/quad.cpp:1597–1602 + 3 sisters)
                |
                v
  Stage D.  Texture-manager batching/sort — per-frame list building
   mcTextureManager->renderLists (mclib/txmmgr.cpp:902, body to ~1530)
         in:  masterVertexNodes[] / masterHardwareVertexNodes[] populated
              by addTriangle/addVertices (Stage B+C)
         out: ordered GL submissions, partitioned by phase
         phases (zone-tagged):
           Render.3DObjects        (txmmgr.cpp:1086–1146)
           Shadow.StaticAccum      (txmmgr.cpp:1184–1242)
           Shadow.DynPass          (txmmgr.cpp:1245–1276)
           Render.TerrainSolid     (txmmgr.cpp:1281–1343) ← terrain solid
           Render.GpuStaticProps   (txmmgr.cpp:1352–1358)
           Render.TerrainOverlays  (txmmgr.cpp:1367)
           Render.Decals           (txmmgr.cpp:1372)
           Render.Overlays (water) (txmmgr.cpp:1378–1456)
                |
                v
  Stage E.  GL submission — per-batch draw
   Inside Render.TerrainSolid:
     gos_SetTerrainBatchExtras (mclib/txmmgr.cpp:1309)
     gos_RenderIndexedArray    (mclib/txmmgr.cpp:1317)
        → gosRenderer::drawIndexedTris (gameos_graphics.cpp:2987)
        → gosRenderer::terrainDrawIndexedPatches
                                 (gameos_graphics.cpp:2677–2844)
         in:  pvertex_data_/pindex_data_ (gos_VERTEX), terrain_extra_vb_
              (worldPos/worldNorm), terrainMVP, material normals,
              shadow maps, ~25 uniforms
         out: glDrawElements(GL_PATCHES, ...) (gameos_graphics.cpp:2822)
                |
                v
  Stage F.  GPU pipeline — tessellation + shading
   gos_terrain.tcs / gos_terrain.tese (TES projects via terrainMVP,
        abs(clip.w) load-bearing per C5)
   gos_terrain.frag (POM, splatting, MRT GBuffer1 via render_contract.hglsl
        rc_gbuffer1_* helpers per C8)
        out: HDR color (attachment 0) + GBuffer1 normal/flag (attachment 1)
                |
                v
  Stage G.  Post-process — shadow_screen, bloom, etc. (out of terrain
            scope but consumes GBuffer1 produced in F)
```

Cross-references: stages A/A1 = §1.3, stage A2 = §1.4, stage C = §1.5, stage D = §1.6, stage E = §1.7, GPU pipeline shaders = §3 / §6.3.

## 2. Hot-path inventory

Per-frame terrain hot-path functions, ordered along the pipeline. Each row: function with file:line, what it owns, what calls it, what it calls, CPU cost shape.

| # | Function | File:line | Owns | Called from | Calls | CPU cost shape |
|---|---|---|---|---|---|---|
| H1 | `Terrain::geometry` | `mclib/terrain.cpp:943` (body 980–1193) | Per-frame admission of all visible terrain vertices + cull-active flag side-effects + per-quad setup launch | `mission->update()` (`code/mission.cpp:474–499` Tracy zones `GameLogic.Mission.Terrain` / `.TerrainGeometry`) | `vertexProjectLoop` body, `quadSetupTextures` loop, `cloudUpdate` | Per-frame-once outer; per-vertex inner (~14400 stock / 40000 Wolfman) |
| H2 | `Terrain::geometry vertexProjectLoop` | `mclib/terrain.cpp:1002–1162` (Tracy zone) | The per-vertex projection + angular cull + clipInfo write + setObjBlockActive/setObjVertexActive side-effects (load-bearing C1) | inline within `Terrain::geometry` | `Camera::cameraFrame.trans_to_frame`, `eye->projectForTerrainAdmission` (`mclib/camera.h:525`, dispatches `projectZ` `mclib/camera.h:435–470`), `setObjBlockActive` (`mclib/terrain.cpp:1284`), `setObjVertexActive` | **Per-vertex** (O(visibleVerticesPerSide²); 800k–1.2M FP ops/frame at Wolfman) |
| H3 | `Terrain::geometry quadSetupTextures` (loop body) | `mclib/terrain.cpp:1169` calling `mclib/terrain.cpp:1127` (`currentQuad->setupTextures()`) | Iteration that drives per-quad texture setup; hot-path equivalent to operator-named "quadSetupTextures" lives one level down at H4 | inline within `Terrain::geometry` | `TerrainQuad::setupTextures` per visible quad | **Per-quad** (numberQuads) |
| H4 | `TerrainQuad::setupTextures` (operator-named "quadSetupTextures") | declaration `mclib/quad.h:130`; body `mclib/quad.cpp:108` (this is a wrapper) and the heavy work block at `mclib/quad.cpp:250–545`; cache hit at `mclib/quad.cpp:433` (`TerrainQuad::setupTextures cachedVisibleSubmission` zone) | Per-quad UV/handle resolution + capacity-reservation `addTriangle` calls for solid+detail+overlay+mine triangles | `Terrain::geometry` H3 | `MapData::ensureTerrainFaceCacheEntryResident` (`mclib/quad.cpp:434`), `MC_TextureManager::addTriangle` (`mclib/txmmgr.h:685`) ×4–8 per quad | **Per-quad** with hot/cold cache split (cache hit ≈ O(1) per quad; miss heavier) |
| H5 | `Terrain::render` (operator-named "render terrain") | `mclib/terrain.cpp:860–915` | Per-frame iteration over `quadList[0..numberQuads]`, calling `currentQuad->draw()` then `currentQuad->drawMine()` to emit gos_VERTEX + gos_TERRAIN_EXTRA into texture-manager rings | `GameCamera::render` step 3 (`code/gamecam.cpp:199`, "land->render()") | `TerrainQuad::draw` (`mclib/quad.cpp:1515`), `TerrainQuad::drawMine` (`mclib/quad.cpp:3586`); separately `Terrain::renderWater` (`mclib/terrain.cpp:916`) is a sister entry called from `code/gamecam.cpp:215` (step 6) | **Per-quad outer; per-triangle inner** (~80k triangle packs/frame at Wolfman; ~13MB memcpy traffic) |
| H6 | `TerrainQuad::draw` | `mclib/quad.cpp:1515–2173` | Per-triangle gos_VERTEX pack (32B×3) + gos_TERRAIN_EXTRA pack (24B×3); enforces the **`pz` gate** (`mclib/quad.cpp:1597–1602`, load-bearing C6) that drops behind-camera triangles | `Terrain::render` H5 (per visible quad) | `MC_TextureManager::addVertices` (`mclib/txmmgr.h:861` body), `MC_TextureManager::addTerrainExtra` via `fillTerrainExtra` (`mclib/quad.cpp:92, 1608`; body at `mclib/txmmgr.h:1081`) | **Per-triangle** (~80k tri/frame Wolfman; bandwidth-bound memcpy) |
| H7 | `MC_TextureManager::renderLists` (operator-named "textureManager render lists") | `mclib/txmmgr.cpp:902` (body extends past 1530) | Per-frame batched GL submission: state setup, per-flag iteration over `masterVertexNodes[]`/`masterHardwareVertexNodes[]`, partitioned into Tracy phase zones (Render.3DObjects, Shadow.StaticAccum, Shadow.DynPass, Render.TerrainSolid, Render.GpuStaticProps, Render.TerrainOverlays, Render.Decals, Render.Overlays). Note the `renderLists` outer doc said `:995` — the actual function definition starts at `:902`; body content cited in §1.6 (`Render.3DObjects` 1086, `Render.TerrainSolid` 1281, etc.) is correct. | `GameCamera::render` step 10 (`code/gamecam.cpp:242`) — single per-frame call | `gos_DrawShadowBatchTessellated`, `gos_DrawShadowObjectBatch`, `gos_SetTerrainBatchExtras` (`mclib/txmmgr.cpp:1309`), `gos_RenderIndexedArray` (`:1317`), `gos_DrawTerrainOverlays`, `gos_DrawDecals`, `GpuStaticPropBatcher::flush` | **Per-frame-once outer**; **per-batch inner** (~6–10 batches × `terrainDrawIndexedPatches`) |
| H8 | `Render.TerrainSolid` (operator-named "Render.TerrainSolid") | Tracy zone bracket `mclib/txmmgr.cpp:1279–1340` (zone start `1279`, end `1340`), body `1281–1343` | The terrain-specific phase of renderLists: per-node GL state, extras upload, indexed patch draw | inline within `MC_TextureManager::renderLists` H7 | `gos_SetTerrainBatchExtras` (`mclib/txmmgr.cpp:1309`), `gos_RenderIndexedArray` (`:1317`) → `gosRenderer::drawIndexedTris` → `gosRenderer::terrainDrawIndexedPatches` (`GameOS/gameos/gameos_graphics.cpp:2677–2844`, dispatch `:2987–2998`) | **Per-batch** (one node iteration); per-batch GL upload ≈ 1.9MB VBO + 120KB IBO + 0.5–2MB extras |

Note on naming: the operator brief listed "render terrain" and "Render.TerrainSolid" as distinct entry points. They are: `Terrain::render` (`mclib/terrain.cpp:860`) is the **CPU per-quad submit-build loop** that runs inside the GameCamera render order at step 3 (`code/gamecam.cpp:199`); `Render.TerrainSolid` (`mclib/txmmgr.cpp:1279–1340`) is the **GL flush phase** that runs at step 10 inside `renderLists`. These are temporally separated within one frame; H5 produces the data, H8 submits it.

## 3. Data authority map

Rows = data classes the terrain system manages. Columns: owner (file/class), reader callsites, writer callsites, lifecycle, cross-system dependency.

| Data class | Owner | Read at (callsites) | Written at (mutators) | Lifecycle | Cross-system dependency |
|---|---|---|---|---|---|
| **height (heightmap)** | `MapData::blocks` (`mclib/dmapdata.cpp:174`); type `PostcompVertex` (`mclib/vertex.h:32–63`) | CPU: `MapData::terrainElevation` (`mclib/terrain.cpp:1196`), `Terrain::geometry` per-vertex world-pos seed (`mclib/terrain.cpp:1003+`), `MapData::primeMissionTerrainCache` (`mclib/terrain.cpp:561–577`). GPU: indirectly via `gos_TERRAIN_EXTRA worldPos` produced by `fillTerrainExtra` (`mclib/quad.cpp:92, 1608`); never sampled as a heightmap texture. | `MapData::init` from PacketFile (`mclib/dmapdata.cpp:174` allocate; `mclib/terrain.cpp:295` packet load); `Terrain::setVertexHeight` (`mclib/terrain.cpp:844`) for runtime edits | **Per-mission-load** (immutable post-load except via `setVertexHeight`) | `pathing` reads via `MapData::terrainElevation` for mover ground-clamp; CPU/GPU agreement enforced by `cpu_displacement_done.md` (C13). Mod content ships heightmap as part of mission archive — not a hook point. |
| **materials (terrain texture indices)** | `MapData::terrainFaceCache` (`WorldQuadTerrainCacheEntry`, `mclib/mapdata.h:69–122`); per-vertex tile/tex indices in `PostcompVertex` (`mclib/vertex.h:32–63`) | `TerrainQuad::setupTextures` resolves per-quad handles (`mclib/quad.cpp:250–545`); cache hit at `mclib/quad.cpp:433` via `MapData::ensureTerrainFaceCacheEntryResident` (`mclib/quad.cpp:434`) | `Terrain::primeMissionTerrainCache` warms cache at mission load (`mclib/terrain.cpp:561–577`); `Terrain::setTerrain` runtime (`mclib/terrain.cpp:814`) | **Per-mission-load** (cached); resolved/refreshed on demand per quad. `mc2_texture_handle_is_live.md` (C10): texture *handle* slot mutates per-frame in `TransformMultiShape` — never cache the handle, only the slot index. | Asset upscaling overlay (memory `art_4x_gpu/`, `tgl_4x_gpu/`) replaces texture content at load-time but does not change indices/handles. AssetScale subsystem is unrelated. |
| **overlays (cement, scorch, footprint, etc.)** | `Terrain::setOverlay` / `getOverlay` (`mclib/terrain.cpp:808, 838`); `Terrain::setOverlayTile` (`mclib/terrain.cpp:802`); type enum `Overlays` | `TerrainQuad::setupTextures` reads overlay tile to issue `addTriangle` for `MC2_DRAWALPHA` triangles; flushed in `Render.TerrainOverlays` (`mclib/txmmgr.cpp:1367`) | `Terrain::setOverlay`/`setOverlayTile` runtime; mission-load reads from `_terrain.pak` packet | **Per-mission-load** authored, **runtime-mutable** during gameplay (e.g. footprints from movement) | Decals share batching machinery (`gos_DrawDecals`, `mclib/txmmgr.cpp:1372`) but are a distinct data class — see next row. |
| **decals (craterManager craters/footprints)** | `craterManager` (singleton); not in `MapData` | `craterManager->render()` at `code/gamecam.cpp:205` (step 4 of GameCamera::render); `Render.Decals` zone (`mclib/txmmgr.cpp:1372`) | Runtime — explosions, mech footprints | **Per-frame mutable** (LRU; bounded count) | Distinct from `overlays` above; both flow into `Render.TerrainOverlays`/`Render.Decals` phases of renderLists. Blends against terrain depth — Seam-H scope concern (would have to coexist with modern depth output). |
| **water** | `Terrain::calcWater` (`mclib/terrain.cpp:826`); per-quad water flag emitted in `TerrainQuad::draw` (`mclib/quad.cpp:860–863`) and water-specific `TerrainQuad::drawWater` (`mclib/quad.cpp:2174`) | Read in `Render.Overlays` (`mclib/txmmgr.cpp:1378–1456`) on `MC2_ISWATER`/`MC2_ISWATERDETAIL` flags; routes through `gos_State_Water` 1-or-2 (`txmmgr.cpp:1416–1418`) into `gos_tex_vertex.frag` (`isWater` uniform) | `Terrain::renderWater` (`mclib/terrain.cpp:916`) at `code/gamecam.cpp:215` (step 6) — per-frame submit | **Per-frame submit-rebuilt**, mission-load configured (depth thresholds in `calcWater`) | NOT in the terrain splatting/tessellation path — water is a separate `gos_tex_vertex` overlay (memory `water_rendering_architecture.md`). Blocks Seam H/M scope unless F1 (water/shoreline material-alpha overload, OPEN per §8.1) lands first. Terrain frag also writes a 0.25 water-alpha flag on GBuffer1 for shoreline post-process (memory `MEMORY.md` "Shorelines"). |
| **visibility (LOS / fog-of-war / cull)** | `Terrain::markSeen` / `markRadiusSeen` (`mclib/terrain.cpp:1189, 1245`); `Vertex::clipInfo` (`mclib/vertex.h:67+`); per-block `objBlockInfo[]`, `objVertexActive[]` arrays | `Terrain::geometry` reads camera state to compute angular cull + projectZ admission → `clipInfo` write; `setObjBlockActive`/`setObjVertexActive` consumed by `GameObjectManager::update`/`render` (`code/objmgr.cpp:1731, 1486` per memory `cull_gates_are_load_bearing.md`) | `Terrain::geometry vertexProjectLoop` writes `clipInfo` (`mclib/terrain.cpp:1124`) and active-flag side effects (`:1131–1132`); `Terrain::setObjBlockActive` (`:1284`) | **Per-frame-rebuilt** (every frame, every visible vertex) | **C1 load-bearing.** Drives prop iteration; pathing AI does not consume directly but mech `update()` is gated by it. Memory: `cull_gates_are_load_bearing.md`, `tgl_pool_exhaustion_is_silent.md` (silent failure mode if cascade broken). |
| **selection (player tile selection)** | Not present in the terrain pipeline. MC2 selects **objects** (mechs/buildings), not tiles. Region-pick ("LMB drag rectangle") tests against object world-positions, not the heightmap. | n/a in terrain | n/a in terrain | n/a | Confirms Seam H/M does not need to expose tile-selection API — there is no consumer. |
| **pathing (mover pathfinding)** | `MoveMap` / `MoveLevel` (engine pathing); not located in `mclib/terrain.cpp`. Consumes heightmap via `MapData::terrainElevation` (`mclib/terrain.cpp:1196`) for ground clamp. | Pathing system reads `MapData::terrainElevation` and (per `MEMORY.md` Phase 4) cost-grid that mirrors map cells | Pathing writes are mission-load only (cost grid populated from heightmap + terrain types) | **Per-mission-load** for grid; **per-frame** consumption | Pathing reads heightmap that is mutated only by mission load + `setVertexHeight`. ModernTerrainSurface must continue exposing `terrainElevation()` (or whatever computes elevation that matches GPU TES displacement, per memory `cpu_displacement_done.md`). |
| **custom campaign compatibility (Magic, Carver5O, MCO Omnitech)** | None of the three mods alter terrain admission code. They ship: art (loose-file overrides via `data/art/`/`data/tgl/`), mission archives (heightmap + texture indices in standard MC2 packet format), ABL/CSV mech data. Per `MEMORY.md` `magic_abl_contamination_rule.md`, `mc2x_integration_attempt.md`, `mco_omnitech_integration_attempt.md`. | All three flow through standard `MapData::init` packet load — no special hook into terrain code. Magic's pitfall is `code/mechicon.cpp` icon scrambling (`worktree CLAUDE.md` section "Do Not Upscale These Art Assets"), not terrain. | n/a — mods do not write into terrain code | **Per-mission-load** (mod content baked into mission packets) | LOW conflict risk for terrain pipeline directly (§8.3 row); modded missions are confounders for smoke regressions but do not constitute a separate code path. ModernTerrainSurface remains data-format-agnostic for terrain so long as it consumes `PostcompVertex[]`-shaped heightmap and standard texture-index map. |

Cross-system summary derived from above: pathing reads heightmap mutated by mission-load only (and runtime `setVertexHeight`); visibility flags are read by prop iteration, mutated by `Terrain::geometry` per-frame (the cause-chain root C1); water alpha flag is written by `gos_terrain.frag` (MRT_ENABLED guard) and read by post-process; decals/overlays blend against terrain depth and force scope concerns in Seam H. Selection is not a terrain data class.

## 4. Seam candidates

Three named seams (H / M / L) for ModernTerrainSurface. Each lists what stays legacy vs becomes modern, what evidence makes it plausible, what makes it hostile, and a risk register specific to that seam. **No final recommendation.** Absorbs and supersedes §7.

The load-bearing constraint threaded through these seams (operator-required):

> **CPU `projectZ` admission can stay put.** The cull-gate cascade (C1 in §5) is load-bearing through `setObjBlockActive`/`setObjVertexActive` side-effects in `Terrain::geometry vertexProjectLoop` (`mclib/terrain.cpp:1131–1132`), and the 8 `projectFor*` wrappers in `mclib/camera.h:525–610` are the load-bearing contract C9. But the per-frame CPU-built `gos_VERTEX` stream (Stages C/D in §"1. Current terrain render pipeline map") is **largely vestigial under tessellation** — see premise-finding P2 above. That points at a class of seam where projectZ admission stays conservative (proven, coupled to cull machinery) while the modernization target is everything *after* admission: the quad/vertex/texture/batch setup that feeds the shader. Seam M (below) is explicitly this shape; seams H and L describe what they would have to do about projectZ admission, so the trade-off is visible.

### 4.H — Seam H (data-shaped, high)

**What stays legacy:**
- Mission-load packet parsing (`MapData::init`, `mclib/dmapdata.cpp:174`).
- TGL pool / non-terrain GameObjectManager.
- Mech/building rendering through MLR.
- Crater/decal/water *as separate systems* (Seam H either swallows water/decals — large scope — or leaves them legacy and must coexist with their depth assumptions; flagged hostile).

**What becomes modern (on the other side of the seam):**
- All of `Terrain::geometry` (admission + quad setup) — replaced by GPU-driven cull or per-block frustum-AABB CPU test.
- All of `Terrain::render`/`TerrainQuad::draw` — replaced by GPU heightmap sampling (TES samples elevation directly).
- All of `Render.TerrainSolid` and `terrainDrawIndexedPatches` — replaced by ModernTerrainSurface's own GL submission.
- The `gos_TERRAIN_EXTRA` per-triangle worldPos+worldNorm pack disappears (heightmap-implied data replaces it).

**Disposition of projectZ admission (operator-required tradeoff):** **REPLACED.** Seam H is the most aggressive about projectZ. It must either (a) replace the per-vertex `clipInfo` write with a coarser-granularity per-block frustum-AABB test that still drives `setObjBlockActive`/`setObjVertexActive` (preserves C1 cascade at lower granularity — open question Q3 in §"Open questions"), or (b) rewrite the prop iteration in `objmgr.cpp` to no longer depend on terrain-driven cull flags (multiplicatively larger scope). Either route carries C9 wrapper-policy implications because the 8 `projectFor*` wrappers must continue to be the only callsites for per-policy projection; ModernTerrainSurface is allowed to stop *invoking* `projectForTerrainAdmission`, but no other site may reach `projectZ` directly.

**Evidence that makes it plausible:**
- `MapData::blocks` is allocated per-mission and never resized post-load (`mclib/dmapdata.cpp:174`).
- The static prop batcher already exemplifies "registered once at map load, drawn from GPU resident data" (§6.1; `gos_static_prop_batcher.cpp:199, 355`).
- `Terrain::primeMissionTerrainCache` is already a mission-load CPU pass producing residency-friendly data (`mclib/terrain.cpp:561`).
- F3's render contract registry already provides the `rc_gbuffer1_*` typed seam (C8) ModernTerrainSurface inherits.
- The TES already does projection from worldPos (memory `terrain_tes_projection.md`); going one step further to sample heightmap in the TES is incremental shader work.

**Evidence that makes it hostile:**
- **C1 cull cascade.** `Terrain::geometry`'s per-vertex `setObjBlockActive`/`setObjVertexActive` side effects (`mclib/terrain.cpp:1131–1132`) are the cause-chain root for prop iteration in `GameObjectManager` — see `cull_gates_are_load_bearing.md`. Replacing terrain admission either preserves the cascade in coarser form (open Q3) or rewrites prop iteration (huge scope).
- **C6 `pz` gate.** Currently CPU-side pre-rejection at `mclib/quad.cpp:1597–1602` (+ 3 sister sites). ModernTerrainSurface must either replicate the gate against its own internal vertex form, or solve the matrix-sign convention question that `terrain_tes_projection.md` left open. The TES has no clip-space signal that distinguishes front-of-camera from behind-camera verts, so dropping the gate is hazardous.
- **Water + decals scope.** `MC2_ISWATER`/`MC2_ISWATERDETAIL` rasterization is interleaved into `renderLists` flush phases (§1.6 step "Render.Overlays"). Seam H either swallows water (forces F1 to land first per §8.1) or coexists with legacy water depth assumptions.
- **Static + dynamic shadow accum.** `txmmgr.cpp:1184–1276` consumes the same `MC_VertexArrayNode::vertices`/`extras` legacy terrain produces. ModernTerrainSurface either re-feeds shadow with the same shape or both paths get re-shaped together (§8.3 row, HIGH conflict).
- **Q5 matrix question.** Closing `terrainMVP` as a clean GL-convention matrix (per `terrain_tes_projection.md`) is needed to make Seam H's TES-side admission viable.

**Risk register specific to Seam H:**
- HR1: Mech shadows break / buildings vanish (R1 from §9, amplified — Seam H is most likely to disturb C1 cascade).
- HR2: Behind-camera triangles rasterize as garbage (R2 from §9, amplified — Seam H most likely to drop C6 gate).
- HR3: Water pop or shoreline wrap regression (R10 from §9, amplified — Seam H forces water scope).
- HR4: Static shadow-accum mismatch (R8 from §9, amplified — Seam H reshapes the data shadow consumes).
- HR5: Wolfman zoom break (R3 from §9 — coarser cull might over- or under-include at extreme zoom).

### 4.M — Seam M (patch-shaped, mid) — *projectZ admission stays put*

> [Tracy baseline snapshot](#tracy-baseline-snapshot--self-only-3060-frames) empirically supports this seam's prioritization: quad setup ~1.17 ms/frame vs admission ~0.41 ms/frame, ~3× headroom on the downstream side that Seam M targets.

**What stays legacy:**
- All of `Terrain::geometry` including `vertexProjectLoop` (`mclib/terrain.cpp:1002–1162`) — projectZ admission unchanged, `clipInfo` written, `setObjBlockActive`/`setObjVertexActive` cascade intact.
- The 8 `projectFor*` wrappers in `mclib/camera.h:525–610` (C9) — unchanged.
- The `pz` gate at `mclib/quad.cpp:1597–1602` + sisters (C6) — unchanged.
- Crater/decal overlays, water rendering, GameObjectManager — unchanged.

**What becomes modern (on the other side of the seam):**
- `TerrainQuad::setupTextures` capacity reservation (`mclib/quad.cpp:250–545`) — replaced by direct per-patch SSBO write.
- `TerrainQuad::draw` per-triangle gos_VERTEX pack (`mclib/quad.cpp:1515–2173`) — replaced; the vestigial `gos_VERTEX` stream is **dropped**, since under tessellation the TES does not consume `pz/pw/px/py` (premise-finding P2). The per-quad `clipInfo` still drives a "patch is visible" boolean fed to the new submission API.
- The `gos_TERRAIN_EXTRA` per-triangle worldPos+worldNorm stream — kept (TES still uses it), but moved from per-frame upload to a persistent SSBO with per-patch indexing.
- `Render.TerrainSolid` GL submission (`mclib/txmmgr.cpp:1281–1343`) — replaced by ModernTerrainSurface API; one or a few large draws.
- `terrainDrawIndexedPatches` body (`gameos_graphics.cpp:2677–2844`) — replaced.

**Disposition of projectZ admission:** **PRESERVED.** This is the explicit operator-required shape: CPU admission stays conservative and coupled to the cull cascade; modernization targets only the *downstream* CPU work (quad setup, vertex pack, batch submission) that feeds the shader. The per-vertex `clipInfo` continues to be the gate; only its *consumer* changes (a ModernTerrainSurface patch-submission API instead of the texture-manager-node ring).

**Evidence that makes it plausible:**
- This is exactly what `terrainDrawIndexedPatches` already does (§1.7) but with cleaner inputs. The function is already registry-tagged at `gameos_graphics.cpp:2995`.
- The CPU outputs (`Vertex::clipInfo`, per-quad `terrainHandle`, `gos_TERRAIN_EXTRA`) already have the right shape; only the data flow changes (direct to SSBO instead of via `MC_VertexArrayNode` rings).
- The static-prop batcher proves the persistent-SSBO + instanced-draw pattern works in this codebase (§6.1, `gos_static_prop_batcher.cpp:664–838`).
- Premise-finding P2 explicitly identifies the `gos_VERTEX` stream as vestigial under tessellation — Seam M is the realization of that finding.
- F3's `rc_gbuffer1_*` registry trivially carries through (C8, single shader-side change).

**Evidence that makes it hostile:**
- **Doesn't address the CPU bottleneck if it lives in `vertexProjectLoop` (§1.3).** Seam M only modernizes the *submit* step, not the *build* step. The Tracy baseline snapshot shows the projection loop (~0.41 ms/frame) is materially smaller than quad setup (~1.17 ms/frame), so this Seam M concern has empirical support against it being an issue today; a formal exported capture remains desirable before final claims.
- **Per-batch GL upload churn (~13MB/frame).** Seam M can clean this up via persistent buffers, but the C7 deferred-vs-direct uniform discipline must be honored in the same way the static-prop batcher handles it (`gameos_graphics.cpp:738`-style direct uploads).
- **Static shadow accum + grass pass share the data.** `Shadow.StaticAccum` (`txmmgr.cpp:1184–1242`) and `drawGrassPass` (`gameos_graphics.cpp:2847`) both consume the same VBOs. Seam M must re-feed both — adds engineering scope (§8.3 rows, HIGH and MEDIUM conflict respectively).
- **Cache hit path.** `MapData::ensureTerrainFaceCacheEntryResident` (`mclib/quad.cpp:434`) is a CPU residency optimization tightly coupled to `setupTextures`. Seam M either keeps this CPU path (and just retargets its output) or migrates the cache to GPU (additional scope).

**Risk register specific to Seam M:**
- MR1: Per-batch GL state save/restore drift (R4-class AMD breakage if SSBO bind state leaks; mitigated by static-prop batcher pattern at `gos_static_prop_batcher.cpp:692–712, 817–834`).
- MR2: Static shadow accum mismatch if shadow path isn't co-migrated (R8 from §9).
- MR3: Grass pass break if extras-VBO shape changes (R9 from §9).
- MR4: F3 GBuffer1 contract regression if direct GL submission bypasses `rc_gbuffer1_*` helpers (R11 from §9).
- MR5: Insufficient perf gain if bottleneck is `vertexProjectLoop` not submit (open Q1, Q2).
- *Notably absent:* HR1 / HR2 (cull cascade and pz gate stay put — Seam M's defining property).

### 4.L — Seam L (draw-shaped, low)

**What stays legacy:**
- Everything from frame entry through the texture-manager batching/sort. CPU admission, quad setup, vertex pack, per-flag batching — all unchanged.
- `MC_TextureManager::renderLists` (`mclib/txmmgr.cpp:902`+) outer iteration unchanged.
- All §5 constraints transparent (none touch this layer).

**What becomes modern:**
- The body of `terrainDrawIndexedPatches` (`gameos_graphics.cpp:2677–2844`) — refactored to take its inputs as parameters instead of from the texture manager. ModernTerrainSurface owns just the GL submission.
- A natural home for the `enableMRT`/`disableMRT` removal that was deferred from F3 Task 8.

**Disposition of projectZ admission:** **PRESERVED, irrelevant to seam.** Seam L doesn't reach high enough in the pipeline to interact with admission. The seam exists below `Render.TerrainSolid`'s outer iteration; admission is many call layers above.

**Evidence that makes it plausible:**
- Approximately a refactor of the existing function. Almost zero scope.
- The static-prop batcher's `flush()` is comparable size and shape (§6.1, `gos_static_prop_batcher.cpp:664–838`).
- F3 contract preserved by construction (single function changes).

**Evidence that makes it hostile:**
- **Does not touch the CPU bottleneck.** The 3.66 ms claim (per the Tracy baseline snapshot, partially verified — see P1) is in `Terrain::geometry`, not in `terrainDrawIndexedPatches`.
- **Provides no leverage for "fewer, larger draw calls."** The 11–15% GPU util claim implies submission consolidation, but Seam L only refactors *one* call's body, not the per-batch fan-out.
- **Likely a no-op for the brief's stated goal** unless used as containment / cleanup en route to Seams M or H.

**Risk register specific to Seam L:**
- LR1: Stale shader cache mimicking regression during deploy (R12 from §9; same severity as any shader-touching change).
- LR2: AMD GL state regression (R4 from §9; same severity as any GL-state change). Mitigated by direct refactor scope.
- LR3: Wasted effort if Seam M or H is the actual goal (process risk, not technical).
- LR4: F3 GBuffer1 contract regression (R11 from §9; mitigated by Seam L not touching the shader output, only the GL state surrounding the draw).
- *Notably absent:* HR1 / HR2 / MR1 / MR5 — Seam L is too low in the stack to be reached by any cascade hazard.

### Seam comparison summary (no recommendation)

| Axis | Seam H (data) | Seam M (patch) | Seam L (draw) |
|---|---|---|---|
| projectZ admission | Replaced (forces Q3 + Q5) | **Preserved (defining property)** | Preserved (irrelevant) |
| Cull cascade C1 | Hostile — needs rework | Friendly — preserved | Friendly — preserved |
| `pz` gate C6 | Hostile — solves matrix Q5 | Friendly — preserved | Friendly — preserved |
| Wrapper contract C9 | Drops `projectForTerrainAdmission` invocation; other wrappers untouched | Untouched | Untouched |
| Addresses 3.66ms CPU build claim if real | Yes | Partially (only submit, not build) | No |
| Addresses ~13MB/frame submit churn | Yes (subsumed) | Yes (persistent SSBOs) | Partial (one batch path) |
| Touches water/decal scope | Yes (forced) | No | No |
| Estimated commit count | many (10s) | 5–8 | 2–3 |
| Risk of regression | High | Low–medium | Very low |
| Leverages F3 registry | Yes | Yes | Yes |

Brainstorm decision input: pick the seam that matches the measured CPU bottleneck — the Tracy baseline snapshot already gives a directional answer favoring quad/setup/submission over the projection loop, and a formal exported capture would tighten the numbers (open Q2). If the bottleneck is `vertexProjectLoop` (build), only Seam H gets at it directly. If it's `quadSetupTextures` + `addTriangle`/`addVertices` chain (submit), Seam M is sufficient and projectZ admission stays put — which is the operator-flagged "core performance insight from the F1–F3 arc."

---

## Tracy baseline snapshot — Self only, ~3,060 frames

This is operator-supplied Tracy data, **manually extracted from a screenshot** (not CSV export), **Self-time only** — so parent rows do NOT include child-zone time. Approximately 3,060 frames captured. Avg/frame is computed as `total / 3060`, **not** Tracy's MTPC (which is per-call mean, not per-frame mean).

### Table 1 — Terrain / submission relevant zones

| Zone | Total time | Calls | MTPC | Approx avg/frame | Why it matters |
|---|---|---|---|---|---|
| Terrain::geometry quadSetupTextures | 3.58 s | 3,060 | 1.17 ms | 1.17 ms | Major CPU terrain setup target. |
| GameCamera::render terrain | 3.45 s | 3,060 | 1.13 ms | 1.13 ms | Render-side terrain submission wrapper/self time. |
| GameCamera::render water | 3.07 s | 3,060 | 1.00 ms | 1.00 ms | Water/overlay path is significant; do not ignore. |
| Terrain.DrawPatches | 2.54 s | 858,477 | 2.96 µs | 0.83 ms | GPU patch draw path / per-batch submission cost. |
| TerrainQuad::setupTextures resolveFallback | 1.65 s | 22,094,306 | 74 ns | 0.54 ms | Per-quad texture/UV fallback resolution. |
| MC_TextureNode::get_gosTextureHandle | 1.40 s | 96,536,216 | 14 ns | 0.46 ms | Huge call volume; texture handle resolution hot path. |
| Terrain::geometry vertexProjectLoop | 1.27 s | 3,060 | 413.94 µs | 0.41 ms | CPU admission/projectZ cost. Smaller than quad setup here. |
| Terrain.TessDraw | 567.81 ms | 858,477 | 661 ns | 0.19 ms | Tess draw wrapper; not the main CPU issue. |
| TerrainColorMap::getTextureHandle realizeTexture | 445.94 ms | 15,701,723 | 28 ns | 0.15 ms | Texture handle realization. |
| MapData::makeLists vertices | 414.62 ms | 3,059 | 135.54 µs | 0.14 ms | Map list construction. |
| TerrainQuad::setupTextures cachedVisibleSubmission | 358.84 ms | 16,168,244 | 21 ns | 0.12 ms | Cache hit path; much cheaper than fallback but high volume. |
| MapData::makeLists quads | 316.18 ms | 3,060 | 103.33 µs | 0.10 ms | Quad list setup. |
| Render.TerrainOverlays | 119.73 ms | 3,059 | 39.14 µs | 0.04 ms | Overlay render pass is low self time here. |
| Render.TerrainSolid | 67.19 ms | 3,059 | 21.96 µs | 0.02 ms | Solid terrain flush self time is low; children elsewhere dominate. |
| Render.Decals | 2.18 ms | 3,059 | 712 ns | ~0.001 ms | Decal render pass self time is negligible in this capture. |

### Table 2 — Other relevant frame costs

| Zone | Total time | Calls | MTPC | Approx avg/frame | Note |
|---|---|---|---|---|---|
| Frame.FrameCap | 7.29 s | 3,059 | 2.38 ms | 2.38 ms | Artificial cap/wait; exclude from optimization target. |
| TerrainObject::update appearanceUpdate | 3.20 s | 1,764,624 | 1.81 µs | 1.05 ms | Terrain object update cost, separate from terrain surface submission. |
| ApplyRenderStates | 787.04 ms | 1,331,277 | 591 ns | 0.26 ms | High call volume render-state overhead. |
| BasicDraw.Indexed | 745.52 ms | 121,827 | 6.12 µs | 0.24 ms | General indexed draw path. |
| Shadow.DynObjectDirect | 746.04 ms | 683,044 | 1.09 µs | 0.24 ms | Dynamic object shadow path. |
| GameCamera::render objects | 692.41 ms | 3,060 | 226.28 µs | 0.23 ms | Object render self time. |
| GameLogic.Mech3D.UpdateGeometry | 669.69 ms | 14,066 | 47.61 µs | 0.22 ms | Mech geometry update. |
| GameLogic.Units.TerrainObjects | 523.86 ms | 3,060 | 171.2 µs | 0.17 ms | Terrain object game-logic cost. |

### Caveats

- Tracy view is Self only; parent rows do not include child-zone time.
- Values are manually extracted from screenshot, not exported CSV.
- Avg/frame assumes ~3,060 captured frames.
- This is sufficient for seam selection, not for final benchmark claims.

### Interpretation

This capture supports **Seam M** as the first serious candidate. The largest terrain-surface costs are quad setup (1.17 ms), terrain/water submission (1.13 + 1.00 ms), patch drawing (0.83 ms), texture-handle lookup (0.46 ms), and `setupTextures` fallback (0.54 ms). The `projectZ`/admission loop is visible but smaller (~0.41 ms/frame) than quad setup (~1.17 ms/frame) — roughly a 3× headroom on the downstream side. The first modernization should preserve `projectZ` admission and attack downstream CPU submission, texture handle resolution, and per-quad setup.

---

## References

### File:line citations (selected)

- Frame entry: `GameOS/gameos/gameosmain.cpp:464–470`
- UpdateRenderers production: `code/mechcmd2.cpp:690`
- GameCamera::render (terrain admission window): `code/gamecam.cpp:140–256`
- Terrain::geometry CPU vertex pass: `mclib/terrain.cpp:980–1193`
- Cull-active cascade roots: `mclib/terrain.cpp:1131–1132`
- Per-quad reservation: `mclib/quad.cpp:250–545`
- Per-quad triangle fill: `mclib/quad.cpp:1515–2173`
- pz gate clusters: `mclib/quad.cpp:1597–1602` and three sisters
- fillTerrainExtra: `mclib/quad.cpp:92`
- renderLists definition: `mclib/txmmgr.cpp:902` (function start); terrain-flush subsection: `mclib/txmmgr.cpp:995–1530+`
- Shadow accum: `mclib/txmmgr.cpp:1184–1242, 1244–1276`
- Render.TerrainSolid: `mclib/txmmgr.cpp:1281–1343`
- terrainDrawIndexedPatches: `GameOS/gameos/gameos_graphics.cpp:2677–2844`
- terrainDrawIndexedPatches dispatch: `GameOS/gameos/gameos_graphics.cpp:2987–2998`
- gosMesh impl: `GameOS/gameos/gameos_graphics.cpp:445–556`
- gosMesh::indexed_tris_ alloc: `GameOS/gameos/gameos_graphics.cpp:1693`
- terrain_extra_vb_ alloc: `GameOS/gameos/gameos_graphics.cpp:1772`
- Static prop batcher flush: `GameOS/gameos/gos_static_prop_batcher.cpp:664–838`
- terrainMVP build: `code/gamecam.cpp:151–189`
- 8 projectFor* wrappers: `mclib/camera.h:525–610`
- TerrainQuad::draw water flag emit: `mclib/quad.cpp:860–863`
- MapData::WorldQuadTerrainCacheEntry: `mclib/mapdata.h:69–122`
- PostcompVertex (heightmap): `mclib/vertex.h:32–63`
- Vertex (per-frame projected vertex): `mclib/vertex.h:67–end`
- MC_VertexArrayNode (per-texture-node ring): `mclib/txmmgr.h:79–110`
- TGL pool init: `code/mission.cpp:3097–3110` (per memory; not directly read)
- Render contract registry: `mclib/render_contract.{h,cpp}`, `shaders/include/render_contract.hglsl`
- F3 closing report: `docs/superpowers/specs/render-contract-f3-report.md`
- F3 pass audit: `docs/superpowers/specs/render-contract-f3-pass-audit.md`
- Render contract registry design: `docs/superpowers/specs/2026-04-26-render-contract-registry-design.md`
- ProjectZ callsite inventory (template): `docs/superpowers/specs/projectz-callsite-inventory.md`
- Render contract callsite inventory: `docs/superpowers/specs/render-contract-callsite-inventory.md`
- Architecture overview: `docs/architecture.md`
- GPU static-prop cull lessons: `docs/gpu-static-prop-cull-lessons.md`
- Modernization prompt (this doc's commission): `docs/plans/2026-04-27-modern-terrain-surface-prompt.md`

### User-memory references (used as starting points; verified against current code)

- `perf_profiling_results.md` — see P1 caveat on currency
- `cull_gates_are_load_bearing.md` — verified at `mclib/terrain.cpp:1131–1132`
- `tgl_pool_exhaustion_is_silent.md` — verified at `mclib/tgl.h:1022`, `tgl.cpp:2536` (cited; not directly opened)
- `terrain_tes_projection.md` — verified against `shaders/gos_terrain.tese` and `mclib/quad.cpp:1597–1602`
- `terrain_mvp_gl_false.md` — verified at `gameos_graphics.cpp:2720` (`GL_FALSE` confirmed)
- `mc2_argb_packing.md` — referenced; specific line cited
- `cpu_displacement_done.md` — referenced for C13
- `terrain_texture_tuning.md` — referenced (not directly opened)
- `static_prop_projection.md` — verified against `gos_static_prop_batcher.cpp:738`
- `tracy_profiler.md` — verified against zone names in `gameos_graphics.cpp` and `txmmgr.cpp`
- `water_rendering_architecture.md` — verified at `mclib/quad.cpp:860–863, 2174` and `txmmgr.cpp:1399`
- `mc2_texture_handle_is_live.md` — verified against `gos_static_prop_batcher.cpp:790–793`
- `clip_w_sign_trap.md` — referenced for §6.2

