# HANDOFF 2026-05-26 NEXT-SLICE: Extraction v3 — StaticProp DrawPacket/meta build from RenderSnapshot

## Status

Forward-looking handoff. All prerequisites live in `claude/nifty-mendeleev` HEAD `88379448`.
v2.3 soaked (tier1 5/5 PASS default + opt-in). v3 NOT yet started.

## What v3 must do

Build `v6Packets` + `StaticPropDispatchMeta` from RenderSnapshot rows instead of live batcher
state. v6 builder currently reads directly from `s_packets[]`, `s_sortedPacketOrder[]`,
`s_packetMaterialIdx[]`, etc. v3 moves that read to the snapshot.

**Gate:** `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` (default OFF). Strictly `v && v[0]=='1'`.

**Activation requires:**
- `s_snapshotBuildEnabled` (env gate)
- `snap != nullptr && snap->ok == 1u`
- `snap->staticPropPackets.count == totalCmds`

**On mismatch (compare fails):** fall back to live builder, emit mismatch log. Do NOT dispatch
corrupt data.

## Proposed v3 compare strategy

1. Build v6Packets + StaticPropDispatchMeta from snapshot rows
2. Build same from live batcher (existing path)
3. Compare field-by-field per slot
4. If compare passes → dispatch snapshot-built; else → dispatch live-built + log mismatch

New `RenderSnapshot` counters:
- `spBuildCountMismatch` — snapshot row count vs totalCmds
- `spBuildPacketMismatch` — packet field divergence count
- `spBuildMetaMismatch` — meta field divergence count
- `spBuildFallback` — frames where fallback fired

All in ok gate (must be zero for ok=1).

## What snapshot rows provide

From `ExtractedStaticPropPacket` (28B):
- `sortedSlot` — draw order index
- `globalPacketIdx` — index into s_packets[] → enough to rebuild DrawPacket from registry
- `pipelineId` — alpha split
- `materialIdx` — MaterialGpu sidecar index
- `instanceCount` — prev-frame (for snap-cull, already used)
- `texArrayLayer` — albedo tex-array layer

From `ExtractedStaticProp` (256B max):
- `recipeIndex`, `typeId`, `worldMatrix[16]`
- `materialIdx`, `texArrayLayer`
- `alphaClass`, `packetCount`, `firstPacket`

## Files to touch (predicted)

| File | Change |
|------|--------|
| `GameOS/gameos/render_snapshot.h` | New `spBuild*` counter fields; update ok gate comment |
| `GameOS/gameos/render_snapshot.cpp` | Read build stats; extend ok gate; log v2.3 → v3 |
| `GameOS/gameos/gos_static_prop_batcher.h` | New `batcher_getSnapshotBuildStats()` free fn decl |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | Snapshot-driven packet/meta builder; compare; env gate |

## Prerequisites confirmed in HEAD

- `getLastRenderSnapshot()` — delivers trusted snapshot to flush() ✓
- `snap->ok == 1u` gate — proves snapshot is structurally valid ✓
- `ExtractedStaticPropPacket.globalPacketIdx` — enough to reconstruct DrawPacket ✓
- `ExtractedStaticPropPacket.pipelineId` — alpha split ✓
- `ExtractedStaticPropPacket.materialIdx` — MaterialGpu sidecar ✓
- `ExtractedStaticPropPacket.texArrayLayer` — albedo layer ✓
- v6 dispatch path fully isolated behind `runV6` gate ✓

## Anti-goals for v3

- No change to shadow pass (shadow has its own draw path)
- No new snapshot fields beyond the build counters above
- No default-ON flip in v3 — compare-first, always
- No removal of live builder in v3 — it is the fallback

## Session orientation

1. Read `INDEX-RENDERING.md` for v6 dispatch context
2. Read `GameOS/gameos/gos_static_prop_batcher.cpp` — `if (runV6 && ...)` block (~line 4090)
3. Read `GameOS/gameos/render_snapshot.h` for current field layout
4. Read `docs/superpowers/plans/2026-05-26-extraction-v2-3.md` — same pattern as v3 but for snap-cull

## Recommended session sequence

1. Brainstorm v3 spec (use `greybeard` adversarial review before writing plan)
2. Write plan with `superpowers:writing-plans`
3. Execute via `superpowers:subagent-driven-development`
4. Gate: tier1 5/5 PASS both default and `MC2_SNAPSHOT_STATIC_PROP_BUILD=1`
5. If compare gate green for 5/5, consider default-ON flip as v3.1

## Key greybeard concerns to address in spec

- **baseInstance:** built by `prepareBaseInstanceTable()` after extraction — snapshot doesn't own it;
  v3 builder must still call the live helper or replicate it
- **SSBO layout:** v6Packets and StaticPropDispatchMeta are tightly laid out; snapshot-built versions
  must match exactly or the SSBO upload is wrong
- **Per-frame vs static data:** `s_packets[]` is static after `finalizeGeometry()`; snapshot rows are
  always prev-frame; for static data (pipeline, material, tex-layer) this is fine; for dynamic
  per-frame counts it is not (don't snapshot-build instanceCount for the current frame's meta)
