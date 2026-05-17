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
uniform PREC vec4 fog_color;
uniform PREC float time;          // seconds — used for water animation
uniform PREC vec4 cameraPos;      // water-v1: MC2 world-space camera (Fresnel)

// water-v1 baked style constants (compile-time; tune via shader hot-reload;
// promote to a UBO only at per-biome per spec Section 8 TODO(water-v2)).
const vec3  SHALLOW_COLOR      = vec3(0.22, 0.45, 0.38);
const vec3  DEEP_COLOR         = vec3(0.02, 0.08, 0.10);
const float ABSORPTION_DENSITY = 0.15;   // 1/world-units
const float SHORE_BLEND_DEPTH  = 3.0;    // world-units to full opacity
const float NORMAL_STRENGTH    = 0.30;
const float FRESNEL_F0         = 0.02;
const float SUN_INTENSITY      = 1.0;

void main(void)
{
    if (o_isWater == 1) {
        // ---- water-v1 stylized base layer ----
        PREC vec2 wuv = Texcoord * 6.2831853;
        PREC vec2 w = vec2(sin(wuv.y * 3.0 + time * 0.50) + sin(wuv.y * 1.2 + time * 0.355),
                           sin(wuv.x * 3.0 + time * 0.355) + sin(wuv.x * 1.2 + time * 0.50));
        PREC vec3 wN = normalize(vec3(w * NORMAL_STRENGTH, 1.0));

        PREC float t        = clamp(WaterThickness * ABSORPTION_DENSITY, 0.0, 1.0);
        PREC vec3  waterCol = mix(SHALLOW_COLOR, DEEP_COLOR, t);

        PREC float shore = smoothstep(0.0, SHORE_BLEND_DEPTH, WaterThickness);
        if (shore <= 0.0) discard;            // kill invisible land-quad overdraw

        PREC vec3  viewVec = cameraPos.xyz - WorldPos;
        PREC vec3  viewDir = viewVec / max(length(viewVec), 1e-4);  // guard: camera-at-surface => no NaN
        PREC float ct      = max(dot(wN, viewDir), 0.0);
        PREC float fres    = FRESNEL_F0 + (1.0 - FRESNEL_F0) * pow(1.0 - ct, 5.0);

        PREC vec3  reflCol = fog_color.rgb * 1.4;   // sky/fog approx (no reflection pass v1)

        PREC vec3  lightDir = normalize(vec3(0.3, 0.2, 1.0));  // existing FS constant light
        PREC vec3  halfV    = normalize(viewDir + lightDir);
        PREC float spec     = pow(max(dot(wN, halfV), 0.0), 64.0) * fres;

        PREC vec3  vertexLightRGB = Color.bgra.rgb;  // VS packs .bgra (~241); un-swizzle here
        PREC vec3  col = mix(waterCol, reflCol, fres);
        col *= vertexLightRGB;
        col += SUN_INTENSITY * spec;
        if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
            col = mix(fog_color.rgb, col, FogValue);

        FragColor = vec4(col, shore);
        GBuffer1  = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
        return;
    }

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
