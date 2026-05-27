#pragma once
// GameOS/gameos/view_uniforms_gl.h
//
// GL implementation of the ViewUniforms UBO upload.
// Binding: RenderCore::kViewUniformsBinding (3).
// Upload gated by MC2_VIEW_UNIFORMS env var (default OFF).
// F1-3A: upload only. No shader consumption yet (F1-3B).
// F1-4A: setCurrentView/getCurrentView added; s_currentView stores active view.

#include "../../RenderCore/ViewUniforms.h"
#include "../../RenderCore/EngineView.h"

namespace RenderCore {
    void initViewUniformsUbo();
    void uploadViewUniforms(const ViewUniforms& vu);

    // Set/get current frame's active view. setCurrentView also calls uploadViewUniforms.
    void setCurrentView(const EngineView& view);
    const EngineView& getCurrentView();
}
