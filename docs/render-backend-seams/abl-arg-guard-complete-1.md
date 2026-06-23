# ABL-ARG-GUARD-COMPLETE-1: Final ABL Null Out-Pointer Guard Pass

**Date:** 2026-06-23
**Branch:** `claude/abl-arg-guard-complete`
**Worktree:** `A:/Games/mc2-abl-arg-guard-complete`
**Base commit:** `a0dadfd8`
**Commit:** `d1202cfb`
**Gate:** `MC2_ABL_ARG_GUARD=1`

---

## Summary

Guards applied to all remaining HIGH and MEDIUM risk ABL native function out-pointer sites identified in `abl-native-arg-guard-coverage-recon-1.md`. Previous slices (2BC, 3) guarded the HIGH-1 through HIGH-10 set and commander null checks. This slice completes the worklist.

**Total: 20 guard sites across 20 functions** (camera velocity group = 4 functions, bringing the count above the 15-function estimate).

---

## Guarded Sites

| # | Function | File:Line (approx) | Pointer guarded | Return value handling |
|---|---|---|---|---|
| 1 | `execGetContacts` | `code/ablmc2.cpp` ~L598 | `contactList` | `ABLi_pushInteger(0)` |
| 2 | `execGetWeaponsReady` | `code/ablmc2.cpp` ~L875 | `weaponList` | `ABLi_pushInteger(0)` |
| 3 | `execGetWeaponsLocked` | `code/ablmc2.cpp` ~L889 | `weaponList` | `ABLi_pushInteger(0)` |
| 4 | `execGetWeaponsInRange` | `code/ablmc2.cpp` ~L903 | `weaponList` | `ABLi_pushInteger(0)` |
| 5 | `execGetAttackers` | `code/ablmc2.cpp` ~L1157 | `attackers` | `ABLi_pushInteger(0)` |
| 6 | `execGetUnitMates` | `code/ablmc2.cpp` ~L1350 | `mateList` | `ABLi_pushInteger(0)` |
| 7 | `execGetObjects` | `code/ablmc2.cpp` ~L1489 | `objList` | `ABLi_pushInteger(0)` |
| 8 | `execOrderMoveTo` | `code/ablmc2.cpp` ~L1656 | `coordList` | `ABLi_pushInteger(TACORDER_FAILURE)` |
| 9 | `execGetWeapons` | `code/ablmc2.cpp` ~L7334 | `weaponList` | `ABLi_pushInteger(0)` |
| 10 | `execGetWeaponsStatus` | `code/ablmc2.cpp` ~L7370 | `weaponList` | `ABLi_pushInteger(0)` |
| 11 | `execSetMoveArea` | `code/ablmc2.cpp` ~L7384 | `center` | bare return (void) |
| 12 | `execIsOffMap` | `code/ablmc2.cpp` ~L7457 | `worldPos` | `ABLi_pushBoolean(false)` |
| 13 | `execGetCameraPosition` | `code/ablmc2.cpp` ~L5401 | `camPos` | bare return (void) |
| 14 | `execGetCameraGoalPosition` | `code/ablmc2.cpp` ~L5486 | `camPos` | bare return (void) |
| 15 | `execGetCameraRotation` | `code/ablmc2.cpp` ~L5514 | `camRot` | bare return (void) |
| 16 | `execGetCameraGoalRotation` | `code/ablmc2.cpp` ~L5600 | `camRot` | bare return (void) |
| 17 | `execSetCameraVelocity` | `code/ablmc2.cpp` ~L5721 | `camVel` | bare return (void) |
| 18 | `execGetCameraVelocity` | `code/ablmc2.cpp` ~L5748 | `camVel` | bare return (void) |
| 19 | `execSetCameraGoalVelocity` | `code/ablmc2.cpp` ~L5777 | `camVel` | bare return (void) |
| 20 | `execGetCameraGoalVelocity` | `code/ablmc2.cpp` ~L5808 | `camVel` | bare return (void) |

### Functions requiring ABLi_push* before early return

`execGetContacts`, `execGetWeaponsReady`, `execGetWeaponsLocked`, `execGetWeaponsInRange`, `execGetAttackers`, `execGetUnitMates`, `execGetObjects`, `execOrderMoveTo`, `execGetWeapons`, `execGetWeaponsStatus`, `execIsOffMap` (11 functions).

### Void functions (bare return)

