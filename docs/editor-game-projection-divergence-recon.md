# Editor <-> Game Projection / Render / Pick / Cull Divergence -- Recon + Gap-Closing Plan

Status: ANALYSIS + PLAN (no implementation). Worktree: .claude/worktrees/nifty-mendeleev
(checked out claude/fx-tracy-cost-split at recon time; file:line verified there 2026-06-15).

Intent: make the EDITOR consume the GAME unified GL forward-projection
(worldToClipGL -> projectModernClipGL / clipSpaceFrustumAdmitGL), retiring the legacy
D3D-pixel projectZ / cached viewMul / screen-rect path for object screen-pos, pick, marquee,
and visibility-cull.

Symptom legend:
- #2 DRIFT  -- placed objects drift as the camera moves (D3D vs GL projection split).
- #3 SHRINK -- scene shrinks to 1/4 bottom-left on rotate (transient editor VIEWPORT issue; SEPARATE TRACK).
- #4 PICK-BEHIND -- picking selects objects behind the camera (projectZ does fabs of rhw -> loses w-sign; screen-rect-only admit).
- VANISH -- movers/units vanish while moving (visibility gated on projectZ angular/screen-rect, not GL frustum).

---

## 0. The single source of truth that already exists (the F-track)

mclib/camera.h + mclib/camera.cpp + mclib/object_admission_predicate.{h,cpp} already contain a
complete GL-unified projection layer. The editor simply does not call it.

- Camera::worldToClipGL() -- mclib/camera.cpp:2839. The ONE matrix the GPU rasterizes with:
  kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL (cameraToClip * kPixelHomogToGLNDC).
  The F1 single composition source. (camera.h:944)
- Camera::projectModernClipGL(world) -- mclib/camera.cpp:2881. Row-vector world*M, returns
  clip + admit; admit = clipSpaceFrustumAdmitGL(clip).
- clipSpaceFrustumAdmitGL(clip) -- mclib/object_admission_predicate.cpp:163. Standard GL
  clip-volume test: w<=1e-4 reject; -w<=x<=w, -w<=y<=w, 0<=z<=w (ZERO_TO_ONE near).
  The correct front/frustum gate -- NOT fabs(rhw), NOT screen-rect.
- Camera::extractFrustumPlanes(planes 6x4) -- mclib/camera.cpp:752. Gribb-Hartmann on
  worldToClipGL() (the a280dde2 terrain fix). Plus quadAabbInFrustum (camera.cpp:786).
- Intent-specific wrappers in camera.h (all Legacy/Modern x Off/Compare/Bypass env-switched):
  - projectForScreenXY(point,screen) -- camera.h:828. GL-correct screen.xy via viewport remap
    (gos_GetViewport -> ndc -> pixels, Y-flip). Modern/Bypass uses projectModernClipGL.
  - projectForObjectAdmission(point,screen) -- camera.h:593. The object lifecycle cull bool
    feeding windowsVisible -> canBeSeen(). Track A1.
  - projectForTerrainAdmission (:574), projectForEffectAdmission (:644),
    projectForSelectionPicking (:736), projectForDebugOverlay (:890), lighting/shadow.
  - Raw projectZ (camera.h:476) is [[deprecated]] -- every direct caller is a debt site.

Seam is CLEAN at the wrapper level: the editor must stop calling raw projectZ and start calling
the same projectFor* wrappers (Modern) the game already runs. No new projection needed -- only
route the editor onto the F-track.

---

## 1. THE TWO PATHS, SIDE BY SIDE

### 1a. Object screen-projection (world -> pixels)
- GAME: Mech3DAppearance::recalcBounds -> eye->projectForScreenXY(position,screenPos)
  -- mclib/mech3d.cpp:2296. GL-correct (under Modern). Game appearances already migrated.
- EDITOR: raw eye->projectZ(...) at editor/EditorObjectMgr.cpp 219, 233, 1322, 1675, 1692, 1710.
  D3D-pixel: (x*rhw)*viewMulX + viewAddX, screen.w = fabs(rhw) (camera.h:505-508) -- loses w-sign.

### 1b. Picking / selection (cursor -> object)
- GAME (in-mission): GameObjectManager::findTerrainObjectByMouse -- code/objmgr.cpp:3416.
  FORWARD-projects each candidate world-space OBB through the GL eye projection via
  projectPickCandidateRect (code/objmgr.cpp:3083, calls eye->projectForScreenXY at :3184),
  near-plane reject. NEVER unprojects cursor->world. (template b6038fd1)
- EDITOR LIVE single-click Select tool: EditorInterface.cpp:1600 ->
  EditorObjectMgr::getObjectAtScreenPosition (editor/EditorObjectMgr.cpp:882) ->
  EditorObjectMgr_ConsiderScreenPick (:836) which ALREADY uses eye->projectForScreenXY
  (:845,:861) + near-plane (sp.w<=1e-4) + NaN guards. ALREADY on the GL forward-project metafix
  (a2cebf34 / editor_pick_screen_projection_metafix). #4 single-click is largely CLOSED.
