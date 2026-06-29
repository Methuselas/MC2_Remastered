# TERRAIN-INDIRECT-PATCH-RETIRE-RECON-1
# Precise surgical delete targets + retain-list for the IndirectBridge and PatchStreamThin terrain branches

> Extends `terrain-legacy-retire-recon-1.md` (`4aeeb0b0`) — that doc proved reachability;
> this doc supplies EXACT edit targets, retain-list, and slice split.
>
> All `file:line` are nifty HEAD at recon time and **DRIFT CHEAPLY**. Run
> `repo_query.py slice-preflight` before any derived slice.
>
> Branch: `claude/nifty-mendeleev`

---

## TL;DR + Recommended Retirement Order

**Retire PatchStreamThin FIRST, then IndirectBridge.**

Rationale: PatchStream has zero SSBO/barrier/editor entanglement — its entire draw path is a
single function (`flush()`) with no retained GL objects that overlap the non-draw infra. The
IndirectBridge shares the `gos_terrain_bridge_drawIndirect` function and `s_indirectTerrainSampler`
lazy-static, both of which are draw-only but require a careful audit. PatchStream is strictly
lower-blast-radius. Mirror the LEGACY-MLR-DELETE-1 surgical pattern: at each branch's selection
point, insert a `noteTerrainPath` tripwire + skip (no draw), then separately delete the dead-code
in a follow-on slice after the tripwire proves the counter stays 0.

Both draws are **already dead in default** (chunk-on = `mc2TerrainLodChunkEnabled()` true).
The tripwire skips are zero-behavior-change for the default path; they prevent the branches
from running on `MC2_TERRAIN_LOD_CHUNK=0` after retirement.

---

## Q1 — Selection Points in txmmgr.cpp renderLists

File: `mclib/txmmgr.cpp`, live as of nifty HEAD. The selection block is at **:3000–3021**
(line numbers drift; the logic is the `if (gos_terrain_indirect::IsFrameSolidArmed())` block):

```
:3000  if (gos_terrain_indirect::IsFrameSolidArmed()) {
:3001      // comment block ...
:3012      if (mc2TerrainLodChunkEnabled()) {
:3013          modernHandled = true;   // LOD-chunk owns it; suppress IndirectBridge
:3014      } else {
:3015          modernHandled = gos_terrain_indirect::DrawIndirect();  // <-- IndirectBridge draw
:3016      }
:3017  } else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
:3018      // Un-armed frame: patch-stream flush
:3020      modernHandled = TerrainPatchStream::flush();             // <-- PatchStreamThin draw
:3021  }
```

**In default (chunk-on) operation:**
- `mc2TerrainLodChunkEnabled()` is true
- At :3012 the `if`-branch fires → `modernHandled=true`
- The `else` branch at :3015 (`DrawIndirect()`) is **never reached**
- The `else if` at :3017 is never reached either (armed=true, took the first branch)
- **Both draw calls are already dead in default.** Confirmed by `terrain_path.indirect==0`
  and `terrain_path.patch_stream==0` in live telemetry.

Gates:
- **IndirectBridge** is reachable only when `IsFrameSolidArmed()=true` AND `mc2TerrainLodChunkEnabled()=false`.
- **PatchStreamThin** is reachable only when `IsFrameSolidArmed()=false` AND
  `TerrainPatchStream::isReady()=true` AND `!TerrainPatchStream::isOverflowed()`.

---

## Q2 — Surgical Retire Point Per Branch

### PatchStreamThin — minimal edit

**Retire-draw site: `mclib/txmmgr.cpp:3017–3021`**

Replace the `flush()` call with a tripwire-only skip:

```cpp
} else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
    // TERRAIN-INDIRECT-PATCH-RETIRE-RECON-1: PatchStreamThin draw retired.
    // LOD-chunk is the canonical renderer. Tripwire: if this counter increments,
    // something has re-enabled this path — smoke gate: terrain_path.patch_stream==0.
    RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::PatchStreamThin);
    // modernHandled stays false -> legacy MLR tripwire fires for the terrain node
    // (see txmmgr.cpp:3064-3068), surfacing in terrain_path.legacy_mlr.
}
```

