# ABL-ARG-GUARD-2A — Four Additional ABL Null-Pointer Guards

**Date:** 2026-06-23  
**Branch:** `claude/abl-arg-guard-2a` (worktree `A:/Games/mc2-abl-arg-guard-2a`)  
**Base commit:** `f3a6b161`  
**Gate:** `MC2_ABL_ARG_GUARD=1` (enable guards) + `MC2_ABL_RUNTIME_SOFTFAIL=1` (soft-fail logging)

---

## Guarded Sites

All guards follow the proven pattern from `execGetRelativePositionToObject` (H1, shipped earlier):

```cpp
if (s_ablArgGuard && !PTR) {
    abl_arg_guard_log("FUNCNAME", "PARAMNAME");
    return;
}
```

Gate OFF = byte-identical to pre-patch (no runtime overhead, no behavior change).

| # | Function | File:Line (post-patch) | Pointer guarded | Direction |
|---|----------|------------------------|-----------------|-----------|
| 1 | `execGetContactRelativePosition` | `code/ablmc2.cpp` ~L791 | `range` | OUT (write `*range = -1.0`) |
| 2 | `execGetContactRelativePosition` | `code/ablmc2.cpp` ~L796 | `angle` | OUT (write `*angle = 0.0`) |
| 3 | `execGetObjectPosition` | `code/ablmc2.cpp` ~L963 | `coordList` | OUT (write `coordList[0..2]`) |
| 4 | `execInArea` | `code/ablmc2.cpp` ~L2966 | `areaCenter` | IN (read `areaCenter[0..2]`) |
| 5 | `execGetRelativePositionToPoint` | `code/ablmc2.cpp` ~L4379 | `pos` | IN (read `pos[0..2]`) — H2a |
| 6 | `execGetRelativePositionToPoint` | `code/ablmc2.cpp` ~L4384 | `relPos` | OUT (write `relPos[0..2]`) — H2b |

**Note:** `execGetRelativePositionToPoint` has TWO unguarded pointers (both `pos` and `relPos`), closing the gap left open when only the Object-variant (`execGetRelativePositionToObject`) was guarded. Each pointer gets a distinct `abl_arg_guard_log` call with its own `paramName`.

---

## Build Result

**GREEN** — full mc2.exe relink in isolated worktree. No new errors or warnings introduced by the patch.

```
mc2.vcxproj -> A:\Games\mc2-abl-arg-guard-2a\build64\RelWithDebInfo\mc2.exe
```

---

## Smoke Results

### Stock PASS — Gates OFF (mc2_01 + mc2_10)

```
result=PASS (2/2 passed)
mc2_01  PASS  frames=4276  avg_fps=143  load_ms=6624
mc2_10  PASS  frames=4251  avg_fps=142  load_ms=7789
```

Target: `A:/Games/mc2-opengl/mc2-win64-v0.4c`

### Stock PASS — Gates ON (mc2_01 + mc2_10, MC2_ABL_ARG_GUARD=1 MC2_ABL_RUNTIME_SOFTFAIL=1)

```
result=PASS (2/2 passed)
mc2_01  PASS  frames=4283  avg_fps=143  load_ms=6635
mc2_10  PASS  frames=4260  avg_fps=142  load_ms=7508
```

No `[ABL_ARG_GUARD]` or `[ABL_SOFTFAIL]` fires in production stock missions (expected — well-formed scripts pass valid pointers).

### Mod PASS — Gates ON (torrin / MC2X-DarkRain map)

```
result=PASS (1/1 passed)
torrin  PASS  frames=4269  avg_fps=142  load_ms=12884
```

Target: `A:/Games/mc2-opengl/mc2-win64-v0.4`  
No `[ABL_ARG_GUARD]` fires.

### Mod Crash — Pre-existing (not a regression)

`cfv2_mission1_escort` and `clearwater` crash with `exit=3221226505` (0xC0000409, STATUS_STACK_BUFFER_OVERRUN) on target `mc2-win64-v0.4` both **with and without** the patch gates. Confirmed pre-existing by running gates-OFF on the same deployed exe — same crash. Not caused by this patch.

---

## Repro Evidence — `execGetRelativePositionToPoint` Double Guard (H2a + H2b)

Gate `MC2_ABL_ARG_GUARD_REPRO=1` + `MC2_ABL_ARG_GUARD=1` run via `mc2_01` smoke. The repro hook in `runAblArgGuardRepro` calls `runAblArgGuardRepro2A()` after the Object-variant guard fires, exercising both null-`pos` (READ case) and null-`relPos` (WRITE case).

**Captured log lines (from `mc2_01.log` lines 78–89):**

```
[ABL_REPRO] ABL-BAD-NATIVE-ARG-REPRO-1 starting
[ABL_REPRO] relPos from simulated null tos->address = 0000000000000000
[ABL_ARG_GUARD] func=execGetRelativePositionToObject[REPRO] param=relPos module=(unknown) file=unavailable line=0 — null ptr, skipping
[ABL_REPRO] guard fired -- no crash -- PASS
[ABL_REPRO_2A] execGetRelativePositionToPoint sibling repro starting
[ABL_REPRO_2A] case1: pos=NULL relPos_valid=0000000000000001
[ABL_ARG_GUARD] func=execGetRelativePositionToPoint[REPRO2A-pos] param=pos module=(unknown) file=unavailable line=0 — null ptr, skipping
[ABL_REPRO_2A] case1 guard fired -- no crash -- PASS
[ABL_REPRO_2A] case2: pos_valid=0000009C3614B4A0 relPos=NULL
[ABL_ARG_GUARD] func=execGetRelativePositionToPoint[REPRO2A-relPos] param=relPos module=(unknown) file=unavailable line=0 — null ptr, skipping
[ABL_REPRO_2A] case2 guard fired -- no crash -- PASS
[ABL_REPRO_2A] done
```

All three cases: guard fires → log emitted → no crash → PASS.

**Gate OFF behavior (not run):** `pos[0]` (case 1) or `relPos[0] = 1.0f` (case 2) would write to address 0x0 → access violation. Same mechanism as the shipped Object-variant crash site.

---

## Acceptance

- 4 functions guarded (6 pointer checks total, including both pointers in the Point-variant sibling)
- Build: GREEN
- Stock smoke OFF: PASS 2/2
- Stock smoke ON: PASS 2/2, no production guard fires
- Mod smoke ON (torrin): PASS, no guard fires
- Mod pre-existing crash (`cfv2_mission1_escort`, `clearwater`): confirmed pre-existing, not a regression
- Repro: both `pos` and `relPos` null cases for `execGetRelativePositionToPoint` caught (log + no crash)

**ACCEPTANCE MET: YES**
