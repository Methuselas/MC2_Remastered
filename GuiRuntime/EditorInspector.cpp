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

static EditorInspector::InspectorSelection s_selection;
static bool s_open = false;

}  // namespace

void EditorInspector::onCtrlShiftClick(int mouseX, int mouseY) {
    if (!isEnabled()) return;
    // Coords are recorded; missiongui.cpp calls setPickResult after
    // running tryGameplayPick to populate the full selection.
    s_selection = InspectorSelection{};
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    s_open = true;
}

void EditorInspector::setPickResult(int mouseX, int mouseY,
                                    const RenderWorld::LookupResult& lookup) {
    if (!isEnabled()) return;
    s_selection = InspectorSelection{};
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

void EditorInspector::clear() {
    s_selection = InspectorSelection{};
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
        if (ImGui::CollapsingHeader("StaticProp")) {
            ImGui::Text("Recipe via gameObjectId: %u", s_selection.lookup.gameObjectId);
            ImGui::Text("Mesh handle bits:        0x%08X", s_selection.lookup.meshHandleBits);
            ImGui::Text("Mat handle bits:         0x%08X", s_selection.lookup.materialHandleBits);
        }
    } else if (s_selection.kind == RenderWorld::RenderObjectKind::Mech) {
        if (ImGui::CollapsingHeader("Mech")) {
            ImGui::Text("Handle idx: %u  gen: %u",
                s_selection.handle.index(), s_selection.handle.generation());
            ImGui::Text("Game obj ID: %u", s_selection.lookup.gameObjectId);
            ImGui::TextUnformatted("(Reverse BattleMech* lookup requires game layer)");
        }
    } else if (s_selection.kind == RenderWorld::RenderObjectKind::Terrain) {
        if (ImGui::CollapsingHeader("Terrain")) {
            ImGui::TextUnformatted("Terrain pick reserved (M3).");
        }
    }

    ImGui::End();
}
