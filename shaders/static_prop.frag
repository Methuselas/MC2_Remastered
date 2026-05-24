// shaders/static_prop.frag
// GPU static prop renderer — main fragment shader.
// NOTE: no #version directive here — makeProgram() prepends "#version 430\n".
// All formerly-uint uniforms are declared `int` because this project's
// shader_builder crashes on `uniform uint` (see memory/uniform_uint_crash.md).

#define PREC highp

#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           StaticProp
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible (production path)
//                   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible (debug)
//   ShadowContract: castsStatic=true, castsDynamic=true,
//                   skipsPostScreenShadow=false (post-shadow applies)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque,
//                   requiresMRT=true

in vec3  v_normal;
in vec2  v_uv;
flat in uint v_flags;
in vec4  v_highlight;
in vec4  v_fog;
in vec4  v_argb;
flat in uint v_localVertexID;
flat in uint v_drawID;          // plan v3.8 Step 8.3: forwarded from VS as
                                // uint(gl_DrawIDARB) under MC2_COALESCE,
                                // else 0u (vertex shader sets per branch).

#ifdef MC2_COALESCE
// Plan v3.8 Step 8.3 — coalesce-only: per-draw indirection via gl_DrawIDARB.
// `int u_drawIDBase` per uniform_uint_crash.md (uniform uint crashes
// shader_builder); cast to uint at index time.
uniform int u_drawIDBase;
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   objectIdRaw;   // M1.5: handle.raw() (read into uint at use site)
    uint  materialIdx;   // MaterialGpu-3: index into MaterialTable.materials[]
                         // (was _pad1; uint matches uint32_t at std430 offset 28)
};
layout(std430, binding = 4) readonly buffer PerDrawData {
    PerDrawEntry entries[];
} perDraw_;
uniform sampler2DArray u_texArr;

// MaterialGpu-3: material table at binding 5.
// RENDER CONTRACT: static_prop.frag coalesce declares MaterialTable at binding 5
//   after MaterialGpu-3. Binding 5 may be unbound when u_materialGpuSample=0;
//   shader MUST NOT access materialTable_.materials[] in that case (enforced by the uniform branch below).
// Always declared in the coalesce variant so the reflection surface is stable.
// Instance name materialTable_ follows the usage pattern documented in material_gpu.hglsl.
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table

#else
uniform sampler2D u_tex;
uniform int       u_materialFlags;   // bit 0: ALPHA_TEST
uniform int       u_maxLocalVertexID;
uniform int       u_packetID;
#ifdef MC2_OBJECT_ID_BUFFER
// M1.5 non-coalesce path: handle bits uploaded as int (uniform-uint
// crash trap, memory/uniform_uint_crash.md), cast to uint in body.
uniform int       u_objectIdRaw;
#endif
#endif

uniform float u_fogValue;        // 1.0 = clear, 0.0 = fully fogged
uniform int   u_debugAddrMode;   // 0 normal, 1 gradient, 2 hash, 3 white, 4 argb-only, 5 tex-only, 6 highlight-only, 7 tex+highlight

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M1.5: per-pixel object handle. Emitted to GL_COLOR_ATTACHMENT2
// (R32UI). Spec section 5; struct field renamed in Task 7.
layout(location = 2) out uint v_objectId;
#endif

const int ALPHA_TEST_BIT = 1;

