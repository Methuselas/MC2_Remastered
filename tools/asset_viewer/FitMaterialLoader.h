// tools/asset_viewer/FitMaterialLoader.h
#pragma once
#include <string>

// Minimal, read-only parser for ONE Material{} block inside a .fit file.
// NOT a general FIT parser; does not commit any format decision.
struct FitMaterial {
    std::string baseColor;
    std::string normal;
    std::string orm;
    std::string emissive;
    std::string shader;       // informational
    std::string ormPacking;   // e.g. "RAO_GRough_BMetal"
    std::string alphaMode;    // informational
    bool found = false;
};

// Parses the first Material{} block. Paths are returned as-written (caller
// resolves relative to the .fit's directory). Returns found=false if none.
FitMaterial FitMaterialLoader_Parse(const std::string& fitPath, std::string* errorOut);
