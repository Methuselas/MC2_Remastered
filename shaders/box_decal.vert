// BT2018-BOX-DECAL-1 — screen-space AABB decal volume, vertex stage.
//
// Clean-room implementation of the standard, widely-published "screen-space /
// deferred box decal" technique: rasterize a world-space box volume, then in the
// fragment stage reconstruct the underlying scene surface from the depth buffer and
// project a decal onto it inside the box. Authored from the algorithm for MC2's
// forward+MRT / reversed-Z renderer; NOT derived from any third-party shader source.
//
// v1: AXIS-ALIGNED box driven by center + half-extents uniforms (no per-decal model
// matrix), so only the already-proven u_viewProj convention is exercised (oriented
// boxes are a later slice). No #version here — makeProgram() prefixes "#version 430\n".

layout(location = 0) in vec3 a_cubePos;   // unit cube corner in [-0.5, 0.5]

uniform mat4 u_viewProj;                   // same convention as ssao/screenShadow (GL_FALSE)
uniform vec3 u_boxCenter;                  // world (reconstruct frame, Y-up)
uniform vec3 u_boxHalf;                    // world half-extents

void main() {
    // a_cubePos in [-0.5,0.5] -> span [-half, +half] => * (2 * half).
    vec3 world = u_boxCenter + a_cubePos * (u_boxHalf * 2.0);
    gl_Position = u_viewProj * vec4(world, 1.0);
}
