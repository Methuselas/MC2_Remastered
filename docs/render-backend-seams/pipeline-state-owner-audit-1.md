# AUDIT — PIPELINE-STATE-OWNER-AUDIT-1

Read-only. The frame-plan scaffold answers "what drew." This audit answers "who owns the
INVISIBLE state" — the GL state applyPipeline does NOT set (colorMask, MRT/draw-buffers,
viewport, scissor, stencil, VAO, tex-units, cross-phase latches). NO code, NO reorder.

applyPipeline (pipeline_binder.cpp) owns ONLY: depthTest, depthMask, depthFunc, blend
(+factors), cull, frontFace, polygonOffset-enable. Everything below is outside it.

Classification vocabulary: KEEP_RAW_FOR_NOW · MOVE_TO_PIPELINEDESC_LATER ·
FRAME_RESOURCE_LEDGER (target/attachment binding, not fixed-function) · PASS_TRACE_FIELD
(record per-frame in the scaffold) · DO_NOT_MODEL · DANGEROUS_SIDE_EFFECT.

## 1. colorMask  → MOVE_TO_PIPELINEDESC_LATER (+ DANGEROUS_SIDE_EFFECT) — **first-class #1**
Writers (gameos_graphics.cpp): terrain bridge repair `4022 glColorMask(TRUE)` = the
load-bearing M5 terrain-shadow-leak repair; mask/water indirect save→FALSE→restore epilogues
(3809/3916, 4392/4527, 4605/4664); mine 4766/4784; shadow bracket 6052; per-attachment
skybox/HDRI `glColorMaski(0=RGBA,1/2=off)` (gos_postprocess.cpp 2492-2494/2717-2719/2901…,
restored 2616/2810).
- The bridge `glColorMask(TRUE)` repair is **DANGEROUS_SIDE_EFFECT**: it undoes a prior
  shadow-pass `glColorMask(FALSE)`; without it the terrain draw is silently black the frame
  after a shadow pass. applyPipeline does not set colorMask, so it MUST stay explicit until
  the binder owns it. We preserved it through TERRAIN-SOLID-ROUTING-1 for exactly this reason.
- PipelineDesc already has `colorAttachments {bool,bool,bool}` = per-ATTACHMENT enable, which
  models the whole-attachment skybox masks ({t,f,f}) EXACTLY but NOT per-channel RGBA or the
  imperative save/restore epilogues.
- **VERDICT:** colorMask is the highest-value first-class candidate. Path: teach applyPipeline
  to emit `glColorMask`/`glColorMaski` from `colorAttachments` (whole-attachment only), then
  retire the per-attachment writers + the terrain repair INTO the row. The save/restore
  epilogues (indirect bridges) become unnecessary once every pass asserts its own mask.
  Blast radius: every routed pass starts asserting colorMask — needs a careful gated rollout.

## 2. MRT / draw-buffers (setSceneDrawBuffers)  → FRAME_RESOURCE_LEDGER
`setSceneDrawBuffers(SingleColor=1 | MainSceneMRT=3)` is the single policy chokepoint
(gos_postprocess.cpp:205-242). Post passes bind SingleColor (1 attachment); scene draws bind
the 3-entry MRT (color/normal/GBuffer1). Also gos_particle_bridge 682/805 (tube ribbon forces
1 buffer, restores). This is a RENDER-TARGET/attachment concern, NOT per-pipeline fixed-
function — model as a frame-resource binding (which attachments are live this pass), separate
from PipelineDesc. Overlaps `colorAttachments` semantically but the actual `glDrawBuffers` is
FBO-level. KEEP_RAW until a PassAttachmentDesc exists.

## 3. viewport  → KEEP_RAW_FOR_NOW (candidate FRAME_RESOURCE_LEDGER)
21 `glViewport` in gos_postprocess (every post helper; SSAO is HALF-RES `ssaoW_/ssaoH_`;
composite is FORCE-43-boxed), 4 in gameos_graphics, 0 in txmmgr. Genuinely per-pass-variable
(half-res, letterbox) — NOT fixed-pipeline state. Keep raw; later a PassAttachmentDesc could
carry a viewport policy. PASS_TRACE_FIELD candidate (record the active viewport per pass).