No other call sites — `TerrainPatchStream::flush()` is called ONLY at txmmgr.cpp:3020.

**Smoke gate:** `terrain_path.patch_stream == 0` across tier1. Already 0 in default; the retire
makes it permanently 0 even under `MC2_TERRAIN_LOD_CHUNK=0`.

### IndirectBridge — minimal edit

**Retire-draw site: `mclib/txmmgr.cpp:3014–3016`** (the `else` branch of the chunk check):

```cpp
    } else {
        // TERRAIN-INDIRECT-PATCH-RETIRE-RECON-1: IndirectBridge SOLID draw retired.
        // LOD-chunk is the canonical renderer. Tripwire counter.
        // Smoke gate: terrain_path.indirect==0.
        RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::IndirectBridge);
        modernHandled = true;  // suppress legacy MLR fall-through (arming already fired gate-off)
    }
```

Note: `modernHandled = true` is necessary here even without drawing — when `IsFrameSolidArmed()`
is true, the gate-off in `setupTextures()` has already fired and `TerrainPatchStream` has no SOLID
records. Setting `modernHandled=false` would cause the masterVertexNode loop to fire the LegacyMLR
tripwire for terrain nodes, which is a false signal. Keep `modernHandled=true`.

**Smoke gate:** `terrain_path.indirect == 0` across tier1. Already 0 in default.

---

## Q3 — Draw-Path Code That Becomes Deletable

### PatchStreamThin dead-code map

Once `TerrainPatchStream::flush()` has no callers:

| Symbol / TU | Location | Status after retire |
|---|---|---|
| `TerrainPatchStream::flush()` | `GameOS/gameos/gos_terrain_patch_stream.cpp:888` | **Deletable** — no remaining callers |
| `noteTerrainPath(PatchStreamThin)` inside flush | `gos_terrain_patch_stream.cpp:1503` | Deleted with flush() |
| `markTerrainDrawn()` call inside flush | `gos_terrain_patch_stream.cpp:1500` | Deleted with flush() |
| `gos_terrain_bridge_beginBucketLoop()` | `gameos_graphics.cpp:2862` | Used ONLY by flush() — **Deletable** |
| `gos_terrain_bridge_drawSingleBucket()` | `gameos_graphics.cpp` | Used ONLY by flush() — **Deletable** |
| `gos_terrain_bridge_endVertexDeclaration()` | gos_terrain_bridge.h / gameos_graphics.cpp | Used ONLY by flush() — **Deletable** |
| `gos_terrain_bridge_end()` | gos_terrain_bridge.h / gameos_graphics.cpp | Check for non-flush callers before deleting |
| `TerrainPatchStream::appendTriangle/appendQuad/appendQuadRecord/appendThinRecord/addRecordVertParity/addThinRecordVertParity/appendThinRecordDirect` | gos_terrain_patch_stream.cpp:671–883 | Called from `mclib/quad.cpp`; deletable AFTER quad.cpp append-sites are removed |
| `TerrainPatchStream::ensureRecipeForQuad/tryGetRecipeIdx/makeRecipeKey` | gos_terrain_patch_stream.cpp:772–820 | Called from quad.cpp; deletable AFTER quad.cpp sites removed |
| Append call sites in quad.cpp | `mclib/quad.cpp:1698, 2178–2221` | Must be removed to make the PatchStream append chain fully dead |
| `TerrainPatchStream::isFastPathActive/isThinRecordsActive/isReady/isOverflowed/beginFrame` | gos_terrain_patch_stream.cpp:600–670 | Accessors — check all callers before deleting; `beginFrame` called from gameos_graphics.cpp:5726 |
| `TerrainPatchStream::init/destroy` | gos_terrain_patch_stream.cpp:309/536 | Called from gameos_graphics.cpp:5355/5366; deletable once all other TPS symbols gone |
| `emitCensus` | gos_terrain_patch_stream.cpp:449 | PatchStream diagnostic; deletable |
| `getLastFlush*` accessors | gos_terrain_patch_stream.cpp:605–609 | Called from gameosmain.cpp:1528–1554 for RenderSnapshot — **SHARED with snapshot infra; do NOT delete until snapshot callers are also updated** |
| `gos_terrain_patch_stream.cpp` entire TU | — | Deletable AFTER all callers in quad.cpp / gameos_graphics.cpp / gameosmain.cpp are cleaned up |

