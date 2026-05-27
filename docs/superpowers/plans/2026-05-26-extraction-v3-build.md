# Extraction v3 Build — Snapshot-Owned Slot Identity

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `v6Packets` + `StaticPropDispatchMeta` from `RenderSnapshot` rows instead of live batcher state, compare against live-built arrays field-by-field, dispatch snapshot-built arrays when compare passes, fall back to live on any mismatch.

**Architecture:** Two separate static vector pairs live in `gos_static_prop_batcher.cpp` — `v6Packets/v6Meta` (live, unchanged) and `s_snapV6Packets/s_snapV6Meta` (snapshot-built). After the live builder runs each flush, a staged activation guard checks the snapshot, builds snapshot arrays using live current-frame facts for dynamic fields (`instanceCount`, `baseInstance`), runs a field-by-field compare, then a pointer ref-swap selects which pair feeds the existing dispatch loop. Five `spBuild*` counters are written to `RenderSnapshot`; the three mismatch counters gate `ok`.

**Tech Stack:** C++17, OpenGL 4.6 (no new GL calls). Files: `render_snapshot.h`, `render_snapshot.cpp`, `gos_static_prop_batcher.h`, `gos_static_prop_batcher.cpp`, `docs/tier1_env_vars.md`.

---

## File Map

| File | Tasks | Change |
|------|-------|--------|
| `GameOS/gameos/render_snapshot.h` | T1 | Add 5 `spBuild*` fields; update ok gate comment |
| `GameOS/gameos/gos_static_prop_batcher.h` | T2 | Add `batcher_getSnapshotBuildStats()` decl |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | T3, T4, T5 | New statics; env gate; `pipelineId_to_group()`; `batcher_getSnapshotBuildStats()` impl; counter reset; activation guard; builder; compare; ref-swap |
| `GameOS/gameos/render_snapshot.cpp` | T6 | Call `batcher_getSnapshotBuildStats()`; extend ok gate; update log |
| `docs/tier1_env_vars.md` | T7 | Add `MC2_SNAPSHOT_STATIC_PROP_BUILD` entry |

**Working directory for all tasks:** `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\`

**Build command (use this verbatim):**
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

**Smoke command (use this verbatim):**
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

---

## Task 1: Add v3 fields to RenderSnapshot

**Files:**
- Modify: `GameOS/gameos/render_snapshot.h`

Lines 192–198 are the ok gate comment. Line 199 is `uint32_t ok = 0u;`. Lines 220–222 are the snap-cull counters ending with `uint32_t spSnapCullSlotMismatch = 0u;`. Lines 224–229 are arena pointer + `};`.

- [ ] **Step 1: Add v3 fields after snap-cull block (after line 222)**

In `render_snapshot.h`, after `uint32_t spSnapCullSlotMismatch = 0u;` (line 222) and before `// Non-owning pointer...` (line 224), insert:

```cpp
    // --- v3: snapshot build stats (previous-flush; gate: MC2_SNAPSHOT_STATIC_PROP_BUILD=1) ---
    // Written by batcher_getSnapshotBuildStats(); read by ExtractRenderSnapshot().
    // spBuildAttempted and spBuildFallback are informational — excluded from ok gate.
    // spBuildCountMismatch, spBuildPacketMismatch, spBuildMetaMismatch participate in ok gate.
    uint32_t spBuildAttempted      = 0u;  // 1 if gate check ran this flush
    uint32_t spBuildCountMismatch  = 0u;  // snap.count != totalCmds
    uint32_t spBuildPacketMismatch = 0u;  // DrawPacket field divergence (accumulated)
    uint32_t spBuildMetaMismatch   = 0u;  // DispatchMeta field divergence (accumulated)
    uint32_t spBuildFallback       = 0u;  // gate enabled/attempted but snapshot arrays NOT dispatched
```

- [ ] **Step 2: Update ok gate comment (lines 192–198)**

Replace the existing v2.3 ok gate comment block with:

```cpp
    // v3 hard gate — extends v2.3: adds spBuildCountMismatch, spBuildPacketMismatch,
    //   spBuildMetaMismatch (all three must be zero).
    // v2.3 gate: staticPropValidationFail==0, staticPropPacketRangesFail==0,
    //   staticPropPacketInvalid==0, !arenaOverflow,
    //   spCountMismatch==0, spSortedSlotMismatch==0, spGlobalPacketMismatch==0,
    //   spPipelineMismatch==0, spMaterialIdxMismatch==0, spTexLayerMismatch==0,
    //   spSnapCullSlotMismatch==0.
    // Informational (excluded from ok): spInstanceCountMismatch, spSnapCullSkipped,
    //   spSnapCullActive, spBuildAttempted, spBuildFallback.
```

