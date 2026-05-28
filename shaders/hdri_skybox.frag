// HDRI-SKY-1: equirect sky background.
// Direction reconstruction: inverse projection of NDC -> view dir,
// then inverse view rotation (translation excluded) -> world dir.
// Sample equirectangular HDRI. Linear output. No tonemap, no exposure.

in vec2 vNdc;

layout(location = 0) out vec4 FragColor;

uniform mat4 invProj;
uniform mat3 invViewRot;
uniform sampler2D u_hdri;

const float PI = 3.14159265358979323846;

void main()
{
    // NDC -> view direction.
    vec4 clip = vec4(vNdc, 1.0, 1.0);
    vec3 viewDir = (invProj * clip).xyz;
    vec3 worldDir = normalize(invViewRot * viewDir);

    // Equirect mapping. Output the FLIPPED v to handle EXR top-to-bottom
    // scanline order vs GL bottom-to-top texture convention.
    // (All my axis-swap experiments may have been chasing ghosts; isolating
    // v-flip as the single variable to validate. If still upside-down, the
    // bug is in T6's matrix extraction — Stuff uses row-vec convention with
    // column-major memory, which may interact unexpectedly with our explicit
    // transpose-via-element-permutation.)
    vec2 uv = vec2(
        atan(worldDir.z, worldDir.x) / (2.0 * PI) + 0.5,
        1.0 - (asin(clamp(worldDir.y, -1.0, 1.0)) / PI + 0.5)
    );

    FragColor = vec4(texture(u_hdri, uv).rgb, 1.0);
}
