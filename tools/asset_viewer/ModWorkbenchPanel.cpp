// tools/asset_viewer/ModWorkbenchPanel.cpp
// S2: stock vs override side-by-side panel with generation-sync.
#include "ModWorkbenchPanel.h"
#include "imgui.h"

void ModWorkbenchPanel::setDeployDir(const std::string& d) {
    stockPreview_.setDeployDir(d);
    overridePreview_.setDeployDir(d);
}

void ModWorkbenchPanel::syncMeshes(ModWorkbench& wb) {
    if (wb.hasStock())    stockPreview_.setMeshData(wb.stockMesh());
    if (wb.hasOverride()) overridePreview_.setMeshData(wb.overrideMesh());
}

void ModWorkbenchPanel::draw(ModWorkbench& wb, const ImVec2& avail) {
    ImGui::TextUnformatted("Mod Workbench \xe2\x80\x94 drag a .glb/.gltf onto the window");
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f),
        "Preview validates geometry/package. In-game lighting/material may differ (Backend A = v2).");
    ImGui::Separator();

    if (!wb.hasOverride()) {
        ImGui::TextDisabled("No override loaded. %s", wb.lastError().c_str());
        return;
    }

    ImGui::Text("Override: %s", wb.overridePath().c_str());

    static char tgl[256] = "data/tgl/2civliving.tgl";
    ImGui::InputText("Stock .tgl", tgl, sizeof(tgl));
    if (ImGui::Button("Bind stock")) wb.bindStock(tgl);

    if (wb.generation() != lastSyncedGen_) {
        syncMeshes(wb);
        lastSyncedGen_ = wb.generation();
    }

    // Sync orbit controls: override follows stock camera.
    overridePreview_.orbitYaw()   = stockPreview_.orbitYaw();
    overridePreview_.orbitPitch() = stockPreview_.orbitPitch();
    overridePreview_.zoom()       = stockPreview_.zoom();

    float half = (avail.x - 8.0f) * 0.5f;
    float h    = avail.y * 0.55f;

    ImGui::BeginChild("stock", ImVec2(half, h), true);
    ImGui::TextUnformatted("Stock");
    stockPreview_.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("override", ImVec2(half, h), true);
    ImGui::TextUnformatted("Override");
    overridePreview_.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    if (wb.hasStock()) {
        auto d = wb.computeDelta();
        ImGui::Text("footprint ratio (override/stock, max axis): %.2fx", d.maxRatio);
        ImGui::Text("pivot offset (GL): %.2f, %.2f, %.2f",
            d.pivotOffset[0], d.pivotOffset[1], d.pivotOffset[2]);
    }
}
