/***************************************************************
* FILENAME: MissionValidation.cpp
* DESCRIPTION: Mission save-readiness checks and ImGui checklist control surface.
* DATE: 2026-06-07
****************************************************************/

#include "stdafx.h"
#include "MissionValidation.h"
#include "EditorData.h"
#include "MCLib.h"        // extern TerrainPtr land

#ifdef MC2_IMGUI
#include <imgui.h>
#endif

// ---------------------------------------------------------------------------
// ValidateForPakSave
// ---------------------------------------------------------------------------

std::vector<MissionCheck> MissionValidator::ValidateForPakSave() {
    std::vector<MissionCheck> checks;

    auto push = [&](const char* id, const char* label,
                    MissionCheckSeverity sev, bool passed,
                    ChecklistAction action, std::string details) {
        MissionCheck c;
        c.id      = id;
        c.label   = label;
        c.severity = sev;
        c.passed  = passed;
        c.action  = action;
        c.details = std::move(details);
        checks.push_back(c);
    };

    // 1. Terrain loaded -- BLOCKING
    //    land is the global TerrainPtr (extern in MCLib.h / terrain.h).
    //    Save() crashes at land->realVerticesMapSide when land is null.
    bool terrainLoaded = (land != nullptr);
    push("terrain_loaded",
         "Terrain is loaded",
         MissionCheckSeverity::Blocking,
         terrainLoaded,
         ChecklistAction::OpenMapGenerator,
         terrainLoaded
             ? "Terrain data is in memory.  Save can proceed."
             : "No terrain is loaded.  Generate or open a map first.");

    if (!terrainLoaded)
        return checks;

    // 2. Map has a real save path -- INFO
    //    Default 'newmap' path makes Save() redirect to SaveAs() (file dialog).
    //    Not a hard block, but surfaced so users know what to expect.
    const char* mapName = EditorData::instance->getMapName();
    bool hasPath = mapName && strcmp(mapName, "data\\missions\\newmap.pak") != 0;
    push("save_path",
         "Map has a save path",
         MissionCheckSeverity::Info,
         hasPath,
         ChecklistAction::OpenSaveAs,
         hasPath
             ? (std::string("Save path: ") + mapName)
             : "Using the default 'newmap' path.  File > Save (Ctrl+S) will open a "
               "Save As dialog to let you choose a filename before writing the .pak.");

    // 3. MOVE pathfinding data -- WARNING
    //    Generated maps skip MOVE_buildData (intentional CTD prevention when the
    //    MOVE backend is uninitialized for blank terrain).  The save succeeds but
    //    AI movement does not work in the resulting mission.
    //    Fix: save the mission once its terrain features are finalized; MOVE data
    //    is rebuilt automatically on each non-quick save for missions that have it.
    //    For generated maps, the flag becomes true only after a packet-4 round-trip
    //    (i.e. after the mission is saved and reloaded as an existing mission).
    bool moveReady = EditorData::IsMoveDataReadyForFullSave();
    push("move_data_ready",
         "MOVE pathfinding data ready",
         MissionCheckSeverity::Warning,
         moveReady,
         ChecklistAction::OpenSaveAs,   // Save As is the first step; MOVE rebuilds on reload
         moveReady
             ? "MOVE pathfinding data is initialized and will be saved."
             : "Generated terrain does not have MOVE pathfinding data yet.\n"
               "The mission can be saved, but AI movement will not work until MOVE "
               "data is built.\n\n"
               "Rebuild MOVE Data:\n"
               "  1. Save As...  (write the .pak file)\n"
               "  2. Reopen the saved mission  (File > Open)\n"
               "  3. Save again  (the editor rebuilds MOVE on the second save)");

    // 4. Objectives have conditions -- WARNING
    //    Same check the editor shows as a MessageBox during save.  Surfaced here
    //    so users can fix it before save rather than being surprised mid-save.
    bool objsOk = !EditorData::instance->TeamsRef().ThereAreObjectivesWithNoConditions();
    push("objectives_conditions",
         "All objectives have conditions",
         MissionCheckSeverity::Warning,
         objsOk,
         ChecklistAction::OpenObjectives,
         objsOk
             ? "All objectives have at least one condition (or there are no objectives)."
             : "One or more objectives have no conditions -- they can never be completed.\n\n"
               "Open Mission > Teams, select the objective, and add at least one condition.  "
               "Or remove objectives that are not yet ready.");

    return checks;
}

// ---------------------------------------------------------------------------
// Quick helpers
// ---------------------------------------------------------------------------

/*static*/ bool MissionValidator::HasBlockingFailures() {
    for (const auto& c : ValidateForPakSave())
        if (!c.passed && c.severity == MissionCheckSeverity::Blocking) return true;
    return false;
}

