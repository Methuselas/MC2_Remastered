# RECON — POSTPROCESS-PIPELINE-REGISTRATION-RECON-1

Read-only recon (no code). Question: which `gos_postprocess.cpp` fullscreen passes
should become `RenderCore::PipelineDesc` rows (routed via applyPipeline) vs
DO_NOT_MODEL, and what blocks each. Ledger pass `PostProcess` = UNREGISTERED.

## Shape
`GameOS/gameos/gos_postprocess.cpp` (192 KB). **14 fullscreen draws across 12
distinct passes.** `endScene()` (gameosmain.cpp:594) drives the COMPOSITE CHAIN;
the skyboxes are separate public methods the renderer calls earlier (caller binds
scene MRT). All passes: `quadVAO_`, cull OFF, depthTest OFF, depthMask FALSE.
Composite-chain passes draw into `sceneFBO_` via `setSceneDrawBuffers(SingleColor)`
→ attachment 0 only (so attachment 1/2 masking is moot). Chain order (endScene,
L2145-2310): hzbReduce → hzbProbe → cluster_depth_pyramid → lightgrid_build →
screenShadow → cloudShadow → shoreline → SSAO(1,2) → edgeFog → fogOob →
[bind FB0 + FORCE-43] → composite → shadowDebugOverlay → InvalidateRenderStateCache.

## VERIFIED facts (not assumptions)
- `BlendMode` enum (RenderCore/PipelineDesc.h:42) = {Opaque, AlphaBlend, AlphaTest,
  Additive(legacy), AdditiveOneOne, AdditiveSrcAlphaOne}. **No Multiply.**
- Composite draw (L2303) is FORCE-OPAQUE — `glDisable(GL_BLEND)` at L2213 with a
  documented reason (gosFX additive ONE/ONE leak saturates RGBA8 to white).
- MULTIPLY blend `GL_DST_COLOR,GL_ZERO` at L1721/1784/1929/1984 (ssaoApply,
  ssao-debug, screenShadow, cloud) + shoreline.
- `colorAttachments {bool,bool,bool}` is per-ATTACHMENT enable (whole attachment).
  Every `glColorMaski` here is all-4-channels on/off (L2474/2699/2883), i.e. exactly
  `{true,false,false}`. No per-channel RGBA masking anywhere in scope → PipelineDesc
  suffices; no enum extension needed for masking.

## Verdicts

### REGISTER NOW — low risk (set-only state, Opaque/AlphaBlend, SPIR-V pilots)
| Pass | shader | draw | BlendMode | depthFunc | cull | colorAttach |
|---|---|---|---|---|---|---|
| composite (endScene) | `postprocess.frag` | 2303 | Opaque | Always | None | {t,f,f} |
| edgeFog | `edge_fog` | 2054 | AlphaBlend | Always | None | {t,f,f} |
| fogOob | `fog_oob` | 2117 | AlphaBlend | Always | None | {t,f,f} |
Composite is the canonical "post-fx pipeline" row + lowest risk — start there.

### BLOCKED — need `BlendMode::Multiply` (GL_DST_COLOR/GL_ZERO) added first
screenShadow (`shadow_screen`, L1877), cloudShadow (`cloud`, L1952), shoreline
(`shoreline`, L2002), ssaoApply (`ssao_apply`, L1735). All set-only, ALWAYS/None,
{t,f,f}. Prereq slice: add `Multiply = 6` to BlendMode + the pipeline_binder
glBlendFunc(DST_COLOR,ZERO) case + check-pipeline-key allowance. **ssaoApply also
lacks a SPIR-V artifact** (see SPIR-V gap).

### DEFER
- SSAO pass1 (`ssao`, L1712) + hzbReduce (`hzb_reduce`, L1269): draw into OWN FBO
  at a DIFFERENT viewport (half-res / per-mip loop). PipelineDesc models no FBO
  target/viewport → not an applyPipeline shape until the binder owns a target.
- HDRI skybox family (`hdri_skybox`: renderHdriSkybox L2588, Basis L2782, InvVP
  L2998 — THREE feeders, ONE program): does full SAVE-AND-RESTORE-PREVIOUS of all
  GL state incl. the per-attachment masks. applyPipeline is set-don't-restore →
  registering it would DROP the restore and leak colorMaski-1/2-off into later MRT
  geometry. Reconcile the restore contract first. Also no SPIR-V artifact.

### DO_NOT_MODEL
- drawShadowDebugOverlay (`shadow_debug`, L2376): debug overlay, cornered viewport,
  mutates GL_TEXTURE_COMPARE_MODE mid-pass.
- renderSkybox (`skybox`, L2408): **dead call site** (gameosmain.cpp:572 commented).

## SPIR-V pilot gap (shaders/spv/spirv_index.json, nifty worktree)
PILOTS: postprocess(composite), ssao, fog_oob, edge_fog, hzb_reduce, cloud,
shoreline. **NOT pilots:** skybox, hdri_skybox, shadow_screen, shadow_debug,
**ssao_apply**. So screenShadow, ssaoApply, HDRI need a precompile pass before they
can ride a SPIR-V keyed-variant consumer.

## Load-bearing risks (for the eventual registration slice)
1. **Shared programs.** `postprocess.vert` is shared across composite/screenShadow/
   cloud/shoreline/ssao/ssaoApply/edgeFog/fogOob/hzbReduce/shadowDebug; `hdri_skybox`
   across 3 feeders. A program-name-keyed PipelineDesc cannot disambiguate — key on
   the FRAG base, not the vert.
2. **Set-only vs save/restore.** Composite chain hard-SETs + leans on terminal
   InvalidateRenderStateCache (matches applyPipeline). HDRI save/restores (does NOT).
   Only the set-only passes are applyPipeline-shaped.
3. **Multiply enum gap** blocks 4 of the 7 chain passes.

## RECOMMENDED ORDER (next slices, smallest-blast-radius first)
1. **POSTPROCESS-COMPOSITE-REGISTRATION-1** — register composite (Opaque) + route
   endScene L2303 via applyPipeline. Byte-gate via the existing visual harness
   (deterministic, SPIR-V pilot). Proves the post-fx pipeline row.
2. **POSTPROCESS-FOG-REGISTRATION-1** — edgeFog + fogOob (AlphaBlend, pilots).
3. **BLENDMODE-MULTIPLY-1** (prereq) → then screenShadow/cloud/shoreline/ssaoApply.
4. DEFER HDRI (restore-contract) + ssao1/hzb (FBO/viewport) + ssaoApply SPIR-V.
DO_NOT_MODEL: shadowDebug, dead skybox.
