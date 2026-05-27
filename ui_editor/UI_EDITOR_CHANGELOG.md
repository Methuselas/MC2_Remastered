# UI Editor Changelog

## 0.5.3z - 2026-05-25

### Added
- Added 1024x1024 cursor atlas support for generated `legacy_imgui` shared cursor pages.
- Added page-level cursor atlas metadata (`cursorAtlasPixels`, `legacyCursorAtlasPixels`, `cursorAtlasUvScale`, `cursorDisplayScale`) for `cursors.fit` and `cursorsa.fit`.

### Changed
- Updated `data/art/gui/Cursors*.png` and `data/art/gui/Cursors*a.png` to 1K atlas assets while preserving legacy cursor display sizes.
- Scaled generated cursor UV fields from legacy 128px atlas coordinates to 1024px atlas coordinates and preserved the original legacy UVs for traceability.
- Updated the legacy FIT converter so regenerated cursor catalog pages keep 1K atlas UV support.

### Notes
- Cursor display rects and hotspots remain legacy-sized; only the atlas resolution and UV coordinates were modernized.

## 0.5.3y - 2026-05-25

### Fixed
- Restored Viewer button-state rendering for shared `legacy_imgui` Mechlopedia pages by reading generated pressed/highlight/disabled UV fields instead of tinting all category art orange.
- Added Viewer click-edge handling independent of ImGui's helper click state so category/list/scrollbar buttons can respond reliably through the existing game input bridge.
- Added selected-category flash preview for legacy `AnimateBmp` buttons without forking a separate Viewer UI package.

### Notes
- Viewer still uses the shared `legacy_imgui` package. This patch strengthens the Viewer adapter instead of creating a separate Viewer UI set.

## 0.5.3x - 2026-05-25

### Fixed
- Restored Viewer art rendering for shared generated `legacy_imgui` pages by reading modern scalar UV fields (`uvX`, `uvY`, `uvWidth`, `uvHeight`) in addition to legacy `uvPixels`.
- Made canvas hit testing prefer editable button text rectangles before decorative art slices, so clicking the label area selects the button text layer instead of nearby chrome.
- Tightened Project / Layouts filtering so root-level legacy/provenance folders are excluded, not only nested `/legacy/` paths.

### Notes
- Viewer still consumes the `legacy_imgui` package; this patch fixes the generated-page reader rather than forking a separate Viewer UI package.
- Full Windows Viewer launch validation still needs local testing.

## 0.5.3w - 2026-05-25

### Fixed
- Restored Viewer package loading by trying the package-local Viewer manifest first, following `GuiRedirect` targets, and honoring a root page key when the loaded page key belongs to the shared Game Mechlopedia source.
- Recovered Viewer Mechlopedia category/close behavior when the shared generated Game page exposes generic button roles by falling back to legacy help-string IDs.
- Fixed UI Editor text-rect loading for modern generated `textRect` fields, not only legacy `textX/textY/textWidth/textHeight` fields.
- Fixed Save Copy text-rect persistence so edited button text rects update `textRect`, `textOffset`, and the scalar text rect fields together.
- Made selected button text rects draggable even when decorative art slices overlap and win the normal canvas click target.
- Filtered the Project / Layouts browser so it lists actual UI layout FITs instead of manifests, package metadata, define files, font catalogs, redirects, audit files, and provenance/source folders.

### Notes
- This keeps `legacy_imgui` as the current default package while avoiding Viewer assumptions that every surface can use the Engine page without adapter logic.
- No UI art moved; `data/art/gui` remains the UI graphics root.


## 0.5.3v - 2026-05-25

### Changed
- Consolidated the active default UI package so Game, Viewer, Editor, Mechlopedia, and Shared generated UI now live under `data/defs/ui/packages/legacy_imgui/`.
- Updated `legacy_imgui/package.fit` so each surface points at package-local page roots and manifests.
- Updated the legacy conversion tool to regenerate Game/Shared/Editor/Viewer pages directly into the package root instead of loose top-level UI folders.
- Updated generated scrollbar template references to use `data/defs/ui/packages/legacy_imgui/shared/scrollbar.fit`.
- Updated UI Editor default load path and Mechlopedia shortcuts to use the package-local Mechlopedia route.
- Updated UI Editor and Viewer font catalog lookup to prefer `data/defs/ui/packages/legacy_imgui/shared/fonts.fit`.
- Added `tools/gui_fit/cleanup_legacy_imgui_package_move.py` for drop-in users who need to remove old loose generated folders after extracting.

### Removed
- Removed loose generated UI folders from `data/defs/ui/`: `game`, `editor`, `viewer`, `shared`, `mechlopedia`, and `fonts`.

### Notes
- `data/defs/ui/legacy/` remains as source/provenance input for conversion.
- No UI art was moved; UI graphics remain in `data/art/gui`.
- This is a folder/package consolidation pass, not a new visual parity fix.


## 0.5.3u - 2026-05-25

