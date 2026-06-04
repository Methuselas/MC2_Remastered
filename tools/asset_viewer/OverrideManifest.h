#pragma once
#include "WorkbenchWarning.h"
#include <string>
#include <vector>
struct WorkbenchOverrideLod { int lod = 0; std::string sourceRelPath; float distance = 0.0f; };
struct WorkbenchOverride {
    std::string overrideClass = "staticProp";   // EMIT spelling: "staticProp" | "tree"
    std::string appearanceName;
    bool        appearanceVerified = false;
    std::string sourceRelPath;
    float       scale = 1.0f;
    bool        renderOnly = true;
    std::string fallback = "stock";
    std::vector<WorkbenchOverrideLod> lods;
};
