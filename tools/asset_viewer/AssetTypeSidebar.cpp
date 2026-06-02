#include "AssetTypeSidebar.h"
#include "imgui.h"
void AssetTypeSidebar::draw()
{
    ImGui::TextDisabled("Implemented");
    if (ImGui::Selectable("Textures",  active_ == AssetType::Textures))  active_ = AssetType::Textures;
    if (ImGui::Selectable("Materials", active_ == AssetType::Materials)) active_ = AssetType::Materials;
    ImGui::Spacing();
    ImGui::TextDisabled("Deferred");
    static const char* kDeferred[] = {
        "Static Props", "Trees", "Mechs", "Vehicles", "VFX", "Terrain Materials", "Mod Package"
    };
    ImGui::BeginDisabled(true);
    for (const char* name : kDeferred)
        ImGui::Selectable(name, false);
    ImGui::EndDisabled();
}
