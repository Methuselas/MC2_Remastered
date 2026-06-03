# Static-Prop Snapshot Finish (DrawPacket v8) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the per-flush *live* static-prop draw-packet builder + compare so the snapshot becomes the sole owner of the dispatch arrays, behind a default-off kill-switch (Commit 1 of a two-commit retire-then-delete shape).

**Architecture:** In `GpuStaticPropBatcher::flush()`, the snapshot path (`s_snapshotBuildEnabled`, default-on since the v3-flip) already builds `s_snapV6Packets`/`s_snapV6Meta` and dispatches them when a per-flush compare vs the live `v6Packets`/`v6Meta` is clean. This plan makes the snapshot path the sole owner: skip the live builder loop + compare by default, re-home the one borrowed field (`baseInstance`) into the snapshot build, keep a per-frame safety-net fallback to a live build only on a *structurally invalid* snapshot, and surface an explicit `spBuildRetired` state. A kill-switch (`MC2_STATIC_PROP_LIVE_BUILDER=1`) restores today's dual-build+compare path.

**Tech Stack:** C++17, OpenGL 4.x (ARB_base_instance), Tracy (`gos_profiler.h`), MSVC RelWithDebInfo via CMake, `run_smoke.py` tier1 gate, user-driven Tracy for perf proof.

**Spec:** `docs/superpowers/specs/2026-06-02-static-prop-snapshot-finish-spec.md` (read it first; this plan implements Commit 1 only).

**Worktree/branch:** `A:/Games/mc2-snapshot-finish-v8` / `claude/static-prop-snapshot-finish-1`.

---

## Conventions for this plan

- **CMAKE** = `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"`
- **Build:** `cd "A:/Games/mc2-snapshot-finish-v8" && "$CMAKE" --build build64 --config RelWithDebInfo --target mc2` — but `build64` does not exist in a fresh worktree. **Task 1 Step 0** configures it once.
- **Smoke (verbatim, PowerShell):** `py -3 A:\Games\mc2-snapshot-finish-v8\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs` — exit 0 = pass. NEVER `--kill-existing`.
- Line numbers below are from HEAD `65541ed4`; grep the named anchor (shown per task) to re-confirm before editing — the file is ~7300 lines and shifts as you edit.
- This is engine GL code: the verification harness is **build + tier1 smoke + `MC2_RENDER_SNAPSHOT_LOG` inspection + user Tracy**, not unit tests (the dispatch path is not GL-free). Where a GL-free assertion is possible (the `spBuildRetired` ok-gate logic) we add a `rendercore_tests` doctest.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `GameOS/gameos/render_snapshot.h` | `RenderSnapshot` struct + ok-gate contract | Add `spBuildRetired` field; update ok-gate comment |
| `GameOS/gameos/render_snapshot.cpp` | `ExtractRenderSnapshot()`, ok-gate computation | Read `spBuildRetired` from batcher; ok-gate accepts retired state |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | `flush()` build/compare/dispatch; stats accessors | New gate + arm log + collision guard; re-home `baseInstance`; retired dispatch path; relocate counters; Tracy zones; extend stats accessor |
| `GameOS/gameos/gos_static_prop_batcher.h` | batcher free-function decls | Extend `batcher_getSnapshotBuildStats` signature |
| `GameOS/gameos/debug_state_dump.cpp` | JSON state dump | Emit `spBuildRetired` |
| `RenderCore/RendererFeatureRegistry.h` | env-var registry (CI-enforced) | Register `MC2_STATIC_PROP_LIVE_BUILDER` |
| `scripts/run_smoke.py` | smoke env propagation allowlist | Add `MC2_STATIC_PROP_LIVE_BUILDER` |
| `docs/tier1_env_vars.md` | env-var docs | Document the kill-switch |
| `docs/active_campaigns.md` | slice ledger | DrawPacket v8 entry |
| `tests/unit/test_rendercore.cpp` | GL-free doctests | ok-gate-accepts-retired assertion |

---

## Task 0: Pre-task — prove the live builder has no out-of-flush side effects (BLOCKING)

Spec §3.5. No code change. Produces a recorded findings block that gates Tasks 4–5. **Do not start Task 4 until this is confirmed against current HEAD.**

**Files:** read-only — `GameOS/gameos/gos_static_prop_batcher.cpp`.

- [ ] **Step 1: Confirm `v6Packets`/`v6Meta` have no reader outside flush.**

