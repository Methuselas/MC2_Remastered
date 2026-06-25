#pragma once
// BRAIN-DISPATCH-HARNESS-V2: RecordingMechWarrior — records setGeneralTacOrder calls.
// Provides configurable getTeam() / getWatchID() for ATTACK guard tests.
// getBrainRuntime() returns a real MechBrainRuntime so gate-ON emit path works.

#include "warrior.h"
#include "tac_order_sink.h"
#include "mech_brain_runtime.h"
#include <cstdio>

class RecordingMechWarrior : public MechWarrior {
public:
    // Configurable identity for triple-guard testing.
    long     warriorWID = 1;        // returned by getWatchID()
    TeamPtr  warriorTeam = nullptr; // returned by getTeam()

    // Brain runtime for gate-ON intent queue path.
    MechBrainRuntime brainRuntime;

    void setGeneralTacOrder(const TacticalOrder& order) override {
        if (!g_activeSink) return;
        RecordedOrder rec;
        rec.code      = order.code;
        rec.targetWID = (long)order.targetWID;
        rec.waypointX = 0; rec.waypointY = 0; rec.waypointZ = 0;
        if (order.code == TACTICAL_ORDER_MOVETO_POINT &&
            order.moveParams.wayPath.numPoints > 0) {
            rec.waypointX = order.moveParams.wayPath.points[0];
            rec.waypointY = order.moveParams.wayPath.points[1];
            rec.waypointZ = order.moveParams.wayPath.points[2];
        }
        g_activeSink->orders.push_back(rec);
    }

    TeamPtr getTeam() override { return warriorTeam; }

    long getWatchID(bool /*assign*/ = true) override { return warriorWID; }

    MechBrainRuntime* getBrainRuntime() override { return &brainRuntime; }

    // Reset runtime state between gate-OFF / gate-ON runs.
    void resetRuntime() {
        brainRuntime.pendingIntentCount = 0;
    }
};