> **HIGH: `getLastFlushBucketCount/VertCount/ThinRecCount/RecipeCount/wasLastFlushOverflowed`** are
> called from `gameosmain.cpp:1528–1554` to populate `RenderSnapshot::terrainPass`. These are
> **non-draw shared** — deleting them breaks the render snapshot/telemetry system. Retain or
> replace with a stub that returns 0 until gameosmain.cpp is updated.

### IndirectBridge dead-code map

Once `gos_terrain_indirect::DrawIndirect()` has no callers:

| Symbol / TU | Location | Status after retire |
|---|---|---|
| `gos_terrain_indirect::DrawIndirect()` | `GameOS/gameos/gos_terrain_indirect.cpp:3687` | **Deletable** — no remaining callers |
| `gos_terrain_bridge_drawIndirect()` | `GameOS/gameos/gameos_graphics.cpp:4030` | Called ONLY from `DrawIndirect()` at :3702 — **Deletable** |
| `s_indirectTerrainSampler` (lazy-static GLuint) | `gameos_graphics.cpp` inside `gos_terrain_bridge_drawIndirect` | Draw-only; deleted with the function |
| `noteTerrainPath(IndirectBridge)` inside DrawIndirect | `gos_terrain_indirect.cpp:3751` | Deleted with DrawIndirect() |
| `markTerrainDrawn()` call inside DrawIndirect | `gos_terrain_indirect.cpp:3750` | Deleted with DrawIndirect() |
| Ring fence logic (`g_thinRingFences[g_thinRingSlot] = glFenceSync(...)`) | `gos_terrain_indirect.cpp:3736–3739` | Draw-only; deleted with DrawIndirect() |
| `gos_terrain_bridge_drawIndirect` declaration in `gos_terrain_bridge.h:138` | `gos_terrain_bridge.h:135–144` | Remove after function deleted |
| `RenderWaterReflectionPass()` | `gos_terrain_indirect.cpp:3766` | Gates on `IsFrameSolidArmed()` AND env `MC2_WATER_REFLECTION_RT` (default OFF). Calls `DrawIndirect()` internally at :3817. After `DrawIndirect()` is retired, this function also loses its draw entry point — **Deletable in a follow-on slice** once water reflection is confirmed non-functional |

> **HIGH: `RenderWaterReflectionPass()` calls `DrawIndirect()` at gos_terrain_indirect.cpp:3817.**
> It is separately gated (`MC2_WATER_REFLECTION_RT` default OFF) so it does not affect default
> behavior. However, retiring `DrawIndirect()` without retiring `RenderWaterReflectionPass()`
> leaves a dangling call. Either: (a) retire both together, or (b) convert the `DrawIndirect()`
> call inside `RenderWaterReflectionPass()` to a tripwire + early-return before deleting the
> callee. Map as a dependency. The safe order is: retire the txmmgr call site first (slice 1),
> then retire `RenderWaterReflectionPass::DrawIndirect()` call + `DrawIndirect()` body (slice 2).

---

## Q4 — Critical Retain-List (do NOT delete)

### **MUST RETAIN — `IsFrameSolidArmed()` and the arming infrastructure**

