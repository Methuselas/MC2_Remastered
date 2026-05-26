// GameOS/gameos/pipeline_binder.h
//
// Apply a RenderCore::PipelineDesc to the live GL context.
//
// Lives in GameOS (not RenderCore) because RenderCore is deliberately
// GL-free. This translation unit is the only place that maps PipelineDesc
// fields to glEnable / glDisable / glUseProgram calls.
//
// Usage contract:
//   1. Call applyPipeline() to set program + blend + depth + cull state.
//   2. If the pass requires a non-default depth function (e.g. GL_GEQUAL
//      for reverse-Z), call glDepthFunc() immediately after — PipelineDesc
//      does not encode depth comparison function yet.
//   3. VAO binding, SSBO binding, and uniform upload are NOT handled here.
//      The call site retains responsibility for those.
//
// glProgramName == 0 is silently skipped (registry entry not yet wired).
// Callers should assert / log separately if 0 is unexpected at runtime.

#pragma once

#include "../../RenderCore/PipelineDesc.h"

namespace pipeline_binder {

void applyPipeline(const RenderCore::PipelineDesc& desc);

} // namespace pipeline_binder
