// shaders/fixtures/view_uniforms_contract.frag
//
// ViewUniforms reflection fixture. NOT part of the game runtime.
// Never compiled into mc2.exe. Exists solely to produce a spirv-cross
// --reflect golden that validates ViewUniforms struct layout.
//
// Binding 3 is fixture-only — not a runtime allocation.
//
// No #version directive: reflect.py prepends "#version 430\n".

#include <../include/view_uniforms.hglsl>

layout(std430, binding = 3) readonly buffer ViewUniformsBlock {
    ViewUniforms viewBuf[];
} viewBlock_;

layout(location = 0) out vec4 o_color;

void main() {
    // Read all 3 fields through a data-dependent accumulator so spirv-cross
    // cannot optimize the struct away before reflection.
    ViewUniforms v = viewBlock_.viewBuf[0];
    float acc = 0.0;
    acc += v.worldToClipGL[0][0];
    acc += v.worldToViewGL[0][0];
    acc += v.cameraWorldPos.x;
    o_color = vec4(fract(acc * 0.001), 0.0, 0.0, 1.0);
}
