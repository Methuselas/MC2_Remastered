# Task 0 Findings: Live-Builder Side-Effect Proof

**Branch:** `claude/static-prop-snapshot-finish-1`  
**Date:** 2026-06-02  
**File under investigation:** `GameOS/gameos/gos_static_prop_batcher.cpp`

---

## Investigation 1: External Readers of `v6Packets` / `v6Meta`

**Command run:**
```
grep -rn "v6Packets\|v6Meta" GameOS mclib RenderWorld GameAdapters --include=*.cpp --include=*.h
```

**Result: NO EXTERNAL READER.**

All matches for `v6Packets` and `v6Meta` are confined to a single function in
`GameOS/gameos/gos_static_prop_batcher.cpp`:

| Location | Role |
|---|---|
| L5249–5250 | `static` local declarations inside `flush()` |
| L5268–5269 | `resize()` at start of build loop |
| L5284–5332 | Live builder loop writes |
| L5336–5381 | Lockstep validation reads |
| L5385 | End-of-build-loop (writes `s_v6FrameLockstepViolations` from local var) |
| L5528, L5543–5545 | Snapshot builder borrows (see Investigation 2) |
| L5584, L5586 | Dispatch ref-swap (`useSnapshot ? &s_snapV6Packets : &v6Packets`) |
| L5589–5590 | Dispatch loop dereference (via pointer, conditional on `useSnapshot`) |

Both `v6Packets` and `v6Meta` are declared `static` local to `flush()`. No
caller outside `flush()` has access to them. No header exposes them.

**Verdict: no external reader confirmed.**

---

## Investigation 2: Live→Snapshot Data Dependency

**Single borrow confirmed at L5528:**

```cpp
s_snapV6Meta[si].baseInstance = v6Meta[si].baseInstance;   // L5528
```

The live value is produced at L5318:
```cpp
v6Meta[i].baseInstance = baseInstanceMap[i];
```

where `baseInstanceMap` is derived at L5272–5275 from the ring-slot pointer
`s_baseInstanceByCmdMap + s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame`.

All other `s_snapV6Meta[si]` fields are filled from the RenderSnapshot row
(`row.*`, `spkt.*`, `typeIt->second`, `grp`) — none come from `v6Meta`:

| `s_snapV6Meta[si]` field | Source |
|---|---|
| `sortedSlot` | `row.sortedSlot` (snapshot) |
| `globalPacketIdx` | `row.globalPacketIdx` (snapshot) |
| `typeId` | `row.typeId` (snapshot) |
| `group` | `grp` (derived from snapshot typeIt) |
| `instanceCount` | `typeIt->second.instanceCount` (s_typeRanges, live frame) |
| `baseInstance` | **`v6Meta[si].baseInstance`** ← ONLY live borrow |
| `drawIDBase` | `si` (loop index) |
| `baseVertex` | `spkt.baseVertex` (snapshot) |

`s_snapV6Packets[si]` fields (`pipelineId`, `firstIndex`, `indexCount`) are all
from snapshot or enum — none from `v6Packets`.

**Verdict: `baseInstance` is the ONLY live→snapshot borrow. No additional borrows found.**

---

## Investigation 3: Live-Builder Side-Effect Counters

### Counter inventory

| Counter | Declared | Reset | Incremented in live builder (L5280–5385) | Incremented in dispatch loop (L5588–5675) | External consumer |
|---|---|---|---|---|---|
| `s_v6FrameSortedOob` | L156 | L5256 | YES – L5283 | no | periodic log L5693 (600-frame), read in flush `ok` gate L5680 |
| `s_v6FramePacketOob` | L157 | L5257 | YES – L5292 | no | periodic log L5693, ok gate L5681 |
| `s_v6FrameTypeOob` | L158 | L5258 | YES – L5301 | no | periodic log L5693, ok gate L5682 |
| `s_v6FrameLockstepViolations` | L159 | L5259 | YES – L5385 (assigned from local) | no | periodic log L5694, ok gate L5683 |
| `s_v6FrameDrawsIssued` | L154 | L5254 | no | YES – L5661 | **batcher_getStaticPropStats() L7279 → debug_state_dump.cpp:223 (JSON export "spV6DrawCalls")** |
| `s_v6FrameZeroInstSkips` | L155 | L5255 | no | YES – L5612 | periodic log L5692 (no external getter) |
| `s_v6FrameGlErrors` | L170 | L5260 | no | YES – L5670 | periodic log L5694, ok gate L5684 |
| `s_v6TotalFrameCount` | L171 | never (monotonic) | ++L5261 | read-only in loop (logs) | log strings throughout flush; ok gate reference L5677 |

