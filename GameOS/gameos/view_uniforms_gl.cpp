// GameOS/gameos/view_uniforms_gl.cpp
//
// GL implementation: init and upload of the ViewUniforms UBO.
// UBO binding: RenderCore::kViewUniformsBinding (3).
// Gate: MC2_VIEW_UNIFORMS env var (default OFF).
// F1-3A: upload only. No shader consumption yet (F1-3B).
//
// No explicit cleanup: GL context lifetime == process lifetime in this engine.
// Pattern mirrors gpu_cull_compute.cpp s_frustumUbo.

#include "view_uniforms_gl.h"

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>

// Renderer-lifetime persistent. Pattern: static GLuint, zero == not yet allocated.
static GLuint            s_viewUniformsUbo = 0;
static int               s_vuFrame         = 0;
static RenderCore::EngineView s_currentView{};

void RenderCore::initViewUniformsUbo() {
    glGenBuffers(1, &s_viewUniformsUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_viewUniformsUbo);
    glBufferStorage(GL_UNIFORM_BUFFER, sizeof(RenderCore::ViewUniforms), nullptr,
                    GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, RenderCore::kViewUniformsBinding, s_viewUniformsUbo);
    fprintf(stderr, "[VIEW_UNIFORMS v1] event=init binding=%u size=%zu\n",
            RenderCore::kViewUniformsBinding, sizeof(RenderCore::ViewUniforms));
    fflush(stderr);
}

void RenderCore::uploadViewUniforms(const RenderCore::ViewUniforms& vu) {
    if (s_viewUniformsUbo == 0)
        initViewUniformsUbo();  // lazy init; explicit call site preferred
    // Conservative rebind: binding 3 could be stolen by future paths.
    glBindBufferBase(GL_UNIFORM_BUFFER, RenderCore::kViewUniformsBinding, s_viewUniformsUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, s_viewUniformsUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(RenderCore::ViewUniforms), &vu);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    ++s_vuFrame;
    if (s_vuFrame == 1 || s_vuFrame % 600 == 0) {
        fprintf(stderr, "[VIEW_UNIFORMS v1] frame=%d uploaded=1 binding=%u size=%zu\n",
                s_vuFrame, RenderCore::kViewUniformsBinding,
                sizeof(RenderCore::ViewUniforms));
        fflush(stderr);
    }
}

void RenderCore::setCurrentView(const RenderCore::EngineView& view) {
    s_currentView = view;
    uploadViewUniforms(view.viewUniforms);
    // rate-limited log: frame 1 + every 600
    static int s_evFrame = 0;
    ++s_evFrame;
    if (s_evFrame == 1 || s_evFrame % 600 == 0) {
        fprintf(stderr, "[ENGINE_VIEW v1] frame=%d id=%u name=%s\n",
                s_evFrame, view.id, view.debugName ? view.debugName : "?");
        fflush(stderr);
    }
}

const RenderCore::EngineView& RenderCore::getCurrentView() {
    return s_currentView;
}
