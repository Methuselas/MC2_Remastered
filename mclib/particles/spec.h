//==========================================================================//
// File:    spec.h                                                           //
// Contents: GpuParticle C++ struct (std430-mirror of shaders/include/       //
//           particles.hglsl Particle).                                      //
//           Plan v5 §5.4 B1 Stage 1' Commit 2.                              //
//                                                                           //
// std430 lockstep discipline per memory/cpp_glsl_ubo_struct_lockstep.md +   //
// plan §5.4 v2 MAJOR-4 fold-in:                                             //
//   - sizeof()        static_assert (necessary)                             //
//   - alignof()       static_assert (std430 vec4 alignment requirement)     //
//   - per-field       offsetof static_assert (every field GLSL reads)       //
//                                                                           //
// Any change to this struct MUST update both the static_asserts AND the     //
// mirror at shaders/include/particles.hglsl in the SAME atomic commit.      //
// One-source schema generation is filed as B2 polish debt.                  //
//===========================================================================//

#pragma once

#include <cstddef>
#include <cstdint>

namespace mc2 {
namespace particles {

// 64-byte std430 record. Layout chosen so vec3 fields sit on 16-byte
// boundaries (std430 vec3 alignment == 16). Padding is explicit so the
// struct compiles identically MSVC / Clang / GCC.
//
// Field layout (offset / type / purpose):
//   00  vec3+pad  position   world-space spawn origin
//   16  vec4      color      premultiplied RGBA at draw time
//   32  vec3+pad  velocity   world-space per-second delta (Stage 2' uses this;
//                            Stage 1' Card test effect leaves at zero)
//   48  float     lifetime   total lifetime in seconds
//   52  float     age        current age in seconds (CPU updates per frame in
//                            Stage 1'; Stage 2' shifts age advance to GPU via
//                            spawn-frame seed)
//   56  float     size       world-space radius in meters
//   60  uint      atlasIndex texture-atlas page id (Stage 1' Card hardcodes 0)
struct GpuParticle {
    float    position[3];
    float    _pad0;          // pad to vec4 alignment
    float    color[4];
    float    velocity[3];
    float    _pad1;          // pad to vec4 alignment
    float    lifetime;
    float    age;
    float    size;
    uint32_t atlasIndex;
};

// --- std430 lockstep asserts (plan §5.4 v2 MAJOR-4 fold-in) ---
static_assert(sizeof(GpuParticle) == 64,
              "GpuParticle size drift — update shaders/include/particles.hglsl in the same commit");
static_assert(alignof(GpuParticle) >= 4,
              "GpuParticle alignment too low for std430 mirror");
// Per-field offset asserts. Every offset GLSL reads is pinned here. Any
// edit to GpuParticle must update BOTH this list AND particles.hglsl.
static_assert(offsetof(GpuParticle, position)   ==  0, "GpuParticle.position offset drift");
static_assert(offsetof(GpuParticle, color)      == 16, "GpuParticle.color offset drift");
static_assert(offsetof(GpuParticle, velocity)   == 32, "GpuParticle.velocity offset drift");
static_assert(offsetof(GpuParticle, lifetime)   == 48, "GpuParticle.lifetime offset drift");
static_assert(offsetof(GpuParticle, age)        == 52, "GpuParticle.age offset drift");
static_assert(offsetof(GpuParticle, size)       == 56, "GpuParticle.size offset drift");
static_assert(offsetof(GpuParticle, atlasIndex) == 60, "GpuParticle.atlasIndex offset drift");

}  // namespace particles
}  // namespace mc2
