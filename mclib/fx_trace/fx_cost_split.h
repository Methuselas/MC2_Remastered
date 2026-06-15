//==========================================================================//
// File:    fx_cost_split.h                                                  //
// Contents: FX cycle cost-split — env-gated (MC2_FX_COST_SPLIT=1), default   //
//           off. Q2-S0 observability slice (docs/fx-particle-tube-and-       //
//           moddability-recon.md). Instrumentation ONLY — no behavior change.//
//                                                                           //
// RDTSC buckets isolate where FX CPU time goes so we can tell whether the    //
// tube rebuild, the particle sim, or the GPU upload is the real burn BEFORE  //
// optimizing anything (measure-before-infer).                                //
//                                                                           //
// Six inclusive-cycle buckets, one stderr summary every 600 wall frames:     //
//   fx_execute                     gosFX::Effect::Execute                    //
//   particlecloud_execute          gosFX::ParticleCloud::Execute             //
//   spinningcloud_animate_particle gosFX::SpinningCloud::AnimateParticle     //
//   tube_animate_profile           gosFX::Tube::AnimateProfile               //
//   tube_bridge_flush              gos_tube_ribbon_flush_deferred            //
//   particle_bridge_flush          gos_particle_bridge_flush                 //
//                                                                           //
// Buckets are INCLUSIVE (RAII scope brackets the whole function), so nested  //
// buckets double-count (fx_execute nests inside particlecloud_execute, which //
// holds spinningcloud_animate_particle). That is fine for a first relative   //
// magnitude read; Tracy gives the proper hierarchy.                          //
//                                                                           //
// One-way dependency (mirrors fx_trace.h): gosfx/, particles/, and the       //
// gameos particle bridge include this; this header includes nothing from     //
// those trees. Single-threaded accumulation — all FX sim + flush run on the  //
// main render thread, so no locking (faster than fx_trace's mutex path).     //
//===========================================================================//

#pragma once

namespace mc2 {
namespace fx_cost_split {

enum Bucket {
    B_FX_EXECUTE = 0,
    B_PARTICLECLOUD_EXECUTE,
    B_SPINNINGCLOUD_ANIMATE,
    B_TUBE_ANIMATE_PROFILE,
    B_TUBE_BRIDGE_FLUSH,
    B_PARTICLE_BRIDGE_FLUSH,
    B_COUNT
};

// Cached at first call; read once from MC2_FX_COST_SPLIT env.
bool is_enabled();

// Self-contained cycle counter (rdtsc on x86; 0 elsewhere). No gameos dep.
unsigned long long now_cycles();

// Accumulate `cycles` into `bucket` and bump its call count. No-op for OOB.
void add(int bucket, unsigned long long cycles);

// Tick once per WALL frame (call from the per-frame flush drain). Emits the
// [FX_COST v1] summary line and resets accumulators every 600th frame.
void roll_frame_and_maybe_emit();

// RAII timer: reads rdtsc on construct (only when enabled), accumulates on
// destruct. Handles multiple early-returns cleanly. NOT a Tracy zone — safe to
// place in per-particle hot loops (the 100ns Tracy-floor rule is about Tracy
// zones, not env-gated rdtsc buckets).
struct Scope {
    int                on_;
    int                b_;
    unsigned long long t0_;
    explicit Scope(int b)
        : on_(is_enabled() ? 1 : 0), b_(b), t0_(on_ ? now_cycles() : 0ULL) {}
    ~Scope() { if (on_) add(b_, now_cycles() - t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace fx_cost_split
} // namespace mc2
