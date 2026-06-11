// tools/asset_viewer/ModWorkbenchPanel.h
// S2: side-by-side stock/override preview + bounds/pivot delta.
#pragma once
#include "ModWorkbench.h"
#include "MeshPreview3D.h"
#include "AppearanceRoster.h"
#include <cstdint>
#include <string>

struct ImVec2;

class ModWorkbenchPanel {
public:
    void setDeployDir(const std::string& d);
    void draw(ModWorkbench& wb, const ImVec2& avail);

private:
    void syncMeshes(ModWorkbench& wb);

    MeshPreview3D stockPreview_;
    MeshPreview3D overridePreview_;
    uint64_t      lastSyncedGen_ = (uint64_t)-1;
    char          appe_[128] = {0};   // appearance-key edit buffer; reprimed on generation change
    AppearanceRoster roster_;
    char             apFilter_[128] = {0};   // appearance combo filter
    std::string      deployDir_ = ".";       // remembered for roster load/refresh
};
