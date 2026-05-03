// shaders/static_prop.vert — slice 2 stage 2.C.2 GPU-lighting wired
//
// Stage 2.C.2 flips this from "draw with CPU-baked stale colors via the
// per-frame Colors SSBO" to "compute lit ARGB per vertex via lighting.hglsl
// calc_light()". The Colors SSBO is still read for legacy debug modes
// (RAlt+9 mode 4) and to keep the slice 1 substrate intact, but the main
// path no longer depends on it.
//
// Note: the ENABLE_VERTEX_LIGHTING define from lighting.hglsl is used by
// gos_tex_vertex_lighted to select VS-side vs FS-side calc_light placement.
// static_prop.vert ALWAYS does VS-side here — the per-fragment branch in
// the legacy mover path doesn't apply to the slice 1 batcher's draw shape.
#include <include/lighting.hglsl>

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uint  a_localVertexID;
// Slice 2 (object-offload) — Stage 2.C.2: per-vertex hot-color tag from
// TG_TypeVertex::aRGBLight, written by registerType() at VBO offset 36.
// Decoded by get_base_light() against the per-type magic colors below.
layout(location = 4) in uint  a_aRGBLight;

struct Instance {
    mat4  modelMatrix;
    uint  typeID;
    uint  firstColorOffset;
    uint  flags;
    // Slice 2 (object-offload) — Stage 2.A renamed _pad0 → lightDataIndex
    // on the C++ side at offset 76; mirror the rename here for clarity.
    // Stage 2.C.2 reads it to index the LightsData[32] UBO (binding 0)
    // for calc_light below.
    uint  lightDataIndex;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};

// Slice 2 (object-offload) — Stage 2.C.2: per-type hot-color SSBO. Built
// once at finalizeGeometry() from TG_TypeShape::hotPinkRGB / hotYellowRGB /
// hotGreenRGB. Indexed by inst.typeID. 48 bytes per type.
struct PerTypeData {
    vec4 hotPinkRGB;
    vec4 hotYellowRGB;
    vec4 hotGreenRGB;
};

layout(std430, binding = 0) readonly buffer Instances { Instance     i[]; } instances_;
layout(std430, binding = 1) readonly buffer Colors    { uint         c[]; } colors_;
layout(std430, binding = 2) readonly buffer PerType   { PerTypeData  t[]; } perType_;
// Slice 2 (object-offload) — Stage 2.D.1: parity readback harness output.
// Bound only when MC2_OBJECT_PARITY_CHECK=1 from the C++ side; the
// u_parityWrite gate below short-circuits the write when unbound so this
// declaration is harmless on the default-off path. Index convention:
//   parityOut[gl_InstanceID * u_parityVertsPerType + gl_VertexID]
// where u_parityVertsPerType is set per-draw to type.vertexCount (constant
// inside one glDrawElementsInstancedBaseVertex call). The bound range from
// glBindBufferRange is sized exactly instanceCount * vertsPerType uint32
// entries, so writes never overflow.
layout(std430, binding = 3) buffer ParityOut { uint parityOut[]; } parityOut_;

uniform mat4 u_worldToClip;
uniform vec4 u_terrainViewport;
uniform mat4 u_mvp;
// Slice 2 (object-offload) — Stage 2.D.1: parity write gate.
// 0 (default) = no write to parityOut_; nonzero = write per-vertex lit ARGB.
// 'uniform uint' crashes this engine's shader compile (memory/uniform_uint_crash.md)
// so we use int + a >0 test in GLSL.
uniform int u_parityWrite;
// Per-draw vertex count for the type currently being drawn. Constant
// inside one glDrawElementsInstancedBaseVertex call. Set by the C++ side
// to the same value as u_maxLocalVertexID + 1.
uniform int u_parityVertsPerType;

out vec3  v_normal;
out vec2  v_uv;
flat out uint v_flags;
out vec4  v_highlight;
out vec4  v_fog;
out vec4  v_argb;
flat out uint v_localVertexID;

