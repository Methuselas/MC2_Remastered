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

    // Test hook: reload albedo textures at the given tier and update tier_.
    // Thin wrapper over mesh_.reloadAlbedo; safe to call without ImGui.
    void reloadAlbedoAtTier(const std::string& deployDir, int tier) {
        tier_ = tier;
        mesh_.reloadAlbedo(deployDir, tier);
    }

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
    int u_model_    = -1;   // model rotation matrix (world transform)
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
    float pitch_ = 0.35f;
    float dist_  = 3.0f;
    float center_[3] = {0.0f, 0.0f, 0.0f};  // bounds center for orbit pivot
    float lightDir_[3] = { -0.4f, -0.7f, -0.5f };

    // Model rotation: Euler angles in degrees applied as Rx * Ry * Rz (X first).
    // Default -90° X stands most MC2 props upright (they're stored Z-up in TGL).
    float modelRotDeg_[3] = { -90.0f, 0.0f, 0.0f };

    // Texture resolution tier and spotlight-beam toggle.
    int  tier_       = 512;    // active albedo resolution: 128 / 256 / 512 / 1024
    bool showLights_ = false;  // when false, SpotLight_* beam submeshes are hidden

    std::string deployDir_ = ".";
    std::string tglName_;
    std::string errorMsg_;
};
