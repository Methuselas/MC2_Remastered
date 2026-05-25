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
    // NDC -> view direction. z=1, w=1 gives a point on the far plane
    // pre-perspective-divide; after invProj the .xyz direction (before
    // normalize) is what we want.
    vec4 clip = vec4(vNdc, 1.0, 1.0);
    vec3 viewDir = (invProj * clip).xyz;
    vec3 worldDir = normalize(invViewRot * viewDir);

    // Equirect mapping. atan(z,x) maps the horizontal plane onto u.
    // asin(y) maps elevation onto v.
    vec2 uv = vec2(
        atan(worldDir.z, worldDir.x) / (2.0 * PI) + 0.5,
        asin(clamp(worldDir.y, -1.0, 1.0)) / PI + 0.5
    );

    FragColor = vec4(texture(u_hdri, uv).rgb, 1.0);
}