Run:
```bash
cd "A:/Games/mc2-snapshot-finish-v8"
grep -rn "v6Packets\|v6Meta\b" GameOS mclib RenderWorld GameAdapters --include=*.cpp --include=*.h
```
Expected: matches confined to `gos_static_prop_batcher.cpp` inside `flush()` (the builder loop ~L5249-5333, lockstep validator ~L5336-5385, snapshot build borrow ~L5528, dispatch select ~L5583-5590). Record: "no external reader" or list any found.

- [ ] **Step 2: Identify the single live→snapshot data dependency.**

Confirm (grep `baseInstance` within flush) that the snapshot builder borrows exactly one live-produced value:
`s_snapV6Meta[si].baseInstance = v6Meta[si].baseInstance;` (~L5528), where the live value is `v6Meta[i].baseInstance = baseInstanceMap[i];` (~L5318), and `baseInstanceMap` is `s_baseInstanceByCmdMap + s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame` (~L5272-5275). Record: "`baseInstance` is the only borrowed field; re-derivable from `baseInstanceMap` — see Task 4."

- [ ] **Step 3: Inventory live-builder side-effect counters and their consumers.**

Run:
```bash
grep -rn "s_v6FrameDrawsIssued\|s_v6FrameZeroInstSkips\|s_v6FrameSortedOob\|s_v6FramePacketOob\|s_v6FrameTypeOob\|s_v6FrameLockstepViolations\|s_v6FrameGlErrors\|s_v6TotalFrameCount" GameOS --include=*.cpp --include=*.h
```
For each: note whether it is (a) reset/incremented in the live builder loop, (b) incremented in the dispatch loop (survives retirement), (c) read by any `batcher_get*` / log / ImGui consumer. Record a table. Counters that are ONLY produced by the live builder loop AND have an external consumer must be relocated in Task 5; counters incremented in the dispatch loop (e.g. `s_v6FrameZeroInstSkips` at ~L5612) survive unchanged.

- [ ] **Step 4: Confirm no live-builder output feeds shadow / cull / material binding.**

Confirm `flushShadow()` (separate function) does not read `v6Packets`/`v6Meta`/`s_v6Frame*`; confirm the dispatch loop's texture binding (`s_texArrayOn`, bc7 buckets) is driven by `m.group`/`s_slotBucketIndex`, both available in the snapshot meta. Record: "shadow/cull/material binding independent of live builder."

- [ ] **Step 5: Write findings into the plan progress log and commit (docs only).**

Create `docs/superpowers/progress/2026-06-02-snapshot-finish-task0-findings.md` with the four recorded results. Commit:
```bash
git add -f docs/superpowers/progress/2026-06-02-snapshot-finish-task0-findings.md
git commit -m "docs(v8): Task 0 pre-task findings — live-builder side-effect proof"
```
Expected: if Step 2 finds a borrowed field OTHER than `baseInstance`, or Step 3 finds a consumed counter only produced in the live loop, STOP and revise Tasks 4–5 before continuing.

---

## Task 1: `spBuildRetired` field + ok-gate + stats plumbing + dumps

Adds the explicit retired-state field end-to-end (no dispatch behavior yet — `spBuildRetired` stays 0 until Task 2 sets it). Spec §3.3.

**Files:**
- Modify: `GameOS/gameos/render_snapshot.h` (struct ~L242, ok-gate comment ~L200)
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp` (static decl near `s_spBuildAttempted`; accessor ~L7246)
- Modify: `GameOS/gameos/gos_static_prop_batcher.h` (decl ~L547)
- Modify: `GameOS/gameos/render_snapshot.cpp` (stats read ~L378, ok-gate ~L396)
- Modify: `GameOS/gameos/debug_state_dump.cpp` (~L191)
- Test: `tests/unit/test_rendercore.cpp`

- [ ] **Step 0: Configure the build tree (one-time for this worktree).**

Run:
```bash
cd "A:/Games/mc2-snapshot-finish-v8"
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -S . -B build64 -G "Visual Studio 17 2022" -A x64
```
Expected: configure completes, `build64/CMakeCache.txt` created. (If a `CMakePresets.json` exists, prefer `--preset` per repo norm; check with `ls CMakePresets.json`.)

- [ ] **Step 1: Add the `spBuildRetired` field to `RenderSnapshot`.**

In `render_snapshot.h`, after the `spBuildFallback` line (~L242):
```cpp
    uint32_t spBuildFallback       = 0u;  // gate enabled/attempted but snapshot arrays NOT dispatched
    // v8 STATIC-PROP-SNAPSHOT-FINISH: explicit retired state. Informational (excluded from ok gate
    // as a mismatch; but the gate must NOT flag spBuildAttempted==0 as failure when this is 1).
    //   spBuildRetired==1                       → live builder + compare intentionally skipped (sole-owner).
    //   spBuildRetired==0 && spBuildAttempted==0 → suspicious / unvalidated.
    //   spBuildRetired==0 && spBuildAttempted==1 → dual-build path ran the compare (kill-switch on).
    uint32_t spBuildRetired        = 0u;
