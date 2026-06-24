// TECHSCRIPT-SPECIAL-DISPATCH-1A — trace-only dispatcher shell.
// TECHSCRIPT-SPECIAL-DISPATCH-1B — first real effect: Brain.CorePower false → POWERDOWN.
// TECHSCRIPT-SPECIAL-DISPATCH-1C — FSM-TODO surfacer: raw line scan for TODO comments.
// TECHSCRIPT-DISPATCH-1D — per-unit Var namespace + minimal store.
// DISPATCH-LOADER-RAW-1 — replace FitIniFile body reader with raw brace-block scanner
//   that correctly loads inline-quoted verb args (DO Var.Set "foo" 1).
//   FitIniFile fallback retained for legacy [BrainSpecial]/[Body]/DO0=... bracket form.
// TECHSCRIPT-CALL-CHAIN-1A — TechSpecial.Call chaining (parse + index + trace ONLY).
//   Gate: MC2_BRAIN_DISPATCH_CALL=1 (requires MC2_BRAIN_DISPATCH=1; warns + inert otherwise).
//   Index: parseBrainSpecialBody_RawScan populates a SpecialIndex (key→BrainSpecialBody*).
//   Dispatch: TechSpecial.Call "<key>" verb recurses into the named body (trace only).
//   Depth limit: 8 (kCallChainMaxDepth). Per-tick visited set (std::vector<std::string>).
//   NO effects in chained bodies: Brain.CorePower false in a CALLED body traces but does NOT
//   call setGeneralTacOrder. Chained-effect execution is deferred to CALL-CHAIN-1B.
//   FORBIDDEN-CALL CONTRACT (chained dispatch): pure trace + recursion. No order functions.
//   PURE VERB-STREAM COMPOSITION: chaining introduces NO new order function calls.
//
// Gates: MC2_BRAIN_DISPATCH (parse+trace), MC2_BRAIN_DISPATCH_APPLY (effect layer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_FSM_TODO (FSM TODO surfacer, requires DISPATCH),
//        MC2_BRAIN_DISPATCH_VAR (per-unit Var store, requires DISPATCH, default OFF),
//        MC2_BRAIN_DISPATCH_CALL (call chaining, requires DISPATCH, default OFF).
//
// FORBIDDEN-CALL GUARD — executeSpecialBody_TraceOnly / executeSpecialBody_TraceOnlyChained (1A):
// Calls ONLY fprintf + loop + recursive chained dispatch. DOES NOT call setGeneralTacOrder
// or any order/movement function. Verified: no warrior pointer, no MechWarrior type here.
//
// RELAXED-CALL GUARD — executeSpecialBody_Apply (1B):
// The ONLY permitted order call is warrior->setGeneralTacOrder() (for Brain.CorePower false → POWERDOWN)
// IN THE ROOT BODY ONLY. Chained bodies (via TechSpecial.Call) are trace-only in 1A.
// STILL FORBIDDEN: setPlayerTacOrder, setAlarmTacOrder, requestHelp, requestTarget,
// calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder, any movement/attack/OPORD-advance/
// commander function. All other verbs → trace only, zero effect.
//
// FSM-TODO SCANNER (1C — scanFsmTodosFromFile):
// Calls ONLY std::ifstream + std::regex + fprintf. NO order functions, NO movement/attack/OPORD calls.
// Verified by inspection: no warrior pointer, no MechWarrior type, no tac-order writes.
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
// TECHSCRIPT-CALL-CHAIN-1A: depth limit for TechSpecial.Call recursion.
// Corpus max observed depth = 2; limit set to 8 per recon recommendation.
static constexpr int kCallChainMaxDepth = 8;

