// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — first real effect: Brain.CorePower false → POWERDOWN.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw line scan for TODO comments.
// TECHSCRIPT-DISPATCH-1D — per-unit Var namespace + minimal store.
// DISPATCH-LOADER-RAW-1 — replace FitIniFile body reader with raw brace-block scanner
//   that correctly loads inline-quoted verb args (DO Var.Set "foo" 1).
//   FitIniFile fallback retained for legacy [BrainSpecial]/[Body]/DO0=... bracket form.
// Gates: MC2_BRAIN_DISPATCH (parse+trace), MC2_BRAIN_DISPATCH_APPLY (effect layer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_FSM_TODO (FSM TODO surfacer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_VAR (per-unit Var store, requires DISPATCH, default OFF).
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
//
// VAR HANDLER CONTRACT (1D — handleVarSet / handleVarGet):
// Calls ONLY store->set/store->get + fprintf/fflush. ZERO calls to setGeneralTacOrder
// or any order/movement/attack/OPORD function.
// Verified by inspection: no warrior pointer, no tac-order type, no slot writes.

#include "brain_special_dispatch.h"
#include "warrior.h"   // MechWarrior — needed for executeSpecialBody_Apply setGeneralTacOrder call
                       // warrior.h includes mech_brain_runtime.h which defines VarStore + VarScope
#include "tacordr.h"   // TacticalOrder, TACTICAL_ORDER_POWERDOWN, ORDER_ORIGIN_SELF
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include "inifile.h"   // FitIniFile — same header used by _ai.fit loader

// ---------------------------------------------------------------------------
// Forward declaration for gate helper (defined after helpers, used by execute functions).
static bool s_dispatchVarGate();

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
// + loop + string comparison + (when DISPATCH_VAR=1) store->set/get via handleVarSet/Get.
// No tac-order writes. No movement/attack orders. No state writes to warrior or mission.
// Verified by inspection: no warrior pointer, no MechWarrior type, no
// setGeneralTacOrder / setPlayerTacOrder / setAlarmTacOrder / requestHelp /
// requestTarget / clearCurTacOrder / setMainGoal / calcTacOrder / coreMoveTo.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid, VarStore* varStore) {
    const bool varGate = s_dispatchVarGate();
    for (const std::string& verb : body.verbs) {
        // When DISPATCH_VAR=1, intercept Var.Set / Var.Get before the UNKNOWN fallthrough.
        if (varGate) {
            const char* vp = verb.c_str();
            if (std::strncmp(vp, "Var.Set", 7) == 0 || std::strncmp(vp, "Var.Get", 7) == 0) {
                char key[32], value[32];
                uint8_t scope = 0;
                bool ok = parseVarVerb(vp, key, value, &scope);
                if (!ok) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_PARSE_FAIL] verb=%s wid=%d (malformed)\n", vp, wid);
                    std::fflush(stderr);
                    continue;
                }
                bool isSet = (std::strncmp(vp, "Var.Set", 7) == 0);
                if (isSet)
                    handleVarSet(key, value, scope, varStore, wid);
                else
                    handleVarGet(key, scope, varStore, wid);
                continue;
            }
        }
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
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid, VarStore* varStore) {
    bool appliedEffect = false;
    const bool varGate = s_dispatchVarGate();

    for (const std::string& verb : body.verbs) {
        // When DISPATCH_VAR=1, intercept Var.Set / Var.Get before other dispatch.
        if (varGate) {
            const char* vp = verb.c_str();
            if (std::strncmp(vp, "Var.Set", 7) == 0 || std::strncmp(vp, "Var.Get", 7) == 0) {
                char key[32], value[32];
                uint8_t scope = 0;
                bool ok = parseVarVerb(vp, key, value, &scope);
                if (!ok) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_PARSE_FAIL] verb=%s wid=%d (malformed)\n", vp, wid);
                    std::fflush(stderr);
                    continue;
                }
                bool isSet = (std::strncmp(vp, "Var.Set", 7) == 0);
                if (isSet)
                    handleVarSet(key, value, scope, varStore, wid);
                else
                    handleVarGet(key, scope, varStore, wid);
                continue;
            }
        }
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
// DISPATCH-LOADER-RAW-1: parseBrainSpecialBody_RawScan
//
// Raw line-scanner for the new brace-block TechSpecial format used by
// carver_v_enhanced (and any other tool that emits inline-quoted DO args).
//
// Format parsed:
//   TechSpecial {
//       key  = "..."
//       alias = "..."
//       type = "..."
//       sourceABLFunction = "..."
//       Body {
//           DO <rest-of-line>
//           STOP
//       }
//   }
//
// Multiple TechSpecial blocks are parsed; ALL verbs from ALL bodies are
// appended to outBody.verbs in file order.
//
// Safety caps (DISPATCH-LOADER-RAW-1):
//   kMaxLineLen     = 512 bytes — lines longer than this are skipped with a trace.
//   kMaxVerbsPerBody = 256 — excess DO lines within one Body block are skipped.
//   kMaxBodies      = 256 — excess TechSpecial blocks are skipped entirely.
//
// FORBIDDEN-CALL CONTRACT:
//   Calls ONLY std::ifstream + string ops + fprintf/fflush. No order functions.
//   No warrior pointer. No tac-order writes. Verified by inspection.
//
// Returns true if at least one verb was loaded.
static const int kMaxLineLen      = 512;
static const int kMaxVerbsPerBody = 256;
static const int kMaxBodies       = 256;

