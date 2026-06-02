// tools/asset_viewer/MaterialSlots.cpp
#include "MaterialSlots.h"
#include "MaterialPreviewPBR.h"
#include "FileBrowser.h"     // for FileBrowser::PickFile()
#include "imgui.h"

void MaterialSlots::slotRow(const char* label, MaterialSlotKind kind, MaterialPreviewPBR& preview) {
    int i = (int)kind;
    ImGui::PushID(label);
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(paths_[i].empty() ? "(none)" : paths_[i].c_str());
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string picked = FileBrowser::PickFile();
        if (!picked.empty()) {
            std::string err;
            uint32_t tex = MaterialTextureLoader_Load(picked, kind, &err);
            if (tex) { paths_[i] = picked; errors_[i].clear(); preview.setSlotTexture(kind, tex); }
            else     { errors_[i] = err; }
        }
    }
    if (!errors_[i].empty()) ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "  %s", errors_[i].c_str());
    ImGui::PopID();
}

void MaterialSlots::draw(MaterialPreviewPBR& preview) {
    ImGui::SeparatorText("Material slots");
    slotRow("Base Color", MaterialSlotKind::BaseColor, preview);
    slotRow("Normal",     MaterialSlotKind::Normal,    preview);
    slotRow("ORM",        MaterialSlotKind::Orm,       preview);
    slotRow("Emissive",   MaterialSlotKind::Emissive,  preview);

    ImGui::SeparatorText("View");
    ImGui::SliderFloat("Orbit yaw",   &preview.orbitYaw(),   -3.14159f, 3.14159f);
    ImGui::SliderFloat("Orbit pitch", &preview.orbitPitch(), -1.5f, 1.5f);
    ImGui::SliderFloat("Zoom",        &preview.zoom(),        1.2f, 8.0f);
    ImGui::SeparatorText("Light");
    ImGui::SliderFloat3("Light dir", preview.lightDir(), -1.0f, 1.0f);
}