### Added
- Added `data/defs/ui/packages/default_ui_package.fit` and `data/defs/ui/packages/legacy_imgui/package.fit` so the GameOS-derived ImGui UI is explicitly registered as the current default shippable UI package.
- Added package define files for modern rect fields, UI element semantics, action taxonomy, and font binding rules under `data/defs/ui/packages/legacy_imgui/defines/`.
- Added modern rect fields to regenerated legacy ImGui FIT output: `controlRect`, `artRect`, `hitRect`, `textRect`, `textOffset`, `rectModel`, and `uiPackage`.
- Added a composite button model marker for migrated buttons: `composite_hit_art_text_v1`.

### Changed
- Updated the legacy conversion tool so future regenerations keep the new package-facing rect model instead of only emitting legacy raw/source rect fields.
- Kept UI graphics rooted at the existing `data/art/gui` path; this pass does not move art files.

### Notes
- This is a folder/definition/schema foundation pass, not a visual parity hotfix.
- No `mods/ui` folder is added yet. Future UI packages can live under `data/defs/ui/packages/` until the mod system is ready.
- The next visual pass should use these rect fields to finish the linked button/art/text editing model.


## 0.5.3t - 2026-05-25

### Fixed
- Reserved Shift+Left-drag for canvas rect marquee selection so it no longer creates or moves ruler guides.
- Disabled Snap to Grid by default and left guide snapping off by default until grid/guide preferences exist.
- Made selected button text rectangles independently draggable/resizable on the canvas so adjusting button text no longer moves the button border/art layer.

### Added
- Added group movement for multi-selected rects: marquee-select multiple cells, then drag one selected cell to move the whole selection together.
- Added one undoable group-rect history entry for multi-rect moves.
- Added visible text-rect handles for selected button/text-layer cells.

### Notes
- The proposed Photoshop-style left toolbar and Ctrl+G grouping model are intentionally not included here; those need a short UI design pass before implementation.
- This pass does not address main-screen asset parity, Viewer Mechlopedia runtime, or remaining legacy page composition fixes.

## 0.5.3s - 2026-05-25

### Fixed
- Removed the obsolete Simulation controls from the Smart Tools pane so the left panel stays focused on editing tools.
- Prevented left/right panel widths and top/bottom split heights from being permanently rewritten during window minimize or very small window sizes.
- Suppressed legacy `IDS_undefined_string_sebi` placeholder keys from normal text preview and help metadata display.
- Hid runtime help-text rectangles in the default canvas preview so default page truth does not show hover/help strings as static labels.
- Composited `mcl_mechinfo.fit` over `mcl_mdollar.fit` so the mech purchase/detail overlay previews in its runtime parent context.

### Added
- Added Copy Rect, Cut Rect/Hide, and Paste Rect commands for the selected cell.
- Added `Ctrl+C`, `Ctrl+X`, and `Ctrl+V` shortcuts for the rect clipboard.
- Added Shift+Left-drag marquee selection on the canvas for faster rect inspection and batch targeting.

### Notes
- This is a UI Editor cleanup/QOL pass. It does not claim final MechLab runtime data parity, final page-link wiring, or full Engine/Viewer ImGui hookup.


## 0.5.3r - 2026-05-25

### Fixed
- Matched legacy `aButton` text placement by defaulting missing button `Alignment` fields to centered text instead of left/top text.
- Preserved explicit legacy button alignment values while tagging default-centered buttons with the runtime source used by `aButton::init`.
- Expanded legacy string-symbol fallback parsing to include `#define IDS_* <id>` entries and `resource.h`/`Resource.h` scans, improving missing-string diagnostics without restoring `mc2res.dll` as authority.
- Marked shared Mechlopedia `Agency Bold` content styles as Bold so they resolve cleanly through the TTF font catalog.

### Added
- Added generated UI truth audit CSVs under `data/defs/ui/_audit/`:
  - `missing_strings.csv`
  - `button_text_alignment_audit.csv`
  - `font_usage_audit.csv`
  - `page_link_action_audit.csv`
- Added `--audit-ui-truth` to the legacy GameOS-to-ImGui FIT converter.

### Changed
- Regenerated legacy ImGui FIT outputs so Engine, Viewer, and UI Editor testing use the same cleaned `0.5.x` UI truth layer.

### Notes
- Stayed in the `0.5.x` lane. No v0.6 work, no Smart Tools expansion, and no engine hookup in this pass.

## 0.5.3q - 2026-05-25

### Fixed
- Removed the false Launch button and its art children from the generated `mcl_cm_layout.fit` campaign frame preview.
- Marked `mcl_mc.fit` jump-jet-only payload outline, jump-jet icons, jump-jet readout, and jump-jet meters as hidden runtime variants so the default Mech Lab preview no longer shows jumpjets in the bottom-left payload area.
- Regenerated mounted Mechlopedia and Options companion pages with source-local cell rectangles plus `GuiPage.mountOffset`, preventing companion mount offsets from being baked into cells and then applied again during composite preview.

