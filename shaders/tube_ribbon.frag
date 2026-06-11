//#version 430 (provided by makeProgram prefix)
//
// MC2_VFX_ORACLE_TUBE slice 1: gosFX Tube swept-quad ribbon FS.
//
// Samples the Tube's MLR texture with the per-vertex ribbon UV (U along the
// spine, V around the cross-section) and modulates by the per-vertex animated
// RGBA the CPU sim already computed. Handles BOTH alpha ribbons (MissileSmoke)
// and additive ribbons (PPC / ER-PPC bolts) — slice 2 widened the C++ classifier
// to route additive Tubes through this shader (blendMode=1 -> GL_SRC_ALPHA,GL_ONE).
// The fragment discard is therefore blend-aware (uAdditive): alpha ribbons keep
// the alpha-coverage discard; additive ribbons carry low/zero per-vertex alpha
// (additive contributes via RGB, not alpha), so an alpha discard there would kill
// nearly the whole tube and leave only a tiny bright core — the "little ball" bug.
//
// textureLod (NOT texture) per memory/amd_auto_lod_strict_fail.md: AMD RX 7900
// XTX returns black from texture() on an incomplete mip pyramid; explicit LOD 0
// bypasses the auto-LOD selector (same fix mech.frag / billboard FS use).

uniform sampler2D uAtlas;
uniform bool      uAdditive;  // 1 = additive (PPC) ribbon, 0 = alpha ribbon

in vec2 v_uv;
in vec4 v_color;

out vec4 fragColor;

void main() {
    vec4 tex = textureLod(uAtlas, v_uv, 0.0);
    vec4 c   = tex * v_color;
    // Blend-aware discard. Depth-write is OFF for both, so this is purely to
    // skip no-op fragments.
    if (!uAdditive) {
        // Alpha ribbon (GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA): drop fully
        // transparent fragments to avoid depth-test edge cases.
        if (c.a <= 0.0039) discard;
    } else {
        // Additive ribbon (GL_SRC_ALPHA,GL_ONE): alpha is NOT the coverage
        // channel; a black fragment adds nothing, so only discard true black.
        if (max(max(c.r, c.g), c.b) <= 0.0001) discard;
    }
    fragColor = c;
}
