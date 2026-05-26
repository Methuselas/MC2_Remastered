# DrawPacket v4B Spec
Date: 2026-05-26
Status: READY FOR REVIEW

---

## Goal

Log and compare the coalesce alpha-off command set against the DrawPacket
candidate set each frame, confirming that the emitter's alpha-OFF candidate
coverage matches the coalesce draw command count. No GL mutation, no dispatch
change, no render suppression.

---

## Prereqs (shipped slices this depends on)

- **DrawPacket v0**: `emitStaticPropDrawPackets()` exists and produces
  `StaticPropDrawPacketCandidate[]` per frame. SHIPPED.
- **DrawPacket v1**: Type-table walk in emitter; `batcher_getStaticPropTypeDescTable()`
  available. SHIPPED.
- **DrawPacket v2**: `comparePacketsToLegacy()` + `pipelineId` assignment. SHIPPED.
- **DrawPacket v3**: `buildStaticPropDrawPackets()` ABI promotion. SHIPPED.
- **DrawPacket v4A**: Opaque substitutive dispatch (non-coalesce path). SHIPPED.
- **`batcher_getAlphaOffCmdCount()` / `batcher_getAlphaOnCmdCount()`**: Public
  accessors declared in `gos_static_prop_batcher.h` L337-338. SHIPPED.
- **`batcher_getSortedPacketOrder()` / `batcher_getSortedPacketCount()`**:
  Public accessors declared in `gos_static_prop_batcher.h` L335-336. SHIPPED.

---

## Classification

**Observational/diagnostic only.**

v4B adds logging/comparison to `gameosmain.cpp` (the call site that already
invokes `emitStaticPropDrawPackets` + `comparePacketsToLegacy`). It does NOT:
- Issue GL calls
- Change any draw command
- Suppress or replace any existing draw
- Modify batcher state
- Add new SSBOs, programs, or buffers

---

## In scope

- One new log line per frame (throttled to every 600 frames like existing v4A log)
  comparing `s_alphaOffCmdCount` + `s_alphaOnCmdCount` to emitter-derived counts
- Summary counters: `coalesce_off_cmds`, `coalesce_on_cmds`, `candidate_off_pkts`,
  `candidate_on_pkts`, `skip_delta_off`, `skip_delta_on`
- First-frame detailed log (one line per alpha-OFF packet) gated on
  `MC2_DRAW_PACKET_COALESCE_VERBOSE=1`
- Env gate: `MC2_DRAW_PACKET_COALESCE_COMPARE=1`
- Invariant check: `candidate_off_pkts >= coalesce_off_cmds` — FAIL if not

## Out of scope

- Per-slot detailed comparison (requires emitter to emit in sorted-packet order;
  emitter currently uses type-desc order; reordering deferred to v4C)
- Logging alpha-ON group per-slot detail
- Any change to flush() or flushShadow()
- Any change to `emitStaticPropDrawPackets()` or `comparePacketsToLegacy()`
- Any change to batcher SSBOs or GL programs
- Shadow pass comparison

---

## Data ownership / lifetime

| Data | Owner | Lifetime | Access |
|---|---|---|---|
| `s_alphaOffCmdCount` | batcher TU | mission-lifetime (set in finalizeGeometry, cleared in onMapUnload) | `batcher_getAlphaOffCmdCount()` |
| `s_alphaOnCmdCount` | batcher TU | mission-lifetime | `batcher_getAlphaOnCmdCount()` |
| `s_sortedPacketOrder` | batcher TU | mission-lifetime | `batcher_getSortedPacketOrder()` + `batcher_getSortedPacketCount()` |
| `StaticPropDrawPacketCandidate[]` | gameosmain.cpp | frame-scoped (stack/static buffer) | direct (same TU as compare call) |
| `DrawPacketEmitStats` | gameosmain.cpp | frame-scoped | direct |

v4B reads `batcher_getAlphaOffCmdCount()` and `batcher_getAlphaOnCmdCount()`
after `emitStaticPropDrawPackets()` completes. Both calls are safe at any point
after `finalizeGeometry()` and before `onMapUnload()`. No synchronization needed
(single-threaded render loop).

---

## Authority for comparison

The **coalesce command set** (`s_alphaOffCmdCount`, `s_alphaOnCmdCount`) is the
**ground truth** for what the GPU will draw. The emitter candidate set is the
**challenger** — it must cover at least the ground truth to be a valid dispatch
source.

`candidate_off_pkts` is derived from the emitter: count of candidates where
`alphaPass == 0` (opaque group, alphaClass=0 type). The emitter sets `alphaPass`
from `desc.alphaClass` (type-table source); since the batcher's `alphaClass` is
the same OR-reduce used to build `s_sortedPacketOrder`, the two definitions are
consistent.

