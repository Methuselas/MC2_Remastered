// mc2_hitch_trace.h — H1a hitch attribution layer.
// Include AFTER all GL/GameOS includes in suspect TUs. Never include in
// shared or precompiled headers.
//
// Gates: MC2_HITCH_TRACE=1 (enable), MC2_HITCH_MS=<float> (threshold, default 20).
// Zero cost when disabled: g_mc2HitchEnabled is a static-init bool; MC2_GL_*
// macros reduce to raw GL calls with a single predicted-branch bool check.
#pragma once
#include <chrono>
#include <cstdint>

namespace mc2_hitch {

// ---------------------------------------------------------------------------
// Accumulator — reset each BeginFrame, read each EndFrame.
// ---------------------------------------------------------------------------
struct HitchFrameAccum {
    // Wall-clock spans (ms). Accumulated by HitchScope.
    double terrainSolidMs       = 0.0;
    double staticFlushMs        = 0.0;
    double staticSnapshotMs     = 0.0;
    double waterMs              = 0.0;

    // Terrain solid dispatch sub-counters (filled by gos_terrain_indirect.cpp)
    uint32_t terrainBinds       = 0;
    uint32_t terrainUploads     = 0;   // glBufferData + glBufferSubData count
    uint32_t terrainTexImageCalls = 0;

    // Static-prop flush sub-counters (filled by gos_static_prop_batcher.cpp)
    uint32_t staticRecords      = 0;   // caller fills after flush
    uint32_t staticDrawCmds     = 0;   // caller fills after flush
    uint64_t staticBufferBytes  = 0;   // cumulative from MC2_GL_* wrappers
    uint32_t staticRealloc      = 0;   // caller sets on buffer orphan/realloc
    uint32_t staticMap          = 0;   // glMapBufferRange count

    // Water sub-counters (filled by gos_terrain_water_stream.cpp)
    uint8_t  waterCalled        = 0;
    uint8_t  waterPresent       = 0;
    uint8_t  waterEarlyOut      = 0;
    uint32_t waterGlCalls       = 0;   // total MC2_GL_* hits inside water scope

    // GL call counts (all instrumented TUs combined)
    uint32_t glBufferDataCalls        = 0;
    uint64_t glBufferDataBytes        = 0;
    uint32_t glBufferSubDataCalls     = 0;
    uint64_t glBufferSubDataBytes     = 0;
    uint32_t glMapBufferCalls         = 0;
    uint32_t glTexImageCalls          = 0;
    uint64_t glTexImageBytes          = 0;
    uint32_t glTexSubImageCalls       = 0;
    uint64_t glTexSubImageBytes       = 0;
    uint32_t glBindTextureCalls       = 0;
    uint32_t glUseProgramCalls        = 0;
    uint32_t glClientWaitSyncCalls    = 0;
    uint32_t glClientWaitSyncSignaled = 0;
    uint32_t glClientWaitSyncTimedOut = 0;
    uint32_t glFenceSyncCalls         = 0;
    uint32_t glFinishCalls            = 0;
    uint32_t glFlushCalls             = 0;
};

// ---------------------------------------------------------------------------
// Span kinds — one per sub-system.
// ---------------------------------------------------------------------------
enum class HitchSpanKind : uint8_t {
    TerrainSolidDispatch    = 0,
    GpuStaticPropsFlush     = 1,
    GpuStaticPropsSnapshot  = 2,
    WaterFastPath           = 3,
    kCount
};

// ---------------------------------------------------------------------------
// Public API — implemented in mc2_hitch_trace.cpp.
// ---------------------------------------------------------------------------
void BeginFrame(uint32_t frame);
void EndFrame(uint32_t frame, double dtMs);
void SpanBegin(HitchSpanKind kind);
void SpanEnd(HitchSpanKind kind);

// ---------------------------------------------------------------------------
// RAII scope — records wall-clock span for one sub-system.
// Zero cost (no clock call) when disabled.
// ---------------------------------------------------------------------------
struct HitchScope {
    HitchSpanKind kind_;
    explicit HitchScope(HitchSpanKind k) : kind_(k) { SpanBegin(k); }
    ~HitchScope() { SpanEnd(kind_); }
    HitchScope(const HitchScope&)            = delete;
    HitchScope& operator=(const HitchScope&) = delete;
};

} // namespace mc2_hitch