`IsFrameSolidArmed()` is used by **8+ non-draw consumers**:

| Consumer | File | Purpose |
|---|---|---|
| Water fast path gate | `mclib/quad.cpp:222` / `gameos_graphics.cpp` | Selects GPU water cull path |
| Water depth gate | `docs/known_issues.md:105` pattern, water pipeline | `IsFrameSolidArmed()?getDispatchMvp16():live` — stale-water guard |
| Object MVP gate | Multiple LOD/decal/object callers | Dispatch-MVP snapshot vs live fallback |
| Decal / overlay draw gate | `!(IsFrameSolidArmed() && IsFrameOverlayArmed())` conjunction | `render-perf-snapshot.md` decal retirement |
| `IsFrameOverlayArmed()` conjunction | `gos_terrain_indirect.cpp:271` / gos_terrain_indirect.h:255 | **Overlay is DEFAULT-ON; must stay** |
| `gos_visual_diff.cpp:113` | uses sticky `HasArmedEver()` | Visual diff system |
| `mclib/quad.cpp:222-224` | comment + gate | Per-frame cull path selection |
| Editor chunk-parity path | `docs/editor-chunk-path-parity.md:68` | Editor uses `IsFrameSolidArmed` for its surface draw gate |

**The entire arming stack must be retained:**
- `s_frameSolidArmed` + `s_processArmingDisabled` state variables
- `ComputePreflight()` — called from `mclib/terrain.cpp:3485` — **MUST STAY**
- `PackThinRecordsForFrame()` — called from `ComputePreflight()` — arming sets `s_frameSolidArmed`; **MUST STAY**
- `BuildIndirectCommands()` — called from `ComputePreflight()` — fills `g_indirectCmdBuffer`; retain until confirmed unused by any remaining path
- `s_solidGpuDispatchRanThisFrame` + `s_frameSolidCmdCount` + `s_frameSolidPackedThinCount` — retain; used by ring logic
- `g_recipeSSBO` / `g_thinRecordSSBO` / `g_indirectCmdBuffer` — SSBO BUILD infra; retain until all consumers confirmed dead
- `ForceDisableArmingForProcess()` — called from `DrawIndirect()` hard-failure path; can be deleted WITH DrawIndirect()
- **`IsFrameOverlayArmed()`** — separate predicate; overlays are DEFAULT-ON and have their own consumers; **MUST STAY**

### **MUST RETAIN — Recipe and thin-record SSBO BUILD (separate from DRAW)**

The recipe SSBO (`g_recipeSSBO`) and thin-record SSBO (`g_thinRecordSSBO`) are built by
`ComputePreflight()`/`PackThinRecordsForFrame()`. They are:
- **Read by `DrawIndirect()`** (via `gos_terrain_bridge_drawIndirect`) — this is the DRAW path, deletable
- **Read by `RenderWaterReflectionPass()`** (via its own `DrawIndirect()` call) — see HIGH above
- **Built by `ComputePreflight()`** — arming infra, must stay
- **NOT read by any confirmed non-draw consumer** in the current grep — but confirm before deleting the SSBOs themselves

The SSBO BIND-FOR-DRAW inside `gos_terrain_bridge_drawIndirect` (gameos_graphics.cpp:4232–4237)
is draw-only and deletable with that function. The SSBO BUILD in ComputePreflight is retained.

### **MUST RETAIN — Editor SURFACE and OVERLAY paths (SEPARATE from IndirectBridge SOLID draw)**

Per `terrain-legacy-retire-recon-1.md` §5 and `docs/editor-chunk-path-parity.md:68`:

- The editor renders terrain via `gos_terrain_surface_bridge_draw()` (txmmgr.cpp:3050),
  which calls `gos_terrain_surface::IsEnabled()` and is **completely separate** from
  `gos_terrain_indirect::DrawIndirect()`.
- The OVERLAY path (`IsFrameOverlayArmed()`) is also separate from the SOLID IndirectBridge.
- Deleting `DrawIndirect()` does NOT affect the editor surface/overlay draw.

