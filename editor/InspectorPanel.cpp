/***************************************************************
* FILENAME: InspectorPanel.cpp
* DESCRIPTION: Read-only Inspector Lite (Phase 1b modder usability).
*   See InspectorPanel.h. Reads the current selection from the existing
*   EditorObjectMgr selection list (shared by viewport + Scene Outliner)
*   and displays per-object details. No mutation, no save-path, no undo.
* DATE: 2026-06-09
****************************************************************/

#include "stdafx.h"

#include "InspectorPanel.h"

#include "imgui.h"

#include "EditorObjectMgr.h"
#include "EditorObjects.h"
#include "Forest.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool s_open = false;

void InspectorPanel::Open()   { s_open = true; }
void InspectorPanel::Close()  { s_open = false; }
void InspectorPanel::Toggle() { s_open = !s_open; }
bool InspectorPanel::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* InspectorPanel::CategoryToken(EditorObject* obj)
{
    if (!obj)                              return "Object";
    if (dynamic_cast<DropZone*>(obj))      return "DropZone";
    if (dynamic_cast<NavMarker*>(obj))     return "NavMarker";
    if (dynamic_cast<Unit*>(obj))          return "Unit";
    if (obj->getForestID() != -1)          return "Tree";
    return "Building";
}

// Return the first selected object (shared list also feeds viewport + outliner),
// or NULL. selCount (optional) receives the total selection size.
static EditorObject* inspectorFirstSelected(int* selCount)
{
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
    {
        if (selCount) *selCount = 0;
        return NULL;
    }
    EditorObjectMgr::EDITOR_OBJECT_LIST sel = mgr->getSelectedObjectList();
    if (selCount) *selCount = (int)sel.Count();
    for (EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = sel.Begin(); !it.IsDone(); it++)
    {
        if (*it)
            return (*it);
    }
    return NULL;
}

// True when ID maps to an in-range catalog slot, so the asset-name accessors
// (getFileName/getTGAFileName/getObjectTypeNum) are safe to call.
static bool inspectorAssetFieldsSafe(EditorObjectMgr* mgr, int id)
{
    if (!mgr)
        return false;
    int g   = (int)EditorObjectMgr::getGroup(id);
    int idx = (int)EditorObjectMgr::getIndexInGroup(id);
    if (g < 0 || g >= mgr->getBuildingGroupCount())
        return false;
    if (idx < 0 || idx >= mgr->getNumberBuildingsInGroup(g))
        return false;
    return true;
}

static const char* inspectorSpecialTypeName(int specialType)
{
    switch (specialType)
    {
        case EditorObjectMgr::UNSPECIAL:         return "None";
        case EditorObjectMgr::NORMAL_BUILDING:   return "Normal Building";
        case EditorObjectMgr::DROP_ZONE:         return "Drop Zone";
        case EditorObjectMgr::TURRET_CONTROL:    return "Turret Control";
        case EditorObjectMgr::GATE_CONTROL:      return "Gate Control";
        case EditorObjectMgr::POWER_STATION:     return "Power Station";
        case EditorObjectMgr::TURRET_GENERATOR:  return "Turret Generator";
        case EditorObjectMgr::SENSOR_CONTROL:    return "Sensor Control";
        case EditorObjectMgr::EDITOR_GATE:       return "Gate";
        case EditorObjectMgr::EDITOR_TURRET:     return "Turret";
        case EditorObjectMgr::SENSOR_TOWER:      return "Sensor Tower";
        case EditorObjectMgr::EDITOR_BRIDGE:     return "Bridge";
        case EditorObjectMgr::BRIDGE_CONTROL:    return "Bridge Control";
        case EditorObjectMgr::SPOTLIGHT:         return "Spotlight";
        case EditorObjectMgr::SPOTLIGHT_CONTROL: return "Spotlight Control";
        case EditorObjectMgr::DROPZONE:          return "Drop Zone";
        case EditorObjectMgr::NAV_MARKER:        return "Nav Marker";
        case EditorObjectMgr::WALL:              return "Wall";
        case EditorObjectMgr::LOOKOUT:           return "Lookout";
        case EditorObjectMgr::RESOURCE_BUILDING: return "Resource Building";
        case EditorObjectMgr::HELICOPTER:        return "Helicopter";
        default:                                 return "(unknown)";
    }
}

static const char* inspectorTeamLabel(int team)
{
    // -1 (and the editor's magic 8) read as Neutral; others as a numbered team.
    if (team < 0 || team == 8)
        return "Neutral";
    return NULL; // caller prints the number
}

