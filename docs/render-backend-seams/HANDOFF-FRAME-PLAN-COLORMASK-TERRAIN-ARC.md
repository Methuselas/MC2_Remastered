# HANDOFF — frame-plan + colorMask + terrain-routing arc (caveman, subagent-heavy)

Next session start here. Arc MERGED to nifty. Caveman: terrain swamp boss dead, frame now
tattles on itself, colorMask landmine defused. Pick a NEW lane; don't re-fight closed ones.

## WHERE
- nifty `claude/nifty-mendeleev` @ `aa936cee` (this arc merged over sibling DECAL/VFX/LIGHT/BLUR).
- Merge worktree used: `A:/Games/mc2-nifty-land` (build64 warm there). My arc branch
  `claude/terrain-routing-1` @ `91846b91` (off old nifty `36535e93`).
- Build/deploy/smoke: per CLAUDE.md. Build isolated worktree, point CMAKE_PREFIX_PATH at
  `<nifty-mendeleev>/3rdparty`. Deploy `scripts/deploy_payload.py`. ★ ALWAYS md5 deployed-vs-built.
- ★ NEVER launch the game (smoke/capture) without asking.

## WHAT LANDED (this arc)
BLENDMODE-MULTIPLY-1 (Multiply=DST_COLOR/ZERO vocab + binder + 4 post-fx rows routed, byte-proven) ·
RENDER-FRAME-PLAN RECON-1 + SCAFFOLD-1 · PIPELINE-STATE-OWNER-AUDIT-1 ·
COLORMASK-OWNERSHIP-1 (+leak finding) + ROLLOUT-RECON-1 + ROLLOUT-1 (beginScene keystone, proven) ·
TERRAIN-LODCHUNK-APPLYPIPELINE-ROUTING-1 (terrain SOLID ROUTED, binds=2843, sha cb5a700e held).
Earlier same branch: HARNESS-ISOLATION-1, water VISUAL_PROVEN, VFX gate, postprocess composite/fog.

## THE META PATTERN (this is the win — reuse it)
`FramePlan(recon) -> Scaffold(frame self-reports) -> StateOwnerAudit -> [keystone fix] ->
DrawpathRecon -> RoutingProof`. Turned "why does this bind ZERO?" into "the frame TELLS you which
path drew." Gate `MC2_RENDER_FRAME_PLAN_TRACE=1` -> `[FRAME_PLAN] phase=.. pass=.. path=.. pipeline=..`.

## ★ RAKES I STEPPED ON / HAD TO REDO (don't repeat)
1. **Terrain bridge bound 0x.** Routed `gos_terrain_bridge_drawIndirect` (gameos_graphics.cpp) —
   captured 0 binds. Burned ~6 builds guessing (legacy MLR? unarmable bridge?). TRUTH (frame-trace
   found it in ONE line): the LIVE default terrain-solid path is the **LOD-chunk** draw
   (`gos_terrain_lod_chunk.cpp:587`), default-ON via `mc2TerrainLodChunkEnabled()` (opt-out vestigial),
   which SUPPRESSES the bridge. Route the lod-chunk path, not the bridge. The bridge route is PARTIAL/
   dead-under-default (kept, not promoted).
2. **colorMask single-pass opt-in LEAKS.** Composite ({t,f,f}) emits glColorMaski(1/2,FALSE); being the
   LAST world pass it leaked into NEXT frame's MRT scene draw -> GBuffer1/objectId dropped (sha
   cb5a700e->8d40ce4a). Fix = **beginScene keystone**: `glColorMask(TRUE)` at frame start heals any
   set-only leak. Now incremental opt-in is safe (gate `MC2_PIPELINE_COLORMASK`, default OFF).
3. **Capture/smoke != gameplay path.** Deterministic capture does NOT arm the modern terrain bridge;
   it took the lod-chunk path. Per-path probes at the bridge/flush NEVER fired — only the DISPATCH
   CHOKEPOINT (txmmgr.cpp:2868) caught it. Probe at chokepoints, not assumed draw sites.
4. **applyPipeline does NOT own** colorMask / draw-buffer-MRT masks / viewport / scissor / stencil /
   VAO / tex-units. Legacy bridges save→set→restore their own colorMask. Keep their save/restore.
