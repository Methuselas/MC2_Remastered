// tools/asset_viewer/MeshPreview3D.cpp
// Renders a loaded TGL prop on a turntable orbit camera with a simple lit shader.
// GL-state fully contained (exact same save/restore discipline as MaterialPreviewPBR).
#include "MeshPreview3D.h"
#include "TglMeshLoader.h"
#include "imgui.h"
#include <GL/glew.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Math helpers (copied from MaterialPreviewPBR for self-containment)
// ---------------------------------------------------------------------------
static void perspective(float fovyRad, float aspect, float zn, float zf, float m[16]) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0] = f / aspect; m[5] = f;
    m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f;
    m[14] = (2 * zf * zn) / (zn - zf);
}

static void lookAt(const float eye[3], const float ctr[3], const float up[3], float m[16]) {
    float f[3] = { ctr[0]-eye[0], ctr[1]-eye[1], ctr[2]-eye[2] };
    float fl = std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float s[3] = { f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0] };
    float sl = std::sqrt(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
    s[0]/=sl; s[1]/=sl; s[2]/=sl;
    float u[3] = { s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0] };
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];  m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];  m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[3]=0; m[7]=0; m[11]=0; m[15]=1;
}

static void mul4(const float a[16], const float b[16], float o[16]) {
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        o[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1]
                 + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
    }
}

// Identity 4×4 (column-major).
static void identity4(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Build column-major rotation matrix from Euler angles in degrees.
// Order: Rx * Ry * Rz (X applied first, then Y, then Z) — i.e. vertices are
// first rotated by X, then by Y, then by Z.
// The matrix is ROTATION ONLY (no translation/scale).
static void eulerRotMatrix(float xDeg, float yDeg, float zDeg, float m[16]) {
    const float pi = 3.14159265f;
    float rx = xDeg * pi / 180.0f;
    float ry = yDeg * pi / 180.0f;
    float rz = zDeg * pi / 180.0f;

    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    // Rx (column-major):
    float Rx[16]; identity4(Rx);
    Rx[5]=cx; Rx[9]=-sx;
    Rx[6]=sx; Rx[10]= cx;

    // Ry (column-major):
    float Ry[16]; identity4(Ry);
    Ry[0]=cy; Ry[8]=sy;
    Ry[2]=-sy;Ry[10]=cy;

    // Rz (column-major):
    float Rz[16]; identity4(Rz);
    Rz[0]=cz; Rz[4]=-sz;
    Rz[1]=sz; Rz[5]= cz;

    // Combined: Rz * Ry * Rx   (so vertex sees Rx first, then Ry, then Rz)
    float tmp[16];
    mul4(Ry, Rx, tmp);
    mul4(Rz, tmp, m);
}

// Build T(center) * R * T(-center) — rotate about bounds center.
static void rotateAboutCenter(const float cx, const float cy, const float cz,
                               float xDeg, float yDeg, float zDeg, float m[16]) {
    float R[16];
    eulerRotMatrix(xDeg, yDeg, zDeg, R);
    // Pre-apply T(-center) by shifting the translation column:
    // M = T(c) * R * T(-c)
    // Column-major: column 3 = R * (-c) + c
    // = (R[12..15] entries of R are 0,0,0,1 since R is pure rotation)
    // So result column 3 (index 12,13,14,15):
    //   tx = R[0]*(-cx) + R[4]*(-cy) + R[8]*(-cz)  + cx
    //   ty = R[1]*(-cx) + R[5]*(-cy) + R[9]*(-cz)  + cy
    //   tz = R[2]*(-cx) + R[6]*(-cy) + R[10]*(-cz) + cz
    //   tw = 1
    std::memcpy(m, R, 64);  // copy rotation part
    m[12] = -(R[0]*cx + R[4]*cy + R[8]*cz)  + cx;
    m[13] = -(R[1]*cx + R[5]*cy + R[9]*cz)  + cy;
    m[14] = -(R[2]*cx + R[6]*cy + R[10]*cz) + cz;
    m[15] = 1.0f;
}

// ---------------------------------------------------------------------------
// Simple lit shader (N·L Lambert diffuse × albedo + ambient, gamma encode)
// ---------------------------------------------------------------------------
static const char* kVertSrc = R"GLSL(
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;

uniform mat4 u_viewProj;
uniform mat4 u_model;   // world transform (rotation about bounds center)

out vec3 v_posWorld;
out vec3 v_normal;
out vec2 v_uv;

void main() {
    vec4 wp    = u_model * vec4(a_pos, 1.0);
    v_posWorld = wp.xyz;
    v_normal   = normalize(mat3(u_model) * a_normal);
    v_uv       = a_uv;
    gl_Position = u_viewProj * wp;
}
)GLSL";

