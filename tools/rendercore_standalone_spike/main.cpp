// tools/rendercore_standalone_spike/main.cpp
// RENDERCORE-STANDALONE-INIT-0 — the Backend-A-vs-B spike.
//
// Brings up MC2's RenderCore view + pipeline path with NO Mission, NO
// GameObjectManager, NO game code: just an SDL2 GL 4.4 context, the
// mc2host::HostServices seam (Slice 1), and three engine TUs
// (view_uniforms_gl.cpp, pipeline_binder.cpp, RenderCore/PipelineRegistry.cpp).
//
// The fact that this exe LINKS at all is the load-bearing result: it proves
// the view+pipeline substrate has zero link dependency on Mission/game/Stuff,
// so Backend A (real RenderCore in the asset viewer) is viable.
//
// At runtime it: inits the ViewUniforms UBO, fills a ViewUniforms+EngineView
// (camera-as-data, no Camera class), uploads + registers the view, applies a
// PipelineDesc, then renders a pass marker into an offscreen FBO and reads the
// center pixel back. Exit 0 = all checks pass; nonzero = failure (the harness
// IS the test; assertions are the Slice-2 exit criteria).
//
// Design ref: docs/engine-standalone-seams.md Slice 2.

#define SDL_MAIN_HANDLED          // we own main(); no SDL2main / WinMain shim
#include <SDL.h>
#include <GL/glew.h>

#include "HostServices.h"
#include "DefaultHostServices.h"
#include "ViewUniforms.h"
#include "EngineView.h"
#include "view_uniforms_gl.h"
#include "pipeline_binder.h"
#include "PipelineRegistry.h"
#include "PipelineDesc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace mc2host;

namespace {

ILogger* g_log       = nullptr;
int      g_failures  = 0;

void check(bool cond, const char* what) {
    if (cond) {
        g_log->log(ILogger::Level::Info, "SPIKE", what);
    } else {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "FAIL: %s", what);
        g_log->log(ILogger::Level::Error, "SPIKE", buf);
        ++g_failures;
    }
}

// Returns true if the GL error queue is clean; logs and flags otherwise.
bool glClean(const char* where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "glGetError=0x%04X at %s", e, where);
        g_log->log(ILogger::Level::Error, "SPIKE", buf);
        return false;
    }
    return true;
}

void setIdentity(float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

bool near8(unsigned char got, float want01) {
    const int w = static_cast<int>(want01 * 255.0f + 0.5f);
    return std::abs(static_cast<int>(got) - w) <= 2;  // BC/rounding tolerance
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    HostServices host = defaultHostServices();
    g_log = host.log;
    g_log->log(ILogger::Level::Info, "SPIKE",
               "RENDERCORE-STANDALONE-INIT-0: begin (no Mission, no game code)");

    // --- 1. SDL2 + GL 4.4 core context, offscreen (hidden window) ----------
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        g_log->log(ILogger::Level::Error, "SPIKE", SDL_GetError());
        return 2;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);   // glBufferStorage is GL 4.4
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    const int kW = 64, kH = 64;
    SDL_Window* win = SDL_CreateWindow(
        "rendercore-spike", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kW, kH, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { g_log->log(ILogger::Level::Error, "SPIKE", SDL_GetError()); SDL_Quit(); return 2; }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        g_log->log(ILogger::Level::Error, "SPIKE", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 2;
    }
    SDL_GL_MakeCurrent(win, ctx);

    glewExperimental = GL_TRUE;
    const GLenum ge = glewInit();
    if (ge != GLEW_OK) {
        g_log->log(ILogger::Level::Error, "SPIKE",
                   reinterpret_cast<const char*>(glewGetErrorString(ge)));
        SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit(); return 2;
    }
    glGetError();  // swallow GLEW's spurious INVALID_ENUM on core profiles

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "GL_VERSION=%s  GL_RENDERER=%s",
                      reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                      reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        g_log->log(ILogger::Level::Info, "SPIKE", buf);
    }
    check(glBufferStorage != nullptr, "GL 4.4 glBufferStorage entry point present");

    // --- 2. Consume the Slice-1 host seam (config gate) --------------------
    const bool verbose = host.config->flag("MC2_SPIKE_VERBOSE", false);
    check(true, verbose ? "config seam read (verbose=on)" : "config seam read (verbose=off)");

    // --- 3. RenderCore ViewUniforms UBO init (no Mission) ------------------
    RenderCore::initViewUniformsUbo();
    check(glClean("initViewUniformsUbo"), "initViewUniformsUbo: no GL error");

    // --- 4. Camera-as-data: ViewUniforms + EngineView, upload + register ---
    RenderCore::ViewUniforms vu{};
    setIdentity(vu.worldToClipGL);
    setIdentity(vu.worldToViewGL);
    vu.cameraWorldPos[0] = 0.0f;
    vu.cameraWorldPos[1] = 0.0f;
    vu.cameraWorldPos[2] = 10.0f;
    vu.cameraWorldPos[3] = 0.0f;

    RenderCore::EngineView ev{};
    ev.id           = RenderCore::kMainSceneViewId;
    ev.viewUniforms = vu;
    ev.viewport[0]  = 0;  ev.viewport[1] = 0;
    ev.viewport[2]  = kW; ev.viewport[3] = kH;
    ev.debugName    = "spike-main-view";
    ev.kind         = RenderCore::ViewKind::MainScene;
    ev.mode         = RenderCore::ViewMode::Visual;

    RenderCore::setCurrentView(ev);
    RenderCore::uploadViewUniforms(vu);
    check(glClean("uploadViewUniforms"), "uploadViewUniforms: no GL error");
    check(RenderCore::resolveView(RenderCore::kMainSceneViewId) != nullptr,
          "EngineView resolvable by id after setCurrentView");
    check(RenderCore::getViewCount() >= 1, "view registry holds the registered view");

    // --- 5. PipelineDesc -> GL state, no Mission ---------------------------
    const RenderCore::PipelineDesc& pd =
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque);
    pipeline_binder::applyPipeline(pd);
    check(glClean("applyPipeline"), "applyPipeline(StaticPropOpaque): no GL error");

    // --- 6. Pass marker into an offscreen FBO, then read back --------------
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kW, kH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "offscreen FBO is framebuffer-complete");

    const float cr = 0.25f, cg = 0.50f, cb = 0.75f;
    glViewport(0, 0, kW, kH);
    glClearColor(cr, cg, cb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();

    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(kW / 2, kH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    check(glClean("glReadPixels"), "glReadPixels: no GL error");

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "center pixel = (%u,%u,%u,%u)",
                      px[0], px[1], px[2], px[3]);
        g_log->log(ILogger::Level::Info, "SPIKE", buf);
    }
    check((px[0] | px[1] | px[2]) != 0, "center pixel is non-black");
    check(near8(px[0], cr) && near8(px[1], cg) && near8(px[2], cb),
          "center pixel matches the clear color");

    // --- 7. Clean shutdown -------------------------------------------------
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    check(glClean("teardown"), "GL clean at teardown");

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (g_failures == 0) {
        g_log->log(ILogger::Level::Info, "SPIKE",
                   "ALL CHECKS PASSED: RenderCore view+pipeline init is standalone "
                   "(no Mission, no game code) -- Backend A viable");
        return 0;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "SPIKE FAILED: %d check(s) failed", g_failures);
    g_log->log(ILogger::Level::Error, "SPIKE", buf);
    return 1;
}
