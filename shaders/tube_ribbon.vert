//#version 430 (provided by makeProgram prefix)
//
// MC2_VFX_ORACLE_TUBE slice 1: gosFX Tube swept-quad ribbon VS.
//
// This is NOT a billboard/card expansion shader. The CPU Tube sim already
// builds a continuous swept-quad ribbon mesh every frame (m_P_vertices /
// m_P_colors / m_P_uvs + the BuildMesh index stencil). The ribbon bridge
// uploads that mesh verbatim into three std430 SSBOs and issues a single
// glDrawElements over the live profile index range. One oriented quad per
// consecutive profile pair → no ladder/fence, no per-profile card.
//
// Vertex positions are uploaded in MC2/Stuff world space (effect local
// transformed through local_to_world on the CPU). The axis swap
// (GL_x=-Stuff_x, GL_y=Stuff_z, GL_z=Stuff_y) is applied HERE in-shader,
// then u_worldToClipGL (= gos_GetTerrainMVPMat4()) projects to GL clip —
// exactly the same convention the billboard VS uses, so the ribbon lands
// pixel-coincident with where legacy MLR / the billboard path would place it.

layout(std430, binding = 14) readonly buffer RibbonPos   { vec4 positions[]; };
layout(std430, binding = 15) readonly buffer RibbonColor { vec4 colors[];    };
layout(std430, binding = 16) readonly buffer RibbonUv     { vec2 uvs[];        };

uniform mat4 u_worldToClipGL;  // world -> GL clip (kAxisSwapMC2toGL * worldToClip)

out vec2 v_uv;
out vec4 v_color;

void main() {
    // gl_VertexID indexes the bound element buffer -> per-vertex SSBO fetch.
    vec3 stuffPos = positions[gl_VertexID].xyz;
    vec3 glPos    = vec3(-stuffPos.x, stuffPos.z, stuffPos.y);
    gl_Position   = u_worldToClipGL * vec4(glPos, 1.0);
    v_uv    = uvs[gl_VertexID];
    v_color = colors[gl_VertexID];
}
