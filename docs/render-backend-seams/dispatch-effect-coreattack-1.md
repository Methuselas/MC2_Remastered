# DISPATCH-EFFECT-COREATTACK-1 — design as built

**Status:** built on branch `claude/dispatch-effect-coreattack-1` off nifty `dff3f248`. Build green. Gate matrix run from main process after the implementing agent died mid-flight (same recovery pattern as COREMOVETO-1: in-process state lost, code complete on disk).

## Scope

Fifth real TechScript effect. Canonical `OPORD.CoreAttack <target_wid>` with bare-integer WID arg, fires `setGeneralTacOrder(TACTICAL_ORDER_ATTACK_OBJECT)` once per warrior per mission load under pure once-guard semantics. Reuses `MC2_BRAIN_DISPATCH_APPLY` gate (default OFF). No new fn in the forbidden-call guard — existing `setGeneralTacOrder` allowance covers ATTACK_OBJECT (1 new constant, not new fn).

## Anti-pattern flagged + enforced

**DO NOT CALL `orderAttackObject`** in the apply path. Its side-effects (setAttackTarget, setSituationOpenFire, goal planning) are forbidden under the relaxed-call guard. The handler hand-constructs the TacticalOrder following missiongui.cpp:2805 verbatim — `init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_ATTACK_OBJECT, false)` + `.targetWID = parsedWID` + `.attackParams` — then ships exactly one `setGeneralTacOrder(attackOrder)`. The forbidden-call doc-comment (both header line 27-39 + inline line 455-468) lists orderAttackObject explicitly so future agents don't fall into it.

## Triple-guard via single lookup

The pre-apply phase resolves target ONCE (`ObjectManager->getByWatchID(parsedWID)`) and runs three checks:

| Guard | Soft-fail trace | Skip slot write? |
|---|---|---|
| target == nullptr | `[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=<wid> reason=not_found` | Yes |
| target's WID == warrior's own WID | `[BRAIN_DISPATCH_ATTACK_SELF] wid=<wid>` | Yes |
| target same team as warrior | `[BRAIN_DISPATCH_ATTACK_FRIENDLY] wid=<wid>` | Yes |

Plus parse-layer guards:
- Non-numeric token after verb → `[BRAIN_DISPATCH_ATTACK_PARSE_FAIL] verb=<text>`
- Zero/negative parsed WID → `[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=<v>` (no lookup)

All five guards are pre-slot-write; no `setGeneralTacOrder` call fires on any failure mode.

## Files (4 code + 3 fixtures + this doc)

| File | Change |
|---|---|
| `code/brain_special_dispatch.cpp` | Apply handler `OPORD.CoreAttack` ~line 637; `bodyHasCoreAttack` ~line 432; `bodyHasEffect` extended to 5; header doc-comment updated to 5 permitted constants + orderAttackObject ban |
| `code/brain_special_dispatch.h` | `bodyHasCoreAttack` prototype (line 131) |
| `code/mech_brain_runtime.h` | `attackEffectApplied` flag (line 105), next to `moveToEffectApplied` |
| `code/warrior.cpp` | 5-effect `alreadyDone` matrix (lines 2297-2306); flag-set in effect branch (lines 2321-2322) |
| `tests/fixtures/brain_runtime/mc2_01_attack_specials.fit` | Bad-WID gate (WID=0, always fails) |
| `tests/fixtures/brain_runtime/mc2_17_attack_live_specials.fit` | Live attack on Warrior4 → targetWID=49 (stable enemy mover in mc2_17 roster — fragility documented below) |
| `tests/fixtures/brain_runtime/mc2_17_ai.fit` | Sets Warrior4 to Enhanced for mc2_17 (mirror of mc2_01_ai.fit) |

## Design decisions

- **Arg shape (a) bare-integer WID.** `DO OPORD.CoreAttack <wid>` — `strtol`. No resolver in this slice. Brittle for cross-mission use but concrete-numeric is sufficient for proving the effect verb; a `unitRef→WID` resolver is a future slice.
- **Pure once-guard.** Engine auto-clears the attack order at warrior.cpp:5195-5198 when the target's `getByWatchID` returns nullptr (target died/removed). Once-guard prevents oscillation; the engine's death-clear path is the canonical post-effect transition. State-machine model (re-issue after auto-clear) was rejected — would loop on null-WID reads.
- **Forbidden-call guard widened by one CONSTANT, not one FN.** `setGeneralTacOrder` was the sole permitted order fn; ATTACK_OBJECT is now the fifth permitted constant routed through it.
- **No save/load change.** `MechBrainRuntime` is mission-ephemeral.

## Once-guard matrix (5-effect)

```
hasEffect    = pd || ej || gd || mv || at
alreadyDone  = hasEffect &&
               (!hasPd || pdDone) &&
               (!hasEj || ejDone) &&
               (!hasGd || gdDone) &&
               (!hasMv || mvDone) &&
               (!hasAt || atDone)
```

## WID brittleness — documented

Gate C uses WID=0 — always invalid, stable across all mission loads. Proves the bad-WID guard fires without needing a live target.

Gate G uses **targetWID=49** in mc2_17. This WID was identified as a stable enemy mover in the mc2_17 roster (verify-once via a probe run if regressing; subsequent loads with the same content produce the same WID assignment). Fragility: if mc2_17's content changes (mod update, mission edit), WID=49 may no longer point to the intended enemy. Mitigation = the triple-guard: a stale WID becomes a not_found/self/friendly soft-fail instead of a crash. The test would silently no-op (Gate G PASS but the underlying behavior wouldn't fire) — that's a known limitation, accepted for the first slice; a unitRef resolver in a future slice would remove the brittleness.

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

Plus deploy `mc2_<mission>_ai.fit` alongside the specials fixture (Warrior4 → Enhanced; allocates brainRuntime at mission load).

## Deploy lane

v0.5.0 EXCLUSIVE for this slice. A concurrent agent owns v0.4; do not route gate runs to v0.4/0.4c/0.4d-rc1/0.5-testing.

## Acceptance gate matrix

See run results in commit body / verification log. ABORT CONDITION: Gate G mission-failure (combat-resolution-interaction crash, not heartbeat flake) — would halt the commit and trigger investigation.

## Lineage

POWERDOWN (1B) → EJECT → GUARD → MOVETO → **ATTACK (this slice)**. Five effect verbs in the near-term batch banked.
