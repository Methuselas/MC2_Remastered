#pragma once
#include "../RenderWorld/RenderWorld.h"
#include <cstdint>

namespace EditorInspector {

struct InspectorSelection {
    bool valid = false;
    RenderWorld::RenderObjectKind kind = RenderWorld::RenderObjectKind::StaticProp;
    RenderCore::RenderObjectHandle handle;
    RenderWorld::LookupResult lookup;
    int screenX = 0;
    int screenY = 0;
};

struct StaticPropInspectorData {
    bool    populated    = false;
    int32_t recipeIndex  = -1;
    char    shapeName[128] = {};
};

struct MechInspectorData {
    bool    populated        = false;
    char    variantName[64]  = {};
    char    longName[64]     = {};
    int     chassisClass     = 0;
    long    teamId           = -1;
    bool    disabled         = false;
    bool    destroyed        = false;
    bool    crippled         = false;
    int32_t conStat          = 0;
    float   totalCurArmor    = 0.f;
    float   totalMaxArmor    = 0.f;
    float   totalCurStr      = 0.f;
    float   totalMaxStr      = 0.f;
    char    pilotName[64]    = {};
};

void onCtrlShiftClick(int mouseX, int mouseY);  // called by missiongui.cpp (Task 7)
// Task 7 bridge: missiongui.cpp calls tryGameplayPick, then passes the result here.
// Keeps gui_runtime layering clean (gui_runtime must not link against code/ targets).
void setPickResult(int mouseX, int mouseY, const RenderWorld::LookupResult& lookup);
void setStaticPropData(const StaticPropInspectorData& sd);
void setMechData(const MechInspectorData& md);
void drawImGui();                                 // called by GuiRuntime::Render() each frame
void clear();                                     // clear selection

}  // namespace EditorInspector