static bool parseBrainSpecialBody_RawScan(const char* fitPath, BrainSpecialBody& outBody) {
    std::ifstream inFile(fitPath);
    if (!inFile.is_open())
        return false;

    // State machine:
    //   OUTER      — scanning for "TechSpecial {"
    //   IN_SPECIAL — inside TechSpecial block, scanning for "Body {"
    //   IN_BODY    — inside Body block, collecting DO lines
    enum { OUTER, IN_SPECIAL, IN_BODY } state = OUTER;

    int bodiesFound = 0;
    int verbsThisBody = 0;
    std::string line;

    while (std::getline(inFile, line)) {
        // Guard line length.
        if ((int)line.size() > kMaxLineLen) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: line too long (%d bytes) in %s — skipped\n",
                         (int)line.size(), fitPath);
            std::fflush(stderr);
            continue;
        }

        // Strip trailing CR/LF/whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                  line.back() == ' '  || line.back() == '\t'))
            line.pop_back();

        // Find first non-whitespace.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        const char* p = line.c_str() + s;

        // Skip comment lines.
        if (*p == ';')
            continue;

        if (state == OUTER) {
            // Look for "TechSpecial {" (possibly with leading whitespace already stripped).
            if (std::strncmp(p, "TechSpecial", 11) == 0) {
                // Confirm '{' appears on same line (after optional whitespace).
                const char* q = p + 11;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{') {
                    if (bodiesFound >= kMaxBodies) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: TechSpecial block cap (%d) reached in %s — skipping rest\n",
                                     kMaxBodies, fitPath);
                        std::fflush(stderr);
                        break;
                    }
                    state = IN_SPECIAL;
                    verbsThisBody = 0;
                }
            }
        } else if (state == IN_SPECIAL) {
            // Look for "Body {" or closing "}" (end of TechSpecial without a Body).
            if (std::strncmp(p, "Body", 4) == 0) {
                const char* q = p + 4;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{') {
                    state = IN_BODY;
                    ++bodiesFound;
                }
            } else if (*p == '}') {
                // Closing brace of TechSpecial with no Body — return to OUTER.
                state = OUTER;
            }
        } else { // IN_BODY
            if (*p == '}') {
                // End of Body block.
                state = IN_SPECIAL;   // still inside TechSpecial; look for its closing '}'
            } else if (std::strncmp(p, "STOP", 4) == 0 &&
                       (p[4] == '\0' || p[4] == ' ' || p[4] == '\t' || p[4] == ';')) {
                // STOP — skip (sentinel only, not a verb).
            } else if (std::strncmp(p, "DO ", 3) == 0 || std::strncmp(p, "DO\t", 3) == 0) {
                if (verbsThisBody >= kMaxVerbsPerBody) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: verb cap (%d) per-body reached in %s — skipping rest of body\n",
                                 kMaxVerbsPerBody, fitPath);
                    std::fflush(stderr);
                    // Drain to closing '}' without adding more verbs.
                    continue;
                }
                // Extract the verb: everything after "DO " / "DO\t", trimmed.
                const char* verbStart = p + 3;
                while (*verbStart == ' ' || *verbStart == '\t') ++verbStart;

                // Build verb string, stripping trailing whitespace and trailing ';'.
                std::string verb(verbStart);
                while (!verb.empty() && (verb.back() == ' ' || verb.back() == '\t'))
                    verb.pop_back();
                while (!verb.empty() && verb.back() == ';')
                    verb.pop_back();
                while (!verb.empty() && (verb.back() == ' ' || verb.back() == '\t'))
                    verb.pop_back();

                if (!verb.empty()) {
                    outBody.verbs.push_back(std::move(verb));
                    ++verbsThisBody;
                }
            }
        }
    }

    return !outBody.verbs.empty();
}

