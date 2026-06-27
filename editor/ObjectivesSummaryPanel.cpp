/***************************************************************
* FILENAME: ObjectivesSummaryPanel.cpp
* DESCRIPTION: Read-only objectives explainer. See ObjectivesSummaryPanel.h.
*   Walks EditorData::instance->TeamsRef() -> per team CObjectives (an EList of
*   CObjective*). For each objective renders a card: title/priority/visibility/
*   activation/marker, then a WHEN block (success conditions), THEN block
*   (actions) and a Failure block (failure conditions/actions).
*
*   Condition/action lines are built from Description() (the type label) +
*   InstanceDescription() (the params). CRITICAL: the specific-unit and
*   specific-structure condition types deref a live object pointer with no null
*   guard inside InstanceDescription() (Objective.cpp:269 / :383, and they even
*   assert(displayName)). For those species we render Description() only plus an
*   "(object ref)" note, never InstanceDescription(). Area/flag/time/count
*   InstanceDescription() impls touch no object pointer and are safe.
*
*   PURE-ADDITIVE, READ-ONLY: no mutate buttons, no save/load.
* DATE: 2026-06-27
****************************************************************/

// Pull stdafx first (MFC / Windows headers).
#include "stdafx.h"

#ifdef MC2_IMGUI

#include "imgui.h"

#include "ObjectivesSummaryPanel.h"

#include "EditorData.h"   // EditorData::instance, CTeams/CTeam, ObjectivesRef()
#include "Objective.h"    // CObjective(s), condition/action lists, species enums
#include "EString.h"

#include <cstdio>

// GAME_MAX_PLAYERS is pulled in transitively (CTeams holds CTeam[GAME_MAX_PLAYERS]).

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool s_open = false;

void ObjectivesSummaryPanel::Open()   { s_open = true; }
void ObjectivesSummaryPanel::Close()  { s_open = false; }
void ObjectivesSummaryPanel::Toggle() { s_open = !s_open; }
bool ObjectivesSummaryPanel::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// True for the condition species whose InstanceDescription() dereferences a live
// EditorObject pointer (m_pUnit / m_pBuilding) with no null guard. We must NOT
// call InstanceDescription() on these from a read-only viewer.
static bool objIsObjectRefCondition(condition_species_type sp)
{
    switch (sp)
    {
        case DESTROY_SPECIFIC_ENEMY_UNIT:
        case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT:
        case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT:
        case CAPTURE_UNIT:
        case GUARD_SPECIFIC_UNIT:
        case DESTROY_SPECIFIC_STRUCTURE:
        case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE:
        case CAPTURE_STRUCTURE:
        case GUARD_SPECIFIC_STRUCTURE:
            return true;
        default:
            return false;
    }
}

// Render one success/failure condition line. Object-ref species print only their
// safe Description() label plus an "(object ref)" note. All others append the
// (safe) InstanceDescription() params when present.
static void objDrawCondition(CObjectiveCondition* cond)
{
    if (!cond)
    {
        ImGui::BulletText("(null condition)");
        return;
    }

    EString desc = cond->Description();   // local: EString returned by value
    const char* label = desc.Data();
    if (!label) label = "(condition)";

    if (objIsObjectRefCondition(cond->Species()))
    {
        ImGui::BulletText("%s  (object ref)", label);
        return;
    }

    EString inst = cond->InstanceDescription();
    const char* params = inst.Data();
    if (params && params[0])
        ImGui::BulletText("%s  %s", label, params);
    else
        ImGui::BulletText("%s", label);
}

// Render one action line. All action InstanceDescription() impls touch only
// EString/int members (no object pointer deref) -> safe to call.
static void objDrawAction(CObjectiveAction* act)
{
    if (!act)
    {
        ImGui::BulletText("(null action)");
        return;
    }

    EString desc = act->Description();
    const char* label = desc.Data();
    if (!label) label = "(action)";

    EString inst = act->InstanceDescription();
    const char* params = inst.Data();
    if (params && params[0])
        ImGui::BulletText("%s  %s", label, params);
    else
        ImGui::BulletText("%s", label);
}

