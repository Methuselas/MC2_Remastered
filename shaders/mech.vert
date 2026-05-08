// shaders/mech.vert — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".
//
// Bone transform convention: GpuMechBone stores Stuff LinearMatrix4D rows.
// mat4(row0..row3) fills GLSL COLUMNS from those args => boneT is the
// transpose of the original Stuff matrix. boneT * v == row-vec v * M_original.
// Do NOT "fix" the transpose — it is intentional and load-bearing.

#include <include/lighting.hglsl>

// Vertex attributes — 48-byte skinning-ready ABI, locked for Slice A+.
// Storage types: see GpuMechVertex in gos_mech_batcher.h.
layout(location=0) in vec3  a_position;
layout(location=1) in vec3  a_normal;
layout(location=2) in vec2  a_uv;
layout(location=3) in uvec4 a_boneIndices;   // GL_UNSIGNED_BYTE via IPointer; Slice A: .x only
layout(location=4) in vec4  a_boneWeights;   // GL_UNSIGNED_BYTE normalized; Slice A: (1,0,0,0)
layout(location=5) in vec2  a_tangentOct;    // GL_SHORT normalized; zero-fill for stock
layout(location=6) in uint  a_aRGBLight;     // TG_TypeVertex::aRGBLight, BGRA packed

// Per-instance SSBO (binding 0).
struct GpuMechInstance {
    uint  typeLodRecordIndex;
    uint  baseBoneOffset;
    uint  lightDataIndex;
    uint  renderFlags;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};
layout(std430, binding=0) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
};

// Per-frame bone SSBO (binding 1).
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(std430, binding=1) readonly buffer BoneBuffer {
    GpuMechBone bones[];
};

// 'uniform uint' crashes this engine's shader compiler — use int + cast.
uniform int  u_instanceBase;
uniform int  u_materialFlags;
uniform mat4 u_worldToClip;      // upload GL_TRUE (Stuff Matrix4D col-major -> GLSL transpose)
uniform vec4 u_terrainViewport;  // (vmx, vmy, vax, vay) for D3D->GL projection chain
uniform mat4 u_mvp;              // px->NDC (upload GL_TRUE)

// Varyings — FS does NOT read the SSBO; all per-instance data forwarded here.
out vec2 v_uv;
out vec4 v_litColor;
out vec4 v_highlightColor;
out vec3 v_fogRGB;
out vec3 v_normal;   // world-space for GBuffer1

void main() {
    uint instIdx = uint(u_instanceBase) + uint(gl_InstanceID);
    GpuMechInstance inst = instances[instIdx];

    // Bone transform: boneT is the transpose of the Stuff LinearMatrix4D.
    GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
    mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
    vec4 worldPos = boneT * vec4(a_position, 1.0);

    // World-space normal via 3x3 rotation block of boneT.
    vec3 worldNormal = normalize(mat3(boneT) * a_normal);

    // D3D pixel-homogeneous projection chain (identical to static_prop.vert).
    vec4 clip4 = u_worldToClip * worldPos;
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
    px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
    px.z = clip4.z * rhw;
    vec4 ndc   = u_mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

    if (clip4.w < 0.1) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }

    // Slice A: decode a_aRGBLight (BGRA packed uint) as base vertex color.
    // Slice B1: replace with calc_light() using inst.lightDataIndex.
    vec3 baseLight;
    baseLight.x = float((a_aRGBLight >> 16) & 0xFFu) / 255.0;  // r
    baseLight.y = float((a_aRGBLight >>  8) & 0xFFu) / 255.0;  // g
    baseLight.z = float((a_aRGBLight >>  0) & 0xFFu) / 255.0;  // b
    baseLight = clamp(baseLight + inst.aRGBHighlight.rgb, 0.0, 1.0);

    v_uv             = a_uv;
    v_litColor       = vec4(baseLight, 1.0);
    v_highlightColor = inst.aRGBHighlight;
    v_fogRGB         = inst.fogRGB.rgb;
    v_normal         = worldNormal;
}
