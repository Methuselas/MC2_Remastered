// shaders/static_prop_depth.frag
// GPU static prop renderer — DEPTH-PREPASS fragment shader.
//
// FOLIAGE-STATICPROP-DEPTH-PREPASS-1: a cheap depth-only companion to
// static_prop.frag. It performs ONLY the alpha-test discard decision that
// static_prop.frag does at its line ~215, then returns — writing nothing but
// depth (no color, no GBuffer1, no object-id). Pairs with static_prop.vert
// verbatim (same VS program, `invariant gl_Position`) so the depth it lays is
// bit-identical to the color pass, which is the precondition for the color
// pass to run GL_EQUAL early-Z.
//
// THE LOAD-BEARING INVARIANT (spec 2026-06-04-foliage-depth-prepass): the
// discard decision here MUST be byte-identical to static_prop.frag. That means
// `materialFlags` and `tex_color.a` and the UV used to sample MUST be derived
// the SAME way for BOTH the coalesce path (PerDrawEntry SSBO @ binding 4 +
// u_texArr + u_drawIDBase) and the legacy path (u_materialFlags + u_tex), and
// the same `u_debugAddrMode != 8` bypass. The declarations below are copied
// verbatim from static_prop.frag's coalesce/legacy branches so the derivation
// is identical.
//
// NOTE: no #version directive here — makeProgram() prepends the prefix
// ("#version 430\n" plus the same defines used for static_prop.frag, so the
// same MC2_COALESCE / MC2_OBJECT_ID_BUFFER / MC2_USE_VIEW_UNIFORMS /
// MC2_STATICPROP_PBR_SLOTS selection applies). All formerly-uint uniforms are
// declared `int` because this project's shader_builder crashes on
// `uniform uint` (see memory/uniform_uint_crash.md).

#define PREC highp

// static_prop.vert outputs many varyings; the depth frag only consumes v_uv
// and (under coalesce) v_drawID. The rest are written by the VS but ignored
// here — that is allowed (unread VS outputs are fine).
in vec2  v_uv;
flat in uint v_drawID;          // mirrors static_prop.frag: uint(gl_DrawIDARB)
                                // under MC2_COALESCE, else 0u.

#ifdef MC2_COALESCE
// Coalesce-only: per-draw indirection via gl_DrawIDARB, exactly as
// static_prop.frag:65-100. `int u_drawIDBase` per uniform_uint_crash.md.
uniform int u_drawIDBase;
struct PerDrawEntry {
    int   packetID;
    int   materialFlags;
    int   maxLocalVertexID;
    int   texArrayLayer;
    float uvScaleX;
    float uvScaleY;
    int   objectIdRaw;
    uint  materialIdx;
};
layout(std430, binding = 4) readonly buffer PerDrawData {
    PerDrawEntry entries[];
} perDraw_;
uniform sampler2DArray u_texArr;

// MaterialGpu table at binding 5 — declared in the coalesce variant so the
// reflection surface matches static_prop.frag (effectiveLayer derivation reads
// it under u_materialGpuSample). Same include + binding as static_prop.frag.
#include <include/material_gpu.hglsl>
layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;
uniform int u_materialGpuSample;  // 0 = legacy texArrayLayer, 1 = material table
#ifdef MC2_STATICPROP_PBR_SLOTS
// Declared for reflection-surface parity with static_prop.frag; the depth
// shader does NOT sample ORM (roughness/metallic are irrelevant to the alpha
// decision). Kept so the same prefix/defines compile cleanly.
uniform sampler2DArray u_ormTexArr;
uniform int            u_ormSampleEnable;
#endif

#else
uniform sampler2D u_tex;
uniform int       u_materialFlags;   // bit 0: ALPHA_TEST
#endif

// Same as static_prop.frag: 0 normal .. 8 LODBUG probe (bypasses discard).
uniform int   u_debugAddrMode;

// No color/GBuffer/object-id outputs by design — depth only.

const int ALPHA_TEST_BIT = 1;

void main() {
    // Resolve materialFlags + tex_color identically to static_prop.frag's
    // main() so the discard decision is byte-identical.
#ifdef MC2_COALESCE
    int materialFlags = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialFlags;
    int texArrayLayer = perDraw_.entries[v_drawID + uint(u_drawIDBase)].texArrayLayer;
    float uvScaleX    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleX;
    float uvScaleY    = perDraw_.entries[v_drawID + uint(u_drawIDBase)].uvScaleY;
    uint  materialIdx = perDraw_.entries[v_drawID + uint(u_drawIDBase)].materialIdx;
    vec2  uvSampled   = fract(v_uv) * vec2(uvScaleX, uvScaleY);
    // MaterialGpu sampling switch — same logic as static_prop.frag:190-199.
    int effectiveLayer = texArrayLayer;
    if (u_materialGpuSample != 0) {
        uint albedo = materialTable_.materials[materialIdx].albedoTex;
        if (albedo != kMatTexAbsent) {
            effectiveLayer = int(albedo);
        }
    }
    vec4 tex_color = texture(u_texArr, vec3(uvSampled, float(effectiveLayer)));
#else
    int materialFlags = u_materialFlags;
    vec4 tex_color = texture(u_tex, v_uv);
#endif

    // BYTE-IDENTICAL to static_prop.frag:215 — same operands, same threshold,
    // same u_debugAddrMode != 8 bypass. This is the only work the depth shader
    // does; everything else (lighting, fog, SH-L2, PBR, debug views,
    // object-id) is intentionally omitted.
    if (u_debugAddrMode != 8 && (materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }
    // Surviving fragments write depth only (no color attachments declared).
}
