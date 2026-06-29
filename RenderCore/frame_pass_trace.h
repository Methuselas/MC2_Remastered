#pragma once
// FRAME-GRAPH-EXECUTOR-DRYRUN-1 — per-frame OBSERVE-AND-DIFF pure kernel.
//
// The first safe "executor" step is NOT scheduling — it is OBSERVING. This header is the
// GL-free, offline-doctestable kernel that, given a record of which render passes actually
// fired this frame (and in what record order, with which FBO target), DIFFS that against the
// declared kFramePassOrder[] and reports:
//
//   - firedCount        : how many declared passes were observed firing this frame
//   - unobservedCount   : declared passes with NO record this frame. CRITICAL: this is NOT a
//                         divergence. The recon (frame-graph-executor-dryrun-recon-1.md) found
//                         only 7 of 11 passes hit the noteRenderPass seam — Shadow, Water, VFX,
//                         VegetationCards are INVISIBLE today (no callsite). A pass that did not
//                         record must be classified UNOBSERVED, never "missing fire", so the
//                         default config produces ZERO false alarms.
//   - outOfOrderCount   : fired passes whose record sequence is not monotonic vs declared order.
//   - terrainMutexViolation : >1 terrain branch drew this frame (mutual-exclusion break).
//   - terrainLatchMissActive: the dominant terrain branch declares producesTerrainLatch but
//                         !latchActuallyImplemented (recon §4 HIGH: IndirectBridge dominant).
//
// PURE: no GL, no globals, no engine includes beyond the descriptive contract headers. The
// runtime layer (mclib/render_contract.cpp) FILLS a FramePassTrace and calls dryRunCompare();
// it does NOT re-implement ambient/FBO/terrain sampling (those guards already ship). The NEW
// value here is the FIRED-SET + ORDER + terrain mutual-exclusion + latch-miss, per recon Q3.
//
// ZERO behavior change: this header is observe-and-diff metadata only. No scheduling, no
// reordering, no barrier emission, no draw mutation.

#include "RenderCore/RenderPassContract.h"        // RenderPassId, kRenderPassIdCount, kFramePassOrder
#include "RenderCore/RenderResourceRegistry.h"     // RenderResourceId
#include "RenderCore/terrain_path_telemetry.h"      // TerrainPath
#include "RenderCore/terrain_subpass_contract.h"    // findTerrainSubPass (latch-miss reality)

