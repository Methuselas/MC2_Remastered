#include "gpu_cull_substrate.h"
#include "gpu_cull_parity.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// MC2_GPU_CULL_SUBSTRATE_TRACE=1 — verbose per-frame lifecycle prints.
// Default off; the 600-frame summary + first-flush banner emit unconditionally.
static const bool s_substrateTrace = (getenv("MC2_GPU_CULL_SUBSTRATE_TRACE") != nullptr);
#define SUBSTRATE_TRACE(fmt, ...) \
    do { if (s_substrateTrace) { printf("[GPU_CULL v1] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while (0)

namespace gpu_cull {

// ---------------------------------------------------------------------------
// Ring constants
// ---------------------------------------------------------------------------

constexpr uint32_t RING_FRAMES             = 3;
// Binding point 8: 0-3 are static-prop, 5-6 are water. 4 and 7 are unused.
// 8 is the first binding clearly clear of all existing paths.
constexpr uint32_t SUBSTRATE_SSBO_BINDING  = 8;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static GLuint    s_instanceSsbo   = 0;
static void*     s_mappedPtr      = nullptr;
static GLsync    s_fence[RING_FRAMES] = {0, 0, 0};
static uint32_t  s_frameSlot      = 0;

static uint32_t  s_maxActors      = 0;   // set at init, immutable after
static uint32_t  s_perFrameCount  = 0;   // records submitted this frame
static bool      s_initialized    = false;

// 600-frame summary counter.
static uint32_t  s_flushCount     = 0;
static bool      s_firstFlushDone = false;

// C1a: CPU-visible count from last flushUpload() — count of prevVisibilityBit==1 records.
static uint32_t  s_cpuVisibleCount = 0;

// Byte size of one ring slot: header + record array.
static size_t    s_slotBytes      = 0;

// ---------------------------------------------------------------------------
// Env probe (lazy, cached)
// ---------------------------------------------------------------------------

static bool substrate_isEnabled_impl() {
    const char* v = getenv("MC2_GPU_CULL_SUBSTRATE");
    return v != nullptr && v[0] != '0';
}

bool substrate_isEnabled() {
    static bool s_enabled = substrate_isEnabled_impl();
    return s_enabled;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

GLuint substrate_getInstanceSsboName() {
    return s_initialized ? s_instanceSsbo : 0;
}

uint32_t substrate_getInstanceSsboBindingPoint() {
    return SUBSTRATE_SSBO_BINDING;
}

uint32_t substrate_getCpuVisibleCount() {
    return s_cpuVisibleCount;
}

GLintptr substrate_getCurrentSlotOffset() {
    if (!s_initialized) return 0;
    return static_cast<GLintptr>(s_frameSlot * s_slotBytes);
}

GLsizeiptr substrate_getSlotBytes() {
    if (!s_initialized) return 0;
    return static_cast<GLsizeiptr>(s_slotBytes);
}

// ---------------------------------------------------------------------------
// substrate_init
// ---------------------------------------------------------------------------

void substrate_init(uint32_t maxActors) {
    if (!substrate_isEnabled()) return;
    if (s_initialized) {
        // Reinit after a prior mission — shut down first.
        substrate_shutdown();
    }

    s_maxActors      = maxActors;
    s_perFrameCount  = 0;
    s_frameSlot      = 0;
    s_flushCount     = 0;
    s_firstFlushDone = false;

    // Per-slot layout: [GpuActorRecordHeader][GpuActorRecord * maxActors]
    s_slotBytes = sizeof(GpuActorRecordHeader) + maxActors * sizeof(GpuActorRecord);
    const GLsizeiptr totalBytes = static_cast<GLsizeiptr>(RING_FRAMES * s_slotBytes);

    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags     = storageFlags;

    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, storageFlags);
    s_mappedPtr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, totalBytes, mapFlags);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_mappedPtr) {
        fprintf(stderr, "[GPU_CULL v1] FATAL: persistent map failed\n");
        fflush(stderr);
        abort();
    }

    // Zero the entire buffer so unwritten records read as zero.
    memset(s_mappedPtr, 0, static_cast<size_t>(totalBytes));

    s_initialized = true;

    printf("[GPU_CULL v1] event=substrate_init maxActors=%u slotBytes=%zu totalBytes=%lld binding=%u\n",
           maxActors, s_slotBytes, (long long)totalBytes, SUBSTRATE_SSBO_BINDING);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// substrate_shutdown
// ---------------------------------------------------------------------------

void substrate_shutdown() {
    if (!substrate_isEnabled()) return;

    // Wait for all in-flight slots before releasing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }

    if (s_instanceSsbo) {
        if (s_mappedPtr) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            s_mappedPtr = nullptr;
        }
        glDeleteBuffers(1, &s_instanceSsbo);
        s_instanceSsbo = 0;
    }
    s_mappedPtr      = nullptr;
    s_maxActors      = 0;
    s_perFrameCount  = 0;
    s_frameSlot      = 0;
    s_slotBytes      = 0;
    s_initialized    = false;
    s_firstFlushDone = false;

    printf("[GPU_CULL v1] event=substrate_shutdown\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// substrate_frameBegin
// ---------------------------------------------------------------------------

void substrate_frameBegin() {
    if (!substrate_isEnabled() || !s_initialized) return;

    // Advance ring slot.
    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;

    // Wait on this slot's fence if it has not yet been signalled.
    // At 60 Hz with 3-deep ring this should never stall, but be defensive.
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    // Reset the per-frame record count for this slot.
    s_perFrameCount = 0;

    SUBSTRATE_TRACE("event=frame_begin slot=%u", s_frameSlot);
}

// ---------------------------------------------------------------------------
// substrate_submitDynamicActor
// ---------------------------------------------------------------------------

void substrate_submitDynamicActor(const GpuActorRecord& rec) {
    if (!substrate_isEnabled() || !s_initialized) return;
    if (!s_mappedPtr) return;

    if (s_perFrameCount >= s_maxActors) {
        // Overflow: clamp and log (once per overflow event).
        if (s_perFrameCount == s_maxActors) {
            printf("[GPU_CULL v1] event=substrate_overflow at=%u cap=%u\n",
                   s_perFrameCount, s_maxActors);
            fflush(stdout);
        }
        return;
    }

    // Destination = slot_base + sizeof(header) + record_index * sizeof(record)
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    char* dest = static_cast<char*>(s_mappedPtr)
                 + slotOffset
                 + sizeof(GpuActorRecordHeader)
                 + s_perFrameCount * sizeof(GpuActorRecord);
    memcpy(dest, &rec, sizeof(GpuActorRecord));
    ++s_perFrameCount;
}

// ---------------------------------------------------------------------------
// substrate_flushUpload
// ---------------------------------------------------------------------------

void substrate_flushUpload() {
    if (!substrate_isEnabled() || !s_initialized) return;

    // Write the header for this slot.
    const size_t slotOffset = s_frameSlot * s_slotBytes;
    GpuActorRecordHeader* hdr =
        reinterpret_cast<GpuActorRecordHeader*>(
            static_cast<char*>(s_mappedPtr) + slotOffset);

    // C1a: count CPU-visible records (prevVisibilityBit==1) for parity summary.
    {
        uint32_t cpuVis = 0;
        const GpuActorRecord* recs = reinterpret_cast<const GpuActorRecord*>(
            static_cast<const char*>(s_mappedPtr) + slotOffset + sizeof(GpuActorRecordHeader));
        for (uint32_t i = 0; i < s_perFrameCount; ++i) {
            if (recs[i].prevVisibilityBit) ++cpuVis;
        }
        s_cpuVisibleCount = cpuVis;
    }

    hdr->recordCount    = s_perFrameCount;
    hdr->recordCapacity = s_maxActors;
    hdr->visibleCount   = 0;   // written by compute (C1+); CPU sets 0
    hdr->_pad0          = 0;

    // Buffer is coherent (GL_MAP_COHERENT_BIT) — no explicit glFlushMappedBufferRange needed.

    // Insert a fence for this slot so the next visit to this ring slot
    // waits until the GPU has finished reading it.
    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // 600-frame summary + first-flush banner.
    ++s_flushCount;
    const bool firstFlush = !s_firstFlushDone;
    s_firstFlushDone = true;

    if (firstFlush || (s_flushCount % 600) == 0) {
        printf("[GPU_CULL v1] event=substrate_ready records=%u capacity=%u slot=%u\n",
               s_perFrameCount, s_maxActors, s_frameSlot);
        fflush(stdout);
    }

    gpu_cull::parity_flushSummary();

    SUBSTRATE_TRACE("event=flush_upload records=%u slot=%u", s_perFrameCount, s_frameSlot);
}

} // namespace gpu_cull
