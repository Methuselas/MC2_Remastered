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
// In-front discipline post-F1 (kAxisSwapMC2toGL has the R-clipw polarity
// fold per addendum-rclipw-polarity.md, so u_worldToClipGL produces
// clip.w > 0 for in-front MC2 verts). Hardware clip-volume test handles
// rejection of behind-camera vertices; no explicit pz cull in this VS.
// CPU-side particle visibility is gated upstream by the gosFX/MLR system.

#include <include/particles.hglsl>

layout(std430, binding = 14) readonly buffer Particles {
    Particle particles[];
};

uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)

// Atlas UVs for the four billboard corners.
// Stage 1' Card test effect uses the full atlas page (0..1).
const vec2 kCornerUv[4] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

// Two triangles per quad, six vertices, with corner index per VS invocation.
// P0-2 winding fix: both triangles CCW under GL_CCW default.
// tri0 = (BL, BR, TL) = CCW; tri1 = (TL, TR, BR) -> reordered to (TL, TR, BR).
// Old: int[](0,1,2, 2,1,3) — tri1 was (TL,BR,TR) = CW, culled when GL_CULL_FACE on.
// New: int[](0,1,2, 2,3,1) — tri1 is (TL,TR,BR) = CCW.
const int kCornerIdx[6] = int[](0, 1, 2, 2, 3, 1);

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
    // Visibility floor: real gosFX spec halfHeight peaks are often a few world
    // units, which renders sub-pixel at typical mc2 mission camera distances.
    // 8.0 world units is roughly the gamecam canary scale and renders as a
    // visible sprite. Stage 2' polish: respect true spec sizes once the shader
    // does per-frame curve evaluation and the projection stays accurate at
    // smaller pixel coverage.
    float effSize = max(p.size, 8.0);
    vec2 cornerXY = (kCornerUv[cornerIdx] - vec2(0.5, 0.5)) * (2.0 * effSize);
    vec3 worldStuff = p.position.xyz + vec3(cornerXY, 0.0);

    // MC2 axis swap (load-bearing): gosFX SpawnCard emits world position in
    // Stuff::Point3D coords (stuff-space); terrainMVP is composed against the
    // MC2 terrain axis convention. Without this swap, world.y (elev) gets
    // read by terrainMVP as MC2 north and world.z (south distance) as MC2
    // elev — particles fly to wrong-axis positions (canonical "trees in the
    // sky" failure mode, see static_prop.vert:139-144 for the same fix in
    // the static-prop path). The canary at (0,0,50) is special-cased only
    // because two axes are zero — invisible nonetheless because either MC2
    // interpretation lands far from any tier1 mission's SPOT_DIAG range.
    vec3 worldPos = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);

    // F1 Stage A: direct GL clip emit.
    gl_Position = u_worldToClipGL * vec4(worldPos, 1.0);

    v_uv    = kCornerUv[cornerIdx];
    v_color = p.color;
}
