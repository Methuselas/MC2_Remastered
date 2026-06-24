#ifndef UNIT_BRAIN_PANEL_H
#define UNIT_BRAIN_PANEL_H
/***************************************************************
* FILENAME: UnitBrainPanel.h
* DESCRIPTION: Editor "AI / Brain / Orders" inspector panel.
*   Visualizes WHAT the currently-selected unit does: its AI brain script,
*   pilot, lance/squad, and a scaffolded Orders & Stance section (patrol
*   waypoints, attitude/ROE). Read-only display of what EXISTS today; the
*   order/waypoint/stance EDITING controls are present as "this is where this
*   goes" scaffolding, disabled until editor-side order persistence lands and
*   the in-flight TECHSCRIPT brain dispatch (MC2_BRAIN_DISPATCH*, default-OFF)
*   defines the runtime order delivery + precedence.
*   Mirrors the InspectorPanel pattern (own ImGui window, selection-driven,
*   safe with no map / no selection). Does NOT touch save/load or PacketFile.
* DATE: 2026-06-24
****************************************************************/

class UnitBrainPanel
{
public:
	// Panel visibility (own ImGui window; safe with no map / no selection).
	static void Open();
	static void Close();
	static void Toggle();
	static bool IsOpen();

	// Draw the floating panel each frame from renderToolbarImGui().
	// No-op cheaply when closed.
	static void Draw();
};

#endif // UNIT_BRAIN_PANEL_H
