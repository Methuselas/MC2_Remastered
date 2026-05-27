# Extraction v3 Design: Snapshot-Owned Slot Identity

**Date:** 2026-05-26
**Gate:** `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` (default OFF)
**Prerequisite HEAD:** `88379448` (v2.3 shipped, tier1 5/5 PASS)

---

## Authority Boundary

v3 does not make the snapshot own current-frame execution facts. It makes the
snapshot own static-prop draw-slot identity.

**Snapshot owns:**
- `sortedSlot` — draw-order index
- `globalPacketIdx` — packet index into `s_packets[]`
- `typeId` — owning type
- `pipelineId` / `group` — alpha split

**Live state remains authoritative for:**
- Current-frame `instanceCount` (from `s_typeRanges`, built fresh each flush)
- `baseInstance` (from `prepareBaseInstanceTable()`, per-frame ring-buffer)
- Geometry lookup (`firstIndex`, `indexCount`, `baseVertex`) via `s_packets[row.globalPacketIdx]`

This boundary is intentional. Geometry is static after `finalizeGeometry()` so
reading it via snapshot `globalPacketIdx` yields identical results to the live
path, but `instanceCount` and `baseInstance` are current-frame and must not be
snapshotted for dispatch.

---

## New Static Storage

Added to `gos_static_prop_batcher.cpp` (file-scope statics):

```cpp
static std::vector<RenderCore::DrawPacket> s_snapV6Packets;
static std::vector<StaticPropDispatchMeta> s_snapV6Meta;
static bool s_snapshotBuildEnabled = false;  // MC2_SNAPSHOT_STATIC_PROP_BUILD=1
```

`s_snapshotBuildEnabled` is read once at init (same pattern as `s_snapCullEnabled`).
Vectors are `resize(totalCmds)`-reused each frame — no heap alloc after first frame.

---

## flush() Sequence

```
1. Build live arrays (existing: v6Packets, v6Meta)     ← unchanged
2. [IF snapshot gate active]
   Build snapshot arrays (s_snapV6Packets, s_snapV6Meta)
3. [IF snapshot arrays built]
   Compare field-by-field, accumulate mismatch counters
4. Dispatch:
   - If compare clean → dispatch s_snapV6Packets / s_snapV6Meta
   - Else → log mismatch, dispatch v6Packets / v6Meta (live), ++spBuildFallback
```

Live arrays are always built first. The snapshot builder is additive and
never modifies the live arrays.

---

## Snapshot Builder Activation Guard

All conditions must hold; any failure skips the snapshot builder entirely.

Note: `snap->ok == 1u` includes the previous flush's `spBuild*` counters in
its gate. A mismatch frame sets ok=0, causing the builder to skip the next
frame (live dispatched, spBuild* stay zero), restoring ok=1 the frame after.
This oscillation is acceptable for v3 — live arrays are always dispatched on
any mismatch, and the pattern is visible in logs.

```cpp
s_snapshotBuildEnabled
&& snap != nullptr
&& snap->ok == 1u
&& snap->spCountMismatch      == 0u   // v2.2 structural
&& snap->spSortedSlotMismatch == 0u
&& snap->spGlobalPacketMismatch == 0u
&& snap->spPipelineMismatch   == 0u
&& snap->spMaterialIdxMismatch == 0u
&& snap->spTexLayerMismatch   == 0u
&& snap->staticPropPackets.data != nullptr
&& snap->staticPropPackets.count == totalCmds
```

Count mismatch (`snap->staticPropPackets.count != totalCmds`) → increment
`spBuildCountMismatch`, skip builder, dispatch live.

`spBuildAttempted` is set to `1u` whenever the gate check begins (even if
count mismatch fires). This distinguishes "gate off" from "gate on but no
valid snap" in logs.

---

## Snapshot Builder Loop

```
for i in 0..totalCmds:
    row = snap->staticPropPackets.data[i]

    guard: row.globalPacketIdx >= s_packets.size() → packet_oob, zero-fill, continue
    pkt = s_packets[row.globalPacketIdx]

    guard: row.typeId >= s_types.size() → type_oob, zero-fill, continue

    group = pipelineId_to_group(row.pipelineId)
    if group == INVALID → meta_mismatch++, zero-fill, continue

    instCount = s_typeRanges[row.typeId].instanceCount   // current-frame live
    baseInst  = baseInstanceMap[i]                        // per-frame live

    s_snapV6Meta[i] = {
        .sortedSlot      = i,
        .globalPacketIdx = row.globalPacketIdx,
        .typeId          = row.typeId,
        .group           = group,
        .instanceCount   = instCount,
        .baseInstance    = baseInst,
        .drawIDBase      = i,
        .baseVertex      = pkt.baseVertex,
    }

    s_snapV6Packets[i] = {
        .pipelineId  = row.pipelineId (cast to uint32_t),
        .firstIndex  = pkt.firstIndex,
        .indexCount  = pkt.indexCount,
        ...other fields = {}
    }
```

### `pipelineId_to_group()` — fail closed

