# SHADOW-OBSERVE-3-RECON-1: Per-Frame Dynamic Shadow — Honest Observation Without Heuristic

**Date:** 2026-06-29
**Branch:** claude/nifty-mendeleev
**Status:** RECON COMPLETE — implementation ready

---

## TL;DR and Recommended Fix

The per-frame dynamic shadow (`beginDynamicShadowPass`, txmmgr.cpp:2728) is currently **UNOBSERVED** — no `noteRenderPass` call at its begin site. Adding observation at `beginDynamicShadowPass` WITHOUT also fixing the MechOpaque note will produce out_of_order every frame.

**The ordering root is the MechOpaque-note placement** (txmmgr.cpp:2362 fires at the renderLists preamble, BEFORE the dynamic shadow block at :2674), not the shadow observation point.

**Recommended fix: Option A (declarative, zero heuristic):**
1. Add `noteRenderPass(ShadowCaster, "beginDynamicShadowPass")` inside `gosRenderer::beginDynamicShadowPass()` (gameos_graphics.cpp:6393), after `captureShadowGLState`.
2. Move `noteRenderPass(OpaqueObject)` from txmmgr.cpp:2362 (preamble) to txmmgr.cpp:3260 (`GpuMechBatcher::instance().flush()`).

Result: Shadow records first (kFramePassOrder slot 0), MechOpaque second (slot 1) — out_of_order=0 HONESTLY, no heuristic. The ambient guard does not trip (colorMaskOnEntry=Inherit skips comparison; depthFunc=ShadowLess and depthWrite=On both match). `unobserved_total` drops by 1 slot per frame.

---

## Q1: Dynamic-Shadow Ambient Timeline

### `gosRenderer::beginDynamicShadowPass()` (gameos_graphics.cpp:6393, drift-prone)

The function:
1. Captures caller GL state: `captureShadowGLState(s_dynamicPassEntry)` (:6400)
2. Saves prev FBO + viewport via `glGetIntegerv`
3. Unbinds the dynamic shadow texture from the terrain sampler unit (AMD feedback-loop prevention)
4. Disables `GL_TEXTURE_COMPARE_MODE` on the dynamic shadow texture (write mode)
5. **`glBindFramebuffer(GL_FRAMEBUFFER, pp->getDynamicShadowFBO())`** — depth-only FBO (no color attachment)
6. `glViewport(0, 0, getDynamicShadowMapSize(), getDynamicShadowMapSize())`
7. **`applyPipeline(ShadowMech)`** — sets depthTest=ON, depthMask=GL_TRUE, depthFunc=GL_LESS, cullFace=NONE (SHADOW-CASTER-APPLYPIPELINE-ROUTING-1 comment)
8. `glClearDepth(1.0f)` / `glClear(GL_DEPTH_BUFFER_BIT)` / `glClearDepth(0.0f)`
9. Applies shadow terrain material, uploads `lightSpaceMatrix`

### Depth-only enforcement: FBO attachment, NOT glColorMask

