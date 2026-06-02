/***************************************************************
 * FILENAME: AssetViewerApp.cpp
 * DESCRIPTION: Top-level application class for mc2_asset_viewer.
 ***************************************************************/
#include "AssetViewerApp.h"
#include "UiEditorImageCache.h"
#include "imgui.h"

AssetViewerApp::AssetViewerApp()  { UiEditorImageCache_Initialize(); }
AssetViewerApp::~AssetViewerApp() { UiEditorImageCache_Shutdown(); }

void AssetViewerApp::drawUi()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MC2 Asset Viewer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float sidebarW = 180.0f, browserW = 300.0f;
    ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0), true);
    sidebar_.draw();
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("browser", ImVec2(browserW, 0), true);
    browser_.draw();
    if (browser_.hasSelection())
        surface_.setSource(browser_.takeSelection());
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("inspector", ImVec2(0, 0), true);
    inspector_.draw(surface_);
    ImGui::EndChild();

    ImGui::End();
}

int AssetViewerApp::runSmoke(const char* /*fixtureDir*/) { return 0; } // filled in Task 8
