# HANDOFF 2026-05-26: Extraction v2.2 SHIPPED — dispatch-fact compare gates green

## Status

SHIPPED. HEAD `76f63b3f`. Tier1 5/5 PASS.

## What was done

Extraction v2.2 proves every per-packet snapshot row matches live batcher dispatch
facts (sortedSlot, globalPacketIdx, pipelineId, materialIdx, texArrayLayer).

Key additions:
- `texArrayLayer` field added to `ExtractedStaticPropPacket` (struct now 28B)
- `pipelineId` fixed to use `RenderCore::PipelineId` enum values (1=Opaque, 2=AlphaTest)
- `batcher_compareSnapshotPackets(RenderSnapshot*)` free function in batcher.cpp
- 9 new `RenderSnapshot` fields: `spCompare*` + `spInstanceCountMismatch`
- `ok` gate extended: adds spCountMismatch, spSortedSlotMismatch, spGlobalPacketMismatch,
  spPipelineMismatch, spMaterialIdxMismatch, spTexLayerMismatch
- `spInstanceCountMismatch` informational only (prev-frame vs current-frame, excluded from gate)
- Log tag updated to `[RENDER_SNAPSHOT v2.2]`

## Smoke result

```
sp_packets=134 ok=1
[v2.2 compare] snapshot_count=134 live_count=134 count_mismatch=0
sorted_slot_mismatch=0 global_packet_mismatch=0 pipeline_mismatch=0
material_idx_mismatch=0 instance_count_mismatch=0 tex_layer_mismatch=0
```

All 5 missions (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24): ok=1, all structural counters 0.

## What v2.2 proves

The snapshot is trusted for:
- Draw-slot identity (sortedSlot)
- Packet index (globalPacketIdx)
- Pipeline assignment (pipelineId via RenderCore::PipelineId)
- Material index (materialIdx sidecar)
- Texture array layer (texArrayLayer vs albedoTex)

Not yet proven:
- baseInstance (deferred to v2.3 — produced by prepareBaseInstanceTable after extraction)

Not yet consumed:
- DrawPacket/meta build from snapshot (deferred to v3 authority flip)
- Snap-cull (uses snapshot instanceCounts) (v2.3)

## Roadmap clarification

v2.2 shipped: snapshot proven for identity/material/pipeline/layer facts.
Snapshot is NOT yet consumed for DrawPacket/meta build.
Snap-cull not yet shipped (v2.3 is next).

Sequence:
- v2.2 (done): compare gates prove snapshot rows are correct
- v2.3 (next): snap-cull, opt-in — use snapshot instanceCounts to skip zero-instance slots
- v3: authority flip — v6 builder consumes snapshot rows instead of live batcher state

## Commit chain

- `cdb6ef70` — T1: texArrayLayer + pipelineId fix
- `58c01c22` — T1 fix: stale pipelineId comment
- `20afbbeb` — T2: spCompare* fields
- `8290f7da` — T3: batcher_compareSnapshotPackets
- `4552d507` — T4: wire compare + ok gate + log v2.2
- `636c3c4d` — T4 fix: arena overflow warning tag
- `76f63b3f` — T4 fix: ok gate comment in header

## Files modified

- `GameOS/gameos/render_snapshot.h`
- `GameOS/gameos/render_snapshot.cpp`
- `GameOS/gameos/gos_static_prop_batcher.h`
- `GameOS/gameos/gos_static_prop_batcher.cpp`
