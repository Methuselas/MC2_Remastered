//==========================================================================//
// File:    batcher.cpp                                                      //
// Contents: GPU particle batcher implementation.                            //
//           Plan v5 §5.4 B1 Stage 1' Commit 1.                              //
//           FX-GPU-1 Phase 2: BeginGroup / per-group UV sub-rect.           //
//===========================================================================//

#include "batcher.h"

#include <mlr/mlr.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

// Bridge entry point implemented in GameOS/gameos/gos_particle_bridge.cpp.
// Forward-declared here so this TU never #includes GameOS/GL headers — the
// one-way dependency rule (see batcher.h doc comment) is enforced at the
// include level. Stage 1' Commit 4 wires the bridge.
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count,
                                          const mc2::particles::GroupInfo*   groups,
                                          unsigned int                       numGroups);

namespace mc2 {
namespace particles {

// Trail-spawn counters (see batcher.h declaration; incremented by gpu_trail.cpp).
unsigned long long Batcher::s_trail_spawn_total = 0;
unsigned long long Batcher::s_trail_head_total  = 0;

struct Batcher::Impl {
    std::vector<GpuParticle> staging;
    std::vector<GroupInfo>   groups;
    unsigned int             budget;
    bool                     overflowReported;
    bool                     hasOpenGroup;   // true iff BeginGroup was called this frame
    Impl(unsigned int b)
        : staging(), groups(), budget(b), overflowReported(false), hasOpenGroup(false) {
        staging.reserve(b);
        groups.reserve(64);
    }
};

namespace {
bool g_enabled_initialized = false;
bool g_enabled_value       = false;
bool g_log_value           = false;   // MC2_GPU_PARTICLES_LOG=1

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
                 "records_per_flush_max=%u trail_spawn=%llu trail_head=%llu\n",
                 g_emit_total, g_flush_total, g_nonempty_flush_total,
                 g_records_flushed_total, g_records_per_flush_max,
                 Batcher::s_trail_spawn_total,
                 Batcher::s_trail_head_total);
    std::fflush(stderr);
}

void initialize_env() {
    if (g_enabled_initialized) return;
    const char* v = std::getenv("MC2_GPU_PARTICLES");
    g_enabled_value = (v && v[0] == '1');
    const char* vl = std::getenv("MC2_GPU_PARTICLES_LOG");
    g_log_value = (vl && vl[0] == '1');
    g_enabled_initialized = true;
    if (g_enabled_value) {
        std::fprintf(stderr, "[INSTR v1] enabled: gpu_particles log=%d\n",
                     (int)g_log_value);
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

bool Batcher::is_log_enabled() {
    if (!g_enabled_initialized) initialize_env();
    return g_log_value;
}

Batcher::Batcher(unsigned int perFrameBudget)
    : impl_(new Impl(perFrameBudget)) {}

Batcher::~Batcher() { delete impl_; }

void Batcher::BeginGroup(uint32_t handle, float u0, float v0, float us, float vs,
                         int blendMode) {
    if (!is_enabled()) return;
    // Close the previous open group by recording its count.
    if (impl_->hasOpenGroup && !impl_->groups.empty()) {
        GroupInfo& prev = impl_->groups.back();
        prev.count = (unsigned)impl_->staging.size() - prev.start;
    }
    GroupInfo gi;
    gi.handle    = handle;
    gi.u0        = u0;
    gi.v0        = v0;
    gi.us        = us;
    gi.vs        = vs;
    gi.start     = (unsigned)impl_->staging.size();
    gi.count     = 0;
    gi.blendMode = blendMode;
    impl_->groups.push_back(gi);
    impl_->hasOpenGroup = true;
}

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

void Batcher::ResolveTextures()
{
    if (!impl_) return;
    // Force-load all pool textures unconditionally (ForceLoadImages sets
    // unLoadedImages=true before calling LoadImages so the early-exit is
    // bypassed). This handles the common case where gosFX textures were added
    // to the pool AFTER the normal actor-init LoadImages() window, leaving
    // unLoadedImages=false with mcTextureNodeIndex still 0xffffffff.
    if (MidLevelRenderer::MLRTexturePool::Instance)
        MidLevelRenderer::MLRTexturePool::Instance->ForceLoadImages();

    // MC2_GPU_PARTICLES_LOG=1 gated: per-handle RESOLVE_PROBE output.
    // Normal runs see only the first-flush banner; probes require opt-in.
    const bool logEnabled = is_log_enabled();

    // Once-only diagnostic (gated): group/staging counts on first non-empty frame.
    if (logEnabled) {
        static bool s_reported = false;
        if (!s_reported && !impl_->groups.empty()) {
            s_reported = true;
            std::fprintf(stderr,
                         "[RESOLVE_PROBE] first_nonempty groups=%u staging=%u\n",
                         (unsigned)impl_->groups.size(),
                         (unsigned)impl_->staging.size());
            std::fflush(stderr);
        }
    }

    static std::unordered_set<uint32_t> s_probed;
    for (auto& grp : impl_->groups) {
        if (grp.handle == 0) continue;
        if (!MidLevelRenderer::MLRTexturePool::Instance) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u POOL_NULL\n", grp.handle);
            continue;
        }
        MidLevelRenderer::MLRTexture* mlrTex =
            (*MidLevelRenderer::MLRTexturePool::Instance)[static_cast<int>(grp.handle)];
        if (!mlrTex) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u TEX_NULL\n", grp.handle);
            continue;
        }
        MidLevelRenderer::GOSImage* img = mlrTex->GetImage();
        if (!img) {
            if (logEnabled && s_probed.insert(grp.handle).second)
                std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u IMG_NULL\n", grp.handle);
            continue;
        }
        DWORD gosHandle = img->GetHandle();
        if (logEnabled && s_probed.insert(grp.handle).second)
            std::fprintf(stderr, "[RESOLVE_PROBE] handle=%u img=%p isLoaded=%d gosHandle=%u\n",
                grp.handle, (void*)img, (int)img->IsLoaded(), (unsigned)gosHandle);
        grp.handle = static_cast<uint32_t>(gosHandle);
    }
}

void Batcher::Flush() {
    if (!is_enabled()) return;
    ++g_flush_total;
    if (impl_->staging.empty()) {
        impl_->groups.clear();
        impl_->hasOpenGroup = false;
        return;
    }
    // Close the last open group.
    if (impl_->hasOpenGroup && !impl_->groups.empty()) {
        GroupInfo& last = impl_->groups.back();
        last.count = (unsigned)impl_->staging.size() - last.start;
    }
    const unsigned int n = (unsigned int)impl_->staging.size();
    ++g_nonempty_flush_total;
    g_records_flushed_total += n;
    if (n > g_records_per_flush_max) g_records_per_flush_max = n;
    if (!g_first_flush_reported) {
        g_first_flush_reported = true;
        std::fprintf(stderr,
                     "[GPU_PARTICLES v1] event=first_flush records=%u groups=%u\n",
                     n, (unsigned)impl_->groups.size());
        std::fflush(stderr);
    }
    gos_particle_bridge_flush(impl_->staging.data(), n,
                              impl_->groups.empty() ? nullptr : impl_->groups.data(),
                              (unsigned)impl_->groups.size());
    impl_->staging.clear();
    impl_->groups.clear();
    impl_->overflowReported = false;
    impl_->hasOpenGroup = false;
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
