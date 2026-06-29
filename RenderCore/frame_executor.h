#pragma once
// FRAME-GRAPH-EXECUTOR-ISLAND-1 — pure/constexpr IslandContract descriptor.
//
// Declares WHAT the executor validates for each owned island; contains no GL
// includes and no game-side runtime state. The GL-touching wrapper lives in
// gos_postprocess.cpp (executorOwnBegin / executorOwnEnd) where it can access
// gosPostProcess members. This header is offline-testable (tests/unit/test_frame_graph.cpp).
//
// Design note: deliberately NOT in gos_FrameCtx (read-only mirror, no executor
// state may be injected). Counters are standalone process-statics exposed via
// extern "C" mc2_framegraph_executor_owned_passes() / _validation_failures().
//
// Gate: MC2_FRAMEGRAPH_EXECUTOR (default-OFF). When unset -> executorOwnBegin /
// executorOwnEnd are early-return no-ops -> byte-identical to the raw pp->endScene() call.

#include "RenderPassContract.h"   // RenderPassId

namespace RenderCore { namespace framegraph {

// IslandContract — what the executor validates for a single owned pass.
// All fields are plain bool; no GL, no runtime state.
struct IslandContract {
    RenderPassId id;
    bool requiresProgramValid;    // assert pp->executorCompositeProgramValid() before call
    bool requiresSceneColorTex;   // assert pp->executorSceneColorTexValid() before call
    bool warnIfNoTerrainLatch;    // warn (non-fatal) if pp->executorSceneHasTerrain()==false
    bool postRequiresDefaultFbo;  // assert GL_DRAW_FRAMEBUFFER_BINDING==0 after call
};

// Compile-time table of owned islands.  ONE row for now (PostProcess).
// Add rows here as more islands are claimed by the executor.
static constexpr IslandContract kExecutorIslands[] = {
    {
        /*id*/                  RenderPassId::PostProcess,
        /*requiresProgramValid*/true,
        /*requiresSceneColorTex*/true,
        /*warnIfNoTerrainLatch*/ true,   // the sub-stages bail gracefully; WARN not FATAL
        /*postRequiresDefaultFbo*/true,  // composite blit explicitly binds FBO 0 at end
    },
};
static constexpr unsigned kExecutorIslandCount =
    sizeof(kExecutorIslands) / sizeof(kExecutorIslands[0]);

// Find the IslandContract for the given pass, or nullptr if not owned.
// Pure, constexpr-compatible, no GL.
inline const IslandContract* findIslandContract(RenderPassId id) {
    for (unsigned i = 0; i < kExecutorIslandCount; ++i) {
        if (kExecutorIslands[i].id == id)
            return &kExecutorIslands[i];
    }
    return nullptr;
}

}} // namespace RenderCore::framegraph
