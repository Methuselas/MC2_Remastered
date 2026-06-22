//#version 420

#define PREC highp

#include <include/lighting.hglsl>
#include <include/shadow.hglsl>
#include <include/render_contract.hglsl>
#include <include/material_gpu.hglsl>

layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;

uniform vec4 light_offset_;
uniform int gpuProjection;
uniform sampler2D tex1;
uniform sampler2D u_normalTex;
uniform sampler2D u_ormTex;
uniform PREC vec4 fog_color;
uniform PREC vec4 u_buildingPbrControls;

in PREC vec3 Normal;
in PREC vec2 Texcoord;
in PREC vec4 VertexColor;
in PREC vec3 VertexLight;
in PREC vec3 WorldPos;
in PREC vec3 CameraPos;
in PREC vec3 MC2WorldPos;

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

vec3 derivativeTbnNormal(vec3 n, vec3 p, vec2 uv, vec3 tangentNormal)
{
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 t = normalize(dp1 * duv2.y - dp2 * duv1.y);
    vec3 b = normalize(-dp1 * duv2.x + dp2 * duv1.x);
    mat3 tbn = mat3(t, b, normalize(n));
    return normalize(tbn * tangentNormal);
}

void main(void)
{
    PREC vec2 materialUv = Texcoord * max(u_buildingPbrControls.x, 0.001);
    PREC vec4 legacyAlbedo = texture(tex1, Texcoord);

    // UB2-05: sample u_normalTex / u_ormTex and compute the derivative TBN
    // (dFdx/dFdy live in derivativeTbnNormal) BEFORE the conditional ALPHA_TEST
    // discard. Sampling / implicit-LOD / derivatives after a non-uniform discard
    // are GLSL UB — a 2x2 quad with some pixels discarded yields undefined
    // derivatives (vendor-divergent / bites NVIDIA). Hoisting keeps kept-pixel
    // output byte-identical; discarded pixels just compute unused samples then
    // discard. No material/lighting/color change.
    MaterialGpu mat = materialTable_.materials[0];
    PREC vec3 tangentNormal = texture(u_normalTex, materialUv).xyz * 2.0 - 1.0;
    PREC vec3 pbrNormal = derivativeTbnNormal(normalize(Normal), WorldPos, materialUv, tangentNormal);
    PREC vec3 orm = texture(u_ormTex, materialUv).rgb;

#ifdef ALPHA_TEST
    if (legacyAlbedo.a < 0.5)
        discard;
#endif
    PREC float ao = orm.r;
    PREC float roughness = clamp((orm.g * mat.roughnessFactor) + u_buildingPbrControls.y, 0.04, 1.0);
    PREC float metallic = clamp(orm.b * mat.metallicFactor * u_buildingPbrControls.z, 0.0, 1.0);

#if ENABLE_VERTEX_LIGHTING
    PREC vec3 lighting = VertexLight;
#else
    const int lights_index = int(light_offset_.x);
    PREC vec3 lighting = calc_light(lights_index, pbrNormal, WorldPos, VertexLight);
#endif

    PREC vec3 baseColor = legacyAlbedo.rgb;
    PREC vec3 diffuse = baseColor * lighting * mix(1.0, 0.35, metallic);

    PREC vec3 v = normalize(-WorldPos);
    PREC vec3 l = normalize(vec3(-0.35, 0.65, 0.55));
    PREC vec3 h = normalize(v + l);
    PREC float ndotl = max(dot(pbrNormal, l), 0.0);
    PREC float ndoth = max(dot(pbrNormal, h), 0.0);
    PREC float specPower = mix(96.0, 12.0, roughness);
    PREC vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    PREC vec3 specular = f0 * pow(ndoth, specPower) * ndotl * (1.0 - roughness * 0.55);

    PREC vec3 ambientFill = baseColor * (0.18 + 0.22 * ao);
    PREC vec3 color = diffuse * ao + ambientFill + specular;
    color = apply_fog(color, WorldPos.xyz, CameraPos);

    FragColor = vec4(color, legacyAlbedo.a);
    GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(pbrNormal));
}