- EDITOR world-space brush pick (damage/erase/link/selection-brush): getObjectAtPosition
  (editor/EditorObjectMgr.cpp:773). Pure world-XY distance test, NO projection -- robust, no debt.
- EDITOR marquee / rubber-band select: editor/EditorObjectMgr.cpp:1667-1719. Still raw projectZ
  (1675 buildings, 1692 units, 1710 dropzones) + screen-rect xMin..xMax. DEBT -- #4.
- EDITOR DEAD helper: EditorObjectMgr_ConsiderScreenCenter (:211, projectZ at 219/233) has NO
  callers (grep-confirmed). Superseded by ConsiderScreenPick. Delete, do not migrate.

### 1c. Mover / unit visibility-cull (the VANISH gate)
- GAME: code/objmgr.cpp:363-407 computes modernBit from the GL MVP (gos_GetTerrainMVPMat4),
  sign-normalizes clip.w, applies the standard frustum test (:393-396) -- byte-equivalent to
  clipSpaceFrustumAdmitGL. Lockstep with shaders/gpu_cull_predicate.glsl +
  mclib/object_admission_predicate.cpp. The game correct cull.
- SHARED gate (BOTH game render and editor render): Mech3DAppearance::recalcBounds
  (mclib/mech3d.cpp:2280). After the GL projectForScreenXY it STILL gates inView via:
  (i) angular cosine-FOV cosine > eye->cosHalfFOV (mech3d.cpp:2344-2348), and
  (ii) screen-rect on-screen lowerRight>=0 and upperLeft<=screenRes (mech3d.cpp:2381-2383),
  both via setVisibilityGatesFromLegacy(...) (mclib/appear.h:222). clip_w_sign_trap.md flags the
  angular path as geometrically broken at steep pitch (dropped 87% of on-screen buildings in
  measured test). The editor calls appearance()->render() ONLY when recalcBounds() returns true
  (editor/EditorObjectMgr.cpp:1252,1289), so the editor INHERITS this broken gate -- the VANISH
  root, SHARED code, not editor-local.

### 1d. Terrain cull
- GAME: Camera::extractFrustumPlanes on worldToClipGL() (a280dde2, camera.cpp:752) -> unified
  cull/render/pick/admission. Done.
- EDITOR: editor runs the same default-on modern terrain chain (self-skips legacy draw). Terrain
  cull is NOT an editor-specific divergence; no action.

### 1e. Viewport setup
- GAME: full-window scene; gos_GetViewport returns full width; viewMul/viewAdd derive
  consistently. No split.
- EDITOR: EditorGameOS RTT / fixed-layout viewport (MC2_EDITOR_RTT, MC2_EDITOR_AUTODOCK). When the
  scene renders into a SHRUNK sub-rect, cached viewMulX/Y (camera.h:505) that projectZ multiplies
  by can disagree with the GL render viewport -> position-dependent pick error (the residual that
  forced RTT default-OFF). Single-source template 9c3b72b9 + editor_dock_resize_pick_divergence.md:
  fix the source gos_GetViewport, not the cached copies. The #3 SHRINK family -- SEPARATE TRACK
  (viewport, not projection).

---

## 2. DIVERGENCE TABLE

| # | Editor site (file:line) | Game equivalent (file:line) | Symptom | Closing template |
|---|---|---|---|---|
| D1 | EditorObjectMgr.cpp:211/219/233 ConsiderScreenCenter (DEAD) | n/a | #4 latent | DELETE (superseded by ConsiderScreenPick) |
| D2 | EditorObjectMgr.cpp:1322 dropzone overlay projectZ | projectForScreenXY everywhere | #2 dropzone drift | a2cebf34 (projectZ to projectForScreenXY + near/NaN) |
| D3 | EditorObjectMgr.cpp:1675 marquee buildings projectZ + screen-rect | objmgr.cpp:3083 projectPickCandidateRect | #4 pick-behind | b6038fd1 / a2cebf34 (forward-project, near-plane reject) |
| D4 | EditorObjectMgr.cpp:1692 marquee units projectZ + screen-rect | objmgr.cpp:3083 | #4 pick-behind | b6038fd1 / a2cebf34 |
| D5 | EditorObjectMgr.cpp:1710 marquee dropzones projectZ + screen-rect | objmgr.cpp:3083 | #4 pick-behind | b6038fd1 / a2cebf34 |
| D6 | mech3d.cpp:2344-2348 angular cosHalfFOV gate (SHARED) | objmgr.cpp:393-396 modern clip frustum | VANISH | clipSpaceFrustumAdmitGL via projectForObjectAdmission Modern |
| D7 | mech3d.cpp:2381-2383 screen-rect on-screen gate (SHARED) | objmgr.cpp:393-396 | VANISH | clipSpaceFrustumAdmitGL (drop screen-rect, keep clip-admit) |
| D8 | EditorGameOS RTT shrunk viewport vs cached viewMul (camera.h:505) | full-window gos_GetViewport | #2 drift / #3 shrink | 9c3b72b9 single-source gos_GetViewport (SEPARATE TRACK) |

