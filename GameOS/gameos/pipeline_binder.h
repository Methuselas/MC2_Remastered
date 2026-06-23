// GameOS/gameos/pipeline_binder.h
//
// Apply a RenderCore::PipelineDesc to the live GL context.
//
// Lives in GameOS (not RenderCore) because RenderCore is deliberately
// GL-free. This translation unit is the only place that maps PipelineDesc
// fields to glEnable / glDisable / glUseProgram calls.
//
// Usage contract:
//   1. Call applyPipeline() to set program + blend + depth (test + write +
//      comparison function) + cull state + front-face winding + polygon-offset
//      ENABLE/DISABLE. PipelineDesc encodes depthFunc
//      (DepthFunc::GreaterEqual = reverse-Z), and applyPipeline() applies it
//      via glDepthFunc() — call sites no longer issue a separate glDepthFunc().
//   2. VAO binding, SSBO binding, and uniform upload are NOT handled here.
//      The call site retains responsibility for those.
//   3. Polygon-offset MAGNITUDE (glPolygonOffset factor/units) is NOT set here —
//      applyPipeline only toggles GL_POLYGON_OFFSET_FILL from polygonOffsetEnable.
//      The factor/units (runtime/ImGui shadowBias) stay owned by the call site,
//      paired with its existing teardown glDisable. See SHADOW-CASTER-
//      APPLYPIPELINE-ROUTING-1.
//
// glProgramName == 0 is silently skipped (registry entry not yet wired).
// Callers should assert / log separately if 0 is unexpected at runtime.
//
// dbgName: optional label for the MC2_PIPELINE_BIND_TRACE diagnostic (default
// OFF). When set + traced, emits one [PIPELINE_BIND] line per call.

#pragma once

#include "../../RenderCore/PipelineDesc.h"

namespace pipeline_binder {

void applyPipeline(const RenderCore::PipelineDesc& desc, const char* dbgName = nullptr);

} // namespace pipeline_binder
