//#version 430 (provided by makeProgram prefix)
//
// MC2_VFX_ORACLE_TUBE slice 1: gosFX Tube swept-quad ribbon FS.
//
// Samples the Tube's MLR texture with the per-vertex ribbon UV (U along the
// spine, V around the cross-section) and modulates by the per-vertex animated
// RGBA the CPU sim already computed. Alpha blend only in slice 1 (MissileSmoke
// / alpha ribbons); additive Tube kinds fall through to legacy MLR on the C++
// side and never reach this shader.
//
// textureLod (NOT texture) per memory/amd_auto_lod_strict_fail.md: AMD RX 7900
// XTX returns black from texture() on an incomplete mip pyramid; explicit LOD 0
// bypasses the auto-LOD selector (same fix mech.frag / billboard FS use).

uniform sampler2D uAtlas;

in vec2 v_uv;
in vec4 v_color;

out vec4 fragColor;

void main() {
    vec4 tex = textureLod(uAtlas, v_uv, 0.0);
    vec4 c   = tex * v_color;
    // Alpha-blend group: premultiply is unnecessary; the bridge sets
    // GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA. Discard fully transparent
    // fragments to avoid depth-test edge cases (depth-write is OFF anyway).
    if (c.a <= 0.0039) discard;
    fragColor = c;
}
