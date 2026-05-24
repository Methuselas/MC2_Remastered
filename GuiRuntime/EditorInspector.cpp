#include "EditorInspector.h"
#include "imgui.h"
#include <cstdlib>   // getenv

namespace {

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0)
        cached = (std::getenv("MC2_IMGUI_INSPECTOR") != nullptr) ? 1 : 0;
    return cached == 1;
}

static EditorInspector::InspectorSelection     s_selection;
static EditorInspector::StaticPropInspectorData s_staticPropData;
static EditorInspector::MechInspectorData       s_mechData;
static bool s_open = false;

}  // namespace

void EditorInspector::onCtrlShiftClick(int mouseX, int mouseY) {
    if (!isEnabled()) return;
    // Coords are recorded; missiongui.cpp calls setPickResult after
    // running tryGameplayPick to populate the full selection.
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
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
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    s_selection.valid   = lookup.isValid;
    s_selection.lookup  = lookup;
    if (lookup.isValid) {
        s_selection.kind   = lookup.kind;
        s_selection.handle = lookup.handle;
    }
    s_open = true;
}

void EditorInspector::setStaticPropData(const StaticPropInspectorData& sd) {
    s_staticPropData = sd;
}

void EditorInspector::setMechData(const MechInspectorData& md) {
    s_mechData = md;
}

void EditorInspector::clear() {
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_open = false;
}

void EditorInspector::drawImGui() {
    if (!isEnabled() || !s_open) return;

    ImGui::SetNextWindowSize(ImVec2(420, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Object Inspector", &s_open)) {
        ImGui::End();
        return;
    }

    if (!s_selection.valid) {
        ImGui::TextUnformatted("No object selected.");
        ImGui::TextUnformatted("Ctrl+Shift+LMB to pick.");
        ImGui::Text("Last click: (%d, %d)", s_selection.screenX, s_selection.screenY);
        ImGui::End();
        return;
    }

    // Generic header
    const char* kindNames[] = { "StaticProp", "Mech", "Terrain", "Vfx", "Unknown" };
    const unsigned kindIdx = static_cast<unsigned>(s_selection.kind);
    ImGui::Text("Kind:          %s", kindIdx < 4 ? kindNames[kindIdx] : kindNames[4]);
    ImGui::Text("Handle raw:    0x%08X", s_selection.handle.raw());
    ImGui::Text("Handle idx:    %u / gen: %u",
        s_selection.handle.index(), s_selection.handle.generation());
    ImGui::Text("Screen:        (%d, %d)", s_selection.screenX, s_selection.screenY);
    if (s_selection.lookup.worldPosValid)
        ImGui::Text("World:         (%.1f, %.1f, %.1f)",
            s_selection.lookup.worldX, s_selection.lookup.worldY, s_selection.lookup.worldZ);
    else
        ImGui::TextDisabled("World:         (unavailable)");
    ImGui::Separator();

    // LookupResult
    if (ImGui::CollapsingHeader("Lookup", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& lk = s_selection.lookup;
        ImGui::Text("Mesh bits:     0x%08X", lk.meshHandleBits);
        ImGui::Text("Material bits: 0x%08X", lk.materialHandleBits);
        ImGui::Text("LOD:           %u",     static_cast<unsigned>(lk.lodLevel));
        ImGui::Text("Pipeline:      %u",     static_cast<unsigned>(lk.pipelineId));
        ImGui::Text("Draw packet:   %u",     lk.drawPacketIndex);
        ImGui::Text("Path reason:   0x%08X", lk.pathReasonCode);
        ImGui::Text("Game obj ID:   %u",     lk.gameObjectId);
    }

    // Kind-specific
    if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
        if (ImGui::CollapsingHeader("StaticProp", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s_staticPropData.populated) {
                ImGui::TextUnformatted("(no game data -- pick not resolved)");
            } else {
                ImGui::Text("Recipe idx: %d", s_staticPropData.recipeIndex);
                ImGui::Text("Shape:      %s",
                    s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)");
            }
        }
    } else if (s_selection.kind == RenderWorld::RenderObjectKind::Mech) {
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
    } else if (s_selection.kind == RenderWorld::RenderObjectKind::Terrain) {
        if (ImGui::CollapsingHeader("Terrain")) {
            ImGui::TextUnformatted("Terrain pick reserved (M3).");
        }
    }

    ImGui::End();
}