### Notes
- No Smart Tools work in this pass; this is a legacy runtime truth-viewer correction.

## v0.5.3p — Loading / High-Res HUD / Text Rect Truth Pass

### Fixed
- `mcl_loadingscreen*.fit` no longer uses transition animation keyframes as static placement, so loading-screen pieces stop stacking or jumping off-screen.
- `mcl_sp.fit` still uses animation timeline frame-zero placement where the legacy timeline is the actual source of tile placement.
- `buttonlayout1280.fit` now generates and previews as 1280x1024.
- `buttonlayout1600.fit` now generates and previews as 1600x1200.
- High-res button layouts now apply legacy `HiresOffsets` the same way `ControlGui::swapResolutions()` places the runtime HUD.
- Options tab companion pages now composite over `mcl_options.fit` at the runtime tab-content mount.
- Button text without explicit text rectangles now previews centered in button-like controls.
- Button text rectangles now follow child-relative and high-res runtime offsets instead of drawing from stale raw source coordinates.
- Legacy text rectangles with no `TextID` are treated as blank/runtime-populated placeholders instead of false missing-text errors.

### Notes
- This is a legacy truth-viewer/conversion pass only; no Smart Tools or new authoring behavior was added.
- Full Windows CMake/link and live editor launch validation still need local confirmation.


## v0.5.3o — Runtime Component / Companion Page Truth Pass

### Fixed
- `buttonlayout1280.fit` and `buttonlayout1600.fit` now generate 1280x768 and 1600x768 canvases instead of being forced into 800x600 preview space.
- Mechlopedia companion FITs now generate at the GameOS runtime mount offset `(285,58)`.
- Options tab companion FITs now generate at the runtime tab-area mount offset from `mcl_options.fit` rect 2.
- Combo-box and drop-list component children now honor `ChildCoordinatesAreRelative` so edit boxes, expand buttons, and list boxes are not stacked at the component origin.
- Cursor catalogs now preview in a grid instead of stacking every cursor at `0,0`.
- `ScrollBar.fit` keeps its component preview size and its up/down/thumb pieces now render as atlas-backed parts instead of each part drawing a whole synthetic scrollbar.
- Font-resource string IDs such as `AgencyFB17.d3f` are suppressed as drawable text instead of appearing in legacy text boxes.
- Building Mechlopedia structure readout text is now marked as runtime-populated instead of showing the weapons-loadout template string.

### Added
- Added generated Mechlopedia runtime variant pages for Vehicles and History so the six GameOS Mechlopedia sub-screens are represented separately.
- Added redirects for `mechs`, `vehicles`, `personalities`, and `history` to the shared GameOS-generated Mechlopedia page family.
- Added preliminary multi-plane art cell emission for legacy `Element#/Art#` blocks so old compound art screens expose their source art planes instead of only generic layout rects.

### Notes
- Text alignment was intentionally left alone in this pass.
- This remains a legacy UI truth-viewer/conversion pass; no Smart Tools or new authoring features were added.


## v0.5.3n — Text Anchor / Font Type / Button Overlay Truth Pass

### Fixed
- Text anchors now resolve horizontal and vertical axes independently, so `middle_left`, `middle_right`, `top_middle`, and `bottom_middle` no longer collapse to the wrong X/Y position.
- Text pulse/flash now visibly affects normal text as well as button text.
- Font selection now honors the explicit font type/style field and uses catalog family/style matching before falling back to a base face.
- Bold and Italic checkboxes now update the editable `fontStyle` field so they can actually select Bold, Italic, or BoldItalic catalog faces when those TTFonts exist.
- Font loading now probes additional runtime font locations and records the resolved path used for loaded TTFonts.
- `mcui_mr_layout.fit` generated output now marks Mission Results runtime variants and hides non-default variants by default so inactive panes do not stack over the Salvage Area truth view.

### Added
- Added `fontStyle` / Font Type authoring and Save Copy persistence.
- Added button color overlay authoring with normal, pressed, hover/selected, and disabled overlay colors.
- Added editable button text-rect controls for the legacy `XTextLocation/YTextLocation/TextWidth/TextHeight` rectangle.
- Added generated button overlay color metadata from legacy button animation color fields.
- Added redirects so Viewer and shared Mechlopedia routes point at the generated GameOS Mechlopedia ImGui FITs instead of maintaining separate hand-made copies.

### Notes
- This is still a legacy UI truth-viewer/conversion pass.
- No Smart Tools expansion or 1080p authoring work was added.


## v0.5.3m — Legacy Button / Font State Truth Pass