- [ ] **Step 3: Build**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

Expected: zero errors. New fields compile but are unused — no warnings expected in MSVC RelWithDebInfo for unused struct fields.

- [ ] **Step 4: Commit**

```powershell
cd "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev"
git add GameOS/gameos/render_snapshot.h
git commit -m "feat(snapshot): add v3 spBuild* fields to RenderSnapshot"
```

---

## Task 2: Add batcher_getSnapshotBuildStats() declaration

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`

Line 447 is `void batcher_getSnapCullStats(uint32_t* skipped, uint32_t* active, uint32_t* slotMismatch);`. The v3 accessor goes immediately after it.

- [ ] **Step 1: Add decl after batcher_getSnapCullStats() (line 447)**

After `void batcher_getSnapCullStats(...)` (line 447), insert:

```cpp
// v3 snapshot build: read counters written by the most recent flush().
// All output pointers may be nullptr (individual fields skipped).
// attempted:      1 if snapshot build gate check ran this flush.
// countMismatch:  snap.count != totalCmds.
// packetMismatch: DrawPacket field divergence count (accumulated per-slot).
// metaMismatch:   DispatchMeta field divergence count (accumulated per-slot).
// fallback:       gate enabled/attempted but snapshot arrays not dispatched this flush.
void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback);
```

- [ ] **Step 2: Build — expect linker error**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

Expected: unresolved external `batcher_getSnapshotBuildStats` — this is expected (implementation in T3).

- [ ] **Step 3: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.h
git commit -m "feat(batcher): declare batcher_getSnapshotBuildStats() v3 accessor"
```

---

## Task 3: Add v3 statics, env gate, helper, and accessor impl

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

Lines 83–114: existing v5/v6 gate statics (`s_v5Enabled`, `s_v6Enabled`, `s_v6Armed`, etc). Line 114 ends with `static bool s_v6Disarmed = false;`. v3 statics go after this block.

- [ ] **Step 1: Add v3 statics block after line 114 (after s_v6Disarmed)**

After `static bool s_v6Disarmed = false;` (line 114), insert:

```cpp
// v3 snapshot build: separate dispatch arrays for snapshot-built path.
// Reused each flush via resize() — no heap alloc after first frame.
static std::vector<RenderCore::DrawPacket> s_snapV6Packets;
static std::vector<StaticPropDispatchMeta> s_snapV6Meta;

// v3 env gate. File-local — not exported in header. Cached at process start.
static const bool s_snapshotBuildEnabled = []() -> bool {
    const char* v = std::getenv("MC2_SNAPSHOT_STATIC_PROP_BUILD");
    return v && v[0] == '1';
}();

// v3 per-flush counters. Reset each flush by the runV6 block.
// Read by batcher_getSnapshotBuildStats() for render_snapshot.cpp ok gate.
static uint32_t s_spBuildAttempted      = 0u;
static uint32_t s_spBuildCountMismatch  = 0u;
static uint32_t s_spBuildPacketMismatch = 0u;
static uint32_t s_spBuildMetaMismatch   = 0u;
static uint32_t s_spBuildFallback       = 0u;
// Latched on first fallback; never reset. Guards the first-occurrence log line.
static bool s_spBuildFirstFallbackLogged = false;
```

- [ ] **Step 2: Add pipelineId_to_group() static helper**

Immediately after the v3 statics block (still near line 115–135 area), add:

```cpp
// Returns 0 (opaque) or 1 (alpha-test). Returns 0xFFFFFFFFu for unknown pipelineId.
// Used by snapshot builder to derive group from the snapshot row's stored pipelineId.
static uint32_t pipelineId_to_group(uint32_t pid) {
    using P = RenderCore::PipelineId;
    if (pid == static_cast<uint32_t>(P::StaticPropOpaque))    return 0u;
    if (pid == static_cast<uint32_t>(P::StaticPropAlphaTest)) return 1u;
    return 0xFFFFFFFFu;
}
```

