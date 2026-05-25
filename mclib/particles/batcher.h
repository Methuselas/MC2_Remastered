//==========================================================================//
// File:    batcher.h                                                        //
// Contents: GPU particle SSBO batcher (producer-facing API).                //
//           Plan v5 §5.4 B1 Stage 1' Commit 1.                              //
//           FX-GPU-1 Phase 2: per-group UV sub-rect + animated atlas frames. //
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
#include <cstdint>

namespace mc2 {
namespace particles {

// Per-texture-group metadata produced by BeginGroup / closed by Flush.
// The bridge receives a flat array of these records alongside the particle
// staging buffer; each entry describes one contiguous sub-range of the
// staging buffer that shares a single texture and UV sub-rect.
//
// UV rect semantics: (u0,v0) is the lower-left atlas corner; (us,vs) is the
// width and height of the sub-rect in [0,1] UV space. For the full-texture
// case, u0=v0=0, us=vs=1. For an atlas sub-rect, u0/v0 are the frame origin
// and us/vs are the frame size as returned by spec m_UOffset/m_VOffset/
// m_USize/m_VSize.
struct GroupInfo {
    uint32_t handle;    // MLR pool index at spawn; resolved to gos_TextureHandle by ResolveTextures()
    float    u0;        // UV sub-rect origin X (0..1)
    float    v0;        // UV sub-rect origin Y (0..1)
    float    us;        // UV sub-rect width    (0..1)
    float    vs;        // UV sub-rect height   (0..1)
    unsigned start;     // index of first record in staging buffer
    unsigned count;     // number of records in this group
    int      blendMode; // 0 = standard alpha (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
                        // 1 = additive       (GL_SRC_ALPHA, GL_ONE)
};

class Batcher {
 public:
    // Per-frame budget. Stage 1' uses a fixed budget; Stage 2' may grow it
    // based on per-mission Stage 0' coverage data.
    explicit Batcher(unsigned int perFrameBudget);
    ~Batcher();

    // Cached at first call (per debug_instrumentation_rule.md idiom).
    // Returns true iff MC2_GPU_PARTICLES=1.
    static bool is_enabled();

    // Returns true iff MC2_GPU_PARTICLES_LOG=1.
    // Gates verbose per-spec/per-handle SPAWN_PROBE and RESOLVE_PROBE output.
    // Normal runs (is_enabled=1, is_log_enabled=0) produce only first-flush
    // banners and error/overflow one-shots.
    static bool is_log_enabled();

    // Open a new texture group before emitting a batch of particles that
    // share the same texture and UV sub-rect. Must be called at least once
    // before the first Emit; closing the previous group is automatic.
    //
    // Parameters:
    //   handle         MLR pool index from spec->m_state.GetTextureHandle() (resolved to GOS handle by ResolveTextures)
    //   u0, v0         atlas sub-rect origin (0..1)
    //   us, vs         atlas sub-rect size   (0..1)
    //
    // Use u0=0, v0=0, us=1, vs=1 for full-page textures.
    // No-op when is_enabled() is false.
    // blendMode: 0 = standard alpha, 1 = additive (from MLRState AlphaMode).
    void BeginGroup(uint32_t handle, float u0, float v0, float us, float vs,
                    int blendMode = 0);

    // Push one particle record into the per-frame staging buffer. Bounds-
    // checked against perFrameBudget; on overflow the record is dropped and
    // a one-shot stderr line is emitted (overflow is a latent class to track
    // before Stage 5' default-flip). Safe to call when is_enabled() is false
    // (returns immediately without staging). Particle is appended to the
    // currently-open group (most recent BeginGroup call).
    void Emit(const GpuParticle& p);

    // Resolve MLR pool indices in each GroupInfo to GOS texture handles.
    // Must be called AFTER renderLists() (which triggers LoadImages()) and
    // BEFORE Flush(). mclib/particles can include MLR headers; GameOS cannot.
    void ResolveTextures();

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
