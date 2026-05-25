// shaders/mech.frag — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".

// render_contract.hglsl uses PREC macro for portability; the includer must
// define it. Same convention as static_prop.frag (memory: PREC defaults to
// highp in this engine).
#define PREC highp

#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           Mech
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible
//   GBuffer2:       rc_gbuffer2_objectIdU32  // M2.5 (#ifdef MC2_OBJECT_ID_BUFFER)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true

in vec2 v_uv;
in vec4 v_litColor;
in vec4 v_highlightColor;
in vec4 v_fogRGB;  // .rgb = engine fog color, .a = per-actor haze factor (B2)
in vec3 v_normal;

uniform sampler2D u_tex;
uniform int u_materialFlags;  // bit 0: ALPHA_TEST
// Slice B2: u_fogValue retained for backward compat / parity with
// static_prop convention but no longer drives the mix — per-actor
// haze is in v_fogRGB.a now. The CPU side stops uploading the
// uniform (location -1 = silent no-op for glUniform1f) but keeping
// the declaration leaves room for a global haze override later.
uniform float u_fogValue;
// Slice A debug: 0=normal, 1=solid magenta, 2=texture only, 3=light only,
// 4=normal-as-color. Wired through MC2_MECH_FRAG_DEBUG env var on the C++
// side via a uniform write at flush time.
uniform int u_debugMode;

layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M2.5: per-pixel mech ObjectID. Emitted to GL_COLOR_ATTACHMENT2
// (R32_UINT; M1.5 substrate). `flat in` matches mech.vert's
// `flat out uint v_objectIdRaw`. Alpha-tested fragments that
// discard() at line 56 skip this write naturally -- the attachment-2
// pixel retains the clear value (0 = Handle::invalid()), correctly
// classified as background under lookupAtPixel.
flat in uint v_objectIdRaw;
layout(location=2) out uint v_objectId;
#endif

const int ALPHA_TEST_BIT = 1;

void main() {
    // textureLod(.., 0.0) instead of texture(): AMD RX 7900 XTX
    // strict-fails auto-LOD sampling on mech paint-scheme textures even
    // with MIN_FILTER=GL_LINEAR — hardware appears to access mip 1+
    // (which is undefined / empty) and returns black. Constant UV (zero
    // derivatives) clamps LOD to base level and works; varying UV
    // doesn't. textureLod forces level 0 explicitly. Diagnosed
    // 2026-05-08 via debug-mode 7 vs 8 contrast.
    // Static_prop.frag escapes this trap because its textures have
    // a complete mip pyramid by the time it samples; mech textures
    // load with the paint-scheme cycle that doesn't always leave all
    // mip levels populated when the GPU mech batcher's flush runs.
    vec4 tex_color = textureLod(u_tex, v_uv, 0.0);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    vec4 c = tex_color * v_litColor;
    c.rgb += v_highlightColor.rgb * v_highlightColor.a;
    // Slice B2: per-actor haze. v_fogRGB.a=0 → clear, =1 → fully fogged.
    c.rgb  = mix(c.rgb, v_fogRGB.rgb, v_fogRGB.a);

    // Debug overrides (MC2_MECH_FRAG_DEBUG=N).
    if      (u_debugMode == 1) c = vec4(1.0, 0.0, 1.0, 1.0);                    // solid magenta
    else if (u_debugMode == 2) c = vec4(tex_color.rgb, 1.0);                    // texture only
    else if (u_debugMode == 3) c = vec4(v_litColor.rgb, 1.0);                   // light only
    else if (u_debugMode == 4) c = vec4(normalize(v_normal) * 0.5 + 0.5, 1.0);  // normal-as-color
    else if (u_debugMode == 5) c = vec4(fract(v_uv.x), fract(v_uv.y), 0.0, 1.0); // UV visualizer
    else if (u_debugMode == 6) c = vec4(tex_color.aaa, 1.0);                    // texture alpha as grey
    else if (u_debugMode == 7) c = vec4(texture(u_tex, vec2(0.5, 0.5)).rgb, 1.0); // sample at hardcoded UV
    else if (u_debugMode == 8) c = vec4(textureLod(u_tex, v_uv, 0.0).rgb, 1.0);  // explicit LOD 0 sample
    else if (u_debugMode == 9) c = vec4(v_litColor.rgb / max(textureLod(u_tex, v_uv, 0.0).rgb, vec3(0.001)), 1.0); // lighting / texture (texture-inversion overlay)

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: emit per-pixel RenderObjectHandle.raw(). Debug-mode pixels
    // (u_debugMode 1..9 at lines 65-73) DO NOT discard; they still emit
    // a valid handle, which is the correct substrate behavior (a
    // lookupAtPixel on a debug-color pixel returns the actor's handle).
    v_objectId = v_objectIdRaw;
#endif
}