- [ ] **Step 3: Add batcher_getSnapshotBuildStats() implementation**

Search for `batcher_getSnapCullStats` implementation in `gos_static_prop_batcher.cpp`. Add immediately after its closing `}`:

```cpp
void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback)
{
    if (attempted)      *attempted      = s_spBuildAttempted;
    if (countMismatch)  *countMismatch  = s_spBuildCountMismatch;
    if (packetMismatch) *packetMismatch = s_spBuildPacketMismatch;
    if (metaMismatch)   *metaMismatch   = s_spBuildMetaMismatch;
    if (fallback)       *fallback       = s_spBuildFallback;
}
```

- [ ] **Step 4: Build — should compile and link cleanly**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

Expected: zero errors, zero linker errors. v3 code compiles but is entirely inert — no callers yet.

- [ ] **Step 5: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): v3 statics, env gate, pipelineId_to_group, batcher_getSnapshotBuildStats impl"
```

---

## Task 4: Counter reset in flush()

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` — inside `if (runV6)` block (~line 3975)

The existing counter resets are at lines 3975–3981 (`s_v6FrameDrawsIssued = 0u` through `s_v6FrameGlErrors = 0u`). Line 3982 is `++s_v6TotalFrameCount;`.

- [ ] **Step 1: Add v3 counter reset after ++s_v6TotalFrameCount (line 3982)**

After `++s_v6TotalFrameCount;` (line 3982), add:

```cpp
        // v3: reset per-flush build counters so stale stats never persist.
        s_spBuildAttempted = s_spBuildCountMismatch = s_spBuildPacketMismatch =
        s_spBuildMetaMismatch = s_spBuildFallback = 0u;
```

- [ ] **Step 2: Build**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

Expected: zero errors. Counters now reset each flush — still inert (no builder attached).

- [ ] **Step 3: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): reset v3 spBuild* counters each flush"
```

---

## Task 5: Activation guard + builder + compare + dispatch ref-swap

This is the core v3 implementation. All new code is inserted inside `if (runV6 && v6LockstepViolations == 0u)`, after the snap-cull activation block (ending ~line 4150) and before the dispatch loop `for (uint32_t i = 0u; i < totalCmds; ++i)` (~line 4152).

The dispatch loop also needs two lines changed to use ref-swapped pointers.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 1: Locate insertion point**

Find the snap-cull activation block ending inside `if (runV6 && v6LockstepViolations == 0u)`. It ends with:
```cpp
            snapCullActive = !allZero;
            snapN          = static_cast<uint32_t>(snap->staticPropPackets.count);
        }