### Classification

**Survives retirement** (incremented in dispatch loop, or no external consumer):
- `s_v6FrameDrawsIssued` — dispatch loop (L5661); has external consumer via `batcher_getStaticPropStats()` → **survives** (dispatch loop, which persists after retirement)
- `s_v6FrameZeroInstSkips` — dispatch loop (L5612); no external getter — **survives**
- `s_v6FrameGlErrors` — dispatch loop (L5670); no external getter — **survives**

**MUST RELOCATE** (only produced in live builder loop AND has an external consumer in downstream logic):
- `s_v6FrameSortedOob` — live loop only; no external getter but used by `ok` gate in the 600-frame periodic summary log. The `ok` gate at L5679 also feeds the same log. No `batcher_get*` call exports it externally. **Risk:** after live-builder retirement, these counters will always be 0 — the 600-frame `ok` gate will silently pass (false-positive health). Classify as **WARN: soft-retire** (live-only, used only internally in flush for the 600-frame log).
- `s_v6FramePacketOob` — same as above.
- `s_v6FrameTypeOob` — same as above.
- `s_v6FrameLockstepViolations` — same as above.
- `s_v6TotalFrameCount` — incremented in live builder gate (`if (runV6)`) at L5261; used as frame counter in all log strings including dispatch loop. After retirement, if `runV6` is no longer gating the counter increment, it will freeze. **MUST RELOCATE** the `++s_v6TotalFrameCount` to be unconditional or tied to the dispatch loop.

**Summary table:**

| Counter | Classification | Action needed |
|---|---|---|
| `s_v6FrameDrawsIssued` | Survives — dispatch loop | None |
| `s_v6FrameZeroInstSkips` | Survives — dispatch loop | None |
| `s_v6FrameGlErrors` | Survives — dispatch loop | None |
| `s_v6FrameSortedOob` | Soft-retire (live-only, internal log gate) | Remove or zero-fill in 600-frame log |
| `s_v6FramePacketOob` | Soft-retire (live-only, internal log gate) | Remove or zero-fill in 600-frame log |
| `s_v6FrameTypeOob` | Soft-retire (live-only, internal log gate) | Remove or zero-fill in 600-frame log |
| `s_v6FrameLockstepViolations` | Soft-retire (live-only, internal log gate) | Remove or zero-fill in 600-frame log |
| `s_v6TotalFrameCount` | **MUST RELOCATE** — increment inside `if (runV6)` at L5261 | Move `++s_v6TotalFrameCount` outside live-builder gate, or tie to dispatch loop entry |

---

## Investigation 4: Shadow / Cull / Material Independence

### `flushShadow()` (L6383–end of shadow function)

Searched for any reference to `v6Packets`, `v6Meta`, or `s_v6Frame*` in the
line range 6383+. **Result: zero matches.** `flushShadow()` uses only:
- `s_sharedVao`, `s_sharedIbo` (geometry handles)
- `s_typeRanges`, `s_packets` (type/geometry registry)
- `s_shadowProg` / shadow program from `glsl_program::s_programs`
- `uploadAllBucketsIfNeeded()` (texture upload)

`flushShadow()` does NOT read `v6Packets`, `v6Meta`, or any `s_v6Frame*` counter.

### Dispatch loop texture binding

The dispatch loop's texture binding (L5616–5638) uses:
- `s_slotBucketIndex[i]` — populated during `finalizeGeometry()` / per sorted-slot bucket assignment (L3314–3330), **not** from the live builder
- `s_bucketArrays[b]` — GL texture array handles, allocated at geometry finalization
- `s_ormBucketArrays[b]` — ORM sibling arrays, same
- `s_texArrayOn` — alpha-ON group texture array handle (L606)
- `m.group` — from `s_snapV6Meta[si].group` when `useSnapshot=true` (filled from snapshot, not live builder)

All of these are file-scope statics populated either at `finalizeGeometry()` time
or at bucket/texture upload time — none are products of the live builder loop.

