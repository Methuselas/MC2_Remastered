# MC2 Editor Superpowers Roadmap

**Status:** Strategy / design doc, 2026-06-11.
**Scope:** Modern ImGui panels layered over the working XP-era MFC editor (`editor/`, target `EditRel` → "Mission Editor.exe") without replacing its core. The MFC editor remains the authoritative mission authoring tool.
**Siblings:** `telemetry-oracle-cockpit-architecture.md` (telemetry schema + cockpit the Telemetry panel consumes), `mod-packaging-deploy-architecture.md` (mod project layout the editor loads/launches against), `runtime-bridge-architecture.md` (process model the Playtest Launcher rides on), `mc2-modding-toolchain-architecture.md` (tool ownership).
**Recon ground truth:** `.claude/editor-modernization-recon.md`, `.claude/ue-like-editor-recon-1.md`, `.claude/editor-modder-roadmap-recon.md` (repo root). This doc consolidates those into a committed roadmap.

---

## 1. North star

> **The MFC editor stays the engine of authoring — selection, brushes, undo, save/load, dialogs. ImGui panels are *views and remotes*: they read legacy structs directly each frame and mutate only through the existing Action/undo and menu-command paths. A panel that disappears must leave the editor exactly as functional as before it existed.**

Load-bearing consequences:

1. **Panels query; edits go through Actions.** Every mutation routes through `ActionUndoMgr` (`editor/Action.h:190`) or `handleNewMenuMessage(nID)` (`editor/EditorInterface.cpp:1166`) so MFC menus, hotkeys, and ImGui stay one coherent undo timeline. (Rule already stated in `ue-like-editor-recon-1.md` §"Architecture note".)
2. **One frame, one hook.** All panels draw from `EditorInterface::renderToolbarImGui()` (`editor/EditorInterface.cpp:4705`, called from `EditorGameOS.cpp:622` between `GuiRuntime::NewFrame()` and `GuiRuntime::Render()`). Auto-dock (`MC2_EDITOR_AUTODOCK`, `GuiRuntime::AutoDockActive()`) places them in the fixed right column; panels gate their own `SetNextWindowPos` behind `!AutoDockActive()` (pattern: `MissionValidation.cpp:477`, `EditorDebugOverlay.cpp:420`). New panels copy the existing header convention ("Draw the floating panel each frame from renderToolbarImGui()" — see `SceneOutliner.h:35`, `InspectorPanel.h:24`).
3. **No second source of truth.** Panels bind to live engine/editor structs (`EditorObjectMgr`, `EditorData`, `Terrain`, `MissionValidator` results, cockpit artifacts on disk). No mirrored object model, no panel-side caches that can go stale across undo.
4. **The editor never judges engine runtime health.** Playtest + Telemetry panels are pure consumers of artifacts/events defined in `telemetry-oracle-cockpit-architecture.md`; pass/fail authority stays with `smoke_lib/gates.py` and the engine's own tags.
5. **Process work is async via `EditorTaskRunner`** (`editor/EditorTaskRunner.h`, `StartTask`/`PumpMainThread`) — never `system()` (the MapGeneratorDialog freeze was the founding bug; see modernization recon §3.3). GL and editor structs are main-thread only; worker output applies on the pump.

---

## 2. Current editor substrate map (what already exists — verified in worktree)

Frame pump: `EditorMFCApp::OnIdle` → `EditorInterface::OnPaint` → `RunGameOSLogic` (`editor/EditorGameOS.cpp`): scene render → `GuiRuntime::NewFrame()` → `renderToolbarImGui()` (622) → `GuiRuntime::Render()` → swap. Pick divergence resolved by `MC2_EDITOR_RTT` default OFF (full-window scene = exact picking); collapsible panel column toggled by backtick.

