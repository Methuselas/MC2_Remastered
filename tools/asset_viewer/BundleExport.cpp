#include "BundleExport.h"
#include "model_override_registry.h"
#include <filesystem>
#include <fstream>
namespace fs=std::filesystem;
ExportResult ExportBundle(const std::string& outRoot, const std::string& bundleId,
                          const std::string& srcGlbPath, WorkbenchOverride rec){
    ExportResult res;
    if (bundleId.empty()||bundleId.find("..")!=std::string::npos||
        bundleId.find('/')!=std::string::npos||bundleId.find('\\')!=std::string::npos){ res.message="invalid bundle id"; return res; }
    if (!fs::exists(srcGlbPath)){ res.message="source GLB not found: "+srcGlbPath; return res; }
    std::string glbName=fs::path(srcGlbPath).filename().string();
    rec.sourceRelPath=bundleId+"/"+glbName;
    for (const auto& w: ValidateRecordRules(rec))
        if (w.severity==WarnSeverity::Block){ res.message="blocked: "+w.message; return res; }
    std::error_code ec;
    std::string dir=outRoot+"/"+bundleId;
    fs::create_directories(dir,ec); if(ec){ res.message="mkdir failed: "+ec.message(); return res; }
    fs::copy_file(srcGlbPath, dir+"/"+glbName, fs::copy_options::overwrite_existing, ec);
    if(ec){ res.message="copy failed: "+ec.message(); return res; }
    std::string manifest=dir+"/models.generated.json";
    { std::vector<WorkbenchOverride> v{rec}; std::ofstream out(manifest, std::ios::binary);
      if(!out){ res.message="write manifest failed (open)"; return res; }
      out<<ToModelsJson(v); out.close();
      if(!out){ res.message="write manifest failed (incomplete write)"; return res; } }
    ModelOverrideRegistry check; check.loadFromFile(manifest, dir);
    if (check.resolve(rec.overrideClass.c_str(), rec.appearanceName.c_str())==nullptr){
        res.message="EXPORT BLOCKED BY REGISTRY (round-trip failed)"; return res; }
    res.ok=true; res.bundleDir=dir; res.manifestPath=manifest; res.message="exported draft "+rec.sourceRelPath;
    return res;
}