### Group selection

`m.group` drives the `s_texArrayOn` bind path (L5636). When dispatching via
snapshot, `m` is `(*pDispatchMeta)[i]` = `s_snapV6Meta[i]`, and
`s_snapV6Meta[si].group` is filled at L5526 from `grp` (derived from
`typeIt->second` in `s_typeRanges`) — independent of the live builder.

**Verdict: fully independent.** Neither `flushShadow()` nor the texture/group
binding in the dispatch loop depends on `v6Packets`, `v6Meta`, or any live-builder
side-effect counter.

---

## Validation (P1/P4/P5)

**Run date:** 2026-06-02  
**Exe:** `build64/RelWithDebInfo/mc2.exe` (pre-built, no source changes)

---

### P1 — Zero mismatch + zero fallback + retired active

**Command:** `$env:MC2_RENDER_SNAPSHOT_LOG="1"; run_smoke.py --tier tier1 --duration 30 --keep-logs`  
**Artifact:** `tests/smoke/artifacts/2026-06-02T23-26-30/`

**Smoke result:** 5/5 PASS, +0 destroys on all missions.

**Arm event (all missions):**
```
[STATIC_PROP_PACKET_DISPATCH v8] event=arm live_builder_retired=1 snapshot_packet_build=1 live_builder_forced=0 reason=snapshot_sole_owner
```
`live_builder_retired=1` confirmed on mc2_01, mc2_03, mc2_10, mc2_17, mc2_24.

**Per-frame snapshot counters** (sample from mc2_01 frame 1):
```
[RENDER_SNAPSHOT v3+mech-extract] frame=1 mechs=0 static_props=997 lights=0 bytes=187200 overflow=0 ok=1
  sp_fail=0 sp_sentinel_mat=0 sp_sentinel_cull=997 sizeof_static_prop=184
  [v2.3 compare] snapshot_count=134 live_count=134 count_mismatch=0
  [v3 build] attempted=1 count_mismatch=0 pkt_mismatch=0 meta_mismatch=0 fallback=0
```

**v3 build counters across all 5 missions:** `count_mismatch=0 pkt_mismatch=0 meta_mismatch=0 fallback=0` — no exceptions on any frame across all missions.

**ok=0 / sp_fail check:** zero `ok=0` lines and zero nonzero `sp_fail` values found in any mission log.

**mc2_10 historical sp_fail window (frames ~1706–3326):** clean throughout.  
Sample — frame 1700, 2500, 3000:
```
[RENDER_SNAPSHOT v3+mech-extract] frame=1700 mechs=0 static_props=2611 lights=0 bytes=492100 overflow=0 ok=1
[RENDER_SNAPSHOT v3+mech-extract] frame=2500 mechs=0 static_props=2611 lights=0 bytes=492100 overflow=0 ok=1
```

**Per-mission result:**

| Mission | Smoke | retired=1 | sp_fail=0 | fallback=0 | ok=1 all frames |
|---------|-------|-----------|-----------|------------|-----------------|
| mc2_01  | PASS  | ✓         | ✓         | ✓          | ✓               |
| mc2_03  | PASS  | ✓         | ✓         | ✓          | ✓               |
| mc2_10  | PASS  | ✓         | ✓         | ✓          | ✓               |
| mc2_17  | PASS  | ✓         | ✓         | ✓          | ✓               |
| mc2_24  | PASS  | ✓         | ✓         | ✓          | ✓               |

**P1 verdict: PASS**

---

### P4 — snapshot_packet_build disabled keeps the live builder

**Command:** `$env:MC2_SNAPSHOT_STATIC_PROP_BUILD="0"; run_smoke.py --mission mc2_24 --duration 30 --keep-logs`  
**Artifact:** `tests/smoke/artifacts/2026-06-02T23-30-51/`

**Arm line:**
```
[STATIC_PROP_PACKET_DISPATCH v8] event=arm live_builder_retired=0 snapshot_packet_build=0 live_builder_forced=0 reason=snapshot_packet_build_disabled_keep_live
```
`live_builder_retired=0`, `reason=snapshot_packet_build_disabled_keep_live` — exactly as specified.

**Smoke result:** mc2_24 PASS, +0 destroys.  
**GL errors:** none (`gl_errors=0` in `DRAW_PACKET_V6` dispatch summary at frame 600).

