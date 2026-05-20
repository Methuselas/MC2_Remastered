// cpu_proj_cost_split.h
//
// F3 — CPU projection cost-baseline (measurement-only) v1.
//
// Spec: docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md
// Audit: docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-INSTRUMENTATION-AUDIT.md
//
// Env gate: MC2_CPU_PROJ_COST_SPLIT=1. When OFF, ALL chrono and ALL counter
// work is short-circuited at a single inline check (g_cpuProjEnabled). The
// instrumentation is genuinely zero-cost when disabled.
//
// Per-frame discipline:
//   * gos_RendererBeginFrame() -> frame_begin() — reset bucket accumulators.
//   * Bucket scopes (RAII) open/close around outer call boundaries; each
//     scope's elapsed nanoseconds + workload count accumulate into the
//     current-frame bucket.
//   * gos_RendererEndFrame() -> frame_end() — commit per-frame bucket
//     samples into the ring buffer. Every 500 frames, print window stats.
//   * Mission::init / Mission::load -> hard_reset() — clear ring buffer +
//     frame counter at mission boundaries.
//
// Single-threaded per audit (a). Thread-local counters used for safety
// and Vulkan-prep forward-compat.

#pragma once

#include <cstdint>

namespace mc2_cpu_proj_cost {

// Bucket identifiers. Order matters: matches printout order.
enum BucketId : int {
    BUCKET_MATRIX_BUILD          = 0,  // Camera.BuildMVP + SetCameraMatrices
    BUCKET_RECALCBOUNDS_PERFRAME = 1,  // 6 recalcBounds overrides (aggregated)
    BUCKET_TGL_TRANSFORM         = 2,  // TG_MultiShape::TransformMultiShape (aggregated)
    BUCKET_MLR_TOTAL             = 3,  // StartDraw + RenderNow
    BUCKET_COUNT                 = 4,
};

// Sidecar (NEVER part of projection_total adjudication).
enum SidecarId : int {
    SIDECAR_SKINNING_CHAIN          = 0,  // Mech3D.UpdateGeometry outer
    SIDECAR_EVENTDRIVEN_PROJECT_Z   = 1,  // projectZ called outside render loop (count-only)
    SIDECAR_COUNT                   = 2,
};

// Master env flag — checked at the head of every scope/counter path. When
// false, scopes do nothing and counters do not tick. Set once at startup
// by init_from_env(). Defined in cpu_proj_cost_split.cpp.
extern bool g_cpuProjEnabled;

// TLS flag: true inside ObjectManager::render(). Used to attribute
// projectZ calls to render-vs-eventdriven. Set by RenderLoopGuard below.
extern thread_local bool tls_inRenderLoop;

// Public API ---------------------------------------------------------------

// One-shot: read MC2_CPU_PROJ_COST_SPLIT, print banner, init state.
// Safe to call multiple times (idempotent).
void init_from_env();

// Per-frame hooks (no-op when env OFF).
void frame_begin();   // Reset current-frame bucket accumulators.
void frame_end();     // Commit current frame into ring buffer; print window stats if due.

// Mission boundary hook — hard-reset ring buffer + frame counter.
void hard_reset(const char* reason);

// Final-stats flush — called at process exit AND at mission load. Prints
// the [CPU_PROJ v1 final] line using all per-frame samples retained.
void final_stats(const char* reason);

// Workload counter accumulators (per-call, accumulated into the current
// frame's bucket). All gated on g_cpuProjEnabled at the entry point.
void add_workload_recalcbounds(int actorsRecalc);
void add_workload_tgl_transform(int numShapes);
void add_workload_mlr_prim_clipped();
void add_workload_eventdriven_projectZ();   // sidecar count-only

// RAII scope guard — opens chrono, closes + accumulates into bucket on
// destruction. Zero-cost when env OFF (constructor early-out).
class Scope {
public:
    explicit Scope(int bucketId);
    ~Scope();
    // Non-copyable, non-movable.
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    int     bucket_;
    bool    armed_;
    int64_t startNs_;
};

// Sidecar scope (for skinning_chain etc).
class SidecarScope {
public:
    explicit SidecarScope(int sidecarId);
    ~SidecarScope();
    SidecarScope(const SidecarScope&) = delete;
    SidecarScope& operator=(const SidecarScope&) = delete;
private:
    int     sidecar_;
    bool    armed_;
    int64_t startNs_;
};

// TLS render-loop guard — set tls_inRenderLoop=true on construction,
// restore prior value on destruction. RAII discipline (handles nesting).
class RenderLoopGuard {
public:
    RenderLoopGuard();
    ~RenderLoopGuard();
private:
    bool prior_;
    bool armed_;
};

} // namespace mc2_cpu_proj_cost
