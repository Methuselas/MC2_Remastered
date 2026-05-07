// gpu_cull_predicate.glsl
//
// Lockstep port of mclib/object_admission_predicate.cpp clipSpaceFrustumAdmit().
// D3D-style [0, w] depth range (MC2 convention — matches clipSpaceFrustumAdmit in C++).
//
// LOCKSTEP RULE: Any change to this function must be made to the C++ version
// (mclib/object_admission_predicate.cpp::clipSpaceFrustumAdmit) in the same commit.
//
// The depth range uses [0, w] (D3D convention) because MC2's projection matrix
// is built for that range. The OpenGL default is [-w, w] but MC2 uses
// glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) to match D3D clip conventions.

bool clipSpaceFrustumAdmit(vec4 clip) {
    if (clip.w <= 0.0) return false;
    float w = clip.w;
    if (clip.x < -w || clip.x > w) return false;
    if (clip.y < -w || clip.y > w) return false;
    // D3D-style [0, w] depth range (MC2 convention).
    if (clip.z < 0.0 || clip.z > w) return false;
    return true;
}
