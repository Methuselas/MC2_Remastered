#pragma once
// brain_tactic_select.h — TACTIC-WEIGHTS-A deterministic weighted tactic selection.
// Gate: MC2_TACTIC_WEIGHTS (default OFF).
// TRACE-ONLY: selectTactic result is stored to MechBrainRuntime::selectedTactic and
// emitted as [BRAIN_TACTIC_SELECT] trace.  NOT wired into attackParams.tactic (TACTIC-WEIGHTS-B).
//
// All functions are PURE (no global mutation, no warrior mutation) so they are
// harness-linkable and unit-testable without the full engine.
//
// Wang-hash determinism: same (tacticWeights, wid, tick, missionSeed) -> same tactic ALWAYS.

#include <cstdint>

// ---------------------------------------------------------------------------
// Wang hash — deterministic PRNG for tactic selection.
// h = wid XOR tick*2654435761u XOR missionSeed*1013904223u, then Wang-mix.
// Returns a 32-bit hash uniform over [0, UINT32_MAX].
uint32_t tacticWang(int wid, uint32_t tick, uint32_t missionSeed);

// ---------------------------------------------------------------------------
// Pilot modulation: compute effective weights wEff[NUM_TACTICS] from base weights.
// Parameters:
//   wBase[NUM_TACTICS]  — raw FIT-loaded weights (may be zeroed for non-loaded warriors)
//   wEff[NUM_TACTICS]   — output (caller-allocated, length NUM_TACTICS)
//   gunnery             — skills[MWS_GUNNERY] (0..10)
//   aggressiveness      — MechWarrior::aggressiveness (0..10)
//   courage             — MechWarrior::courage (0..10)
//
// Modulation rules (per GDD §4 / recon §4):
//   High aggressiveness (>6) lifts Pursue/HitAndRun/Joust, depresses FightingWithdraw/HullDown.
//   High gunnery (>6) lifts IndirectFire/StopAndFire.
//   courage<3 ZEROES Pursue/HitAndRun/Joust.
//   Normalized to sum=1; all-zero fallback → TACTIC_STOP_AND_FIRE uniform weight.
void applyPilotModulation(const float wBase[], float wEff[], int numTactics,
                          int gunnery, int aggressiveness, int courage);

// ---------------------------------------------------------------------------
// Weighted pick: given effective weights (normalized, length numTactics) and a
// Wang hash, return the selected tactic index [0..numTactics-1].
// Pure: same (wEff, hash) -> same result always.
int selectTactic(const float wEff[], int numTactics, uint32_t hash);

// ---------------------------------------------------------------------------
// Name lookup for trace output (returns short ASCII string, never nullptr).
const char* tacticName(int tacticIdx);