### Fixed
- TTFont loading now probes executable-relative and repo-relative paths, so the editor can actually load `assets/graphics/fonts/ui/*.ttf` when launched from a build output folder.
- Legacy button previews now use state-specific atlas UVs for normal, hover/highlight, pressed, selected, and disabled states instead of always drawing the normal sprite.
- Legacy button text now draws inside the source `XTextLocation/YTextLocation/TextWidth/TextHeight` rect rather than the whole button/image rect.
- Legacy button text colors now use `NormalColor*`, `PressedColor*`, `HighlightColor*`, and `DisabledColor*` instead of a single fallback/default ImGui color.
- Button pulse/flash previews now affect hover/selected/pressed state color blending instead of making every animated button fade constantly.

### Added
- Generated ImGui FIT output now preserves legacy button text rects, state text colors, and state atlas UV metadata for truth-viewer parity.

### Notes
- This is still a legacy UI truth-viewer/conversion pass.
- No Smart Tools expansion or 1080p authoring work was added.


## v0.5.3l — Font Catalog / Flashing Text Truth Pass

### Fixed
- Text pulse/flash now affects the rendered text itself, not only button borders/fills.
- Text cells now draw with TTFonts loaded from `data/defs/ui/packages/legacy_imgui/shared/fonts.fit` instead of the single default ImGui font.
- Missing text IDs now include `strings.res.h` / `stringres.h` symbol diagnostics when the migrated String FIT text body is absent.

### Added
- Added runtime font catalog loading for the replacement UI TTFonts under `assets/graphics/fonts/ui/`.
- Added text effect/source fields to loaded cells so legacy `AnimateText`, flash, and pulse intent remains visible in the editor and Save Copy output.
- Added font catalog choices to the inspector font picker.

### Notes
- Legacy GameOS/MFC/Viewer FIT files remain source truth only.
- Generated/shared ImGui FIT files remain the editable/replacement target.
- No Smart Tools expansion or 1080p authoring work was added.


## v0.5.3k — Text Authoring / Wrap / Shared Mechlopedia Pass

### Fixed
- Text preview now honors `wrapText` and clips/wraps inside the text cell instead of drawing one unwrapped line.
- Text preview now scales using the cell `fontSize` field so legacy text fits closer to the original page geometry.
- Mission Selection now emits runtime-created description list preview text for the SitRep/list box region that GameOS fills from `MissionSelectionScreen::updateListBox()`.
- Deprecated the separate Viewer-specific `viewer/encyclopedia.fit` page and redirected it to the shared hand-authored `mechlopedia/main.fit`.

### Added
- Added text anchors for upper-left, top-middle, upper-right, middle-left, center, middle-right, lower-left, bottom-middle, and lower-right placement.
- Added inspector controls for multiline text placement, text key, alignment, anchor, word wrap, font name, font preset, font size, bold, italic, and font color.
- Added inspector texture editing for image cells so generated ImGui FIT pages can place/change images without editing legacy files directly.
- Added `GuiRedirect` loading support so compatibility files can point at the shared canonical UI page.

### Changed
- Save Copy now persists generated ImGui FIT text/image authoring fields in addition to rectangles.
- Converter now emits `textAlign`, `textAnchor`, `wrapText`, and `colorArgb` for converted legacy text controls.

### Notes
- Legacy GameOS/MFC/Viewer FIT files remain source truth only.
- Generated/shared ImGui FIT files remain the editable/replacement target.
- No Smart Tools expansion or 1080p authoring work was added.



## v0.5.3j — Mission Operation Runtime Text Truth Pass

### Fixed
- Stopped rendering the legacy mission operation `Text0` template `TextID` as real page text.
- Marked mission operation title text as a runtime binding driven by `MissionSelectionScreen::update()` and `LogisticsData::getMissionFriendlyName(...)`.
- Added preview mission title resolution from mission-number button comments, so pages such as `MCL_CM_Op1_2.fit` preview `Reacquisition: Base Gemini` instead of the dialog quit prompt.
- Normalized migrated text FIT rows that use `value` instead of `text` so editor/resource legacy string catalogs resolve consistently.

### Changed
- UI Editor loader and inspector now preserve runtime text binding/source diagnostics and legacy template text diagnostics.

### Notes
- Legacy GameOS/MFC/Viewer FIT files remain source truth only.
- Generated ImGui FIT files remain the editable/replacement target.
- No Smart Tools or new authoring features were added.


## v0.5.3i — Legacy Text / Scrollbar / 3D View Truth Pass

### Fixed
- Kept legacy `[Texts]` container/count sections as metadata instead of generating bogus 0,0 drawable text cells.
- Expanded text catalog discovery from the UI compatibility catalog and all current `data/defs/text/en_us` FIT sources, including Mechlopedia text FITs.
- Preserved and displayed resolved legacy `TextID` strings for generated `GuiText` and text-bearing controls.
- Marked the default Mechlopedia category button selected so the truth viewer reflects the legacy opening state.
- Converted legacy "walking mech" / "rotating picture" placeholder rects into explicit `Gui3DView` cells instead of mislabeled static/image cells.
- Added runtime-created `aListBox` / `mcScrollBar` preview cells for legacy listbox and scroll-box rects, including Mechlopedia list/detail scrollbars.

