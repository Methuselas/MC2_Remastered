# Static-Prop Snapshot Bridge Retire — Recon

**Date:** 2026-06-03  
**Lane:** `PERF-STATICPROP-SNAPSHOT-BRIDGE-RETIRE-1`  
**Branch:** `claude/perf-staticprop-snapshot-bridge-retire-1`  
**Status:** RECON — no production code changes; plan pending review  
**Out of scope:** 3/4/5 GPU-Scene flip, TerrainObjects update fix, LOD/model work, shader changes  

---

## 1. Executive verdict

**All four Tracy zones under scope are pure per-frame bridge work against data that is
stable or monotonically settling after mission load.**

| Zone | Tracy cost | Root cause | Verdict |
|---|---:|---|---|
| `Extract.SP.Fill` | ~1.13ms | 2× full `s_objectRecords` scan/frame (count + fill) — no generation guard | BRIDGE: dirty-only via `s_registryGeneration` |
| `Extract.SP.WriteLoop` | ~157µs | 9+ registry calls/prop/frame for immutable + monotonic fields | BRIDGE: all fields cacheable; only `hasCullRecord` is not immutable |
| `StaticProp.SnapshotBuild` | ~383µs | Per-frame rebuild of dispatch arrays from snapshot rows — only `baseInstance` varies | LIKELY BRIDGE: `baseInstance` stable after persistent buckets (verify) |
| `ExtractRenderSnapshot` self | ~315µs | `batcher_compareSnapshotPackets`, visibility query, arena overhead | PARTIAL: compare stays; arena amortized by dirty-only model |

**Recommended approach:** Persistent snapshot row cache (built once at mission load, patched
dirty-only via two generation signals: `s_registryGeneration` + `s_cullRecordVersion`).
On a clean frame, skip `fillStaticPropSlots` + `WriteLoop` entirely, memcpy cached rows into
arena. Expected: `Extract.SP.Fill` + `Extract.SP.WriteLoop` → ~0 on steady-state frames
(after warmup). `StaticProp.SnapshotBuild` follow-on: verify `baseInstance` stability, then
cache the dispatch arrays dirty-only.

**First slice: `STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1`** — build persistent cache in
parallel with legacy rebuild; compare field-by-field; prove mismatch = 0.

---

## 2. Exact call chain

```
gameosmain.cpp:1200  →  ExtractRenderSnapshot()               [render_snapshot.cpp:61]
  │
  ├── RenderWorld::getStaticPropSlotCount()                   [RenderWorld.cpp:1118]
  │     s_objectRecordsMutex lock
  │     full scan of s_objectRecords for kind==StaticProp
  │     returns count (alive + dead)
  │
  ├── views.resize(slotCount)                                 [heap alloc, scratch only]
  │
  ├── [Tracy: Extract.SP.Fill]
  │   RenderWorld::fillStaticPropSlots(views.data(), slotCount) [RenderWorld.cpp:1127]
  │     s_objectRecordsMutex lock
  │     full scan of s_objectRecords — writes StaticPropRecordView per SP slot
  │     StaticPropRecordView = { handle, recipeIndex, alive, generationValid }
  │     recipeIndex == slot index == RW handle index (identity by construction)
  │
  ├── count alive slots → aliveCount
  ├── arena.allocArray<ExtractedStaticProp>(aliveCount)
  │
  ├── [Tracy: Extract.SP.WriteLoop]
  │   for each alive view:
  │     staticPropGetModelMatrix(recipeIndex)        → s_recipes[rng.first].modelMatrix
  │     staticPropGetTypeId(recipeIndex)             → s_recipes[rng.first].typeID
  │     staticPropGetInstanceFlags(recipeIndex)      → s_recipes[rng.first].flags
  │     staticPropGetExtentRadius(recipeIndex)       → s_recipes[rng.first].extentRadius (?)
  │     staticPropGetLightDataIndex(recipeIndex)     → s_recipes[rng.first].lightDataIndex
  │     staticPropGetHasCullRecord(recipeIndex)      → s_recipeHasSubstrateRecord[recipeIndex]
  │     staticPropGetMaterialCacheInfo(recipeIndex)  → s_typeMatCache[typeID]
  │     getRecipeShapeName(recipeIndex)              → s_shapeName[?] / snprintf copy
  │     batcher_getPacketTexArrayLayer(firstPkt+count-1)  → packet sidecar check
  │   writes ExtractedStaticProp[writeIdx]
  │
  ├── [Tracy: Extract.SP.Packets]
  │   for each draw slot: batcher_getDrawSlotEntry → ExtractedStaticPropPacket
  │   (tiny; ~0.4% of total; not in scope)
  │
  ├── batcher_compareSnapshotPackets(&snap)          [v2.3 live-vs-snap count compare]
  ├── batcher_getSnapCullStats()
  ├── batcher_getSnapshotBuildStats()                [v8 build stats, reads prev-flush]
  ├── queryVisibility() + log line
  └── store s_lastSnapshot, return snap
```

