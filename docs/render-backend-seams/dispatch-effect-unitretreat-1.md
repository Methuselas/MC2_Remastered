# DISPATCH-EFFECT-UNITRETREAT-1 — design as built

**Status:** built on branch `claude/dispatch-effect-unitretreat-1` off nifty `a3c00396`. Build green. Gate matrix run 2026-06-24.

## Scope

Sixth real TechScript effect. Canonical `Unit.Retreat` (no args), alias `coreRetreat`, fires `setGeneralTacOrder(TACTICAL_ORDER_WITHDRAW)` once per warrior per mission load under pure once-guard semantics. Reuses `MC2_BRAIN_DISPATCH_APPLY` gate (default OFF). No new gate. No args — identical shape to Unit.Eject and OPORD.CoreGuard.

## Files (4 code + 2 fixtures + this doc)

| File | Change |
|---|---|
| `code/brain_special_dispatch.cpp` | Apply handler `Unit.Retreat` (no-arg block after OPORD.CoreAttack); `bodyHasUnitRetreat` ~line 444; `aliasToCanonical` extended with `coreRetreat`→`Unit.Retreat`; `bodyHasEffect` extended to 6 effects; BOTH doc-comments updated to SIX total |
| `code/brain_special_dispatch.h` | `bodyHasUnitRetreat` prototype; `bodyHasEffect` doc updated |
| `code/mech_brain_runtime.h` | `retreatEffectApplied` flag (line 106), next to `attackEffectApplied` |
| `code/warrior.cpp` | 6-effect `alreadyDone` matrix; `hasRetreat`/`retreatDone` computed; `retreatEffectApplied` flag-set in effect branch |
| `tests/fixtures/brain_runtime/mc2_01_retreat_specials.fit` | Canonical `Unit.Retreat` verb gate fixture |
| `tests/fixtures/brain_runtime/mc2_01_retreatalias_specials.fit` | `coreRetreat` alias fixture |

## BOTH doc-comments confirmed updated

The RELAXED-CALL GUARD CONTRACT block (at function definition ~line 471) is the ONLY location of this comment in `brain_special_dispatch.cpp`. Both the count (`FIVE` → `SIX`) and the sixth permitted constant (`Unit.Retreat (or coreRetreat alias) → WITHDRAW`) are present. Confirmed by `grep "RELAXED-CALL GUARD CONTRACT"` returning exactly one hit, and `grep "SIX total"` confirming the update.

## Engine constant

`TACTICAL_ORDER_WITHDRAW` (code/tacordr.h:149). Handled at mover.cpp:3317 as a NO-PARAM order (grouped with REFIT/CAPTURE/RECOVER — all `break` without reading any location/target field). Engine owns withdraw navigation via its `withdrawing` flag.

## Construct used

```cpp
TacticalOrder retreatOrder;
retreatOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WITHDRAW, false);
warrior->setGeneralTacOrder(retreatOrder);
```

Mirrors GUARD shape exactly (same `init` signature, no extra fields).

## Once-guard matrix (6-effect)

```
hasEffect    = pd || ej || gd || mv || at || rt
alreadyDone  = hasEffect &&
               (!hasPd || pdDone) &&
               (!hasEj || ejDone) &&
               (!hasGd || gdDone) &&
               (!hasMv || mvDone) &&
               (!hasAt || atDone) &&
               (!hasRt || rtDone)
```

## Required env bundle (canonical)

```
MC2_BRAIN_RUNTIME=1
MC2_BRAIN_RUNTIME_APPLY=1
MC2_BRAIN_RUNTIME_FORCE_MODE=enhanced    # lowercase — case-sensitive parser
MC2_BRAIN_TASKQ=1
MC2_BRAIN_DISPATCH=1
MC2_BRAIN_DISPATCH_APPLY=1
MC2_BRAIN_SPECIAL_FIT=<fixture-stem>
```

Fixture stem convention: `MC2_BRAIN_SPECIAL_FIT=mc2_01_retreat` loads `data/missions/mc2_01_retreat_specials.fit`.

## Deploy lane

v0.5.0 EXCLUSIVE. Deployed exe fingerprint: `60281923eed16914` (sha256 first 16 hex chars). Commit: `13289413`.

## Acceptance gate matrix

| Gate | Description | Result |
|------|-------------|--------|
| A | no env, tier1 30s, 5/5 | PASS 5/5 |
| B | APPLY=0, retreat fixture, mc2_01 | PASS — Unit.Retreat recognized, no Apply |
| C | APPLY=1, retreat fixture, mc2_01 | PASS — exactly ONE `[BRAIN_DISPATCH_APPLY] verb=Unit.Retreat effect=WITHDRAW wid=4`; HOLD suppressed |
| C2 | APPLY=1, retreatalias fixture (coreRetreat) | PASS — trace identical (alias resolves) |
| D | APPLY=1, mc2_01_specials (POWERDOWN), mc2_01 | PASS — `verb=Brain.CorePower effect=POWERDOWN wid=4` |
| E | APPLY=1, mc2_01_eject (EJECT), mc2_01 | PASS — `verb=Unit.Eject effect=EJECT wid=4` |
| F | APPLY=1, mc2_01_guard (GUARD), mc2_01 | PASS — `verb=OPORD.CoreGuard effect=GUARD wid=4` |
| FF | APPLY=1, mc2_01_moveto (MOVETO), mc2_01 | PASS — `verb=OPORD.CoreMoveTo effect=MOVETO_POINT pos=(100 200 0) wid=4` |
| GG | APPLY=1, mc2_01_attack (ATTACK, WID=0), mc2_01 | PASS — `[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=0 wid=4` soft-fail |
| H | APPLY=1, no fixture, mc2_10 + mc2_24 | PASS — no spurious WITHDRAW traces |

### Gate C trace (verbatim)

```
[BRAIN_DISPATCH_APPLY] verb=Unit.Retreat effect=WITHDRAW wid=4
```

Count: 1 (once-guard confirmed).

### Gate C2 alias trace (verbatim)

```
[BRAIN_DISPATCH_APPLY] verb=Unit.Retreat effect=WITHDRAW wid=4
```

Identical — `coreRetreat` resolves to `Unit.Retreat` via `aliasToCanonical`.

## Design decisions

- **No args.** Engine owns withdraw navigation via `withdrawing` flag; no coords or target needed.
- **Pure once-guard.** Mirrors EJECT/GUARD shape. Flag set before calling Apply (re-entrant tick safety).
- **Unit.* namespace.** Withdraw is a survival action (mirrors Unit.Eject), not a tactical OPORD.
- **No new gate.** Reuses `MC2_BRAIN_DISPATCH_APPLY` (default OFF).
- **No save/load change.** `MechBrainRuntime` is mission-ephemeral.

## Lineage

POWERDOWN (1B) → EJECT → GUARD → MOVETO → ATTACK → **RETREAT (this slice)**. Six effect verbs banked.
