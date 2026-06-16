// Depth-only instanced+skinned mech shadow. Mirrors mech.vert SSBO/bone
// math for POSITION ONLY. mat4(row0..row3) is intentionally the transpose
// of the Stuff bone matrix so boneT*v == row-vec v*M (see mech.vert) -
// do NOT "fix" it.
layout(location=0) in vec3 a_position;
layout(location=3) in uvec4 a_boneIndices;
layout(location=4) in vec4 a_boneWeights;

// MUST match the full 64-byte std430 layout of the C++ GpuMechInstance
// (gos_mech_batcher.h) AND mech.vert. A truncated struct here (was 48 bytes)
// strides instances[] wrong -> every bucket with instanceBase>0 reads a
// shifted record -> garbage baseBoneOffset -> those mech parts collapse out of
// the shadow depth (present parts sharp, same parts missing every frame).
struct GpuMechInstance {
    uint typeLodRecordIndex; uint baseBoneOffset; uint lightDataIndex; uint renderFlags;
    vec4 aRGBHighlight; vec4 fogRGB;
    uint objectIdRaw; uint materialIdx; uint _pad2; uint _pad3;
};
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(std430, binding=0) readonly buffer InstanceBuffer { GpuMechInstance instances[]; };
layout(std430, binding=1) readonly buffer BoneBuffer { GpuMechBone bones[]; };

uniform int  u_instanceBase;
uniform int  u_skinningMode;
uniform mat4 lightSpaceMatrix;

void main() {
    uint instIdx = uint(u_instanceBase) + uint(gl_InstanceID);
    GpuMechInstance inst = instances[instIdx];
    mat4 boneT;
    if (u_skinningMode != 0) {
        boneT = mat4(0.0);
        for (int i = 0; i < 4; ++i) {
            float w = a_boneWeights[i];
            if (w > 0.0) {
                GpuMechBone b = bones[a_boneIndices[i] + inst.baseBoneOffset];
                boneT += w * mat4(b.row0, b.row1, b.row2, b.row3);
            }
        }
    } else {
        GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
        boneT = mat4(b.row0, b.row1, b.row2, b.row3);
    }
    vec4 worldStuff = boneT * vec4(a_position, 1.0);
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    gl_Position = lightSpaceMatrix * vec4(worldMC2, 1.0);
}
