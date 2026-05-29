//#version 420 (version provided by prefix)

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D sceneTex;   // unit 0: scene
uniform sampler2D bloomTex;   // unit 1: blurred bloom
uniform float exposure;       // exposure multiplier (default 1.0)
uniform int enableBloom;      // 0 = off, 1 = on
uniform int enableFXAA;       // 0 = off, 1 = on
uniform int enableTonemap;    // 0 = off (passthrough), 1 = on
uniform float bloomIntensity; // bloom mix strength
uniform vec2 inverseScreenSize; // 1/width, 1/height

// VIEWMODE-POSTPROCESS-PRESENTATION-1: presentation-mode uniforms.
// u_viewMode: 0=Visual (default, byte-identical), 1=ObjectIdDebug.
// u_objectIdTex: GL_R32UI object-ID buffer (usampler2D, not sampler2D).
// Both are only read when MC2_VIEWMODE_FRAMEWORK is active; u_viewMode
// defaults to 0 so the Visual path is never disturbed when the gate is OFF.
uniform int u_viewMode;                // 0=Visual, 1=ObjectIdDebug, 3=Thermal, 5=LowLight
uniform usampler2D u_objectIdTex;     // unit 2: sceneObjectIdTex_ (GL_R32UI)
// GAMEADAPTERS-VISUAL-STATE-BRIDGE: lowest object-ID-buffer index that is a
// mech (engine-bearing). Pixels with (objectId & 0xFFFFF) >= this value are
// mechs and read hot in Thermal. 0 = OID buffer unavailable -> luminance only.
uniform int u_engineIdxBase;

// LOWLIGHT-NIGHTVISION-MVP-1: night-vision tunables (read only when
// u_viewMode == 5). Always set by the composite each frame.
uniform float u_lowLightGain;  // luminance amplification (default 2.5)
uniform vec3  u_lowLightTint;  // green-phosphor tint (default 0.7,1.0,0.6)

// ACES Filmic tonemapping (Krzysztof Narkowicz fit)
// Note: designed for linear HDR input. Our pipeline is sRGB so this acts
// as a gentle contrast/color curve rather than true HDR compression.
vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Tonemap a single sample (used by FXAA neighbor reads)
vec3 tonemapSample(vec3 color)
{
    // TONEMAP-ACES-MVP-1 tuned 2026-05-29: trim the ACES input ~10% so the
    // filmic result reads slightly less hot. Tonemap branch ONLY -> the
    // non-tonemap (HDR-off / default) path is unchanged (byte-identical).
    if (enableTonemap == 1)
        return ACESFilm(color * exposure * 0.9);
    return color * exposure;
}

// FXAA 3.11 simplified (Timothy Lottes algorithm)
vec3 applyFXAA_LDR(vec2 uv, vec2 invScreenSize)
{
    float FXAA_SPAN_MAX = 8.0;
    float FXAA_REDUCE_MUL = 1.0 / 8.0;
    float FXAA_REDUCE_MIN = 1.0 / 128.0;

    vec3 rgbNW = tonemapSample(texture(sceneTex, uv + vec2(-1.0, -1.0) * invScreenSize).rgb);
    vec3 rgbNE = tonemapSample(texture(sceneTex, uv + vec2( 1.0, -1.0) * invScreenSize).rgb);
    vec3 rgbSW = tonemapSample(texture(sceneTex, uv + vec2(-1.0,  1.0) * invScreenSize).rgb);
    vec3 rgbSE = tonemapSample(texture(sceneTex, uv + vec2( 1.0,  1.0) * invScreenSize).rgb);
    vec3 rgbM  = tonemapSample(texture(sceneTex, uv).rgb);

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * FXAA_REDUCE_MUL, FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * invScreenSize;

    vec3 rgbA = 0.5 * (
        tonemapSample(texture(sceneTex, uv + dir * (1.0/3.0 - 0.5)).rgb) +
        tonemapSample(texture(sceneTex, uv + dir * (2.0/3.0 - 0.5)).rgb));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        tonemapSample(texture(sceneTex, uv + dir * -0.5).rgb) +
        tonemapSample(texture(sceneTex, uv + dir *  0.5).rgb));

    float lumaB = dot(rgbB, luma);
    if (lumaB < lumaMin || lumaB > lumaMax)
        return rgbA;
    else
        return rgbB;
}

