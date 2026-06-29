#pragma once
// FRAME-GRAPH-SKELETON-1 (ambient ledger, DECLARATIONS ONLY).
//
// The frame-graph resource validator (frame_graph_validate.h) checks the resource DAG,
// but the recon (docs/render-backend-seams/frame-graph-recon-1.md) found the real
// executor blocker is AMBIENT GL state that applyPipeline does NOT own: colorMask,
// viewport, scissor, depth func/write, blend, bound FBO, and cross-pass latches
// (markTerrainDrawn / sceneHasTerrain_). An executor that reorders passes would strand
// that state (e.g. colorMask FALSE -> invisible scene).
//
// This file is the FIRST step: declare the per-pass ambient contract as data, starting
// with the single most dangerous axis the recon named -- the colorMask handshake
// (shadow lanes set per-attachment write OFF; terrain re-asserts RGBA write ON before
// the color-writing passes that follow). It is DECLARATIVE ONLY: no runtime
// enforcement, no scheduling. It exists so the handshake is a TESTED invariant (a
// future edit dropping the terrain re-assert fails offline) and so a later
// FRAME-GRAPH-AMBIENT-LEDGER slice has a data home to grow into. Do NOT build an
// executor against this until the ledger is complete and validated.
#include "RenderPassContract.h"

namespace RenderCore { namespace framegraph {

enum class ColorMaskState : uint8_t {
    Inherit = 0,  // pass does not establish a colorMask; inherits prior state
    AllOn,        // pass requires/establishes RGBA write enabled
    AllOff,       // pass writes depth only (color write disabled)
};

// Sparse: only passes with a notable ambient contract are listed. Absent = Inherit /
// no special ambient requirement (today). Grows as the ledger is filled.
struct AmbientContract {
    RenderPassId   id;
    ColorMaskState colorMaskOnEntry;        // colorMask state this pass establishes
    bool           reassertsColorMaskAllOn; // restores RGBA write for FOLLOWING passes
    bool           disablesColorWrite;      // depth-only pass (leaves color write OFF)
    const char*    note;
};

static constexpr AmbientContract kPassAmbient[] = {
    // Shadow lanes render depth-only and set per-attachment color write OFF
    // (gos_postprocess.cpp colorMaski FALSE). If a later color pass runs without a
    // re-assert, the scene is invisible -> the handshake below is load-bearing.
    { RenderPassId::Shadow,  ColorMaskState::AllOff, /*reassert*/ false, /*disables*/ true,
      "depth-only; sets per-attachment color write OFF" },
    // Terrain re-asserts glColorMask(TRUE) (gameos_graphics.cpp ~:1418/3501), restoring
    // RGBA write that the shadow pass disabled, for every color-writing pass after it.
    { RenderPassId::Terrain, ColorMaskState::AllOn,  /*reassert*/ true,  /*disables*/ false,
      "re-asserts glColorMask(TRUE) after shadow disabled it" },
};
static constexpr int kPassAmbientCount =
    sizeof(kPassAmbient) / sizeof(kPassAmbient[0]);

inline const AmbientContract* findAmbient(RenderPassId id) {
    for (int i = 0; i < kPassAmbientCount; ++i)
        if (kPassAmbient[i].id == id) return &kPassAmbient[i];
    return nullptr;
}

// The recon's #1 executor blocker, as a checkable invariant: SOME pass must disable
// color write (shadow) and SOME pass must re-assert it (terrain). If a future edit
// drops the terrain re-assert, this returns false and the offline test fails -- instead
// of shipping an invisible scene.
inline bool colorMaskHandshakeDeclared() {
    bool anyDisables = false, anyReasserts = false;
    for (int i = 0; i < kPassAmbientCount; ++i) {
        if (kPassAmbient[i].disablesColorWrite)      anyDisables  = true;
        if (kPassAmbient[i].reassertsColorMaskAllOn) anyReasserts = true;
    }
    return anyDisables && anyReasserts;
}

}} // namespace RenderCore::framegraph
