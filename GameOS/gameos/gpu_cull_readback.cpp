// gpu_cull_readback.cpp — Track C, Slice C2: async readback ring buffer.
//
// 3-slot readback ring + per-frame glFenceSync.
// Non-blocking tryConsume with three-tier fallback:
//   T1: N-1 slot ready     → use it
//   T2: N-2 slot ready     → use it, emit readback_fallback_n2
//   T3: both not ready     → conservative (all visible), emit readback_fallback_conservative
//
// Dual-buffer design (AMD RDNA3 compatibility):
//   s_gpuSsbo    — pure GPU buffer (no persistent-map flags); compute shader writes here.
//   s_stagingBuf — CPU-readable persistent-mapped buffer (MAP_READ | PERSISTENT | COHERENT);
//                  compute_dispatch() copies current slot here post-dispatch.
//
// CPU reads only from s_stagingBuf; GPU writes only to s_gpuSsbo.
// This separates GPU-write memory (VRAM) from CPU-read memory (BAR/system RAM).
//
// glClientWaitSync timeout is ALWAYS 0 (zero) — never GL_TIMEOUT_IGNORED on hot path.

#include "gpu_cull_readback.h"
#include "gpu_cull_substrate.h"  // substrate_getCpuVisibleCount(), substrate_getSlotRecords()
#include "gpu_cull_record.h"     // GpuActorRecord
#include "gos_profiler.h"        // Tracy.hpp + TracyPlot
#include <gameos.hpp>            // STOP()

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Env-gated trace macro.
static const bool s_readbackTrace = (getenv("MC2_GPU_CULL_READBACK_TRACE") != nullptr);
#define RB_TRACE(fmt, ...) \
    do { if (s_readbackTrace) { printf("[GPU_CULL v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

namespace gpu_cull {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr uint32_t RING_FRAMES           = 3u;
// READBACK_SSBO_BINDING is exported from gpu_cull_readback.h (single source of truth).

// ---------------------------------------------------------------------------
// ReadbackHeader: 16 bytes, std430-aligned.
// ---------------------------------------------------------------------------
struct alignas(16) ReadbackHeader {
    uint32_t visibleCount;   // written by compute shader (atomicAdd)
    uint32_t frameIndex;     // CPU-provided frame index for validation
    uint32_t dilatedAdmits;  // motion-tolerance slice: actors admitted by dilated test only
    uint32_t _pad1;
};
static_assert(sizeof(ReadbackHeader) == 16, "ReadbackHeader must be 16 B");

// ---------------------------------------------------------------------------
// Env probes (lazy, cached)
// ---------------------------------------------------------------------------
bool readback_isEnabled() {
    static bool s_checked = false;
    static bool s_enabled = false;
    if (!s_checked) {
        s_checked = true;
        s_enabled = (getenv("MC2_GPU_CULL_READBACK") != nullptr);
    }
    return s_enabled;
}

// Conservative-OR snapshot consumption (motion-tolerance slice).
// Default ON: an actor is marked visible if either the freshest readback slot
// (N-1) OR the previous slot (N-2) reported it visible. Set to "0" to opt out.
// Lazy-evaluated static (canonical shape, see gos_terrain_indirect.cpp).
static bool readback_isConservativeOrEnabled() {
    static bool s_inited = false;
    static bool s_value  = true;
    if (!s_inited) {
        s_inited = true;
        const char* v = getenv("MC2_GPU_CULL_CONSERVATIVE_OR");
        if (v && v[0] == '0' && v[1] == '\0') s_value = false;
    }
    return s_value;
}

static bool s_forceNotReady = false;  // set at init time from env

// Accumulators for the [GPU_CULL v1] event=motion_tolerance summary.
// dilated_admits: per-frame value pulled from ReadbackHeader.dilatedAdmits, summed.
// conservative_or_admits: actors flipped 0->1 by the OR-merge over what N-1 alone gave.
static uint64_t s_accDilatedAdmits      = 0u;
static uint64_t s_accConservativeOrAdmits = 0u;
static uint32_t s_motionTolFrames       = 0u;
static uint32_t s_motionTolSummaryCount = 0u;
static bool     s_motionTolFirstDone    = false;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static GLuint     s_gpuSsbo                    = 0;  // GPU-only SSBO (compute writes here)
static GLuint     s_stagingBuf                 = 0;  // CPU-readable persistent-mapped staging
static void*      s_stagingMapped              = nullptr;
static GLsync     s_readbackFence[RING_FRAMES] = {nullptr, nullptr, nullptr};
static uint32_t   s_currentSlot               = 0;  // next slot to write fence for
static uint32_t   s_lastGoodSlot              = UINT32_MAX;
static uint32_t   s_lastGoodVisibleCount      = UINT32_MAX;
static uint32_t   s_maxActors                 = 0;
static size_t     s_slotBytes                 = 0;
static bool       s_initialized               = false;

// 600-frame summary counter.
static uint32_t   s_consumeCount              = 0;
static bool       s_firstConsumeDone          = false;

// M-4: stale-slot protection. When both readback fences miss (T3) for this many
// consecutive frames, reset s_lastGoodSlot to UINT32_MAX (conservative) so callers
// stop using data that is arbitrarily old (GPU stall / driver freeze scenario).
static constexpr uint32_t MAX_STALE_FRAMES    = 10u;
static uint32_t   s_staleFrameCount           = 0u;

// Per-actor GPU visibility snapshot. Declared at module scope (before
// readback_selftest) so the selftest can synthesize OR-merge bit patterns
// without forward-referencing.
constexpr uint32_t MAX_ACTOR_HANDLE = 4096u;
static uint8_t s_actorVis[MAX_ACTOR_HANDLE];  // 0=invisible, 1=visible

// 600-call counter for lifecycle_snapshot log line (readback_buildActorVisSnapshot).
// Defined here so readback_init/readback_shutdown can reset them without forward-ref.
static uint32_t   s_snapshotCallCount         = 0u;
static bool       s_snapshotFirstDone         = false;

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
GLuint readback_getSsboBuf() {
    // Returns the GPU-side SSBO; compute shader binds this for writes.
    return s_initialized ? s_gpuSsbo : 0;
}

GLuint readback_getStagingBuf() {
    // Returns the CPU-readable staging buffer; diagnostics read from here.
    return s_initialized ? s_stagingBuf : 0;
}

GLintptr readback_getCurrentSlotOffset() {
    if (!s_initialized) return 0;
    return static_cast<GLintptr>(s_currentSlot * s_slotBytes);
}

GLsizeiptr readback_getSlotBytes() {
    if (!s_initialized) return 0;
    return static_cast<GLsizeiptr>(s_slotBytes);
}

uint32_t readback_getLastGoodVisibleCount() {
    return s_lastGoodVisibleCount;
}

void readback_zeroCurrentSlotVisibleCount() {
    if (!s_initialized || !s_gpuSsbo) return;
    // Zero the entire ReadbackHeader (16 B = 4 uints: rb_visibleCount,
    // rb_frameIndex, rb_dilatedAdmits, rb_pad1) so atomicAdd accumulators
    // start at 0 each frame. Pure GPU operation (no CPU map).
    const GLuint zero = 0u;
    glClearNamedBufferSubData(s_gpuSsbo, GL_R32UI,
                              static_cast<GLintptr>(s_currentSlot * s_slotBytes),
                              static_cast<GLsizeiptr>(sizeof(ReadbackHeader)),
                              GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

// ---------------------------------------------------------------------------
// readback_init
// ---------------------------------------------------------------------------
bool readback_init(uint32_t maxActors) {
    if (!readback_isEnabled()) return true;

    if (s_initialized) {
        readback_shutdown();
    }

    s_forceNotReady = (getenv("MC2_GPU_CULL_FORCE_FENCE_NOT_READY") != nullptr);

    s_maxActors    = maxActors;
    s_currentSlot  = 0;
    s_lastGoodSlot = UINT32_MAX;
    s_lastGoodVisibleCount = UINT32_MAX;
    s_consumeCount = 0;
    s_firstConsumeDone = false;
    s_snapshotCallCount = 0u;
    s_snapshotFirstDone = false;
    s_staleFrameCount = 0u;
    s_accDilatedAdmits        = 0u;
    s_accConservativeOrAdmits = 0u;
    s_motionTolFrames         = 0u;
    s_motionTolSummaryCount   = 0u;
    s_motionTolFirstDone      = false;

    // Per-slot layout: [ReadbackHeader][uint32_t * maxActors]
    // Align slot size to GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT.
    GLint ssboAlignment = 256;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlignment);
    if (ssboAlignment < 256) ssboAlignment = 256;

    const size_t rawSlotBytes = sizeof(ReadbackHeader) + maxActors * sizeof(uint32_t);
    s_slotBytes = (rawSlotBytes + static_cast<size_t>(ssboAlignment) - 1u)
                  & ~(static_cast<size_t>(ssboAlignment) - 1u);

    const GLsizeiptr totalBytes = static_cast<GLsizeiptr>(RING_FRAMES * s_slotBytes);

    // --- GPU-side SSBO (compute shader writes here) ---
    // GL_DYNAMIC_STORAGE_BIT: allows glClearNamedBufferSubData to zero rb_visibleCount.
    // No map flags — pure GPU memory (VRAM on RDNA3), ensuring atomicAdd writes work.
    glGenBuffers(1, &s_gpuSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_gpuSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Zero GPU SSBO so unwritten slots read as zero on first sync readback.
    {
        const GLuint zero = 0u;
        glClearNamedBufferSubData(s_gpuSsbo, GL_R32UI, 0, totalBytes,
                                  GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    }

    // --- CPU staging buffer (CPU reads from here via persistent map) ---
    // MAP_READ|WRITE + MAP_PERSISTENT + MAP_COHERENT: read-write persistent map.
    // GPU writes to s_gpuSsbo, then glCopyBufferSubData copies to this staging buffer.
    // WRITE_BIT is required because readback_frameEnd() also writes hdr->frameIndex
    // into the staging buffer (CPU side). Writing to a READ_BIT-only map is spec-UB.
    // On AMD RDNA3 this pattern works reliably because:
    //   - s_gpuSsbo lives in pure VRAM (compute atomics work correctly)
    //   - s_stagingBuf lives in CPU-visible BAR memory (CPU reads work correctly)
    //   - glCopyBufferSubData crosses the VRAM→BAR boundary explicitly
    const GLbitfield stageStorageFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
                                       | GL_MAP_PERSISTENT_BIT
                                       | GL_MAP_COHERENT_BIT;
    const GLbitfield stageMapFlags     = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT
                                       | GL_MAP_PERSISTENT_BIT
                                       | GL_MAP_COHERENT_BIT;

    glGenBuffers(1, &s_stagingBuf);
    glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingBuf);
    glBufferStorage(GL_COPY_WRITE_BUFFER, totalBytes, nullptr, stageStorageFlags);
    s_stagingMapped = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, totalBytes, stageMapFlags);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    if (!s_stagingMapped) {
        fprintf(stderr, "[GPU_CULL v1] FATAL: readback staging persistent map failed\n");
        fflush(stderr);
        abort();
    }

    // Zero staging so unread slots return zero.
    memset(s_stagingMapped, 0, static_cast<size_t>(totalBytes));

    // Clear fences.
    for (uint32_t i = 0; i < RING_FRAMES; ++i)
        s_readbackFence[i] = nullptr;

    s_initialized = true;

    printf("[GPU_CULL v1] event=readback_init maxActors=%u slotBytes=%zu totalBytes=%lld"
           " ssboAlignment=%d binding=%u forceNotReady=%d dual_buf=1\n",
           maxActors, s_slotBytes, (long long)totalBytes,
           ssboAlignment, READBACK_SSBO_BINDING, (int)s_forceNotReady);
    fflush(stdout);

    // Run the three-tier selftest now that the ring is initialized.
    readback_selftest();

    return true;
}

// ---------------------------------------------------------------------------
// readback_shutdown
// ---------------------------------------------------------------------------
void readback_shutdown() {
    if (!readback_isEnabled()) return;

    // Wait for all in-flight fences before releasing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_readbackFence[i]) {
            glClientWaitSync(s_readbackFence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_readbackFence[i]);
            s_readbackFence[i] = nullptr;
        }
    }

    if (s_stagingBuf) {
        if (s_stagingMapped) {
            glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingBuf);
            glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            s_stagingMapped = nullptr;
        }
        glDeleteBuffers(1, &s_stagingBuf);
        s_stagingBuf = 0;
    }

    if (s_gpuSsbo) {
        glDeleteBuffers(1, &s_gpuSsbo);
        s_gpuSsbo = 0;
    }

    s_maxActors    = 0;
    s_slotBytes    = 0;
    s_currentSlot  = 0;
    s_lastGoodSlot = UINT32_MAX;
    s_lastGoodVisibleCount = UINT32_MAX;
    s_staleFrameCount = 0u;
    s_initialized  = false;
    s_firstConsumeDone = false;
    s_snapshotCallCount = 0u;
    s_snapshotFirstDone = false;

    printf("[GPU_CULL v1] event=readback_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// readback_tryConsume — NEVER blocks (timeout=0 on all glClientWaitSync calls)
// ---------------------------------------------------------------------------
ReadbackTier readback_tryConsume() {
    if (!readback_isEnabled() || !s_initialized) {
        return ReadbackTier::Tier3_Conservative;
    }

    const uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;
    const uint32_t n2Slot = (s_currentSlot + RING_FRAMES - 2u) % RING_FRAMES;

    // Helper: check whether a fence is signaled (non-blocking).
    auto isFenceReady = [&](uint32_t slot) -> bool {
        if (s_forceNotReady) return false;
        if (!s_readbackFence[slot]) return false;
        GLenum result = glClientWaitSync(s_readbackFence[slot], 0, 0);
        return (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED);
    };

    // --- Tier 1: N-1 (most recent) ---
    if (isFenceReady(n1Slot)) {
        glDeleteSync(s_readbackFence[n1Slot]);
        s_readbackFence[n1Slot] = nullptr;
        glInvalidateBufferSubData(s_stagingBuf,
            static_cast<GLintptr>(n1Slot * s_slotBytes),
            static_cast<GLsizeiptr>(s_slotBytes));

        s_lastGoodSlot = n1Slot;

        // Read visible count from staging buffer (CPU-side, copied from GPU SSBO post-dispatch).
        const ReadbackHeader* hdr = reinterpret_cast<const ReadbackHeader*>(
            static_cast<const char*>(s_stagingMapped) + n1Slot * s_slotBytes);
        s_lastGoodVisibleCount = hdr->visibleCount;

        if (s_readbackTrace) {
            printf("[GPU_CULL v1] DEBUG tier1 slot=%u visCount_staged=%u\n",
                   n1Slot, s_lastGoodVisibleCount);
            fflush(stdout);
        }

        // Tracy plots.
        {
            const uint32_t cpuVis = substrate_getCpuVisibleCount();
            TracyPlot("GPU.VisibleCount.GPU", (int64_t)s_lastGoodVisibleCount);
            TracyPlot("GPU.VisibleCount.CPU", (int64_t)cpuVis);
        }

        s_staleFrameCount = 0u;  // M-4: fence ready — staleness cleared
        ++s_consumeCount;
        const bool doLog = !s_firstConsumeDone || ((s_consumeCount % 600u) == 0u);
        if (doLog) {
            s_firstConsumeDone = true;
            printf("[GPU_CULL v1] event=readback_ok slot=%u stale_frames=0 visibleCount=%u\n",
                   n1Slot, s_lastGoodVisibleCount);
            fflush(stdout);
        }

        RB_TRACE("event=consume_tier1 slot=%u visible=%u", n1Slot, s_lastGoodVisibleCount);
        return ReadbackTier::Tier1_NMinusOne;
    }

    // --- Tier 2: N-2 (one frame older) ---
    if (isFenceReady(n2Slot)) {
        glDeleteSync(s_readbackFence[n2Slot]);
        s_readbackFence[n2Slot] = nullptr;
        glInvalidateBufferSubData(s_stagingBuf,
            static_cast<GLintptr>(n2Slot * s_slotBytes),
            static_cast<GLsizeiptr>(s_slotBytes));

        s_lastGoodSlot = n2Slot;

        const ReadbackHeader* hdr = reinterpret_cast<const ReadbackHeader*>(
            static_cast<const char*>(s_stagingMapped) + n2Slot * s_slotBytes);
        s_lastGoodVisibleCount = hdr->visibleCount;

        // Tracy plots (T2 — 2 frames stale, still useful).
        {
            const uint32_t cpuVis = substrate_getCpuVisibleCount();
            TracyPlot("GPU.VisibleCount.GPU", (int64_t)s_lastGoodVisibleCount);
            TracyPlot("GPU.VisibleCount.CPU", (int64_t)cpuVis);
        }

        s_staleFrameCount = 0u;  // M-4: fence ready — staleness cleared
        printf("[GPU_CULL v1] event=readback_fallback_n2 slot=%u stale_frames=2\n",
               n2Slot);
        fflush(stdout);

        RB_TRACE("event=consume_tier2 slot=%u visible=%u", n2Slot, s_lastGoodVisibleCount);
        return ReadbackTier::Tier2_NMinusTwo;
    }

    // --- Tier 3: conservative (both fences not ready) ---
    // M-4: track consecutive T3 misses; after MAX_STALE_FRAMES, abandon s_lastGoodSlot
    // so downstream callers revert to conservative (all-visible) rather than using
    // data that is arbitrarily stale (covers GPU stall / driver-freeze scenarios).
    ++s_staleFrameCount;
    if (s_staleFrameCount >= MAX_STALE_FRAMES && s_lastGoodSlot != UINT32_MAX) {
        printf("[GPU_CULL v1] event=readback_stale_reset stale_frames=%u — clearing last-good slot\n",
               s_staleFrameCount);
        fflush(stdout);
        s_lastGoodSlot = UINT32_MAX;
        s_lastGoodVisibleCount = UINT32_MAX;
    }
    printf("[GPU_CULL v1] event=readback_fallback_conservative stale=%u\n", s_staleFrameCount);
    fflush(stdout);

    RB_TRACE("event=consume_tier3_conservative stale=%u", s_staleFrameCount);
    return ReadbackTier::Tier3_Conservative;
}

// ---------------------------------------------------------------------------
// readback_frameEnd
// Called AFTER all GPU commands are issued (post compute_dispatch).
// 1. Place GL_SHADER_STORAGE_BARRIER_BIT to ensure SSBO writes are complete.
// 2. Copy current GPU SSBO slot → staging buffer (explicit GPU→CPU copy).
// 3. Place GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT for persistent-map visibility.
// 4. Insert glFenceSync for the current slot.
// 5. Advance s_currentSlot.
// ---------------------------------------------------------------------------
void readback_frameEnd(uint32_t frameIndex) {
    if (!readback_isEnabled() || !s_initialized) return;

    // 1. Ensure GPU SSBO writes (atomicAdd in compute shader) are complete
    //    before the copy. GL_SHADER_STORAGE_BARRIER_BIT covers SSBO writes.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // 2. Copy current GPU SSBO slot → staging buffer.
    //    This is the explicit VRAM→BAR crossing that AMD RDNA3 requires.
    const GLintptr  slotOff  = static_cast<GLintptr>(s_currentSlot * s_slotBytes);
    const GLsizeiptr slotSz  = static_cast<GLsizeiptr>(s_slotBytes);
    glBindBuffer(GL_COPY_READ_BUFFER,  s_gpuSsbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, s_stagingBuf);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                        slotOff, slotOff, slotSz);
    glBindBuffer(GL_COPY_READ_BUFFER,  0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    // 3. GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT: makes the copy result visible
    //    through the persistent-mapped pointer (s_stagingMapped).
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);

    // 4. Write header frameIndex into staging (CPU-side, for freshness check).
    ReadbackHeader* hdr = reinterpret_cast<ReadbackHeader*>(
        static_cast<char*>(s_stagingMapped) + s_currentSlot * s_slotBytes);
    hdr->frameIndex = frameIndex;

    // Delete previous fence for this slot if it exists.
    if (s_readbackFence[s_currentSlot]) {
        glDeleteSync(s_readbackFence[s_currentSlot]);
        s_readbackFence[s_currentSlot] = nullptr;
    }

    // 5. Place fence for the current slot.
    s_readbackFence[s_currentSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    RB_TRACE("event=frame_end slot=%u frameIndex=%u", s_currentSlot, frameIndex);

    // 6. Advance slot for next frame.
    s_currentSlot = (s_currentSlot + 1u) % RING_FRAMES;
}

// ---------------------------------------------------------------------------
// readback_selftest — hard-fails via STOP() on any unexercised tier.
// ---------------------------------------------------------------------------
void readback_selftest() {
    if (!readback_isEnabled()) return;

    // m-1: save persistent state that tryConsume mutates, restore after selftest
    // so test probes don't corrupt the production ring-buffer state.
    const uint32_t savedLastGoodSlot         = s_lastGoodSlot;
    const uint32_t savedLastGoodVisibleCount = s_lastGoodVisibleCount;
    const uint32_t savedStaleFrameCount      = s_staleFrameCount;

    int pass = 0;
    int fail = 0;

    // Mock: simulate all fences NOT ready → expect Tier3.
    {
        const bool savedForce = s_forceNotReady;
        s_forceNotReady = true;

        ReadbackTier t = readback_tryConsume();
        if (t == ReadbackTier::Tier3_Conservative) {
            ++pass;
        } else {
            ++fail;
            printf("[GPU_CULL v1] event=readback_selftest FAIL tier3 expected got=%d\n", (int)t);
            fflush(stdout);
        }
        s_forceNotReady = savedForce;
    }

    // Mock: simulate only N-2 fence ready → expect Tier2.
    {
        const uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;
        const uint32_t n2Slot = (s_currentSlot + RING_FRAMES - 2u) % RING_FRAMES;

        GLsync savedN1 = s_readbackFence[n1Slot];
        GLsync savedN2 = s_readbackFence[n2Slot];

        s_readbackFence[n1Slot] = nullptr;

        GLsync n2Fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        glClientWaitSync(n2Fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        s_readbackFence[n2Slot] = n2Fence;

        ReadbackTier t = readback_tryConsume();
        if (t == ReadbackTier::Tier2_NMinusTwo) {
            ++pass;
        } else {
            ++fail;
            printf("[GPU_CULL v1] event=readback_selftest FAIL tier2 expected got=%d\n", (int)t);
            fflush(stdout);
        }

        s_readbackFence[n1Slot] = savedN1;
        s_readbackFence[n2Slot] = savedN2;
    }

    // Mock: simulate N-1 fence ready → expect Tier1.
    {
        const uint32_t n1Slot = (s_currentSlot + RING_FRAMES - 1u) % RING_FRAMES;

        GLsync savedN1 = s_readbackFence[n1Slot];

        GLsync n1Fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        glClientWaitSync(n1Fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        s_readbackFence[n1Slot] = n1Fence;

        ReadbackTier t = readback_tryConsume();
        if (t == ReadbackTier::Tier1_NMinusOne) {
            ++pass;
        } else {
            ++fail;
            printf("[GPU_CULL v1] event=readback_selftest FAIL tier1 expected got=%d\n", (int)t);
            fflush(stdout);
        }

        s_readbackFence[n1Slot] = savedN1;
    }

    // m-1: restore state that tryConsume side-effected during the test probes.
    s_lastGoodSlot         = savedLastGoodSlot;
    s_lastGoodVisibleCount = savedLastGoodVisibleCount;
    s_staleFrameCount      = savedStaleFrameCount;

    // Conservative-OR synthetic bit-merge test (motion-tolerance slice).
    // Verifies the OR semantics on a 2-slot scenario without touching substrate:
    //   slot N-1 says actor 1=visible, actor 2=invisible
    //   slot N-2 says actor 1=invisible, actor 2=visible
    //   OR-merge expects both actors visible.
    {
        // Save & seed s_actorVis.
        uint8_t saved[8];
        memcpy(saved, s_actorVis, sizeof(saved));
        // Primary write (slot N-1): actor 1 visible, actor 2 invisible.
        s_actorVis[1] = 1u;
        s_actorVis[2] = 0u;
        // Synthetic secondary OR pass: actor 1 invisible, actor 2 visible.
        // Apply OR semantics manually (mirrors applySlot's secondary branch).
        const uint8_t synthPrev[3] = { 0u, 0u, 1u };  // index 0 unused
        uint32_t orFlipsTest = 0u;
        for (uint32_t id = 1u; id <= 2u; ++id) {
            if (synthPrev[id] && s_actorVis[id] == 0u) {
                s_actorVis[id] = 1u;
                ++orFlipsTest;
            }
        }
        const bool orOk = (s_actorVis[1] == 1u) && (s_actorVis[2] == 1u) && (orFlipsTest == 1u);
        if (orOk) {
            ++pass;
        } else {
            ++fail;
            printf("[GPU_CULL v1] event=readback_selftest FAIL or_merge actor1=%u actor2=%u flips=%u\n",
                   (unsigned)s_actorVis[1], (unsigned)s_actorVis[2], orFlipsTest);
            fflush(stdout);
        }
        memcpy(s_actorVis, saved, sizeof(saved));
    }

    printf("[GPU_CULL v1] event=readback_selftest pass=%d fail=%d\n", pass, fail);
    fflush(stdout);

    if (fail > 0) {
        STOP(("[GPU_CULL] readback_selftest failed %d/%d tiers — tier logic is broken",
              fail, pass + fail));
    }
}

// ---------------------------------------------------------------------------
// C3: per-actor GPU visibility snapshot
// ---------------------------------------------------------------------------
//
// Flat array indexed by actorId (== GameObjectHandle == objList index, 1-based).
// Default (all 1) = fail-open: actors not in last-good substrate slot stay visible.
// Only actors explicitly in the last-good readback slot can be marked invisible (0).
// MAX_ACTOR_HANDLE must exceed any valid actorId; MC2 maximum is ~2000 objects.
// (Declared at module scope above to allow readback_selftest forward use.)

void readback_buildActorVisSnapshot(uint32_t maxActorHandle) {
    // Clamp caller's bound to the fixed array size; memset always covers the full array
    // so entries above the caller's bound remain fail-open (visible).
    const uint32_t cap = (maxActorHandle < MAX_ACTOR_HANDLE) ? maxActorHandle : MAX_ACTOR_HANDLE;
    // Default: all visible (fail-open for T3 / disabled / no valid slot).
    memset(s_actorVis, 1, sizeof(s_actorVis));

    // T3 sentinel: UINT32_MAX means no valid readback yet (or T3 conservative).
    if (!readback_isEnabled() || s_lastGoodSlot == UINT32_MAX) {
        // Log on first call and every 600 calls so the operator can see the state.
        ++s_snapshotCallCount;
        const bool doLog = !s_snapshotFirstDone || ((s_snapshotCallCount % 600u) == 0u);
        if (doLog) {
            s_snapshotFirstDone = true;
            printf("[GPU_CULL v1] event=lifecycle_snapshot slot=none visible=0 invisible=0 conservative=1\n");
            fflush(stdout);
        }
        return;
    }

    // Also treat T3 (UINT32_MAX visible count) as all-visible.
    if (s_lastGoodVisibleCount == UINT32_MAX) {
        ++s_snapshotCallCount;
        const bool doLog = !s_snapshotFirstDone || ((s_snapshotCallCount % 600u) == 0u);
        if (doLog) {
            s_snapshotFirstDone = true;
            printf("[GPU_CULL v1] event=lifecycle_snapshot slot=%u visible=0 invisible=0 conservative=1\n",
                   s_lastGoodSlot);
            fflush(stdout);
        }
        return;
    }

    // Conservative-OR slot triple (motion-tolerance slice):
    //
    //   Readback slot K — substrate slot consumed by GPU is (K+1)%3.
    //   The "last good" readback slot s_lastGoodSlot (call it K) is consumed
    //   first. The previous readback slot (K-1)%3 holds an older but still
    //   valid GPU snapshot (it lived in the persistent staging buffer when it
    //   was current; the only risk reading it without a fence is partial
    //   data, which is fail-open — visibility OR-merge can only over-admit).
    //
    //   Conservative-OR: bit_visible(actor) =
    //       bit(slot=K)
    //     | bit(slot=(K-1)%3)
    //
    //   The "current substrate write" slot has not yet been consumed by the
    //   GPU (that happens in the *next* compute_dispatch), so it cannot
    //   contribute a per-actor-visibility decision and is intentionally
    //   excluded. Per-actor "always visible" flags already short-circuit the
    //   cull on the GPU side, so they need no extra handling here.
    //
    //   Default ON. MC2_GPU_CULL_CONSERVATIVE_OR=0 to opt out.
    const bool useOr = readback_isConservativeOrEnabled();
    const uint32_t prevReadbackSlot = (s_lastGoodSlot + RING_FRAMES - 1u) % RING_FRAMES;

    auto applySlot = [&](uint32_t readbackSlot, bool isPrimary,
                         uint32_t* outFlippedOnAlone, uint32_t* outFlippedByOr) {
        const uint32_t substrateSlotForReadback = (readbackSlot + 1u) % 3u;
        uint32_t recCount = 0u;
        const GpuActorRecord* recs = substrate_getSlotRecords(substrateSlotForReadback, &recCount);
        if (!recs || recCount == 0u) return false;

        const char* slotBase = static_cast<const char*>(s_stagingMapped)
                             + readbackSlot * s_slotBytes;
        const uint32_t* rbVis = reinterpret_cast<const uint32_t*>(slotBase + sizeof(ReadbackHeader));

        for (uint32_t i = 0u; i < recCount; ++i) {
            const uint32_t id = recs[i].actorId;
            if (id == 0u) continue;
            if (id >= MAX_ACTOR_HANDLE) {
                STOP(("[GPU_CULL] readback actor overflow: id=%u cap=%u; raise MAX_ACTOR_HANDLE", id, MAX_ACTOR_HANDLE));
            }
            if (id >= cap) continue;
            const uint8_t vis = (rbVis[i] != 0u) ? 1u : 0u;
            if (isPrimary) {
                // Primary pass: write the bit (overrides the all-1 default).
                s_actorVis[id] = vis;
                if (outFlippedOnAlone && vis) ++(*outFlippedOnAlone);
            } else {
                // Secondary (OR) pass: only flip 0 -> 1, count flips.
                if (vis && s_actorVis[id] == 0u) {
                    s_actorVis[id] = 1u;
                    if (outFlippedByOr) ++(*outFlippedByOr);
                }
            }
        }
        return true;
    };

    uint32_t primaryVisible = 0u;
    if (!applySlot(s_lastGoodSlot, true, &primaryVisible, nullptr)) {
        ++s_snapshotCallCount;
        const bool doLog = !s_snapshotFirstDone || ((s_snapshotCallCount % 600u) == 0u);
        if (doLog) {
            s_snapshotFirstDone = true;
            printf("[GPU_CULL v1] event=lifecycle_snapshot slot=%u visible=0 invisible=0 conservative=1\n",
                   s_lastGoodSlot);
            fflush(stdout);
        }
        return;
    }

    uint32_t orFlips = 0u;
    if (useOr && prevReadbackSlot != s_lastGoodSlot) {
        applySlot(prevReadbackSlot, false, nullptr, &orFlips);
    }

    // Counters for the motion_tolerance summary.
    s_accConservativeOrAdmits += orFlips;
    {
        const ReadbackHeader* hdr = reinterpret_cast<const ReadbackHeader*>(
            static_cast<const char*>(s_stagingMapped) + s_lastGoodSlot * s_slotBytes);
        s_accDilatedAdmits += hdr->dilatedAdmits;
    }
    ++s_motionTolFrames;
    ++s_motionTolSummaryCount;
    const bool doMotionLog = !s_motionTolFirstDone || ((s_motionTolSummaryCount % 600u) == 0u);
    if (doMotionLog) {
        s_motionTolFirstDone = true;
        printf("[GPU_CULL v1] event=motion_tolerance frames=%u dilated_admits=%llu conservative_or_admits=%llu or_enabled=%d\n",
               s_motionTolFrames,
               (unsigned long long)s_accDilatedAdmits,
               (unsigned long long)s_accConservativeOrAdmits,
               (int)useOr);
        fflush(stdout);
        s_accDilatedAdmits        = 0u;
        s_accConservativeOrAdmits = 0u;
        s_motionTolFrames         = 0u;
    }

    // Lifecycle snapshot stats (recount from final s_actorVis to reflect OR-merge).
    uint32_t nVisible = 0u, nInvisible = 0u;
    (void)primaryVisible;
    for (uint32_t id = 1u; id < cap; ++id) {
        if (s_actorVis[id]) ++nVisible; else ++nInvisible;
    }
    ++s_snapshotCallCount;
    const bool doLog = !s_snapshotFirstDone || ((s_snapshotCallCount % 600u) == 0u);
    if (doLog) {
        s_snapshotFirstDone = true;
        printf("[GPU_CULL v1] event=lifecycle_snapshot slot=%u visible=%u invisible=%u conservative=0 or_admits=%u\n",
               s_lastGoodSlot, nVisible, nInvisible, orFlips);
        fflush(stdout);
    }
}

bool readback_isActorVisibleLagged(uint32_t actorId) {
    // Fail-open: if readback is disabled, no valid slot, or out-of-range handle → visible.
    if (!readback_isEnabled() || s_lastGoodSlot == UINT32_MAX) return true;
    if (s_lastGoodVisibleCount == UINT32_MAX) return true;
    if (actorId == 0u || actorId >= MAX_ACTOR_HANDLE) return true;
    return s_actorVis[actorId] != 0u;
}

} // namespace gpu_cull