```

The next line is `for (uint32_t i = 0u; i < totalCmds; ++i) {`. Insert the entire v3 block between these two.

- [ ] **Step 2: Insert activation guard + builder + compare + ref-swap**

After the snap-cull `}` and before `for (uint32_t i = 0u; ...)`, insert:

```cpp
            // ---------------------------------------------------------------
            // v3 snapshot builder: activation guard → build → compare
            // ---------------------------------------------------------------
            bool snapBuilt = false;
            s_spBuildAttempted = s_snapshotBuildEnabled ? 1u : 0u;

            if (s_snapshotBuildEnabled) {
                // Stage 1: snap-cull collision — both gates active → disable v3.
                if (s_snapCullEnabled) {
                    std::fprintf(stderr,
                        "[RENDER_SNAPSHOT v3] frame=%u disabled — MC2_SNAP_CULL collision\n",
                        s_v6TotalFrameCount);
                    ++s_spBuildFallback;
                }
                // Stage 2: structural guards — snap unusable (no counter).
                else if (snap == nullptr || snap->ok != 1u ||
                         snap->staticPropPackets.data == nullptr) {
                    /* no counter — snap is unusable, not a v3 failure */
                }
                // Stage 3: v2.2 structural must be clean.
                else if (snap->spCountMismatch       != 0u ||
                         snap->spSortedSlotMismatch  != 0u ||
                         snap->spGlobalPacketMismatch != 0u ||
                         snap->spPipelineMismatch     != 0u ||
                         snap->spMaterialIdxMismatch  != 0u ||
                         snap->spTexLayerMismatch     != 0u) {
                    /* no counter — v2.2 gate failed; ordering unreliable */
                }
                // Stage 4: count mismatch → record and fall back.
                else if (snap->staticPropPackets.count != totalCmds) {
                    ++s_spBuildCountMismatch;
                    ++s_spBuildFallback;
                }
                else {
                    // All guards passed — build snapshot arrays.
                    s_snapV6Packets.resize(totalCmds);
                    s_snapV6Meta.resize(totalCmds);

                    for (uint32_t si = 0u; si < totalCmds; ++si) {
                        const ExtractedStaticPropPacket& row =
                            snap->staticPropPackets.data[si];

                        // Snapshot owns slot identity — row must claim this slot.
                        if (row.sortedSlot != si) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Guard: globalPacketIdx in bounds.
                        if (row.globalPacketIdx >=
                                static_cast<uint32_t>(s_packets.size())) {
                            ++s_spBuildPacketMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        const GpuStaticPropPacket& spkt = s_packets[row.globalPacketIdx];

                        // Guard: typeId in type table.
                        if (row.typeId >= static_cast<uint32_t>(s_types.size())) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Fail closed on unknown pipelineId.
                        const uint32_t grp = pipelineId_to_group(row.pipelineId);
                        if (grp == 0xFFFFFFFFu) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // instanceCount from current-frame live (NOT snapshot prev-frame).
                        const auto typeIt = s_typeRanges.find(row.typeId);
                        const uint32_t instCount =
                            (typeIt != s_typeRanges.end())
                                ? typeIt->second.instanceCount : 0u;

                        s_snapV6Meta[si].sortedSlot      = row.sortedSlot;
                        s_snapV6Meta[si].globalPacketIdx = row.globalPacketIdx;
                        s_snapV6Meta[si].typeId          = row.typeId;
                        s_snapV6Meta[si].group           = grp;
                        s_snapV6Meta[si].instanceCount   = instCount;
                        s_snapV6Meta[si].baseInstance    = v6Meta[si].baseInstance;
                        s_snapV6Meta[si].drawIDBase      = si;
                        s_snapV6Meta[si].baseVertex      = spkt.baseVertex;

                        s_snapV6Packets[si]            = RenderCore::DrawPacket{};
                        s_snapV6Packets[si].pipelineId = static_cast<uint32_t>(
                            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                                      : RenderCore::PipelineId::StaticPropAlphaTest);
                        s_snapV6Packets[si].firstIndex = spkt.firstIndex;
                        s_snapV6Packets[si].indexCount = spkt.indexCount;
                    }

                    // Compare snapshot-built vs live-built, field-by-field per slot.
                    for (uint32_t ci = 0u; ci < totalCmds; ++ci) {
                        const StaticPropDispatchMeta& sm = s_snapV6Meta[ci];
                        const StaticPropDispatchMeta& lm = v6Meta[ci];
                        const RenderCore::DrawPacket&  sp = s_snapV6Packets[ci];
                        const RenderCore::DrawPacket&  lp = v6Packets[ci];

                        if (sm.globalPacketIdx != lm.globalPacketIdx) ++s_spBuildMetaMismatch;
                        if (sm.typeId          != lm.typeId)          ++s_spBuildMetaMismatch;
                        if (sm.group           != lm.group)           ++s_spBuildMetaMismatch;
                        if (sm.sortedSlot      != lm.sortedSlot)      ++s_spBuildMetaMismatch;
                        if (sm.baseVertex      != lm.baseVertex)      ++s_spBuildMetaMismatch;
                        if (sm.drawIDBase      != lm.drawIDBase)      ++s_spBuildMetaMismatch;

                        if (sp.pipelineId != lp.pipelineId) ++s_spBuildPacketMismatch;
                        if (sp.firstIndex  != lp.firstIndex)  ++s_spBuildPacketMismatch;
                        if (sp.indexCount  != lp.indexCount)  ++s_spBuildPacketMismatch;
                    }

                    snapBuilt = true;
                }
            }

            // Dispatch ref-swap: use snapshot arrays only when compare clean.
            const bool useSnapshot = snapBuilt
                && s_spBuildPacketMismatch == 0
                && s_spBuildMetaMismatch   == 0;

            if (snapBuilt && !useSnapshot) {
                ++s_spBuildFallback;
                if (!s_spBuildFirstFallbackLogged) {
                    s_spBuildFirstFallbackLogged = true;
                    std::fprintf(stderr,
                        "[RENDER_SNAPSHOT v3] frame=%u first-fallback"
                        " attempted=1 count_mismatch=%u pkt_mismatch=%u"
                        " meta_mismatch=%u fallback=1\n",
                        s_v6TotalFrameCount,
                        s_spBuildCountMismatch,
                        s_spBuildPacketMismatch,
                        s_spBuildMetaMismatch);
                }
            }

            const std::vector<RenderCore::DrawPacket>* pDispatchPackets =
                useSnapshot ? &s_snapV6Packets : &v6Packets;
            const std::vector<StaticPropDispatchMeta>* pDispatchMeta =
                useSnapshot ? &s_snapV6Meta    : &v6Meta;
```

- [ ] **Step 3: Modify dispatch loop — change v6Meta[i]/v6Packets[i] references**

In the dispatch loop `for (uint32_t i = 0u; i < totalCmds; ++i)`, find lines:
```cpp
                const StaticPropDispatchMeta& m  = v6Meta[i];
                const RenderCore::DrawPacket&  dp = v6Packets[i];
```

Replace with:
```cpp
                const StaticPropDispatchMeta& m  = (*pDispatchMeta)[i];
                const RenderCore::DrawPacket&  dp = (*pDispatchPackets)[i];
```

All other lines in the dispatch loop remain unchanged (snap-cull check, zero-inst skip, GL draw call, verbose trace, counter increments).

- [ ] **Step 4: Add v3 lines to 600-frame summary**

The 600-frame summary block starts at `if ((s_v6TotalFrameCount % 600u) == 0u)` (~line 4221). After the existing `std::fprintf` for `[DRAW_PACKET_V6] event=dispatch_summary`, add:

```cpp
                if (s_snapshotBuildEnabled) {
                    std::fprintf(stderr,
                        "[RENDER_SNAPSHOT v3] frame=%u"
                        " attempted=%u count_mismatch=%u pkt_mismatch=%u"
                        " meta_mismatch=%u fallback=%u using_snapshot=%d\n",
                        s_v6TotalFrameCount,
                        s_spBuildAttempted,
                        s_spBuildCountMismatch,
                        s_spBuildPacketMismatch,
                        s_spBuildMetaMismatch,
                        s_spBuildFallback,
                        useSnapshot ? 1 : 0);
                }
```

- [ ] **Step 5: Build**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

Expected: zero errors. v3 builder is live. `render_snapshot.cpp` not yet wired, so `spBuild*` counters don't reach the ok gate yet — that is fine at this step.

- [ ] **Step 6: Quick inner-loop smoke (default OFF)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_01,mc2_24 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0. Gate is off — zero behavioral change.

- [ ] **Step 7: Commit**

```powershell
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): v3 activation guard + snapshot builder + compare + dispatch ref-swap"
```

---

## Task 6: Wire v3 stats into render_snapshot.cpp

**Files:**
- Modify: `GameOS/gameos/render_snapshot.cpp`

Line 310: closing `}` of snap-cull stats block. Lines 312–324: ok gate computation. Lines 342–397: per-frame log fprintf.

- [ ] **Step 1: Add v3 stats read after snap-cull stats (after line 310)**

After the snap-cull stats block closing `}` (line 310) and before the ok gate comment (line 312), insert:

```cpp
    // v3: read snapshot build stats from the most recent flush().
    {
        uint32_t attempted = 0u, countMis = 0u, pktMis = 0u, metaMis = 0u, fallback = 0u;
        batcher_getSnapshotBuildStats(&attempted, &countMis, &pktMis, &metaMis, &fallback);
        snap.spBuildAttempted      = attempted;
        snap.spBuildCountMismatch  = countMis;
        snap.spBuildPacketMismatch = pktMis;
        snap.spBuildMetaMismatch   = metaMis;
        snap.spBuildFallback       = fallback;
    }
