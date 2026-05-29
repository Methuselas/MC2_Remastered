#include "gos_postprocess.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/vec.h"
#include "gos_hdri.h"
#include "gos_profiler.h"
#include "gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "gameos.hpp"      // gos_InvalidateRenderStateCache (RENDER_STATES v1)
#include "../../RenderWorld/RenderWorld.h"  // M1.5: IsObjectIdBufferEnabled
#include "../../RenderCore/RenderResourceRegistry.h"
#include "../../RenderCore/EngineView.h"
#include "view_uniforms_gl.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <SDL2/SDL.h>

namespace {

// M1.5 C1 fix + M3 plan-review fix: centralized scene-FBO draw-buffer
// policy. Every site that calls glDrawBuffers against sceneFBO_ routes
// through this helper. The caller passes objectIdAttachmentReady so the
// helper does not have to guess whether sceneObjectIdTex_ has been
// allocated yet (avoids GL_INVALID_VALUE when env-ON but FBO setup
// hasn't run). Callers pass `sceneObjectIdTex_ != 0` for MRT sites;
// SingleColor sites pass false.
//
// glClearBufferuiv(GL_COLOR, 2, ...) at frame-entry is ONLY safe
// after setSceneDrawBuffers(MainSceneMRT, true) has bound the 3-entry list.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 3.
enum class SceneDrawBufferMode { MainSceneMRT, SingleColor };

static void setSceneDrawBuffers(SceneDrawBufferMode mode,
                                bool objectIdAttachmentReady) {
    const bool oid =
        RenderWorld::IsObjectIdBufferEnabled() && objectIdAttachmentReady;

    if (mode == SceneDrawBufferMode::SingleColor) {
        GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, bufs);
        return;
    }

    if (oid) {
        GLenum bufs[3] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2
        };
        glDrawBuffers(3, bufs);
    } else {
        GLenum bufs[2] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1
        };
        glDrawBuffers(2, bufs);
    }
}

} // namespace

static gosPostProcess* s_postProcess = nullptr;

gosPostProcess* getGosPostProcess()
{
    return s_postProcess;
}

// Fullscreen quad vertices: 2 triangles covering NDC [-1,1]
// Each vertex: pos.x, pos.y, uv.x, uv.y
static const float kQuadVerts[] = {
    // Triangle 1
    -1.0f, -1.0f,  0.0f, 0.0f,
    1.0f, -1.0f,  1.0f, 0.0f,
    1.0f,  1.0f,  1.0f, 1.0f,
    // Triangle 2
    -1.0f, -1.0f,  0.0f, 0.0f,
    1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
};

gosPostProcess::gosPostProcess()
    : exposure_(1.0f)
    , bloomEnabled_(false)
    , fxaaEnabled_(false)
    , tonemapEnabled_(false)
    , bloomIntensity_(0.3f)
    , bloomThreshold_(0.6f)
    , sceneFBO_(0)
    , sceneColorTex_(0)
    , sceneDepthTex_(0)
    , sceneNormalTex_(0)
    , quadVAO_(0)
    , quadVBO_(0)
    , compositeProg_(nullptr)
    , skyboxProg_(nullptr)
    , bloomThresholdProg_(nullptr)
    , bloomBlurProg_(nullptr)
    , width_(0)
    , height_(0)
    , initialized_(false)
    , shadowFBO_(0)
    , shadowDepthTex_(0)
    , shadowDummyColorTex_(0)
    , shadowDepthProg_(nullptr)
    , shadowMapSize_(4096)
    , shadowsEnabled_(true)
    , staticLightMatrixBuilt_(false)
    , mapHalfExtent_(0.0f)
    , dynShadowFBO_(0)
    , dynShadowDepthTex_(0)
    , dynShadowDummyColorTex_(0)
    , dynShadowMapSize_(2048)
    , shadowDebugProg_(nullptr)
    , screenShadowProg_(nullptr)
    , screenShadowEnabled_(false)
    , screenShadowDebug_(0)
    , sceneHasTerrain_(false)
    , prevFrameHadTerrain_(false)
    , godrayEnabled_(false)  // disabled: no visible sky at RTS zoom. RAlt+6 to test.
    , godrayProg_(nullptr)
    , godrayFBO_(0)
    , godrayColorTex_(0)
    , shorelineEnabled_(true)
    , shorelineProg_(nullptr)
{
    bloomFBO_[0] = bloomFBO_[1] = 0;
    bloomColorTex_[0] = bloomColorTex_[1] = 0;
    memset(staticLightSpaceMatrix_, 0, sizeof(staticLightSpaceMatrix_));
    memset(dynamicLightSpaceMatrix_, 0, sizeof(dynamicLightSpaceMatrix_));
    memset(savedViewport_, 0, sizeof(savedViewport_));
    memset(inverseViewProj_, 0, sizeof(inverseViewProj_));
    memset(viewProj_, 0, sizeof(viewProj_));
    showShadowDebug_ = false;
    shadowDebugMode_ = 0;
    sunScreenPos_[0] = 0.5f;
    sunScreenPos_[1] = 0.5f;
}

gosPostProcess::~gosPostProcess()
{
    if (initialized_)
        destroy();
}

