#pragma once
// SAME-ORDER-EXECUTOR-VALIDATE-1 — pure/constexpr top-level pass executor descriptor.
//
// Declares WHICH top-level (frame-order) passes the executor validate-owns and what
// preconditions to check. Contains NO GL, NO runtime state. The GL-touching runtime
// wrapper lives in mclib/render_contract.cpp (executorOwnBeginTopLevel /
// executorOwnEndTopLevel). This header is offline-testable (tests/unit/test_frame_graph.cpp).
//
// Design note: VALIDATE-ONLY. The body still sets its own state; the executor wraps,
// validates preconditions + postconditions, and counts. NO glApplyState, NO reorder,
// NO scheduling.
//
// Gate: MC2_FRAMEGRAPH_EXECUTOR (default-OFF). When unset -> executorOwnBeginTopLevel /
// executorOwnEndTopLevel are early-return no-ops -> byte-identical to unwrapped draw.
//
// DEFERRED (beyond slice 2): Water, VFX, UI (ambient/FBO gaps per same-order recon).
// Shadow and MechOpaque are NOW OWNED in this slice (SAME-ORDER-EXECUTOR-SLICE-2):
//   Shadow   — dynamic-shadow seam: gos_BeginDynamicShadowPass / gos_EndDynamicShadowPass
//              (txmmgr.cpp); noteRenderPass(ShadowCaster) fires after FBO bind in
//              gosRenderer::beginDynamicShadowPass (SHADOW-OBSERVE-3).
//   MechOpaque — GpuMechBatcher::flush() seam in renderLists(); noteRenderPass(OpaqueObject)
//              relocated from preamble to the real draw site (MECHOPAQUE-NOTE-RELOCATE-1).

#include "RenderCore/RenderPassContract.h"  // RenderPassId

namespace RenderCore { namespace framegraph {

// What the executor validates at begin/end of a top-level pass.
// All fields are plain bool; no GL, no runtime state.
struct TopLevelPassContract {
    RenderPassId id;
    bool         validateAmbient; // check ambient ledger (colorMask/depthFunc/depthWrite) on begin
    bool         validateFbo;     // check FBO ledger (declared vs live draw FBO) on begin
    const char*  note;            // human-readable label (for log/debug only)
};

// Compile-time table of top-level passes the executor validate-owns this slice.
// Order is documentary; the executor does NOT reorder.
//
// VegetationCards: gated MC2_VEGETATION_CARDS default-OFF -> legitimately fires 0 times
// per frame in default smoke. That is correct and NOT a validation failure.
// Shadow: dynamic-shadow only (per-frame). Static shadow (once/mission) is NOT wrapped
// here — it is not a per-frame top-level pass. The seam is gos_Begin/EndDynamicShadowPass
// in txmmgr.cpp renderLists(). Shadow FBO = ShadowDynamicMap (registered in gos_postprocess).
// MechOpaque: wraps GpuMechBatcher::flush() in renderLists(). FBO = MainColor (scene FBO).
static constexpr TopLevelPassContract kTopLevelExecutorPasses[] = {
    {
        /*id*/              RenderPassId::Shadow,
        /*validateAmbient*/ true,   // AmbientContract row: ShadowLess depthFunc, ShadowMap viewport
        /*validateFbo*/     true,   // FBO ledger: ShadowDynamicMap registered in gos_postprocess
        /*note*/            "Shadow/gosRenderer_beginDynamicShadowPass",
    },
    {
        /*id*/              RenderPassId::MechOpaque,
        /*validateAmbient*/ true,   // AmbientContract row: SceneGEqual depthFunc, MainScene viewport
        /*validateFbo*/     true,   // FBO ledger: MainColor (scene FBO, established by terrain)
        /*note*/            "MechOpaque/GpuMechBatcher_flush",
    },
    {
        /*id*/              RenderPassId::StaticPropOpaque,
        /*validateAmbient*/ true,   // AmbientContract row exists (SceneGEqual, depthWrite=On)
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "StaticPropOpaque/GpuStaticPropBatcher_flush",
    },
    {
        /*id*/              RenderPassId::Terrain,
        /*validateAmbient*/ true,   // AmbientContract row exists (SceneGEqual, depthWrite=On, AllOn colorMask)
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "Terrain/gos_TerrainLodChunk_SubmitDrawCommands",
    },
    {
        /*id*/              RenderPassId::TerrainOverlay,
        /*validateAmbient*/ false,  // no AmbientContract row for TerrainOverlay
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "TerrainOverlay/gosRenderer_drawTerrainOverlays",
    },
    {
        /*id*/              RenderPassId::TerrainDecal,
        /*validateAmbient*/ false,  // no AmbientContract row for TerrainDecal
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "TerrainDecal/gosRenderer_drawDecals",
    },
    {
        /*id*/              RenderPassId::Water,
        /*validateAmbient*/ true,   // AmbientContract row: SceneGEqual depthFunc, depthWrite On, MainScene viewport
        /*validateFbo*/     true,   // FBO ledger declares MainColor target (scene FBO)
        /*note*/            "Water/gosRenderer_renderWaterFastPath",
    },
    {
        /*id*/              RenderPassId::VegetationCards,
        /*validateAmbient*/ false,  // DRYRUN-OBSERVE-COVERAGE-1: no AmbientContract row
        /*validateFbo*/     false,  // no declared FBO target for VegetationCards
        /*note*/            "VegetationCards/GosVegetation_flush",
    },
    {
        /*id*/              RenderPassId::VFX,
        /*validateAmbient*/ false,  // VFX-FBO-ONLY-VALIDATE-1: note seam (gamecam:594) fires
                                    // pre-body in a different TU -> ambient not honestly declarable
        /*validateFbo*/     true,   // FBO ledger declares MainColor (scene HDR FBO bound across whole flush window)
        /*note*/            "VFX/gamecam_particlesFlush",
    },
    {
        /*id*/              RenderPassId::UI,
        /*validateAmbient*/ false,  // UI-SAME-ORDER-VALIDATE-1: UI ambient is per-draw legacy gos
                                    // dynamic state (DO_NOT_MODEL) -> no AmbientContract row
        /*validateFbo*/     true,   // FBO ledger declares Backbuffer (default FBO 0, post-composite)
        /*note*/            "UI/gosRenderer_flushHUDBatch",
    },
};
static constexpr unsigned kTopLevelExecutorPassCount =
    sizeof(kTopLevelExecutorPasses) / sizeof(kTopLevelExecutorPasses[0]);

// Find the TopLevelPassContract for the given pass id, or nullptr if not executor-owned.
// Pure, constexpr-compatible, no GL.
inline const TopLevelPassContract* findTopLevelExecutorPass(RenderPassId id) {
    for (unsigned i = 0; i < kTopLevelExecutorPassCount; ++i) {
        if (kTopLevelExecutorPasses[i].id == id)
            return &kTopLevelExecutorPasses[i];
    }
    return nullptr;
}

// Count of deferred top-level passes (not executor-owned this slice).
// none = 0. (UI now owned FBO-only in UI-SAME-ORDER-VALIDATE-1 -> EVERY top-level pass is
// executor-owned; VFX in VFX-FBO-ONLY-VALIDATE-1; Water in WATER-SAME-ORDER-VALIDATE-1;
// Shadow + MechOpaque in SLICE-2.)
// Called by the dump to populate executor_skipped_deferred_passes.
static constexpr unsigned kTopLevelDeferredPassCount = 0u;

}} // namespace RenderCore::framegraph
