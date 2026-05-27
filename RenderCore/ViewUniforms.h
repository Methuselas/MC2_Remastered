#pragma once
// RenderCore/ViewUniforms.h — per-frame view/projection ABI contract.
//
// SHADER-SCHEMA-2: this struct is the C++ authority for the ViewUniforms
// SSBO/UBO layout. GLSL mirror: shaders/include/view_uniforms.hglsl.
// ABI gate: cmake --build build64 --target shader_schema
//
// Row-major storage (matches GL_FALSE upload convention used throughout
// the engine). GLSL mat4 reads columns; ssbo_readViewUniforms() caller
// must assemble column-major mat4 from row-major rows if needed.
//
// Not yet wired into any shader. Defines the contract that F1 Phase 3
// (ViewUniforms atomic flip) and multi-view work must honour.

#include <cstddef>   // offsetof
#include <cstdint>   // uint32_t

namespace RenderCore {

// 144-byte per-frame view/projection record.
// std430-compatible; same layout in std140 for these types.
struct alignas(16) ViewUniforms {
    float worldToClipGL[16];   //   0 — world → GL clip (row-major, GL_FALSE upload)
    float worldToViewGL[16];   //  64 — world → view   (row-major, GL_FALSE upload)
    float cameraWorldPos[4];   // 128 — world-space camera position (w unused)
};

static_assert(sizeof(ViewUniforms) == 144,
              "ViewUniforms must be 144 bytes for clean std430 stride");
static_assert(offsetof(ViewUniforms, worldToClipGL) ==   0, "worldToClipGL offset");
static_assert(offsetof(ViewUniforms, worldToViewGL) ==  64, "worldToViewGL offset");
static_assert(offsetof(ViewUniforms, cameraWorldPos) == 128, "cameraWorldPos offset");

// UBO binding point for ViewUniforms. Binding 2 is reserved by the compute-cull
// frustum UBO (gpu_cull_compute.cpp CULL_UBO_BINDING=2). Binding 3 is the next
// free UBO slot (UBO and SSBO namespaces are separate in GL 4.3+).
// GL impl: GameOS/gameos/view_uniforms_gl.h
constexpr uint32_t kViewUniformsBinding = 3;

} // namespace RenderCore