**Separately, inside `draw_screen()` path:**

```
textureManagerRenderLists()
  └── GpuStaticPropBatcher::flush(snap)
        └── [Tracy: StaticProp.SnapshotBuild]              [gos_static_prop_batcher.cpp:~5444]
              for each sorted draw command:
                s_snapV6Packets[si] ← snapshot row (pipelineId, firstIndex, indexCount)
                s_snapV6Meta[si]    ← snapshot row + typeId + group + instanceCount
                s_snapV6Meta[si].baseInstance ← baseInstanceMapDisp[si]
                  where baseInstanceMapDisp =
                    s_baseInstanceByCmdMap + s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame
```

---

## 3. Data ownership table

### `ExtractedStaticProp` fields (per alive prop, written in `Extract.SP.WriteLoop`)

| Field | Source | Classification | Per-frame? | Cacheable? |
|---|---|---|---|---|
| `rwHandle` | `views[i].handle` (RenderWorld) | stable (immutable once alive) | No — generation checked at registration | Yes |
| `recipeIndex` | `views[i].recipeIndex` | IMMUTABLE (slot index = handle.index(), never relocated) | No | Yes |
| `typeId` | `s_recipes[rng.first].typeID` | IMMUTABLE (set at finalize, never changed) | No | Yes |
| `instanceFlags` | `s_recipes[rng.first].flags` | IMMUTABLE (set at finalize) | No | Yes |
| `worldMatrix` | `s_recipes[rng.first].modelMatrix` | IMMUTABLE (position fixed at mission load) | No | Yes |
| `worldCenterX/Y/Z` | derived from modelMatrix | IMMUTABLE | No | Yes |
| `boundingRadius` | registry extentRadius | IMMUTABLE (written once, rare markVisible change bumps generation) | No (generation-gated) | Yes |
| `lightDataIndex` | `s_recipes[rng.first].lightDataIndex` | PERMANENT (== recipeIndex, proved by Slice 1 `MC2_LIGHTBAKE_PARITY`) | No | Yes |
| `hasCullRecord` | `s_recipeHasSubstrateRecord[recipeIndex]` | MONOTONIC (false→true after first flush submission, then stays true for prop lifetime) | No (after warmup) | Yes (with `s_cullRecordVersion` dirty signal) |
| `texArrayLayer`, `materialIdx`, `alphaClass`, `packetCount`, `firstPacket` | `s_typeMatCache[typeID]` | IMMUTABLE (set at geometry finalization) | No | Yes |
| `shapeName` | `getRecipeShapeName(recipeIndex)` | IMMUTABLE (set at registration) | No | Yes |
| `packetRangesOk` (sidecar check) | `batcher_getPacketTexArrayLayer` | IMMUTABLE (packet sidecar built at finalize) | No | Yes |

**Finding: ALL fields in `ExtractedStaticProp` are immutable or monotonically-settling after
mission load.** `hasCullRecord` is the only field that changes after initial registration,
and only from `false`→`true` on the frame after a prop's first flush submission.

### Dispatch arrays (`s_snapV6Packets`/`s_snapV6Meta`) built in `StaticProp.SnapshotBuild`