### Changed
- UI Editor canvas rendering now draws `GuiScrollbar` previews with visible top button, thumb, and bottom button.
- UI Editor canvas rendering now draws `Gui3DView` placeholders as 3D viewport regions.
- Loader now consumes generated selected/checked/disabled/pulse state fields for legacy truth-viewer previews.

### Notes
- Legacy GameOS/MFC/Viewer FIT files remain source truth only.
- Generated ImGui FIT files remain the editable/replacement target.
- No Smart Tools or new authoring features were added.


## 0.5.3h - 2026-05-24

### Fixed
- Regenerated all current legacy_imgui Game, Shared, Editor, and Viewer FIT outputs from the current converter instead of leaving older stale generated files mixed in.
- Treated `ScrollBar.fit` as a shared dynamic component template instead of a fake 800x600 page.
- Moved shared scrollbar bottom-button and thumb preview positions using the runtime component rules so the pieces no longer stack at the raw FIT origin.
- Preserved 1-pixel legacy line rectangles when `left == right` or `top == bottom`.

### Added
- Added `--convert-all-legacy` support to the legacy FIT converter.
- Added a generated FIT coverage audit helper for stale paths, remaining drawable legacy blocks, zero-size drawable cells, and likely stacked rect clusters.
- Added generated ImGui FITs for the current Editor and Viewer legacy source folders.

### Notes
- Legacy GameOS/MFC/Viewer FITs remain source truth only.
- UI Editor editing remains targeted at generated ImGui-format FITs, not the legacy source files.
- No Smart Tools or new authoring features were added.

## v0.5.3g — Animation Timeline Placement Truth Pass

- Fixed legacy animated UI conversion so GameOS animation timelines can provide the initial draw position.
- `mcl_sp.fit` no longer stacks every splash-screen tile at 0,0; `Stars#` timeline `Pos0X/Pos0Y` now drives the preview rect.
- Referenced animation timeline blocks are treated as source metadata for their owning `GuiAnimation` instead of separate drawable cells.
- Regenerated `data/defs/ui/packages/legacy_imgui/game/mcl_sp.fit` with `animation_pos0` rect sources and visible animation metadata.
- No new UI Editor authoring features; this remains legacy UI truth-viewer work.

## v0.5.3f — Legacy UI Truth Viewer Coverage Pass

- Expanded the legacy UI converter from Mechlopedia-only assumptions toward full GameOS/MFC/Viewer coverage.
- Converted legacy `left/right/top/bottom` rectangles into real ImGui `rect` and `legacyLocalRect` values instead of losing them as `0x0` layout blocks.
- Added meaningful generated roles/layers/aliases for images, rects, buttons, text, lists, scrollbars, edit boxes, meters, animations, metadata, and unknown legacy blocks.
- Added legacy text lookup against modern String FIT catalogs so `TextID`, `HelpCaption`, and `HelpDesc` can surface `textKey`, `visibleText`, help text, source, symbol, and missing-text diagnostics.
- Preserved legacy section/comment/context/control metadata into loaded UI Editor cells for diagnostics and composition matching.
- Corrected campaign mission companion mount priority so operation/planet/map pages prefer the MissionSelectionScreen map mount (`CMRect3`) before the left video/VID COM rect.
- Updated UI Editor version display to `0.5.3f`.
- No Smart Tools or new authoring features; this is truth-viewer/converter infrastructure only.

## v0.5.3e — Runtime Composition Profiles + 1080p Profile Foundation

- Added source-accurate runtime composition profiles for legacy UI preview work.
- Legacy GameOS Mechlopedia detail pages now compose over the locked `mcl_en.fit` base and preview at the runtime detail mount offset `285,58`.
- Campaign/mission operation pages now derive companion mount offsets from the composed campaign base when possible, preferring video/tacscreen/planet mount rects before map/operation rects and only then falling back to the old MAP_RECT offset.
- Added 1920x1080 modern-master planning notes for future page generation without auto-generating replacement pages yet.
- No Smart Tools, canvas, or simulation behavior changes.

## v0.5.3d — Mission Mount Compile Hotfix

- Removed a direct `UiEditorFitCell::alias` reference from mission mount detection.
- No behavior changes.

## v0.5.3c — Legacy Mechlopedia / Mission Mount Hotfix

- Attempted legacy GameOS Mechlopedia and mission companion mount corrections.
- Superseded by v0.5.3e runtime composition profiles.

## v0.5.3b — Compile + Changelog Hotfix

- Removed a stray prose line that caused a compile cascade in `UiEditorMain.cpp`.
- Rebuilt the changelog into a cleaner format.

## v0.5.3a — Mission Companion Mount Hotfix

- Added first-pass mission/planet companion mount handling.
- Superseded by v0.5.3e runtime composition profiles.

## v0.5.3 — Correct UI Page Loading

