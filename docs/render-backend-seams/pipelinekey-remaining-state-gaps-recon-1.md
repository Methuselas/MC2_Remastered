# PIPELINEKEY-REMAINING-STATE-GAPS-RECON-1

**Type:** RECON (no build, no code change). Inventories every PSO-state axis and
unregistered draw pass still NOT authoritative after the shaderVariant /
vertexLayout / frontFace / cull / depth / polygonOffsetEnable work, and classifies
each: **PROMOTE NOW · PROMOTE LATER · DO NOT MODEL · NEEDS PASS REGISTRATION FIRST**.

Scratch evidence: `.claude/PSOGAPS-RECON-axes.md`, `.claude/PSOGAPS-RECON-passes.md`.

**Bottom line:** the PSO *state vocabulary* is nearly complete — only **one** new
field is even a candidate (`colorWriteEnable`, 1 bit). The remaining value is in
**registering more passes**, not adding fields. Highest-value next slice =
register the two stable terrain passes (TerrainOverlay, TerrainDecal), which
already have RenderPassIds and finite state — pure metadata, same proven pattern.

---

## A. State axes

### PROMOTE NOW
*(nothing new — the promotable state axes are already done)*
- **depthWrite, depthFunc, depthTest** — already modeled + applied authoritatively
  (`pipeline_binder.cpp:36-49`). `depthFunc` genuinely varies (GEQUAL scene vs
  LESS shadow), already proven. No work.

### PROMOTE LATER
- **`colorWriteEnable` (1-bit, all-or-nothing RGBA)** — the ONLY new field
  candidate; fits the last `PipelineDesc` padding byte (`sizeof<=20` holds).
  Today color-off is caller-owned `glColorMask(F,F,F,F)` for depth-only passes
  (`gameos_graphics.cpp:4377/4590`, `gos_postprocess.cpp:3122`,
  `gos_static_prop_batcher.cpp:5140`). Promoting it would let `applyPipeline`
  enforce the `StaticPropDepth` row's color-off. **Later, not now:** it is
  all-or-nothing (low descriptive value), and *routing* it is a behavior change
  needing a pixel-identical gate — defer until there's a consumer that benefits.
  Per-channel `glColorMaski` exists ONLY in unregistered post-fx
  (`gos_postprocess.cpp:2449-2451`) → not PSO-relevant.

### NEEDS PASS REGISTRATION FIRST
- **MRT / `glDrawBuffers`** — pass/FBO-level, set once at `beginScene`
  (`gos_postprocess.cpp:221/231/237`) through a single policy point. This is
  **VkRenderPass attachment config, not PSO state**; `colorAttachments` mask
  stays descriptive. Model only when passes get explicit RenderPass attachment
  records.
- **Viewport** — dynamic per-render-target save/restore
  (`captureShadowGLState`/`restoreShadowGLState`); never per-pipeline. VkDynamicState.

### DO NOT MODEL
- **Blend equation** — only `glBlendEquation` call in the engine is ImGui debug
  (`debug_renderer.cpp:357/382`). Every registered/real pass is implicit
  `GL_FUNC_ADD`; no subtract/min/max, no dual-source. Constant → not a key axis.
- **Stencil** — absent engine-wide (only `GL_STENCIL_BUFFER_BIT` in a clear,
  `gameosmain.cpp:563`); no func/op/test anywhere.
- **Scissor** (`GL_SCISSOR_TEST`/`glScissor`) — zero hits.
- **glDepthRange / GL_DEPTH_CLAMP** — none anywhere.

---

## B. Unregistered draw passes

All five families already own a `RenderPassId` (Water=6, VFX=5, UI=11, Decal=9,
Overlay=10) — the gap is a `PipelineDesc` row + `pipelineDescRegistered=true`.
No blend equation other than `FUNC_ADD`; no dual-source, anywhere.

### PROMOTE NOW (stable, finite, RenderPassId exists)
- **TerrainOverlay** (`gameos_graphics.cpp:9760`, `overlayProg_`) — opaque,
  no-blend, GEQUAL, depth-write; identical per-frame + static. **Easiest** — a
  fourth "opaque-like" row, same shape as StaticPropOpaque.