// specialIndexFind — linear-scan lookup by key (case-sensitive).
// Returns pointer into idx for the found entry, or nullptr.
const SpecialIndexEntry* specialIndexFind(const SpecialIndex& idx, const std::string& key) {
    for (const SpecialIndexEntry& e : idx) {
        if (e.key == key)
            return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Gate helper for DISPATCH_CALL (TECHSCRIPT-CALL-CHAIN-1A).
static bool s_dispatchCallGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_DISPATCH_CALL");
        if (v && std::atoi(v) != 0) {
            const char* d = std::getenv("MC2_BRAIN_DISPATCH");
            if (!d || std::atoi(d) == 0) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] WARNING: MC2_BRAIN_DISPATCH_CALL=1 requires MC2_BRAIN_DISPATCH=1 — call chaining is INERT\n");
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
// parseCallVerbKey — parse the quoted key from a TechSpecial.Call verb string.
// Supports:  TechSpecial.Call "key"   (double-quoted)
//            TechSpecial.Call [key]   (bracket-quoted, FIT-safe)
// outKey must be at least 128 chars.
// Returns true on success, false on malformed input (missing delimiter, empty key).
static bool parseCallVerbKey(const char* verbStr, char outKey[128]) {
    outKey[0] = '\0';
    // Skip "TechSpecial.Call" prefix.
    static const char kPrefix[] = "TechSpecial.Call";
    const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (std::strncmp(verbStr, kPrefix, kPrefixLen) != 0)
        return false;
    const char* p = verbStr + kPrefixLen;
    // Skip whitespace.
    while (*p == ' ' || *p == '\t') ++p;
    // Detect delimiter.
    char closeDelim = '\0';
    if (*p == '"')       { closeDelim = '"'; }
    else if (*p == '[')  { closeDelim = ']'; }
    else                  return false;
    ++p; // skip open delimiter
    const char* keyStart = p;
    const char* keyEnd   = std::strchr(p, closeDelim);
    if (!keyEnd) return false;
    size_t keyLen = (size_t)(keyEnd - keyStart);
    if (keyLen == 0) return false;
    if (keyLen > 127) keyLen = 127;
    std::strncpy(outKey, keyStart, keyLen);
    outKey[keyLen] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// executeSpecialBody_TraceOnlyChained
//
// TECHSCRIPT-CALL-CHAIN-1A: recursive verb-stream trace for called bodies.
//
// FORBIDDEN-CALL CONTRACT:
//   This function calls ONLY fprintf + fflush + loop + recursive calls to itself.
//   NO setGeneralTacOrder, NO order/movement/attack function.
//   NO effects executed on chained bodies (1A: trace only; 1B will add effect dispatch).
//   Brain.CorePower false in a CALLED body is traced but does NOT call setGeneralTacOrder.
//   This is PURE VERB-STREAM COMPOSITION.
//   Verified by inspection: no warrior pointer, no MechWarrior type, no TacticalOrder.
void executeSpecialBody_TraceOnlyChained(const BrainSpecialBody& body,
                                          int wid,
                                          const SpecialIndex& index,
                                          int depth,
                                          std::vector<std::string>& visited,
                                          const char* fromKey,
                                          VarStore* varStore)
{
    // Trace each verb in the called body with depth marker.
    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();

        // Handle nested TechSpecial.Call within a called body.
        if (std::strncmp(vp, "TechSpecial.Call", 16) == 0 &&
            (vp[16] == ' ' || vp[16] == '\t' || vp[16] == '\0')) {
            char callKey[128];
            if (!parseCallVerbKey(vp, callKey)) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=<parse-fail> depth=%d wid=%d\n",
                             fromKey, depth, wid);
                std::fflush(stderr);
                continue;
            }
            // Depth limit check.
            if (depth >= kCallChainMaxDepth) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_DEPTH] depth=%d max=%d wid=%d\n",
                             depth, kCallChainMaxDepth, wid);
                std::fflush(stderr);
                continue;
            }
            // Cycle guard.
            for (const std::string& vis : visited) {
                if (vis == callKey) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_CYCLE] from=%s to=%s depth=%d wid=%d\n",
                                 fromKey, callKey, depth, wid);
                    std::fflush(stderr);
                    goto next_verb_chained;
                }
            }
            {
                // Resolve key.
                const SpecialIndexEntry* target = specialIndexFind(index, callKey);
                if (!target) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=%s wid=%d\n",
                                 fromKey, callKey, wid);
                    std::fflush(stderr);
                    continue;
                }
                // Emit call trace, recurse.
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] from=%s to=%s depth=%d wid=%d\n",
                             fromKey, callKey, depth + 1, wid);
                std::fflush(stderr);
                visited.push_back(callKey);
                executeSpecialBody_TraceOnlyChained(target->body, wid, index,
                                                    depth + 1, visited, callKey, varStore);
                visited.pop_back();
            }
            next_verb_chained:;
            continue;
        }

        // Var.* handling (when DISPATCH_VAR=1) — reuse the 1D handlers.
        if (s_dispatchVarGate() && varStore) {
            if (std::strncmp(vp, "Var.Set", 7) == 0 || std::strncmp(vp, "Var.Get", 7) == 0) {
                char key[32], value[32];
                uint8_t scope = 0;
                bool ok = parseVarVerb(vp, key, value, &scope);
                if (!ok) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_PARSE_FAIL] verb=%s wid=%d depth=%d (malformed)\n",
                                 vp, wid, depth);
                    std::fflush(stderr);
                    continue;
                }
                bool isSet = (std::strncmp(vp, "Var.Set", 7) == 0);
                if (isSet) handleVarSet(key, value, scope, varStore, wid);
                else       handleVarGet(key, scope, varStore, wid);
                continue;
            }
        }

        // Standard verb trace with depth.
        // NOTE: Brain.CorePower false in a CALLED body traces as recognized but
        // does NOT call setGeneralTacOrder (1A: no chained effects; deferred to 1B).
        if (isRecognizedVerb(vp)) {
            std::fprintf(stderr, "[BRAIN_DISPATCH] depth=%d verb=%s wid=%d\n", depth, vp, wid);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] depth=%d verb=%s wid=%d\n", depth, vp, wid);
        }
        std::fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// executeSpecialBody_TraceOnly
