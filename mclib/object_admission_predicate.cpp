// mclib/object_admission_predicate.cpp
#include "object_admission_predicate.h"
#include "stuff/stuff.hpp"   // Stuff::Vector4D
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool             s_initialized = false;
ObjectAdmissionPredicateMode s_mode = ObjectAdmissionPredicateMode::Legacy;

const char* modeLabel(ObjectAdmissionPredicateMode m) {
    return (m == ObjectAdmissionPredicateMode::Modern) ? "modern" : "legacy";
}

} // namespace

void objectAdmissionPredicate_init() {
    if (s_initialized) return;
    const char* env = std::getenv("MC2_OBJECT_ADMISSION_PREDICATE");
    if (env && std::strcmp(env, "legacy") == 0) {
        s_mode = ObjectAdmissionPredicateMode::Legacy;
    } else {
        s_mode = ObjectAdmissionPredicateMode::Modern;  // default
    }
    s_initialized = true;
    std::printf("[INSTR v1] object_admission_mode=%s\n", modeLabel(s_mode));
    std::fflush(stdout);
}

ObjectAdmissionPredicateMode objectAdmissionPredicateMode() {
    // Lazy init — startup ordering is non-load-bearing.
    objectAdmissionPredicate_init();
    return s_mode;
}

bool clipSpaceFrustumAdmit(const Stuff::Vector4D& rawClip) {
    // w must be in front of camera. Behind-camera vertices have rawClip.w <= 0
    // and the canonical clip-space tests below become meaningless.
    if (rawClip.w <= 0.0f) return false;
    const float w = rawClip.w;
    if (rawClip.x < -w || rawClip.x > w) return false;
    if (rawClip.y < -w || rawClip.y > w) return false;
    // MC2 uses D3D-style [0, w] depth range (matches existing `rectNearFar`
    // candidate at projectz_trace.cpp:154: `rawClip.z >= 0 && rawClip.z <= rawClip.w`).
    if (rawClip.z < 0.0f || rawClip.z > w) return false;
    return true;
}

int objectAdmissionPredicate_selftest() {
    int fails = 0;

    auto runCase = [&](const char* name, const Stuff::Vector4D& rawClip, bool expected) {
        bool actual = clipSpaceFrustumAdmit(rawClip);
        const char* result = (actual == expected) ? "pass" : "fail";
        if (actual != expected) ++fails;
        std::printf("[OBJECT_ADMISSION v1] event=selftest_%s case=%s expected=%d actual=%d "
                    "rawClip=(%.3f,%.3f,%.3f,%.3f)\n",
                    result, name, expected ? 1 : 0, actual ? 1 : 0,
                    rawClip.x, rawClip.y, rawClip.z, rawClip.w);
    };

    // Center of frustum, w=1, point fully inside.
    Stuff::Vector4D clip;
    clip.x =  0.0f; clip.y =  0.0f; clip.z = 0.5f; clip.w = 1.0f;
    runCase("center_inside", clip, true);

    // Behind camera: w negative.
    clip.x =  0.0f; clip.y =  0.0f; clip.z = 0.5f; clip.w = -1.0f;
    runCase("behind_camera", clip, false);

    // Beyond left clip plane.
    clip.x = -1.5f; clip.y = 0.0f; clip.z = 0.5f; clip.w = 1.0f;
    runCase("left_outside", clip, false);

    // On left clip plane (x == -w): admitted by `<=` boundary semantics.
    clip.x = -1.0f; clip.y = 0.0f; clip.z = 0.5f; clip.w = 1.0f;
    runCase("left_edge_inclusive", clip, true);

    // Past far plane (z > w).
    clip.x =  0.0f; clip.y =  0.0f; clip.z = 1.5f; clip.w = 1.0f;
    runCase("past_far", clip, false);

    // Closer than near plane (z < 0).
    clip.x = 0.0f; clip.y = 0.0f; clip.z = -0.1f; clip.w = 1.0f;
    runCase("before_near", clip, false);

    // w == 0: pathological / degenerate. Reject (cannot represent a finite point).
    clip.x = 0.0f; clip.y = 0.0f; clip.z = 0.0f; clip.w = 0.0f;
    runCase("w_zero_degenerate", clip, false);

    // Far corner of frustum: x=w, y=w, z=w. All on inclusive boundaries.
    clip.x = 1.0f; clip.y = 1.0f; clip.z = 1.0f; clip.w = 1.0f;
    runCase("far_corner_inclusive", clip, true);

    return fails;
}
