// tools/asset_viewer/MaterialPreviewPBR.cpp
#include "MaterialPreviewPBR.h"
#include "imgui.h"
#include <GL/glew.h>
#include <cmath>
#include <vector>

static void perspective(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i=0;i<16;i++) m[i]=0;
    m[0]=f/aspect; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1.0f; m[14]=(2*zf*zn)/(zn-zf);
}
static void lookAt(const float eye[3], const float ctr[3], const float up[3], float m[16]) {
    float f[3]={ctr[0]-eye[0],ctr[1]-eye[1],ctr[2]-eye[2]};
    float fl=std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); f[0]/=fl;f[1]/=fl;f[2]/=fl;
    float s[3]={f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0]};
    float sl=std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); s[0]/=sl;s[1]/=sl;s[2]/=sl;
    float u[3]={s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0]};
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];  m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];  m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[3]=0;m[7]=0;m[11]=0;m[15]=1;
}
static void mul4(const float a[16], const float b[16], float o[16]) {  // o = a*b (column-major)
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) {
        o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
    }
}

MaterialPreviewPBR::MaterialPreviewPBR() : backend_(std::make_unique<LocalPbrMaterialBackend>()) {}
MaterialPreviewPBR::~MaterialPreviewPBR() {
    if (slots_.baseColor) glDeleteTextures(1, &slots_.baseColor);
    if (slots_.normal)    glDeleteTextures(1, &slots_.normal);
    if (slots_.orm)       glDeleteTextures(1, &slots_.orm);
    if (slots_.emissive)  glDeleteTextures(1, &slots_.emissive);
    destroyFbo();
    if (glReady_) { mesh_.destroyGL(); backend_->shutdown(); }
}

void MaterialPreviewPBR::setSource(const std::string& path) { fitPath_ = path; /* Task 8 */ }

void MaterialPreviewPBR::setSlotTexture(MaterialSlotKind kind, uint32_t glTex) {
    uint32_t* dst = nullptr;
    switch (kind) {
      case MaterialSlotKind::BaseColor: dst = &slots_.baseColor; break;
      case MaterialSlotKind::Normal:    dst = &slots_.normal;    break;
      case MaterialSlotKind::Orm:       dst = &slots_.orm;       break;
      case MaterialSlotKind::Emissive:  dst = &slots_.emissive;  break;
    }
    if (dst) { if (*dst) glDeleteTextures(1, dst); *dst = glTex; }
}

