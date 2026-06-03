// tools/asset_viewer/MeshPreview3D.h
// PreviewSurface impl: loads a .tgl via TglMeshLoader, uploads to MeshGpu,
// renders on a turntable orbit camera into an FBO, blits via ImGui::Image.
// Follows the same GL-state-containment contract as MaterialPreviewPBR.
#pragma once
#include "PreviewSurface.h"
#include "MeshGpu.h"
#include <cstdint>
#include <string>
#include <vector>

class MeshPreview3D : public PreviewSurface {
public:
    MeshPreview3D();
    ~MeshPreview3D() override;

    // PreviewSurface interface
    void setSource(const std::string& tglName) override;
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Model (3D)"; }

    // Set the deploy directory before calling setSource().
    void setDeployDir(const std::string& dir) { deployDir_ = dir; }

    // Test hook: render into an internal FBO and read back RGBA pixels.
    // No ImGui context required. Returns false if init or FBO failed.
    bool renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut);

    // Camera / light controls (for UI extensions).
    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
    float* lightDir()   { return lightDir_; }

private:
    void ensureGL(int w, int h);
    void destroyFbo();
    void buildViewProj(int w, int h,
                       const float center[3],
                       float out[16], float camPosOut[3]) const;
    void renderScene(int w, int h);

    // GL program (compiled once in ensureGL)
    unsigned prog_  = 0;
    int u_viewProj  = -1;
    int u_cameraPos = -1;
    int u_lightDir  = -1;
    int u_albedo    = -1;
    int u_hasAlbedo = -1;

    MeshGpu mesh_;

    // FBO
    unsigned fbo_ = 0, colorTex_ = 0, depthRbo_ = 0;
    int fboW_ = 0, fboH_ = 0;

    bool backendOk_   = false;
    bool fboComplete_ = false;
    bool glReady_     = false;

    // Camera / light
    float yaw_   = 0.6f;
    float pitch_ = 0.3f;
    float dist_  = 3.0f;
    float center_[3] = {0.0f, 0.0f, 0.0f};  // bounds center for orbit pivot
    float lightDir_[3] = { -0.4f, -0.7f, -0.5f };

    std::string deployDir_ = ".";
    std::string tglName_;
    std::string errorMsg_;
};