uint hash_u(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main() {
    // Plan v3.8 Step 8.5 — resolve the three per-draw values to locals so
    // the rest of main() reads them identically in legacy and coalesce
    // modes. Under MC2_COALESCE these come from PerDrawData.entries[] at
    // (v_drawID + u_drawIDBase); under legacy from per-packet/per-type
    // uniforms (uploaded by flush() at :1567/:1685/:1687).
#ifdef MC2_COALESCE
    int materialFlags    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialFlags;
    int packetID         = perDraw_.entries[v_drawID + uint(u_drawIDBase)].packetID;
    int maxLocalVertexID = perDraw_.entries[v_drawID + uint(u_drawIDBase)].maxLocalVertexID;
    int texArrayLayer    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer;
    // v3.8 mixed-size handling: the texture array is allocated at the
    // group's max(W,H). Each layer's actual texture occupies the
    // upper-left sub-region (0..uvScaleX, 0..uvScaleY). uvScale=1.0 →
    // texture fills layer (no remap). uvScale<1 → fract(v_uv) restores
    // GL_REPEAT semantics on the original texture's [0,1] domain, then
    // multiply by uvScale crops to the sub-region. Sampler is set to
    // GL_CLAMP_TO_EDGE so accidental over-shoot doesn't bleed into
    // adjacent (zero-padded) area at the edge.
    float uvScaleX = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleX;
    float uvScaleY   = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
    uint  materialIdx = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;
    vec2  uvSampled = fract(v_uv) * vec2(uvScaleX, uvScaleY);
    // MaterialGpu-3: runtime switch between legacy layer and material table.
    // u_materialGpuSample is a pass-wide (not per-fragment) uniform —
    // the branch collapses to a single predicate on AMD hardware.
    // materialTable_.materials[] is only accessed when u_materialGpuSample != 0,
    // enforcing the render contract above (binding 5 must be set when sampling).
    int effectiveLayer = texArrayLayer;
    if (u_materialGpuSample != 0) {
        uint albedo = materialTable_.materials[materialIdx].albedoTex;
        if (albedo != kMatTexAbsent) {
            // kMatTexAbsent = 0xFFFFFFFFu (defined in material_gpu.hglsl).
            // v2 mismatches=0 strongly predicts this guard is never triggered
            // for well-formed static-prop packets. Retained defensively.
            effectiveLayer = int(albedo);
        }
    }
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(effectiveLayer)));
#else
    int materialFlags    = u_materialFlags;
    int packetID         = u_packetID;
    int maxLocalVertexID = u_maxLocalVertexID;
    vec4 tex_color = texture(u_tex, v_uv);
#endif

    // LODBUG probe (debug-mode 8) bypasses alpha-test discard so it reports
    // "draw call landed here" for materials whose texture coverage is empty
    // (e.g. an unregistered LOD-1 typeShape that took a CPU-fallback path
    // emitting no geometry would simply produce zero fragments — mode 8 lets
    // the user verify that *no* fragment is being emitted, vs. "drawn but
    // transparent"). All other debug modes (1..7) retain the discard so
    // existing tree-card / alpha-test diagnostics are unchanged.
    if (u_debugAddrMode != 8 && (materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    if (u_debugAddrMode == 1) {
        float t = float(v_localVertexID) / max(float(maxLocalVertexID), 1.0);
        FragColor = vec4(t, t, t, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    if (u_debugAddrMode == 2) {
        uint h = hash_u(uint(packetID) * 2654435761u + v_localVertexID);
        FragColor = vec4(
            float((h >>  0) & 0xFFu) / 255.0,
            float((h >>  8) & 0xFFu) / 255.0,
            float((h >> 16) & 0xFFu) / 255.0,
            1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    // Bisection modes (RAlt+9 cycles 0..7).
    if (u_debugAddrMode == 3) { FragColor = vec4(1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 4) { FragColor = vec4(v_argb.rgb,    1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 5) { FragColor = vec4(tex_color.rgb, 1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 6) { FragColor = vec4(v_highlight.rgb * v_highlight.a, 1.0); GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible(); return; }
    if (u_debugAddrMode == 7) {
        vec3 rgb = tex_color.rgb + v_highlight.rgb * v_highlight.a;
        FragColor = vec4(rgb, 1.0);
        GBuffer1 = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }
    // LODBUG probe (mode 8): solid magenta, alpha-test bypassed above. Every
    // fragment emitted by any draw call lands here as bright pink so the user
    // can answer "did this building get drawn at all post LOD swap?" without
    // ambiguity from texture sampling, lighting, fog, or highlight math.
    if (u_debugAddrMode == 8) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0);
        GBuffer1  = rc_gbuffer1_legacyDebugSentinelScreenShadowEligible();
        return;
    }

    vec3 litRgb = v_argb.rgb;
    if ((materialFlags & ALPHA_TEST_BIT) != 0) {
        // Tree cards/leaves read too black on the light-facing falloff side.
        // Keep their lighting variation, but cap the darkest side at ~50%.
        litRgb = max(litRgb, vec3(0.5));
    }

    vec4 c = tex_color * vec4(litRgb, v_argb.a);
    c.rgb += v_highlight.rgb * v_highlight.a;
    c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue);

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
#ifdef MC2_OBJECT_ID_BUFFER
    // M1.5: emit handle.raw() to attachment-2. Alpha-tested fragments
    // that discard() above skip this write naturally. Coalesce path
    // sources from PerDrawData.entries[]; legacy path from u_objectIdRaw.
#ifdef MC2_COALESCE
    v_objectId = uint(perDraw_.entries[v_drawID + uint(u_drawIDBase)].objectIdRaw);
#else
    v_objectId = uint(u_objectIdRaw);
#endif
#endif
}