```

- [ ] **Step 2: Extend ok gate (lines 314–324)**

Replace the existing ok gate:
```cpp
    snap.ok = (snap.staticPropValidationFail  == 0u &&
               snap.staticPropPacketRangesFail == 0u &&
               snap.staticPropPacketInvalid    == 0u &&
               !snap.arenaOverflow             &&
               snap.spCountMismatch            == 0u &&
               snap.spSortedSlotMismatch       == 0u &&
               snap.spGlobalPacketMismatch     == 0u &&
               snap.spPipelineMismatch         == 0u &&
               snap.spMaterialIdxMismatch      == 0u &&
               snap.spTexLayerMismatch         == 0u &&
               snap.spSnapCullSlotMismatch     == 0u) ? 1u : 0u;
```

With:
```cpp
    // v3: extends v2.3 ok gate — adds three spBuild mismatch counters.
    // spBuildAttempted and spBuildFallback excluded (informational).
    snap.ok = (snap.staticPropValidationFail  == 0u &&
               snap.staticPropPacketRangesFail == 0u &&
               snap.staticPropPacketInvalid    == 0u &&
               !snap.arenaOverflow             &&
               snap.spCountMismatch            == 0u &&
               snap.spSortedSlotMismatch       == 0u &&
               snap.spGlobalPacketMismatch     == 0u &&
               snap.spPipelineMismatch         == 0u &&
               snap.spMaterialIdxMismatch      == 0u &&
               snap.spTexLayerMismatch         == 0u &&
               snap.spSnapCullSlotMismatch     == 0u &&
               snap.spBuildCountMismatch       == 0u &&
               snap.spBuildPacketMismatch      == 0u &&
               snap.spBuildMetaMismatch        == 0u) ? 1u : 0u;
