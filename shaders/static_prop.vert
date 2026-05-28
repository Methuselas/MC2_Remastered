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
//
// Stage 2.D.2.1 (C1): MC2_STATIC_PROP_LIGHTING gates the .zyx swizzle in
// lighting.hglsl get_base_light(). This shader passes perVertexARGB with
// layout (.x=B, .y=G, .z=R, .w=A), so .zyx reorders to (R,G,B) for
// correct RGB-ordered input to calc_light. gos_tex_vertex_lighted.vert does
// NOT define this symbol and therefore takes the .xyz (legacy) path which
// preserves its pre-38ba240 behavior. See memory/mc2_argb_packing.md.
#define MC2_STATIC_PROP_LIGHTING
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
//   parityOut[gl_InstanceID * u_parityVertsPerType + (gl_VertexID - u_parityBaseVertex)]
// where u_parityVertsPerType is set per-type to numTypeTriangles*3 (expanded
// inside one glDrawElementsInstancedBaseVertex call). The bound range from
// glBindBufferRange is sized exactly instanceCount * vertsPerType uint32
// entries, so writes never overflow.
layout(std430, binding = 3) buffer ParityOut { uint parityOut[]; } parityOut_;

#ifdef MC2_USE_VIEW_UNIFORMS
#include <include/view_uniforms.hglsl>
#else
uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)
#endif
// Slice 2 (object-offload) — Stage 2.D.1: parity write gate.
// 0 (default) = no write to parityOut_; nonzero = write per-vertex lit ARGB.
// 'uniform uint' crashes this engine's shader compile (memory/uniform_uint_crash.md)
// so we use int + a >0 test in GLSL.
uniform int u_parityWrite;
// Per-draw vertex count for the type currently being drawn. Constant
// inside one glDrawElementsInstancedBaseVertex call. Equal to
// typeShape->numTypeTriangles * 3 (expanded/triangle-soup vertex count).
uniform int u_parityVertsPerType;
// VBO base vertex for the current type. gl_VertexID (with baseVertex) is
// absolute; subtract this to get the type-local [0, parityVerts) index.
// Set per-type (constant across all packets of the same type).
uniform int u_parityBaseVertex;
// Diagnostic Addition 3 (Approach A): when non-zero, the shader writes
// light[inst.lightDataIndex].numLights.x into parityOut_[0] from the
// (inst=0, vert=0) invocation only.  All other invocations still write
// their normal lit-ARGB if u_parityWrite>0.  C++ reads slot 0 back and
// prints [PARITY_DIAG v2] event=shader_observed gpu_numLights=N.
// Default 0 = disabled.  Set to 1 per-frame for typeId=474 draws when
// MC2_OBJECT_PARITY_TRACE=1.  'uniform uint' crashes engine compile
// (memory/uniform_uint_crash.md); use int.
uniform int u_parityNumLightsDebugMode;
// Diagnostic Addition 4 (Approach A): when non-zero, the shader writes
// get_base_light()'s RGB output (packed as B|G<<8|R<<16|0xFF<<24) into
// parityOut_[0] from the (inst=0, vert=0) invocation only.  C++ reads
// slot 0 back and prints [PARITY_DIAG v2] event=base_light_observed
// typeId=84 rgb=R,G,B (packed=0xPACKED).  Default 0 = disabled.
// Set to 1 per-frame for typeId=84 draws when MC2_OBJECT_PARITY_TRACE=1.
// 'uniform uint' crashes engine compile (memory/uniform_uint_crash.md).
uniform int u_parityBaseLightDebugMode;

// V-AMBIENT-STATIC-1: hemisphere ambient fill (StaticPropOpaque lane only).
// Strength 0.0 (default) == OFF and is mathematically a no-op (adds vec3(0)
// to lit). CPU uploads 1.0 when MC2_STATIC_PROP_AMBIENT_V1=1 (default OFF).
// Sky/ground are subtle neutrals; first improvement intentionally subtle.
// Axis: worldNormal is in STUFF/model space (a_normal * mat3(inst.modelMatrix)
// — see line ~226). inst.modelMatrix is Stuff-convention (.x=left, .y=elev,
// .z=fwd), and the Stuff->MC2 axis swap at line 156 is applied to POSITION
// ONLY, not to the normal. So Stuff "up" = worldNormal.y here, not .z.
// Window-flag nodes skip this term (they are magic-color only — matches
// existing window-skip of calc_light).
uniform float u_ambientV1Strength;

