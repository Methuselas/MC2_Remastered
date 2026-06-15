#ifndef TELEMETRY_PANEL_H
#define TELEMETRY_PANEL_H
/***************************************************************
* FILENAME: TelemetryPanel.h
* DESCRIPTION: Editor authoring-telemetry panel (S17 edit depth).
*   A read-only ImGui panel surfacing live editor state for modders:
*   frame rate, selection / placed-object counts, undo-redo depth,
*   unsaved (dirty) state, and map / mission info. No mutation, no
*   save/load -- pure observation, safe with no map / no selection.
* DATE: 2026-06-15
****************************************************************/

class TelemetryPanel
{
public:
    // Panel visibility (own ImGui window; no-op cheaply when closed).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // Read-only: reads editor singletons/statics, mutates nothing.
    static void Draw();
};

#endif // TELEMETRY_PANEL_H
