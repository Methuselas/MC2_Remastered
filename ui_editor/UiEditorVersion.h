/***************************************************************
 * FILENAME: UiEditorVersion.h
 * DESCRIPTION: Version constants and title helpers for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Added centralized UI Editor SemVer declarations.
 * - Updated UI Editor SemVer for read-only FIT loading.
 * - Updated UI Editor SemVer for explicit FIT load controls.
 * - Updated UI Editor SemVer for right sidebar layout split.
 * - Updated UI Editor SemVer for cell alias display.
 * - Updated UI Editor SemVer for in-memory transform editing.
 * - Updated UI Editor SemVer for composite preview support.
 * - Updated UI Editor SemVer for v0.3.3 composite mount alignment.
 * - Updated UI Editor SemVer for v0.3.4 image preview loading.
 * - Updated UI Editor SemVer for v0.4.0 Save Copy output.
 * - Updated UI Editor SemVer for v0.4.1 File menu actions.
 * - Updated UI Editor SemVer for v0.4.2 save/reload safety.
 * - Updated UI Editor SemVer for v0.4.3 layout workflow polish.
 * - Updated UI Editor SemVer for v0.4.4 canvas HUD/load hint polish.
 * - Updated UI Editor SemVer for v0.4.5 vertical HUD fix.
 * - Updated UI Editor SemVer through v0.4.20 edit command infrastructure.
 * - Updated UI Editor SemVer for v0.5.0 Smart Tools foundation.
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

#pragma once

#define UI_EDITOR_VERSION_MAJOR 0
#define UI_EDITOR_VERSION_MINOR 5
#define UI_EDITOR_VERSION_PATCH 3

const char* UiEditorVersion_GetSemVer();
const char* UiEditorVersion_GetApplicationName();
const char* UiEditorVersion_GetWindowTitle();
