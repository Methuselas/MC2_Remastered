// tools/asset_viewer/ShaderContractReport.cpp
#include "ShaderContractReport.h"
#include "imgui.h"

void DrawShaderContractReport(const ShaderContractReport& r) {
    if (!ImGui::CollapsingHeader("Shader Contract (Backend-A)")) return;
    ImGui::Text("status: %s", r.ok() ? "OK" : "ERROR (fell open to Backend-B)");
    ImGui::TextWrapped("root: %s", r.shaderRoot.c_str());
    ImGui::TextWrapped("vert: %s  frag: %s", r.vertPath.c_str(), r.fragPath.c_str());
    ImGui::Text("compile: %s   link: %s", r.compileOk ? "ok" : "FAIL", r.linkOk ? "ok" : "FAIL");
    ImGui::TextWrapped("defines: %s", r.activeDefines.c_str());
    ImGui::Text("texture mode: %s", r.textureMode.c_str());
    ImGui::Text("stubs: ObjectLights=%s shadow=%s fog=%s",
        r.objectLightsStubActive ? "on" : "off",
        r.shadowStubActive ? "on" : "off",
        r.fogDisabled ? "disabled" : "on");
    if (!r.unresolvedIncludes.empty()) {
        ImGui::TextColored(ImVec4(1,0.5f,0.5f,1), "unresolved includes:");
        for (auto& u : r.unresolvedIncludes) ImGui::BulletText("%s", u.c_str());
    }
    if (!r.compileLog.empty()) { ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "compile log:"); ImGui::TextWrapped("%s", r.compileLog.c_str()); }
    if (!r.linkLog.empty())    { ImGui::TextColored(ImVec4(1,0.8f,0.3f,1), "link log:");    ImGui::TextWrapped("%s", r.linkLog.c_str()); }
    if (!r.lastError.empty())  ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "error: %s", r.lastError.c_str());
}
