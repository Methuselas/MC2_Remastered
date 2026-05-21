//#version 430 (provided by makeProgram prefix)
//
// GPU particle billboard expansion VS. Per plan v5 §5.4 B1 Stage 1' Commit 3.
//
// Draw call: glDrawArrays(GL_TRIANGLES, 0, 6 * particleCount) — issued by the
// GameOS bridge. No index buffer. Each particle expands to two triangles
// (six vertices); the per-vertex corner index is gl_VertexID % 6.
//
// Per AMD discipline (memory: gpu_direct_renderer_bringup_checklist.md
// trap #10 + amd-driver-rules.md:6):
//   - AMD attribute-0 rule: we reference gl_VertexID directly; no
//     layout(location=0) attribute needed because the engine binds a
//     dummy/empty VAO and the GL spec says gl_VertexID is always defined
//     even when no attribute is enabled.
//   - textureLod (not texture) in the FS — partner shader uses textureLod
//     to dodge AMD auto-LOD black-on-incomplete-mips strict-fail.
//
// In-front discipline (memory: clip_w_sign_trap.md): MC2's worldToClip
// (terrainMVP) produces well-formed finite clip values for behind-camera
// vertices that have the same sign-of-w as visible verts. We must not use
// sign(clip.w) as an in-front test. The pz computation below mirrors the
// projectZ() pattern used by gos_terrain_thin.vert / gos_terrain_water_fast.vert:
//   pz = clip.z / abs(clip.w); valid iff pz in [0, 1) under reverse-Z.
// When pz is out of range we emit a degenerate gl_Position to cull the
// particle entirely.

#include <include/particles.hglsl>

layout(std430, binding = 14) readonly buffer Particles {
    Particle particles[];
};

uniform mat4 terrainMVP;  // GL_FALSE upload (row-major direct) per
                          // memory/terrain_mvp_gl_false.md.

// Atlas UVs for the four billboard corners.
// Stage 1' Card test effect uses the full atlas page (0..1).
const vec2 kCornerUv[4] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

// Two triangles per quad, six vertices, with corner index per VS invocation.
// Order: tri0 = (BL, BR, TL), tri1 = (TL, BR, TR).
const int kCornerIdx[6] = int[](0, 1, 2, 2, 1, 3);

out vec2 v_uv;
out vec4 v_color;

void main() {
    uint particleId = uint(gl_VertexID) / 6u;
    int  cornerSlot = int(uint(gl_VertexID) % 6u);
    int  cornerIdx  = kCornerIdx[cornerSlot];

    Particle p = particles[particleId];

    // Stage 1' billboard: world-axis-aligned quad in the XY plane around the
    // particle position. A true view-aligned billboard requires the camera
    // basis uniform; Stage 2' adds that. For the Card test effect the
    // distinction is invisible (single quad at one position).
    vec2 cornerXY = (kCornerUv[cornerIdx] - vec2(0.5, 0.5)) * (2.0 * p.size);
    vec3 worldPos = p.position.xyz + vec3(cornerXY, 0.0);

    vec4 clip = terrainMVP * vec4(worldPos, 1.0);

    // In-front cull per clip_w_sign_trap.md. abs() to defang MC2's matrix
    // sign behavior; pz in [0,1) under reverse-Z is the visibility window.
    float absW = abs(clip.w);
    float pz   = (absW > 0.0) ? (clip.z / absW) : -1.0;
    bool  inFront = (pz >= 0.0 && pz < 1.0);

    if (!inFront) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);  // degenerate (behind near plane)
    } else {
        gl_Position = clip;
    }

    v_uv    = kCornerUv[cornerIdx];
    v_color = p.color;
}
