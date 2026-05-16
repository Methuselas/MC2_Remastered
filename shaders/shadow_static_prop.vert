// Depth-only instanced static-prop shadow. Mirrors static_prop.vert
// legacy (non-coalesce) SSBO path. v*M row-vector order (Stuff std430
// col-major), then Stuff->MC2 swizzle - matches static_prop.vert, NOT
// shadow_object.vert's column-vector convention.
//
// NOTE: the non-coalesce branch in static_prop.vert indexes instances_.i[gl_InstanceID]
// (bare, no u_instanceBase). The SSBO range is bound per-type by the caller
// (Task 3), so gl_InstanceID is relative to the bound range start.
layout(location=0) in vec3 a_position;

struct Instance {
    mat4  modelMatrix;
    uint  typeID;
    uint  firstColorOffset;
    uint  flags;
    uint  lightDataIndex;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};
layout(std430, binding=0) readonly buffer Instances { Instance i[]; } instances_;

uniform mat4 lightSpaceMatrix;

void main() {
    Instance inst = instances_.i[gl_InstanceID];
    vec4 worldStuff = vec4(a_position, 1.0) * inst.modelMatrix;
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    gl_Position = lightSpaceMatrix * vec4(worldMC2, 1.0);
}
