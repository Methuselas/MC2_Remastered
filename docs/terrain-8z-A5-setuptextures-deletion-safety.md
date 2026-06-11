# A5 Safety Recon: `TerrainQuad::setupTextures()` Deletion

**Branch:** `claude/terrain-gen-pcg` (nifty-mendeleev worktree)
**Date:** 2026-06-10
**Analyst:** Claude (read-only recon — no source edits made)

---

## Verdict

**A5 is BLOCKED for the mine handle (R7-a). A5 is SAFE for the overlay handle (R7-b).**

`TerrainQuad::mineTextureHandle` and `TerrainQuad::blownTextureHandle` are **static class
members** (quad.cpp:326-327) whose ONLY non-sentinel assignment in the entire codebase is
the lazy-load inside `setupTextures()` (quad.cpp:687-698). No other path calls
`mcTextureManager->loadTexture` for `mine_00.tga` / `minescorch_00.tga`. The GPU-indirect
`BuildMineTextureArray()` (gos_terrain_indirect.cpp:3799-3818) reads these handles and bails
with a retry-defer when they are still `0xffffffff` — but after deleting `setupTextures`, they
will be **permanently** `0xffffffff` (reset to sentinel on mission load at terrain.cpp:1465-1466
and never re-loaded). This permanently breaks mine tile rendering via the static-bake path
and is a hard blocker.

The overlay handle (R7-b) is **not** lazy-loaded by `setupTextures`. It is established during
`MapData::buildTerrainFaceCache()` (mapdata.cpp:299) which runs at mission load time, well
before the first paint frame. The `BuildDecalStaticVBO()` bake reads it directly from the
terrain face cache, not from `TerrainQuad::overlayHandle`. R7-b is safe.

---

## Q1 — What does `setupTextures()` actually establish? (quad.cpp:684-988)

### Persistent side-effects (survive the function call; read by other subsystems)

| State | Location in setupTextures | Where read outside |
|---|---|---|
| `TerrainQuad::mineTextureHandle` (static) | quad.cpp:691 — `mcTextureManager->loadTexture("mine_00.tga")` assigned on first call | `gos_terrain_indirect.cpp:3804` (`BuildMineTextureArray`), `quad.cpp:459/461/799/801/878/880` (legacy mine draw) |
| `TerrainQuad::blownTextureHandle` (static) | quad.cpp:698 — `mcTextureManager->loadTexture("minescorch_00.tga")` assigned on first call | `gos_terrain_indirect.cpp:3805` (`BuildMineTextureArray`), `quad.cpp:467/469/887/889` (legacy mine draw) |
| `TerrainQuad::mineResult` (instance) | quad.cpp:784/863 — `mineResult.setMine()` for per-quad mine-cell state | `quad.cpp:459-469` legacy mine enqueue (now gated out under armed+LOD); `enqueueTerrainMineState()` (also gated) |

### Pure local / draw-only effects (die with the function in game builds post-8z-B)

- `terrainHandle`, `terrainDetailHandle`, `overlayHandle`, `uvData`, `isCement` — instance
  members used by `draw()` and `drawWater()`. Under armed chunk path, `draw()` is already
  suppressed (terrain.cpp:2221-2270, `if (!mc2TerrainLodChunkEnabled())`), so these fields
  have no live consumer in steady state. 8z-B compile-gating removes them.
- `waterHandle`, `waterDetailHandle` — instance members used by the legacy (ii) water draw.
  Under `WaterFastPathOwnsArmedDraw()`, the water draw-side is skipped at quad.cpp:725-728;
  the narrow-candidate walk (terrain.cpp:3807-3838) reads `pVertex->water` directly, NOT
  `waterHandle`. No persistent consumer.
- `clipInfo` — set via `projectForTerrainAdmission` per vertex; consumed only by `drawLine()`
  in the editor. 8z-B quarantine handles this.
- `addTerrainTriangles(recipe)` / `mcTextureManager->addTriangle(...)` — per-frame triangle
  batch submissions. Not persistent state; die with the frame.
- Sentinel resets (`terrainHandle = 0xffffffff` etc.) — local draw-guard resets, no
  cross-frame consumers.

### Summary: the only persistent handles with external consumers are `mineTextureHandle` and `blownTextureHandle`.

---

## Q2 — Mine handle (R7-a): gos_terrain_indirect.cpp:3799-3818

