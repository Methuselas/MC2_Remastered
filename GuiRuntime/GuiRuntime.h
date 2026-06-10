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

    // Render-to-texture viewport (editor). When enabled, the editor renders the
    // scene into an offscreen texture and the dockspace paints it into the central
    // node via the background draw list (no ImGui window -> no WantCaptureMouse, so
    // picking still reaches GameOS). EditorGameOS hands the texture id here each
    // frame after compositing+blit. Picking offsets client mouse by the rect origin.
    bool RttEnabled();                              // MC2_EDITOR_RTT (default ON)
    // True when the editor force-docks all panels into the right column each launch
    // (MC2_EDITOR_AUTODOCK != 0). Panel windows must then SKIP SetNextWindowPos before
    // Begin -- an explicit next-window-pos pulls the window out of the dock and floats it.
    bool AutoDockActive();
    void SetViewportTexture(unsigned int glTex);    // GLuint; called pre-Render()
    // Fixed-layout scene rect (client/screen pixels, left-anchored). When set (w>0),
    // the dockspace paints the RTT texture into THIS rect instead of the dynamic
    // central node, and the pick offset uses it. Set each frame by EditorGameOS.
    void SetFixedViewportRect(int x, int y, int w, int h);
    // Central-node screen rect (ImGui DisplaySize space == GL-child client coords).
    int  ViewportRectX();
    int  ViewportRectY();
    int  ViewportRectW();
    int  ViewportRectH();
}

extern bool g_imguiInitialized;
