#pragma once
// FRAME-GRAPH-AMBIENT-LEDGER-1 (DECLARATIONS ONLY).
//
// The resource validator (frame_graph_validate.h) checks the GPU-resource DAG. The
// recon (docs/render-backend-seams/frame-graph-recon-1.md §2) found the real executor
// blocker is AMBIENT GL state that applyPipeline does NOT own (~8 of ~13 axes):
// colorMask, viewport, scissor/stencil, depth func, blend, bound FBO, tex-unit latches,
// clip-control/reverse-Z, VAO, and cross-phase latches (markTerrainDrawn /
// sceneHasTerrain_). An executor that reorders passes strands that state (colorMask
// FALSE -> invisible scene; terrain latch unset -> 4 post passes silently bail).
//
// This file declares the per-pass ambient contract as DATA so the two documented
// landmine handshakes become TESTED invariants (a future edit that drops one fails
// offline, not as an invisible scene three days later). It is declarative only: NO
// runtime enforcement, NO scheduling. Static-grep enforcement of the live code lives in
// scripts/check-colormask-ownership.py / check-drawbuffer-ownership.py; this is the
// machine-readable model an executor would later diff against. DO NOT build an executor
// against this until the ledger is complete AND a runtime cross-check exists.
//
// All file:line refs are from the recon and are drift-prone; re-grep before trusting.
#include "RenderPassContract.h"

