#pragma once
// MechBrainRuntime — mission-ephemeral brain runtime state for BRAIN-RUNTIME-1A.
// Not serialized (same non-persistence policy as BrainTaskQueue — if mid-mission
// save/load support is confirmed later, persistence is a separate BRAIN-SAVELOAD-1 slice).
// Gate: MC2_BRAIN_RUNTIME (default OFF). Requires MC2_BRAIN_TASKQ=1.
// 1A contract: struct exists + mode detected + arbitration COMPUTED+TRACE-LOGGED ONLY.
// NO slot writes, NO ABL short-circuit — those are BRAIN-RUNTIME-1B / DISPATCH-1.

#include <cstdint>

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