> **Re-verification (live):** `gos_terrain_surface_bridge_draw()` at `mclib/txmmgr.cpp:3050`
> runs unconditionally (no `IsFrameSolidArmed()` guard); it is upstream of the
> `IsFrameSolidArmed()` block at :3000. The SOLID IndirectBridge draw at :3015 is a different
> code path. The editor SURFACE path is confirmed SEPARATE. This was stated in `4aeeb0b0` and
> re-verified by live grep.

### **MUST RETAIN — `terrainBindThinUniformsForPatchStream()` and related bridge uniforms**

`gos_terrain_bridge_drawIndirect` calls `terrainBindThinUniformsForPatchStream()` for its
uniform setup. Check whether `TerrainPatchStream::flush()` also calls this (it calls
`gos_terrain_bridge_bindUniforms()` at gos_terrain_patch_stream.cpp:1026 instead — a different
function). Verify both bridge functions' caller sets before deleting.

---

## Q5 — Retirement Order + Slice Split

### Recommended order: PatchStream first, then Indirect

**Slice 1 (retire PatchStreamThin draw):**
1. Edit: `mclib/txmmgr.cpp:3017–3021` — replace `flush()` call with `noteTerrainPath(PatchStreamThin)` + no draw (see Q2)
2. Build `RelWithDebInfo`, run smoke tier1 (30s)
3. Gate: `terrain_path.patch_stream == 0` in diagnostic dump
4. If passing: commit `TERRAIN-PATCH-STREAM-DRAW-RETIRE-1`

**Slice 2 (retire IndirectBridge draw):**
1. Edit: `mclib/txmmgr.cpp:3014–3016` — replace `DrawIndirect()` call with `noteTerrainPath(IndirectBridge)` + `modernHandled=true` (see Q2)
2. Handle `RenderWaterReflectionPass()` dependency: either convert its internal `DrawIndirect()` call at `gos_terrain_indirect.cpp:3817` to early-return, or retire together
3. Build, smoke tier1
4. Gate: `terrain_path.indirect == 0`
5. If passing: commit `TERRAIN-INDIRECT-DRAW-RETIRE-1`

**Slice 3 (delete PatchStreamThin dead code) — AFTER slice 1 proves counter==0:**
- Delete `TerrainPatchStream::flush()` body
- Delete `gos_terrain_bridge_beginBucketLoop/drawSingleBucket/endVertexDeclaration` (verify sole caller was flush)
- Remove append call sites from `mclib/quad.cpp` (1698, 2178–2221)
- Update `gameosmain.cpp:1528–1554` `getLastFlush*` callers → stub/zero or remove
- Smoke gate again

**Slice 4 (delete IndirectBridge dead code) — AFTER slice 2 proves counter==0:**
- Delete `gos_terrain_indirect::DrawIndirect()` body (gos_terrain_indirect.cpp:3687–3755)
- Delete `gos_terrain_bridge_drawIndirect()` body (gameos_graphics.cpp:4030–~4290)
- Delete or stub `RenderWaterReflectionPass()` (gos_terrain_indirect.cpp:3766–end) — gated `MC2_WATER_REFLECTION_RT` default OFF so zero behavior change
- Remove declaration from `gos_terrain_bridge.h:138`
- **Do NOT delete `IsFrameSolidArmed`, `ComputePreflight`, arming infrastructure, or recipe/thin-SSBO build**
- Smoke gate

---

## Q6 — `terrainDrawIndexedPatches` / gameos_graphics.cpp:7304 Status

**`terrainDrawIndexedPatches` is now fully dead.**

Analysis:
- It is called at `gameos_graphics.cpp:7301` (inside `gosRenderer::gos_RenderIndexedArray`) only
  when `curStates_[gos_State_Terrain]=1` AND `terrain_batch_extras_count_ > 0`.
