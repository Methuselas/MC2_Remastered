#pragma once
// POSTPROCESS-SUBGRAPH-1: declarative sub-stage inventory for the PostProcess pass.
//
// Mirrors the kRenderPassContracts static-array + sentinel + static_assert pattern
// (RenderPassContract.h). GL-free; no game-side includes. Offline-testable.
//
// Declares the 6 Slice-1 sub-stages IN CALL ORDER from endScene() (gos_postprocess.cpp):
//   CloudShadow        (:2458)
//   Shoreline          (:2464)
//   EdgeFog            (:2477)
//   FogOob             (:2484)
//   Composite          (:2489) — SUBGRAPH OUTPUT EDGE (binds FBO 0)
//   ShadowDebugOverlay (:2653) — debug overlay on FBO 0 (post-composite)
//
// DEFERRED (Slice 2/3): HzbReduce, HzbProbe, ClusterDepthPyramid, LightgridBuild,
//   PostprocessComputeBlur, ScreenShadow, SSAO, BoxDecals.
// DEFERRED (HIGH-3): top-level kRenderPassContracts PostProcess row MainNormal producer.
//
// INVARIANTS:
//   - reads[]/writes[] are Unknown-terminated (RenderResourceId::Unknown = 0).
//   - ownedByExecutor=true -> sub-stage is wrapped by executorOwnBeginSub/EndSub.
//   - ownedByExecutor=false -> declared for documentation / offline validation only.
//   - Composite must be the sole subpass with fboTarget==Backbuffer that precedes
//     ShadowDebugOverlay; it is the subgraph's output edge (sceneFBO_->FBO0).
//   - ShadowDebugOverlay may follow Composite (FBO 0 already bound); ownedByExecutor=false.

#include <cstddef>
#include "RenderCore/RenderResourceRegistry.h"
#include "RenderCore/frame_executor.h"  // ExecutorIslandId

