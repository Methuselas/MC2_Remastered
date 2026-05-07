// gpu_cull_predicate.glsl
//
// Lockstep port of mclib/object_admission_predicate.cpp clipSpaceFrustumAdmit().
// D3D-style [0, w] depth range (MC2 convention — matches clipSpaceFrustumAdmit in C++).
//
// LOCKSTEP RULE: Any change to this function must be made to the C++ version
// (mclib/object_admission_predicate.cpp::clipSpaceFrustumAdmit) in the same commit.
//
// IMPORTANT — MC2 clip.w sign convention (from clip_w_sign_trap.md):
// MC2's Stuff worldToClip matrix produces clip.w of EITHER sign for visible vertices.
// The TES uses abs(clip.w) for this reason (see terrain_tes_projection.md).
// clip.w <= 0 does NOT mean "behind camera" — do not use sign(clip.w) as a front-test.
//
// We normalize the clip vector so w > 0 before applying the standard frustum test
// (the GPU's homogeneous clipper does this implicitly). Multiplying by sign(w) flips
// the inequalities consistently for all components.
//
// The depth range uses [0, w] (D3D convention) because MC2's projection matrix
// is built for that range. The OpenGL default is [-w, w] but MC2 uses
// glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) to match D3D clip conventions.

bool clipSpaceFrustumAdmit(vec4 clip) {
    // Normalize clip vector so w > 0 (GPU homogeneous clip convention).
    // If clip.w < 0, multiply all components by -1 to flip to w > 0.
    float s = (clip.w < 0.0) ? -1.0 : 1.0;
    float cx = clip.x * s;
    float cy = clip.y * s;
    float cz = clip.z * s;
    float cw = clip.w * s;   // always >= 0 after this
    // Degenerate point (clip.w == 0) cannot be projected.
    if (cw < 1e-5) return false;
    // Test x, y: NDC x/w ∈ [-1, 1].
    if (cx < -cw || cx > cw) return false;
    if (cy < -cw || cy > cw) return false;
    // D3D-style [0, w] depth range: NDC z/w ∈ [0, 1].
    if (cz < 0.0 || cz > cw) return false;
    return true;
}
