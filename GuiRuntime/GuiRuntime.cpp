#include "GuiRuntime.h"
#include "EditorInspector.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <cstdlib>   // getenv

#include "../GameOS/gameos/gos_render.h"

bool g_imguiInitialized = false;

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_IMGUI");
        cached = (!v || v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

void GuiRuntime::Init() {
    if (!isEnabled()) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(graphics::getSDLWindow(), graphics::getSDLGLContext());
    ImGui_ImplOpenGL3_Init("#version 430");

    g_imguiInitialized = true;
}

void GuiRuntime::Shutdown() {
    if (!g_imguiInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_imguiInitialized = false;
}

void GuiRuntime::NewFrame() {
    if (!g_imguiInitialized) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void GuiRuntime::Render() {
    if (!g_imguiInitialized) return;

    // F toggles the ImGui demo window.
    static bool s_showDemo = false;
    if (ImGui::IsKeyPressed(ImGuiKey_F, /*repeat=*/false))
        s_showDemo = !s_showDemo;
    if (s_showDemo)
        ImGui::ShowDemoWindow(&s_showDemo);

    EditorInspector::drawImGui();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
