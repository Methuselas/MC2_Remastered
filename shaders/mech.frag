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
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true

in vec2 v_uv;
in vec4 v_litColor;
in vec4 v_highlightColor;
in vec3 v_fogRGB;
in vec3 v_normal;

uniform sampler2D u_tex;
uniform int u_materialFlags;  // bit 0: ALPHA_TEST
uniform float u_fogValue;     // 1.0 = clear (per static_prop convention)

layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;

const int ALPHA_TEST_BIT = 1;

void main() {
    vec4 tex_color = texture(u_tex, v_uv);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

    vec4 c = tex_color * v_litColor;
    c.rgb += v_highlightColor.rgb * v_highlightColor.a;
    c.rgb  = mix(v_fogRGB, c.rgb, u_fogValue);

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(normalize(v_normal));
}
