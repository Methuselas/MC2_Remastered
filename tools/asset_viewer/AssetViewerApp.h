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
    static int runSmokeDecoder();
    static int runSmokeKtxParse(const char* fixtureDir);
    static int runSmokeKtx(const char* fixtureDir);
    static int runSmokePreview(const char* fixtureDir);
    static int runSmokeFit();
    static int runSmokeTiers(const char* fixtureDir);
private:
    FileBrowser browser_;
    AssetTypeSidebar sidebar_;
    TextureInspectorPanel inspector_;
    TexturePreview2D surface_;
};
