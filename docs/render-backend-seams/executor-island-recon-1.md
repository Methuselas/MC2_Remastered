# RECON — FRAME-GRAPH-EXECUTOR-ISLAND-RECON-1

Pick and de-risk the first executor-owned island (FRAME-GRAPH-EXECUTOR-ISLAND-1).

> All `file:line` refs are drift-prone. Re-grep before any slice derived from this doc.

---

## TL;DR

**Recommended first island: the PostProcess final-composite blit** (the `glDrawArrays` quad
in `gosPostProcess::endScene()` that draws `sceneColorTex_` to the default framebuffer,
`gos_postprocess.cpp:2629`).

One-line why: it is the last action in `endScene()`, always fires, binds the default
framebuffer (GLuint 0 = `RenderResourceId::Backbuffer`) explicitly, reads only
`sceneColorTex_` (explicit bind, no inherited texture latch), is driven by the registered
`PostProcessComposite` pipeline row, and is already downstream of all terrain-latch
consumers. The executor owns EXACTLY the validate→bind→assert→call→validate wrapper around
the existing composite draw — nothing else.

**PostProcess-WHOLE is too big.** The full `endScene()` chain (11 sub-stages, 5 of which
are conditional on `sceneHasTerrain_`) is not a BORING island. The composite blit itself
IS boring.

---

## Q1 — PostProcess sub-stage map

Entry point: `gameosmain.cpp:606` calls `noteRenderPass(PostProcess, ...)` then
`beginPassScope(PostProcess, ...)` then `pp->endScene()` (`gos_postprocess.cpp:2397`).

### Sub-stages in call order (`endScene()`, gos_postprocess.cpp:2397–2652)

| # | Sub-stage | Method | Condition | FBO target | Notes |
|---|---|---|---|---|---|
| 1 | HZB reduce | `runHzbReduce()` :1468 | `hzbEnabled_` (MC2_HZB_BUILD, default OFF) | sceneFBO_ | no-op default |
| 2 | HZB probe | `runHzbProbe()` :1521 | `hzbProbeEnabled_` (MC2_HZB_PROBE, default OFF) | none (diagnostic) | no-op default |
| 3 | Cluster depth pyramid | `cluster_depth_pyramid::Run()` :2424 | MC2_CLUSTER_DEPTH_PYRAMID default OFF | none | no-op default |
| 4 | Light grid build | `lightgrid_build::Run()` :2432 | MC2_LIGHTGRID_BUILD default OFF | none | no-op default |
| 5 | Compute blur substrate | `postprocess_blur::Run()` :2442 | MC2_POSTPROCESS_COMPUTE_BLUR default OFF | none | no-op default |
| 6 | Screen shadow | `runScreenShadow()` :2027 | `screenShadowEnabled_ && sceneHasTerrain_ && shadowsEnabled_` | sceneFBO_ | **terrain-latch consumer** |
| 7 | Cloud shadow | `runCloudShadow()` :2159 | `enableCloudShadow_ && sceneHasTerrain_` | sceneFBO_ | **terrain-latch consumer** |
| 8 | Shoreline | `runShoreline()` :2232 | `shorelineEnabled_ && sceneHasTerrain_` | sceneFBO_ | **terrain-latch consumer** |
| 9 | SSAO | `runSSAO()` :1931 | `ssaoEnabled_` (MC2_SSAO, default OFF) | sceneFBO_ | no-op default |
| 10 | Box decals | `drawBoxDecals()` | MC2_BOX_DECAL default OFF | sceneFBO_ | no-op default |
| 11 | Edge fog | `runEdgeFog()` :2278 | `edgeFogEnabled_ && sceneHasTerrain_` | sceneFBO_ | **terrain-latch consumer** |
| 12 | OOB fog | `runFogOob()` :2333 | `sceneHasTerrain_` | sceneFBO_ | **terrain-latch consumer** |
| 13 | **Final composite blit** | inline in `endScene()` :2471–2633 | always (if `initialized_`) | **FBO 0 (Backbuffer)** | **BORING island candidate** |
| 14 | Shadow debug overlay | `drawShadowDebugOverlay()` :2654 | `showShadowDebug_` (debug, default off) | FBO 0 | debug only |

