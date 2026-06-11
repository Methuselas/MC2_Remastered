//#version 430 (provided by makeProgram prefix)
//
// VFX mesh substrate FS (MC2_VFX_ORACLE_SHAPE slice — gosFX::Shape only).
//
// textureLod (NOT texture) per memory/amd_auto_lod_strict_fail.md — the AMD
// RX 7900 XTX returns black from texture() on an incomplete mip pyramid even
// with LINEAR/MAX_LEVEL=0. Explicit LOD 0 bypasses the auto-LOD selector, same
// fix the billboard / mech / static-prop paths use.
//
// Output = atlasTexel * per-instance rgba. Alpha-vs-additive blend is selected
// by the bridge's glBlendFunc (carried from MLRState alpha mode); the FS just
// emits straight rgba. No object-ID write — single color attachment only.

uniform sampler2D uAtlas;
uniform int       u_hasTexture;   // 1 = sample uAtlas; 0 = white (untextured prim)

in vec2 v_uv;
in vec4 v_color;

out vec4 fragColor;

void main() {
    vec4 texel = (u_hasTexture != 0)
                 ? textureLod(uAtlas, v_uv, 0.0)
                 : vec4(1.0);
    fragColor = texel * v_color;
}
