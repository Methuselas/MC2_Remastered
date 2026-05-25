// RenderCore/MaterialGpu.h
//
// GPU-visible material record. Schema + offline prototype only.
//
// *** RUNTIME STATUS: NOT WIRED ***
//   No runtime code reads MaterialGpu yet.
//   No shader includes material_gpu.hglsl yet.
//   No SSBO binding is allocated yet.
//   No texture binding behavior is changed.
//   MaterialGpu-1 (reflect/contract fixture) and MaterialGpu-2
//   (first runtime upload) are future slices, gated on C++17 upgrade.
//
// GLSL mirror: shaders/include/material_gpu.hglsl
// Manifest:    tools/material_cook/material_manifest.schema.json
// Mirror gate: scripts/check-material-gpu-mirror.sh
//
// std430 contract:
//   All members are uint32_t or float (base align = 4).
//   Struct size = 32 bytes → stride = 32 bytes in a std430 array.
//   Indexed by MaterialHandle::index() (20-bit; max 1M entries).
//
// Texture index convention:
//   Each *Tex field names a slot in the future bindless/array table.
//   kMaterialTexAbsent (0xFFFFFFFF) = slot absent; shader uses default.
//   Static-prop-first table layout: indices 0..N-1 are static props;
//   mech / VFX materials follow in later slices.
//
// CHANGING THIS STRUCT REQUIRES CHANGING THE GLSL MIRROR IN LOCKSTEP.
// Invariant enforced by: scripts/check-material-gpu-mirror.sh
// Reference doc: memory/cpp_glsl_ubo_struct_lockstep.md

#pragma once

#include <cstdint>
#include <cstddef>

namespace RenderCore {

// Material flags (bit positions in MaterialGpu::flags).
namespace MaterialFlags {
    // Bit 0 — alpha-test: discard fragments with alpha < 0.5.
    // Mirrors STATIC_PROP_FLAG_ALPHA_TEST in gos_static_prop_batcher.h.
    constexpr uint32_t kAlphaTest          = 1u << 0;
    // Bit 1 — has normal map: normalTex is a valid tangent-space normal map.
    constexpr uint32_t kNormalMap          = 1u << 1;
    // Bit 2 — has metallic-roughness map: metallicRoughnessTex is valid
    //         (R=AO, G=roughness, B=metalness packed).
    constexpr uint32_t kMetallicRoughness  = 1u << 2;
    // Bit 3 — has emissive map: emissiveTex is valid.
    constexpr uint32_t kEmissive           = 1u << 3;
    // Bit 4 — double-sided: back faces are not discarded.
    constexpr uint32_t kDoubleSided        = 1u << 4;
    // Bit 5 — is window: hot-color magic applies; sun/ambient lighting
    //         skipped. Mirrors kFlagIsWindow in static_prop.vert bit 1.
    constexpr uint32_t kWindow             = 1u << 5;
    // Bits 6-31 reserved; must be 0 in cooked manifests.
} // namespace MaterialFlags

// Sentinel: texture slot is absent (no map provided for this material).
constexpr uint32_t kMaterialTexAbsent = 0xFFFFFFFFu;

// Field order matches shaders/include/material_gpu.hglsl exactly.
// Mirror gate: scripts/check-material-gpu-mirror.sh verifies field
// names and order match between the two files at CI time.
struct alignas(4) MaterialGpu {
    // --- Texture indices (4 × uint32 = 16 bytes) ---
    uint32_t albedoTex;             //  0  — diffuse / albedo (required)
    uint32_t normalTex;             //  4  — tangent-space normal map
    uint32_t metallicRoughnessTex;  //  8  — R=AO  G=roughness  B=metalness
    uint32_t emissiveTex;           // 12  — emissive color map

    // --- Control word (1 × uint32 = 4 bytes) ---
    uint32_t flags;                 // 16  — see MaterialFlags namespace

    // --- PBR scalar parameters (3 × float = 12 bytes) ---
    // Multipliers: applied to map sample when map present, used as flat
    // value when the map slot is kMaterialTexAbsent.
    float    baseColorFactor;       // 20  — [0, 1] albedo tint / brightness
    float    metallicFactor;        // 24  — [0, 1] metalness override/mult
    float    roughnessFactor;       // 28  — [0, 1] roughness override/mult
};
static_assert(sizeof(MaterialGpu) == 32,
              "MaterialGpu must be 32 bytes for clean std430 stride");
static_assert(offsetof(MaterialGpu, albedoTex)            ==  0, "albedoTex offset");
static_assert(offsetof(MaterialGpu, normalTex)            ==  4, "normalTex offset");
static_assert(offsetof(MaterialGpu, metallicRoughnessTex) ==  8, "metallicRoughnessTex offset");
static_assert(offsetof(MaterialGpu, emissiveTex)          == 12, "emissiveTex offset");
static_assert(offsetof(MaterialGpu, flags)                == 16, "flags offset");
static_assert(offsetof(MaterialGpu, baseColorFactor)      == 20, "baseColorFactor offset");
static_assert(offsetof(MaterialGpu, metallicFactor)       == 24, "metallicFactor offset");
static_assert(offsetof(MaterialGpu, roughnessFactor)      == 28, "roughnessFactor offset");

} // namespace RenderCore
