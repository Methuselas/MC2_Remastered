// shaders/mech.vert — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".
//
// Bone transform convention: GpuMechBone stores Stuff LinearMatrix4D rows.
// mat4(row0..row3) fills GLSL COLUMNS from those args => boneT is the
// transpose of the original Stuff matrix. boneT * v == row-vec v * M_original.
// Do NOT "fix" the transpose — it is intentional and load-bearing.

// Slice B1: define MC2_STATIC_PROP_LIGHTING before lighting.hglsl so
// calc_light's no-lights early-return preserves base_light (our ambient
// floor) instead of returning vec3(1). The macro name is unfortunate
// (we're not a static prop) but behaviorally correct — the BGR/RGB
// swizzle path it gates inside get_base_light() is irrelevant because
// mech.vert never calls get_base_light(). See lighting.hglsl:131,196.
#define MC2_STATIC_PROP_LIGHTING

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
// Slice B1: 0 = Slice A passthrough (baseLight=vec3(1.0)),
// 1 = VS-side calc_light per-vertex. Driven by MC2_GPU_MECH_LIGHTING.
// 'uniform uint' crashes the engine's shader compile (see
// memory/uniform_uint_crash.md); use int.
uniform int u_lightingMode;

// Varyings — FS does NOT read the SSBO; all per-instance data forwarded here.
out vec2 v_uv;
out vec4 v_litColor;
out vec4 v_highlightColor;
// Slice B2: vec4 — .rgb = fog color (engine global from g_scene.fogColor),
// .a = per-actor haze factor (0=clear, 1=fully fogged) sourced from
// inst.fogRGB.a (mech3d.cpp packs Mech3DAppearance::hazeFactor into the
// alpha byte of desc.fogARGB).
out vec4 v_fogRGB;
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

    // Slice B1: per-vertex GPU lighting via calc_light from lighting.hglsl.
    // u_lightingMode=0 keeps Slice A's flat-white passthrough (used as a
    // fast bisect lever during soak); =1 enables calc_light. The
    // LightsData UBO at LIGHT_DATA_ATTACHMENT_SLOT=0 is bound once at
    // session start by MC_TextureManager (mclib/txmmgr.cpp:318); no
    // per-frame rebind needed. inst.lightDataIndex selects this actor's
    // ObjectLights entry, populated per-actor by
    // mechShape->CacheGpuLightData() in Mech3DAppearance::update().
    //
    // Ambient floor 0.35 prevents shadowed mechs from going pure black
    // (CPU mech path has implicit ambient via its lighting model).
    // Tunable post-soak.
    const float kAmbientFloor = 0.35;
    // Slice B+ (2026-05-09): per-actor lightsOut from inst.renderFlags
    // bit 1. Set by mech3d.cpp from OBJECT_STATUS_DESTROYED / DISABLED
    // / SHUTDOWN. Skip per-light contributions and use ambient floor
    // only — visually similar to the CPU path's
    // mechShape->SetLightsOut(true) effect.
    const uint kRenderFlagLightsOut = 0x2u;
    bool lightsOut = (inst.renderFlags & kRenderFlagLightsOut) != 0u;

    vec3 baseLight;
    if (u_lightingMode != 0) {
        if (lightsOut) {
            baseLight = vec3(kAmbientFloor);
        } else {
            vec3 base = vec3(kAmbientFloor);
            vec3 litRGB = calc_light(int(inst.lightDataIndex),
                                     worldNormal,
                                     worldMC2,
                                     base);
            baseLight = clamp(litRGB + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a,
                              0.0, 1.0);
        }
    } else {
        // Slice A passthrough: flat white + highlight (lightsOut still
        // dims via the ambient floor for parity with the CPU path).
        if (lightsOut) {
            baseLight = vec3(kAmbientFloor);
        } else {
            baseLight = clamp(vec3(1.0) + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a,
                              0.0, 1.0);
        }
    }

    v_uv             = a_uv;
    v_litColor       = vec4(baseLight, 1.0);
    v_highlightColor = inst.aRGBHighlight;
    // Slice B2: combine engine-global fog COLOR with per-actor haze
    // FACTOR. g_scene is from scene.hglsl (included transitively via
    // lighting.hglsl). inst.fogRGB.a carries hazeFactor packed by
    // mech3d.cpp; .rgb of inst.fogRGB is unused for mechs.
    v_fogRGB         = vec4(g_scene.fogColor.rgb, inst.fogRGB.a);
    v_normal         = worldNormal;
}
