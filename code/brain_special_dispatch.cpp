// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — first real effect: Brain.CorePower false → POWERDOWN.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw line scan for TODO comments.
// Gates: MC2_BRAIN_DISPATCH (parse+trace), MC2_BRAIN_DISPATCH_APPLY (effect layer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_FSM_TODO (FSM TODO surfacer, requires DISPATCH).
//
// FORBIDDEN-CALL GUARD — executeSpecialBody_TraceOnly (1A, UNCHANGED):
// Calls ONLY fprintf + loop. DOES NOT call setGeneralTacOrder or any order/movement function.
// Verified: no warrior pointer, no MechWarrior type exists in this TU.
//
// RELAXED-CALL GUARD — executeSpecialBody_Apply (1B):
// The ONLY permitted order call is warrior->setGeneralTacOrder() (for Brain.CorePower false → POWERDOWN).
// STILL FORBIDDEN: setPlayerTacOrder, setAlarmTacOrder, requestHelp, requestTarget,
// calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder, any movement/attack/OPORD-advance/
// commander function. All other verbs → trace only, zero effect.
//
// FSM-TODO SCANNER (1C — scanFsmTodosFromFile):
// Calls ONLY std::ifstream + std::regex + fprintf. NO order functions, NO movement/attack/OPORD calls.
// Verified by inspection: no warrior pointer, no MechWarrior type, no tac-order writes.
// This is INFORMATION ONLY: it surfaces what FSM logic the auto-conversion dropped,
// giving the modder an inventory of state machine structure that needs hand-porting.

#include "brain_special_dispatch.h"
#include "warrior.h"   // MechWarrior — needed for executeSpecialBody_Apply setGeneralTacOrder call
#include "tacordr.h"   // TacticalOrder, TACTICAL_ORDER_POWERDOWN, ORDER_ORIGIN_SELF
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include "inifile.h"   // FitIniFile — same header used by _ai.fit loader

// ---------------------------------------------------------------------------
// Recognized verb table for DISPATCH-1A.
// All other verbs produce [BRAIN_DISPATCH_UNKNOWN] trace.
static const char* const kRecognizedVerbs[] = {
    "Brain.CorePower",
    "Brain.CoreAttack",
    "OPORD.CoreGuard",
    "OPORD.CorePatrol",
    "OPORD.CoreMoveTo",
    "Unit.Retreat",
    "HOLD",
    nullptr  // sentinel
};

