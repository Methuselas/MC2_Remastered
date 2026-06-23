// RenderCore/PipelineDesc.h
//
// Non-owning description of all fixed-function GL pipeline state a draw call
// requires. Header-only; no runtime dispatch yet.
//
// Purpose: name the implicit render contracts so future DrawPacket sorting
// and editor debug rendering have a typed handle to build cache keys from.
// Do NOT swap live render paths to use this type until DrawPacket dispatch
// is wired (a future slice).
//
// BlendMode values are intentionally identical to
// render_contract::PassStateContract::BlendMode so callers can cast freely.
// A static_assert enforcing this lives in mclib/render_contract.cpp when
// MC2_RENDER_CONTRACT_ASSERT is active.
//
// SSBO binding slots referenced by ssboBindingsMask (bit N = slot N):
//   8  = SUBSTRATE         (gpu_cull_substrate.cpp)
//   9  = DEBUG             (gpu_cull_compute.cpp)
//  14  = READBACK          (gpu_cull_readback.h)
//  16  = BASE_INSTANCE     (gos_static_prop_batcher.cpp)
//  20  = LIGHT_DATA        (gameos.hpp / lighting.hglsl)
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md §6

#pragma once

#include <cstdint>

namespace RenderCore {

// Must stay value-identical to render_contract::PassStateContract::BlendMode.
enum class BlendMode : uint8_t { Opaque, AlphaBlend, AlphaTest, Additive };

enum class CullMode  : uint8_t { None, Back, Front };

// Front-face winding. GL default is Ccw (GL_CCW); MC2 has used the ambient
// process-wide default and never authored it (only the shadow pass
// save/restores it). PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1 makes it an
// explicit per-pipeline row so registered pipelines stop depending on leaked
// global state. NOTE: the legacy fixed-function path encodes winding by flipping
// the CULLED FACE (not glFrontFace) — that path is untouched by this field.
enum class FrontFace : uint8_t { Ccw, Cw };

// Depth comparison function. Matches GL_LESS / GL_GEQUAL etc. without
// pulling in GL headers. LessEqual is the conventional forward-Z default;
// GreaterEqual is the reverse-Z default used throughout MC2.
enum class DepthFunc : uint8_t { LessEqual, GreaterEqual, Always, Equal };

// Which GL_COLOR_ATTACHMENTx slots must be non-NONE in the active FBO.
// Mirrors render_contract::RequiredAttachments; kept separate to avoid an
// mclib → rendercore include cycle.
struct ColorAttachmentMask {
    bool color0;   // GL_COLOR_ATTACHMENT0 — albedo / HDR scene color
    bool color1;   // GL_COLOR_ATTACHMENT1 — GBuffer normal + post-shadow mask
    bool color2;   // GL_COLOR_ATTACHMENT2 — R32_UINT object ID (M1.5+)
};

struct PipelineDesc {
    // GL program object name (same underlying type as GLuint). Non-owning.
    // Zero = unset / invalid. Matches DrawPacket::pipelineId when used as
    // a cache key (DrawPacket sorts by this in the upper bits of sortKey).
    uint32_t            glProgramName;

    BlendMode           blend;
    bool                depthTestEnable;
    bool                depthWriteEnable;
    DepthFunc           depthFunc;   // applied by applyPipeline(); GreaterEqual = reverse-Z
    CullMode            cullMode;

    ColorAttachmentMask colorAttachments;
    // True when the fragment shader declares layout(location=2) out uint
    // for object-ID. Distinct from colorAttachments.color2: a pass can
    // require color2 bound without writing it (terrain does this).
    bool                objectIdWriteEnabled;

    // Front-face winding for this pipeline (applied by applyPipeline() via
    // glFrontFace). All registered pipelines are Ccw (the GL default) today;
    // the field exists so the row states the truth instead of relying on
    // ambient global state. Occupies one of the 3 padding bytes before
    // ssboBindingsMask — no struct growth.
    FrontFace           frontFace;

    // Bit N set → SSBO binding slot N is required. Covers slots 0-31.
    // See binding table in the file header above.
    uint32_t            ssboBindingsMask;
};

// v1 added DepthFunc (1 byte after depthWriteEnable) which pushes
// ssboBindingsMask to offset 16 after natural padding.  FrontFace (v2) drops
// into that same padding window, so the struct stays at 20 bytes — the
// intentional budget; keep repacking off unless the struct grows further.
static_assert(sizeof(PipelineDesc) <= 20,
              "PipelineDesc must stay small; it lives in hot-path cache entries.");

} // namespace RenderCore
