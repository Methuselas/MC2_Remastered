# HANDOFF: ui-phase1 bug queue (2026-07-05)

Worktree `A:/Games/mc2-ui-phase1`, branch `claude/ui-phase1-integration` (~17 local commits).
Deploy `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0` (deploy_payload --build-dir build64/RelWithDebInfo --allow-new-target).
**After EVERY deploy: `cp /tmp/uimerge/run-with-log.bat.user-backup <deploy>/run-with-log.bat`** (user file; deploy clobbers it).
Probes: env `MC2_LOG_PREVIEW=1`, output = `<deploy>/preview_debug.log` (fopen-append pattern ONLY — stdout/stderr unreliable across launch modes).
Verify meta: NO tier1 smokes. Harness `scripts/mechbay_preview_harness.py bay|purchase|loadout|launch` for logistics captures; user drives anything needing clicks (encyclopedia, menus). Headless `-mission` runs never draw the HUD (no input) — don't A/B HUD headless.

## FIXED+user-confirmed (don't re-open)
preview FBO (drawOldWay force), 4x supersample, bay/pilot icon bridge (beginGuiBridge wrap), pilot photo (uvLegacySpace opt-in), in-mission detach (gos_SetRenderViewport store-only + HUD-RES clamp restore), HUD 85% (s_hud_scale_exempt beginFrame hygiene), res-list dedupe, menu bleed (ImGui backdrop both branches in MissionBegin::render), windowed mode (MC2_WINDOWED, overrides applyPrefs too; launcher checkbox), c-bills mirror, encyclo lists (ENCYCLO-LAZYLOAD-1) + undefined filter.

## OPEN QUEUE (priority order)

1. ~~**ENCYCLO-3D black previews**~~ **FIXED 2026-07-05 `fc1b2d4f`** (pending user confirm; agent-verified fresh-boot Anubis visible via MC2_BOOT_TO_SCREEN=encyclopedia + dev-shell screenshot). Texture-cold theory was WRONG (chain fully healthy — probes proved tga loads, paint lock real, gos handle live). TWO real causes: (A) buildings.csv "Mechlopedia Scale" col EMPTY for all mechs → readFloat NO_ERR+0.0 → chassis scale 0 (logisticsdata.cpp now defaults ≤0 → 1.0); (B) UiDefs::getElementScreenRect scaled with offset (0,0) but render() uses (globalX+285,58) → preview composited at page-LOCAL [24,64.5] instead of screen [594,151.5], hidden under buttons (Impl now records lastRender offsets). Also: old nonBg FBO probe was BLIND (read 800x600 corner of 4x-supersampled 3200x2400 FBO) — fixed to full-viewport scan. Check vehicles/buildings/weapons tabs too (same shared fix, unverified).
2. **UI-ASPECT-ANCHOR-1 (meta-aspect)** — user-approved: ALL UI layers uniform height-fit scale s=displayH/600, centered pillar (xPad=(dw-800s)/2); flanks = terrain in mission / black in menus (backdrops exist). Choke points: UiDefs currentPageScale, aObject bridge sx/sy computations (~6 tb sites), mouse inverse (defs input + gos_HudInverseMousePoint). ONE shared helper. Test on 16:9 via launcher Width=3840 Height=1080 windowed (32:9 sim).
3. **UI-LAYER-CONTRACT-1 (meta-ghost/invisible)** — invert bridging: LogisticsScreen base when defs page active = auto black-clear GameOS layer + wrap WHOLE legacy render in gui bridge (opt-OUT per widget), screenChanged() clears both layers. Do together with #2 (same choke points). Kills the per-screen whack-a-mole class.
4. **Floating loading-art banner** at mission start (~1s, world-floating blue panel). Frames 100/250 probes missed; rides the early resolution-transition window (diagonal double-scene residue settles by ~1400). Likely fixed-by or related-to #2/#3 layer work; re-test after.
5. **AAR/salvage/promotion layout** — blanket bridge REVERTED (mixed scale spaces, warning comment in MissionResults::render). Needs per-widget bridge pass w/ harness captures (soak reaches results: MC2_SOAK_AUTOWIN + KILL_ENEMY for salvage rows).
6. **Ultrawide #45** — after #2 lands, most of it IS #2. Verify in-mission camera aspect uses drawable (not clamped Environment) at 32:9 sim.
7. **MERGE-CONFLICT-UI-PHASE1 markers** — grep; GuiRuntime editor-init + tgl.cpp GPU-shape branches.

## NIFTY side (worktree .claude/worktrees/nifty-mendeleev)
- DEV-SHELL complete (docs/dev-shell.md = playbook; ping/reload_shaders/screenshot/ui_reload/texture_refresh/framegraph). GAMEOS-SPLIT plan at docs/splits/gameos-graphics-split-1-plan.md — execute slice 1 (gosFont) when worktree free.

## CHURN-KILLERS (do first, ~1h, pays back immediately)
a. **Port DEV-SHELL commits from nifty onto ui-phase1** (cherry-pick e687057d ec5662b0 3338801e 7aec1844 68b241d7 + watcher a2158389; gos_dev_shell TU is additive). Biggest win: screenshots/gates/ui_reload/texture_refresh on THIS branch = no more build-run-quit-grep loops for UI work.
b. **`MC2_BOOT_TO_SCREEN=encyclopedia`** boot target (missionbegin/mainmenu ~20 lines) → encyclopedia harness-capturable, no user clicks.
c. **tools/dev_shell/shot.py**: TGA→PNG thumbnail + crop + diff-bbox in one CLI (replaces inline PIL one-liners).
d. **deploy_payload `--preserve run-with-log.bat`** (or skip-if-exists) → kills the manual cp ritual.
e. Keep probes file-based + env-gated; grep the log, never tail stdout.
