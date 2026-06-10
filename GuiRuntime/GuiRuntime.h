// Dear ImGui in-game integration -- wired for MC2 OpenGL port in collaboration with @methuselas.
// Dear ImGui by Omar Cornut et al. -- https://github.com/ocornut/imgui
#pragma once

namespace GuiRuntime {
    void Init();      // call once after GL context created
    void Shutdown();  // call once before context destroyed
    void NewFrame();  // call each frame before game UI
    void Render();    // call each frame after all game drawing, before swap

    // Size of the dockspace CENTRAL node (the area not covered by docked panels) in
    // pixels. The editor shrinks its scene viewport to this so the map fills only the
    // un-docked region. 0 until the dockspace is laid out (or docking off). Lags the
    // live layout by one frame (built in NewFrame, read at frame top).
    int SceneViewportWidth();
    int SceneViewportHeight();
}

extern bool g_imguiInitialized;
