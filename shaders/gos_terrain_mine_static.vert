//#version 430 (version provided by makeProgram prefix)

// PR2c Stage 2c — mine static-bake VS.
//
// Reads a static-baked vertex stream (built once per mission/setMine event by
// gos_terrain_indirect::BuildMineStaticVBO). Each vertex carries world-space
// position + sprite UV + texture-array layer index (0=mine sprite, 1=blown).
//
// Vertex format (24 bytes, matches MineVert in gos_terrain_indirect.cpp):
//   offset  0: vec3 pos    (world XYZ)
//   offset 12: vec2 uv     (sprite [0,1]^2)
//   offset 20: uint layer  (0=mine, 1=blown)
//
// Shader keeps it dumb-simple: standard terrainMVP transform, varyings to FS.
// No SSBO indirection, no thin-record decoding — the VBO IS the data.

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in uint a_layer;

uniform mat4 terrainMVP;   // worldToClip (matches PR1 SOLID terrainMVP)

out vec2  v_uv;
flat out uint v_layer;

void main() {
    gl_Position = terrainMVP * vec4(a_pos, 1.0);
    v_uv    = a_uv;
    v_layer = a_layer;
}