// V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane.
// Coefficient order (must match projector tools/ibl/project_sh.py +
// generated header RenderCore/IblShCoeffs.h):
//   [0]=L00, [1]=L1-1, [2]=L10, [3]=L11,
//   [4]=L2-2, [5]=L2-1, [6]=L20, [7]=L21, [8]=L22
// Coefficients are RAW projection integrals C_lm = integral(L * Y_lm dw).
// The Ramamoorthi-Hanrahan c1..c5 constants below absorb the per-band
// diffuse cosine-lobe kernel (pi, 2pi/3, pi/4) -- so this evaluator yields
// IRRADIANCE directly. Do NOT premultiply the kernel in the projector,
// and do NOT apply additional /pi at the caller -- c1..c5 are pre-convolved.
// Default u_iblShStrength = 0.0 (CPU uploads 0.0 when env unset/0).
// The `if (u_iblShStrength > 0.0)` guard below short-circuits evalShL2
// entirely -> byte-identical to pre-slice output.
uniform vec3  u_iblSh[9];
uniform float u_iblShStrength;

// V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular on
// StaticPropOpaque lane. Default strength = 0.0 (mathematical no-op via the
// `if (u_pbrV1Strength > 0.0)` early-out below). CPU uploads 0.0 when
// MC2_STATIC_PROP_PBR_V1 unset/=0 OR when MC2_VIEW_UNIFORMS=0 (the safety
// interlock; the entire PBR block requires u_cameraWorldPos which is only
// declared inside `#if defined(MC2_USE_VIEW_UNIFORMS)`).
//
// Per-vertex limitation: uses constant F0=vec3(0.04) (dielectric default)
// and fallback material scalars metallic=0.0, roughness=1.0. Per-fragment
// MaterialGpu lookup (per-instance metallicFactor/roughnessFactor + albedo-
// tinted F0 for metallics) deferred to V-MATERIAL-PBR-3 -- adding it here
// would require plumbing perDraw_ into the vertex stage (out of scope).
//
// Gate-ON visual: with the fallback roughness=1.0, smoothness=0 collapses
// the specPower to 1.0, so `pow(NdotH, 1)` yields a broad dielectric sheen
// (NOT a tight highlight). This is EXPECTED and acknowledged in
// RenderCore/RendererFeatureRegistry.h kFeatureTable doc. Gate-ON is NOT
// byte-identical to default-OFF; only default-OFF (strength=0.0) is.
uniform float u_pbrV1Strength;

// SH-L2 evaluator (Ramamoorthi-Hanrahan 2001 named constants).
// Axis convention: Y-up world (same as V-AMBIENT-STATIC-1 hemi_t at
// static_prop.vert:281). worldNormal is Stuff-space Y-up (model normal *
// mat3(Stuff modelMatrix); the Stuff->MC2 axis swap is applied to position
// only). Projector tools/ibl/project_sh.py uses the same Y-up basis
// (n = (sin(theta)*cos(phi), cos(theta), sin(theta)*sin(phi)), theta from
// +Y) so L20 binds to (3*n.y*n.y - 1) and L22 to (n.x*n.x - n.z*n.z).
vec3 evalShL2(vec3 n) {
    // Named Ramamoorthi-Hanrahan c1..c5 (Eq. 13). These constants ARE the
    // diffuse-kernel-times-basis-normalization-times-polynomial-coefficient
    // products: c1=0.429043 (band L=2 quadratic terms), c2=0.511664 (band L=1
    // linear terms), c3=0.743125 (L20 quadratic weight), c4=0.886227 (L00
    // constant), c5=0.247708 (L20 constant offset). Together they encode
    // the cosine-lobe kernel (pi, 2pi/3, pi/4) baked into the evaluator,
    // so the input u_iblSh[i] are RAW projection coefficients and the output
    // is diffuse irradiance directly.
    const float kSH_c1 = 0.429043;
    const float kSH_c2 = 0.511664;
    const float kSH_c3 = 0.743125;
    const float kSH_c4 = 0.886227;
    const float kSH_c5 = 0.247708;
    // Y-up axis convention: pole on .y. L20 polynomial binds to (3y^2 - 1),
    // L22 polynomial to (x^2 - z^2). Matches projector basis in
    // tools/ibl/project_sh.py (n = (sin(theta)*cos(phi), cos(theta),
    // sin(theta)*sin(phi)) with theta the polar angle from +Y).
    return (
          kSH_c1 * u_iblSh[8] * (n.x * n.x - n.z * n.z)         // L22:  x^2 - z^2
        + kSH_c3 * u_iblSh[6] * (n.y * n.y)                     // L20 quadratic
        + kSH_c4 * u_iblSh[0]                                   // L00
        - kSH_c5 * u_iblSh[6]                                   // L20 offset
        + 2.0 * kSH_c1 * u_iblSh[4] * (n.x * n.y)               // L2-2: xy
        + 2.0 * kSH_c1 * u_iblSh[5] * (n.y * n.z)               // L2-1: yz
        + 2.0 * kSH_c1 * u_iblSh[7] * (n.x * n.z)               // L21:  xz
        + 2.0 * kSH_c2 * u_iblSh[3] * n.x                       // L11:  x
        + 2.0 * kSH_c2 * u_iblSh[1] * n.y                       // L1-1: y (Y-up pole)
        + 2.0 * kSH_c2 * u_iblSh[2] * n.z                       // L10:  z
    );
}

