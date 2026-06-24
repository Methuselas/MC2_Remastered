# DISPATCH-EFFECT-COREMOVETO-1 — design as built

**Status:** built on branch `claude/dispatch-effect-coremoveto-1` off nifty `fa938b0f`. Build green. Gate matrix run from main process after the implementing agent died mid-flight (in-process state lost but code complete on disk).

## Scope

Fourth real TechScript effect verb. Canonical `OPORD.CoreMoveTo` with **bare-numeric `x y z [params=N]`** args, world-space float coords, fires `setGeneralTacOrder(TACTICAL_ORDER_MOVETO_POINT)` exactly once per mission per warrior under pure once-guard semantics. Reuses `MC2_BRAIN_DISPATCH_APPLY` gate (default OFF). No new gate, no new fn in the forbidden-call guard (existing `setGeneralTacOrder` allowance covers MoveTo).

## Files (4 code + 2 fixtures + this doc)

| File | Change |
|---|---|
| `code/brain_special_dispatch.cpp` | Apply handler for `OPORD.CoreMoveTo` (lines ~562-615), `bodyHasCoreMoveTo` helper (line 411), `bodyHasEffect` extended (line 428), header doc-comment updated to 4 effects |
| `code/brain_special_dispatch.h` | `bodyHasCoreMoveTo` prototype (line 124) |
| `code/mech_brain_runtime.h` | `moveToEffectApplied` flag (line 104) next to `guardEffectApplied` |
| `code/warrior.cpp` | `alreadyDone` 4-effect matrix (line 2296-2304); flag-set in effect branch (line 2317-2318) |
| `tests/fixtures/brain_runtime/mc2_01_moveto_specials.fit` | Canonical fixture: `DO OPORD.CoreMoveTo 100 200 0` |
| `tests/fixtures/brain_runtime/mc2_01_moveto_nan_specials.fit` | NaN-guard fixture: `DO OPORD.CoreMoveTo nan 200 0` |

## Design decisions

- **Arg shape (b) bare-numeric.** `DO OPORD.CoreMoveTo x y z [params=N]` — `strtof` parse, whitespace split. Rationale: carver content uses ABL-var-resolved waypoint names (e.g. `group1Loc`) which the TechSpecial dispatcher has no var store for; bare floats are the lowest-risk first cut. Named-waypoint resolution is a future slice.
- **Once-guard model (a) pure once-guard** (NOT recon-recommended (c) state-machine). Rationale: patrol patterns belong to `OPORD.CorePatrol` (separate verb, future slice). MoveTo is one-shot. State-machine re-issue would (i) cause unit jitter at the destination, (ii) clobber later non-MOVETO orders. Pure once-guard matches POWERDOWN/EJECT/GUARD's pattern exactly: single slot write, single trace, GENERAL slot claimed for the mission.
- **NaN-coord soft-fail guard.** `std::isnan` check on x/y/z before constructing the TacticalOrder; emits `[BRAIN_DISPATCH_MOVETO_NAN]` and skips (no `setGeneralTacOrder` call, no slot claim). Parse failures emit `[BRAIN_DISPATCH_MOVETO_PARSE_FAIL]` and skip identically. Mirrors `ablmc2.cpp:5031` defensive pattern.
- **Forbidden-call guard unchanged.** `setGeneralTacOrder` was already the only permitted order fn. Adding MoveTo just extends the set of permitted **constants** (POWERDOWN/EJECT/GUARD + MOVETO_POINT), not permitted fns.
- **No save/load change.** `MechBrainRuntime` is mission-ephemeral.

## Once-guard matrix (now 4-effect)

```
hasEffect    = pd || ej || gd || mv
alreadyDone  = hasEffect &&
               (!hasPd || pdDone) &&
               (!hasEj || ejDone) &&
               (!hasGd || gdDone) &&
               (!hasMv || mvDone)
```

Each effect's `xDone = hasX && xEffectApplied`. Flags set BEFORE Apply runs (so a re-entrant tick can't double-fire). Same shape as the 3-effect Guard slice, extended by one column.

## Failure modes addressed

| Mode | Path |
|---|---|
| NaN coord | Soft-fail trace, no slot claim, no apply |
| Parse failure (non-numeric token) | Soft-fail trace, no slot claim, no apply |
| Unit cannot move (POWERDOWN already applied, ejected, etc.) | Engine's `setGeneralTacOrder` is a no-op for incapacitated units; relaxed-guard does not need extra logic — POWERDOWN's once-guard already prevents POWERDOWN+MoveTo coexistence at the slot level |
| Coord out of bounds | Engine's PathManager handles unreachable destinations (silent failure to pathfind, unit idles) — not a crash class. Verified by smoke. |

## Acceptance gate matrix

See `dispatch-effect-coremoveto-1.gates.md` (run from main process; agent that built the code did not run gates before dying).

## Lineage

POWERDOWN (1B) → EJECT (UNITEJECT-1) → GUARD (COREGUARD-1) → **MOVETO (this slice)**. Pattern fully mechanical now; arg parsing + NaN guard are the new template additions every coord-bearing verb will reuse.
