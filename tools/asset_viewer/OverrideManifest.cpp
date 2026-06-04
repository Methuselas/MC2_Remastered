#include "OverrideManifest.h"
#include <cctype>
static std::string lower(std::string s){ for(char& c:s) c=(char)std::tolower((unsigned char)c); return s; }
static bool isSafeSource(const std::string& s){
    if (s.empty()||s[0]=='/'||s[0]=='\\') return false;
    if (s.size()>=2 && s[1]==':') return false;
    if (s.find("..")!=std::string::npos) return false;
    std::string l=lower(s);
    return (l.size()>=4 && l.compare(l.size()-4,4,".glb")==0) || (l.size()>=5 && l.compare(l.size()-5,5,".gltf")==0);
}
std::vector<Warning> ValidateRecordRules(const WorkbenchOverride& r){
    std::vector<Warning> w; auto block=[&](const char* c,const char* m){ w.push_back({WarnSeverity::Block,c,m}); };
    if (!r.renderOnly)                       block("renderOnly","renderOnly must be true (MVP)");
    if (r.fallback!="stock")                 block("fallback","fallback must be \"stock\" (MVP)");
    if (r.scale!=1.0f)                       block("scale","runtime scale must be exactly 1.0 — bake scale into the GLB");
    { std::string c=lower(r.overrideClass); if (c!="staticprop" && c!="tree") block("class","class must be staticProp or tree"); }
    if (r.appearanceName.empty())            block("replaces","no stock appearance bound");
    if (!r.appearanceVerified)               block("appearance-unverified","appearance key not confirmed — verify it matches the engine appearance name");
    if (!isSafeSource(r.sourceRelPath))      block("source","source must be a safe relative .glb/.gltf path");
    int last=0; for (const auto& l: r.lods){
        if (l.lod<=last)                     block("lod-order","LOD indices must strictly ascend (LOD0=source)");
        if (!isSafeSource(l.sourceRelPath))  block("lod-source","LOD source must be a safe relative .glb/.gltf path");
        last=l.lod; }
    return w;
}
