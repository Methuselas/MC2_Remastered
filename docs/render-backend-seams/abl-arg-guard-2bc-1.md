# ABL-ARG-GUARD-2BC Coverage Report

**Branch:** `claude/abl-arg-guard-2bc`  
**Worktree:** `A:/Games/mc2-abl-arg-guard-2bc`  
**Base commit:** `90bea9eb` (nifty HEAD)  
**Date:** 2026-06-23

---

## Guarded Sites

All guards inserted in `code/ablmc2.cpp` immediately after the `ABLi_pop*Ptr()` call, before first dereference.

| # | Function | Param | Pop call type | Guard line (approx) | First deref protected |
|---|----------|-------|---------------|---------------------|-----------------------|
| 1 | `execGetFireRanges` | `ranges` | `ABLi_popRealPtr()` | ~1129 | `ranges[0] = WeaponRange[...]` |
| 2 | `execGetSalvage` | `items` | `ABLi_popIntegerPtr()` | ~3776 | `items[i] = -1` in loop |
| 3 | `execGetSalvage` | `quantities` | `ABLi_popIntegerPtr()` | ~3777 | `quantities[i] = -1` in loop |
| 4 | `execConvertCoords` | `worldPos` | `ABLi_popRealPtr()` | ~4943 | `worldPos[0]` in worldToCell branch |
| 5 | `execConvertCoords` | `cellPos` | `ABLi_popIntegerPtr()` | ~4944 | `cellPos[0]` in worldToCell branch |
| 6 | `execCoreMoveTo` | `location` | `ABLi_popRealPtr()` | ~4968 | `isnan(location[0])` |
| 7 | `execGetMapInfo` | `mapInfo` | `ABLi_popIntegerPtr()` | ~7438 | `mapInfo[0] = GameMap->getHeight()` |

Note: `execCoreMoveTo` guard pushes `ABLi_pushInteger(0)` before returning (function has return value "i"; matches the `ABLi_getSkipOrder()` path which also pushes 1 before returning). `execConvertCoords` guard fires after `ABLi_pushInteger(0)` (return value already pushed).

---

## execGetObjectFireRanges Resolution

**NOT FOUND.** No function named `execGetObjectFireRanges` exists in `code/ablmc2.cpp`. The recon doc's `~L938` reference was pointing to the `calcFireRanges()` call sites at lines 832/842 (pilot->getVehicle()->calcFireRanges()). The correct ABL-exposed function is `execGetFireRanges` (registered as `"getfireranges"`, mask `"R"`, line 7906) — guarded above as site #1.

---

## Build Result

**GREEN** — full link, no errors, 5 benign linker warnings (DELAYLOAD avcodec/avformat/avutil/swscale/swresample).

---

## Smoke Results

Deploy target: `A:/Games/mc2-opengl/mc2-win64-v0.4c`  
Note: v0.4d-rc1 deploy target does not exist (skipped per deploy_payload.py guard).

### Stock gate OFF (MC2_ABL_ARG_GUARD unset)
| Mission | Result |
|---------|--------|
| mc2_01  | PASS   |
| mc2_10  | PASS   |

### Stock gate ON (MC2_ABL_ARG_GUARD=1, MC2_ABL_RUNTIME_SOFTFAIL=1)
| Mission | Result |
|---------|--------|
| mc2_01  | PASS   |
| mc2_10  | PASS   |

### MCO cfv2_mission1_escort gate ON
| Mission | Result | Bucket |
|---------|--------|--------|
| cfv2_mission1_escort | FAIL | env_mission_not_found |

MCO FAIL is an environment issue: `A:\Games\mc2-opengl\mc2-win64-v0.4\mods\MCO-ClanEagle` not installed at smoke runner's expected path. Not a code regression.

---

## ABL_ARG_GUARD Log Hits

**None triggered.** Grepped all `.log` files under `mc2-win64-v0.4c/` and `mc2-win64-v0.4/` for `[ABL_ARG_GUARD]` — zero matches across all smoke runs.

---

## Acceptance Verdict

**ACCEPTED.** 7 guard sites inserted across 5 functions. Gate OFF = byte-identical behavior (PASS). Gate ON = no guard fires on stock mc2_01/mc2_10 missions. MCO FAIL is pre-existing env gap, not introduced by this change. Build GREEN.