namespace RenderCore { namespace framegraph {

enum class ColorMaskState : uint8_t {
    Inherit = 0,  // pass does not establish colorMask; inherits prior state
    AllOn,        // RGBA write enabled
    AllOff,       // depth-only (color write disabled)
};

// Reverse-Z scene depth uses GL_GEQUAL; shadow passes use GL_LESS
// (mclib/render_contract.cpp:493-506). Inherit = pass does not set depth func.
enum class DepthFuncState : uint8_t { Inherit = 0, SceneGEqual, ShadowLess };

// Viewport is owned by no one (inherited / ad-hoc re-set). Declared so an executor can
// detect a pass running under the wrong viewport. MainScene = full color target;
// ShadowMap = the shadow atlas size.
enum class ViewportKind : uint8_t { Inherit = 0, MainScene, ShadowMap };

// FRAME-GRAPH-AMBIENT-LEDGER-2 axes, both samplable (glIsEnabled(GL_BLEND),
// glGetBooleanv(GL_DEPTH_WRITEMASK)).
//
// *** depthWrite IS a clean per-pass axis: declared ON for shadow+opaque, the runtime
//     probe confirmed dwMiss=0 across ~9.8k samples. Kept + verified.
//
// *** blend is NOT a per-pass axis in THIS engine: the runtime probe (MC2_AMBIENT_PROBE)
//     proved GL_BLEND is GLOBALLY ENABLED (live bl=On) even during opaque passes -- MC2
//     leaves blend enabled and controls via the blend FUNC, not the enable. The textbook
//     "opaque => blend OFF" was wrong here (5881/9804 false mismatches when declared Off).
//     So blend is left Inherit (not asserted). This is exactly why the probe ships first:
//     measure reality before declaring. A per-pass blend contract would need the blend
//     FUNC/equation, not the enable bit -- deferred.
//
// Deferred axes with NO stable per-pass declarable value (recon §2): bound FBO (dynamic
// GLuint), scissor/stencil (frame-global, inherited), tex-unit latches (NVIDIA leak), VAO
// (endScene save/restore). Those need a different model than a per-pass enum.
enum class BlendState      : uint8_t { Inherit = 0, On, Off };
enum class DepthWriteState : uint8_t { Inherit = 0, On, Off };

// Sparse: only passes with a notable ambient contract are listed. Absent = no special
// ambient requirement declared yet (the ledger grows incrementally).
struct AmbientContract {
    RenderPassId   id;
    ColorMaskState colorMaskOnEntry;        // colorMask state this pass establishes
    bool           reassertsColorMaskAllOn; // restores RGBA write for FOLLOWING passes
    bool           disablesColorWrite;      // depth-only pass (leaves color write OFF)
    DepthFuncState depthFunc;               // depth comparison this pass establishes
    ViewportKind   viewport;                // viewport this pass renders under
    bool           producesTerrainLatch;    // sets markTerrainDrawn / sceneHasTerrain_
    bool           consumesTerrainLatch;    // bails if !sceneHasTerrain_ (post sub-passes)
    const char*    note;
    // LEDGER-2 axes (default Inherit so existing rows are unaffected unless they opt in).
    BlendState      blend      = BlendState::Inherit;
    DepthWriteState depthWrite = DepthWriteState::Inherit;
};

static constexpr AmbientContract kPassAmbient[] = {
    // Shadow: depth-only; enforced via FBO attachment (DrawBufferSet::ShadowDepthOnly,
    // no color attachment), NOT via glColorMask. The active new shadow pre-pass
    // (gameos_graphics.cpp beginShadowPrePass) never calls glColorMask(FALSE) —
    // the legacy path (gos_postprocess.cpp :3490/:3512) does, but that is not the
    // active path. colorMask is AllOn at the observe point (:6270). Declared Inherit
    // so the ambient guard skips colorMask comparison (avoids a guaranteed AllOn-vs-AllOff
    // false mismatch). disablesColorWrite stays true — shadow IS depth-only, however
    // enforced — so the colorMaskHandshakeDeclared() landmine is not weakened.
    // SHADOW-OBSERVE-2: FBO-enforced depth-only; colorMaskOnEntry relaxed Inherit.
    { RenderPassId::Shadow,
      ColorMaskState::Inherit, /*reassert*/ false, /*disables*/ true,
      DepthFuncState::ShadowLess, ViewportKind::ShadowMap,
      /*producesLatch*/ false, /*consumesLatch*/ false,
      "depth-only via FBO attachment; colorMask Inherit (not an entry invariant); GL_LESS; shadow-atlas viewport",
      /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::On },

    { RenderPassId::StaticPropOpaque,
      ColorMaskState::Inherit, false, false,
      DepthFuncState::SceneGEqual, ViewportKind::MainScene,
      false, false, "reverse-Z scene opaque",
      /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::On },

    // Terrain: re-asserts glColorMask(TRUE) after shadow disabled it (the load-bearing
    // shadow-leak repair, gameos_graphics.cpp ~:1418/3501) AND sets the markTerrainDrawn
    // / sceneHasTerrain_ latch that 4 post sub-passes depend on.
    { RenderPassId::Terrain,
      ColorMaskState::AllOn,  /*reassert*/ true,  /*disables*/ false,
      DepthFuncState::SceneGEqual, ViewportKind::MainScene,
      /*producesLatch*/ true, /*consumesLatch*/ false,
      "re-asserts glColorMask(TRUE); sets sceneHasTerrain_ latch",
      /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::On },

    { RenderPassId::MechOpaque,
      ColorMaskState::Inherit, false, false,
      DepthFuncState::SceneGEqual, ViewportKind::MainScene,
      false, false, "reverse-Z scene opaque",
      /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::On },

    // WATER-SAME-ORDER-VALIDATE-1: alpha-blend water overlay. blend is GLOBALLY enabled
    // in MC2 (controlled via func, not enable) -> Inherit. colorMask Inherit (not an entry
    // invariant). depthFunc=GEQUAL is the WaterArmed reverse-Z value.
    // WATER-SAME-ORDER-VALIDATE-1 fixup: depthWrite relaxed Inherit. The begin sample fires
    // at function ENTRY, BEFORE applyPipeline(WaterArmed); the water path then sets
    // depth-mask OFF temporarily (savedDepthMask save/restore epilogue). Live entry sample
    // measured depthWrite=Off (dwMiss=1 vs declared On) -> not an entry invariant. depthFunc
    // sampled Inherit at entry too -> skipped (dfMiss=0), so GEQUAL is safe to keep.
    { RenderPassId::Water,
      ColorMaskState::Inherit, false, false,
      DepthFuncState::SceneGEqual, ViewportKind::MainScene,
      /*producesLatch*/ false, /*consumesLatch*/ false,
      "alpha-blend water; reverse-Z GEQUAL; depthWrite Inherit (entry-sample measured Off)",
      /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::Inherit },

    // PostProcess: screenShadow/cloudShadow/shoreline/edgeFog/fogOob bail if
    // !sceneHasTerrain_ (gos_postprocess.cpp :1303/1936/2030/2173/2234/2284/2341) -> the
    // terrain latch is a hard cross-phase dependency; reorder terrain after post and 4
    // passes silently vanish.
    { RenderPassId::PostProcess,
      ColorMaskState::Inherit, false, false,
      DepthFuncState::Inherit, ViewportKind::MainScene,
      /*producesLatch*/ false, /*consumesLatch*/ true,
      "consumes sceneHasTerrain_; FORCE-43 viewport re-set before composite" },
};
static constexpr int kPassAmbientCount =
    sizeof(kPassAmbient) / sizeof(kPassAmbient[0]);

inline const AmbientContract* findAmbient(RenderPassId id) {
    for (int i = 0; i < kPassAmbientCount; ++i)
        if (kPassAmbient[i].id == id) return &kPassAmbient[i];
    return nullptr;
}

// --- Runtime cross-check support (FRAME-GRAPH-AMBIENT-RUNTIME-1) -------------------
// The COMPARISON is pure and offline-testable; only the GL SAMPLING is runtime
// (mclib/render_contract.cpp samples glGet* into an AmbientSample, then calls this).
// A live value the caller could not classify is left Inherit and SKIPPED, so an
// undeclared-or-unclassifiable axis never produces a false positive. Only axes the
// ledger DECLARES (non-Inherit) are compared.
//
// CAVEAT (semantics): colorMaskOnEntry models the state a pass ESTABLISHES, which for
// terrain is re-asserted mid-pass. Sampling at beginPass-ENTRY may therefore legitimately
// differ -- that mismatch is DATA (it tells us the entry-vs-established boundary), which
// is exactly why the runtime probe ships default-OFF and diagnostic, not as a guard.
struct AmbientSample {
    ColorMaskState  colorMask  = ColorMaskState::Inherit;
    DepthFuncState  depthFunc  = DepthFuncState::Inherit;
    BlendState      blend      = BlendState::Inherit;
    DepthWriteState depthWrite = DepthWriteState::Inherit;
};
struct AmbientMismatch {
    bool colorMask  = false;
    bool depthFunc  = false;
    bool blend      = false;
    bool depthWrite = false;
    bool any() const { return colorMask || depthFunc || blend || depthWrite; }
};
inline AmbientMismatch compareAmbient(const AmbientContract& decl, const AmbientSample& live) {
    AmbientMismatch m;
    if (decl.colorMaskOnEntry != ColorMaskState::Inherit &&
        live.colorMask        != ColorMaskState::Inherit)
        m.colorMask = (decl.colorMaskOnEntry != live.colorMask);
    if (decl.depthFunc != DepthFuncState::Inherit &&
        live.depthFunc != DepthFuncState::Inherit)
        m.depthFunc = (decl.depthFunc != live.depthFunc);
    if (decl.blend != BlendState::Inherit &&
        live.blend != BlendState::Inherit)
        m.blend = (decl.blend != live.blend);
    if (decl.depthWrite != DepthWriteState::Inherit &&
        live.depthWrite != DepthWriteState::Inherit)
        m.depthWrite = (decl.depthWrite != live.depthWrite);
    return m;
}

// Landmine #1 (recon §2): SOME pass must disable color write (shadow) and SOME pass
// must re-assert it (terrain). Drop the terrain re-assert -> invisible scene.
inline bool colorMaskHandshakeDeclared() {
    bool anyDisables = false, anyReasserts = false;
    for (int i = 0; i < kPassAmbientCount; ++i) {
        if (kPassAmbient[i].disablesColorWrite)      anyDisables  = true;
        if (kPassAmbient[i].reassertsColorMaskAllOn) anyReasserts = true;
    }
    return anyDisables && anyReasserts;
}

// Landmine #2 (recon §2): SOME pass must SET the terrain latch and SOME pass CONSUMES
// it. Drop the producer -> 4 post passes silently bail (documented landmine).
inline bool terrainLatchHandshakeDeclared() {
    bool anyProduces = false, anyConsumes = false;
    for (int i = 0; i < kPassAmbientCount; ++i) {
        if (kPassAmbient[i].producesTerrainLatch) anyProduces = true;
        if (kPassAmbient[i].consumesTerrainLatch) anyConsumes = true;
    }
    return anyProduces && anyConsumes;
}

}} // namespace RenderCore::framegraph
