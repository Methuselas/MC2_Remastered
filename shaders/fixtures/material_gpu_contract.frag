// shaders/fixtures/material_gpu_contract.frag
//
// MaterialGpu-1 reflection fixture. NOT part of the game runtime.
// Never compiled into mc2.exe. Exists solely to produce a spirv-cross
// --reflect golden that validates MaterialGpu struct layout.
//
// Binding 5 is fixture/prototype-only — not a runtime allocation.
// Runtime binding is assigned in MaterialGpu-2.
//
// No #version directive: makeProgram() / reflect.py prepend "#version 430\n".

#include <../include/material_gpu.hglsl>

layout(std430, binding = 5) readonly buffer MaterialTable {
    MaterialGpu materials[];
} materialTable_;

layout(location = 0) out vec4 o_color;

void main() {
    // Read all 8 fields through a data-dependent accumulator so spirv-cross
    // cannot optimize the struct away before reflection.
    // fract(acc * 0.001) keeps the output in [0, 1] for a valid fragment.
    MaterialGpu m = materialTable_.materials[0];
    float acc = 0.0;
    acc += float(m.albedoTex);
    acc += float(m.normalTex);
    acc += float(m.metallicRoughnessTex);
    acc += float(m.emissiveTex);
    acc += float(m.flags);
    acc += m.baseColorFactor;
    acc += m.metallicFactor;
    acc += m.roughnessFactor;
    o_color = vec4(fract(acc * 0.001), 0.0, 0.0, 1.0);
}
