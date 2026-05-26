# DrawPacket v4B Recon Report
Date: 2026-05-26
Author: recon agent

## Scope

This document answers: "what exactly is inside the coalesce alpha-off dispatch
bucket, and can we map it to DrawPacket candidates?" It is read-only recon;
no code changes are proposed here.

---

## 1. `s_alphaOffCmdCount` and `s_alphaOnCmdCount` — what they are

**Declarations (gos_static_prop_batcher.cpp L329-330):**

```cpp
std::vector<uint32_t> s_sortedPacketOrder;   // global packet indices in [OFF | ON] order
uint32_t s_alphaOffCmdCount = 0;             // number of packets in alpha-OFF group
uint32_t s_alphaOnCmdCount  = 0;             // number of packets in alpha-ON group
```

These are **packet counts**, not type counts. One "cmd" (draw command) corresponds
to one `GpuStaticPropPacket` (one geometry range, one texture slot), not one type.
A type with `packetCount=3` contributes 3 commands.

The values are set in **`finalizeGeometry()`** (L2102-2136) during the per-packet
sort. They are cleared at `onMapUnload()` (L1175-1176).

There is **no `s_alphaOffCmds[]` array**. The "commands" are:

1. The GPU-side indirect command buffer (`gpu_cull::compute_getIndirectCmdBuf()`),
   which is a GL buffer of `DrawElementsIndirectCommand` structs (one per sorted
   packet slot). The indirect buffer is NOT a CPU-side array; it is written by the
   GPU compute cull pass.
2. The CPU-side `s_sortedPacketOrder[]` array (L328), which maps draw slot index
   to `globalPacketIdx`. This is the only CPU-visible per-command data.

`s_alphaOffCmdCount` is the count of entries in `s_sortedPacketOrder` belonging
to the alpha-OFF group. The alpha-ON entries follow immediately, and
`s_alphaOnCmdCount` gives their count. Sum = total indirect draws.

---

## 2. Command layout — what each command contains (CPU-visible)

Each slot `i` in the coalesce draw corresponds to:

- **`s_sortedPacketOrder[i]`** — the `globalPacketIdx` (an index into `s_packets[]`)
- **`s_perDrawSsbo` slot `i`** — a `PerDrawEntry` struct (32 bytes, std430):

```cpp
struct PerDrawEntry {
    int32_t packetID;          // = globalPacketIdx (L51, gos_static_prop_batcher.h)
    int32_t materialFlags;     // 0 or STATIC_PROP_FLAG_ALPHA_TEST
    int32_t maxLocalVertexID;  // type.vertexCount - 1
    int32_t texArrayLayer;     // group-relative layer in s_texArrayOff/On
    float   uvScaleX;
    float   uvScaleY;
    int32_t objectIdRaw;       // M1.5 handle bits
    uint32_t materialIdx;      // MaterialGpu-3 table index
};
```

So each slot `i` contains:
- The owning packet's `globalPacketIdx` (via both `s_sortedPacketOrder[i]` and
  `PerDrawEntry::packetID`)
- The owning type's `typeID` (via `s_cmdToBucketSsbo[i]`, L331 + L2273-2278)
- Alpha classification (materialFlags)

**Per-packet mapping is fully feasible** from the CPU side:
`s_sortedPacketOrder[i]` gives `globalPacketIdx`, which can be looked up in
`s_packets[]` for geometry and in `s_types[pkt.owningTypeID]` for type-level data.

---

## 3. Where alpha-off commands are built

**`finalizeGeometry()`, L2102-2386.** This function runs once per map load.

The build sequence:

1. **L2107-2136**: Double loop over `[group=0 (OFF), group=1 (ON)]` × sorted types.
   For each type's packets, skip those with `layerForPacket[globalPktIdx] < 0`
   (texture unavailable — e.g., damage shapes not yet loaded). The remainder
   are pushed to `s_sortedPacketOrder`.
   `s_alphaOffCmdCount` is latched after the group=0 sweep (L2132).

