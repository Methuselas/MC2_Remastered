# DrawPacket v4C Spec — Coalesce Alpha-Off Slot-Level Coverage Soak

**Date:** 2026-05-26
**Status:** DRAFT (pending advisor review before plan/execution)

---

## Classification

| Axis | Value |
|---|---|
| Kind | Observational / diagnostic |
| Pixels changed | No |
| GL calls added | No |
| Dispatch changed | No |
| Files changed | 2 (gameosmain.cpp, docs/tier1_env_vars.md) |

**Note: Compare-only soak.** Substitutive dispatch for alpha-OFF group is deferred to v5.
This decision was made because GPU-cull instance counts are GPU-write-only (no CPU copy without ~6ms stall),
and correct substitution requires per-type instanced draws with correct base instances from
`s_baseInstanceByCmdSsbo` — a data structure problem more appropriate to resolve in v5 alongside
the full `StaticPropDispatchMeta` design.

---

## Goal

Extend v4B coverage compare to slot-level: for each DrawPacket candidate in the alpha-OFF group,
verify that its `globalPacketIdx` appears in `s_sortedPacketOrder[0..s_alphaOffCmdCount)`.
Log a summary counter per frame: `matched_off=N unmatched_off=M`. PASS gate: `unmatched_off == 0`.

This confirms that the DrawPacket candidate set is a valid subset of the coalesce alpha-OFF layout
slot-by-slot, not just count-level. It is the final prerequisite validation before v5 can
implement substitutive dispatch.

---

## Prereqs

- v4B SHIPPED (da7eb1e3): coalesce coverage compare at count level. PASS: ok=1 all missions.
- v4B key finding: emitter is snapshot-driven (visible types only); `candidate < coalesce` is expected;
  `skip_delta = coalesce - candidate > 0` is normal (non-visible types in coalesce layout).

---

## Key finding from instance-count recon

GPU-cull instance counts are GPU-write-only to the indirect buffer; no CPU read without ~6ms stall.
Per-type instance counts ARE available CPU-side from `s_bucketsByType` (populated before flush() via
`batcher_prepareBaseInstanceTable()`). This means:
- v4C (diagnostic) has no need for instance counts — slot-level membership check only
- v5 (substitutive) will use `s_bucketsByType` instance counts + `s_baseInstanceByCmdSsbo` base instances

---

## In scope

- Build a slot presence map from `s_sortedPacketOrder[0..s_alphaOffCmdCount)`:
  - `std::unordered_set<uint32_t>` or `std::vector<bool>` indexed by globalPacketIdx
  - Built once per frame after `batcher_isCoalesceLayoutReady()` confirms layout is finalized
- For each alpha-OFF candidate (alphaPass==0, iterate [0, stats.emitted)):
  - Check `globalPacketIdx` in the presence map
  - Increment `matched_off` or `unmatched_off`
- Same for alpha-ON group (alphaPass!=0) vs `s_sortedPacketOrder[s_alphaOffCmdCount..total)`)
- Throttled summary log every 600 frames: `matched_off`, `unmatched_off`, `matched_on`, `unmatched_on`, `ok`
- On FAIL (any `unmatched_off > 0` or `unmatched_on > 0`): emit immediately (not throttled) with
  the first failed `globalPacketIdx` for diagnosis
- Env gate: `MC2_DRAW_PACKET_COALESCE_V4C=1`
- Add to `docs/tier1_env_vars.md`

## Out of scope

- Any draw call change
- Any batcher SSBO change
- Instance count sourcing (deferred to v5)
- Per-draw-call substitution of glMultiDrawElementsIndirect (deferred to v5)
- Shadow pass
- Alpha-ON substitution

---

## Data ownership / lifetime

| Data | Owner | Lifetime | Access |
|---|---|---|---|
| `s_sortedPacketOrder` | batcher TU | mission-lifetime | `batcher_getSortedPacketOrder()` (h L335) |
| `s_alphaOffCmdCount` | batcher TU | mission-lifetime | `batcher_getAlphaOffCmdCount()` (h L337) |
| `s_alphaOnCmdCount` | batcher TU | mission-lifetime | `batcher_getAlphaOnCmdCount()` (h L338) |
| `StaticPropDrawPacketCandidate[]` | gameosmain.cpp | frame-scoped | direct, [0..stats.emitted) |
| presence map (local) | gameosmain.cpp | frame-scoped (built each frame inside gate) | local variable |

Presence map is rebuilt every frame (not cached across frames) to ensure correctness when layout
changes (e.g., damage-shape packets reloaded). Cost: O(s_alphaOffCmdCount + s_alphaOnCmdCount)
insertions per frame when gate is on — acceptable for a diagnostic gate.

---

## Log schema

### Summary line (every 600 frames, `MC2_DRAW_PACKET_COALESCE_V4C=1`)