namespace RenderCore { namespace framegraph {

// One slot per declared kFramePassOrder position. The trace is indexed by declared-order
// slot so a recorded pass maps directly onto its kFramePassOrder position; the order in
// which slots were filled (recordSeq) gives the observed sequence for the order check.
struct FramePassEntry {
    RenderPassId     id          = RenderPassId::None;
    bool             fired       = false;
    int              sequenceIdx = -1;                         // monotonic record order this frame
    RenderResourceId fboTarget   = RenderResourceId::Unknown;  // resolved bound draw-FBO (optional)
};

// One per kFramePassOrderCount slot. Filled at runtime behind the MC2_FRAMEGRAPH_DRYRUN gate;
// fabricated directly in offline tests.
struct FramePassTrace {
    FramePassEntry entries[kFramePassOrderCount];
    int            recordedCount = 0;                 // distinct passes that fired this frame
    // Terrain mutual-exclusion + latch-miss inputs. Kept as plain data so the kernel stays
    // pure (does NOT read the live telemetry globals). terrainBranch = the dominant branch,
    // terrainDrewCount = how many distinct terrain branches drew this frame.
    TerrainPath    terrainBranch    = TerrainPath::Count;
    int            terrainDrewCount = 0;
};

struct DryRunReport {
    int          firedCount             = 0;
    int          unobservedCount        = 0;
    int          outOfOrderCount        = 0;
    bool         terrainMutexViolation  = false;
    bool         terrainLatchMissActive = false;
    RenderPassId firstOutOfOrderPass    = RenderPassId::None;
};

// Find the slot index of `id` within `order` (declared-order position), or -1.
inline int declaredOrderIndex(RenderPassId id, const RenderPassId* order, int orderCount) {
    for (int i = 0; i < orderCount; ++i)
        if (order[i] == id) return i;
    return -1;
}

// Reset a trace to "nothing observed", pre-seeding each slot's id from the declared order so a
// fired pass can be addressed by its declared-order slot. Pure helper; callable from runtime.
inline void resetTrace(FramePassTrace& t, const RenderPassId* order, int orderCount) {
    const int n = (orderCount < kFramePassOrderCount) ? orderCount : kFramePassOrderCount;
    for (int i = 0; i < kFramePassOrderCount; ++i) {
        t.entries[i] = FramePassEntry{};
        if (i < n) t.entries[i].id = order[i];
    }
    t.recordedCount    = 0;
    t.terrainBranch    = TerrainPath::Count;
    t.terrainDrewCount = 0;
}

// Record that pass `id` fired. Maps it to its declared-order slot, stamps a monotonic
// sequenceIdx (= recordedCount before increment). A pass with no declared slot, or a duplicate
// fire, is ignored (the first record wins for ordering). Pure; safe to call from runtime.
inline void recordPassFired(FramePassTrace& t, RenderPassId id, RenderResourceId fboTarget,
                            const RenderPassId* order, int orderCount) {
    const int slot = declaredOrderIndex(id, order, orderCount);
    if (slot < 0 || slot >= kFramePassOrderCount) return;
    FramePassEntry& e = t.entries[slot];
    if (e.fired) return;                  // first fire wins; duplicates do not perturb ordering
    e.id          = id;
    e.fired       = true;
    e.sequenceIdx = t.recordedCount;
    e.fboTarget   = fboTarget;
    ++t.recordedCount;
}

// Does the dominant terrain branch declare the latch but not implement it? (recon §4 HIGH:
// IndirectBridge). Pure: reads only the descriptive terrain_subpass_contract.h table.
inline bool terrainBranchLatchMisses(TerrainPath branch) {
    if (branch == TerrainPath::Count) return false;        // no terrain drew -> nothing to miss
    const TerrainSubPass* sp = findTerrainSubPass(branch);
    if (!sp) return false;
    return sp->producesTerrainLatch && !sp->latchActuallyImplemented;
}

// THE KERNEL. Walk the declared order; classify each declared slot:
//   - entry.fired == false                 -> unobservedCount++   (NOT a divergence)
//   - entry.fired and record sequence not monotonic vs declared order -> outOfOrderCount++
// Terrain axes come from the trace's pre-snapshotted branch + drew-count (kernel reads NO
// globals): terrainDrewCount > 1 -> mutex violation; latch-miss via the contract table.
inline DryRunReport dryRunCompare(const FramePassTrace& trace,
                                  const RenderPassId* order, int orderCount) {
    (void)order;   // trace.entries[] is already indexed by declared-order slot (see resetTrace).
    DryRunReport r;
    const int n = (orderCount < kFramePassOrderCount) ? orderCount : kFramePassOrderCount;

    int prevSeq = -1;   // last fired pass's record sequence, walking in DECLARED order
    for (int i = 0; i < n; ++i) {
        const FramePassEntry& e = trace.entries[i];
        if (!e.fired) {
            ++r.unobservedCount;             // unobserved != diverged
            continue;
        }
        ++r.firedCount;
        // In declared order, record sequences of fired passes must be strictly increasing.
        if (e.sequenceIdx < prevSeq) {
            ++r.outOfOrderCount;
            if (r.firstOutOfOrderPass == RenderPassId::None)
                r.firstOutOfOrderPass = e.id;
        } else {
            prevSeq = e.sequenceIdx;
        }
    }

    r.terrainMutexViolation  = (trace.terrainDrewCount > 1);
    r.terrainLatchMissActive = terrainBranchLatchMisses(trace.terrainBranch);
    return r;
}

}} // namespace RenderCore::framegraph
