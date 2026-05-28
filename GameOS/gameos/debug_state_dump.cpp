#include "debug_state_dump.h"

#include "gos_static_prop_batcher.h"
#include "render_snapshot.h"
#include "view_uniforms_gl.h"
#include "../../RenderCore/RendererFeatureRegistry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

extern char missionName[1024];

namespace {

constexpr uint64_t kDumpIntervalFrames = 300u;

bool envFlagOn(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

bool featureActive(const char* envVar, bool defaultOn) {
    const char* v = std::getenv(envVar);
    if (!v || !v[0]) return defaultOn;
    return v[0] != '0';
}

std::string jsonEscape(const char* s) {
    std::string out;
    if (!s) return out;
    for (const unsigned char c : std::string(s)) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0x0F];
                    out += hex[c & 0x0F];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::filesystem::path outputPath() {
    if (const char* dir = std::getenv("MC2_DEBUG_STATE_DUMP_DIR")) {
        if (dir[0]) return std::filesystem::path(dir) / "latest_render_state.json";
    }
    return std::filesystem::path("debug_state") / "latest_render_state.json";
}

const char* viewKindForId(RenderCore::ViewId id) {
    return id == RenderCore::kMainSceneViewId ? "MainScene" : "unknown";
}

const char* buildConfigString() {
#if defined(_DEBUG)
    return "Debug";
#elif defined(NDEBUG)
    return "Release";
#else
    return "RelWithDebInfo";
#endif
}

void writeBool(std::ofstream& out, bool v) {
    out << (v ? "true" : "false");
}

} // namespace

namespace mc2_debug_state {

void maybeWriteRenderState(const RenderSnapshot& snap) {
    static const bool s_enabled = envFlagOn("MC2_DEBUG_STATE_DUMP");
    if (!s_enabled) return;
    if (snap.frameIndex != 1u && (snap.frameIndex % kDumpIntervalFrames) != 0u)
        return;

    const std::filesystem::path path = outputPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return;

    StaticPropOpaqueDebugState sp{};
    batcher_getStaticPropOpaqueDebugState(&sp);

    const RenderCore::EngineView& view = RenderCore::getCurrentView();
    const bool viewKnown = view.id != RenderCore::kInvalidViewId;
    const bool missionKnown = missionName[0] != '\0';

    out << "{\n";
    out << "  \"schema\": \"MC2_DEBUG_STATE_V1\",\n";
    out << "  \"frame\": " << static_cast<unsigned long long>(snap.frameIndex) << ",\n";
    out << "  \"mission\": {\n";
    out << "    \"name\": \"" << jsonEscape(missionKnown ? missionName : "") << "\",\n";
    out << "    \"known\": "; writeBool(out, missionKnown); out << "\n";
    out << "  },\n";
    out << "  \"build\": {\n";
    out << "    \"commit\": \"unknown\",\n";
    out << "    \"config\": \"" << buildConfigString() << "\"\n";
    out << "  },\n";
    out << "  \"features\": {\n";
    out << "    \"MC2_DEBUG_STATE_DUMP\": true,\n";
    out << "    \"MC2_VIEW_UNIFORMS\": "; writeBool(out, featureActive("MC2_VIEW_UNIFORMS", true)); out << ",\n";
    out << "    \"MC2_SNAPSHOT_STATIC_PROP_BUILD\": "; writeBool(out, featureActive("MC2_SNAPSHOT_STATIC_PROP_BUILD", true)); out << ",\n";
    out << "    \"MC2_MATERIAL_GPU\": "; writeBool(out, featureActive("MC2_MATERIAL_GPU", true)); out << ",\n";
    out << "    \"MC2_MATERIAL_GPU_SAMPLE\": "; writeBool(out, featureActive("MC2_MATERIAL_GPU_SAMPLE", true)); out << ",\n";
    out << "    \"MC2_STATIC_PROP_IBL_SH\": "; writeBool(out, featureActive("MC2_STATIC_PROP_IBL_SH", true)); out << ",\n";
    out << "    \"MC2_STATIC_PROP_PBR_V1\": "; writeBool(out, featureActive("MC2_STATIC_PROP_PBR_V1", false)); out << "\n";
    out << "  },\n";
    out << "  \"engineView\": {\n";
    out << "    \"known\": "; writeBool(out, viewKnown); out << ",\n";
    out << "    \"viewId\": " << view.id << ",\n";
    out << "    \"viewKind\": \"" << viewKindForId(view.id) << "\",\n";
    out << "    \"viewMode\": \"" << RenderCore::toString(view.mode) << "\",\n";
    out << "    \"viewUniformsBinding\": " << RenderCore::kViewUniformsBinding << ",\n";
    out << "    \"viewport\": [" << view.viewport[0] << ", " << view.viewport[1] << ", "
        << view.viewport[2] << ", " << view.viewport[3] << "]\n";
    out << "  },\n";
    out << "  \"renderSnapshot\": {\n";
    out << "    \"ok\": "; writeBool(out, snap.ok != 0u); out << ",\n";
    out << "    \"staticPropValidationFail\": " << snap.staticPropValidationFail << ",\n";
    out << "    \"staticPropPacketRangesFail\": " << snap.staticPropPacketRangesFail << ",\n";
    out << "    \"staticPropPacketInvalid\": " << snap.staticPropPacketInvalid << ",\n";
    out << "    \"arenaOverflow\": "; writeBool(out, snap.arenaOverflow); out << ",\n";
    out << "    \"spBuildAttempted\": " << snap.spBuildAttempted << ",\n";
    out << "    \"spBuildFallback\": " << snap.spBuildFallback << ",\n";
    out << "    \"spBuildCountMismatch\": " << snap.spBuildCountMismatch << ",\n";
    out << "    \"spBuildPacketMismatch\": " << snap.spBuildPacketMismatch << ",\n";
    out << "    \"spBuildMetaMismatch\": " << snap.spBuildMetaMismatch << "\n";
    out << "  },\n";
    out << "  \"staticPropOpaque\": {\n";
    out << "    \"snapshotDispatchDefault\": "; writeBool(out, sp.snapshotDispatchDefault); out << ",\n";
    out << "    \"legacyDispatch\": "; writeBool(out, sp.legacyDispatch); out << ",\n";
    out << "    \"materialGpuEnabled\": "; writeBool(out, sp.materialGpuEnabled); out << ",\n";
    out << "    \"materialGpuSample\": "; writeBool(out, sp.materialGpuSample); out << ",\n";
    out << "    \"iblShEnabled\": "; writeBool(out, sp.iblShEnabled); out << ",\n";
    out << "    \"iblShStrength\": " << sp.iblShStrength << ",\n";
    out << "    \"iblShSet\": \"" << jsonEscape(sp.iblShSet) << "\",\n";
    out << "    \"pbrEnabled\": "; writeBool(out, sp.pbrEnabled); out << ",\n";
    out << "    \"pbrStrength\": " << sp.pbrStrength << ",\n";
    out << "    \"pbrRoughnessOverrideEnabled\": "; writeBool(out, sp.pbrRoughnessOverrideEnabled); out << ",\n";
    out << "    \"pbrRoughnessOverride\": " << sp.pbrRoughnessOverride << ",\n";
    out << "    \"debugMaterialMode\": " << sp.debugMaterialMode << "\n";
    out << "  }\n";
    out << "}\n";
}

} // namespace mc2_debug_state
