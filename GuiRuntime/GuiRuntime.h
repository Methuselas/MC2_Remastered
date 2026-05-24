// Dear ImGui in-game integration.
// ImGui authored by @methuselas (Omar Cornut et al.) -- https://github.com/ocornut/imgui
// GameOS/SDL2 backend wired for MC2 OpenGL port.
#pragma once

namespace GuiRuntime {
    void Init();      // call once after GL context created
    void Shutdown();  // call once before context destroyed
    void NewFrame();  // call each frame before game UI
    void Render();    // call each frame after all game drawing, before swap
}

extern bool g_imguiInitialized;
