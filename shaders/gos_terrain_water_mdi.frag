//#version 430 (provided by makeProgram prefix)
//
// MDI fragment shader for Task 1.5 (Stage 1 GPU-driven water).
// Based on gos_tex_vertex.frag with these changes:
//   - uniform int isWater removed; replaced by flat in int o_isWater from VS
//   - uniform sampler2D tex2 added (unit 1 = detail texture)
//   - tex_color selection: base layer uses tex1, detail layer uses tex2
//   - all isWater references in wave-animation block replaced with o_isWater
//
// Everything else (fog blend, GBuffer1, ALPHA_TEST ifdef, PREC, etc.) is
// identical to gos_tex_vertex.frag.

#define PREC highp

#include <include/render_contract.hglsl>

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
flat in int o_isWater;
in PREC float WaterThickness;     // water-v1: world-unit column from VS
in PREC vec3  WorldPos;           // water-v1: surface pos (Fresnel view vector)

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;
uniform sampler2D tex2;
uniform sampler2D reflTex;            // unit 2: whole-map colormap atlas
uniform int   reflectionOn;           // 0 -> skip entire S3 block
uniform float atlasMapTopLeftX;
uniform float atlasMapTopLeftY;
uniform float atlasOneOverWorldUnits;
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water animation
uniform PREC vec4 cameraPos;      // water-v1: MC2 world-space camera (Fresnel)
uniform PREC float alphaDepth;    // MapData::alphaDepth (world-units); shore smoothstep range
uniform int u_waterDebugMode;     // WATER-DEBUG-VIEWS-1: fragment/material-space debug.
                                  // 0=Final 1=Tint 2=Alpha 3=Normal 4=Depth 5=Shore 6=Lighting.
                                  // Distinct from VS geometry-space debugMode (MC2_RENDER_WATER_FASTPATH_DEBUG).

// water-v1 baked style constants (compile-time; tune via shader hot-reload;
// promote to a UBO only at per-biome per spec Section 8 TODO(water-v2)).
const vec3  SHALLOW_COLOR      = vec3(0.22, 0.45, 0.38);  // user-approved teal (keep)
const vec3  DEEP_COLOR         = vec3(0.03, 0.13, 0.20);  // dark blue, NOT black
const float ABSORPTION_DENSITY = 0.022;  // 1/world-units (Beer-Lambert k; ~45u e-fold over 0..150)
const float WATER_MAX_ALPHA    = 0.87;   // mild transparency: deep water never 100% opaque (lakebed shows through). 1.0 = old opaque slab; lower = more see-through. f(depth) only - camera-indep
const float SKY_AMBIENT        = 0.18;   // brightness floor (camera-independent)
// --- camera-INDEPENDENT procedural water detail (BAR-style: 2 fBm layers,
//     OPPOSITE scroll dirs -> organic churn, no grid). f(WorldPos,time) only. ---
const float WAVE_FREQ   = 0.030;   // 1/world-u; lower = bigger waves, visible at zoom-out
const float WAVE_SPEED  = 6.0;     // world-u/sec domain scroll
const float RIPPLE_GAIN  = 0.22;   // crest BRIGHTEN amount - mild, low color variance
const vec3  GLINT_TINT   = vec3(0.82, 0.88, 0.94);  // near-WHITE wave-cap (slightly cool)
const float GLINT_GAIN   = 0.30;   // additive camera-INDEPENDENT white crest shimmer (more white on surface per user)
const float GLINT_THRESH = 0.36;   // a bit more crest area shows white caps
const float WAVE_FADE_NEAR = 9000.0;  // full detail well out (visible at zoom-out now)
const float WAVE_FADE_FAR  = 40000.0; // only the very furthest extreme calms (no flat-at-zoom)

// S3 reflection DISABLED (user 2026-05-17): any perceptible camera-dependence
// in the water was rejected. A reflection is inherently camera-dependent, so
// it cannot satisfy that - shelved. Compile-time false => the whole S3 block
// below is dead-stripped (water is provably 100% camera-independent again;
// zero perf). Scaffolding (uniforms / C++ bind / probe) retained dormant for
// the deferred Option-B path (see spec). Flip to true only to re-experiment.
const bool  S3_REFLECTION_ENABLED = false;
const int   REFL_STEPS      = 5;
const float REFL_STEP_LEN   = 96.0;   // ~one terrain-tile world distance
const float REFL_F0         = 0.02;
const float REFL_STRENGTH   = 0.35;
const float REFL_MAX        = 0.22;   // hard ceiling on the mix factor
const float REFL_WAVE_SLOPE = 0.05;   // nz-gradient -> perturbation slope (range 0.02-0.10)

