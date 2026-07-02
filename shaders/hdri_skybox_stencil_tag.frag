// SKYBOX-FOG-EXCLUDE-1: stencil-only tag pass, drawn immediately after the
// normal HDRI skybox color draw (renderHdriSkybox), gated MC2_SKYBOX_FOG_EXCLUDE.
//
// Purpose: mark stencil=1 for pixels the fog passes must treat as "true sky,
// do not fog" -- reusing the EXACT same worldDir.z < -0.22 band that
// fog_oob.frag already uses to fade in its cloud band (shaders/fog_oob.frag).
// Pixels inside/below that band (near-horizon and the OOB void) are left
// untouched (discard) so their stencil stays at the frame-clear value (0);
// the fog passes still apply there, matching current behavior. Pixels above
// the band (deep sky) are tagged 1 so the fog passes hard-exclude them.
//
// Color writes are masked off by the caller (glColorMask all FALSE) --
// this pass only affects the stencil buffer. Same worldDir reconstruction
// as hdri_skybox.frag's default (frameFix==0) path: invProj + invViewRot.
// Must be kept in sync with that path; frameFix variants (1/2) are dead
// call sites (not reachable from the live frame loop) so they are
// intentionally not mirrored here.

in vec2 vNdc;

layout(location = 0) out vec4 FragColor;  // unused: color writes masked off

uniform mat4  invProj;
uniform mat3  invViewRot;
uniform float skyYaw;  // same sun-sync rotation as the color pass

void main()
{
    vec4 clip = vec4(vNdc, 1.0, 1.0);
    vec3 viewDir = (invProj * clip).xyz;
    vec3 worldDir = normalize(invViewRot * viewDir);

    if (skyYaw != 0.0) {
        float s = sin(skyYaw);
        float c = cos(skyYaw);
        worldDir = vec3(worldDir.x * c - worldDir.z * s,
                        worldDir.y,
                        worldDir.x * s + worldDir.z * c);
    }

    // Same threshold as fog_oob.frag's "worldDir.z < -0.22 => clouds fully
    // clipped, deep sky" cutoff. Below/at the threshold: leave stencil alone
    // (discard) so fog still applies (horizon band + OOB void unchanged).
    if (worldDir.z >= -0.22) {
        discard;
    }

    FragColor = vec4(0.0);
}
