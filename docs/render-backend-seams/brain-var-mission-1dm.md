# TECHSCRIPT-DISPATCH-1D-M: Mission-scope Var store

## Summary

Completes the Var system. 1D shipped per-unit Var.Set/Get. 1D-M adds real mission-scope read+write behind gate `MC2_BRAIN_VAR_MISSION` (default OFF).

## MissionVarStore

- **Location:** `code/brain_mission_var_store.h` (struct + inline methods) + `code/brain_special_dispatch.cpp` (global definition + resetMissionVarStore()).
- **Global:** `MissionVarStore g_missionVarStore` (defined in brain_special_dispatch.cpp).
- **Cap:** 64 entries × {key[32], value[32], lastWriterWID int, lastWriteTick uint32_t}. Linear scan.
- **Not serialized.** Mission-ephemeral only.

## Gate semantics

| Gate | Behavior |
|---|---|
| `MC2_BRAIN_VAR_MISSION=0` (default) | Var.Set/Get scope=Mission → trace-only (1D behavior unchanged). |
| `MC2_BRAIN_VAR_MISSION=1` (requires `MC2_BRAIN_DISPATCH_VAR=1`) | Var.Set scope=Mission → writes to `g_missionVarStore`. Var.Get scope=Mission → reads from `g_missionVarStore`. |

If `MC2_BRAIN_VAR_MISSION=1` is set without `MC2_BRAIN_DISPATCH_VAR=1`, a warning is emitted and the gate is inert.

## Reset at mission load

`resetMissionVarStore()` is called from `code/mission.cpp` at mission load when `MC2_BRAIN_VAR_MISSION=1`, inside the `TECHSCRIPT-SPECIAL-DISPATCH-1A` block (approximately line 3112+). This clears the store before any warrior executes its first specials tick. It is always safe to call (no-op if store is empty).

## Guarded-single-writer policy (WID-order-wins tiebreak)

**Why this was deferred from 1D:** allowing multiple warriors to write the same mission-scope key in one tick introduces cross-warrior nondeterminism. The policy chosen makes the result deterministic whether warriors execute sequentially OR in parallel.

**Policy:** within the same `brainTick`, the HIGHER WID wins. If a write arrives for a key that was already written in the same tick by a higher-WID warrior, the new write is suppressed and a conflict trace is emitted:

```
[BRAIN_VAR_MISSION_CONFLICT] key=<k> wid=<W> prevWid=<P> tick=<t> (higher-WID wins; this write suppressed)
```

Across different ticks, last-write-wins (normal overwrite).

**Threading-ready:** even if warriors run in parallel, the deterministic outcome is: highest-WID writer wins within a tick. This is auditable via the CONFLICT trace.

**Implementation:** `MissionVarStore::trySet()` in `brain_mission_var_store.h`. Compares `writerWID < entry.lastWriterWID` when `tick == entry.lastWriteTick`.

## Trace lines

- `[BRAIN_VAR_MISSION_RESET]` — store cleared at mission load.
- `[BRAIN_VAR_MISSION_SET] key=<k> val=<v> wid=<W> tick=<T>` — write committed.
- `[BRAIN_VAR_MISSION_GET] key=<k> val=<v> wid=<W> (hit|default)` — read result.
- `[BRAIN_VAR_MISSION_CONFLICT]` — write suppressed by WID-order rule (audit only, not fatal).
- `[BRAIN_VAR_MISSION_SET_FULL]` — cap (64) hit, write skipped.

Optional sub-gate: the CONFLICT trace is always on when `MC2_BRAIN_VAR_MISSION=1`. SET/GET traces are always on (they are low-volume per-tick).

## Handler changes

- `handleVarSet()` in `code/brain_special_dispatch.cpp`: scope=Mission + gate ON → calls `g_missionVarStore.trySet()`.
- `handleVarGet()` in `code/brain_special_dispatch.cpp`: scope=Mission + gate ON → calls `g_missionVarStore.get()`.
- Per-unit (Unit scope) behavior: unchanged from 1D.

## What stays deferred

- Serialization (mission-ephemeral by design).
- Cross-mission persistence.
- FSM DSL / conditional branches reading Var values.
- Threading (the policy is threading-ready but no parallel execution added here).
- Per-unit Var behavior (unchanged).
- Gate-OFF behavior (unchanged from 1D).

## Fixture

`tests/fixtures/brain_runtime/mc2_01_varmission_specials.fit` — raw brace-block TechSpecial with `Var.Set "mflag" 7 scope=Mission` then `Var.Get "mflag" scope=Mission`. Manifest entry `varmission-roundtrip` expects `BRAIN_VAR_MISSION_SET key=mflag val=7` + `BRAIN_VAR_MISSION_GET` in trace output. Harness sets `MC2_BRAIN_VAR_MISSION=1` unconditionally.
