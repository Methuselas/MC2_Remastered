//============================================================================
// gos_render_pass_timer — see gos_render_pass_timer.h.
//============================================================================
#include <GL/glew.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gos_render_pass_timer.h"

namespace gos_render_pass_timer {

namespace {

// Stable emit keys, index == Pass enum. Never reorder (smoke scripts regex them).
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

const int kRing = 4;   // frames in flight; oldest is polled, never waited on

struct Slot {
    GLuint   q[Pass_Count];     // GL_TIME_ELAPSED query per pass (lazy-gen)
    GLuint   tsBegin;           // GL_TIMESTAMP at first pass begin of the frame
    GLuint   tsEnd;             // GL_TIMESTAMP at FrameEnd
    unsigned ranMask;           // bit i set = pass i has a committed scope
    bool     frameOpen;         // tsBegin issued this frame
    bool     pending;           // submitted, awaiting harvest
};

Slot     s_ring[kRing];
int      s_cur = 0;
bool     s_inited = false;
bool     s_on = false;

int      s_openPass = -1;       // pass whose scope is currently open (-1 none)
bool     s_openReal = false;    // glBeginQuery actually issued for it

// Window accumulators (reset at each emit).
double   s_sumMs[Pass_Count] = {0};
unsigned s_samples[Pass_Count] = {0};
double   s_sumTotalMs = 0.0;
unsigned s_frames = 0;          // harvested frames in window
unsigned s_dropped = 0;
unsigned long s_frameNo = 0;    // monotonic FrameEnd count

int emitEvery()
{
    static int s_every = 0;
    if (s_every == 0) {
        const char* v = std::getenv("MC2_RENDER_PASS_TIME_EVERY");
        const int n = (v && v[0]) ? std::atoi(v) : 0;
        s_every = (n > 0) ? n : 60;
    }
    return s_every;
}

void harvest(Slot& s)
{
    for (int p = 0; p < Pass_Count; ++p) {
        if (!(s.ranMask & (1u << p))) continue;
        GLuint64 ns = 0;
        glGetQueryObjectui64v(s.q[p], GL_QUERY_RESULT, &ns);
        s_sumMs[p] += (double)ns / 1.0e6;
        ++s_samples[p];
    }
    if (s.frameOpen && s.tsBegin && s.tsEnd) {
        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(s.tsBegin, GL_QUERY_RESULT, &t0);
        glGetQueryObjectui64v(s.tsEnd, GL_QUERY_RESULT, &t1);
        if (t1 > t0) s_sumTotalMs += (double)(t1 - t0) / 1.0e6;
    }
    ++s_frames;
    s.pending = false;
    s.ranMask = 0;
    s.frameOpen = false;
}

void emitWindow()
{
    if (s_frames == 0) {
        // No slot harvested this window (GPU results never became available).
        // Still emit a heartbeat so silence is diagnosable, then reset.
        std::printf("[RENDER_PASS_TIME v1] frame=%lu n=0 dropped=%u\n",
            s_frameNo, s_dropped);
        std::fflush(stdout);
        s_dropped = 0;
        return;
    }
    char line[1024];
    int off = 0;
    off += std::snprintf(line + off, sizeof(line) - off,
        "[RENDER_PASS_TIME v1] frame=%lu n=%u gpu_total=%.2f",
        s_frameNo, s_frames, s_sumTotalMs / (double)s_frames);
    for (int p = 0; p < Pass_Count; ++p) {
        if (!s_samples[p]) continue;   // pass never ran in window: key absent
        off += std::snprintf(line + off, sizeof(line) - off, " %s=%.2f",
            kPassKey[p], s_sumMs[p] / (double)s_frames);
    }
    if (s_dropped)
        off += std::snprintf(line + off, sizeof(line) - off, " dropped=%u", s_dropped);
    std::printf("%s\n", line);
    std::fflush(stdout);

    std::memset(s_sumMs, 0, sizeof(s_sumMs));
    std::memset(s_samples, 0, sizeof(s_samples));
    s_sumTotalMs = 0.0;
    s_frames = 0;
    s_dropped = 0;
}

} // namespace

bool Enabled()
{
    if (!s_inited) {
        const char* v = std::getenv("MC2_RENDER_PASS_TIME");
        s_on = (v && v[0] == '1' && v[1] == '\0');
        s_inited = true;
        if (s_on) {
            std::printf("[RENDER_PASS_TIME v1] armed every=%d ring=%d\n",
                emitEvery(), kRing);
            std::fflush(stdout);
        }
    }
    return s_on;
}

bool QueryActive()
{
    return Enabled() && s_openPass >= 0 && s_openReal;
}

void Begin(Pass p)
{
    if (!Enabled()) return;
    if (p < 0 || p >= Pass_Count) return;
    if (s_openPass >= 0) {
        // Defensive: GL_TIME_ELAPSED cannot nest. Record the open pass so the
        // matching End() is ignored; do not disturb the outer scope.
        return;
    }
    Slot& s = s_ring[s_cur];
    if (s.pending) {
        // Ring lap caught an unharvested slot (GPU far behind): drop its
        // measurements and reuse it rather than stall.
        ++s_dropped;
        s.pending = false;
        s.ranMask = 0;
        s.frameOpen = false;
    }
    if (!s.frameOpen) {
        if (!s.tsBegin) glGenQueries(1, &s.tsBegin);
        if (!s.tsEnd)   glGenQueries(1, &s.tsEnd);
        glQueryCounter(s.tsBegin, GL_TIMESTAMP);
        s.frameOpen = true;
    }
    if (s.ranMask & (1u << p)) {
        // Pass already timed this frame (should not happen with the disjoint
        // placement); keep the first measurement, skip re-begin.
        s_openPass = p;
        s_openReal = false;
        return;
    }
    if (!s.q[p]) glGenQueries(1, &s.q[p]);
    glBeginQuery(GL_TIME_ELAPSED, s.q[p]);
    s_openPass = p;
    s_openReal = true;
}

void End(Pass p)
{
    if (!Enabled()) return;
    if (s_openPass != (int)p) return;   // unmatched / nested-skipped: ignore
    if (s_openReal) {
        glEndQuery(GL_TIME_ELAPSED);
        s_ring[s_cur].ranMask |= (1u << (int)p);
    }
    s_openPass = -1;
    s_openReal = false;
}

void FrameEnd()
{
    if (!Enabled()) return;
    if (s_openPass >= 0) {
        // Scope leaked across the frame boundary (should not happen): close it
        // so the next frame starts clean.
        if (s_openReal) {
            glEndQuery(GL_TIME_ELAPSED);
            s_ring[s_cur].ranMask |= (1u << s_openPass);
        }
        s_openPass = -1;
        s_openReal = false;
    }
    ++s_frameNo;

    Slot& cur = s_ring[s_cur];
    if (cur.frameOpen) {
        glQueryCounter(cur.tsEnd, GL_TIMESTAMP);
        cur.pending = true;
        s_cur = (s_cur + 1) % kRing;
    }
    // else: no pass ran this frame (menus etc.) — keep the slot, nothing pending.

    // Poll pending slots oldest-first; harvest every slot whose LAST query
    // (tsEnd, issued after all pass queries) is available. Never block.
    for (int i = 1; i <= kRing; ++i) {
        Slot& s = s_ring[(s_cur + i) % kRing];
        if (!s.pending) continue;
        GLint avail = 0;
        glGetQueryObjectiv(s.tsEnd, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;   // younger slots cannot be ready either
        harvest(s);
    }

    if (s_frameNo % (unsigned long)emitEvery() == 0)
        emitWindow();
}

} // namespace gos_render_pass_timer
