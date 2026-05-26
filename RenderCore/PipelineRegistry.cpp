// RenderCore/PipelineRegistry.cpp
//
// Static table of PipelineDesc entries, indexed by PipelineId.
// Add new rows here whenever PipelineRegistry.h gains a new PipelineId value;
// the static_assert below will fire if the table and enum fall out of sync.
//
// GL state encoded in this table (applied by pipeline_binder::applyPipeline):
//   depthTestEnable / depthWriteEnable / depthFunc=GreaterEqual (reverse-Z)
//   blend=Opaque or AlphaTest (GL_BLEND stays disabled for both)
//   cullMode=Back
// SSBO slots still bound explicitly in flush() (slot 2, 0, 1) — ssboBindingsMask
// documents them but does not drive binding; that is a future automation pass.
//
// AlphaTest note: static prop alpha is shader-discard (binary cutout).
// GL_BLEND stays disabled and GL_DEPTH_MASK stays GL_TRUE for both passes —
// the "alpha" split drives texture-array selection, not GL blend state.
//
// Object-ID note: color2 / objectIdWriteEnabled are false here. The static
// prop fragment shaders do not yet declare layout(location=2). Flip both
// flags when that shader output is added.

#include "PipelineRegistry.h"

#include <array>
#include <cassert>
#include <cstddef>

namespace RenderCore {

// SSBO binding slots — bit N = slot N. See PipelineDesc.h for the full table.
static constexpr uint32_t kSsboInstances = 1u << 0;   // slot 0 — per-type instance data
static constexpr uint32_t kSsboColors    = 1u << 1;   // slot 1 — per-type color data
static constexpr uint32_t kSsboPerType   = 1u << 2;   // slot 2 — hot-color s_perTypeSsbo

static constexpr uint32_t kStaticPropSsbos = kSsboInstances | kSsboColors | kSsboPerType;

// Table indexed by static_cast<size_t>(PipelineId).
// Row 0 must be the Invalid sentinel (zeroed PipelineDesc).
static std::array<PipelineDesc, static_cast<size_t>(PipelineId::Count_)> s_descs = {{

    // [0] Invalid — zeroed sentinel; returned for bad lookups.
    PipelineDesc{},

    // [1] StaticPropOpaque
    // Renders the alpha-OFF texture-array group. Full depth, no blending.
    {
        /* glProgramName       */ 0u,              // filled by bindProgram()
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z throughout MC2
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,           // TODO: flip when shader adds loc=2
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [2] StaticPropAlphaTest
    // Renders the alpha-ON texture-array group via shader discard.
    // GL blend stays disabled; depth write stays on (binary cutout writes depth).
    {
        /* glProgramName       */ 0u,
        /* blend               */ BlendMode::AlphaTest,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,            // intentional: discard ≠ blend
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

}};

static_assert(
    s_descs.size() == static_cast<size_t>(PipelineId::Count_),
    "s_descs row count must match PipelineId::Count_. "
    "Add a row for every new PipelineId value.");

// Returned for Invalid / out-of-range lookups.
static const PipelineDesc s_nullDesc{};

const PipelineDesc& getPipelineDesc(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return s_nullDesc;
    return s_descs[idx];
}

void bindProgram(PipelineId id, uint32_t glProgramName) {
    const auto idx = static_cast<size_t>(id);
    assert(idx > 0u && idx < static_cast<size_t>(PipelineId::Count_) &&
           "bindProgram: invalid PipelineId");
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return;
    s_descs[idx].glProgramName = glProgramName;
}

} // namespace RenderCore
