# RECON-2 — TERRAIN-SOLID-LIVE-DRAWPATH-RECON-2

Read-only. Triggered by ROUTING-1 PARTIAL: applyPipeline(TerrainSolid) added at the
indirect bridge, but [PIPELINE_BIND] TerrainSolid binds=0 in capture (water +
werewolf-armed). Question: what actually draws terrain solid in the capture harness,
and where should TerrainSolid be routed/proven?

## Path topology (verified, file:line)
txmmgr `renderLists()` terrain dispatch (mclib/txmmgr.cpp:2895):
```
if (IsFrameSolidArmed())              -> DrawIndirect()  (else LOD-chunk owns it)
else if (PatchStream::isReady()...)   -> TerrainPatchStream::flush()
// neither handled -> modernHandled=false -> LEGACY MLR draw (loop @ 2931)
```
- **DrawIndirect()** (gos_terrain_indirect.cpp:3639) gates `if (!IsFrameSolidArmed()) return`,
  then calls **gos_terrain_bridge_drawIndirect** (3653) — the MODERN bridge, where
  ROUTING-1's applyPipeline(TerrainSolid) lives (gameos_graphics.cpp ~4005, draw ~4276).
- **IsFrameSolidArmed()** (2491) = a CAMERA-WINDOWED per-frame gate (header §108:
  "Camera-windowed solid dispatch gate"); armed only when visible solid terrain quads are
  in the camera window. Reset each frame by endFrame(). `first_arm` logs ONCE at first arm,
  NOT per-frame — so a logged first_arm does NOT mean the captured frame is armed.
- **IsTerrainSolidEnabled()** (gpu_driven_common.cpp:63) = DEFAULT-ON (IsGlobalEnabled() &&
  MC2_GPU_DRIVEN_TERRAIN_SOLID != "0"). Not the blocker.
- **Legacy MLR** (txmmgr.cpp:2931 masterVertexNodes loop, gos_SetRenderState-driven; line
  2936 skips it `if (modernHandled && MC2_ISTERRAIN)`) = the un-armed fallback. Uses the
  legacy render-state abstraction, NOT PipelineDesc — never binds applyPipeline.
- **3528** (gameos_graphics.cpp) = WATER MDI (`WaterStream::GetIndirectCmdBuffer`, reflection
  RT, water_fast). NOT terrain. (Corrects the ROUTING-1 hypothesis.)

## What the capture exercised (empirical)
- water_overview_high (alt 1900, all water): not solid-armed -> legacy MLR.
- werewolf (ground, alt 260): `first_arm` logged, but captured frame `TerrainSolid` binds=0
  -> that frame was NOT solid-armed either -> legacy MLR.
- Cull probe corroborated: only the dispatch CHOKEPOINT (txmmgr:2868) fired; per-path probes
  at the bridge AND patch-stream flush never fired -> neither modern path ran in capture.

## CONCLUSION
ROUTING-1 routed the **correct (modern) pipe**. It is not dead — it is the real-gameplay
terrain-solid path. It binds 0 in capture ONLY because the two existing bookmarks do not
camera-window-arm solid terrain, so capture falls through to the (deprecated) legacy MLR
path. The legacy MLR path must NOT be routed (it is the thing being replaced).

## PATH FORWARD (revises the "route a different chokepoint" plan)
The fix is NOT a new route target — it is a **solid-armed bookmark** that makes the bridge
fire in deterministic capture:
1. **TERRAIN-SOLID-ARMED-BOOKMARK-1**: author an mc2_01 (or mc2_24) bookmark at ground level
   FACING a solid-terrain expanse (not water, not sky) so IsFrameSolidArmed() is true at the
   capture frame. Verify by [PIPELINE_BIND] TerrainSolid binds>0 with MC2_PIPELINE_BIND_TRACE.
2. **TERRAIN-SOLID-APPLYPIPELINE-ROUTING-2 = re-gate ROUTING-1** on that bookmark: byte/visual
   no-regression + TerrainSolid binds>0 + state matches the probed row + colorMask repair
   intact -> THEN promote TerrainSolid to ROUTED_BY_APPLYPIPELINE.
3. If no bookmark can arm in the deterministic sweep (camera-window gate too restrictive under
   sim-freeze), fall back to a RenderDoc capture on a real armed gameplay frame (needs explicit
   launch approval) as the proof hook.

## DO NOT
- Route the legacy MLR path (deprecated; do not extend its life).
- Remove the bridge routing (ROUTING-1) — it is the correct modern target, just unproven.
- Promote to ROUTED until TerrainSolid binds>0 on a proof frame.
- Trust `first_arm` as evidence the captured frame is armed (it is a once-only banner).