The dynamic shadow uses `pp->getDynamicShadowFBO()` registered as `RenderResourceId::ShadowDynamicMap` (gos_postprocess.cpp:3758, drift-prone). This FBO has only a depth attachment — color writes are physically prevented by the FBO, not by `glColorMask(FALSE)`. Color mask is **AllOn at the entry point** (scene's scene-FBO state) but irrelevant because no color attachment is bound.

### AmbientContract vs dynamic shadow actual state

AmbientContract row for `RenderPassId::Shadow` (`ambient_contract.h:87-92`, drift-prone):

| Axis | Declared | Dynamic shadow actual | Match? |
|---|---|---|---|
| colorMaskOnEntry | **Inherit** (comparison SKIPPED) | AllOn (from scene state; FBO-enforced depth-only) | N/A — skipped |
| depthFunc | **ShadowLess** (GL_LESS) | GL_LESS — set by `applyPipeline(ShadowMech)` | MATCH |
| depthWrite | **On** | GL_TRUE — set by `applyPipeline(ShadowMech)` | MATCH |
| disablesColorWrite | true (architectural) | true (FBO attachment) | consistent |
| viewport | ShadowMap | Shadow FBO dimensions | MATCH |

`compareAmbient` skips any axis where either side is `Inherit` (`ambient_contract.h:161-163`). Since `colorMaskOnEntry=Inherit`, the colorMask axis is entirely skipped.

**VERDICT: ambient mismatch = 0. Adding `noteRenderPass(ShadowCaster)` at `beginDynamicShadowPass` does NOT trip the ambient guard.**

---

## Q2: Producer→Consumer Ordering + MechOpaque-Note Analysis

### Actual runtime sequence inside `renderLists()` (txmmgr.cpp, all line numbers drift-prone)

```
:2362  noteRenderPass(OpaqueObject, "submit")    ← MechOpaque preamble
         [ZoneScopedN("RenderLists.Preamble") — render state setup, gos_SetRenderState]
:2599  gos_BeginShadowPrePass / gos_EndShadowPrePass  ← STATIC shadow (once-per-mission, gated)
:2653  ZoneScopedN("RenderLists.StaticPropRegistryFlush")
:2674  ZoneScopedN("RenderLists.DynamicShadowPass")
:2728    gos_BeginDynamicShadowPass()            ← Shadow PRODUCER [SLOT 0 in kFramePassOrder]
           [prop shadow draws via GpuStaticPropBatcher::drawDynamicPropShadows]
           [GpuMechBatcher::instance().flushShadow()]
:2897    gos_EndDynamicShadowPass()
           [CSM cascade replay if MC2_SHADOW_CSM]
...
:3260  GpuMechBatcher::instance().flush()        ← actual mech opaque draw [SLOT 1]
```

If both notes are added naively (shadow at :2728, MechOpaque kept at :2362), the dryrun trace records:

```
recordSeq 0: MechOpaque (declaredIndex=1 in kFramePassOrder)
recordSeq 1: Shadow     (declaredIndex=0)
```

Shadow's recordSeq (1) > declaredIndex (0) relative to MechOpaque — **out_of_order for Shadow every frame.**

### **HIGH: MechOpaque preamble note at txmmgr.cpp:2362 is mis-placed**

The note fires at the top of `renderLists()` before any actual mech GPU draw. The actual mech draw is `GpuMechBatcher::instance().flush()` at txmmgr.cpp:3260 — roughly 900 lines later, after the entire dynamic shadow block. The note was intended to mark the TGL-geometry submit window, but:
- TGL draws have their own note at tgl.cpp:3016 (`noteRenderPass(OpaqueObject)`)
- The preamble note is redundant and fires in the wrong position relative to Shadow

**Moving the note from :2362 to :3260 makes the observed sequence Shadow(slot 0) → MechOpaque(slot 1) — matching kFramePassOrder exactly.**

### Producer→Consumer: ShadowDynamicMap readers

The dynamic shadow MAP is produced at txmmgr:2728 mid-renderLists. Consumers of `ShadowDynamicMap`:
- **LOD-chunk Terrain** (gamecam.cpp:508, pre-renderLists): samples the PREVIOUS frame's map. Accepted behavior, papered over by "frameBegin() pre-seeds ShadowDynamicMap" (RenderPassContract.h:389-390). The `knownEarlyDrawSite` suppression for Terrain already handles this.
- **StaticPropOpaque** (inside renderLists, after shadow block): samples current-frame map correctly.
- **GpuMechBatcher::flush()** (txmmgr:3260, after shadow block): samples current-frame map correctly.
- **PostProcess** (gameosmain.cpp): samples current-frame map correctly.

No new pre-seed or workaround is needed for the observation fix.

---

## Q3: Fix Options — DECLARATIVE Only, No Heuristic

**Option A (RECOMMENDED): Add shadow note + move MechOpaque note**

- `gosRenderer::beginDynamicShadowPass()` (gameos_graphics.cpp:6393): add `render_contract::noteRenderPass(render_contract::PassIdentity::ShadowCaster, "gosRenderer::beginDynamicShadowPass")` after `captureShadowGLState(s_dynamicPassEntry)` (before any GL mutation, so the FBO-resolve in `dryrunRecordPass` captures the shadow FBO).
- txmmgr.cpp:2362: REMOVE the `noteRenderPass(OpaqueObject)` call from the preamble.
- txmmgr.cpp:3260: ADD `render_contract::noteRenderPass(render_contract::PassIdentity::OpaqueObject, "GpuMechBatcher_flush")` just before `GpuMechBatcher::instance().flush()`.

Resulting observed sequence: Shadow(seq=0, slot=0) → MechOpaque(seq=1, slot=1) → out_of_order=0 HONEST.

The `dryrunRecordPass` function (render_contract.cpp:724) samples `GL_DRAW_FRAMEBUFFER_BINDING` at record time and resolves it via `fboLedger()`. At the shadow note point, the FBO will be the scene/scene-FBO (captureShadowGLState saves the caller FBO; `glBindFramebuffer` to the shadow FBO happens AFTER). To ensure the shadow FBO is bound at the note, the note must fire AFTER `glBindFramebuffer(GL_FRAMEBUFFER, pp->getDynamicShadowFBO())` (line 6415). Adjust placement accordingly.

**Option B: knownEarlyDrawSite for Shadow**

Model Shadow as having a known-early exception in the dry-run. This is the runtime heuristic REVISE-1 (`ee49ee48`) explicitly removed from the static shadow. **Rejected — it masks the real ordering bug.**

**Option C: Two separate Shadow slots**

Add a second kFramePassOrder entry for the dynamic shadow. Unnecessary complexity; there is one Shadow pass per frame. **Rejected.**

**Recommendation: Option A only.**

---

## Q4: MechOpaque-Note Placement (cross-ref with MECHOPAQUE-PASSIDENTITY recon)

Current state:
- `txmmgr.cpp:2362` — preamble note (`OpaqueObject`, fires before shadow and before any mech draw)
- `tgl.cpp:3016` — TGL-path mech/vehicle/building draw note (fires at actual TGL object draw time)

The txmmgr:2362 note was created when the render contract was first wired (MechOpaque was the only GPU-path note). Now that `GpuMechBatcher::flush()` is the authoritative mech draw site, the note belongs there.

**After the fix:**
- txmmgr:3260 — note for GpuMechBatcher flush (new placement)
- tgl.cpp:3016 — note for TGL-path draws (unchanged)

Both map to `RenderPassId::MechOpaque` via `toRenderPassId(OpaqueObject)`. The dryrun records only the FIRST fire per slot per frame, so whichever fires first wins the slot. With the preamble note removed, the shadow note fires first (slot 0), then the GpuMechBatcher note fires second (slot 1). **The two recons agree: MechOpaque's seam is GpuMechBatcher::flush() at txmmgr:3260.**

---

## Q5: Same-Order Executor Ownership Readiness

The dynamic shadow has clean begin/end seams:
- `gosRenderer::beginDynamicShadowPass()` (gameos_graphics.cpp:6393)
- `gosRenderer::endDynamicShadowPass()` (gameos_graphics.cpp:6440)
- Public wrappers: `gos_BeginDynamicShadowPass()` (:9291), `gos_EndDynamicShadowPass()` (:9294)

The FBO is already registered in the ledger (`gos_postprocess.cpp:3758`, `ShadowDynamicMap`). The AmbientContract row exists (`ambient_contract.h:87-92`). `top_level_pass_executor.h:16` already documents Shadow as deferred for slice 2.

**Shadow is ready for executor ownership in a single slice after SHADOW-OBSERVE-3 is committed:**

Executor ownership requires:
1. `noteRenderPass(ShadowCaster)` at begin — adds this now (SHADOW-OBSERVE-3)
2. A `TopLevelPassContract` row in `kTopLevelExecutorPasses` (`top_level_pass_executor.h:37`) — add in slice 2
3. Calls to `executorOwnBeginTopLevel` / `executorOwnEndTopLevel` around `gos_BeginDynamicShadowPass` / `gos_EndDynamicShadowPass` — add in slice 2

No GL-state changes, no reorder, no new barriers needed. The executor is validate-only (SAME-ORDER-EXECUTOR-VALIDATE-1). **Shadow executor ownership = slice 2 of SHADOW-OBSERVE-3.**

---

## Q6: Verification Plan

### Step 1: Build + out_of_order=0 HONEST check

After the code changes (Option A):

```powershell
$env:MC2_FRAMEGRAPH_DRYRUN="1"
$env:MC2_DEBUG_STATE_DUMP="1"
$env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs
```

Check `diagnostic_trace.jsonl`:
- `out_of_order` must equal 0 (HONEST — no heuristic suppressing it)
- `unobserved_total` must be lower than baseline (Shadow slot now observed)
- `known_early_suppressed` must remain non-zero (Terrain LOD-chunk suppression still active)

### Step 2: Ambient guard no mismatch

```powershell
$env:MC2_FRAMEGRAPH_DRYRUN="1"
$env:MC2_SHADOW_ENABLE="1"
# + above env
```

Observe `[AMBIENT_MISMATCH]` log lines — must be zero for Shadow. `depthFunc=ShadowLess` and `depthWrite=On` verified by `applyPipeline(ShadowMech)`.

### Step 3: Shadow becomes observed

Confirm `unobserved_total` per frame drops from N to N-1 (Shadow slot now fires every frame when `MC2_SHADOW_ENABLE=1`).

### Step 4: Smoke 2/2

Full tier1 smoke (mc2_01 + mc2_24 inner loop, then full tier1):
```powershell
$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```

Exit 0 = pass. No regression on stock (MC2_SHADOW_ENABLE unset = dynamic shadow no-op, note still wires but record is vacuous).

---

## File Change Summary (Option A)

| File | Line (drift-prone) | Change |
|---|---|---|
| `GameOS/gameos/gameos_graphics.cpp` | :6393 `beginDynamicShadowPass` | Add `noteRenderPass(ShadowCaster)` after FBO bind (:6415) |
| `mclib/txmmgr.cpp` | :2362 | REMOVE preamble `noteRenderPass(OpaqueObject)` |
| `mclib/txmmgr.cpp` | :3260 | ADD `noteRenderPass(OpaqueObject)` before `GpuMechBatcher::flush()` |

No new headers needed (both files already include `render_contract.h`).

`gameos_graphics.cpp` includes `render_contract.h` (line 40). `txmmgr.cpp` includes `render_contract.h` (line 19).

---

## Key Line References (all drift-prone — verify via grep before coding)

- `gosRenderer::beginDynamicShadowPass` definition: `gameos_graphics.cpp:6393`
- `gosRenderer::endDynamicShadowPass` definition: `gameos_graphics.cpp:6440`
- `gos_BeginDynamicShadowPass` public wrapper: `gameos_graphics.cpp:9291`
- `captureShadowGLState(s_dynamicPassEntry)`: `gameos_graphics.cpp:6400`
- `glBindFramebuffer` to shadow FBO: `gameos_graphics.cpp:6415`
- `applyPipeline(ShadowMech)`: `gameos_graphics.cpp:6422`
- txmmgr preamble note (REMOVE): `txmmgr.cpp:2362`
- txmmgr DynamicShadowPass zone start: `txmmgr.cpp:2674`
- txmmgr `gos_BeginDynamicShadowPass` call: `txmmgr.cpp:2728`
- txmmgr `gos_EndDynamicShadowPass` call: `txmmgr.cpp:2897`
- txmmgr `GpuMechBatcher::flush()` (ADD note here): `txmmgr.cpp:3260`
- AmbientContract Shadow row: `RenderCore/ambient_contract.h:87-92`
- `kFramePassOrder` array: `RenderCore/RenderPassContract.h:394`
- `dryrunRecordPass` (samples FBO at note time): `mclib/render_contract.cpp:724`
- `top_level_pass_executor.h` DEFERRED comment: line 16 (`Shadow` listed as deferred slice 2)
