# DISPATCH-EFFECT-UNITEJECT-1

**Status:** SHIPPED — branch `claude/dispatch-effect-uniteject-1`
**Date:** 2026-06-24

## Purpose

Extends the brain-special dispatch system (TECHSCRIPT-SPECIAL-DISPATCH-1B) to handle `Unit.Eject` / `coreEject` verbs, issuing `TACTICAL_ORDER_EJECT` once per warrior per mission load. Mirrors the existing `Brain.CorePower false → POWERDOWN` path.

## Changes

| File | Change |
|---|---|
| `code/brain_special_dispatch.cpp` | `Unit.Eject` in `kRecognizedVerbs[]`; `aliasToCanonical()`; `bodyHasUnitEject()`; `bodyHasEffect()`; Unit.Eject handler in `executeSpecialBody_Apply` |
| `code/brain_special_dispatch.h` | Declarations for `bodyHasUnitEject`, `bodyHasEffect` |
| `code/mech_brain_runtime.h` | `ejectEffectApplied` flag in `MechBrainRuntime` |
| `code/warrior.cpp` | Dispatch block uses `bodyHasEffect()` + per-effect once-guards |
| `code/mission.cpp` | `MC2_BRAIN_SPECIAL_FIT` env override redirects `parseBrainSpecialBody` |
| `scripts/run_smoke.py` | `MC2_BRAIN_SPECIAL_FIT` and `MC2_BRAIN_DISPATCH_CALL` added to allowlist |
| `tests/fixtures/brain_runtime/mc2_01_eject_specials.fit` | Gate C fixture — canonical `Unit.Eject` |
| `tests/fixtures/brain_runtime/mc2_01_ejectalias_specials.fit` | Gate D fixture — `coreEject` alias |

## Alias map

`aliasToCanonical()` in `brain_special_dispatch.cpp`:

- `"coreEject"` → `"Unit.Eject"`
- `"corePower"` → `"Brain.CorePower"`

## Fixture naming convention

`MC2_BRAIN_SPECIAL_FIT=<name>` → engine opens `data/missions/<name>_specials.fit`.

Examples:
- `MC2_BRAIN_SPECIAL_FIT=mc2_01_eject` → `data/missions/mc2_01_eject_specials.fit`
- `MC2_BRAIN_SPECIAL_FIT=mc2_01_ejectalias` → `data/missions/mc2_01_ejectalias_specials.fit`

## Gate matrix

| Gate | Config | Expected | Result |
|---|---|---|---|
| A | tier1, no gates | PASS 5/5, no BRAIN traces | PASS |
| B | `MC2_BRAIN_DISPATCH_APPLY=1`, no fixture | PASS 5/5 | PASS |
| C | all brain gates + `MC2_BRAIN_SPECIAL_FIT=mc2_01_eject` | EXACTLY ONE `[BRAIN_DISPATCH_APPLY] verb=Unit.Eject effect=EJECT wid=4` | PASS |
| D | all brain gates + `MC2_BRAIN_SPECIAL_FIT=mc2_01_ejectalias` | EXACTLY ONE `[BRAIN_DISPATCH_APPLY] verb=Unit.Eject effect=EJECT wid=4` | PASS |
| E | all brain gates + `MC2_BRAIN_SPECIAL_FIT=mc2_01_specials_raw` | EXACTLY ONE `[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=4`, no EJECT | PASS |
| F | mc2_10 + mc2_24 + `MC2_BRAIN_DISPATCH_APPLY=1`, no fixture | PASS 2/2, no spurious EJECT | PASS |

Required gates for effect traces:
`MC2_BRAIN_RUNTIME=1 MC2_BRAIN_RUNTIME_APPLY=1 MC2_BRAIN_RUNTIME_FORCE_MODE=enhanced MC2_BRAIN_TASKQ=1 MC2_BRAIN_DISPATCH=1 MC2_BRAIN_DISPATCH_APPLY=1`

## RELAXED-CALL GUARD

Only `warrior->setGeneralTacOrder()` is permitted inside `executeSpecialBody_Apply`. No FSM changes, no var-store writes, no OPORD advancement, no save changes.

Covered effects: `POWERDOWN` (Brain.CorePower false) and `EJECT` (Unit.Eject / coreEject).
