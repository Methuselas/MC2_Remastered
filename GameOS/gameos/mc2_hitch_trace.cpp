// mc2_hitch_trace.cpp — H1a hitch attribution layer implementation.
// This file does NOT include mc2_hitch_trace.h after GL headers to avoid
// recursive macro expansion. The header is included first before any GL
// headers that might be transitively pulled in.
#include "mc2_hitch_trace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>

// ---------------------------------------------------------------------------
// Globals (extern-declared in mc2_hitch_trace.h)
// ---------------------------------------------------------------------------
bool                        g_mc2HitchEnabled = false;
mc2_hitch::HitchFrameAccum  g_mc2HitchAccum   = {};

// ---------------------------------------------------------------------------
// Private state
// ---------------------------------------------------------------------------
namespace {

double s_thresholdMs = 20.0;

using Clock = std::chrono::steady_clock;
std::array<Clock::time_point, static_cast<size_t>(mc2_hitch::HitchSpanKind::kCount)>
    s_spanStart = {};

struct HitchInit {
    HitchInit() {
        g_mc2HitchEnabled = (getenv("MC2_HITCH_TRACE") != nullptr);
        if (g_mc2HitchEnabled) {
            const char* e = getenv("MC2_HITCH_MS");
            if (e && e[0]) {
                double v = atof(e);
                if (v > 0.0) s_thresholdMs = v;
            }
            fprintf(stderr,
                "[HITCH_INIT] enabled threshold=%.1fms\n", s_thresholdMs);
            fflush(stderr);
        }
    }
} s_hitchInit;

} // namespace

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
namespace mc2_hitch {

void BeginFrame(uint32_t /*frame*/) {
    if (!g_mc2HitchEnabled) return;
    g_mc2HitchAccum = HitchFrameAccum{};
}

void EndFrame(uint32_t frame, double dtMs) {
    if (!g_mc2HitchEnabled) return;
    if (dtMs < s_thresholdMs) return;

    const HitchFrameAccum& a = g_mc2HitchAccum;

    // Primary summary line.
    fprintf(stderr,
        "[HITCH] frame=%u dt=%.2f"
        " terrainSolid=%.2f staticFlush=%.2f staticSnapshot=%.2f water=%.2f"
        " glBufData=%u glBufBytes=%llu"
        " glBufSubData=%u glBufSubBytes=%llu"
        " glMapBuf=%u glTexImage=%u glTexImageBytes=%llu"
        " glBindTex=%u glUseProgram=%u"
        " glWaitSync=%u(sig=%u to=%u)"
        " glFence=%u glFinish=%u glFlush=%u\n",
        frame, dtMs,
        a.terrainSolidMs, a.staticFlushMs, a.staticSnapshotMs, a.waterMs,
        a.glBufferDataCalls,    (unsigned long long)a.glBufferDataBytes,
        a.glBufferSubDataCalls, (unsigned long long)a.glBufferSubDataBytes,
        a.glMapBufferCalls,
        a.glTexImageCalls, (unsigned long long)a.glTexImageBytes,
        a.glBindTextureCalls, a.glUseProgramCalls,
        a.glClientWaitSyncCalls, a.glClientWaitSyncSignaled, a.glClientWaitSyncTimedOut,
        a.glFenceSyncCalls, a.glFinishCalls, a.glFlushCalls);

    // Detail line: terrain solid dispatch.
    if (a.terrainSolidMs > 0.0 || a.terrainBinds || a.terrainUploads) {
        fprintf(stderr,
            "[HITCH_TERRAIN_TEX] quadSetupMs=%.2f binds=%u uploads=%u texImageCalls=%u\n",
            a.terrainSolidMs, a.terrainBinds, a.terrainUploads, a.terrainTexImageCalls);
    }

    // Detail line: static-prop flush.
    if (a.staticFlushMs > 0.0 || a.staticRecords) {
        fprintf(stderr,
            "[HITCH_STATIC_FLUSH] flushMs=%.2f records=%u draws=%u"
            " bufferBytes=%llu realloc=%u map=%u subData=%u\n",
            a.staticFlushMs, a.staticRecords, a.staticDrawCmds,
            (unsigned long long)a.staticBufferBytes,
            a.staticRealloc, a.staticMap, a.glBufferSubDataCalls);
    }

    // Detail line: water.
    if (a.waterCalled) {
        fprintf(stderr,
            "[HITCH_WATER] called=%u waterPresent=%u earlyOut=%u glCalls=%u\n",
            (unsigned)a.waterCalled, (unsigned)a.waterPresent,
            (unsigned)a.waterEarlyOut, a.waterGlCalls);
    }

    // Detail line: GL totals.
    fprintf(stderr,
        "[HITCH_GL]"
        " glBufferData=%u glBufferDataBytes=%llu"
        " glBufferSubData=%u glBufferSubDataBytes=%llu"
        " glMapBuffer=%u"
        " glTexImage=%u glTexImageBytes=%llu"
        " glTexSubImage=%u glTexSubImageBytes=%llu"
        " glBindTexture=%u glUseProgram=%u"
        " glClientWaitSync=%u(sig=%u to=%u)"
        " glFenceSync=%u glFinish=%u glFlush=%u\n",
        a.glBufferDataCalls,    (unsigned long long)a.glBufferDataBytes,
        a.glBufferSubDataCalls, (unsigned long long)a.glBufferSubDataBytes,
        a.glMapBufferCalls,
        a.glTexImageCalls, (unsigned long long)a.glTexImageBytes,
        a.glTexSubImageCalls, (unsigned long long)a.glTexSubImageBytes,
        a.glBindTextureCalls, a.glUseProgramCalls,
        a.glClientWaitSyncCalls, a.glClientWaitSyncSignaled, a.glClientWaitSyncTimedOut,
        a.glFenceSyncCalls, a.glFinishCalls, a.glFlushCalls);

    fflush(stderr);
}

void SpanBegin(HitchSpanKind kind) {
    if (!g_mc2HitchEnabled) return;
    s_spanStart[static_cast<size_t>(kind)] = Clock::now();
}

void SpanEnd(HitchSpanKind kind) {
    if (!g_mc2HitchEnabled) return;
    using namespace std::chrono;
    const auto dt = duration<double, std::milli>(
        Clock::now() - s_spanStart[static_cast<size_t>(kind)]).count();
    switch (kind) {
        case HitchSpanKind::TerrainSolidDispatch:   g_mc2HitchAccum.terrainSolidMs   += dt; break;
        case HitchSpanKind::GpuStaticPropsFlush:    g_mc2HitchAccum.staticFlushMs    += dt; break;
        case HitchSpanKind::GpuStaticPropsSnapshot: g_mc2HitchAccum.staticSnapshotMs += dt; break;
        case HitchSpanKind::WaterFastPath:          g_mc2HitchAccum.waterMs          += dt; break;
        default: break;
    }
}

} // namespace mc2_hitch
