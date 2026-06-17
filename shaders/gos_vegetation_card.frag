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
in PREC float v_camTrueDist;
in PREC float v_cardBottom;
in PREC float v_seed;
in PREC vec3  v_worldPos;
in PREC float v_lodFade;   // 1.0=LOD0 full, 0.4=LOD1 dithered

layout(location=0) out PREC vec4 FragColor;
layout(location=1) out PREC vec4 GBuffer1;

uniform sampler2D    u_atlas;
uniform PREC vec4    u_terrainLightDir;

void main()
{
    // Bayer 4×4 ordered dither for LOD1 distance fade.
    // v_lodFade=1.0 → never discard (LOD0 full).
    // v_lodFade=0.3 → discard ~70% of pixels (LOD1 flat patch, sparse ground cover look).
    const float bayer[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    int bx = int(gl_FragCoord.x) & 3;
    int by = int(gl_FragCoord.y) & 3;
    if (v_lodFade < bayer[by * 4 + bx]) discard;

    // LOD bias -1.5 applied at bind site.
    vec4 col = texture(u_atlas, v_atlasUV);

    // Alpha test.
    if (col.a < 0.5) discard;

    // Lighting: raised ambient so shadows don't read as black blots.
    float NdotL = max(dot(vec3(0.0, 0.0, 1.0), u_terrainLightDir.xyz), 0.0);
    col.rgb *= 0.72 + 0.28 * NdotL;

    // Root lift.
    col.rgb += vec3(v_cardBottom * 0.10);

    // Terrain colormap tint deferred (no colormap sampler in v1 flush API).

    col.rgb = min(vec3(1.0), col.rgb);

    // Dim LOD1 surviving pixels — proxy for missing CSM shadow at mid range.
    // (Proper shadow requires shadow map samplers in this pass — deferred.)
    col.rgb *= (v_lodFade >= 1.0) ? 1.0 : 0.60;

    FragColor = vec4(col.rgb, 1.0);
    GBuffer1  = vec4(0.0);
}