static const char* kFragSrc = R"GLSL(
#version 330 core
in vec3 v_posWorld;
in vec3 v_normal;
in vec2 v_uv;

uniform vec3      u_cameraPos;
uniform vec3      u_lightDir;   // world-space, points FROM light (normalize in shader)
uniform sampler2D u_albedo;
uniform int       u_hasAlbedo;

out vec4 fragColor;

void main() {
    vec3 N = normalize(v_normal);
    vec3 L = normalize(-u_lightDir);

    // Albedo
    vec3 albedo = (u_hasAlbedo != 0)
        ? texture(u_albedo, v_uv).rgb
        : vec3(0.78, 0.78, 0.78);   // flat grey fallback

    // Lambert diffuse + ambient
    float NdotL  = max(dot(N, L), 0.0);
    vec3  color  = albedo * (NdotL * 0.85 + 0.15);

    // Gamma encode (linear -> sRGB approx)
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));

    fragColor = vec4(color, 1.0);
}
)GLSL";

// Compile one shader stage; returns 0 on failure (prints to stderr).
static unsigned compileShader(GLenum type, const char* src) {
    unsigned sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
        std::fprintf(stderr, "[MeshPreview3D] shader compile error: %s\n", buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Link; returns 0 on failure.
static unsigned linkProgram(unsigned vs, unsigned fs) {
    unsigned p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024]; glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        std::fprintf(stderr, "[MeshPreview3D] program link error: %s\n", buf);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ---------------------------------------------------------------------------
// MeshPreview3D lifecycle
// ---------------------------------------------------------------------------
MeshPreview3D::MeshPreview3D() = default;

MeshPreview3D::~MeshPreview3D() {
    mesh_.destroy();
    destroyFbo();
    if (prog_) glDeleteProgram(prog_);
}

void MeshPreview3D::destroyFbo() {
    if (depthRbo_) glDeleteRenderbuffers(1, &depthRbo_);
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (fbo_)      glDeleteFramebuffers(1, &fbo_);
    fbo_ = colorTex_ = depthRbo_ = 0;
    fboW_ = fboH_ = 0;
    fboComplete_ = false;
}

void MeshPreview3D::ensureGL(int w, int h) {
    if (!glReady_) {
        glReady_ = true;
        // Compile the lit shader
        unsigned vs = compileShader(GL_VERTEX_SHADER,   kVertSrc);
        unsigned fs = compileShader(GL_FRAGMENT_SHADER, kFragSrc);
        if (vs && fs) {
            prog_ = linkProgram(vs, fs);
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);

        if (prog_) {
            u_viewProj  = glGetUniformLocation(prog_, "u_viewProj");
            u_model_    = glGetUniformLocation(prog_, "u_model");
            u_cameraPos = glGetUniformLocation(prog_, "u_cameraPos");
            u_lightDir  = glGetUniformLocation(prog_, "u_lightDir");
            u_albedo    = glGetUniformLocation(prog_, "u_albedo");
            u_hasAlbedo = glGetUniformLocation(prog_, "u_hasAlbedo");
            backendOk_  = true;
        } else {
            std::fprintf(stderr, "[MeshPreview3D] shader compile/link failed\n");
            backendOk_ = false;
        }
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
        if (!fboComplete_) std::fprintf(stderr, "[MeshPreview3D] FBO incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

// ---------------------------------------------------------------------------
// setSource: load mesh from TglMeshLoader and upload to GPU
// ---------------------------------------------------------------------------
void MeshPreview3D::setSource(const std::string& tglName) {
    errorMsg_.clear();
    tglName_ = tglName;

    if (!TglMeshLoader::ensureFastFile(deployDir_.c_str())) {
        errorMsg_ = "FastFile init failed (deployDir: " + deployDir_ + ")";
        return;
    }

    MeshData md = TglMeshLoader::loadMesh(tglName);
    if (!md.ok) {
        errorMsg_ = md.error;
        return;
    }

    mesh_.destroy();
    mesh_.upload(md, deployDir_, tier_);

    if (!mesh_.valid()) {
        errorMsg_ = "MeshGpu upload produced no submeshes";
        return;
    }

    // Auto-frame: dist = 1.6 * radius; center at bounds center.
    const float* bmin = mesh_.bmin();
    const float* bmax = mesh_.bmax();
    float dx = bmax[0] - bmin[0];
    float dy = bmax[1] - bmin[1];
    float dz = bmax[2] - bmin[2];
    float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    if (radius < 0.01f) radius = 0.01f;
    dist_     = 1.6f * radius;
    center_[0] = 0.5f * (bmin[0] + bmax[0]);
    center_[1] = 0.5f * (bmin[1] + bmax[1]);
    center_[2] = 0.5f * (bmin[2] + bmax[2]);
}

// ---------------------------------------------------------------------------
// buildViewProj: orbit camera centered at center_
// ---------------------------------------------------------------------------
void MeshPreview3D::buildViewProj(int w, int h, const float center[3],
                                   float out[16], float camPosOut[3]) const {
    float cp = std::cos(pitch_), sp = std::sin(pitch_);
    float cy = std::cos(yaw_),   sy = std::sin(yaw_);
    float eye[3] = {
        center[0] + dist_ * cp * sy,
        center[1] + dist_ * sp,
        center[2] + dist_ * cp * cy
    };
    float up[3] = {0.0f, 1.0f, 0.0f};
    float view[16], proj[16];
    lookAt(eye, center, up, view);
    perspective(0.8f, (float)w / (float)h, 0.01f, 500.0f, proj);
    mul4(proj, view, out);
    camPosOut[0] = eye[0]; camPosOut[1] = eye[1]; camPosOut[2] = eye[2];
}

// ---------------------------------------------------------------------------
// renderScene: fully GL-state-contained render into fbo_.
// ---------------------------------------------------------------------------
void MeshPreview3D::renderScene(int w, int h) {
    // --- Save state ---
    GLint  prevFbo = 0;
    GLint  prevVp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    GLboolean wasDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean wasCull  = glIsEnabled(GL_CULL_FACE);
    GLboolean wasBlend = glIsEnabled(GL_BLEND);
    GLint prevDepthFunc = GL_LESS, prevCullMode = GL_BACK;
    GLfloat prevClear[4];
    glGetIntegerv(GL_DEPTH_FUNC,      &prevDepthFunc);
    glGetIntegerv(GL_CULL_FACE_MODE,  &prevCullMode);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,  prevClear);

    // --- Render into FBO ---
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    // Disable backface culling: some MC2 props have inverted winding; render both
    // sides to guarantee a visible model even without known winding order.
    glDisable(GL_CULL_FACE);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (mesh_.valid() && prog_) {
        float vp[16], cam[3];
        buildViewProj(w, h, center_, vp, cam);

        glUseProgram(prog_);
        glUniformMatrix4fv(u_viewProj,  1, GL_FALSE, vp);

        // Build T(center)*R*T(-center) so rotation pivots at the bounds center.
        float modelMat[16];
        rotateAboutCenter(center_[0], center_[1], center_[2],
                          modelRotDeg_[0], modelRotDeg_[1], modelRotDeg_[2],
                          modelMat);
        glUniformMatrix4fv(u_model_, 1, GL_FALSE, modelMat);

        glUniform3fv(u_cameraPos, 1, cam);
        glUniform3fv(u_lightDir,  1, lightDir_);
        glUniform1i(u_albedo, 0);   // sampler bound on unit 0

        // Draw each submesh; drawLit sets u_hasAlbedo per submesh.
        mesh_.drawLit(u_hasAlbedo, showLights_);
    }

    // --- Restore state ---
    // Unbind texture units used (only unit 0 here), leave unit 0 active.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);

    if (wasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(prevDepthFunc);
    if (wasCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    glCullFace(prevCullMode);
    if (wasBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
}

// ---------------------------------------------------------------------------
// draw (ImGui path)
// ---------------------------------------------------------------------------
void MeshPreview3D::draw(const ImVec2& availableSize) {
    // -----------------------------------------------------------------------
    // Controls strip (parity with Materials mode)
    // -----------------------------------------------------------------------
    ImGui::SeparatorText("View");
    ImGui::SliderFloat("Orbit yaw",   &yaw_,   -3.14159f, 3.14159f);
    ImGui::SliderFloat("Orbit pitch", &pitch_, -1.5f, 1.5f);
    ImGui::SliderFloat("Zoom",        &dist_,   0.05f, 50.0f);
    // Model rotation: Euler X/Y/Z degrees; default -90 X stands props upright.
    // Rotation order applied to vertices: Rx first, then Ry, then Rz.
    ImGui::SliderFloat3("Model rot (deg)", modelRotDeg_, -180.0f, 180.0f);
    ImGui::SeparatorText("Light");
    ImGui::SliderFloat3("Light dir", lightDir_, -1.0f, 1.0f);

    // -----------------------------------------------------------------------
    // Texture resolution swap
    // -----------------------------------------------------------------------
    ImGui::SeparatorText("Texture");
    {
        static const int kTiers[] = { 128, 256, 512, 1024 };
        ImGui::Text("Resolution:");
        ImGui::SameLine();
        for (int ti = 0; ti < 4; ++ti) {
            int t = kTiers[ti];
            bool active = (tier_ == t);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%d##tier", t);
            if (ImGui::Button(lbl) && !active) {
                tier_ = t;
                mesh_.reloadAlbedo(deployDir_, tier_);
            }
            if (active) ImGui::PopStyleColor();
            if (ti < 3) ImGui::SameLine();
        }
    }
    ImGui::Checkbox("Show lights", &showLights_);

    if (ImGui::Button("Reset view")) {
        yaw_   = 0.6f;
        pitch_ = 0.35f;
        modelRotDeg_[0] = -90.0f;
        modelRotDeg_[1] =   0.0f;
        modelRotDeg_[2] =   0.0f;
        showLights_ = false;
        // Re-frame dist_ from current bounds (same formula as setSource).
        const float* bmin = mesh_.bmin();
        const float* bmax = mesh_.bmax();
        float dx = bmax[0] - bmin[0];
        float dy = bmax[1] - bmin[1];
        float dz = bmax[2] - bmin[2];
        float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
        if (radius < 0.01f) radius = 0.01f;
        dist_ = 1.6f * radius;
        // Reset tier to default and reload albedo at 512.
        if (tier_ != 512) {
            tier_ = 512;
            mesh_.reloadAlbedo(deployDir_, tier_);
        }
    }
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
        "Local preview (not exact MC2 shader)");
    ImGui::Separator();

    // Subtract controls height from available size for the 3D viewport.
    ImVec2 vpSize = availableSize;
    float usedH = ImGui::GetCursorPos().y - ImGui::GetWindowContentRegionMin().y;
    if (usedH > 0.0f && vpSize.y > usedH + 16.0f)
        vpSize.y -= usedH;

    int w = (int)vpSize.x, h = (int)vpSize.y;
    if (w < 16) w = 16; if (h < 16) h = 16;
    ensureGL(w, h);

    if (!errorMsg_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
            "Model load error: %s", errorMsg_.c_str());
        return;
    }

    if (!backendOk_ || !fboComplete_) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
            "Model preview unavailable (shader compile or FBO init failed). See stderr.");
        return;
    }

    renderScene(w, h);

    ImGui::Image((ImTextureID)(intptr_t)colorTex_,
                 ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0));

    // -----------------------------------------------------------------------
    // Mouse orbit + zoom (active while hovered over the Image above)
    // -----------------------------------------------------------------------
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            yaw_   += io.MouseDelta.x * 0.01f;
            if (yaw_ >  3.14159f) yaw_ -= 6.28318f;     // wrap to slider range [-pi,pi]
            if (yaw_ < -3.14159f) yaw_ += 6.28318f;
            pitch_ += io.MouseDelta.y * 0.01f;
            const float lim = 1.55f;
            if (pitch_ >  lim) pitch_ =  lim;
            if (pitch_ < -lim) pitch_ = -lim;
        }
        if (io.MouseWheel != 0.0f) {
            dist_ *= (1.0f - io.MouseWheel * 0.1f);
            if (dist_ < 0.05f) dist_ = 0.05f;
        }
    }
}

// ---------------------------------------------------------------------------
// renderToPixels (test hook, ImGui-free)
// ---------------------------------------------------------------------------
bool MeshPreview3D::renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut) {
    if (w < 1) w = 1; if (h < 1) h = 1;
    ensureGL(w, h);
    if (!backendOk_ || !fboComplete_) return false;
    renderScene(w, h);
    rgbaOut.resize((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
