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

    // Equirect mapping IN MC2 WORLD AXES (NOT GL axes).
    //
    // Camera::worldToCameraMatrix is in Stuff/MC2 space. The documented
    // MC2->GL axis swap (camera.cpp:77-89) is:
    //   GL.x = -MC2.x   GL.y = MC2.z (up)   GL.z = MC2.y (forward)
    //
    // Our worldDir = invViewRot * viewDir comes out in MC2 axes, so:
    //   MC2.x = horizontal axis 1
    //   MC2.y = ground/forward (horizontal axis 2)
    //   MC2.z = elevation / UP
    //
    // Therefore azimuth = atan(MC2.y, MC2.x), elevation = asin(MC2.z).
    // First v0 of the shader (commit a2c3ef3f) sampled MC2.y as elevation,
    // which manifested as "sky upside-down" because the forward axis was
    // mapping to vertical UV. Confirmed visually 2026-05-25.
    vec2 uv = vec2(
        atan(worldDir.y, worldDir.x) / (2.0 * PI) + 0.5,
        asin(clamp(worldDir.z, -1.0, 1.0)) / PI + 0.5
    );

    FragColor = vec4(texture(u_hdri, uv).rgb, 1.0);
}
