//#version 430 (version provided by prefix)
//
// SSAO-GTAO-LITE-MVP-1 (Track V): apply the half-res AO buffer to the scene.
// Runs as a fullscreen pass into the scene color FBO with multiplicative
// blending (GL_DST_COLOR, GL_ZERO) set by the caller, so the scene RGB is
// scaled by the AO factor. A small box tap upsamples + denoises the half-res
// AO. Sky pixels carry AO = 1 (set in ssao.frag), so background stays intact.
//
// debugMode != 0: caller disables blend and we OVERWRITE with the AO as
// grayscale so the raw occlusion buffer can be inspected.

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D ssaoTex;       // unit 0: half-res AO
uniform vec2 ssaoTexel;          // 1/halfWidth, 1/halfHeight
uniform int debugMode;           // 0 = multiply scene, 1 = show AO grayscale

void main()
{
    // 4-tap box around the texel (linear filtering already smooths between).
    float ao = 0.0;
    ao += texture(ssaoTex, TexCoord + vec2(-0.5, -0.5) * ssaoTexel).r;
    ao += texture(ssaoTex, TexCoord + vec2( 0.5, -0.5) * ssaoTexel).r;
    ao += texture(ssaoTex, TexCoord + vec2(-0.5,  0.5) * ssaoTexel).r;
    ao += texture(ssaoTex, TexCoord + vec2( 0.5,  0.5) * ssaoTexel).r;
    ao *= 0.25;

    if (debugMode != 0) {
        FragColor = vec4(ao, ao, ao, 1.0);   // overwrite (blend disabled)
        return;
    }
    // Multiplicative apply (blend = DST_COLOR, ZERO): scene *= ao.
    FragColor = vec4(ao, ao, ao, 1.0);
}