`candidate_off_pkts` may legitimately EXCEED `coalesce_off_cmds` when some
packets have `layerForPacket < 0` at finalize (damage-shape textures not yet
loaded). The emitter has no visibility into the per-packet texture-layer skip
rule — it always emits all packets for visible types. The skip delta
(`skip_delta_off = coalesce_off_cmds - candidate_off_pkts`) is expected to be
<= 0. A positive `skip_delta_off` is a FAIL (coalesce has more commands than
candidates — emitter has coverage gaps).

---

## Env gates

| Variable | Default | Description |
|---|---|---|
| `MC2_DRAW_PACKET_COALESCE_COMPARE` | 0 (off) | Master gate for v4B logging |
| `MC2_DRAW_PACKET_COALESCE_VERBOSE` | 0 (off) | First-frame per-slot alpha-OFF detail |

Both gates are checked once at process start (static bool, like existing
`MC2_DRAW_PACKET_COMPARE`). The summary log fires only when
`MC2_DRAW_PACKET_COALESCE_COMPARE=1`. Verbose detail fires additionally when
`MC2_DRAW_PACKET_COALESCE_VERBOSE=1`.

---

## Log schema (tag + version + fields)

### Summary line (every 600 frames when gate on)

```
[DRAW_PACKET_COALESCE_COMPARE v1] frame=<N> event=coverage_check
  coalesce_off_cmds=<uint>
  coalesce_on_cmds=<uint>
  candidate_off_pkts=<uint>
  candidate_on_pkts=<uint>
  skip_delta_off=<int>
  skip_delta_on=<int>
  ok=<0|1>
```

`ok=1` iff `skip_delta_off <= 0 && skip_delta_on <= 0`.

All fields on one line; line break shown here for readability.

### Per-slot detail line (first frame, `MC2_DRAW_PACKET_COALESCE_VERBOSE=1`)

For each slot `i` in `[0, s_alphaOffCmdCount)`:

```
[DRAW_PACKET_COALESCE_COMPARE v1] event=slot_off slot=<i>
  coalesce_pkt=<s_sortedPacketOrder[i]>
  coalesce_typeId=<s_packets[pkt].owningTypeID via batcher_getPacketDrawInfo>
  candidate_match=<0|1>
```

`candidate_match=1` iff the emitter emitted a candidate with
`globalPacketIdx == s_sortedPacketOrder[i]`. The emitter candidate array must
be indexed by `globalPacketIdx` for this comparison; add a `bool[]` presence map
(size = `batcher_getSortedPacketCount()`) to gameosmain's v4B block.

### First-frame one-time summary (both gates)

```
[DRAW_PACKET_COALESCE_COMPARE v1] event=finalize_snapshot
  off_cmds=<s_alphaOffCmdCount>
  on_cmds=<s_alphaOnCmdCount>
  total_cmds=<sum>
```

Emitted once at the first frame `emitStaticPropDrawPackets()` runs after
`finalizeGeometry()`. Lets the reader see the static shape of the command set.

---

## Success criteria (exact counters)

All three required for PASS:

1. `skip_delta_off <= 0` every frame (candidate coverage >= coalesce command count
   for alpha-OFF group). Equality (delta=0) is the normal case with no damage shapes.
2. `skip_delta_on <= 0` every frame (same for alpha-ON group).
3. `ok=1` in every summary line observed in a tier1 smoke run where no building
   destruction occurs.

Informational (not blocking):
- `skip_delta_off < 0` indicates damage-shape skips are active (some packets
  present in emitter but absent from coalesce due to `layerForPacket < 0`);
  expected nonzero only on missions with destroyed buildings.
- `skip_delta_on` is expected 0 on all stock missions (no damage-shape alpha-test
  textures at finalize time in test conditions).

---

## Implementation order

1. Add `static bool s_coalesceCompareEnabled` gate check (process-once getenv) in
   gameosmain.cpp near the existing `s_compareEnabled` gate at L1193.
   Also add `static bool s_coalesceVerboseEnabled` for the verbose gate.
2. After the `comparePacketsToLegacy()` call site, in the same frame-end block:
   a. Pre-flight: skip if `!s_coalesceCompareEnabled` or `!batcher_isCoalesceLayoutReady()`.
   b. Call `batcher_getAlphaOffCmdCount()` + `batcher_getAlphaOnCmdCount()`.
   c. Derive `candidate_off_pkts` from the emitted `StaticPropDrawPacketCandidate[]`
      (count entries with `alphaPass == 0`). Iterate `[0, stats.emitted)` ONLY —
      NOT `s_candidates.size()` (buffer is pre-allocated; only [0..emitted) are valid).
   d. Derive `candidate_on_pkts` (count entries with `alphaPass != 0`).
   e. Compute `skip_delta_off`, `skip_delta_on`, `ok` (`ok = skip_delta_off<=0 && skip_delta_on<=0`).
   f. Throttle: log summary line when `snap.frameIndex % 600 == 0`
      (use `snap.frameIndex`, NOT `g_mc2FrameCounter` — the latter is incremented
      AFTER the EmitDrawPackets zone and reads stale-by-1 from inside the zone).