## 4. scissor  → DO_NOT_MODEL
No `glScissor` in the hot path (postprocess/renderLists). Nobody owns it; inherited/unused.

## 5. stencil  → DO_NOT_MODEL
Cleared once (gameosmain.cpp:563 GL_STENCIL_BUFFER_BIT); no per-pass `glStencil*` in the hot
path. Not used as a render feature. Revisit only if a pass introduces stencil.

## 6. VAO  → KEEP_RAW_FOR_NOW
`quadVAO_` for fullscreen passes; endScene saves/restores the caller VAO; HUD rebinds gVAO;
`gos_RendererRebindVAO()` for post-endScene callers. VAO is a GEOMETRY-SOURCE binding,
orthogonal to fixed-function pipeline state — does not belong in PipelineDesc. Keep raw.

## 7. texture-unit bindings  → KEEP_RAW_FOR_NOW (+ DANGEROUS_SIDE_EFFECT for unguarded)
`GlScopedTextureUnit` RAII guards exist at composite unit-0/2 (gos_postprocess.cpp:2303/2304)
and decals/overlays (gameos_graphics.cpp:9765/9887/9960). `gos_InvalidateRenderStateCache`
does NOT track tex units, so any UNGUARDED tex-unit bind can leak into a later 2D/menu/HUD
consumer (the GLSTATE-TEXUNIT class of bugs). RAII guard is the correct pattern (NOT
PipelineDesc — sampler bindings are material/resource state). Hazard = the unguarded sites;
KEEP_RAW but extend guard coverage as leaks surface.

## 8. markTerrainDrawn / sceneHasTerrain_ / prevFrameHadTerrain_  → PASS_TRACE_FIELD (+ DANGEROUS_SIDE_EFFECT)
Cross-phase latch. Writers (ALL 3 terrain paths): lod-chunk `gos_terrain_lod_chunk.cpp:931`,
legacy `gameos_graphics.cpp:7170`, patch-stream `gos_terrain_patch_stream.cpp:1500`. Readers:
screenShadow(1678) cloud(1916) shoreline(1977) edgeFog(2027) fogOob(2084) + clear color
(gameosmain.cpp:557, reads PREVIOUS frame). `prevFrameHadTerrain_=sceneHasTerrain_` then reset
each frame (gos_postprocess.cpp:1158-1159).
- **DANGEROUS_SIDE_EFFECT:** a terrain path that forgets `markTerrainDrawn()` silently kills
  5 post passes + flips the clear color. The lod-chunk path had to add the call EXPLICITLY
  (`:926-931` comment: legacy sites don't fire under lod-chunk) — proof this landmine already
  bit once. The 8z cutover narrowly avoided a regression here.
- **VERDICT:** make this a first-class PASS_TRACE_FIELD in the scaffold (emit
  `sceneHasTerrain=0/1` + which path set it) so a future terrain re-route can't silently
  strand the post chain. Do NOT fold into PipelineDesc (it's a frame-flow latch, not pipeline
  state).

## RECOMMENDATION — first state to make first-class
**colorMask.** It is (a) load-bearing (the terrain shadow-leak repair), (b) already half-
modeled (`colorAttachments`), (c) the highest-risk strand-on-reorder, and (d) the blocker
that forced us to keep raw GL at the terrain routing site. Making applyPipeline emit
colorMask from colorAttachments (whole-attachment, gated rollout) is the highest-leverage
state-ownership win — and it directly unblocks a clean TerrainSolid (lod-chunk) routing.

Secondary, cheap, and high-value: add `sceneHasTerrain` + terrain-path to the scaffold's
PassTrace (PASS_TRACE_FIELD) — the latch landmine is the single most likely silent regression
for the upcoming terrain re-route.

## NEXT (per advisor)
1. PIPELINE-COLORMASK-OWNERSHIP-1 (gated: applyPipeline emits colorMask from colorAttachments;
   retire the terrain repair + save/restore epilogues into the rows). First-class colorMask.
2. TERRAIN-LODCHUNK-PIPELINE-ROUTING-RECON-1 (the real terrain target, now that the live path
   is known and colorMask ownership is on the table).
Do NOT route TerrainSolidLODChunk before colorMask ownership — lod-chunk is RawGL and likely
entangled with the colorMask/MRT state this audit just classified.