| Field | Source | Per-frame? | Notes |
|---|---|---|---|
| `pipelineId`, `firstIndex`, `indexCount` | snapshot row (immutable) | No | From finalized geometry |
| `sortedSlot`, `globalPacketIdx`, `typeId` | snapshot row | No | Stable after registration |
| `group` | `s_typeRanges[typeId]` | No | Stable after persistent buckets |
| `instanceCount` | `typeIt->second.instanceCount` (from `s_typeRanges`) | No (after persistent buckets) | Stable after 2b Stage 2 |
| `drawIDBase` | loop index `si` | No | Stable by construction |
| `baseVertex` | `spkt.baseVertex` (packet sidecar) | No | Stable after finalize |
| **`baseInstance`** | `baseInstanceMapDisp[si]` = ring-slot[`s_coalesceFrameSlot`][si] | **Verify** | Ring-slot flips but static layout fixed → values likely stable |

---

## 4. Consumer table

| Consumer | Still live? | Needs per-frame rebuild? | Replacement |
|---|---|---|---|
| `GpuStaticPropBatcher::flush()` — dispatch arrays (`s_snapV6Packets`/`s_snapV6Meta`) | YES (v8 = sole owner) | No (persistent cache; dirty-only) | Dirty-only dispatch cache |
| `GpuStaticPropBatcher::flush()` — snap-cull slot compare | YES (snap-cull optional, `MC2_SNAP_CULL=1`) | Only on cull-result change | Cull results come from GPU, separate path |
| `batcher_compareSnapshotPackets()` — v2.3 live-vs-snap count compare | YES (runs every frame) | Yes (reads snap.staticProps.size()) | Can read from persistent count |
| HZB / visibility probe (`staticPropGetHasCullRecord`) | YES (read by probe submit path) | No (reads registry directly, not snapshot) | Not affected |
| Object ID / picking (`lookupAtPixel`) | YES | No (reads RenderWorld directly) | Not affected |
| Debug overlays (`MC2_RENDER_SNAPSHOT_LOG`) | YES (gated) | No (reads snap counters) | Counters can come from persistent mirror |
| Snapshot compare/oracle (`spBuildRetired`, `ok` gate) | YES | No (counters stable) | Not affected |
| `render_snapshot.h` ok gate | YES | No (reads snap fields) | Persistent snap carries same fields |
| Mech extraction path | YES (gated `MC2_SNAPSHOT_MECH_EXTRACT`) | Yes (mechs are dynamic) | Not in scope |
| Tests / scripts | Smoke gate (`ok=1`, zero mismatch) | No behavior change needed | Same gate behavior |

**Key finding:** the only consumer that genuinely requires per-frame data is the
**dynamic portion** (mechs/vehicles). Static props are consumed by `flush()` which
now only needs the persistent cache. The `batcher_compareSnapshotPackets` v2.3 compare
needs a count but not a full per-prop scan.

---

## 5. Current duplication / bridge map

```
Per-frame work                    What it produces          Actually needed?
──────────────────────────────────────────────────────────────────────────────
getStaticPropSlotCount()          count (N_slots)           Once (stable)
  └─ full s_objectRecords scan

fillStaticPropSlots()             StaticPropRecordView[N]   Once (stable)
  └─ full s_objectRecords scan    handle + recipeIndex

WriteLoop: 9 registry calls/prop  ExtractedStaticProp[N]    Once per prop (stable)
  └─ modelMatrix, typeId,         + hasCullRecord (mono)    hasCullRecord: on change
     instanceFlags, extentRadius,
     lightDataIndex, matCache,
     shapeName, sidecar check

arena alloc + memcpy              Span into ping-pong buf   Per-frame (BUT the DATA
                                                            is stable; copy of cache OK)

StaticProp.SnapshotBuild          s_snapV6Packets[N_cmds]   Once (stable if baseInstance
  └─ from snapshot rows           s_snapV6Meta[N_cmds]       values are stable — verify)
     + baseInstance (ring slot)
```

**The structural duplication:** the flush path (registry + buckets) is already
persistent/dirty-only after 2b Stage 2. The extraction path was NOT updated in parallel.
`fillStaticPropSlots` + `WriteLoop` rebuild data that the registry already owns persistently
and that `flush()` itself already reads persistently. The snapshot is a bridge that was
necessary pre-2b but is now a redundant per-frame rebuild of persistent data.