**5 of 12 sub-stages are conditional on `sceneHasTerrain_` (the terrain latch).**
Stages 1–5 and 9–10 are default-OFF substrates. Stage 13 (composite) is the single
always-active sub-stage writing to the default framebuffer.

### Conclusion (Q1)

PostProcess-whole is a 13-sub-stage chain, 5 of which are terrain-latch consumers.
**Too big / too path-variable for a BORING first island.** The composite blit (stage 13)
is a single always-on fullscreen quad with explicit inputs — the boring island.

---

## Q2 — FBO in/out declared for PostProcess

**Contract row** (`RenderPassContract.h:259–277`):
```
reads:  MainColor, MainDepth, ShadowDynamicMap
writes: MainColor
colorFinalLayout: Present
```

**FBO ledger status** (`fbo_ledger.h:55–63`): PostProcess is **intentionally absent** from
`kPassFboTarget`. Comment at line 55: *"PostProcess is intentionally absent (its FBO at the
sample seam is timing-uncertain — the probe will tell us before we declare it, the same
discipline that caught blend)."*

The `noteRenderPass(PostProcess)` callsite fires at `gameosmain.cpp:606` — BEFORE
`endScene()` runs. At that moment the bound FBO is `sceneFBO_` (the last scene pass left it
bound). The composite blit itself binds `FBO 0` at `gos_postprocess.cpp:2472`.

**HIGH: The PostProcess pass-whole FBO is ambiguous at the noteRenderPass seam.** The seam
fires while sceneFBO_ is bound, but the final composite writes to FBO 0 (Backbuffer). An
executor declaring `declaredFboTarget(PostProcess) = Backbuffer` would mismatch the live
bound FBO at the noteRenderPass seam.

**For the composite blit sub-stage, the FBO IS declared and explicit:** `glBindFramebuffer(GL_FRAMEBUFFER, 0)` at `gos_postprocess.cpp:2472`. The ledger already maps GLuint 0 →
`RenderResourceId::Backbuffer` (`fbo_ledger.h:35`). An island executor wrapping only the
composite blit can assert `Backbuffer` at the `glBindFramebuffer` call — no ambiguity.

---

## Q3 — Texture-latch reliance

The composite blit (`endScene()` :2608–2633):
- `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, sceneColorTex_)` — **explicit bind**
- `glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, sceneObjectIdTex_)` — **explicit bind** (only when sceneObjectIdTex_ != 0)
- `mc2gl::GlScopedTextureUnit unit0Guard(0)` / `unit2Guard(2)` — **scope-guarded, restores on exit**

The composite does NOT inherit any texture unit state from prior passes. The `gl_state_guard.h`
`GlScopedTextureUnit` guards (`gos_postprocess.cpp:2608–2609`) save and restore units 0 and 2.

**Conclusion: zero inherited texture-latch reliance for the composite blit.** No TEX-LATCH blocker.

---

## Q4 — Ambient weirdness

From `ambient_contract.h:113–117`:
```cpp
{ RenderPassId::PostProcess,
  ColorMaskState::Inherit, false, false,
  DepthFuncState::Inherit, ViewportKind::MainScene,
  /*producesLatch*/ false, /*consumesLatch*/ true,
  "consumes sceneHasTerrain_; FORCE-43 viewport re-set before composite" }
```

