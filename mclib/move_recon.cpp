// move_recon.cpp — MC2_MOVE_RECON pathfinding cost instrumentation implementation.
// Default OFF. Zero behavior change when MC2_MOVE_RECON is unset.

#include "move_recon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// -----------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------

bool     g_moveReconEnabled        = false;

uint64_t g_moveRecon_ctrl_ns             = 0;
uint64_t g_moveRecon_pathlock_ns         = 0;
uint64_t g_moveRecon_astar_local_ns      = 0;
uint64_t g_moveRecon_astar_global_ns     = 0;

uint64_t g_moveRecon_movers_total        = 0;
uint64_t g_moveRecon_calcMovePath_calls  = 0;
uint64_t g_moveRecon_astar_local_calls   = 0;
uint64_t g_moveRecon_astar_global_calls  = 0;
uint64_t g_moveRecon_setPathlock_calls   = 0;
uint64_t g_moveRecon_getPathlock_calls   = 0;
uint64_t g_moveRecon_offmap_noops        = 0;
uint64_t g_moveRecon_frames              = 0;

uint64_t g_moveRecon_frame_ctrl_ns          = 0;
uint64_t g_moveRecon_frame_pathlock_ns      = 0;
uint64_t g_moveRecon_frame_astar_local_ns   = 0;
uint64_t g_moveRecon_frame_astar_global_ns  = 0;
uint64_t g_moveRecon_frame_movers           = 0;

uint64_t g_moveRecon_max_ctrl_ns            = 0;
uint64_t g_moveRecon_max_pathlock_ns        = 0;
uint64_t g_moveRecon_max_astar_local_ns     = 0;
uint64_t g_moveRecon_max_astar_global_ns    = 0;

uint64_t g_moveRecon_ag_prologue_ns         = 0;
uint64_t g_moveRecon_ag_search_ns           = 0;
uint64_t g_moveRecon_ag_nodesPopped         = 0;
uint64_t g_moveRecon_ag_gatesScanned        = 0;
uint64_t g_moveRecon_ag_maxNumDoors         = 0;
uint64_t g_moveRecon_ag_maxNumAreas         = 0;
uint64_t g_moveRecon_ag_max_prologue_ns     = 0;
uint64_t g_moveRecon_ag_max_search_ns       = 0;

// -----------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------

static bool s_initialized   = false;
static bool s_atexitRegistered = false;

// Forward-declared here; GameMap/GlobalMoveMap types are in move.h which
// is not included from this file to keep includes minimal.  We obtain the
// dimensions via weak external symbols set by moveReconFrameTick callers.
int g_moveRecon_gameMapW = 0;
int g_moveRecon_gameMapH = 0;

// -----------------------------------------------------------------------
// moveReconEmit — atexit handler
// -----------------------------------------------------------------------

void moveReconEmit()
{
    if (!g_moveReconEnabled)
        return;

    std::printf(
        "[MOVE_RECON v1] event=shutdown"
        " frames=%llu"
        " movers_total=%llu"
        " ctrl_ns=%llu"
        " pathlock_ns=%llu"
        " astar_local_ns=%llu"
        " astar_global_ns=%llu"
        " calcMovePath_calls=%llu"
        " astar_local_calls=%llu"
        " astar_global_calls=%llu"
        " setPathlock_calls=%llu"
        " getPathlock_calls=%llu"
        " offmap_noops=%llu"
        " max_ctrl_ns=%llu"
        " max_pathlock_ns=%llu"
        " max_astar_local_ns=%llu"
        " max_astar_global_ns=%llu"
        " ag_prologue_ns=%llu"
        " ag_search_ns=%llu"
        " ag_nodesPopped=%llu"
        " ag_gatesScanned=%llu"
        " ag_numDoors=%llu"
        " ag_numAreas=%llu"
        " ag_max_prologue_ns=%llu"
        " ag_max_search_ns=%llu\n",
        (unsigned long long)g_moveRecon_frames,
        (unsigned long long)g_moveRecon_movers_total,
        (unsigned long long)g_moveRecon_ctrl_ns,
        (unsigned long long)g_moveRecon_pathlock_ns,
        (unsigned long long)g_moveRecon_astar_local_ns,
        (unsigned long long)g_moveRecon_astar_global_ns,
        (unsigned long long)g_moveRecon_calcMovePath_calls,
        (unsigned long long)g_moveRecon_astar_local_calls,
        (unsigned long long)g_moveRecon_astar_global_calls,
        (unsigned long long)g_moveRecon_setPathlock_calls,
        (unsigned long long)g_moveRecon_getPathlock_calls,
        (unsigned long long)g_moveRecon_offmap_noops,
        (unsigned long long)g_moveRecon_max_ctrl_ns,
        (unsigned long long)g_moveRecon_max_pathlock_ns,
        (unsigned long long)g_moveRecon_max_astar_local_ns,
        (unsigned long long)g_moveRecon_max_astar_global_ns,
        (unsigned long long)g_moveRecon_ag_prologue_ns,
        (unsigned long long)g_moveRecon_ag_search_ns,
        (unsigned long long)g_moveRecon_ag_nodesPopped,
        (unsigned long long)g_moveRecon_ag_gatesScanned,
        (unsigned long long)g_moveRecon_ag_maxNumDoors,
        (unsigned long long)g_moveRecon_ag_maxNumAreas,
        (unsigned long long)g_moveRecon_ag_max_prologue_ns,
        (unsigned long long)g_moveRecon_ag_max_search_ns
    );
    std::fflush(stdout);
}

