// VULKAN-SHADER-TOOLCHAIN-1 -- trivial fullscreen fragment shader.
// Vulkan-flavored GLSL (#version 450, explicit locations). Outputs the
// interpolated UV as color (solid-ish gradient). Compiled to SPIR-V at build
// time only under MC2_VULKAN. Not yet rendered anywhere.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vUV, 0.0, 1.0);
}
