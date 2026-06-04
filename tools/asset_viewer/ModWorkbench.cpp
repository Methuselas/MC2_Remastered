#include "ModWorkbench.h"
#include "GlbMeshLoader.h"
#include <cmath>
#include <cctype>
#include <string>

// Extract basename without extension (lowercase) from a path.
static std::string basenameNoExt(const std::string& path){
    // Find last slash
    size_t sep = path.find_last_of("/\\");
    std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
    // Strip extension
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    // Lowercase
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return name;
}

// Extract filename only (no directory) from a path.
static std::string filenameOnly(const std::string& path){
    size_t sep = path.find_last_of("/\\");
    return (sep == std::string::npos) ? path : path.substr(sep + 1);
}

bool ModWorkbench::loadOverride(const std::string& glbPath){
    overridePath_ = glbPath;
    overrideMesh_ = GlbMeshLoader::load(glbPath);
    lastError_ = overrideMesh_.ok ? std::string() : overrideMesh_.error;
    if (overrideMesh_.ok){
        ++generation_;
        // Set sourceRelPath to just the filename (S4 rewrites to <id>/<file>)
        record_.sourceRelPath = filenameOnly(glbPath);
    }
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
    // Suggest appearanceName from tgl basename (strip dir + extension, lowercase)
    record_.overrideClass = "staticProp";
    if (record_.appearanceName.empty()){
        record_.appearanceName = basenameNoExt(tglName);
        record_.appearanceVerified = false;
    }
    return true;
}

void ModWorkbench::revalidate(const std::vector<std::string>& missing){
    warnings_.clear();
    auto b=ValidateRecordRules(record_); warnings_.insert(warnings_.end(),b.begin(),b.end());
    SemanticInputs si;
    si.overrideMesh = overrideMesh_.ok?&overrideMesh_:nullptr;
    si.stockMesh    = stockMesh_.ok?&stockMesh_:nullptr;
    auto d=computeDelta(); si.maxFootprintRatio=d.maxRatio;
    si.pivotOffsetXZ=std::sqrt(d.pivotOffset[0]*d.pivotOffset[0]+d.pivotOffset[2]*d.pivotOffset[2]);
    si.pivotOffsetY=d.pivotOffset[1]; si.missingTextures=missing; si.hasImpostorLod=!record_.lods.empty();
    auto s=ValidateSemantics(record_,si); warnings_.insert(warnings_.end(),s.begin(),s.end());
}

bool ModWorkbench::hasBlocking() const {
    for(const auto& w:warnings_) if(w.severity==WarnSeverity::Block) return true;
    return false;
}

ExportResult ModWorkbench::exportBundle(const std::string& outRoot, const std::string& bundleId){
    revalidate();
    if (hasBlocking()){ ExportResult r; r.message="fix BLOCK warnings before export"; return r; }
    return ExportBundle(outRoot, bundleId, overridePath_, record_);
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
