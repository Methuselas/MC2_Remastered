# SYMBOLIC-OPORD-ARGS-RECON-1

**Read-only recon, 2026-06-25.** Scopes whether carver's symbolic OPORD args
(`OPORD.CorePatrol startBase1PatrolPath …`, `OPORD.CoreGuard startPosition …`)
can reuse the `brain_symbol_resolve` enum resolver shipped in
UNITQUERY-SETTARGETPRIORITY-1. Verdict-first; crux independently verified.

## VERDICT: DEFER — needs a per-unit typed var-store + init-phase subsystem

These are **NOT enum constants**. They are **mission-defined, per-unit, runtime
typed references**. `brain_symbol_resolve` (compile-time TARGET_PRIORITY_* /
CONTACT_CRITERIA_* / int tables) has **no extension path** for them.

## Evidence

**Carver usage (symbolic, mission-varied):**
```
DO OPORD.CorePatrol startBase1PatrolState startBase1PatrolPath AttackStateHandle
DO OPORD.CorePatrol PatrolState PatrolPath attackStateHandle
DO OPORD.CoreGuard  startPosition  -1 AttackStateHandle
DO OPORD.CoreGuard  GuardSpot      -1 -1
DO OPORD.CoreGuard  buildingPos    -1 AttackStateHandle
```
Distinct tokens vary per mission (`CurrentPosition`, `GuardBase`, `GuardSpot`,
`baseGuard`, `buildingPos`, `startBase1PatrolPath`, `spawnPath`, …). `-1` is a
literal int.

**Definition (legacy ABL, per-unit FSM-local typed vars)** —
`carver_v_enhanced/missions/mc2_01/legacy_source/warriors/mc2_01_pat1.abl`:
```
static WorldPosition  startPosition;
static PatrolState    startPatrolState;
static PatrolPath     startPatrolPath;
static integer        AttackStateHandle;
...
getObjectPosition(-1, startPosition);        // live spawn coords
AttackStateHandle = getStateHandle("attack"); // runtime FSM-state lookup by name
startBase1PatrolPath[0,0] = 3008.0; ...       // literal world coords filled in init
```
They are populated by the ABL VM in the unit's `init` block before any tick. The
carver transpiler emitted the **variable names as raw text** into `.fit`, not the
evaluated values.

**Engine side** — `code/brain_special_dispatch.cpp` `OPORD.CorePatrol` parser
expects numeric `x1 y1 z1 x2 y2 z2 … [loop|once]` (float parse); symbolic names
fail. No `getPath`/`pathByName`/`namedPath`/`getMarker` brain-queryable registry
exists in `code/` — `getObjectPosition`/`getStateHandle` were ABL VM built-ins
operating in VM context, not C++ runtime lookups by brain-variable name.

## Why the enum resolver cannot cover it
- `startPosition` = live `WorldPosition` from the warrior's spawn — known only at
  unit spawn, per-unit.
- `*PatrolPath` / `*PatrolState` = typed arrays (world-coord pairs; loop mode /
  point count / direction) filled per-unit in `init`.
- `*StateHandle` = `getStateHandle("name")` result — an FSM-state int resolved
  against that unit's FSM at init.
None are fixed compile-time constants. Orthogonal to `brain_symbol_resolve`.

## Smallest viable subsystem (when prioritized)
1. Per-unit typed var-store on `MechBrainRuntime`: `name → {type: WORLD_POS |
   PATROL_STATE | PATROL_PATH | INTEGER, data}`. (Distinct from the existing
   string/int `VarStore` used by Var.Set/Get.)
2. An `init`-phase pass: execute the unit's `init`-labelled TechSpecial block
   once before the first tick — evaluate `getObjectPosition(-1,…)`,
   `getStateHandle(…)`, and the literal array assignments into the var-store.
3. OPORD parsers: if arg is non-numeric, look it up in the var-store by name and
   expand (PatrolPath→waypoint coords; PatrolState→loop/dir/count; WorldPosition→
   guard anchor xyz; StateHandle→int). All mutations still route through the
   intent→commit chokepoint.

This is a multi-structure subsystem + an init-execution model. Build as its own
arc (BRAIN-UNIT-VARSTORE / BRAIN-INIT-PHASE), not a verb slice. It also gates
real patrol/guard behavior for carver content (until then those OPORD verbs no-op
on symbolic args).

## Cross-refs
- Resolver shipped: [[brain-symbol-resolver-and-settargetpriority]] (enum only).
- Modder gap census: `docs/render-backend-seams/modder-carver-conversion-diff-recon-1.md` §6 (flagged "symbolic OPORD args").
