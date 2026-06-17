//#version 430 (version provided by material prefix)

#define PREC highp

#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           VegetationCards
//   Color0:         RGBA opaque output (alpha-tested)
//   GBuffer1:       rc_gbuffer1_shadowHandled (no post-shadow)
//   ShadowContract: castsStatic=false, castsDynamic=false,
//                   skipsPostScreenShadow=true
//   StateContract:  depthTest=true, depthWrite=false, blend=none,
//                   cullFace=false, requiresMRT=true

in PREC vec2  v_atlasUV;
in PREC float v_camDist;
in PREC float v_seed;
in PREC vec3  v_worldPos;

layout(location=0) out PREC vec4 FragColor;
layout(location=1) out PREC vec4 GBuffer1;

uniform sampler2D    u_atlas;
uniform PREC vec4    u_terrainLightDir; // matches terrainLightDir convention in other passes

void main()
{
    // --- Distance dither fade (Bayer 4x4, stable, no TAA needed) ---
    PREC float fade = 1.0 - smoothstep(3000.0, 5000.0, v_camDist);
    if (fade <= 0.0) discard;

    const float bayer[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    ivec2 fc = ivec2(gl_FragCoord.xy) & 3;
    float bayerThreshold = bayer[fc.y * 4 + fc.x];
    if (fade < bayerThreshold) discard;

    // --- Atlas sample ---
    vec4 texel = texture(u_atlas, v_atlasUV);

    // --- Alpha cutoff ---
    if (texel.a < 0.5) discard;

    // --- Simple diffuse lighting (v1: upward-facing approximate normal, no normal map) ---
    PREC vec3 cardNormal = normalize(vec3(u_terrainLightDir.x * 0.2, u_terrainLightDir.y * 0.2, 1.0));
    PREC float NdotL = clamp(dot(cardNormal, u_terrainLightDir.xyz), 0.1, 1.0);
    PREC float diffuse = mix(0.5, 1.0, NdotL);

    // --- Output ---
    vec3 color = texel.rgb * diffuse;
    FragColor = vec4(color, 1.0);
    GBuffer1  = rc_gbuffer1_shadowHandled(cardNormal);
}
