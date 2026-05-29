// RenderCore/PipelineRegistry.h
//
// Named pipeline IDs and their PipelineDesc lookup.
// Not consumed by live render paths yet — exists so DrawPacket emitters
// can carry a typed identity instead of pipelineId=0.
//
// Lifecycle:
//   1. Engine reads getPipelineDesc(id) for static GL-state contracts
//      (blend, depth, cull, attachments, ssboBindingsMask) at any time.
//   2. Renderer calls bindProgram(id, glName) once during GL init to wire
//      the actual program object into the table.
//   3. DrawPacket::pipelineId = static_cast<uint32_t>(PipelineId::Foo).
//
// Extending: add enum values before Count_, then add a matching row to
// the s_descs initializer in PipelineRegistry.cpp. The static_assert in
// that file will catch any mismatch at compile time.

#pragma once

#include <cstdint>
#include "PipelineDesc.h"

namespace RenderCore {

enum class PipelineId : uint32_t {
    Invalid             = 0,
    StaticPropOpaque    = 1,   // opaque geometry, alpha-off group
    StaticPropAlphaTest = 2,   // alpha-tested geometry, alpha-on group (shader discard)
    MechOpaque          = 3,   // GPU mech batcher opaque pass (reverse-Z, cull back)
    // Future: Terrain, Water, DebugWireframe, ...
    Count_              = 4,   // sentinel — do not use as an ID
};

// Return the static GL-state contract for id.
// glProgramName is 0 until bindProgram() is called for that id.
// Out-of-range or Invalid returns a zeroed sentinel (all false, all 0).
const PipelineDesc& getPipelineDesc(PipelineId id);

// Wire in the actual GL program object name from the renderer at GL init.
// Must be called before any DrawPacket using this id is dispatched.
// No-op (with assert) if id is Invalid or out-of-range.
void bindProgram(PipelineId id, uint32_t glProgramName);

} // namespace RenderCore
