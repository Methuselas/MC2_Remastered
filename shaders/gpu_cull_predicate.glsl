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

// Sphere-aware variant: admit when ANY part of a world-space sphere of the
// given radius reaches inside the frustum. Required for static props whose
// centroid is offset from their visible silhouette (large buildings — a
// 100-unit footprint rejects on its centroid even when 80% is on screen).
//
// Approximation: scale the world radius by a per-vertex factor that converts
// world distance to clip-space half-extent. For perspective projection,
// `radius * (cw / nearPlaneDist)` would be exact; we use `radius` directly as
// a conservative tolerance in NDC-aligned clip units, which over-admits at
// far distances and under-admits at very close range. Both are acceptable —
// over-admit at edge wastes a tiny bit of fragment work; under-admit at
// near-camera is masked because near-camera buildings already pass the
// strict test (centroid in frustum).
//
// The world-radius-to-clip-tolerance approximation is the same shape as the
// terrain TES uses for sphere-vs-frustum culling (terrain_tes_projection.md).
bool clipSpaceFrustumAdmitSphere(vec4 clip, float worldRadius) {
    float s = (clip.w < 0.0) ? -1.0 : 1.0;
    float cx = clip.x * s;
    float cy = clip.y * s;
    float cz = clip.z * s;
    float cw = clip.w * s;
    if (cw < 1e-5) {
        // Centroid behind/on camera plane. Conservative: admit if the prop
        // has any extent (radius > 0). Cheap, prevents losing a building
        // whose centroid is just behind the near plane while its front
        // half is visible.
        return worldRadius > 0.0;
    }
    // Tolerance: world radius interpreted as a clip-space half-extent.
    // For MC2's perspective projection (~1 unit near, ~12000 unit far,
    // ~60° vertical FOV), this gives a tolerance proportional to apparent
    // on-screen size for centroids in the typical zoom range.
    float tol = worldRadius;
    if (cx < -cw - tol || cx > cw + tol) return false;
    if (cy < -cw - tol || cy > cw + tol) return false;
    if (cz < -tol      || cz > cw + tol) return false;
    return true;
}

// Dilated variant: expand each clip-plane bound by (1 + d) on the half-extent.
// d=0 reproduces the strict test exactly; d=0.08 admits actors within ~8% of
// the frustum edge ("about to enter"). Hides readback staleness during camera
// motion. See gpu_cull_compute.cpp for CPU-side env wiring.
bool clipSpaceFrustumAdmitDilated(vec4 clip, float d) {
    float s = (clip.w < 0.0) ? -1.0 : 1.0;
    float cx = clip.x * s;
    float cy = clip.y * s;
    float cz = clip.z * s;
    float cw = clip.w * s;
    if (cw < 1e-5) return false;
    float bound = cw * (1.0 + d);
    if (cx < -bound || cx > bound) return false;
    if (cy < -bound || cy > bound) return false;
    // Depth: extend on both ends. Near plane: cz >= -d*cw (small negative slack).
    // Far plane: cz <= (1+d)*cw.
    if (cz < -d * cw || cz > bound) return false;
    return true;
}
