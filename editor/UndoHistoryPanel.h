#ifndef UNDO_HISTORY_PANEL_H
#define UNDO_HISTORY_PANEL_H
/***************************************************************
* FILENAME: UndoHistoryPanel.h
* DESCRIPTION: Display-only Undo History panel (v1).
*   Shows the full undo stack with the current cursor position
*   highlighted; rows past the cursor are the redo branch and
*   are rendered dimmed. No mutation -- purely for inspection.
*
*   Access path to the undo manager:
*     The panel calls ActionUndoMgr::instance (static singleton
*     set in ActionUndoMgr::ActionUndoMgr()). This is safe whenever
*     the editor is running and avoids any dependency on the
*     EditorInterface instance.
*
*   ORCHESTRATOR NOTE:
*     No accessor is needed on EditorInterface because
*     ActionUndoMgr::instance is already publicly available.
*     If that ever changes, add to EditorInterface:
*
*       // in EditorInterface.h (public section):
*       static const ActionUndoMgr* undoManager();
*
*       // in EditorInterface.cpp:
*       /*static*\/ const ActionUndoMgr* EditorInterface::undoManager()
*       { return ActionUndoMgr::instance; }
*
*     Then replace the ActionUndoMgr::instance reference in
*     UndoHistoryPanel.cpp with EditorInterface::undoManager().
*
* DATE: 2026-06-10
****************************************************************/

class UndoHistoryPanel
{
public:
    // Panel visibility (own ImGui window; safe to call with no map loaded).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed. Purely read-only -- no undo/redo mutation.
    static void Draw();
};

#endif // UNDO_HISTORY_PANEL_H