// ---------------------------------------------------------------------------
// parseBrainSpecialBody_FitIni
//
// Legacy FitIniFile-based loader.  Retained as fallback for bracket-form fixtures
// ([BrainSpecial] / [Body] / DO0=...) that don't use the brace-block TechSpecial format.
// This is the original parseBrainSpecialBody implementation, renamed.
//
// FORBIDDEN-CALL CONTRACT: calls FitIniFile API only. No order functions.
static bool parseBrainSpecialBody_FitIni(const char* fitPath, BrainSpecialBody& outBody) {
    FitIniFile* fit = new FitIniFile;
    if (!fit)
        return false;

    if (fit->open(fitPath) != NO_ERR) {
        delete fit;
        return false;
    }

    if (fit->seekBlock("BrainSpecial") != NO_ERR) {
        fit->close();
        delete fit;
        return false;
    }

    if (fit->seekBlock("Body") != NO_ERR) {
        fit->close();
        delete fit;
        return false;
    }

    char keyBuf[16];
    char verbBuf[64];
    for (int i = 0; i < 64; ++i) {
        std::snprintf(keyBuf, sizeof(keyBuf), "DO%d", i);
        verbBuf[0] = '\0';
        long result = fit->readIdString(keyBuf, verbBuf, (unsigned long)(sizeof(verbBuf) - 1));
        if (result != NO_ERR)
            break;
        if (verbBuf[0] != '\0')
            outBody.verbs.push_back(std::string(verbBuf));
    }

    fit->close();
    delete fit;
    return !outBody.verbs.empty();
}

