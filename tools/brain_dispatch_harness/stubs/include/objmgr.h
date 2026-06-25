#pragma once
#ifndef OBJMGR_H
#define OBJMGR_H
// BRAIN-DISPATCH-HARNESS-1: objmgr.h stub
// Provides ObjectManager global pointer + getByWatchID.
// No engine header dependencies.

#include "gameobj.h"

class GameObjectManager {
public:
    virtual ~GameObjectManager() = default;
    virtual GameObjectPtr getByWatchID(long watchID) { return nullptr; }
};

typedef GameObjectManager* GameObjectManagerPtr;

// Global singleton pointer — defined in stubs/objmgr_stub.cpp
extern GameObjectManagerPtr ObjectManager;

#endif // OBJMGR_H
