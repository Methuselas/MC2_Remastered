#pragma once
// MechBrainRuntime — mission-ephemeral brain runtime state for BRAIN-RUNTIME-1A.
// Not serialized (same non-persistence policy as BrainTaskQueue — if mid-mission
// save/load support is confirmed later, persistence is a separate BRAIN-SAVELOAD-1 slice).
// Gate: MC2_BRAIN_RUNTIME (default OFF). Requires MC2_BRAIN_TASKQ=1.
// 1A contract: struct exists + mode detected + arbitration COMPUTED+TRACE-LOGGED ONLY.
// NO slot writes, NO ABL short-circuit — those are BRAIN-RUNTIME-1B / DISPATCH-1.
//
// TECHSCRIPT-DISPATCH-1D: per-unit Var store.
// Gate: MC2_BRAIN_DISPATCH_VAR=1 (requires MC2_BRAIN_DISPATCH=1).
// Fixed-cap array of VarEntry (cap 32). Mission-ephemeral. NOT serialized.

#include <cstdint>
#include "brain_special_dispatch.h"
#include "brain_order_intent.h"

// ---------------------------------------------------------------------------
// Per-warrior Var store (TECHSCRIPT-DISPATCH-1D).
// Allocated inline in MechBrainRuntime (no heap). Cap = kVarStoreCap entries.
// Linear-scan get/set — 32 entries is fast enough; no STL containers here.

enum class VarScope : uint8_t {
    Unit    = 0,  // default; key auto-namespaced per warrior id at write time
    Mission = 1,  // modder's scope=Mission — trace-only in 1D; no shared store write
};

struct VarEntry {
    char     key[32];    // null-terminated key (truncated to 31 chars + '\0')
    char     value[32];  // raw token text (int/bool/float/string all stored as-is)
    VarScope scope;
};

struct VarStore {
    static constexpr int kVarStoreCap = 32;
    VarEntry entries[kVarStoreCap];
    int      count = 0;

    // Returns pointer to existing entry matching key+scope, or nullptr.
    VarEntry* find(const char* k, VarScope sc) {
        for (int i = 0; i < count; ++i)
            if (entries[i].scope == sc && std::strncmp(entries[i].key, k, 31) == 0)
                return &entries[i];
        return nullptr;
    }

    // Returns const pointer (read-only lookup).
    const VarEntry* find(const char* k, VarScope sc) const {
        for (int i = 0; i < count; ++i)
            if (entries[i].scope == sc && std::strncmp(entries[i].key, k, 31) == 0)
                return &entries[i];
        return nullptr;
    }

    // Returns false if cap reached (soft-fail; caller emits trace).
    bool set(const char* k, const char* v, VarScope sc) {
        VarEntry* e = find(k, sc);
        if (e) {
            // Overwrite existing.
            std::strncpy(e->value, v, 31);
            e->value[31] = '\0';
            return true;
        }
        if (count >= kVarStoreCap)
            return false;
        VarEntry& ne = entries[count++];
        std::strncpy(ne.key,   k, 31); ne.key[31]   = '\0';
        std::strncpy(ne.value, v, 31); ne.value[31] = '\0';
        ne.scope = sc;
        return true;
    }

    // Returns stored value string, or "0" if not found.
    const char* get(const char* k, VarScope sc) const {
        const VarEntry* e = find(k, sc);
        return e ? e->value : "0";
    }
};

enum class BrainRuntimeMode : uint8_t {
    Legacy   = 0,  // ABL owns all slots exclusively; runtime inert
    Hybrid   = 1,  // ABL owns GENERAL; runtime would-own PLAYER+ALARM (NOT applied in 1A)
    Enhanced = 2,  // Runtime would-own all three slots (NOT applied in 1A)
};

// Slot ownership bitmask (for trace-log compute; NOT applied to actual tac-order writes in 1A)
// Bit 0 = ORDERSTATE_GENERAL, Bit 1 = ORDERSTATE_PLAYER, Bit 2 = ORDERSTATE_ALARM
static constexpr uint8_t kBrainOwnsGeneral = 0x01;
static constexpr uint8_t kBrainOwnsPlayer  = 0x02;
static constexpr uint8_t kBrainOwnsAlarm   = 0x04;

