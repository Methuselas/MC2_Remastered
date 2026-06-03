// tools/asset_viewer/MaterialSlots.cpp
#include "MaterialSlots.h"
#include "MaterialPreviewPBR.h"
#include "FileBrowser.h"     // for FileBrowser::PickFile()
#include "FitMaterialLoader.h"
#include "imgui.h"
#include <filesystem>

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

int MaterialSlots::loadFit(const std::string& fitPath, MaterialPreviewPBR& preview) {
    namespace fs = std::filesystem;
    std::string err;
    FitMaterial fm = FitMaterialLoader_Parse(fitPath, &err);
    if (!fm.found) return 0;

    fs::path fitDir = fs::path(fitPath).parent_path();
    fs::path cwd    = fs::current_path();

    // Pairs of (relative-path, slot-kind, index) to load.
    struct SlotDesc { const std::string& rel; MaterialSlotKind kind; int idx; };
    SlotDesc descs[] = {
        { fm.baseColor, MaterialSlotKind::BaseColor, 0 },
        { fm.normal,    MaterialSlotKind::Normal,    1 },
        { fm.orm,       MaterialSlotKind::Orm,       2 },
        { fm.emissive,  MaterialSlotKind::Emissive,  3 },
    };

    int loaded = 0;
    for (auto& d : descs) {
        if (d.rel.empty()) continue;
        // Try 3 candidate bases in order: fitDir, cwd, bare rel.
        std::string candidates[] = {
            (fitDir / d.rel).string(),
            (cwd    / d.rel).string(),
            d.rel,
        };
        bool ok = false;
        for (auto& cand : candidates) {
            std::string e;
            uint32_t t = MaterialTextureLoader_Load(cand, d.kind, &e);
            if (t) {
                paths_[d.idx]  = cand;
                errors_[d.idx].clear();
                preview.setSlotTexture(d.kind, t);
                ++loaded;
                ok = true;
                break;
            }
        }
        if (!ok) {
            // Record the fitDir-relative attempt's error as the representative error.
            std::string e;
            MaterialTextureLoader_Load(candidates[0], d.kind, &e);
            errors_[d.idx] = e;
        }
    }
    return loaded;
}

void MaterialSlots::draw(MaterialPreviewPBR& preview) {
    if (ImGui::Button("Load .fit material")) {
        std::string fit = FileBrowser::PickFile();
        if (!fit.empty())
            loadFit(fit, preview);
    }
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
