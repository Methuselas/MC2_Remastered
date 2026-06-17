//#version 430 (version provided by material prefix)

// Instanced billboard vegetation card vertex shader.
// Two crossed vertical quads per instance (8 vertices, 2 quads in static VBO).

const float PI_HALF = 1.5707963;

// Per-vertex static attributes (from static VBO, 8 vertices)
layout(location=0) in vec3 a_card;        // (horizontal[-0.5..0.5], vertical[0..1], yawOffset[0 or PI_HALF])
layout(location=1) in vec2 a_uv;          // local UV [0..1]x[0..1]

// Per-instance attributes (divisor=1, from instance VBO)
layout(location=2) in vec3  i_worldPos;   // MC2 world (x=east, y=north, z=elev)
layout(location=3) in float i_yaw;        // rotation about Z axis (radians)
layout(location=4) in float i_scale;      // uniform scale
layout(location=5) in uint  i_atlasFrame; // 0..3 (which 256-wide column in 1024x256 atlas)
layout(location=6) in float i_seed;       // per-instance random [0,1] for stable dither

uniform mat4  u_worldToClipGL; // MC2 world -> GL clip (from eye->worldToClipGL())
uniform vec4  u_cameraPos;     // Stuff/MLR space: x=left(-east), y=elev, z=forward(north)
uniform float u_time;          // elapsed seconds

out vec2  v_atlasUV;   // remapped to atlas column
out float v_camDist;   // horizontal camera distance (for dither fade)
out float v_seed;      // pass-through for dither stability
out vec3  v_worldPos;  // world pos for potential future use

void main()
{
    // Atlas UV: map a_uv.x [0..1] into the correct 256-wide column of 1024-wide atlas
    v_atlasUV = vec2(a_uv.x * 0.25 + float(i_atlasFrame) * 0.25, a_uv.y);

    // Billboard expansion: total yaw = instance yaw + per-card yaw offset (0 or PI/2)
    float totalYaw = i_yaw + a_card.z;
    vec2 right = vec2(cos(totalYaw), sin(totalYaw));

    vec3 worldPos = i_worldPos;
    worldPos.xy += a_card.x * i_scale * right;  // horizontal spread
    worldPos.z  += a_card.y * i_scale * 2.2;    // card height ~2.2 world units

    // Wind sway: applied proportionally (a_card.y = 0 at base, 1 at tip)
    float windPhase = u_time * 2.0 + i_worldPos.x * 0.3 + i_worldPos.y * 0.2;
    float windOff = sin(windPhase) * 0.18 * a_card.y;
    // Apply perpendicular to right vector (along card front/back axis)
    worldPos.xy += windOff * vec2(-right.y, right.x);

    // Camera ground position in MC2 world coords (Stuff/MLR: x=left(-east), z=forward(north))
    vec2 camGround = vec2(-u_cameraPos.x, u_cameraPos.z);
    v_camDist = distance(i_worldPos.xy, camGround);

    gl_Position = u_worldToClipGL * vec4(worldPos, 1.0);

    v_seed     = i_seed;
    v_worldPos = worldPos;
}
