#pragma once
/***************************************************************
* FILENAME: MapGeneratorDialog.h
* DESCRIPTION: ImGui Map Generator dialog for the MC2 Mission Editor.
*   Replaces the MFC TerrainDlg + MapSizeDlg flow with a single integrated
*   window that has biome/size/height/material controls and a fast thumbnail
*   preview before full generation.
* DATE: 2026-06-05
****************************************************************/

#ifndef MAP_GENERATOR_DIALOG_H
#define MAP_GENERATOR_DIALOG_H

// Forward-declare OpenGL type to avoid GL header pollution in this header.
typedef unsigned int GLuint;

namespace MapGeneratorDialog {

    // Pending deferred action requested by the dialog (set each frame; acted
    // on in EditorInterface::update() OUTSIDE the ImGui render pass).
    enum class PendingAction {
        None,
        Preview,      // run python --preview, load thumbnail
        Generate,     // run python full, apply to editor, close dialog
        LoadPreset,   // copy pre-baked preset files then apply (no python run)
    };

    // Open (or re-open) the dialog.  Call from toolbar "Generate Map" button
    // and from the startup path when ID_MAPGENERATOR is returned.
    void Open();

    // Close + clean up preview texture.
    void Close();

    bool IsOpen();

    // Draw the ImGui window.  Call each frame from renderToolbarImGui()
    // (or equivalent editor render hook).  Sets the pending action if the
    // user clicks Preview or Generate.
    void Draw();

    // Query the pending action and clear it atomically.
    PendingAction TakeAction();

    // Execute the pending action (called from update(), outside ImGui pass).
    void ExecutePreview();
    void ExecuteGenerate();
    // Load a pre-baked flat preset (no Python run).
    // Checks terrain_gen_presets/<biome>_<sizeN>/ for pre-generated files.
    void ExecuteLoadPreset();

} // namespace MapGeneratorDialog

#endif // MAP_GENERATOR_DIALOG_H
