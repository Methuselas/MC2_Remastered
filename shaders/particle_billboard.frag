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

in vec2 v_uv;
in vec4 v_color;

out vec4 outColor;

void main() {
    vec4 tex = textureLod(uAtlas, v_uv, 0.0);
    // Discard colorkey pixels (MC2 particle textures use magenta 0xFF00FF as transparent)
    if (tex.r > 0.9 && tex.g < 0.1 && tex.b > 0.9) discard;
    outColor = tex * v_color;
    // Also discard genuinely transparent pixels
    if (outColor.a < 0.01) discard;
}
