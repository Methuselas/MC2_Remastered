// tools/asset_viewer/CentralManifestMerge.cpp
#include "CentralManifestMerge.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cctype>
#include "model_override_registry.h"   // mclib on include path

using nlohmann::json;
namespace fs = std::filesystem;

static std::string lower(std::string s){ for(char&c:s) c=(char)std::tolower((unsigned char)c); return s; }

MergeResult MergeIntoCentralManifest(const std::string& manifestPath,
                                     const WorkbenchOverride& rec) {
    MergeResult out;
    // 1. BLOCK pre-check (same authority as exportBundle).
    for (const auto& w : ValidateRecordRules(rec)) {
        if (w.severity == WarnSeverity::Block) {
            out.message = std::string("fix BLOCK before merge: ") + w.message;
            return out;   // original file untouched
        }
    }
    const std::string key = lower(rec.overrideClass) + ":" + lower(rec.appearanceName);

    // 2. Parse existing overrides array (preserve unknown fields verbatim).
    json root;
    if (fs::exists(manifestPath)) {
        std::ifstream f(manifestPath);
        try { f >> root; }
        catch (...) { out.message = "central models.json is not valid JSON — refusing to overwrite"; return out; }
    }
    if (!root.is_object() || !root.contains("overrides") || !root["overrides"].is_array())
        root = json{{"overrides", json::array()}};

    // 3. Serialize this record via the existing serializer, take its single object.
    json oneRoot;
    try { oneRoot = json::parse(ToModelsJson(std::vector<WorkbenchOverride>{rec})); }
    catch (...) { out.message = "internal: failed to serialize record"; return out; }
    json newObj = oneRoot["overrides"][0];

    // 4. Splice by key: replace same "replaces", else append.
    bool replaced = false;
    for (auto& e : root["overrides"]) {
        if (e.contains("replaces") && lower(e["replaces"].get<std::string>()) == key) {
            e = newObj; replaced = true; break;
        }
    }
    if (!replaced) root["overrides"].push_back(newObj);

    // 5. .bak (if a file exists), then atomic temp -> rename.
    std::error_code ec;
    if (fs::exists(manifestPath)) fs::copy_file(manifestPath, manifestPath + ".bak",
                                                fs::copy_options::overwrite_existing, ec);
    std::string tmp = manifestPath + ".tmp";
    { std::ofstream o(tmp, std::ios::binary);
      if (!o) { out.message = "cannot open temp for write"; return out; }
      o << root.dump(2) << "\n";
      if (!o.good()) { o.close(); fs::remove(tmp, ec); out.message="write failed"; return out; } }
    fs::rename(tmp, manifestPath, ec);
    if (ec) { fs::remove(tmp, ec); out.message="atomic rename failed"; return out; }

    // 6. Round-trip verify via the engine-faithful registry.
    ModelOverrideRegistry g;
    g.loadFromFile(manifestPath, fs::path(manifestPath).parent_path().string());
    if (g.resolve(rec.overrideClass.c_str(), rec.appearanceName.c_str()) == nullptr) {
        if (fs::exists(manifestPath + ".bak"))
            fs::copy_file(manifestPath + ".bak", manifestPath,
                          fs::copy_options::overwrite_existing, ec);
        out.message = "round-trip verify failed — rolled back to .bak";
        return out;
    }

    out.ok = true;
    out.replacedExisting = replaced;
    out.recordCount = g.count();
    out.message = replaced ? "replaced existing record" : "appended new record";
    return out;
}