- Added first-pass composition loading for UI pages that require shared/base FIT layers.
- Mechlopedia detail pages can load a locked base layer.
- Campaign/mission pages can load a locked campaign frame layer.
- Composition layers are display-only.
- No Smart Tools or simulation polish changes.

## v0.5.2 — Button Compatibility + Simulation Foundation

- Added legacy button action presets and action metadata.
- Added page-link metadata fields.
- Added button state metadata for hover, pressed, selected, disabled, pulse, and toggle behavior.
- Added preview-state cycling foundation.
- Added FIT search improvements in the browser.

## v0.5.2-pre-f — Legacy Layout Sanity Hotfix

- Skips converted legacy `0x0` metadata/container blocks as drawable/selectable cells.
- Re-applies filename resolution inference after parsing so `_1280`, `_1600`, and related pages are not forced back to stale 800x600 metadata.

## v0.5.2-pre-e — Legacy Image / Resolution Fix

- Added first-pass handling for converted per-piece PNGs and direct PNG-pixel UVs.
- Added layout resolution inference fixes for non-800 legacy page variants.

## v0.5.2-pre-d — Legacy UV Field Image Fix

- Added support for legacy separate UV fields such as `uvX`, `uvY`, `uvWidth`, and `uvHeight`.
- Prevents many legacy atlas/image cells from stretching whole textures when a source slice is present.

## v0.5.2-pre-c — FIT Cell Metadata Compile Hotfix

- Added missing `sourceStyle` and `sourceLine` metadata fields to `UiEditorFitCell`.
- No behavior changes.

## v0.5.2-pre-b — FIT Cell Compile Hotfix

- Added compatibility metadata fields needed by the diagnostics/editor path.
- No behavior changes.

## v0.5.2-pre-a — Image Compatibility Diagnostics

- Added image diagnostics toggle for FIT/legacy UI preview auditing.
- Missing texture references can draw visible diagnostic placeholders.
- Image cells can show atlas-vs-whole-image diagnostic overlays.
- Image cache probes extensionless legacy texture references with common image extensions.
- Image cache tries `data/art/gui/<basename>` and `data/art/<basename>` fallbacks for legacy texture refs.

## v0.5.2-pre — Compatibility Audit Seed

- Added UI element compatibility audit report.
- Added legacy action preset catalog.
- Added legacy element inventory seed.
- Added text source catalog seed.
- Data/report only.

## v0.5.1d — Frame Canvas Shortcut

- Added `F` keyboard shortcut to frame/center the active canvas workspace in the viewport.
- Shortcut is ignored while text/numeric input fields are active.
- No Smart Tool, inspector layout, or canvas scroll behavior changes.

## v0.5.1c — Numeric Double-Click Input Polish

- Reverted the v0.5.1b numeric field layout change.
- Numeric fields keep the compact drag/slider layout.
- Double-clicking supported numeric fields switches that field into direct typed input.
- Left-drag still scrubs values horizontally.
- No Smart Tool creation or canvas behavior changes.

## v0.5.1b — Numeric Field Input Polish

- Added direct input to numeric fields, but the layout was too crude.
- Superseded by v0.5.1c.

## v0.5.1a — Color/Text Inspector Compile Hotfix

- Added missing inspector text edit buffer declarations used by the v0.5.1 Color/Text production inspector.
- No behavior changes.

## v0.5.1 — Color/Text Production Pass

- Color Block, Panel, Button, and Text smart widgets carry editable fill color, border color, border width, and opacity.
- Text and Button widgets expose visible text, text key, alignment, and wrap controls in the Inspector.
- Canvas preview renders smart widget colors and basic text labels instead of placeholder-only boxes.
- Save Copy appends generated smart widgets with editable style/text metadata.
- No canvas scroll behavior changes.

## v0.5.0e — Canvas Scroll Regression Revert

- Reverted the v0.5.0d canvas scroll persistence changes that caused duplicate/flickering artboard draws.
- Restored the v0.5.0c canvas/scroll behavior.
- Kept the v0.5.0c modal centering compile fix.

## v0.5.0d — Canvas Scroll Persistence Hotfix

- Attempted to preserve canvas scroll offsets during centered zoom.
- Superseded by v0.5.0e.

## v0.5.0c — Modal Centering Compile Hotfix

- Removed `ImGui::SetNextWindowViewport`, which is unavailable in the bundled ImGui version.
- Kept main-viewport modal positioning via `ImGui::GetMainViewport()` and `SetNextWindowPos()`.

## v0.5.0b — True Viewport Modal Centering Hotfix

- Centered modal popups against the main application viewport instead of the current dock/canvas region.
- Superseded at compile level by v0.5.0c.

## v0.5.0a — Popup / Zoom Polish Hotfix

- Centered modal popups on the main editor viewport.
- Prevented HUD overlay from rendering above modal popups.
- Changed mouse-wheel zoom anchoring to the center of the canvas viewport.

## v0.5.0 — Smart Tools Foundation

