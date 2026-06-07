#pragma once
/***************************************************************
* FILENAME: MissionValidation.h
* DESCRIPTION: Read-only mission save-readiness checks and ImGui checklist panel.
*   Inspects the live editor state (terrain, map name, MOVE flag, objectives) and
*   surfaces blocking issues and warnings without mutating any data.
*   Commit 2 adds MissionMinimalBuilder which acts on autoFixable checks.
* DATE: 2026-06-07
****************************************************************/

#ifndef MISSION_VALIDATION_H
#define MISSION_VALIDATION_H

#include <vector>
#include <string>

// Severity drives icon colour and summary counters.
enum class MissionCheckSeverity {
    Blocking,   // save may crash or produce corrupt output if this fails
    Warning,    // save completes but saved mission may not be game-playable
    Info,       // informational only; no action required
};

struct MissionCheck {
    const char*          id;           // stable string key (e.g. "terrain_loaded")
    const char*          label;        // one-line display string shown in panel
    std::string          details;      // tooltip / multi-line explanation
    MissionCheckSeverity severity;
    bool                 passed;
    bool                 autoFixable;  // MissionMinimalBuilder::FixCheck(id) handles it
};

class MissionValidator {
public:
    // Run all checks for File > Save As (.pak).
    // Returns every check (passed and failed); caller inspects .passed + .severity.
    static std::vector<MissionCheck> ValidateForPakSave();

    // ImGui floating panel — open/close/draw each frame from renderToolbarImGui().
    static void Open();
    static void Close();
    static bool IsOpen();
    static void Draw();
};

#endif // MISSION_VALIDATION_H
