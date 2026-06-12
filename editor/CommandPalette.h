#ifndef COMMAND_PALETTE_H
#define COMMAND_PALETTE_H
/***************************************************************
* FILENAME: CommandPalette.h
* DESCRIPTION: Searchable Command Palette (UE/VSCode style) ImGui panel.
*   Opens with Ctrl+P (wired by orchestrator). Filters editor commands
*   by label and category; click or Enter dispatches through
*   EditorInterface::handleNewMenuMessage() preserving undo.
* DATE: 2026-06-10
****************************************************************/

class CommandPalette
{
public:
    // Panel visibility. Safe to call with no map loaded.
    static void Open();
    static void Close();
    static void Toggle();
    static bool IsOpen();

    // Draw the floating panel each frame from renderToolbarImGui().
    // No-op cheaply when closed.
    static void Draw();
};

#endif // COMMAND_PALETTE_H
