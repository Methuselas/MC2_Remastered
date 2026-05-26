# DrawPacket v4B — Coalesce Alpha-Off Packet Coverage Compare

**Date:** 2026-05-26
**Status:** PENDING

---

## Goal

Add a diagnostic-only block to `gameosmain.cpp` that cross-checks the number of alpha-off/on packets reported by the batcher's coalesce layout against the count derived from the v2-emitted candidate buffer. No GL calls, no batcher state changes, no pixel change.

---

## Classification

| Axis | Value |
|---|---|
| Kind | Observational / diagnostic |
| Pixels changed | No |
| GL calls added | No |
| Dispatch changed | No |
| Files changed | 2 (`gameosmain.cpp`, `docs/tier1_env_vars.md`) |
| New env vars | `MC2_DRAW_PACKET_COALESCE_COMPARE`, `MC2_DRAW_PACKET_COALESCE_VERBOSE` |

---

## Prerequisites (shipped slices)

- DrawPacket v2 compare (`MC2_DRAW_PACKET_COMPARE`) — candidates iterable via `stats.emitted`
- DrawPacket v3 — `buildStaticPropDrawPackets` available (not consumed here)
- DrawPacket v4A — `batcher_setOpaqueDispatchCandidates` at ~L1302; v4B inserts immediately after
- Public batcher accessors already declared in `gos_static_prop_batcher.h`:
  `batcher_getAlphaOffCmdCount`, `batcher_getAlphaOnCmdCount`,
  `batcher_getSortedPacketOrder`, `batcher_getSortedPacketCount`,
  `batcher_isCoalesceLayoutReady`, `batcher_getPacketDrawInfo`

---

## Tasks

### Task 1 — Add gate latches (gameosmain.cpp ~L1291, after v4A comment block opens)

**File:** `GameOS/gameos/gameosmain.cpp`
**Anchor:** immediately after `batcher_setOpaqueDispatchCandidates(s_opaqueViews.data(), stats.emitted);` (~L1302), before the closing `}` at ~L1303.

Insert the following block. The two gate latches follow the identical lambda pattern used at L1192 (`s_dpCompareEnabled`) and L1203 (`s_v3Enabled`).

```cpp
            // v4B: Coalesce alpha-off packet coverage compare.
            // Compares batcher coalesce layout cmd counts vs candidate buffer alpha groups.
            // No GL calls, no batcher mutations, no pixel change.
            // Master gate: MC2_DRAW_PACKET_COALESCE_COMPARE=1
            // Verbose gate: MC2_DRAW_PACKET_COALESCE_VERBOSE=1 (per-slot, fires ONCE per process)
            static const bool s_coalesceCmpEnabled = [] {
                const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_COMPARE");
                return v && v[0] == '1';
            }();
            static const bool s_coalesceCmpVerbose = [] {
                const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_VERBOSE");
                return v && v[0] == '1';
            }();
```

### Task 2 — Add finalize snapshot (once per process, inside the v4B block)

**File:** `GameOS/gameos/gameosmain.cpp`
**Anchor:** immediately after the two gate latches from Task 1.

```cpp
            if (s_coalesceCmpEnabled && batcher_isCoalesceLayoutReady()) {
                // Finalize snapshot — emitted once on the first frame both gates pass.
                static bool s_finalizeSnapDone = false;
                if (!s_finalizeSnapDone) {
                    s_finalizeSnapDone = true;
                    const uint32_t offCmds = batcher_getAlphaOffCmdCount();
                    const uint32_t onCmds  = batcher_getAlphaOnCmdCount();
                    std::fprintf(stderr,
                        "[DRAW_PACKET_COALESCE_COMPARE v1]"
                        " event=finalize_snapshot off_cmds=%u on_cmds=%u total_cmds=%u\n",
                        offCmds, onCmds, offCmds + onCmds);
                }
```

### Task 3 — Count candidate alpha groups from emitted buffer

**File:** `GameOS/gameos/gameosmain.cpp`
**Anchor:** immediately after the `s_finalizeSnapDone` block from Task 2, still inside the outer `if` from Task 2.