```

- [ ] **Step 2: Update the ok-gate comment block** (~L206-208) to note the retired semantics:
```cpp
    // Informational (excluded from ok): spInstanceCountMismatch, spSnapCullSkipped,
    //   spSnapCullActive, spBuildAttempted, spBuildFallback, spBuildRetired,
    //   mechSnapshotCount, mechMatValid, mechMatSentinel.
    // v8: when spBuildRetired==1 there is no per-flush compare; spBuild*Mismatch are 0 by
    //   construction and the gate must not treat spBuildAttempted==0 as a failure.
```

- [ ] **Step 3: Add the batcher-side static counter.**

In `gos_static_prop_batcher.cpp`, grep `static uint32_t s_spBuildAttempted` and add beside it:
```cpp
static uint32_t s_spBuildRetired       = 0u;  // v8: 1 when live builder + compare retired this flush
```

- [ ] **Step 4: Extend `batcher_getSnapshotBuildStats` to output it.**

In `gos_static_prop_batcher.h` (~L547) change the decl:
```cpp
void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback, uint32_t* retired);
```
In `gos_static_prop_batcher.cpp` (~L7246) match it and add:
```cpp
void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback, uint32_t* retired)
{
    if (attempted)      *attempted      = s_spBuildAttempted;
    if (countMismatch)  *countMismatch  = s_spBuildCountMismatch;
    if (packetMismatch) *packetMismatch = s_spBuildPacketMismatch;
    if (metaMismatch)   *metaMismatch   = s_spBuildMetaMismatch;
    if (fallback)       *fallback       = s_spBuildFallback;
    if (retired)        *retired        = s_spBuildRetired;
}
```

- [ ] **Step 5: Read it in `ExtractRenderSnapshot` and feed the ok-gate.**

In `render_snapshot.cpp` (~L378-386):
```cpp
    // v3/v8: read snapshot build stats from the most recent flush().
    {
        uint32_t attempted = 0u, countMis = 0u, pktMis = 0u, metaMis = 0u, fallback = 0u, retired = 0u;
        batcher_getSnapshotBuildStats(&attempted, &countMis, &pktMis, &metaMis, &fallback, &retired);
        snap.spBuildAttempted      = attempted;
        snap.spBuildCountMismatch  = countMis;
        snap.spBuildPacketMismatch = pktMis;
        snap.spBuildMetaMismatch   = metaMis;
        snap.spBuildFallback       = fallback;
        snap.spBuildRetired        = retired;
    }
```
The ok-gate (~L396) is unchanged in formula: `spBuild*Mismatch==0` already holds when retired (they stay 0). No new term needed — but add an inline comment above L407 so future readers know retired is intentionally not a gate term:
```cpp
               // v8: spBuild*Mismatch are 0 when spBuildRetired==1 (no compare) — gate stays valid.
               snap.spBuildCountMismatch       == 0u &&
```

- [ ] **Step 6: Emit `spBuildRetired` in the JSON dump.**

In `debug_state_dump.cpp` (~L187-191), change the block so `spBuildMetaMismatch` keeps a trailing comma and add the new last field:
```cpp
    s << "    \"spBuildMetaMismatch\": " << snap.spBuildMetaMismatch << ",\n";
    s << "    \"spBuildRetired\": " << snap.spBuildRetired << "\n";
