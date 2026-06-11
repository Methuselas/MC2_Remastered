// tools/asset_viewer/ModelPreviewEngineShader.h
// Backend-A v2: PreviewSurface rendering one static prop with the REAL engine
// static_prop.{vert,frag} (referenced, not copied) + StandaloneSceneStubs. Faithful
// to engine shading/material/pipeline-state; NOT mission-lighting exact. Fails open:
// on any compile/link/resource error, ok()==false and the caller renders Backend-B.
#pragma once
#include "PreviewSurface.h"
#include "MeshGpu.h"
#include "StandaloneSceneStubs.h"
#include "ShaderContractReport.h"
#include <string>
#include <vector>
#include <cstdint>

class ModelPreviewEngineShader : public PreviewSurface {
public:
    ~ModelPreviewEngineShader() override;
    void setShaderRoot(const std::string& dir) { shaderRoot_ = dir; }
    void setDeployDir(const std::string& dir)  { deployDir_ = dir; }
    void setSource(const std::string& tglName) override;   // loads prop into MeshGpu
    void draw(const ImVec2& availableSize) override;       // renders; falls open if !ok
    const char* label() const override { return "Engine Shader (Backend-A)"; }

    bool ok() const { return report_.ok(); }               // false => caller draws Backend-B
    const ShaderContractReport& report() const { return report_; }

    // Headless test hook (no ImGui): ensure built, render to an FBO, read back RGBA.
    bool renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut);

    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
private:
    bool ensureProgram();                 // resolve+compile+link; populates report_
    void ensureFbo(int w, int h);         // MRT: Color0 + GBuffer1 + depth
    void renderScene(int w, int h);
    void buildReverseZViewProj(int w, int h, float out[16], float camPos[3]) const;

    std::string shaderRoot_ = "shaders", deployDir_ = ".", tglName_;
    unsigned prog_ = 0;
    unsigned fbo_ = 0, color0_ = 0, gbuffer1_ = 0, depthRbo_ = 0;
    unsigned sceneUbo_ = 0;
    int fboW_ = 0, fboH_ = 0;
    MeshGpu mesh_;
    StandaloneSceneStubs stubs_;
    ShaderContractReport report_;
    bool triedProgram_ = false;
    bool stubsBuilt_ = false;
    float yaw_ = 0.6f, pitch_ = 0.35f, dist_ = 3.0f;
    float center_[3] = {0.0f, 0.0f, 0.0f};
    float modelRotDeg_[3] = { -90.0f, 0.0f, 0.0f };  // same upright convention as Backend-B
};
