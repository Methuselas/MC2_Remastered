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
#include "gpu_cull_substrate.h"  // substrate_getCpuVisibleCount()
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
    uint32_t _pad0;
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

static bool s_forceNotReady = false;  // set at init time from env

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
    // Zero rb_visibleCount (first uint32 in the GPU SSBO's current slot)
    // via glClearNamedBufferSubData (pure GPU operation — GPU buffer, no CPU map).
    const GLuint zero = 0u;
    glClearNamedBufferSubData(s_gpuSsbo, GL_R32UI,
                              static_cast<GLintptr>(s_currentSlot * s_slotBytes),
                              sizeof(uint32_t),
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
    s_initialized  = false;
    s_firstConsumeDone = false;

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

        printf("[GPU_CULL v1] event=readback_fallback_n2 slot=%u stale_frames=2\n",
               n2Slot);
        fflush(stdout);

        RB_TRACE("event=consume_tier2 slot=%u visible=%u", n2Slot, s_lastGoodVisibleCount);
        return ReadbackTier::Tier2_NMinusTwo;
    }

    // --- Tier 3: conservative (both fences not ready) ---
    printf("[GPU_CULL v1] event=readback_fallback_conservative\n");
    fflush(stdout);

    RB_TRACE("event=consume_tier3_conservative");
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

    printf("[GPU_CULL v1] event=readback_selftest pass=%d fail=%d\n", pass, fail);
    fflush(stdout);

    if (fail > 0) {
        STOP(("[GPU_CULL] readback_selftest failed %d/%d tiers — tier logic is broken",
              fail, pass + fail));
    }
}

} // namespace gpu_cull
