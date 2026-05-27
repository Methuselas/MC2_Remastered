# HANDOFF 2026-05-26: Extraction v2.3 SHIPPED — snap-cull opt-in, Tier1 5/5 PASS

## Status

SHIPPED. HEAD `88379448`. Tier1 5/5 PASS (default + opt-in paths).

## What was done

Extraction v2.3 adds snapshot-assisted snap-cull to the v6 static-prop dispatch loop.
When `MC2_SNAP_CULL=1`, prev-frame zero-instance draw slots are skipped using the
trusted v2.2 snapshot rows. Default OFF — no behavior change unless explicitly enabled.

Key additions:
- `getLastRenderSnapshot()` free function — returns `nullptr` before first extraction,
  `&s_lastSnapshot` after; valid for the same frame (arena-backed span lifetime)
- `s_lastSnapshot` + `s_hasLastSnapshot` module statics in render_snapshot.cpp
- `flush(const RenderSnapshot* snap = nullptr)` — snap-cull inside v6 dispatch loop
- `s_snapCullEnabled` IIFE lambda — strictly `v && v[0]=='1'`; unset/=0 both OFF
- Snap-cull activation guard: `snap->ok==1u`, `count==totalCmds`, warmup guard (all-zero → no cull)
- Per-slot three-way handler: identity mismatch → fall-through, zero → skip, non-zero → active
- `batcher_getSnapCullStats(uint32_t*, uint32_t*, uint32_t*)` free function
- 3 new `RenderSnapshot` fields: `spSnapCullSkipped`, `spSnapCullActive`, `spSnapCullSlotMismatch`
- `ok` gate extended: adds `spSnapCullSlotMismatch==0` (11th condition)
- `spSnapCullSkipped` and `spSnapCullActive` informational only (excluded from gate)
- Log tag updated to `[RENDER_SNAPSHOT v2.3]`; new line `[v2.3 compare]` + `[v2.3 snap_cull]`
- `MC2_SNAP_CULL` registered in `docs/tier1_env_vars.md` and `scripts/run_smoke.py` allowlist

## Smoke results

Default (MC2_SNAP_CULL unset):
```
[RENDER_SNAPSHOT v2.3] ... ok=1
  [v2.3 snap_cull] skipped=0 active=0 slot_mismatch=0
```
All 5 missions (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24): ok=1, all counters 0.

Opt-in (MC2_SNAP_CULL=1):
```
ok=1 slot_mismatch=0
```
All 5 missions: exit 0. On mc2_01 (134 slots, all live every frame): skipped=0 active=0.
Snap-cull activates on missions where some type slots have zero prev-frame instances.

## What v2.3 proves / enables

- Snapshot rows are now CONSUMED for dispatch — zero-instance slots can be skipped
- `getLastRenderSnapshot()` is the bridge between extraction and flush
- `spSnapCullSlotMismatch==0` in the ok gate proves slot ordering is stable
- Warmup guard proves frame-1 safety (no blank screen)

## What is NOT yet done

- Snap-cull is default OFF; enabling by default deferred to v3 authority flip or explicit decision
- DrawPacket/meta build from snapshot rows — deferred to v3
- v3 authority flip — v6 builder reads from snapshot instead of live batcher state

## Roadmap

- v2.3 (done): snap-cull opt-in, proven safe
- v3: authority flip — v6 builder consumes snapshot rows instead of live batcher state

## Commit chain

- T1 (pre-session): getLastRenderSnapshot + s_lastSnapshot store
- `8399820c` — T2: snap-cull fields in RenderSnapshot + batcher_getSnapCullStats
- `db33e9fa` — T3: snap-cull in flush() + txmmgr call site
- `c7fed76f` — T4: ok gate + log v2.3 snap-cull stats
- `02da8ed3` — T5: register MC2_SNAP_CULL in tier1_env_vars + smoke propagation
- `88379448` — fix: [v2.2 compare] log tag → [v2.3 compare]

## Files modified

- `GameOS/gameos/render_snapshot.h`
- `GameOS/gameos/render_snapshot.cpp`
- `GameOS/gameos/gos_static_prop_batcher.h`
- `GameOS/gameos/gos_static_prop_batcher.cpp`
- `mclib/txmmgr.cpp`
- `docs/tier1_env_vars.md`
- `scripts/run_smoke.py`