2. **L2272-2385**: The `PerDrawEntry` array is built from `s_sortedPacketOrder`,
   indexed in draw-slot order. The `cmdToBucket` array (= typeID per slot) is
   also built here. Both are uploaded as GL_STATIC_DRAW SSBOs.

The alpha-ON/OFF group boundary is purely from each type's **`alphaClass`** field
(0=alpha-OFF, 1=alpha-ON), which is an OR-reduce over all packets' alpha-test bits
for that type (Step 5.NEW, L1536-1568). A mixed-alpha type (some packets opaque,
some alpha-test) gets `alphaClass=1` and ALL its packets go to the alpha-ON group.

---

## 4. Per-type submits in coalesce mode — the draw loop

Coalesce draw (L3676-3936) issues two `glMultiDrawElementsIndirect` calls:

1. **Alpha-OFF draw** (L3871-3892): issues `s_alphaOffCmdCount` commands starting
   at offset 0 in the indirect buffer. Binds `s_texArrayOff`.
2. **Alpha-ON draw** (L3895-3913): issues `s_alphaOnCmdCount` commands starting
   at byte offset `s_alphaOffCmdCount * kDrawElementsIndirectCommandSize`.
   Binds `s_texArrayOn`.

The fragment shader reads `entries[gl_DrawID + u_drawIDBase]` where
`u_drawIDBase=0` for alpha-OFF and `u_drawIDBase=s_alphaOffCmdCount` for alpha-ON.
So `gl_DrawID` is the within-group draw index, not the global slot index.

There is **no per-type loop** in the coalesce path. All draws are issued in two
multidraw calls. The type granularity is only expressed via the sorted order
(`s_sortedTypeOrder` determines which packet slots land in which group).

---

## 5. Mixed-alpha types in the coalesce alpha-off bucket

**Mixed-alpha types (alphaClass=1) are EXCLUDED from the alpha-OFF bucket.**

The sort at L2120-2136 uses `type.alphaClass != group` to skip. Since alphaClass
is an OR-reduce at the type level:
- A type where ANY packet has `textureAlpha=true` → `alphaClass=1` → ALL its
  packets land in the alpha-ON group.
- The alpha-OFF group contains ONLY types where ALL packets are purely opaque.

This is the same alpha-class used by the v4A dispatch gate at L3390:
`if (s_types[v.typeId].alphaClass != 0u) { ++v4MixedTypeDeferred; continue; }`

So the emitter's `pipelineId=StaticPropOpaque` candidates (alphaClass=0 types)
map exactly to the coalesce alpha-OFF packet set.

---

## 6. `s_typeRanges` in the coalesce path

`s_typeRanges` is an `unordered_map<uint32_t, TypeRangeSsbo>` built by
`uploadAllBucketsIfNeeded()` (L3138-3163). It is populated **unconditionally**
every frame (both coalesce and legacy paths call `uploadAllBucketsIfNeeded()`
at flush time).

However, `s_typeRanges` is used by:
- The v4A dispatch block (L3396-3399) — non-coalesce only
- The legacy per-type draw loop (L3949-3950) — non-coalesce only

In the coalesce path (L3676+), `s_typeRanges` is NOT consulted. The coalesce
draw uses `s_coalesceInstanceSsbo` with group-level `glBindBufferRange` calls.
So `s_typeRanges` exists in coalesce mode but is unused for drawing.

---

## 7. Candidate-to-command mapping feasibility

The emitter (draw_packet_emitter.cpp) produces `StaticPropDrawPacketCandidate`
entries with fields:
- `typeId` — matches `s_packets[globalPacketIdx].owningTypeID`
- `globalPacketIdx` — directly indexes `s_sortedPacketOrder` and `PerDrawEntry::packetID`
- `alphaPass` — 0=opaque (alphaClass=0 type), maps to coalesce alpha-OFF group
- `instanceCount` — from snapshot count for this typeId

