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
// B1 C14: terrainMVP is D3D pixel-homog clip-space (per
// gos_terrain_surface.vert documentation), NOT GL clip. To get to GL
// clip we must mirror the 3-step chain used by static_prop.vert:148-156
// and mech.vert: (1) terrainMVP -> pixel-homog clip4, (2) viewport
// divide via u_terrainViewport, (3) u_mvp NDC remap with absW restore.
// C13's inspection identified the prior direct gl_Position = clip4
// assignment as the ~90%-confidence invisibility root cause.
uniform vec4 u_terrainViewport;
uniform mat4 u_mvp;

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

    // B1 C14: 3-step chain mirroring static_prop.vert:148-156.
    // Step 1: world -> pixel-homog clip via terrainMVP (D3D convention).
    vec4 clip4 = terrainMVP * vec4(worldPos, 1.0);
    float rhw  = 1.0 / clip4.w;
    // Step 2: viewport divide (pixel space).
    vec3 px;
    px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
    px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
    px.z = clip4.z * rhw;
    // Step 3: pixel -> GL NDC via u_mvp, then absW restore so behind-camera
    // vertices get correct GL clip-space w (preserves clip_w_sign_trap.md
    // discipline without bespoke pz cull — GL clipper handles it natively).
    vec4 ndc  = u_mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

    v_uv    = kCornerUv[cornerIdx];
    v_color = p.color;
}
