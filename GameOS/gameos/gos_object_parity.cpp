// GameOS/gameos/gos_object_parity.cpp
//
// Slice 2 (object-offload) — Stage 2.D.1: parity readback harness.
//
// See gos_object_parity.h for the slice overview, the schema reservations,
// the default-off contract, and the public API.
//
// Stage 2.D.1: env-gate readers, counter Add/Get pairs, parity-printer
// skeleton (not invoked by the batcher in 2.D.1 — exists for 2.D.2/2.D.3
// to plug into), and EnsureParityOutputSSBO/Release. The static-prop
// batcher edits in this same commit:
//   - Call EnsureParityOutputSSBO(kSlotBytes) before per-type draws when
//     IsParityCheckEnabled() is true.
//   - Bind the SSBO to binding=3 with the slot-relative range covering
//     instanceCount * vertexCount uint32 entries; set u_parityWrite=1u.
//   - After all per-type draws (the existing fence creation site at
//     gos_static_prop_batcher.cpp:1201), do an async-readback handshake
//     that piggybacks on the existing s_fence ring — at top of the next
//     visit to that slot, glClientWaitSync (already done by slice 1) makes
//     the buffer readable; we issue glGetBufferSubData of the slot range
//     and DISCARD the bytes (Stage 2.D.1 acceptance: pipeline runs, not
//     content correctness).
//   - Call ParityFrameTick() once per flush() at the bottom.
//
// Stage 2.D.2 plugs in P3 dual-emit (at first eligible frame, run BOTH
// MultiTransformShape and the GPU path, byte-compare listOfTriangles[].aRGBLight
// against parityOut[]). Stage 2.D.3 plugs in P1 round-robin sampler with
// parity_skipped_allowed_late_reg accounting.

#include "gos_object_parity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <GL/glew.h>

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
}  // namespace

namespace gos_object_parity {

void Counters_AddVerticesWrittenThisFrame(uint64_t n) {
    s_vertices_written_this_frame += n;
}

void Counters_AddReadbackBytesThisFrame(uint64_t n) {
    s_readback_bytes_this_frame += n;
}

uint64_t Counters_GetVerticesWrittenTotal() { return s_vertices_written_total; }
uint64_t Counters_GetReadbackBytesTotal()   { return s_readback_bytes_total; }

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

// Match slice 1's RING_FRAMES literally (gos_static_prop_batcher.cpp:34).
constexpr unsigned kRingFrames = 3;

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
// Mismatch printer + 600-frame summary.
//
// Stage 2.D.1: ParityPrintMismatch is a SKELETON (matching the spec schema)
// that no caller invokes yet. ParityFrameTick is invoked unconditionally
// from GpuStaticPropBatcher::flush(); when the env-gate is off it is a
// no-op (single branch-predicted bool load).
// ---------------------------------------------------------------------------
namespace {
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryMismatches   = 0;
}  // namespace

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

    if (s_paritySummaryFrames % 600 == 0) {
        // Stage 2.D.1 minimal summary — enough fields to prove the harness
        // ticks across a tier1 mission and that the readback is producing
        // bytes. 2.D.2/2.D.3 add compared/passed/mismatched/
        // skipped_allowed_late_reg.
        std::fprintf(stderr,
            "[OBJECT_PARITY v1] event=summary frames=%lld "
            "vertices_written=%llu readback_bytes=%llu mismatches=%lld\n",
            s_paritySummaryFrames,
            static_cast<unsigned long long>(s_vertices_written_total),
            static_cast<unsigned long long>(s_readback_bytes_total),
            s_paritySummaryMismatches);
        std::fflush(stderr);
    }
}

}  // namespace gos_object_parity
