// GameOS/gameos/pipeline_binder.cpp
//
// GL implementation of pipeline_binder::applyPipeline().
// This is the only site that translates RenderCore::PipelineDesc fields
// into live GL calls. Keep it thin — no caching, no state tracking.
// A future "diff-apply" pass can add redundant-call elision without
// changing the interface.

#include "pipeline_binder.h"

#include <GL/glew.h>

namespace pipeline_binder {

void applyPipeline(const RenderCore::PipelineDesc& desc) {
    // --- Program ---
    // 0 means not yet registered (bindProgram not called). Skip rather than
    // binding program 0, which would silently fall back to fixed-function.
    if (desc.glProgramName != 0u)
        glUseProgram(static_cast<GLuint>(desc.glProgramName));

    // --- Depth ---
    if (desc.depthTestEnable) glEnable(GL_DEPTH_TEST);
    else                       glDisable(GL_DEPTH_TEST);
    glDepthMask(desc.depthWriteEnable ? GL_TRUE : GL_FALSE);
    {
        GLenum glDepthFn = GL_LEQUAL;
        switch (desc.depthFunc) {
            case RenderCore::DepthFunc::LessEqual:    glDepthFn = GL_LEQUAL;  break;
            case RenderCore::DepthFunc::GreaterEqual: glDepthFn = GL_GEQUAL;  break;
            case RenderCore::DepthFunc::Always:       glDepthFn = GL_ALWAYS;  break;
            case RenderCore::DepthFunc::Equal:        glDepthFn = GL_EQUAL;   break;
        }
        glDepthFunc(glDepthFn);
    }

    // --- Blend ---
    // GLSTATE-BLEND-RESTORE-1: always set glBlendFunc regardless of enable/disable.
    // NVIDIA retains stale blend factors when GL_BLEND is disabled; any subsequent
    // glEnable(GL_BLEND) without a matching glBlendFunc inherits them. Resetting to
    // GL_ONE/GL_ZERO for Opaque/AlphaTest ensures a predictable neutral baseline.
    switch (desc.blend) {
        case RenderCore::BlendMode::Opaque:
            glDisable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ZERO);
            break;
        case RenderCore::BlendMode::AlphaTest:
            // Alpha-tested geometry uses shader discard; GL_BLEND stays off.
            glDisable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ZERO);
            break;
        case RenderCore::BlendMode::AlphaBlend:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case RenderCore::BlendMode::Additive:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            break;
    }

    // --- Cull ---
    switch (desc.cullMode) {
        case RenderCore::CullMode::None:
            glDisable(GL_CULL_FACE);
            break;
        case RenderCore::CullMode::Back:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;
        case RenderCore::CullMode::Front:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            break;
    }

    // --- Front-face winding ---
    // PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1: make winding explicit per
    // pipeline instead of relying on the ambient process-wide default. All
    // registered pipelines are Ccw (GL default), so this is a behavioral no-op
    // today — it stops the registered set from depending on leaked global state.
    // The legacy fixed-function face-flip path is independent of this call.
    glFrontFace(desc.frontFace == RenderCore::FrontFace::Cw ? GL_CW : GL_CCW);
}

} // namespace pipeline_binder
