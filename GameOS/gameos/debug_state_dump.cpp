#include "debug_state_dump.h"

#include "gos_postprocess.h"
#include "gos_static_prop_batcher.h"
#include "render_snapshot.h"
#include "view_uniforms_gl.h"
#include "../../RenderCore/RendererFeatureRegistry.h"
#include "../../RenderCore/RenderResourceRegistry.h"

// Texture name lookup for mech node indices (mcTextureManager slot → name string).
// Defined in gos_mech_batcher.cpp; not declared in any header.
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx);

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

extern char missionName[1024];

namespace {

constexpr uint64_t kDumpIntervalFrames = 300u;
constexpr uint32_t kHistorySlots       = 8u;

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

std::filesystem::path outputDir() {
    if (const char* dir = std::getenv("MC2_DEBUG_STATE_DUMP_DIR")) {
        if (dir[0]) return std::filesystem::path(dir);
    }
    return std::filesystem::path("debug_state");
}

const char* viewKindForId(RenderCore::ViewId id) {
    if (const auto* v = RenderCore::resolveView(id))
        return RenderCore::toString(v->kind);
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

struct RenderPassState {
    bool shadow        = false;
    bool screenShadow  = false;
    bool bloom         = false;
    bool fxaa          = false;
    bool tonemap       = false;
};

RenderPassState readPassState() {
    RenderPassState ps{};
    if (const gosPostProcess* pp = getGosPostProcess()) {
        ps.shadow       = pp->shadowsEnabled_;
        ps.screenShadow = pp->screenShadowEnabled_;
        ps.bloom        = pp->bloomEnabled_;
        ps.fxaa         = pp->fxaaEnabled_;
        ps.tonemap      = pp->tonemapEnabled_;
    }
    return ps;
}

// Derive human-readable shader variant string from runtime flags.
std::string spShaderVariant(const StaticPropOpaqueDebugState& sp) {
    std::string v = sp.snapshotDispatchDefault ? "snapshot" : "legacy";
    if (sp.materialGpuEnabled) {
        v += "+materialGpu";
        if (sp.materialGpuSample) v += "+sample";
    }
    if (sp.iblShEnabled) v += "+iblSh";
    if (sp.pbrEnabled)   v += "+pbr";
    if (sp.debugMaterialMode != 0) {
        v += "+debug(";
        v += std::to_string(sp.debugMaterialMode);
        v += ")";
    }
    return v;
}

static void b(std::ostringstream& s, bool v) { s << (v ? "true" : "false"); }

std::string buildSnapshotJson(const RenderSnapshot& snap,
                              const StaticPropOpaqueDebugState& sp,
                              const RenderCore::EngineView& view,
                              const RenderPassState& ps) {
    const bool viewKnown    = view.id != RenderCore::kInvalidViewId;
    const bool missionKnown = missionName[0] != '\0';

    std::ostringstream s;
    s << "{\n";
    s << "  \"schema\": \"MC2_DEBUG_STATE_V1\",\n";
    s << "  \"frame\": " << static_cast<unsigned long long>(snap.frameIndex) << ",\n";
    s << "  \"mission\": {\n";
    s << "    \"name\": \"" << jsonEscape(missionKnown ? missionName : "") << "\",\n";
    s << "    \"known\": "; b(s, missionKnown); s << "\n";
    s << "  },\n";
    s << "  \"build\": {\n";
    s << "    \"commit\": \"unknown\",\n";
    s << "    \"config\": \"" << buildConfigString() << "\"\n";
    s << "  },\n";
    s << "  \"features\": {\n";
    s << "    \"MC2_DEBUG_STATE_DUMP\": true,\n";
    s << "    \"MC2_VIEW_UNIFORMS\": "; b(s, featureActive("MC2_VIEW_UNIFORMS", true)); s << ",\n";
    s << "    \"MC2_SNAPSHOT_STATIC_PROP_BUILD\": "; b(s, featureActive("MC2_SNAPSHOT_STATIC_PROP_BUILD", true)); s << ",\n";
    s << "    \"MC2_MATERIAL_GPU\": "; b(s, featureActive("MC2_MATERIAL_GPU", true)); s << ",\n";
    s << "    \"MC2_MATERIAL_GPU_SAMPLE\": "; b(s, featureActive("MC2_MATERIAL_GPU_SAMPLE", true)); s << ",\n";
    s << "    \"MC2_STATIC_PROP_IBL_SH\": "; b(s, featureActive("MC2_STATIC_PROP_IBL_SH", true)); s << ",\n";
    s << "    \"MC2_STATIC_PROP_PBR_V1\": "; b(s, featureActive("MC2_STATIC_PROP_PBR_V1", false)); s << ",\n";
    s << "    \"MC2_QUADSETUP_ARMED_SKIP\": "; b(s, featureActive("MC2_QUADSETUP_ARMED_SKIP", true)); s << "\n";
    s << "  },\n";
    s << "  \"engineView\": {\n";
    s << "    \"known\": "; b(s, viewKnown); s << ",\n";
    s << "    \"viewId\": " << view.id << ",\n";
    s << "    \"viewKind\": \"" << viewKindForId(view.id) << "\",\n";
    s << "    \"viewMode\": \"" << RenderCore::toString(view.mode) << "\",\n";
    s << "    \"viewUniformsBinding\": " << RenderCore::kViewUniformsBinding << ",\n";
    s << "    \"viewport\": [" << view.viewport[0] << ", " << view.viewport[1] << ", "
      << view.viewport[2] << ", " << view.viewport[3] << "]\n";
    s << "  },\n";
    {
        const uint32_t viewCount = RenderCore::getViewCount();
        s << "  \"registeredViews\": [\n";
        for (uint32_t i = 0; i < viewCount; ++i) {
            const RenderCore::EngineView* v = RenderCore::getViewByIndex(i);
            if (!v) continue;
            s << "    {\n";
            s << "      \"viewId\": " << v->id << ",\n";
            s << "      \"viewKind\": \"" << RenderCore::toString(v->kind) << "\",\n";
            s << "      \"viewMode\": \"" << RenderCore::toString(v->mode) << "\",\n";
            s << "      \"debugName\": \"" << jsonEscape(v->debugName ? v->debugName : "") << "\",\n";
            s << "      \"viewport\": [" << v->viewport[0] << ", " << v->viewport[1] << ", "
              << v->viewport[2] << ", " << v->viewport[3] << "]\n";
            s << "    }" << (i + 1 < viewCount ? "," : "") << "\n";
        }
        s << "  ],\n";
    }
    s << "  \"renderSnapshot\": {\n";
    s << "    \"ok\": "; b(s, snap.ok != 0u); s << ",\n";
    s << "    \"staticPropValidationFail\": " << snap.staticPropValidationFail << ",\n";
    s << "    \"staticPropPacketRangesFail\": " << snap.staticPropPacketRangesFail << ",\n";
    s << "    \"staticPropPacketInvalid\": " << snap.staticPropPacketInvalid << ",\n";
    s << "    \"arenaOverflow\": "; b(s, snap.arenaOverflow); s << ",\n";
    s << "    \"spBuildAttempted\": " << snap.spBuildAttempted << ",\n";
    s << "    \"spBuildFallback\": " << snap.spBuildFallback << ",\n";
    s << "    \"spBuildCountMismatch\": " << snap.spBuildCountMismatch << ",\n";
    s << "    \"spBuildPacketMismatch\": " << snap.spBuildPacketMismatch << ",\n";
    s << "    \"spBuildMetaMismatch\": " << snap.spBuildMetaMismatch << ",\n";
    s << "    \"spBuildRetired\": " << snap.spBuildRetired << "\n";
    s << "  },\n";
    s << "  \"renderPasses\": {\n";
    s << "    \"shadow\": "; b(s, ps.shadow); s << ",\n";
    s << "    \"screenShadow\": "; b(s, ps.screenShadow); s << ",\n";
    s << "    \"bloom\": "; b(s, ps.bloom); s << ",\n";
    s << "    \"fxaa\": "; b(s, ps.fxaa); s << ",\n";
    s << "    \"tonemap\": "; b(s, ps.tonemap); s << "\n";
    s << "  },\n";
    s << "  \"registeredViews\": [\n";
    {
        const uint32_t vc = RenderCore::getViewCount();
        for (uint32_t i = 0; i < vc; ++i) {
            const auto* v = RenderCore::getViewByIndex(i);
            if (!v) continue;
            s << "    {";
            s << " \"id\": " << v->id << ",";
            s << " \"name\": \"" << jsonEscape(v->debugName) << "\",";
            s << " \"kind\": \"" << RenderCore::toString(v->kind) << "\",";
            s << " \"valid\": " << (v->id != RenderCore::kInvalidViewId ? "true" : "false") << ",";
            s << " \"viewport\": [" << v->viewport[0] << "," << v->viewport[1]
              << "," << v->viewport[2] << "," << v->viewport[3] << "]";
            s << " }";
            if (i + 1 < vc) s << ",";
            s << "\n";
        }
    }
    s << "  ],\n";
    s << "  \"staticPropOpaque\": {\n";
    s << "    \"snapshotDispatchDefault\": "; b(s, sp.snapshotDispatchDefault); s << ",\n";
    s << "    \"legacyDispatch\": "; b(s, sp.legacyDispatch); s << ",\n";
    s << "    \"shaderVariant\": \"" << spShaderVariant(sp) << "\",\n";
    s << "    \"spV6DrawCalls\": " << sp.spV6DrawCalls << ",\n";
    s << "    \"spAlphaOffPackets\": " << sp.spAlphaOffPackets << ",\n";
    s << "    \"materialGpuEnabled\": "; b(s, sp.materialGpuEnabled); s << ",\n";
    s << "    \"materialGpuSample\": "; b(s, sp.materialGpuSample); s << ",\n";
    s << "    \"materialGpuTableSize\": " << sp.materialGpuTableSize << ",\n";
    s << "    \"materialInventorySize\": " << sp.materialInventorySize << ",\n";
    s << "    \"iblShEnabled\": "; b(s, sp.iblShEnabled); s << ",\n";
    s << "    \"iblShStrength\": " << sp.iblShStrength << ",\n";
    s << "    \"iblShSet\": \"" << jsonEscape(sp.iblShSet) << "\",\n";
    s << "    \"pbrEnabled\": "; b(s, sp.pbrEnabled); s << ",\n";
    s << "    \"pbrStrength\": " << sp.pbrStrength << ",\n";
    s << "    \"pbrRoughnessOverrideEnabled\": "; b(s, sp.pbrRoughnessOverrideEnabled); s << ",\n";
    s << "    \"pbrRoughnessOverride\": " << sp.pbrRoughnessOverride << ",\n";
    s << "    \"debugMaterialMode\": " << sp.debugMaterialMode << "\n";
    s << "  },\n";
    // MECH-MATERIAL-INVENTORY-1: mech snapshot section.
    // Gate: MC2_SNAPSHOT_MECH_EXTRACT=1 (default OFF). Reads the already-extracted
    // mech rows/counters directly from the snap parameter — no side effects, does
    // not force the extract on.
    {
        constexpr uint32_t kMechPacketCap = 32u;
        const bool extractEnabled = featureActive("MC2_SNAPSHOT_MECH_EXTRACT", false);
        // Determine whether data is present: mirrors EditorInspector.cpp gate check.
        const bool gateOn = extractEnabled
            && (snap.mechSnapshotCount > 0u
                || snap.mechMatValid   > 0u
                || snap.mechMatSentinel > 0u);
        s << "  \"mech\": {\n";
        s << "    \"extractEnabled\": "; b(s, extractEnabled); s << ",\n";
        if (!gateOn) {
            // No data this frame — emit zero counters and empty packet array.
            s << "    \"rows\": 0,\n";
            s << "    \"mat_valid\": 0,\n";
            s << "    \"mat_sentinel\": 0,\n";
            s << "    \"countMismatch\": 0,\n";
            s << "    \"handleMismatch\": 0,\n";
            s << "    \"objectIdMismatch\": 0,\n";
            s << "    \"texHandleMismatch\": 0,\n";
            s << "    \"materialIdxMismatch\": 0,\n";
            s << "    \"truncated\": false,\n";
            s << "    \"packets\": []\n";
        } else {
            s << "    \"rows\": "              << snap.mechSnapshotCount       << ",\n";
            s << "    \"mat_valid\": "         << snap.mechMatValid             << ",\n";
            s << "    \"mat_sentinel\": "      << snap.mechMatSentinel          << ",\n";
            s << "    \"countMismatch\": "     << snap.mechCountMismatch        << ",\n";
            s << "    \"handleMismatch\": "    << snap.mechHandleMismatch       << ",\n";
            s << "    \"objectIdMismatch\": "  << snap.mechObjectIdMismatch     << ",\n";
            s << "    \"texHandleMismatch\": " << snap.mechTexHandleMismatch    << ",\n";
            s << "    \"materialIdxMismatch\": " << snap.mechMaterialIdxMismatch << ",\n";
            const uint32_t total     = snap.mechPackets.size();
            const uint32_t emitCount = total < kMechPacketCap ? total : kMechPacketCap;
            const bool     truncated = (total > kMechPacketCap);
            s << "    \"truncated\": "; b(s, truncated); s << ",\n";
            s << "    \"packets\": [\n";
            for (uint32_t i = 0u; i < emitCount; ++i) {
                const ExtractedMechPacket& row = snap.mechPackets[i];
                const bool sentinel = (row.materialIdx == 0xFFFFFFFFu);
                s << "      {\n";
                s << "        \"objectIdRaw\": "         << row.objectIdRaw  << ",\n";
                s << "        \"instanceIdx\": "         << row.instanceIdx  << ",\n";
                s << "        \"texHandle\": "           << row.texHandle    << ",\n";
                {
                    const char* texName = gos_getMechTextureNameByNodeIdx(row.texHandle);
                    s << "        \"textureName\": \""
                      << jsonEscape((texName && *texName) ? texName : "") << "\",\n";
                }
                s << "        \"materialIdx\": "         << row.materialIdx  << ",\n";
                s << "        \"materialIdxSentinel\": "; b(s, sentinel); s << ",\n";
                s << "        \"typeLodIdx\": "          << row.typeLodIdx   << ",\n";
                s << "        \"renderFlags\": "         << row.renderFlags  << ",\n";
                // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: per-mech visual state.
                s << "        \"heat01\": "              << row.heat01       << ",\n";
                s << "        \"damage01\": "            << row.damage01     << ",\n";
                s << "        \"visualFlags\": "         << row.visualFlags  << "\n";
                s << "      }" << (i + 1u < emitCount ? "," : "") << "\n";
            }
            s << "    ]\n";
        }
        s << "  },\n";
    }
    {
        const size_t count = RenderCore::getRenderResourceCount();
        s << "  \"renderResources\": [\n";
        for (size_t i = 0; i < count; ++i) {
            const RenderCore::RenderResourceDesc* r = RenderCore::getRenderResourceByIndex(i);
            if (!r) continue;
            s << "    {\n";
            s << "      \"id\": \""       << RenderCore::toString(r->id)     << "\",\n";
            s << "      \"kind\": \""     << RenderCore::toString(r->kind)   << "\",\n";
            s << "      \"format\": \""   << RenderCore::toString(r->format) << "\",\n";
            s << "      \"debugName\": \"" << jsonEscape(r->debugName ? r->debugName : "") << "\",\n";
            s << "      \"width\": "   << r->width   << ",\n";
            s << "      \"height\": "  << r->height  << ",\n";
            s << "      \"layers\": "  << r->layers  << ",\n";
            s << "      \"samples\": " << r->samples << ",\n";
            s << "      \"glName\": "  << r->glName  << ",\n";
            s << "      \"sizeBytes\": " << static_cast<unsigned long long>(r->sizeBytes) << ",\n";
            s << "      \"valid\": true\n";
            s << "    }" << (i + 1 < count ? "," : "") << "\n";
        }
        s << "  ]\n";
    }
    s << "}\n";
    return s.str();
}

bool writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << content;
    return true;
}

} // namespace

namespace mc2_debug_state {

void maybeWriteRenderState(const RenderSnapshot& snap) {
    static const bool s_enabled = envFlagOn("MC2_DEBUG_STATE_DUMP");
    if (!s_enabled) return;
    if (snap.frameIndex != 1u && (snap.frameIndex % kDumpIntervalFrames) != 0u)
        return;

    const std::filesystem::path dir = outputDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;

    StaticPropOpaqueDebugState sp{};
    batcher_getStaticPropOpaqueDebugState(&sp);
    const RenderCore::EngineView& view = RenderCore::getCurrentView();
    const RenderPassState ps = readPassState();

    const std::string json = buildSnapshotJson(snap, sp, view, ps);

    writeFile(dir / "latest_render_state.json", json);

    static const bool s_historyEnabled = envFlagOn("MC2_DEBUG_STATE_DUMP_HISTORY");
    if (s_historyEnabled) {
        static uint32_t s_historySlot = 0u;
        char name[32];
        snprintf(name, sizeof(name), "history_%u.json", s_historySlot);
        writeFile(dir / name, json);
        s_historySlot = (s_historySlot + 1u) % kHistorySlots;
    }
}

} // namespace mc2_debug_state