---

## 6. Proposed source-of-truth model

```
Source of truth (persistent, already exists)
─────────────────────────────────────────────
GpuStaticPropRegistry::s_recipes[]          → modelMatrix, typeId, flags, extentRadius,
                                              lightDataIndex, shapeName
GpuStaticPropRegistry::s_typeMatCache[]     → texArrayLayer, materialIdx, alphaClass,
                                              packetCount, firstPacket
GpuStaticPropRegistry::s_recipeHasSubstrate → hasCullRecord
GpuStaticPropBatcher packet sidecar         → packetRangesOk
RenderWorld::s_objectRecords                → alive/dead status, handle (stable)

Proposed: persistent snapshot row cache
───────────────────────────────────────
module-static std::vector<ExtractedStaticProp> s_spRowCache  [render_snapshot.cpp]
uint64_t s_spRowCacheGen = 0     ← matches (s_registryGeneration, s_cullRecordVersion)

On ExtractRenderSnapshot():
  spRowCacheDirty = (GpuStaticPropRegistry::getRegistryGeneration() != cached_regGen)
                  | (GpuStaticPropRegistry::getCullRecordVersion()   != cached_cullGen)
  if (!spRowCacheDirty):
    // Fast path: memcpy cache into arena, skip Fill + WriteLoop entirely
    snap.staticProps = arena copy of s_spRowCache
  else:
    // Slow path (mission load / spawn-despawn / light-change / new cull submission):
    rebuild s_spRowCache (existing Fill + WriteLoop code)
    update cached_regGen, cached_cullGen

Dirty signals required
──────────────────────
getRegistryGeneration()      → ALREADY EXISTS (s_registryGeneration, gos_static_prop_registry.cpp:658)
getCullRecordVersion()       → NEW: monotonic counter in gos_static_prop_registry.cpp,
                                   bumped when s_recipeHasSubstrateRecord[i] goes 0→1
                                   (inside the flush submission path at lines 1006 and 1086)
```

---

## 7. Dirty-only replacement design

### Phase 1: cache + compare (slice `STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1`)

- Add `getCullRecordVersion()` to `gos_static_prop_registry.h/.cpp` (monotonic uint64_t)
- Add `s_spRowCache` + generation pair to `render_snapshot.cpp`
- Each frame: **also** build cache (parallel to legacy rebuild), compare field-by-field
- Env gate `MC2_STATIC_PROP_SNAPSHOT_CACHE_COMPARE=1` (default-off; compare only with gate on)
- No behavior change; proves mismatch = 0

### Phase 2: dirty-only fill (slice `STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1`)

- Default: skip `fillStaticPropSlots` + `WriteLoop` on clean generation
- Fast path: `arena.allocArray(N_alive)` + `memcpy(s_spRowCache)` → 1 alloc + ~180KB copy for mc2_01
- Dirty path (mission load): legacy rebuild (existing code unchanged)
- Kill-switch `MC2_STATIC_PROP_SNAPSHOT_FILL_LEGACY=1` restores current rebuild path
- Kill-switch `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE=1` runs old+new and compares rows
- Target: `Extract.SP.Fill` + `Extract.SP.WriteLoop` → 0 on steady-state frames

### Phase 3: dispatch array cache (slice `STATICPROP-SNAPSHOTBUILD-DIRTYONLY-1`, tentative)

- Prerequisite: prove `baseInstance[si]` values are identical across ring slots 0 and 1
  (compare values from slot 0 vs slot 1 for each draw command; expect identity for static-only content)
- If stable: add `s_snapV6PacketsCache` / `s_snapV6MetaCache` to batcher TU
- Default: memcpy cache into dispatch arrays on clean generation (O(N_cmds) × 32B/cmd = ~4KB for 134 cmds)
- Dirty path: rebuild (existing `StaticProp.SnapshotBuild` code unchanged)
- Kill-switch `MC2_STATIC_PROP_SNAPSHOTBUILD_LEGACY=1`
- Target: `StaticProp.SnapshotBuild` → 0 on steady-state frames

