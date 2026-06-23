# ABL-ARG-GUARD-3-1: commander null-guard in execRequestHelp / execRequestTarget

## Guarded sites

| Function | File | Line | Expression guarded |
|---|---|---|---|
| `execRequestHelp` | `code/ablmc2.cpp` | 6081 | `CurWarrior->getCommander()->getId()` |
| `execRequestTarget` | `code/ablmc2.cpp` | 6122 | `CurWarrior->getCommander()->getId()` |

Both sites were inside `if (MPlayer || (CurWarrior->getTeam() == Team::home))`.  
`getCommander()` returns `CommanderPtr` (declared `warrior.h:1362`, `mover.h:1249`).  
A warrior with no commander returns NULL → unguarded `->getId()` is a NULL deref crash.

## Fix structure

```cpp
CommanderPtr _cmdr = CurWarrior->getCommander();
if (s_ablArgGuard && !_cmdr)
    abl_arg_guard_log("execRequestHelp"/"execRequestTarget", "commander");
else if (_cmdr)
    commanderID = _cmdr->getId();
else
    commanderID = CurWarrior->getCommander()->getId();  // gate OFF: original unguarded deref
```

Gate OFF (`s_ablArgGuard == false`): `_cmdr` is non-null → `else if` branch sets `commanderID` normally. If `_cmdr` IS null and gate is OFF, falls through to original unguarded deref — byte-identical behavior to pre-patch.  
Gate ON + null: logs `[ABL_ARG_GUARD]` and `commanderID` stays -1 (graceful default, warrior searches all commanders).

## Graceful-default rationale

`commanderID = -1` is the already-initialized default immediately above the guard block. Passing -1 to `getMoversWithinRadius` causes it to return all friendly movers regardless of commander assignment — a safe, conservative fallback that avoids hard-mission-fail or crash while still allowing the help/targeting request to proceed with a wider unit pool.

## Build result

GREEN — full link, zero new errors. Warnings: pre-existing C4267 in mainmenu.cpp + DELAYLOAD LNK4199 (avcodec/avformat/avutil/swscale/swresample, pre-existing).  
Worktree: `A:/Games/mc2-abl-arg-guard-3` branch `claude/abl-arg-guard-3` off `b8250999`.

## Smoke results

### Stock (v0.4c) — mc2_01 + mc2_10

| Run | mc2_01 | mc2_10 |
|---|---|---|
| Gates OFF | PASS | PASS |
| Gates ON (ARG_GUARD + SOFTFAIL) | PASS | PASS |

### MCO ClanEagle (v0.4d-rc1) — cfv2_mission1_escort

| Run | cfv2_mission1_escort |
|---|---|
| Gates ON (ARG_GUARD + SOFTFAIL) | PASS |
| Gates OFF | PASS |

## Production guard fires

No `[ABL_ARG_GUARD] func=execRequestHelp` or `func=execRequestTarget` lines found in any smoke log. Commander was non-null on all tested missions during the 30-second windows.