- Promoted the lower-left Smart Tools pane into the active creation surface.
- Added Select, Color Block, Text, Button, Panel, and Image tool modes.
- Added drag-on-canvas creation for Smart Tool primitives.
- New widgets are created in memory, selected immediately, and tracked through edit history.
- Save Copy appends generated Smart Tool widgets as new FIT blocks while preserving source files.

## v0.4.20 — Edit Command Infrastructure

- Added centralized edit command records for rect, alias, visibility, and lock changes.
- Added Undo/Redo menu actions and toolbar controls backed by command history stacks.
- Added Ctrl+Z / Ctrl+Y shortcuts when text input is not active.
- Preserved per-tab edit history and redo history when switching canvas tabs.
- Added command counts to the status bar.

## v0.4.19a — Page Creation Compile Hotfix

- Replaced the New Page profile picker `ImGui::Combo` callback overload with a `BeginCombo`/`Selectable` implementation for MSVC/ImGui compatibility.

## v0.4.19 — Page Creation + Navigation Model

- Added File/New Page flow for creating empty FIT-backed UI page shells.
- Added surface/profile/output path controls for new pages.
- Added editor-side page-flow metadata staging from selected widgets.
- Added Inspector Page Flow section for openPage / launchMission style links.
- Removed stale v0.4.12 resolution/profile inspector hint.

## v0.4.18 — Concept Shell Pass

- Added a cleaner grouped top toolbar for modes, creation prep, and file actions.
- Made the View menu control Grid, Snap, Rulers, Guides, Images, Labels, and HUD visibility.
- Added a bottom status bar with layout status, cell count, tab count, zoom, canvas size, and dirty marker.
- Polished the dark editor style with stronger dock/panel framing and tab/menu colors.

## v0.4.17d — Guide Edit/Delete Hotfix

- Existing guides can be grabbed and repositioned.
- Dragging a guide back into its source ruler deletes it.
- Right-clicking a guide deletes it.
- Hovered/active guides highlight while editing.

## v0.4.17c — Canvas Toolbar Overflow Hotfix

- Moved dense canvas display toggles into a compact Canvas Options popup.
- Keeps Zoom, W, H, profile controls, tabs, centered workspace, rulers/guides, and HUD behavior intact.
- Prevents HUD/Grid/Guides/Images/Labels controls from clipping under the right panel.

## v0.4.17b — Scrollbar Interaction Hotfix

- Removed empty-canvas left-drag panning because it intercepted normal scrollbar interaction.
- Kept the Photoshop-style centered workspace, tabs, HUD controls, rulers, guides, and scrollable canvas behavior.

## v0.4.17a — PanelBlue Compile Hotfix

- Added the missing `PanelBlue()` helper used by the active canvas tab button.

## v0.4.17 — Photoshop-style Canvas Viewport + Tabs

- Centered the artboard inside a larger Photoshop-style canvas workspace.
- Made the canvas workspace larger than the artboard so rulers and guides can extend beyond 1920x1080.
- Added initial empty-canvas hand panning with left mouse drag.
- Added HUD visibility and HUD mode controls.
- Added a canvas tab strip foundation.
- Preserved v0.4.16 rulers, guides, grid, snapping, and Editor FIT compatibility.

## v0.4.16 — Photoshop Canvas Foundation

- Added ruler bars to the canvas viewport.
- Added draggable vertical and horizontal guides from the ruler bars.
- Added guide visibility and snap-to-guides controls.
- Fixed transform snapping so move/resize can snap to guides as well as grid increments.

## v0.4.15 — Editor FIT Compatibility Pass

- Added loader support for converted `GuiMfcControl` blocks.
- Preserved MFC control metadata for Inspector review.
- Added display aliases from converted visible text/control IDs.
- Added `GuiLegacyBlock` grouping for existing `data/defs/ui/packages/legacy_imgui/editor` pages.
- Page `rect` can define canvas size when `localWidth/localHeight` are not present.

## v0.4.14a — Side Panel Resize Compile Hotfix

- Restored missing left/right panel width state fields.
- Restored horizontal panel splitter helper signatures.

## v0.4.14 — Side Panel Resize + Image Fallback

- Left Project / Layouts panel can be resized horizontally.
- Right Hierarchy / Inspector panel can be resized horizontally.
- Broader UI FIT image previews resolve extensionless legacy texture names by trying common image extensions.

## v0.4.13 — UI FIT Browser Expansion

- Project / Layouts scans `data/defs/ui/` recursively.
- Browser lists UI FITs across game/editor/shared/mechlopedia/fonts and other UI folders.
- Added search/filter and Refresh.
- Mechlopedia composite preview behavior remains special-cased only for Mechlopedia detail pages.
- CMake requests C++17 for the UI Editor target because the browser uses `std::filesystem`.

## v0.4.12 — Resolution-Independent Layout Foundation

