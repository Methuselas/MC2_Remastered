# BRAIN-OPORD-COREPATROL-1 — Implementation Doc

Status: SHIPPED (claude/brain-corepatrol-1)
Gate: MC2_BRAIN_PATROL (default OFF)

## State Fields (MechBrainRuntime)

Added at `code/mech_brain_runtime.h` after the FSM fields:

| Field | Type | Default | Purpose |
|---|---|---|---|
| `patrolWaypointCount` | `uint8_t` | 0 | Number of parsed waypoints (0..8) |
| `patrolWaypointIndex` | `uint8_t` | 0 | Current cursor position |
| `patrolWaypoints[8][3]` | `float` | {} | x/y/z per waypoint, inline coords |
| `patrolLoop` | `bool` | true | true=cycle to first; false=stop at last |
| `patrolActive` | `bool` | false | true=patrol is running |

Memory cost: 8*3*4 + 4 = 100 bytes per warrior runtime. Mission-ephemeral (zeroed by default ctor).

## Arg Shape

```
OPORD.CorePatrol x1 y1 z1 x2 y2 z2 ... [loop|once]
```

- Up to 8 waypoint triples (strtof triplets), parsed until non-numeric token or end of arg.
- Trailing `once` keyword → `patrolLoop=false`; default (or `loop`) → `patrolLoop=true`.
- NaN coords: soft-fail trace `[BRAIN_PATROL_NAN]`, waypoint skipped.
- If <2 valid waypoints after parse: `[BRAIN_PATROL_PARSE_FAIL]`, no patrol started.

## Arg Parse Location

`code/brain_special_dispatch.cpp` — inside `executeSpecialBody_Apply`, in the `OPORD.CorePatrol` handler branch. Gate: `s_brainPatrolGate()`.

## First-Dispatch (patrolActive==false)

1. Parse waypoints into `prt->patrolWaypoints[]`, set `patrolWaypointCount`, `patrolWaypointIndex=0`, `patrolLoop`, `patrolActive=true`.
2. Emit `[BRAIN_PATROL_START] count=<n> loop=<0|1> wid=<W>`.
3. Emit MOVETO_POINT to `waypoints[0]` via `emitBrainIntent` (gate ON) or direct `setGeneralTacOrder` (gate OFF).
4. Emit `[BRAIN_PATROL_EMIT] index=0 pos=(...) wid=<W>`.

Re-ticks with `patrolActive==true`: trace `[BRAIN_PATROL_ACTIVE]`, skip re-parse. Per-tick advance is handled by `tickPatrolAdvance()`.

## Per-Tick Advance

`tickPatrolAdvance(warrior, runtime, wid)` in `code/brain_special_dispatch.cpp`, called from `warrior.cpp` every brain tick unconditionally (outside `alreadyDone` guard).

- Gate OFF or `patrolActive==false`: no-op.
- Polls `warrior->curTacOrder.status(warrior) == TACORDER_SUCCESS` (tacordr.cpp:1271, `cellDistanceToGoal<1`).
- On arrival:
  - loop: `newIndex = (oldIndex+1) % count`
  - once: `newIndex = oldIndex+1`; if clamped to `count-1`, set `patrolActive=false` + `[BRAIN_PATROL_DONE]`.
- Emits `[BRAIN_PATROL_ADVANCE] from=<i> to=<j> wid=<W>`.
- Emits MOVETO_POINT to `waypoints[newIndex]`.
- Returns true (caller sets `dispatcherAppliedEffect=true` to suppress HOLD).

## moveToEffectApplied Bypass

`moveToEffectApplied` is intentionally NOT set or checked by the patrol path. Rationale: patrol needs to re-emit MOVETO_POINT repeatedly (once per cursor advance). `patrolActive` + cursor index are patrol's own guard. The once-guard at warrior.cpp:2469–2470 only fires for `bodyHasCoreMoveTo` (prefix `OPORD.CoreMoveTo`); patrol uses prefix `OPORD.CorePatrol` and is a separate code path. No interference.

## Gate Semantics

- Gate OFF: `OPORD.CorePatrol` falls through as `[BRAIN_DISPATCH_UNKNOWN]` (existing recognized-verb table entry at brain_special_dispatch.cpp:164 covers it for trace; apply handler is guarded by `s_brainPatrolGate()`). Byte-identical behavior when gate OFF.
- Gate ON: first dispatch starts patrol, per-tick tickPatrolAdvance polls arrival.
- `MC2_BRAIN_PATROL=1` requires `MC2_BRAIN_DISPATCH=1` and `MC2_BRAIN_DISPATCH_APPLY=1` to produce any order.

## Harness-Proven vs Smoke-Only

**Harness-proven** (offline, no live mover):
- `bodyHasCorePatrol()` returns true for fixture body.
- `PATROL_START` fires on first dispatch with count=2, loop=1.
- `PATROL_EMIT index=0 pos=(100 100 0)` fires — MOVETO_POINT emitted to wp0.
- Waypoints parsed correctly (100,100,0 and 200,200,0).
- Existing fixtures unaffected (relaxed_guard_doc=6 unchanged).

**Smoke-only** (requires live pathfinder / mover):
- `PATROL_ADVANCE` cursor cycling (arrival detection via curTacOrder.status).
- Loop wrap-around (0→1→0→...).
- `PATROL_DONE` for once-mode completion.
- Warrior visually navigating between waypoints.

## Deferred

- Named waypoint tables from fit files (future slice, no format extension designed).
- FSM compose (attack break-out) — requires `MC2_BRAIN_FSM` ON, separate fixture.
- Multi-warrior commit proof on torrin — mod-content change (DarkRain Enhanced-brain bodies).
- Ping-pong semantics — loop/once only this slice.
