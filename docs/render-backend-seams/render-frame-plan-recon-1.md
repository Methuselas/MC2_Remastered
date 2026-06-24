# RECON — RENDER-FRAME-PLAN-RECON-1

Read-only. Documents HOW THE FRAME ACTUALLY DRAWS RIGHT NOW (not how it should), so a
governance "frame plan" layer can be built on real topology instead of assumptions.
This session repeatedly stepped on the same rake (terrain MLR-vs-bridge, water frame-
windowing, colorMask outside applyPipeline, capture not exercising the assumed path).
Make the frame describe itself first. NO code, NO reorder.

## A. Actual per-frame draw ORDER (in-mission)
Driver: `gameosmain.cpp` frame body → `Environment.UpdateRenderers()` → `gamecam.cpp`
render path → `txmmgr.cpp renderLists()` → `gos_postprocess.cpp endScene()`.

| # | Phase | Where | Notes |
|---|---|---|---|
| 0 | Scene FBO begin + clear (reverse-Z clearDepth=0) | gameosmain.cpp:510/563; clearGBuffer1 :569 | sky/black clear reads PREVIOUS frame terrain flag :554 |
| — | Skybox | gameosmain.cpp:572 **COMMENTED OUT** | DEAD in this worktree — model ABSENT, not deferred |
| 1 | gamecam world ENQUEUE | gamecam.cpp:455-499 | most calls enqueue; **ObjectManager :473 draws MLR immediately** |
| 2 | Terrain LOD-chunk flush | gamecam.cpp:508 | no-op unless MC2_TERRAIN_LOD_CHUNK |
| 3 | **renderLists() main submit** | gamecam.cpp:513 → txmmgr.cpp:2251 | see §B |
| 4 | Water reflection RT | gamecam.cpp:522 | gated MC2_WATER_REFLECTION_RT |
| 5 | **Water fast path MDI** | gamecam.cpp:530 → gameos_graphics.cpp:3528 | gated; then gos_InvalidateRenderStateCache |
| 6 | Vegetation cards | gamecam.cpp:554 | gated MC2_VEGETATION_CARDS |
| 7 | GPU particles + tube ribbons | gamecam.cpp:585/594 | default-ON |
| 8 | MLR FX clipper | gamecam.cpp:613 | legacy immediate draw |
| 9-10 | Weather / DebugRenderer | gamecam.cpp:620/694 | legacy / gated |
| 11 | **PostProcess endScene()** | gameosmain.cpp:594 → gos_postprocess.cpp:2124 | see §C |
| 12 | HUD batch replay (UI) | gameos_graphics.cpp:7584 | 2D deferred-recorded, replayed at frame end |
| 13 | Present | gameosmain.cpp | — |

### §B renderLists() internal order (txmmgr.cpp:2251)
R0 preamble state+fog+cull CW (2261-2309) · R1 light SSBO (2312) · R2 scene UBO (2352) ·
**R3 Render.3DObjects hardware queue** ShapeRenderer mech/veh/bldg (2377-2439) · R4 static
full-map shadow build once (2472) · R5 StaticPropRegistry flush (2548) · **R6 dynamic shadow
pass** caster cull + instanced draw + CSM (2569-2820) · **R7 Render.TerrainSolid** indirect-
vs-legacy (2836-3003, see §E) · R8 GPU static props compute+MDI (3012-3120) · R9 GPU mechs
(3120) · R10 terrain mask solid (3139) · R11 terrain overlays + static-decal indirect (3152) ·
R12 terrain mines (3177) · R13 decals/overlays (3184) · R14 legacy terrain alpha+water loops
(3207-3337) · R15 legacy shadow blobs (3345) · R16 legacy non-terrain alpha ×2 (3417-3488).
NOTE: R3-before-R7 and the relocated flush (2444-2447, 3006) are DEPTH-FRAGILE explicit
orderings (TerrainSolid overwrote building pixels otherwise) — pin them, don't infer.

### §C endScene() post order (gos_postprocess.cpp:2124)
runHzbReduce → cluster_depth_pyramid::Run → lightgrid_build::Run → **runScreenShadow →
runCloudShadow → runShoreline → runSSAO → runEdgeFog → runFogOob** → bind FB0 + FORCE-43
viewport → **composite**. All fullscreen passes = applyPipeline + glDrawArrays(TRIANGLES,0,6).

## D. Chokepoint classification (path · state owner · PipelineDesc · capture? · proof)
Legend: M-AP modern applyPipeline · M-UR modern unrouted (raw GL) · MLR legacy master-node ·
RAW legacy raw GL · BAT batcher · CMP compute-feeds-draw.

