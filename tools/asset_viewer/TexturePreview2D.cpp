#include "TexturePreview2D.h"
#include "TextureDecoderRegistry.h"
#include "imgui.h"
#include <GL/glew.h>
#include <filesystem>
#include <system_error>

void TexturePreview2D::releaseOwned()
{
    if (current_.ownsGlTexture && current_.glTexture) {
        GLuint t = current_.glTexture;
        glDeleteTextures(1, &t);
    }
    current_ = DecodedTexture{};
}

TexturePreview2D::~TexturePreview2D() { releaseOwned(); }

void TexturePreview2D::setSource(const std::string& path)
{
    releaseOwned();                 // free previous owned texture before replacing
    path_ = path;
    meta_ = TextureMetadata{};
    hasError_ = false;
    errorText_.clear();

    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec) meta_.fileBytes = sz;

    current_ = textureDecoderRegistry().load(path);
    if (!current_.error.empty() || current_.glTexture == 0) {
        hasError_ = true;
        errorText_ = current_.error.empty() ? "Failed to load texture." : current_.error;
        // Do not hold a half-result that could be mistaken for a live texture.
        current_ = DecodedTexture{};
        return;
    }
    meta_.width       = current_.width;
    meta_.height      = current_.height;
    meta_.channels    = 0;
    meta_.formatLabel = current_.formatLabel;
    meta_.mipCount    = current_.mipCount;
}

void TexturePreview2D::draw(const ImVec2& availableSize)
{
    if (hasError_) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", errorText_.c_str());
        ImGui::TextWrapped("Path: %s", path_.c_str());
        return;
    }
    if (current_.glTexture == 0) {
        ImGui::TextDisabled("No texture selected.");
        return;
    }
    ImGui::SliderFloat("Zoom", &zoom_, 0.1f, 8.0f, "%.1fx");
    ImVec2 imageSize((float)meta_.width * zoom_, (float)meta_.height * zoom_);
    ImGui::BeginChild("tex_scroll", availableSize, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::Image((ImTextureID)(intptr_t)current_.glTexture, imageSize);
    ImGui::EndChild();
}
