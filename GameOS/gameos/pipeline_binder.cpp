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
    switch (desc.blend) {
        case RenderCore::BlendMode::Opaque:
            glDisable(GL_BLEND);
            break;
        case RenderCore::BlendMode::AlphaTest:
            // Alpha-tested geometry uses shader discard; GL_BLEND stays off.
            glDisable(GL_BLEND);
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
}

} // namespace pipeline_binder
