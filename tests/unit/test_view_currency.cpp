// VIEW-EPOCH-DEDUPE-1 / RENDER-VIEW-CURRENCY-1 offline verification.
//
// Proves the object/mech MVP currency math WITHOUT GL, deploy, or a 30s smoke. The
// engine (gos_SetWorldToClipGL dedupe, gos_GetObjectDrawMVP accessor) and these tests
// call the SAME pure kernel (RenderCore/view_currency.h), so the tests cannot drift
// from shipped behavior. The COUNTER-PROOF case encodes the exact "stale every frame"
// bug the live telemetry caught, so a regression to a raw publish counter fails here
// offline instead of shipping.
#include "doctest.h"
#include "RenderCore/view_currency.h"
#include <cstring>

using namespace mc2::view_currency;

namespace {
void mat(float* m, float fill) { for (int i = 0; i < 16; ++i) m[i] = fill; }

// Tiny replay of the engine's publish/dispatch/draw timeline, driving the SAME kernel
// functions the engine uses. publish() == gos_SetWorldToClipGL, dispatch() == the
// ComputeDispatch snapshot stamp, draw() == gos_GetObjectDrawMVP.
struct Timeline {
    static constexpr float kEps = 1e-6f;
    long  contentEpoch  = 0;
    float prev[16]      = {0};
    bool  havePrev      = false;
    long  snapshotEpoch = -1;
    bool  snapshotValid = false;
    bool  armed         = false;
    unsigned used = 0, stale = 0;

    void publish(const float* m) {
        if (viewContentChanged(prev, m, havePrev, kEps)) ++contentEpoch;
        std::memcpy(prev, m, sizeof(prev));
        havePrev = true;
    }
    void dispatch() { snapshotEpoch = contentEpoch; snapshotValid = true; armed = true; }
    void draw(bool fixB = true) {
        if (objectMvpUseSnapshot(fixB, armed, snapshotEpoch, contentEpoch, snapshotValid))
            ++used;
        else if (fixB && armed)
            ++stale;
    }
};
} // namespace

TEST_SUITE("ViewCurrency") {

TEST_CASE("viewContentChanged: dedupe predicate") {
    float a[16]; mat(a, 1.0f);
    float b[16]; mat(b, 1.0f);
    CHECK(viewContentChanged(a, b, false, 1e-6f) == true);   // first publish forces a bump
    CHECK(viewContentChanged(a, b, true,  1e-6f) == false);  // identical -> no bump
    b[5] += 1.0f;
    CHECK(viewContentChanged(a, b, true,  1e-6f) == true);   // real change -> bump
    float c[16]; mat(c, 1.0f); c[7] += 1e-9f;
    CHECK(viewContentChanged(a, c, true,  1e-6f) == false);  // sub-eps jitter ignored
}

TEST_CASE("objectMvpUseSnapshot: decision table") {
    CHECK(objectMvpUseSnapshot(false, true,  5, 5, true)  == false); // fixB off
    CHECK(objectMvpUseSnapshot(true,  false, 5, 5, true)  == false); // not armed
    CHECK(objectMvpUseSnapshot(true,  true,  5, 5, false) == false); // null snapshot
    CHECK(objectMvpUseSnapshot(true,  true,  4, 5, true)  == false); // epoch mismatch
    CHECK(objectMvpUseSnapshot(true,  true,  5, 5, true)  == true);  // current -> use
}

TEST_CASE("timeline: static camera + same-camera double-publish -> snapshot used every frame") {
    // VIEW-EPOCH-DEDUPE-1 regression guard. Each frame: early-publish(cam) -> dispatch
    // -> gamecam re-publish(SAME cam) -> draw. The redundant second publish must NOT
    // advance the content epoch, so the snapshot stays current -> used, zero stale.
    Timeline t;
    float cam[16]; mat(cam, 2.0f);
    const int N = 100;
    for (int f = 0; f < N; ++f) { t.publish(cam); t.dispatch(); t.publish(cam); t.draw(); }
    CHECK(t.used  == (unsigned)N);
    CHECK(t.stale == 0u);
}

TEST_CASE("timeline: COUNTER-PROOF -- a raw publish counter false-stales every frame") {
    // WHY the dedupe is required. If the epoch bumped on EVERY publish (the pre-dedupe
    // bug), the redundant same-camera second publish makes the snapshot stale -> object
    // draw drops to live -> FixB z-fight fix silently dead. This reproduces the exact
    // telemetry observed pre-dedupe (stale_mvp_reads ~= frame count).
    long pubEpoch = 0, snapEpoch = -1; unsigned used = 0, stale = 0;
    const int N = 100;
    for (int f = 0; f < N; ++f) {
        ++pubEpoch;             // early-publish bumps
        snapEpoch = pubEpoch;   // dispatch stamps
        ++pubEpoch;             // gamecam re-publish bumps AGAIN (same camera!)
        if (objectMvpUseSnapshot(true, true, snapEpoch, pubEpoch, true)) ++used; else ++stale;
    }
    CHECK(used  == 0u);
    CHECK(stale == (unsigned)N);
}

TEST_CASE("timeline: real camera motion across frames -> snapshot still used (re-dispatched)") {
    Timeline t;
    const int N = 50;
    for (int f = 0; f < N; ++f) {
        float cam[16]; mat(cam, 2.0f); cam[12] = (float)f;  // translate each frame
        t.publish(cam); t.dispatch(); t.publish(cam); t.draw();
    }
    CHECK(t.used  == (unsigned)N);  // snapshot re-stamped each frame -> current
    CHECK(t.stale == 0u);
}

TEST_CASE("timeline: armed-but-not-redispatched after camera change -> live fallback") {
    // Snapshot from an earlier view; camera moves; ComputeDispatch did NOT run this
    // frame (armed sticky). Object draw must reject the stale snapshot -> live MVP.
    Timeline t;
    float cam0[16]; mat(cam0, 2.0f);
    t.publish(cam0); t.dispatch(); t.publish(cam0); t.draw();
    CHECK(t.used == 1u); CHECK(t.stale == 0u);
    float cam1[16]; mat(cam1, 2.0f); cam1[12] = 99.0f;
    t.publish(cam1);   // content epoch advances; no dispatch this frame
    t.draw();          // snapshotEpoch (old) != contentEpoch -> stale -> live
    CHECK(t.stale == 1u);
}

TEST_CASE("timeline: fixB killswitch off -> never uses snapshot, never counts stale") {
    Timeline t;
    float cam[16]; mat(cam, 2.0f);
    for (int f = 0; f < 20; ++f) { t.publish(cam); t.dispatch(); t.draw(/*fixB*/false); }
    CHECK(t.used  == 0u);
    CHECK(t.stale == 0u);
}

} // TEST_SUITE
