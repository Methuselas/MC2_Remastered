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
// RELAXED-CALL GUARD — executeSpecialBody_Apply (1B + UNITEJECT + COREGUARD + COREMOVETO + COREATTACK + UNITRETREAT):
// Permitted order calls: warrior->setGeneralTacOrder() for:
//   Brain.CorePower false                 → TACTICAL_ORDER_POWERDOWN
//   Unit.Eject (or alias coreEject)       → TACTICAL_ORDER_EJECT
//   OPORD.CoreGuard                       → TACTICAL_ORDER_GUARD
//   OPORD.CoreMoveTo x y z [params=N]     → TACTICAL_ORDER_MOVETO_POINT (NaN-coord soft-fail guard)
//   OPORD.CoreAttack <target_wid>         → TACTICAL_ORDER_ATTACK_OBJECT (triple-guard: bad-WID/self/friendly)
//   Unit.Retreat (or alias coreRetreat)   → TACTICAL_ORDER_WITHDRAW (no-arg, engine owns withdraw nav)
// All fire IN THE ROOT BODY ONLY. Chained bodies (via TechSpecial.Call) are trace-only in 1A.
// BRAIN-FSM-1K-A NOTE: Unit.SetState / Unit.SetStatePrev / Unit.InState / Unit.NotInState are NOT
// order calls — they write currentState/prevState fields or set stateGateOpen only.
// FSM verbs NEVER call setGeneralTacOrder. The relaxed_guard_doc checker count = 6 (unchanged).
// STILL FORBIDDEN: orderAttackObject (forbidden side-effects), setAttackTarget, setSituationOpenFire,
// setPlayerTacOrder, setAlarmTacOrder, requestHelp, requestTarget, calcTacOrder,
// coreMoveTo (the C++ Mover method, distinct from the OPORD DSL verb), setMainGoal,
// clearCurTacOrder, any movement/attack/OPORD-advance/commander function NOT listed above.
// DISPATCH-EFFECT-UNITRETREAT-1: Unit.Retreat + coreRetreat alias added (no-arg, pure once-guard).
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
#include "brain_mission_var_store.h" // TECHSCRIPT-DISPATCH-1D-M: MissionVarStore global
#include "brain_world_snapshot.h" // BRAIN-WORLD-SNAPSHOT-1: BrainWorldSnapshot POD + gate
#include "warrior.h"   // MechWarrior — needed for executeSpecialBody_Apply setGeneralTacOrder call
                       // warrior.h includes mech_brain_runtime.h which defines VarStore + VarScope
#include "tacordr.h"   // TacticalOrder, TACTICAL_ORDER_POWERDOWN, ORDER_ORIGIN_SELF
#include "gameobj.h"   // GameObjectPtr, GameObject::getWatchID/getTeam — DISPATCH-EFFECT-COREATTACK-1
#include "objmgr.h"    // ObjectManager (global) — DISPATCH-EFFECT-COREATTACK-1
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include "inifile.h"   // FitIniFile — same header used by _ai.fit loader

// ---------------------------------------------------------------------------
// TECHSCRIPT-DISPATCH-1D-M: MissionVarStore global definition.
// Mission-ephemeral: reset via resetMissionVarStore() at each mission load.
// Not serialized.
MissionVarStore g_missionVarStore;

// Gate helper for MC2_BRAIN_VAR_MISSION (1D-M).
// Requires MC2_BRAIN_DISPATCH_VAR=1 to be meaningful, but has its own separate gate
// so it can be turned on independently. Warns if turned on without DISPATCH_VAR.
static bool s_missionVarGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_VAR_MISSION");
        if (v && std::atoi(v) != 0) {
            const char* dv = std::getenv("MC2_BRAIN_DISPATCH_VAR");
            if (!dv || std::atoi(dv) == 0) {
                std::fprintf(stderr, "[BRAIN_VAR_MISSION] WARNING: MC2_BRAIN_VAR_MISSION=1 requires MC2_BRAIN_DISPATCH_VAR=1 — mission-scope writes/reads are INERT\n");
                std::fflush(stderr);
                return false;
            }
            return true;
        }
        return false;
    })();
    return kGate;
}

