//#version 430 (provided by makeProgram prefix)
//
// GPU particle billboard FS. Per plan v5 §5.4 B1 Stage 1' Commit 3.
//
// Per memory/amd_auto_lod_strict_fail.md + gpu_direct_renderer_bringup_checklist.md
// trap #10: use textureLod (NOT texture). AMD RX 7900 XTX returns black from
// texture(sampler, varyingUV) when the sampled texture has an incomplete mip
// pyramid — even with MIN_FILTER=LINEAR and MAX_LEVEL=0. textureLod with an
// explicit LOD value bypasses the auto-LOD selector and is the standard fix
// used by mech.frag (modes 7/8/0) and the static-prop batcher path.
//
// Blend state (memory/blend_state_inheritance_in_post_process.md): the bridge
// sets gos_Alpha_AlphaBlend (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) explicitly
// and restores. This FS premultiplies alpha into the color so additive use
// later (Stage 2') is a one-line state change at the bridge.

uniform sampler2D uAtlas;

// VFX-DEBUG-VIEWS-1: particle debug visualization selector. Uploaded by
// gos_particle_bridge per flush from MC2_VFX_DEBUG_MODE (default 0). Mode 0 is
// byte-identical to the pre-slice output. All modes preserve the colorkey +
// alpha discards so debug views show exactly the fragments that actually draw.
//   0 = Final        (tex * v_color, head-brighten)   — default, unchanged
//   1 = Albedo       (raw atlas texel rgb, no tint)
//   2 = Alpha        (final alpha as grayscale)
//   3 = ParticleKind (distinct color per kind_flags kind)
//   4 = Overdraw     (constant additive proxy for blend buildup)
//   5 = Age          (heat-map: blue=newborn, green=mid-life, red=dying)
//                    VFX-SHADER-AGE-FADE-PARITY-1: confirms oracle age upload
//                    flows through VS→FS normalized [0,1]. Set MC2_VFX_DEBUG_MODE=5.
uniform int u_debugMode;

// VFX-TUNING-UI-1: user intensity scales, uploaded by gos_particle_bridge.
// Defaults are all 1.0 (set per-flush from runtime state seeded at 1.0) so the
// default frame is byte-identical (multiply by exactly 1.0 is IEEE identity).
// These tune look only — no emission/lifetime/sorting/timing effect.
uniform float u_vfxBrightness;          // global rgb scale (all particles)
uniform float u_vfxAdditiveBrightness;  // extra rgb scale, additive groups only
uniform float u_vfxAlphaScale;          // alpha (opacity) scale (all particles)
uniform int   u_vfxIsAdditive;          // 1 if the current draw group is additive

// VFX-SOFT-PARTICLES-MVP-1: depth-fade alpha particles at scene intersections.
// Disabled (byte-identical) when u_softDistance <= 0. u_invWorldToClip is the
// exact inverse of u_worldToClipGL (gosPostProcess::inverseViewProj_), so the
// fragment's own gl_FragCoord.z and the sampled scene depth reconstruct into
// the same world space the scene was rendered in. Reverse-Z (glClipControl
// ZERO_TO_ONE): window depth and NDC z share [0,1]; far/sky == 0.0.
uniform sampler2D u_sceneDepth;     // copy of scene depth (no FBO feedback loop)
uniform mat4      u_invWorldToClip; // world from (ndc.xy, windowDepth)
uniform vec2      u_screenSize;     // pixels
uniform float     u_softDistance;   // world-unit fade band; 0 = disabled

// VFX-LIT-PARTICLES-MVP-1: simple scene lighting for alpha smoke/dust.
// Disabled (byte-identical) when u_vfxLitStrength <= 0. Direction-independent
// soft fill from the scene sun + ambient (no per-fragment normal — billboards
// have none, so a directional N.L would flicker as the camera turns). Additive
// groups stay self-emissive and are skipped. Sourced from the global camera
// (eye->light*/ambient*), the same lighting terrain consumes.
uniform float u_vfxLitStrength;   // 0 = unlit; 1 = full scene-lit
uniform vec3  u_vfxSunColor;      // scene sun color     (0..1)
uniform vec3  u_vfxAmbientColor;  // scene ambient color (0..1)