```
// gos_terrain_indirect.cpp:3799-3806
// R7: handles must be loaded by setupTextures before this fires.
const DWORD mineSlot  = TerrainQuad::mineTextureHandle;
const DWORD blownSlot = TerrainQuad::blownTextureHandle;
if (mineSlot == 0xffffffffu || blownSlot == 0xffffffffu) {
    // bail — next dirty event retries
    return;
}
```

The comment at gos_terrain_indirect.cpp:3799-3803 is explicit: the R7 timing trap exists
precisely BECAUSE the handles are lazy-loaded by `setupTextures` and `BuildMineTextureArray`
fires after mission load (at `RebuildMineStaticVBOIfDirty` time). The bail-and-retry
semantics assume `setupTextures` will eventually run on a paint frame and populate the handles.

**Under the FAST PATH (setupTextures skipped when `fullyArmed`):** mineTextureHandle and
blownTextureHandle are loaded on the WARMUP frames (frames before `fullyArmed` becomes true,
when `skipSetup == false`). In steady-state armed mode they are already non-sentinel (loaded
on those early frames) and `BuildMineTextureArray` sees them as valid. The static mine VBO
is then built and remains valid across frames (it is only invalidated by `MarkMineDirty`).

**After A5 deletes setupTextures from game builds:** the handles are reset to `0xffffffff` at
mission load (terrain.cpp:1465-1466) and there is NO path that re-loads them. Every call to
`BuildMineTextureArray` will bail permanently at the sentinel check. The mine static VBO
will never be built. Mine tiles will never render.

**VERDICT: R7-a is a hard BLOCKER for A5.** The mine handle lazy-load is NOT yet migrated
to the GPU-indirect init path. There is no alternative loader.

### Required fix before A5

Add a mission-load-time mine handle loader to the GPU-indirect init path, independent of
`setupTextures`. Suggested site: `gos_terrain_indirect::ResetMineStaticVBO()` or a new
`gos_terrain_indirect::InitMineHandles(const char* texturePath)` call invoked from
`Terrain::primeMissionTerrainCache()` (terrain.cpp, same call site as
`ResetMineStaticVBO()` at line ~971). The loader should replicate the logic at
quad.cpp:687-698:

```cpp
// Proposed new init (to add to gos_terrain_indirect.cpp or a terrain init path):
// Call at mission-load time, AFTER mcTextureManager is ready, BEFORE first paint.
void InitMineTextureHandles(const char* texturePath) {
    if (TerrainQuad::mineTextureHandle == 0xffffffffu) {
        FullPathFileName name;
        name.init(texturePath, "defaults" PATH_SEPARATOR "mine_00", ".tga");
        TerrainQuad::mineTextureHandle = mcTextureManager->loadTexture(
            name, gos_Texture_Alpha, gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
    }
    if (TerrainQuad::blownTextureHandle == 0xffffffffu) {
        FullPathFileName name;
        name.init(texturePath, "defaults" PATH_SEPARATOR "minescorch_00", ".tga");
        TerrainQuad::blownTextureHandle = mcTextureManager->loadTexture(
            name, gos_Texture_Alpha, gosHint_DisableMipmap | gosHint_DontShrink, 0, 0x1);
    }
}
```

This makes `BuildMineTextureArray`'s retry semantics unnecessary for the normal path and
resolves R7-a cleanly.

---

## Q3 — Overlay tex-handle lazy-load (R7-b): gos_terrain_indirect.cpp:4050-4075

The comment at gos_terrain_indirect.cpp:4058-4061 says:

> "...the overlay tex handles lazy-load in TerrainQuad::setupTextures during the first paint
> cycle, before Render.TerrainOverlaysStatic fires"

This comment describes the **reason for deferred build** (not the source of the handle).
Reading `BuildDecalStaticVBO()` (gos_terrain_indirect.cpp:4147-4185), it iterates the
Shape-C terrain face cache and uses `e->overlayHandle` directly from the cache entry:

```cpp
// gos_terrain_indirect.cpp:4182-4183
const DWORD overlayHandle = e->overlayHandle;
if (overlayHandle == 0xffffffffu) continue;
```

The cache entry's `overlayHandle` is set at `MapData::buildTerrainFaceCache()` time
(mapdata.cpp:299: `entry.overlayHandle = Terrain::terrainTextures->peekTextureHandle(baseTexture)`)
and also at mapdata.cpp:1529. This runs at mission-load time, before any paint frame.

