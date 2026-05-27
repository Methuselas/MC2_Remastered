#include "EditorInspector.h"
#include "imgui.h"
#include <cstdio>    // snprintf
#include <cstdlib>   // getenv
#include "../GameOS/gameos/debug_renderer.h"  // IMG-INSPECT-3
#include "draw_packet_emitter.h"              // g_dpSelectedRecipeIndex
#include "../RenderCore/RendererFeatureRegistry.h"

// MECH-SPINE-1: read-only accessors for mech pass-level state. Defined in
// gos_mech_batcher.cpp; declared here so the inspector can reference them
// without including engine-private mech batcher headers.
extern "C" uint32_t gos_getMechProgramId();
extern "C" uint32_t gos_getMechShadowProgramId();
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx);

namespace {

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_IMGUI_INSPECTOR");
        cached = (!v || v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

static const char* pickKindName(EditorInspector::InspectorPickKind k) {
    switch (k) {
        case EditorInspector::InspectorPickKind::StaticProp: return "StaticProp";
        case EditorInspector::InspectorPickKind::Mech:       return "Mech";
        case EditorInspector::InspectorPickKind::Terrain:    return "Terrain";
        default:                                              return "None";
    }
}

static EditorInspector::InspectorSelection     s_selection;
static EditorInspector::StaticPropInspectorData s_staticPropData;
static EditorInspector::MechInspectorData       s_mechData;
static EditorInspector::TerrainInspectorData    s_terrainData;
static EditorInspector::TerrainPassSnapshot     s_terrainPass;   // TERRAIN-SPINE-0
static EditorInspector::ShadowPassSnapshot      s_shadowPass;    // SHADOW-SPINE-0
static EditorInspector::VfxPassSnapshot         s_vfxPass;       // VFX-SPINE-0
static bool s_open = false;
static bool s_featuresOpen = false;

}  // namespace

void EditorInspector::onCtrlShiftClick(int mouseX, int mouseY) {
    if (!isEnabled()) return;
    // Coords are recorded; missiongui.cpp calls setPickResult after
    // running tryGameplayPick to populate the full selection.
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    g_dpSelectedRecipeIndex = -1;  // cleared until setStaticPropData confirms
    s_open = true;
}

void EditorInspector::setPickResult(int mouseX, int mouseY,
                                    const RenderWorld::LookupResult& lookup) {
    if (!isEnabled()) return;
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    s_selection.valid   = lookup.isValid;
    s_selection.lookup  = lookup;
    // Clear bridge until setStaticPropData fires with the confirmed recipeIndex.
    // Non-StaticProp picks never call setStaticPropData, so they stay at -1.
    g_dpSelectedRecipeIndex = -1;
    if (lookup.isValid) {
        s_selection.kind   = lookup.kind;
        s_selection.handle = lookup.handle;
        s_selection.hasSelection = true;
        if (lookup.kind == RenderWorld::RenderObjectKind::StaticProp)
            s_selection.pickKind = InspectorPickKind::StaticProp;
        else if (lookup.kind == RenderWorld::RenderObjectKind::Mech)
            s_selection.pickKind = InspectorPickKind::Mech;
        else
            s_selection.pickKind = InspectorPickKind::None;
    }
    s_open = true;
}

void EditorInspector::setStaticPropData(const StaticPropInspectorData& sd) {
    s_staticPropData = sd;
    // Publish recipeIndex so gameosmain can fill g_dpSelProp each frame.
    g_dpSelectedRecipeIndex = sd.populated ? sd.recipeIndex : -1;
}

void EditorInspector::setMechData(const MechInspectorData& md) {
    s_mechData = md;
}

void EditorInspector::setTerrainData(const TerrainInspectorData& td) {
    if (!isEnabled()) return;
    s_terrainData = td;
    s_selection.hasSelection     = td.populated;
    s_selection.pickKind         = InspectorPickKind::Terrain;
    s_selection.lookup.worldX      = td.worldX;
    s_selection.lookup.worldY      = td.worldY;
    s_selection.lookup.worldZ      = td.worldZ;
    s_selection.lookup.worldPosValid = td.populated;
    // NB: s_selection.valid stays false (no RenderWorld lookup).
}

void EditorInspector::setTerrainPassSnapshot(const TerrainPassSnapshot& ts) {
    // TERRAIN-SPINE-0: pass-level snapshot. Always accept (no enable gate so the
    // snapshot is current even when the inspector window is closed; drawImGui is
    // already gated by isEnabled()).
    s_terrainPass = ts;
}

void EditorInspector::setShadowPassSnapshot(const ShadowPassSnapshot& sp) {
    // SHADOW-SPINE-0: same always-accept policy as the terrain snapshot.
    s_shadowPass = sp;
}

void EditorInspector::setVfxPassSnapshot(const VfxPassSnapshot& vs) {
    // VFX-SPINE-0: same always-accept policy as the terrain / shadow snapshots.
    s_vfxPass = vs;
}

void EditorInspector::clear() {
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    g_dpSelectedRecipeIndex = -1;
    s_open = false;
}

void EditorInspector::drawImGui() {
    if (!isEnabled()) return;

    // Ctrl+Shift+I toggles the panel regardless of current open state.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I))
        s_open = !s_open;

    // Renderer Features — standalone window. Runs independent of Object Inspector.
    // Ctrl+Shift+F to toggle. Placed BEFORE if (!s_open) return.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F))
        s_featuresOpen = !s_featuresOpen;

    if (s_featuresOpen) {
        ImGui::SetNextWindowSize(ImVec2(600, 440), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Renderer Features", &s_featuresOpen)) {
            ImGui::TextUnformatted("MC2_* feature gates  --  Ctrl+Shift+F to close");
            ImGui::TextUnformatted("Hover 'Feature' column for doc string.");
            ImGui::Separator();

            if (ImGui::BeginTable("##feats", 4,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Feature",
                    ImGuiTableColumnFlags_WidthFixed, 210.f);
                ImGui::TableSetupColumn("Env var",
                    ImGuiTableColumnFlags_WidthFixed, 210.f);
                ImGui::TableSetupColumn("Default",
                    ImGuiTableColumnFlags_WidthFixed,  52.f);
                ImGui::TableSetupColumn("Status",
                    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (int i = 0; i < static_cast<int>(RenderCore::RendererFeature::COUNT); ++i) {
                    const RenderCore::EnvVarDesc& d = RenderCore::kFeatureTable[i];
                    ImGui::TableNextRow();

                    // Feature column — hover for doc string.
                    ImGui::TableSetColumnIndex(0);
                    if (d.kind == RenderCore::EnvVarKind::Retired)
                        ImGui::TextDisabled("%s", d.featureId);
                    else
                        ImGui::TextUnformatted(d.featureId);
                    if (ImGui::IsItemHovered() && d.doc)
                        ImGui::SetTooltip("%s", d.doc);

                    // Env var column.
                    ImGui::TableSetColumnIndex(1);
                    if (d.envVar)
                        ImGui::TextDisabled("%s", d.envVar);
                    else
                        ImGui::TextDisabled("(none)");

                    // Default column.
                    ImGui::TableSetColumnIndex(2);
                    if (d.kind == RenderCore::EnvVarKind::Retired)
                        ImGui::TextDisabled("--");
                    else if (!d.envVar)
                        ImGui::TextDisabled("ON");
                    else
                        ImGui::TextDisabled(d.defaultOn ? "ON" : "off");

                    // Status column.
                    ImGui::TableSetColumnIndex(3);
                    if (d.kind == RenderCore::EnvVarKind::Retired) {
                        ImGui::TextDisabled("[retired]");
                    } else if (!d.envVar) {
                        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "always-on");
                    } else {
                        const char* v = std::getenv(d.envVar);
                        bool on;
                        const char* src;
                        if (!v) {
                            on  = d.defaultOn;
                            src = d.defaultOn ? "on (default)" : "off (default)";
                        } else if (v[0] == '0') {
                            on  = false;
                            src = "off (forced)";
                        } else {
                            on  = true;
                            src = "on (forced)";
                        }
                        if (on)
                            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s", src);
                        else
                            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f), "%s", src);
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Object Inspector", &s_open)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Ctrl+Shift+F -- Renderer Features");
    ImGui::Separator();

    const auto& lk = s_selection.lookup;

    // Object-ID — shown even for invalid / failed picks.
    if (ImGui::CollapsingHeader("Object-ID", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Raw pixel:     0x%08X", lk.rawObjectId);
        if (lk.rawObjectId != 0u) {
            RenderCore::RenderObjectHandle hraw;
            hraw.bits = lk.rawObjectId;
            ImGui::Text("  idx [19:0]:  %u", hraw.index());
            ImGui::Text("  gen [31:20]: %u", hraw.generation());
        }
        if (lk.isValid) {
            ImGui::TextColored(ImVec4(0.4f,1.0f,0.4f,1.f), "Valid:         yes");
        } else {
            ImGui::TextColored(ImVec4(1.0f,0.55f,0.3f,1.f), "Valid:         no");
            if (lk.lookupFailReason)
                ImGui::Text("  Reason:      %s", lk.lookupFailReason);
        }
    }

    if (!s_selection.hasSelection) {
        ImGui::Spacing();
        ImGui::TextUnformatted("No valid object selected.");
        ImGui::TextUnformatted("Ctrl+Shift+LMB to pick.");
        ImGui::Text("Screen: (%d, %d)", s_selection.screenX, s_selection.screenY);
        ImGui::End();
        return;
    }

    // Generic header
    const char* kindName = pickKindName(s_selection.pickKind);
    ImGui::Separator();
    ImGui::Text("Kind:          %s", kindName);
    ImGui::Text("Handle raw:    0x%08X", s_selection.handle.raw());
    ImGui::Text("Handle idx:    %u / gen: %u",
        s_selection.handle.index(), s_selection.handle.generation());
    ImGui::Text("Screen:        (%d, %d)", s_selection.screenX, s_selection.screenY);
    if (lk.worldPosValid)
        ImGui::Text("World:         (%.1f, %.1f, %.1f)", lk.worldX, lk.worldY, lk.worldZ);
    else
        ImGui::TextDisabled("World:         (unavailable)");

    // Copy All — formats everything visible in the window to the clipboard.
    ImGui::Spacing();
    if (ImGui::Button("Copy All")) {
        char buf[3072];
        int  n = 0;
        n += std::snprintf(buf+n, sizeof(buf)-n,
            "ObjID raw: 0x%08X  valid: %s%s%s\n",
            lk.rawObjectId, lk.isValid ? "yes" : "no",
            (!lk.isValid && lk.lookupFailReason) ? "  reason: " : "",
            (!lk.isValid && lk.lookupFailReason) ? lk.lookupFailReason : "");
        n += std::snprintf(buf+n, sizeof(buf)-n, "Kind: %s\n", kindName);
        n += std::snprintf(buf+n, sizeof(buf)-n,
            "Handle: 0x%08X  idx %u / gen %u\n",
            s_selection.handle.raw(),
            s_selection.handle.index(), s_selection.handle.generation());
        if (lk.worldPosValid)
            n += std::snprintf(buf+n, sizeof(buf)-n,
                "World: (%.1f, %.1f, %.1f)\n", lk.worldX, lk.worldY, lk.worldZ);
        n += std::snprintf(buf+n, sizeof(buf)-n,
            "Mesh: 0x%08X  Mat: 0x%08X  LOD: %u  Pipeline: %u\n"
            "DrawPacket: %u  PathReason: 0x%08X  GameObjID: %u\n",
            lk.meshHandleBits, lk.materialHandleBits,
            static_cast<unsigned>(lk.lodLevel), static_cast<unsigned>(lk.pipelineId),
            lk.drawPacketIndex, lk.pathReasonCode, lk.gameObjectId);
        if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                && s_staticPropData.populated) {
            n += std::snprintf(buf+n, sizeof(buf)-n,
                "RecipeIdx: %d  Shape: %s\n",
                s_staticPropData.recipeIndex,
                s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)");
            if (s_staticPropData.materialGpuPopulated) {
                const auto& mg = s_staticPropData.materialGpu;
                n += std::snprintf(buf+n, sizeof(buf)-n,
                    "MatIdx: %u  albedo: %u  normal: %u  mrTex: %u  emit: %u\n"
                    "flags: 0x%08X  baseColor: %.3f  metallic: %.3f  rough: %.3f\n",
                    s_staticPropData.materialIdx,
                    mg.albedoTex, mg.normalTex, mg.metallicRoughnessTex, mg.emissiveTex,
                    mg.flags, mg.baseColorFactor, mg.metallicFactor, mg.roughnessFactor);
            }
        }
        if (s_selection.kind == RenderWorld::RenderObjectKind::Mech
                && s_mechData.populated) {
            static const char* cnames[] = { "None","Light","Medium","Heavy","Assault" };
            static const char* snames[] = {
                "None","Sensor Q1","Sensor Q2","Sensor Q3","Sensor Q4","Visual"
            };
            const int ci  = (s_mechData.chassisClass >= 0 && s_mechData.chassisClass < 5)
                            ? s_mechData.chassisClass : 0;
            const int csi = (s_mechData.conStat >= 0 && s_mechData.conStat < 6)
                            ? s_mechData.conStat : 0;
            n += std::snprintf(buf+n, sizeof(buf)-n,
                "Variant: %s  Name: %s\n"
                "Class: %s  TeamID: %ld  Pilot: %s\n"
                "Status: %s%s%s\n"
                "Sensor: %s\n"
                "Armor: %.0f / %.0f  Structure: %.0f / %.0f\n",
                s_mechData.variantName[0] ? s_mechData.variantName : "(none)",
                s_mechData.longName[0]    ? s_mechData.longName    : "(none)",
                cnames[ci], s_mechData.teamId,
                s_mechData.pilotName[0]   ? s_mechData.pilotName   : "(none)",
                s_mechData.destroyed ? "DESTROYED " : "",
                s_mechData.disabled  ? "disabled "  : "",
                (!s_mechData.destroyed && !s_mechData.disabled && !s_mechData.crippled)
                    ? "OK" : (s_mechData.crippled ? "crippled" : ""),
                snames[csi],
                s_mechData.totalCurArmor, s_mechData.totalMaxArmor,
                s_mechData.totalCurStr,   s_mechData.totalMaxStr);
        }
        ImGui::SetClipboardText(buf);
    }
    ImGui::Separator();

    // Render Explain — consolidated one-line-per-fact render path summary.
    // Shown only for valid RenderWorld picks (not terrain). Detailed panels below.
    if (s_selection.valid) {
        if (ImGui::CollapsingHeader("Render Explain##re", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Object
            const char* kindStr = (s_selection.kind == RenderWorld::RenderObjectKind::Mech)
                                  ? "Mech" : "StaticProp";
            ImGui::Text("Object:  %s  handle 0x%08X", kindStr, s_selection.handle.raw());

            // Fresh handle-direct probe — independent of pixel-pick data age.
            RenderWorld::ObjectRecordView rv{};
            const bool rvOk = RenderWorld::getObjectRecordView(s_selection.handle, &rv);
            if (!rvOk) {
                ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f),
                                   "  (handle stale -- object may be destroyed)");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Path:");

            // DrawPacket dispatch status (StaticProp only).
            if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                if (rs.valid) {
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
                                       "  dispatch       DrawPacket default ON");
                } else {
                    ImGui::TextDisabled("  dispatch       (no Render Spine data -- pick not resolved?)");
                }
            } else {
                ImGui::TextDisabled("  dispatch       (Mech batcher path)");
            }

            // Pipeline name. Prefer Render Spine row (authoritative) over stale
            // lk.pipelineId (which is 0/unknown for static props in M1.5 lookup).
            {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                const uint32_t pipeId =
                    (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                     && rs.valid && rs.rowCount > 0)
                    ? rs.rows[0].pipelineId
                    : static_cast<uint32_t>(lk.pipelineId);
                char pipeBuf[48];
                switch (pipeId) {
                    case 0:  std::snprintf(pipeBuf, sizeof(pipeBuf), "(unknown)"); break;
                    case 1:  std::snprintf(pipeBuf, sizeof(pipeBuf), "StaticPropOpaque (1)"); break;
                    case 2:  std::snprintf(pipeBuf, sizeof(pipeBuf), "StaticPropAlphaTest (2)"); break;
                    default: std::snprintf(pipeBuf, sizeof(pipeBuf), "?(%u)", pipeId); break;
                }
                ImGui::Text("  pipeline       %s", pipeBuf);
            }

            // Material summary (StaticProp + GPU material populated).
            if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                    && s_staticPropData.populated
                    && s_staticPropData.materialGpuPopulated) {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                const int32_t layer = (rs.valid && rs.rowCount > 0)
                                      ? rs.rows[0].texArrayLayer : -1;
                const bool matchOk  = (rs.valid && rs.rowCount > 0)
                                      && (rs.rows[0].albedoTex != 0xFFFFFFFFu)
                                      && (layer >= 0)
                                      && rs.rows[0].materialMatchesLegacy;
                char matBuf[128];
                std::snprintf(matBuf, sizeof(matBuf),
                    "MaterialGpu  idx %u  albedo %u  layer %d  %s",
                    s_staticPropData.materialIdx,
                    s_staticPropData.materialGpu.albedoTex,
                    layer,
                    matchOk ? "OK" : "(mismatch)");
                ImGui::Text("  material       %s", matBuf);
            } else if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
                ImGui::TextDisabled("  material       (no GPU material data)");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Fallbacks:");

            // Key kill-switches / feature gates relevant to the static-prop path.
            struct FallbackRow { const char* label; const char* envVar; bool defaultOn; };
            static const FallbackRow kFallbacks[] = {
                { "legacy kill-switch",  "MC2_STATIC_PROP_LEGACY_DISPATCH", false },
                { "material sample",     "MC2_MATERIAL_GPU_SAMPLE",         true  },
                { "object-ID buffer",    "MC2_OBJECT_ID_BUFFER",            true  },
            };
            for (const auto& fb : kFallbacks) {
                const char* v = std::getenv(fb.envVar);
                bool on;
                if (!v)               on = fb.defaultOn;
                else if (v[0] == '0') on = false;
                else                  on = true;
                ImGui::Text("  %-22s", fb.label);
                ImGui::SameLine(0.f, 4.f);
                if (on)
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "ON");
                else
                    ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f), "OFF");
            }
        }
    }

    // LookupResult
    if (ImGui::CollapsingHeader("Lookup", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Mesh bits:     0x%08X", lk.meshHandleBits);
        ImGui::Text("Material bits: 0x%08X", lk.materialHandleBits);
        ImGui::Text("LOD:           %u",     static_cast<unsigned>(lk.lodLevel));
        ImGui::Text("Pipeline:      %u",     static_cast<unsigned>(lk.pipelineId));
        ImGui::Text("Draw packet:   %u",     lk.drawPacketIndex);
        ImGui::Text("Path reason:   0x%08X", lk.pathReasonCode);
        ImGui::Text("Game obj ID:   %u",     lk.gameObjectId);
    }

    // Kind-specific
    if (s_selection.pickKind == InspectorPickKind::StaticProp) {
        if (ImGui::CollapsingHeader("StaticProp", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s_staticPropData.populated) {
                ImGui::TextUnformatted("(no game data -- pick not resolved)");
            } else {
                ImGui::Text("Recipe idx: %d", s_staticPropData.recipeIndex);
                ImGui::Text("Shape:      %s",
                    s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)");
            }
        }

        // Render Spine — reads from g_dpSelProp (filled by gameosmain each frame).
        if (ImGui::CollapsingHeader("Render Spine##rs", ImGuiTreeNodeFlags_DefaultOpen)) {
            const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
            if (!rs.valid) {
                ImGui::TextDisabled("(not populated -- pick not resolved or no packet data)");
            } else {
                char buf[64];

                // Header summary
                if (ImGui::BeginTable("##rssum", 2,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("##rsk", ImGuiTableColumnFlags_WidthFixed, 100.f);
                    ImGui::TableSetupColumn("##rsv", ImGuiTableColumnFlags_WidthStretch);

                    auto row2 = [&](const char* k, const char* v) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k);
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(v);
                    };

                    std::snprintf(buf, sizeof(buf), "%u", rs.typeId);
                    row2("typeId", buf);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Batcher type slot (0..typeCount-1). Groups all\n"
                                          "instances that share geometry + pipeline.");

                    std::snprintf(buf, sizeof(buf), "%d", rs.recipeIndex);
                    row2("recipeIndex", buf);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Static-prop recipe/type index — NOT the\n"
                                          "RenderWorld instance slot (Handle idx).\n"
                                          "Multiple Handle slots share one recipeIndex.");

                    std::snprintf(buf, sizeof(buf), "%u", rs.packetCount);
                    row2("packetCount", buf);
                    std::snprintf(buf, sizeof(buf), "%u", rs.instanceCount);
                    row2("instances", buf);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Visible instances of this typeId this frame\n"
                                          "(count of snapshot entries with matching typeId).");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("dispatch");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(0.4f,1.f,0.4f,1.f), "DrawPacket default ON");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("materialIdx");
                    ImGui::TableSetColumnIndex(1);
                    if (rs.materialIdx == 0xFFFFFFFFu)
                        ImGui::TextDisabled("(sentinel)");
                    else {
                        std::snprintf(buf, sizeof(buf), "%u", rs.materialIdx);
                        ImGui::TextUnformatted(buf);
                    }

                    ImGui::EndTable();
                }

                // Per-packet rows
                if (rs.rowCount > 0) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Packets:");
                    if (ImGui::BeginTable("##rspkts", 8,
                            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                            | ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupColumn("pkt",      ImGuiTableColumnFlags_WidthFixed, 36.f);
                        ImGui::TableSetupColumn("pipeline", ImGuiTableColumnFlags_WidthFixed, 68.f);
                        ImGui::TableSetupColumn("matIdx",   ImGuiTableColumnFlags_WidthFixed, 52.f);
                        ImGui::TableSetupColumn("albedo",   ImGuiTableColumnFlags_WidthFixed, 52.f);
                        ImGui::TableSetupColumn("layer",    ImGuiTableColumnFlags_WidthFixed, 40.f);
                        ImGui::TableSetupColumn("match",    ImGuiTableColumnFlags_WidthFixed, 44.f);
                        ImGui::TableSetupColumn("baseV",    ImGuiTableColumnFlags_WidthFixed, 52.f);
                        ImGui::TableSetupColumn("baseInst", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (uint32_t r = 0; r < rs.rowCount; ++r) {
                            const DrawPacketPropRow& pr = rs.rows[r];
                            ImGui::TableNextRow();

                            ImGui::TableSetColumnIndex(0);
                            std::snprintf(buf, sizeof(buf), "%u", pr.globalPacketIdx);
                            ImGui::TextUnformatted(buf);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Global packet index in batcher sorted array.\n"
                                                  "Not a RenderWorld drawPacketIndex.");

                            ImGui::TableSetColumnIndex(1);
                            switch (pr.pipelineId) {
                                case 0:  ImGui::TextDisabled("invalid");      break;
                                case 1:  ImGui::TextUnformatted("opaque");    break;
                                case 2:  ImGui::TextColored(ImVec4(1.f,0.85f,0.3f,1.f), "alpha_test"); break;
                                default:
                                    std::snprintf(buf, sizeof(buf), "?(%u)", pr.pipelineId);
                                    ImGui::TextColored(ImVec4(1.f,0.5f,0.3f,1.f), "%s", buf);
                                    break;
                            }

                            ImGui::TableSetColumnIndex(2);
                            if (rs.materialIdx == 0xFFFFFFFFu)
                                ImGui::TextDisabled("--");
                            else {
                                std::snprintf(buf, sizeof(buf), "%u", rs.materialIdx);
                                ImGui::TextUnformatted(buf);
                            }

                            ImGui::TableSetColumnIndex(3);
                            if (pr.albedoTex == 0xFFFFFFFFu)
                                ImGui::TextDisabled("--");
                            else {
                                std::snprintf(buf, sizeof(buf), "%u", pr.albedoTex);
                                ImGui::TextUnformatted(buf);
                            }

                            ImGui::TableSetColumnIndex(4);
                            if (pr.texArrayLayer < 0)
                                ImGui::TextDisabled("--");
                            else {
                                std::snprintf(buf, sizeof(buf), "%d", pr.texArrayLayer);
                                ImGui::TextUnformatted(buf);
                            }

                            ImGui::TableSetColumnIndex(5);
                            if (pr.albedoTex == 0xFFFFFFFFu || pr.texArrayLayer < 0)
                                ImGui::TextDisabled("n/a");
                            else if (pr.materialMatchesLegacy)
                                ImGui::TextColored(ImVec4(0.4f,1.f,0.4f,1.f), "OK");
                            else
                                ImGui::TextColored(ImVec4(1.f,0.4f,0.3f,1.f), "DIFF");

                            ImGui::TableSetColumnIndex(6);
                            std::snprintf(buf, sizeof(buf), "%d", pr.baseVertex);
                            ImGui::TextUnformatted(buf);

                            ImGui::TableSetColumnIndex(7);
                            ImGui::TextDisabled("--");
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Deferred — only valid during dispatch (v1+).\n"
                                                  "Not available at emit time.");
                        }
                        ImGui::EndTable();
                    }
                }
            }
        }
    } else if (s_selection.pickKind == InspectorPickKind::Mech) {
        if (ImGui::CollapsingHeader("Mech", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s_mechData.populated) {
                ImGui::TextUnformatted("(no game data -- pick not resolved)");
            } else {
                static const char* s_classNames[] = { "None","Light","Medium","Heavy","Assault" };
                static const char* s_conStatNames[] = {
                    "None","Sensor Q1","Sensor Q2","Sensor Q3","Sensor Q4","Visual"
                };
                const int ci  = (s_mechData.chassisClass >= 0 && s_mechData.chassisClass < 5)
                                ? s_mechData.chassisClass : 0;
                const int csi = (s_mechData.conStat >= 0 && s_mechData.conStat < 6)
                                ? s_mechData.conStat : 0;

                ImGui::Text("Variant:   %s", s_mechData.variantName[0] ? s_mechData.variantName : "(none)");
                ImGui::Text("Name:      %s", s_mechData.longName[0]    ? s_mechData.longName    : "(none)");
                ImGui::Text("Class:     %s", s_classNames[ci]);
                ImGui::Text("Team ID:   %ld", s_mechData.teamId);
                ImGui::Text("Pilot:     %s", s_mechData.pilotName[0]   ? s_mechData.pilotName   : "(none)");
                ImGui::Separator();
                if (!s_mechData.destroyed && !s_mechData.disabled && !s_mechData.crippled) {
                    ImGui::Text("Status:    OK");
                } else {
                    ImGui::Text("Status:    %s%s%s",
                        s_mechData.destroyed ? "DESTROYED " : "",
                        s_mechData.disabled  ? "disabled "  : "",
                        s_mechData.crippled  ? "crippled"   : "");
                }
                ImGui::Text("Sensor:    %s", s_conStatNames[csi]);
                ImGui::Separator();
                ImGui::Text("Armor:     %.0f / %.0f", s_mechData.totalCurArmor, s_mechData.totalMaxArmor);
                ImGui::Text("Structure: %.0f / %.0f", s_mechData.totalCurStr,   s_mechData.totalMaxStr);
            }
        }

        // MECH-EXTRACTION-2: mech snapshot panel (gate: MC2_SNAPSHOT_MECH_EXTRACT=1).
        // Read-only view of RenderSnapshot mech counters + per-selected-mech row detail.
        {
        const RenderSnapshot* snap = getLastRenderSnapshot();
        const bool gateOn = snap && (snap->mechSnapshotCount > 0
                                     || snap->mechMatValid > 0
                                     || snap->mechMatSentinel > 0);
        const ImGuiTreeNodeFlags snapFlags = gateOn
            ? (ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap)
            : ImGuiTreeNodeFlags_AllowOverlap;
        bool msOpen = ImGui::CollapsingHeader("Mech Snapshot##ms", snapFlags);
        if (msOpen) {
            if (!snap || !gateOn) {
                ImGui::TextDisabled("(no data -- set MC2_SNAPSHOT_MECH_EXTRACT=1)");
            } else {
                // Frame summary
                ImGui::Text("rows=%u  mat_valid=%u  mat_sentinel=%u",
                    snap->mechSnapshotCount, snap->mechMatValid, snap->mechMatSentinel);

                const bool anyMismatch = (snap->mechCountMismatch      != 0u
                                       || snap->mechHandleMismatch     != 0u
                                       || snap->mechObjectIdMismatch   != 0u
                                       || snap->mechTexHandleMismatch  != 0u
                                       || snap->mechMaterialIdxMismatch != 0u);
                if (anyMismatch) {
                    ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                        "countMis=%u handleMis=%u objectIdMis=%u texMis=%u matMis=%u",
                        snap->mechCountMismatch, snap->mechHandleMismatch,
                        snap->mechObjectIdMismatch, snap->mechTexHandleMismatch,
                        snap->mechMaterialIdxMismatch);
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
                        "countMis=0 handleMis=0 objectIdMis=0 texMis=0 matMis=0");
                }

                // MECH-SPINE-1: pass-level program ids (legacy path — mech
                // is NOT on the PipelineDesc registry yet; surfacing the gap
                // here mirrors the TERRAIN-SPINE-0 "ViewUniforms not bound"
                // red-flag label).
                ImGui::Separator();
                const uint32_t mechProg   = gos_getMechProgramId();
                const uint32_t shadowProg = gos_getMechShadowProgramId();
                ImGui::Text("mech program       %u", mechProg);
                ImGui::Text("shadow_mech program %u", shadowProg);
                ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                    "PipelineDesc: legacy (not on registry)");
                ImGui::TextDisabled("pass: opaque + shadow (legacy mech path)");

                // Per-selected-mech row (match by objectIdRaw == handle.raw())
                ImGui::Separator();
                const uint32_t selRaw = s_selection.handle.raw();
                bool rowFound = false;
                for (uint32_t i = 0u; i < snap->mechPackets.size(); ++i) {
                    const ExtractedMechPacket& row = snap->mechPackets[i];
                    if (row.objectIdRaw != selRaw) continue;
                    rowFound = true;
                    ImGui::Text("Row %u:  (extracted this frame)", i);
                    ImGui::Text("  handle      0x%08X", row.objectIdRaw);
                    {
                        const char* texName =
                            gos_getMechTextureNameByNodeIdx(row.texHandle);
                        if (texName && *texName)
                            ImGui::Text("  texHandle   %u  (%s)", row.texHandle, texName);
                        else
                            ImGui::Text("  texHandle   %u", row.texHandle);
                    }
                    if (row.materialIdx == 0xFFFFFFFFu)
                        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                            "  materialIdx sentinel (0xFFFFFFFF)");
                    else
                        ImGui::Text("  materialIdx %u", row.materialIdx);
                    ImGui::Text("  typeLodIdx  %u", row.typeLodIdx);
                    ImGui::Text("  renderFlags 0x%02X", row.renderFlags);
                    break;
                }
                if (!rowFound)
                    ImGui::TextDisabled("  (selected handle NOT in snapshot this frame)");
            }
        }
        } // Mech Snapshot block

    } else if (s_selection.pickKind == InspectorPickKind::Terrain) {
        if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s_terrainData.populated) {
                ImGui::TextUnformatted("(terrain pick not resolved)");
            } else {
                ImGui::Text("World:  (%.1f, %.1f, %.1f)",
                    s_terrainData.worldX, s_terrainData.worldY, s_terrainData.worldZ);
                ImGui::Text("Tile:   row %-4d  col %d",
                    s_terrainData.tileRow, s_terrainData.tileCol);
                ImGui::Text("Cell:   row %-4d  col %d",
                    s_terrainData.cellRow, s_terrainData.cellCol);
                if (s_terrainData.terrainType >= 0)
                    ImGui::Text("Type:   %d", s_terrainData.terrainType);
                else
                    ImGui::TextDisabled("Type:   (n/a v1)");
            }
            static int s_drEnabled = -1;
            if (s_drEnabled < 0) {
                const char* v = std::getenv("MC2_DEBUG_RENDERER");
                s_drEnabled = (!v || v[0] != '0') ? 1 : 0;  // default-ON
            }
            if (!s_drEnabled)
                ImGui::TextDisabled("Highlight: off (MC2_DEBUG_RENDERER=0)");
        }
    }

    // TERRAIN-SPINE-0: pass-level (not selection-driven) view of the terrain
    // render spine. Visible regardless of pick state.
    if (ImGui::CollapsingHeader("Terrain Pass##tp", ImGuiTreeNodeFlags_DefaultOpen)) {
        const TerrainPassSnapshot& ts = s_terrainPass;
        ImGui::Text("View: id=%u (%s)", ts.currentViewId,
                    ts.currentViewName ? ts.currentViewName : "");
        if (ts.viewUniformsBoundForTerrain) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "ViewUniforms (binding=3): consumed");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "ViewUniforms (binding=3): NOT consumed (legacy uniforms)");
        }
        ImGui::Separator();
        ImGui::Text("Programs:");
        ImGui::BulletText("surface (solid):      %u", ts.surfaceProgramId);
        ImGui::BulletText("thin records:         %u", ts.thinProgramId);
        ImGui::BulletText("water fast:           %u", ts.waterFastProgramId);
        ImGui::BulletText("overlay:              %u", ts.overlayProgramId);
        ImGui::Separator();
        ImGui::Text("Last flush stats:");
        ImGui::BulletText("draw buckets:    %u", ts.bucketCount);
        ImGui::BulletText("verts (expanded): %u", ts.vertCount);
        ImGui::BulletText("thin records:    %u", ts.thinRecCount);
        ImGui::BulletText("recipes:         %u", ts.recipeCount);
        if (ts.overflow) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "OVERFLOWED last flush");
        }
        ImGui::Separator();
        ImGui::Text("Tessellation: %s", ts.tessellationOn ? "ON (always)" : "off");
    }

    // SHADOW-SPINE-0: pass-level (not selection-driven) view of the shadow
    // render spine. Mirrors the terrain header above.
    if (ImGui::CollapsingHeader("Shadow Pass##sp", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ShadowPassSnapshot& sp = s_shadowPass;
        ImGui::Text("Shadows enabled: %s", sp.shadowsEnabled ? "yes" : "no");
        ImGui::Text("Static light matrix built: %s", sp.staticLightMatrixBuilt ? "yes" : "no");
        ImGui::Text("Shadow map size: %d   Dyn shadow map size: %d",
                    sp.shadowMapSize, sp.dynShadowMapSize);
        if (sp.viewUniformsBoundForShadow) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "ViewUniforms (binding=3): consumed");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "ViewUniforms (binding=3): NOT consumed (legacy shadow matrices)");
        }
        ImGui::Separator();
        ImGui::Text("Programs:");
        ImGui::BulletText("terrain shadow:     %u", sp.terrainShadowProgramId);
        ImGui::BulletText("mech shadow:        %u", sp.mechShadowProgramId);
        ImGui::BulletText("static-prop shadow: %u", sp.staticPropShadowProgramId);
        ImGui::Separator();
        ImGui::Text("Caster counts (last flushShadow):");
        ImGui::BulletText("mech         types=%u  instances=%u",
                          sp.mechShadowTypesDrawn, sp.mechShadowInstDrawn);
        ImGui::BulletText("static-prop  types=%u  instances=%u",
                          sp.staticPropShadowTypesDrawn, sp.staticPropShadowInstDrawn);
        ImGui::Separator();
        ImGui::TextDisabled("PipelineDesc: legacy (shadow pass not on registry)");
    }

    // VFX-SPINE-0: pass-level view of the GPU particle / VFX render spine.
    // Mirrors the Shadow Pass header above. Read-only — no GL state, no VFX
    // mutation. VFX object-IDs are prohibited and intentionally absent.
    if (ImGui::CollapsingHeader("VFX Pass##vfx", ImGuiTreeNodeFlags_DefaultOpen)) {
        const VfxPassSnapshot& vs = s_vfxPass;
        // GPU particles enabled status (driven by MC2_GPU_PARTICLES env var).
        if (vs.gpuParticlesEnabled) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "GPU particles: enabled (MC2_GPU_PARTICLES default ON)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "GPU particles: DISABLED (MC2_GPU_PARTICLES=0 — legacy CPU FX only)");
        }
        ImGui::Text("Verbose log gate (MC2_GPU_PARTICLES_LOG): %s",
                    vs.gpuParticlesLogEnabled ? "ON" : "off");
        if (vs.initFailed) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Bridge init: FAILED (program compile/link error)");
        }
        ImGui::Text("Camera basis set this frame: %s",
                    vs.cameraSetThisFrame ? "yes" : "no (using last-known)");
        if (vs.viewUniformsBoundForVfx) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "ViewUniforms (binding=3): consumed");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "ViewUniforms (binding=3): NOT consumed (legacy gosFX path)");
        }
        ImGui::Separator();
        ImGui::Text("Program:");
        ImGui::BulletText("particle_billboard: %u", vs.particleProgramId);
        ImGui::Separator();
        ImGui::Text("Blend modes (per-group): alpha (0) or additive (1) — set per BeginGroup");
        ImGui::Separator();
        ImGui::Text("Particle buffers:");
        ImGui::BulletText("CPU staging budget:   %u records", vs.perFrameBudget);
        ImGui::BulletText("GL SSBO capacity:     %u records (binding=14)", vs.ssboCapacityRecords);
        if (vs.overflowReported) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "OVERFLOW reported (staging exceeded budget — record dropped)");
        }
        ImGui::Separator();
        ImGui::Text("Process-lifetime aggregates:");
        ImGui::BulletText("emit total:                %llu", vs.emitTotal);
        ImGui::BulletText("flush total:               %llu", vs.flushTotal);
        ImGui::BulletText("non-empty flushes:         %llu", vs.nonemptyFlushTotal);
        ImGui::BulletText("records flushed total:     %llu", vs.recordsFlushedTotal);
        ImGui::BulletText("records per flush (max):   %u",   vs.recordsPerFlushMax);
        ImGui::BulletText("trail spawn total:         %llu", vs.trailSpawnTotal);
        ImGui::BulletText("trail head total:          %llu", vs.trailHeadTotal);
        ImGui::Separator();
        ImGui::Text("Active particle kinds (GpuTrailKind enum):");
        // TODO: wire per-kind draw counts — not currently tracked by Batcher.
        // gosFX legacy specs use distinct names (Card/PertCloud/PointCloud/etc.)
        // but the GPU path collapses them into a single SSBO; per-kind counts
        // would require a new counter and are out of scope for VFX-SPINE-0.
        ImGui::BulletText("None         (no trail)         draws: n/a");
        ImGui::BulletText("MissileSmoke (handle 41)         draws: n/a");
        ImGui::BulletText("PpcBolt      (handle 33)         draws: n/a");
        ImGui::Separator();
        ImGui::TextDisabled("PipelineDesc: legacy (VFX not on registry; object-IDs prohibited)");
    }

    // Material — shown for any kind; GPU fields only when MC2_MATERIAL_GPU active.
    // For static props: handle bits come from RenderWorld lookup (currently unpopulated →
    // handle=0). Use Render Spine above for authoritative materialIdx + albedoTex.
    if (ImGui::CollapsingHeader("Material (RW handle lookup)")) {
        RenderCore::MaterialHandle mh;
        mh.bits = lk.materialHandleBits;
        if (mh.bits == 0u) {
            ImGui::TextDisabled("Handle:        (unknown)");
            // For static props: handle bits are not yet populated by the RenderWorld
            // lookup path. Render Spine materialIdx is the authoritative source.
            if (s_selection.pickKind == InspectorPickKind::StaticProp)
                ImGui::TextDisabled("  (use Render Spine for authoritative materialIdx)");
        } else {
            ImGui::Text("Handle idx:    %u", mh.index());
            ImGui::Text("Handle gen:    %u", mh.generation());
            ImGui::Text("Handle raw:    0x%08X", mh.raw());
        }
        ImGui::Spacing();
        if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                && s_staticPropData.populated) {
            if (!s_staticPropData.materialGpuActive) {
                ImGui::TextDisabled("Source:        legacy texArrayLayer");
                ImGui::TextDisabled("(MC2_MATERIAL_GPU not active)");
            } else if (!s_staticPropData.materialGpuPopulated) {
                ImGui::TextColored(ImVec4(1.f,0.55f,0.3f,1.f),
                    "Source:        MaterialGpu");
                ImGui::TextDisabled("  handle bits unpopulated (see Render Spine)");
            } else {
                const auto& mg = s_staticPropData.materialGpu;
                ImGui::Text("Source:        MaterialGpu");
                ImGui::Text("matIdx:        %u / gen %u",
                    s_staticPropData.materialIdx, s_staticPropData.materialGen);
                ImGui::Separator();
                auto texLabel = [](uint32_t t, char* b, size_t n) {
                    if (t == 0xFFFFFFFFu) std::snprintf(b, n, "(absent)");
                    else std::snprintf(b, n, "%u", t);
                };
                char tb[24];
                texLabel(mg.albedoTex, tb, sizeof(tb));
                ImGui::Text("albedoTex:     %s", tb);
                texLabel(mg.normalTex, tb, sizeof(tb));
                ImGui::Text("normalTex:     %s", tb);
                texLabel(mg.metallicRoughnessTex, tb, sizeof(tb));
                ImGui::Text("mrTex:         %s", tb);
                texLabel(mg.emissiveTex, tb, sizeof(tb));
                ImGui::Text("emissiveTex:   %s", tb);
                ImGui::Separator();
                ImGui::Text("flags:         0x%08X%s%s%s%s%s%s",
                    mg.flags,
                    (mg.flags & 1u)  ? " AlphaTest"  : "",
                    (mg.flags & 2u)  ? " NormalMap"  : "",
                    (mg.flags & 4u)  ? " MetalRough" : "",
                    (mg.flags & 8u)  ? " Emissive"   : "",
                    (mg.flags & 16u) ? " DblSided"   : "",
                    (mg.flags & 32u) ? " Window"     : "");
                ImGui::Text("baseColor:     %.3f", mg.baseColorFactor);
                ImGui::Text("metallic:      %.3f", mg.metallicFactor);
                ImGui::Text("roughness:     %.3f", mg.roughnessFactor);
            }
        } else {
            ImGui::TextDisabled("(MaterialGpu data: StaticProp only)");
        }
    }

    // Env Gates — live read of all MC2_* flags (default-ON: absent = enabled).
    if (ImGui::CollapsingHeader("Env Gates")) {
        struct GateInfo { const char* name; bool defaultOn; };
        static const GateInfo kGates[] = {
            { "MC2_IMGUI",            true  },
            { "MC2_IMGUI_INSPECTOR",  true  },
            { "MC2_OBJECT_ID_BUFFER", true  },
            { "MC2_DEBUG_RENDERER",   true  },
        };
        for (const auto& g : kGates) {
            const char* v = std::getenv(g.name);
            bool on;
            const char* src;
            if (!v) {
                on  = g.defaultOn;
                src = g.defaultOn ? "(default ON)" : "(default OFF)";
            } else if (v[0] == '0') {
                on  = false;
                src = "(=0)";
            } else {
                on  = true;
                src = "(=1)";
            }
            ImGui::Text("%s", g.name);
            ImGui::SameLine();
            if (on)
                ImGui::TextColored(ImVec4(0.4f,1.f,0.4f,1.f), "ON  %s", src);
            else
                ImGui::TextColored(ImVec4(1.f,0.55f,0.3f,1.f), "OFF %s", src);
        }
    }

    ImGui::End();
}

