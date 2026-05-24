#pragma once
#include "../RenderWorld/RenderWorld.h"

namespace EditorInspector {

struct InspectorSelection {
    bool valid = false;
    RenderWorld::RenderObjectKind kind = RenderWorld::RenderObjectKind::StaticProp;
    RenderCore::RenderObjectHandle handle;
    RenderWorld::LookupResult lookup;
    int screenX = 0;
    int screenY = 0;
};

void onCtrlShiftClick(int mouseX, int mouseY);  // called by missiongui.cpp (Task 7)
// Task 7 bridge: missiongui.cpp calls tryGameplayPick, then passes the result here.
// Keeps gui_runtime layering clean (gui_runtime must not link against code/ targets).
void setPickResult(int mouseX, int mouseY, const RenderWorld::LookupResult& lookup);
void drawImGui();                                 // called by GuiRuntime::Render() each frame
void clear();                                     // clear selection

}  // namespace EditorInspector
