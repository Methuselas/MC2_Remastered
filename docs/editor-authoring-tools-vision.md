# Editor authoring-tools vision (objectives + campaign + layout)

Designer-provided redesign briefs for the MC2 map editor. The editor currently
exposes the **raw engine data model** in big modal dialogs; the goal is to turn
each into a **task-oriented authoring tool** (think UE/Unity). Feasibility already
reconned — see memory `authoring-tools-feasibility-objectives-campaign.md` and
`editor-crash-hardening-and-convolution.md` for data-model maps + file:line.

**Status (2026-06-27):** Phase-1 read-only explainer panels SHIPPED for BOTH
objectives + campaign (commit `ce3666ac`): toolbar buttons "Objectives" /
"Campaign" → `editor/ObjectivesSummaryPanel.*` / `editor/CampaignSummaryPanel.*`.
(Buttons may need a clean editor restart to appear — verify first.) Everything
below Phase 1 is NOT built.

---

## A. Objectives → mission-logic editor

**Core idea:** stop dumping the raw objective struct (title/desc/priority/flags/
conditions/actions/markers) in one modal. Make the author think:
*what should the player do? how does the game know? what happens next? what fails
it? where do they see it?*

**Three-pane workflow:** Objective List | readable WHEN/THEN flow | Inspector.
Each objective reads like a small script:
```
Objective: Investigate Abandoned Airfield   [Primary] [Visible] [Has Marker]
Activation: active from mission start
Completion  WHEN any friendly unit enters Airfield Area (x3136,y-789,r256)
            THEN set flag 0 = 1; mark complete
Failure:    none
```

**Data model:** `editor/Objective.h/.cpp` (EDITOR copy, NOT engine
`code/objective.*`). 23 condition types + 6 action types (closed enums) with
`Description()`+`InstanceDescription()` already producing readable text. Access via
`EditorData::instance->TeamsRef().TeamRef(t).ObjectivesRef()`. Object-ref condition
types (unit/structure) deref live ptrs unguarded → render label-only.

**Staged plan:** 1 read-only explainer ✅ → 2 list+badges+3-pane → 3 viewport-pick
"locate" (reuse the GPU pick spine) → 4 inline scalar edit (FIRST write; gate on
`Save→Read→operator==` golden test, operators exist) → 5 template wizard
("Move to area / Destroy object / Set hidden trigger / …" → generate conditions via
`new_CObjectiveCondition/Action` factories; needs additive public area setters +
editor-side `_REMOVE_STRUCTURE`) → 6 full block editor + viewport write.

**Key UX wins:** templates (don't start blank), readable WHEN/THEN blocks with
named areas/flags, viewport-pick for areas/markers, validation warnings (no success
condition / hidden+marker / unnamed flag / etc.), designer terminology
(Priority→Objective Type, Activate On Flag→Starts when flag is set, Objective
Success→Completion, Logistics Model→Marker display model, …).

---

## B. Campaign → campaign flow editor

**Core idea:** the campaign editor is a raw file editor; make the
`Campaign → Operations → Missions → Finale` structure obvious as a visual flow.

**Three-pane:** Structure tree | flow cards (Campaign → Operation cards → Finale) |
Inspector for the selected item. Operation card:
```
Operation 1: Beachhead                        [warning: unnamed]
Progression: complete 1 of 1 missions to advance
Briefing cinema1.bik | Op file MCL_CM_op1_1 | Music 22
Missions: [mc2_01]
```

**Data model:** `editor/campaignData.h` `CCampaignData`/`CGroupData`/`CMissionData`
(editor-local, MFC-free data; FitIni round-trip `CampaignData.cpp` Read:225/Save:193;
Save rewrites whole file → drops comments). EList Append/Insert/Delete = clean
add/remove/reorder.

**Staged plan:** 1 read-only summary ✅ → 2 tree/cards+badges → 3 inspector editing
(FIRST write; whole-file-save caveat; don't run new panel + legacy modal on the same
data) → 4 wizards (New Campaign / Add Operation / Add Mission via default-constructed
objects) → 5 flow graph.

**Cheap validations (data-only):** op with 0 missions, NumToComplete>count, empty
label, STANDIN placeholder video, empty mission filename, 0 operations.

**Designer terminology:** Group→Operation, Pre-Video→Briefing Video, Video→Debrief/
Operation Video, Tune→Music Track, "Number of missions required to complete"→
"Missions Required to Advance" (+ live plain-English "may skip N" / "all required").

---

## C. Overall editor layout / IA

**Diagnosis:** the toolbar is a developer toolbox dump — primary authoring tools
mixed with ~25 render-debug overlays; tool settings always visible; panels rendered
as same-weight buttons instead of windows.

**Target (5 workspaces / modes):** Select · Terrain · Place · Mission · Playtest,
with Debug/Analysis collapsed by default. Layout: top toolbar (File/Edit/View/Tools/
Build/Playtest + quick actions) · LEFT dock (Outliner / Content / Mission tabs) ·
center viewport + mode toolbar · RIGHT dock = **contextual** Inspector/Tool-Settings
(only the active tool's controls) · bottom dock (Output/Validation/Telemetry/Results).
The ~25 render-debug tools (GBuffer/Draw Packets/Shadow Pass/Render Explain/Mech
Debug/Telemetry/…) → a Debug menu/workspace, hidden by default.

**User's refinement:** RIGHT dock = contextual tool settings (as above). LEFT dock =
a NEW "current object info + manipulation" panel. Plus a viewport right-click action
menu (Delete / Move / Assign to group/team).

**KEYSTONE / blockers (from convolution recon):**
- The editor has **4 sources of truth for "active tool"** (`curBrush`,
  `currentBrushID`, `currentBrushMenuID`, painting/selecting/dragging bools). S1
  (`setActiveBrush()` single source) SHIPPED `ce3666ac` — this unblocks a reliable
  contextual UI (the earlier contextual-panel attempt failed precisely because the
  active tool couldn't be reliably read).
- **Viewport docking is gated** on a projection-unification slice: migrate the ~4
  legacy `projectZ` object sites (`EditorObjectMgr.cpp:1322/1675/1692/1710`) to GL
  `projectForScreenXY` so object projection matches the rendered scene; until then a
  center-pane viewport (origin-offset) drifts the pick. Side PANELS that don't move
  the viewport are safe NOW. See `docs/editor-game-projection-divergence-recon.md`.
- `EditorInterface` is a 6934-line god-class. Simplification stages S0-S5 in
  `editor-crash-hardening-and-convolution.md`.

**Phased rollout:** Phase 1 = pure regrouping (workspaces/tabs, move debug tools to a
Debug menu, contextual tool settings, panels→window toggles) — biggest win, no
viewport move. Phase 2 = mode toolbar + command palette + selection inspector.
Phase 3 = dockable layouts (after the projection-unification slice).

---

## Pointers
- Panel pattern to copy: `editor/InspectorPanel.{h,cpp}`, `editor/MapGeneratorDialog.*`;
  register Draw()+button in `EditorInterface::renderToolbarImGui()`.
- Pick spine (for viewport-pick): `EditorPickObjectAtScreen()` (GPU ObjectID),
  `editor/EditorInterface.cpp` ~:4164.
- Crash/convolution inventory + remaining Pass-2 crashes:
  memory `editor-crash-hardening-and-convolution.md`.
