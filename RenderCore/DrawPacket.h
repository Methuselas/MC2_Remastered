// RenderCore/DrawPacket.h
//
// Slice M1: documentary only. The struct exists so future slices can
// land DrawPacket-based dispatch without re-litigating the type. The
// M1 backend continues to emit `glDrawElementsIndirect` via the
// existing GpuStaticPropBatcher::flush() path; no packets are
// dispatched at runtime in M1.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 6 (draw packet model).

#pragma once

#include <cstdint>
#include "Handle.h"

namespace RenderCore {

// Sort-key layout per spec section 6 (proposed; documented for the next
// slice that actually sorts by it). NOT consumed in M1.
//
// [63:60]  pass priority   (opaque=0, alpha=8, overlay=12)
// [59:56]  view priority   (main=0, shadow=4, minimap=8)
// [55:32]  pipeline id     (24 bits, group key into PipelineDesc cache)
// [31:16]  material id     (16-bit hash bucket per spec MINOR m1)
// [15:0]   depth bucket    (alpha back-to-front; opaque inverted)
struct DrawPacket {
    uint32_t       pipelineId;    // static_cast<uint32_t>(PipelineId) — NOT glProgramName; see PipelineRegistry.h
    MeshHandle     mesh;
    MaterialHandle material;
    uint32_t       objectIndex;
    uint32_t       lightIndex;
    uint32_t       firstIndex;
    uint32_t       indexCount;
    uint32_t       instanceCount;
    uint64_t       sortKey;
};

static_assert(sizeof(DrawPacket) <= 64,
              "DrawPacket should fit in one cache line for hot-loop emission.");

} // namespace RenderCore