The coalesce alpha-OFF draw command set (the first `s_alphaOffCmdCount` entries
of `s_sortedPacketOrder`) is exactly the set of packets from types with
`alphaClass=0` that had a valid `layerForPacket >= 0` at `finalizeGeometry()`.

**The skip rule (layerForPacket < 0)** is the only divergence: damage-shape
packets that are skipped from `s_sortedPacketOrder` will never appear as an
indirect command. The emitter (v0/v1) does not model this skip; it walks all
packets via `desc.firstPacket..firstPacket+packetCount`. So:

- `emitter.emitted` (alpha-OFF type packets) may be >= `s_alphaOffCmdCount`
  if any damage-shape packets were skipped.
- In practice on non-destroyed maps: all packets have `layerForPacket >= 0`
  so the sets should be equal.

---

## 8. Proposed v4B log format for coalesce coverage compare

```
[DRAW_PACKET_COALESCE_COMPARE v1] frame=<N>
  event=coverage_check
  coalesce_off_cmds=<s_alphaOffCmdCount>
  coalesce_on_cmds=<s_alphaOnCmdCount>
  candidate_off_pkts=<emitter alpha-OFF candidate count>
  candidate_on_pkts=<emitter alpha-ON candidate count>
  skip_delta_off=<coalesce_off_cmds - candidate_off_pkts>
  skip_delta_on=<coalesce_on_cmds - candidate_on_pkts>
  ok=<1 if both deltas <= 0 and candidate counts >= coalesce counts, else 0>
```

**Invariant to check:**
`candidate_off_pkts >= coalesce_off_cmds` (coalesce set is a subset of emitter
set — equality when no damage-shape skips are active).

A `skip_delta_off < 0` (coalesce has MORE commands than candidates) would
indicate an emitter bug. A positive delta is the damage-shape skip signature.

Per-slot comparison can also log:
```
[DRAW_PACKET_COALESCE_COMPARE v1] event=slot_detail
  slot=<i>
  coalesce_pkt=<s_sortedPacketOrder[i]>
  candidate_pkt=<matching globalPacketIdx from emitter>
  match=<1/0>
```
This requires the emitter to produce one candidate per sorted-order slot,
which it does not currently (emitter walks type-desc order, not packet-sort
order). To do per-slot compare, either the emitter must be reordered to
emit in `s_sortedPacketOrder` order, or a post-emit sort step is needed.

**Recommendation for v4B**: implement the summary-level compare (counts only)
first. Per-slot compare requires emitter reordering and can be a v4B follow-up.

---

## 9. Summary of key data structures for v4B

| Name | Type | Location | Description |
|---|---|---|---|
| `s_sortedPacketOrder` | `vector<uint32_t>` | L328 | globalPacketIdx per draw slot, [OFF\|ON] order |
| `s_alphaOffCmdCount` | `uint32_t` | L329 | # packets in alpha-OFF group (= # alpha-OFF draw cmds) |
| `s_alphaOnCmdCount` | `uint32_t` | L330 | # packets in alpha-ON group |
| `s_perDrawSsbo` | `GLuint` | (unnamed) | PerDrawEntry per draw slot (mission-lifetime GL_STATIC_DRAW) |
| `PerDrawEntry::packetID` | `int32_t` | .h L52 | = globalPacketIdx for slot |
| `s_cmdToBucketSsbo` | `GLuint` | L331 | typeID per draw slot (binding 7) |
| `batcher_getAlphaOffCmdCount()` | function | .h L337 | public accessor |
| `batcher_getSortedPacketOrder()` | function | .h L335 | public accessor |
| `batcher_getSortedPacketCount()` | function | .h L336 | public accessor |

All public via `.h` — v4B log code in `gameosmain.cpp` can call these without
modifying the batcher TU.
