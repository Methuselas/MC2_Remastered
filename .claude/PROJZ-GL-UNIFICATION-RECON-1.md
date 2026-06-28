# PROJZ-GL-UNIFICATION-RECON-1 — editor object projection unification (legacy projectZ → GL projectForScreenXY)

Scoping doc for the slice that gates a docked center-pane viewport (panels shrinking the scene).
Recon 2026-06-27 (read-only). Blocker context: editor projects objects with legacy `projectZ`
(full-window cached viewMul) while the scene renders via GL `projectForScreenXY` (fresh
`gos_GetViewport`); an origin-offset / shrunk scene rect makes the two diverge → pick drift.

> **Headline:** the 4 sites the vision named (EditorObjectMgr.cpp:1322/1675/1692/1710) are ALREADY
> migrated to the GL path. Remaining live `projectZ` callers are brush-paint + a terrain overlay —
> isolated, low-risk. Center-pane docking is gated ONLY by Slice 5 (RTT viewport single-source).
> **Side panels that do NOT move the scene rect are safe TODAY** (no Slice 5 needed). RTT=0 is exact.

## 1. Live projectZ call sites (editor)
- **EditorInterface.cpp ~3050-3063** — commented-out `eye->projectZ(...)`, zero callers → DELETE (Slice 0).
- **EditorInterface.cpp ~5107** — green terrain-boundary quad; projects 4 corners → `gos_DrawQuads` screen-XY. LIVE (Slice 1).
- **ObjectSelectionBrush.cpp ~105, ~176** — marquee anchor (`lastWorldPos`→`lastPos`). LIVE (Slice 2).
- **SelectionBrush.cpp ~149-150** — `TerrainElevationToScreenRatio()` screen-space elevation scale (brush responsiveness, low stakes; keep-or-migrate). **~488** — marquee drag membership. LIVE (Slice 3).
- Comment-only mentions (no live call): HeightBrush/LinkBrush/ScatterBrush/terrainBrush/StampBrush/BuildingLink, EditorDebugOverlay, FoliageRender.

## 2. GL path to migrate TO
- **`Camera::projectForScreenXY(Vector3D&, Vector4D&)`** — `mclib/camera.h:957-1015`. Modern+Bypass → `projectModernClipGL` then NDC→viewport remap via fresh `gos_GetViewport()`; near guard `clip.w > 1e-4`.
- **`Camera::projectModernClipGL(const Vector3D&) const`** — `mclib/camera.cpp:2991-3005`. `clip = world * worldToClipGL()` where `worldToClipGL = kAxisSwapMC2toGL * worldToCamera * cameraToClipGL` (camera.cpp:2949-2961) — SAME matrix the GPU/ViewUniforms use.
- **`clipSpaceFrustumAdmitGL(Vector4D)`** — `object_admission_predicate.cpp:163-171`. SIGNED-w behind-camera reject (`w<=1e-4`), ZERO_TO_ONE near.
- **Editor wrapper `EditorObjectMgr_ProjectScreenXY_GL(const Vector3D& wp, Vector4D& sp)`** — `editor/EditorObjectMgr.cpp:865-881`. Use THIS in editor sites: calls `projectModernClipGL`, near-guard, `gos_GetViewport(vax,vay,vmx,vmy)`, `sp.x=vax+(ndcX*.5+.5)*vmx`, `sp.y=vay+(1-(ndcY*.5+.5))*vmy`, Y-flip.

## 3. Exact divergence
- **projectZ** (camera.h:559-562): `screen.x = (clip.x*rhw)*viewMulX + viewAddX`; `screen.w = fabs(rhw)`. `viewMulX/Y,viewAddX/Y` are CACHED at camera-update (camera.h:505) from the **full-window** viewport. `fabs(rhw)` LOSES the w-sign → behind-camera points pass as positive.
- **projectForScreenXY**: `ndcX=clip.x/clip.w` then remap with vax/vay/vmx/vmy read FRESH from `gos_GetViewport()` (current scene rect). Signed-w → behind-camera correctly rejected.
- **Scenario** (1920 window, 320 right panel → scene 1600): projectZ uses stale 1920, GL uses current 1600 → **1.2× scale mismatch** → right-edge objects project off-viewport → no selection despite being visible. Plus projectZ can box behind-camera objects in a marquee; GL rejects them.

## 4. Scene-rect plumbing
- `GuiRuntime/GuiRuntime.cpp:102-135` statics `s_vpX/s_vpY`, `s_fixedX/Y/W/H`; updated :228-229 (`useFixed? s_fixedX : central->Pos.x`).
- `editor/EditorGameOS.cpp:520-535` under RTT: `sw=w-panelPx; Environment.screenWidth=sw; GuiRuntime::SetFixedViewportRect(0,0,sw,sh)` — offset hardcoded (0,0); panel on right, scene left-anchored.
- **ROOT CAUSE** (EditorGameOS.cpp:537-560): `gos_SetupViewport()` is applied, but `Camera::update()` caches `viewMulX/Y` for projectZ; if setup runs AFTER camera update, projectZ reads stale full-window cache. `projectForScreenXY` is immune (fresh read).

## 5. Slice plan
- **Slice 0** (no risk): delete dead commented projectZ EditorInterface.cpp ~3050-3063.
- **Slice 1** (low): EditorInterface.cpp ~5107 boundary quad → `EditorObjectMgr_ProjectScreenXY_GL` + near guard. Verify: quad glued to ground on zoom; cleanly vanishes behind camera; not shifted under RTT.
- **Slice 2** (low): ObjectSelectionBrush.cpp 105/176 marquee anchors → GL wrapper. Verify: horizon marquee no longer boxes behind-camera objects.
- **Slice 3** (med): SelectionBrush.cpp 488 → GL wrapper; 149-150 keep-or-migrate (elevation ratio = responsiveness, not pick).
- **Slice 4** (med-high, SHARED w/ game render): mech3d.cpp recalcBounds inView gate via modern predicate. Env `MC2_OBJECT_ADMISSION_PREDICATE_MODE=Compare` soak, tier1 5/5 required. Separate track.
- **Slice 5** (high, ORTHOGONAL — the docked-panel gate): EditorGameOS.cpp:520-560 + GuiRuntime.cpp:226-236 — order `gos_SetupViewport()` BEFORE `Camera::update()` (or drop projectZ cache). ONLY needed for center-pane shrunk scene. Verify `MC2_EDITOR_RTT=1` + dock + click map right edge → cursor/highlight coincide, no parallax on rotate.

**Sequencing:** ship 0+1+2+3 together (isolated, no game blast) → soak 4 in Compare → defer 5 until docked center-pane is actually wanted. Until then: side panels (left Object overlay, right Tools dock) are safe; full-window RTT=0 is exact.

## 6. Already-closed (no action)
EditorObjectMgr.cpp ConsiderScreenPick (uses projectForScreenXY), getObjectAtPosition (world-XY, no projection), dropzone/marquee 1640/2015/2037 (already EditorObjectMgr_ProjectScreenXY_GL).
