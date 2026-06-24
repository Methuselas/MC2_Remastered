#pragma once
// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — adds MC2_BRAIN_DISPATCH_APPLY gate for first effect.
//
// Gate ladder:
//   MC2_BRAIN_DISPATCH=1                          → parse + trace ONLY (1A behavior, byte-identical)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_APPLY=1 → allow the ONE first effect (Brain.CorePower false → POWERDOWN)
//   MC2_BRAIN_DISPATCH_APPLY=1 alone              → warns + is inert (DISPATCH required)
//
// FORBIDDEN-CALL GUARD (1A — executeSpecialBody_TraceOnly):
//   MUST NOT call ANY of: setGeneralTacOrder, setPlayerTacOrder, setAlarmTacOrder,
//   requestHelp, requestTarget, clearCurTacOrder, setMainGoal, calcTacOrder, coreMoveTo,
//   or ANY movement/attack/order function.
//   Calls ONLY: token parsing + fprintf. Verified by inspection.
//
// RELAXED-CALL GUARD (1B — executeSpecialBody_Apply):
//   The ONLY order function this path may call is setGeneralTacOrder (for Brain.CorePower false → POWERDOWN).
//   STILL FORBIDDEN: setPlayerTacOrder, setAlarmTacOrder, requestHelp, requestTarget,
//   calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder, any movement/attack/
//   OPORD-advance/commander function.
//   All other verbs (Brain.CoreAttack, OPORD.*, Unit.Retreat, Unit.InState, Var.*) → trace only, NO effect.

#include <vector>
#include <string>

class MechWarrior;   // forward decl — executeSpecialBody_Apply needs the warrior pointer

struct BrainSpecialBody {
    std::vector<std::string> verbs;  // raw DO-verb tokens from BrainSpecial Body block
    bool loaded = false;
};

// Returns true if body contains the Brain.CorePower false (POWERDOWN) verb.
bool bodyHasPowerdown(const BrainSpecialBody& body);

// TRACE ONLY. Zero effects. No orders. No state writes.
// Gate: MC2_BRAIN_DISPATCH=1.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid);

// 1B EFFECT DISPATCHER.
// Applies the ONE recognized effect when MC2_BRAIN_DISPATCH_APPLY=1:
//   Brain.CorePower false → warrior->setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)
// All other verbs: trace only, no effect.
// warrior must be non-null (caller guards).
// Returns true if a recognized effect was applied (caller uses to suppress synthetic HOLD).
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid);

// Parse BrainSpecial Body DO-verbs from open FitIniFile. Returns false if block absent.
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody);