| Substrate | Where | State |
|---|---|---|
| ImGui runtime + autodock | `GuiRuntime/` lib; `MC2_EDITOR_AUTODOCK`; editor CMake FATAL if `MC2_IMGUI=OFF` | SHIPPED |
| Panel set v0 | `SceneOutliner.{h,cpp}`, `InspectorPanel.{h,cpp}`, `AssetBrowser.{h,cpp}`, `MissionValidation.{h,cpp}` (8 checks, `ValidateForPakSave`), `EditorDebugOverlay` (chunk/superchunk grids), `CommandPalette.{h,cpp}`, `GameplayDebugger.{h,cpp}`, `UndoHistoryPanel.{h,cpp}`, `ModPicker.{h,cpp}`, `MapGeneratorDialog` | SHIPPED (varying depth) |
| Selection | `EditorObjectMgr::selectedObjects` (`EditorObjectMgr.h:300`), `select(EditorObject&,bool)` (cpp:1724), picker `getObjectAtScreenPosition` (cpp:830), forests via `selectForest(long)` | SHIPPED |
| Undo | `Action` base + per-brush `*Action` classes; `ActionUndoMgr::instance` (`Action.h:190`); `ModifyBuildingAction` whole-object snapshot | SHIPPED (gaps: forests, objectives, teams) |
| Brushes | `Brush.h:18` pattern, 15 concrete brushes, `curBrush` in `EditorInterface.h:272` | SHIPPED — do not touch |
| Async tasks | `EditorTaskRunner` (subprocess + `PROGRESS pct stage msg` parse + main-thread pump) | SHIPPED |
| Watchdog / GPU timers | `editor/EditorWatchdog.*` (`MC2_EDITOR_WATCHDOG=1`, DbgHelp stackwalk on >120ms stall), `EditorGpuTimer` (`MC2_EDITOR_GPU_TIMERS=1`) | SHIPPED, env-gated |
| Headless smoke | CLI flags `-gen-map`/`-smoke-*`/`-frames` (`EditorMFC.cpp:206-234`), `scripts/run_editor_smoke.py`, `[ESMOKE v1]` facts | SHIPPED |
| Mission data | `EditorData` (teams/players/objectives/save), packets 0=terrain 3=tacmap 4=MOVE, oversized-map MOVE synthesis + bounds-guarded accessors | SHIPPED |
| Asset catalog | `EditorObjectMgr::init()` unified catalog (BLDG/MECH/GV/TREED), `tgaName` thumbnails loaded but unshown (`EditorObjectMgr.cpp:516`, getter `.h:372`) | DATA SHIPPED, UI THIN |
| Mod overlay | `mclib/file.cpp` `MC2_ACTIVE_MOD` + `g_modIndex`; `ModPicker` panel | SHIPPED |

So "Superpowers" = **deepen and connect existing panels + add two new ones (Playtest, Telemetry)** — not greenfield.

---

## 3. Panel catalog

Each panel: what it WRAPS (never rewrites), data source, MVP scope, risk.

### 3.1 Object Inspector (deepen `InspectorPanel`)
- **Wraps:** `ModifyBuildingAction` snapshot path + `EditorObjectMgr` selection; the existing MFC property dialogs remain the deep editors.
- **Data source:** live `selectedObjects` walk each frame; fields per the editable-field matrix (`ue-like-editor-recon-1.md` §3): position/rotation, team/alignment, variant/chassis, pilot, damage, squad, paint — all serialized + undo-covered today.
- **Interaction model:** read-only display always; edit widgets only for v1-safe fields, each commit = one `ModifyBuildingAction` push. Multi-select shows common fields. "Open full dialog…" button fires the existing MFC modal via `handleNewMenuMessage`.
- **MVP:** selected object name/type/team/pos/rot read-only + editable team & rotation with undo. **Risk: LOW-MED** — only hazard is bypassing Actions (banned) or editing fields with no undo (objective links, AI brain — excluded until Phase 4).

### 3.2 Terrain / Material Inspector (new panel, wraps existing terrain state)
- **Wraps:** `Terrain` globals + chunk renderer state (`gos_terrain_indirect` accessors, e.g. `SolidWindowStagingData()`), `EditorDebugOverlay` toggles, `MapGeneratorDialog`/`terrain_gen.py` recipe pipeline. Does NOT touch TerrainBrush/OverlayBrush.
- **Data source:** read-only — map dims (`realVerticesMapSide`), cell under cursor (terrain type/overlay/passability via bounds-guarded `MissionMap` accessors), colormap/burnin source + resolution (`docs/asset-pipeline.md` cook state), splat/texture-array bindings, chunk-LOD telemetry (`[TerrainLOD prod]` objBlocks counts).
- **Interaction model:** hover-readout (cursor cell → terrain/overlay/passable), toggles delegating to `EditorDebugOverlay` and the existing passability overlay menu item, "Regenerate…" delegating to MapGeneratorDialog's TaskRunner path.
- **MVP:** map stats + cursor-cell readout + overlay toggles. **Risk: LOW** read-only; MED only if it later writes terrain (must go through TerrainBrush actions, not direct vertex pokes).