- **TerrainDecal** (`gameos_graphics.cpp:9953`, `decalProg_`) — `BlendMode::AlphaBlend`
  (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`), depth-test no-write GEQUAL, no cull; fixed
  state. First registered ALPHABLEND pipeline (exercises the blend factor axis).
- **Water — ARMED fast path base only** (`gameos_graphics.cpp:3076`
  `renderWaterFastPath`, `water_fast_prog_`/MDI) — opaque, no-blend, GEQUAL,
  depth-write, cull-back. **Caveat:** the legacy quad fallback (`quad.cpp:2612`)
  is alpha + data-driven — model ONLY the armed opaque base, document the fallback
  as out-of-scope.

### PROMOTE LATER
- **VFX / particles** (`gos_particle_bridge.cpp:1061/1141/533/771`,
  `gos_vfx_mesh_bridge.cpp:315`) — 3 programs (particle_billboard / tube_ribbon /
  vfx_mesh), GEQUAL depth-test, depth-mask OFF, no cull. Blend is **data-driven
  per group/ribbon/instance** (alpha vs additive — 2 modes, both FUNC_ADD).
  ⇒ ~6 finite rows (3 prog × 2 blend) + a submit-time blend selector. Tractable
  but needs the multi-row + selection machinery; defer behind the terrain passes.
- **Mine static** (`gameos_graphics.cpp:4756`, alpha-test) — stable state but
  **NO RenderPassId** → really NEEDS PASS REGISTRATION FIRST (add an id or fold
  into TerrainOverlay).

### DO NOT MODEL
- **UI / HUD** (`gameos_graphics.cpp:855` `gosMesh::draw`, `:5773/7543/7572`) —
  the gos legacy state machine: 5 blend modes, 3 programs, per-draw depth/cull/
  zwrite from the `gos_State_*` array. This IS the dynamic-state emulator;
  modeling it as static PSO rows fights its design.
- **Picking / readback** (`RenderWorld.cpp:863`) — NO draw; pure `glReadPixels`
  of the objId ATTACHMENT2 that the already-covered opaque PSOs write. Nothing to
  model.
- **Roads / runways** — a terrain surface-type, not a pass.

### NEEDS PASS REGISTRATION FIRST
- **Terrain MAIN solid pass** — still fully ad-hoc, no PipelineDesc; the big
  remaining registration, its own slice (not bundled here).
- **Mine static** — see above (no RenderPassId).

---

## Recommended next slices (in order)

1. **TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1** (metadata-first, PROMOTE NOW):
   add `TerrainOverlay` + `TerrainDecal` PipelineIds + descriptive rows + schema +
   checker (incl. `check-pipeline-desc.py` field-list if any field is added — but
   none is needed). TerrainDecal is the first AlphaBlend row → light validation
   that the blend-factor axis generalizes. Then a routing slice with the
   pixel-identical gate. **No new PipelineDesc field.**
2. **WATER-ARMED-PIPELINE-REGISTRATION-1** — register the armed fast-path opaque
   base; document the legacy-quad alpha fallback as unmodeled.
3. **VFX-PIPELINE-REGISTRATION-1** (later) — the 6-row blend-selector family.
4. (Optional) **PIPELINEKEY-COLORWRITE-AUTHORITY-1** — only if a consumer wants
   the depth-prepass color-off to be row-driven; 1-bit field + routed glColorMask
   behind a pixel gate.

## Durable pattern (reused for every pass above)
`metadata registration → checker → behavior routing (applyPipeline) → pixel-identical proof`.
TerrainOverlay/Decal/Water/VFX all follow this; UI and Picking never get modeled.

## Do NOT do
No blend-equation / stencil / scissor / depth-range fields (absent or constant).
No MRT/viewport as PSO fields (render-pass / dynamic state). No UI or picking
registration. No terrain-main-solid in these slices.