// VFX-BLACKBODY-1: temperature->color emissive tint for additive groups only.
// Gate MC2_VFX_BLACKBODY (default OFF). u_vfxBlackbody == 0 -> branch skipped,
// byte-identical to today. When 1, additive/emissive particles are tinted by an
// analytic Planckian-locus (blackbody) color derived from per-particle brightness
// so hot flashes ramp warm-white -> orange the way real fire/plume/impact light does.
uniform int u_vfxBlackbody;       // 1 = apply blackbody emissive tint (additive only)

// VFX-DISTORTION-1: heat-haze refraction. Gate MC2_VFX_DISTORTION (default OFF).
// u_vfxDistort == 0 -> branch skipped, byte-identical to today. When 1 (set only
// for the distortion alpha group, only while the gate is ON AND the scene-color
// grab is present), this ALPHA fragment REPLACES dst with the pre-VFX scene color
// sampled at a procedurally-wobbled screen UV (in-house value-noise offset, NO
// imported noise/normal/LUT art). Soft-clip (u_softDistance) gates replace strength
// so haze does not bleed over near geometry. Requires VFX-SCENECOLOR-GRAB-1; if the
// grab is absent the bridge forces u_vfxDistort=0 -> inert (no GL-0 sampler bind).
uniform sampler2D u_sceneColor;   // pre-VFX scene-color snapshot (RGBA16F)
uniform int   u_vfxDistort;       // 1 = this group is a distortion (heat-haze) group
uniform float u_time;             // seconds, monotonic; uploaded once per flush
uniform float u_distortAmp;       // screen-UV wobble amplitude (~0.03-0.05, small)

// In-house 2-octave value-noise fbm. AUTHORED, not imported: a hash-based
// value noise (no texture, no table, no third-party code) used only to give the
// procedural screen-UV offset an organic shimmer instead of a pure sine grid.
float vd_hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
float vd_valnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = vd_hash(i + vec2(0.0, 0.0));
    float b = vd_hash(i + vec2(1.0, 0.0));
    float c = vd_hash(i + vec2(0.0, 1.0));
    float d = vd_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float vd_fbm(vec2 p) {
    return 0.6 * vd_valnoise(p) + 0.4 * vd_valnoise(p * 2.03 + 7.0);
}

// Analytic Planckian-locus -> linear RGB approximation. AUTHORED, not imported:
// this is an MC2-native rational/exponential fit hand-tuned to the qualitative
// shape of the blackbody (Planckian) chromaticity curve over ~1000K..6500K. It is
// NOT a copied table, NOT a sampled LUT, and NOT any third-party shader's code —
// purely an in-house closed-form approximation. Output is normalized so the
// brightest channel == 1.0 (a pure hue tint); the caller supplies intensity.
// Behavior: ~1000K deep red-orange, ~2000K orange, ~3500K warm white,
// ~5500K near-white, ~6500K faintly cool-white.
vec3 blackbodyRGB(float tempK) {
    float t = clamp(tempK, 1000.0, 6500.0) / 1000.0;  // work in kilokelvin
    // Red rises fast and saturates; modeled as a saturating rational curve.
    float r = clamp(1.30 - 0.55 / (t - 0.55), 0.0, 1.0);
    r = max(r, 0.55);  // never let the red floor drop out of a "hot" tint
    // Green climbs gradually with temperature (cooler -> less green).
    float g = clamp(0.38 * log(max(t, 1.0001)) + 0.18, 0.0, 1.0);
    // Blue stays near zero while cool/orange, then ramps in toward white.
    float b = clamp(0.62 * (t - 2.2) / 4.3, 0.0, 1.0);
    vec3 c = vec3(r, g, b);
    return c / max(max(c.r, max(c.g, c.b)), 1e-4);  // normalize brightest -> 1
}