5. **markTerrainDrawn() latch** (set by all 3 terrain paths incl. lod-chunk:931) gates 5 post passes
   via `sceneHasTerrain_`. A terrain path that forgets it silently kills screenShadow/cloud/shoreline/
   edge/oob fog. Preserve it.
6. **lod-chunk has TWO branches** — RAII guards (`useGuards`, default-ON `MC2_GLSTATEGUARD_TERRAIN`)
   AND manual save/restore. recon-1 only saw the manual one. Live = RAII. Routed by asserting
   applyPipeline AFTER the state block (byte-identical re-assert + [PIPELINE_BIND] hook).
7. **Deploy-lease collisions:** siblings cycle v0.4/0.4c. deploy_payload SILENTLY skips a stale exe if a
   mc2.exe from that dir runs -> ALWAYS md5-verify; use a FREE deploy folder (there are 5).
8. **HARNESS-ISOLATION:** run_visual_capture is own-only-child reap now (never kills foreign mc2.exe);
   `--no-kill` default. Multi-run writes `<out_dir>/rN`.

## SUBAGENTS (use them, but VERIFY)
- Topology recon: dispatch `mc2-render-expert` (frame order, FBO, queue/flush) and
  `mc2-terrain-indirect-expert` (terrain paths) READ-ONLY. They are great for the map.
- ★ VERIFY their line-claims against source. They got real things WRONG this arc: "gos_terrain.frag
  single-attachment" (it writes GBuffer1 location=1 too), "3528 is terrain" (it's WATER MDI),
  "lod-chunk is opt-in" (default-ON). Always grep the actual gl* block + frag `layout(location=)`.

## KEY FILES / GATES
- `GameOS/gameos/render_frame_plan.h` (header-only PassTrace) · `pipeline_binder.cpp`
  (applyPipeline; gated colorMask emit; opt-in `rowOwnsColorMask`) ·
  `gos_postprocess.cpp` beginScene keystone + composite/fog/multiply routing + traces ·
  `gos_terrain_lod_chunk.cpp` terrain solid route · `txmmgr.cpp:2868` dispatch chokepoint trace.
- Checkers (in check-contracts.sh): `colormask_ownership` (black-frame guard), `multiply_blend`,
  `vfx_blend_distinction`, plus pipeline_desc/key/pass_coverage.
- Ledger: `docs/render-backend-seams/pipeline-pass-coverage-ledger.{json,md}` (Terrain solid now
  ROUTED; overlays/decals/mask/water/mine/surface + legacy MLR + water-fast still SEPARATE).
- Gates: `MC2_RENDER_FRAME_PLAN_TRACE` `MC2_PIPELINE_COLORMASK` `MC2_PIPELINE_BIND_TRACE`
  `MC2_TERRAIN_CULL_PROBE` (all default OFF, allowlisted).
- Banked fixture: `tests/visual/bookmarks/mc2_01_terrain_solid.json` (land poses; for future
  RenderDoc/armed-frame terrain work).

## NEXT MENU (pick ONE, fresh lane)
1. **COLORMASK full rollout** — now keystone is in, opt-in more passes (each must keep color0=true;
   checker enforces). Cheap, unblocks more PipelineDesc ownership.
2. **FRAME-RESOURCE-LEDGER-1** — depth/normal/color-copy + MRT/draw-buffer ownership (audit class
   FRAME_RESOURCE_LEDGER). Sibling already started `.claude/FRAME-RESOURCE-LEDGER-1.md` — COORDINATE.
3. **Remaining terrain-family routing** — overlays/decals, mask/water/mine/surface, water fast path
   (separate slices; reuse the lod-chunk recon->route->frame-trace pattern).
4. **VFX visual proof** — VFX is ROUTED but proof-pending (nondeterministic); needs a deterministic
   fixture or oracle. Siblings added VFX-DISTORTION/SCENECOLOR-GRAB/BLACKBODY — coordinate.

## DISCIPLINE
Honest ledger ONLY (two byte-gates caught real regressions this arc — trust them). Don't promote a
pass without bind-positive + byte/visual proof. Don't route legacy MLR. Don't claim "all terrain
routed" — only solid lod-chunk is.