- Added source and preview resolution profile controls for legacy and modern UI authoring.
- Added required 1920x1080 modern master profile alongside legacy 640/800/1024/1280/1600 profiles.
- Added profile scale display, aspect-category detection, and aspect-mismatch warning text.
- Added safe-area overlay groundwork and inspector prep fields for anchors and element scale policies.
- Canvas drawing separates editor zoom from resolution/profile preview scaling.

## v0.4.11 — Canvas Controls + History Prep

- Added editable canvas width/height controls.
- Added cursor-centered mousewheel zoom in the canvas viewport.
- Added zoom range expansion up to 3.00x.
- Added lightweight edit-history entry capture for move, resize, and inspector rectangle edits.

## v0.4.10 — Hierarchy Visibility / Lock Toggles

- Added in-memory Hierarchy / Layers visibility toggles for active FIT cells.
- Added in-memory lock toggles for active FIT cells.
- Hidden active cells are not drawn or selected.
- Locked active cells remain visible but cannot be moved or resized.

## v0.4.9 — Canvas Viewport Scrollbars

- Reworked the canvas area into a real scrollable viewport child.
- The 800x600 artboard defines the scrollable content surface.
- HUD is drawn after the viewport as a foreground overlay.

## v0.4.7 — Rename Alias

- Added right-click Rename Alias support in the Hierarchy / Layers pane.
- Added in-memory alias overrides that update hierarchy labels, canvas labels, HUD, and inspector display.

## v0.4.6 — Page Switch Dirty Modal Fix

- Fixed page switching after dirty edits by opening the discard-edits confirmation from the root popup scope.

## v0.4.5 — HUD Vertical Fix

- Fixed the selected-cell canvas HUD so it renders Name, X, Y, W, H as separate vertical lines.
- Updated the UI Editor window title and SemVer after the prior title helper still reported 0.4.3.

## v0.4.4 — Tiny Polish Pass

- Changed the selected-cell canvas HUD to a compact vertical readout.
- Removed the redundant File hint from the Project / Layouts pane.
- Updated SemVer to `0.4.4`.

## v0.4.3 — Layout / Workflow Polish

- Added separate bordered panes for Project / Layouts and Smart Tools.
- Added draggable horizontal splitters for side panels.
- Added File > Load Path.
- Added Save Copy dialog.
- Improved hit testing to prefer the smallest editable cell under cursor.

## v0.4.2 — Save / Reload Safety

- Added File > Reload.
- Added dirty-layout protection when loading or reloading a FIT.
- Added overwrite confirmation when Save Copy output already exists.
- Added Save Copy result popup feedback.

## v0.4.1 — File Menu Cleanup

- Added File > Save Copy and File > Quit.
- Removed duplicate toolbar and left-panel Save Copy buttons.

## v0.4.0 — Save Copy

- Added safe Save Copy output for edited active FIT layouts.
- Added source-preserving rect updates.
- Save Copy refuses to overwrite the loaded source file.

## v0.3.7 — Label Clutter Pass

- Added a Labels canvas toggle.
- Canvas labels default to selected/hovered cells when image preview is enabled.

## v0.3.6 — Legacy UV / Atlas Preview

- Updated FIT image previews to respect `uvPixels` atlas slices.
- Matched Viewer legacy 256x256 atlas scaling rule.
- Inspector shows `uvPixels`.

## v0.3.5 — STB Image Preview

- Replaced optional SDL2_image preview path with stb_image-backed image loading.

## v0.3.4 — Image Preview

- Added optional image preview rendering for FIT cells with texture/image references.
- Added Images toggle and image dimensions/path display.

## v0.3.3 — Composite Alignment Fix

- Fixed Mechlopedia composite preview alignment by drawing detail FIT cells at their page `mountOffset`.

## v0.3.2 — Composite Preview

- Added locked `main.fit` composite preview behind Mechlopedia detail FITs.

## v0.3.1 — Local Rect Display Fix

- Updated FIT cell display to prefer `legacyLocalRect` when present.

## v0.3.0 — In-Memory Editing

- Added in-memory cell move, resize, inspector rect editing, and dirty-state tracking.

## v0.2.3 — Alias Display Pass

- Added rule-based human-readable aliases for loaded FIT cells.

## v0.2.2 — Right Panel Split

- Split the right sidebar into hierarchy/layers and inspector panes.

## v0.2.1 — Load Controls Hotfix

- Added FIT path input, Load Path, Reload, and clickable Mechlopedia entries.

## v0.2.0 — Initial FIT Loading

- Added read-only typed FIT loader for `GuiPage` and `Gui*` cell blocks.
- Added default load path for `data/defs/ui/packages/legacy_imgui/mechlopedia/main.fit`.

## v0.1.1 — Hygiene Pass

- Added centralized UI Editor SemVer constants and title helpers.
- Added `UI_EDITOR_CHANGELOG.md`.

## v0.1.0 — Compile Shell

- Added the first standalone UI Editor shell.
- Added SDL2 + OpenGL + ImGui window with project/tool panels, canvas, hierarchy/inspector, and status controls.