/*static*/ bool MissionValidator::HasWarnings() {
    for (const auto& c : ValidateForPakSave())
        if (!c.passed && c.severity == MissionCheckSeverity::Warning) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

static bool                      s_open         = false;
static std::vector<MissionCheck> s_lastChecks;
static bool                      s_needsRefresh = true;
static ChecklistAction           s_pendingAction = ChecklistAction::None;

void MissionValidator::Open()    { s_open = true;  s_needsRefresh = true; }
void MissionValidator::Close()   { s_open = false; }
bool MissionValidator::IsOpen()  { return s_open; }

/*static*/ ChecklistAction MissionValidator::TakeAction() {
    ChecklistAction act = s_pendingAction;
    s_pendingAction = ChecklistAction::None;
    return act;
}

/*static*/ void MissionValidator::QueueAction(ChecklistAction act) {
    s_pendingAction = act;
}

// ---------------------------------------------------------------------------
// ImGui panel
// ---------------------------------------------------------------------------

#ifdef MC2_IMGUI

namespace {
    constexpr ImVec4 kColBlock { 0.95f, 0.25f, 0.20f, 1.0f };
    constexpr ImVec4 kColWarn  { 0.95f, 0.75f, 0.10f, 1.0f };
    constexpr ImVec4 kColInfo  { 0.50f, 0.70f, 1.00f, 1.0f };
    constexpr ImVec4 kColPass  { 0.30f, 0.85f, 0.30f, 1.0f };
    constexpr ImVec4 kColGrey  { 0.55f, 0.55f, 0.55f, 1.0f };

    const char* ActionLabel(ChecklistAction act) {
        switch (act) {
            case ChecklistAction::OpenMapGenerator: return "Generate Map...";
            case ChecklistAction::OpenSaveAs:       return "Save As...";
            case ChecklistAction::OpenObjectives:   return "Open Objectives";
            default:                                return nullptr;
        }
    }
}

void MissionValidator::Draw() {
    if (!s_open) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float    sc = io.DisplaySize.x / 1280.f;

    ImGui::SetNextWindowSize(ImVec2(450.f * sc, 0.f), ImGuiCond_Once);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f - 225.f * sc,
               io.DisplaySize.y * 0.5f - 150.f * sc),
        ImGuiCond_Once);

    bool open = s_open;
    if (!ImGui::Begin("Mission Save Readiness", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        s_open = open;
        return;
    }
    s_open = open;

    if (s_needsRefresh) {
        s_lastChecks   = MissionValidator::ValidateForPakSave();
        s_needsRefresh = false;
    }

    if (ImGui::Button("Refresh"))
        s_lastChecks = MissionValidator::ValidateForPakSave();
    ImGui::SameLine();
    ImGui::TextDisabled("Checks run against live editor state.");

    ImGui::Separator();

    // Summary
    int blockCount = 0, warnCount = 0;
    for (const auto& c : s_lastChecks) {
        if (!c.passed) {
            if      (c.severity == MissionCheckSeverity::Blocking) ++blockCount;
            else if (c.severity == MissionCheckSeverity::Warning)  ++warnCount;
        }
    }

    if (blockCount > 0) {
        ImGui::TextColored(kColBlock,
            "%d blocking issue%s, %d warning%s  --  fix blocking issues before saving.",
            blockCount, blockCount == 1 ? "" : "s",
            warnCount,  warnCount  == 1 ? "" : "s");
    } else if (warnCount > 0) {
        ImGui::TextColored(kColWarn,
            "No blocking issues.  %d warning%s  --  save will work but review warnings.",
            warnCount, warnCount == 1 ? "" : "s");
    } else if (!s_lastChecks.empty()) {
        ImGui::TextColored(kColPass, "All checks passed -- ready to save.");
    } else {
        ImGui::TextColored(kColGrey, "(No checks run -- click Refresh)");
    }

    ImGui::Spacing();

    // Check rows
    for (const auto& c : s_lastChecks) {
        const char* icon;
        ImVec4      colour;
        if (c.passed) {
            icon   = "OK";
            colour = kColPass;
        } else {
            switch (c.severity) {
                case MissionCheckSeverity::Blocking: icon = "!!"; colour = kColBlock; break;
                case MissionCheckSeverity::Warning:  icon = " !"; colour = kColWarn;  break;
                default:                             icon = " i"; colour = kColInfo;  break;
            }
        }

        ImGui::TextColored(colour, "[%s]", icon);
        if (ImGui::IsItemHovered() && !c.details.empty())
            ImGui::SetTooltip("%s", c.details.c_str());

        ImGui::SameLine();
        ImGui::TextUnformatted(c.label);
        if (ImGui::IsItemHovered() && !c.details.empty())
            ImGui::SetTooltip("%s", c.details.c_str());

        // Per-check action button (only on failing checks that have an action)
        const char* btnLabel = (!c.passed) ? ActionLabel(c.action) : nullptr;
        if (btnLabel) {
            ImGui::SameLine();
            char btnId[128];
            snprintf(btnId, sizeof(btnId), "%s##%s", btnLabel, c.id);
            if (ImGui::SmallButton(btnId))
                s_pendingAction = c.action;
            if (ImGui::IsItemHovered() && !c.details.empty())
                ImGui::SetTooltip("%s", c.details.c_str());
        }
    }

    ImGui::Separator();

    // "Prepare Saveable Mission" bottom button.
    // Re-validates and triggers the first failing check's action.
    // Does NOT create mission content -- only navigates to the right editor panel.
    bool anyFailing = (blockCount > 0 || warnCount > 0);
    if (!anyFailing) ImGui::BeginDisabled();
    if (ImGui::Button("Prepare Saveable Mission", ImVec2(-1.f, 0.f)))
        s_pendingAction = ChecklistAction::PrepareSaveable;
    if (!anyFailing) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(anyFailing
            ? "Navigates to the first issue that needs attention.\n"
              "Does NOT create fake mission content."
            : "No issues to fix.");
    }

    ImGui::End();
}

#else // !MC2_IMGUI
void MissionValidator::Draw() {}
#endif // MC2_IMGUI
