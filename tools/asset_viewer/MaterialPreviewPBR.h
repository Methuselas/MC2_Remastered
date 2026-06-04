// tools/asset_viewer/MaterialPreviewPBR.h
#pragma once
#include "PreviewSurface.h"
#include "MaterialRenderBackend.h"
#include "LocalPbrMaterialBackend.h"
#include "MaterialTextureLoader.h"
#include "SphereMesh.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

class MaterialPreviewPBR : public PreviewSurface {
public:
    MaterialPreviewPBR();
    ~MaterialPreviewPBR() override;

    void setSource(const std::string& path) override;          // FIT path (Task 8); no-op for now
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Material (PBR)"; }

    // Slot assignment (called by MaterialSlots UI in Task 5). Takes ownership of the GL texture.
    void setSlotTexture(MaterialSlotKind kind, uint32_t glTex);

    // Camera/light controls exposed so the UI (Task 5) can drive them.
    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
    float* lightDir()   { return lightDir_; }

    // Test hook: render the sphere with current slots into an internal FBO and read it back.
    bool renderToPixels(int w, int h, std::vector<uint8_t>& rgbaOut);

private:
    void ensureGL(int w, int h);     // lazy backend/mesh init + (re)create FBO on size change
    void destroyFbo();
    void buildViewProj(int w, int h, float out[16], float camPosOut[3]) const;

    std::unique_ptr<MaterialRenderBackend> backend_;
    SphereMesh mesh_;
    MaterialSlotTextures slots_{};
    bool glReady_ = false;
    bool backendOk_ = false;     // review fix MAJOR 6: backend_->init() succeeded
    bool fboComplete_ = false;   // review fix MAJOR 6: last FBO build was complete

    unsigned fbo_ = 0, colorTex_ = 0, depthRbo_ = 0;
    int fboW_ = 0, fboH_ = 0;

    float yaw_ = 0.6f, pitch_ = 0.3f, dist_ = 3.0f;
    float lightDir_[3] = { -0.4f, -0.7f, -0.5f };
    std::string fitPath_;
};
