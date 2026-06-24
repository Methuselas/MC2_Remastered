// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// Gate: MC2_BRAIN_DISPATCH (default OFF).
//
// FORBIDDEN-CALL GUARD (ENFORCED BY CONTRACT):
// executeSpecialBody_TraceOnly calls ONLY fprintf + loop.
// It does NOT call: setGeneralTacOrder, setPlayerTacOrder, setAlarmTacOrder,
// requestHelp, requestTarget, clearCurTacOrder, setMainGoal, calcTacOrder,
// coreMoveTo, or ANY movement/attack/order function.
// Verified: no such symbols appear anywhere in this translation unit.

#include "brain_special_dispatch.h"
#include <cstdio>
#include <cstring>
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
    for (int i = 0; kRecognizedVerbs[i] != nullptr; ++i) {
        if (std::strcmp(verb, kRecognizedVerbs[i]) == 0)
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
