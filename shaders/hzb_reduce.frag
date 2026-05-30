//#version 430 (version provided by prefix)
//
// HZB-DEPTH-PYRAMID-MVP-1 (TRACKRV-HZB-VISIBILITY-OPUS-1).
// Custom reverse-Z Hi-Z depth-pyramid reduction. ONE fullscreen-quad pass per
// destination mip level. Runtime MUST match docs/hzb-depth-convention.md and
// tests/unit/test_depth_hzb.cpp:
//
//   * Reverse-Z, GL_ZERO_TO_ONE: near = 1.0, far = 0.0, larger depth = closer.
//   * Conservative occlusion reduction = MIN (parent stores the FARTHEST
//     occluder = smallest reverse-Z value). NOT max -- max would over-cull
//     visible geometry behind thin foreground slivers (invisible-object bug).
//   * Ceil mip ladder + clamped 2x2 fetch: GL_CLAMP_TO_EDGE on the source makes
//     the +/-0.5-texel taps safe on odd extents, so no source texel is dropped.
//
// This is NOT glGenerateMipmap (which averages and floors -- both wrong here).
//
// uReduce == 0 : level-0 seed. Source is the scene depth texture sampled 1:1
//                (DEPTH24_STENCIL8 read through a sampler2D returns depth in .r).
// uReduce == 1 : 2x2 MIN reduction of the previous HZB level.
//
// The caller constrains the bound source texture to exactly the source level
// (GL_TEXTURE_BASE_LEVEL == GL_TEXTURE_MAX_LEVEL) so textureLod(...,0.0) reads
// that level and there is no read/write feedback on the single HZB texture.

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uSrc;       // unit 0: source level (scene depth, or HZB level i)
uniform vec2      uSrcTexel;  // 1.0 / source-level dimensions
uniform int       uReduce;    // 0 = seed copy (1:1), 1 = 2x2 MIN reduction

void main()
{
    if (uReduce == 0) {
        // Seed HZB level 0 with the raw reverse-Z scene depth (pass-through).
        FragColor = vec4(textureLod(uSrc, TexCoord, 0.0).r);
        return;
    }

    // 2x2 block centered on the destination texel center. NEAREST + the +/-0.5
    // source-texel offsets land squarely in the four source texels; on odd
    // extents the out-of-range tap clamps to the edge texel (conservative).
    vec2 t = uSrcTexel;
    float d00 = textureLod(uSrc, TexCoord + vec2(-0.5, -0.5) * t, 0.0).r;
    float d10 = textureLod(uSrc, TexCoord + vec2( 0.5, -0.5) * t, 0.0).r;
    float d01 = textureLod(uSrc, TexCoord + vec2(-0.5,  0.5) * t, 0.0).r;
    float d11 = textureLod(uSrc, TexCoord + vec2( 0.5,  0.5) * t, 0.0).r;

    // MIN = farthest occluder = conservative reverse-Z HZB.
    FragColor = vec4(min(min(d00, d10), min(d01, d11)));
}