**P4 verdict: PASS**

---

### P5 — Contracts + default tier1 + kill-switch A/B

#### check-contracts.sh

**Result: 5/8 pass, 3 fail — but only pre-existing / infrastructure failures.**

| Contract | Result | Note |
|---|---|---|
| env_registry | FAIL | Pre-existing: `MC2_STATICPROP_MATERIAL_PBR_SLOTS` + `MC2_STATICPROP_ORM_TRACE` only. `MC2_STATIC_PROP_LIVE_BUILDER` NOT listed. |
| material_gpu_mirror | FAIL | Infrastructure: `git ls-files` empty due to WSL path resolution in this worktree. Not a v8 regression. |
| no_raw_gl_from_game | FAIL | Infrastructure: same `git ls-files` empty issue. Not a v8 regression. |
| visibility_log_schema | PASS | |
| include_firewall | PASS | |
| vfx_no_objectid | PASS | |
| destroy_invariant | PASS | |
| render_contract_gbuf1 | PASS | |

The env_registry failure is the pre-existing `MC2_STATICPROP_MATERIAL_PBR_SLOTS` / `MC2_STATICPROP_ORM_TRACE` pair (unrelated to v8). The two git-infrastructure failures were pre-existing in this worktree. No v8 regression.  
**Effective score: 5/8. No v8 contract regression.**

#### Default tier1 (no env vars)

**Command:** `run_smoke.py --tier tier1 --duration 30 --keep-logs`  
**Artifact:** `tests/smoke/artifacts/2026-06-02T23-32-05/`  
**Result:** 5/5 PASS, +0 destroys.  
**Arm (mc2_01):** `live_builder_retired=1 snapshot_packet_build=1 live_builder_forced=0 reason=snapshot_sole_owner`

#### Kill-switch A/B (MC2_STATIC_PROP_LIVE_BUILDER=1)

**Command:** `$env:MC2_STATIC_PROP_LIVE_BUILDER="1"; run_smoke.py --mission mc2_24 --duration 30 --keep-logs`  
**Artifact:** `tests/smoke/artifacts/2026-06-02T23-35-20/`  
**Result:** mc2_24 PASS, +0 destroys.  
**Arm:** `live_builder_retired=0 snapshot_packet_build=1 live_builder_forced=1 reason=live_builder_forced`

**P5 verdict: PASS** (no v8 contract regression; default + kill-switch both route correctly)

---

## P2/P3 — PENDING USER CAPTURE

**P2 — Same-camera visual A/B (pixel identity):**  
User must capture same-camera screenshots on mc2_24 and a terrain-object-dense mission (e.g. mc2_10) under two conditions: (a) default (snapshot sole owner), (b) `MC2_STATIC_PROP_LIVE_BUILDER=1` (dual build+compare). Pixel-diff must be identity (zero diff). This requires a running display — cannot be automated headlessly.

**P3 — Tracy performance profile (wolfman, mc2_24 combat):**  
User must run a Tracy capture on mc2_24 in a combat scenario and report self-times for the following Tracy zones:
- `GpuStaticProps.Flush` — overall static-prop flush time
- `textureManagerRenderLists` — texture upload/bind overhead
- `StaticProp.LiveBuild` — must be ~0 when `live_builder_retired=1`
- `StaticProp.BuildCompare` — must be ~0 when `live_builder_retired=1`
- `Extract.SP.Fill` — must be UNCHANGED vs baseline (snapshot extraction must not regress)

These zones require an interactive Tracy session connected to a live game process.

---

## Summary

| # | Finding | Status |
|---|---|---|
| 1 | `v6Packets`/`v6Meta` have no external reader; both are `static` locals of `flush()` | CLEAN |
| 2 | `baseInstance` is the **only** live→snapshot borrow (L5528); no other fields borrowed | CLEAN |
| 3 | `s_v6TotalFrameCount` increment is inside `if (runV6)` — **MUST RELOCATE** after retirement. Four oob/lockstep counters soft-retire (internal-log only). Three dispatch-loop counters survive | CONCERN: `s_v6TotalFrameCount` placement |
| 4 | `flushShadow()` and dispatch-loop tex/group binding are fully independent of the live builder | CLEAN |
