// tools/asset_viewer/ShaderIncludeResolver.h
// Minimal GLSL #include inliner rooted at a shaders/ directory. Resolves both
// #include <path> and #include "path" relative to the root. Cycle-guarded.
// Used by Backend-A to compile the real engine shaders without copying includes.
#pragma once
#include <string>
#include <vector>

struct ShaderResolveResult {
    bool ok = false;
    std::string source;
    std::vector<std::string> includedFiles;
    std::vector<std::string> unresolved;
    std::string error;
};

ShaderResolveResult ResolveShaderIncludes(const std::string& shaderRoot,
                                          const std::string& entryFile);