```

- [ ] **Step 3: Update log label and add v3 lines**

In the `std::fprintf(stderr, ...)` call (line ~342):

Change the first format string line from:
```cpp
            "[RENDER_SNAPSHOT v2.3] frame=%llu mechs=%u static_props=%u lights=%u "
```
to:
```cpp
            "[RENDER_SNAPSHOT v3] frame=%llu mechs=%u static_props=%u lights=%u "
```

Add a v3 build stats line to the format string. Find this line near the end of the format string:
```cpp
            "  [v2.3 snap_cull] skipped=%u active=%u slot_mismatch=%u\n",
```

Replace with:
```cpp
            "  [v2.3 snap_cull] skipped=%u active=%u slot_mismatch=%u\n"
            "  [v3 build] attempted=%u count_mismatch=%u pkt_mismatch=%u"
            " meta_mismatch=%u fallback=%u\n",
```

Add corresponding arguments at the end of the argument list (after `snap.spSnapCullSlotMismatch`):
```cpp
            snap.spSnapCullSlotMismatch,
            snap.spBuildAttempted,
            snap.spBuildCountMismatch,
            snap.spBuildPacketMismatch,
            snap.spBuildMetaMismatch,
            snap.spBuildFallback);
```

- [ ] **Step 4: Build**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build64 --config RelWithDebInfo --target mc2 -j8 2>&1 | Select-Object -Last 20
```

Expected: zero errors. v3 stats now flow: batcher counters → `batcher_getSnapshotBuildStats()` → `snap.spBuild*` → ok gate → log.

- [ ] **Step 5: Commit**

```powershell
git add GameOS/gameos/render_snapshot.cpp
git commit -m "feat(snapshot): wire v3 build stats into ok gate and log"
```

---

## Task 7: Add MC2_SNAPSHOT_STATIC_PROP_BUILD to tier1_env_vars.md

**Files:**
- Modify: `docs/tier1_env_vars.md`

Line 113 is the `## Snapshot-assisted dispatch (Extraction v2.3)` heading. Line 115 is the `MC2_SNAP_CULL=1` bullet. The v3 entry goes after line 115.

- [ ] **Step 1: Add entry after MC2_SNAP_CULL bullet (after line 115)**

After the `MC2_SNAP_CULL=1` bullet, insert:

```markdown
- `MC2_SNAPSHOT_STATIC_PROP_BUILD=1` — opt-in snapshot-owned slot identity dispatch (Extraction v3). When enabled, builds a second set of dispatch arrays (`s_snapV6Packets`/`s_snapV6Meta`) from the previous frame's `RenderSnapshot` rows, compares against live-built arrays field-by-field, and dispatches snapshot-built arrays if compare passes (zero mismatch). Falls back to live arrays on any mismatch. Incompatible with `MC2_SNAP_CULL=1` (collision → live dispatch + log line). Counters: `spBuildAttempted`/`spBuildFallback` informational; `spBuildCountMismatch`/`spBuildPacketMismatch`/`spBuildMetaMismatch` in `ok` gate. Default OFF. Cached at process start. Implementation: `gos_static_prop_batcher.cpp`; accessor `batcher_getSnapshotBuildStats()`.
```

