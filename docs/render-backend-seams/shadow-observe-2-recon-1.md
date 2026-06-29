# SHADOW-OBSERVE-2-RECON-1: observing the shadow pass without tripping the default-ON ambient guard

**Status:** RECON COMPLETE — recommendation: Option A (relax `colorMaskOnEntry` to `Inherit`, place `noteRenderPass` at `beginShadowPrePass` after `applyPipeline`).

All file:line refs are drift-prone; re-grep before acting. Marked `[DRIFT]` where the line number depends on local edits.

---

## TL;DR

The Shadow `AmbientContract` row declares `colorMaskOnEntry=AllOff`, but `AllOff` is **never established at `beginShadowPrePass` entry**. At that point colorMask is `AllOn` (inherited from the prior frame's `beginScene` keystone at `gos_postprocess.cpp:1422`). The `glColorMask(GL_FALSE)` that the contract claims is not set by `applyPipeline(ShadowTerrain)` (colorMask is explicitly excluded from its scope), and the new shadow path (`beginShadowPrePass` / `drawShadowBatchTessellated`) never calls `glColorMask(GL_FALSE)` at all — the depth-only invariant is enforced by binding a shadow FBO that has no color attachment, not by masking. The legacy `gosPostProcess::beginShadowPass` (gos_postprocess.cpp:3467) does call `glColorMask(GL_FALSE)` but is a separate code path, not called from the pre-pass bracket.

**Consequence:** `colorMaskOnEntry=AllOff` in the `AmbientContract` is **mismodeled** — it is not an entry invariant for the instrumented code path, and is not established mid-pass either (in the new path). The honest fix is to relax it to `Inherit`.

**Recommendation: Option A** — relax `Shadow.colorMaskOnEntry` to `Inherit`, add `noteRenderPass(ShadowCaster)` at `beginShadowPrePass` after `applyPipeline` (line 6270 [DRIFT]). This gives the dry-run 2 of 3 axes checked (depthFunc=ShadowLess ✓, depthWrite=On ✓, colorMask skipped — skipped because not an entry invariant, not because wrong), guard stays clean, `ambient_mismatch_count` stays 0 on both smoke OFF and ON, Shadow becomes observed.

---

## Q1 — GL-state timeline

### colorMask

| Point | Code | colorMask value | Notes |
|---|---|---|---|
| Frame begin | `gos_postprocess.cpp:1422` [DRIFT] `glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE)` | **AllOn** | COLORMASK-ROLLOUT-1 keystone asserts at every `beginScene()` |
| `beginShadowPrePass` entry (before any of its GL calls) | inherited | **AllOn** | `captureShadowGLState` at :6243 [DRIFT] reads it; it will be AllOn |
| `applyPipeline(ShadowTerrain)` at :6270 [DRIFT] | `pipeline_binder.cpp:74-181` | **AllOn — unchanged** | `applyPipeline` only touches colorMask for opted-in rows (`rowOwnsColorMask`). "ShadowTerrain" is NOT in the opt-in set (pipeline_binder.cpp:63-72). So colorMask stays AllOn. |
| `shadow_terrain_material_->apply()` at :6288 [DRIFT] | no colorMask call | **AllOn** | material apply does not touch colorMask |
| `drawShadowBatchTessellated` per-batch | gameos_graphics.cpp:6298-6395 [DRIFT] | **AllOn** | no `glColorMask` call anywhere in this function |
| Shadow FBO depth-only enforcement | FBO has no color attachment (`DrawBufferSet::ShadowDepthOnly`, PipelineRegistry.cpp:146) | n/a | depth-only is enforced by FBO attachment, NOT by colorMask |
| `endShadowPrePass` / `restoreShadowGLState` | gameos_graphics.cpp:6148 [DRIFT] | restored to captured value (AllOn) | explicit save/restore; passes AllOn to Terrain pass |

**colorMask is AllOn throughout the entire `beginShadowPrePass` bracket in the new path.** The legacy `gosPostProcess::beginShadowPass` (gos_postprocess.cpp:3490/3512 [DRIFT]) does call `glColorMask(GL_FALSE)`, but this function is NOT invoked from the new bracket. The new path achieves depth-only by using a depth-only FBO.

### depthFunc

| Point | Code | depthFunc value |
|---|---|---|
| `applyPipeline(ShadowTerrain)` | pipeline_binder.cpp:90-94; `DepthFunc::Less` → `GL_LESS` | **GL_LESS (ShadowLess)** — valid at :6270 |
| `drawShadowBatchTessellated` | inherited from `applyPipeline` | **GL_LESS** |

**depthFunc=ShadowLess IS correctly established at the `applyPipeline` call and holds through the batch draw.**

### depthWrite

| Point | Code | depthWrite value |
|---|---|---|
| `applyPipeline(ShadowTerrain)` | pipeline_binder.cpp:84; `depthWriteEnable=true` → `glDepthMask(GL_TRUE)` | **On — valid at :6270** |
| `drawShadowBatchTessellated` | inherited | **On** |

**depthWrite=On IS correctly established at the `applyPipeline` call and holds through the batch draw.**

### Summary at `beginShadowPrePass` entry (after `applyPipeline`, before first batch)

| Axis | Declared | Actual | Valid at observe point? |
|---|---|---|---|
| colorMask | AllOff | AllOn | **NO — not established** |
| depthFunc | ShadowLess | ShadowLess | **YES** |
| depthWrite | On | On | **YES** |

---

## Q2 — Is the Shadow `colorMaskOnEntry=AllOff` mismodeled?

**YES, it is mismodeled** — in two compounding ways:

1. **New path never sets colorMask=AllOff at all.** `drawShadowBatchTessellated` (gameos_graphics.cpp:6298-6395 [DRIFT]) contains no `glColorMask` call. The depth-only invariant is enforced by `DrawBufferSet::ShadowDepthOnly` (no color attachment), not by the colorMask register. The legacy `gosPostProcess::beginShadowPass` at gos_postprocess.cpp:3490 [DRIFT] does set `glColorMask(GL_FALSE)` but is a different, non-active code path for this bracket.

2. **Even if colorMask were set mid-pass, the CAVEAT in `ambient_contract.h:135-138` [DRIFT] acknowledges this gap**: it says "colorMaskOnEntry models the state a pass ESTABLISHES … Sampling at beginPass-ENTRY may therefore legitimately differ." The CAVEAT was written for terrain (which re-asserts AllOn mid-pass, not at entry). For Shadow, the situation is stronger: the state is never established at all in the new path.

**Impact of the mismodel on `colorMaskHandshakeDeclared()`:** The `colorMaskHandshakeDeclared()` function (ambient_contract.h:171 [DRIFT]) uses `disablesColorWrite`, NOT `colorMaskOnEntry`. Shadow's `disablesColorWrite=true` — this field is correct conceptually (shadow IS depth-only) and the unit test at test_frame_graph.cpp:92 [DRIFT] checks `shadow->disablesColorWrite == true`. Setting `colorMaskOnEntry` to `Inherit` does NOT affect `disablesColorWrite`, so:

- `colorMaskHandshakeDeclared()` continues to return `true` ✓
- The unit test at test_frame_graph.cpp:89-92 [DRIFT] continues to pass ✓
- The Terrain `reassertsColorMaskAllOn=true` landmine guard is unaffected ✓

**No other consumer reads `Shadow.colorMaskOnEntry` except `compareAmbient` in `ambientProbeAtPassBegin`.** Relaxing it to `Inherit` makes `compareAmbient` skip the colorMask axis for Shadow, which is the correct behavior for an axis that has no stable per-entry value in this pass.

---

## Q3 — Alternative: observe at the per-batch draw point (Option B)

In the new path, even at the per-batch point, `drawShadowBatchTessellated` does **not** set `glColorMask(GL_FALSE)`. There is no point inside the shadow bracket (new path) where colorMask=AllOff is live. The legacy `gosPostProcess::beginShadowPass` does set it, but only on the legacy path.

**Option B is not viable for the colorMask axis in the current new-path implementation.** The only place where all 3 axes are simultaneously "as declared" is: depthFunc=ShadowLess, depthWrite=On (valid from `applyPipeline` at :6270), colorMask=AllOff (never valid in new path). There is no observation point that satisfies all 3 declared axes simultaneously.

Even setting aside colorMask, placing the observe inside `drawShadowBatchTessellated` would require a once-per-frame latch (the function is called N times per frame, one per terrain batch). That is additional complexity for no benefit over Option A, since the begin-point is already a clean once-per-call location.

---

## Q4 — depthFunc/depthWrite at entry: partial coverage analysis

With `colorMaskOnEntry=Inherit`, `ambientProbeAtPassBegin` at `beginShadowPrePass` would check:
- depthFunc: declared ShadowLess, live ShadowLess → **match, no mismatch** ✓
- depthWrite: declared On, live On → **match, no mismatch** ✓
- colorMask: declared Inherit → **skipped** (compareAmbient skips Inherit axes)

This gives **2 of 3 axes checked, 1 skipped.** The skipped axis (colorMask) is correctly modeled as "not an entry invariant for this pass" — the shadow FBO enforces depth-only without relying on the colorMask register. This is not a coverage loss; it is an honest model of what the pass actually establishes.

**Is 2/3 acceptable?** Yes. The two axes that ARE checked (depthFunc=ShadowLess, depthWrite=On) are the highest-value ones for shadow correctness — a pass that accidentally renders into shadow with GL_GEQUAL (scene depth func) or depthWrite=Off would produce a broken shadow map. The colorMask axis adds no protection here because the FBO has no color attachment to accidentally corrupt.

---

## Q5 — Option comparison and recommendation

### Option A: Relax `Shadow.colorMaskOnEntry` to `Inherit`, place `noteRenderPass` at `beginShadowPrePass` after `applyPipeline`

**Change surface:**
- `RenderCore/ambient_contract.h`: change `ColorMaskState::AllOff` to `ColorMaskState::Inherit` in the Shadow row (line 81 [DRIFT]).
- `GameOS/gameos/gameos_graphics.cpp`: add `noteRenderPass(PassIdentity::ShadowCaster, "gosRenderer::beginShadowPrePass")` after the `applyPipeline` call at line 6270 [DRIFT] (so both axes are live), removing the exclusion comment block.
- No change to `disablesColorWrite` (stays `true` — the conceptual fact is correct, just not observable at begin-entry).

**Guard impact:** `ambientProbeAtPassBegin(Shadow)` will now fire. It will check depthFunc and depthWrite (both valid), skip colorMask (Inherit), and produce **zero mismatches**. `g_ambientMismatchCount` stays 0. `scripts/check-ambient-guard.py` stays green on both smoke OFF and ON (the guard is default-ON).

**Dry-run impact:** Shadow moves from UNOBSERVED to OBSERVED in the dry-run trace. `g_dryrunUnobservedTotal` decreases, `g_dryrunObservedTotal` increases.

**Blast radius:** Minimal. One `kPassAmbient` row change (data), one `noteRenderPass` call added (no draw change).

**Byte-identical:** Yes when `MC2_FRAMEGRAPH_DRYRUN=0` and `MC2_FRAMEGRAPH_AMBIENT_GUARD=0`; in normal runs the only runtime effect is two `glGetBooleanv`/`glGetIntegerv` calls at pass begin (already paid by other observed passes).

**Weakens any real guard?** No. The colorMask axis for Shadow was never checked (Shadow was excluded from `noteRenderPass` entirely). Relaxing `colorMaskOnEntry` from `AllOff` to `Inherit` changes the behavior from "excluded → no check" to "included → Inherit → skip colorMask axis, check depthFunc+depthWrite." Net effect: two new real checks are added; nothing is removed.

**Unblocks executor?** Yes — Shadow is now modeled as an observable pass with a correct (partial) ambient contract. An executor that later owns Shadow can assert depthFunc/depthWrite at pass scheduling time.

### Option B: Batch-scope observation at the AllOff point

Not viable. In the new pre-pass path there is no point where colorMask=AllOff is live. Would require either instrumenting the legacy `gosPostProcess::beginShadowPass` path (a different bracket) or accepting that colorMask observation is impossible in the new path — which is exactly what Option A's `Inherit` modeling says. Additional complexity of a once-per-frame latch for no coverage gain.

### Option C: New field distinguishing entry-state vs established-state

Over-engineered for this case. The CAVEAT comment at `ambient_contract.h:135-138` [DRIFT] already acknowledges the distinction. Adding a new enum value or a second field to `AmbientContract` would require touching all rows, updating `compareAmbient`, and updating `ambientProbeAtPassBegin` — substantial blast radius. Reserve for a future "the ledger grows to cover mid-pass state" phase.

**Recommendation: Option A.** Smallest blast radius, guard-clean, honest model, unblocks executor, adds real coverage for the two axes that matter most for shadow correctness.

---

## SLICE PROPOSAL (not built)

### Files to change

**`RenderCore/ambient_contract.h` line ~81 [DRIFT]:**
```cpp
// Before:
{ RenderPassId::Shadow,
  ColorMaskState::AllOff, /*reassert*/ false, /*disables*/ true,
  DepthFuncState::ShadowLess, ViewportKind::ShadowMap,
  /*producesLatch*/ false, /*consumesLatch*/ false,
  "depth-only; color write OFF; GL_LESS; shadow-atlas viewport",
  /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::On },

// After:
{ RenderPassId::Shadow,
  ColorMaskState::Inherit,   // SHADOW-OBSERVE-2: colorMask NOT established at begin-entry
                              // in the new pre-pass path (depth-only via FBO, not colorMask);
                              // Inherit skips the axis in compareAmbient. disablesColorWrite
                              // stays true (conceptual fact: it IS depth-only).
  /*reassert*/ false, /*disables*/ true,
  DepthFuncState::ShadowLess, ViewportKind::ShadowMap,
  /*producesLatch*/ false, /*consumesLatch*/ false,
  "depth-only via FBO; GL_LESS; shadow-atlas viewport; colorMask Inherit (FBO enforces)",
  /*blend*/ BlendState::Inherit, /*depthWrite*/ DepthWriteState::On },
```

**`GameOS/gameos/gameos_graphics.cpp` around line 6274 [DRIFT]:**

Remove the DRYRUN-OBSERVE-COVERAGE-1 exclusion comment block and add the `noteRenderPass` call after `applyPipeline` (so depthFunc and depthWrite are established):
```cpp
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowTerrain), "ShadowTerrain");
    // SHADOW-OBSERVE-2: Shadow is now observable. colorMaskOnEntry=Inherit (depth-only via
    // FBO, not colorMask register). depthFunc=ShadowLess and depthWrite=On are both live
    // here (applyPipeline sets them above). ambient guard checks these 2 axes; skips colorMask.
    render_contract::noteRenderPass(render_contract::PassIdentity::ShadowCaster,
                                    "gosRenderer::beginShadowPrePass");
    render_contract::assertPassContract(render_contract::PassIdentity::ShadowCaster,
                                        "gosRenderer::beginShadowPrePass");
```

(The existing `assertPassContract` call stays; `noteRenderPass` is added before it or the two can be merged if `assertPassContract` already calls `noteRenderPass` internally — verify by reading `assertPassContract` before building.)

**`tests/unit/test_frame_graph.cpp`:** Add a test asserting `Shadow.colorMaskOnEntry == Inherit` (and separately that `Shadow.disablesColorWrite == true` stays). The existing test at line 92 [DRIFT] checks `disablesColorWrite` — keep it. Add alongside:
```cpp
CHECK(shadow->colorMaskOnEntry == ColorMaskState::Inherit);
```

No other test files reference `Shadow.colorMaskOnEntry` directly.

---

## Q6 — Verification plan

### Step 1: Build + smoke gate

```powershell
$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```

Expected: exit 0, 2/2 missions pass.

### Step 2: Ambient guard stays clean (default-ON guard; no MC2_FRAMEGRAPH_AMBIENT_GUARD=0 override)

```powershell
$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,AMBIENT"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs
```

Then:
```powershell
py -3 scripts/check-ambient-guard.py debug_state/diagnostic_trace.jsonl
```

Expected: `ambient_mismatch_count=0`, `ambient_probe_samples > 0` (Shadow now contributes samples), exit 0.

### Step 3: Shadow now appears as OBSERVED in dry-run trace

```powershell
$env:MC2_FRAMEGRAPH_DRYRUN="1"; $env:MC2_DEBUG_STATE_DUMP="1"; ...
```

Run mission mc2_01. Check that `[RENDER_PASS v1]` lines include a Shadow/ShadowCaster entry. Check that `g_dryrunUnobservedTotal` decreases (Shadow no longer in the unobserved bucket) and `g_dryrunObservedTotal` increases.

### Step 4: `out_of_order` stays 0

With `MC2_RENDER_PASS_ORDER=1`, confirm no order violations introduced by the new observe point. Shadow should appear before Terrain in the frame order (it renders into the shadow atlas before the scene pass).

### Step 5: Unit tests

```powershell
cd build64; ctest -R test_frame_graph -V
```

Expected: all `ambient ledger: *` tests pass, including the new `shadow->colorMaskOnEntry == Inherit` check.

### Step 6: Smoke with guard fatal (catch any regressions from other passes)

```powershell
$env:MC2_FRAMEGRAPH_AMBIENT_FATAL="1"; ...run inner-loop smoke...
```

Expected: no abort (no ambient mismatches anywhere).

---

## HIGH findings

**HIGH: `Shadow.colorMaskOnEntry=AllOff` is structurally mismodeled for the new pre-pass path.** The ambient contract declares an entry invariant that is not established — not at begin, not mid-pass — in the active code path (`beginShadowPrePass` / `drawShadowBatchTessellated`). The depth-only behavior is correctly enforced by the FBO attachment, but the colorMask register is AllOn throughout. The exclusion comment at gameos_graphics.cpp:6276-6285 [DRIFT] correctly diagnosed the *symptom* (mismatch would fire) but the root cause is that the declaration is wrong, not just that the observation point is wrong.

**Finding: relaxing `colorMaskOnEntry` to `Inherit` does NOT weaken any real guard.** The `colorMaskHandshakeDeclared()` invariant and the Terrain-reasserts-AllOn guard depend on `disablesColorWrite` (which stays `true`) and `reassertsColorMaskAllOn` (Terrain, unchanged). No consumer of `colorMaskOnEntry` for Shadow exists except `compareAmbient`, which correctly skips `Inherit` axes.

**Finding: the new shadow path enforces depth-only via FBO, not colorMask.** This is the architecturally correct approach (depth attachment only, `DrawBufferSet::ShadowDepthOnly`, PipelineRegistry.cpp:146 [DRIFT]). The legacy `gosPostProcess::beginShadowPass` (gos_postprocess.cpp:3490 [DRIFT]) used `glColorMask(GL_FALSE)` as a belt-and-suspenders guard on an FBO that may have had a color attachment. The new path has no such ambiguity.