void MaterialPreviewPBR::ensureGL(int w, int h) {
    if (!glReady_) {
        backendOk_ = backend_->init();                 // review fix MAJOR 6: capture result
        mesh_.generate(1.0f, 48, 96); mesh_.uploadGL();
        glReady_ = true;
    }
    if (w != fboW_ || h != fboH_ || !fbo_) {
        destroyFbo();
        fboW_ = w; fboH_ = h;
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glGenTextures(1, &colorTex_);
        glBindTexture(GL_TEXTURE_2D, colorTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
        glGenRenderbuffers(1, &depthRbo_);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRbo_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo_);
        fboComplete_ = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        if (!fboComplete_) fprintf(stderr, "[MaterialPreviewPBR] FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void MaterialPreviewPBR::destroyFbo() {
    if (depthRbo_) glDeleteRenderbuffers(1, &depthRbo_);
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (fbo_)      glDeleteFramebuffers(1, &fbo_);
    fbo_ = colorTex_ = depthRbo_ = 0;
}

void MaterialPreviewPBR::buildViewProj(int w, int h, float out[16], float camPosOut[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_), cy = std::cos(yaw_), sy = std::sin(yaw_);
    float eye[3] = { dist_*cp*sy, dist_*sp, dist_*cp*cy };
    float ctr[3] = {0,0,0}, up[3] = {0,1,0};
    float view[16], proj[16];
    lookAt(eye, ctr, up, view);
    perspective(0.8f, (float)w/(float)h, 0.05f, 50.0f, proj);
    mul4(proj, view, out);
    camPosOut[0]=eye[0]; camPosOut[1]=eye[1]; camPosOut[2]=eye[2];
}

// Shared core: render into fbo_ with full GL-state containment.
static void renderContained(MaterialRenderBackend* backend, SphereMesh* mesh,
                            unsigned fbo, int w, int h,
                            const float viewProj[16], const float camPos[3], const float lightDir[3]) {
    // save state
    GLint prevFbo = 0, prevVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST), wasCull = glIsEnabled(GL_CULL_FACE), wasBlend = glIsEnabled(GL_BLEND);
    GLint prevDepthFunc = GL_LESS, prevCullMode = GL_BACK; GLfloat prevClear[4];
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullMode);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    RenderInputs in{};
    in.mesh = mesh;
    for (int i=0;i<16;i++) in.viewProj[i] = viewProj[i];
    for (int i=0;i<3;i++) { in.cameraPosWorld[i]=camPos[i]; in.lightDirWorld[i]=lightDir[i]; }
    in.lightColor[0]=in.lightColor[1]=in.lightColor[2]=3.0f;
    in.ambient[0]=in.ambient[1]=in.ambient[2]=0.15f;
    in.fboWidth=w; in.fboHeight=h;
    backend->render(in);

    // restore state (don't leak into ImGui) — review fix MAJOR 4.
    // The backend bound textures on units 0..3 and left glUseProgram(0). ImGui's
    // GL3 backend re-binds its own program/blend/scissor each frame, but it
    // assumes the ACTIVE texture unit is 0 and does not unbind our 2D textures.
    // So explicitly: unbind units 3..0 and leave unit 0 active.
    for (int u = 3; u >= 0; --u) { glActiveTexture(GL_TEXTURE0 + u); glBindTexture(GL_TEXTURE_2D, 0); }
    // (glActiveTexture loop ends on GL_TEXTURE0, the unit ImGui expects.)
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    // Restore enable bits AND the exact prior func/mode/clear (true containment).
    if (wasDepth) { glEnable(GL_DEPTH_TEST); } else { glDisable(GL_DEPTH_TEST); }
    glDepthFunc(prevDepthFunc);
    if (wasCull) { glEnable(GL_CULL_FACE); } else { glDisable(GL_CULL_FACE); }
    glCullFace(prevCullMode);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
}

void MaterialPreviewPBR::draw(const ImVec2& availableSize) {
    int w = (int)availableSize.x, h = (int)availableSize.y;
    if (w < 16) w = 16; if (h < 16) h = 16;
    ensureGL(w, h);
    if (!backendOk_ || !fboComplete_) {   // review fix MAJOR 6: surface init failure instead of a misleading blank
        ImGui::TextColored(ImVec4(1.0f,0.4f,0.3f,1.0f),
            "Material preview unavailable (shader compile or FBO init failed). See stderr.");
        return;
    }
    backend_->setMaterial(slots_);
    float vp[16], cam[3];
    buildViewProj(w, h, vp, cam);
    renderContained(backend_.get(), &mesh_, fbo_, w, h, vp, cam, lightDir_);

    ImGui::Image((ImTextureID)(intptr_t)colorTex_, ImVec2((float)w, (float)h), ImVec2(0,1), ImVec2(1,0));
    if (backend_->isApproximate())
        ImGui::TextColored(ImVec4(1.0f,0.7f,0.2f,1.0f),
            "Preview mode: Local PBR approximation, not exact MC2 shader.");
}

bool MaterialPreviewPBR::renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut) {
    if (w < 1) w = 1; if (h < 1) h = 1;   // guard buildViewProj div-by-zero (review fix)
    ensureGL(w, h);
    if (!backendOk_ || !fboComplete_) return false;   // review fix MAJOR 6
    backend_->setMaterial(slots_);
    float vp[16], cam[3];
    buildViewProj(w, h, vp, cam);
    renderContained(backend_.get(), &mesh_, fbo_, w, h, vp, cam, lightDir_);
    rgbaOut.resize((size_t)w*h*4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
