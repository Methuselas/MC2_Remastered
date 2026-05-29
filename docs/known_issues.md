# Known issues (current)

To report a new issue: [GitHub Issues](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues)

---

## Player-visible issues

Known bugs that affect anyone running the game. Full detail in the beta doc:
[docs/beta/known_issues.md](beta/known_issues.md)

- **Shadow stutter** when the camera jumps more than ~500 units in one move. Panning is smooth; only large instant jumps trigger it.
- **Shadow banding** shifts slightly with camera rotation due to view-dependent tessellation.
- **Water shoreline z-fight** visible when zooming in or changing elevation (not on pan).
- **First-launch black terrain** (intermittent) — relaunch the mission; second load is normal.
- **Options menu writes bad resolution** to `options.cfg` on 4K displays. Delete `<game-deploy-dir>\options.cfg` to reset.
- **Bloom/FXAA/tonemapping apply to HUD** — scene and HUD share the same framebuffer.

---

## Developer / Internal

Extracted from CLAUDE.md 2026-05-24. Issues here are KNOWN — don't re-discover
them. Add new findings as new bullets; remove fixed ones outright (don't append
"FIXED" — delete the bullet).

## Shadows

- **Shadow re-render stutter when camera moves >500 units.** Fix: static world-fixed shadow map (design ready).
- **Shadow banding shifts with camera rotation** (view-dependent terrain geometry).
- **Dynamic prop shadows: no light-box cull yet** — with MC2_SHADOW_DYNAMIC_PROP_CASTERS=1 the dynamic pass draws ALL registered non-building props every frame (visibility-independent, depth-only) regardless of the light box. Cheap now; **HZB is the planned long-term fix**. Interim: CPU light-box AABB cull (must cull vs the LIGHT box, not the camera frustum). See `memory/shadow_dynamic_projection_and_caster_feed_fixed.md`.
- **Static building shadow goes stale on destroy** — MC2_STATIC_PROP_BUILDING_SHADOW bakes the world-fixed building shadow map once per mission; destroyed buildings keep their shadow until reload.
- **Foliage shadow alpha-discard (SHADOW-FOLIAGE-ALPHA-DISCARD-1, deferred)** — tree cards cast solid-quad shadows (the alpha cutout isn't applied in the depth-only shadow program).

## Water / terrain rendering

- **Water shoreline z-fight on zoom/elevation-change (NOT pan); water sits slightly low** (pre-existing). Interim fast-path fixes shipped 2026-05-17. Full state: `memory/water_fastpath_interim_fixes_and_residuals.md`.

## First-launch / startup

- **First-launch black terrain intermittency** — tier1 first mission occasionally renders black; second normal. Suspected: GPU/shader state dirty from previous mission teardown. Repro: tier1 with `--fail-fast`.

## Options / config

- **Options menu writes bad ResolutionX/Y to options.cfg** (observed 4096x2160 on 4K). Engine UI canvas is 800x600 and self-scales; other values break HUD scale + video positioning. Diagnostic: `memory/options_cfg_resolution_drift.md`.

## Blocked slice work

- **Stage 0.5 §4 (renderVisible repoint) BLOCKED — empirically NO-GO 2026-05-20 EVENING.** Tentative ship `40a54b7` reverted as `dc2e8f6` (popping + black-textures + resurrected 2026-05-05 black-tree class). §2.5 sticky-bit (`91b6991`) shipped independently and is durable. §4 deferred to alpha-Stage 1 OR pivot to v4 (gate render on `blockVisBits[]` directly under sticky). Full: `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`.

## Default-on flags / state

- **drawPass-retirement decal static-bake (`MC2_TERRAIN_INDIRECT_OVERLAY`): DEFAULT-ON since Stage-6 flip `60f2ef8`** (only `=0` reverts). On default path both SOLID and OVERLAY are armed so per-quad `draw()` loop in `Terrain::render drawPass` is SKIPPED. Slice A+B WIRED & validated 2026-05-17. Remaining endpoint: user-driven substitutive non-COST_SPLIT Tracy proof + decal visual canary. Full state: `memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md`.

## RenderSnapshot ok gate

- ~~**mc2_10 `ok=0` due to `staticPropValidationFail` (`sp_fail=1`).**~~ **FIXED `7bdbd1fd`.** `invalidateStaticRegistration()` tombstoned the registry recipe but never called `retireRecord` on the matching `s_objectRecords` slot; the slot stayed `alive=true` with a tombstoned recipe, causing `sp_fail=1` from frame ~1706 onward. Fix: `retireRecord` call added. Requires re-validation of mc2_10 with `MC2_RENDER_SNAPSHOT_LOG=1` to confirm ok=1 throughout.

## RenderWorld arc residuals

- **MLR-rendered mechs do not write object IDs (M2.5 known gap).** M2.6 pickup will work only for GPU-batched mech pixels. Empirical tier1 data shows `mlr_mech_draws=0` across all 5 missions, so the gap is rare-in-practice. If tier1 ever shows `mlr_mech_draws>0`, M2.6 must preserve mover-first legacy fallback for those mechs and cannot claim full mech GPU-pick coverage. Counter: `[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M` on per-mission summary.
- **gosFX dev-override broken under unified projection (F1 ship).** Running with `MC2_DISABLE_GOSFX=0` after F1 will render gosFX particles wrong: MLR's `mlrclipper.cpp:206-209,305,321,347` reads of `cameraToClip(2,2)` and `(3,2)` use stale MC2-pixel-homogeneous convention while the runtime uniforms drive shaders via the new GL convention (clip.w > 0 for in-front; polarity folded into `kAxisSwapMC2toGL` per addendum-rclipw-polarity.md). Default `MC2_DISABLE_GOSFX=1` (gate-dead MLR work-leaves) is unaffected. Runtime guard prints `[UNIFIED_PROJ v1] WARN:` stderr once per startup. Dev-override path re-enabled when MLR retirement Slices 1-5 ship.

## Asset / atlas pre-commit guards

- **Do NOT upscale the icon atlases.** `code/mechicon.cpp` hardcodes `unitIconX/Y` (32/38) and reads source-pixel offsets against `s_MechTextures->width` / `s_pilotTextures->width`. Oversized source TGAs (upscaler `*_4x_gpu/` or Magic's Unofficial Expansion overlay) scramble icon sub-rectangles. Keep all 9 icon atlases at FST-archive resolution + `mcl_pr_pilotskillicons.tga`. Pre-commit guard: `sh scripts/check-asset-scale-callers.sh`.
