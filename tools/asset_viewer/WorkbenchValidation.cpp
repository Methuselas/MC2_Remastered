#include "WorkbenchValidation.h"
#include <cmath>
std::vector<Warning> ValidateSemantics(const WorkbenchOverride&, const SemanticInputs& in){
    std::vector<Warning> w; auto warn=[&](const char* c,const std::string& m){ w.push_back({WarnSeverity::Warn,c,m}); };
    if (in.stockMesh && in.stockMesh->ok){
        if (in.maxFootprintRatio>1.5f || in.maxFootprintRatio<0.67f)
            warn("bounds-delta","override footprint "+std::to_string(in.maxFootprintRatio)+"x stock — verify scale (runtime forces 1.0)");
        if (in.pivotOffsetXZ>0.25f) warn("pivot-xz","override off-center vs stock by "+std::to_string(in.pivotOffsetXZ));
        if (std::fabs(in.pivotOffsetY)>0.25f) warn("pivot-y","override base vertically offset by "+std::to_string(in.pivotOffsetY));
    } else warn("no-stock","no stock bound — bounds/pivot not validated");
    for (const auto& t: in.missingTextures) warn("texture-missing","referenced texture not found: "+t);
    bool alpha=false;
    if (in.overrideMesh) for (const auto& sm: in.overrideMesh->submeshes)
        if (sm.textureName.rfind("a_",0)==0 || sm.textureName.find("_a_")!=std::string::npos) alpha=true;
    if (alpha && !in.hasImpostorLod) warn("overdraw","alpha-card override with no far-LOD impostor — likely high overdraw in-game (heuristic)");
    return w;
}