ALREADY-CLOSED (no action):
- Live single-click Select pick getObjectAtScreenPosition/ConsiderScreenPick
  (EditorObjectMgr.cpp:882/836) -- already projectForScreenXY (a2cebf34). #4 single-click DONE.
- World-space brush pick getObjectAtPosition (EditorObjectMgr.cpp:773) -- world-XY distance, no
  projection. DONE.
- Game appearance screen-pos recalcBounds -> projectForScreenXY (mech3d.cpp:2296). DONE.

Live Select-tool pick: EditorInterface.cpp:1600 (currentBrushID==IDS_SELECT) ->
getObjectAtScreenPosition (the FIXED forward-project path). Marquee is the remaining live projectZ
pick -- confirm its exact entry interactively (likely SelectionBrush drag-rect -> the 1667-1719 loop).

---

## 3. THE UNIFICATION TARGET

Single GL forward-projection the editor should adopt:
- Object screen-position (overlays/brackets/dropzones/marquee): Camera::projectForScreenXY
  (camera.h:828) -- already viewport-remapped + Y-flipped, GL-NDC. Run editor in Modern.
- Visibility/cull admission: Camera::projectForObjectAdmission (camera.h:593) whose Modern path
  returns clipSpaceFrustumAdmitGL(projectModernClipGL(p).clip) -- the SAME predicate the game
  objmgr.cpp:393-396 and gpu_cull_predicate.glsl use (lockstep).
- Marquee rect test: forward-project each center via projectForScreenXY, reject sp.w<=1e-4 (near),
  then test the pixel rect -- the ConsiderScreenPick recipe (a2cebf34), rect-membership instead of
  nearest-pixel.

Shared worldToClipGL the editor can bind (the F1 idea)? YES at the matrix level:
Camera::worldToClipGL() (camera.cpp:2839) is the single composition source the GPU uniform path
already consumes; the projectFor* wrappers are the CPU front door onto it. The editor does NOT need
per-call matrix surgery -- it needs to (a) switch the relevant wrappers to Modern and (b) replace
its 6 raw projectZ sites with the matching wrapper. The deeper item (D8) is purely the editor
VIEWPORT feeding wrong viewMul/aspect into the remap when RTT shrinks the scene -- a viewport
single-source problem, the only place lacking a clean seam, and the SEPARATE #3 track.

09707cd8 framing: one GL-NDC ViewUniforms matrix for ALL passes dissolves the whole family.
CPU analog: route every editor projection through the projectFor* Modern wrappers so editor and
game share the one worldToClipGL. Once D8 viewport is single-sourced, D2-D7 collapses.

---

## 4. GAP-CLOSING PLAN (sequenced, smallest-valuable-first)

Each slice is independently validatable by an eyes-on editor run. Env knobs exist:
MC2_*_PREDICATE_MODE=Modern + MC2_PROJECTZ_BYPASS_MODE=Compare logs >1px disagreements.

### Slice 0 -- DEAD-CODE removal (zero-risk) [D1]
- File: editor/EditorObjectMgr.cpp (delete ConsiderScreenCenter :211-250).
- Template: none (grep-confirmed no callers).
- Validate: builds; editor runs; selection unchanged.

### Slice 1 -- Marquee / rubber-band forward-project [D3,D4,D5] (#4) -- LOW RISK DROP-IN
- File: editor/EditorObjectMgr.cpp:1667-1719 (3 loops).
- Change: replace eye->projectZ(pos,screenPos) + screen-rect with eye->projectForScreenXY(pos,sp)
  + sp.w>1e-4 near-plane reject + NaN guard, THEN rect membership xMin<=sp.x<=xMax and yMin<=sp.y<=yMax.
  (Mirror ConsiderScreenPick, a2cebf34.)
- Template: a2cebf34 (drop lossy projectZ + near-plane reject) + b6038fd1 (forward-project, never unproject).
- Validate: drag a marquee including the horizon / behind-camera region. BEFORE: behind-camera
  objects get boxed. AFTER: only on-screen objects select. Also marquee while rotating -- selection
  must track what is drawn.