// Render the full card for one objective.
static void objDrawObjective(CObjective* obj, int objIndex)
{
    if (!obj)
    {
        ImGui::TextDisabled("Objective %d: (null)", objIndex);
        return;
    }

    // Title (resource-string objectives have no inline text; show a placeholder).
    EString title = obj->Title();
    const char* titleStr = title.Data();
    char header[160];
    if (obj->TitleUseResourceString())
        std::snprintf(header, sizeof(header), "Objective %d: (resource string #%d)",
                      objIndex, obj->TitleResourceStringID());
    else
        std::snprintf(header, sizeof(header), "Objective %d: %s",
                      objIndex, (titleStr && titleStr[0]) ? titleStr : "(untitled)");

    if (!ImGui::TreeNodeEx((void*)obj, ImGuiTreeNodeFlags_DefaultOpen, "%s", header))
        return;

    // Type / priority. Priority 1 is the editor's "primary" marker (see
    // ObjectivesDlg list flags).
    ImGui::Text("Priority: %d%s", obj->Priority(),
                (obj->Priority() == 1) ? " (primary)" : "");
    ImGui::Text("Visibility: %s", obj->IsHiddenTrigger() ? "Hidden trigger" : "Visible");

    // Activation gating.
    if (obj->PreviousPrimaryObjectiveMustBeComplete())
        ImGui::BulletText("Activates after previous primary objective completes");
    if (obj->AllPreviousPrimaryObjectivesMustBeComplete())
        ImGui::BulletText("Activates after ALL previous primary objectives complete");
    if (obj->ActivateOnFlag())
    {
        EString flag = obj->ActivateFlagID();
        const char* f = flag.Data();
        ImGui::BulletText("Activates on flag: %s", (f && f[0]) ? f : "(flag)");
    }

    // Marker.
    if (obj->DisplayMarker())
        ImGui::Text("Marker: shown at (%.1f, %.1f)", obj->MarkerX(), obj->MarkerY());
    else
        ImGui::TextDisabled("Marker: none");

    // WHEN: success conditions. CObjective inherits CObjectiveConditionList, so
    // iterating *this* objective walks its success conditions.
    ImGui::Separator();
    ImGui::TextUnformatted("WHEN (success conditions)");
    if (obj->Count() == 0)
    {
        ImGui::TextDisabled("  (no conditions)");
    }
    else
    {
        for (CObjective::condition_list_type::EIterator it = obj->Begin(); !it.IsDone(); it++)
            objDrawCondition(*it);
    }

    // THEN: actions fired on success.
    ImGui::TextUnformatted("THEN (actions)");
    if (obj->m_actionList.Count() == 0)
    {
        ImGui::TextDisabled("  (no actions)");
    }
    else
    {
        for (CObjective::action_list_type::EIterator it = obj->m_actionList.Begin(); !it.IsDone(); it++)
            objDrawAction(*it);
    }

    // Failure block (only if present).
    if (obj->m_failureConditionList.Count() > 0 || obj->m_failureActionList.Count() > 0)
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Failure conditions");
        if (obj->m_failureConditionList.Count() == 0)
            ImGui::TextDisabled("  (none)");
        else
            for (CObjective::condition_list_type::EIterator it = obj->m_failureConditionList.Begin(); !it.IsDone(); it++)
                objDrawCondition(*it);

        ImGui::TextUnformatted("Failure actions");
        if (obj->m_failureActionList.Count() == 0)
            ImGui::TextDisabled("  (none)");
        else
            for (CObjective::action_list_type::EIterator it = obj->m_failureActionList.Begin(); !it.IsDone(); it++)
                objDrawAction(*it);
    }

    ImGui::TreePop();
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void ObjectivesSummaryPanel::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(420.f, 480.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Objectives Overview", &s_open))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Read-only objective overview. Edit via the legacy Objectives dialog.");
    ImGui::Separator();

    if (!EditorData::instance)
    {
        ImGui::TextDisabled("No mission loaded.");
        ImGui::End();
        return;
    }

    CTeams& teams = EditorData::instance->TeamsRef();
    for (int t = 0; t < GAME_MAX_PLAYERS; ++t)
    {
        CObjectives& objs = teams.TeamRef(t).ObjectivesRef();

        char teamHdr[64];
        std::snprintf(teamHdr, sizeof(teamHdr), "Team %d  (%lu objective%s)###objteam%d",
                      t, (unsigned long)objs.Count(),
                      (objs.Count() == 1) ? "" : "s", t);

        if (!ImGui::CollapsingHeader(teamHdr))
            continue;

        ImGui::Indent();
        if (objs.Count() == 0)
        {
            ImGui::TextDisabled("(no objectives)");
        }
        else
        {
            int idx = 0;
            for (CObjectives::EIterator it = objs.Begin(); !it.IsDone(); it++, ++idx)
                objDrawObjective(*it, idx);
        }
        ImGui::Unindent();
    }

    ImGui::End();
}

#endif // MC2_IMGUI
