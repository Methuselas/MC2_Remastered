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

// C12 diagnostic counters. Process-lifetime aggregates; dumped at atexit.
// Three diagnostic categories:
//   emit_total == 0                              -> routing bug
//   emit_total > 0, records_flushed_total == 0   -> state-lifecycle bug
//   emit_total > 0, records_flushed_total > 0    -> GPU render bug
unsigned long long g_emit_total              = 0;
unsigned long long g_flush_total             = 0;
unsigned long long g_records_flushed_total   = 0;
unsigned int       g_records_per_flush_max   = 0;
unsigned long long g_nonempty_flush_total    = 0;
bool               g_first_flush_reported    = false;
bool               g_atexit_registered       = false;

void dump_summary() {
    std::fprintf(stderr,
                 "[GPU_PARTICLES v1] event=summary emit_total=%llu flush_total=%llu "
                 "nonempty_flush_total=%llu records_flushed_total=%llu "
                 "records_per_flush_max=%u\n",
                 g_emit_total, g_flush_total, g_nonempty_flush_total,
                 g_records_flushed_total, g_records_per_flush_max);
    std::fflush(stderr);
}

void initialize_env() {
    if (g_enabled_initialized) return;
    const char* v = std::getenv("MC2_GPU_PARTICLES");
    g_enabled_value = (v && v[0] == '1');
    g_enabled_initialized = true;
    if (g_enabled_value) {
        std::fprintf(stderr, "[INSTR v1] enabled: gpu_particles\n");
        std::fflush(stderr);
        if (!g_atexit_registered) {
            std::atexit(dump_summary);
            g_atexit_registered = true;
        }
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
    ++g_emit_total;
}

void Batcher::Flush() {
    if (!is_enabled()) return;
    ++g_flush_total;
    if (impl_->staging.empty()) return;
    const unsigned int n = (unsigned int)impl_->staging.size();
    ++g_nonempty_flush_total;
    g_records_flushed_total += n;
    if (n > g_records_per_flush_max) g_records_per_flush_max = n;
    if (!g_first_flush_reported) {
        g_first_flush_reported = true;
        std::fprintf(stderr,
                     "[GPU_PARTICLES v1] event=first_flush records=%u\n", n);
        std::fflush(stderr);
    }
    gos_particle_bridge_flush(impl_->staging.data(), n);
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
