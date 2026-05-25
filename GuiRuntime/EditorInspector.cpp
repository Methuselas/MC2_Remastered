#include "EditorInspector.h"
#include "imgui.h"
#include <cstdio>    // snprintf
#include <cstdlib>   // getenv
#include "../GameOS/gameos/debug_renderer.h"  // IMG-INSPECT-3

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
static bool s_open = false;

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

void EditorInspector::clear() {
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    s_open = false;
}

void EditorInspector::drawImGui() {
    if (!isEnabled()) return;

    // Ctrl+Shift+I toggles the panel regardless of current open state.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I))
        s_open = !s_open;

    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Object Inspector", &s_open)) {
        ImGui::End();
        return;
    }

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
            if (s_drEnabled < 0)
                s_drEnabled = (std::getenv("MC2_DEBUG_RENDERER") != nullptr) ? 1 : 0;
            if (!s_drEnabled)
                ImGui::TextDisabled("Highlight: off (set MC2_DEBUG_RENDERER=0 to disable)");
        }
    }

    // Material — shown for any kind; GPU fields only when MC2_MATERIAL_GPU active.
    if (ImGui::CollapsingHeader("Material")) {
        RenderCore::MaterialHandle mh;
        mh.bits = lk.materialHandleBits;
        if (mh.bits == 0u) {
            ImGui::TextDisabled("Handle:        (unknown)");
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
                ImGui::TextDisabled("  idx out of range");
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
