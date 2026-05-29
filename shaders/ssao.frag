//#version 430 (version provided by prefix)
//
// SSAO-GTAO-LITE-MVP-1 (Track V). Lightweight half-res screen-space ambient
// occlusion. World-space hemisphere AO that REUSES the scene depth + GBuffer1
// world normals + inverseViewProj already produced for the screen-shadow pass
// (shaders/shadow_screen.frag), so there is no GBuffer rewrite.
//
// Occlusion test is window-depth-ordering based with a world-distance range
// check, so it needs NO camera-position uniform (sidesteps the Stuff->MC2
// axis-swap hazard). Convention matches shadow_screen.frag: depth >= 1.0 is
// sky / no-geometry (AO = 1, no darkening); larger window depth = farther.
//
// Output: single grayscale AO factor in [0,1] (1 = unoccluded). Applied
// multiplicatively to the scene by ssao_apply.frag.

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D sceneDepthTex;    // unit 0: full-res scene depth
uniform sampler2D sceneNormalTex;   // unit 1: GBuffer1 (rgb = encoded world normal)
uniform mat4 inverseViewProj;       // NDC -> world (D3D-style, ZERO_TO_ONE)
uniform mat4 viewProj;              // world -> clip (matches inverseViewProj)
uniform vec2 screenSize;            // full-res width,height
uniform float aoRadius;             // world-units sample radius
uniform float aoBias;               // depth-compare bias (window-depth units)
uniform float aoStrength;           // 0 = none .. 1+ = strong
uniform float aoPower;              // contrast curve on the final AO

// 16-sample cosine-ish hemisphere kernel (z > 0). Fixed (deterministic).
const vec3 kKernel[16] = vec3[](
    vec3( 0.024, -0.012,  0.040), vec3(-0.045,  0.039,  0.052),
    vec3( 0.071,  0.061,  0.090), vec3(-0.083, -0.052,  0.071),
    vec3( 0.012,  0.114,  0.140), vec3( 0.131, -0.078,  0.121),
    vec3(-0.151,  0.026,  0.180), vec3( 0.067,  0.175,  0.205),
    vec3(-0.092, -0.196,  0.171), vec3( 0.226,  0.041,  0.241),
    vec3(-0.198,  0.158,  0.276), vec3( 0.046, -0.281,  0.252),
    vec3( 0.258,  0.182,  0.330), vec3(-0.314, -0.072,  0.301),
    vec3( 0.121,  0.341,  0.388), vec3(-0.279,  0.252,  0.421)
);

vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    float ndc_z = depth;  // ZERO_TO_ONE: window depth == NDC z
    vec4 w = inverseViewProj * vec4(ndc_xy, ndc_z, 1.0);
    return w.xyz / w.w;
}

float hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
    float depth = texture(sceneDepthTex, TexCoord).r;

    // Sky / no geometry: never darken (matches shadow_screen.frag sky guard).
    if (depth >= 1.0) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 P = reconstructWorldPos(TexCoord, depth);
    vec3 N = normalize(texture(sceneNormalTex, TexCoord).xyz * 2.0 - 1.0);

    // Per-pixel random rotation for the kernel (TBN basis).
    float rnd = hash12(TexCoord * screenSize);
    float ca = cos(rnd * 6.2831853);
    float sa = sin(rnd * 6.2831853);
    vec3 randomVec = normalize(vec3(ca, sa, 0.5));
    vec3 tangent   = normalize(randomVec - N * dot(randomVec, N));
    vec3 bitangent = cross(N, tangent);
    mat3 TBN = mat3(tangent, bitangent, N);

    float occlusion = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec3 sampleWorld = P + (TBN * kKernel[i]) * aoRadius;

        // Project the sample to screen.
        vec4 clip = viewProj * vec4(sampleWorld, 1.0);
        if (clip.w <= 0.0) continue;
        vec3 ndc = clip.xyz / clip.w;
        vec2 sUV = ndc.xy * 0.5 + 0.5;
        if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) continue;

        float sampleProjDepth = ndc.z;                 // [0,1] ZERO_TO_ONE
        float storedDepth = texture(sceneDepthTex, sUV).r;
        if (storedDepth >= 1.0) continue;              // sky behind sample

        // Stored surface NEARER the eye than the sample point (larger depth =
        // farther, so smaller stored depth = nearer = occluder).
        bool occluded = storedDepth < sampleProjDepth - aoBias;

        // World-distance range check: only count nearby occluders so distant
        // geometry does not bleed AO (halo suppression).
        vec3 storedWorld = reconstructWorldPos(sUV, storedDepth);
        float dist = length(storedWorld - P);
        float rangeCheck = smoothstep(0.0, 1.0, aoRadius / max(dist, 1e-4));

        occlusion += (occluded ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - (occlusion / 16.0) * aoStrength;
    ao = pow(clamp(ao, 0.0, 1.0), max(aoPower, 0.0001));
    FragColor = vec4(ao, ao, ao, 1.0);
}