struct MechBrainRuntime {
    BrainRuntimeMode mode                = BrainRuntimeMode::Legacy;
    uint8_t          activeOpordSlot     = 0;       // 0=GENERAL,1=PLAYER,2=ALARM — which slot is "running"
    uint8_t          opordFallbackPolicy = 0;       // 0=HOLD(default); 1=LOOP_PRIMARY; 2=WITHDRAW; 3=KEEP_REQUESTING
    uint8_t          completionFlags     = 0;       // bit0=primary done,bit1=secondary done,bit2=tertiary done
    float            tacticWeights[8]    = {};      // FIT-loaded tactic weights (zeroed; loader deferred to later slice)
    uint8_t          weightsNormalized   = 0;       // 1 = sum enforced to 1.0 at load time
    uint8_t          pendingEventMask    = 0;       // PilotAlarmType event bits
    uint8_t          _pad[1]             = {};
    uint32_t         lastRequestOrdersFrame = 0;    // g_mc2FrameCounter at last RequestOrdersTask push
    uint8_t          initialHoldPushed      = 0;    // 1 = initial HOLD_TASK already pushed (BRAIN-RUNTIME-1B)
    uint8_t          dispatchEffectApplied  = 0;    // 1 = BrainSpecial effect (POWERDOWN) applied once (DISPATCH-1B)
    uint8_t          ejectEffectApplied     = 0;    // 1 = Unit.Eject EJECT order issued once (DISPATCH-EFFECT-UNITEJECT-1)
    uint8_t          guardEffectApplied     = 0;    // 1 = OPORD.CoreGuard GUARD order issued once (DISPATCH-EFFECT-COREGUARD-1)
    uint8_t          moveToEffectApplied    = 0;    // 1 = OPORD.CoreMoveTo MOVETO_POINT order issued once (DISPATCH-EFFECT-COREMOVETO-1)
    uint8_t          attackEffectApplied   = 0;    // 1 = OPORD.CoreAttack ATTACK_OBJECT order issued once (DISPATCH-EFFECT-COREATTACK-1)
    uint8_t          retreatEffectApplied  = 0;    // 1 = Unit.Retreat WITHDRAW order issued once (DISPATCH-EFFECT-UNITRETREAT-1)
    // BRAIN-FSM-1K-A: per-warrior FSM state fields.
    // Gate: MC2_BRAIN_FSM (default OFF).  Mission-ephemeral — zeroed by default ctor (brace-init).
    // currentState[0]=='\0' = unnamed default state; InState guards always fail (safe no-op for non-FSM brains).
    // prevState filled by SetState (prevState←currentState before overwrite).
    // SetStatePrev swaps currentState↔prevState.  One level of stack = sufficient for transBack pattern.
    char             currentState[32]     = {};   // current FSM state name, "" = no-FSM / default
    char             prevState[32]        = {};   // for Unit.SetStatePrev (transBack equivalent)
    BrainSpecialBody specialBody;  // parsed from _specials.fit; loaded when MC2_BRAIN_DISPATCH set
    VarStore         varStore;     // per-unit Var namespace (DISPATCH-1D); populated by Var.Set/Var.Get
                                    // ABI: plain struct (no virtuals); sizeof increases by sizeof(vector)+sizeof(bool)
    // TECHSCRIPT-CALL-CHAIN-1A: per-mission TechSpecial index.
    // One entry per TechSpecial block parsed from <mission>_specials.fit.
    // Lifecycle: mission-ephemeral (same as specialBody). Cleared at mission teardown.
    // Populated by parseBrainSpecialBody() alongside specialBody.
    // Shared across warriors for the same mission (all warriors load the same file;
    // this copy is per-warrior-runtime but content is identical — acceptable for 1A's small index).
    SpecialIndex     specialIndex;

    // BRAIN-DECISION-INTENT-QUEUE-1: per-warrior pending intent buffer.
    // Gate: MC2_BRAIN_INTENT_QUEUE (default OFF).
    // When gate ON, executeSpecialBody_Apply emits BrainOrderIntents here instead of
    // calling setGeneralTacOrder directly.  commitBrainIntents() drains the buffer inline.
    // Cap = kBrainIntentCap (4).  Mission-ephemeral.  NOT serialized.
    // pendingIntentCount is reset to 0 at the start of each commitBrainIntents() call.
    BrainOrderIntent pendingIntents[kBrainIntentCap];
    int              pendingIntentCount = 0;

    // Compute which slots Brain WOULD own in this mode (trace-only; never applied in 1A).
    // Returns bitmask of kBrainOwns* flags.
    uint8_t computeWouldOwnMask() const {
        switch (mode) {
            case BrainRuntimeMode::Legacy:   return 0;
            case BrainRuntimeMode::Hybrid:   return kBrainOwnsPlayer | kBrainOwnsAlarm;
            case BrainRuntimeMode::Enhanced: return kBrainOwnsGeneral | kBrainOwnsPlayer | kBrainOwnsAlarm;
        }
        return 0;
    }

    static const char* modeString(BrainRuntimeMode m) {
        switch (m) {
            case BrainRuntimeMode::Legacy:   return "Legacy";
            case BrainRuntimeMode::Hybrid:   return "Hybrid";
            case BrainRuntimeMode::Enhanced: return "Enhanced";
        }
        return "Unknown";
    }
};