void gosPostProcess::init(int w, int h)
{
    ZoneScopedN("gosPostProcess::init");
    assert(!initialized_);

    width_ = w;
    height_ = h;

    createFBOs(w, h);
    createFullscreenQuad();

    // Load shaders — version provided via prefix (shader files must NOT have #version).
    // 4.3 matches the GL context requirement (SSBO + std430 used by the static-prop
    // renderer); using a lower version here worked on AMD but broke on NVIDIA which
    // defaults to GLSL 1.10 when the context/shader versions disagree at the boundary.
    static const char* kShaderPrefix = "#version 430\n";

    compositeProg_ = glsl_program::makeProgram(
        "postprocess",
        "shaders/postprocess.vert",
        "shaders/postprocess.frag",
        kShaderPrefix
    );

    if (!compositeProg_ || !compositeProg_->is_valid()) {
        fprintf(stderr, "gosPostProcess: failed to compile postprocess shader\n");
    }

    skyboxProg_ = glsl_program::makeProgram("skybox",
        "shaders/skybox.vert", "shaders/skybox.frag", kShaderPrefix);
    if (!skyboxProg_ || !skyboxProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile skybox shader\n");

    // HDRI-SKY-1 init. Gate read once; default enabled unless env var == "0".
    {
        const char* gateEnv = getenv("MC2_HDRI_SKY");
        hdriEnabled_ = !(gateEnv && gateEnv[0] == '0' && gateEnv[1] == '\0');

        if (hdriEnabled_) {
            const char* hdrPath = "data/hdr/DaySkyHDRI063B_4K.exr";
            hdriTex_ = loadHdriTexture(hdrPath);  // logs failures internally

            hdriSkyboxProg_ = glsl_program::makeProgram(
                "hdri_skybox",
                "shaders/hdri_skybox.vert",
                "shaders/hdri_skybox.frag",
                kShaderPrefix
            );

            hdriReady_ = (hdriTex_ != 0)
                      && (hdriSkyboxProg_ != nullptr)
                      && hdriSkyboxProg_->is_valid();

            if (!hdriReady_) {
                std::fprintf(stderr,
                    "[HDRI_SKY v1] enabled=0 reason=init_failed "
                    "tex=%u prog=%p valid=%d\n",
                    hdriTex_, (void*)hdriSkyboxProg_,
                    hdriSkyboxProg_ ? (int)hdriSkyboxProg_->is_valid() : 0);
            }
        } else {
            std::fprintf(stderr,
                "[HDRI_SKY v1] enabled=0 reason=env_gate MC2_HDRI_SKY=0\n");
        }
    }

    bloomThresholdProg_ = glsl_program::makeProgram("bloom_threshold",
        "shaders/postprocess.vert", "shaders/bloom_threshold.frag", kShaderPrefix);
    if (!bloomThresholdProg_ || !bloomThresholdProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile bloom_threshold shader\n");

    bloomBlurProg_ = glsl_program::makeProgram("bloom_blur",
        "shaders/postprocess.vert", "shaders/bloom_blur.frag", kShaderPrefix);
    if (!bloomBlurProg_ || !bloomBlurProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile bloom_blur shader\n");

    shadowDebugProg_ = glsl_program::makeProgram("shadow_debug",
        "shaders/postprocess.vert", "shaders/shadow_debug.frag", kShaderPrefix);
    if (!shadowDebugProg_ || !shadowDebugProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_debug shader\n");

    screenShadowProg_ = glsl_program::makeProgram("shadow_screen",
        "shaders/postprocess.vert", "shaders/shadow_screen.frag", kShaderPrefix);
    if (!screenShadowProg_ || !screenShadowProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shadow_screen shader\n");

    godrayProg_ = glsl_program::makeProgram("godray",
        "shaders/postprocess.vert", "shaders/godray.frag", kShaderPrefix);
    if (!godrayProg_ || !godrayProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile godray shader\n");

    shorelineProg_ = glsl_program::makeProgram("shoreline",
        "shaders/postprocess.vert", "shaders/shoreline.frag", kShaderPrefix);
    if (!shorelineProg_ || !shorelineProg_->is_valid())
        fprintf(stderr, "gosPostProcess: failed to compile shoreline shader\n");

    initShadows();
    initDynamicShadows();

    s_postProcess = this;
    initialized_ = true;
}

void gosPostProcess::destroy()
{
    if (!initialized_)
        return;

    destroyFBOs();
    destroyFullscreenQuad();

    if (compositeProg_) {
        glsl_program::deleteProgram("postprocess");
        compositeProg_ = nullptr;
    }

    if (skyboxProg_) {
        glsl_program::deleteProgram("skybox");
        skyboxProg_ = nullptr;
    }

    if (hdriSkyboxProg_) {
        glsl_program::deleteProgram("hdri_skybox");
        hdriSkyboxProg_ = nullptr;
    }
    if (hdriTex_) {
        glDeleteTextures(1, &hdriTex_);
        hdriTex_ = 0;
    }
    if (hdriDummyVao_) {
        glDeleteVertexArrays(1, &hdriDummyVao_);
        hdriDummyVao_ = 0;
    }
    hdriReady_ = false;
    hdriEnabled_ = false;

    if (bloomThresholdProg_) {
        glsl_program::deleteProgram("bloom_threshold");
        bloomThresholdProg_ = nullptr;
    }
    if (bloomBlurProg_) {
        glsl_program::deleteProgram("bloom_blur");
        bloomBlurProg_ = nullptr;
    }

    if (shadowDebugProg_) {
        glsl_program::deleteProgram("shadow_debug");
        shadowDebugProg_ = nullptr;
    }

    if (screenShadowProg_) {
        glsl_program::deleteProgram("shadow_screen");
        screenShadowProg_ = nullptr;
    }

    if (godrayProg_) {
        glsl_program::deleteProgram("godray");
        godrayProg_ = nullptr;
    }

    if (shorelineProg_) {
        glsl_program::deleteProgram("shoreline");
        shorelineProg_ = nullptr;
    }

    destroyShadows();
    destroyDynamicShadows();

    s_postProcess = nullptr;
    initialized_ = false;
}


void gosPostProcess::resize(int w, int h)
{
    if (w == width_ && h == height_)
        return;

    width_ = w;
    height_ = h;

    destroyFBOs();
    createFBOs(w, h);
}

void gosPostProcess::createFBOs(int w, int h)
{
    // --- Scene FBO (full resolution, HDR) ---
    glGenFramebuffers(1, &sceneFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);

    // Color attachment: RGBA16F
    glGenTextures(1, &sceneColorTex_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColorTex_, 0);

    // Depth/stencil texture (sampleable for post-process depth reconstruction)
    glGenTextures(1, &sceneDepthTex_);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                           GL_TEXTURE_2D, sceneDepthTex_, 0);

    // Normal buffer: MRT attachment 1 (rgb=world normal encoded, a=shadow skip flag)
    glGenTextures(1, &sceneNormalTex_);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sceneNormalTex_, 0);

    // M1.5: object-ID attachment-2 (GL_R32UI). Gated on
    // MC2_OBJECT_ID_BUFFER; when env-OFF we skip the texture
    // creation entirely so env-OFF runtime cost is exactly zero on
    // the FBO side. glTexImage2D matches the sceneNormalTex_ pattern
    // above (decision m4); glTexStorage2D migration deferred.
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        glGenTextures(1, &sceneObjectIdTex_);
        glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0,
                     GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,
                               GL_TEXTURE_2D, sceneObjectIdTex_, 0);
    }

    // MRT: draw to color attachments via centralized policy. Helper
    // adds GL_COLOR_ATTACHMENT2 when env-ON AND sceneObjectIdTex_
    // exists (M1.5 C1 + M3 fix).
    setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                        sceneObjectIdTex_ != 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gosPostProcess: scene FBO incomplete (0x%x)\n", status);
    }

    // --- Bloom ping-pong FBOs (half resolution) ---
    int halfW = w / 2;
    int halfH = h / 2;
    if (halfW < 1) halfW = 1;
    if (halfH < 1) halfH = 1;

    for (int i = 0; i < 2; ++i) {
        glGenFramebuffers(1, &bloomFBO_[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[i]);

        glGenTextures(1, &bloomColorTex_[i]);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, halfW, halfH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomColorTex_[i], 0);

        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gosPostProcess: bloom FBO[%d] incomplete (0x%x)\n", i, status);
        }
    }

    // --- God ray FBO (half resolution) ---
    {
        int ghw = w / 2, ghh = h / 2;
        if (ghw < 1) ghw = 1;
        if (ghh < 1) ghh = 1;

        glGenTextures(1, &godrayColorTex_);
        glBindTexture(GL_TEXTURE_2D, godrayColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ghw, ghh, 0, GL_RGBA, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &godrayFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, godrayFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, godrayColorTex_, 0);

        GLenum grStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (grStatus != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: god ray FBO incomplete (0x%x)\n", grStatus);
    }

    // --- WATER-REFLECTION-RESOURCE-1: quarter-res reflection target ---
    // Substrate only: allocated + registered, but NO producer renders into it
    // until Phase C, so the texture reads black. Color (RGBA16F) + depth (D24).
    {
        waterReflW_ = w / 4; if (waterReflW_ < 1) waterReflW_ = 1;
        waterReflH_ = h / 4; if (waterReflH_ < 1) waterReflH_ = 1;

        glGenTextures(1, &waterReflColorTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflColorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, waterReflW_, waterReflH_, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &waterReflDepthTex_);
        glBindTexture(GL_TEXTURE_2D, waterReflDepthTex_);
        // GL_FLOAT type + CLAMP_TO_EDGE wrap: match the project's other depth
        // textures (shadow maps) to avoid AMD quirks + OOB-sample wrapping traps.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, waterReflW_, waterReflH_, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &waterReflFBO_);
        glBindFramebuffer(GL_FRAMEBUFFER, waterReflFBO_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, waterReflColorTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, waterReflDepthTex_, 0);

        GLenum wrStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (wrStatus != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gosPostProcess: water reflection FBO incomplete (0x%x)\n", wrStatus);

        // Register descriptors (descriptive; this owner keeps the GL lifetime).
        RenderCore::RenderResourceDesc cdesc;
        cdesc.id        = RenderCore::RenderResourceId::WaterReflectionColor;
        cdesc.kind      = RenderCore::RenderResourceKind::Texture2D;
        cdesc.format    = RenderCore::RenderResourceFormat::RGBA16F;
        cdesc.debugName = "WaterReflectionColor";
        cdesc.width     = (uint32_t)waterReflW_;
        cdesc.height    = (uint32_t)waterReflH_;
        cdesc.glName    = waterReflColorTex_;
        cdesc.valid     = (wrStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(cdesc);

        RenderCore::RenderResourceDesc ddesc;
        ddesc.id        = RenderCore::RenderResourceId::WaterReflectionDepth;
        ddesc.kind      = RenderCore::RenderResourceKind::Texture2D;
        ddesc.format    = RenderCore::RenderResourceFormat::Depth24;
        ddesc.debugName = "WaterReflectionDepth";
        ddesc.width     = (uint32_t)waterReflW_;
        ddesc.height    = (uint32_t)waterReflH_;
        ddesc.glName    = waterReflDepthTex_;
        ddesc.valid     = (wrStatus == GL_FRAMEBUFFER_COMPLETE);
        RenderCore::registerOrUpdateRenderResource(ddesc);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gosPostProcess::destroyFBOs()
{
    if (sceneFBO_) {
        glDeleteFramebuffers(1, &sceneFBO_);
        sceneFBO_ = 0;
    }
    if (sceneColorTex_) {
        glDeleteTextures(1, &sceneColorTex_);
        sceneColorTex_ = 0;
    }
    if (sceneDepthTex_) {
        glDeleteTextures(1, &sceneDepthTex_);
        sceneDepthTex_ = 0;
    }
    if (sceneNormalTex_) {
        glDeleteTextures(1, &sceneNormalTex_);
        sceneNormalTex_ = 0;
    }
    if (sceneObjectIdTex_) {
        glDeleteTextures(1, &sceneObjectIdTex_);
        sceneObjectIdTex_ = 0;
    }
    for (int i = 0; i < 2; ++i) {
        if (bloomFBO_[i]) {
            glDeleteFramebuffers(1, &bloomFBO_[i]);
            bloomFBO_[i] = 0;
        }
        if (bloomColorTex_[i]) {
            glDeleteTextures(1, &bloomColorTex_[i]);
            bloomColorTex_[i] = 0;
        }
    }
    if (godrayColorTex_) { glDeleteTextures(1, &godrayColorTex_); godrayColorTex_ = 0; }
    if (godrayFBO_) { glDeleteFramebuffers(1, &godrayFBO_); godrayFBO_ = 0; }

    // WATER-REFLECTION-RESOURCE-1: free reflection target + mark slots invalid.
    if (waterReflColorTex_) { glDeleteTextures(1, &waterReflColorTex_); waterReflColorTex_ = 0; }
    if (waterReflDepthTex_) { glDeleteTextures(1, &waterReflDepthTex_); waterReflDepthTex_ = 0; }
    if (waterReflFBO_)      { glDeleteFramebuffers(1, &waterReflFBO_);   waterReflFBO_ = 0; }
    waterReflW_ = waterReflH_ = 0;
    {
        RenderCore::RenderResourceDesc inv;
        inv.id = RenderCore::RenderResourceId::WaterReflectionColor; inv.valid = false;
        RenderCore::registerOrUpdateRenderResource(inv);
        inv.id = RenderCore::RenderResourceId::WaterReflectionDepth;
        RenderCore::registerOrUpdateRenderResource(inv);
    }
}

void gosPostProcess::createFullscreenQuad()
{
    glGenVertexArrays(1, &quadVAO_);
    glBindVertexArray(quadVAO_);

    glGenBuffers(1, &quadVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);

    // layout(location = 0) in vec2 pos
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    // layout(location = 1) in vec2 uv
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void gosPostProcess::destroyFullscreenQuad()
{
    if (quadVBO_) {
        glDeleteBuffers(1, &quadVBO_);
        quadVBO_ = 0;
    }
    if (quadVAO_) {
        glDeleteVertexArrays(1, &quadVAO_);
        quadVAO_ = 0;
    }
}

void gosPostProcess::beginScene()
{
    if (!initialized_)
        return;

    prevFrameHadTerrain_ = sceneHasTerrain_;  // save for clear color decision
    sceneHasTerrain_ = false;  // reset each frame; set by markTerrainDrawn()

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // Bind both draw buffers so the upcoming glClear in gameosmain.cpp clears
    // both COLOR0 and COLOR1 (the GBuffer1 normal/post-shadow-mask attachment).
    // After the clear, gameosmain.cpp calls pp->clearGBuffer1() to overwrite
    // attachment 1 with the post-shadow-eligible sentinel (0.5, 0.5, 1.0, 0.0)
    // before the scene renders. MRT remains bound for the entire scene draw;
    // every Group I/II shader either writes GBuffer1 explicitly via the
    // render-contract registry helpers or relies on the pre-cleared sentinel.
    // See docs/superpowers/specs/render-contract-f3-report.md for the F3
    // coherence guarantee. AMD location=1 corruption claim refuted 2026-04-27;
    // see docs/amd-driver-rules.md "Tested-and-refuted claims".
    if (sceneNormalTex_) {
        // M1.5 C1 + M3 fix: helper takes readiness flag explicitly.
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT,
                            sceneObjectIdTex_ != 0);
    }
    // M1.5 m1 clear-order rule + M3 plan-review fix: glClearBufferuiv
    // at INDEX 2 only safe AFTER the env-ON 3-entry list is bound.
    // Guarded by the same readiness predicate that selects the 3-entry
    // list above; env-OFF byte-identical.
    if (RenderWorld::IsObjectIdBufferEnabled() && sceneObjectIdTex_) {
        static const GLuint kClearZero[4] = { 0u, 0u, 0u, 0u };
        setSceneDrawBuffers(SceneDrawBufferMode::MainSceneMRT, true);
        glClearBufferuiv(GL_COLOR, 2, kClearZero);
    }
    glViewport(0, 0, width_, height_);
}

void gosPostProcess::runBloom()
{
    if (!bloomEnabled_ || !bloomThresholdProg_ || !bloomBlurProg_) return;
    if (!bloomThresholdProg_->is_valid() || !bloomBlurProg_->is_valid()) return;

    int hw = width_ / 2, hh = height_ / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Pass 1: Threshold — extract bright pixels from scene into bloomFBO_[0]
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[0]);
    glViewport(0, 0, hw, hh);
    glClear(GL_COLOR_BUFFER_BIT);

    bloomThresholdProg_->setInt("sceneTex", 0);
    bloomThresholdProg_->setFloat("threshold", bloomThreshold_);
    bloomThresholdProg_->apply();  // flush uniforms + bind program

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Pass 2+: Ping-pong Gaussian blur (2 iterations = 4 passes)
    float texelSize[2] = { 1.0f / (float)hw, 1.0f / (float)hh };
    bloomBlurProg_->setFloat2("texelSize", texelSize);
    bloomBlurProg_->setInt("image", 0);

    bool horiz = true;
    for (int i = 0; i < 4; i++) {
        int src = horiz ? 0 : 1;
        int dst = horiz ? 1 : 0;

        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO_[dst]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[src]);
        bloomBlurProg_->setInt("horizontal", horiz ? 1 : 0);
        bloomBlurProg_->apply();  // flush uniforms + bind program each pass
        glDrawArrays(GL_TRIANGLES, 0, 6);

        horiz = !horiz;
    }
    // Result is in bloomColorTex_[0]

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void gosPostProcess::clearGBuffer1()
{
    if (!sceneNormalTex_) return;
    // Sentinel: flat-up encoded normal (0.5, 0.5, 1.0), alpha=0.0
    // (post-shadow eligible). Matches rc_gbuffer1_screenShadowEligible(vec3(0,0,1))
    // in shaders/include/render_contract.hglsl.
    static const GLfloat sentinel[4] = { 0.5f, 0.5f, 1.0f, 0.0f };
    // glClearBufferfv with buffer=GL_COLOR, drawbuffer=1 clears the
    // SECOND draw buffer of the currently bound FBO. Because beginScene()
    // calls glDrawBuffers(2, {COLOR0, COLOR1}), drawbuffer index 1 maps
    // to GL_COLOR_ATTACHMENT1 here. Caller must ensure MRT is bound.
    glClearBufferfv(GL_COLOR, 1, sentinel);
}


void gosPostProcess::runScreenShadow()
{
    ZoneScopedN("Render.ScreenShadow");
    TracyGpuZone("Render.ScreenShadow");

    if (!screenShadowEnabled_) return;
    if (!sceneHasTerrain_) return;
    if (!screenShadowProg_ || !screenShadowProg_->is_valid()) return;
    if (!shadowsEnabled_) return;

    // Render to sceneFBO_ color-only (no normal write) with multiplicative blending
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color composite. Helper preserves env-OFF/ON parity
    // (the postprocess composite never writes attachment-2 regardless).
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Multiplicative blending: dst * src (shadow darkening)
    // In debug mode, overwrite scene color entirely
    if (screenShadowDebug_ == 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
    } else {
        glDisable(GL_BLEND);
    }

    // Set uniforms BEFORE apply()
    screenShadowProg_->setInt("sceneDepthTex", 0);
    screenShadowProg_->setInt("sceneNormalTex", 1);
    screenShadowProg_->setInt("shadowMap", 2);
    screenShadowProg_->setInt("dynamicShadowMap", 3);
    screenShadowProg_->setInt("overlayPass", 0);
    screenShadowProg_->setInt("enableShadows", shadowsEnabled_ ? 1 : 0);
    screenShadowProg_->setInt("enableDynamicShadows", (dynShadowDepthTex_ != 0) ? 1 : 0);
    screenShadowProg_->setFloat("shadowSoftness", 0.9f);  // match terrain default
    screenShadowProg_->setInt("debugMode", screenShadowDebug_);
    float screenSz[2] = { (float)width_, (float)height_ };
    screenShadowProg_->setFloat2("screenSize", screenSz);
    screenShadowProg_->setFloat("time", (float)SDL_GetTicks() * 0.001f);
    screenShadowProg_->apply();

    // Upload matrices via direct GL (after apply binds the program)
    GLint loc;
    loc = glGetUniformLocation(screenShadowProg_->shp_, "inverseViewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, inverseViewProj_);
    loc = glGetUniformLocation(screenShadowProg_->shp_, "lightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, staticLightSpaceMatrix_);
    loc = glGetUniformLocation(screenShadowProg_->shp_, "dynamicLightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, dynamicLightSpaceMatrix_);

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, dynShadowDepthTex_);

    // Draw fullscreen quad — pass 1: normal (skip terrain)
    // Draw fullscreen quad - single pass for terrain, objects, and overlays.
    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);

    // Restore state
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::runGodRays()
{
    ZoneScopedN("Render.GodRays");
    TracyGpuZone("Render.GodRays");

    if (!godrayEnabled_ || !sceneHasTerrain_ || !godrayProg_ || !godrayProg_->is_valid()) {
        return;
    }
    static int gr_run = 0;
    if (gr_run++ < 3)
        fprintf(stderr, "GodRays RUNNING: sunPos=%.2f,%.2f halfRes=%dx%d\n",
            sunScreenPos_[0], sunScreenPos_[1], width_/2, height_/2);

    int hw = width_ / 2, hh = height_ / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    // Pass 1: Render god rays into half-res FBO
    glBindFramebuffer(GL_FRAMEBUFFER, godrayFBO_);
    glViewport(0, 0, hw, hh);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    float elapsed = (float)SDL_GetTicks() / 1000.0f;

    godrayProg_->setInt("sceneDepthTex", 0);
    godrayProg_->setInt("sceneColorTex", 1);
    godrayProg_->setFloat2("sunScreenPos", sunScreenPos_);
    godrayProg_->setFloat("time", elapsed);
    godrayProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Pass 2: Additive composite onto scene at full res
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color composite (additive); helper preserves env shape.
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // Additive

    // Use bloom threshold shader as pass-through (threshold = -1 passes everything)
    bloomThresholdProg_->setInt("sceneTex", 0);
    bloomThresholdProg_->setFloat("threshold", -1.0f);
    bloomThresholdProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, godrayColorTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::runShoreline()
{
    ZoneScopedN("Render.Shoreline");
    TracyGpuZone("Render.Shoreline");

    if (!shorelineEnabled_ || !sceneHasTerrain_ || !shorelineProg_ || !shorelineProg_->is_valid()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_);
    // M1.5: single-color multiplicative composite; helper preserves env shape.
    setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false);
    glViewport(0, 0, width_, height_);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    // Multiplicative blend: values > 1.0 brighten water at shoreline
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);

    shorelineProg_->setInt("sceneDepthTex", 0);
    shorelineProg_->setInt("sceneNormalTex", 1);
    float screenSz[2] = { (float)width_, (float)height_ };
    shorelineProg_->setFloat2("screenSize", screenSz);
    float elapsed = (float)SDL_GetTicks() / 1000.0f;
    shorelineProg_->setFloat("time", elapsed);
    shorelineProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sceneNormalTex_);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
}

void gosPostProcess::endScene()
{
    ZoneScopedN("Render.PostProcess");
    TracyGpuZone("Render.PostProcess");

    if (!initialized_)
        return;

    // Post-process shadow pass: covers terrain, objects, and overlays in one
    // pass, with reduced terrain darkening to avoid obvious double-shadowing.
    runScreenShadow();

    // Shoreline foam pass (brightens water pixels adjacent to terrain)
    runShoreline();

    // God rays pass (radial light scattering, additive)
    runGodRays();

    runBloom();

    // Bind default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);

    // Disable depth test and face culling for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    // gosFX/MLR additive draws (gos_Alpha_OneOne) leak GL_BLEND+GL_ONE/GL_ONE
    // state into the composite. With an RGBA8 backbuffer that clamps at 1.0,
    // the additive accumulation saturates to white over ~1s — pylon power
    // generator effect on mc2_05/mc2_24 was the canary. Composite is meant
    // to fully overwrite the backbuffer; force opaque.
    glDisable(GL_BLEND);

    // Draw fullscreen quad with composite shader
    if (compositeProg_ && compositeProg_->is_valid()) {
        // Set uniforms BEFORE apply() — apply() binds program + flushes dirty uniforms
        compositeProg_->setInt("sceneTex", 0);
        compositeProg_->setInt("bloomTex", 1);
        compositeProg_->setFloat("exposure", exposure_);
        compositeProg_->setInt("enableBloom", bloomEnabled_ ? 1 : 0);
        compositeProg_->setInt("enableFXAA", fxaaEnabled_ ? 1 : 0);
        compositeProg_->setInt("enableTonemap", tonemapEnabled_ ? 1 : 0);
        compositeProg_->setFloat("bloomIntensity", bloomIntensity_);

        float invSize[2] = { 1.0f / (float)width_, 1.0f / (float)height_ };
        compositeProg_->setFloat2("inverseScreenSize", invSize);
        compositeProg_->apply();

        // Bind scene color texture to unit 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColorTex_);

        // Bind bloom texture to unit 1 (unused for now, bind first bloom tex)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomColorTex_[0]);

        // Draw the fullscreen quad
        glBindVertexArray(quadVAO_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glActiveTexture(GL_TEXTURE0);
    }

    // Shadow debug overlay (draws on top of composite)
    drawShadowDebugOverlay();

    // Re-enable depth test
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    // RENDER_STATES v1: post-process disturbed program, depth, blend, sampler,
    // and unit-0/1 texture bindings outside applyRenderStates. Invalidate the
    // applyRenderStates cache so the next renderer (e.g. HUD/debug overlay)
    // gets a full state re-apply, not a stale-cache short-circuit.
    gos_InvalidateRenderStateCache();

    drainGLErrors("post_process");
}

void gosPostProcess::drawShadowDebugOverlay()
{
    if (!showShadowDebug_ || !shadowDebugProg_ || !shadowDebugProg_->is_valid())
        return;
    if (!initialized_)
        return;

    GLuint tex = (shadowDebugMode_ == 0) ? shadowDepthTex_ : dynShadowDepthTex_;
    if (!tex)
        return;

    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    int quadSize = 256;
    int margin = 16;
    glViewport(margin, height_ - quadSize - margin, quadSize, quadSize);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    // Temporarily switch shadow texture from comparison mode to raw depth read
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    shadowDebugProg_->setInt("shadowDebugMap", 0);
    shadowDebugProg_->apply();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // CRITICAL: restore comparison mode so PCF sampling works next frame
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);

    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void gosPostProcess::renderSkybox(float sunDirX, float sunDirY, float sunDirZ)
{
    if (!skyboxProg_ || !skyboxProg_->is_valid()) return;

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Set uniforms BEFORE apply() — apply() flushes dirty uniforms to GPU
    float sunDirVec[3] = { sunDirX, sunDirY, sunDirZ };
    float zenith[3] = { 0.18f, 0.35f, 0.72f };    // deeper blue overhead
    float horizon[3] = { 0.55f, 0.62f, 0.72f };   // desaturated blue-grey haze
    float sun[3] = { 0.9f, 0.8f, 0.6f };           // warm but subtle
    skyboxProg_->setFloat3("sunDir", sunDirVec);
    skyboxProg_->setFloat3("zenithColor", zenith);
    skyboxProg_->setFloat3("horizonColor", horizon);
    skyboxProg_->setFloat3("sunColor", sun);
    skyboxProg_->apply();

    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    // Compute sun screen position by projecting sun direction through VP matrix
    {
        float sunWorld[4] = { sunDirX * 100000.0f, sunDirY * 100000.0f, sunDirZ * 100000.0f, 1.0f };
        float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // viewProj_ is column-major
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                clip[r] += viewProj_[c * 4 + r] * sunWorld[c];
        if (clip[3] > 0.0f) {
            sunScreenPos_[0] = (clip[0] / clip[3]) * 0.5f + 0.5f;
            sunScreenPos_[1] = (clip[1] / clip[3]) * 0.5f + 0.5f;
        }
    }
}

void gosPostProcess::renderHdriSkybox(const float* viewMat, const float* projMat)
{
    if (!hdriReady_ || !hdriSkyboxProg_ || !hdriSkyboxProg_->is_valid()
        || !hdriTex_ || !viewMat || !projMat) {
        return;  // no-op: black sky baseline
    }

    // Compute the inverse projection. Convert float arrays to mat4 structs.
    // mat4 constructor takes column-major order; projMat is already column-major.
    mat4 projMat4;
    memcpy(&projMat4.elem[0][0], projMat, 16 * sizeof(float));
    mat4 invProj = inverseMat4(projMat4);
    float invProjArray[16];
    memcpy(invProjArray, &invProj.elem[0][0], 16 * sizeof(float));

    // Extract upper 3x3 of column-major viewMat and transpose
    // (transpose-of-rotation = inverse-of-rotation). Translation is
    // intentionally excluded so the sky does not parallax with the camera.
    float invViewRot[9] = {
        viewMat[0], viewMat[4], viewMat[8],
        viewMat[1], viewMat[5], viewMat[9],
        viewMat[2], viewMat[6], viewMat[10]
    };

    // Query runtime cap on color attachments so save/mask/restore
    // adapts to whichever buffers are bound (ObjectID attachment 2
    // only exists when MC2_OBJECT_ID_BUFFER is set).
    GLint maxDrawBuffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    const int nAtt = (maxDrawBuffers < 3) ? maxDrawBuffers : 3;

    // --- Save state ---
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    GLboolean prevDepthTest  = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend      = glIsEnabled(GL_BLEND);
    GLboolean prevCull       = glIsEnabled(GL_CULL_FACE);
    GLint     prevActiveTex  = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    glActiveTexture(GL_TEXTURE0);
    GLint     prevTex2DBind  = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2DBind);
    GLint     prevProgram    = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint     prevVAO        = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    // Save per-attachment color masks for whichever attachments exist.
    GLboolean prevMask[3][4] = {
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
        { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE },
    };
    for (int i = 0; i < nAtt; ++i) {
        glGetBooleani_v(GL_COLOR_WRITEMASK, (GLuint)i, prevMask[i]);
    }

    // Mask writes to attachments 1..2 (preserve normals + ObjectID).
    if (nAtt > 0) glColorMaski(0, GL_TRUE,  GL_TRUE,  GL_TRUE,  GL_TRUE);
    if (nAtt > 1) glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (nAtt > 2) glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // Bind shader + uniforms + texture.
    hdriSkyboxProg_->apply();
    hdriSkyboxProg_->setMat4("invProj", invProjArray);
    hdriSkyboxProg_->setMat3("invViewRot", invViewRot);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdriTex_);
    hdriSkyboxProg_->setInt("u_hdri", 0);

    // Fullscreen triangle. Core profile requires a non-zero VAO bound.
    if (quadVAO_ != 0) {
        glBindVertexArray(quadVAO_);
    } else {
        if (hdriDummyVao_ == 0) glGenVertexArrays(1, &hdriDummyVao_);
        glBindVertexArray(hdriDummyVao_);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // --- Restore state (exact) ---
    glBindVertexArray(prevVAO);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2DBind);
    glActiveTexture(prevActiveTex);
    glUseProgram(prevProgram);

    for (int i = 0; i < nAtt; ++i) {
        glColorMaski((GLuint)i,
            prevMask[i][0], prevMask[i][1], prevMask[i][2], prevMask[i][3]);
    }

    glDepthMask(prevDepthMask);
    if (prevDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevBlend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevCull)      glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    // Note: do NOT call glDrawBuffers anywhere in this function.
    // setSceneDrawBuffers owns the FBO draw-buffer array.
}

void gosPostProcess::initShadows()
{
    // Static shadow map covers the whole playable map in one ortho frustum, so
    // texel density is shadowMapSize²/(mapHalfExtent*2)². 4096² = 16M texels vs 2048² = 4M;
    // quadruples per-texel density, directly reduces stair-step banding on cliffs.
    shadowMapSize_ = 4096;

    static const char* kShaderPrefix = "#version 430\n";
    shadowDepthProg_ = glsl_program::makeProgram("shadow_depth",
        "shaders/shadow_depth.vert", "shaders/shadow_depth.frag", kShaderPrefix);

    glGenFramebuffers(1, &shadowFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);

    glGenTextures(1, &shadowDepthTex_);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
        shadowMapSize_, shadowMapSize_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowDepthTex_, 0);

    // Dummy color attachment — AMD drivers skip rasterization on depth-only FBOs
    glGenTextures(1, &shadowDummyColorTex_);
    glBindTexture(GL_TEXTURE_2D, shadowDummyColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
        shadowMapSize_, shadowMapSize_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadowDummyColorTex_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "gosPostProcess: shadow FBO incomplete\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Initialize with identity so shadow map reads white (no shadow)
    memset(staticLightSpaceMatrix_, 0, sizeof(staticLightSpaceMatrix_));
    staticLightSpaceMatrix_[0] = staticLightSpaceMatrix_[5] = staticLightSpaceMatrix_[10] = staticLightSpaceMatrix_[15] = 1.0f;
    staticLightMatrixBuilt_ = false;

    // Clear shadow map to max depth (1.0) so everything is "lit".
    // Reverse-Z (U2) state-safe partition: the scene sets glClearDepth(0);
    // the shadow path stays forward-Z, so set glClearDepth(1.0f)
    // explicitly here and restore the scene reverse-Z default after.
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::ShadowStaticMap;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.format    = RenderCore::RenderResourceFormat::Depth24;
        d.debugName = "ShadowStaticMap";
        d.width     = static_cast<uint32_t>(shadowMapSize_);
        d.height    = static_cast<uint32_t>(shadowMapSize_);
        d.glName    = static_cast<uint32_t>(shadowDepthTex_);
        d.sizeBytes = static_cast<uint64_t>(shadowMapSize_) * static_cast<uint64_t>(shadowMapSize_) * 4u;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

void gosPostProcess::beginShadowPass()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glGetIntegerv(GL_VIEWPORT, savedViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glViewport(0, 0, shadowMapSize_, shadowMapSize_);
    // Reverse-Z (U2) state-safe partition: shadow stays forward-Z; scene
    // set glClearDepth(0), so force 1.0f here and restore 0 after.
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);

    // Force depth test and writing ON
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    // Polygon offset to reduce shadow acne
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_);

    // Only need depth — disable color writes
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // Disable culling so both faces write depth
    glDisable(GL_CULL_FACE);
}

void gosPostProcess::beginShadowPassNoClear()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glGetIntegerv(GL_VIEWPORT, savedViewport_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO_);
    glViewport(0, 0, shadowMapSize_, shadowMapSize_);
    // NO glClear — accumulate depth from previous frames

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_CULL_FACE);
}