3. If `s_coalesceVerboseEnabled`, log per-slot detail ONCE per process lifetime:
   a. Guard with `static bool s_verboseDone = false`; return immediately if already set.
   b. Set `s_verboseDone = true` after the verbose log emits (process-lifetime latch).
   c. Build a presence set (or `bool[batcher_getSortedPacketCount()]`) from emitted
      candidates keyed by `globalPacketIdx`, iterating `[0, stats.emitted)`.
   d. Iterate `batcher_getSortedPacketOrder()` for slots `[0, s_alphaOffCmdCount)`.
   e. Log each slot's `candidate_match`.
4. Add `MC2_DRAW_PACKET_COALESCE_COMPARE` and `MC2_DRAW_PACKET_COALESCE_VERBOSE`
   to `docs/tier1_env_vars.md`.

No changes to batcher.cpp, no new GL calls, no new SSBOs.

---

## Risks / open questions

### Risk 1: Emitter `alphaPass` is TRANSITIONAL

`StaticPropDrawPacketCandidate::alphaPass` is documented as
"TRANSITIONAL/debug-only; NOT authoritative v2+" (draw_packet_emitter.h L32).
Derive `candidate_off_pkts` from `alphaPass == 0`. Do NOT cross-check against
`cachedMaterialFlags == 0`: these are different axes.

- `alphaPass == 0` reflects `desc.alphaClass == 0` = type-level render group
  (all packets of that type in the alpha-OFF draw bucket)
- `cachedMaterialFlags & STATIC_PROP_FLAG_ALPHA_TEST` = per-packet material flag

A type with `alphaClass=0` (all packets go to alpha-OFF bucket) can still have
packets with the ALPHA_TEST material flag. Cross-check would false-fire on every
such packet. The cross-check is REMOVED from the implementation.

If a secondary signal is desired, compare `pipelineId == StaticPropOpaque`
(advisory note only — no WARN emitted from v4B). This is consistent with alphaPass.

Risk is LOW: `alphaPass` is sourced from `desc.alphaClass` (type-table), same
field used to build `s_sortedPacketOrder`. No divergence expected.

### Risk 2: Damage-shape skip asymmetry

If a mission has destroyed buildings at map load time (possible in savegame
scenarios), some packets may be skipped from `s_sortedPacketOrder` due to
`layerForPacket < 0`. In that case, `skip_delta_off < 0` is expected and is NOT
a bug. v4B logs the delta but does not assert. The invariant `ok=1` still holds
(delta <= 0).

### Risk 3: Per-slot compare requires emitter reordering (NOT a v4B blocker)

The per-slot `candidate_match` check in the verbose path builds a presence map
indexed by `globalPacketIdx`. This works without reordering the emitter; it
simply checks membership. The match is unordered (not slot-order). Per-slot
ORDER comparison (verifying that candidate slot i maps to the same globalPacketIdx
as coalesce slot i) requires emitter to emit in `s_sortedPacketOrder` sequence,
which it currently does not. This is noted as a TODO for v4C, not a v4B blocker.

### Risk 4: `s_alphaOffCmdCount` is 0 when coalesce is disarmed

When `IsCoalesceEnabled()` returns false, `s_alphaOffCmdCount` is still set from
`finalizeGeometry()`. The coalesce draw does not fire, but the count is still
valid as the expected-command count had coalesce been armed. The compare is still
useful: it tells us whether the candidates match the "would-be" coalesce set.
Gate v4B on `batcher_isCoalesceLayoutReady()` to suppress logging when the layout
was never built (early in mission load).

### No per-packet mapping BLOCKER

Recon confirmed that `globalPacketIdx` is directly available from
`s_sortedPacketOrder[i]` and from `PerDrawEntry::packetID` (both CPU-accessible).
No batcher changes are required for the mapping. Per-slot compare (verbose mode)
is implementable without schema changes.

---

## Appendix: key line references

| Item | File | Line |
|---|---|---|
| `s_alphaOffCmdCount` declaration | gos_static_prop_batcher.cpp | L329 |
| `s_alphaOnCmdCount` declaration | gos_static_prop_batcher.cpp | L330 |
| `s_sortedPacketOrder` declaration | gos_static_prop_batcher.cpp | L328 |
| Sort + count build in finalizeGeometry | gos_static_prop_batcher.cpp | L2107-2136 |
| `PerDrawEntry::packetID` field | gos_static_prop_batcher.h | L52 |
| `batcher_getAlphaOffCmdCount()` | gos_static_prop_batcher.h | L337 |
| `batcher_getSortedPacketOrder()` | gos_static_prop_batcher.h | L335 |
| `StaticPropDrawPacketCandidate::alphaPass` | draw_packet_emitter.h | L32 |
| Coalesce alpha-OFF draw (flush) | gos_static_prop_batcher.cpp | L3871-3892 |
| Coalesce alpha-ON draw (flush) | gos_static_prop_batcher.cpp | L3895-3913 |
| v4A coalesce_noop log | gos_static_prop_batcher.cpp | L3381-3383 |