void EditorInspector::flushDebugHighlight() {
    if (!isEnabled()) return;
    if (!s_open)      return;
    if (!s_selection.hasSelection) return;

    // 0xRRGGBBAA, opaque.
    constexpr uint32_t kStaticPropCol = 0xFFFFFFFFu;  // white
    constexpr uint32_t kMechCol       = 0xFFFF00FFu;  // yellow
    constexpr uint32_t kTerrainCol    = 0x44FF88FFu;  // green

    const auto pk = s_selection.pickKind;

    if ((pk == InspectorPickKind::StaticProp || pk == InspectorPickKind::Mech)
            && s_selection.valid) {
        const float radius   = (pk == InspectorPickKind::Mech) ? 4.f : 2.f;
        const uint32_t col   = (pk == InspectorPickKind::Mech) ? kMechCol : kStaticPropCol;
        DebugRenderer::Vec3 c{
            s_selection.lookup.worldX,
            s_selection.lookup.worldY,
            s_selection.lookup.worldZ
        };
        DebugRenderer::drawRingWorld(c, radius, 16, col);

    } else if (pk == InspectorPickKind::Terrain && s_terrainData.populated) {
        // Flat X/Z crosshair at terrain hit. No vertical arm - terrain
        // marker should look flush with the ground, not like an object bracket.
        const float x   = s_terrainData.worldX;
        const float y   = s_terrainData.worldY;
        const float z   = s_terrainData.worldZ;
        const float arm = 5.f;
        DebugRenderer::drawLineWorld({ x-arm, y, z     }, { x+arm, y, z     }, kTerrainCol);
        DebugRenderer::drawLineWorld({ x,     y, z-arm }, { x,     y, z+arm }, kTerrainCol);
    }
}
