//#version 430 (version provided by makeProgram prefix)

// PR2c Stage 2c — mine static-bake FS.
//
// Samples a 2-layer GL_TEXTURE_2D_ARRAY at sampler unit 5 (built by
// BuildMineTextureArray). Layer 0 = defaults/mine_00.tga; layer 1 =
// defaults/minescorch_00.tga. Both are 16x16 RGBA8 alpha-keyed sprites.
//
// Alpha-test discard mirrors the legacy gos_Texture_Alpha behavior at
// quad.cpp:524, :531 (gosHint_DisableMipmap | DontShrink). Threshold 0.5
// is the standard alpha-cutout pivot.

in vec2  v_uv;
flat in uint v_layer;

uniform sampler2DArray mineSpriteArray;  // bound at unit 5

out vec4 FragColor;

void main() {
    vec4 c = texture(mineSpriteArray, vec3(v_uv, float(v_layer)));
    if (c.a < 0.5) discard;
    FragColor = c;
}
