# Extraction v3 Design: Snapshot-Owned Slot Identity

**Date:** 2026-05-26
**Slice kind:** dispatch-changing (gated) — snapshot arrays drive actual `glDraw*` calls when compare clean; live fallback on mismatch. Not observational/diagnostic.
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

Staged — counters recorded at each fail point, not swallowed by a single boolean.

```cpp
// Stage 0: env gate
s_spBuildAttempted = s_snapshotBuildEnabled ? 1u : 0u;
if (!s_snapshotBuildEnabled) { /* dispatch live */ }

// Stage 1: snap-cull collision guard
// If MC2_SNAP_CULL=1 is also active, disable v3 builder; dispatch live.
if (s_snapCullEnabled) {
    // log: [RENDER_SNAPSHOT v3] disabled — MC2_SNAP_CULL collision
    /* dispatch live */
}

// Stage 2: structural guards (no counter increment — snap is unusable)
if (snap == nullptr || snap->ok != 1u || snap->staticPropPackets.data == nullptr)
    { /* dispatch live */ }

// Stage 3: v2.2 clean — snapshot ordering is structurally valid
if (snap->spCountMismatch      != 0u ||
    snap->spSortedSlotMismatch != 0u ||
    snap->spGlobalPacketMismatch != 0u ||
    snap->spPipelineMismatch   != 0u ||
    snap->spMaterialIdxMismatch != 0u ||
    snap->spTexLayerMismatch   != 0u)
    { /* dispatch live */ }

// Stage 4: count check — record mismatch if gate would otherwise fire
if (snap->staticPropPackets.count != totalCmds) {
    ++s_spBuildCountMismatch;
    ++s_spBuildFallback;   // attempted but not dispatched
    /* dispatch live */
}

// → proceed to snapshot builder
```

Note: `snap->ok == 1u` (Stage 2) includes the previous flush's `spBuild*` mismatch
counters. A mismatch frame sets ok=0, builder skips next frame, restoring ok=1 the
frame after. Oscillation is acceptable — live arrays always dispatched on mismatch,
pattern visible in logs.

---

## Snapshot Builder Loop

All v3 batcher counters are reset to 0 at the start of each flush (before
this loop runs), so stale stats from a prior frame never persist:

```cpp
s_spBuildAttempted = s_spBuildCountMismatch = s_spBuildPacketMismatch =
s_spBuildMetaMismatch = s_spBuildFallback = 0u;
```

Per-slot loop:

```
for i in 0..totalCmds:
    row = snap->staticPropPackets.data[i]

    // Snapshot owns slot identity — row must claim this exact slot.
    if row.sortedSlot != i:
        ++spBuildMetaMismatch
        zero-fill s_snapV6Meta[i], s_snapV6Packets[i]
        continue

    // OOB guards — each increments its mismatch counter immediately.
    if row.globalPacketIdx >= s_packets.size():
        ++spBuildPacketMismatch
        zero-fill; continue

    pkt = s_packets[row.globalPacketIdx]

    // Guard s_typeRanges (the array actually indexed for instanceCount).
    if row.typeId >= s_typeRanges.size():
        ++spBuildMetaMismatch
        zero-fill; continue

    group = pipelineId_to_group(row.pipelineId)
    if group == INVALID:
        ++spBuildMetaMismatch
        zero-fill; continue

    instCount = s_typeRanges[row.typeId].instanceCount   // current-frame live
    baseInst  = baseInstanceMap[i]                        // per-frame live

    s_snapV6Meta[i] = {
        .sortedSlot      = row.sortedSlot,    // from snapshot, verified == i above
        .globalPacketIdx = row.globalPacketIdx,
        .typeId          = row.typeId,
        .group           = group,
        .instanceCount   = instCount,
        .baseInstance    = baseInst,
        .drawIDBase      = i,                 // always == i for both paths
        .baseVertex      = pkt.baseVertex,
    }

    // Only dispatch-consumed DrawPacket fields are set; remaining fields
    // (mesh, material, objectIndex, lightIndex, sortKey) are not consumed
    // by the static-prop v6 dispatch loop and are zero-initialized.
    s_snapV6Packets[i] = {
        .pipelineId  = static_cast<uint32_t>(group == 0
                           ? RenderCore::PipelineId::StaticPropOpaque
                           : RenderCore::PipelineId::StaticPropAlphaTest),
        .firstIndex  = pkt.firstIndex,
        .indexCount  = pkt.indexCount,
        // all other fields = {}  (not read by dispatch loop)
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
uint32_t spBuildAttempted      = 0u;  // 1 if gate check ran this flush
uint32_t spBuildCountMismatch  = 0u;  // snap.count != totalCmds
uint32_t spBuildPacketMismatch = 0u;  // DrawPacket field divergence (accumulated)
uint32_t spBuildMetaMismatch   = 0u;  // DispatchMeta field divergence (accumulated)
uint32_t spBuildFallback       = 0u;  // gate enabled/attempted but snapshot arrays NOT dispatched
```