`setupTextures` also assigns `TerrainQuad::overlayHandle` (instance member, quad.cpp:964)
from the same cache entry (quad.cpp:632: `r.overlayHandle = entry->overlayHandle`), but
that instance member is consumed only by `draw()` and `drawWater()` — which are already
compiler-gated out of game builds by 8z-B. The `BuildDecalStaticVBO` bake does NOT read
`TerrainQuad::overlayHandle`; it reads the cache directly.

**VERDICT: R7-b is SAFE. The overlay handle is fully established at mission-load time
via `MapData::buildTerrainFaceCache()`, independent of `setupTextures`. A5 does not
break cement/overlay tile rendering.**

Note on the deferred-build comment at gos_terrain_indirect.cpp:4058-4061: the comment is
describing a historical design consideration (the overlay handle needed by `TerrainQuad::draw`
for the LEGACY M2d path was established by `setupTextures`). The decal static bake path
itself has always used the cache entry's handle, never the instance member. The comment
is accurate in describing the REASON the first `BuildDecalStaticVBO` build is deferred to
the first paint frame (not eager at `ResetDecalStaticVBO`), but that reason is about
ORDERING SAFETY in the presence of `setupTextures` — not a hard dependency on it.
After A5, `ResetDecalStaticVBO` could technically build eagerly at mission-load time
(since the cache entry handles are ready then); the deferral is defensive, not required.

---

## Q4 — Armed-skip "already effectively deleted at runtime" reasoning

**The reasoning is sound for most handles, but NOT for the mine handles.**

The argument is: since `MC2_QUADSETUP_ARMED_SKIP` (default ON) has been default-ON since
2026-06-03, `setupTextures` is already skipped every frame in steady-state production.
Therefore any handle the GPU-indirect path needs MUST already be established without
`setupTextures` — otherwise the game would be broken today.

This reasoning holds for:
- `terrainHandle`, `terrainDetailHandle`, `overlayHandle` (instance), `uvData`, `isCement` —
  consumed only by `draw()`/`addTerrainTriangles()`, which are gated off under armed. No
  problem.
- `waterHandle` — consumed only by (ii) water draw, which is suppressed under
  `WaterFastPathOwnsArmedDraw()`. Narrow walk reads `pVertex` directly.

**The reasoning fails for `mineTextureHandle` and `blownTextureHandle`:**

These static handles are loaded exactly ONCE per mission — in the warmup frames (the 1..N
frames before `fullyArmed` becomes true) when `skipSetup == false`. During those frames,
`setupTextures` runs and populates the statics. After that, `skipSetup == true` and
`setupTextures` is skipped — but the handles are already loaded from the warmup frames.
`BuildMineTextureArray` then sees non-sentinel values and builds the VBO successfully.

The game today is NOT broken because `setupTextures` DOES run during warmup. The armed-skip
only fires AFTER `fullyArmed` is true. Deleting `setupTextures` entirely eliminates the
warmup execution that currently establishes the mine handles. The game would break on first
mine tile draw after A5.

**First-frame / warmup window analysis:**

The warmup window for `fullyArmed` requires all of:
- `IsFrameSolidArmed()` — GPU solid recipe + compute path ready
- `IsFrameOverlayArmed()` — GPU overlay path ready
- `IsFrameMineArmed()` — mine static VBO env gate on
- `WaterFastPathOwnsArmedDraw()` — water GPU stream ready
- `terrainTextures2 != NULL` — atlas loaded
- `!drawTerrainGrid`
- `WaterStream::NarrowEnabled()`

On a typical mission load, this condition is false for the first 1-3 frames while GPU
resources initialize. During those frames, `setupTextures` runs per-quad and loads the
mine handles. After A5, those frames produce nothing (black terrain, no mine handle load).

For the mine path, this is not a latency issue but a **permanent failure**: after the warmup
frames pass and `fullyArmed` becomes true, the mine handles remain at their reset-to-sentinel
value from `terrain.cpp:1465-1466`, and `BuildMineTextureArray` bails every frame forever.

---

## Residual Risks and Concrete Tests

### R7-a Mine Handle — BLOCKER (must be fixed before A5)

**Required action:** Add `InitMineTextureHandles(texturePath)` to the GPU-indirect init
path as described in Q2 above. The natural site is `Terrain::primeMissionTerrainCache()`
near the existing `ResetMineStaticVBO()` / `ResetMineTextureArray()` calls
(terrain.cpp:971-972), where `texturePath` is already available.

