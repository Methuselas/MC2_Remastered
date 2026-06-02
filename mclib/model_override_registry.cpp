// model_override_registry.cpp — MODEL-OVERRIDE-MVP-1 Slice 1.
// ONLY engine TU permitted to include nlohmann/json (scripts/check-json-isolation.sh).
#include "model_override_registry.h"
#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <utility>

using nlohmann::json;

static void logDrop(const char* why, const std::string& key) {
    fprintf(stderr, "[MODOVERRIDE] dropped '%s': %s\n", key.c_str(), why);
    fflush(stderr);
}

static std::string normalizeKey(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    std::string out = s.substr(b, e - b);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool isSafeSource(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    if (s.size() >= 2 && s[1] == ':') return false;
    // conservative: rejects any "..", including the rare literal "a..b.glb"; traversal safety > that edge case.
    if (s.find("..") != std::string::npos) return false;
    std::string low = s; for (char& c : low) c = (char)std::tolower((unsigned char)c);
    const bool glb  = low.size() >= 4 && low.compare(low.size() - 4, 4, ".glb")  == 0;
    const bool gltf = low.size() >= 5 && low.compare(low.size() - 5, 5, ".gltf") == 0;
    return glb || gltf;
}

int ModelOverrideRegistry::loadFromFile(const std::string& manifestPath,
                                        const std::string& manifestDir) {
    records_.clear();
    manifestDir_ = manifestDir;

    std::ifstream in(manifestPath.c_str());
    if (!in.is_open()) return 0;
    json root;
    try { in >> root; }
    catch (const std::exception& e) {
        fprintf(stderr, "[MODOVERRIDE] parse error in %s: %s\n", manifestPath.c_str(), e.what());
        return 0;
    }
    if (!root.is_object() || !root.contains("overrides") || !root["overrides"].is_array()) {
        fprintf(stderr, "[MODOVERRIDE] %s: missing 'overrides' array\n", manifestPath.c_str());
        return 0;
    }

    for (const auto& e : root["overrides"]) {
        try {
        if (!e.is_object()) { logDrop("entry is not an object", "<non-object>"); continue; }
        std::string replaces = e.value("replaces", std::string());
        const std::string key = replaces.empty() ? "<no-replaces>" : replaces;

        if (e.value("type", std::string()) != "model") { logDrop("type!=model", key); continue; }
        if (!e.value("renderOnly", false))             { logDrop("renderOnly!=true", key); continue; }
        if (e.value("fallback", std::string()) != "stock") { logDrop("fallback!=stock", key); continue; }

        const float scale = e.value("scale", 1.0f);
        if (scale != 1.0f) { logDrop("scale!=1.0 (MVP requires 1.0)", key); continue; }

        // split on FIRST ':'; MC2 appearance names contain no ':' so a trailing ':' in name is not expected.
        size_t colon = replaces.find(':');
        if (colon == std::string::npos) { logDrop("replaces not '<class>:<name>'", key); continue; }
        std::string cls  = normalizeKey(replaces.substr(0, colon));
        std::string name = normalizeKey(replaces.substr(colon + 1));
        if (cls.empty() || name.empty()) { logDrop("empty class or name in replaces", key); continue; }

        if (cls != "staticprop" && cls != "tree") { logDrop("class not staticProp|tree", key); continue; }
        if (e.contains("class")) {
            std::string clsField = normalizeKey(e.value("class", std::string()));
            if (clsField != cls) { logDrop("class field disagrees with replaces", key); continue; }
        }

        // Trim leading/trailing ASCII whitespace before safety check (case-preserving).
        std::string source = e.value("source", std::string());
        {
            size_t b = 0, en = source.size();
            while (b < en && std::isspace((unsigned char)source[b])) ++b;
            while (en > b && std::isspace((unsigned char)source[en - 1])) --en;
            source = source.substr(b, en - b);
        }
        if (!isSafeSource(source)) { logDrop("unsafe/non-glTF source path", key); continue; }

        bool dup = false;
        for (const auto& r : records_) {
            if (r.overrideClass == cls && r.appearanceName == name) { dup = true; break; }
        }
        if (dup) { logDrop("duplicate key (first entry wins)", key); continue; }

        ModelOverrideRecord rec;
        rec.overrideClass  = cls;
        rec.appearanceName = name;
        rec.sourceRelPath  = source;
        rec.scale          = scale;
        records_.push_back(std::move(rec));
        } catch (const std::exception& ex) {
            logDrop(ex.what(), "<entry>"); continue;
        }
    }
    return (int)records_.size();
}

const ModelOverrideRecord* ModelOverrideRegistry::resolve(
        const char* overrideClass, const char* appearanceName) const {
    if (!overrideClass || !appearanceName) return nullptr;
    const std::string cls  = normalizeKey(overrideClass);
    const std::string name = normalizeKey(appearanceName);
    for (const auto& r : records_) {
        if (r.overrideClass == cls && r.appearanceName == name) return &r;
    }
    return nullptr;
}

ModelOverrideRegistry& ModelOverrideRegistry::instance() {
    static ModelOverrideRegistry g;
    static std::once_flag once;
    std::call_once(once, []{
        g.loadFromFile("data/model_overrides/models.json", "data/model_overrides");
        fprintf(stderr, "[MODOVERRIDE] registry loaded: %d override(s)\n", g.count());
        fflush(stderr);
    });
    return g;
}
