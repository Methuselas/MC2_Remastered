#include "AssetTypeSidebar.h"
#include "imgui.h"
void AssetTypeSidebar::draw()
{
    ImGui::TextDisabled("Implemented");
    ImGui::Selectable("Textures", true);
    ImGui::Spacing();
    ImGui::TextDisabled("Deferred");
    static const char* kDeferred[] = {
        "Materials", "Static Props", "Trees", "Mechs",
        "Vehicles", "VFX", "Terrain Materials", "Mod Package"
    };
    ImGui::BeginDisabled(true);
    for (const char* name : kDeferred)
        ImGui::Selectable(name, false);
    ImGui::EndDisabled();
}