vec3 sp_reconstructWorld(vec2 uv, float depth) {
    vec4 p = u_invWorldToClip * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return p.xyz / p.w;
}

in vec2 v_uv;
in vec4 v_color;
flat in uint v_kind;
flat in uint v_is_head;
flat in float v_age;  // VFX-SHADER-AGE-FADE-PARITY-1: normalized [0,1] age from VS

out vec4 outColor;

void main() {
    vec4 tex = textureLod(uAtlas, v_uv, 0.0);
    // Discard colorkey pixels (MC2 particle textures use magenta 0xFF00FF as transparent)
    if (tex.r > 0.9 && tex.g < 0.1 && tex.b > 0.9) discard;
    vec4 finalColor = tex * v_color;
    // Head-sprite brightening: particles with is_head=1 are rendered 1.5x brighter
    if (v_is_head == 1u) finalColor.rgb *= 1.5;
    // Also discard genuinely transparent pixels
    if (finalColor.a < 0.01) discard;

    // VFX-DISTORTION-1: heat-haze refraction. Runs ONLY for the distortion alpha
    // group (u_vfxDistort==1, set by the bridge only when the gate is ON and the
    // scene-color grab is present) and only outside debug views. Replaces dst with
    // the pre-VFX scene color sampled at a procedurally-wobbled screen UV. The blend
    // is the existing ALPHA pipeline (SRC_ALPHA/ONE_MINUS_SRC_ALPHA): outputting the
    // warped scene as rgb with a moderate alpha cross-fades dst toward the refracted
    // sample, so the background visibly wobbles behind the haze card. Amp is kept
    // SMALL for subtlety. The soft-clip (u_softDistance) machinery below still scales
    // the final alpha, so haze does not bleed over near geometry.
    if (u_vfxDistort == 1 && u_debugMode == 0) {
        vec2 suv = gl_FragCoord.xy / u_screenSize;
        // Procedural offset: cheap sine base + in-house fbm shimmer, animated by
        // u_time. Authored, no imported art. Amplitude in screen-UV space.
        float F = 9.0;
        vec2 base = vec2(sin(v_uv.y * F + u_time), cos(v_uv.x * F + u_time));
        float n = vd_fbm(v_uv * 6.0 + u_time * 0.7) - 0.5;
        vec2 offset = u_distortAmp * (base + vec2(n, -n));
        vec3 warped = textureLod(u_sceneColor, clamp(suv + offset, 0.0, 1.0), 0.0).rgb;
        // Replace strength = card coverage (finalColor.a) so the haze fades at the
        // card edges; further scaled by the soft-clip below.
        outColor = vec4(warped, finalColor.a);
        // Soft-clip: scale replace strength down where the card approaches opaque
        // scene behind it (reuse the alpha-only depth-fade). u_softDistance==0
        // (soft-particles gate OFF) -> branch skipped, full strength.
        if (u_softDistance > 0.0) {
            vec2 dsuv = gl_FragCoord.xy / u_screenSize;
            float sceneDepth = textureLod(u_sceneDepth, dsuv, 0.0).r;
            if (sceneDepth > 0.0001) {
                vec3 wScene = sp_reconstructWorld(dsuv, sceneDepth);
                vec3 wFrag  = sp_reconstructWorld(dsuv, gl_FragCoord.z);
                float fade  = clamp(distance(wScene, wFrag) / u_softDistance, 0.0, 1.0);
                outColor.a *= fade;
            }
        }
        outColor.a *= u_vfxAlphaScale;
        return;
    }

    if (u_debugMode == 1) {
        // Albedo: raw atlas texel, drop vertex-color tint; keep final alpha so
        // blend/coverage matches the real draw.
        outColor = vec4(tex.rgb, finalColor.a);
    } else if (u_debugMode == 2) {
        // Alpha: visualize the final alpha as grayscale.
        outColor = vec4(vec3(finalColor.a), finalColor.a);
    } else if (u_debugMode == 3) {
        // ParticleKind: hashed palette from the 4-bit kind id (kind_flags[7:4]).
        float k  = float(v_kind);
        vec3  kc = vec3(fract(k * 0.6180339 + 0.10),
                        fract(k * 0.3000000 + 0.40),
                        fract(k * 0.1300000 + 0.70));
        outColor = vec4(kc, finalColor.a);
    } else if (u_debugMode == 4) {
        // Overdraw proxy: each fragment contributes a small constant so blend
        // accumulation reveals overdraw hot-spots.
        outColor = vec4(0.15, 0.0, 0.0, 0.15);
    } else if (u_debugMode == 5) {
        // Age heat-map: blue=newborn (age~0.0), green=mid-life, red=dying (age~1.0).
        // VFX-SHADER-AGE-FADE-PARITY-1: confirms oracle age upload flows through
        // VS->FS correctly normalized [0,1]. Oracle particles show a gradient;
        // non-oracle particles (age==0) show solid blue. Set MC2_VFX_DEBUG_MODE=5.
        outColor = vec4(v_age, 1.0 - abs(v_age * 2.0 - 1.0), 1.0 - v_age, finalColor.a);
    } else {
        // 0 = Final (default, byte-identical to pre-slice output).
        outColor = finalColor;
    }

    // VFX-TUNING-UI-1: apply user intensity scales last. At defaults
    // (brightness=additive=alpha=1.0) every multiply is *= 1.0 → byte-identical.
    outColor.rgb *= u_vfxBrightness;
    if (u_vfxIsAdditive == 1) outColor.rgb *= u_vfxAdditiveBrightness;
    outColor.a *= u_vfxAlphaScale;

    // VFX-BLACKBODY-1: blackbody emissive tint, additive/emissive groups only.
    // OFF (default / gate unset) -> u_vfxBlackbody==0 -> branch skipped -> the
    // additive path is byte-identical to today. Skipped in debug views so the
    // diagnostics keep showing the raw fragments. Temperature is derived from the
    // particle's own emissive brightness (luma of the already-computed color) — NO
    // new per-particle data field, NO new art. Hotter (brighter) -> whiter; cooler
    // (dimmer) -> deeper orange, mapping luma 0..1 onto ~1200K..6000K.
    if (u_vfxBlackbody == 1 && u_vfxIsAdditive == 1 && u_debugMode == 0) {
        float luma = clamp(dot(outColor.rgb, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        float tempK = mix(1200.0, 6000.0, luma);
        outColor.rgb *= blackbodyRGB(tempK);
    }

    // VFX-LIT-PARTICLES-MVP-1: tint alpha smoke/dust by the scene sun+ambient
    // so it reads as lit volume rather than a flat decal. strength 0 (default /
    // gate OFF) -> mix() resolves to vec3(1.0) -> byte-identical. Alpha groups
    // only; additive flashes/lasers stay emissive. Skipped in debug views.
    if (u_vfxLitStrength > 0.0 && u_vfxIsAdditive == 0 && u_debugMode == 0) {
        vec3 fill = u_vfxAmbientColor + u_vfxSunColor * 0.5;
        outColor.rgb *= mix(vec3(1.0), fill, u_vfxLitStrength);
    }

    // VFX-SOFT-PARTICLES-MVP-1: soften alpha where the particle approaches the
    // opaque scene behind it. Alpha groups only (additive flashes unaffected).
    // u_softDistance == 0 (default / gate OFF) -> skipped -> byte-identical.
    if (u_softDistance > 0.0 && u_vfxIsAdditive == 0) {
        vec2 suv = gl_FragCoord.xy / u_screenSize;
        float sceneDepth = textureLod(u_sceneDepth, suv, 0.0).r;
        if (sceneDepth > 0.0001) {            // skip sky / far plane (reverse-Z far=0)
            vec3 wScene = sp_reconstructWorld(suv, sceneDepth);
            vec3 wFrag  = sp_reconstructWorld(suv, gl_FragCoord.z);
            float fade  = clamp(distance(wScene, wFrag) / u_softDistance, 0.0, 1.0);
            outColor.a *= fade;
        }
    }
}
