# RECON — FRAMEGRAPH-APPLY-STATE-ISLAND-1-RECON-1

Pick the safest first apply-state sub-stage and define what "apply" means.

> All `file:line` refs are drift-prone. Re-grep before any slice derived from this doc.

---

## TL;DR

**Recommended first apply-state island: EdgeFog** (`ExecutorIslandId::EdgeFog`,
`gos_postprocess.cpp:2279`).

**What "apply" means:** the executor calls
`pipeline_binder::applyPipeline(getPipelineDesc(PostProcessEdgeFog), "PostProcessEdgeFog")`
+ `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` + `setSceneDrawBuffers(SingleColor, false)`
+ `glViewport(0, 0, width_, height_)` BEFORE the body. The body's own identical calls
become redundant double-sets — idempotent in slice 1, removable in slice 2.

**Idempotency / byte-identical argument:** `applyPipeline` is a pure GL-state setter
(no draw, no side-effect counter). `glBindFramebuffer` + `setSceneDrawBuffers` +
`glViewport` are also pure state. Re-applying the same values twice leaves the GPU
pipeline state byte-identical to a single application. The body's early-return guards
(`!edgeFogEnabled_` etc.) are checked BEFORE the executor pre-applies, so the executor
pre-apply only fires when the body will draw. Gate OFF = body-only = current behaviour.

**RenderStateDesc shape:** a new `SubStageStateDesc` table keyed on `ExecutorIslandId`
(4 rows: EdgeFog, FogOob, Shoreline, CloudShadow). Minimal fields: `{islandId,
pipelineId, fboTarget (RenderResourceId::MainColor), viewport (ViewportKind::MainScene)}`.
This is SEPARATE from `kPassRenderState[]` which is per top-level `RenderPassId`; the
PostProcess row there stays `pipelineId=Invalid, fboTarget=Unknown`.

**First metric:** a new `g_executorApplyStatePasses` counter (parallel to
`g_executorOwnedPasses`) incremented by `executorOwnEndSub` when apply-state was
performed, gated by `MC2_FRAMEGRAPH_EXECUTOR=1`. OFF → counter stays 0 (no apply);
ON → counter increments ≥1 per EdgeFog frame.

---

## Q1 — Per-sub-stage GL state inventory

State at ENTRY to each body (lines verified live in nifty-mendeleev):

### EdgeFog (`gos_postprocess.cpp:2279`)

Early-return gates: `!edgeFogEnabled_ || !edgeFogProg_ || !edgeFogProg_->is_valid()` or
`mapHalfExtent_ <= 0` or `!sceneHasTerrain_`.

State set before first draw:
1. `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` — :2293
2. `setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false)` — :2294
3. `glViewport(0, 0, width_, height_)` — :2295
4. `pipeline_binder::applyPipeline(getPipelineDesc(PostProcessEdgeFog), ...)` — :2302–2304
   → depth test OFF, depth write OFF, cull None, AlphaBlend (SRC_ALPHA/ONE_MINUS_SRC_ALPHA)
5. `edgeFogProg_->apply()` (binds program) — :2309
6. `setInt("depthTex", 0)` after `apply()` — :2310
7. `glUniformMatrix4fv(invViewProj, ...)` direct after `apply()` — :2311–2313
8. `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, sceneDepthTex_)` — :2321–2322

Teardown (OWNED BY BODY, NOT by applyPipeline):
- `glDisable(GL_BLEND); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); glActiveTexture(GL_TEXTURE0)`

**Captured by PostProcessEdgeFog PipelineId:** items 4 only (depth/blend/cull).
**Hand-set outside PipelineId:** items 1–3 (FBO bind, draw-buffer mode, viewport),
items 5–8 (program bind, uniforms, texture), teardown.

### FogOob (`gos_postprocess.cpp:2334`)

Early-return gates: `!fogOobEnabled_ || !fogOobProg_ || !fogOobProg_->is_valid()` or
`!sceneHasTerrain_`.

State set before first draw:
1. `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` — :2349
2. `setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false)` — :2350
3. `glViewport(0, 0, width_, height_)` — :2351
4. `pipeline_binder::applyPipeline(getPipelineDesc(PostProcessFogOob), ...)` — :2358–2360
   → identical state to EdgeFog (AlphaBlend, depth OFF, cull None)
