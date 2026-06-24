#pragma once
#ifndef WARRIOR_H
#define WARRIOR_H
// BRAIN-DISPATCH-HARNESS-1: warrior.h stub
// Provides MechWarrior — only what executeSpecialBody_Apply calls.
// Does NOT include mclib.h or any heavy engine headers.

#include "tacordr.h"
#include "gameobj.h"
#include "mech_brain_runtime.h"   // VarStore, MechBrainRuntime (LEAF header)

class MechWarrior {
public:
    virtual ~MechWarrior() = default;

    // Called by executeSpecialBody_Apply (all 6 effect verbs)
    virtual void setGeneralTacOrder(const TacticalOrder& order) {}

    // Called by executeSpecialBody_Apply (friendly-fire check)
    virtual TeamPtr getTeam() { return nullptr; }

    // Called by executeSpecialBody_Apply (self-attack check via warrior->getTeam())
    virtual long getWatchID(bool assign = true) { return 0; }
};

#endif // WARRIOR_H
