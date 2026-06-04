#include "ModWorkbench.h"
#include "GlbMeshLoader.h"
bool ModWorkbench::loadOverride(const std::string& glbPath){
    overridePath_ = glbPath;
    overrideMesh_ = GlbMeshLoader::load(glbPath);
    lastError_ = overrideMesh_.ok ? std::string() : overrideMesh_.error;
    if (overrideMesh_.ok) ++generation_;
    return overrideMesh_.ok;
}
