// OOB-FOG-7: sea-of-clouds at the map horizon.
//
// Uses 3D FBM sampled directly on the unit sphere (worldDir) — no 2D UV
// projection, no seams, no directional bias, no diagonal streaks.
// Solid cloud base with gentle spherical FBM surface variation.
//
// MC2 / invWorldToClipGL frame: worldDir.z < 0 = sky up, worldDir.z > 0 = down.
//
// Gate:  MC2_OOB_FOG (default ON, "0" to disable).
// Color: MC2_OOB_FOG_COLOR=r,g,b

in vec2 TexCoord;
layout(location = 0) out vec4 outFog;

uniform sampler2D depthTex;
uniform mat4      invViewProj;
uniform vec3      u_fogColor;
uniform float     u_fogOpacity;
uniform float     u_time;

// ---- 3D value noise ----
// Sampling worldDir (unit sphere) in 3D eliminates all UV projection
// artifacts: no seams, no directional tilt, no vertical streaks.

float hash31(vec3 p) {
    p = fract(p * vec3(127.1, 311.7, 74.7));
    return fract(sin(dot(p, vec3(269.5, 183.3, 246.1))) * 43758.5453);
}

float vnoise3(vec3 p) {
    vec3 i = floor(p); vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash31(i),              hash31(i+vec3(1,0,0)), f.x),
            mix(hash31(i+vec3(0,1,0)),  hash31(i+vec3(1,1,0)), f.x), f.y),
        mix(mix(hash31(i+vec3(0,0,1)),  hash31(i+vec3(1,0,1)), f.x),
            mix(hash31(i+vec3(0,1,1)),  hash31(i+vec3(1,1,1)), f.x), f.y),
        f.z);
}

float fbm3D(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += vnoise3(p) * a;
        p  = p * 2.1 + vec3(1.7, 0.9, 1.4);
        a *= 0.5;
    }
    return v;
}

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;
    if (rawDepth > 0.0001) { outFog = vec4(0.0); return; }

    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 worldDir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);

    // worldDir.z < 0 = sky. Clouds at horizon (z~0) and fill the void below.
    if (worldDir.z < -0.22) { outFog = vec4(0.0); return; }

    // Fade in from just-above-horizon. No fade-out below — fill the full void.
    float skyFade = smoothstep(-0.22, -0.01, worldDir.z);

    // Slow horizontal drift for cloud animation. Shift worldDir in XY plane.
    vec3 p3 = worldDir * 4.5 + vec3(u_time * 0.008, u_time * 0.002, 0.0);

    // 3D FBM on the sphere — naturally isotropic, no UV artifacts.
    float n = fbm3D(p3);

    // Solid cloud base + gentle surface variation from noise.
    float base   = 0.86;
    float ripple = (n - 0.50) * 0.28;
    float alpha  = clamp((base + ripple) * skyFade * u_fogOpacity, 0.0, 1.0);

    // Sunlit tops bright, shadowed hollows slightly grey.
    float lit = smoothstep(0.38, 0.68, n);
    vec3  col = mix(u_fogColor * 0.78, u_fogColor * 1.05, lit);

    outFog = vec4(clamp(col, 0.0, 1.0), alpha);
}
