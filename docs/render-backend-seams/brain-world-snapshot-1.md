# BRAIN-WORLD-SNAPSHOT-1 — design as built

**Status:** built on branch `claude/brain-world-snapshot-1`.  
**Gate:** `MC2_BRAIN_SNAPSHOT` (default OFF).  
**Ladder rung:** 6 (of Brain decoupling ladder).

---

## What this rung does

Introduces a per-warrior POD snapshot of world state read by the emit phase.  
The snapshot is captured on the main thread right before the once-guard check block in `warrior.cpp`.

**Gate OFF (default):** once-guards read live `runtime->*EffectApplied` directly — byte-identical to pre-snapshot behavior.  
**Gate ON:** once-guards read `snapshot.effectApplied[N]`; all live runtime _writes_ (`*EffectApplied = 1`) are unchanged.

---

## BrainWorldSnapshot struct

File: `code/brain_world_snapshot.h`

```cpp
struct BrainWorldSnapshot {
    int32_t  warriorId;              // vehicleWID — WatchID of this warrior's vehicle
    int32_t  teamId;                 // warrior->getTeam() (0 = deferred rung 7/8)
    uint8_t  isDisabled;             // warrior->isDisabled()
    uint8_t  commanderIsHome;        // 0 = deferred rung 7/8
    int32_t  mainGoalObjectWID;      // mainGoal object WatchID (0 if none)
    float    mainGoalLocation[3];    // mainGoal location XYZ
    float    mainGoalControlRadius;  // mainGoal controlRadius
    int32_t  attackOrderTargetWID;   // attackOrders.targetWID (0 if none)
    uint8_t  effectApplied[6];       // per-effect once-guard flags
                                     //   [0]=POWERDOWN [1]=EJECT [2]=GUARD
                                     //   [3]=MOVETO    [4]=ATTACK [5]=RETREAT
    uint32_t brainTick;              // debug tick stamp (getBrainTickIndex())
};
```

Index constants: `BSNAPFX_POWERDOWN`…`BSNAPFX_RETREAT` (`BSNAPFX_COUNT = 6`).

---

## Populate seam

The populate lambda `buildBrainWorldSnapshotLocal` is defined inline at `code/warrior.cpp` in `MechWarrior::requestOrders` (the frame-tick dispatch entry), approximately line 2360.

It is a lambda (not a free function) because it needs access to `MechWarrior`'s protected members (`mainGoalObjectWID`, `mainGoalLocation`, `mainGoalControlRadius`, `attackOrders.targetWID`), which are inaccessible from `brain_special_dispatch.cpp`.

**Populate call site** (warrior.cpp, inside the `s_dispatchApply` guard block):
```cpp
BrainWorldSnapshot snap = {};
const bool snapshotGate = s_brainSnapshotEnabled();
if (snapshotGate)
    buildBrainWorldSnapshotLocal(snap);
```

Called on the main thread, right before the once-guard check block.

---

## What reads snapshot vs what stays live

| Read | Snapshot (gate ON) | Live (gate ON) | Notes |
|---|---|---|---|
| `dispatchEffectApplied` (POWERDOWN once-guard) | `snap.effectApplied[0]` | — | Routed this rung |
| `ejectEffectApplied` (EJECT once-guard) | `snap.effectApplied[1]` | — | Routed this rung |
| `guardEffectApplied` (GUARD once-guard) | `snap.effectApplied[2]` | — | Routed this rung |
| `moveToEffectApplied` (MOVETO once-guard) | `snap.effectApplied[3]` | — | Routed this rung |
| `attackEffectApplied` (ATTACK once-guard) | `snap.effectApplied[4]` | — | Routed this rung |
| `retreatEffectApplied` (RETREAT once-guard) | `snap.effectApplied[5]` | — | Routed this rung |
| ATTACK target existence + team | — | live via `ObjectManager->getByWatchID(parsedWID)` | Deferred to rung 7/8 |
| ATTACK self-check (attacker WID) | — | `wid` param already the warrior's WID | Already an ID, no live deref |
| `*EffectApplied = 1` writes | — | live (`runtime->*EffectApplied`) | Writes not decoupled this rung |

---

## IDs-not-pointers discipline

All references to game objects in the snapshot are WatchIDs (`int32_t`), not pointers. Pointers are unsafe to hold across ticks; WIDs are stable references resolved at use time via `ObjectManager::getByWatchID`. The snapshot does NOT capture any pointer; team identity is deferred.

---

## Gate implementation

`s_brainSnapshotEnabled()` — defined in `code/brain_special_dispatch.cpp`, declared in `code/brain_special_dispatch.h`.

Pattern matches existing `MC2_BRAIN_*` gates (static-local lambda `getenv` + `atoi`). Default OFF.

---

## Deferred to rung 7/8

- Target/contact table snapshot: ATTACK's `target->getTeam()` + `target->getWatchID()` still read live via `ObjectManager::getByWatchID(targetWID)`. A rung-7/8 target table snapshot would capture team + existence for all known contacts.
- Commander identity (`commanderIsHome`): reads live commander pointer; not captured this rung.
- Team table: `teamId = 0` this rung; full team identity deferred.
- PLAYER/ALARM slot reads: not in scope for any snapshot rung yet.

---

## Gate OFF byte-identity proof

Gate OFF → `s_brainSnapshotEnabled()` returns false → `buildBrainWorldSnapshotLocal` is not called → `snap` is zero-initialised but unused → all once-guard reads fall to the `else` branch which reads `brainRuntime->*EffectApplied` directly — identical to pre-snapshot code.

---

## File locations

| File | Purpose |
|---|---|
| `code/brain_world_snapshot.h` | POD struct + index enum (no engine headers; stub-linkable) |
| `code/brain_special_dispatch.h` | `s_brainSnapshotEnabled()` declaration + `#include "brain_world_snapshot.h"` |
| `code/brain_special_dispatch.cpp` | `s_brainSnapshotEnabled()` definition |
| `code/warrior.cpp` | `buildBrainWorldSnapshotLocal` lambda + call site + snapshot routing |
