#ifndef INSPECTOR_PANEL_H
#define INSPECTOR_PANEL_H
/***************************************************************
* FILENAME: InspectorPanel.h
* DESCRIPTION: Read-only modder-friendly Inspector Lite (Phase 1b).
*   Shows details of the current selection (the first selected object from
*   the existing EditorObjectMgr selection list -- so it reflects BOTH
*   viewport selection and Scene Outliner selection). Read-only: no mutation,
*   no dirty flag, no undo. Does NOT touch save/load or PacketFile.
* DATE: 2026-06-09
****************************************************************/

class EditorObject;

class InspectorPanel
{
public:
    // Panel visibility (own ImGui window; safe with no map / no selection).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed. Pure read-only.
    static void Draw();

    // Compact, no-space category token for an object (smoke + display):
    // "Unit" / "Building" / "NavMarker" / "DropZone" / "Tree" / "Object".
    static const char* CategoryToken(EditorObject* obj);

    // Smoke/manual helper: summarise the current selection without drawing.
    // Returns true if something is selected; writes the category token of the
    // first selected object into outType (always NUL-terminated). On no
    // selection returns false and writes "none".
    static bool GetSelectionSummary(char* outType, unsigned int typeLen);
};

#endif // INSPECTOR_PANEL_H
