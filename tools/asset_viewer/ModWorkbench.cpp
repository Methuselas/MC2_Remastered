#include "ModWorkbench.h"
#include "GlbMeshLoader.h"
#include <cmath>

bool ModWorkbench::loadOverride(const std::string& glbPath){
    overridePath_ = glbPath;
    overrideMesh_ = GlbMeshLoader::load(glbPath);
    lastError_ = overrideMesh_.ok ? std::string() : overrideMesh_.error;
    if (overrideMesh_.ok) ++generation_;
    return overrideMesh_.ok;
}

bool ModWorkbench::bindStock(const std::string& tglName){
    if (!TglMeshLoader::ensureFastFile(deployDir_.c_str())){
        lastError_ = "tgl.fst not found";
        return false;
    }
    stockTgl_ = tglName;
    stockMesh_ = TglMeshLoader::loadMesh(tglName);
    if (!stockMesh_.ok){ lastError_ = stockMesh_.error; return false; }
    ++generation_;
    return true;
}

ModWorkbench::BoundsDelta ModWorkbench::computeDelta() const {
    BoundsDelta d;
    auto ext = [](const MeshData& m, float o[3]){
        for (int k = 0; k < 3; ++k) o[k] = m.ok ? (m.bmax[k] - m.bmin[k]) : 0.0f;
    };
    ext(overrideMesh_, d.overrideExt);
    ext(stockMesh_,    d.stockExt);
    d.maxRatio = 0.0f;
    for (int k = 0; k < 3; ++k){
        if (d.stockExt[k] > 1e-4f){
            float r = d.overrideExt[k] / d.stockExt[k];
            if (r > d.maxRatio) d.maxRatio = r;
        }
    }
    if (d.maxRatio == 0.0f) d.maxRatio = 1.0f;
    auto base = [](const MeshData& m, float o[3]){
        o[0] = m.ok ? 0.5f * (m.bmin[0] + m.bmax[0]) : 0.0f;
        o[1] = m.ok ? m.bmin[1]                       : 0.0f;
        o[2] = m.ok ? 0.5f * (m.bmin[2] + m.bmax[2]) : 0.0f;
    };
    float ob[3], sb[3];
    base(overrideMesh_, ob);
    base(stockMesh_,    sb);
    for (int k = 0; k < 3; ++k) d.pivotOffset[k] = ob[k] - sb[k];
    return d;
}
