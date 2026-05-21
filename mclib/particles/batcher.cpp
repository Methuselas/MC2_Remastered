//==========================================================================//
// File:    batcher.cpp                                                      //
// Contents: GPU particle batcher implementation.                            //
//           Plan v5 §5.4 B1 Stage 1' Commit 1.                              //
//===========================================================================//

#include "batcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Bridge entry point implemented in GameOS/gameos/gos_particle_bridge.cpp.
// Forward-declared here so this TU never #includes GameOS/GL headers — the
// one-way dependency rule (see batcher.h doc comment) is enforced at the
// include level. Stage 1' Commit 4 wires the bridge.
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count);

namespace mc2 {
namespace particles {

struct Batcher::Impl {
    std::vector<GpuParticle> staging;
    unsigned int             budget;
    bool                     overflowReported;
    Impl(unsigned int b) : staging(), budget(b), overflowReported(false) {
        staging.reserve(b);
    }
};

namespace {
bool g_enabled_initialized = false;
bool g_enabled_value       = false;

void initialize_env() {
    if (g_enabled_initialized) return;
    const char* v = std::getenv("MC2_GPU_PARTICLES");
    g_enabled_value = (v && v[0] == '1');
    g_enabled_initialized = true;
    if (g_enabled_value) {
        std::fprintf(stderr, "[INSTR v1] enabled: gpu_particles\n");
        std::fflush(stderr);
    }
}
}  // namespace

bool Batcher::is_enabled() {
    if (!g_enabled_initialized) initialize_env();
    return g_enabled_value;
}

Batcher::Batcher(unsigned int perFrameBudget)
    : impl_(new Impl(perFrameBudget)) {}

Batcher::~Batcher() { delete impl_; }

void Batcher::Emit(const GpuParticle& p) {
    if (!is_enabled()) return;
    if (impl_->staging.size() >= impl_->budget) {
        if (!impl_->overflowReported) {
            impl_->overflowReported = true;
            std::fprintf(stderr,
                         "[GPU_PARTICLES v1] event=overflow budget=%u — record dropped\n",
                         impl_->budget);
            std::fflush(stderr);
        }
        return;
    }
    impl_->staging.push_back(p);
}

void Batcher::Flush() {
    if (!is_enabled()) return;
    if (impl_->staging.empty()) return;
    gos_particle_bridge_flush(impl_->staging.data(),
                              (unsigned int)impl_->staging.size());
    impl_->staging.clear();
    impl_->overflowReported = false;
}

Batcher& Batcher::Instance() {
    // Intentionally leaked; same static-dtor-ordering reasoning as the
    // fx_trace tables (see mclib/fx_trace/fx_trace.cpp). Default budget
    // 4096 is a Stage 1' placeholder; Stage 2' sizes from coverage data.
    static Batcher* s_instance = new Batcher(4096);
    return *s_instance;
}

}  // namespace particles
}  // namespace mc2
