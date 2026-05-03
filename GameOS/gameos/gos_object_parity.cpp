// GameOS/gameos/gos_object_parity.cpp
//
// Slice 2 (object-offload) — Stage 2.D parity readback harness.
//
// See gos_object_parity.h for the slice overview, the schema reservations,
// the default-off contract, and the public API.
//
// Stage 2.D.1 landed: env-gate readers, SSBO allocation, fence-ring
// readback (bytes discarded), counter infrastructure, 600-frame summary.
//
// Stage 2.D.1.1 landed: slot-overflow accounting, uniform location cache.
//
// Stage 2.D.2 (this file revision): P3 dual-emit state machine + per-type
// snapshot collection + bytewise compare.
//
// Dual-emit flow summary (2.D.2):
//   Frame N  (state=Armed):
//     - bdactor/genactor::update: for each GPU-eligible actor, runs BOTH
//       TransformMultiShape_PositionsOnly (normal GPU) AND
//       TransformMultiShape (full, to populate listOfTriangles[].aRGBLight).
//     - GpuStaticPropBatcher::submitMultiShape: for each accepted leaf,
//       RecordInstanceSnapshot captures per-vertex CPU aRGBLight in
//       submission order (matches parity SSBO instance order).
//     - GpuStaticPropBatcher::flush per-type loop: for each type that gets
//       a parity write, RecordParityTypeRange stores the cursor offset,
//       instanceCount, and vertexCount.
//     - After the fence: AdvanceDualEmitToWaiting() → state=WaitingForReadback.
//   Frame N+1 (state=WaitingForReadback):
//     - GpuStaticPropBatcher::flush readback: CompareAndReport() decodes the
//       readback bytes and compares against the snapshots. Throttled mismatch
//       lines + 600-frame summary.
//     - AdvanceDualEmitToDone() → state=Done. Snapshot and range map freed.

#include "gos_object_parity.h"

// Stage 2.D.1.1 (Item 4, Option A): include the batcher header so kRingFrames
// can reference STATIC_PROP_RING_FRAMES directly rather than duplicating the
// literal 3. No circular dependency: gos_object_parity.h has no batcher.h
// dependency; the coupling is one-directional (parity .cpp → batcher .h).
#include "gos_static_prop_batcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