- `gos_State_Terrain=1` is set by the terrain bridge functions (`gos_terrain_bridge_beginBucketLoop`
  at :2868 and `gos_terrain_bridge_drawSingleBucket` via the bucket loop at :2845).
- Both bridge functions are called ONLY from `TerrainPatchStream::flush()`.
- After LEGACY-MLR-DELETE-1, txmmgr's masterVertexNode loop at :3064–3068 unconditionally skips
  terrain nodes and never sends them to `gos_RenderIndexedArray`. Non-terrain nodes clear
  `gos_State_Terrain=0` at :3072.
- Therefore `gos_State_Terrain=1` can only be set by `flush()`, but `flush()` calls
  `gos_terrain_bridge_drawSingleBucket` directly (not `gos_RenderIndexedArray`).
- **`terrainDrawIndexedPatches` at :7301 is unreachable in the current default.**

The `noteTerrainPath(LegacyMLR)` at `gameos_graphics.cpp:7304` is **also unreachable**.
Both `:7301` and `:7304` are deletable in slice 3 when PatchStream dead code is cleaned up.

`terrainDrawIndexedPatches()` itself at `gameos_graphics.cpp:7052` is deletable at that time.
Its forward declaration at `:1752` can also be removed.

> **Drift caveat:** If `TerrainPatchStream::flush()` is retired (slice 1) but not yet deleted
> (slices 3), there is a transitional window where `terrainDrawIndexedPatches` is orphaned but
> not yet removed. Track as part of slice 3's deletion list.

---

## Summary Table

| Item | File:line (DRIFT-PRONE) | Action |
|---|---|---|
| PatchStreamThin selection site | `mclib/txmmgr.cpp:3017–3021` | Slice 1: replace flush() with tripwire |
| IndirectBridge selection site | `mclib/txmmgr.cpp:3014–3016` | Slice 2: replace DrawIndirect() with tripwire |
| `TerrainPatchStream::flush()` | `gos_terrain_patch_stream.cpp:888` | Slice 3: delete |
| `gos_terrain_bridge_beginBucketLoop/drawSingleBucket` | `gameos_graphics.cpp:2862/~2900` | Slice 3: delete |
| quad.cpp append sites | `mclib/quad.cpp:1698, 2178–2221` | Slice 3: remove |
| **HIGH: `getLastFlush*` accessors** | `gos_terrain_patch_stream.cpp:605–609` | **SHARED** — update gameosmain.cpp callers before deleting |
| `gos_terrain_indirect::DrawIndirect()` | `gos_terrain_indirect.cpp:3687` | Slice 4: delete |
| `gos_terrain_bridge_drawIndirect()` | `gameos_graphics.cpp:4030` | Slice 4: delete |
| **HIGH: `RenderWaterReflectionPass()` calls DrawIndirect** | `gos_terrain_indirect.cpp:3817` | Must stub/retire before deleting DrawIndirect() |
| `terrainDrawIndexedPatches()` | `gameos_graphics.cpp:7052` | Slice 3: delete (unreachable after LEGACY-MLR-DELETE-1) |
| LegacyMLR telemetry at :7304 | `gameos_graphics.cpp:7304` | Deleted with terrainDrawIndexedPatches |
| **RETAIN: `IsFrameSolidArmed()` + `ComputePreflight()`** | `gos_terrain_indirect.cpp` | **DO NOT DELETE — 8+ non-draw consumers** |
| **RETAIN: `IsFrameOverlayArmed()`** | `gos_terrain_indirect.cpp:271` | **DO NOT DELETE — overlay DEFAULT-ON** |
| **RETAIN: recipe/thin SSBO BUILD** | `gos_terrain_indirect.cpp` ComputePreflight | **DO NOT DELETE — arming infra** |
| **RETAIN: `gos_terrain_surface_bridge_draw()`** | `mclib/txmmgr.cpp:3050` | **Editor surface path — completely separate** |
