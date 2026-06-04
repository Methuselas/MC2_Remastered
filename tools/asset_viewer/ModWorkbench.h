// tools/asset_viewer/ModWorkbench.h
#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"
#include "WorkbenchWarning.h"
#include <cstdint>
#include <string>
class ModWorkbench {
public:
    bool loadOverride(const std::string& glbPath);   // bumps generation on success
    bool hasOverride() const { return overrideMesh_.ok; }
    const MeshData& overrideMesh() const { return overrideMesh_; }
    const std::string& overridePath() const { return overridePath_; }
    const std::string& lastError() const { return lastError_; }
    uint64_t generation() const { return generation_; }
private:
    std::string overridePath_;
    MeshData    overrideMesh_;
    std::string lastError_;
    uint64_t    generation_ = 0;
};