// ---------------------------------------------------------------------------
// Globals exposed so MC2_GL_* macros inline without a function call.
// ---------------------------------------------------------------------------
extern bool                        g_mc2HitchEnabled;
extern mc2_hitch::HitchFrameAccum  g_mc2HitchAccum;

// ---------------------------------------------------------------------------
// MC2_GL_* macro wrappers.
// Use these in place of the raw GL calls in instrumented TUs.
// Value-returning calls (glClientWaitSync, glMapBufferRange, glFenceSync)
// use a lambda so the return value passes through correctly.
// ---------------------------------------------------------------------------

#define MC2_GL_BufferData(target, size, data, usage)                        \
    do {                                                                    \
        if (g_mc2HitchEnabled) {                                            \
            ++g_mc2HitchAccum.glBufferDataCalls;                            \
            g_mc2HitchAccum.glBufferDataBytes += (uint64_t)(size);          \
        }                                                                   \
        glBufferData(target, size, data, usage);                            \
    } while(0)

#define MC2_GL_BufferSubData(target, offset, size, data)                    \
    do {                                                                    \
        if (g_mc2HitchEnabled) {                                            \
            ++g_mc2HitchAccum.glBufferSubDataCalls;                         \
            g_mc2HitchAccum.glBufferSubDataBytes += (uint64_t)(size);       \
        }                                                                   \
        glBufferSubData(target, offset, size, data);                        \
    } while(0)

#define MC2_GL_MapBufferRange(target, offset, length, access)               \
    [&]() -> void* {                                                        \
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.glMapBufferCalls;         \
        return glMapBufferRange(target, offset, length, access);            \
    }()

#define MC2_GL_TexImage2D(target, level, ifmt, w, h, brd, fmt, type, px)   \
    do {                                                                    \
        if (g_mc2HitchEnabled) {                                            \
            ++g_mc2HitchAccum.glTexImageCalls;                              \
            /* Estimate bytes: width * height * 4 (conservative RGBA8) */  \
            g_mc2HitchAccum.glTexImageBytes +=                              \
                (uint64_t)(w) * (uint64_t)(h) * 4u;                        \
        }                                                                   \
        glTexImage2D(target, level, ifmt, w, h, brd, fmt, type, px);       \
    } while(0)

#define MC2_GL_TexSubImage2D(target, level, xoff, yoff, w, h, fmt, type, px) \
    do {                                                                    \
        if (g_mc2HitchEnabled) {                                            \
            ++g_mc2HitchAccum.glTexSubImageCalls;                           \
            g_mc2HitchAccum.glTexSubImageBytes +=                           \
                (uint64_t)(w) * (uint64_t)(h) * 4u;                        \
        }                                                                   \
        glTexSubImage2D(target, level, xoff, yoff, w, h, fmt, type, px);   \
    } while(0)

#define MC2_GL_BindTexture(target, tex)                                     \
    do {                                                                    \
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.glBindTextureCalls;       \
        glBindTexture(target, tex);                                         \
    } while(0)

#define MC2_GL_UseProgram(prog)                                             \
    do {                                                                    \
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.glUseProgramCalls;        \
        glUseProgram(prog);                                                 \
    } while(0)

#define MC2_GL_ClientWaitSync(sync, flags, timeout)                         \
    [&]() -> GLenum {                                                       \
        GLenum _r = glClientWaitSync(sync, flags, timeout);                 \
        if (g_mc2HitchEnabled) {                                            \
            ++g_mc2HitchAccum.glClientWaitSyncCalls;                        \
            if (_r == GL_ALREADY_SIGNALED || _r == GL_CONDITION_SATISFIED)  \
                ++g_mc2HitchAccum.glClientWaitSyncSignaled;                 \
            else if (_r == GL_TIMEOUT_EXPIRED)                              \
                ++g_mc2HitchAccum.glClientWaitSyncTimedOut;                 \
        }                                                                   \
        return _r;                                                          \
    }()

#define MC2_GL_FenceSync(condition, flags)                                  \
    [&]() -> GLsync {                                                       \
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.glFenceSyncCalls;         \
        return glFenceSync(condition, flags);                               \
    }()

#define MC2_GL_Finish()                                                     \
    do {                                                                    \
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.glFinishCalls;            \
        glFinish();                                                         \
    } while(0)

#define MC2_GL_Flush()                                                      \
    do {                                                                    \
        if (g_mc2HitchEnabled) ++g_mc2HitchAccum.glFlushCalls;             \
        glFlush();                                                          \
    } while(0)
