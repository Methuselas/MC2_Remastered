/***************************************************************
* FILENAME: ObjectivesSummaryPanel.cpp
* DESCRIPTION: Read-only objectives explainer / mission-logic viewer.
*   Phase 2: three-pane shell (Objective List | WHEN/THEN flow | Inspector).
*   Walks EditorData::instance->TeamsRef() -> per team CObjectives (an EList of
*   CObjective*). LEFT pane lists every objective (grouped by team) with status
*   badges and a Selectable; CENTER pane renders the selected objective's
*   readable activation + WHEN/THEN/Failure flow; RIGHT pane dumps the raw
*   scalar fields (inspector).
*
*   Condition/action lines are built from Description() (the type label) +
*   InstanceDescription() (the params). CRITICAL: the specific-unit and
*   specific-structure condition types deref a live object pointer with no null
*   guard inside InstanceDescription() (Objective.cpp:269 / :383, and they even
*   assert(displayName)). For those species we render Description() only plus an
*   "(object ref)" note, never InstanceDescription(). Area/flag/time/count
*   InstanceDescription() impls touch no object pointer and are safe.
*
*   PURE-ADDITIVE, READ-ONLY: no mutate buttons, no save/load. Selection is a raw
*   CObjective* compared by identity; a mission reload simply clears it (the old
*   pointer no longer appears in the rebuilt list).
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
#include <cstring>

// GAME_MAX_PLAYERS is pulled in transitively (CTeams holds CTeam[GAME_MAX_PLAYERS]).

// ---------------------------------------------------------------------------
// Panel + selection state
// ---------------------------------------------------------------------------
static bool        s_open    = false;
static CObjective* s_sel     = nullptr;   // selected objective (identity compare)
static int         s_selTeam = -1;        // team the selection belongs to (label only)

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

// Compose a short status-badge string for the list row, e.g. "[Primary][Hidden]".
static void objBadges(CObjective* obj, char* out, size_t cap)
{
    out[0] = '\0';
    if (!obj || cap == 0) return;
    if (obj->Priority() == 1)   std::strncat(out, "[Primary]", cap - std::strlen(out) - 1);
    if (obj->IsHiddenTrigger()) std::strncat(out, "[Hidden]",  cap - std::strlen(out) - 1);
    else                        std::strncat(out, "[Visible]", cap - std::strlen(out) - 1);
    if (obj->DisplayMarker())   std::strncat(out, "[Marker]",  cap - std::strlen(out) - 1);
}

// Build the readable title text for a row / header.
static void objTitleText(CObjective* obj, int objIndex, char* out, size_t cap)
{
    if (obj->TitleUseResourceString())
    {
        std::snprintf(out, cap, "%d: (resource string #%d)", objIndex,
                      obj->TitleResourceStringID());
        return;
    }
    EString title = obj->Title();
    const char* t = title.Data();
    std::snprintf(out, cap, "%d: %s", objIndex, (t && t[0]) ? t : "(untitled)");
}

// CENTER pane: readable activation + WHEN/THEN/Failure flow for one objective.
static void objDrawFlow(CObjective* obj)
{
    if (!obj)
    {
        ImGui::TextDisabled("Select an objective from the list.");
        return;
    }

    char title[160];
    objTitleText(obj, 0, title, sizeof(title));   // index shown by the list, not here
    const char* colon = std::strchr(title, ':');
    ImGui::TextWrapped("%s", colon ? colon + 2 : title);

    char badges[64];
    objBadges(obj, badges, sizeof(badges));
    ImGui::TextDisabled("%s", badges);
    ImGui::Separator();

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
    if (!obj->PreviousPrimaryObjectiveMustBeComplete() &&
        !obj->AllPreviousPrimaryObjectivesMustBeComplete() &&
        !obj->ActivateOnFlag())
        ImGui::BulletText("Active from mission start");

    if (obj->DisplayMarker())
        ImGui::Text("Marker: shown at (%.1f, %.1f)", obj->MarkerX(), obj->MarkerY());
    else
        ImGui::TextDisabled("Marker: none");

    // WHEN: success conditions. CObjective inherits CObjectiveConditionList, so
    // iterating *this* objective walks its success conditions.
    ImGui::Separator();
    ImGui::TextUnformatted("WHEN (success conditions)");
    if (obj->Count() == 0)
        ImGui::TextDisabled("  (no conditions)");
    else
        for (CObjective::condition_list_type::EIterator it = obj->Begin(); !it.IsDone(); it++)
            objDrawCondition(*it);

    // THEN: actions fired on success.
    ImGui::TextUnformatted("THEN (actions)");
    if (obj->m_actionList.Count() == 0)
        ImGui::TextDisabled("  (no actions)");
    else
        for (CObjective::action_list_type::EIterator it = obj->m_actionList.Begin(); !it.IsDone(); it++)
            objDrawAction(*it);

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
}

// RIGHT pane: raw scalar fields for the selected objective.
static void objDrawInspector(CObjective* obj)
{
    if (!obj)
    {
        ImGui::TextDisabled("(no selection)");
        return;
    }

    ImGui::Text("Priority:        %d", obj->Priority());
    ImGui::Text("Resource pts:    %d", obj->ResourcePoints());
    ImGui::Text("Hidden trigger:  %s", obj->IsHiddenTrigger() ? "yes" : "no");
    ImGui::Text("Display marker:  %s", obj->DisplayMarker() ? "yes" : "no");
    ImGui::Text("Marker X/Y:      %.1f / %.1f", obj->MarkerX(), obj->MarkerY());
    ImGui::Separator();
    ImGui::Text("Prev primary req:    %s", obj->PreviousPrimaryObjectiveMustBeComplete() ? "yes" : "no");
    ImGui::Text("All prev primary:    %s", obj->AllPreviousPrimaryObjectivesMustBeComplete() ? "yes" : "no");
    ImGui::Text("Activate on flag:    %s", obj->ActivateOnFlag() ? "yes" : "no");
    {
        EString flag = obj->ActivateFlagID();
        const char* f = flag.Data();
        ImGui::Text("Activate flag id:    %s", (f && f[0]) ? f : "(none)");
    }
    ImGui::Text("Reset on flag:       %s", obj->ResetStatusOnFlag() ? "yes" : "no");
    {
        EString flag = obj->ResetStatusFlagID();
        const char* f = flag.Data();
        ImGui::Text("Reset flag id:       %s", (f && f[0]) ? f : "(none)");
    }
    ImGui::Separator();
    ImGui::Text("Success conditions:  %lu", (unsigned long)obj->Count());
    ImGui::Text("Success actions:     %lu", (unsigned long)obj->m_actionList.Count());
    ImGui::Text("Failure conditions:  %lu", (unsigned long)obj->m_failureConditionList.Count());
    ImGui::Text("Failure actions:     %lu", (unsigned long)obj->m_failureActionList.Count());
    ImGui::Separator();
    ImGui::Text("Title resource str:  %s", obj->TitleUseResourceString() ? "yes" : "no");
    ImGui::Text("Model ID:            %ld", obj->ModelID());
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void ObjectivesSummaryPanel::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(800.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Objectives Overview", &s_open))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Read-only mission-logic viewer. Edit via the legacy Objectives dialog.");
    ImGui::Separator();

    if (!EditorData::instance)
    {
        ImGui::TextDisabled("No mission loaded.");
        ImGui::End();
        return;
    }

    CTeams& teams = EditorData::instance->TeamsRef();

    // Validate the current selection still exists in the live data; otherwise drop
    // it (mission reloads rebuild the objective objects).
    bool selStillValid = false;

    // ---- LEFT pane: objective list grouped by team -------------------------
    ImGui::BeginChild("obj_list", ImVec2(250.f, 0.f), true);
    for (int t = 0; t < GAME_MAX_PLAYERS; ++t)
    {
        CObjectives& objs = teams.TeamRef(t).ObjectivesRef();
        if (objs.Count() == 0)
            continue;

        char teamHdr[64];
        std::snprintf(teamHdr, sizeof(teamHdr), "Team %d (%lu)###objteam%d",
                      t, (unsigned long)objs.Count(), t);
        if (!ImGui::CollapsingHeader(teamHdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        int idx = 0;
        for (CObjectives::EIterator it = objs.Begin(); !it.IsDone(); it++, ++idx)
        {
            CObjective* obj = *it;
            if (!obj)
                continue;

            char title[160], badges[64], row[240];
            objTitleText(obj, idx, title, sizeof(title));
            objBadges(obj, badges, sizeof(badges));
            std::snprintf(row, sizeof(row), "%s  %s###obj_%p", title, badges, (void*)obj);

            bool selected = (obj == s_sel);
            if (ImGui::Selectable(row, selected))
            {
                s_sel = obj;
                s_selTeam = t;
            }
            if (obj == s_sel)
                selStillValid = true;
        }
    }
    ImGui::EndChild();

    if (!selStillValid)
    {
        s_sel = nullptr;
        s_selTeam = -1;
    }

    // ---- CENTER pane: WHEN/THEN flow ---------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("obj_flow", ImVec2(-230.f, 0.f), true);
    if (s_sel && s_selTeam >= 0)
        ImGui::TextDisabled("Team %d", s_selTeam);
    objDrawFlow(s_sel);
    ImGui::EndChild();

    // ---- RIGHT pane: inspector ---------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("obj_inspector", ImVec2(0.f, 0.f), true);
    ImGui::TextUnformatted("Inspector");
    ImGui::Separator();
    objDrawInspector(s_sel);
    ImGui::EndChild();

    ImGui::End();
}

#endif // MC2_IMGUI