static bool isRecognizedVerb(const char* verb) {
    // Verbs are stored as full "verb arg..." tokens; match only the verb part
    // (up to the first whitespace) so "Brain.CorePower false" recognizes as
    // "Brain.CorePower" rather than falling through to UNKNOWN.
    for (int i = 0; kRecognizedVerbs[i] != nullptr; ++i) {
        size_t n = std::strlen(kRecognizedVerbs[i]);
        if (std::strncmp(verb, kRecognizedVerbs[i], n) == 0 &&
            (verb[n] == '\0' || verb[n] == ' ' || verb[n] == '\t'))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// executeSpecialBody_TraceOnly
//
// FORBIDDEN-CALL CONTRACT: this function body contains ONLY fprintf + fflush
// + loop + string comparison. No tac-order writes. No movement/attack orders.
// No state writes to any warrior or mission object.
// Verified by inspection: no warrior pointer, no MechWarrior type, no
// setGeneralTacOrder / setPlayerTacOrder / setAlarmTacOrder / requestHelp /
// requestTarget / clearCurTacOrder / setMainGoal / calcTacOrder / coreMoveTo.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid) {
    for (const std::string& verb : body.verbs) {
        if (isRecognizedVerb(verb.c_str())) {
            std::fprintf(stderr, "[BRAIN_DISPATCH] verb=%s wid=%d\n", verb.c_str(), wid);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] verb=%s wid=%d\n", verb.c_str(), wid);
        }
        std::fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// bodyHasPowerdown
// Returns true if the body contains the Brain.CorePower false verb token.
bool bodyHasPowerdown(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        if (verb == "Brain.CorePower false")
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// executeSpecialBody_Apply
//
// 1B EFFECT DISPATCHER — gate MC2_BRAIN_DISPATCH_APPLY=1 (requires MC2_BRAIN_DISPATCH=1).
//
// RELAXED-CALL GUARD CONTRACT:
//   The ONLY order function called here is warrior->setGeneralTacOrder().
//   Called for exactly ONE verb: "Brain.CorePower false" → TACTICAL_ORDER_POWERDOWN.
//   All other verbs produce [BRAIN_DISPATCH] or [BRAIN_DISPATCH_UNKNOWN] trace only.
//   FORBIDDEN in this function: setPlayerTacOrder, setAlarmTacOrder, requestHelp,
//   requestTarget, calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder,
//   any movement/attack/OPORD-advance/commander function.
//   Verified by inspection: only one setGeneralTacOrder call below; no other order calls.
//
// Returns true if the Brain.CorePower false POWERDOWN effect was applied.
// Caller uses the return value to suppress the synthetic HOLD_TASK (one GENERAL-slot write per tick).
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid) {
    bool appliedEffect = false;

    for (const std::string& verb : body.verbs) {
        if (verb == "Brain.CorePower false") {
            // ONLY permitted order call in this function (RELAXED-CALL GUARD).
            TacticalOrder pdOrder;
            pdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
            warrior->setGeneralTacOrder(pdOrder);
            std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=%d\n", wid);
            std::fflush(stderr);
            appliedEffect = true;
        } else if (isRecognizedVerb(verb.c_str())) {
            // Recognized but no effect implemented this slice — trace only.
            std::fprintf(stderr, "[BRAIN_DISPATCH] verb=%s wid=%d (apply-mode: no effect this verb)\n", verb.c_str(), wid);
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] verb=%s wid=%d\n", verb.c_str(), wid);
            std::fflush(stderr);
        }
    }

    return appliedEffect;
}

// ---------------------------------------------------------------------------
// parseBrainSpecialBody
//
// Opens data/missions/<missionName>_specials.fit, seeks [BrainSpecial] block,
// seeks [Body] sub-block, reads DO0..DON verb strings.
// Returns true if at least one verb was loaded; false if file/block absent.
// Caller passes missionName like "mc2_01" (no path prefix, no extension).
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody) {
    outBody.verbs.clear();
    outBody.loaded = false;

    char fitPath[256];
    std::snprintf(fitPath, sizeof(fitPath), "data/missions/%s_specials.fit", missionName);

    FitIniFile* fit = new FitIniFile;
    if (!fit) {
        return false;
    }

    if (fit->open(fitPath) != NO_ERR) {
        delete fit;
        return false;
    }

    // Seek BrainSpecial block.
    if (fit->seekBlock("BrainSpecial") != NO_ERR) {
        fit->close();
        delete fit;
        return false;
    }

    // Seek Body sub-block inside BrainSpecial.
    if (fit->seekBlock("Body") != NO_ERR) {
        fit->close();
        delete fit;
        return false;
    }

    // Read DO0, DO1, ... until readIdString returns non-NO_ERR.
    char keyBuf[16];
    char verbBuf[64];
    for (int i = 0; i < 64; ++i) {
        std::snprintf(keyBuf, sizeof(keyBuf), "DO%d", i);
        verbBuf[0] = '\0';
        long result = fit->readIdString(keyBuf, verbBuf, (unsigned long)(sizeof(verbBuf) - 1));
        if (result != NO_ERR) {
            break;
        }
        if (verbBuf[0] != '\0') {
            outBody.verbs.push_back(std::string(verbBuf));
        }
    }

    fit->close();
    delete fit;

    if (!outBody.verbs.empty()) {
        outBody.loaded = true;
        std::fprintf(stderr, "[BRAIN_DISPATCH] parsed %s_specials.fit: %d verbs\n",
                     missionName, (int)outBody.verbs.size());
        std::fflush(stderr);
    }
    return outBody.loaded;
}

