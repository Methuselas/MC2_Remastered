/***************************************************************
* FILENAME: MissionValidation.cpp
* DESCRIPTION: Read-only mission save-readiness checks and ImGui checklist panel.
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
                    MissionCheckSeverity sev, bool passed, bool fixable,
                    std::string details) {
        MissionCheck c;
        c.id          = id;
        c.label       = label;
        c.severity    = sev;
        c.passed      = passed;
        c.autoFixable = fixable;
        c.details     = std::move(details);
        checks.push_back(c);
    };

    // 1. Terrain loaded -- BLOCKING
    //    land is the global TerrainPtr (extern in MCLib.h / terrain.h).
    //    If null the save path crashes at land->realVerticesMapSide.
    bool terrainLoaded = (land != nullptr);
    push("terrain_loaded",
         "Terrain is loaded",
         MissionCheckSeverity::Blocking,
         terrainLoaded,
         /*fixable*/ false,
         terrainLoaded
             ? "Terrain data is in memory.  Save can proceed."
             : "No terrain is loaded.  Generate or open a map first "
               "(File > New or the Generate Map button).");

    if (!terrainLoaded)
        return checks;   // remaining checks dereference land or EditorData

    // 2. Map has a save path (not the default 'newmap') -- INFO
    //    If the map name is "data\\missions\\newmap.pak" (or null), Save() redirects
    //    to SaveAs() which opens a file-chooser dialog.  Not a hard block, but
    //    surfaced here so users know what to expect.
    const char* mapName = EditorData::instance->getMapName();
    bool hasPath = mapName && (strcmp(mapName, "data\\missions\\newmap.pak") != 0);
    push("save_path",
         "Map has a save path",
         MissionCheckSeverity::Info,
         hasPath,
         /*fixable*/ false,
         hasPath
             ? (std::string("Save path: ") + mapName)
             : "Using the default 'newmap' path.  File > Save (Ctrl+S) will open a "
               "Save As dialog so you can choose a filename before saving.");

    // 3. MOVE pathfinding data ready -- WARNING
    //    gEditorDataMoveDataReadyForFullSave is set false when terrain is generated
    //    from scratch (to prevent CTD from uninitialized MOVE backend).  The .pak
    //    saves without it but the game AI cannot path-find in the mission.
    bool moveReady = EditorData::IsMoveDataReadyForFullSave();
    push("move_data_ready",
         "MOVE pathfinding data ready",
         MissionCheckSeverity::Warning,
         moveReady,
         /*fixable*/ false,
         moveReady
             ? "MOVE pathfinding data is initialized and will be included in the save."
             : "MOVE pathfinding data is NOT initialized (generated maps start without it).\n\n"
               "The mission will save successfully, but AI units will not path-find when "
               "the saved map is played in-engine.\n\n"
               "To rebuild MOVE data: load an existing mission, copy/sculpt your terrain "
               "there, and save -- the save rebuilds MOVE from the live terrain.");

    // 4. Objectives have conditions -- WARNING
    //    Same check the editor shows as a MessageBox during save.  Surfaced here so
    //    users can fix it before save rather than being surprised mid-save.
    bool objsOk = !EditorData::instance->TeamsRef().ThereAreObjectivesWithNoConditions();
    push("objectives_conditions",
         "All objectives have conditions",
         MissionCheckSeverity::Warning,
         objsOk,
         /*fixable*/ false,
         objsOk
             ? "All objectives have at least one condition (or there are no objectives)."
             : "One or more objectives have no conditions -- they can never be completed.\n\n"
               "Open Mission > Teams, select the objective, and add at least one condition.  "
               "Or delete objectives that are not yet ready.");

    return checks;
}

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

static bool                     s_open         = false;
static std::vector<MissionCheck> s_lastChecks;
static bool                     s_needsRefresh = true;

void MissionValidator::Open()    { s_open = true;  s_needsRefresh = true; }
void MissionValidator::Close()   { s_open = false; }
bool MissionValidator::IsOpen()  { return s_open; }

// ---------------------------------------------------------------------------
// ImGui panel
// ---------------------------------------------------------------------------

#ifdef MC2_IMGUI

namespace {
    constexpr ImVec4 kColBlock { 0.95f, 0.25f, 0.20f, 1.0f };  // red
    constexpr ImVec4 kColWarn  { 0.95f, 0.75f, 0.10f, 1.0f };  // amber
    constexpr ImVec4 kColInfo  { 0.50f, 0.70f, 1.00f, 1.0f };  // blue
    constexpr ImVec4 kColPass  { 0.30f, 0.85f, 0.30f, 1.0f };  // green
    constexpr ImVec4 kColGrey  { 0.55f, 0.55f, 0.55f, 1.0f };  // greyed
}

void MissionValidator::Draw() {
    if (!s_open) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float    sc = io.DisplaySize.x / 1280.f;

    ImGui::SetNextWindowSize(ImVec2(440.f * sc, 0.f), ImGuiCond_Once);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f - 220.f * sc,
               io.DisplaySize.y * 0.5f - 140.f * sc),
        ImGuiCond_Once);

    bool open = s_open;
    if (!ImGui::Begin("Mission Save Checklist", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        s_open = open;
        return;
    }
    s_open = open;

    // Refresh button
    if (s_needsRefresh) {
        s_lastChecks  = MissionValidator::ValidateForPakSave();
        s_needsRefresh = false;
    }

    if (ImGui::Button("Refresh")) {
        s_lastChecks  = MissionValidator::ValidateForPakSave();
    }
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
            "%d blocking issue%s, %d warning%s  -- fix blocking issues before saving.",
            blockCount, blockCount == 1 ? "" : "s",
            warnCount,  warnCount  == 1 ? "" : "s");
    } else if (warnCount > 0) {
        ImGui::TextColored(kColWarn,
            "No blocking issues.  %d warning%s -- save will work but review warnings.",
            warnCount, warnCount == 1 ? "" : "s");
    } else if (!s_lastChecks.empty()) {
        ImGui::TextColored(kColPass, "All checks passed -- ready to save.");
    } else {
        ImGui::TextColored(kColGrey, "(No checks run yet -- click Refresh)");
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

        // Fix button -- disabled in commit 1 (MissionMinimalBuilder not yet present)
        if (c.autoFixable && !c.passed) {
            char fixId[64];
            snprintf(fixId, sizeof(fixId), "Fix##%s", c.id);
            ImGui::SameLine();
            ImGui::BeginDisabled();
            ImGui::SmallButton(fixId);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Auto-fix available in next update.");
        }
    }

    ImGui::Separator();

    // "Create Minimum Viable Mission" -- disabled in commit 1
    ImGui::BeginDisabled();
    if (ImGui::Button("Create Minimum Viable Mission", ImVec2(-1.f, 0.f)))
        {}  // commit 2: call MissionMinimalBuilder::CreateMinimumViableMission()
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "Coming in next commit: auto-fills the bare minimum required for the saved\n"
            "map to be loadable by the game engine (mission name, teams, player entry).\n"
            "Conservative: does NOT place objects, objectives, or scripted events.");
    }

    ImGui::End();
}

#else // !MC2_IMGUI
void MissionValidator::Draw() {}
#endif // MC2_IMGUI
