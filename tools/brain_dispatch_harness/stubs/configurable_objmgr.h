#pragma once
// BRAIN-DISPATCH-HARNESS-V2: ConfigurableGameObjectManager — fake object table for ATTACK guard tests.
// Harness driver populates fakeObjects before calling Apply.

#include "objmgr.h"
#include "gameobj.h"
#include <vector>

// A fake GameObject with configurable WID and team.
struct FakeGameObject : public GameObject {
    long    fakeWID  = 0;
    TeamPtr fakeTeam = nullptr;

    long    getWatchID(bool /*assign*/ = true) override { return fakeWID; }
    TeamPtr getTeam() override { return fakeTeam; }
};

// Configurable manager — has a small table of FakeGameObject*.
// getByWatchID returns the matching entry or nullptr.
class ConfigurableGameObjectManager : public GameObjectManager {
public:
    struct Entry {
        long            wid;
        FakeGameObject* obj; // non-owning pointer
    };
    std::vector<Entry> table;

    void addObject(long wid, FakeGameObject* obj) {
        table.push_back({wid, obj});
    }

    void clear() { table.clear(); }

    GameObjectPtr getByWatchID(long watchID) override {
        for (auto& e : table)
            if (e.wid == watchID) return e.obj;
        return nullptr;
    }
};