```
[DRAW_PACKET_COALESCE_SOAK v1] frame=<N> event=slot_coverage
  matched_off=<uint> unmatched_off=<uint>
  matched_on=<uint> unmatched_on=<uint>
  ok=<0|1>
```

`ok=1` iff `unmatched_off == 0 && unmatched_on == 0`.

### Immediate fail line (any frame where `unmatched > 0`)

```
[DRAW_PACKET_COALESCE_SOAK v1] event=slot_miss
  group=<off|on> candidate_pkt=<globalPacketIdx> typeId=<uint>
```

Emitted for the FIRST unmatched candidate per frame. Subsequent misses increment the counter
but do not emit per-slot lines to prevent log flooding.

---

## Success criteria

1. `unmatched_off == 0` every frame (all alpha-OFF candidates' globalPacketIdx found in sorted order alpha-OFF slots)
2. `unmatched_on == 0` every frame
3. `ok=1` in every summary line, tier1 5/5 no-destruction smoke
4. No immediate fail lines emitted

---

## Implementation order (gameosmain.cpp, after v4B block)

1. Add `static const bool s_coalesceV4CEnabled` gate latch (getenv, same lambda pattern)
2. Inside `if (s_coalesceV4CEnabled && batcher_isCoalesceLayoutReady())`:
   a. Get `offCount = batcher_getAlphaOffCmdCount()`, `onCount = batcher_getAlphaOnCmdCount()`
   b. Get `sortedOrder = batcher_getSortedPacketOrder()`, `sortedCount = batcher_getSortedPacketCount()`
   c. Build two `std::unordered_set<uint32_t>` (or vector<bool> of size sortedCount):
      - `offSlots`: insert `sortedOrder[i]` for `i in [0, offCount)`
      - `onSlots`: insert `sortedOrder[i]` for `i in [offCount, offCount+onCount)`
   d. Iterate `[0, stats.emitted)`:
      - If `alphaPass==0`: check `offSlots.count(globalPacketIdx)` → matched_off or unmatched_off
      - Else: check `onSlots.count(globalPacketIdx)` → matched_on or unmatched_on
      - On first unmatched: emit immediate fail line
   e. Log summary if `snap.frameIndex % 600 == 0`

No batcher changes needed. No new public accessors needed (all used in v4B are sufficient).

---

## Risks / open questions

### Risk 1: presence map allocation per-frame

The `unordered_set` allocations run every frame when gate is on. For a diagnostic gate this is
acceptable (the gate is off in production). For very large maps (mc2_24: 134 total coalesce slots)
the cost is negligible.

Mitigation: use frame-local `std::vector<bool>` indexed by globalPacketIdx (size =
`batcher_getSortedPacketCount()`) instead of unordered_set. O(1) lookup, O(sortedCount) allocation.
Pre-declare as static and resize each frame; this avoids per-frame heap allocation.

### Risk 2: expected unmatched candidates (non-loaded packet types)

v4B established that `candidate < coalesce` is expected. The candidates are from visible types
(snapshot-driven). Non-visible types don't appear in candidates. So `matched_off` will equal
`candidate_off_pkts` (from v4B), and `unmatched_off` will be 0. This is correct — the candidates
are a subset of the coalesce OFF slots, which is the expected case.

The `unmatched_off > 0` FAIL case would only occur if a candidate's globalPacketIdx is NOT in the
alpha-OFF sorted order — which would indicate the emitter's type-table is out of sync with the
batcher's sorted layout. This would be a real emitter bug.

### Risk 3: Log tag vs v4B tag

New tag `[DRAW_PACKET_COALESCE_SOAK v1]` (not v4B's `[DRAW_PACKET_COALESCE_COMPARE v1]`).
These are logically distinct: v4B = count-level compare; v4C = slot-level membership check.
Separate tags allow separate grep filtering in smoke log analysis.

---

## v5 design inputs (from recon)

For v5 substitutive dispatch of alpha-OFF group:
- Per-type instance count: `s_bucketsByType[typeId].instances.size()` — via accessor
  `batcher_getTypeInstanceCount(typeId)` (NEW, to be added in v5 batcher.h)
- Per-packet base instance: `s_baseInstanceByCmdSsbo[slot]` — via persistent-mapped accessor
  `batcher_getBaseInstanceForSlot(slot)` (NEW, to be added in v5 batcher.h)
- Instance data SSBO: `s_coalesceInstanceSsbo` — bind via
  `batcher_bindCoalesceInstanceSsbo()` (NEW)
- Per-draw-call dispatch:
  `glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT,
   (void*)(firstIndex * 4), instanceCount, baseVertex, baseInstance)` per candidate
- Caveats: uses CPU-snapshot instance counts (not GPU-cull-reduced); draws all snapshot instances,
  not just GPU-cull-surviving ones. This is semantically equivalent to the legacy path (which also
  uses CPU counts). Acceptable for v5 proof; GPU-cull integration remains a v6+ consideration.