void main()
{
    vec3 color;

    // FXAA first — operates on scene texture neighbors for edge detection
    if (enableFXAA == 1) {
        color = applyFXAA_LDR(TexCoord, inverseScreenSize);
    } else {
        // Tonemapping (no gamma — pipeline is already sRGB)
        color = tonemapSample(texture(sceneTex, TexCoord).rgb);
    }

    // Add bloom AFTER FXAA (bloom is soft glow, doesn't need AA)
    if (enableBloom == 1) {
        vec3 bloom = texture(bloomTex, TexCoord).rgb;
        color += bloom * bloomIntensity;
    }

    // --- Sunset filter (subtle) ---
    // Light warm grade + gentle vignette + soft top-of-screen warmth.
    {
        // Gentle unconditional warm push.
        color *= vec3(1.05, 1.01, 0.94);

        // Luminance-based highlight warming / shadow cooling.
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        vec3 warm = vec3(1.05, 1.00, 0.93);
        vec3 cool = vec3(0.95, 0.98, 1.04);
        vec3 grade = mix(cool, warm, smoothstep(0.10, 0.75, lum));
        color *= grade;

        // Subtle radial vignette.
        vec2 vdir = TexCoord - vec2(0.5);
        float vdist = length(vdir * vec2(1.0, 0.6));
        float vignette = smoothstep(0.85, 0.25, vdist);
        color *= mix(0.82, 1.0, vignette);

        // Soft top-of-screen warm glow.
        float sunBias = pow(smoothstep(0.0, 1.0, 1.0 - TexCoord.y), 1.5);
        color = mix(color, color * vec3(1.10, 1.03, 0.92), sunBias * 0.25);
    }

    FragColor = vec4(color, 1.0);

    // VIEWMODE-POSTPROCESS-PRESENTATION-1: ObjectIdDebug overlay.
    // Overrides FragColor when u_viewMode == 1. All other values fall through
    // to the Visual path above (byte-identical). No sampling of u_objectIdTex
    // occurs in the Visual path — the sampler binding is safe to leave unread.
    if (u_viewMode == 1) {
        uint id = texture(u_objectIdTex, TexCoord).r;
        if (id == 0u) {
            // No object at this pixel — dark grey background
            FragColor = vec4(0.1, 0.1, 0.1, 1.0);
        } else {
            // Hash id to a stable RGB color per object
            float r = float((id       ) & 0xFFu) / 255.0;
            float g = float((id >>  8u) & 0xFFu) / 255.0;
            float b = float((id >> 16u) & 0xFFu) / 255.0;
            FragColor = vec4(r, g, b, 1.0);
        }
    }
    // THERMAL-VIEW-MECH-HOT-1: engine-bearing units (mechs) read HOT, the rest
    // of the scene maps Visual luminance to an iron palette (emissive/bright
    // regions — fire, exhaust, muzzle flash, specular — still read warm; dark
    // terrain reads cool). "Engine = hot" classification uses the object-ID
    // buffer: mech handles occupy index >= u_engineIdxBase (kMechHandleBase),
    // static props/terrain are below it. Real per-unit heat does not exist
    // (USEHEAT compiled out); vehicles render via the static-prop batcher and
    // are not yet distinguishable (vehicles-hot is a documented follow-up).
    // u_engineIdxBase == 0 means the OID buffer is unavailable -> luminance
    // only, identical to the prior placeholder.
    else if (u_viewMode == 3) {
        float t = clamp(dot(color, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        if (u_engineIdxBase > 0) {
            uint idx = texture(u_objectIdTex, TexCoord).r & 0xFFFFFu;
            if (idx >= uint(u_engineIdxBase))
                t = max(t, 0.9);  // force mech pixels into the hot band
        }
        vec3 c0 = vec3(0.00, 0.00, 0.10);
        vec3 c1 = vec3(0.30, 0.00, 0.40);
        vec3 c2 = vec3(0.85, 0.10, 0.05);
        vec3 c3 = vec3(1.00, 0.55, 0.00);
        vec3 c4 = vec3(1.00, 0.95, 0.40);
        vec3 c5 = vec3(1.00, 1.00, 1.00);
        vec3 heat;
        if      (t < 0.2) heat = mix(c0, c1, t / 0.2);
        else if (t < 0.4) heat = mix(c1, c2, (t - 0.2) / 0.2);
        else if (t < 0.6) heat = mix(c2, c3, (t - 0.4) / 0.2);
        else if (t < 0.8) heat = mix(c3, c4, (t - 0.6) / 0.2);
        else              heat = mix(c4, c5, (t - 0.8) / 0.2);
        FragColor = vec4(heat, 1.0);
    }
    // LOWLIGHT-NIGHTVISION-MVP-1: amplify the dark-adapted Visual luminance and
    // tint toward green phosphor with a stronger NV-tube vignette. Operates on
    // the graded Visual color, so it works regardless of HDR/tonemap state.
    else if (u_viewMode == 5) {
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        float boosted = lum * u_lowLightGain;
        boosted = boosted / (1.0 + boosted * 0.3); // soft-knee; preserve headroom
        vec3 nv = boosted * u_lowLightTint;
        vec2 vd = TexCoord - vec2(0.5);
        float vdist = length(vd * vec2(1.0, 0.75));
        float nvVignette = smoothstep(0.70, 0.20, vdist);
        nv *= mix(0.65, 1.0, nvVignette);
        FragColor = vec4(nv, 1.0);
    }
}
