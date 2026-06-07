#pragma once
/***************************************************************
* FILENAME: MissionValidation.h
* DESCRIPTION: Mission save-readiness checks and ImGui checklist control surface.
*   Commit 1: read-only validator + panel.
*   Commit 2: per-check action buttons, HasBlockingFailures/HasWarnings helpers,
*             TakeAction() deferred dispatch, PrepareSaveableMission.
* DATE: 2026-06-07
****************************************************************/

#ifndef MISSION_VALIDATION_H
#define MISSION_VALIDATION_H

#include <vector>
#include <string>

// Severity drives icon colour and summary counters.
enum class MissionCheckSeverity {
    Blocking,   // save may crash or produce corrupt output
    Warning,    // save works but mission may not be game-playable
    Info,       // informational only
};

// Action the panel button next to a failing check should trigger.
// None = no button (check is purely informational or not fixable from here).
enum class ChecklistAction {
    None,
    OpenMapGenerator,   // open the Map Generator dialog
    OpenSaveAs,         // open File > Save As dialog
    OpenObjectives,     // open Mission > Teams (team 1) objective editor
    PrepareSaveable,    // "Prepare Saveable Mission" bottom button:
                        //   re-validate, trigger first failing check's action
};

struct MissionCheck {
    const char*          id;       // stable key (e.g. "terrain_loaded")
    const char*          label;    // one-line display string
    std::string          details;  // tooltip / multi-line explanation
    MissionCheckSeverity severity;
    bool                 passed;
    ChecklistAction      action;   // None = no button shown
};

class MissionValidator {
public:
    // ---------------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------------

    // Run all checks for File > Save As (.pak).
    // Returns every check (passed and failed).
    static std::vector<MissionCheck> ValidateForPakSave();

    // Quick helpers used by the Save() intercept.
    static bool HasBlockingFailures();   // re-validates inline
    static bool HasWarnings();           // re-validates inline

    // ---------------------------------------------------------------------------
    // ImGui panel
    // ---------------------------------------------------------------------------

    static void Open();
    static void Close();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // Sets a pending action when the user clicks a check button or the
    // "Prepare Saveable Mission" bottom button.
    static void Draw();

    // ---------------------------------------------------------------------------
    // Deferred action dispatch (processed in EditorInterface::update())
    // ---------------------------------------------------------------------------

    // Return and clear the pending action (atomically).  Returns None when idle.
    static ChecklistAction TakeAction();

    // Queue an action from outside the panel (e.g. PrepareSaveable handler needs
    // to queue the first failing action for the next update() tick).
    static void QueueAction(ChecklistAction act);
};

#endif // MISSION_VALIDATION_H