For the composite blit specifically:
- **Viewport**: the FORCE-43 block re-sets viewport (`gos_postprocess.cpp:2503–2510`) before the draw. This is an INTERNAL state set within `endScene()`; the executor does NOT need to set it before calling the existing body.
- **Blend**: `pipeline_binder::applyPipeline(PostProcessComposite)` at :2525–2527 sets `GL_BLEND OFF` (Opaque blend). The composite forces blend-off explicitly via the pipeline row.
- **Depth test/write**: depth test OFF, depth-write OFF established by `PostProcessComposite` pipeline row.
- **Scissor**: the OOB-letterbox block at :2495–2501 disables scissor for the clear, re-enables if it was on. The composite draw itself does not rely on scissor.
- **colorMask**: `ColorMaskState::Inherit` — does not re-set colorMask. The prior pass (UI or VegetationCards) leaves colorMask in an unknown state. **The composite calls `applyPipeline(PostProcessComposite)` which should own colorMask**. Verify the PostProcessComposite pipeline row declares `colorMask=AllOn`. If it does NOT, this is a potential ambient precondition the executor must assert.

**Ambient preconditions an executor must assert before calling the composite body:**
1. Assert FBO 0 is the target (or bind it explicitly — the body binds it anyway at :2472).
2. Assert `sceneColorTex_` is populated (non-zero; check at validate-inputs time).
3. Assert `initialized_ == true` (the body early-returns if false).
4. **No colorMask assertion needed** — `applyPipeline` owns this inside the body.

**No FORCE-43 weirdness for the executor:** the viewport re-set is inside `endScene()` and is called by the existing body unchanged. The executor wraps the call, it does not replicate it.

---

## Q5 — Path-variance branches in the composite blit

From `endScene()` :2532–2633:
- `gos_GetSelectedViewMode()` — reads a static mode (default 0 = Visual). Path-variable but harmless: the composite shader handles all modes.
- `fxaaEnabled_` — read once from env (default OFF). Shader branch, not a draw-count branch.
- `sceneObjectIdTex_` check at :2554 / :2622 — fallback to Visual when OID buffer absent. No extra draw.
- `compositeProg_->is_valid()` guard — early-return if shader failed to compile. Executor SHOULD assert `is_valid()` at validate-inputs time.

**Conclusion: the composite blit has zero draw-count path variance.** It always fires one `glDrawArrays(GL_TRIANGLES, 0, 6)`. All conditionals are uniform-value branches inside the shader.

---

## Q6 — Isolation

The composite blit:
- Reads `sceneColorTex_` (the fully-composed scene color after all prior sub-stages).
- Writes to the default framebuffer (FBO 0).
- Does NOT write to `sceneFBO_`, `shadowFBO_`, `dynShadowFBO_`, or any resource another pass reads.
- Does NOT touch `sceneHasTerrain_` (no `markTerrainDrawn()` call).
- Does NOT produce any texture or buffer that terrain, objects, water, or shadow depends on.

**An executor owning only the composite blit cannot break any other pass.** The worst possible
failure mode is a blank/wrong screen — no upstream resource corruption.

What the executor "owns" for this island:
1. The `validate declared inputs` check (assert `compositeProg_->is_valid()` and `sceneColorTex_ != 0`).
2. The `bind declared FBO` step (bind FBO 0 — the body does this at :2472, but the executor declares it owns the bind).
3. The `assert ambient preconditions` check (FBO is now Backbuffer; `initialized_ == true`).
4. The existing `pp->endScene()` call body **unchanged** — the executor calls the full `endScene()` unchanged, which includes the sub-stages. The composite IS the final action in `endScene()`. If a sub-island of endScene() is desired (composite only, not the full chain), that requires refactoring `endScene()` into `endScenePreCompute()` + `compositeToBackbuffer()`. That is NOT boring. Do NOT do this for the first island — wrap the full `pp->endScene()` call.
5. The `validate postconditions` check (assert FBO 0 is still bound after endScene returns; assert `gl_error == GL_NO_ERROR`).

---

## RECOMMENDATION: Wrap `pp->endScene()` as the first island