// resetMissionVarStore — clears g_missionVarStore.
// Call at mission load before warriors execute their first tick.
// Safe to call at any time (no-op if store is already empty).
void resetMissionVarStore() {
    g_missionVarStore.reset();
    std::fprintf(stderr, "[BRAIN_VAR_MISSION_RESET] g_missionVarStore cleared\n");
    std::fflush(stderr);
}

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
// Gate helper for MC2_BRAIN_FSM (BRAIN-FSM-1K-A).
// Default OFF. When ON, FSM verbs (SetState/SetStatePrev/InState/NotInState)
// are active.  When OFF, FSM verb strings fall through to the isRecognizedVerb
// path (trace-only [BRAIN_DISPATCH] line) — byte-identical for brains without FSM verbs.
static bool s_brainFsmGate() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_FSM");
        return v && std::atoi(v) != 0;
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// Recognized verb table for DISPATCH-1A.
// All other verbs produce [BRAIN_DISPATCH_UNKNOWN] trace.
static const char* const kRecognizedVerbs[] = {
    "Brain.CorePower",
    "Brain.CoreAttack",
    "OPORD.CoreAttack",
    "OPORD.CoreGuard",
    "OPORD.CorePatrol",
    "OPORD.CoreMoveTo",
    "Unit.Retreat",
    "Unit.Eject",
    "HOLD",
    // BRAIN-FSM-1K-A: FSM state verbs (gate MC2_BRAIN_FSM, default OFF)
    "Unit.SetState",
    "Unit.SetStatePrev",
    "Unit.InState",
    "Unit.NotInState",
    // BRAIN-FSM-1K-B: conditional FSM transition (gate MC2_BRAIN_FSM, requires MC2_BRAIN_DISPATCH_VAR)
    "Unit.SetStateIf",
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
// aliasToCanonical — DISPATCH-EFFECT-UNITEJECT-1
// Maps legacy ABL-style verb aliases to canonical dispatch verb names.
//   "coreEject"  → "Unit.Eject"
//   "corePower"  → "Brain.CorePower" (future; mapped here for completeness)
// Returns the canonical form if the input is a known alias; otherwise returns input unchanged.
static const char* aliasToCanonical(const char* verb) {
    if (std::strcmp(verb, "coreEject") == 0)
        return "Unit.Eject";
    if (std::strcmp(verb, "corePower") == 0)
        return "Brain.CorePower";
    if (std::strcmp(verb, "coreGuard") == 0)
        return "OPORD.CoreGuard";
    if (std::strcmp(verb, "coreRetreat") == 0)
        return "Unit.Retreat";
    return verb;
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
// bodyHasUnitEject — DISPATCH-EFFECT-UNITEJECT-1
// Returns true if the body contains a Unit.Eject (or coreEject alias) verb token.
bool bodyHasUnitEject(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        const char* canonical = aliasToCanonical(verb.c_str());
        if (std::strcmp(canonical, "Unit.Eject") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCoreGuard — DISPATCH-EFFECT-COREGUARD-1
// Returns true if the body contains an OPORD.CoreGuard (or coreGuard alias) verb token.
bool bodyHasCoreGuard(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        const char* canonical = aliasToCanonical(verb.c_str());
        if (std::strcmp(canonical, "OPORD.CoreGuard") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCoreMoveTo — DISPATCH-EFFECT-COREMOVETO-1
// Returns true if the body contains an OPORD.CoreMoveTo verb token.
// Token-prefix match: first word of the stored verb string must equal "OPORD.CoreMoveTo".
bool bodyHasCoreMoveTo(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        if (std::strncmp(verb.c_str(), "OPORD.CoreMoveTo", 16) == 0
            && (verb.size() == 16 || verb[16] == ' '))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasCoreAttack — DISPATCH-EFFECT-COREATTACK-1
// Returns true if the body contains an OPORD.CoreAttack verb token.
// Token-prefix match: first word of the stored verb string must equal "OPORD.CoreAttack".
bool bodyHasCoreAttack(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        if (std::strncmp(verb.c_str(), "OPORD.CoreAttack", 16) == 0
            && (verb.size() == 16 || verb[16] == ' '))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasUnitRetreat — DISPATCH-EFFECT-UNITRETREAT-1
// Returns true if the body contains a Unit.Retreat (or coreRetreat alias) verb token.
bool bodyHasUnitRetreat(const BrainSpecialBody& body) {
    for (const std::string& verb : body.verbs) {
        const char* canonical = aliasToCanonical(verb.c_str());
        if (std::strcmp(canonical, "Unit.Retreat") == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// bodyHasEffect — DISPATCH-EFFECT-UNITRETREAT-1 (extended from COREATTACK-1)
// Returns true if the body has ANY effect verb that claims the GENERAL slot
// (currently: POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, or RETREAT).
bool bodyHasEffect(const BrainSpecialBody& body) {
    return bodyHasPowerdown(body) || bodyHasUnitEject(body) || bodyHasCoreGuard(body)
        || bodyHasCoreMoveTo(body) || bodyHasCoreAttack(body) || bodyHasUnitRetreat(body);
}

// ---------------------------------------------------------------------------
// BRAIN-DECISION-INTENT-QUEUE-1: MC2_BRAIN_INTENT_QUEUE gate + emit helper.
//
// When gate ON, the 6 verb handlers in executeSpecialBody_Apply push a BrainOrderIntent
// to runtime->pendingIntents[] INSTEAD of calling warrior->setGeneralTacOrder directly.
// commitBrainIntents() (below) is the ONLY function that calls setGeneralTacOrder when
// gate ON.
//
// Gate OFF (default): each verb calls setGeneralTacOrder directly — byte-identical to
// the pre-BRAIN-DECISION-INTENT-QUEUE-1 behavior.

static bool s_intentQueueEnabled() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_INTENT_QUEUE");
        return (v && std::atoi(v) != 0);
    })();
    return kGate;
}

// ---------------------------------------------------------------------------
// BRAIN-WORLD-SNAPSHOT-1: MC2_BRAIN_SNAPSHOT gate (default OFF).
//
// Gate OFF (default): once-guards in warrior.cpp read live runtime->*EffectApplied
//   directly — byte-identical to pre-snapshot behavior.
// Gate ON: once-guards read snapshot.effectApplied[N] populated by
//   buildBrainWorldSnapshot() called right before the once-guard block.
//   The live runtime writes (*EffectApplied = 1) are NOT moved this rung.
//
// IDs-not-pointers: snapshot stores WatchIDs, not pointers. ATTACK target
//   team + existence still read live via ObjectManager::getByWatchID (deferred
//   to rung 7/8 when a target/contact table snapshot is added).
bool s_brainSnapshotEnabled() {
    static const bool kGate = ([](){
        const char* v = std::getenv("MC2_BRAIN_SNAPSHOT");
        return (v && std::atoi(v) != 0);
    })();
    return kGate;
}

// buildBrainWorldSnapshot — defined in warrior.cpp (static helper; needs protected access).

// Pushes one BrainOrderIntent onto runtime->pendingIntents[].
// Returns true if pushed, false if buffer full (soft-fail; rare — cap is 4).
// Caller must check gate before calling.
static bool emitBrainIntent(MechBrainRuntime* runtime, int warriorId,
                             TacticalOrderCode orderType, int targetWID,
                             float wx, float wy, float wz,
                             uint32_t sourceBodyId) {
    if (!runtime) return false;
    if (runtime->pendingIntentCount >= kBrainIntentCap) {
        std::fprintf(stderr, "[BRAIN_INTENT_EMIT_OVERFLOW] wid=%d order=%d cap=%d\n",
                     warriorId, (int)orderType, kBrainIntentCap);
        std::fflush(stderr);
        return false;
    }
    BrainOrderIntent& out = runtime->pendingIntents[runtime->pendingIntentCount++];
    out.warriorId   = warriorId;
    out.slot        = BrainIntentSlot::GENERAL;
    out.orderType   = orderType;
    out.targetWID   = targetWID;
    out.waypoint[0] = wx;
    out.waypoint[1] = wy;
    out.waypoint[2] = wz;
    out.sourceBodyId = sourceBodyId;
    out.brainTick   = getBrainTickIndex();
    return true;
}

// ---------------------------------------------------------------------------
// executeSpecialBody_Apply
//
// 1B EFFECT DISPATCHER — gate MC2_BRAIN_DISPATCH_APPLY=1 (requires MC2_BRAIN_DISPATCH=1).
//
// RELAXED-CALL GUARD CONTRACT:
//   Permitted verbs (SIX total): Brain.CorePower false → POWERDOWN,
//   Unit.Eject (or coreEject alias) → EJECT, OPORD.CoreGuard (or coreGuard alias) → GUARD,
//   OPORD.CoreMoveTo x y z → MOVETO_POINT, OPORD.CoreAttack <wid> → ATTACK_OBJECT,
//   Unit.Retreat (or coreRetreat alias) → WITHDRAW.
//   All other verbs produce [BRAIN_DISPATCH] or [BRAIN_DISPATCH_UNKNOWN] trace only.
//   FORBIDDEN in this function: orderAttackObject, setAttackTarget, setSituationOpenFire,
//   setPlayerTacOrder, setAlarmTacOrder, requestHelp,
//   requestTarget, calcTacOrder, coreMoveTo, setMainGoal, clearCurTacOrder,
//   any movement/attack/OPORD-advance/commander function NOT listed above.
//   BRAIN-DECISION-INTENT-QUEUE-1 (gate MC2_BRAIN_INTENT_QUEUE):
//   When gate ON:  the 6 verb handlers call emitBrainIntent() — NO setGeneralTacOrder here.
//                  commitBrainIntents() (below) is the sole caller of setGeneralTacOrder.
//   When gate OFF: the 6 verb handlers call warrior->setGeneralTacOrder() directly (unchanged).
//   The checker (check_brain_relaxed_guard_doc.py) counts call-sites across BOTH this function
//   AND commitBrainIntents to confirm the verb count matches the doc.
//   Verified by inspection: exactly six effect-verb branches (gate OFF callsites = 6;
//   gate ON emitBrainIntent() calls = 6 in commitBrainIntents).
//
// Returns true if a GENERAL-slot effect (POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, or RETREAT) was applied.
// Caller uses the return value to suppress the synthetic HOLD_TASK (one GENERAL-slot write per tick).
//
// CALL-CHAIN-1A NOTE: TechSpecial.Call verbs in the ROOT body are dispatched trace-only
// (chained bodies traced without effects). Brain.CorePower false in a CALLED body is
// traced but does NOT call setGeneralTacOrder. Chained-effect execution = CALL-CHAIN-1B.
bool executeSpecialBody_Apply(const BrainSpecialBody& body, MechWarrior* warrior, int wid,
                               VarStore* varStore, const SpecialIndex* index,
                               const char* callerKey) {
    bool appliedEffect = false;
    // BRAIN-DECISION-INTENT-QUEUE-1: get runtime for intent emit (gate ON only).
    MechBrainRuntime* runtime = (warrior && s_intentQueueEnabled()) ? warrior->getBrainRuntime() : nullptr;
    const bool varGate  = s_dispatchVarGate();
    const bool callGate = s_dispatchCallGate() && (index != nullptr);
    // BRAIN-FSM-1K-A: stateGateOpen — stack-local, reset true at body entry.
    // Unit.InState / Unit.NotInState set this false on mismatch; subsequent verbs are skipped.
    // ONE continue check at top of loop; the 6 effect handlers are UNCHANGED.
    const bool fsmGate = s_brainFsmGate();
    bool stateGateOpen = true;

    // Per-tick visited set for cycle guard.
    std::vector<std::string> visited;
    if (callerKey && callerKey[0] != '\0')
        visited.push_back(std::string(callerKey));

    for (const std::string& verb : body.verbs) {
        const char* vp = verb.c_str();

        // BRAIN-FSM-1K-A: gate check — skip effect verbs after a failed InState/NotInState.
        if (!stateGateOpen) continue;

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
        // DISPATCH-EFFECT-UNITEJECT-1: alias resolution before effect dispatch.
        const char* vpCanon = aliasToCanonical(vp);

        if (verb == "Brain.CorePower false") {
            // BRAIN-DECISION-INTENT-QUEUE-1: gate ON → emit intent; gate OFF → direct call.
            // NOTE: This fires only for the ROOT body. Chained-body POWERDOWN = 1B.
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_POWERDOWN, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Brain.CorePower order=POWERDOWN tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder pdOrder;
                pdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
                warrior->setGeneralTacOrder(pdOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Brain.CorePower effect=POWERDOWN wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strcmp(vpCanon, "Unit.Eject") == 0) {
            // DISPATCH-EFFECT-UNITEJECT-1: Unit.Eject (or alias coreEject) → TACTICAL_ORDER_EJECT.
            // ROOT BODY ONLY. Fires exactly once (ejectEffectApplied once-guard in warrior.cpp).
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_EJECT, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Unit.Eject order=EJECT tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder ejectOrder;
                ejectOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_EJECT);
                warrior->setGeneralTacOrder(ejectOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Unit.Eject effect=EJECT wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strcmp(vpCanon, "OPORD.CoreGuard") == 0) {
            // DISPATCH-EFFECT-COREGUARD-1: OPORD.CoreGuard (or alias coreGuard) → TACTICAL_ORDER_GUARD.
            // ROOT BODY ONLY. Fires exactly once (guardEffectApplied once-guard in warrior.cpp).
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_GUARD, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=OPORD.CoreGuard order=GUARD tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder guardOrder;
                guardOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_GUARD);
                warrior->setGeneralTacOrder(guardOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreGuard effect=GUARD wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (std::strncmp(vpCanon, "OPORD.CoreMoveTo", 16) == 0) {
            // DISPATCH-EFFECT-COREMOVETO-1: OPORD.CoreMoveTo x y z [params=N]
            //
            // Arg parser — bare-numeric form only:
            //   verb text after "OPORD.CoreMoveTo" is split by whitespace.
            //   First three numeric tokens are x y z (float via std::strtof).
            //   Optional trailing "params=N" token (int, default 0) is parsed but not
            //   used this slice (reserved for future movement-mode expansion).
            //   On any parse failure: soft-fail trace, skip order.
            //   NaN guard: std::isnan(x||y||z) → soft-fail trace, skip order.
            const char* args = vpCanon + 16; // points past "OPORD.CoreMoveTo"
            while (*args == ' ') ++args;     // skip leading spaces
            char* endp = nullptr;
            float mx = std::strtof(args, &endp);
            if (endp == args) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                std::fflush(stderr);
            } else {
                args = endp;
                while (*args == ' ') ++args;
                float my = std::strtof(args, &endp);
                if (endp == args) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                    std::fflush(stderr);
                } else {
                    args = endp;
                    while (*args == ' ') ++args;
                    float mz = std::strtof(args, &endp);
                    if (endp == args) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                        std::fflush(stderr);
                    } else if (std::isnan(mx) || std::isnan(my) || std::isnan(mz)) {
                        std::fprintf(stderr, "[BRAIN_DISPATCH_MOVETO_NAN] x=%g y=%g z=%g wid=%d\n", mx, my, mz, wid);
                        std::fflush(stderr);
                    } else if (runtime) {
                        // Gate ON: emit intent; commitBrainIntents rebuilds the TacticalOrder.
                        emitBrainIntent(runtime, wid, TACTICAL_ORDER_MOVETO_POINT, -1, mx, my, mz, 0);
                        std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=OPORD.CoreMoveTo order=MOVETO_POINT pos=(%g %g %g) tick=%u wid=%d\n",
                                     mx, my, mz, getBrainTickIndex(), wid);
                        std::fflush(stderr);
                        appliedEffect = true;
                    } else {
                        // Gate OFF: build TacticalOrder following missiongui.cpp:4161 exactly.
                        LocationNode moveNode;
                        moveNode.location.x = mx;
                        moveNode.location.y = my;
                        moveNode.location.z = mz;
                        moveNode.run  = true;
                        moveNode.next = nullptr;
                        TacticalOrder moveOrder;
                        moveOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
                        moveOrder.initWayPath(&moveNode);
                        moveOrder.moveParams.wait = false;
                        moveOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                        moveOrder.pack(nullptr, nullptr);
                        warrior->setGeneralTacOrder(moveOrder);
                        std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreMoveTo effect=MOVETO_POINT pos=(%g %g %g) wid=%d\n",
                                     mx, my, mz, wid);
                        std::fflush(stderr);
                        appliedEffect = true;
                    }
                }
            }
        } else if (std::strncmp(vpCanon, "OPORD.CoreAttack", 16) == 0
                   && (vpCanon[16] == ' ' || vpCanon[16] == '\0')) {
            // DISPATCH-EFFECT-COREATTACK-1: OPORD.CoreAttack <wid>
            //
            // Arg parser — bare-integer WID:
            //   Token after "OPORD.CoreAttack" is the decimal WID (strtol).
            //   Zero or negative WID → soft-fail [BRAIN_DISPATCH_ATTACK_BAD_WID].
            //   Non-numeric token → soft-fail [BRAIN_DISPATCH_ATTACK_PARSE_FAIL].
            // Triple guard (single lookup):
            //   1. target not found → [BRAIN_DISPATCH_ATTACK_BAD_WID] reason=not_found
            //   2. target WID == self WID → [BRAIN_DISPATCH_ATTACK_SELF]
            //   3. target same team → [BRAIN_DISPATCH_ATTACK_FRIENDLY]
            // ANTI-PATTERN: do NOT call orderAttackObject — it has forbidden side-effects.
            // Construct TacticalOrder by hand mirroring missiongui.cpp:2805.
            const char* aargs = vpCanon + 16;
            while (*aargs == ' ') ++aargs;
            char* aendp = nullptr;
            long parsedWID = std::strtol(aargs, &aendp, 10);
            if (aendp == aargs) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_PARSE_FAIL] verb=%s wid=%d\n", vp, wid);
                std::fflush(stderr);
            } else if (parsedWID <= 0) {
                std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=%ld wid=%d\n", parsedWID, wid);
                std::fflush(stderr);
            } else {
                GameObjectPtr target = ObjectManager->getByWatchID((long)parsedWID);
                if (!target) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_BAD_WID] wid=%ld reason=not_found wid=%d\n", parsedWID, wid);
                    std::fflush(stderr);
                } else if (target->getWatchID() == (long)wid) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_SELF] wid=%ld wid=%d\n", parsedWID, wid);
                    std::fflush(stderr);
                } else if (target->getTeam() == warrior->getTeam()) {
                    std::fprintf(stderr, "[BRAIN_DISPATCH_ATTACK_FRIENDLY] wid=%ld wid=%d\n", parsedWID, wid);
                    std::fflush(stderr);
                } else if (runtime) {
                    // Gate ON: emit intent; commitBrainIntents rebuilds the TacticalOrder.
                    emitBrainIntent(runtime, wid, TACTICAL_ORDER_ATTACK_OBJECT, (int)parsedWID, 0,0,0, 0);
                    std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=OPORD.CoreAttack order=ATTACK_OBJECT targetWID=%ld tick=%u wid=%d\n",
                                 parsedWID, getBrainTickIndex(), wid);
                    std::fflush(stderr);
                    appliedEffect = true;
                } else {
                    // Gate OFF: construct TacticalOrder by hand — mirror missiongui.cpp:2805.
                    // FORBIDDEN: orderAttackObject, setAttackTarget, setSituationOpenFire.
                    TacticalOrder attackOrder;
                    attackOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_ATTACK_OBJECT, false);
                    attackOrder.targetWID = (long)parsedWID;
                    attackOrder.attackParams.type   = ATTACK_TO_DESTROY;
                    attackOrder.attackParams.method = ATTACKMETHOD_RANGED;
                    attackOrder.attackParams.range  = FIRERANGE_OPTIMAL;
                    attackOrder.attackParams.pursue = true;
                    attackOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                    warrior->setGeneralTacOrder(attackOrder);
                    std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=OPORD.CoreAttack effect=ATTACK_OBJECT targetWID=%ld wid=%d\n",
                                 parsedWID, wid);
                    std::fflush(stderr);
                    appliedEffect = true;
                }
            }
        } else if (std::strcmp(vpCanon, "Unit.Retreat") == 0) {
            // DISPATCH-EFFECT-UNITRETREAT-1: Unit.Retreat (or alias coreRetreat) → TACTICAL_ORDER_WITHDRAW.
            // ROOT BODY ONLY. Fires exactly once (retreatEffectApplied once-guard in warrior.cpp).
            if (runtime) {
                emitBrainIntent(runtime, wid, TACTICAL_ORDER_WITHDRAW, -1, 0,0,0, 0);
                std::fprintf(stderr, "[BRAIN_INTENT_EMIT] verb=Unit.Retreat order=WITHDRAW tick=%u wid=%d\n",
                             getBrainTickIndex(), wid);
                std::fflush(stderr);
            } else {
                TacticalOrder retreatOrder;
                retreatOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WITHDRAW, false);
                warrior->setGeneralTacOrder(retreatOrder);
                std::fprintf(stderr, "[BRAIN_DISPATCH_APPLY] verb=Unit.Retreat effect=WITHDRAW wid=%d\n", wid);
                std::fflush(stderr);
            }
            appliedEffect = true;
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.SetState", 13) == 0 &&
                   (vpCanon[13] == ' ' || vpCanon[13] == '\t' || vpCanon[13] == '\0')) {
            // BRAIN-FSM-1K-A: Unit.SetState "x" — explicit FSM transition.
            // prevState←currentState; currentState←x; early return (no order call).
            // NOTE: NOT an order call — relaxed_guard_doc setGeneralTacOrder count = 6 (unchanged).
            MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
            if (fsm_rt) {
                // Parse quoted state name: verb is "Unit.SetState \"stateName\""
                const char* sp = vpCanon + 13;
                while (*sp == ' ' || *sp == '\t') ++sp;
                if (*sp == '"') ++sp;
                char newState[32] = {};
                int ni = 0;
                while (*sp && *sp != '"' && ni < 31) newState[ni++] = *sp++;
                newState[ni] = '\0';
                std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
                fsm_rt->prevState[31] = '\0';
                std::strncpy(fsm_rt->currentState, newState, 31);
                fsm_rt->currentState[31] = '\0';
                std::fprintf(stderr, "[BRAIN_FSM_SETSTATE] from=%s to=%s wid=%d\n",
                             fsm_rt->prevState, fsm_rt->currentState, wid);
                std::fflush(stderr);
            }
            return appliedEffect;  // early exit — no subsequent verbs after SetState
        } else if (fsmGate && std::strcmp(vpCanon, "Unit.SetStatePrev") == 0) {
            // BRAIN-FSM-1K-A: Unit.SetStatePrev — transBack equivalent, swaps currentState↔prevState.
            // Early return (no order call).
            MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
            if (fsm_rt) {
                char tmp[32];
                std::memcpy(tmp, fsm_rt->prevState, 32);
                std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
                fsm_rt->prevState[31] = '\0';
                std::memcpy(fsm_rt->currentState, tmp, 32);
                std::fprintf(stderr, "[BRAIN_FSM_SETSTATEPREV] now=%s wid=%d\n",
                             fsm_rt->currentState, wid);
                std::fflush(stderr);
            }
            return appliedEffect;  // early exit — no subsequent verbs after SetStatePrev
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.InState", 12) == 0 &&
                   (vpCanon[12] == ' ' || vpCanon[12] == '\t' || vpCanon[12] == '\0')) {
            // BRAIN-FSM-1K-A: Unit.InState "x" — guard verb; does NOT call any order function.
            // If currentState != x, stateGateOpen←false; subsequent verbs in this body are skipped.
            MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
            const char* sp = vpCanon + 12;
            while (*sp == ' ' || *sp == '\t') ++sp;
            if (*sp == '"') ++sp;
            char testState[32] = {};
            int ni = 0;
            while (*sp && *sp != '"' && ni < 31) testState[ni++] = *sp++;
            testState[ni] = '\0';
            const char* cur = fsm_rt ? fsm_rt->currentState : "";
            int match = (std::strcmp(cur, testState) == 0) ? 1 : 0;
            if (!match) stateGateOpen = false;
            std::fprintf(stderr, "[BRAIN_FSM_INSTATE] state=%s match=%d wid=%d\n", testState, match, wid);
            std::fflush(stderr);
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.NotInState", 15) == 0 &&
                   (vpCanon[15] == ' ' || vpCanon[15] == '\t' || vpCanon[15] == '\0')) {
            // BRAIN-FSM-1K-A: Unit.NotInState "x" — inverse guard; does NOT call any order function.
            // If currentState == x, stateGateOpen←false; subsequent verbs in this body are skipped.
            MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
            const char* sp = vpCanon + 15;
            while (*sp == ' ' || *sp == '\t') ++sp;
            if (*sp == '"') ++sp;
            char testState[32] = {};
            int ni = 0;
            while (*sp && *sp != '"' && ni < 31) testState[ni++] = *sp++;
            testState[ni] = '\0';
            const char* cur = fsm_rt ? fsm_rt->currentState : "";
            int match = (std::strcmp(cur, testState) == 0) ? 1 : 0;
            if (match) stateGateOpen = false;
            std::fprintf(stderr, "[BRAIN_FSM_NOTINSTATE] state=%s match=%d wid=%d\n", testState, match, wid);
            std::fflush(stderr);
        } else if (fsmGate && std::strncmp(vpCanon, "Unit.SetStateIf", 15) == 0 &&
                   (vpCanon[15] == ' ' || vpCanon[15] == '\t' || vpCanon[15] == '\0')) {
            // BRAIN-FSM-1K-B: Unit.SetStateIf "stateName" "varKey" "varValue"
            // If Var store (unit scope) varKey == varValue → SetState transition (prevState←currentState; currentState←stateName); early return.
            // If no match → no-op; body continues.
            // Requires MC2_BRAIN_DISPATCH_VAR=1. If VAR gate OFF, emits one-time warning and is INERT.
            //
            // Parse 3 quoted args: stateName, varKey, varValue.
            MechBrainRuntime* fsm_rt = warrior ? warrior->getBrainRuntime() : nullptr;
            // Requires-VAR guard (mirror 1D-M pattern): FSM ON but VAR OFF → inert + one-time warning.
            if (!varGate) {
                static bool s_setstateif_var_warn = false;
                if (!s_setstateif_var_warn) {
                    s_setstateif_var_warn = true;
                    std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] WARNING: Unit.SetStateIf requires MC2_BRAIN_DISPATCH_VAR=1 — verb is INERT\n");
                    std::fflush(stderr);
                }
                // INERT: no transition, body continues.
            } else {
                // Parse three quoted args: "stateName" "varKey" "varValue"
                const char* sp = vpCanon + 15;
                while (*sp == ' ' || *sp == '\t') ++sp;

                // Arg 1: stateName
                char stateName[32] = {};
                if (*sp == '"') ++sp;
                int ni = 0;
                while (*sp && *sp != '"' && ni < 31) stateName[ni++] = *sp++;
                stateName[ni] = '\0';
                if (*sp == '"') ++sp;
                while (*sp == ' ' || *sp == '\t') ++sp;

                // Arg 2: varKey
                char varKey[32] = {};
                if (*sp == '"') ++sp;
                ni = 0;
                while (*sp && *sp != '"' && ni < 31) varKey[ni++] = *sp++;
                varKey[ni] = '\0';
                if (*sp == '"') ++sp;
                while (*sp == ' ' || *sp == '\t') ++sp;

                // Arg 3: varValue (expected value)
                char varValue[32] = {};
                if (*sp == '"') ++sp;
                ni = 0;
                while (*sp && *sp != '"' && ni < 31) varValue[ni++] = *sp++;
                varValue[ni] = '\0';

                // Read unit-scope var (unit-scope only; mission-scope deferred to future slice).
                const char* curVal = (varStore && varKey[0] != '\0')
                    ? varStore->get(varKey, VarScope::Unit)
                    : "0";

                int match = (std::strcmp(curVal, varValue) == 0) ? 1 : 0;

                if (match) {
                    // Perform SetState transition: prevState←currentState; currentState←stateName.
                    if (fsm_rt) {
                        std::strncpy(fsm_rt->prevState, fsm_rt->currentState, 31);
                        fsm_rt->prevState[31] = '\0';
                        std::strncpy(fsm_rt->currentState, stateName, 31);
                        fsm_rt->currentState[31] = '\0';
                        std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] var=%s cur=%s want=%s match=1 from=%s to=%s wid=%d\n",
                                     varKey, curVal, varValue, fsm_rt->prevState, fsm_rt->currentState, wid);
                    } else {
                        std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] var=%s cur=%s want=%s match=1 from=(no-rt) to=%s wid=%d\n",
                                     varKey, curVal, varValue, stateName, wid);
                    }
                    std::fflush(stderr);
                    return appliedEffect;  // early exit on match — same semantics as SetState
                } else {
                    std::fprintf(stderr, "[BRAIN_FSM_SETSTATEIF] var=%s cur=%s want=%s match=0 wid=%d\n",
                                 varKey, curVal, varValue, wid);
                    std::fflush(stderr);
                    // No transition; body continues.
                }
            }
        } else if (isRecognizedVerb(vpCanon)) {
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
// BRAIN-DECISION-INTENT-QUEUE-1: commitBrainIntents
//
// COMMIT PHASE — the ONLY function that calls warrior->setGeneralTacOrder() when
// MC2_BRAIN_INTENT_QUEUE=1.  Called right after executeSpecialBody_Apply returns
// (inline, same thread, this rung).  Drains runtime->pendingIntents[] and for each
// intent reconstructs a TacticalOrder and calls warrior->setGeneralTacOrder().
//
// FORBIDDEN-CALL GUARD (commit phase): this function and only this function may call
// warrior->setGeneralTacOrder() when gate ON.  The verb handlers in
// executeSpecialBody_Apply must NOT call setGeneralTacOrder when gate ON.
//
// MOVETO reconstruction mirrors missiongui.cpp:4161 (same as the gate-OFF path above).
// ATTACK reconstruction mirrors missiongui.cpp:2805.
// All other verbs: simple init(ORDER_ORIGIN_SELF, orderType).
//
// Emits: [BRAIN_INTENT_COMMIT] verb=<v> order=<type> tick=<brainTick> wid=<W> per intent.
// After draining, resets pendingIntentCount to 0.
// warrior and runtime must be non-null (caller guards).
void commitBrainIntents(MechWarrior* warrior, MechBrainRuntime* runtime) {
    if (!warrior || !runtime) return;
    const int n = runtime->pendingIntentCount;
    for (int i = 0; i < n; ++i) {
        const BrainOrderIntent& intent = runtime->pendingIntents[i];
        switch (intent.orderType) {
            case TACTICAL_ORDER_POWERDOWN: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_POWERDOWN);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Brain.CorePower order=POWERDOWN tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_EJECT: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_EJECT);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Unit.Eject order=EJECT tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_GUARD: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_GUARD);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=OPORD.CoreGuard order=GUARD tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_MOVETO_POINT: {
                LocationNode moveNode;
                moveNode.location.x = intent.waypoint[0];
                moveNode.location.y = intent.waypoint[1];
                moveNode.location.z = intent.waypoint[2];
                moveNode.run  = true;
                moveNode.next = nullptr;
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_MOVETO_POINT, false);
                o.initWayPath(&moveNode);
                o.moveParams.wait = false;
                o.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                o.pack(nullptr, nullptr);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=OPORD.CoreMoveTo order=MOVETO_POINT pos=(%g %g %g) tick=%u wid=%d\n",
                             intent.waypoint[0], intent.waypoint[1], intent.waypoint[2],
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_ATTACK_OBJECT: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_ATTACK_OBJECT, false);
                o.targetWID = (long)intent.targetWID;
                o.attackParams.type   = ATTACK_TO_DESTROY;
                o.attackParams.method = ATTACKMETHOD_RANGED;
                o.attackParams.range  = FIRERANGE_OPTIMAL;
                o.attackParams.pursue = true;
                o.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=OPORD.CoreAttack order=ATTACK_OBJECT targetWID=%d tick=%u wid=%d\n",
                             intent.targetWID, intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            case TACTICAL_ORDER_WITHDRAW: {
                TacticalOrder o;
                o.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_WITHDRAW, false);
                warrior->setGeneralTacOrder(o);
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT] verb=Unit.Retreat order=WITHDRAW tick=%u wid=%d\n",
                             intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
            }
            default:
                std::fprintf(stderr, "[BRAIN_INTENT_COMMIT_UNKNOWN] order=%d tick=%u wid=%d\n",
                             (int)intent.orderType, intent.brainTick, intent.warriorId);
                std::fflush(stderr);
                break;
        }
    }
    runtime->pendingIntentCount = 0;
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
// BRAIN-DISPATCH-HARNESS-1: parseBrainSpecialBodyFromPath
//
// Explicit-path variant of parseBrainSpecialBody for offline harness use.
// Directly opens fitPath without the "data/missions/<name>_specials.fit" construction.
// Same scanner logic (raw brace-block first, FitIni fallback).
bool parseBrainSpecialBodyFromPath(const char* fitPath, BrainSpecialBody& outBody,
                                    SpecialIndex* outIndex) {
    outBody.verbs.clear();
    outBody.loaded = false;

    bool ok = parseBrainSpecialBody_RawScan(fitPath, outBody, outIndex);
    if (!ok) {
        std::fprintf(stderr, "[BRAIN_DISPATCH_RAW] no TechSpecial blocks in %s — trying FitIni fallback\n", fitPath);
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
        std::fprintf(stderr, "[BRAIN_DISPATCH] parsed %s: %d verbs (index=%d blocks)\n",
                     fitPath, (int)outBody.verbs.size(),
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
        // TECHSCRIPT-DISPATCH-1D-M: MC2_BRAIN_VAR_MISSION gate ON → real write to g_missionVarStore.
        // Gate OFF → trace-only (1D deferred behavior preserved).
        if (!s_missionVarGate()) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_SET_MISSION_TRACE] key=%s value=%s wid=%d (write deferred -- MC2_BRAIN_VAR_MISSION not set)\n",
                         key, value, wid);
            std::fflush(stderr);
            return;
        }
        // Guarded-single-writer: WID-order-wins within same tick.
        // brainTick via getBrainTickIndex() — available in dispatch.cpp context.
        uint32_t tick = getBrainTickIndex();
        bool conflict = false;
        bool ok = g_missionVarStore.trySet(key, value, wid, tick, &conflict);
        if (conflict) {
            // A higher-WID already wrote this key this tick — lower WID suppressed.
            // Retrieve the winning value and prevWID for the conflict trace.
            const MissionVarEntry* e = g_missionVarStore.find(key);
            int prevWid = e ? e->lastWriterWID : -1;
            std::fprintf(stderr, "[BRAIN_VAR_MISSION_CONFLICT] key=%s wid=%d prevWid=%d tick=%u (higher-WID wins; this write suppressed)\n",
                         key, wid, prevWid, tick);
            std::fflush(stderr);
            return;
        }
        if (!ok) {
            std::fprintf(stderr, "[BRAIN_VAR_MISSION_SET_FULL] key=%s wid=%d (cap=%d reached -- skipped)\n",
                         key, wid, MissionVarStore::kMissionVarStoreCap);
            std::fflush(stderr);
            return;
        }
        std::fprintf(stderr, "[BRAIN_VAR_MISSION_SET] key=%s val=%s wid=%d tick=%u\n", key, value, wid, tick);
        std::fflush(stderr);
        return;
    }
    // Unit scope (unchanged from 1D).
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
        // TECHSCRIPT-DISPATCH-1D-M: MC2_BRAIN_VAR_MISSION gate ON → real read from g_missionVarStore.
        // Gate OFF → trace-only default-only (1D deferred behavior preserved).
        if (!s_missionVarGate()) {
            std::fprintf(stderr, "[BRAIN_DISPATCH_VAR_GET_MISSION_TRACE] key=%s wid=%d (default -- MC2_BRAIN_VAR_MISSION not set)\n",
                         key, wid);
            std::fflush(stderr);
            return;
        }
        const char* val = g_missionVarStore.get(key);
        bool isHit = (g_missionVarStore.find(key) != nullptr);
        std::fprintf(stderr, "[BRAIN_VAR_MISSION_GET] key=%s val=%s wid=%d (%s)\n",
                     key, val, wid, isHit ? "hit" : "default");
        std::fflush(stderr);
        return;
    }
    // Unit scope (unchanged from 1D).
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
