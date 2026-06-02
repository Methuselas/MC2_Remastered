/***************************************************************
 * FILENAME: AssetViewerApp.h
 * DESCRIPTION: Top-level application class for mc2_asset_viewer.
 ***************************************************************/
#pragma once
#include "FileBrowser.h"
#include "AssetTypeSidebar.h"
#include "TextureInspectorPanel.h"
#include "TexturePreview2D.h"

class AssetViewerApp {
public:
    AssetViewerApp();
    ~AssetViewerApp();
    void drawUi();
    static int runSmoke(const char* fixtureDir);
private:
    FileBrowser browser_;
    AssetTypeSidebar sidebar_;
    TextureInspectorPanel inspector_;
    TexturePreview2D surface_;
};
