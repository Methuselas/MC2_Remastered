#include "TextureInspectorPanel.h"
#include "TexturePreview2D.h"
#include "TextureMetadata.h"
#include "imgui.h"

void TextureInspectorPanel::draw(TexturePreview2D& surface)
{
    if (surface.sourcePath().empty()) {
        ImGui::TextDisabled("Select a texture from the browser.");
        return;
    }
    ImGui::TextWrapped("%s", surface.sourcePath().c_str());
    if (!surface.hasError()) {
        const TextureMetadata& m = surface.metadata();
        ImGui::Text("Dimensions: %s", FormatDimensions(m).c_str());
        ImGui::Text("Channels:   %s", FormatChannels(m).c_str());
        ImGui::Text("File size:  %s", FormatFileSize(m).c_str());
        ImGui::Text("Format:     %s", FormatTextureFormat(m).c_str());
        if (m.mipCount > 1) ImGui::Text("Mips:       %d", m.mipCount);
    }
    ImGui::Separator();
    surface.draw(ImGui::GetContentRegionAvail());
}
