#pragma once

namespace GraphicsOptionsWindow {
    void draw();       // call from GuiRuntime::Render(), before ImGui::Render()
    void setOpen(bool open);
    bool isOpen();
}
