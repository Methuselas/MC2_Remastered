#pragma once

namespace GuiRuntime {
    void Init();      // call once after GL context created
    void Shutdown();  // call once before context destroyed
    void NewFrame();  // call each frame before game UI
    void Render();    // call each frame after all game drawing, before swap
}

extern bool g_imguiInitialized;