`spBuildFallback` semantics (Option A): incremented whenever `spBuildAttempted==1`
but live arrays were dispatched — covers count mismatch, invalid snap, OOB rows,
unknown pipeline, packet/meta mismatch. "attempted + fallback" in logs means "v3
did not dispatch this frame."

These are previous-flush stats written back to the snapshot the same way
`spSnapCull*` counters are. The mismatch subset participates in the next
snapshot's `ok` gate; `spBuildAttempted` and `spBuildFallback` are informational.

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
// snapBuilt = true only if activation guard fully passed AND builder loop ran.
// All early-exit paths (count mismatch, null snap, OOB rows) already incremented
// s_spBuildFallback before reaching here.

const bool useSnapshot = snapBuilt
    && s_spBuildPacketMismatch == 0
    && s_spBuildMetaMismatch   == 0;

if (snapBuilt && !useSnapshot) {
    ++s_spBuildFallback;  // compare failed after builder ran
    // first-fallback log (always):
    // [RENDER_SNAPSHOT v3] attempted=1 count_mismatch=0 pkt_mismatch=N meta_mismatch=N fallback=1
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

**NOT gated on `s_v6TraceEnabled`** — that gate fires per draw-slot (134+ lines/frame) and would
DOS logs. Instead emit the v3 summary line:
- Always, on first fallback/mismatch frame (unconditional)
- Every 600 frames when `spBuildAttempted == 1` (rate-limited steady-state)
- Never per-slot

Per-slot verbose tracing remains under the existing `s_v6TraceEnabled` gate and is unaffected.

---

## Files Touched

| File | Change |
|------|--------|
| `GameOS/gameos/render_snapshot.h` | Add 5 `spBuild*` fields (v3 block); update ok gate comment |
| `GameOS/gameos/render_snapshot.cpp` | Read build stats from batcher; extend ok gate; bump log label v2.3→v3 |
| `GameOS/gameos/gos_static_prop_batcher.h` | Declare accessor(s) for v3 build stats; keep `s_snapshotBuildEnabled` file-local in `.cpp` |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | `s_snapV6Packets/Meta` statics; init env gate; snapshot builder loop; compare; dispatch ref-swap |
| `docs/tier1_env_vars.md` | Add `MC2_SNAPSHOT_STATIC_PROP_BUILD` entry (parallel to `MC2_SNAP_CULL`) |

**Not touched:** shadow pass, `prepareBaseInstanceTable`, any other GPU path.

---

## Snap-Cull Collision (MC2_SNAP_CULL + MC2_SNAPSHOT_STATIC_PROP_BUILD)

If both env vars are set, the v3 builder is disabled for that flush — v3 builder
dispatches live, emits one log line:

```
[RENDER_SNAPSHOT v3] disabled — MC2_SNAP_CULL collision
```

`spBuildAttempted` is still set to 1; `spBuildFallback` increments.
This keeps behavior small and bisectable. Joint enablement is defined but
produces live dispatch — not undefined behavior.

## Smoke Gate

| Tier | Env | Expected |
|------|-----|----------|
| Tier1 default | `MC2_SNAPSHOT_STATIC_PROP_BUILD` unset | ok=1, all `spBuild*`=0, no regression |
| Tier1 opt-in | `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` | ok=1, `spBuildFallback`=0, mismatch counters=0 |
| Tier1 collision | `MC2_SNAPSHOT_STATIC_PROP_BUILD=1 MC2_SNAP_CULL=1` | ok=1, `spBuildAttempted`=1, `spBuildFallback`>0, no crash |

All three must be 5/5 PASS before merge. Default must pass unconditionally —
five new fields zero when gate off is the invariant.

---

## Anti-Goals

- No change to shadow pass
- No new snapshot fields beyond the five `spBuild*` above
- No default-ON flip in v3 — compare-first, always
- No removal of live builder in v3 — it is the fallback
- No factoring of dispatch loop into a helper in v3 (defer to v3.1+ if soaked)
