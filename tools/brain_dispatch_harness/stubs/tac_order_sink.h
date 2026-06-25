#pragma once
// BRAIN-DISPATCH-HARNESS-V2: TacOrderSink — records setGeneralTacOrder calls offline.
// Used by RecordingMechWarrior to capture what orders were issued (gate-OFF or gate-ON via commit).
// Plain structs + vectors, no engine deps.

#include "tacordr.h"
#include <vector>
#include <string>
#include <cstring>

// One recorded order call.
struct RecordedOrder {
    TacticalOrderCode code;
    long              targetWID;   // for ATTACK_OBJECT; 0 otherwise
    float             waypointX, waypointY, waypointZ; // for MOVETO_POINT
};

// Sink accumulates all setGeneralTacOrder calls for one Apply+Commit run.
struct TacOrderSink {
    std::vector<RecordedOrder> orders;
    std::vector<std::string>   traceLines; // stderr captured by captureStderr()

    void clear() { orders.clear(); traceLines.clear(); }

    // Count orders with a specific code.
    int countCode(TacticalOrderCode c) const {
        int n = 0;
        for (const auto& o : orders)
            if (o.code == c) ++n;
        return n;
    }

    bool empty() const { return orders.empty(); }
};

// Global sink pointer — set by harness driver before each Apply run.
// RecordingMechWarrior writes into it.
extern TacOrderSink* g_activeSink;
