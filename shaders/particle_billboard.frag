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
}
