/***************************************************************
 * FILENAME: AssetViewerApp.h
 * DESCRIPTION: Top-level application class for mc2_asset_viewer.
 ***************************************************************/
#pragma once
#include "FileBrowser.h"
#include "AssetTypeSidebar.h"
#include "TextureInspectorPanel.h"
#include "TexturePreview2D.h"
#include "MaterialPreviewPBR.h"
#include "MaterialSlots.h"
#include "MeshPreview3D.h"
#include "ModelBrowser.h"
#include "ModWorkbench.h"

class AssetViewerApp {
public:
    AssetViewerApp();
    ~AssetViewerApp();
    void drawUi();
    void onFileDropped(const char* path);
    static int runSmoke(const char* fixtureDir);
    static int runSmokeDecoder();
    static int runSmokeKtxParse(const char* fixtureDir);
    static int runSmokeKtx(const char* fixtureDir);
    static int runSmokePreview(const char* fixtureDir);
    static int runSmokeFit();
    static int runSmokeTiers(const char* fixtureDir);
    static int runSmokeSphere();                        // validates SphereMesh geometry + tangent basis
    static int runSmokeBackend();                       // compiles Cook-Torrance PBR program on a GL 3.3 context
    static int runSmokeTexLoad(const char* fixtureDir); // slot-aware sRGB/linear upload check
    static int runSmokeRender(const char* fixtureDir);  // offscreen FBO render: sphere distinct from background
    static int runSmokeTangent(const char* fixtureDir); // tangent correctness: flat==no-normal, tilt perturbs, no seam blow-up
    static int runSmokeFitMaterial(const char* fixtureDir); // minimal FIT parser: Material{} block -> slot paths
    static int runSmokeFitLoad(const char* fixtureDir);    // loadFit() multi-base resolution + GL upload check
    static int runSmokeTglLoad(const char* deployDir);     // headless NS3 TGL loader: FastFileInit + LoadBinaryCopy
    static int runSmokeMeshBuild(const char* deployDir);    // Task 1: TglMeshLoader CPU mesh extraction (no GL)
    static int runSmokeMeshRender(const char* deployDir);   // Task 2: MeshPreview3D GL render: model distinct from background
    static int runSmokeMeshOrient(const char* deployDir);   // orientation gate: 2civliving tall axis must be GL-Y after transform
    static int runSmokeSpotlight(const char* deployDir);    // spotlight gate: vehicle has ≥1 isSpotlight submesh; 2civliving has 0
    static int runSmokeWorkbenchLink();   // S0: registry + assimp link/run
    static int runSmokeWorkbenchGlb(const char* fixtureDir);   // S1
private:
    FileBrowser browser_;
    AssetTypeSidebar sidebar_;
    TextureInspectorPanel inspector_;
    TexturePreview2D surface_;
    MaterialPreviewPBR materialSurface_;
    MaterialSlots      materialSlots_;
    MeshPreview3D      meshSurface_;
    ModelBrowser       modelBrowser_;
    ModWorkbench       workbench_;
    bool materialsAutoLoaded_ = false;
};
