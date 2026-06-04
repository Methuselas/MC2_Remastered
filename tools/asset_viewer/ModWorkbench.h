// tools/asset_viewer/ModWorkbench.h
#pragma once
#include "TglMeshLoader.h"
#include "OverrideManifest.h"
#include "BundleExport.h"
#include "WorkbenchWarning.h"
#include "WorkbenchValidation.h"
#include <cstdint>
#include <string>
#include <vector>
class ModWorkbench {
public:
    bool loadOverride(const std::string& glbPath);   // bumps generation on success
    bool hasOverride() const { return overrideMesh_.ok; }
    const MeshData& overrideMesh() const { return overrideMesh_; }
    const std::string& overridePath() const { return overridePath_; }
    const std::string& lastError() const { return lastError_; }
    uint64_t generation() const { return generation_; }

    void setDeployDir(const std::string& d) { deployDir_ = d; }
    bool bindStock(const std::string& tglName);   // bumps generation on success
    bool hasStock() const { return stockMesh_.ok; }
    const MeshData& stockMesh() const { return stockMesh_; }

    struct BoundsDelta {
        float overrideExt[3]{};
        float stockExt[3]{};
        float maxRatio = 1.0f;
        float pivotOffset[3]{};
    };
    BoundsDelta computeDelta() const;

    WorkbenchOverride& record(){ return record_; }
    const std::vector<Warning>& warnings() const { return warnings_; }
    bool hasBlocking() const;
    void revalidate(const std::vector<std::string>& missingTextures = {});
    ExportResult exportBundle(const std::string& outRoot, const std::string& bundleId);

private:
    std::string overridePath_;
    MeshData    overrideMesh_;
    std::string lastError_;
    uint64_t    generation_ = 0;

    std::string deployDir_ = ".";
    std::string stockTgl_;
    MeshData    stockMesh_;

    WorkbenchOverride        record_;
    std::vector<Warning>     warnings_;
};