### 3.3 Mission Validation Panel (extend `MissionValidation`)
- **Wraps:** `MissionValidator::ValidateForPakSave` (`MissionValidation.cpp:23`) — already a live Blocking/Warning/Info panel.
- **Data source:** in-memory `EditorObjectMgr` + `EditorData`; re-run on dirty-flag change or button.
- **Interaction model:** click result → select offending object (existing `select()`) + frame camera (slice 1 below); auto-fix buttons (snap-to-bounds, clamp alignment) each emitting an Action.
- **MVP:** add the 5 recon-identified checks (out-of-bounds object, missing appearance nullptr [BLOCKING — saveMechs crash], alignment>7 clamp, broken objective refs, duplicate IDs); promote the save-time MFC modal warnings (no enemy/player units, >104 pilots, objectives-without-conditions) into the live panel per `editor-modder-roadmap-recon.md` §5.J. **Risk: LOW** — append-only checks; auto-fixes MED (must be Actions).

### 3.4 Playtest Launcher (new panel)
- **Wraps:** `EditorTaskRunner` subprocess machinery + mc2.exe's existing `-mission <pak>` CLI (game side, `code/mission.cpp:~2295`) + `MC2_ACTIVE_MOD` env from `ModPicker` selection. Process model per `runtime-bridge-architecture.md`.
- **Data source:** current `EditorData` mission path + dirty flag; active mod id; deploy roots — **respect the v0.4 (game) vs 0.4c (editor) split** (handoff trap: building/deploying editor does NOT update game exe).
- **Interaction model:** one button: prompt-save if dirty → `StartTask("playtest", "mc2.exe -mission <pak>", gameDeployDir, ...)` with `MC2_ACTIVE_MOD` set; live status from task; on exit, hand the session log path to the Telemetry panel. Stop button = task cancel. Refuse concurrent launches and warn if a smoke run holds the lock (run_smoke concurrency rule).
- **MVP:** launch + exit-code report + "open log". **Risk: MED** — process lifecycle (orphaned mc2.exe is a known trap), stale-exe confusion (mitigate: show game-exe mtime vs current build in the panel), never taskkill blindly.

### 3.5 Telemetry Panel (new panel — the cockpit's editor view)
- **Wraps:** the artifact schema in `telemetry-oracle-cockpit-architecture.md`: per-run `telemetry.ndjson` + `report.json` + tag registry (`tests/telemetry/tag-registry.json`). Pure consumer — never computes a counter the engine could emit, never alters smoke verdicts.
- **Data source:** v1 = files: tail the log/NDJSON of the most recent Playtest session (and `tests/smoke/artifacts/<timestamp>/` runs). Later = live lane over the bridge stdout protocol, same grammar.
- **Interaction model:** session selector → oracle table (mismatch counters, expected 0, red if not), counter sparklines (fps, destroys), event feed with `raw_line` jump-to-log. "Open in cockpit" defers anything heavier.
- **MVP:** tail current playtest log, lift bracketed `[TAG vN] key=value` lines via the same lifter as `smoke_lib/logparse.py` (shared code, not a reimplementation), show oracle pass/fail chips. **Risk: LOW** read-only; the only real risk is schema drift — mitigated by depending on the lifter library, not parsing in the panel.

### 3.6 Asset Placement Browser (deepen `AssetBrowser`)
- **Wraps:** `EditorObjectMgr` unified catalog + `selectBuildingObject` → `BuildingBrush` (placement and undo already correct: `BuildingAction`, `EditorInterface.cpp:1821`) + `ObjectRecentRing` MRU.
- **Data source:** catalog groups/names (`getBuildingNamesInGroup`), thumbnails from `getTGAFileName()` (`EditorObjectMgr.h:372` — loaded at cpp:516, currently unshown), active-mod overlay so mod-added assets appear (mod data shadows base via `g_modIndex`).
- **Interaction model:** filter box + category tree + thumbnail grid (`ImGui::Image`); click = arm BuildingBrush exactly as the menu does; MRU strip on top.
- **MVP:** thumbnails + filter over existing list UI. **Risk: LOW** — placement path untouched; only cost is TGA→GL texture upload lifetime management (upload lazily, cache by name, main thread only).

