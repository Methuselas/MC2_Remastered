#pragma once
// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — adds MC2_BRAIN_DISPATCH_APPLY gate for first effect.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw-scan specials.fit for
//   "; TODO: manual ABL line: <payload>" comments (stripped by FitIniFile) and emit
//   structured [BRAIN_DISPATCH_FSM_TODO] traces (information only, no behavior change).
// TECHSCRIPT-DISPATCH-1D — per-unit Var namespace + minimal store.
//   Gate: MC2_BRAIN_DISPATCH_VAR=1 (requires MC2_BRAIN_DISPATCH=1; default OFF).
//   Var.Set "<key>" <value> [scope=Mission]  — stores per-warrior or traces mission-scope.
//   Var.Get "<key>" [scope=Mission]          — reads per-warrior store, traces result.
//   Mission-scope writes are TRACE-ONLY in 1D (no shared store; nondeterminism deferred).
//   Gate-OFF: Var.* fall through to [BRAIN_DISPATCH_UNKNOWN] as before — byte-identical.
//
// Gate ladder:
//   MC2_BRAIN_DISPATCH=1                          → parse + trace ONLY (1A behavior, byte-identical)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_APPLY=1 → allow the ONE first effect (Brain.CorePower false → POWERDOWN)
//   MC2_BRAIN_DISPATCH_APPLY=1 alone              → warns + is inert (DISPATCH required)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_FSM_TODO=1 → additionally surface FSM TODO markers
//   MC2_BRAIN_DISPATCH_FSM_TODO=1 alone           → warns + is inert (DISPATCH required)
//   MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_VAR=1 → Var.Set/Get handled; per-unit store active
//   MC2_BRAIN_DISPATCH_VAR=1 alone                → warns + is inert (DISPATCH required)
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
#include <cstring>   // strncmp/strncpy used in VarStore inline methods (pulled via mech_brain_runtime.h too)

class MechWarrior;   // forward decl — executeSpecialBody_Apply needs the warrior pointer
struct VarStore;     // forward decl — Var.Set/Get handlers take a VarStore* (defined in mech_brain_runtime.h)

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
// varStore: pass &brainRuntime->varStore when DISPATCH_VAR=1; nullptr is safe (gate-off path).
// FORBIDDEN-CALL CONTRACT: calls ONLY fprintf + fflush + loop + store->set/get.
//   No setGeneralTacOrder, no order/movement/attack function.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid, VarStore* varStore = nullptr);

// 1B EFFECT DISPATCHER.
// Applies the ONE recognized effect when MC2_BRAIN_DISPATCH_APPLY=1:
//   Brain.CorePower false → warrior->setGeneralTacOrder(TACTICAL_ORDER_POWERDOWN)
// All other verbs: trace only, no effect (Var.* handled when DISPATCH_VAR=1).
// warrior must be non-null (caller guards).
// varStore: pass &brainRuntime->varStore when DISPATCH_VAR=1; nullptr is safe.
// Returns true if a recognized effect was applied (caller uses to suppress synthetic HOLD).
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid, VarStore* varStore = nullptr);

// DISPATCH-LOADER-RAW-1: Parse BrainSpecial/TechSpecial Body DO-verbs.
// Primary: raw brace-block scanner (handles inline-quoted DO args).
// Fallback: legacy FitIniFile [BrainSpecial]/[Body]/DO0=... bracket form.
// Returns false if file absent or no verbs found.
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody);

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: Var.Set / Var.Get parser + handler.
//
// parseVarVerb — minimal tokenizer for "Var.Set/Var.Get" verb strings.
//   verbStr  — the full DO-verb string (e.g. `Var.Set "foo" 1 scope=Mission`).
//   outKey   — output: key string (up to 31 chars + '\0').
//   outValue — output (Set only; unused for Get): value token (up to 31 chars + '\0').
//   outScope — output: VarScope::Unit (default) or VarScope::Mission.
//   Returns true and populates outputs on success.
//   Returns false (soft-fail) on malformed input (empty key, missing value for Set).
//
// handleVarSet / handleVarGet — called from executeSpecialBody_TraceOnly +
//   executeSpecialBody_Apply when MC2_BRAIN_DISPATCH_VAR=1.
//   FORBIDDEN-CALL CONTRACT (VAR handlers): call ONLY store->set/get + fprintf/fflush.
//   ZERO calls to setGeneralTacOrder or any order/movement/attack function.
//   Verified by inspection: no warrior pointer, no tac-order type.
//
// NOTE: VarScope enum is defined in mech_brain_runtime.h (included after this header by callers).
// To avoid a circular include, handleVarSet/Get take VarScope as uint8_t cast internally.
// The free function prototypes use a forward-declared uint8_t scope parameter convention;
// see brain_special_dispatch.cpp for the actual VarScope-aware implementation.

bool parseVarVerb(const char* verbStr, char outKey[32], char outValue[32], uint8_t* outScope);

// Handles "Var.Set" when DISPATCH_VAR=1.
// store may be nullptr if gate-off or runtime unavailable — soft-fails gracefully.
void handleVarSet(const char* key, const char* value, uint8_t scope, VarStore* store, int wid);

// Handles "Var.Get" when DISPATCH_VAR=1.
void handleVarGet(const char* key, uint8_t scope, VarStore* store, int wid);

// TECHSCRIPT-SPECIAL-DISPATCH-1C: Raw-text scan of the specials file for FSM TODO markers.
// Gate: MC2_BRAIN_DISPATCH_FSM_TODO=1 (requires MC2_BRAIN_DISPATCH=1; callee warns if invoked without).
// Populates outBody.fsmTodos (capped at BrainSpecialBody::kMaxFsmTodos).
// Returns count of FSM TODO lines found (0 if file absent or no TODO lines).
// FORBIDDEN-CALL CONTRACT: calls ONLY std::ifstream + classify + fprintf. No order functions.
int scanFsmTodosFromFile(const char* missionName, BrainSpecialBody& outBody);
