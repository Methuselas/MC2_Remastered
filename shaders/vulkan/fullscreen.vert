// VULKAN-SHADER-TOOLCHAIN-1 -- trivial fullscreen-triangle vertex shader.
// Vulkan-flavored GLSL (#version 450, explicit locations, no C++ prefix inject).
// Emits a single oversized triangle covering the viewport; no vertex buffer.
// Compiled to SPIR-V at build time only under MC2_VULKAN. Not yet rendered.
#version 450

layout(location = 0) out vec2 vUV;

void main() {
    // gl_VertexIndex in {0,1,2} -> fullscreen triangle.
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