//
// FORBIDDEN-CALL CONTRACT: this function body contains ONLY fprintf + fflush
// + loop + string comparison + (when DISPATCH_VAR=1) store->set/get via handleVarSet/Get
// + (when DISPATCH_CALL=1) recursive executeSpecialBody_TraceOnlyChained.
// No tac-order writes. No movement/attack orders. No state writes to warrior or mission.
// Verified by inspection: no warrior pointer, no MechWarrior type, no
// setGeneralTacOrder / setPlayerTacOrder / setAlarmTacOrder / requestHelp /
// requestTarget / clearCurTacOrder / setMainGoal / calcTacOrder / coreMoveTo.
void executeSpecialBody_TraceOnly(const BrainSpecialBody& body, int wid, VarStore* varStore,
                                   const SpecialIndex* index, const char* callerKey) {
    const bool varGate  = s_dispatchVarGate();
    const bool callGate = s_dispatchCallGate() && (index != nullptr);

    // Per-tick visited set for cycle guard (CALL-CHAIN-1A).
    // Created here (root dispatch); passed into chained calls by reference.
    std::vector<std::string> visited;
    if (callerKey && callerKey[0] != '\0')
        visited.push_back(std::string(callerKey));

    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();

        // CALL-CHAIN-1A: TechSpecial.Call "<key>" handling.
        if (callGate && std::strncmp(vp, "TechSpecial.Call", 16) == 0 &&
            (vp[16] == ' ' || vp[16] == '\t' || vp[16] == '\0')) {
            char callKey[128];
            if (!parseCallVerbKey(vp, callKey)) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=<parse-fail> wid=%d\n",
                             callerKey ? callerKey : "root", wid);
                std::fflush(stderr);
                continue;
            }
            // Depth limit (root call = depth 1).
            if (kCallChainMaxDepth < 1) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_DEPTH] depth=1 max=%d wid=%d\n",
                             kCallChainMaxDepth, wid);
                std::fflush(stderr);
                continue;
            }
            // Cycle guard.
            bool cycle = false;
            for (const std::string& vis : visited) {
                if (vis == callKey) { cycle = true; break; }
            }
            if (cycle) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_CYCLE] from=%s to=%s depth=1 wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            // Resolve key.
            const SpecialIndexEntry* target = specialIndexFind(*index, callKey);
            if (!target) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=%s wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            // Emit call trace, then recurse.
            std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] from=%s to=%s depth=1 wid=%d\n",
                         callerKey ? callerKey : "root", callKey, wid);
            std::fflush(stderr);
            visited.push_back(std::string(callKey));
            executeSpecialBody_TraceOnlyChained(target->body, wid, *index,
                                                 1, visited, callKey, varStore);
            visited.pop_back();
            continue;
        }

        // When DISPATCH_VAR=1, intercept Var.Set / Var.Get before the UNKNOWN fallthrough.
        if (varGate) {
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
        if (isRecognizedVerb(vp)) {
            std::fprintf(stderr, "[BRAIN_DISPATCH] verb=%s wid=%d\n", vp, wid);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] verb=%s wid=%d\n", vp, wid);
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
//
// CALL-CHAIN-1A NOTE: TechSpecial.Call verbs in the ROOT body are dispatched trace-only
// (chained bodies traced without effects). Brain.CorePower false in a CALLED body is
// traced but does NOT call setGeneralTacOrder. Chained-effect execution = CALL-CHAIN-1B.
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid,
                               VarStore* varStore, const SpecialIndex* index,
                               const char* callerKey) {
    bool appliedEffect = false;
    const bool varGate  = s_dispatchVarGate();
    const bool callGate = s_dispatchCallGate() && (index != nullptr);

    // Per-tick visited set for cycle guard.
    std::vector<std::string> visited;
    if (callerKey && callerKey[0] != '\0')
        visited.push_back(std::string(callerKey));

    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();

        // CALL-CHAIN-1A: TechSpecial.Call handling (trace only — no chained effects in 1A).
        if (callGate && std::strncmp(vp, "TechSpecial.Call", 16) == 0 &&
            (vp[16] == ' ' || vp[16] == '\t' || vp[16] == '\0')) {
            char callKey[128];
            if (!parseCallVerbKey(vp, callKey)) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=<parse-fail> wid=%d\n",
                             callerKey ? callerKey : "root", wid);
                std::fflush(stderr);
                continue;
            }
            if (kCallChainMaxDepth < 1) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_DEPTH] depth=1 max=%d wid=%d\n",
                             kCallChainMaxDepth, wid);
                std::fflush(stderr);
                continue;
            }
            bool cycle = false;
            for (const std::string& vis : visited) {
                if (vis == callKey) { cycle = true; break; }
            }
            if (cycle) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_CYCLE] from=%s to=%s depth=1 wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            const SpecialIndexEntry* target = specialIndexFind(*index, callKey);
            if (!target) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_CALL_UNKNOWN] from=%s target=%s wid=%d\n",
                             callerKey ? callerKey : "root", callKey, wid);
                std::fflush(stderr);
                continue;
            }
            std::fprintf(stderr, "[BRAIN_DISPATCH_CALL] from=%s to=%s depth=1 wid=%d\n",
                         callerKey ? callerKey : "root", callKey, wid);
            std::fflush(stderr);
            // Recurse trace-only (1A: no effects in chained bodies).
            visited.push_back(std::string(callKey));
            executeSpecialBody_TraceOnlyChained(target->body, wid, *index,
                                                 1, visited, callKey, varStore);
            visited.pop_back();
            continue;
        }

        // When DISPATCH_VAR=1, intercept Var.Set / Var.Get before other dispatch.
        if (varGate) {
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
            // NOTE: This fires only for the ROOT body. Chained-body POWERDOWN = 1B.
            TacticalOrder pdOrder;
            pdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
            warrior->setGeneralTacOrder(pdOrder);
            std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=%d\n", wid);
            std::fflush(stderr);
            appliedEffect = true;
        } else if (isRecognizedVerb(vp)) {
            // Recognized but no effect implemented this slice — trace only.
            std::fprintf(stderr, "[BRAIN_DISPATCH] verb=%s wid=%d (apply-mode: no effect this verb)\n", vp, wid);
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[BRAIN_DISPATCH_UNKNOWN] verb=%s wid=%d\n", vp, wid);
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

// parseBrainSpecialBody_RawScan
//
// TECHSCRIPT-CALL-CHAIN-1A: extended to also populate outIndex (if non-null).
// For each TechSpecial block, the key= field is captured into a SpecialIndexEntry
// alongside the block's Body verbs. This is unconditional (not gated) — the index
// is always populated when the raw scanner runs, regardless of DISPATCH_CALL state.
// The gate controls runtime call resolution only.
//
// Entry-body selection rule (for outBody — the existing single-body member):
//   1. First TechSpecial block whose key contains "scenario_main" (case-sensitive).
//   2. Else first block whose type field = "MissionSpecial".
//   3. Else the first block found.
// If no blocks are found, outBody remains empty.
//
// Helper: parse a quoted string value from "key = \"...\""  or  "key = [...]".
// Returns the value token or "" on failure.
static std::string parseQuotedField(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    char closeDelim = '\0';
    if (*p == '"')      closeDelim = '"';
    else if (*p == '[') closeDelim = ']';
    else                return "";
    ++p;
    const char* end = std::strchr(p, closeDelim);
    if (!end) return "";
    return std::string(p, end);
}

static bool parseBrainSpecialBody_RawScan(const char* fitPath, BrainSpecialBody& outBody,
                                           SpecialIndex* outIndex) {
    std::ifstream inFile(fitPath);
    if (!inFile.is_open())
        return false;

    // State machine:
    //   OUTER      — scanning for "TechSpecial {"
    //   IN_SPECIAL — inside TechSpecial block, scanning for "Body {" or key=/type= fields
    //   IN_BODY    — inside Body block, collecting DO lines
    enum { OUTER, IN_SPECIAL, IN_BODY } state = OUTER;

    int bodiesFound = 0;
    int verbsThisBody = 0;
    std::string line;

    // Per-block state (reset on each TechSpecial {).
    std::string currentKey;
    std::string currentType;
    std::vector<std::string> currentVerbs;

    // Entry-body selection tracking.
    // We collect ALL blocks into the index, then choose outBody post-scan.
    // indexEntries built in parallel.
    // (outBody is set from the selected block at the end.)
    struct RawBlock {
        std::string key;
        std::string type;
        std::vector<std::string> verbs;
    };
    std::vector<RawBlock> rawBlocks;

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
            // Look for "TechSpecial {".
            if (std::strncmp(p, "TechSpecial", 11) == 0) {
                const char* q = p + 11;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{') {
                    if ((int)rawBlocks.size() >= kMaxBodies) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: TechSpecial block cap (%d) reached in %s — skipping rest\n",
                                     kMaxBodies, fitPath);
                        std::fflush(stderr);
                        break;
                    }
                    state = IN_SPECIAL;
                    verbsThisBody = 0;
                    currentKey.clear();
                    currentType.clear();
                    currentVerbs.clear();
                }
            }
        } else if (state == IN_SPECIAL) {
            // Look for key=, type=, "Body {", or closing "}".
            if (std::strncmp(p, "key", 3) == 0) {
                const char* q = p + 3;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '=') {
                    ++q;
                    currentKey = parseQuotedField(q);
                }
            } else if (std::strncmp(p, "type", 4) == 0) {
                const char* q = p + 4;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '=') {
                    ++q;
                    currentType = parseQuotedField(q);
                }
            } else if (std::strncmp(p, "Body", 4) == 0) {
                const char* q = p + 4;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '{') {
                    state = IN_BODY;
                    ++bodiesFound;
                }
            } else if (*p == '}') {
                // Closing brace of TechSpecial with no Body — commit block (no verbs).
                RawBlock rb;
                rb.key  = currentKey;
                rb.type = currentType;
                // no verbs
                rawBlocks.push_back(std::move(rb));
                state = OUTER;
            }
        } else { // IN_BODY
            if (*p == '}') {
                // End of Body block — commit the block.
                {
                    RawBlock rb;
                    rb.key   = currentKey;
                    rb.type  = currentType;
                    rb.verbs = currentVerbs;
                    rawBlocks.push_back(std::move(rb));
                }
                state = IN_SPECIAL;   // still inside TechSpecial; look for its closing '}'
            } else if (std::strncmp(p, "STOP", 4) == 0 &&
                       (p[4] == '\0' || p[4] == ' ' || p[4] == '\t' || p[4] == ';')) {
                // STOP — skip (sentinel only, not a verb).
            } else if (std::strncmp(p, "DO ", 3) == 0 || std::strncmp(p, "DO\t", 3) == 0) {
                if (verbsThisBody >= kMaxVerbsPerBody) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] WARN: verb cap (%d) per-body reached in %s — skipping rest of body\n",
                                 kMaxVerbsPerBody, fitPath);
                    std::fflush(stderr);
                    continue;
                }
                const char* verbStart = p + 3;
                while (*verbStart == ' ' || *verbStart == '\t') ++verbStart;

                std::string verb(verbStart);
                while (!verb.empty() && (verb.back() == ' ' || verb.back() == '\t'))
                    verb.pop_back();
                while (!verb.empty() && verb.back() == ';')
                    verb.pop_back();
                while (!verb.empty() && (verb.back() == ' ' || verb.back() == '\t'))
                    verb.pop_back();

                if (!verb.empty()) {
                    currentVerbs.push_back(std::move(verb));
                    ++verbsThisBody;
                }
            }
        }
    }

    if (rawBlocks.empty())
        return false;

    // Populate outIndex (all blocks, unconditionally).
    if (outIndex) {
        outIndex->clear();
        for (const RawBlock& rb : rawBlocks) {
            SpecialIndexEntry entry;
            entry.key        = rb.key;
            entry.body.verbs = rb.verbs;
            entry.body.loaded = !rb.verbs.empty();
            outIndex->push_back(std::move(entry));
        }
    }

    // Entry-body selection: populate outBody from chosen block.
    //   Rule 1: first block whose key contains "scenario_main".
    //   Rule 2: else first block with type == "MissionSpecial".
    //   Rule 3: else first block found.
    const RawBlock* chosen = nullptr;
    for (const RawBlock& rb : rawBlocks) {
        if (rb.key.find("scenario_main") != std::string::npos) {
            chosen = &rb; break;
        }
    }
    if (!chosen) {
        for (const RawBlock& rb : rawBlocks) {
            if (rb.type == "MissionSpecial") { chosen = &rb; break; }
        }
    }
    if (!chosen)
        chosen = &rawBlocks[0];

    // Migrate chosen block's verbs into outBody.
    // (verbs from ALL blocks were previously all merged; now only the entry-body is outBody.
    //  Callers that want all verbs should iterate the index.)
    outBody.verbs = chosen->verbs;

    return !outBody.verbs.empty() || !rawBlocks.empty();
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
bool parseBrainSpecialBody(const char* missionName, BrainSpecialBody& outBody,
                            SpecialIndex* outIndex) {
    outBody.verbs.clear();
    outBody.loaded = false;

    char fitPath[256];
    std::snprintf(fitPath, sizeof(fitPath), "data/missions/%s_specials.fit", missionName);

    // Try raw brace-block scanner first.
    // CALL-CHAIN-1A: outIndex forwarded (may be nullptr if caller doesn't need it).
    bool ok = parseBrainSpecialBody_RawScan(fitPath, outBody, outIndex);

    if (!ok) {
        // No TechSpecial brace blocks found — try legacy bracket-form fallback.
        // NOTE: legacy FitIni form does NOT populate the index (no TechSpecial blocks).
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
        std::fprintf(stderr, "[BRAIN_DISPATCH] parsed %s_specials.fit: %d verbs (index=%d blocks)\n",
                     missionName, (int)outBody.verbs.size(),
                     outIndex ? (int)outIndex->size() : 0);
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
