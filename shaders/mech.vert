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
// terrainMVP is the CPU-composed axisSwap * worldToClip matrix (row-major,
// upload GL_FALSE). Same uniform name + upload convention as
// static_prop.vert / terrain_overlay.vert, sourced from
// gos_GetTerrainMVPMat4(). Using raw TG_Shape::s_worldToClip would skip
// the axis swap and place mech vertices off-screen.
uniform mat4 terrainMVP;
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
    // boneT * vec4(pos,1) yields the world position in the Stuff/MLR camera
    // frame (.x=left, .y=elev, .z=forward).
    GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
    mat4 boneT = mat4(b.row0, b.row1, b.row2, b.row3);
    vec4 worldStuff = boneT * vec4(a_position, 1.0);

    // MC2/Stuff axis swap: terrainMVP is composed in MC2 world coords
    // (x=east, y=north, z=elev). Without this swap world.y (elev) gets
    // read as MC2 north and world.z (forward) as MC2 elev — mech ends up
    // in the sky / off-screen. Same swap as static_prop.vert (after
    // 2026-05-04 trees-in-the-sky fix).
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    vec3 normalStuff = mat3(boneT) * a_normal;
    vec3 worldNormal = normalize(vec3(-normalStuff.x, normalStuff.z, normalStuff.y));

    // D3D pixel-homogeneous projection chain (identical to static_prop.vert).
    vec4 clip4 = terrainMVP * vec4(worldMC2, 1.0);
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
    px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
    px.z = clip4.z * rhw;
    vec4 ndc   = u_mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);
    // No "clip4.w < 0.1 -> push offscreen" clause: MC2 clip.w sign is NOT
    // front/back per memory/clip_w_sign_trap.md; never sign-test clip.w.
    // static_prop.vert correctly omits the guard. The plan template
    // included it and made all mech vertices end up at (2,2,2,1).

    // Slice A: mech vertices have aRGBLight = 0 (no pre-baked static
    // lighting — the legacy CPU mech path computes lighting per-frame via
    // calc_light from listOfShapes[i].worldLights). Decoding a_aRGBLight
    // here gives 0 for all mech vertices, and the unselected actor's
    // highlightColor is also 0, so v_litColor would be (0,0,0) and the
    // mech would render fully black even when textures sample correctly.
    //
    // Default-on baseLight to white in Slice A so the texture passes
    // through unmodified. Slice B1 replaces this with the real
    // calc_light(inst.lightDataIndex) once the LightsData UBO is wired
    // for mech actors.
    vec3 baseLight = vec3(1.0);
    // Highlight is still added (selected mechs glow). Highlight alpha=0 by
    // default makes this a no-op for unselected actors.
    baseLight = clamp(baseLight + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a, 0.0, 1.0);
    // Reference of dead-code aRGBLight decode for Slice B1: it would unpack
    // the BGRA-packed uint into per-channel light, e.g.
    //     baseLight.x = float((a_aRGBLight >> 16) & 0xFFu) / 255.0;
    //     baseLight.y = float((a_aRGBLight >>  8) & 0xFFu) / 255.0;
    //     baseLight.z = float((a_aRGBLight >>  0) & 0xFFu) / 255.0;
    // but mech aRGBLight is 0 in stock data, so this is replaced by
    // calc_light(inst.lightDataIndex) when Slice B1 wires the UBO.

    v_uv             = a_uv;
    v_litColor       = vec4(baseLight, 1.0);
    v_highlightColor = inst.aRGBHighlight;
    v_fogRGB         = inst.fogRGB.rgb;
    v_normal         = worldNormal;
}
