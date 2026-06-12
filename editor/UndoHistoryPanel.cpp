/***************************************************************
* FILENAME: UndoHistoryPanel.cpp
* DESCRIPTION: Display-only Undo History panel (v1).
*   See UndoHistoryPanel.h. Lists all actions in the undo stack,
*   marks the cursor row with "->", dims redo-branch rows.
*   No buttons, no mutation, no save/load interaction.
* DATE: 2026-06-10
****************************************************************/

#include "stdafx.h"

#include "UndoHistoryPanel.h"

#include "imgui.h"

#include "Action.h"   // ActionUndoMgr, ActionUndoMgr::instance

#include <cstdio>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool s_open = false;

void UndoHistoryPanel::Open()   { s_open = true; }
void UndoHistoryPanel::Close()  { s_open = false; }
void UndoHistoryPanel::Toggle() { s_open = !s_open; }
bool UndoHistoryPanel::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void UndoHistoryPanel::Draw()
{
    if (!s_open)
        return;

    const ActionUndoMgr* mgr = ActionUndoMgr::instance;

    ImGui::SetNextWindowSize(ImVec2(340.f, 300.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo History", &s_open))
    {
        ImGui::End();
        return;
    }

    if (!mgr)
    {
        ImGui::TextDisabled("Undo manager not initialised.");
        ImGui::End();
        return;
    }

    const int count  = mgr->GetActionCount();
    const int cursor = mgr->GetCurrentPosition();

    // Header line
    {
        char header[80];
        std::snprintf(header, sizeof(header),
                      "Undo history (%d action%s, cursor %d)",
                      count, count == 1 ? "" : "s", cursor);
        ImGui::TextUnformatted(header);
    }
    ImGui::Separator();

    if (count == 0)
    {
        ImGui::TextDisabled("(empty)");
        ImGui::End();
        return;
    }

    // Scrollable child so a long stack doesn't overflow the window.
    ImGui::BeginChild("##undoList", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_HorizontalScrollbar);

    for (int i = 0; i < count; ++i)
    {
        const char* desc = mgr->GetActionDescription(i);

        // Redo branch: rows strictly after the cursor position.
        const bool isRedo = (i > cursor);

        char label[300];
        if (i == cursor)
        {
            // Current cursor: prefix with arrow to make it stand out.
            std::snprintf(label, sizeof(label), "-> [%d] %s", i, desc);
        }
        else
        {
            std::snprintf(label, sizeof(label), "   [%d] %s", i, desc);
        }

        ImGui::PushID(i);
        if (isRedo)
        {
            ImGui::TextDisabled("%s", label);
        }
        else if (i == cursor)
        {
            // Highlight the current cursor row.
            ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.40f, 1.00f), "%s", label);
        }
        else
        {
            ImGui::TextUnformatted(label);
        }
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}
