// SLICE 3a: LOD-chunk tessellation EVALUATION shader. Barycentric interpolation
// of the corner outputs -> at any tess level, with no displacement, lands on the
// SAME plane as the original triangle (structurally equivalent; strict byte-
// identity is a gate-OFF property only). Crack-free (no displacement moves shared
// edges). ccw matches the CCW index winding.
//
// SLICE 3b: when MC2_TERRAIN_CLIFF_DISPLACE is on, this TES DISPLACES the surface
// along the geometric (away-from-cliff) face normal with directional vertical
// fluting (see include/cliff_displace.hglsl), then recomputes clip from the
// DISPLACED world position (NOT the interpolated tc_clip). The tess program is
// only ever built when the gate is on, so this file's displacement never runs on
// the default-OFF (GL_TRIANGLES) path -> gate-OFF stays byte-identical.
layout(triangles, equal_spacing, ccw) in;

in  vec3  tc_worldPos[];
in  float tc_terrainType[];
in  vec4  tc_clip[];

out vec3  v_worldPos;
out float v_terrainType;

// SLICE 3b: the displaced surface must be projected with the SAME world->clip
// matrix and depth fudge the VS applied (terrain_lod_chunk.vert). Both are shared
// program uniforms (the tess program shares the VS+FS); the C++ once/frame mirror
// copies u_worldToClipGL + cameraPos to the tess program. Declaring them here lets
// this stage read them.
uniform mat4  u_worldToClipGL;
// cameraPos: Stuff/MLR eye (.x=left, .y=elevation, .z=forward); MC2 world =
// (-x, z, y). SAME uniform name + frame as terrain_lod_chunk.frag's POM view path.
uniform vec4  cameraPos;

// Lockstep with terrain_lod_chunk.vert's local TERRAIN_DEPTH_FUDGE (0.0) and
// shaders/include/terrain_depth_bias.hglsl. Applied PRE-DIVIDE (clip.z += 2*F*w).
const float TERRAIN_DEPTH_FUDGE = 0.0;

#include <include/cliff_displace.hglsl>

void main() {
    vec3 b = gl_TessCoord;
    vec3 P = b.x * tc_worldPos[0]    + b.y * tc_worldPos[1]    + b.z * tc_worldPos[2];
    v_terrainType = b.x * tc_terrainType[0] + b.y * tc_terrainType[1] + b.z * tc_terrainType[2];

    // Geometric per-triangle face normal from the patch corners (flat = the
    // away-from-cliff direction on a face). Flip so it points up/away (+Z up).
    vec3 e0 = tc_worldPos[1] - tc_worldPos[0];
    vec3 e1 = tc_worldPos[2] - tc_worldPos[0];
    vec3 N  = cross(e0, e1);
    float nl = length(N);
    N = (nl > 1e-8) ? (N / nl) : vec3(0.0, 0.0, 1.0);
    if (N.z < 0.0) N = -N;   // up = +Z: point the normal up/away from the cliff

    // Camera ground distance for the near-field fade (frag POM parity: eye in MC2
    // world + altitude boost so overhead views still fade with true distance).
    vec3  camW    = vec3(-cameraPos.x, cameraPos.z, cameraPos.y);
    float altBoost = max(camW.z - P.z, 0.0) * 0.7;
    float camDist  = distance(P.xy, camW.xy) + altBoost;

    // edgeFade = 1.0 for Task 1 (skirt-crack edge-fade lands in Task 2).
    P = cliffDisplace(P, N, 1.0, camDist);

    v_worldPos = P;   // frag lights/samples the DISPLACED surface

    // Clip from the DISPLACED position with the same pre-divide depth fudge the VS
    // applied (moving the fudge post-perturbation, spec §9.7 / Task 3).
    vec4 clip = u_worldToClipGL * vec4(P, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;
    gl_Position = clip;
}
