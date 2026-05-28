#pragma once
// GameOS/gameos/view_uniforms_gl.h
//
// GL implementation of the ViewUniforms UBO upload.
// Binding: RenderCore::kViewUniformsBinding (3).
// Upload gated by MC2_VIEW_UNIFORMS env var (default OFF).
// F1-3A: upload only. No shader consumption yet (F1-3B).
// F1-4A: setCurrentView/getCurrentView added; s_currentView stores active view.
// F1-4B: setCurrentView is now store-only (no upload); upload stays in caller.
//        resolveView(viewId) added for VisibilityRequest wiring.

#include "../../RenderCore/ViewUniforms.h"
#include "../../RenderCore/EngineView.h"

namespace RenderCore {
    void initViewUniformsUbo();
    void uploadViewUniforms(const ViewUniforms& vu);

    // Store the current frame's active view (store-only; does NOT upload UBO).
    // F1-4B: decoupled from uploadViewUniforms so EngineView is always registered
    // regardless of the MC2_VIEW_UNIFORMS gate.
    void setCurrentView(const EngineView& view);
    const EngineView& getCurrentView();

    // Returns pointer to registered EngineView for the given id, or nullptr if unknown.
    // kMainSceneViewId always resolves if setCurrentView has been called this frame.
    const EngineView* resolveView(ViewId viewId);

    // View registry: upsert by id, read back by index.
    // Thread-unsafe — call only from the render thread.
    void registerOrUpdateView(const EngineView& view);
    uint32_t getViewCount();
    const EngineView* getViewByIndex(uint32_t index);
}
