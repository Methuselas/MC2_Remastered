//==========================================================================//
// File:    batcher.h                                                        //
// Contents: GPU particle SSBO batcher (producer-facing API).                //
//           Plan v5 §5.4 B1 Stage 1' Commit 1.                              //
//                                                                           //
// This is the producer-facing scaffold: Emit() pushes a GpuParticle record  //
// into per-frame CPU staging; Flush() forwards the staged buffer to the     //
// GameOS GL bridge (gos_particle_bridge_flush). The bridge owns the actual  //
// SSBO upload, VAO, sampler save/restore, depth-state, blend-state, and the //
// glDrawArrays expansion to billboard quads.                                //
//                                                                           //
// Env-gated: MC2_GPU_PARTICLES=1 opt-in. When OFF, Flush() is a no-op and   //
// the bridge is never called — zero behavior change to the default render   //
// path. Default OFF until Stage 5'.                                         //
//                                                                           //
// One-way dependency: this header has NO GameOS / GL dependency; the .cpp   //
// calls a single C-style bridge function. Producers (later: per-primitive   //
// Spawn impls in Stage 2') include this header only.                        //
//===========================================================================//

#pragma once

#include "spec.h"

namespace mc2 {
namespace particles {

class Batcher {
 public:
    // Per-frame budget. Stage 1' uses a fixed budget; Stage 2' may grow it
    // based on per-mission Stage 0' coverage data.
    explicit Batcher(unsigned int perFrameBudget);
    ~Batcher();

    // Cached at first call (per debug_instrumentation_rule.md idiom).
    // Returns true iff MC2_GPU_PARTICLES=1.
    static bool is_enabled();

    // Push one particle record into the per-frame staging buffer. Bounds-
    // checked against perFrameBudget; on overflow the record is dropped and
    // a one-shot stderr line is emitted (overflow is a latent class to track
    // before Stage 5' default-flip). Safe to call when is_enabled() is false
    // (returns immediately without staging).
    void Emit(const GpuParticle& p);

    // Upload staging to the SSBO and issue the billboard draw. Must run
    // AFTER mcTextureManager->renderLists() (per gpu_direct_renderer_
    // bringup_checklist.md trap #6 — render order). The hook site is
    // code/gamecam.cpp post-renderLists. Clears staging after flush.
    // No-op when is_enabled() is false OR staged count is zero.
    void Flush();

    // Global singleton accessor. Stage 1' uses one batcher per process;
    // Stage 2' (per-type Spawn) may sharded if budget breakdown demands.
    static Batcher& Instance();

 private:
    struct Impl;
    Impl* impl_;
    Batcher(const Batcher&)            = delete;
    Batcher& operator=(const Batcher&) = delete;
};

}  // namespace particles
}  // namespace mc2
