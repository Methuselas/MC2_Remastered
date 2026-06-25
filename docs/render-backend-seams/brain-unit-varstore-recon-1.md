# BRAIN-UNIT-VARSTORE-RECON-1 — REFRAMED (var-store is the WRONG path)

**Read-only recon, 2026-06-25.** Intended to design the per-unit typed var-store
that SYMBOLIC-OPORD-ARGS-RECON-1 said was needed. **Instead it overturns that
premise.** Crux independently verified (the recon agent's verdict was wrong).

## HEADLINE: the resolved OPORD data is ALREADY in `mission.fit`, declaratively

The carver conversion emitted, inline in each `[WarriorN]` `Brain {}` block in
`mission.fit`, the **fully-resolved, literal** OPORD config:

```
PrimaryOPORD {
    type = Patrol
    loop = true
    Waypoint { index=0; x=3008.000; y=1472.000 }
    Waypoint { index=1; x=3136.000; y=960.000 }
}
SecondaryOPORD { type = Guard }
Tactics { Flank = 0.45  Standard = 0.35  HitAndRun = 0.20 }
```
mc2_01 `mission.fit`: **29 `PrimaryOPORD` blocks, 64 `Waypoint` blocks**;
`PrimaryOPORD { type = PlayerControlled }` for player units; `SecondaryOPORD
{ type = Guard }` widely. The patrol waypoint `(3008,1472)` is exactly the ABL
`startBase1PatrolPath[0]` — but here it is a literal in structured DSL, NOT a
symbolic name.

So the symbolic tick-script line `DO OPORD.CorePatrol startBase1PatrolState
startBase1PatrolPath AttackStateHandle` (in `mission_specials.fit`) is the
**legacy imperative form**; `mission.fit`'s `PrimaryOPORD{…Waypoint…}` is the
**declarative resolved form** of the same intent. The declarative form needs NO
symbol resolution, NO var-store, NO ABL scan.

## What the two prior recons got wrong
- **SYMBOLIC-OPORD-ARGS-RECON-1** (DEFER → var-store): correct that the *tick
  script's* args are symbolic, but missed that the resolved values live in
  `mission.fit` declaratively. Var-store is not required to get patrol/guard
  behavior.
- **BRAIN-UNIT-VARSTORE-RECON-1 agent verdict** (scan `legacy_source/*.abl` at
  runtime): WRONG. Claimed the patrol literals exist "exclusively in
  legacy_source ABL". Verified false — they are in `mission.fit` Waypoint blocks.
  Reading audit-only legacy ABL at runtime would have been an architecturally bad
  design built on a false premise. (Agent emitted this from ~1 tool-use — see
  memory `recon-agent-verify-crux-claims`.)

## Corrected next arc: BRAIN-MISSIONFIT-OPORD-CONSUMER
Read the declarative `Brain {}` OPORD config from `mission.fit` and drive the
existing patrol/guard system from it:
- `PrimaryOPORD/SecondaryOPORD { type=Patrol; loop; Waypoint{index,x,y} }` →
  populate `MechBrainRuntime::patrolWaypoints/patrolWaypointCount/patrolLoop`
  (the structures BRAIN-OPORD-COREPATROL-1 already added) at mission load.
- `type = Guard` → guard at spawn/anchor. `type = PlayerControlled` → no AI.
- `Tactics { Name=weight }` → the tactic-weight table (TACTIC-WEIGHTS-A path).
- The PARSER ALREADY EXISTS: `_validate_brain_mission` in
  `scripts/check-brain-fit-schema.py` (BRAIN-SCHEMA-CARVER-COMPAT-1) reads
  exactly these blocks. A C++ loader mirrors that shape.
This sidesteps symbolic args AND the var-store entirely for patrol/guard. All
mutations still route through the intent→commit chokepoint.

## Open questions before building the consumer
1. Does the engine load `mission.fit` `[WarriorN]` `Brain {}` blocks at all today?
   (Almost certainly NOT — these are carver-new; stock uses `_ai.fit`/ABL.) Need
   to find where `mission.fit` warrior records are parsed and add a `Brain {}`
   reader.
2. Authority: when BOTH a declarative `mission.fit Brain{}` AND an imperative
   tick TechSpecial (`BrainTrigger run=…tick`) exist for a unit, which drives?
   Likely: declarative config sets standing OPORD; tick script handles dynamic
   FSM/specials. Need to decide precedence (probably: load declarative as the
   base OPORD, let tick verbs override).
3. The symbolic tick verbs (`OPORD.CorePatrol startBase1PatrolPath`) — once the
   declarative path drives patrol, can the symbolic tick verb be a NO-OP
   (declarative already applied) or must it re-resolve? Likely no-op/ignore if
   the declarative OPORD is loaded.
4. RUNTIME-COMPUTED bits that are NOT in mission.fit: `startPosition` =
   warrior spawn (a C++ `getPosition()` hook, trivial); `getStateHandle("attack")`
   = FSM transition target (store the name). These remain, but are small and only
   matter for the tick-script path, not the declarative path.

## VERDICT
DO NOT build the typed var-store / ABL scanner. The resolved data is declarative
in `mission.fit`. Scope the real unlock as **BRAIN-MISSIONFIT-OPORD-CONSUMER**
(C++ loader for the inline `Brain {}` OPORD/Waypoint/Tactics blocks → existing
patrol/guard/tactic systems), reusing the parser shape already proven by
`_validate_brain_mission`. Needs a short follow-up recon on Q1 (where mission.fit
warrior records are parsed today) before implementation.

## Cross-refs
- Supersedes the var-store framing in [[symbolic-opord-args-recon-1]] / this file.
- Parser already shipped: BRAIN-SCHEMA-CARVER-COMPAT-1 (`_validate_brain_mission`).
- Recon-verify lesson: memory `recon-agent-verify-crux-claims`.