// ---------------------------------------------------------------------------
// Smoke helper
// ---------------------------------------------------------------------------
bool InspectorPanel::GetSelectionSummary(char* outType, unsigned int typeLen)
{
    if (outType && typeLen)
        outType[0] = '\0';

    EditorObject* obj = inspectorFirstSelected(NULL);
    if (!obj)
    {
        if (outType && typeLen)
            std::snprintf(outType, typeLen, "%s", "none");
        return false;
    }

    if (outType && typeLen)
        std::snprintf(outType, typeLen, "%s", CategoryToken(obj));
    return true;
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void InspectorPanel::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(300.f, 360.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector", &s_open))
    {
        ImGui::End();
        return;
    }

    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    int selCount = 0;
    EditorObject* obj = inspectorFirstSelected(&selCount);

    if (!mgr)
    {
        ImGui::TextDisabled("No map loaded.");
        ImGui::End();
        return;
    }
    if (!obj)
    {
        ImGui::TextDisabled("No selection.");
        ImGui::End();
        return;
    }

    if (selCount > 1)
        ImGui::TextDisabled("%d objects selected (showing first)", selCount);

    const char* cat  = CategoryToken(obj);
    const char* name = obj->getDisplayName();
    if (!name || !name[0])
        name = "(unnamed)";

    ImGui::Text("Category: %s", cat);
    ImGui::Text("Name:     %s", name);
    ImGui::Text("ID:       0x%08lX", (unsigned long)obj->getID());
    ImGui::Separator();

    const ObjectAppearance* app = obj->appearance();
    if (app)
    {
        // teamId access is only safe with a live appearance.
        const char* teamLbl = inspectorTeamLabel(obj->getAlignment());
        if (teamLbl)
            ImGui::Text("Team:     %s", teamLbl);
        else
            ImGui::Text("Team:     %d", obj->getAlignment());

        ImGui::Text("Position: %.1f, %.1f, %.1f",
                    app->position.x, app->position.y, app->position.z);
        ImGui::Text("Rotation: %.1f", app->rotation);
    }
    else
    {
        ImGui::TextDisabled("(no appearance loaded)");
    }

    ImGui::Text("Special:  %s", inspectorSpecialTypeName(obj->getSpecialType()));

    // Asset / catalog references (bounds-guarded against malformed ids).
    if (inspectorAssetFieldsSafe(mgr, obj->getID()))
    {
        const char* file = mgr->getFileName(obj->getID());
        const char* tga  = mgr->getTGAFileName(obj->getID());
        ImGui::Text("Asset:    %s", (file && file[0]) ? file : "(none)");
        if (tga && tga[0] && std::strcmp(tga, "NONE") != 0)
            ImGui::Text("Icon TGA: %s", tga);
        ImGui::Text("Type Num: %d", mgr->getObjectTypeNum(obj->getID()));
    }

    // Unit-specific fields.
    if (Unit* unit = dynamic_cast<Unit*>(obj))
    {
        ImGui::Separator();
        ImGui::Text("Variant:  %d", unit->getVariant());
        ImGui::Text("Squad:    %lu", (unsigned long)unit->getSquad());
        int lance = -1, lanceIdx = -1;
        unit->getLanceInfo(lance, lanceIdx);
        ImGui::Text("Lance:    %d (slot %d)", lance, lanceIdx);
        Pilot* pilot = unit->getPilot();
        const char* pilotName = (pilot && pilot->info) ? pilot->getName() : NULL;
        ImGui::Text("Pilot:    %s", (pilotName && pilotName[0]) ? pilotName : "(none)");
    }

    // DropZone-specific fields.
    if (DropZone* dz = dynamic_cast<DropZone*>(obj))
    {
        ImGui::Separator();
        ImGui::Text("VTOL:     %s", dz->isVTol() ? "yes" : "no");
    }

    // Forest membership (forests select their member trees; show summary).
    if (obj->getForestID() != -1)
    {
        const Forest* f = mgr->getForest(obj->getForestID());
        ImGui::Separator();
        if (f)
        {
            const char* fname = f->getName();
            const char* ffile = f->getFileName();
            ImGui::Text("Forest:   %s (id %ld)",
                        (fname && fname[0]) ? fname : "(unnamed)", f->getID());
            if (ffile && ffile[0])
                ImGui::Text("Tree set: %s", ffile);
        }
        else
        {
            ImGui::Text("Forest:   member of forest %ld", obj->getForestID());
        }
    }

    ImGui::End();
}
