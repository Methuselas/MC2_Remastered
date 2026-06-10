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

uint64_t g_moveRecon_al_nodesPopped         = 0;
uint64_t g_moveRecon_al_maxNodes            = 0;
uint64_t g_moveRecon_al_goalFound           = 0;
uint64_t g_moveRecon_al_goalMissed          = 0;

uint64_t g_chunkShadow_calls          = 0;
uint64_t g_chunkShadow_inside0        = 0;
uint64_t g_chunkShadow_inside1        = 0;
uint64_t g_chunkShadow_inside2        = 0;
uint64_t g_chunkShadow_sum_poppedBox  = 0;
uint64_t g_chunkShadow_sum_pathBox    = 0;
uint64_t g_chunkShadow_sum_nodes      = 0;
uint64_t g_chunkShadow_sum_pathLen    = 0;
uint64_t g_chunkShadow_maxOverflow    = 0;

// Threshold: only sample the expensive tail (the 2ms Warrior.Path calls).
static const int CHUNK_SHADOW_MIN_NODES = 200;
static const int CHUNK_SHADOW_CHUNK_SIZE = 16;
static int s_chunkShadowSamplePrints = 0;  // cap verbose per-call lines

void moveReconChunkSample(int startR, int startC, int goalR, int goalC,
                          unsigned long long nodes, int pathLen,
                          int poppedMinR, int poppedMaxR, int poppedMinC, int poppedMaxC,
                          int pathMinR, int pathMaxR, int pathMinC, int pathMaxC)
{
    if (!g_moveReconEnabled || nodes < (unsigned long long)CHUNK_SHADOW_MIN_NODES)
        return;

    const int cs = CHUNK_SHADOW_CHUNK_SIZE;
    uint64_t poppedBox = (uint64_t)(poppedMaxR - poppedMinR + 1) * (uint64_t)(poppedMaxC - poppedMinC + 1);

    int overflow = -1;          // -1 = goal missed (no path)
    uint64_t pathBox = 0;
    if (pathLen > 0) {
        pathBox = (uint64_t)(pathMaxR - pathMinR + 1) * (uint64_t)(pathMaxC - pathMinC + 1);
        // Start<->goal chunk rectangle.
        int sgMinR = (startR < goalR ? startR : goalR) / cs;
        int sgMaxR = (startR > goalR ? startR : goalR) / cs;
        int sgMinC = (startC < goalC ? startC : goalC) / cs;
        int sgMaxC = (startC > goalC ? startC : goalC) / cs;
        // Final-path chunk bbox.
        int pMinR = pathMinR / cs, pMaxR = pathMaxR / cs;
        int pMinC = pathMinC / cs, pMaxC = pathMaxC / cs;
        // Chunks the path strays beyond the start-goal rectangle (any side).
        int ofR0 = sgMinR - pMinR, ofR1 = pMaxR - sgMaxR;
        int ofC0 = sgMinC - pMinC, ofC1 = pMaxC - sgMaxC;
        overflow = 0;
        if (ofR0 > overflow) overflow = ofR0;
        if (ofR1 > overflow) overflow = ofR1;
        if (ofC0 > overflow) overflow = ofC0;
        if (ofC1 > overflow) overflow = ofC1;
    }

    g_chunkShadow_calls++;
    g_chunkShadow_sum_poppedBox += poppedBox;
    g_chunkShadow_sum_pathBox   += pathBox;
    g_chunkShadow_sum_nodes     += nodes;
    g_chunkShadow_sum_pathLen   += (pathLen > 0 ? (uint64_t)pathLen : 0);
    if (overflow >= 0) {
        if (overflow == 0) g_chunkShadow_inside0++;
        if (overflow <= 1) g_chunkShadow_inside1++;
        if (overflow <= 2) g_chunkShadow_inside2++;
        if ((uint64_t)overflow > g_chunkShadow_maxOverflow) g_chunkShadow_maxOverflow = (uint64_t)overflow;
    }

    if (s_chunkShadowSamplePrints < 40) {
        s_chunkShadowSamplePrints++;
        std::printf("[MOVE_CHUNK_SHADOW v1] call=%llu start=(%d,%d) goal=(%d,%d) "
            "cell_nodes=%llu cell_path_len=%d popped_box=%llu path_box=%llu "
            "chunk_size=%d overflow_chunks=%d\n",
            (unsigned long long)g_chunkShadow_calls, startR, startC, goalR, goalC,
            nodes, pathLen, (unsigned long long)poppedBox, (unsigned long long)pathBox,
            cs, overflow);
        std::fflush(stdout);
    }
}

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
        " ag_max_search_ns=%llu"
        " al_nodesPopped=%llu"
        " al_maxNodes=%llu"
        " al_goalFound=%llu"
        " al_goalMissed=%llu\n",
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
        (unsigned long long)g_moveRecon_ag_max_search_ns,
        (unsigned long long)g_moveRecon_al_nodesPopped,
        (unsigned long long)g_moveRecon_al_maxNodes,
        (unsigned long long)g_moveRecon_al_goalFound,
        (unsigned long long)g_moveRecon_al_goalMissed
    );
    std::fflush(stdout);

    std::printf(
        "[MOVE_CHUNK_SHADOW v1] event=shutdown"
        " sampled_calls=%llu"
        " inside0=%llu inside1=%llu inside2=%llu"
        " max_overflow_chunks=%llu"
        " sum_popped_box=%llu sum_path_box=%llu"
        " sum_nodes=%llu sum_path_len=%llu"
        " (chunk_size=%d node_threshold=%d)\n",
        (unsigned long long)g_chunkShadow_calls,
        (unsigned long long)g_chunkShadow_inside0,
        (unsigned long long)g_chunkShadow_inside1,
        (unsigned long long)g_chunkShadow_inside2,
        (unsigned long long)g_chunkShadow_maxOverflow,
        (unsigned long long)g_chunkShadow_sum_poppedBox,
        (unsigned long long)g_chunkShadow_sum_pathBox,
        (unsigned long long)g_chunkShadow_sum_nodes,
        (unsigned long long)g_chunkShadow_sum_pathLen,
        CHUNK_SHADOW_CHUNK_SIZE, CHUNK_SHADOW_MIN_NODES);
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