void gosPostProcess::endShadowPass()
{
    if (!shadowsEnabled_ || !shadowFBO_) return;

    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_); // restore to scene FBO
    glViewport(savedViewport_[0], savedViewport_[1], savedViewport_[2], savedViewport_[3]);
}

void gosPostProcess::destroyShadows()
{
    if (shadowFBO_) { glDeleteFramebuffers(1, &shadowFBO_); shadowFBO_ = 0; }
    if (shadowDepthTex_) { glDeleteTextures(1, &shadowDepthTex_); shadowDepthTex_ = 0; }
    if (shadowDummyColorTex_) { glDeleteTextures(1, &shadowDummyColorTex_); shadowDummyColorTex_ = 0; }
    if (shadowDepthProg_) {
        glsl_program::deleteProgram("shadow_depth");
        shadowDepthProg_ = nullptr;
    }

    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::ShadowStaticMap;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

// CP-1: per-mission reset of process-scoped static-shadow priming state.
// Resets the static light matrix built flag so the next mission rebuilds it
// against fresh blocks[]. The gos_*ShadowRebuild* one-shot-flag API was
// RETIRED with the move to the build-once full-map static shadow (see the
// note in gameos_graphics.cpp by gos_ResetStaticLightMatrix); the matrix
// rebuild alone re-primes the accumulation, no camera-motion trigger.
void gos_ResetStaticShadowPriming()
{
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->resetStaticLightMatrix();
}

void gosPostProcess::buildStaticLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                             float mapHalfExtent)
{
    if (!shadowsEnabled_ || !shadowFBO_) return;
    if (staticLightMatrixBuilt_) return;

    // Build world-fixed orthographic light-space matrix centered at map origin
    // sunDir points light→scene (already negated by caller)
    float len = sqrtf(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ);
    if (len < 0.001f) return;
    float fx = sunDirX/len, fy = sunDirY/len, fz = sunDirZ/len;

    // Map center is origin (0,0,0) in MC2 world space
    float r = mapHalfExtent * sqrtf(2.0f) * 1.05f;  // covers full map diagonal at any sun angle
    float lightPosX = -fx * r;
    float lightPosY = -fy * r;
    float lightPosZ = -fz * r;

    // Right = cross(forward, up_hint); Z-up for MC2
    float ux = 0, uy = 0, uz = 1;
    if (fabsf(fz) > 0.9f) { ux = 0; uy = 1; uz = 0; }

    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    len = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= len; ry /= len; rz /= len;

    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;

    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    // Ortho covers full map; near/far envelope the full elevation range.
    // Z-row emits clip-z in [0,1] (near->0, far->1) to match the engine-global
    // glClipControl(GL_ZERO_TO_ONE) set in gameosmain.cpp. Mirrors the scene
    // ortho precedent camera.cpp:2032/2037; the classic [-1,1] form clipped the
    // near half of the light frustum away (wedge atlas / half-map shadow).
    float nearP = 1.0f, farP = 2.0f * r;
    float ortho[16] = {
        1.0f/r, 0, 0, 0,
        0, 1.0f/r, 0, 0,
        0, 0, -1.0f/(farP - nearP), 0,
        0, 0, -nearP/(farP - nearP), 1
    };

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            staticLightSpaceMatrix_[col * 4 + row] = sum;
        }
    }

    {
        RenderCore::EngineView sv;
        sv.id        = RenderCore::kShadowDirectional0ViewId;
        sv.kind      = RenderCore::ViewKind::ShadowStatic;
        sv.debugName = "ShadowDirectional0-Static";
        sv.viewport[2] = shadowMapSize_;
        sv.viewport[3] = shadowMapSize_;
        memcpy(sv.viewUniforms.worldToClipGL, staticLightSpaceMatrix_,
               sizeof(sv.viewUniforms.worldToClipGL));
        RenderCore::registerOrUpdateView(sv);
    }

    fprintf(stderr, "gosPostProcess: rendering static shadows (map half-extent=%.0f)\n", mapHalfExtent);

    // [SHADOWFRUSTUM v1] VPL-#shadow: prove the static-shadow CLIPPER is
    // correct (user suspected light/clipper; render-expert grep says it's
    // feed-scope, not the matrix). INERT, env MC2_DEBUG_SHADOW_FRUSTUM,
    // one-shot (function early-returns on staticLightMatrixBuilt_ latch).
    // Input scalars only -- NO staticLightSpaceMatrix_[] slot reads (the
    // matrix-index discipline lesson). If orthoHalf == mapHalfExtent*1.485
    // and the map's real half-size, the clipper covers the FULL map ->
    // the bug is definitively the FEED (camera-windowed terrain), not the
    // light/clipper. buildCount>1 would mean the latch resets (rebuild).
    if (getenv("MC2_DEBUG_SHADOW_FRUSTUM") != nullptr) {
        static int s_buildCount = 0;
        ++s_buildCount;
        fprintf(stderr,
            "[SHADOWFRUSTUM v1] event=build n=%d sunDirIn=(%.4f,%.4f,%.4f) "
            "sunDirNorm=(%.4f,%.4f,%.4f) mapHalfExtent=%.1f orthoHalf(r)=%.1f "
            "near=%.2f far=%.2f coversWorldXY=[-%.1f,%.1f]\n",
            s_buildCount, sunDirX, sunDirY, sunDirZ, fx, fy, fz,
            mapHalfExtent, r, nearP, farP, r, r);
    }

    // [SHADOWZRANGE v1] VPL-#10: prove the [0,1] ZERO_TO_ONE conversion is
    // correct. Transform map center + 4 corners (z=0) through the COMPOSITE
    // staticLightSpaceMatrix_ (column-major: [col*4+row]); every clip.z/clip.w
    // MUST land in [0,1]. A sign error in the ortho z-row shows here as an
    // out-of-range value BEFORE any GPU round-trip. Unconditional env-gated
    // fprintf (assert is a no-op under RelWithDebInfo); one-shot via the
    // staticLightMatrixBuilt_ latch.
    if (getenv("MC2_DEBUG_SHADOW_ZRANGE") != nullptr) {
        const float* M = staticLightSpaceMatrix_;
        const float pts[5][3] = {
            { 0.0f, 0.0f, 0.0f },
            {  0.95f*r,  0.95f*r, 0.0f }, { -0.95f*r,  0.95f*r, 0.0f },
            {  0.95f*r, -0.95f*r, 0.0f }, { -0.95f*r, -0.95f*r, 0.0f }
        };
        for (int i = 0; i < 5; i++) {
            float px = pts[i][0], py = pts[i][1], pz = pts[i][2];
            float cz = M[0*4+2]*px + M[1*4+2]*py + M[2*4+2]*pz + M[3*4+2];
            float cw = M[0*4+3]*px + M[1*4+3]*py + M[2*4+3]*pz + M[3*4+3];
            float ndc = (cw != 0.0f) ? cz / cw : 0.0f;
            fprintf(stderr,
                "[SHADOWZRANGE v1] event=static pt=%d world=(%.0f,%.0f,%.0f) "
                "clipZ=%.5f clipW=%.5f ndcZ=%.5f inRange=%d\n",
                i, px, py, pz, cz, cw, ndc, (ndc >= 0.0f && ndc <= 1.0f) ? 1 : 0);
        }
    }
}

