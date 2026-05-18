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

// water-v1 baked style constants (compile-time; tune via shader hot-reload;
// promote to a UBO only at per-biome per spec Section 8 TODO(water-v2)).
const vec3  SHALLOW_COLOR      = vec3(0.22, 0.45, 0.38);  // user-approved teal (keep)
const vec3  DEEP_COLOR         = vec3(0.03, 0.13, 0.20);  // dark blue, NOT black
const float ABSORPTION_DENSITY = 0.022;  // 1/world-units (Beer-Lambert k; ~45u e-fold over 0..150)
const float SHORE_BLEND_DEPTH  = 7.0;    // world-units to full opacity (widened 3->7 for a SOFTER edge; tune by eye - larger = broader shallow fade)
const float WATER_MAX_ALPHA    = 0.87;   // mild transparency: deep water never 100% opaque (lakebed shows through). 1.0 = old opaque slab; lower = more see-through. f(depth) only - camera-indep
const float SKY_AMBIENT        = 0.18;   // brightness floor (camera-independent)
// --- BAR-style procedural shoreline: foam/surf band + soft wavy edge.
//     Camera-INDEPENDENT: f(WaterThickness, nz=f(WorldPos,time), distance).
//     Pure-FS, hot-reloadable; all values tune-by-eye. ---
const float FOAM_DEPTH_MIN   = 0.0;   // world-u: full foam at/under this depth
const float FOAM_DEPTH_MAX   = 8.0;   // world-u: foam gone beyond this depth (surf-band width; 6->8 for visibility)
const float FOAM_NOISE_AMP   = 2.5;   // world-u: nz perturbs the band edge -> irregular + moving (not a clean ring)
const float FOAM_TEX_VAR     = 0.45;  // nz texture variation, ALWAYS-ON (foam = band*((1-VAR)+VAR*nz01)). Replaces the old smoothstep "break" gate which zeroed foam (nz is ~zero-mean -> sat in the dead band)
const float FOAM_INTENSITY   = 1.00;  // white blend strength at waterline (BOLD first pass; dial DOWN by eye if too much)
const vec3  FOAM_COLOR       = vec3(0.90, 0.95, 0.96);  // near-white, faint cool blue-green surf
const float FOAM_ALPHA_BOOST = 0.90;  // foam is NOT see-through: near-opaque surf
const float SHORE_NOISE_AMP  = 5.0;   // world-u: nz perturbs the kill/feather boundary -> wavy coastline (1.5 was too subtle; 5 = pronounced)
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

        PREC float trans    = exp(-WaterThickness * ABSORPTION_DENSITY);  // 1 at shore -> 0 deep
        PREC vec3  waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans);

        // Soft + WAVY edge: perturb the feather/kill boundary by the existing
        // nz fBm so the waterline is an irregular coastline, not the faceted
        // water-quad polygon silhouette. Camera-INDEPENDENT (nz=f(WorldPos,time)).
        PREC float wtShore = WaterThickness + nz * SHORE_NOISE_AMP;
        PREC float shore = smoothstep(0.0, SHORE_BLEND_DEPTH, wtShore);
        if (shore <= 0.0) discard;            // kill land-quad overdraw (now wavy, not pointy)

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

        // BAR-style procedural foam/surf band (camera-INDEPENDENT). Foam fills
        // a shallow depth band whose edge is perturbed AND animated by the
        // existing nz fBm (-> irregular, gently moving surf, not a clean ring);
        // nz also breaks it into froth patches; waveLOD distance-fades it so it
        // does not alias when zoomed out. Foam is opaque-ish (surf is froth,
        // not see-through) so it adds to alpha.
        PREC float foamBand  = smoothstep(FOAM_DEPTH_MAX, FOAM_DEPTH_MIN,
                                          WaterThickness + nz * FOAM_NOISE_AMP);
        // Always-on gentle nz texture (NOT a hard gate): foam is present
        // across the whole shallow band, nz only varies its intensity ->
        // organic surf, never zeroed. (The prior smoothstep "break" sat in
        // nz's ~zero-mean dead band and killed foam entirely.)
        PREC float foamTex = (1.0 - FOAM_TEX_VAR) + FOAM_TEX_VAR * (nz * 0.5 + 0.5);
        PREC float foam = clamp(foamBand * foamTex * FOAM_INTENSITY * waveLOD,
                                0.0, 1.0);
        col = mix(col, FOAM_COLOR, foam);
        PREC float wAlpha = clamp(shore * WATER_MAX_ALPHA
                                  + foam * FOAM_ALPHA_BOOST, 0.0, 1.0);
        FragColor = vec4(col, wAlpha);  // shore ramp + foam; deep water still mildly transparent
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
