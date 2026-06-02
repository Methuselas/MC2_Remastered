/***************************************************************
 * FILENAME: AssetViewerApp.cpp
 * DESCRIPTION: Top-level application class for mc2_asset_viewer.
 ***************************************************************/
#include "AssetViewerApp.h"
#include "UiEditorImageCache.h"
#include "imgui.h"
#include "TextureExtensions.h"
#include "TextureMetadata.h"
#include "TexturePreview2D.h"
#include <SDL.h>
#include <GL/glew.h>
#include <cstdio>
#include <string>

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

static int smokeFail(const char* msg) { std::fprintf(stderr, "[smoke] FAIL: %s\n", msg); return 1; }

int AssetViewerApp::runSmoke(const char* fixtureDir)
{
    if (!IsSupportedTextureFile("a.PNG"))  return smokeFail("PNG should be supported");
    if ( IsSupportedTextureFile("a.dds"))  return smokeFail("dds should NOT be supported");
    if ( IsSupportedTextureFile("noext"))  return smokeFail("extensionless should NOT be supported");
    if (FormatDimensions(TextureMetadata{256,128,4,0}) != "256 x 128") return smokeFail("dims format");
    if (FormatChannels(TextureMetadata{0,0,4,0})       != "RGBA")      return smokeFail("channels format");
    { TextureMetadata m; m.fileBytes = 1572864; if (FormatFileSize(m) != "1.5 MB") return smokeFail("size format"); }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE; glewInit();

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;
        std::string path = std::string(fixtureDir) + "/test_rgba.png";
        surface.setSource(path);
        if (surface.hasError())               rc = smokeFail("fixture failed to load");
        else if (surface.metadata().width  != 4) rc = smokeFail("fixture width != 4");
        else if (surface.metadata().height != 2) rc = smokeFail("fixture height != 2");
        UiEditorImageCache_Shutdown();
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS\n");
    return rc;
}
