/***************************************************************
 * FILENAME: UiEditorImageCache.h
 * DESCRIPTION: Lightweight image texture cache for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Added optional SDL_image-backed texture loading for FIT image preview cells.
 ***************************************************************/

#pragma once

#include "imgui.h"

struct UiEditorImageTexture
{
	bool loaded;
	bool unavailable;
	int width;
	int height;
	const char* resolvedPath;
	ImTextureID textureId;

	UiEditorImageTexture();
};

bool UiEditorImageCache_Initialize();
void UiEditorImageCache_Shutdown();
void UiEditorImageCache_Clear();
const UiEditorImageTexture* UiEditorImageCache_Get(const char* path);
const char* UiEditorImageCache_GetStatus();
