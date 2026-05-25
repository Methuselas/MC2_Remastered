#pragma once
#include "../RenderWorld/RenderWorld.h"
#include "../RenderCore/MaterialGpu.h"
#include <cstdint>

namespace EditorInspector {

// Inspector's own pick-source taxonomy. Independent of RenderWorld::RenderObjectKind
// so terrain (which is not a RenderWorld object) can be a first-class pick result.
enum class InspectorPickKind : unsigned char {
    None       = 0,
    StaticProp = 1,
    Mech       = 2,
    Terrain    = 3,
};

struct InspectorSelection {
    bool valid = false;                                                  // (existing) RenderWorld lookup valid
    bool hasSelection = false;                                           // NEW
    InspectorPickKind pickKind = InspectorPickKind::None;                // NEW
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
    // MaterialGpu sidecar (populated when MC2_MATERIAL_GPU=1).
    bool    materialGpuActive     = false;
    uint32_t materialIdx          = 0xFFFFFFFFu;
    uint32_t materialGen          = 0u;
    bool    materialGpuPopulated  = false;
    RenderCore::MaterialGpu materialGpu = {};
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

struct TerrainInspectorData {
    bool  populated  = false;
    float worldX = 0.f, worldY = 0.f, worldZ = 0.f;
    int   tileRow = -1, tileCol = -1;
    int   cellRow = -1, cellCol = -1;
    int   terrainType = -1;
};

void onCtrlShiftClick(int mouseX, int mouseY);  // called by missiongui.cpp (Task 7)
// Task 7 bridge: missiongui.cpp calls tryGameplayPick, then passes the result here.
// Keeps gui_runtime layering clean (gui_runtime must not link against code/ targets).
void setPickResult(int mouseX, int mouseY, const RenderWorld::LookupResult& lookup);
void setStaticPropData(const StaticPropInspectorData& sd);
void setMechData(const MechInspectorData& md);
void setTerrainData(const TerrainInspectorData& td);
void flushDebugHighlight();
void drawImGui();                                 // called by GuiRuntime::Render() each frame
void clear();                                     // clear selection

}  // namespace EditorInspector