// Precision-safe (fract-early) hash -> stable for large MC2 world coords;
// value-noise + 3-octave fBm. No texture, no seam (continuous WorldPos).
PREC float h21(PREC vec2 ip){
    PREC vec3 q = fract(vec3(ip.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
PREC float vnoise(PREC vec2 uv){
    PREC vec2 i = floor(uv), f = fract(uv);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h21(i),               h21(i + vec2(1.0, 0.0)), f.x),
               mix(h21(i + vec2(0.0,1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y);
}
PREC float fbm3(PREC vec2 uv){
    PREC float s = 0.0, a = 0.5;
    for (int k = 0; k < 3; ++k){ s += a * vnoise(uv); uv = uv * 2.03 + 11.7; a *= 0.5; }
    return s;   // ~0 .. 0.875, mean ~0.4375
}

void main(void)
{
    if (o_isWater == 1) {
        // ---- water-v1 stylized base layer ----
        // viewVec used ONLY for distance (waveLOD anti-alias) - NOT angle.
        PREC vec3  viewVec = cameraPos.xyz - WorldPos;
        PREC float waveLOD = 1.0 - smoothstep(WAVE_FADE_NEAR, WAVE_FADE_FAR, length(viewVec));

        // BAR-style: TWO fBm layers scrolling in OPPOSITE directions -> organic
        // churn, no grid/lattice. Continuous WorldPos = seam-free; fract-early
        // hash = precision-safe at large MC2 coords. Camera-INDEPENDENT.
        PREC float sc = time * WAVE_SPEED * WAVE_FREQ;
        PREC vec2  q0 = WorldPos.xy * WAVE_FREQ        + vec2( 1.00,  0.60) * sc;
        PREC vec2  q1 = WorldPos.xy * WAVE_FREQ * 1.70 + vec2(-0.80, -1.10) * sc;
        PREC float nz = (fbm3(q0) + fbm3(q1)) - 0.875;   // ~zero-mean organic detail

        PREC float trans    = clamp(exp(-WaterThickness * ABSORPTION_DENSITY), 0.0, 1.0);  // clamp handles negative WaterThickness (above-water shore tiles)
        PREC vec3  waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans);

        // alphaDepth is MapData::alphaDepth (world-units); guard against 0.
        // Shore-extension fix: extend blend into above-water tiles (negative
        // WaterThickness) so the water tint creeps onto the beach, matching
        // the original CPU alpha-band behavior (alphaEdge on above-water verts).
        PREC float shoreBlend = max(alphaDepth, 1.0);
        PREC float shore = smoothstep(-shoreBlend * 0.5, shoreBlend, WaterThickness);
        if (shore <= 0.0) discard;            // kill tiles too far above waterline

        PREC vec3  vertexLightRGB = Color.bgra.rgb;  // VS packs .bgra; un-swizzle (camera-indep)
        PREC vec3  col = waterCol * max(vertexLightRGB, vec3(SKY_AMBIENT))
                       + waterCol * SKY_AMBIENT * 0.5;
        // Camera-INDEPENDENT granular wave detail (dual counter-scroll fBm).
        // BRIGHTEN-only (no darkening) + sharpened reflective crest shimmer ->
        // reads as light catching the ripples. No Fresnel/specular: MC2 has no
        // sun "for now"; the ONLY camera-dependent term will be S3 terrain
        // planar reflection (deferred).
        PREC float crest = max(nz, 0.0);                       // troughs stay at base
        col *= 1.0 + RIPPLE_GAIN * waveLOD * crest;            // brighten only
        PREC float glint = smoothstep(GLINT_THRESH, 0.80, nz); // sharp crest sparkle
        col += glint * GLINT_GAIN * waveLOD * GLINT_TINT;      // camera-INDEPENDENT shimmer
        if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
            col = mix(fog_color.rgb, col, FogValue);

        // S3: pure-FS reflected-ray terrain-colormap reflection.
        // The ONLY camera-dependent term in the water material (v2 ruling).
        if (S3_REFLECTION_ENABLED && reflectionOn == 1) {
            // S1 has no normal in this branch (scalar fBm). Derive the perturbation
            // from the screen-space gradient of the in-scope scalar fBm nz (~:84,
            // f(WorldPos,time)). clamp() prevents zoom-out over-distortion. This is
            // the GROUNDING-authoritative construction (supersedes wave1/wave2).
            PREC vec2  nzGrad     = clamp(vec2(dFdx(nz), dFdy(nz)), -2.0, 2.0);
            PREC vec3  waveNormal = normalize(vec3(nzGrad * REFL_WAVE_SLOPE, 1.0));
            PREC vec3  vdir       = normalize(cameraPos.xyz - WorldPos); // sole cam-dep input
            PREC vec3  rdir       = reflect(-vdir, waveNormal);
            PREC vec3  acc  = vec3(0.0);
            PREC float wsum = 0.0;
            for (int i = 1; i <= REFL_STEPS; ++i) {
                PREC vec2 wp = WorldPos.xy + rdir.xy * (float(i) * REFL_STEP_LEN);
                PREC vec2 uv;
                uv.x = (wp.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;  // X: not flipped
                uv.y = (atlasMapTopLeftY - wp.y) * atlasOneOverWorldUnits;  // Y: inverted
                PREC float inb = step(0.0, uv.x) * step(uv.x, 1.0)
                          * step(0.0, uv.y) * step(uv.y, 1.0);
                acc  += inb * texture(reflTex, uv).rgb;
                wsum += inb;
            }
            PREC vec3  refl = (wsum > 0.0) ? acc / wsum : col;        // all off-map -> no-op
            // flat-water normal is +Z (matches the GBuffer1 screenShadowEligible(vec3(0,0,1)) below); vdir.z = N.V
            PREC float fres = REFL_F0 + (1.0 - REFL_F0)
                         * pow(1.0 - max(vdir.z, 0.0), 5.0);
            col = mix(col, refl,
                      clamp(fres * REFL_STRENGTH * waveLOD, 0.0, REFL_MAX));
        }

        // WATER-DEBUG-VIEWS-1: fragment/material-space debug visualizations.
        // mode 0 (Final) leaves the path byte-identical; each non-zero mode is
        // backed by a real water-v1 term computed above, output opaque so it
        // reads clearly over terrain. No reflection mode (S3 dead-stripped).
        if (u_waterDebugMode != 0) {
            PREC vec3 dbg;
            if      (u_waterDebugMode == 1) dbg = waterCol;                       // 1 Tint: DEEP<->SHALLOW mix, pre-ripple
            else if (u_waterDebugMode == 2) dbg = vec3(shore * WATER_MAX_ALPHA);  // 2 Alpha: final alpha as grayscale
            else if (u_waterDebugMode == 3) dbg = vec3(0.5, 0.5, 1.0);            // 3 Normal: flat-up only (no real surface normal)
            else if (u_waterDebugMode == 4) dbg = vec3(trans);                    // 4 Depth: Beer-Lambert transmittance (1=shallow,0=deep)
            else if (u_waterDebugMode == 5) dbg = vec3(shore);                    // 5 Shore: shoreline ramp mask
            else if (u_waterDebugMode == 6) dbg = vec3(RIPPLE_GAIN * waveLOD * crest)   // 6 Lighting: ripple brighten
                                                + glint * GLINT_GAIN * waveLOD * GLINT_TINT;  //            + crest glint
            else                            dbg = vec3(1.0, 0.0, 1.0);            // unknown -> magenta sentinel
            FragColor = vec4(dbg, 1.0);
            GBuffer1  = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
            return;
        }

        FragColor = vec4(col, shore * WATER_MAX_ALPHA);  // shore ramp preserved; capped so deep water is mildly transparent
        GBuffer1  = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
        return;
    }

    // water-v1: suppress the legacy detail/spray layer (o_isWater==2). Its tiled
    // tex2 + UV-wrap "loop catch" looks bad against the new base; proper animated
    // detail is deferred to water-v2. Verbatim legacy path kept (dead) below.
    if (o_isWater == 2) discard;

    PREC vec4 c = Color.bgra;
    PREC vec4 tex_color = (o_isWater <= 1) ? texture(tex1, Texcoord) : texture(tex2, Texcoord);
    c *= tex_color;

#ifdef ALPHA_TEST
    if(tex_color.a < 0.5)
        discard;
#endif
    // Animated water
    if (o_isWater > 0) {
        PREC vec2 wuv = Texcoord * 6.2831853;  // 2*PI so sin tiles at integer UVs

        if (o_isWater == 1) {
            // Base water — moderate speed, slightly more visible
            PREC float wave1 = sin(wuv.x * 3.0 + wuv.y * 2.0 + time * 0.4)
                             + sin(wuv.x * 2.0 - wuv.y * 3.0 + time * 0.3);
            PREC float wave2 = sin(wuv.x * 5.0 + wuv.y * 4.0 - time * 0.5)
                             + sin(wuv.x * 4.0 - wuv.y * 5.0 - time * 0.2);
            wave1 *= 0.5;
            wave2 *= 0.5;

            PREC float waveBrightness = 1.0 + wave1 * 0.025 + wave2 * 0.015;
            c.rgb *= waveBrightness;
        } else {
            // Detail/spray layer — very slow, gentle undulation
            PREC float wave1 = sin(wuv.x * 3.0 + wuv.y * 2.0 + time * 0.12)
                             + sin(wuv.x * 2.0 - wuv.y * 3.0 + time * 0.08);
            PREC float wave2 = sin(wuv.x * 5.0 + wuv.y * 4.0 - time * 0.15)
                             + sin(wuv.x * 4.0 - wuv.y * 5.0 - time * 0.06);
            wave1 *= 0.5;
            wave2 *= 0.5;

            PREC float waveBrightness = 1.0 + wave1 * 0.015 + wave2 * 0.01;
            c.rgb *= waveBrightness;

            // Faint specular glint on detail layer
            PREC vec3 waveNormal = normalize(vec3(wave1 * 0.06, wave2 * 0.06, 1.0));
            PREC vec3 lightDir = normalize(vec3(0.3, 0.2, 1.0));
            PREC float spec = pow(max(dot(reflect(-lightDir, waveNormal), vec3(0.0, 0.0, 1.0)), 0.0), 96.0);
            c.rgb += vec3(spec * 0.08);
        }
    }

	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
	FragColor = c;

	// F3 Option A: flat-up fallback (compatibility — no surface normal available).
	GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
}