```

- [ ] **Step 7: Add a GL-free doctest for the retired ok-gate semantics.**

In `tests/unit/test_rendercore.cpp`, add (adapt include of `render_snapshot.h` if the test target can see it; if `RenderSnapshot` is not visible to `rendercore_tests`, SKIP this step and rely on smoke — record the skip):
```cpp
TEST_CASE("RenderSnapshot ok-gate tolerates retired build state" * doctest::test_suite("RenderCore")) {
    RenderSnapshot snap;                 // all mismatch counters default 0
    snap.spBuildRetired   = 1u;
    snap.spBuildAttempted = 0u;
    // Recompute ok the same way ExtractRenderSnapshot does for the static-prop terms:
    const bool ok = (snap.staticPropValidationFail == 0u &&
                     snap.spBuildCountMismatch == 0u &&
                     snap.spBuildPacketMismatch == 0u &&
                     snap.spBuildMetaMismatch == 0u);
    CHECK(ok);   // retired + attempted==0 must NOT fail the static-prop gate terms
}
```

- [ ] **Step 8: Build.**

Run: `cd "A:/Games/mc2-snapshot-finish-v8" && "$CMAKE" --build build64 --config RelWithDebInfo --target mc2`
Expected: links clean; `render_snapshot.cpp`, `gos_static_prop_batcher.cpp`, `debug_state_dump.cpp` recompile.

- [ ] **Step 9: Run the GL-free tests (if Step 7 added one).**

Run: `"$CMAKE" --build build64 --config RelWithDebInfo --target rendercore_tests` then run the test exe with `--ts=RenderCore`.
Expected: new case PASS. (If skipped in Step 7, note "no unit coverage; smoke-only".)

- [ ] **Step 10: Commit.**
```bash
git add -f GameOS/gameos/render_snapshot.h GameOS/gameos/render_snapshot.cpp \
  GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp \
  GameOS/gameos/debug_state_dump.cpp tests/unit/test_rendercore.cpp
git commit -m "feat(v8): add explicit spBuildRetired field + stats plumbing (no behavior change)"
```

---

## Task 2: New gate + arm log + collision guard (no dispatch change yet)

Adds `MC2_STATIC_PROP_LIVE_BUILDER` and computes `retireLiveBuilder`, sets `s_spBuildRetired`, emits the arm log — but does NOT yet branch dispatch (Task 4). This isolates the gate wiring from the behavior change. Spec §3.0, §3.1.

**Files:** Modify `GameOS/gameos/gos_static_prop_batcher.cpp` (gate block near `s_snapshotBuildEnabled` ~L127; flush ~L5247).

- [ ] **Step 1: Add the kill-switch gate** beside `s_snapshotBuildEnabled` (~L130):
```cpp
// STATIC-PROP-SNAPSHOT-FINISH (v8): snapshot is the sole draw-packet owner.
// DEBUG/LEGACY kill-switch — default OFF (retired). =1 restores the v3-flip dual
// build (live + snapshot_packet_build) + compare path for regression bisect / A-B.
static const bool s_keepLiveBuilder = []() -> bool {
    const char* keep = std::getenv("MC2_STATIC_PROP_LIVE_BUILDER");
    return keep && keep[0] == '1';
}();
// One-shot arm log latch.
static bool s_v8ArmLogged = false;
```

- [ ] **Step 2: Compute `retireLiveBuilder` and arm-log, at the top of the `if (runV6)` block** (~L5253, immediately after `++s_v6TotalFrameCount;`):
```cpp
            // v8: retire the live builder only when the kill-switch is unset AND
            // snapshot_packet_build is enabled (it produces the dispatch arrays) AND
            // snap-cull is not colliding (snap-cull disables snapshot_packet_build below).
            const bool retireLiveBuilder =
                !s_keepLiveBuilder && s_snapshotBuildEnabled && !s_snapCullEnabled;
            s_spBuildRetired = retireLiveBuilder ? 1u : 0u;
            if (!s_v8ArmLogged) {
                s_v8ArmLogged = true;
                const char* reason =
                    retireLiveBuilder         ? "snapshot_sole_owner" :
                    s_keepLiveBuilder         ? "live_builder_forced" :
                    !s_snapshotBuildEnabled   ? "snapshot_packet_build_disabled_keep_live" :
                                                "snap_cull_collision_keep_live";
                std::fprintf(stderr,
                    "[STATIC_PROP_PACKET_DISPATCH v8] event=arm live_builder_retired=%d"
                    " snapshot_packet_build=%d live_builder_forced=%d reason=%s\n",
                    retireLiveBuilder ? 1 : 0,
                    s_snapshotBuildEnabled ? 1 : 0,
                    s_keepLiveBuilder ? 1 : 0,
                    reason);
                std::fflush(stderr);
            }
