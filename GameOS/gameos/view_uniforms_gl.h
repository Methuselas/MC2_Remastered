#pragma once
// GameOS/gameos/view_uniforms_gl.h
//
// GL implementation of the ViewUniforms UBO upload.
// Binding: RenderCore::kViewUniformsBinding (3).
// Upload gated by MC2_VIEW_UNIFORMS env var (default OFF).
// F1-3A: upload only. No shader consumption yet (F1-3B).

#include "../../RenderCore/ViewUniforms.h"

namespace RenderCore {
    void initViewUniformsUbo();
    void uploadViewUniforms(const ViewUniforms& vu);
}
