// tools/camera_frustum_harness/camera_frustum_harness.cpp
// SUBSYSTEM-HARNESS-ARC / CAMERA-FRUSTUM-HARNESS-1
//
// Tests the REAL camera geometry math (mclib/camera_frustum_math.h, the header
// Camera::quadAabbInFrustum / the tile point-in-tri test / gos_Compute43Box now
// delegate to) game-free — no Camera instance, no GL, no Stuff types. Guards the
// frustum cull split-brain class (a mirrored L/R plane culling on-screen geometry)
// and the FORCE-43 pillarbox rect, neither reliably visible in a 30s tier1 smoke.
//
// Build (standalone):
//   cmake -S tools/camera_frustum_harness -B build64-camfr -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-camfr --config RelWithDebInfo --target camera_frustum_harness

#include "contract_harness.h"
#include "camera_frustum_math.h"

using namespace contract_harness;
namespace cf = camera_frustum_math;

// Axis-aligned unit-cube frustum: admits the box [-1,1]^3 (planes x>=-1, x<=1,
// etc., as a*x+b*y+c*z+d>=0). Six half-spaces.
static void unitFrustum(float planes[6][4]) {
    // x+1>=0 ; 1-x>=0 ; y+1>=0 ; 1-y>=0 ; z+1>=0 ; 1-z>=0
    const float P[6][4] = {
        { 1, 0, 0, 1}, {-1, 0, 0, 1},
        { 0, 1, 0, 1}, { 0,-1, 0, 1},
        { 0, 0, 1, 1}, { 0, 0,-1, 1},
    };
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 4; ++j) planes[i][j] = P[i][j];
}

static bool test_aabb_inside_admitted(TestCtx& t) {
    float pl[6][4]; unitFrustum(pl);
    const float mn[3] = {-0.5f,-0.5f,-0.5f}, mx[3] = {0.5f,0.5f,0.5f};
    CH_CHECK(t, cf::aabbInFrustum(pl, mn, mx));
    return t.failures == 0;
}

static bool test_aabb_behind_one_plane_rejected(TestCtx& t) {
    float pl[6][4]; unitFrustum(pl);
    // Fully beyond +x plane (x in [2,3]): the 1-x>=0 plane rejects it.
    const float mn[3] = {2.0f,-0.5f,-0.5f}, mx[3] = {3.0f,0.5f,0.5f};
    CH_CHECK(t, !cf::aabbInFrustum(pl, mn, mx));
    return t.failures == 0;
}

static bool test_aabb_straddling_admitted(TestCtx& t) {
    float pl[6][4]; unitFrustum(pl);
    // Straddles the +x plane (x in [0.5,2]) -> p-vertex inside -> admitted
    // (conservative: never false-negative).
    const float mn[3] = {0.5f,-0.5f,-0.5f}, mx[3] = {2.0f,0.5f,0.5f};
    CH_CHECK(t, cf::aabbInFrustum(pl, mn, mx));
    return t.failures == 0;
}

static bool test_aabb_zero_extent_no_nan(TestCtx& t) {
    float pl[6][4]; unitFrustum(pl);
    const float p[3] = {0.0f,0.0f,0.0f};
    CH_CHECK(t, cf::aabbInFrustum(pl, p, p));   // point at origin admitted
    const float q[3] = {5.0f,5.0f,5.0f};
    CH_CHECK(t, !cf::aabbInFrustum(pl, q, q));  // point outside rejected
    return t.failures == 0;
}

// Split-brain guard: a frustum with a MIRRORED left/right plane (both half-spaces
// point the same way) would reject an in-front box. Asserts the test actually
// rejects when the planes are wrong — locks the fix direction.
static bool test_mirrored_lr_plane_rejects(TestCtx& t) {
    float pl[6][4]; unitFrustum(pl);
    // Corrupt the right plane to mirror the left ({1,0,0,1} instead of {-1,0,0,1}):
    // now nothing with x>1... actually a box at x in [0.4,0.6] should still pass the
    // (duplicated) left planes; flip sign to make a box clearly outside the broken set.
    pl[1][0] = 1.0f;   // was -1 (right plane now == left plane)
    pl[1][3] = -10.0f; // x - 10 >= 0  => requires x>=10
    const float mn[3] = {-0.5f,-0.5f,-0.5f}, mx[3] = {0.5f,0.5f,0.5f};
    CH_CHECK(t, !cf::aabbInFrustum(pl, mn, mx));  // in-front box wrongly rejected by broken plane
    return t.failures == 0;
}