// -----------------------------------------------------------------------
// moveReconFrameTick — call once per frame at end of GOM::update
// -----------------------------------------------------------------------

void moveReconFrameTick()
{
    // Lazy init on first call (safe: GOM::update is the guaranteed-run path).
    if (!s_initialized) {
        s_initialized = true;
        const char* env = std::getenv("MC2_MOVE_RECON");
        g_moveReconEnabled = (env != nullptr && env[0] != '\0' && env[0] != '0');
        if (g_moveReconEnabled && !s_atexitRegistered) {
            std::atexit(moveReconEmit);
            s_atexitRegistered = true;
        }
        if (g_moveReconEnabled) {
            std::printf("[MOVE_RECON v1] init enabled\n");
            std::fflush(stdout);
        }
    }

    if (!g_moveReconEnabled)
        return;

    g_moveRecon_frames++;

    // Update rolling maxima.
    if (g_moveRecon_frame_ctrl_ns         > g_moveRecon_max_ctrl_ns)
        g_moveRecon_max_ctrl_ns         = g_moveRecon_frame_ctrl_ns;
    if (g_moveRecon_frame_pathlock_ns     > g_moveRecon_max_pathlock_ns)
        g_moveRecon_max_pathlock_ns     = g_moveRecon_frame_pathlock_ns;
    if (g_moveRecon_frame_astar_local_ns  > g_moveRecon_max_astar_local_ns)
        g_moveRecon_max_astar_local_ns  = g_moveRecon_frame_astar_local_ns;
    if (g_moveRecon_frame_astar_global_ns > g_moveRecon_max_astar_global_ns)
        g_moveRecon_max_astar_global_ns = g_moveRecon_frame_astar_global_ns;

    // Emit every 30 frames.
    if (g_moveRecon_frames % 30 == 0) {
        std::printf(
            "[MOVE_RECON v1] frame=%llu"
            " movers=%llu"
            " ctrl_ns=%llu"
            " pathlock_ns=%llu"
            " astar_local_ns=%llu"
            " astar_global_ns=%llu"
            " calcMovePath_calls=%llu"
            " offmap_noops=%llu\n",
            (unsigned long long)g_moveRecon_frames,
            (unsigned long long)g_moveRecon_frame_movers,
            (unsigned long long)g_moveRecon_frame_ctrl_ns,
            (unsigned long long)g_moveRecon_frame_pathlock_ns,
            (unsigned long long)g_moveRecon_frame_astar_local_ns,
            (unsigned long long)g_moveRecon_frame_astar_global_ns,
            (unsigned long long)g_moveRecon_calcMovePath_calls,
            (unsigned long long)g_moveRecon_offmap_noops
        );
        std::fflush(stdout);
    }

    // Reset per-frame accumulators.
    g_moveRecon_frame_ctrl_ns         = 0;
    g_moveRecon_frame_pathlock_ns     = 0;
    g_moveRecon_frame_astar_local_ns  = 0;
    g_moveRecon_frame_astar_global_ns = 0;
    g_moveRecon_frame_movers          = 0;
}