---

## 8. Minimal first implementation slice

**`STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1`** — compare-only, no behavior change.

Files:
- `GameOS/gameos/gos_static_prop_registry.h/.cpp`: add `getCullRecordVersion()` + bump point
- `GameOS/gameos/render_snapshot.cpp`: add `s_spRowCache`, build it alongside legacy path, field-by-field compare, emit mismatch counters to snap + log

Shape of compare:
```cpp
// In ExtractRenderSnapshot, after WriteLoop:
if (s_compareCacheEnabled) {
    // Build s_spRowCache using same code, verify:
    for (uint32_t i = 0; i < writeIdx; ++i) {
        const ExtractedStaticProp& live  = propBuf[i];
        const ExtractedStaticProp& cache = s_spRowCache[i];
        // Compare each field; count mismatches per type
        // Emit [SNAPSHOT_BRIDGE_COMPARE v1] event=mismatch / event=match per frame
    }
}
```

Gate: `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE=1` (default-off; CI-registered).

**Stop condition:** if any field other than `hasCullRecord` shows mismatches, stop and diagnose.
Expected: zero mismatches on all immutable fields; `hasCullRecord` mismatches only in the
first ~2 frames after mission load (new props transitioning false→true).

---

## 9. Validation plan

### For `STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1`

- Enable `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE=1`
- Run tier1 5/5; confirm log shows `event=match` every frame after warmup (frame 3+)
- Confirm `hasCullRecord` mismatch count > 0 only on frames 1-2 (expected false→true transition)
- Confirm all other fields: `mismatch=0` throughout
- mc2_01, mc2_10, mc2_17, mc2_24

### For `STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1`

- **Substitutive perf proof** (load-bearing): user-driven Tracy at wolfman/mc2_24
  - `Extract.SP.Fill` → ~0 on steady-state frames (non-zero only on mission-load frame)
  - `Extract.SP.WriteLoop` → ~0 on steady-state frames
  - `ExtractRenderSnapshot` total cost drops accordingly
  - No `Extract.SP.Fill` cost displaced to another zone
- **Correctness**: `ok=1`, zero snapshot mismatches, tier1 5/5 +0 destroys
- **Kill-switch**: `MC2_STATIC_PROP_SNAPSHOT_FILL_LEGACY=1` restores legacy path; same visual output
- mc2_01 / mc2_17 / mc2_24

### For `STATICPROP-SNAPSHOTBUILD-DIRTYONLY-1`

- Prerequisite: compare `baseInstance` values across ring slots (add compare trace, 1 frame)
- **Substitutive**: `StaticProp.SnapshotBuild` → ~0 on steady-state frames
- **Kill-switch**: `MC2_STATIC_PROP_SNAPSHOTBUILD_LEGACY=1`
- GL-clean, tier1 5/5

---

## 10. Risks / stop conditions

| Risk | Severity | Stop condition |
|---|---|---|
| `hasCullRecord` oscillates (goes true→false unexpectedly) | MED | If `getCullRecordVersion` bumps every frame → dirty-only model degrades to per-frame rebuild; investigate `s_recipeHasSubstrateRecord` write path |
| `s_objectRecords` is modified during a frame (concurrently) | LOW | Mutex protects; but fast-path skips the mutex — safe as long as we only read the cache under the assumption that mission is active |
| Arena lifetime: persistent cache pointer vs ping-pong | MED | Solution: always memcpy into arena; never point snap.staticProps at persistent buffer directly |
| `baseInstance` values differ across ring slots | MED | If ring-slot-0 ≠ ring-slot-1 for static props → Phase 3 blocked; Phases 1+2 unaffected |
| `StaticProp.SnapshotBuild` rows needed per-frame by a consumer not identified | LOW | Task 0 recon (done) confirmed no external reader of `v6Packets`/`v6Meta`; but cache-of-cache adds another indirection level to audit |
| Arena size (1 MiB): mc2_10 has 2611 props × 184B = 469KB | LOW | Under 1 MiB; no overflow expected; check `arenaOverflow` gate |
| New prop registered mid-gameplay (late-spawn path via `adoptStaticPropRecipe`) | LOW | Late-spawn bumps `s_registryGeneration` → dirty flag fires → cache rebuilt | 
| `staticPropGetMaterialCacheInfo` returns false (no primary material) | LOW | Already handled in legacy path (`hasPrimary` guard); cache must preserve this exclusion |
| Compare slice changes extract timing (cache build is additive) | LOW | COMPARE slice is additive (both paths run); use `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE=0` default; gate explicitly |