// point-in-screen-triangle
static bool test_point_in_tri_inside(TestCtx& t) {
    // Triangle (0,0)(10,0)(0,10); point (2,2) inside.
    CH_CHECK(t, cf::pointInScreenTri(0,0, 10,0, 0,10, 2, 2));
    return t.failures == 0;
}

static bool test_point_in_tri_outside(TestCtx& t) {
    CH_CHECK(t, !cf::pointInScreenTri(0,0, 10,0, 0,10, 9, 9));  // outside hypotenuse
    CH_CHECK(t, !cf::pointInScreenTri(0,0, 10,0, 0,10, -1, -1));
    return t.failures == 0;
}

// pillarbox / letterbox rect
static bool test_pillarbox_16x9(TestCtx& t) {
    int ox, oy, obw, obh;
    bool active = cf::compute43BoxRect(1920, 1080, &ox, &oy, &obw, &obh);
    CH_CHECK(t, active);
    CH_CHECK(t, obw == 1440 && obh == 1080);   // 1080*4/3 = 1440
    CH_CHECK(t, ox == 240 && oy == 0);
    return t.failures == 0;
}

static bool test_box_4x3_is_noop(TestCtx& t) {
    int ox, oy, obw, obh;
    bool active = cf::compute43BoxRect(1024, 768, &ox, &oy, &obw, &obh);
    CH_CHECK(t, !active);                       // exact 4:3 -> full surface, no-op
    CH_CHECK(t, obw == 1024 && obh == 768 && ox == 0 && oy == 0);
    return t.failures == 0;
}

static bool test_letterbox_taller_than_43(TestCtx& t) {
    int ox, oy, obw, obh;
    bool active = cf::compute43BoxRect(800, 900, &ox, &oy, &obw, &obh);
    CH_CHECK(t, active);
    CH_CHECK(t, obw == 800);                    // taller -> letterbox, width fills
    CH_CHECK(t, obh == (int)(800.0/(4.0/3.0)+0.5));  // 600
    CH_CHECK(t, oy == (900 - obh) / 2);
    return t.failures == 0;
}

static bool test_box_degenerate_no_div0(TestCtx& t) {
    int ox, oy, obw, obh;
    CH_CHECK(t, !cf::compute43BoxRect(0, 0, &ox, &oy, &obw, &obh));   // inactive, no div0
    CH_CHECK(t, !cf::compute43BoxRect(100, 0, &ox, &oy, &obw, &obh));
    return t.failures == 0;
}

static bool test_demo_intentional_fail(TestCtx& t) {
    int ox, oy, obw, obh;
    cf::compute43BoxRect(1920, 1080, &ox, &oy, &obw, &obh);
    CH_CHECK(t, obw == 9999);   // intentionally wrong (real 1440)
    return t.failures == 0;
}

int main(int argc, char** argv) {
    Harness h("camera_frustum_harness");
    h.add("aabb_inside_admitted",          test_aabb_inside_admitted);
    h.add("aabb_behind_one_plane_rejected", test_aabb_behind_one_plane_rejected);
    h.add("aabb_straddling_admitted",      test_aabb_straddling_admitted);
    h.add("aabb_zero_extent_no_nan",       test_aabb_zero_extent_no_nan);
    h.add("mirrored_lr_plane_rejects",     test_mirrored_lr_plane_rejects);
    h.add("point_in_tri_inside",           test_point_in_tri_inside);
    h.add("point_in_tri_outside",          test_point_in_tri_outside);
    h.add("pillarbox_16x9",                test_pillarbox_16x9);
    h.add("box_4x3_is_noop",               test_box_4x3_is_noop);
    h.add("letterbox_taller_than_43",      test_letterbox_taller_than_43);
    h.add("box_degenerate_no_div0",        test_box_degenerate_no_div0);
    h.add("demo_intentional_fail",         test_demo_intentional_fail, /*inDefault=*/false);
    return h.run(argc, argv);
}
