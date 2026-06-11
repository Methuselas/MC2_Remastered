// tools/asset_viewer/ModWorkbenchPanel.cpp
// S2: stock vs override side-by-side panel with generation-sync.
// S3: warnings panel + appearance-verify control.
// S4: export draft bundle button.
#include "ModWorkbenchPanel.h"
#include "imgui.h"
#include <cctype>
#include <cfloat>
#include <cstring>
#include <string>

void ModWorkbenchPanel::setDeployDir(const std::string& d) {
    stockPreview_.setDeployDir(d);
    overridePreview_.setDeployDir(d);
    deployDir_ = d;
    roster_.load(d);
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

    ImGui::TextUnformatted("Stock prop (click to bind):");
    stockBrowser_.draw();   // filter box + scrollable .tgl list; tooltip shows full path
    if (stockBrowser_.hasSelection()) {
        std::string pick = stockBrowser_.takeSelection();
        wb.bindStock(pick);
    }
    if (wb.hasStock())
        ImGui::TextDisabled("bound stock: %s", wb.stockMesh().ok ? "ok" : "--");

    if (wb.generation() != lastSyncedGen_) {
        syncMeshes(wb);
        lastSyncedGen_ = wb.generation();
        appe_[0] = '\0';   // reprime appearance buffer from the (reset/suggested) record
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

    auto& rec = wb.record();
    if (appe_[0]=='\0' && !rec.appearanceName.empty()) strncpy(appe_, rec.appearanceName.c_str(), sizeof(appe_)-1);
    // Free-type field; verified state is DERIVED (roster pick or roster match = true).
    if (ImGui::InputText("Appearance key", appe_, sizeof(appe_))) {
        rec.appearanceName = appe_;
        rec.appearanceVerified = roster_.contains(rec.appearanceName);
    } else {
        rec.appearanceName = appe_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh roster")) roster_.refresh(deployDir_);
    ImGui::TextDisabled("appearance source: %d data/tgl/*.ini scanned, %zu keys%s",
        roster_.scannedFileCount(), roster_.names().size(),
        rec.appearanceVerified ? "  [verified: roster match]" : "  [unverified: not in roster]");
    // Filterable roster picker.
    ImGui::InputText("filter##appearance", apFilter_, sizeof(apFilter_));
    if (ImGui::BeginListBox("##appearance_roster", ImVec2(-FLT_MIN, 120))) {
        std::string lf; for (char* p = apFilter_; *p; ++p) lf += (char)std::tolower((unsigned char)*p);
        for (const auto& n : roster_.names()) {
            std::string ln; for (char c : n) ln += (char)std::tolower((unsigned char)c);
            if (!lf.empty() && ln.find(lf) == std::string::npos) continue;
            if (ImGui::Selectable(n.c_str(), n == rec.appearanceName)) {
                strncpy(appe_, n.c_str(), sizeof(appe_)-1); appe_[sizeof(appe_)-1]='\0';
                rec.appearanceName = n;
                rec.appearanceVerified = true;          // explicit roster pick
            }
        }
        ImGui::EndListBox();
    }
    wb.revalidate();
    ImGui::Separator(); ImGui::TextUnformatted("Warnings:");
    if (wb.warnings().empty()) ImGui::TextDisabled("  none");
    for (const auto& w : wb.warnings()){
        bool blk = w.severity==WarnSeverity::Block;
        ImGui::TextColored(blk?ImVec4(1,0.4f,0.4f,1):ImVec4(1,0.8f,0.3f,1),
                           "  [%s] %s", blk?"BLOCK (mirror, advisory)":"WARN", w.message.c_str());
    }

    ImGui::Separator();
    static char bundleId[128]="my_override";
    static char outRoot[260]="data/model_overrides";
    static std::string exportMsg;
    ImGui::InputText("Bundle id", bundleId, sizeof(bundleId));
    ImGui::InputText("Out root (model_overrides dir)", outRoot, sizeof(outRoot));
    ImGui::BeginDisabled(wb.hasBlocking());
    if (ImGui::Button("Export draft bundle")){
        ExportResult r=wb.exportBundle(outRoot,bundleId);
        exportMsg=(r.ok?"OK: ":"FAILED: ")+r.message;
    }
    ImGui::EndDisabled();
    if (!exportMsg.empty()) ImGui::TextWrapped("%s", exportMsg.c_str());
    ImGui::TextDisabled("Writes <out>/<id>/{model.glb, models.generated.json}. Does NOT edit your central models.json.");
}
