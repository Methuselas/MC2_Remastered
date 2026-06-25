// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: unit test for the pure mission.fit OPORD parser.
// Engine-free — compiles code/brain_missionfit_oporbd.cpp only. Exit 0 = pass, 1 = fail.
#include "brain_missionfit_oporbd.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) {
    if (ok) { ++g_pass; } else { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fail; }
}
static bool feq(float a, float b) { return std::fabs(a - b) < 0.01f; }

// A realistic carver mission.fit excerpt: 3 player units, 1 patroller (loop, 2 wps), 1 guard.
static const char* kFit =
"[Warrior1]\n"
"st Brain = \"\"\n"
"Brain {\n"
"    sourceABLBrain = \"PBrain\"\n"
"    PrimaryOPORD { type = PlayerControlled }\n"
"}\n"
"[Warrior4]\n"
"Brain {\n"
"    sourceABLBrain = \"mc2_01_Pat1\"\n"
"    PrimaryOPORD {\n"
"        type = Patrol\n"
"        loop = true\n"
"        Waypoint { index=0; x=3008.000; y=1472.000 }\n"
"        Waypoint { index=1; x=3136.000; y=960.000 }\n"
"    }\n"
"    SecondaryOPORD { type = Guard }\n"
"}\n"
"[Warrior7]\n"
"Brain {\n"
"    PrimaryOPORD { type = Guard }\n"
"}\n"
"[Warrior9]\n"          // no Brain block — must be skipped
"st Profile = \"x\"\n";

int main() {
    MissionFitOpord recs[16];
    int n = parseMissionFitOpords(kFit, recs, 16);

    check(n == 3, "record count == 3 (W1, W4, W7; W9 has no Brain)");

    // Find by warrior index (order-independent).
    const MissionFitOpord* w1 = nullptr; const MissionFitOpord* w4 = nullptr; const MissionFitOpord* w7 = nullptr;
    for (int i = 0; i < n; ++i) {
        if (recs[i].warriorIndex == 1) w1 = &recs[i];
        if (recs[i].warriorIndex == 4) w4 = &recs[i];
        if (recs[i].warriorIndex == 7) w7 = &recs[i];
    }

    check(w1 && std::strcmp(w1->primaryType, "PlayerControlled") == 0, "W1 type PlayerControlled");
    check(w1 && w1->waypointCount == 0, "W1 no waypoints");

    check(w4 && std::strcmp(w4->primaryType, "Patrol") == 0, "W4 type Patrol");
    check(w4 && w4->loop == true, "W4 loop true");
    check(w4 && w4->waypointCount == 2, "W4 2 waypoints");
    check(w4 && feq(w4->waypoints[0].x, 3008.0f) && feq(w4->waypoints[0].y, 1472.0f), "W4 wp0 = (3008,1472)");
    check(w4 && feq(w4->waypoints[1].x, 3136.0f) && feq(w4->waypoints[1].y, 960.0f), "W4 wp1 = (3136,960)");

    check(w7 && std::strcmp(w7->primaryType, "Guard") == 0, "W7 type Guard");
    check(w7 && w7->loop == false, "W7 loop false (no loop= key)");
    check(w7 && w7->waypointCount == 0, "W7 no waypoints");

    // Empty / null inputs.
    check(parseMissionFitOpords("", recs, 16) == 0, "empty text → 0");
    check(parseMissionFitOpords("[Warrior1]\nst Profile = \"x\"\n", recs, 16) == 0, "no Brain → 0");

    std::printf("brain_missionfit_oporbd_test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