- [ ] **Step 2: Commit**

```powershell
git add docs/tier1_env_vars.md
git commit -m "docs: add MC2_SNAPSHOT_STATIC_PROP_BUILD to tier1_env_vars.md"
```

---

## Task 8: Smoke gate — Tier1 default (gate OFF)

**Goal:** Confirm v3 code is inert when `MC2_SNAPSHOT_STATIC_PROP_BUILD` is unset. All `spBuild*` counters must be zero; `ok=1`; no render regression.

- [ ] **Step 1: Run Tier1 5/5 default smoke**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

Expected: exit 0, 5/5 PASS.

- [ ] **Step 2: Verify ok=1 and spBuild* == 0 in logs**

```powershell
$env:MC2_RENDER_SNAPSHOT_LOG = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --missions mc2_01,mc2_24 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_RENDER_SNAPSHOT_LOG -ErrorAction SilentlyContinue

$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Get-ChildItem $logDir -Filter "*.log" | ForEach-Object {
    Select-String "RENDER_SNAPSHOT v3" $_.FullName | Select-Object -Last 2
}
```

Expected: lines with `ok=1` and `[v3 build] attempted=0 count_mismatch=0 pkt_mismatch=0 meta_mismatch=0 fallback=0`.

**Fail conditions:**
- `ok=0` — v3 counters are non-zero when gate is off → counter reset bug in flush()
- Any `[RENDER_SNAPSHOT v3] disabled` line — gate firing when not set → env check bug

- [ ] **Step 3: Commit smoke record**

```powershell
git commit --allow-empty -m "test: tier1 default smoke PASS — v3 gate off, spBuild* zero, ok=1 (5/5)"
```

---

## Task 9: Smoke gate — Tier1 opt-in (MC2_SNAPSHOT_STATIC_PROP_BUILD=1)

**Goal:** Confirm snapshot builder runs, compare passes, snapshot arrays dispatched, `ok=1`, no render regression.

- [ ] **Step 1: Run Tier1 5/5 with v3 gate on**

```powershell
$env:MC2_SNAPSHOT_STATIC_PROP_BUILD = "1"
$env:MC2_RENDER_SNAPSHOT_LOG = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_SNAPSHOT_STATIC_PROP_BUILD -ErrorAction SilentlyContinue
Remove-Item Env:MC2_RENDER_SNAPSHOT_LOG -ErrorAction SilentlyContinue
```

Expected: exit 0, 5/5 PASS.

- [ ] **Step 2: Verify counter state in logs**

```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Get-ChildItem $logDir -Filter "*.log" | ForEach-Object {
    Select-String "RENDER_SNAPSHOT v3" $_.FullName | Select-Object -Last 3
}
```

Expected per mission (steady state, not frame 0 which has no snapshot yet):
- `ok=1`
- `[v3 build] attempted=1 count_mismatch=0 pkt_mismatch=0 meta_mismatch=0 fallback=0`
- `[RENDER_SNAPSHOT v3] frame=N ... using_snapshot=1` in the 600-frame summary

**Fail conditions — investigate before proceeding:**
- `fallback=1` or any mismatch counter > 0 → snapshot compare failed; check for `first-fallback` log line and identify which field diverged
- `ok=0` → mismatch counter in ok gate; trace back to which compare failed
- `attempted=0` → gate check not reaching batcher; verify env var propagation (check env in mc2.exe process)
- `using_snapshot=0` with zero mismatches → dispatch ref-swap logic error

- [ ] **Step 3: Commit smoke record**

```powershell
git commit --allow-empty -m "test: tier1 opt-in smoke PASS — v3 snapshot dispatch, ok=1, fallback=0 (5/5)"
```

---

## Task 10: Smoke gate — Tier1 collision (both gates active)

**Goal:** Confirm `MC2_SNAPSHOT_STATIC_PROP_BUILD=1 MC2_SNAP_CULL=1` produces defined safe behavior — v3 disabled cleanly, live dispatch, no crash, `ok=1`.

- [ ] **Step 1: Run Tier1 5/5 with both gates on**

```powershell
$env:MC2_SNAPSHOT_STATIC_PROP_BUILD = "1"
$env:MC2_SNAP_CULL = "1"
$env:MC2_RENDER_SNAPSHOT_LOG = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
Remove-Item Env:MC2_SNAPSHOT_STATIC_PROP_BUILD -ErrorAction SilentlyContinue
Remove-Item Env:MC2_SNAP_CULL -ErrorAction SilentlyContinue
Remove-Item Env:MC2_RENDER_SNAPSHOT_LOG -ErrorAction SilentlyContinue
```