namespace gos_object_parity {

// ---------------------------------------------------------------------------
// Env-gate readers
// ---------------------------------------------------------------------------

bool IsParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_OBJECT_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsParityTraceEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_OBJECT_PARITY_TRACE");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Counter storage (TU-private; cross-TU access via public Add/Get)
// ---------------------------------------------------------------------------
namespace {
uint64_t s_vertices_written_total = 0;
uint64_t s_vertices_written_this_frame = 0;
uint64_t s_readback_bytes_total = 0;
uint64_t s_readback_bytes_this_frame = 0;

// Stage 2.D.1.1 (Item 1): slot-overflow tracking.
uint64_t s_slot_overflows_total = 0;
uint64_t s_slot_overflows_this_frame = 0;

// Once-per-type overflow print guard. Tracks which typeIds have already had
// their "slot_overflow" line emitted. Grows unboundedly only if there are
// unbounded unique typeIds, but the type count is fixed at map-load time
// (bounded by the number of registered TG_TypeShape objects, typically <100).
std::set<uint32_t> s_overflowedTypes;
}  // namespace

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Dual-emit state machine storage.
// ---------------------------------------------------------------------------
namespace {

enum class DualEmitState { Armed, WaitingForReadback, Done };
DualEmitState s_dualEmitState = DualEmitState::Done;

// The ring slot (s_frameSlot value) on which the Armed frame wrote its parity
// data. CompareAndReport should only run when the ring revisits this slot
// (i.e. when the GPU-complete fence wait clears that slot's writes).
// RING_FRAMES = 3, so that slot comes back 3 flushes later.
uint32_t s_dualEmitSlot = 0xFFFFFFFFu;  // invalid sentinel

// Per-type CPU snapshot: flattened [instanceIdx * vertexCount + vertexId] →
// aRGBLight. Built incrementally by RecordInstanceSnapshot during the Armed
// frame; consumed by CompareAndReport on the WaitingForReadback frame.
struct DualEmitTypeSnapshot {
    uint32_t              vertexCount;    // type.vertexCount (fixed per type)
    std::vector<uint32_t> cpuARGB;        // grows by vertexCount per instance
};
std::unordered_map<uint32_t, DualEmitTypeSnapshot> s_dualEmitSnapshots;

// Per-type SSBO byte range recorded at draw time (frame N); used for decode
// at compare time (frame N+1). cursorOffset is the byte offset within the
// parity slot (NOT the absolute buffer offset — already slot-relative).
struct DualEmitTypeRange {
    size_t   cursorOffset;   // slot-relative byte offset
    uint32_t instanceCount;
    uint32_t vertexCount;
};
std::unordered_map<uint32_t, DualEmitTypeRange> s_dualEmitRanges;

// Stage 2.D.2 counters (compare totals across dual-emit + future P1).
uint64_t s_compared_total   = 0;
uint64_t s_passed_total     = 0;
uint64_t s_mismatched_total = 0;

// Mismatch throttle + summary counters (moved from the per-printer namespace
// block so that ParityPrintMismatchByIndex can access them before the
// existing ParityPrintMismatch definition).
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryMismatches   = 0;

}  // namespace

namespace gos_object_parity {

void Counters_AddVerticesWrittenThisFrame(uint64_t n) {
    s_vertices_written_this_frame += n;
}

void Counters_AddReadbackBytesThisFrame(uint64_t n) {
    s_readback_bytes_this_frame += n;
}

void Counters_AddSlotOverflowThisFrame(uint32_t typeId,
                                        size_t   needBytes,
                                        size_t   budgetBytes) {
    ++s_slot_overflows_this_frame;
    // Once-per-type printf so a busy scene doesn't spam the log.
    if (s_overflowedTypes.find(typeId) == s_overflowedTypes.end()) {
        s_overflowedTypes.insert(typeId);
        std::fprintf(stderr,
            "[OBJECT_PARITY v1] event=slot_overflow type=%u "
            "need=%zu budget=%zu\n",
            typeId, needBytes, budgetBytes);
        std::fflush(stderr);
    }
}

uint64_t Counters_GetVerticesWrittenTotal()  { return s_vertices_written_total; }
uint64_t Counters_GetReadbackBytesTotal()    { return s_readback_bytes_total; }
uint64_t Counters_GetSlotOverflowsTotal()    { return s_slot_overflows_total; }

// Stage 2.D.2 compare counters.
uint64_t Counters_GetComparedTotal()    { return s_compared_total; }
uint64_t Counters_GetPassedTotal()      { return s_passed_total; }
uint64_t Counters_GetMismatchedTotal()  { return s_mismatched_total; }
void     Counters_AddCompared(uint64_t n)    { s_compared_total   += n; }
void     Counters_AddPassed(uint64_t n)      { s_passed_total     += n; }
void     Counters_AddMismatched(uint64_t n)  { s_mismatched_total += n; }

void Counters_ResetSlotOverflowsThisFrame() {
    // Called from ParityFrameTick(); not for external use.
    s_slot_overflows_total     += s_slot_overflows_this_frame;
    s_slot_overflows_this_frame = 0;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Parity-output SSBO storage.
//
// Single GL buffer sized RING_FRAMES * s_slotBytes. Lazy-allocated on the
// first call to EnsureParityOutputSSBO() when IsParityCheckEnabled() is
// true. Slice 1's batcher already runs a 3-frame fence ring on
// s_fence[s_frameSlot]; we mirror its RING_FRAMES so a one-frame async
// readback (at top of the next visit to a slot) is naturally synchronized
// by the same fence — no parallel fence ring per the advisor's load-bearing
// rule in the hand-off prompt.
// ---------------------------------------------------------------------------
namespace {

// Stage 2.D.1.1 (Item 4, Option A): use STATIC_PROP_RING_FRAMES from the
// batcher header instead of duplicating the literal. A mismatched ring depth
// would corrupt the async-readback handshake (the batcher's fence and the
// parity SSBO ring must advance in lock-step).
constexpr unsigned kRingFrames = STATIC_PROP_RING_FRAMES;

GLuint   s_paritySSBO  = 0;
size_t   s_slotBytes   = 0;

}  // namespace

namespace gos_object_parity {

GLuint EnsureParityOutputSSBO(size_t bytesPerSlot) {
    if (!IsParityCheckEnabled()) return 0;
    if (bytesPerSlot == 0) return s_paritySSBO;

    // Re-create on grow. 2.D.1 batcher edits pass a fixed kSlotBytes so
    // this branch only fires on the very first call; future stages may
    // ratchet it up as P1 sampling expands per-frame coverage.
    if (s_paritySSBO != 0 && bytesPerSlot > s_slotBytes) {
        glDeleteBuffers(1, &s_paritySSBO);
        s_paritySSBO = 0;
        s_slotBytes  = 0;
    }

    if (s_paritySSBO == 0) {
        glGenBuffers(1, &s_paritySSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_paritySSBO);
        // glBufferStorage with DYNAMIC_STORAGE makes the buffer eligible
        // for glGetBufferSubData (the Stage 2.D.1 readback path) while
        // still allowing the GPU to write to it via the std430 binding.
        // No persistent map — we only want one client-side read per frame
        // at the top of flush(), which glGetBufferSubData handles directly.
        const GLsizeiptr totalBytes =
            static_cast<GLsizeiptr>(bytesPerSlot) * static_cast<GLsizeiptr>(kRingFrames);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr,
                        GL_DYNAMIC_STORAGE_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_slotBytes = bytesPerSlot;

        if (IsParityTraceEnabled()) {
            std::fprintf(stderr,
                "[OBJECT_PARITY v1] event=ssbo_alloc bytes_per_slot=%zu "
                "ring_frames=%u total_bytes=%lld handle=%u\n",
                bytesPerSlot, kRingFrames,
                static_cast<long long>(totalBytes),
                static_cast<unsigned>(s_paritySSBO));
            std::fflush(stderr);
        }
    }
    return s_paritySSBO;
}

size_t   SlotBytes()  { return s_slotBytes; }
unsigned RingFrames() { return kRingFrames; }

void ReleaseParityOutputSSBO() {
    if (s_paritySSBO) {
        glDeleteBuffers(1, &s_paritySSBO);
        s_paritySSBO = 0;
        s_slotBytes  = 0;
    }
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Dual-emit state machine implementations.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

void OnMissionLoad() {
    if (!IsParityCheckEnabled()) return;
    s_dualEmitState = DualEmitState::Armed;
    s_dualEmitSnapshots.clear();
    s_dualEmitRanges.clear();
    if (IsParityTraceEnabled()) {
        std::fprintf(stderr,
            "[OBJECT_PARITY v1] event=dual_emit_armed\n");
        std::fflush(stderr);
    }
}

bool IsDualEmitArmed() {
    if (!IsParityCheckEnabled()) return false;
    return s_dualEmitState == DualEmitState::Armed;
}

bool IsDualEmitReadyForSlot(uint32_t slotNow) {
    if (!IsParityCheckEnabled()) return false;
    if (s_dualEmitState != DualEmitState::WaitingForReadback) return false;
    return slotNow == s_dualEmitSlot;
}

void AdvanceDualEmitToWaiting(uint32_t slotUsed) {
    if (!IsParityCheckEnabled()) return;
    if (s_dualEmitState != DualEmitState::Armed) return;
    s_dualEmitSlot  = slotUsed;
    s_dualEmitState = DualEmitState::WaitingForReadback;
    if (IsParityTraceEnabled()) {
        std::fprintf(stderr,
            "[OBJECT_PARITY v1] event=dual_emit_waiting slot=%u "
            "types_snapshotted=%zu types_ranged=%zu\n",
            slotUsed,
            s_dualEmitSnapshots.size(), s_dualEmitRanges.size());
        std::fflush(stderr);
    }
}

void AdvanceDualEmitToDone() {
    if (!IsParityCheckEnabled()) return;
    s_dualEmitState = DualEmitState::Done;
    // Free snapshot and range maps — not needed for the rest of this mission.
    s_dualEmitSnapshots.clear();
    s_dualEmitRanges.clear();
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Snapshot collection and type-range record.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

void RecordInstanceSnapshot(uint32_t typeId,
                             const uint32_t* perVertexCpuARGB,
                             uint32_t vertexCount) {
    // Should only be called when Armed; caller gates on IsDualEmitArmed().
    // Double-check defensively.
    if (s_dualEmitState != DualEmitState::Armed) return;
    if (!perVertexCpuARGB || vertexCount == 0) return;

    DualEmitTypeSnapshot& snap = s_dualEmitSnapshots[typeId];
    snap.vertexCount = vertexCount;
    // Append one instance's worth of per-vertex aRGBLight.
    snap.cpuARGB.insert(snap.cpuARGB.end(),
                        perVertexCpuARGB,
                        perVertexCpuARGB + vertexCount);
}

void RecordParityTypeRange(uint32_t typeId,
                           size_t   cursorOffset,
                           uint32_t instanceCount,
                           uint32_t vertexCount) {
    // Should only be called when Armed; caller gates on IsDualEmitArmed().
    if (s_dualEmitState != DualEmitState::Armed) return;
    DualEmitTypeRange& rng = s_dualEmitRanges[typeId];
    rng.cursorOffset  = cursorOffset;
    rng.instanceCount = instanceCount;
    rng.vertexCount   = vertexCount;
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Stage 2.D.2 — CompareAndReport.
//
// readbackBytes is the raw bytes glGetBufferSubData'd from the parity SSBO
// slot for frame N. The byte range is slot-relative (offset 0 = start of the
// slot). Each recorded type range's cursorOffset is already slot-relative
// (recorded as the cursor within the slot by the draw loop).
//
// GPU parity SSBO packs per-vertex uint32 as b8|(g8<<8)|(r8<<16)|(a8<<24)
// (matching the CPU ARGB format (A<<24)|(R<<16)|(G<<8)|B — same bit layout,
// same uint32 value).
//
// ULP tolerance: ±2 LSB per channel independently.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

static bool channelsWithin2(uint32_t cpu, uint32_t gpu) {
    // Extract each channel and test.
    const int channels[4] = {
        static_cast<int>(cpu & 0xFF)        - static_cast<int>(gpu & 0xFF),
        static_cast<int>((cpu >>  8) & 0xFF) - static_cast<int>((gpu >>  8) & 0xFF),
        static_cast<int>((cpu >> 16) & 0xFF) - static_cast<int>((gpu >> 16) & 0xFF),
        static_cast<int>((cpu >> 24) & 0xFF) - static_cast<int>((gpu >> 24) & 0xFF),
    };
    for (int d : channels) {
        if (d < -2 || d > 2) return false;
    }
    return true;
}

void CompareAndReport(const uint8_t* readbackBytes,
                      size_t         readbackSize,
                      unsigned       frameNum) {
    if (!IsParityCheckEnabled()) return;
    if (s_dualEmitState != DualEmitState::WaitingForReadback) return;
    if (!readbackBytes || readbackSize == 0) return;

    uint64_t totalCompared   = 0;
    uint64_t totalPassed     = 0;
    uint64_t totalMismatched = 0;

    // Iterate every type that has both a snapshot and a range record.
    for (const auto& snapKv : s_dualEmitSnapshots) {
        const uint32_t typeId = snapKv.first;
        const DualEmitTypeSnapshot& snap = snapKv.second;

        auto rangeIt = s_dualEmitRanges.find(typeId);
        if (rangeIt == s_dualEmitRanges.end()) {
            // No GPU range recorded — slot overflow or type got no parity write.
            // Skip silently: these are counted by the overflow counter elsewhere.
            continue;
        }
        const DualEmitTypeRange& rng = rangeIt->second;

        // Sanity: vertexCount must agree between snapshot and range record.
        if (snap.vertexCount == 0 || rng.vertexCount == 0) continue;
        if (snap.vertexCount != rng.vertexCount) {
            // Geometry changed between submit and draw — skip (shouldn't happen).
            std::fprintf(stderr,
                "[OBJECT_PARITY v1] event=vertex_count_mismatch type=%u "
                "snap_verts=%u range_verts=%u -- skipping type\n",
                typeId, snap.vertexCount, rng.vertexCount);
            std::fflush(stderr);
            continue;
        }

        const uint32_t verts = snap.vertexCount;
        const uint32_t instCount = rng.instanceCount;

        // Verify bounds against readbackSize.
        // Each instance occupies verts * sizeof(uint32_t) bytes in the SSBO.
        const size_t typeByteCount = (size_t)instCount * (size_t)verts * sizeof(uint32_t);
        if (rng.cursorOffset + typeByteCount > readbackSize) {
            // Readback buffer is smaller than expected. Skip this type —
            // don't read out-of-bounds.
            if (IsParityTraceEnabled()) {
                std::fprintf(stderr,
                    "[OBJECT_PARITY v1] event=readback_bounds_miss type=%u "
                    "cursor=%zu need=%zu avail=%zu\n",
                    typeId, rng.cursorOffset, rng.cursorOffset + typeByteCount,
                    readbackSize);
                std::fflush(stderr);
            }
            continue;
        }

        // Walk each (instance, vertex) pair.
        const uint32_t* gpuBase = reinterpret_cast<const uint32_t*>(
            readbackBytes + rng.cursorOffset);

        for (uint32_t inst = 0; inst < instCount; ++inst) {
            // Snapshot CPU data for this instance starts at inst * verts.
            const uint32_t snapBase = inst * verts;
            if (snapBase + verts > static_cast<uint32_t>(snap.cpuARGB.size())) {
                // Snapshot underrun — should not happen; break defensively.
                break;
            }
            for (uint32_t v = 0; v < verts; ++v) {
                const uint32_t cpuARGB = snap.cpuARGB[snapBase + v];
                const uint32_t gpuARGB = gpuBase[inst * verts + v];
                ++totalCompared;
                if (channelsWithin2(cpuARGB, gpuARGB)) {
                    ++totalPassed;
                } else {
                    ++totalMismatched;
                    ParityPrintMismatchByIndex(frameNum, typeId, inst, v,
                                               cpuARGB, gpuARGB);
                }
            }
        }
    }

    Counters_AddCompared(totalCompared);
    Counters_AddPassed(totalPassed);
    Counters_AddMismatched(totalMismatched);

    if (IsParityTraceEnabled() || totalMismatched > 0) {
        std::fprintf(stderr,
            "[OBJECT_PARITY v1] event=dual_emit_compare frame=%u "
            "compared=%llu passed=%llu mismatched=%llu\n",
            frameNum,
            static_cast<unsigned long long>(totalCompared),
            static_cast<unsigned long long>(totalPassed),
            static_cast<unsigned long long>(totalMismatched));
        std::fflush(stderr);
    }
}

void ParityPrintMismatchByIndex(unsigned frame,
                                uint32_t typeId,
                                uint32_t instIdx,
                                uint32_t vertIdx,
                                uint32_t cpuARGB,
                                uint32_t gpuARGB) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    std::fprintf(stderr,
        "[OBJECT_PARITY v1] event=lighting_mismatch frame=%u "
        "typeId=%u inst=%u vert=%u cpu=0x%08X gpu=0x%08X\n",
        frame, typeId, instIdx, vertIdx, cpuARGB, gpuARGB);
    std::fflush(stderr);
}

}  // namespace gos_object_parity

// ---------------------------------------------------------------------------
// Mismatch printer + 600-frame summary.
//
// Stage 2.D.1: ParityPrintMismatch is a SKELETON (matching the spec schema)
// that no caller invokes yet. ParityFrameTick is invoked unconditionally
// from GpuStaticPropBatcher::flush(); when the env-gate is off it is a
// no-op (single branch-predicted bool load).
// Stage 2.D.2: ParityPrintMismatchByIndex defined earlier (in the CompareAndReport
// section) since it shares s_parityMismatchesThisFrame. Storage for that counter
// consolidated in the Stage 2.D.2 anonymous namespace block above.
// ---------------------------------------------------------------------------
namespace gos_object_parity {

void ParityPrintMismatch(int frame, const char* actor, int tri, int corner,
                         uint32_t cpuARGB, uint32_t gpuARGB) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    std::fprintf(stderr,
        "[OBJECT_PARITY v1] event=lighting_mismatch frame=%d actor=%s "
        "tri=%d corner=%d cpu=0x%08X gpu=0x%08X\n",
        frame, actor ? actor : "?", tri, corner, cpuARGB, gpuARGB);
    std::fflush(stderr);
}

void ParityFrameTick() {
    // Default-off short-circuit: when MC2_OBJECT_PARITY_CHECK is unset, no
    // counters were ticked, no SSBO was allocated, and no summary line is
    // useful. Single bool load = same cost as the unconditional call site.
    if (!IsParityCheckEnabled()) return;

    ++s_paritySummaryFrames;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;

    // Roll per-frame counters into totals.
    s_vertices_written_total += s_vertices_written_this_frame;
    s_readback_bytes_total   += s_readback_bytes_this_frame;
    s_vertices_written_this_frame = 0;
    s_readback_bytes_this_frame   = 0;
    // Stage 2.D.1.1 (Item 1): roll per-frame slot-overflow count.
    Counters_ResetSlotOverflowsThisFrame();

    if (s_paritySummaryFrames % 600 == 0) {
        // Stage 2.D.2: extended summary with compared/passed/mismatched fields.
        std::fprintf(stderr,
            "[OBJECT_PARITY v1] event=summary frames=%lld "
            "vertices_written=%llu readback_bytes=%llu mismatches=%lld "
            "slot_overflows=%llu compared=%llu passed=%llu mismatched=%llu\n",
            s_paritySummaryFrames,
            static_cast<unsigned long long>(s_vertices_written_total),
            static_cast<unsigned long long>(s_readback_bytes_total),
            s_paritySummaryMismatches,
            static_cast<unsigned long long>(s_slot_overflows_total),
            static_cast<unsigned long long>(s_compared_total),
            static_cast<unsigned long long>(s_passed_total),
            static_cast<unsigned long long>(s_mismatched_total));
        std::fflush(stderr);
    }
}

}  // namespace gos_object_parity
