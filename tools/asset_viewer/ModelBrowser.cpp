// tools/asset_viewer/ModelBrowser.cpp
#include "ModelBrowser.h"
#include "TglMeshLoader.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------

static std::string toLower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

void ModelBrowser::ensureList()
{
    if (listed_) return;
    listed_ = true;  // attempt once regardless of success

    // Viewer runs with cwd = deploy root; "." -> ./tgl.fst.
    if (!TglMeshLoader::ensureFastFile(".")) return;
    all_ = TglMeshLoader::listTgl();
    rebuildFiltered();
}

void ModelBrowser::rebuildFiltered()
{
    filtered_.clear();
    std::string needle = toLower(std::string(filter_));
    for (const auto& name : all_) {
        if (needle.empty() || toLower(name).find(needle) != std::string::npos)
            filtered_.push_back(name);
    }
    // Selection might be stale after filter change — keep selected_ pointing at
    // the same entry (by value) if it still exists, otherwise reset.
    if (selected_ >= 0 && !selectionPath_.empty()) {
        int found = -1;
        for (int i = 0; i < (int)filtered_.size(); ++i)
            if (filtered_[i] == selectionPath_) { found = i; break; }
        selected_ = found;
    }
}

void ModelBrowser::draw()
{
    ensureList();

    if (all_.empty()) {
        ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f},
            listed_ ? "No .tgl entries found in tgl.fst"
                    : "tgl.fst not loaded");
        return;
    }

    // Filter input
    bool filterChanged = ImGui::InputText("##tglfilter", filter_, sizeof(filter_));
    if (filterChanged) rebuildFiltered();

    ImGui::SameLine();
    if (ImGui::Button("x")) {
        filter_[0] = '\0';
        rebuildFiltered();
    }

    ImGui::Text("%d / %d props", (int)filtered_.size(), (int)all_.size());
    ImGui::Separator();

    // Scrollable list
    ImGui::BeginChild("##tgllist", ImVec2(0, 0), false);
    for (int i = 0; i < (int)filtered_.size(); ++i) {
        bool sel = (selected_ == i);
        if (ImGui::Selectable(filtered_[i].c_str(), sel)) {
            selected_      = i;
            selectionPath_ = filtered_[i];
            hasSelection_  = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", filtered_[i].c_str());
    }
    ImGui::EndChild();
}

std::string ModelBrowser::takeSelection()
{
    hasSelection_ = false;
    return selectionPath_;
}
