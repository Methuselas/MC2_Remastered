/***************************************************************
* FILENAME: UnitBrainPanel.cpp
* DESCRIPTION: Editor "AI / Brain / Orders" inspector panel — see header.
*   Read-only display of what a placed unit DOES today (AI brain script,
*   pilot, lance/squad) plus a scaffolded Orders & Stance section ("this is
*   where this goes") for patrol waypoints + attitude/ROE editing. The edit
*   controls are intentionally disabled: there is no editor-side persistence
*   for unit orders/waypoints yet, and runtime order delivery is tied to the
*   in-flight TECHSCRIPT brain dispatch (MC2_BRAIN_DISPATCH*, default-OFF).
* DATE: 2026-06-24
****************************************************************/

#include "stdafx.h"

#include "UnitBrainPanel.h"

#include "imgui.h"

#include "EditorObjectMgr.h"
#include "EditorObjects.h"
#include "InspectorPanel.h"   // CategoryToken (shared category labelling)

#include <cstdio>
#include <cstring>

namespace {

bool s_open = false;

// First selected object from the shared selection list (viewport + Scene
// Outliner both feed it), mirroring InspectorPanel's selection access.
EditorObject* firstSelected(int* outCount)
{
	int count = 0;
	EditorObject* first = nullptr;
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if (mgr)
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST sel = mgr->getSelectedObjectList();
		for (EditorObjectMgr::EDITOR_OBJECT_LIST::EIterator it = sel.Begin();
			!it.IsDone(); it++)
		{
			if (*it)
			{
				if (!first)
					first = *it;
				++count;
			}
		}
	}
	if (outCount)
		*outCount = count;
	return first;
}

// Combat attitude / stance labels. Mirrors enum AttitudeType in code/tacordr.h
// (CAUTIOUS..SUICIDAL). Kept as a local string table so this panel pulls in no
// runtime-only game headers (the value a unit actually carries is runtime state
// today — see the Orders & Stance scaffold note).
const char* const kAttitudeNames[] = {
	"Cautious", "Conservative", "Normal", "Aggressive", "Berserker", "Suicidal"
};
const int kAttitudeCount = (int)(sizeof(kAttitudeNames) / sizeof(kAttitudeNames[0]));

} // namespace

void UnitBrainPanel::Open()  { s_open = true; }
void UnitBrainPanel::Close() { s_open = false; }
void UnitBrainPanel::Toggle(){ s_open = !s_open; }
bool UnitBrainPanel::IsOpen(){ return s_open; }

void UnitBrainPanel::Draw()
{
	if (!s_open)
		return;

	ImGui::SetNextWindowSize(ImVec2(320.f, 420.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("AI / Brain / Orders", &s_open))
	{
		ImGui::End();
		return;
	}

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if (!mgr)
	{
		ImGui::TextDisabled("No map loaded.");
		ImGui::End();
		return;
	}

	int selCount = 0;
	EditorObject* obj = firstSelected(&selCount);
	if (!obj)
	{
		ImGui::TextDisabled("Select a unit to view its AI / orders.");
		ImGui::End();
		return;
	}

	Unit* unit = dynamic_cast<Unit*>(obj);
	if (!unit)
	{
		ImGui::TextDisabled("Selected object is not a unit.");
		ImGui::Text("Category: %s", InspectorPanel::CategoryToken(obj));
		ImGui::End();
		return;
	}

	if (selCount > 1)
		ImGui::TextDisabled("%d objects selected (showing first unit)", selCount);

	// --- Identity ---------------------------------------------------------
	const char* name = obj->getDisplayName();
	if (!name || !name[0])
		name = "(unnamed)";
	ImGui::Text("Unit:  %s", name);
	ImGui::Text("ID:    0x%08lX", (unsigned long)obj->getID());

	Pilot* pilot = unit->getPilot();
	const char* pilotName = (pilot && pilot->info) ? pilot->getName() : nullptr;
	ImGui::Text("Pilot: %s", (pilotName && pilotName[0]) ? pilotName : "(none)");

	int lance = -1, lanceIdx = -1;
	unit->getLanceInfo(lance, lanceIdx);
	ImGui::Text("Lance: %d (slot %d)   Squad: %lu",
		lance, lanceIdx, (unsigned long)unit->getSquad());

	ImGui::Separator();

	// --- AI Brain (what this unit runs) -----------------------------------
	if (ImGui::CollapsingHeader("AI Brain", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const Brain& brain = unit->getBrain();
		const char* bn = brain.getBrainName();
		ImGui::Text("Brain script: %s", (bn && bn[0]) ? bn : "(none / default)");
		ImGui::Text("ABL cells:    %ld", brain.getNumCells());
		ImGui::Text("Static vars:  %ld", brain.getNumStaticVars());
		ImGui::TextDisabled("This is the AI program the unit runs at mission start.");
	}

	// --- Orders & Stance (scaffold — "this is where this goes") ------------
	if (ImGui::CollapsingHeader("Orders & Stance", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped(
			"Initial orders, patrol path and stance for this unit go here. "
			"Editing is not wired yet: the editor does not persist per-unit "
			"orders today (they are runtime-only), and runtime delivery is "
			"tied to the in-flight TECHSCRIPT brain dispatch "
			"(MC2_BRAIN_DISPATCH*, default-OFF). The controls below show the "
			"intended shape.");

		ImGui::Spacing();

		// Stance / attitude (preview — not persisted yet).
		ImGui::BeginDisabled(true);
		static int s_attitude = 2; // Normal — display default
		ImGui::Combo("Stance", &s_attitude, kAttitudeNames, kAttitudeCount);
		ImGui::EndDisabled();

		ImGui::Spacing();

		// Patrol waypoints (preview — not persisted yet).
		ImGui::Text("Patrol waypoints: %d", 0);
		ImGui::BeginDisabled(true);
		ImGui::Button("Add waypoint (click map)");
		ImGui::SameLine();
		ImGui::Button("Clear path");
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::TextDisabled(
			"Next slice: editor-side order/waypoint persistence + an undo "
			"action, then bind to the brain-dispatch order delivery.");
	}

	ImGui::End();
}