void gosPostProcess::initDynamicShadows()
{
    // Dynamic shadow covers radius=1200 around frustum center, so at 2048²
    // each texel ≈ 1.17 world units — much bigger than a mech foot, hence blocky
    // mech shadow edges. 4096² → ~0.59 world units/texel (half the step).
    dynShadowMapSize_ = 4096;

    glGenFramebuffers(1, &dynShadowFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, dynShadowFBO_);

    glGenTextures(1, &dynShadowDepthTex_);
    glBindTexture(GL_TEXTURE_2D, dynShadowDepthTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
        dynShadowMapSize_, dynShadowMapSize_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dynShadowDepthTex_, 0);

    // AMD dummy color attachment
    glGenTextures(1, &dynShadowDummyColorTex_);
    glBindTexture(GL_TEXTURE_2D, dynShadowDummyColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
        dynShadowMapSize_, dynShadowMapSize_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dynShadowDummyColorTex_, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "gosPostProcess: dynamic shadow FBO incomplete\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    memset(dynamicLightSpaceMatrix_, 0, sizeof(dynamicLightSpaceMatrix_));
    dynamicLightSpaceMatrix_[0] = dynamicLightSpaceMatrix_[5] = dynamicLightSpaceMatrix_[10] = dynamicLightSpaceMatrix_[15] = 1.0f;

    // Clear to max depth (fully lit). Reverse-Z (U2) state-safe partition:
    // dynamic shadow stays forward-Z; scene set glClearDepth(0), so force
    // 1.0f here and restore the scene reverse-Z default after.
    glBindFramebuffer(GL_FRAMEBUFFER, dynShadowFBO_);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearDepth(0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::ShadowDynamicMap;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.format    = RenderCore::RenderResourceFormat::Depth24;
        d.debugName = "ShadowDynamicMap";
        d.width     = static_cast<uint32_t>(dynShadowMapSize_);
        d.height    = static_cast<uint32_t>(dynShadowMapSize_);
        d.glName    = static_cast<uint32_t>(dynShadowDepthTex_);
        d.sizeBytes = static_cast<uint64_t>(dynShadowMapSize_) * static_cast<uint64_t>(dynShadowMapSize_) * 4u;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

void gosPostProcess::destroyDynamicShadows()
{
    if (dynShadowFBO_) { glDeleteFramebuffers(1, &dynShadowFBO_); dynShadowFBO_ = 0; }
    if (dynShadowDepthTex_) { glDeleteTextures(1, &dynShadowDepthTex_); dynShadowDepthTex_ = 0; }
    if (dynShadowDummyColorTex_) { glDeleteTextures(1, &dynShadowDummyColorTex_); dynShadowDummyColorTex_ = 0; }

    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::ShadowDynamicMap;
    RenderCore::registerOrUpdateRenderResource(invalid);
}

void gosPostProcess::buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                              const float camFitCornersMC2[8][3])
{
    if (!shadowsEnabled_ || !dynShadowFBO_) return;

    ZoneScopedN("Shadow.DynMatrixBuild");

    float len = sqrtf(sunDirX*sunDirX + sunDirY*sunDirY + sunDirZ*sunDirZ);
    if (len < 0.001f) return;
    float fx = sunDirX/len, fy = sunDirY/len, fz = sunDirZ/len;

    // --- Frustum-fit XY extent in raw-MC2 (corners supplied by caller).
    // The map-bounds clamp below (r, mirrors the static path) is the
    // footprint safety net that bounds the low-sun "frustum misses ground"
    // case; no separate elevation slab is needed for an XY ortho fit.
    float minX =  1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int c = 0; c < 8; ++c) {
        float x = camFitCornersMC2[c][0];
        float y = camFitCornersMC2[c][1];
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
    }
    float r = mapHalfExtent_ * sqrtf(2.0f) * 1.05f;   // mirror static path's r
    if (minX < -r) minX = -r; if (maxX > r) maxX = r;
    if (minY < -r) minY = -r; if (maxY > r) maxY = r;
    float cx = 0.5f * (minX + maxX);
    float cy = 0.5f * (minY + maxY);
    float halfX = 0.5f * (maxX - minX);
    float halfY = 0.5f * (maxY - minY);
    float fitRadius = (halfX > halfY ? halfX : halfY);
    if (fitRadius < 64.0f) fitRadius = 64.0f;
    if (fitRadius > r)     fitRadius = r;
    float xyRadius = 64.0f;
    while (xyRadius < fitRadius) xyRadius *= 2.0f;     // pow2 anti-shimmer
    if (xyRadius > r) xyRadius = r;
    float worldUnitsPerTexel = (2.0f * xyRadius) / (float)dynShadowMapSize_;
    float camX = floorf(cx / worldUnitsPerTexel) * worldUnitsPerTexel;
    float camY = floorf(cy / worldUnitsPerTexel) * worldUnitsPerTexel;
    float camZ = 0.0f;
    float depthDist = 5000.0f;

    float lightPosX = camX - fx * depthDist;
    float lightPosY = camY - fy * depthDist;
    float lightPosZ = camZ - fz * depthDist;

    float ux = 0, uy = 0, uz = 1;
    if (fabsf(fz) > 0.9f) { ux = 0; uy = 1; uz = 0; }

    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    len = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= len; ry /= len; rz /= len;

    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;

    float view[16] = {
         rx,  ux, -fx, 0,
         ry,  uy, -fy, 0,
         rz,  uz, -fz, 0,
        -(rx*lightPosX + ry*lightPosY + rz*lightPosZ),
        -(ux*lightPosX + uy*lightPosY + uz*lightPosZ),
         (fx*lightPosX + fy*lightPosY + fz*lightPosZ),
        1
    };

    // Z-row emits clip-z in [0,1] (near->0, far->1) for glClipControl
    // (GL_ZERO_TO_ONE), lockstep with buildStaticLightMatrix above and the
    // .xy-only sampler remap in shadow.hglsl / shadow_screen.frag.
    float nearP = 1.0f, farP = 2.0f * depthDist;
    float ortho[16] = {
        1.0f/xyRadius, 0, 0, 0,
        0, 1.0f/xyRadius, 0, 0,
        0, 0, -1.0f/(farP - nearP), 0,
        0, 0, -nearP/(farP - nearP), 1
    };

    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0;
            for (int k = 0; k < 4; k++)
                sum += ortho[k * 4 + row] * view[col * 4 + k];
            dynamicLightSpaceMatrix_[col * 4 + row] = sum;
        }
    }

    {
        RenderCore::EngineView dv;
        dv.id        = RenderCore::kShadowDynamicViewId;
        dv.kind      = RenderCore::ViewKind::ShadowDynamic;
        dv.debugName = "ShadowDynamic";
        dv.viewport[2] = dynShadowMapSize_;
        dv.viewport[3] = dynShadowMapSize_;
        memcpy(dv.viewUniforms.worldToClipGL, dynamicLightSpaceMatrix_,
               sizeof(dv.viewUniforms.worldToClipGL));
        RenderCore::registerOrUpdateView(dv);
    }

    // [SHADOWZRANGE v1] VPL-#10: dynamic-path [0,1] verification. Rebuilds
    // per frame -> one-shot via static counter. Transforms the snapped camera
    // center + offsets through dynamicLightSpaceMatrix_; clip.z/clip.w MUST be
    // in [0,1] (lockstep with the static probe + the .xy-only sampler remap).
    if (getenv("MC2_DEBUG_SHADOW_ZRANGE") != nullptr) {
        static int s_dynN = 0;
        if (++s_dynN <= 1) {
            const float* M = dynamicLightSpaceMatrix_;
            const float pts[3][3] = {
                { camX, camY, camZ },
                { camX + 0.95f*xyRadius, camY + 0.95f*xyRadius, camZ },
                { camX - 0.95f*xyRadius, camY - 0.95f*xyRadius, camZ }
            };
            for (int i = 0; i < 3; i++) {
                float px = pts[i][0], py = pts[i][1], pz = pts[i][2];
                float cz = M[0*4+2]*px + M[1*4+2]*py + M[2*4+2]*pz + M[3*4+2];
                float cw = M[0*4+3]*px + M[1*4+3]*py + M[2*4+3]*pz + M[3*4+3];
                float ndc = (cw != 0.0f) ? cz / cw : 0.0f;
                fprintf(stderr,
                    "[SHADOWZRANGE v1] event=dynamic pt=%d world=(%.0f,%.0f,%.0f) "
                    "clipZ=%.5f clipW=%.5f ndcZ=%.5f inRange=%d\n",
                    i, px, py, pz, cz, cw, ndc, (ndc >= 0.0f && ndc <= 1.0f) ? 1 : 0);
            }
        }
    }

    // Compute sun screen position for god rays (project sun direction through VP matrix)
    float sunWorld[4] = { fx * 100000.0f, fy * 100000.0f, fz * 100000.0f, 1.0f };
    float clip[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            clip[r] += viewProj_[c * 4 + r] * sunWorld[c];
    if (clip[3] > 0.0f) {
        sunScreenPos_[0] = (clip[0] / clip[3]) * 0.5f + 0.5f;
        sunScreenPos_[1] = (clip[1] / clip[3]) * 0.5f + 0.5f;
    }
}
