#pragma once
// BRAIN-DECISION-INTENT-QUEUE-1 — rung 5 of Brain decoupling ladder.
// Gate: MC2_BRAIN_INTENT_QUEUE (default OFF).
//
// BrainOrderIntent: a deferred order record emitted by an effect verb handler instead of
// calling setGeneralTacOrder directly.  The commit function (commitBrainIntents) drains
// pendingIntents and calls setGeneralTacOrder once per intent — inline, same thread,
// this rung only.  No threading, no snapshot, no separated commit phase.
//
// Design:
//   EMIT phase  — verb handler builds a BrainOrderIntent + pushes to pendingIntents[];
//                 the forbidden-call guard (no setGeneralTacOrder here) moves to COMMIT.
//   COMMIT phase— commitBrainIntents() drains the buffer and calls setGeneralTacOrder.
//                 commitBrainIntents() is the ONLY path that calls setGeneralTacOrder
//                 when gate ON.
//
// Stub-linkable: POD + stdint only; no TacticalOrder/MechWarrior includes needed.
// The harness (tools/brain_dispatch_harness) does not exercise Apply, so it does not
// need to link brain_order_intent.h beyond the struct definition.

#include <cstdint>
#include "tacordr.h"   // TacticalOrderCode — we store the code by value

// ---------------------------------------------------------------------------
// Order-slot tag: all 6 verbs write to the GENERAL slot (this rung).
// PLAYER / ALARM slots are out of scope until rung 7.
enum class BrainIntentSlot : uint8_t {
    GENERAL = 0,
};

// ---------------------------------------------------------------------------
// BrainOrderIntent: per-emit intent record.
// Emitted by each of the 6 verb handlers when MC2_BRAIN_INTENT_QUEUE=1.
// Committed by commitBrainIntents() immediately after executeSpecialBody_Apply returns.
//
// Fields used per-verb:
//   POWERDOWN / EJECT / GUARD / WITHDRAW  — warriorId, slot, orderType, brainTick, sourceBodyId
//   MOVETO_POINT                           — + waypoint[3]
//   ATTACK_OBJECT                          — + targetWID
//
// Unused fields are zeroed at construction time.
struct BrainOrderIntent {
    int               warriorId;    // vehicleWID of the emitting warrior
    BrainIntentSlot   slot;         // always GENERAL for the 6 current verbs
    TacticalOrderCode orderType;    // which TACTICAL_ORDER_* to issue on commit
    int               targetWID;    // ATTACK_OBJECT: target watch-ID; -1 otherwise
    float             waypoint[3];  // MOVETO_POINT: x/y/z destination; zeroed otherwise
    uint32_t          sourceBodyId; // 0 = root body; future: index into SpecialIndex
    uint32_t          brainTick;    // s_brainTickIndex at time of emission
};

// ---------------------------------------------------------------------------
// Per-warrior pending intent buffer capacity.
// Fixed cap 4: at most one intent per effect per mission due to once-guards;
// the buffer is sized to hold all 6 verbs if they somehow all fired together
// (defensive), plus 2 spare.  Inline in MechBrainRuntime (no heap).
static constexpr int kBrainIntentCap = 4;

// ---------------------------------------------------------------------------
// Accessor for the monotonic brain tick counter defined in warrior.cpp.
// Used by the emit path to stamp each intent with the current tick index.
// Declared here so brain_special_dispatch.cpp can call it without #include warrior.cpp.
uint32_t getBrainTickIndex();