namespace RenderCore { namespace framegraph {

// ---------------------------------------------------------------------------
// PostProcessSubpass descriptor
// ---------------------------------------------------------------------------

struct PostProcessSubpass {
    ExecutorIslandId id;
    const char*      name;
    // Resources read.  Unknown-terminated (max 4 entries + terminator).
    RenderResourceId reads[5];
    // Resources written. Unknown-terminated (max 2 entries + terminator).
    RenderResourceId writes[3];
    // FBO target for this sub-stage (MainColor=sceneFBO_, Backbuffer=FBO0).
    RenderResourceId fboTarget;
    // true = sub-stage always runs when initialized_ is true (no env/latch gate).
    bool             conditional;
    // true = this sub-stage is wrapped by executorOwnBeginSub/EndSub.
    bool             ownedByExecutor;
    // Note for cross-reference / slice rationale (string literal).
    const char*      note;
};

// ---------------------------------------------------------------------------
// kPostProcessSubpasses — 6 Slice-1 rows, IN CALL ORDER from endScene().
// ---------------------------------------------------------------------------

static constexpr PostProcessSubpass kPostProcessSubpasses[] = {
    // -----------------------------------------------------------------------
    // 1. CloudShadow (call-site :2458)
    //    Gate: enableCloudShadow_ && sceneHasTerrain_ (MC2_CLOUD_SHADOW, default OFF)
    //    Reads sceneDepthTex_ (unit 0). Writes sceneFBO_.COLOR0 (multiply darken).
    //    Stays on sceneFBO_; does NOT bind FBO 0.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::CloudShadow,
        /*name*/           "CloudShadow",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/true,
        /*note*/           "ISLAND-3 owned. Procedural cloud darken over all non-sky pixels "
                           "(MC2_CLOUD_SHADOW default-OFF). Reads MainDepth (unit0). "
                           "Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 2. Shoreline (call-site :2464)
    //    Gate: shorelineEnabled_ && sceneHasTerrain_ (default ON in-mission)
    //    Reads sceneDepthTex_ (unit 0) + sceneNormalTex_ (unit 1) = MainNormal.
    //    Writes sceneFBO_.COLOR0 (multiply brighten at shore boundary).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::Shoreline,
        /*name*/           "Shoreline",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::MainNormal,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/true,
        /*note*/           "ISLAND-3 owned. Shoreline foam brightening. "
                           "Reads MainDepth (unit0) + MainNormal/sceneNormalTex_ (unit1). "
                           "Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 3. EdgeFog (call-site :2477)
    //    Gate: edgeFogEnabled_ && mapHalfExtent_ > 0 && sceneHasTerrain_ (default ON)
    //    Reads sceneDepthTex_ (unit 0). Writes sceneFBO_.COLOR0 (alpha-blend fog).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::EdgeFog,
        /*name*/           "EdgeFog",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/true,
        /*note*/           "ISLAND-2 owned. Map-boundary edge fog (default ON in-mission). "
                           "Reads MainDepth (unit0). Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 4. FogOob (call-site :2484)
    //    Gate: fogOobEnabled_ && sceneHasTerrain_ (default ON in-mission)
    //    Reads sceneDepthTex_ (unit 0). Writes sceneFBO_.COLOR0 (alpha-blend OOB fog).
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::FogOob,
        /*name*/           "FogOob",
        /*reads*/          { RenderResourceId::MainDepth,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::MainColor,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::MainColor,
        /*conditional*/    true,
        /*ownedByExecutor*/true,
        /*note*/           "ISLAND-2 owned. Out-of-bounds ground fog (default ON in-mission). "
                           "Reads MainDepth (unit0). Writes MainColor via sceneFBO_.",
    },
    // -----------------------------------------------------------------------
    // 5. Composite (call-site :2489 glBindFramebuffer(GL_FRAMEBUFFER, 0))
    //    Gate: always (compositeProg_ valid + initialized_)
    //    Reads sceneColorTex_ (MainColor, unit 0). Writes FBO 0 / Backbuffer.
    //    SUBGRAPH OUTPUT EDGE: the sole FBO-0 bind in endScene().
    //
    //    POSTPROCESS-SCENEOBJECTID-RESOURCE-1: also reads sceneObjectIdTex_ (unit 2,
    //    GL_R32UI, COLOR_ATTACHMENT2) when effectiveMode==1 (objectId debug view,
    //    RenderWorld::IsObjectIdBufferEnabled() && sceneObjectIdTex_!=0).
    //    SceneObjectId is produced upstream by MechOpaque+StaticPropOpaque geometry
    //    passes (external to this PP subgraph). Declared in reads[] and in the
    //    external set so the subgraph validator sees the read satisfied externally.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::Composite,
        /*name*/           "Composite",
        /*reads*/          { RenderResourceId::MainColor,
                             RenderResourceId::SceneObjectId,  // POSTPROCESS-SCENEOBJECTID-RESOURCE-1: unit2, conditional effectiveMode==1 / IsObjectIdBufferEnabled
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Backbuffer,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Backbuffer,
        /*conditional*/    false,
        /*ownedByExecutor*/true,
        /*note*/           "SUBGRAPH OUTPUT EDGE. ExecutorIslandId::PostProcess (ISLAND-1). "
                           "Binds FBO 0 at :2489; full-screen quad blits sceneFBO_.COLOR0 -> "
                           "backbuffer with FXAA/exposure/viewmode/LOWLIGHT. Always runs. "
                           "CORRECTION vs recon: reads={MainColor} not {MainColor,Backbuffer}; "
                           "Composite writes Backbuffer, it does not sample it. "
                           "POSTPROCESS-SCENEOBJECTID-RESOURCE-1: reads SceneObjectId (unit2) "
                           "when effectiveMode==1 (IsObjectIdBufferEnabled && sceneObjectIdTex_!=0); "
                           "produced externally by MechOpaque+StaticPropOpaque geometry passes.",
    },
    // -----------------------------------------------------------------------
    // 6. ShadowDebugOverlay (call-site :2653)
    //    Gate: showShadowDebug_ (default OFF, debug member only)
    //    Reads shadowDepthTex_ or dynShadowArrayTex_ depending on CSM mode.
    //    Writes FBO 0 (already bound by Composite above). NOT executor-owned.
    // -----------------------------------------------------------------------
    {
        /*id*/             ExecutorIslandId::ShadowDebugOverlay,
        /*name*/           "ShadowDebugOverlay",
        /*reads*/          { RenderResourceId::ShadowStaticMap,
                             RenderResourceId::ShadowDynamicMap,
                             RenderResourceId::Unknown },
        /*writes*/         { RenderResourceId::Backbuffer,
                             RenderResourceId::Unknown },
        /*fboTarget*/      RenderResourceId::Backbuffer,
        /*conditional*/    true,
        /*ownedByExecutor*/false,
        /*note*/           "Debug shadow-map overlay. Draws on top of Composite on FBO 0 "
                           "(already bound). showShadowDebug_ default-OFF. "
                           "NOT executor-owned: post-composite position, debug-only. "
                           "Uses 2D_ARRAY on CSM path (dynShadowArrayTex_).",
    },
};

static constexpr unsigned kPostProcessSubpassCount =
    static_cast<unsigned>(sizeof(kPostProcessSubpasses) / sizeof(kPostProcessSubpasses[0]));

// Verify count at compile time.
static_assert(kPostProcessSubpassCount == 6,
    "POSTPROCESS-SUBGRAPH-1: expected 6 Slice-1 rows (Slice 2/3 deferred)");

// ---------------------------------------------------------------------------
// findPostProcessSubpass — linear scan, pure, no GL.
// Returns nullptr if id is not found in kPostProcessSubpasses.
// ---------------------------------------------------------------------------
inline const PostProcessSubpass* findPostProcessSubpass(ExecutorIslandId id) {
    for (unsigned i = 0; i < kPostProcessSubpassCount; ++i) {
        if (kPostProcessSubpasses[i].id == id)
            return &kPostProcessSubpasses[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Self-consistency validator
// ---------------------------------------------------------------------------

struct PostProcessValidationResult {
    bool             ok               = false;
    ExecutorIslandId offendingSubpass = ExecutorIslandId::Count;
    RenderResourceId missingResource  = RenderResourceId::Unknown;
};

// validatePostProcessSubgraph — walks all kPostProcessSubpassCount rows in order.
// For each row, every non-Unknown read must be EITHER:
//   (a) produced by an earlier subpass's writes[], OR
//   (b) listed in the external[] set (subgraph inputs from the frame's upstream passes).
// external[] is Unknown-terminated.
//
// Also asserts structural invariants:
//   - Composite (fboTarget=Backbuffer, conditional=false) precedes ShadowDebugOverlay.
//   - No post-Composite subpass (other than ShadowDebugOverlay) writes MainColor.
inline PostProcessValidationResult validatePostProcessSubgraph(
    const RenderResourceId* external, int externalCount)
{
    PostProcessValidationResult res;

    // produced[i] = true if resource i has been written by a prior subpass.
    // Use a flat array indexed by RenderResourceId value (max = Count).
    constexpr int kMaxId = static_cast<int>(RenderResourceId::Count);
    bool produced[kMaxId] = {};

    // Seed with external inputs.
    for (int i = 0; i < externalCount; ++i) {
        const int idx = static_cast<int>(external[i]);
        if (idx > 0 && idx < kMaxId)
            produced[idx] = true;
    }

    bool compositeSeen = false;

    for (unsigned si = 0; si < kPostProcessSubpassCount; ++si) {
        const PostProcessSubpass& sp = kPostProcessSubpasses[si];

        // Check that every read is satisfied.
        for (int ri = 0; ri < 5; ++ri) {
            const RenderResourceId r = sp.reads[ri];
            if (r == RenderResourceId::Unknown) break;
            const int idx = static_cast<int>(r);
            if (idx <= 0 || idx >= kMaxId || !produced[idx]) {
                res.ok               = false;
                res.offendingSubpass = sp.id;
                res.missingResource  = r;
                return res;
            }
        }

        // Mark writes as produced.
        for (int wi = 0; wi < 3; ++wi) {
            const RenderResourceId w = sp.writes[wi];
            if (w == RenderResourceId::Unknown) break;
            const int idx = static_cast<int>(w);
            if (idx > 0 && idx < kMaxId)
                produced[idx] = true;
        }

        // Structural invariant: Composite must precede ShadowDebugOverlay.
        if (sp.id == ExecutorIslandId::Composite) {
            compositeSeen = true;
        }
        if (sp.id == ExecutorIslandId::ShadowDebugOverlay && !compositeSeen) {
            // ShadowDebugOverlay before Composite — structural error.
            res.ok               = false;
            res.offendingSubpass = sp.id;
            res.missingResource  = RenderResourceId::Unknown;
            return res;
        }
    }

    res.ok = true;
    return res;
}

// Convenience overload with the baked-in Slice-1 external resource set.
// External inputs to the PostProcess subgraph from upstream frame-graph passes:
//   MainColor        — produced by Terrain + MechOpaque + StaticPropOpaque + VFX passes
//   MainDepth        — produced by Terrain + opaque passes
//   MainNormal       — produced by Terrain (GBuffer1 / sceneNormalTex_)
//   ShadowStaticMap  — external (pre-mission bake)
//   ShadowDynamicMap — produced by Shadow pass
//   SceneDepthCopy   — POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: produced by VFX pass
//                      (copySceneDepthForParticles, gos_particle_bridge.cpp:1068), cross-boundary.
//                      BoxDecals (SUBGRAPH-2) will consume it for soft-depth reject.
//   SceneObjectId    — POSTPROCESS-SCENEOBJECTID-RESOURCE-1: GBuffer2 (sceneObjectIdTex_,
//                      COLOR_ATTACHMENT2); produced externally by MechOpaque+StaticPropOpaque
//                      geometry passes (conditional on IsObjectIdBufferEnabled); read by
//                      Composite (unit2, effectiveMode==1).
inline PostProcessValidationResult validateShippedPostProcessSubgraph() {
    static const RenderResourceId kExternal[] = {
        RenderResourceId::MainColor,
        RenderResourceId::MainDepth,
        RenderResourceId::MainNormal,
        RenderResourceId::ShadowStaticMap,
        RenderResourceId::ShadowDynamicMap,
        RenderResourceId::SceneDepthCopy,  // POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: VFX producer, BoxDecals (SUBGRAPH-2) consumer
        RenderResourceId::SceneObjectId,   // POSTPROCESS-SCENEOBJECTID-RESOURCE-1: MechOpaque+StaticPropOpaque producer, Composite consumer
        RenderResourceId::Unknown,
    };
    constexpr int kExternalCount = 7;  // excludes the Unknown terminator
    return validatePostProcessSubgraph(kExternal, kExternalCount);
}

}} // namespace RenderCore::framegraph