```cpp
                // Count candidates per alpha group from v2-emitted buffer.
                // Iterate [0, stats.emitted) — NOT s_candidates.size() (buffer capacity).
                // alphaPass==0 → alpha-OFF group; nonzero → alpha-ON group.
                // Do NOT cross-check cachedMaterialFlags — different axes (spec Risk 1).
                uint32_t candidateOffPkts = 0;
                uint32_t candidateOnPkts  = 0;
                for (uint32_t i = 0; i < stats.emitted; ++i) {
                    if (s_candidates[i].alphaPass == 0)
                        ++candidateOffPkts;
                    else
                        ++candidateOnPkts;
                }
```

### Task 4 — Per-slot verbose (once per process, verbose gate)

**File:** `GameOS/gameos/gameosmain.cpp`
**Anchor:** immediately after the candidate count loop from Task 3.

```cpp
                // Per-slot verbose: fires ONCE per process lifetime (verbose gate only).
                // Iterates sorted packet order for the OFF group only (slots [0, offCmds)).
                static bool s_verboseDone = false;
                if (s_coalesceCmpVerbose && !s_verboseDone) {
                    s_verboseDone = true;
                    const uint32_t offCmds      = batcher_getAlphaOffCmdCount();
                    const uint32_t* sortedOrder  = batcher_getSortedPacketOrder();
                    const uint32_t  sortedCount  = batcher_getSortedPacketCount();
                    for (uint32_t slot = 0; slot < offCmds && slot < sortedCount; ++slot) {
                        const uint32_t globalPktIdx = sortedOrder[slot];
                        // Check whether this packet index appears in the candidate buffer.
                        bool found = false;
                        for (uint32_t ci = 0; ci < stats.emitted && !found; ++ci) {
                            if (s_candidates[ci].alphaPass == 0 &&
                                s_candidates[ci].globalPacketIdx == globalPktIdx)
                                found = true;
                        }
                        std::fprintf(stderr,
                            "[DRAW_PACKET_COALESCE_COMPARE v1]"
                            " event=slot_off slot=%u coalesce_pkt=%u candidate_match=%d\n",
                            slot, globalPktIdx, found ? 1 : 0);
                    }
                }
```

### Task 5 — Throttled summary log (every 600 frames)

**File:** `GameOS/gameos/gameosmain.cpp`
**Anchor:** immediately after the verbose block from Task 4, still inside the outer `if` from Task 2.

```cpp
                // Summary log every 600 frames.
                // Uses snap.frameIndex (NOT g_mc2FrameCounter — stale-by-1 from EmitDrawPackets zone).
                if (snap.frameIndex % 600 == 0) {
                    const uint32_t offCmds = batcher_getAlphaOffCmdCount();
                    const uint32_t onCmds  = batcher_getAlphaOnCmdCount();
                    // skip_delta: negative on destruction missions (expected). Positive = FAIL in clean smoke.
                    const int skipDeltaOff = static_cast<int>(candidateOffPkts) - static_cast<int>(offCmds);
                    const int skipDeltaOn  = static_cast<int>(candidateOnPkts)  - static_cast<int>(onCmds);
                    const int ok = (candidateOffPkts >= offCmds && candidateOnPkts >= onCmds) ? 1 : 0;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_COALESCE_COMPARE v1]"
                        " frame=%u event=coverage_check"
                        " coalesce_off_cmds=%u coalesce_on_cmds=%u"
                        " candidate_off_pkts=%u candidate_on_pkts=%u"
                        " skip_delta_off=%d skip_delta_on=%d ok=%d\n",
                        static_cast<uint32_t>(snap.frameIndex),
                        offCmds, onCmds,
                        candidateOffPkts, candidateOnPkts,
                        skipDeltaOff, skipDeltaOn, ok);
                }
            } // end s_coalesceCmpEnabled && isCoalesceLayoutReady
```

### Task 6 — Document env vars in tier1_env_vars.md

**File:** `docs/tier1_env_vars.md`
**Anchor:** append after the `MC2_DRAWPACKET_STATIC_PROP_OPAQUE` entry (~L76), inside the `## DrawPacket v2 compare` section.

Add:

```markdown
- `MC2_DRAW_PACKET_COALESCE_COMPARE=1` — master gate for DrawPacket v4B. Emits `[DRAW_PACKET_COALESCE_COMPARE v1]` to stderr: one `event=finalize_snapshot` line on first frame coalesce layout is ready, then one `event=coverage_check` line every 600 frames reporting `coalesce_off_cmds`, `coalesce_on_cmds`, `candidate_off_pkts`, `candidate_on_pkts`, `skip_delta_off`, `skip_delta_on`, `ok`. `ok=1` means candidate counts cover coalesce cmd counts. Default OFF. Cached at process start.
- `MC2_DRAW_PACKET_COALESCE_VERBOSE=1` — per-slot verbose output for the alpha-OFF coalesce group. Fires ONCE per process lifetime (latch `s_verboseDone`). Emits one `event=slot_off` line per OFF slot with `coalesce_pkt` (globalPacketIdx from sorted order) and `candidate_match` (1 if found in candidate buffer, 0 if not). Requires `MC2_DRAW_PACKET_COALESCE_COMPARE=1` to have any effect. Default OFF. Cached at process start.
```

---

## Invariants (hard gates for smoke)

These must hold for every tier1 5/5 no-destruction smoke run:

1. `candidate_off_pkts >= coalesce_off_cmds` → `ok=1` in every summary line
2. `candidate_on_pkts >= coalesce_on_cmds` → `ok=1` in every summary line
3. `skip_delta_off < 0` — allowed on destruction missions; NOT a failure
4. `skip_delta_off > 0` — FAIL in no-destruction smoke (more candidates than coalesce cmds means a candidate mapping gap)
5. Block is skipped entirely when `!batcher_isCoalesceLayoutReady()` — no output until coalesce layout is finalized

---

## Smoke gate

Canonical invocation (verbatim, no substitutions):

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

- Run with `MC2_DRAW_PACKET_COALESCE_COMPARE=1` set in environment.
- Pass criteria: exit 0, and `grep "ok=0" <log>` returns no matches across all 5 missions.
- No regression vs pre-v4B HEAD (baseline smoke must also be clean).

---

## Env var documentation entries (copy-paste for tier1_env_vars.md)

Paste these two lines after the `MC2_DRAWPACKET_STATIC_PROP_OPAQUE` bullet in `docs/tier1_env_vars.md`:

```
- `MC2_DRAW_PACKET_COALESCE_COMPARE=1` — master gate for DrawPacket v4B. Emits `[DRAW_PACKET_COALESCE_COMPARE v1]` to stderr: one `event=finalize_snapshot` line on first frame coalesce layout is ready, then one `event=coverage_check` line every 600 frames reporting `coalesce_off_cmds`, `coalesce_on_cmds`, `candidate_off_pkts`, `candidate_on_pkts`, `skip_delta_off`, `skip_delta_on`, `ok`. `ok=1` means candidate counts cover coalesce cmd counts. Default OFF. Cached at process start.
- `MC2_DRAW_PACKET_COALESCE_VERBOSE=1` — per-slot verbose output for the alpha-OFF coalesce group. Fires ONCE per process lifetime (latch `s_verboseDone`). Emits one `event=slot_off` line per OFF slot with `coalesce_pkt` (globalPacketIdx from sorted order) and `candidate_match` (1 if found in candidate buffer, 0 if not). Requires `MC2_DRAW_PACKET_COALESCE_COMPARE=1` to have any effect. Default OFF. Cached at process start.
```

---

## Anti-goals

This slice MUST NOT:

- Make any GL calls
- Mutate batcher state (no writes to `s_packets`, `s_sortedPacketOrder`, or any batcher-internal array)
- Change any dispatch path (legacy `flush()` or v4A opaque dispatch are untouched)
- Change pixel output
- Iterate `s_candidates.size()` — always use `stats.emitted` as the upper bound
- Cross-check `cachedMaterialFlags` to determine alpha group membership — `alphaPass` is the sole axis
- Use `g_mc2FrameCounter` for throttle — use `snap.frameIndex` (g_mc2FrameCounter is stale-by-1 at this call site)
- Re-fire the verbose latch after the first emit — `s_verboseDone` is set once and never cleared
- Block or log anything when `batcher_isCoalesceLayoutReady()` returns false
