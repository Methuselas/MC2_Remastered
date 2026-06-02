// model_override_registry.h — MODEL-OVERRIDE-MVP-1 Slice 1.
// Engine-independent parse of model_overrides/models.json. No TG_*/GameOS deps.
// Render-only model overrides keyed by "<class>:<appearanceName>". MVP rules:
// renderOnly must be true, fallback must be "stock". Invalid entries are logged
// and dropped, never fatal. See docs/model-override-mvp-plan.md.
#pragma once
#include <string>
#include <vector>

struct ModelOverrideRecord {
    std::string overrideClass;   // "staticprop" | "tree" — NORMALIZED lowercase
    std::string appearanceName;  // from "replaces" after ':' — NORMALIZED lowercase
    std::string sourceRelPath;   // .glb/.gltf relative to manifest dir (validated safe)
    float       scale = 1.0f;    // MVP requires exactly 1.0
    // MVP invariants (validated at load; entry dropped + logged if violated):
    //   type=="model", renderOnly==true, fallback=="stock", scale==1.0,
    //   safe relative .glb/.gltf source, class in {staticProp,tree},
    //   class field (if present) agrees with replaces. Duplicate key: first wins.
};

class ModelOverrideRegistry {
public:
    int loadFromFile(const std::string& manifestPath,
                     const std::string& manifestDir);
    const ModelOverrideRecord* resolve(const char* overrideClass,
                                       const char* appearanceName) const;
    int count() const { return (int)records_.size(); }
    static ModelOverrideRegistry& instance();
private:
    std::vector<ModelOverrideRecord> records_;
    std::string manifestDir_;
};