5. `fogOobProg_->apply()` — :2378
6. uniforms set after `apply()`, `glActiveTexture(GL_TEXTURE0); glBindTexture(..., sceneDepthTex_)` — :2385–2386

Teardown: `glDisable(GL_BLEND); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); glActiveTexture(GL_TEXTURE0)`

**Captured by PostProcessFogOob:** item 4 (state twin of EdgeFog).
**Hand-set outside:** items 1–3, 5–6, teardown. Identical profile to EdgeFog.

### Shoreline (`gos_postprocess.cpp:2233`)

Early-return: `!shorelineEnabled_ || !sceneHasTerrain_ || !shorelineProg_ || !shorelineProg_->is_valid()`

State set before draw:
1. `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` — :2240
2. `setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false)` — :2242
3. `glViewport(0, 0, width_, height_)` — :2243
4. `pipeline_binder::applyPipeline(getPipelineDesc(PostProcessShoreline), ...)` — :2248–2250
   → Multiply (DST_COLOR/ZERO), depth OFF, cull None
5. `shorelineProg_->apply()` — :2262
6. uniforms after `apply()`, then **TWO** texture unit binds:
   `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, sceneDepthTex_)` — :2264–2265
   `glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, sceneNormalTex_)` — :2266–2267

Teardown: `glDisable(GL_BLEND); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); glActiveTexture(GL_TEXTURE0)`

**Captured by PostProcessShoreline:** item 4 (Multiply blend).
**Hand-set outside:** items 1–3, 5–6, teardown.
**Extra texture unit (GL_TEXTURE1 / sceneNormalTex_):** this is safe — it is a BIND,
not state the executor would pre-apply. The executor does not touch texture bindings;
the body still owns all tex unit binds. No executor risk here.

### CloudShadow (`gos_postprocess.cpp:2160`)

Early-return: `!enableCloudShadow_` or `!sceneHasTerrain_` or `!cloudProg_->is_valid()`

State set before draw:
1. `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` — :2186
2. `setSceneDrawBuffers(SceneDrawBufferMode::SingleColor, false)` — :2187
3. `glViewport(0, 0, width_, height_)` — :2188
4. `pipeline_binder::applyPipeline(getPipelineDesc(PostProcessCloudShadow), ...)` — :2192–2194
   → Multiply (DST_COLOR/ZERO), depth OFF, cull None
5. `cloudProg_->setInt("sceneDepthTex", 0)` BEFORE `apply()` — :2198
   (setInt before apply is legal: glsl_program queues the uniform to flush at apply())
6. Several `setFloat` / `setFloat2` / `setInt` before `apply()` — :2198–2209
7. `cloudProg_->apply()` — :2210
8. `glUniformMatrix4fv(inverseViewProj, ...)` AFTER `apply()` — :2212–2213
9. `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, sceneDepthTex_)` — :2215–2216

Teardown: `glDisable(GL_BLEND); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST); glActiveTexture(GL_TEXTURE0)`

**Captured by PostProcessCloudShadow:** item 4 (Multiply).
**Hand-set outside:** items 1–3, 5–9, teardown.
**No special risk vs EdgeFog/FogOob/Shoreline** — identical structure.

---

## Q2 — What "apply" means and idempotency analysis

### Minimal apply definition

The executor pre-applies **three categories** of state before the body:

| # | What | GL calls | Owned by executor |
|---|---|---|---|
| A | Pipeline state | `applyPipeline(getPipelineDesc(<PipelineId>), <dbgName>)` | depth/blend/cull/frontFace |
| B | FBO bind | `glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_)` | FBO target |
| C | Draw buffer + viewport | `setSceneDrawBuffers(SingleColor, false); glViewport(0,0,w,h)` | attachment/viewport |

The body's existing calls to the same three groups then execute. Because all six GL
calls (executor + body) produce the same GL state transitions, the final state entering
the draw is identical.