**The one concrete test that closes R7-a:**

```powershell
# Set env to skip setupTextures from frame 0 (before fullyArmed), confirm mines render:
$env:MC2_QUADSETUP_ARMED_SKIP = "1"  # already default
# Add a NEW env to force skip even on warmup frames (before fullyArmed):
# e.g. MC2_SETUPSKIP_WARMUP=1 — forces skipSetup=true from frame 0 regardless of fullyArmed
# Then load a mission with mine tiles (e.g. mc2_01 has none; use a mission with mines)
# and confirm mine textures still render.
# If mines fail = mine handle not established without setupTextures.
# If mines pass = R7-a is resolved.
```

A simpler zero-code test: add a `printf` guard to `BuildMineTextureArray` that asserts
`mineTextureHandle != 0xffffffff` before the warmup frames complete (i.e., on frame 2+).
If the assert fires with `MC2_SETUPSKIP_WARMUP=1`, R7-a is confirmed blocked. If it
passes, the handles were established by the new init path.

### R7-b Overlay — SAFE (no action required for A5)

The overlay handle is established by `MapData::buildTerrainFaceCache()` at mission load.
`BuildDecalStaticVBO` reads the cache entry directly. No warmup dependency, no setupTextures
dependency.

**Residual risk:** The comment at gos_terrain_indirect.cpp:4058-4061 suggests the build was
INTENTIONALLY deferred to post-first-paint to allow `setupTextures` to load overlay handles.
After A5, if `ResetDecalStaticVBO`'s dirty flag triggers an eager build attempt at
mission-load time (before the cache is fully warmed), the `e->overlayHandle` from the face
cache may still be non-sentinel (face cache is built at the same mission-load phase). Verify
that `buildTerrainFaceCache()` completes before `RebuildDecalStaticVBOIfDirty` is first
called. This is not a blocker but warrants a trace-level log confirm.

### Editor (8z-B compile gate)

8z-B must land before A5. The `#ifdef MC2_IS_EDITOR` guard preserves the full `setupTextures`
body (including mine handle lazy-load) in editor builds. No change needed for the editor path
— the editor continues to use `setupTextures` for `clipInfo`, legacy draw, and the mine/overlay
handle paths. The mine handle fix in the GPU-indirect init path also needs to be
`#ifdef MC2_IS_EDITOR` excluded (or harmlessly idempotent with it) if the editor's
`mcTextureManager` has different lifetime semantics.

### T16/T19 loud-fail (P5 gate)

After A5, processes where `ForceDisableArmingForProcess()` has been called will have
permanently black terrain with no fallback. The P5 loud-fail commit (adding a
`MC2_PANIC_LOG` at `ComputePreflight()` early-return) must land before A5.

---

## Evidence Citations

| Claim | File:line |
|---|---|
| `mineTextureHandle` static definition, sentinel init | `mclib/quad.cpp:326` |
| `blownTextureHandle` static definition, sentinel init | `mclib/quad.cpp:327` |
| Mine handles reset to sentinel at mission load | `mclib/terrain.cpp:1465-1466` |
| `mineTextureHandle` lazy-load — ONLY assignment site in codebase | `mclib/quad.cpp:691` |
| `blownTextureHandle` lazy-load — ONLY assignment site in codebase | `mclib/quad.cpp:698` |
| R7 timing trap comment and bail-on-sentinel in `BuildMineTextureArray` | `GameOS/gameos/gos_terrain_indirect.cpp:3799-3818` |
| `BuildDecalStaticVBO` reads `e->overlayHandle` from terrain face cache | `GameOS/gameos/gos_terrain_indirect.cpp:4182-4183` |
| `overlayHandle` established in `buildTerrainFaceCache` at mission load | `mclib/mapdata.cpp:299` |
| `fullyArmed` predicate (all conjuncts) | `mclib/terrain.cpp:3718-3725` |
| Armed-skip default-ON since 2026-06-03 | `mclib/terrain.cpp:3714-3717` (comment + code) |
| setupTextures call gated by `skipSetup` | `mclib/terrain.cpp:3803-3806` |
| deferred-build comment naming setupTextures as source of overlay handles | `GameOS/gameos/gos_terrain_indirect.cpp:4058-4061` |