void main() {
    Instance inst = instances_.i[gl_InstanceID];
    // u_worldToClip uploaded GL_TRUE: GLSL sees transpose, so `M * v` ==
    // row-vec math == Stuff convention.
    // modelMatrix from SSBO std430 default col-major: GLSL sees same
    // matrix as memory. For row-vec convention (translation in row 3),
    // we need `v * M` in GLSL to apply translation correctly.
    vec4 world = vec4(a_position, 1.0) * inst.modelMatrix;
    // Apply full D3D->GL projection chain (identical to terrain_overlay.vert).
    // u_worldToClip outputs screen-pixel-homogeneous coords (D3D style).
    vec4 clip4 = u_worldToClip * world;
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
    px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
    px.z = clip4.z * rhw;
    vec4 ndc = u_mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

    // Behind-camera guard. The D3D-style manual perspective divide
    // (rhw = 1/clip.w, then px = clip.xy/w, then remap) produces
    // degenerate positions when clip.w <= 0 (vertex at or behind the
    // camera plane). Symptom: a triangle where one vertex is behind
    // the camera projects to spans-the-whole-screen stretched
    // artifacts at certain camera angles. The CPU path never sees
    // this because CPU pre-culls out-of-view objects; the GPU path
    // under the killswitch submits everything, so we have to clip
    // degenerate vertices here. Push them outside the clip volume so
    // OpenGL's standard triangle clipping handles it.
    if (clip4.w < 0.1) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }

    // Slice 2 (object-offload) — Stage 2.C.2: GPU vertex lighting.
    //
    // 1. Decode the per-vertex aRGBLight tag (a_aRGBLight) into a vec4 in
    //    B,G,R,A order to match get_base_light()'s expected swizzle. On
    //    little-endian x86 the C++ DWORD memcpy at registerType() lands as
    //    B,G,R,A bytes in memory; GL_UNSIGNED_INT pulls all 4 bytes as a
    //    single uint, then we extract per-byte and pack into vec4 with
    //    .xyzw = (B,G,R,A) so get_base_light()'s b|g<<8|r<<16|a<<24 decode
    //    matches mclib/tgl.cpp's per-vertex hot-color magic comparisons.
    vec4 perVertexARGB;
    perVertexARGB.x = float((a_aRGBLight >>  0) & 0xFFu) / 255.0;  // b
    perVertexARGB.y = float((a_aRGBLight >>  8) & 0xFFu) / 255.0;  // g
    perVertexARGB.z = float((a_aRGBLight >> 16) & 0xFFu) / 255.0;  // r
    perVertexARGB.w = float((a_aRGBLight >> 24) & 0xFFu) / 255.0;  // a

    // 2. Look up per-type hot-color magic. inst.typeID indexes the per-type
    //    SSBO populated at finalizeGeometry().
    PerTypeData ptd = perType_.t[inst.typeID];

    // 3. Decode base lighting (resolves the hot-color magic tags).
    //    isNight/nightFactor/lightsOut are stubbed to false/0/false for now —
    //    the legacy gos_tex_vertex_lighted shader at line 75 does the same
    //    pending the eye-state UBO wiring. Building "lit windows at night"
    //    will not light up until that follow-up.
    vec3 base_light = get_base_light(
        perVertexARGB,
        false, 0.0, false, false,
        ptd.hotPinkRGB.rgb,
        ptd.hotYellowRGB.rgb,
        ptd.hotGreenRGB.rgb);

    // 4. Compute world-space normal and position for calc_light's distance
    //    math. modelMatrix is `v * M` form (Stuff convention), so
    //    `vec4(p,1) * inst.modelMatrix` = world-space position. Same for
    //    normal modulo the (column-major mat3 of M) trick used at line ~80
    //    of the original; reuse the existing computation.
    vec3 worldNormal = mat3(inst.modelMatrix) * a_normal;
    vec3 worldPos    = world.xyz;

    // 5. Per-vertex full 6-type lighting via lighting.hglsl calc_light.
    //    inst.lightDataIndex addresses one ObjectLights entry in the
    //    LightsData[32] UBO populated per-actor by GatherGpuObjectLightDataOnly().
    vec3 lit = calc_light(int(inst.lightDataIndex), worldNormal, worldPos, base_light);

    // 6. Output. Alpha from the per-vertex tag (supports alpha-test path
    //    and matches CPU emit's alpha encoding). RGB is the lit color.
    v_argb = vec4(lit, perVertexARGB.w);

    // 7. Slice 2 (object-offload) — Stage 2.D.1: parity readback harness.
    //    Pack the per-vertex lit ARGB into a single uint matching the
    //    in-memory B,G,R,A byte order convention used everywhere in this
    //    engine (see memory/mc2_argb_packing.md and the
    //    a_aRGBLight decode at lines 107-110 above). The CPU side that
    //    Stage 2.D.2/2.D.3 will compare against (listOfTriangles[].aRGBLight)
    //    is also a DWORD-as-BGRA, so this packing keeps the bytewise
    //    compare arithmetic-free.
    //
    //    The bound parity buffer range is sized exactly
    //    instanceCount * u_parityVertsPerType uint entries on the C++ side,
    //    so this index is always in-bounds when u_parityWrite > 0.
    if (u_parityWrite > 0) {
        uint b8 = uint(clamp(lit.b * 255.0, 0.0, 255.0));
        uint g8 = uint(clamp(lit.g * 255.0, 0.0, 255.0));
        uint r8 = uint(clamp(lit.r * 255.0, 0.0, 255.0));
        uint a8 = uint(clamp(perVertexARGB.w * 255.0, 0.0, 255.0));
        uint packed = b8 | (g8 << 8) | (r8 << 16) | (a8 << 24);
        int idx = gl_InstanceID * u_parityVertsPerType + gl_VertexID;
        parityOut_.parityOut[idx] = packed;
    }

    v_normal     = worldNormal;
    v_uv         = a_uv;
    v_flags      = inst.flags;
    v_highlight  = inst.aRGBHighlight;
    v_fog        = inst.fogRGB;
    v_localVertexID = a_localVertexID;
}
