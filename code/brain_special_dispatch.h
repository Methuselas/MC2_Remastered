#pragma once
// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// Gate: MC2_BRAIN_DISPATCH (default OFF).
//
// FORBIDDEN-CALL GUARD: executeSpecialBody_TraceOnly MUST NOT call ANY of:
//   setGeneralTacOrder, setPlayerTacOrder, setAlarmTacOrder, requestHelp,
//   requestTarget, clearCurTacOrder, setMainGoal, calcTacOrder, coreMoveTo,
//   or ANY movement/attack/order function.
// This function calls ONLY: token parsing + fprintf.
// Enforcement: verified by code inspection + comment contract below.

#include <vector>
#include <string>

struct BrainSpecialBody {
    std::vector<std::string> verbs;  // raw DO-verb tokens from BrainSpecial Body block
    bool loaded = false;
};

// Recognized verbs (1A mappable set from recon):
//   Brain.CorePower, Brain.CoreAttack, OPORD.CoreGuard, OPORD.CorePatrol,
//   OPORD.CoreMoveTo, Unit.Retreat, HOLD
// All others → UNKNOWN trace.
//
// TRACE ONLY. Zero effects. No orders. No state writes.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid);

// Parse BrainSpecial Body DO-verbs from open FitIniFile. Returns false if block absent.
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody);
