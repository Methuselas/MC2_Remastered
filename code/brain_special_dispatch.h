#pragma once
// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — adds MC2_BRAIN_DISPATCH_APPLY gate for first effect.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw-scan specials.fit for
//   "; TODO: manual ABL line: <payload>" comments (stripped by FitIniFile) and emit
//   structured [BRAIN_DISPATCH_FSM_TODO] traces (information only, no behavior change).
//
// Gate ladder:
//   MC2_BRAIN_DISPATCH=1                          → parse + trace ONLY (1A behavior, byte-identical)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_APPLY=1 → allow the ONE first effect (Brain.CorePower false → POWERDOWN)
//   MC2_BRAIN_DISPATCH_APPLY=1 alone              → warns + is inert (DISPATCH required)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_FSM_TODO=1 → additionally surface FSM TODO markers
//   MC2_BRAIN_DISPATCH_FSM_TODO=1 alone           → warns + is inert (DISPATCH required)
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
//
// FSM-TODO SCANNER (1C — scanFsmTodosFromFile):
//   Calls ONLY std::ifstream + std::regex + fprintf. NO order functions, NO movement/attack/OPORD calls.
//   Verified by inspection: no warrior pointer, no MechWarrior type, no tac-order writes.

#include <vector>
#include <string>

class MechWarrior;   // forward decl — executeSpecialBody_Apply needs the warrior pointer

// ---------------------------------------------------------------------------
// TECHSCRIPT-SPECIAL-DISPATCH-1C: FSM TODO marker classification.
// These are surfaced from "; TODO: manual ABL line: <payload>" comments that
// the FitIniFile parser strips. They are INFORMATION ONLY — no execution, no state change.

enum class FsmTodoKind : uint8_t {
    STATE_DEF,   // state <name>;
    STATE_END,   // endstate;
    TRANS,       // trans <target>;
    TRANS_BACK,  // transBack;
    OTHER_TODO,  // anything else (variable decls, comments, etc.)
};

struct FsmTodoEntry {
    FsmTodoKind kind;
    std::string name;  // populated for STATE_DEF (state name) and TRANS (target name); empty otherwise
};

struct BrainSpecialBody {
    std::vector<std::string>   verbs;      // raw DO-verb tokens from BrainSpecial Body block
    bool                       loaded = false;

    // 1C: FSM TODO markers extracted from raw-text scan (populated when MC2_BRAIN_DISPATCH_FSM_TODO=1).
    // Max 64 entries enforced in scanFsmTodosFromFile.
    static constexpr int kMaxFsmTodos = 64;
    std::vector<FsmTodoEntry>  fsmTodos;
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

// TECHSCRIPT-SPECIAL-DISPATCH-1C: Raw-text scan of the specials file for FSM TODO markers.
// Gate: MC2_BRAIN_DISPATCH_FSM_TODO=1 (requires MC2_BRAIN_DISPATCH=1; callee warns if invoked without).
// Populates outBody.fsmTodos (capped at BrainSpecialBody::kMaxFsmTodos).
// Returns count of FSM TODO lines found (0 if file absent or no TODO lines).
// FORBIDDEN-CALL CONTRACT: calls ONLY std::ifstream + classify + fprintf. No order functions.
int scanFsmTodosFromFile(const char* missionName, BrainSpecialBody& outBody);
