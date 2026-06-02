// tools/asset_viewer/LocalPbrMaterialBackend.h
#pragma once
#include "MaterialRenderBackend.h"

class LocalPbrMaterialBackend : public MaterialRenderBackend {
public:
    bool init() override;
    void setMaterial(const MaterialSlotTextures& slots) override;
    void render(const RenderInputs& in) override;
    void shutdown() override;
    const char* name() const override { return "LocalPBR (approximate)"; }
    bool isApproximate() const override { return true; }

private:
    unsigned program_ = 0;
    MaterialSlotTextures slots_{};
    // cached uniform locations
    int locViewProj_ = -1, locCamPos_ = -1, locLightDir_ = -1, locLightColor_ = -1, locAmbient_ = -1;
    int locHasNormal_ = -1, locHasOrm_ = -1, locHasEmissive_ = -1;
    int locBaseColor_ = -1, locNormalTex_ = -1, locOrmTex_ = -1, locEmissiveTex_ = -1;
};
