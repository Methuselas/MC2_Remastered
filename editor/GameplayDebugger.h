#ifndef GAMEPLAY_DEBUGGER_H
#define GAMEPLAY_DEBUGGER_H
/***************************************************************
* FILENAME: GameplayDebugger.h
* DESCRIPTION: Read-only "Gameplay Debugger" ImGui panel.
*   Shows brain/path/combat state for the selected unit.
*
*   RUNTIME AVAILABILITY NOTE: The editor process NEVER has live
*   Mover/MechWarrior objects. ObjectManager is always nullptr in
*   the editor (see EditorGlobals.cpp:34). This panel therefore
*   shows static editor-side data (type, team, variant, pilot,
*   position) from EditorObjectMgr and displays a clear notice
*   that runtime simulation state is unavailable.
*
* DATE: 2026-06-10
****************************************************************/

class GameplayDebugger
{
public:
    // Panel visibility (own ImGui window; safe to call with no map loaded).
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed. Pure read-only; no object mutation.
    static void Draw();

    // Smoke hook (-smoke-gameplay-debugger): exercises the same null-guarded
    // selection gather as Draw() but without an ImGui frame, so it runs even
    // when the editor renders minimized. Writes the selected object's type name
    // into outType (always NUL-terminated) and returns true if something was
    // selected. Read-only; safe on empty map / no selection (returns false,
    // outType = "none").
    static bool SmokeProbe(char* outType, unsigned int cap);
};

#endif // GAMEPLAY_DEBUGGER_H
