/***************************************************************
* FILENAME: GameplayDebugger.cpp
* DESCRIPTION: Read-only "Gameplay Debugger" ImGui panel.
*   See GameplayDebugger.h for full design notes.
*
*   RUNTIME AVAILABILITY: The editor process has NO live
*   Mover/MechWarrior instances. ObjectManager == nullptr at all
*   times in the editor (EditorGlobals.cpp:34). All Mover/
*   MechWarrior getters (getTacOrder, getBrainState, MovePath,
*   getAttackTarget, inventory, getSensorSystem, etc.) are
*   UNREACHABLE safely — dereffing them would fault on the null
*   ObjectManager. This panel therefore shows static editor-side
*   data only and displays a prominent notice.
*
* DATE: 2026-06-10
****************************************************************/

#include "stdafx.h"

#include "GameplayDebugger.h"

#include "imgui.h"

#include "EditorObjectMgr.h"
#include "EditorObjects.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool s_open = false;

void GameplayDebugger::Open()   { s_open = true; }
void GameplayDebugger::Close()  { s_open = false; }
void GameplayDebugger::Toggle() { s_open = !s_open; }
bool GameplayDebugger::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Map EditorObject type integer to a human-readable string.
static const char* gdbrTypeName(int t)
{
    switch (t)
    {
    case BLDG_TYPE: return "Building";
    case GV_TYPE:   return "Unit";
    default:        return "Unknown";
    }
}

// Safely read the display name, never returning null.
static const char* gdbrSafeName(const EditorObject* obj)
{
    if (!obj)
        return "(none)";
    const char* n = obj->getDisplayName();
    return (n && n[0]) ? n : "(unnamed)";
}

// ---------------------------------------------------------------------------
// Smoke hook -- mirrors Draw()'s selection gather without an ImGui frame.
// ---------------------------------------------------------------------------
bool GameplayDebugger::SmokeProbe(char* outType, unsigned int cap)
{
    if (outType && cap)
        std::snprintf(outType, cap, "%s", "none");

    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
        return false;

    EditorObjectMgr::EDITOR_OBJECT_LIST sel = mgr->getSelectedObjectList();
    EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = sel.Begin();
    if (it.IsDone() || !(*it))
        return false;

    EditorObject* obj = *it;
    if (outType && cap)
        std::snprintf(outType, cap, "%s", gdbrTypeName(obj->getType()));
    return true;
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void GameplayDebugger::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(340.f, 420.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Gameplay Debugger", &s_open))
    {
        ImGui::End();
        return;
    }

    // ---- Runtime availability notice (always shown) ----------------------
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
    ImGui::TextWrapped(
        "No runtime objects (editor is not simulating).\n"
        "Brain/path/combat state requires a live game session.\n"
        "Static placement data is shown below.");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ---- Selected object -------------------------------------------------
    EditorObjectMgr* mgr = EditorObjectMgr::instance();
    if (!mgr)
    {
        ImGui::TextDisabled("No map loaded.");
        ImGui::End();
        return;
    }

    // Collect selected objects; we care only about the first.
    EditorObjectMgr::EDITOR_OBJECT_LIST sel = mgr->getSelectedObjectList();
    EditorObject* obj = NULL;
    {
        EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = sel.Begin();
        if (!it.IsDone())
            obj = *it;
    }

    if (!obj)
    {
        ImGui::TextDisabled("No object selected.");
        ImGui::End();
        return;
    }

    // ---- Static editor-side data -----------------------------------------
    ImGui::Text("Selected: %s", gdbrSafeName(obj));
    ImGui::Separator();

    // Type
    ImGui::Text("Type:  %s (%d)", gdbrTypeName(obj->getType()), obj->getType());

    // Team / alignment
    if (obj->appearance())
        ImGui::Text("Team:  %d", obj->getAlignment());
    else
        ImGui::TextDisabled("Team:  (no appearance)");

    // Position
    {
        const Stuff::Vector3D& pos = obj->getPosition();
        ImGui::Text("Pos:   (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
    }

    // Unit-specific fields (variant, pilot) -- only on Unit objects.
    // Unit reports GV_TYPE; safe downcast gated on getType().
    if (obj->getType() == GV_TYPE)
    {
        Unit* unit = static_cast<Unit*>(obj);

        // Variant index
        ImGui::Text("Variant: %d", unit->getVariant());

        // Pilot name
        Pilot* pilot = unit->getPilot();
        if (pilot && pilot->info && pilot->info->name && pilot->info->name[0])
            ImGui::Text("Pilot: %s", pilot->info->name);
        else
            ImGui::TextDisabled("Pilot: (none)");

        // Lance
        {
            int lance = 0, lanceIdx = 0;
            unit->getLanceInfo(lance, lanceIdx);
            ImGui::Text("Lance: %d  slot: %d", lance, lanceIdx);
        }

        // Squad
        ImGui::Text("Squad: %lu", (unsigned long)unit->getSquad());
    }

    ImGui::Separator();

    // ---- Runtime state section (always shown as unavailable) -------------
    if (ImGui::CollapsingHeader("Runtime State (unavailable in editor)"))
    {
        ImGui::TextDisabled("Tac Order:     --");
        ImGui::TextDisabled("Brain State:   --");
        ImGui::TextDisabled("Path Steps:    --");
        ImGui::TextDisabled("Path Cost:     --");
        ImGui::TextDisabled("Attack Target: --");
        ImGui::TextDisabled("Body State:    --");
        ImGui::TextDisabled("Shutdown:      --");
        ImGui::TextDisabled("Path Locks:    --");
        ImGui::TextDisabled("Sensor Range:  --");
        ImGui::TextDisabled("Sensor Contacts: --");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "These fields are populated when the engine runs a live game\n"
            "session with simulation active. The editor only holds\n"
            "placement data; ObjectManager is null in this process.");
    }

    ImGui::End();
}
