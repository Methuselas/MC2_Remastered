#pragma once
// move_recon.h — MC2_MOVE_RECON per-frame + atexit pathfinding cost instrumentation.
// Default OFF (zero behavior change when MC2_MOVE_RECON is unset).
// State lives in mclib so both mclib (move.h inlines) and code/ (mover/mech/warrior/objmgr)
// can reference it without creating a dependency in the wrong direction.

#include <chrono>
#include <cstdint>

// Master enable flag — set once from getenv("MC2_MOVE_RECON") on first moveReconFrameTick().
extern bool g_moveReconEnabled;

// Monotonic (lifetime) accumulators — never reset.
extern uint64_t g_moveRecon_ctrl_ns;
extern uint64_t g_moveRecon_pathlock_ns;
extern uint64_t g_moveRecon_astar_local_ns;
extern uint64_t g_moveRecon_astar_global_ns;

// Monotonic counters.
extern uint64_t g_moveRecon_movers_total;
extern uint64_t g_moveRecon_calcMovePath_calls;
extern uint64_t g_moveRecon_astar_local_calls;
extern uint64_t g_moveRecon_astar_global_calls;
extern uint64_t g_moveRecon_setPathlock_calls;
extern uint64_t g_moveRecon_getPathlock_calls;
extern uint64_t g_moveRecon_offmap_noops;
extern uint64_t g_moveRecon_frames;

// Global-A* (GlobalMap::calcPath) internal split — sizes the per-call FIXED
// reinit cost (door-clear loop O(numDoors) + gate-scan loop O(numAreas)) vs the
// actual A* search (while-openList). Hypothesis: dense-urban prologue dominates.
extern uint64_t g_moveRecon_ag_prologue_ns;   // door-clear + gate-scan reinit
extern uint64_t g_moveRecon_ag_search_ns;     // the while(!openList->isEmpty)
extern uint64_t g_moveRecon_ag_nodesPopped;   // total A* nodes expanded
extern uint64_t g_moveRecon_ag_gatesScanned;  // total gate-areas open/closed
extern uint64_t g_moveRecon_ag_maxNumDoors;   // map size (last/max seen)
extern uint64_t g_moveRecon_ag_maxNumAreas;
extern uint64_t g_moveRecon_ag_max_prologue_ns; // worst single-call prologue
extern uint64_t g_moveRecon_ag_max_search_ns;   // worst single-call search

// Per-frame accumulators (reset each frame by moveReconFrameTick).
extern uint64_t g_moveRecon_frame_ctrl_ns;
extern uint64_t g_moveRecon_frame_pathlock_ns;
extern uint64_t g_moveRecon_frame_astar_local_ns;
extern uint64_t g_moveRecon_frame_astar_global_ns;
extern uint64_t g_moveRecon_frame_movers;

// Per-frame maxima (rolling maximum across all frames, never reset).
extern uint64_t g_moveRecon_max_ctrl_ns;
extern uint64_t g_moveRecon_max_pathlock_ns;
extern uint64_t g_moveRecon_max_astar_local_ns;
extern uint64_t g_moveRecon_max_astar_global_ns;

// Called once per frame at end of GameObjectManager::update.
// Lazily inits, registers atexit on first call, emits every 30 frames, resets per-frame accums.
void moveReconFrameTick();

// atexit handler — prints shutdown summary to stdout.
void moveReconEmit();

// RAII scope timer — no-op when !g_moveReconEnabled.
struct MoveReconScope {
    uint64_t* acc_mono;
    uint64_t* acc_frame;
    std::chrono::steady_clock::time_point t0;

    MoveReconScope(uint64_t* mono, uint64_t* frame)
        : acc_mono(mono), acc_frame(frame)
    {
        if (g_moveReconEnabled)
            t0 = std::chrono::steady_clock::now();
    }

    ~MoveReconScope()
    {
        if (g_moveReconEnabled) {
            auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            *acc_mono  += ns;
            *acc_frame += ns;
        }
    }
};
