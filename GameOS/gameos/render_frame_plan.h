// RENDER-FRAME-PLAN-SCAFFOLD-1 — per-frame pass-trace governance.
//
// OBSERVE, NOT STEER. This header adds NO draw reorder, NO state change, NO routing.
// It lets each frame self-report which render phase / pass / draw-path actually ran,
// so the renderer stops being discovered by stepping on rakes (e.g. terrain solid
// taking the legacy MLR branch in capture instead of the modern bridge).
//
// Gated by MC2_RENDER_FRAME_PLAN_TRACE (default OFF). Header-only + all-inline, so it
// is byte-for-byte zero-cost when the gate is off (a single cached getenv bool check).
//
// Phase- and CHOKEPOINT-level only. Do NOT trace every triangle/bind/state mutation —
// that is noise. One [FRAME_PLAN] line per pass per frame.
//
//   [FRAME_PLAN] phase=TerrainOpaque pass=TerrainSolid path=LegacyMLR draws=4 pipeline=None
//   [FRAME_PLAN] phase=PostProcess pass=Composite path=ApplyPipeline draws=1 pipeline=PostProcessComposite
#pragma once

#include <cstdio>
#include <cstdlib>

namespace render_frame_plan {

enum class Phase {
    FrameSetup,
    ShadowDepth,
    TerrainOpaque,
    StaticOpaque,
    MechOpaque,
    TerrainOverlayDecal,
    Water,
    Vegetation,
    Vfx,
    PostProcess,
    Hud,
};

enum class PathKind {
    ApplyPipeline,   // routed through pipeline_binder::applyPipeline (PipelineDesc-owned state)
    RawGL,           // hand-set GL state at the call site (modern but unrouted)
    MLR,             // legacy master-vertex-node / gos_SetRenderState path
    Batcher,         // GPU mech / static-prop batcher
    Compute,         // compute dispatch that feeds an indirect draw
    Hud,             // 2D HUD batch replay
    Special,         // editor/menu/other special lifecycle
};

inline bool traceEnabled() {
    static const bool s = (std::getenv("MC2_RENDER_FRAME_PLAN_TRACE") != nullptr);
    return s;
}

inline const char* phaseName(Phase p) {
    switch (p) {
        case Phase::FrameSetup:          return "FrameSetup";
        case Phase::ShadowDepth:         return "ShadowDepth";
        case Phase::TerrainOpaque:       return "TerrainOpaque";
        case Phase::StaticOpaque:        return "StaticOpaque";
        case Phase::MechOpaque:          return "MechOpaque";
        case Phase::TerrainOverlayDecal: return "TerrainOverlayDecal";
        case Phase::Water:               return "Water";
        case Phase::Vegetation:          return "Vegetation";
        case Phase::Vfx:                 return "Vfx";
        case Phase::PostProcess:         return "PostProcess";
        case Phase::Hud:                 return "Hud";
    }
    return "?";
}

inline const char* pathName(PathKind k) {
    switch (k) {
        case PathKind::ApplyPipeline: return "ApplyPipeline";
        case PathKind::RawGL:         return "RawGL";
        case PathKind::MLR:           return "MLR";
        case PathKind::Batcher:       return "Batcher";
        case PathKind::Compute:       return "Compute";
        case PathKind::Hud:           return "Hud";
        case PathKind::Special:       return "Special";
    }
    return "?";
}

// One pass-trace line. `draws` may be -1 when a precise count is not cheap to obtain
// (phase-level is fine). `pipeline` = the PipelineId name when applyPipeline-routed,
// else "None".
inline void trace(Phase phase, const char* pass, PathKind path,
                  int draws, const char* pipeline) {
    if (!traceEnabled()) return;
    std::fprintf(stderr,
        "[FRAME_PLAN] phase=%s pass=%s path=%s draws=%d pipeline=%s\n",
        phaseName(phase), pass, pathName(path), draws,
        pipeline ? pipeline : "None");
    std::fflush(stderr);
}

}  // namespace render_frame_plan