// ---------------------------------------------------------------------------
// scanFsmTodosFromFile — TECHSCRIPT-SPECIAL-DISPATCH-1C
//
// FORBIDDEN-CALL CONTRACT:
//   This function calls ONLY std::ifstream + std::regex (classify) + fprintf/fflush.
//   It does NOT call setGeneralTacOrder, setPlayerTacOrder, setAlarmTacOrder,
//   requestHelp, requestTarget, clearCurTacOrder, setMainGoal, calcTacOrder,
//   coreMoveTo, or ANY movement/attack/order function.
//   No warrior pointer. No MechWarrior type. No tac-order writes.
//   Verified by inspection.
//
// Context: auto-converted mission_specials.fit files produced by the modder's tool
// contain ABL state machine constructs ONLY as FIT comments of the form:
//   "; TODO: manual ABL line: <text>"
// FitIniFile strips comments, so parseBrainSpecialBody never sees them.
// This function does a second raw-text pass to surface those markers as structured
// trace data, giving the modder an inventory of dropped FSM logic to hand-port.
//
// Classifier rules (applied to <payload> after stripping the TODO prefix):
//   "^\s*state\s+(\w+);"  → STATE_DEF(name)
//   "^\s*endstate\s*;?"   → STATE_END
//   "^\s*trans\s+(\w+);"  → TRANS(target)
//   "^\s*transBack\s*;"   → TRANS_BACK
//   else                  → OTHER_TODO (variable decls, misc — counted but not detail-traced)
int scanFsmTodosFromFile(const char* missionName, BrainSpecialBody& outBody) {
    outBody.fsmTodos.clear();

    char fitPath[256];
    std::snprintf(fitPath, sizeof(fitPath), "data/missions/%s_specials.fit", missionName);

    std::ifstream inFile(fitPath);
    if (!inFile.is_open()) {
        return 0;
    }

    // Regex to match: optional-whitespace ; optional-whitespace TODO: optional-whitespace
    // "manual ABL line:" optional-whitespace <payload>
    // We use a simple string search rather than a full regex for the outer match
    // to avoid locale/platform regex engine differences in the prefix, then classify
    // the payload with targeted regexes.
    static const std::regex reStateDef(R"(^\s*state\s+(\w+)\s*;)",
                                        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reStateEnd(R"(^\s*endstate\s*;?)",
                                        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reTrans   (R"(^\s*trans\s+(\w+)\s*;)",
                                        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reTransBack(R"(^\s*transBack\s*;)",
                                        std::regex::ECMAScript | std::regex::optimize);

    // Needle we look for (case-sensitive, exactly as produced by the converter tool).
    static const char kTodoPrefix[] = "; TODO: manual ABL line:";
    const size_t kTodoPrefixLen = sizeof(kTodoPrefix) - 1;

    int found = 0;
    std::string line;
    while (std::getline(inFile, line)) {
        // Trim leading whitespace to find the semicolon prefix.
        size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;

        // Check for "; TODO: manual ABL line:" prefix.
        if (line.size() - pos < kTodoPrefixLen)
            continue;
        if (std::strncmp(line.c_str() + pos, kTodoPrefix, kTodoPrefixLen) != 0)
            continue;

        // Extract payload (everything after the prefix, trimmed).
        std::string payload = line.substr(pos + kTodoPrefixLen);
        // Trim leading whitespace from payload.
        size_t pstart = 0;
        while (pstart < payload.size() && (payload[pstart] == ' ' || payload[pstart] == '\t'))
            ++pstart;
        payload = payload.substr(pstart);
        // Trim trailing whitespace/CR.
        while (!payload.empty() && (payload.back() == '\r' || payload.back() == '\n' ||
                                    payload.back() == ' '  || payload.back() == '\t'))
            payload.pop_back();

        if (payload.empty())
            continue;

        // Cap at kMaxFsmTodos.
        if (found >= BrainSpecialBody::kMaxFsmTodos) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_FSM_TODO] WARN: FSM TODO cap (%d) reached in %s_specials.fit — truncating\n",
                         BrainSpecialBody::kMaxFsmTodos, missionName);
            std::fflush(stderr);
            break;
        }

        // Classify payload.
        FsmTodoEntry entry;
        std::smatch m;
        if (std::regex_search(payload, m, reStateDef)) {
            entry.kind = FsmTodoKind::STATE_DEF;
            entry.name = m[1].str();
        } else if (std::regex_search(payload, m, reStateEnd)) {
            entry.kind = FsmTodoKind::STATE_END;
        } else if (std::regex_search(payload, m, reTrans)) {
            entry.kind = FsmTodoKind::TRANS;
            entry.name = m[1].str();
        } else if (std::regex_search(payload, m, reTransBack)) {
            entry.kind = FsmTodoKind::TRANS_BACK;
        } else {
            entry.kind = FsmTodoKind::OTHER_TODO;
            entry.name = payload;  // store full payload for OTHER_TODO (for potential future use)
        }

        outBody.fsmTodos.push_back(std::move(entry));
        ++found;
    }

    return found;
}
