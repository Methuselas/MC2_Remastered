# Editor Debug Overlay — Phase 2

Branch `claude/terrain-gen-pcg`. Commit `feat(editor): debug overlay layer …` + smoke minimized flag.

## What shipped
- `editor/EditorDebugOverlay.{h,cpp}` — self-contained overlay layer.
  - `RenderWorldOverlay(Camera*)` — chunk grid (20 cells), superchunk grid (N chunks,
    default 3, 1–8), water-bounds rectangle. Drawn from `EditorInterface::render()`
    right after `FoliageRender::Render()` (same frame/projection). No-op when nothing
    enabled or no terrain.
  - `RenderImGui()` — "Debug Overlays" panel: 4 toggles, superchunk-size / opacity /
    height-bias sliders, "grid on water plane vs follow terrain", stats (terrain verts,
    chunks/superchunks across, water elevation, foliage instance count).
- Pure visual — no terrain/save/mission state touched. Follows FoliageRender: project
  world pts via `Camera::projectForScreenXY`, submit `gos_DrawLines`, strips break on
  failed projection, render state (texture/alpha/ZCompare/ZWrite) restored after.
- `scripts/run_editor_smoke.py` — `--minimized` (default ON): launches the editor via
  `STARTUPINFO` `SW_SHOWMINNOACTIVE` so smoke never steals focus / pops a window.
  `--no-minimized` for a normal window.

## Build / smoke
- `check-editor-build.sh` → **PASS** (EditRel + asset_viewer).
- `run_editor_smoke.py` (minimized) → original **6/6 PASS**
  (gen_map_basic, foliage present/missing/garbage, gen_save_load gen+save / load).
- `foliage_menu_commands` (7th case) FAILs `menu_reload_bad` — **PRE-EXISTING**, added in
  commit `0abfda98` (parent of Phase 2), lives in EditorMFC.cpp, untouched by this work;
  reproduces identically with and without the overlay. Out of Phase 2 scope.

## Manual smoke (overlay)
Launch `A:/Games/mc2-opengl/mc2-win64-v0.4/Mission Editor.exe`, open/generate a map.
1. Tools panel → **Debug Overlays** window. Tick **Show Chunk Grid** → cyan grid every
   20 cells. Tick **Show Superchunk Grid** → yellow grid every N×20 cells.
2. Pan / zoom / rotate: grid stays aligned to terrain, strips break cleanly at screen
   edges (no screen-spanning streaks).
3. Toggle off → grid gone. Save / reload map → unaffected (overlay never serializes).
4. **Show Water Debug** → blue rectangle on the water plane at map bounds.

## Water-row artifact diagnosis (the point of Phase 2)
1. Reproduce the mid-zoom missing-water-row artifact.
2. Keep **Grid on water plane** ON (default) and enable **Show Chunk Grid** +
   **Show Superchunk Grid**. The grid now lies exactly on the water plane, co-planar
   with the water rows.
3. Read the alignment:
   - missing rows land on **cyan** lines → artifact is **chunk-row** aligned.
   - on **yellow** lines → **superchunk-row** aligned (try superchunk = 2 and 3 to see
     which spacing matches).
   - on neither → not chunk-driven: suspect LOD strips, a cull band, or depth/state.
4. Adjust **superchunk = N chunks** live to find the exact period of the missing rows.
   The stats panel reports chunks/superchunks across for cross-checking against the
   row count.

## Limitations / notes
- Grid drawn always-on-top (ZCompare off) so it reads over water/terrain — it does not
  depth-test against geometry (intentional for a diagnostic).
- No hardware line-width control used; superchunk lines are distinguished by colour, not
  thickness.
- Foliage debug = stats only (`FoliageRender::Count()/Visible()`); no per-instance bounds
  (would require a new FoliageRender getter — deferred to stay in scope).
