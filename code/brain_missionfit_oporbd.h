#pragma once
// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: pure parser for carver's declarative OPORD config.
//
// carver_v_enhanced's mission.fit carries, inline in each [WarriorN] section, a brace-block
//   Brain { … PrimaryOPORD { type=Patrol; loop=true; Waypoint{index=0;x=…;y=…} … } … }
// This is the RESOLVED, literal patrol/guard config (the symbolic tick-script args resolve to
// exactly these values). FitIniFile cannot see brace-blocks (flat [section]/typed-prefix
// reader), so this parser RAW-SCANS the mission.fit text and extracts the PrimaryOPORD per
// warrior, to drive the existing patrol/guard systems.
//
// PURE: no engine headers, no globals, no I/O. Unit-tested in isolation
// (tools/brain_missionfit_oporbd_test). Scope of slice 1: PrimaryOPORD type + loop +
// Waypoints (the patrol/guard essentials). SecondaryOPORD / Tactics deferred.

#include <cstdint>

static constexpr int kMissionFitMaxWaypoints = 8;

struct MissionFitWaypoint { float x; float y; };

struct MissionFitOpord {
    int   warriorIndex;                              // N from [WarriorN]
    char  primaryType[24];                           // PrimaryOPORD type token, '\0' if absent
    bool  loop;                                      // PrimaryOPORD loop= (patrol); false if absent
    int   waypointCount;                             // 0..kMissionFitMaxWaypoints
    MissionFitWaypoint waypoints[kMissionFitMaxWaypoints];
};

// Parse all [WarriorN] inline Brain{} PrimaryOPORD blocks from mission.fit text.
// Writes one record per warrior that has a PrimaryOPORD block, into out[0..maxOut).
// Returns the number of records written (>=0). Warriors without a Brain{}/PrimaryOPORD
// are skipped. Waypoints beyond kMissionFitMaxWaypoints are ignored (count clamps).
int parseMissionFitOpords(const char* text, MissionFitOpord* out, int maxOut);