```
NOTE: `retireLiveBuilder` is declared here but first *used* for branching in Task 4. Until then it only drives the log + `s_spBuildRetired`. That is intentional — behavior is unchanged this task.

- [ ] **Step 3: Build.**

Run: `"$CMAKE" --build build64 --config RelWithDebInfo --target mc2`
Expected: clean. A `-Wunused-variable` on `retireLiveBuilder` is acceptable here (consumed in Task 4); if the build treats warnings as errors, add `(void)retireLiveBuilder;` after the log block and remove it in Task 4.

- [ ] **Step 4: Smoke + confirm arm log.**

Run: `py -3 A:\Games\mc2-snapshot-finish-v8\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs`
Then grep a mission log under `tests/smoke/artifacts/<ts>/` for the arm line.
Expected: tier1 5/5 PASS; arm log shows `live_builder_retired=1 ... reason=snapshot_sole_owner` (default env). `+0 destroys`. Behavior identical (dispatch unchanged this task).

- [ ] **Step 5: Commit.**
```bash
git add -f GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(v8): add MC2_STATIC_PROP_LIVE_BUILDER gate + arm log + collision guard (no dispatch change)"
```

---

## Task 3: Tracy subzones around live-build / snapshot-build / compare

So the perf proof can attribute the delta (spec §3.6). Per-loop zones only (100ns floor).

**Files:** Modify `GameOS/gameos/gos_static_prop_batcher.cpp`. Confirm `#include "gos_profiler.h"` is present (grep; the file already uses `ZoneScopedN` for `GpuStaticProps.Flush` — it is).

- [ ] **Step 1: Wrap the live builder loop.** Around the builder `for` (~L5280) and its lockstep validator, open a scope:
```cpp
            {
            ZoneScopedN("StaticProp.LiveBuild");
            // --- Builder loop ---
            for (uint32_t i = 0u; i < totalCmds; ++i) {
                ... existing builder body ...
            }
            ... existing lockstep validator ...
            s_v6FrameLockstepViolations = v6LockstepViolations;
            }  // end StaticProp.LiveBuild
```
(Place the closing brace right after `s_v6FrameLockstepViolations = v6LockstepViolations;` ~L5385. `v6LockstepViolations` is declared earlier at ~L5251 and read later at L5393 — keep it OUTSIDE this scope; only wrap the loop+validator that WRITE it. Verify it remains in scope at L5393.)

- [ ] **Step 2: Wrap the snapshot builder loop** — around the `for (uint32_t si...)` build loop (~L5481), inside the `else` at L5476:
```cpp
                else {
                    ZoneScopedN("StaticProp.SnapshotBuild");
                    s_snapV6Packets.resize(totalCmds);
                    ... existing snapshot build loop ...
```
(The `ZoneScopedN` lives to the end of the `else` block, which also contains the compare loop — acceptable; or split per Step 3.)

- [ ] **Step 3: Wrap the compare loop** — around the field-by-field compare `for (uint32_t ci...)` (~L5541):
```cpp
                    {
                    ZoneScopedN("StaticProp.BuildCompare");
                    for (uint32_t ci = 0u; ci < totalCmds; ++ci) {
                        ... existing compare body ...
                    }
                    }  // end StaticProp.BuildCompare
```

- [ ] **Step 4: Build + smoke.**

Run build, then smoke tier1.
Expected: clean, 5/5 PASS, +0 destroys. (Zones are observational; no behavior change.)

- [ ] **Step 5: Commit.**
```bash
git add -f GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "perf(v8): Tracy subzones StaticProp.LiveBuild/SnapshotBuild/BuildCompare"
```

---

## Task 4: Retired dispatch path — re-home `baseInstance`, skip live build+compare, safe fallback

The behavior change. Default: skip the live builder + compare, dispatch the snapshot arrays; fall back to a live build only on a *structurally invalid* snapshot (NOT empty). Spec §3.2.

**Files:** Modify `GameOS/gameos/gos_static_prop_batcher.cpp` (the `if (runV6)` builder ~L5253; the dispatch block ~L5393-5590).

- [ ] **Step 1: Gate the live builder loop on `!retireLiveBuilder`.**

Wrap the live builder loop + lockstep validator (the `StaticProp.LiveBuild` scope from Task 3) so it only runs when NOT retired OR when a fallback is needed. Because the fallback decision depends on snapshot validity computed later, run the live build when `!retireLiveBuilder` here, and add a *deferred* live build in Step 4 for the invalid-snapshot fallback. Change the scope open:
```cpp
            if (!retireLiveBuilder) {
            ZoneScopedN("StaticProp.LiveBuild");
            // --- Builder loop ---  (unchanged body)
            ...
            s_v6FrameLockstepViolations = v6LockstepViolations;
            }  // end StaticProp.LiveBuild / !retireLiveBuilder
```

- [ ] **Step 2: Make `baseInstanceMap` available in the dispatch block.**

The snapshot build (~L5481) borrows `v6Meta[si].baseInstance`, which is empty when the live build is skipped. Re-home it. At the top of the dispatch block (~L5393, inside `if (runV6 && v6LockstepViolations == 0u)`, before the snapshot builder), recompute the pointer (same arithmetic as L5272-5275):
```cpp
            // v8: snapshot build must derive baseInstance itself when the live builder is retired.
            const size_t fr_off_bi_disp =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMapDisp = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_disp);
```