| chokepoint | path | state owner | PipelineDesc | capture | 
|---|---|---|---|---|
| mech batcher `glDrawElementsInstancedBaseVertex` (gos_mech_batcher.cpp:1004/2454) | BAT | **applyPipeline** | **ROUTED** (MechOpaque) | yes |
| static props `glMultiDrawElementsIndirect` (gos_static_prop_batcher.cpp:6051) | BAT/CMP | **applyPipeline** | **ROUTED** (StaticProp{Opaque,AlphaTest,Depth}) | variable |
| endScene composite + 6 post passes (gos_postprocess.cpp) | M-AP | **applyPipeline** (+ raw colorMask/viewport) | **ROUTED** | yes (gated subset) |
| terrain SOLID indirect MDI (gameos_graphics.cpp:4257, via DrawIndirect) | CMP→M-UR | raw GL | none (descriptive TerrainSolid) | **NO** (needs IsFrameSolidArmed) |
| water MDI fast path (gameos_graphics.cpp:3528 WaterStream) | M-UR | raw GL + invalidate | descriptive (WaterArmed) | variable |
| terrain mask solid/water indirect (4490/4649), surface (3889), mines (4768) | M-UR | raw GL | none | variable (arming-gated) |
| **legacy terrain solid MLR** (txmmgr.cpp:2931-2993 gos_RenderIndexedArray) | MLR | gos_SetRenderState | none | **YES** (capture un-armed hits THIS) |
| hardware-queue objects R3 (txmmgr.cpp:2426 ShapeRenderer) | BAT | gos_SetRenderState+ShapeRenderer | none | yes |
| legacy non-terrain alpha (txmmgr.cpp:3451/3467), shadow blobs (3345) | MLR | gos_SetRenderState | none | yes |
| terrain overlays/decals (gameos_graphics.cpp:9772/9893/9967) | RAW | raw GL | none | yes |
| tessellated patches (gameos_graphics.cpp:2794/6278) | MLR | raw GL | none | variable |
| GPU cull dispatch (gpu_cull_compute.cpp:1113/1338/1388) | CMP | raw GL | none | variable |
| HUD 2D (gameos_graphics.cpp:7653) | RAW | renderStates_ snapshot replay | none | yes |
| ObjectManager MLR / MLR FX clipper (gamecam.cpp:473/613) | MLR | gos_SetRenderState | none | yes |

## E. THE structural smell — terrain SOLID is path-variable
`txmmgr.cpp:2895-2916`: `if (IsFrameSolidArmed()) DrawIndirect()  [modern bridge]
else if (PatchStream::isReady()) flush()  else -> LEGACY MLR (2931)`. `IsFrameSolidArmed()`
is CAMERA-WINDOWED. Deterministic capture/smoke does NOT arm → capture draws terrain via
LEGACY MLR. Any frame plan modeling only the modern bridge will NOT match capture. The plan
MUST encode BOTH branches + which one capture hits.

## F. Ambient state NOT owned by applyPipeline (the parallel ledger the scaffold needs)
applyPipeline sets only depth/blend/cull/frontFace/polyOffset. Outside it:
- **colorMask**: per-attachment glColorMaski in skybox/shadow/hzb helpers; **the load-bearing
  terrain shadow-leak repair** — shadow pass sets FALSE, terrain re-asserts `glColorMask(TRUE)`
  (gos_postprocess.cpp:3150/3172/3181 + the terrain bridge :4011). Reorder/wrap strands it FALSE.
- **MRT/draw-buffer masks**: glDrawBuffers(1/3/2) vs sceneFBO_ (gos_postprocess.cpp:225/235/241);
  GBuffer1 sentinel cleared gameosmain.cpp:569; objectId/Thermal viewmode depends on sceneObjectIdTex_.
- **viewport**: raw everywhere (gameosmain + every post helper + txmmgr:2407/2453).
- **scissor / stencil**: no per-pass owner; stencil cleared once (gameosmain.cpp:563); inherited.
- **VAO**: endScene saves/restores caller VAO; HUD rebinds gVAO.
- **tex-unit bindings**: only composite unit-0/2 are guard-wrapped; invalidate does NOT track them.
- **Cross-phase latches**: `markTerrainDrawn()`/`sceneHasTerrain_`/`prevFrameHadTerrain_` — set
  during terrain draw, READ by screenShadow/shoreline/edgeFog + clear color. Terrain taking a
  path that skips the latch silently kills 4 post passes (documented landmine).

## G. Legacy escapes — KEEP / ROUTE-CANDIDATE / DO_NOT_MODEL
- **KEEP**: legacy terrain solid MLR (txmmgr:2931 — what capture exercises; do NOT delete until
  indirect arms in capture); hardware-queue objects R3; legacy shadow blobs; HUD batch.
- **ROUTE-CANDIDATE**: legacy non-terrain alpha loops (txmmgr:3417); terrain overlays/decals
  (gameos_graphics:9772/9893); plus the modern-UNROUTED cluster (terrain indirect, water MDI,
  mines, surface, mask draws) — highest-value PipelineDesc targets but arming-gated, so parity-
  proving them needs FORCED arming in capture.
- **DO_NOT_MODEL (v1)**: MLR FX clipper, ObjectManager immediate MLR, weather/compass, skybox
  (dead-commented), editor/mechbay preview.

## H. Top risks for the scaffold
1. Capture/smoke divergence is STRUCTURAL (terrain solid path-variable; capture = legacy branch).
2. colorMask invisible to applyPipeline + self-repairs by side effect → needs a parallel ambient ledger.
3. markTerrainDrawn/sceneHasTerrain_ cross-phase latches gate 4 post passes.
4. Post-fx samples previous color/depth target; composite "force opaque" depends on no additive leak.
5. GBuffer1 attachment-mask + objectId sentinel = a separate state machine.
6. compute→GL_COMMAND_BARRIER→indirect dependency (mis-order → zero/stale instances).
7. Intra-renderLists order is depth-fragile (relocated flushes; R3-before-R7) — pin, don't infer.
8. Mode axis: in-mission vs menu vs editor have DIFFERENT chokepoint sets.

## NEXT (per advisor)
RENDER-FRAME-PLAN-SCAFFOLD-1 (data/logging only, no behavior change): RenderPhase enum +
per-frame PassTrace (`[FRAME_PLAN] phase=... pass=... path=... draws=N pipeline=...`) so the
frame self-reports which path drew — would have caught the terrain confusion immediately.
Then PIPELINE-STATE-OWNER-AUDIT-1 (colorMask/scissor/stencil/viewport first-class) +
LEGACY-DRAWPATH-LEDGER-1. Do NOT reorder passes or delete legacy paths cold.
