#include "TexturePreview2D.h"
#include "UiEditorImageCache.h"
#include "imgui.h"
#include <filesystem>
#include <system_error>

void TexturePreview2D::setSource(const std::string& path)
{
    path_ = path;
    meta_ = TextureMetadata{};
    textureId_ = (ImTextureID)0;
    hasTexture_ = false;
    hasError_ = false;
    errorText_.clear();

    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec) meta_.fileBytes = sz;

    const UiEditorImageTexture* tex = UiEditorImageCache_Get(path.c_str());
    if (!tex || !tex->loaded) {
        hasError_ = true;
        errorText_ = tex && tex->unavailable
            ? "Image format not supported or file unreadable."
            : "Failed to load image (not found or decode error).";
        return;
    }
    meta_.width = tex->width;
    meta_.height = tex->height;
    meta_.channels = 0;
    textureId_ = tex->textureId;
    hasTexture_ = true;
}

void TexturePreview2D::draw(const ImVec2& availableSize)
{
    if (hasError_) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", errorText_.c_str());
        ImGui::TextWrapped("Path: %s", path_.c_str());
        return;
    }
    if (!hasTexture_) {
        ImGui::TextDisabled("No texture selected.");
        return;
    }
    ImGui::SliderFloat("Zoom", &zoom_, 0.1f, 8.0f, "%.1fx");
    ImVec2 imageSize((float)meta_.width * zoom_, (float)meta_.height * zoom_);
    ImGui::BeginChild("tex_scroll", availableSize, true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image(textureId_, imageSize);
    ImGui::EndChild();
}