**The executor does NOT pre-apply:**
- Program bind (body's `prog->apply()` still binds it — `glProgramName=0` in PostProcessEdgeFog row)
- Uniforms (body owns `setInt/setFloat/apply()` sequencing — including the before-vs-after-apply ordering)
- Texture bindings (executor never touches tex units)
- Teardown state (`glDisable(GL_BLEND)` etc. — body owns exit state, validated by `executorOwnEndSub`)

### Idempotency proof

`applyPipeline` is a sequence of `glEnable/Disable`, `glDepthMask`, `glDepthFunc`,
`glCullFace`, `glBlendFunc`, `glBlendEquation` — all pure state setters, no draw.
Called twice with the same `PipelineDesc` yields identical GL state: GL state is a
write-only register for these calls, no accumulation, no counter. ✓

`glBindFramebuffer`, `setSceneDrawBuffers`, `glViewport` similarly: repeated identical
calls are pure state overwrites. ✓

### Could any body state depend on state set BETWEEN applyPipeline and the draw?

EdgeFog body sequence post-apply: `edgeFogProg_->apply()` (program bind) → `setInt` →
`glUniformMatrix4fv` (direct after apply) → `glActiveTexture/glBindTexture`. None of
these depend on the blend/depth/cull state set by applyPipeline. The uniform sets are
program-state, not pipeline-state. ✓

FogOob: same structure, safe. ✓

Shoreline: same, safe. The GL_TEXTURE1 bind is a tex-unit operation, independent of
blend/depth. ✓

CloudShadow: `setInt/setFloat` calls BEFORE `cloudProg_->apply()` are queued in the
`glsl_program` object (not live GL calls until `apply()`). They do not depend on
pipeline state. ✓

**No sub-stage has body state between applyPipeline and the draw that depends on
pipeline state.** Executor pre-apply is safe for all four.

---

## Q3 — Safest first target: EdgeFog confirmed

Ranking criteria: "boring" = PipelineId fully captures the state + clean sceneFBO_ +
viewport MainScene + no exotic texture dependency.

| Sub-stage | PipelineId | Blend | FBO | VP | Tex units | IslandContract | Notes |
|---|---|---|---|---|---|---|---|
| **EdgeFog** | PostProcessEdgeFog (18) | AlphaBlend | sceneFBO_ (MainColor) | MainScene | TEXTURE0 only | ISLAND-2 | **BORING — recommend first** |
| **FogOob** | PostProcessFogOob (19) | AlphaBlend | sceneFBO_ | MainScene | TEXTURE0 only | ISLAND-2 | Equally boring; state twin of EdgeFog |
| Shoreline | PostProcessShoreline (23) | Multiply | sceneFBO_ | MainScene | TEXTURE0 + TEXTURE1 | ISLAND-3 | Safe but two tex units (minor extra check) |
| CloudShadow | PostProcessCloudShadow (22) | Multiply | sceneFBO_ | MainScene | TEXTURE0 only | ISLAND-3 | Default-OFF reduces test coverage |
| Composite | PostProcessComposite (17) | Opaque | FBO 0 | custom (FORCE-43) | TEXTURE0 only | ISLAND-1 | **AVOID** — FBO-0 boundary, letterbox viewport |
| ScreenShadow | PostProcessScreenShadow (21) | Multiply | sceneFBO_ | MainScene | units 0–4 incl. 2D_ARRAY | NOT owned | **AVOID** — not owned; tex-unit leak documented |

**EdgeFog is the recommended first island** for apply-state:
- PipelineId PostProcessEdgeFog (18) is routed, registered, and byte-proven
  (`pipeline-pass-coverage-ledger.json` : edgeFog+fogOob byte-identical-by-construction)
- FBO = sceneFBO_ (RenderResourceId::MainColor) — unambiguous, already in the existing
  IslandContract `postRequiresDefaultFbo=false`
- Viewport = full-scene (`width_, height_`) — no FORCE-43 letterbox complication
- Single texture unit (TEXTURE0 = sceneDepthTex_) — no unit-1 restore check needed
- Default-ON in-mission (`edgeFogEnabled_` is true when a map half-extent is set)
  → the apply-state path is exercised every in-mission frame → good smoke coverage

FogOob is equally boring and could be the second in the same slice (it is a state twin).

---

## Q4 — RenderStateDesc shape for sub-stages

**`kPassRenderState[]` (per `RenderPassId`) is the WRONG table for sub-stages.**
The PostProcess row has `pipelineId=Invalid` and `fboTarget=Unknown` — correctly so,
because PostProcess is a multi-sub-pipeline pass. Adding sub-stage state there would
require either a multi-pipeline union or breaking the one-row-per-pass invariant.

**Recommended shape: a new `kSubStageStateDesc[]` table, keyed on `ExecutorIslandId`.**

```cpp
// RenderCore/frame_executor.h  (or a new sub_stage_state_desc.h)
struct SubStageStateDesc {
    ExecutorIslandId   islandId;
    RenderCore::PipelineId  pipelineId;   // the single representative pipeline
    RenderResourceId   fboTarget;         // logical FBO (MainColor for sceneFBO_)
    ViewportKind       viewport;          // MainScene for all four owned sub-stages
};

static constexpr SubStageStateDesc kSubStageStateDesc[] = {
    { ExecutorIslandId::EdgeFog,     PipelineId::PostProcessEdgeFog,    RenderResourceId::MainColor, ViewportKind::MainScene },
    { ExecutorIslandId::FogOob,      PipelineId::PostProcessFogOob,     RenderResourceId::MainColor, ViewportKind::MainScene },
    { ExecutorIslandId::Shoreline,   PipelineId::PostProcessShoreline,  RenderResourceId::MainColor, ViewportKind::MainScene },
    { ExecutorIslandId::CloudShadow, PipelineId::PostProcessCloudShadow,RenderResourceId::MainColor, ViewportKind::MainScene },
};
```

This table lives alongside `kExecutorIslands[]` in `frame_executor.h`. It is
validation+schema only (no GL, no runtime state) — consistent with the design note in
that file. The `executorOwnBeginSub` implementation in `gos_postprocess.cpp` looks up
the row by `islandId` when apply-state is requested.

**Why NOT reuse the existing PostProcess `kPassRenderState` row?**
- That row is per-`RenderPassId`, one per pass; sub-stages are `ExecutorIslandId`,
  inside a single pass.
- PostProcessSubpass would need a shadow row for each sub-stage — same structural
  problem.
- A new keyed table avoids retrofitting the static_assert invariant on `kPassRenderState`.

**Why NOT inline into `IslandContract`?**
- `IslandContract` carries validation postconditions (`postRequiresBlendDisabled` etc.).
  Mixing in apply-state fields (pipelineId, fboTarget, viewport) conflates two concerns.
- A separate `SubStageStateDesc` table keeps the split clean and is independently
  addressable for the future apply-pass-executor-dryrun.

---

## Q5 — Apply vs validate coexistence; byte-identical argument

### Split: apply + validate run on the SAME sub-stage

The executor pre-applies state (new) and validates postconditions (existing). These are
sequential, not conflicting:

```
executorOwnBeginSub(EdgeFog):
    willRun check (unchanged)
    [NEW] pre-apply: applyPipeline + glBindFramebuffer + setSceneDrawBuffers + glViewport
    requiresSceneDepthTex check (unchanged)

runEdgeFog():
    early-return gates (unchanged)
    glBindFramebuffer + setSceneDrawBuffers + glViewport   <- redundant in slice 1, safe
    applyPipeline(PostProcessEdgeFog)                      <- redundant in slice 1, safe
    edgeFogProg_->apply() + uniforms + tex bind + draw     <- body-owned, unchanged
    glDisable(GL_BLEND) + glDepthMask + glEnable(GL_DEPTH_TEST) + glActiveTexture  <- teardown

executorOwnEndSub(EdgeFog):
    postRequiresBlendDisabled check (unchanged) ← still valid: body's teardown ran
    postRequiresActiveTexture0 check (unchanged)
    glGetError check (unchanged)
    ++g_executorOwnedPasses (unchanged)
    [NEW] ++g_executorApplyStatePasses
```

### Metric: `g_executorApplyStatePasses`

A new standalone process-static counter, parallel to `g_executorOwnedPasses`, incremented
in `executorOwnEndSub` only when apply-state was performed AND postchecks passed.
Exposed via a new `mc2_framegraph_executor_apply_state_passes()` extern "C" function
(parallel to `mc2_framegraph_executor_owned_passes()`).

Gate OFF → `executorOwnBeginSub` returns early → pre-apply never executes → counter
stays 0. Body-only. Byte-identical to today. ✓

Gate ON → pre-apply executes → redundant double-set → functionally identical GL state
(same values written twice) → byte-identical output. Counter ≥1 after first EdgeFog
frame. ✓

### Double-set ordering risk

The executor pre-applies `applyPipeline` FIRST, then the body's own `applyPipeline`
fires. Both supply the same `PipelineDesc` (same `PipelineId` → same row from
`s_descs[]`). No race, no interleaving — this is single-threaded GL on the render thread.
The second call writes the same GL state the first call already wrote. Zero visible
difference. ✓

---

## Q6 — Migration path and risk

### Slice 1 (this slice — FRAMEGRAPH-APPLY-STATE-ISLAND-1)

**Target:** EdgeFog (optionally FogOob in the same commit — they are state twins, low
marginal cost to add both).

**Files to touch:**
- `RenderCore/frame_executor.h` — add `SubStageStateDesc` struct + `kSubStageStateDesc[]`
  table (4 rows: EdgeFog, FogOob, Shoreline, CloudShadow); add lookup helper
  `findSubStageStateDesc(ExecutorIslandId)`.
- `GameOS/gameos/gos_postprocess.cpp` — modify `executorOwnBeginSub`:
  when `executorEnabled()` AND `willRun`, look up `SubStageStateDesc` for this island;
  if found, pre-apply: `applyPipeline(getPipelineDesc(desc.pipelineId), dbgName)` +
  `glBindFramebuffer(GL_FRAMEBUFFER, pp->sceneFBO_)` +
  `setSceneDrawBuffers(SingleColor, false)` +
  `glViewport(0, 0, pp->width_, pp->height_)`.
  In `executorOwnEndSub` (post-check path): `++g_executorApplyStatePasses`.
  Add `extern "C" unsigned long mc2_framegraph_executor_apply_state_passes()` accessor.
- Body (`runEdgeFog`, `runFogOob`) is **NOT modified in slice 1**. Redundant
  double-set stays — explicitly documented as safe by idempotency argument above.

**Gate:** `MC2_FRAMEGRAPH_EXECUTOR=1` (existing gate). No new env var.
`MC2_FRAMEGRAPH_EXECUTOR=0` (default) → `executorOwnBeginSub` early-returns → no
pre-apply → no metric → zero change from today. ✓

### Verification plan

1. **Gate OFF (default) byte-identical check:**
   Run `MC2_FRAMEGRAPH_EXECUTOR=0` (default). No code path changes. All 5 tier1
   missions must produce the same pixel SHA as the pre-patch baseline. This is trivially
   true because no code runs. `executor_apply_state_passes=0` confirms gate is off.

2. **Gate ON functional identity check:**
   Run `MC2_FRAMEGRAPH_EXECUTOR=1`, `MC2_DEBUG_STATE_DUMP=1`.
   Check `executor_apply_state_passes > 0` in the diagnostic trace (at least 1 per frame
   that EdgeFog fires — every in-mission frame when the map has a half-extent).
   Check `executor_validation_failures == 0` (all postconditions pass).
   Run the tier1 smoke: `--mission mc2_01 --mission mc2_24`. Pixel SHA must match the
   same `MC2_FRAMEGRAPH_EXECUTOR=1` baseline from before the patch (no change in
   visual output). If no pre-patch MC2_FRAMEGRAPH_EXECUTOR=1 baseline exists, take one
   before the patch then rerun after — compare equal.

3. **Smoke gate (standard):**
   Full tier1 (`mc2_01 mc2_03 mc2_10 mc2_17 mc2_24`, 30s each) with gate OFF (default).
   Exit 0 required.

### Risk register

| Risk | Severity | Likelihood | Mitigation |
|---|---|---|---|
| `executorOwnBeginSub` tries to access `pp->sceneFBO_` (private member) before body's bind | LOW | Impossible — `sceneFBO_` is the SAME value the body would bind; it is already set in `createFBOs()` at startup | Access is fine if exposed via a `pp->executorSceneFbo()` accessor (mirrors the existing `executorSceneColorTexValid()` pattern) |
| `setSceneDrawBuffers` is a member function of `gosPostProcess` — not callable from a free function | MEDIUM | Expected | Add `pp->executorSetSingleColorDrawBuffers()` or expose via the existing `pp->executorXxx` accessor pattern; or move the body of `setSceneDrawBuffers(SingleColor,false)` into a helper the executor can call |
| `pp->width_` / `pp->height_` not accessible | LOW | Expected | Expose via `pp->executorWidth()` / `pp->executorHeight()` — already implicitly needed; mirrored by existing accessor pattern |
| Pre-apply fires but body early-returns (gate race) | IMPOSSIBLE | WillRun check in `executorOwnBeginSub` mirrors the body's early-return gates exactly; if WillRun==false the pre-apply is skipped | Already handled by existing WillRun guard |
| FogOob invViewProj convention (transposed `invT` vs `GL_FALSE` direct) | LOW | The executor pre-apply does NOT touch uniforms; this convention quirk is entirely in the body | Not a pre-apply concern |

### **HIGH risk: none identified for EdgeFog/FogOob**

No sub-stage body state that the executor pre-applies is non-idempotent or
draw-order-sensitive. The only potential HIGH would be if a sub-stage called
`glBlendFunc` between `applyPipeline` and its draw with DIFFERENT values than
`applyPipeline` set — but none of the four do so. ✓

**Do NOT apply-state for ScreenShadow** (frame_executor.h:41 explicitly notes: "uses
tex units 0-4 incl. GL_TEXTURE_2D_ARRAY on unit 3; does NOT restore glActiveTexture").
ScreenShadow is intentionally excluded from the executor island set. ✓

**Do NOT apply-state for Composite** — its FBO is FBO-0 with a FORCE-43 viewport that
varies at runtime; the executor would need to replicate the letterbox geometry calculation.
Not a "boring" apply target. ✓

---

## SLICE PROPOSAL (not built) — FRAMEGRAPH-APPLY-STATE-ISLAND-1

### Files

| File | Change |
|---|---|
| `RenderCore/frame_executor.h` | Add `SubStageStateDesc` struct; `kSubStageStateDesc[4]` table; `findSubStageStateDesc()` lookup. SCHEMA ONLY — no GL. |
| `GameOS/gameos/gos_postprocess.cpp` | `executorOwnBeginSub`: after `willRun` check, lookup `SubStageStateDesc`; if found, call `applyPipeline` + `glBindFramebuffer` + `setSceneDrawBuffers` + `glViewport` via new pp accessor methods. `executorOwnEndSub`: add `++g_executorApplyStatePasses` on success path. Add `g_executorApplyStatePasses` static + `mc2_framegraph_executor_apply_state_passes()` extern "C". |
| `GameOS/gameos/gos_postprocess.h` | Expose 3–4 new `executor*` accessor methods: `executorSceneFbo()`, `executorWidth()`, `executorHeight()`, `executorDoSingleColorDrawBuffers()` (or split into the helper). |

### Symbols touched

- `executorOwnBeginSub` (definition + body) — `gos_postprocess.cpp:4723`
- `executorOwnEndSub` (definition + body) — `gos_postprocess.cpp:4767`
- `g_executorOwnedPasses` (parallel counter) — `gos_postprocess.cpp:4600`
- `IslandContract` / `kExecutorIslands` — read-only (no change)
- `SubStageStateDesc` / `kSubStageStateDesc` — NEW in `frame_executor.h`

### Verification (verbatim smoke)

```powershell
# Gate OFF — byte-identical to today
$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
# expect: exit 0; executor_apply_state_passes=0 in trace

# Gate ON — functionally identical, apply_state_passes > 0
$env:MC2_FRAMEGRAPH_EXECUTOR="1"; $env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs
# expect: exit 0; executor_apply_state_passes >= 1; executor_validation_failures == 0
```

### Slice 2 (future, not in this proposal)

Remove redundant body calls in `runEdgeFog` (items 1–4 listed in Q1 inventory above)
once slice 1 is verified. At that point the executor owns state-setting; body's
`applyPipeline` / `glBindFramebuffer` / etc. are deleted. FogOob follows identically.
Shoreline and CloudShadow are slice 3 or merged with slice 2.