- [ ] **Step 3: Source `baseInstance` from the re-homed map in the snapshot build.**

Change the borrow (~L5528) from the live array to the re-homed map:
```cpp
                        s_snapV6Meta[si].baseInstance    = baseInstanceMapDisp[si];
```
(For `si` that pass all guards, `si == row.sortedSlot == sorted slot index`, identical to the live builder's `baseInstanceMap[i]`. The kill-switch A/B in validation P2 confirms byte-identity.)

- [ ] **Step 4: Skip the compare and branch dispatch when retired; add the invalid-snapshot fallback.**

The compare loop (~L5541) and `useSnapshot` (~L5564) assume `v6Meta`/`v6Packets` exist. Restructure the dispatch selection (~L5559-5586) to:
```cpp
                    snapBuilt = true;
                }  // end else (snapshot built) — Task 3 BuildCompare scope already closed before this
            }

            // v8: dispatch selection.
            bool useSnapshot;
            if (retireLiveBuilder) {
                // Snapshot is the sole owner. Dispatch it unless STRUCTURALLY invalid.
                // snapshotInvalid = NOT built (guards failed) — i.e. snap nullptr / ok!=1 /
                // count mismatch / malformed metadata. An empty-but-valid snapshot (snapBuilt
                // with totalCmds==0) is VALID and dispatches zero — never a fallback.
                const bool snapshotInvalid = !snapBuilt;
                if (snapshotInvalid) {
                    // Per-frame survival net: build live arrays for THIS frame only.
                    ++s_spBuildFallback;
                    ZoneScopedN("StaticProp.LiveBuild");   // fallback build (rare; counted)
                    for (uint32_t i = 0u; i < totalCmds; ++i) {
                        ... // EXACT COPY of the builder-loop body from Step 1 (uses baseInstanceMapDisp)
                    }
                    useSnapshot = false;
                } else {
                    useSnapshot = true;
                }
            } else {
                // Legacy dual-build path (kill-switch on): compare-gated select (unchanged).
                useSnapshot = snapBuilt
                    && s_spBuildPacketMismatch == 0
                    && s_spBuildMetaMismatch   == 0;
                if (snapBuilt && !useSnapshot) {
                    ++s_spBuildFallback;
                    ... // existing first-fallback log (unchanged)
                }
            }
```
**DRY note:** the builder-loop body appears twice (Step 1 main path + Step 4 fallback). To avoid divergence, extract it into a file-local helper `static void buildLiveV6Arrays(uint32_t totalCmds, const uint32_t* baseInstanceMap, std::vector<RenderCore::DrawPacket>& outPackets, std::vector<StaticPropDispatchMeta>& outMeta, /* counters by ref */ ...)` and call it from both sites. If the parameter list grows unwieldy (many `s_v6Frame*` counters), keep the counters as the existing file-statics (the helper writes them directly) and pass only `totalCmds` + `baseInstanceMap`. Implement the helper, replace both loop bodies with a call.

- [ ] **Step 5: Guard the snapshot builder's collision/structural stages for retired mode.**

The snapshot builder's Stage-1 snap-cull collision (~L5448) sets `++s_spBuildFallback` and leaves `snapBuilt=false`. In retired mode this can't happen (`retireLiveBuilder` requires `!s_snapCullEnabled`), so it's inert. Confirm no path sets `snapBuilt=false` for an *empty-but-valid* snapshot: when `totalCmds==0`, the builder `else` branch (~L5476) still runs, resizes to 0, the loops are no-ops, and sets `snapBuilt=true` → dispatch zero. Verify by reading; if `totalCmds==0` short-circuits earlier and leaves `snapBuilt=false`, add an explicit `if (totalCmds==0u) { snapBuilt = true; }` in retired mode so zero-props is VALID, not a fallback. Record which case holds.

- [ ] **Step 6: Build.**

Run: `"$CMAKE" --build build64 --config RelWithDebInfo --target mc2`
Expected: clean.

- [ ] **Step 7: Smoke (default = retired) + zero-fallback check.**

Run: `py -3 A:\Games\mc2-snapshot-finish-v8\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs`
Then, with `MC2_RENDER_SNAPSHOT_LOG=1` on a single mission:
```powershell
$env:MC2_RENDER_SNAPSHOT_LOG="1"
py -3 A:\Games\mc2-snapshot-finish-v8\scripts\run_smoke.py --missions mc2_24 --duration 30 --keep-logs
Remove-Item Env:\MC2_RENDER_SNAPSHOT_LOG
```
Expected: tier1 5/5 PASS, +0 destroys, GL-clean. Log shows `spBuildRetired=1` (after Task 7 wires it into the log line — until then check the JSON dump via `MC2_DEBUG_STATE_DUMP=1`) and **`spBuildFallback=0`** every frame. ANY fallback = an extraction bug; STOP and diagnose (do not proceed).

- [ ] **Step 8: A/B kill-switch sanity.**

Run mc2_24 with `MC2_STATIC_PROP_LIVE_BUILDER=1`; expect identical visuals + 5/5 + arm log `live_builder_retired=0 reason=live_builder_forced`.

- [ ] **Step 9: Commit.**
```bash
git add -f GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(v8): retire live builder by default — snapshot sole owner, re-homed baseInstance, structural-invalid fallback"
```

---

## Task 5: Relocate any live-builder-only diagnostic counter consumed elsewhere

Driven by Task 0 Step 3 findings. Only act on counters that (a) are written ONLY in the live builder loop AND (b) have an external consumer that would now read a stale/zero value when retired.

**Files:** Modify `GameOS/gameos/gos_static_prop_batcher.cpp`.

- [ ] **Step 1: For each counter flagged in Task 0 Step 3, relocate its increment.**

Example pattern — if `s_v6FrameSortedOob`/`s_v6FramePacketOob`/`s_v6FrameTypeOob` (OOB guards in the live builder, ~L5283/5292/5301) feed a consumed `batcher_get*` or summary log, they would zero in retired mode. Move equivalent guard checks into the snapshot dispatch loop, or compute them in the shared `buildLiveV6Arrays` helper which now runs only as fallback. If a counter's ONLY consumer is the lockstep validator (also retired), it needs no relocation — record that. For each: show the exact moved increment with its new call site.

(If Task 0 found NO externally-consumed live-only counter — the likely outcome, since `s_v6Frame*` are diagnostic and `s_v6FrameZeroInstSkips` is incremented in the dispatch loop which survives — this task is a no-op: record "no relocation needed" and skip to commit. Do NOT invent relocations.)

- [ ] **Step 2: Build + smoke.**

Expected: 5/5 PASS, +0 destroys.

- [ ] **Step 3: Commit (or record no-op).**
```bash
git add -f GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(v8): relocate live-only diagnostic counters surviving live-builder retirement"
```
(If no-op: skip the commit; note in the progress doc.)

---

## Task 6: Env registration, smoke propagation, log line, docs

Makes CI green and the soak observable. Spec §3.4.

**Files:**
- Modify: `RenderCore/RendererFeatureRegistry.h` (`kAuxEnvVars`)
- Modify: `scripts/run_smoke.py` (propagation allowlist)
- Modify: `GameOS/gameos/gameosmain.cpp` (`MC2_RENDER_SNAPSHOT_LOG` line ~L1303)
- Modify: `docs/tier1_env_vars.md`, `docs/active_campaigns.md`

- [ ] **Step 1: Register the env var.** In `RendererFeatureRegistry.h`, grep `kAuxEnvVars` and add an entry (match the existing entry shape exactly — string + comment):
```cpp
    "MC2_STATIC_PROP_LIVE_BUILDER",   // v8 kill-switch: =1 restores live build + snapshot compare
```

- [ ] **Step 2: Add to smoke propagation.** In `scripts/run_smoke.py`, grep `MC2_STATIC_PROP_LEGACY_DISPATCH` (the v7 kill-switch, already in the allowlist) and add `"MC2_STATIC_PROP_LIVE_BUILDER"` adjacent, matching the existing tuple/list format.

- [ ] **Step 3: Surface `spBuildRetired` in the snapshot log line.** In `gameosmain.cpp` (~L1303), the `MC2_RENDER_SNAPSHOT_LOG` `[DRAW_PACKET v1]` line — append `spBuildRetired` to the format + args using `snap.spBuildRetired` (add `" retired=%u"` and the arg). Show the exact edited `fprintf`.

- [ ] **Step 4: Run the env-registry contract check.**

Run: `cd "A:/Games/mc2-snapshot-finish-v8" && bash scripts/check-contracts.sh`
Expected: 8/8 PASS (env_registry green with the new var registered).

- [ ] **Step 5: Document.** Add to `docs/tier1_env_vars.md` (under the DrawPacket/static-prop group):
```
MC2_STATIC_PROP_LIVE_BUILDER — default OFF. Legacy/debug kill-switch.
  unset/0: snapshot is sole draw-packet owner (live builder + per-flush compare retired).
  1:       restore v3-flip dual build (live + snapshot_packet_build) + compare.
```
Add a one-line DrawPacket v8 entry to `docs/active_campaigns.md` under the DrawPacket line.

- [ ] **Step 6: Build + smoke + commit.**
```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
# smoke tier1 5/5
git add -f RenderCore/RendererFeatureRegistry.h scripts/run_smoke.py \
  GameOS/gameos/gameosmain.cpp docs/tier1_env_vars.md docs/active_campaigns.md
git commit -m "chore(v8): register kill-switch, smoke propagation, retired in snapshot log + docs"
```

---

## Task 7: Validation gates (spec §5) — record evidence

No code change; produces the merge-gate evidence. Captures requiring user-driven Tracy (P3) are handed to the user.

**Files:** append results to `docs/superpowers/progress/2026-06-02-snapshot-finish-task0-findings.md` (rename section to "Validation").

- [ ] **Step 1: P1 — zero mismatch + zero fallback + retired active.** Run tier1 with `MC2_RENDER_SNAPSHOT_LOG=1`; confirm every frame `spBuild*Mismatch=0`, `spBuildFallback=0`, `spBuildRetired=1`, `ok=1`; specifically scan mc2_10 frames 1706–3326. Record pass/fail per mission.

- [ ] **Step 2: P4 — snapshot_packet_build disabled.** Run mc2_24 with `MC2_SNAPSHOT_STATIC_PROP_BUILD=0`; confirm arm log `live_builder_retired=0 reason=snapshot_packet_build_disabled_keep_live`, no vanished props, GL-clean.

- [ ] **Step 3: P5 — gates green.** `bash scripts/check-contracts.sh` 8/8; tier1 5/5 `+0 destroys`; kill-switch A/B (Task 4 Step 8) byte-identical.

- [ ] **Step 4: P2 + P3 — HAND TO USER (cannot self-drive).** Write the exact capture instructions for the user: same-camera A/B (retired vs `MC2_STATIC_PROP_LIVE_BUILDER=1`) on mc2_24 + a terrain-object-dense mission for pixel identity (P2); user-driven wolfman Tracy reporting `GpuStaticProps.Flush` self-time, `textureManagerRenderLists`, `StaticProp.LiveBuild`+`StaticProp.BuildCompare`→~0, and `Extract.SP.Fill` UNCHANGED (P3). Record as "pending user capture."

- [ ] **Step 5: Commit the validation record.**
```bash
git add -f docs/superpowers/progress/2026-06-02-snapshot-finish-task0-findings.md
git commit -m "docs(v8): validation evidence — P1/P4/P5 recorded, P2/P3 pending user capture"
```

---

## Out of scope (do NOT implement here)

- `Extract.SP.Fill` dirty-list (`RenderWorld::fillStaticPropSlots` double-scan) — follow-up `PERF-EXTRACT-SNAPSHOT-FILL-DIRTYLIST-1`.
- Deleting the live builder code — Commit 2 (`STATIC-PROP-LIVE-BUILDER-DELETE`), only after the multi-mission soak in spec §4.
- Shadow pass (`flushShadow`) — stays legacy (v6-arch §7).
- Mech/terrain/VFX extraction, sort keys, GPU-cull count integration.

---

## Self-review (completed by plan author)

- **Spec coverage:** §3.0 naming → Task 2/6 logs+docs; §3.1 gate+collision guard → Task 2; §3.2 snapshotInvalid+fallback → Task 4 Steps 4-5; §3.3 spBuildRetired → Task 1; §3.4 supporting → Task 6; §3.5 pre-task → Task 0; §3.6 Tracy → Task 3; §5 validation → Task 7; two-commit shape → Out-of-scope (Commit 2 deferred). Covered.
- **Type consistency:** `batcher_getSnapshotBuildStats` 6-arg signature consistent (Task 1 .h + .cpp + Task 1 Step 5 caller). `s_spBuildRetired`/`spBuildRetired`/`retireLiveBuilder`/`s_keepLiveBuilder` named consistently throughout.
- **Known risk in plan:** Task 4 Step 4 duplicates the builder loop body for fallback — mitigated by the DRY helper note (`buildLiveV6Arrays`). The executor MUST extract the helper rather than copy-paste, to keep main + fallback builds identical.
- **Placeholder scan:** Task 5 is conditionally a no-op (explicitly allowed, driven by Task 0 evidence — not a placeholder). Task 0 Step 5 / Task 7 Step 4 produce evidence docs, not deferred code.
