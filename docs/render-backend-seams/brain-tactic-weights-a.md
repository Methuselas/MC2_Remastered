# TACTIC-WEIGHTS-A — Deterministic Weighted Tactic Selection

**Status:** SHIPPED (TRACE-ONLY)
**Gate:** `MC2_TACTIC_WEIGHTS=1` (default OFF)
**Mission seed:** `MC2_BRAIN_MISSION_SEED=<uint>` (default `0xDEADBEEF` placeholder)
**Deferred to B:** wiring `selectedTactic` → `attackParams.tactic`
**Deferred to C:** per-FSM-state weight tables

---

## Scope (TRACE-ONLY)

Gate OFF → byte-identical to pre-A.
Gate ON → per-warrior hash → modulated weights → deterministic tactic selection →
`rt->selectedTactic` stored + `[BRAIN_TACTIC_SELECT]` emitted. **NO behavior write.**

---

## Enum additions (tacordr.h)

Appended after `TACTIC_JOUST` (numeric stability maintained):

| Value | Name | Numeric |
|-------|------|---------|
| `TACTIC_INDIRECT_FIRE` | IndirectFire | 7 |
| `TACTIC_HULL_DOWN` | HullDown | 8 |
| `TACTIC_FIGHTING_WITHDRAW` | FightingWithdraw | 9 |
| `TACTIC_PURSUE` | Pursue | 10 |
| `TACTIC_HIT_AND_RUN` | HitAndRun | 11 |
| `NUM_TACTICS` | — | 12 |

---

## MechBrainRuntime addition (mech_brain_runtime.h)

```cpp
int selectedTactic = 0;  // TacticType enum; TRACE-ONLY; mission-ephemeral
```

---

## Pure functions (code/brain_tactic_select.{h,cpp})

### `tacticWang(wid, tick, missionSeed) → uint32_t`
Wang-hash PRNG. Input mix:
```
h = (uint32_t)wid ^ (tick * 2654435761u) ^ (missionSeed * 1013904223u)
```
Then two Wang finaliser rounds (`h ^= h>>16; h *= 0x45d9f3b; h ^= h>>16; h *= 0x45d9f3b; h ^= h>>16`).
Deterministic: same `(wid, tick, missionSeed)` → same `h` always.

### `applyPilotModulation(wBase[], wEff[], numTactics, gunnery, aggressiveness, courage)`
- High aggressiveness (`aggNorm = aggressiveness/10`) → `lerp(0.7, 1.5, aggNorm)` boosts Joust/Pursue/HitAndRun; `lerp(1.3, 0.5, aggNorm)` depresses FightingWithdraw/HullDown.
- High gunnery (`gunNorm = gunnery/10`) → `lerp(0.8, 1.4, gunNorm)` boosts IndirectFire/StopAndFire.
- `courage < 3` → zeroes Pursue/HitAndRun/Joust (coward refuses aggressive commitments).
- Normalize to sum=1; all-zero fallback → `TACTIC_STOP_AND_FIRE`.

### `selectTactic(wEff[], numTactics, hash) → int`
```
r = (hash & 0xFFFFFF) / 16777216.0f  // [0,1)
walk cumulative weights; return first i where cumulative > r
```
Pure: same `(wEff, hash)` → same result always.

---

## [Tactics] FIT loader

**Location:** `code/mission.cpp` — immediately after the `[Brain]` block loader
(after `delete aiFit;`, before TECHSCRIPT-SPECIAL-DISPATCH-1A).

Format (`<mission>_ai.fit`):
```
[Tactics]
f StopAndFire      = 0.30
f Joust            = 0.15
f IndirectFire     = 0.12
f HullDown         = 0.08
f FightingWithdraw = 0.07
f Pursue           = 0.05
f HitAndRun        = 0.03
; (remaining tactics: omitted keys default to 0.0)
```
Loaded via `FitIniFile::readIdFloat`. Sum normalized at load; `weightsNormalized=1` set.
All-zero warriors keep zero weights — `applyPilotModulation` all-zero branch handles fallback.

**Fixture:** `tests/fixtures/brain_runtime/mc2_01_ai.fit` — `[Tactics]` block added.

---

## Call site (warrior.cpp)

**Location:** `MechWarrior::runBrain`, immediately before unconditional housekeeping
(`CurGroup = NULL;`, ~line 2549 pre-patch, ~line 2590 post-patch).

Runs for BOTH legacy and enhanced paths. Gate ON + `brainRuntime != nullptr`:
1. Reads `skills[MWS_GUNNERY]`, `aggressiveness`, `courage` from warrior.
2. Computes `h = tacticWang(vehicleWID, s_brainTickIndex, s_missionSeed)`.
3. Calls `applyPilotModulation(rt->tacticWeights, wEff, ...)`.
4. Calls `selectTactic(wEff, NUM_TACTICS, h)` → `rt->selectedTactic`.
5. Emits `[BRAIN_TACTIC_SELECT] wid=<W> tick=<T> tactic=<name>(<idx>)`.
6. **No write to `attackParams.tactic`** — deferred to TACTIC-WEIGHTS-B.

---

## Determinism smoke (LEAD runs)

Two smoke runs with identical `MC2_BRAIN_MISSION_SEED` must produce identical `[BRAIN_TACTIC_SELECT]` lines. Gate: `MC2_TACTIC_WEIGHTS=1`.

---

## Deferred

- **TACTIC-WEIGHTS-B:** wire `selectedTactic` → `attackParams.tactic` (behavior wiring).
- **TACTIC-WEIGHTS-C:** per-FSM-state weight tables (override global weights by state).
- Save-game serialization (BRAIN-SAVELOAD-1 prereq, not owned here).
- Difficulty modulation (post-B).
