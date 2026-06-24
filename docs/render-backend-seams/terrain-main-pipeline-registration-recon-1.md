# RECON — TERRAIN-MAIN-PIPELINE-REGISTRATION-RECON-1

Read-only recon (no code). Question: register the MAIN SOLID terrain pass as a
`RenderCore::PipelineDesc` row routed via applyPipeline? Ledger `Terrain` =
UNREGISTERED. Verdict: **registerable as ONE row, but DEFER routing until two
load-bearing blockers are resolved** (cull-state unknown; colorMask repair).

## Live path
Default = **GPU-indirect "thin" MDI**: `glMultiDrawArraysIndirect` at
`gameos_graphics.cpp:4251`, in `gos_terrain_bridge_drawIndirect` (3943). Shaders
`gos_terrain_thin.vert` (SSBO/gl_VertexID, no attribs) + `gos_terrain.frag`.
Chain: txmmgr `renderLists()` → `gos_terrain_indirect::DrawIndirect()` (txmmgr.cpp:2884)
→ bridge. Gate `MC2_TERRAIN_INDIRECT` **default-ON**; arm predicate `ShouldArmGpuTerrain`
(IsTerrainSolidEnabled + non-empty nodeIds + atlas tex). Un-armed M2
`TerrainPatchStream::flush()` shares the same shaders/state (folds into the same row).
Other paths: LOD-chunk (`MC2_TERRAIN_LOD_CHUNK=1`, opt-in, mutually exclusive, draw
`gos_terrain_lod_chunk.cpp:887`) — SEPARATE row; legacy CPU quad (`mclib/quad.cpp`) —
DEAD when armed.

## VERIFIED draw-site state (gameos_graphics.cpp:4005-4013), save/restored 3959-4305
- depthTest ON; depthFunc **GEQUAL** (reverse-Z, explicit); depthMask TRUE
- blend OFF (opaque)
- `glColorMask(TRUE,TRUE,TRUE,TRUE)` at 4011 — **repairs** a prior shadow-pass
  `glColorMask(FALSE,…)` (gos_postprocess.cpp:1134/1156); without it the indirect path
  draws NOTHING the frame after a shadow pass.
- **cull / frontFace: NEVER SET in the bridge (3943-4307) — inherits ambient.** ✗ unknown
- colorAttachments: `gos_terrain.frag` writes **location=0 FragColor AND location=1
  GBuffer1** (lines 30,32) → **{color0=true, color1=true, color2=false}**. (NOTE: the
  agent recon's "single-attachment, color0 only" was WRONG — verify-caught at frag:32.)
  objectId (color2) NOT written → objectIdWriteEnabled=false.

## Proposed row (live indirect main-solid) — INCOMPLETE pending blocker #1
```
glProgramName        = 0
blend                = Opaque
depthTestEnable      = true
depthWriteEnable     = true
depthFunc            = GreaterEqual          (reverse-Z, already explicit)
cullMode             = ??? BLOCKER #1        (draw never sets it; inherits ambient)
frontFace            = ??? (inherited; unspecified)
colorAttachments     = { true, true, false } (color0 + GBuffer1)
objectIdWriteEnabled = false
polygonOffsetEnable  = false
```

## BLOCKERS (must resolve before routing)
1. **Cull-mode is undefined at the draw site.** The bridge never sets/saves
   GL_CULL_FACE/glFrontFace — terrain renders under whatever the previous pass
   (mech/shadow/postprocess) left. Naming a cullMode in the row converts
   "inherit ambient" → "explicit set", which CHANGES behavior. The thin VS emits
   world-space tris whose winding is not guaranteed CCW-consistent (degenerate
   culled-record quads), so a wrong cullMode/frontFace could blank or half-cull
   terrain. **Must determine empirically first**: instrument `glIsEnabled(GL_CULL_FACE)`
   + `GL_CULL_FACE_MODE` + `GL_FRONT_FACE` at gameos_graphics.cpp:4004 on a live armed
   frame (or RenderDoc). THE load-bearing unknown.
2. **applyPipeline does NOT set glColorMask.** The binder applies depth/blend/cull/
   frontFace/polyOffset only — colorAttachments is DESCRIPTIVE (draw-buffer concern),
   not emitted as glColorMask. So routing terrain naively would DROP the load-bearing
   `glColorMask(TRUE)` shadow-leak repair (4011) → the post-shadow blank-frame bug
   returns. The repair line must STAY at the call site alongside applyPipeline, OR the
   binder must learn to emit glColorMask from colorAttachments (a binder change with
   wider blast radius — every routed pass would then assert colorMask).

## RECOMMENDED SEQUENCE
1. **TERRAIN-CULL-STATE-PROBE-1** (tiny, gated): instrument the ambient cull/frontFace
   at gameos_graphics.cpp:4004 under `MC2_TERRAIN_CULL_PROBE`, capture on a live armed
   mc2_01/mc2_24 frame. Pin cullMode + frontFace empirically. (Resolves blocker #1.)
2. **TERRAIN-SOLID-PIPELINE-REGISTRATION-1** (DESCRIPTIVE row, like WaterArmed/shadows
   were): add PipelineId::TerrainSolid with the probed cull + {t,t,f} colorAttachments.
   glProgramName=0, NOT routed yet. States the truth; moves the ledger to DESCRIPTIVE.
3. **TERRAIN-SOLID-APPLYPIPELINE-ROUTING-1**: route the draw via applyPipeline, KEEP the
   glColorMask(TRUE) repair at the call site (blocker #2), byte-gate via the harness
   (terrain is deterministic — water_overview frame already exercises it; expect
   sha cb5a700e to hold).
4. SEPARATE later: LOD-chunk row (cullMode=None per gos_terrain_lod_chunk.cpp:592,
   colorAttachments color1=true, MRT GBuffer1) — opt-in path, its own slice.

## DO NOT
- Cover indirect + LOD-chunk with one row (different cull + the LOD path is opt-in).
- Route before the cull probe (risk: blanked/half-culled terrain).
- Assume single-attachment (terrain writes GBuffer1).
