// tools/asset_viewer/ShaderContractReport.h
// Diagnostic snapshot of a Backend-A shader build + bound stubs. Displayed in the
// static-prop panel so failures are visible (fail-open contract).
#pragma once
#include <string>
#include <vector>
struct ImVec2;

struct ShaderContractReport {
    std::string shaderRoot;
    std::string vertPath, fragPath;
    bool   compileOk = false;
    bool   linkOk    = false;
    std::string compileLog, linkLog;
    std::string activeDefines;
    std::vector<std::string> includedFiles;
    std::vector<std::string> unresolvedIncludes;
    std::string textureMode = "legacy sampler2D u_tex";
    bool   objectLightsStubActive = false;
    bool   shadowStubActive       = false;
    bool   fogDisabled            = true;
    std::string lastError;         // non-empty => Backend-A fell open to Backend-B

    bool ok() const { return compileOk && linkOk && lastError.empty(); }
};

// Render the report as a collapsible ImGui section.
void DrawShaderContractReport(const ShaderContractReport& r);