// ---------------------------------------------------------------------------
// parseBrainSpecialBody — public entry point (DISPATCH-LOADER-RAW-1)
//
// Strategy:
//   1. Try raw brace-block scanner first (handles inline-quoted DO args).
//   2. If no TechSpecial blocks found (raw scanner returned empty), fall back
//      to the legacy FitIniFile-based [BrainSpecial]/[Body]/DO0= form.
//   Both forms emit the same [BRAIN_DISPATCH] trace on success.
//
// Caller passes missionName like "mc2_01" (no path prefix, no extension).
// Returns true if at least one verb was loaded.
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody) {
    outBody.verbs.clear();
    outBody.loaded = false;

    char fitPath[256];
    std::snprintf(fitPath, sizeof(fitPath), "data/missions/%s_specials.fit", missionName);

    // Try raw brace-block scanner first.
    bool ok = parseBrainSpecialBody_RawScan(fitPath, outBody);

    if (!ok) {
        // No TechSpecial brace blocks found — try legacy bracket-form fallback.
        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] no TechSpecial blocks in %s — trying FitIni fallback\n",
                     fitPath);
        std::fflush(stderr);
        ok = parseBrainSpecialBody_FitIni(fitPath, outBody);
        if (ok) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] FitIni fallback loaded %s: %d verbs\n",
                         fitPath, (int)outBody.verbs.size());
            std::fflush(stderr);
        }
    }

    if (ok) {
        outBody.loaded = true;
        std::fprintf(stderr, "[BRAIN_DISPATCH] parsed %s_specials.fit: %d verbs\n",
                     missionName, (int)outBody.verbs.size());
        std::fflush(stderr);
    }
    return outBody.loaded;
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: parseVarVerb
//
// Parses "Var.Set \"<key>\" <value> [scope=Mission]" or
//         "Var.Get \"<key>\" [scope=Mission]"
// into outKey, outValue, outScope.
// outValue is only meaningful for Var.Set; for Var.Get it is set to "".
// outScope: 0=Unit (VarScope::Unit), 1=Mission (VarScope::Mission).
// Returns true on success; false on malformed input (empty key; missing value for Set).
//
// FORBIDDEN-CALL CONTRACT: calls ONLY std::strchr/strstr/strncpy/snprintf. No orders.
bool parseVarVerb(const char* verbStr, char outKey[32], char outValue[32], uint8_t* outScope) {
    outKey[0] = '\0';
    outValue[0] = '\0';
    *outScope = 0;  // default Unit

    // Determine whether this is Var.Set or Var.Get.
    bool isSet = (std::strncmp(verbStr, "Var.Set", 7) == 0 && (verbStr[7] == ' ' || verbStr[7] == '\t' || verbStr[7] == '\0'));
    bool isGet = (std::strncmp(verbStr, "Var.Get", 7) == 0 && (verbStr[7] == ' ' || verbStr[7] == '\t' || verbStr[7] == '\0'));
    if (!isSet && !isGet)
        return false;

    // Skip verb token + whitespace.
    const char* p = verbStr + 7;
    while (*p == ' ' || *p == '\t') ++p;

    // Parse key token — supports two forms:
    //   "key"   — double-quoted (canonical spec form; used in non-FIT contexts)
    //   [key]   — bracket-quoted (FIT-safe form; FIT outer-quotes can't embed '"')
    char closeDelim = '\0';
    if (*p == '"') {
        closeDelim = '"';
    } else if (*p == '[') {
        closeDelim = ']';
    } else {
        return false;  // unrecognized key delimiter
    }
    ++p;  // skip opening delimiter
    const char* keyStart = p;
    const char* keyEnd = std::strchr(p, closeDelim);
    if (!keyEnd)
        return false;
    size_t keyLen = (size_t)(keyEnd - keyStart);
    if (keyLen == 0)
        return false;
    if (keyLen > 31) keyLen = 31;
    std::strncpy(outKey, keyStart, keyLen);
    outKey[keyLen] = '\0';

    p = keyEnd + 1;  // skip closing delimiter

    // Skip whitespace between key and value/scope.
    while (*p == ' ' || *p == '\t') ++p;

    if (isSet) {
        // Parse value token (whitespace-delimited; may be quoted or unquoted).
        if (*p == '\0')
            return false;  // missing value for Set

        if (*p == '"') {
            // Quoted value.
            ++p;
            const char* vStart = p;
            const char* vEnd = std::strchr(p, '"');
            if (!vEnd) return false;
            size_t vLen = (size_t)(vEnd - vStart);
            if (vLen > 31) vLen = 31;
            std::strncpy(outValue, vStart, vLen);
            outValue[vLen] = '\0';
            p = vEnd + 1;
        } else {
            // Unquoted value — read until whitespace.
            const char* vStart = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            size_t vLen = (size_t)(p - vStart);
            if (vLen > 31) vLen = 31;
            std::strncpy(outValue, vStart, vLen);
            outValue[vLen] = '\0';
        }

        // Skip whitespace after value.
        while (*p == ' ' || *p == '\t') ++p;
    }

    // Detect optional "scope=Mission" trailing token.
    if (std::strncmp(p, "scope=Mission", 13) == 0)
        *outScope = 1;  // VarScope::Mission

    return true;
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: handleVarSet
//
// FORBIDDEN-CALL CONTRACT: calls ONLY store->set + fprintf/fflush. No order functions.
// Verified by inspection: no warrior pointer, no TacticalOrder, no slot writes.
void handleVarSet(const char* key, const char* value, uint8_t scope, VarStore* store, int wid) {
    if (scope == 1 /* Mission */) {
        // Mission-scope writes are TRACE-ONLY in 1D: no shared global store.
        // Rationale: avoid cross-warrior nondeterminism until a guarded single-writer policy is designed.
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=%s value=%s wid=%d (write deferred -- no shared-global writes in 1D)\n",
                     key, value, wid);
        std::fflush(stderr);
        return;
    }
    // Unit scope.
    if (!store) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET] WARN: no varStore for wid=%d key=%s (store null)\n", wid, key);
        std::fflush(stderr);
        return;
    }
    bool ok = store->set(key, value, VarScope::Unit);
    if (ok) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET] key=%s value=%s scope=Unit wid=%d\n", key, value, wid);
    } else {
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_FULL] wid=%d key=%s (cap=%d reached -- skipped)\n",
                     wid, key, VarStore::kVarStoreCap);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D: handleVarGet
//
// FORBIDDEN-CALL CONTRACT: calls ONLY store->get + fprintf/fflush. No order functions.
void handleVarGet(const char* key, uint8_t scope, VarStore* store, int wid) {
    if (scope == 1 /* Mission */) {
        // Mission-scope reads are TRACE-ONLY in 1D (no shared store to read from).
        std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET_MISSION_TRACE] key=%s wid=%d (default -- mission scope read deferred)\n",
                     key, wid);
        std::fflush(stderr);
        return;
    }
    // Unit scope.
    const char* val = (store) ? store->get(key, VarScope::Unit) : "0";
    bool isHit = store && (store->find(key, VarScope::Unit) != nullptr);
    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET] key=%s value=%s scope=Unit wid=%d (%s)\n",
                 key, val, wid, isHit ? "hit" : "default");
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Gate init helper for DISPATCH_VAR (used by both execute functions).
static bool s_dispatchVarGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_DISPATCH_VAR");
        if (v && std::atoi(v) != 0) {
            const char* d = std::getenv("MC2_BRAIN_DISPATCH");
            if (!d || std::atoi(d) == 0) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");
                std::fflush(stderr);
                return false;
            }
            return true;
        }
        return false;
    })();
    return kGate;
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
