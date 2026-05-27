// RenderCore/MaterialGpu.h
//
// GPU-visible material record.
//
// *** RUNTIME STATUS: LIVE ***
//   Static props: table upload default-ON, albedo sampling default-ON (as of 2026-05-26, v5/v7)
//   Mechs:        per-instance materialIdx substrate live (Mech-1, 2026-05-26), shader sampling
//                 pending texture model decision (see docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md)
//   Kill switches: MC2_MATERIAL_GPU=0 (disable table upload/bind/compare)
//                  MC2_MATERIAL_GPU_SAMPLE=0 (disable static-prop shader sampling)
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
// Texture field schema — per-consumer semantics
//
//   albedoTex
//     static props : GL_TEXTURE_2D_ARRAY layer index (shader-actionable;
//                   shader samples via texture(u_texArr, vec3(uv, layer)))
//     mechs        : mcTextureManager texHandle/slot — compare-only;
//                   NOT shader-actionable without a dedicated texture-model arc.
//                   CPU resolves: get_gosTextureHandle(slot) → gos handle → GL tex.
//     VFX / future : undefined until each consumer defines its own texture identity.
//
//   normalTex / metallicRoughnessTex / emissiveTex
//     Not yet wired for any consumer. kMaterialTexAbsent in all live entries.
//     Future semantics will match albedoTex once a unified texture model exists.
//
//   kMaterialTexAbsent (0xFFFFFFFF) : slot absent; shader uses default.
//
// Future direction — typed semantic (not yet implemented):
//
//   enum class MaterialTextureSemantic {
//       TextureArrayLayer,    // static-prop current model
//       TextureManagerSlot,   // mech current model (compare-only)
//       BindlessHandle,       // ARB_bindless_texture (needs AMD driver audit)
//       DescriptorIndex,      // indirection table SSBO index
//   };
//
//   A MaterialTextureSemantic field per consumer type would make the
//   kind-specific divergence explicit in code rather than in comments.
//   Prerequisite: mech texture-model arc decision.
//   See: docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md
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
    // Semantics are consumer-specific — see "Texture field schema" above.
    // Do NOT add a mech shader consumer without first defining a shader-actionable
    // texture identity for mechs. See decision doc in header comment above.
    uint32_t albedoTex;             //  0  — diffuse/albedo; KIND-SEMANTIC (see above)
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
