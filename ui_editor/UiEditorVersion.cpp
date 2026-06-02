/***************************************************************
 * FILENAME: UiEditorVersion.cpp
 * DESCRIPTION: Version string helpers for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Added UI Editor SemVer and window-title helper functions.
 * - Updated UI Editor SemVer for v0.2.1 FIT loading.
 * - Updated UI Editor SemVer for v0.2.2 right sidebar split.
 * - Updated UI Editor SemVer for v0.2.3 cell alias display.
 * - Updated UI Editor SemVer for v0.3.0 in-memory transform editing.
 * - Updated UI Editor SemVer for v0.3.1 local rect display.
 * - Updated UI Editor SemVer for v0.3.2 composite preview.
 * - Updated UI Editor SemVer for v0.3.3 composite mount alignment.
 * - Updated UI Editor SemVer for v0.3.4 image preview loading.
 * - Updated UI Editor SemVer for v0.3.5 stb_image preview loading.
 * - Updated UI Editor SemVer for v0.3.6 legacy atlas UV preview.
 * - Updated UI Editor SemVer for v0.3.7 canvas label clutter controls.
 * - Updated UI Editor SemVer for v0.4.0 Save Copy output.
 * - Updated UI Editor SemVer for v0.4.1 File menu actions.
 * - Updated UI Editor SemVer for v0.4.2 save/reload safety.
 * - Updated UI Editor SemVer for v0.4.3 layout workflow polish.
 * - Updated UI Editor SemVer for v0.4.4 canvas HUD/load hint polish.
 * - Updated UI Editor SemVer for v0.4.5 vertical HUD fix.
 * - Updated UI Editor SemVer for v0.4.6 page switch dirty-modal fix.
 * - Updated UI Editor SemVer for v0.4.7 in-memory alias rename.
 * - Updated UI Editor SemVer for v0.4.9 canvas viewport scrollbars.
 * - Updated UI Editor SemVer for v0.4.10 hierarchy visibility/lock toggles.
 * - Updated UI Editor SemVer for v0.4.11 canvas controls and edit-history prep.
 * - Updated UI Editor SemVer for v0.4.12 resolution-independent layout foundation.
 * - Updated UI Editor SemVer for v0.4.13 UI FIT browser expansion.
 * - Updated UI Editor SemVer for v0.4.14 side-panel resizing and image fallback.
 * - Updated UI Editor SemVer for v0.4.15 Editor FIT compatibility.
 * - Updated UI Editor SemVer for v0.4.16 Photoshop canvas foundation.
 * - Updated UI Editor SemVer for v0.4.17 Photoshop-style canvas viewport and tabs.
 * - Updated UI Editor SemVer for v0.4.17b scrollbar interaction hotfix.
 * - Updated UI Editor SemVer for v0.4.17c canvas toolbar overflow hotfix.
 * - Updated UI Editor SemVer for v0.4.17d guide editing hotfix.
 * - Updated UI Editor SemVer for v0.4.18 Concept Shell pass.
 * - Updated UI Editor SemVer for v0.4.19 page creation and navigation model.
 * - Updated UI Editor SemVer for v0.4.19a compile hotfix.
 * - Updated UI Editor SemVer for v0.4.20 edit command infrastructure.
 * - Updated UI Editor SemVer for v0.5.0 Smart Tools foundation.
 * - Updated UI Editor SemVer for v0.5.0a popup/zoom polish hotfix.
 * - Updated UI Editor SemVer for v0.5.0b true viewport modal centering.
 * - Updated UI Editor SemVer for v0.5.0c ImGui compatibility hotfix.
 * - Updated UI Editor SemVer for v0.5.0e canvas scroll regression revert.
 * - Updated UI Editor SemVer for v0.5.1 Color/Text production pass.
 * - Updated UI Editor SemVer for v0.5.1c numeric double-click input polish.
 * - Updated UI Editor SemVer for v0.5.1d frame canvas shortcut.
 * - Updated UI Editor SemVer for v0.5.2-pre-a image diagnostics.
 * - Updated UI Editor SemVer for v0.5.3f legacy UI truth viewer coverage.
 * - Updated UI Editor SemVer for v0.5.3g animation timeline placement truth.
 * - Updated UI Editor SemVer for v0.5.3j legacy batch regeneration and shared scrollbar component preview.
 * - Updated UI Editor SemVer for v0.5.3j legacy text, scrollbar, and 3D viewport truth viewer.
 * - Updated UI Editor SemVer for v0.5.3j mission operation runtime text truth.
 * - Updated UI Editor SemVer for v0.5.3k text authoring and shared Mechlopedia redirect.
 * - Updated UI Editor SemVer for v0.5.3l font catalog and flashing text truth.
 * - Updated UI Editor SemVer for v0.5.3o legacy button/font state truth.
 * - Updated UI Editor SemVer for v0.5.3o text anchor/font type/button overlay truth.
 * - Updated UI Editor SemVer for v0.5.3o component/page truth fixes.
 * - Updated UI Editor SemVer for v0.5.3p loading/high-res HUD/text rect truth.
 * - Updated UI Editor SemVer for v0.5.3t campaign/mechlab/mechlopedia composition fixes.
 * - Updated UI Editor SemVer for v0.5.3t runtime UI truth audit cleanup.
 * - Updated UI Editor SemVer for v0.5.3t editing QOL and preview cleanup.
 * - Updated UI Editor SemVer for v0.5.3t selection, group-move, and text-rect editing cleanup.
 * - Updated UI Editor SemVer for v0.5.3u package/defines/modern rect foundation.
 * - Updated UI Editor SemVer for v0.5.3w viewer/text-rect/browser cleanup.
 * - Updated UI Editor SemVer for v0.5.3v package-local UI folder consolidation.
 * - Updated UI Editor SemVer for v0.5.3x viewer UV/text-rect/browser hotfix.
 * - Updated UI Editor SemVer for v0.5.3y Viewer button-state adapter cleanup.
 * - Updated UI Editor SemVer for v0.5.3z 1K cursor atlas support.
 ***************************************************************/

#include "UiEditorVersion.h"

const char* UiEditorVersion_GetSemVer()
{
    return "0.5.3z";
}

const char* UiEditorVersion_GetApplicationName()
{
    return "MC2R UI Editor";
}

const char* UiEditorVersion_GetWindowTitle()
{
    return "MC2R UI Editor 0.5.3z";
}