The composite blit is the de-risked sub-stage, but the minimal change that proves "executor can own execution" is wrapping the **existing `pp->endScene()` call at gameosmain.cpp:614** in the validate→bind→assert→call→validate shell. This wraps the full PostProcess pass (all sub-stages) without refactoring `endScene()`. The boring island is PostProcess-whole-call, not PostProcess-whole-chain — the difference is that the executor owns the WRAPPER around the existing call, not the internals.

**HIGH: PostProcess-whole is NOT a boring island for an executor that owns FBO binding inside the chain.** It IS a boring island for an executor that owns only the pre/post-call assertions. This recon recommends the latter (assertion-only wrapper).

---

## SLICE PROPOSAL (not built) — FRAME-GRAPH-EXECUTOR-ISLAND-1

### What "owned" means

The executor:
1. Validates declared inputs before the call.
2. Asserts ambient preconditions at the call site.
3. Calls the EXISTING `pp->endScene()` unchanged.
4. Validates postconditions after the call.
5. Reports `executor_owned_passes=1` in the frame_context dump.

The executor does NOT: bind FBOs, reorder passes, move barriers, alias resources, or touch any other pass.

### Wrapper shape (pseudocode)

```cpp
// In gameosmain.cpp, replacing the raw pp->endScene() call at ~:614
if (executorEnabled()) {
    // 1. Validate declared inputs
    executor::validatePassInputs(RenderPassId::PostProcess);
    //    checks: pp->initialized_, compositeProg_->is_valid(), sceneColorTex_ != 0

    // 2. Assert ambient preconditions
    //    PostProcess: ViewportKind::MainScene, consumesTerrainLatch (warn if !sceneHasTerrain_)
    executor::assertAmbientPreconditions(RenderPassId::PostProcess);

    // 3. Call existing body UNCHANGED
    pp->endScene();

    // 4. Validate postconditions
    //    Assert FBO 0 is bound (the composite blit is the last FBO bind)
    //    Assert glGetError() == GL_NO_ERROR (drainGLErrors already called inside endScene)
    executor::validatePassPostconditions(RenderPassId::PostProcess);

    // 5. Record ownership
    gos_FrameCtxMarkExecutorOwned(RenderPassId::PostProcess);  // bumps executor_owned_passes
} else {
    pp->endScene();   // OFF path: byte-identical
}
```

### Files touched

| File | Change |
|---|---|
| `GameOS/gameos/gameosmain.cpp` | Replace raw `pp->endScene()` at :614 with executor branch |
| `GameOS/gameos/gos_frame_context.h` | Add `executor_owned_passes` field + `gos_FrameCtxMarkExecutorOwned()` |
| `GameOS/gameos/gos_frame_context.cpp` | Implement `gos_FrameCtxMarkExecutorOwned()` |
| `GameOS/gameos/debug_state_dump.cpp` | Emit `executor_owned_passes` in `frame_context` JSON block |
| `RenderCore/executor_island.h` | New: pure header, `validatePassInputs`, `assertAmbientPreconditions`, `validatePassPostconditions` (gate `MC2_FRAMEGRAPH_EXECUTOR`) |
| `scripts/mcp/mc2_render_state_server.py` | Expose `executor_owned_passes` in `get_frame_context()` |

### New gate

`MC2_FRAMEGRAPH_EXECUTOR` — default-OFF. OFF path = byte-identical (raw `pp->endScene()` called directly, no assertions).

### executor_owned_passes field

Add to `RenderFrameContext` (`gos_frame_context.h`):
```cpp
int executorOwnedPasses = 0;  // count of passes owned by the executor this frame
```
Reset to 0 at `frameBegin()`. Incremented by `gos_FrameCtxMarkExecutorOwned()`.

MCP `get_frame_context()` already exposes the `frame_context` block; the new field appears there automatically once the dump emits it. Success criterion: `executor_owned_passes=1` when `MC2_FRAMEGRAPH_EXECUTOR=1` and smoke is green.

### Validate declared inputs

