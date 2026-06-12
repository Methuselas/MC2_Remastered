//============================================================================
// gos_frame_pass_stats — see gos_frame_pass_stats.h.
//============================================================================
#include <GL/glew.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gos_frame_pass_stats.h"
#include "gos_render_pass_timer.h"

namespace gos_frame_pass_stats {

using gos_render_pass_timer::Pass_Count;

namespace {

// Stable emit keys, index == gos_render_pass_timer::Pass. Kept identical to the
// timer's kPassKey so both telemetry streams line up. Never reorder.
const char* const kPassKey[Pass_Count] = {
    "shadowStatic",
    "shadowDyn",
    "obj3d",
    "terrainChunk",
    "terrainSolid",
    "spColor",
    "mechs",
    "overlays",
    "water",
    "blobs",
    "alphaVfx",
    "hud",
    "post",
};

PassRow         s_rows[Pass_Count];   // current frame (filling)
PassRow         s_last[Pass_Count];   // last completed frame (for consumers)
FrameAggregates s_agg;                // current frame (filling)
FrameAggregates s_lastAgg;            // last completed frame

bool s_inited = false;
bool s_on     = false;

int emitEvery()
{
    static int s_every = 0;
    if (s_every == 0) {
        const char* v = std::getenv("MC2_FRAME_PASS_STATS_EVERY");
        const int n = (v && v[0]) ? std::atoi(v) : 0;
        s_every = (n > 0) ? n : 60;
    }
    return s_every;
}

void emitLine(unsigned long frameNo)
{
    char line[2048];
    int off = 0;
    off += std::snprintf(line + off, sizeof(line) - off,
        "[FRAME_PASS_STATS v1] frame=%lu chunks=%u spBatches=%u mechInst=%u vfx=%u",
        frameNo, s_lastAgg.visibleTerrainChunks, s_lastAgg.staticPropBatches,
        s_lastAgg.mechBatchInstances, s_lastAgg.vfxCount);
    for (int p = 0; p < Pass_Count; ++p) {
        const PassRow& r = s_last[p];
        if (!r.ran) continue;   // pass never ran this frame: key absent
        off += std::snprintf(line + off, sizeof(line) - off,
            " %s{fbo=%u vp=%dx%d dt=%d dm=%d bl=%d cull=%d draws=%u inst=%u}",
            kPassKey[p], r.fbo, r.viewport[2], r.viewport[3],
            r.depthTest ? 1 : 0, r.depthMask ? 1 : 0, r.blend ? 1 : 0,
            r.cull ? 1 : 0, r.drawCount, r.instanceCount);
        if (off >= (int)sizeof(line) - 96) break;   // guard truncation
    }
    std::printf("%s\n", line);
    std::fflush(stdout);
}

} // namespace

bool Enabled()
{
    if (!s_inited) {
        const char* v = std::getenv("MC2_FRAME_PASS_STATS");
        s_on = (v && v[0] == '1' && v[1] == '\0');
        s_inited = true;
        if (s_on) {
            std::printf("[FRAME_PASS_STATS v1] armed every=%d\n", emitEvery());
            std::fflush(stdout);
        }
    }
    return s_on;
}

void RecordPassBegin(gos_render_pass_timer::Pass p)
{
    if (!Enabled()) return;
    const int pi = (int)p;
    if (pi < 0 || pi >= Pass_Count) return;
    PassRow& r = s_rows[pi];

    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, r.viewport);
    GLboolean depthMask = GL_FALSE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);

    r.ran           = true;
    r.fbo           = (uint32_t)fbo;
    r.depthTest     = glIsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    r.depthMask     = depthMask != GL_FALSE;
    r.blend         = glIsEnabled(GL_BLEND) != GL_FALSE;
    r.cull          = glIsEnabled(GL_CULL_FACE) != GL_FALSE;
    // drawCount / instanceCount are filled separately via SetPassCounts.
}

void SetFrameAggregates(const FrameAggregates& a)
{
    if (!Enabled()) return;
    // Preserve the chunk count set by the terrain flush producer (the seam
    // does not know it); the seam fills the other three from accessors.
    const uint32_t chunks = s_agg.visibleTerrainChunks;
    s_agg = a;
    if (a.visibleTerrainChunks == 0u)
        s_agg.visibleTerrainChunks = chunks;
}

void SetVisibleTerrainChunks(uint32_t chunks)
{
    if (!Enabled()) return;
    s_agg.visibleTerrainChunks = chunks;
}

void SetPassCounts(gos_render_pass_timer::Pass p,
                   uint32_t drawCount, uint32_t instanceCount)
{
    if (!Enabled()) return;
    const int pi = (int)p;
    if (pi < 0 || pi >= Pass_Count) return;
    s_rows[pi].drawCount     = drawCount;
    s_rows[pi].instanceCount = instanceCount;
}

void FrameEnd(unsigned long frameNo)
{
    if (!Enabled()) return;

    // Promote this frame's rows to the "last completed" snapshot consumers read.
    std::memcpy(s_last, s_rows, sizeof(s_last));
    s_lastAgg = s_agg;

    if (emitEvery() > 0 && frameNo % (unsigned long)emitEvery() == 0)
        emitLine(frameNo);

    // Reset for the next frame.
    for (int p = 0; p < Pass_Count; ++p)
        s_rows[p] = PassRow{};
    s_agg = FrameAggregates{};
}

const PassRow& GetPassRow(int passIndex)
{
    static const PassRow s_empty{};
    if (passIndex < 0 || passIndex >= Pass_Count) return s_empty;
    return s_last[passIndex];
}

const FrameAggregates& GetFrameAggregates() { return s_lastAgg; }

int PassCount() { return Pass_Count; }

const char* PassKey(int passIndex)
{
    if (passIndex < 0 || passIndex >= Pass_Count) return "";
    return kPassKey[passIndex];
}

} // namespace gos_frame_pass_stats