out vec3  v_normal;
out vec2  v_uv;
flat out uint v_flags;
out vec4  v_highlight;
out vec4  v_fog;
out vec4  v_argb;
flat out uint v_localVertexID;
flat out uint v_drawID;          // plan v3.8 Step 8.2: forwarded to FS as
                                 // uint(gl_DrawIDARB) under MC2_COALESCE,
                                 // else 0u (fragment shader's MC2_COALESCE
                                 // branch indexes perDraw_.entries[] by it).

void main() {
    // Plan v3.8 Step 8.1 — coalesce variant indexes by
    // (gl_BaseInstanceARB + gl_InstanceID): the multi-draw issues N
    // instances starting at gl_BaseInstanceARB, and gl_InstanceID
    // restarts at 0 each draw call. Legacy single-draw uses bare
    // gl_InstanceID.  ARB-suffixed builtins are mandatory under
    // #version 430 + GL_ARB_shader_draw_parameters; the unsuffixed
    // gl_BaseInstance / gl_DrawID are the GL 4.6 core promotion names
    // and are NOT defined under the extension.
    // Plan v3.8 Step 8.2 — v_drawID forwarding to fragment.
#ifdef MC2_COALESCE
    Instance inst = instances_.i[gl_BaseInstanceARB + gl_InstanceID];
    v_drawID      = uint(gl_DrawIDARB);
#else
    Instance inst = instances_.i[gl_InstanceID];
    v_drawID      = 0u;
#endif
    // modelMatrix from SSBO std430 default col-major: GLSL sees the same
    // matrix as memory. For Stuff row-vec convention (translation in row 3),
    // use `v * M` to apply translation.
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // 2026-05-04 fix: inst.modelMatrix is shapeToWorld in Stuff/MLR camera
    // frame (.x=left, .y=elev, .z=forward). terrainMVP expects MC2 world
    // (x=east, y=north, z=elev). Apply the documented mapping
    // (memory/cpu_displacement_done.md):
    //     terrain.x = -camera.x, terrain.y = camera.z, terrain.z = camera.y
    // Without this swap, world.y (elev) gets read by terrainMVP as MC2
    // north and world.z (south distance) as MC2 elev → assets misplaced
    // along a wrong axis (visible 2026-05-04 as "trees in the sky" pattern).
    vec3 world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y);
    vec4 world = vec4(world_mc2, 1.0);
    // Match terrain_overlay.vert exactly: terrainMVP is the CPU-composed
    // axisSwap * worldToClip matrix uploaded GL_FALSE.
    gl_Position = u_worldToClipGL * world;

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
    //    isNight/nightFactor are stubbed to false/0 for now — the legacy
    //    gos_tex_vertex_lighted shader at line 75 does the same pending the
    //    eye-state UBO wiring.
    //
    // Stage 2.C.4 N1.5 (revised): pass lightsOut INTO get_base_light() via
    // its existing 5th parameter rather than short-circuiting at the call
    // site. The prior short-circuit (if (kFlagIsLightsOut) base_light=vec3(0))
    // was over-broad: it suppressed ALL aRGBLight values including magic-tag
    // branches (hot-pink/yellow/green) that CPU handles unconditionally
    // BEFORE consulting lightsOut. CPU's `if (!lightsOut)` gate at
    // tgl.cpp:1880 is ONLY inside the `else if (startVLight & 0x00ffffff)`
    // non-magic colored-seed arm. get_base_light()'s existing gate at
    // lighting.hglsl:116 is already in the right place — passing the real
    // lightsOut bit lets the function-internal gate fire for only that arm,
    // exactly mirroring CPU behavior.
    //
    // Example: hot-pink + lightsOut=1 + isWindow=1:
    //   CPU:  magic-tag arm wins (tgl.cpp:1804-1828, sets 0x2F2F2F); lightsOut
    //         NOT consulted; window-skip suppresses lighting loop.
    //   GPU pre-fix: N1.5 short-circuit fires → base_light=vec3(0) → lit=0 → 0x000000 WRONG.
    //   GPU post-fix: get_base_light runs hot-pink branch → vec3(0x2F/255); window-skip
    //         passes lit=base_light → 0x2F2F2F. MATCHES CPU.
    const uint kFlagIsLightsOut = (1u << 0);
    bool lightsOut = (inst.flags & kFlagIsLightsOut) != 0u;
    vec3 base_light = get_base_light(
        perVertexARGB,
        false, 0.0, false, lightsOut,
        ptd.hotPinkRGB.rgb,
        ptd.hotYellowRGB.rgb,
        ptd.hotGreenRGB.rgb);

    // 4. Compute world-space normal and position for calc_light's distance
    //    math. modelMatrix is `v * M` form (Stuff convention, SSBO std430),
    //    so `vec4(p,1) * inst.modelMatrix` = world-space position. Normals
    //    must use the same row-vector convention: `a_normal * mat3(M)`.
    //    Using `mat3(M) * a_normal` (column-vector) applies the INVERSE
    //    rotation and produces wrong lighting for any non-identity rotation.
    //    Proved by Stage 2.D.2 parity: ~94% mismatch before fix, 0% after.
    //
    //    Stage 2.D.2.1 (m5) NOTE: ASSUMES no non-uniform scale on static-prop
    //    shapeToWorld. If non-uniform scale is ever introduced, the correct
    //    transform for normals is the inverse-transpose of the 3x3 submatrix,
    //    not `a_normal * mat3(M)` — that fix becomes WRONG under non-uniform
    //    scale and would silently produce incorrect lighting.
    vec3 worldNormal = a_normal * mat3(inst.modelMatrix);
    vec3 worldPos    = world.xyz;

    // 5. Per-vertex full 6-type lighting via lighting.hglsl calc_light.
    //    inst.lightDataIndex addresses one ObjectLights entry in the
    //    LightsData[32] UBO populated per-actor by GatherGpuObjectLightDataOnly().
    //
    //    isWindow (inst.flags bit 1): mirrors CPU tgl.cpp:1929 `!isWindow` guard.
    //    Window nodes (LitWin_* node names) skip ambient + directional lighting
    //    so their hot-color magic (dark-grey daytime, glowing at night) is
    //    unaffected by the sun direction. GPU must match: skip calc_light when
    //    isWindow is set, returning only get_base_light() output.
    //    Stage 2.D.2 parity proved this is the cause of ~86% mismatch on window
    //    building types — GPU was adding lighting where CPU skips it.
    //
    //    [T3.2] Bit-2 (kFlagIsSpotlight) read deleted. Post-T3.1 the C++
    //    submit path skips spotlight children unconditionally (T1.3 ->
    //    unconditional `continue` in gos_static_prop_batcher.cpp
    //    submitMultiShape). The CPP-side bit-2 emission is dead, so the
    //    shader-side bit-2 read is removed in lockstep per
    //    memory/cpp_glsl_ubo_struct_lockstep.md. Spotlights now contribute
    //    via real TG_Light registrations (BldgAppearance::update), not via
    //    cone-billboard packets.
    const uint kFlagIsWindow    = (1u << 1);
    vec3 lit;
    if ((inst.flags & kFlagIsWindow) != 0u) {
        // Window node: hot-color magic only, no sun/ambient lighting.
        lit = base_light;
    } else {
        lit = calc_light(int(inst.lightDataIndex), worldNormal, worldPos, base_light);

        // V-AMBIENT-STATIC-1: hemisphere fill. Strength=0.0 (default) is a
        // bitwise no-op (add vec3(0)). Skip for window branch to preserve
        // hot-color magic behavior exactly. Axis: worldNormal is STUFF-space
        // (model normal * mat3(Stuff modelMatrix); the Stuff->MC2 swap at
        // line 156 transforms POSITION only). In Stuff convention .y is
        // elevation, so +y = up. Using .z here would gradient along the
        // Stuff fwd axis ("north/depth"), making north-facing walls
        // brighten instead of rooftops — see reviewer finding pre-fix.
        const vec3 kAmbientV1Sky    = vec3(0.20, 0.22, 0.28);
        const vec3 kAmbientV1Ground = vec3(0.10, 0.09, 0.07);
        float hemi_t = 0.5 + 0.5 * worldNormal.y;
        vec3 ambient_v1 = mix(kAmbientV1Ground, kAmbientV1Sky, hemi_t)
                          * u_ambientV1Strength;
        lit += ambient_v1;

        // V-IBL-STATIC-1: SH-L2 image-based ambient. Strength gate via env
        // (s_iblShEnabled in batcher); when OFF the CPU uploads 0.0 and the
        // guard short-circuits evalShL2 entirely -> byte-identical pixel.
        // worldNormal is Stuff-space Y-up, matching the projector basis
        // (see tools/ibl/project_sh.py axis-convention block).
        if (u_iblShStrength > 0.0) {
            vec3 ibl = evalShL2(normalize(worldNormal));
            lit += ibl * u_iblShStrength;
        }

        // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular.
        // GUARDED by `#if defined(MC2_USE_VIEW_UNIFORMS)` because the math
        // requires u_cameraWorldPos which is only declared inside that
        // include block (view_uniforms.hglsl). When MC2_VIEW_UNIFORMS=0,
        // both the shader-side compile-guard AND the CPU-side runtime
        // safety interlock (s_viewUniformsDisabled in gos_static_prop_batcher.cpp)
        // force the path off -- defense in depth.
        //
        // Sun direction/color: inline iteration over LightsData SSBO
        // (Phase 0 Option A). The sun is whichever ObjectLights entry has
        // light_type==TG_LIGHT_INFINITE (no stable slot guarantee). We take
        // the FIRST such entry; mclib/tgl.cpp populates exactly one
        // INFINITE light per ObjectLights entry under stock missions
        // (the sun), so this is unambiguous. If no INFINITE light is
        // present we skip the specular contribution.
        //
        // Sun direction convention: lighting.hglsl calc_light's INFINITE
        // branch uses `dot(normal, -ld.light_dir[i].xyz)`, so ld.light_dir
        // is the surface-to-sun direction negated (i.e. it points
        // FROM the sun TO the surface). To get L (surface->light) we
        // negate: L = -ld.light_dir[i].xyz.
#if defined(MC2_USE_VIEW_UNIFORMS)
        if (u_pbrV1Strength > 0.0) {
            ObjectLights ld_pbr = light[int(inst.lightDataIndex)];
            vec3 sunDir   = vec3(0.0);
            vec3 sunColor = vec3(0.0);
            bool sunFound = false;
            int n_pbr = min(ld_pbr.numLights.x, MAX_LIGHTS_IN_WORLD);
            for (int i = 0; i < n_pbr; ++i) {
                if (int(ld_pbr.light_dir[i].w) == TG_LIGHT_INFINITE) {
                    sunDir   = ld_pbr.light_dir[i].xyz;
                    sunColor = ld_pbr.light_color[i].xyz;
                    sunFound = true;
                    break;
                }
            }
            if (sunFound) {
                vec3 N = normalize(worldNormal);
                vec3 V_eye = normalize(u_cameraWorldPos.xyz - worldPos);
                vec3 L = normalize(-sunDir);
                vec3 H = normalize(L + V_eye);
                // Per-vertex fallbacks: metallic=0, roughness=1
                // (per-fragment MaterialGpu lookup deferred to V-MATERIAL-PBR-3).
                float metallicFactor  = 0.0;
                float roughnessFactor = 1.0;
                const vec3 kF0Dielectric = vec3(0.04);
                vec3  F0     = mix(kF0Dielectric, vec3(0.04), metallicFactor);
                // ^ Equivalent to vec3(0.04); albedo-tinted F0 for metallics
                //   deferred (requires per-fragment access).
                vec3  F      = F0 + (vec3(1.0) - F0)
                                 * pow(max(0.0, 1.0 - dot(N, V_eye)), 5.0);
                float smoothness = 1.0 - roughnessFactor;
                float specPower  = mix(1.0, 512.0, smoothness * smoothness);
                float NdotH      = max(0.0, dot(N, H));
                float NdotL      = max(0.0, dot(N, L));
                vec3  specular   = sunColor * F * pow(NdotH, specPower) * NdotL;
                lit += specular * u_pbrV1Strength;
            }
        }
#endif  // MC2_USE_VIEW_UNIFORMS
    }

    // 6. aRGBHighlight additive contribution — mirrors CPU tgl.cpp:2313-2335.
    //    CPU adds R/G/B channels of aRGBHighlight (objective/selection tint)
    //    to the lit argb, clamping each channel to [0, 255]. Must apply
    //    BEFORE the parity pack below so the readback includes the highlight.
    //    inst.aRGBHighlight is stored as (R,G,B,A) float[4] by submit() at
    //    gos_static_prop_batcher.cpp:745-748 (decode from packed ARGB uint).
    //    When no highlight is active the CPU passes aRGBHighlight=0 → r/g/b=0
    //    → no-op add, matching tgl.cpp:2313 `if (aRGBHighlight)` gate.
    //    Stage 2.D.2.1 (M3): closes the gap vs CPU for highlighted actors.
    lit = clamp(lit + inst.aRGBHighlight.rgb, 0.0, 1.0);

    // 6b. Output. Alpha is always 1.0 — CPU tgl.cpp:2225 hardcodes
    //    `(0xFF << 24) | r | g | b`; the raw aRGBLight alpha byte is
    //    NEVER propagated into the per-vertex lit argb. GPU must match.
    //    (The alpha in some aRGBLight tags like 0x02FFFFFF or 0xFAxxxxxx
    //    is MC2 data that the CPU simply ignores for vertex lighting.)
    v_argb = vec4(lit, 1.0);

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
        // Alpha is always 0xFF to match CPU tgl.cpp:2225 which hardcodes
        // (0xFF << 24) and ignores the raw aRGBLight alpha byte.
        uint a8 = 255u;
        uint bits = b8 | (g8 << 8) | (r8 << 16) | (a8 << 24);
        // gl_VertexID = IBO[i] + baseVertex (from glDrawElementsInstancedBaseVertex).
        // Subtract u_parityBaseVertex (the type's VBO baseVertex) to get the
        // type-local index in [0, u_parityVertsPerType). This keeps the write
        // in-bounds for types whose VBO region doesn't start at 0.
        int localVert = gl_VertexID - u_parityBaseVertex;
        int idx = gl_InstanceID * u_parityVertsPerType + localVert;
        parityOut_.parityOut[idx] = bits;

        // Addition 3 (Approach A): diagnostic numLights capture.
        // When u_parityNumLightsDebugMode==1 and this is the (inst=0, vert=0)
        // invocation, write the raw numLights.x value into slot 0 of the parity
        // buffer so C++ can read it back.  This overwrites the lit-ARGB at that
        // slot; the C++ side skips the normal parity compare for typeId=474 on
        // this frame and instead prints the numLights value.
        if (u_parityNumLightsDebugMode > 0 &&
            gl_InstanceID == 0 && localVert == 0) {
            ObjectLights ld = light[int(inst.lightDataIndex)];
            parityOut_.parityOut[0] = uint(ld.numLights.x);
        }
        // Addition 4 (Approach A): base_light diagnostic capture for typeId=84.
        // When u_parityBaseLightDebugMode==1 and this is the (inst=0, vert=0)
        // invocation, pack get_base_light()'s output (base_light, computed
        // BEFORE calc_light) as B|G<<8|R<<16|0xFF<<24 into parityOut_[0].
        // C++ reads slot 0 back and prints event=base_light_observed.
        // This runs AFTER Addition 3 so typeId=84 never triggers Addition 3.
        if (u_parityBaseLightDebugMode > 0 &&
            gl_InstanceID == 0 && localVert == 0) {
            uint bl_b = uint(clamp(base_light.b * 255.0, 0.0, 255.0));
            uint bl_g = uint(clamp(base_light.g * 255.0, 0.0, 255.0));
            uint bl_r = uint(clamp(base_light.r * 255.0, 0.0, 255.0));
            parityOut_.parityOut[0] = bl_b | (bl_g << 8u) | (bl_r << 16u) | (0xFFu << 24u);
        }
    }

    v_normal     = worldNormal;
    v_uv         = a_uv;
    v_flags      = inst.flags;
    v_highlight  = inst.aRGBHighlight;
    v_fog        = inst.fogRGB;
    v_localVertexID = a_localVertexID;
}