For `RenderPassId::PostProcess`:
- `pp != nullptr`
- `pp->initialized_`
- `compositeProg_ && compositeProg_->is_valid()` (shader compiled)
- `sceneColorTex_ != 0` (scene FBO populated)
- Read the contract row: `reads[]` = {MainColor, MainDepth, ShadowDynamicMap}; validate each via `RenderResourceRegistry::glName(id) != 0` if registered.

On failure: log and skip the executor wrapper (fall through to raw `pp->endScene()`); do NOT abort the frame.

### Assert ambient preconditions

For `RenderPassId::PostProcess` (from `ambient_contract.h:113–117`):
- `consumesTerrainLatch == true`: warn (not assert) if `!sceneHasTerrain_`; the sub-stages already bail gracefully when terrain is absent (menus/frontend).
- No colorMask or depthFunc pre-assert needed — the `applyPipeline` call inside endScene owns those.
- No viewport pre-assert needed — the FORCE-43 block inside endScene owns viewport.

### Validate postconditions

- Assert `glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING) == 0` — the composite blit left FBO 0 bound (GLSTATE-VAO-RESTORE-1 also restores VAO; the FBO restore path ends at 0 because the body binds 0 explicitly).
- The `drainGLErrors("post_process")` inside endScene at :2648 already consumes any GL errors. The postcondition validator can call `glGetError()` and assert `GL_NO_ERROR`.

### Verification plan

1. **Offline**: `RenderCore/executor_island.h` is pure — add to `tests/unit/test_executor_island.cpp`: validate-inputs-pass, validate-inputs-fail (null prog), postcondition-fbo-mismatch. Run `bash scripts/run-unit-tests.sh --ts=ExecutorIsland`.
2. **OFF smoke** (byte-identical): `MC2_FRAMEGRAPH_EXECUTOR` not set → `executor_owned_passes=0`, no behavior change. Tier-1 2-mission smoke: EXIT 0.
3. **ON smoke**: `MC2_FRAMEGRAPH_EXECUTOR=1` → `executor_owned_passes=1`. MCP `get_frame_context()` confirms field. Tier-1 2-mission smoke: EXIT 0, zero ambient mismatches, zero fbo mismatches.
4. **check-ambient-guard.py** must still exit 0 with the executor gate ON.

---

## Summary of HIGH findings

- **HIGH: PostProcess-whole is NOT a boring first island** if the executor tries to own FBO binds or drive sub-stage sequencing. The 5 `sceneHasTerrain_`-gated sub-stages create path variance; the `sceneFBO_` vs FBO-0 switch midway through `endScene()` makes the FBO contract ambiguous at the `noteRenderPass` seam. Do NOT attempt to own the PostProcess FBO binding for the full chain.

- **HIGH: kPassFboTarget intentionally excludes PostProcess** (`fbo_ledger.h:55–63`). The declared FBO is `Unknown` because the bound FBO at the `noteRenderPass(PostProcess)` seam (`gameosmain.cpp:606`) is `sceneFBO_` (not FBO 0). The composite blit itself binds FBO 0, but that happens 70+ lines into `endScene()`. An executor using the noteRenderPass seam to sample the FBO will see `sceneFBO_`, not `Backbuffer`. The safe approach: the executor wraps the CALL (assertions around the existing body), not the FBO bind.

- **SAFE: sceneColorTex_ and sceneObjectIdTex_ use explicit binds** — no inherited texture-latch risk for the composite. The `GlScopedTextureUnit` guards restore units 0/2 on exit.

- **SAFE: The composite blit writes only to FBO 0 (Backbuffer)** — no resource contamination of any upstream pass. Worst failure = wrong pixels on screen.

- **SAFE: `pp->endScene()` is already fully self-contained** — it saves/restores VAO (GLSTATE-VAO-RESTORE-1 :2410–2651), drains GL errors (:2648), and calls `gos_InvalidateRenderStateCache()` (:2646). The executor wrapper needs no teardown.
