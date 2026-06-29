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
//
// FRAME-GRAPH-EXECUTOR-ISLAND-2: re-keyed IslandContract to ExecutorIslandId (owns
// sub-stage islands EdgeFog + FogOob in addition to PostProcess).

namespace RenderCore { namespace framegraph {

// Dedicated island id — independent of RenderPassId so sub-stage islands
// (which live inside a single RenderPassId::PostProcess frame slot) can each
// have their own contract row.
enum class ExecutorIslandId : uint8_t {
    PostProcess = 0,  // outer composite blit (ISLAND-1)
    EdgeFog,          // sub-stage: map-boundary edge fog (ISLAND-2)
    FogOob,           // sub-stage: out-of-bounds ground fog (ISLAND-2)
    Count,
};

// IslandContract — what the executor validates for a single owned island.
// All fields are plain bool; no GL, no runtime state.
struct IslandContract {
    ExecutorIslandId id;
    bool requiresProgramValid;      // assert program is compiled+valid before call
    bool requiresSceneColorTex;     // assert sceneColorTex_ != 0 before call
    bool requiresSceneDepthTex;     // assert sceneDepthTex_ != 0 before call (EdgeFog/FogOob)
    bool warnIfNoTerrainLatch;      // warn (non-fatal) if sceneHasTerrain_==false
    bool postRequiresDefaultFbo;    // assert GL_DRAW_FRAMEBUFFER_BINDING==0 after call
    bool postRequiresBlendDisabled; // assert GL_BLEND disabled after call
    bool postRequiresActiveTexture0;// assert active texture unit == GL_TEXTURE0 after call
};

// Compile-time table of owned islands.
static constexpr IslandContract kExecutorIslands[] = {
    {
        /*id*/                      ExecutorIslandId::PostProcess,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   true,
        /*requiresSceneDepthTex*/   false,
        /*warnIfNoTerrainLatch*/    true,   // sub-stages bail gracefully; WARN not FATAL
        /*postRequiresDefaultFbo*/  true,   // composite blit explicitly binds FBO 0 at end
        /*postRequiresBlendDisabled*/false, // PostProcess end-state not asserted (composite varies)
        /*postRequiresActiveTexture0*/false,
    },
    {
        /*id*/                      ExecutorIslandId::EdgeFog,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   false,
        /*requiresSceneDepthTex*/   true,   // EdgeFog reads sceneDepthTex_, not sceneColorTex_
        /*warnIfNoTerrainLatch*/    true,   // runEdgeFog() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/  false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,  // runEdgeFog() calls glDisable(GL_BLEND) on exit
        /*postRequiresActiveTexture0*/true, // runEdgeFog() calls glActiveTexture(GL_TEXTURE0) on exit
    },
    {
        /*id*/                      ExecutorIslandId::FogOob,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   false,
        /*requiresSceneDepthTex*/   true,   // FogOob reads sceneDepthTex_
        /*warnIfNoTerrainLatch*/    true,   // runFogOob() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/  false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,  // runFogOob() calls glDisable(GL_BLEND) on exit
        /*postRequiresActiveTexture0*/true, // runFogOob() calls glActiveTexture(GL_TEXTURE0) on exit
    },
};
static constexpr unsigned kExecutorIslandCount =
    sizeof(kExecutorIslands) / sizeof(kExecutorIslands[0]);

// Find the IslandContract for the given island id, or nullptr if not owned.
// Pure, constexpr-compatible, no GL.
inline const IslandContract* findIslandContract(ExecutorIslandId id) {
    for (unsigned i = 0; i < kExecutorIslandCount; ++i) {
        if (kExecutorIslands[i].id == id)
            return &kExecutorIslands[i];
    }
    return nullptr;
}

}} // namespace RenderCore::framegraph
