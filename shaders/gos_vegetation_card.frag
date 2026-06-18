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
in PREC float v_tilt;      // 0.0=vertical, 0.5=tilted, 1.0=top/cap
in PREC float v_distFade;  // 1.0=near, 0.0=at/beyond max dist

layout(location=0) out PREC vec4 FragColor;
layout(location=1) out PREC vec4 GBuffer1;

uniform sampler2D    u_atlas;
uniform PREC vec4    u_terrainLightDir;

void main()
{
    // Unified Bayer gate: gate = lodFade * distFade.
    //   LOD0 near  (gate=1.0): never discard — full density.
    //   LOD1 near  (gate=0.4): discard ~60% → sparser beyond block LOD boundary.
    //   LOD0 far   (gate<1.0): progressive discard → smooth fade to invisible.
    //   LOD1 far   (gate~0.2): very sparse — thin out then gone.
    // Shimmer note: screen-space Bayer shifts with camera motion. Accepted for v1;
    // instance-stable hash dithering is planned for v2.
    float gate = v_lodFade * v_distFade;
    const float bayer[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    int bx = int(gl_FragCoord.x) & 3;
    int by = int(gl_FragCoord.y) & 3;
    if (gate < bayer[by * 4 + bx]) discard;

    // LOD bias -1.5 applied at bind site.
    vec4 col = texture(u_atlas, v_atlasUV);

    // Alpha test: top/cap cards use softer threshold (0.25) for wispy edges.
    float alphaThresh = mix(0.50, 0.25, v_tilt);
    if (col.a < alphaThresh) discard;

    // Desaturate to reduce uniform vivid-green look.
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    col.rgb = mix(col.rgb, vec3(lum), 0.30);

    // Per-instance warmth variation: some cards yellower, some cooler.
    col.r += (v_seed - 0.5) * 0.07;
    col.b -= v_seed * 0.05;

    // Lighting. Top/cap cards are slightly darker: horizontal face = less direct sun.
    float NdotL = max(dot(vec3(0.0, 0.0, 1.0), u_terrainLightDir.xyz), 0.0);
    float topDim = mix(1.0, 0.80, v_tilt);
    col.rgb *= (0.60 + 0.38 * NdotL) * topDim;

    // Root lift.
    col.rgb += vec3(v_cardBottom * 0.08);

    col.rgb = min(vec3(1.0), col.rgb);

    FragColor = vec4(col.rgb, 1.0);
    // Sets GBuffer1.a > 0.5 so post-screen-shadow pass skips vegetation.
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
}