### Slice 2 -- Dropzone overlay forward-project [D2] (#2) -- LOW RISK DROP-IN
- File: editor/EditorObjectMgr.cpp:1322.
- Change: projectZ -> projectForScreenXY + near-plane/NaN guard before the 6px rect.
- Template: a2cebf34.
- Validate: place a dropzone, orbit/zoom -- marker stays glued to the ground point (no drift),
  disappears cleanly behind camera.

### Slice 3 -- VANISH gate: route recalcBounds visibility onto clipSpaceFrustumAdmitGL [D6,D7] (VANISH) -- MEDIUM RISK (shared code)
- File: mclib/mech3d.cpp:2280 recalcBounds (+ parallel BldgAppearance/GVAppearance/
  GenericAppearance/TreeAppearance recalcBounds -- grep setVisibilityGatesFromLegacy).
- Change: gate inView via projectForObjectAdmission(position,screen) Modern result
  (== clipSpaceFrustumAdmitGL) instead of angular cosHalfFOV (:2344) + screen-rect (:2381).
  Keep haze/far-clip distance logic. CPU twin of objmgr.cpp:393-396.
- Template: clip_w_sign_trap.md (clip-space admit, never angular sphere-clip) + the game own
  objmgr.cpp:363-407 modern-bit block as reference.
- Risk: SHARED with game render. Gate behind MC2_OBJECT_ADMISSION_PREDICATE_MODE=Modern + Compare
  first; tier1 smoke (game) AND eyes-on editor before default-on. Edge-flicker hysteresis
  (mech3d.cpp:2393 fix flickering) must be re-checked under the new gate.
- Validate (editor): fly the camera so a mech passes through the screen edge / behind camera while
  moving. BEFORE: vanishes early / pops. AFTER: visible until OBB truly leaves the GL frustum.
  Validate (game): tier1 5/5, no actor pop-in regression.

### Slice 4 -- #3 VIEWPORT single-source [D8] (#2 residual / #3 shrink) -- SEPARATE TRACK, HIGHER RISK
- File(s): editor/EditorGameOS.cpp (RTT / fixed-layout viewport), GuiRuntime viewport rect,
  gos_SetupViewport / gos_GetViewport, camera.h changeResolution (viewMul/viewAdd re-apply).
- Change: make gos_GetViewport the ONE source feeding viewMul/TG_Shape/userInput/the GL remap, so
  a shrunk RTT scene and the projection agree by construction.
- Template: 9c3b72b9 + editor_dock_resize_pick_divergence.md (fix the source, dump ALL caches in one line).
- Risk: the unsolved RTT split-brain; currently mitigated by MC2_EDITOR_RTT=0 default-off
  (full-window = exact). Only needed if panels-beside-map (shrunk scene) is wanted.
- Validate: MC2_EDITOR_RTT=1, dock asset browser beside map, click at the map right edge --
  cursor and highlight coincide across the whole viewport incl. edges, no parallax on orbit.

Sequencing rationale: 0 and 1/2 are isolated editor-local drop-ins (a2cebf34 shape, no game blast
radius) -> ship first. 3 touches SHARED appearance code -> Compare-mode soak + game smoke -> ship
behind env, default-on after parity. 4 is orthogonal viewport plumbing -> own track, only if
shrunk-scene docking is desired; otherwise RTT-off already gives exact picking.

---

## 5. RISKS / UNKNOWNS (need interactive repro)

- Which marquee entry is live: confirm the rubber-band drag reaches EditorObjectMgr.cpp:1667-1719
  (vs a SelectionBrush path). SelectionBrush.cpp/ObjectSelectionBrush.cpp call
  getObjectAtScreenPosition (the FIXED path) for single hits; the rect loop caller must be
  confirmed by stepping a drag in the editor.
- Slice 3 blast radius: recalcBounds is shared by ALL appearance types and runs every frame
  in-game. The angular cosHalfFOV gate also feeds LOD/haze -- verify switching only the inView
  admit (not haze/distance) does not regress game LOD. Compare-mode log + tier1 first.
- Edge-flicker hysteresis (mech3d.cpp:2393): the legacy gate carries a deliberate wasInView hold to
  stop screen-edge flicker; the clip-space admit must preserve equivalent hysteresis or edge mechs
  flicker. Eyes-on at the screen edge.
- #3 exact viewport trigger: scene shrinks to 1/4 bottom-left on ROTATE -- one-frame
  gos_GetViewport race on the rotate input (RedrawWindow/Invalidate timing) or a persistent viewMul
  mismatch? Capture the one-line cache dump (PICKW pattern, 9c3b72b9) during a rotate to pin which
  cache reads stale width. Until then #3 stays a SEPARATE track.
- Compare-mode in editor: confirm the editor process honors MC2_*_PREDICATE_MODE /
  MC2_PROJECTZ_BYPASS_MODE (game does); if the editor inits the predicate singletons differently,
  run the Compare soak via the editor exe, not game.