`execSetMoveArea`, `execGetCameraPosition`, `execGetCameraGoalPosition`, `execGetCameraRotation`, `execGetCameraGoalRotation`, `execSetCameraVelocity`, `execGetCameraVelocity`, `execSetCameraGoalVelocity`, `execGetCameraGoalVelocity` (9 functions).

---

## Special-Case Resolutions

### execGetObjectFireRanges

**Non-existent.** The recon-table entry #28 referenced this name but the actual function at that line range is `execGetWeaponRanges` (registered as `"getweaponranges"`). `execGetObjectFireRanges` as a distinct symbol does not exist in `code/ablmc2.cpp`. Skipped per task instruction.

### execOrderMoveToPoint

**Not found.** The movement-order function is `execOrderMoveTo` (line ~1637, registered as `"ordermoveto"`). `execOrderMoveToPoint` does not exist as a separate function. Guarded `execOrderMoveTo` instead.

### execGetContactRange

**Same as already-guarded `execGetContactRelativePosition`.** The function at ablmc2.cpp:774 is `execGetContactRelativePosition`. Recon table row #1 listed `execGetContactRange` at line 788 which is the body of `execGetContactRelativePosition`. This was already guarded in the 2A slice (both `range` and `angle` pointers). No action required.

---

## Build Result

**GREEN** — full mc2.exe relink. No new errors. Pre-existing warnings only (C4267 mainmenu.cpp/logisticsdata.cpp, C4838 logisticsdialog.cpp, LNK4199 DELAYLOAD avcodec/avformat/avutil/swscale/swresample).

```
mc2.vcxproj -> A:\Games\mc2-abl-arg-guard-complete\build64\RelWithDebInfo\mc2.exe
```

Commit: `d1202cfb` (1 file changed, 134 insertions, 23 deletions).

---

## Deploy

Deployed to `A:/Games/mc2-opengl/mc2-win64-v0.4` (stock mc2):

```
[deploy_payload] PDB check OK: deployed PDB matches source hash
[deploy_payload] manifest written: ... (157 rows, v1, src_commit d1202cfb4f1d)
[deploy_payload] deploy COMPLETE — payload verified, manifest written
```

Note on v0.4c: also deployed there, but v0.4c showed a pre-existing mc2_01 crash_silent at ~20s that reproduced equally with the nifty HEAD binary (sha=7adbe73b) — confirmed not a regression from this slice. Acceptance smoke ran against v0.4.

---

## Smoke Results

All smoke ran against `A:/Games/mc2-opengl/mc2-win64-v0.4` with our commit `d1202cfb`.

### Stock — Gate OFF (mc2_01 + mc2_10)  run `2026-06-23T16-12-56`

| Mission | Result | Frames | Avg FPS | Load ms |
|---|---|---|---|---|
| mc2_01 | PASS | 4256 | 142 | 6810 |
| mc2_10 | PASS | 4228 | 141 | 8005 |

**result=PASS 2/2**

### Stock — Gate ON (mc2_01 + mc2_10, MC2_ABL_ARG_GUARD=1)  run `2026-06-23T16-14-24`

| Mission | Result | Frames | Avg FPS | Load ms |
|---|---|---|---|---|
| mc2_01 | PASS | 4247 | 142 | 7000 |
| mc2_10 | PASS | 2898 | 97 | 27952 |

**result=PASS 2/2**

### MCO (v0.4d-rc1)

N/A — `A:/Games/mc2-opengl/mc2-win64-v0.4d-rc1` does not exist.

---

## [ABL_ARG_GUARD] Fires

**Zero fires** in both gate-ON runs (mc2_01 + mc2_10, 30s each). Expected: stock MC2 scripts pass well-formed pointers during normal 30s windows. Guards are defensive against malformed/mod scripts and reproduce the exact crash class seen in Exodus `execGetRelativePositionToObject`.

---

## Acceptance

- 20 guard sites, 20 functions guarded
- execGetObjectFireRanges: non-existent (naming artifact), skip confirmed
- execOrderMoveToPoint: maps to execOrderMoveTo, guarded
- execGetContactRange: same as already-guarded execGetContactRelativePosition, skip confirmed
- 11 functions needed ABLi_push* before early return; 9 are void (bare return)
- Build: GREEN
- Deploy fingerprint: confirmed (PDB check OK, manifest src_commit d1202cfb)
- Stock gate OFF: PASS 2/2
- Stock gate ON: PASS 2/2, zero [ABL_ARG_GUARD] fires
- MCO: N/A

**ACCEPTANCE MET: YES**
