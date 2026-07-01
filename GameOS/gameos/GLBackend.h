// GameOS/gameos/GLBackend.h
//
// Concrete GL implementation of RenderCore::IRenderBackend. Lives in GameOS (not
// RenderCore) because RenderCore is GL-free by construction — same split as
// PipelineDesc -> pipeline_binder. This is the ONLY place IRenderBackend's ops
// map to real gl* calls.
//
// getGLBackend() returns a process-wide singleton. First boundary slice
// (RENDER-BACKEND-IFACE-POSTPROCESS-1) implements only bindBackbuffer(); the
// interface grows one edge at a time.

#pragma once

#include "RenderCore/IRenderBackend.h"

namespace RenderCore {

// Process-wide GL backend accessor. The returned reference outlives all callers.
IRenderBackend& getGLBackend();

}  // namespace RenderCore