### 3.7 Script / Event (ABL) Browser (new panel, read-only first)
- **Wraps:** ABL module references already serialized on units (AI Brain, `ue-like-editor-recon-1.md` §3 — "serialized, no UI at all") + objective condition/action tree (`Objective.h`, 25 condition species) + `.abl` files on disk (`data/missions/*.abl`, mod overlay aware).
- **Data source:** per-unit brain name from `EditorObject`/Unit fields; mission objective tree from `EditorData::TeamsRef`; file list via the same resolution order the engine uses (`mclib/file.cpp` overlay).
- **Interaction model:** v1 read-only: list brains in use, which units reference them, missing-file flags (feeds a Validation check); click → open file in external editor (ShellExecute — do NOT build a text editor). Objective tree shown read-only. Editing objectives/brains stays in the MFC dialogs until objective undo exists (Phase 4 of modder recon — HIGH risk, deliberately last).
- **MVP:** brain inventory + missing-script validation. **Risk: LOW read-only / HIGH if it ever writes** — objective serialization is fragile; writing is out of scope for this roadmap's horizon.

---

## 4. Shared infrastructure (build once, panels reuse)

1. **Selection service (formalize, don't invent):** thin facade over `EditorObjectMgr::select`/`selectedObjects`/`selectForest` + a new **frame-camera** helper wrapping `Camera::setGoalPosition` (`camera.h:1374`; TODO already marked at `SceneOutliner.cpp:353`). Consumers: Outliner, Inspector, Validation click-to-select, ABL browser.
2. **Undo integration rule + history surface:** all panel mutations construct existing `*Action` subclasses; add the two read-only methods (`GetActionCount()`, `GetActionDescription(int)`) to `ActionUndoMgr` that `UndoHistoryPanel` needs (display-only). New Action subclasses only for currently-uncovered ops (forest create — easy win #9 in ue recon).
3. **Data binding to legacy structs:** convention, not framework — panels read structs live each frame (frame-valid pointers only; no retained `EditorObject*` across frames — store IDs/WIDs and re-resolve). A tiny `PropRow(label, getter, optional setter→Action)` helper unifies Inspector/Terrain panel rendering. No reflection layer, no mirrored model.
4. **Command dispatch:** the `EditorCommand{id,label,hotkey,mfc_id,callback}` table from ue recon §6 — `CommandPalette` exists; finish populating it so panels invoke commands by id (preserving undo via `handleNewMenuMessage`) instead of duplicating switch cases.
5. **Task/process layer:** `EditorTaskRunner` as-is; Playtest adds a "single foreground game session" slot + env injection (`MC2_ACTIVE_MOD`, telemetry env gates).
6. **Telemetry lifter library:** shared line→NDJSON lift (extension of `smoke_lib/logparse.py` per cockpit doc §3) callable from the editor panel — one grammar, no duplicate parsers.
7. **Overlay draw discipline:** world-space overlays draw in `EditorInterface::render()` (~2565) and MUST set+restore depth/blend/cull explicitly (the terrain transparency saga lesson; FoliageRender is the template).

---

## 5. What NOT to rewrite

- **MFC dialogs that work** (unit settings, objective editor, team setup, file dialogs): panels link to them; mass MFC→ImGui migration is explicitly banned (modder recon §7).
- **Brush tools** (15 brushes, `Brush.h` pattern) — the placement/paint interaction core. Panels *arm* brushes, never reimplement painting.
- **Save/load** (`EditorData::save`, PacketFile packets, FitIniFile blocks): no binary-format changes; additive `.fit` blocks only.
- **Undo manager:** extend with read-only getters + new Action subclasses; never a parallel undo stack.
- **The MFC frame/message pump** (`EditorMFCApp::OnIdle`, `MainFrame`): it just got watchdog-verified and perf-fixed; it stays.
- **Picking/projection:** full-window picking is exact with RTT off — do not reintroduce an RTT scene rect (unsolved D3D↔GL pick split-brain documented in handoff).
- **Gizmo system:** drag-move + bracket-rotate stay; gizmo rewrite deferred indefinitely.

## 6. Anti-goals

- No editor-side game simulation / PIE-in-viewport (see `pie-feasibility-recon.md`; Playtest = launch real mc2.exe).
- No visual scripting graph; no objective/trigger builder until objective undo exists.
- No second asset database, no panel-side mirrored object model, no nlohmann in arbitrary editor TUs (hand-parse like FoliageRender).
- No threads touching GL or editor structs; no `system()` calls.
- No renderer changes ridden in on panel work; no smoke-verdict logic in the editor.
- No ImGui ini-persistence layout wars — autodock owns layout.

---

## 7. Phased roadmap

- **Phase A — Connect what exists (read-only deepening):** frame-camera on select; Validation new checks + click-to-select; Inspector read-only completeness; Terrain Inspector MVP; AssetBrowser thumbnails. Exit: every existing panel binds live data and selection round-trips Outliner↔viewport↔Validation.
- **Phase B — Edit safely (Action-routed mutation):** Inspector editable v1-safe fields; Validation auto-fixes; forest-create undo; UndoHistoryPanel getters; command table completion. Exit: `-smoke-inspector-edit` mutate+undo+assert-revert green.
- **Phase C — Run loop (process + telemetry):** Playtest Launcher MVP; Telemetry panel file-lane MVP (shared lifter); ModPicker→Playtest env wiring per mod-packaging doc. Exit: one click from edited mission to running game to oracle chips.
- **Phase D — Authoring depth (gated):** ABL browser write-side, objective/team undo, asset-browser mod-publishing hooks — each gated on the undo coverage and packaging milestones, per modder recon Phase 4 warnings.

## 8. First 5 implementation slices

1. **Frame-camera + Validation click-to-select** — wrap `Camera::setGoalPosition` behind a `FocusCameraOn(EditorObject&)` helper; call from `SceneOutliner.cpp:353` TODO and Validation results. Smoke: `-smoke-outliner` extended fact.
2. **Validator hardening** — add missing-appearance (BLOCKING), out-of-bounds, alignment-clamp, duplicate-ID, broken-objective-ref checks to `ValidateForPakSave`; promote save-time modal warnings into the panel.
3. **AssetBrowser thumbnails** — lazy TGA→GL cache keyed by `getTGAFileName()`; `ImGui::Image` grid in `AssetBrowser::Draw`. Manual visual check (smoke can only assert no-crash).
4. **Inspector editable team+rotation** — two widgets committing `ModifyBuildingAction`; smoke `-smoke-inspector-edit` (edit → undo → assert original value).
5. **Playtest Launcher MVP** — `EditorTaskRunner` launch of game-deploy `mc2.exe -mission <pak>` with `MC2_ACTIVE_MOD`; dirty-save prompt; exe-mtime staleness warning; exit-code + log-path report. Telemetry panel consumes that log in the next slice.

Each slice: editor build via worktree `build64`, deploy via EditRel to 0.4c, run via `run-editor.bat` (captures stderr), `run_editor_smoke.py` green, tier1 untouched.

## 9. Follow-up prompts (for Opus/Codex)

1. *"In worktree `.claude/worktrees/nifty-mendeleev`, implement roadmap slice 1 of `docs/superpowers/strategy/editor-superpowers-roadmap.md`: a `FocusCameraOn(EditorObject&)` helper wrapping `Camera::setGoalPosition` (camera.h:1374), called from the SceneOutliner double-click TODO (SceneOutliner.cpp:353) and from MissionValidation result rows (with `EditorObjectMgr::select` first). Extend `-smoke-outliner` to emit a `[ESMOKE]` fact proving the camera goal moved. No brush, save, or renderer changes."*
2. *"Design the editor-side Telemetry panel per `editor-superpowers-roadmap.md` §3.5 and `telemetry-oracle-cockpit-architecture.md` §3: factor the bracketed-tag line lifter out of `scripts/smoke_lib/logparse.py` into a form a C++ panel can reuse (decide: tiny C++ port vs sidecar process via EditorTaskRunner), define the tag-registry read path, and spec the oracle-chip UI. Recommend one option with tradeoffs before any code."*
3. *"Adversarially review the Playtest Launcher MVP plan (`editor-superpowers-roadmap.md` §3.4 + slice 5) against the known traps: v0.4 game vs 0.4c editor deploy split, stale-exe mtime, orphaned mc2.exe on TaskStop, run_smoke concurrency lock, and `MC2_ACTIVE_MOD` env scoping. Produce a failure-mode table and the minimal guard set before implementation."*