```cpp
// Returns UINT32_MAX on unknown pipelineId.
static uint32_t pipelineId_to_group(uint32_t pid) {
    using P = RenderCore::PipelineId;
    if (pid == static_cast<uint32_t>(P::StaticPropOpaque))     return 0u;
    if (pid == static_cast<uint32_t>(P::StaticPropAlphaTest))  return 1u;
    return 0xFFFFFFFFu;  // unknown → invalid
}
```

Unknown pipelineId increments `spBuildMetaMismatch` and zero-fills the slot.
Do not infer group from `!= opaque`.

---

## Compare Fields

Compare runs after both arrays are built, per slot:

### `spBuildPacketMismatch` accumulates:
- `snap_pkt.pipelineId != live_pkt.pipelineId`
- `snap_pkt.firstIndex  != live_pkt.firstIndex`  (consequence of globalPacketIdx — sanity)
- `snap_pkt.indexCount  != live_pkt.indexCount`  (same)

### `spBuildMetaMismatch` accumulates:
- `snap_meta.globalPacketIdx != live_meta.globalPacketIdx`
- `snap_meta.typeId          != live_meta.typeId`
- `snap_meta.group           != live_meta.group`
- `snap_meta.sortedSlot      != live_meta.sortedSlot`
- `snap_meta.baseVertex      != live_meta.baseVertex`
- `snap_meta.drawIDBase      != live_meta.drawIDBase`

**Not compared:** `instanceCount` (current-frame, identical source in both paths),
`baseInstance` (per-frame, identical source). Comparing them would be tautological.

---

## New RenderSnapshot Fields

```cpp
// --- v3: snapshot build stats (previous-flush; only active when gate on) ---
uint32_t spBuildAttempted      = 0u;  // 1 if snapshot build gate check ran
uint32_t spBuildCountMismatch  = 0u;  // snap.count != totalCmds
uint32_t spBuildPacketMismatch = 0u;  // DrawPacket field divergence (accumulated)
uint32_t spBuildMetaMismatch   = 0u;  // DispatchMeta field divergence (accumulated)
uint32_t spBuildFallback       = 0u;  // frames where live arrays were dispatched
```

These are previous-flush stats written back to the snapshot the same way
`spSnapCull*` counters are — they gate the NEXT snapshot's `ok`.

### ok gate additions

```
ok requires (in addition to existing v2.2/v2.3 gates):
  spBuildCountMismatch  == 0
  spBuildPacketMismatch == 0
  spBuildMetaMismatch   == 0
```

`spBuildFallback` is **informational only** — not in ok gate. If a fallback
happened because of a mismatch, the mismatch counter already gates `ok=0`. A
standalone fallback without mismatch should not be possible by construction.

`spBuildAttempted` is informational.

When `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` (default), all five counters stay zero → ok
unaffected.

---

## Dispatch

```cpp
const bool useSnapshot = snapBuilt
    && spBuildPacketMismatch == 0
    && spBuildMetaMismatch   == 0;

if (!useSnapshot && snapBuilt) {
    ++s_snapBuildFallback;
    // log: [RENDER_SNAPSHOT v3] fallback pkt_mismatch=N meta_mismatch=N
}

const auto& dispatchPackets = useSnapshot ? s_snapV6Packets : v6Packets;
const auto& dispatchMeta    = useSnapshot ? s_snapV6Meta    : v6Meta;

// Existing dispatch loop unchanged — iterate dispatchPackets/dispatchMeta.
```

No new helper function required for v3. The reference swap keeps the loop
intact and review simple.

---

## Log Tag

```
[RENDER_SNAPSHOT v3] attempted=1 count_mismatch=0 packet_mismatch=0 meta_mismatch=0 fallback=0
```

Emitted each flush when `spBuildAttempted == 1` and `s_v6TraceEnabled` is set,
or always to stderr on first fallback.

---

## Files Touched

| File | Change |
|------|--------|
| `GameOS/gameos/render_snapshot.h` | Add 5 `spBuild*` fields (v3 block); update ok gate comment |
| `GameOS/gameos/render_snapshot.cpp` | Read build stats from batcher; extend ok gate; bump log label v2.3→v3 |
| `GameOS/gameos/gos_static_prop_batcher.h` | Declare `s_snapshotBuildEnabled` or accessor; update log-version constant |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | `s_snapV6Packets/Meta` statics; init env gate; snapshot builder loop; compare; dispatch ref-swap |

**Not touched:** shadow pass, `prepareBaseInstanceTable`, any other GPU path.

---

## Smoke Gate

| Tier | Env | Expected |
|------|-----|----------|
| Tier1 default | `MC2_SNAPSHOT_STATIC_PROP_BUILD` unset | ok=1, all `spBuild*`=0, no regression |
| Tier1 opt-in | `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` | ok=1, `spBuildFallback`=0, mismatch counters=0 |

Both must be 5/5 PASS before merge. Default must pass unconditionally —
the five new fields being zero when gate is off is the invariant.

---

## Anti-Goals

- No change to shadow pass
- No new snapshot fields beyond the five `spBuild*` above
- No default-ON flip in v3 — compare-first, always
- No removal of live builder in v3 — it is the fallback
- No factoring of dispatch loop into a helper in v3 (defer to v3.1+ if soaked)