Expected: exit 0, 5/5 PASS.

- [ ] **Step 2: Verify collision behavior in logs**

```powershell
$logDir = (Get-ChildItem "A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\tests\smoke\artifacts" -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Get-ChildItem $logDir -Filter "*.log" | ForEach-Object {
    Select-String "RENDER_SNAPSHOT v3" $_.FullName | Select-Object -First 2
    Select-String "RENDER_SNAPSHOT v3" $_.FullName | Select-Object -Last 2
}
```

Expected:
- `[RENDER_SNAPSHOT v3] frame=N disabled — MC2_SNAP_CULL collision` (appears once per mission start)
- `ok=1` in snapshot log lines
- `[RENDER_SNAPSHOT v3] frame=N attempted=1 ... fallback=N using_snapshot=0` in 600-frame summary
- No `using_snapshot=1` lines (v3 never dispatches under collision)

**Fail conditions:**
- Crash or exit nonzero → collision guard not firing before null dereference; check Stage 1 guard
- `ok=0` → collision path unexpectedly incrementing a mismatch counter; check that Stage 1 only increments `spBuildFallback`

- [ ] **Step 3: Commit smoke record**

```powershell
git commit --allow-empty -m "test: tier1 collision smoke PASS — both gates, v3 disabled cleanly, ok=1 (5/5)"
```

---

## Plan Self-Review

**Spec coverage:**
- [x] §Authority Boundary (live instanceCount/baseInstance) → T5 builder loop `s_typeRanges.find` + `v6Meta[si].baseInstance`
- [x] §New Static Storage → T3 `s_snapV6Packets/Meta`, `s_snapshotBuildEnabled`
- [x] §flush() Sequence (live first, then snap) → T4 (counter reset), T5 (guard+build+compare+ref-swap)
- [x] §Staged Activation Guard (stages 0–4, Stage 1 collision) → T5 Step 2
- [x] §sortedSlot fail-closed (row.sortedSlot != si) → T5 Step 2 first guard in builder loop
- [x] §OOB guards increment counters immediately → T5 Step 2 all four guard paths
- [x] §typeId guard uses s_types.size() + s_typeRanges.find() → T5 Step 2
- [x] §pipelineId_to_group fail-closed (0xFFFFFFFFu) → T3 Step 2 + T5 Step 2
- [x] §Compare Fields (meta: 6 fields; packet: 3 fields) → T5 Step 2 compare loop
- [x] §spBuildFallback Option A (all fallback paths) → T5 Stage 1 + Stage 4 + compare-fail
- [x] §Dispatch ref-swap (pointer pair, no helper function) → T5 Steps 2–3
- [x] §Log Tag (600-frame rate, first-fallback unconditional) → T5 Steps 2, 4
- [x] §New RenderSnapshot Fields (5 fields) → T1
- [x] §ok gate (3 mismatch counters only) → T6 Step 2
- [x] §Files Touched (tier1_env_vars.md) → T7
- [x] §Snap-Cull Collision (Stage 1 guard + T10 smoke) → T5 Stage 1, T10
- [x] §Smoke Gate (all 3 tiers) → T8, T9, T10
- [x] §Anti-Goals (no shadow, no helper function, no default-ON flip) → confirmed absent from all tasks

**Placeholder scan:** No TBD/TODO/vague references. All code blocks complete.

**Type consistency:**
- `s_snapV6Packets`: `std::vector<RenderCore::DrawPacket>` matches `v6Packets` type ✓
- `s_snapV6Meta`: `std::vector<StaticPropDispatchMeta>` matches `v6Meta` type ✓
- `pDispatchPackets`: `const std::vector<RenderCore::DrawPacket>*` — `(*pDispatchPackets)[i]` yields `const RenderCore::DrawPacket&` ✓
- `pDispatchMeta`: same pattern ✓
- `batcher_getSnapshotBuildStats()`: signature consistent between decl (T2) and impl (T3) ✓
- `snap.spBuildAttempted` etc.: field names match T1 additions ✓
- Log format arg count matches format specifiers in T6 Step 3 ✓
