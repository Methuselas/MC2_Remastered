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

vec3 sp_reconstructWorld(vec2 uv, float depth) {
    vec4 p = u_invWorldToClip * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return p.xyz / p.w;
}

in vec2 v_uv;
in vec4 v_color;
flat in uint v_kind;
flat in uint v_is_head;

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
    } else {
        // 0 = Final (default, byte-identical to pre-slice output).
        outColor = finalColor;
    }

    // VFX-TUNING-UI-1: apply user intensity scales last. At defaults
    // (brightness=additive=alpha=1.0) every multiply is *= 1.0 → byte-identical.
    outColor.rgb *= u_vfxBrightness;
    if (u_vfxIsAdditive == 1) outColor.rgb *= u_vfxAdditiveBrightness;
    outColor.a *= u_vfxAlphaScale;

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
