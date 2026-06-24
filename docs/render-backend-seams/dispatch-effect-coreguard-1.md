# DISPATCH-EFFECT-COREGUARD-1

**Status:** SHIPPED — branch `claude/dispatch-effect-coreguard-1`
**Date:** 2026-06-24

## Purpose

Third TechScript effect verb. Extends the brain-special dispatch system (TECHSCRIPT-SPECIAL-DISPATCH-1B / DISPATCH-EFFECT-UNITEJECT-1) to handle `OPORD.CoreGuard` / `coreGuard` verbs, issuing `TACTICAL_ORDER_GUARD` once per warrior per mission load. Mirrors the existing POWERDOWN and EJECT paths exactly.

## Changes

| File | Change |
|---|---|
| `code/brain_special_dispatch.cpp` | `coreGuard` alias in `aliasToCanonical()`; `bodyHasCoreGuard()`; `bodyHasEffect()` extended; Guard handler in `executeSpecialBody_Apply`; RELAXED-CALL GUARD doc updated to 3 permitted calls |
| `code/brain_special_dispatch.h` | Declaration for `bodyHasCoreGuard`; updated `bodyHasEffect` doc |
| `code/mech_brain_runtime.h` | `guardEffectApplied` flag in `MechBrainRuntime` |
| `code/warrior.cpp` | `hasGuard`/`guardDone` once-guard vars; `alreadyDone` extended; `guardEffectApplied` set before Apply |
| `tests/fixtures/brain_runtime/mc2_01_guard_specials.fit` | Gate C proof fixture — canonical `OPORD.CoreGuard` |

## Alias map

`aliasToCanonical()` in `brain_special_dispatch.cpp`:

- `"coreGuard"` → `"OPORD.CoreGuard"`  (DISPATCH-EFFECT-COREGUARD-1)
- `"coreEject"` → `"Unit.Eject"`        (DISPATCH-EFFECT-UNITEJECT-1)
- `"corePower"` → `"Brain.CorePower"`   (future)

## Three-effect generalization

`bodyHasEffect()` now returns: `bodyHasPowerdown(body) || bodyHasUnitEject(body) || bodyHasCoreGuard(body)`

`alreadyDone` in warrior.cpp:
```cpp
const bool hasGuard   = bodyHasCoreGuard(brainRuntime->specialBody);
const bool guardDone  = hasGuard && brainRuntime->guardEffectApplied;
const bool alreadyDone = hasEffect && ((!hasPowerdown || powerdownDone) && (!hasEject || ejectDone) && (!hasGuard || guardDone));
```

Each effect has an independent once-guard flag. A body with multiple effect verbs (e.g. EJECT + GUARD) fires each exactly once.

## RELAXED-CALL GUARD

`executeSpecialBody_Apply` now permits THREE `setGeneralTacOrder` call-sites:
1. `Brain.CorePower false` → `TACTICAL_ORDER_POWERDOWN`
2. `Unit.Eject` (or `coreEject`) → `TACTICAL_ORDER_EJECT`
3. `OPORD.CoreGuard` (or `coreGuard`) → `TACTICAL_ORDER_GUARD`

All other verbs = trace only. FORBIDDEN: `setPlayerTacOrder`, `setAlarmTacOrder`, `requestHelp`, `requestTarget`, `calcTacOrder`, `coreMoveTo`, `setMainGoal`, `clearCurTacOrder`, any movement/attack/OPORD-advance/commander function.

## SUPERSEDE-HOLD

Guard claims the GENERAL slot (same as POWERDOWN/EJECT). `dispatcherAppliedEffect=true` suppresses the synthetic HOLD_TASK write.

## Gate matrix

Required env for effect traces: `MC2_BRAIN_RUNTIME=1 MC2_BRAIN_RUNTIME_APPLY=1 MC2_BRAIN_RUNTIME_FORCE_MODE=enhanced MC2_BRAIN_TASKQ=1 MC2_BRAIN_DISPATCH=1 MC2_BRAIN_DISPATCH_APPLY=1`

| Gate | Config | Expected | Result |
|---|---|---|---|
| A | tier1, no gates | PASS 5/5, byte-identical | PASS (mc2_24 flake retry once) |
| B | `MC2_BRAIN_DISPATCH=1 MC2_BRAIN_DISPATCH_APPLY=0 MC2_BRAIN_SPECIAL_FIT=mc2_01_guard` | PASS, no APPLY trace | PASS |
| C | full brain env + `MC2_BRAIN_SPECIAL_FIT=mc2_01_guard` | EXACTLY ONE `[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreGuard effect=GUARD wid=4` | **PASS** |
| D | full brain env + `MC2_BRAIN_SPECIAL_FIT=mc2_01` (POWERDOWN fixture) | POWERDOWN fires once (GUARD also fires — both in fixture) | PASS |
| E | full brain env + `MC2_BRAIN_SPECIAL_FIT=mc2_01_eject` (EJECT fixture) | EJECT fires exactly once, GUARD also fires once (both in fixture) | PASS |
| F | mc2_10 + mc2_24 + full brain env, no fixture | PASS 2/2, no spurious GUARD | PASS |

### Gate C proof line (verbatim from log):
```
[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreGuard effect=GUARD wid=4
```
One APPLY line, one GENERAL-slot write, HOLD suppressed thereafter.

## Fingerprint

`sha=54f541a8b04e branch=claude/dispatch-effect-coreguard-1 dirty=1`

## No save change

`MechBrainRuntime` is mission-ephemeral (`brainRuntime = new MechBrainRuntime()` at mission load, not serialized). Adding `guardEffectApplied` does not affect save/load format.
