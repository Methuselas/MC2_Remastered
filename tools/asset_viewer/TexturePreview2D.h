#pragma once
#include "PreviewSurface.h"
#include "TextureMetadata.h"
#include <string>
class TexturePreview2D : public PreviewSurface {
public:
    void setSource(const std::string& path) override;
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Texture"; }
    bool hasError() const { return hasError_; }
    const std::string& errorText() const { return errorText_; }
    const TextureMetadata& metadata() const { return meta_; }
    const std::string& sourcePath() const { return path_; }
private:
    std::string path_;
    TextureMetadata meta_;
    ImTextureID textureId_ = (ImTextureID)0;
    bool hasTexture_ = false;
    bool hasError_ = false;
    std::string errorText_;
    float zoom_ = 1.0f;
};