**Hard stops:**

- Any field in `ExtractedStaticProp` that consistently mismatches AFTER mission-load warmup AND cannot be explained by a known dirty signal → do NOT proceed to Phase 2
- `hasCullRecord` oscillating (going true→false) → investigate before proceeding (reset path exists in invalidateByRecipe; check if it's called mid-gameplay)
- `s_registryGeneration` bumping every frame → dirty-only model worthless → investigate unexpected writes
- `baseInstance` values diverging across ring slots for static-only content → Phase 3 blocked; no stop for Phases 1+2

---

## 11. Final recommendation

**Proceed with `STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1`** (compare slice, no behavior change):
- Low risk: no dispatch path touched, additive-only
- Proves the invariant before committing to the dirty-only model
- Delivers `getCullRecordVersion()` infra used in Phase 2
- Expected result: proves mismatch = 0 for all immutable fields on steady-state frames

**Then `STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1`** (after compare proves clean):
- Expected to retire ~1.13ms + ~157µs = ~1.29ms/frame on steady-state frames
- Mission-load frame still runs the legacy rebuild (acceptable; mission loads are infrequent)
- Kill-switch provides A/B at any time

**Then `STATICPROP-SNAPSHOTBUILD-DIRTYONLY-1`** (after Phase 2 soaks and `baseInstance` stability verified):
- Expected to retire ~383µs/frame (additional)
- Combined expected retirement: ~1.67ms/frame from `ExtractRenderSnapshot` zone

**Standing follow-up (do NOT conflate):**
- `STATICPROP-BLDG-TYPE-ANIM-GATE-FIX-1`: idle animatable building types wrongly
  disqualified from the static fast path. Separate lane. Keep on active follow-up list.
- `GameLogic.Units.TerrainObjects` (~1.44ms): separate high-priority lane; not in scope here.
- 3/4/5 GPU-Scene flip: owner-gated; out of scope.

---

## 12. Existing persistent infrastructure (no duplication needed)

These already exist after the static-prop CPU campaign; the dirty-only model builds on them:

| Infrastructure | Status | Relevant to |
|---|---|---|
| `s_registryGeneration` / `getRegistryGeneration()` | LIVE in `gos_static_prop_registry.cpp:658` | Primary dirty signal for Fill + WriteLoop |
| Persistent light slots (permanent index = recipeIndex) | PROVED (Slice 1) | `lightDataIndex` cacheable trivially |
| Persistent static instance store (2b Stage 2, `MC2_STATIC_PROP_PERSISTENT_BUCKETS`) | DEFAULT-ON | `instanceCount` in dispatch arrays stable |
| Cached blob flush (2a, `MC2_STATIC_PROP_FLUSH_CACHED_BLOB`) | DEFAULT-OFF (soak) | Parallel to snapshot cache; does not conflict |
| `s_snapV6Packets`/`s_snapV6Meta` dispatch arrays | LIVE (v8 = sole owner) | Phase 3 dispatch cache |
| Snapshot ok gate / `spBuildRetired` / `spBuildFallback` | LIVE (v8) | Validation harness |
| Compare-oracle infrastructure (`MC2_STATIC_PROP_LIVE_BUILDER=1`) | LIVE (kill-switch) | Independent A/B for dispatch; not snapshot fill |

**New infra needed:**
- `getCullRecordVersion()` in `gos_static_prop_registry.h/.cpp` — 5-line addition
- `s_spRowCache` + generation pair in `render_snapshot.cpp` — persistent mirror
- Dirty-check predicate in `ExtractRenderSnapshot` — 4-line check before Fill
