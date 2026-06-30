# RECON — FRAMEGRAPH-STATEPACK-1-RECON-1

Per-pass GL-state declaration model (reusing PipelineDesc) and first-safe-slice proposal.
Phase 5 of the render-frame-graph arc. Read-only; proposes `RenderStateDesc` shape — does
NOT build it.

**Branch tip:** `claude/nifty-mendeleev` (2026-06-29). All `file:line` refs drift-prone — re-grep before acting.

---

## TL;DR

PipelineDesc already covers blend (via BlendMode), depth (test/write/func), cull, frontFace,
polygonOffset, colorAttachments, and drawBuffers — 8 of the 13 major axes. The ambient_contract.h
ledger already covers colorMask, depthFunc (as a per-pass override), viewport (via ViewportKind),
blend (Inherit, proven globally-on), and depthWrite. The FBO ledger maps live GLuints to logical
targets. Together these three systems cover the low-risk axes.

**What PipelineDesc does NOT cover that matters for a StatePack:**
- Viewport actual pixel dimensions (only "MainScene" or "ShadowMap" kind is declared)
- Scissor (unowned, frame-global)
- Stencil (unowned, cleared once at frame start, never per-pass)
- Texture-unit/sampler bindings (deferred; NVIDIA latch leak is live)
- VAO / input layout (per-pass save/restore, not declared)

**Recommended first-safe slice:** Add a `RenderStateDesc` struct that COMPOSES an existing
`PipelineId` by reference and adds the four already-sampled ambient axes (colorMask/depthFunc
/depthWrite via ambient_contract.h + logical FBO via fbo_ledger.h) as per-pass declared fields.
This formalizes what is already runtime-sampled into one typed struct per pass — no new GL calls,
no new guards, zero risk.

**Blend-semantic enum:** PipelineDesc::BlendMode already IS the semantic enum (Opaque,
AlphaBlend, AlphaTest, AdditiveOneOne, AdditiveSrcAlphaOne, Multiply) — it must be REUSED
not duplicated. The ambient_contract BlendState{On/Off} is proven wrong for MC2 (blend is
globally-on; measured by runtime probe). Drop BlendState from the StatePack; use BlendMode
from the PipelineDesc row.

**HIGH finding — blend is intractable at the AMBIENT level:** ambient_contract.h's BlendState
is correctly Inherit for all passes because the runtime probe proved GL_BLEND is globally
enabled. Any StatePack field that tries to declare per-pass blend ENABLE/DISABLE is provably
wrong. The semantic is in PipelineDesc::BlendMode (and applyPipeline emits the correct
glBlendFunc); the StatePack does not need to add a blend field.

---

## Q1 — Axis-Coverage Table

Axis = a single GL state axis that matters for a pass. Columns: which system already declares it.

| Axis | PipelineDesc / applyPipeline | ambient_contract.h | fbo_ledger.h | Hand-set (ad hoc) | Notes |
|------|----------------------------|--------------------|--------------|-------------------|-------|
| **FBO target (logical)** | `drawBuffers: DrawBufferSet` (descriptive, NOT emitted by applyPipeline) | — | `kPassFboTarget[]` → `declaredFboTarget()` | raw `glBindFramebuffer` in gos_postprocess.cpp | fbo_ledger maps GLuint→RenderResourceId at runtime; partial coverage (4 of 11 passes declared) |
| **Viewport kind** | — | `ViewportKind` {MainScene/ShadowMap} declared per pass | — | `glViewport` scattered: gameosmain.cpp:521, gos_postprocess.cpp:1344/1448/1951+, gameos_graphics.cpp:6085/6204/6455 | Kind declared; pixel dimensions are NOT — they depend on runtime resolution |
| **Scissor** | — | — | — | none — GL scissor set once in gameosmain.cpp:563 (cleared), never per-pass | **Unowned / unmodeled** |
| **Depth func** | `depthFunc: DepthFunc` applied by applyPipeline | `DepthFuncState` {SceneGEqual/ShadowLess} for routed passes | — | raw `glDepthFunc` in legacy non-routed draws (terrain solid, water, VFX) | applyPipeline owns routed passes; non-routed hand-set |
| **Depth write** | `depthWriteEnable: bool` applied by applyPipeline | `DepthWriteState` {On} for 4 passes — runtime-verified (dwMiss=0) | — | `glDepthMask` in non-routed draws | dual coverage; ambient_contract checked and confirmed clean |
| **Depth test enable** | `depthTestEnable: bool` applied by applyPipeline | — | — | `glEnable/Disable(GL_DEPTH_TEST)` in non-routed draws | Only in PipelineDesc; non-routed hand-set |
| **Color mask (per-attachment)** | `colorAttachments: ColorAttachmentMask` opt-in via applyPipeline (COLORMASK-OWNERSHIP-1, default-OFF gate) | `colorMaskOnEntry: ColorMaskState` {AllOn/AllOff/Inherit} | — | `glColorMask/glColorMaski` ad-hoc in shadow passes and terrain repair | applyPipeline opt-in expanding (7 PostProcess passes opted-in); ambient_contract declares terrain re-assert landmine |
| **Blend enable+func** | `blend: BlendMode` applied by applyPipeline (`glBlendFunc` + `glEnable/Disable(GL_BLEND)`) | `BlendState` {Inherit} for all passes (runtime-proven globally-on) | — | raw `glBlendFunc(savedSrcRGB,...)` save/restore in legacy paths (gameos_graphics.cpp:3250/3595/3611/…) | applyPipeline owns routed passes; legacy hand-set via save/restore |
| **Cull mode** | `cullMode: CullMode` applied by applyPipeline | — | — | `glCullFace / glEnable/Disable` in non-routed draws | applyPipeline owns routed |
| **Front face winding** | `frontFace: FrontFace` applied by applyPipeline (PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1) | — | — | none (process-wide Ccw default; only shadow bracketed CCW save/restore in gameos_graphics.cpp) | Clean |
| **Polygon offset enable** | `polygonOffsetEnable: bool` applied by applyPipeline (SHADOW-CASTER-APPLYPIPELINE-ROUTING-1) | — | — | magnitude (`glPolygonOffset`) left dynamic at call site | Enable/disable in PipelineDesc; magnitude hand-set |
| **GL program (shader)** | `glProgramName` applied by applyPipeline (skips if 0) | — | — | `glUseProgram` in non-routed draws | Clean for routed; non-routed hand-set |
| **VAO / input layout** | `VertexLayoutId` recorded in PipelineRegistry (metadata, NOT set by applyPipeline) | — | — | `glBindVertexArray` at each draw site with save/restore (gameos_graphics.cpp:3597/3613/3694/…, gos_mech_batcher.cpp:1003/1064) | **Unmodeled as executable.** VertexLayoutId is metadata only; applyPipeline does NOT bind VAOs |
| **Texture bindings** | `ssboBindingsMask` (SSBO slots only, NOT tex units) | — | — | `glActiveTexture` + `glBindTexture` ad hoc at every draw site; deferred tex-latch issue live (NVIDIA 2D_ARRAY leak, tex-latch-executor-recon-1.md) | **Deferred / intractable now** |
| **Stencil** | — | — | — | `glClearStencil` once at frame start (gameosmain.cpp:563); no per-pass stencil ops found | **Unowned / unmodeled** — not used as a per-pass discriminator |
| **Draw buffers (MRT mask)** | `drawBuffers: DrawBufferSet` + `colorAttachments` (descriptive) | — | — | `setSceneDrawBuffers()` in gos_postprocess.cpp (lines ~225/235/241); `check-drawbuffer-ownership.py` enforces lockstep | Descriptive in PipelineDesc; live authority is `setSceneDrawBuffers()` chokepoint |

---

## Q2 — PipelineDesc Overlap Map

**PipelineDesc covers:** blend (BlendMode semantic), depthTest, depthWrite, depthFunc, cullMode,
frontFace, polygonOffsetEnable, glProgramName, colorAttachments, drawBuffers, ssboBindingsMask.
That is the COMPLETE fixed-function pipeline state minus viewport/scissor/stencil/VAO/textures.

**PipelineId → pass mapping** (from `RenderCore/PipelineRegistry.h:25-91` and
`RenderCore/RenderPassContract.h:163-341`):

| PipelineId | Pass (RenderPassId) | Routed via applyPipeline? | `pipelineDescRegistered` in contract |
|------------|---------------------|--------------------------|--------------------------------------|
| StaticPropOpaque (1) | StaticPropOpaque | YES | true |
| StaticPropAlphaTest (2) | StaticPropOpaque (alpha variant) | YES | true |
| MechOpaque (3) | MechOpaque | YES | true |
| StaticPropDepth (4) | StaticPropOpaque (depth-prepass) | YES | true |
| ShadowTerrain (5) | Shadow | DESCRIPTIVE only | false |
| ShadowStaticProp (6) | Shadow | DESCRIPTIVE only | false |
| ShadowMech (7) | Shadow | DESCRIPTIVE only | false |
| TerrainOverlay (8) | TerrainOverlay | YES (gameos_graphics.cpp:9811/9931) | false (contract stale) |
| TerrainDecal (9) | TerrainDecal | YES (gameos_graphics.cpp:10006) | false (contract stale) |
| WaterArmed (10) | Water | DESCRIPTIVE only | false |
| VfxBillboard{Alpha,Additive} (11,12) | VFX | YES (gos_particle_bridge.cpp:1265) | false |
| VfxTube{Alpha,Additive} (13,14) | VFX | YES (gos_particle_bridge.cpp:849) | false |
| VfxMesh{Alpha,Additive} (15,16) | VFX | YES (gos_vfx_mesh_bridge.cpp:315) | false |
| PostProcessComposite (17) | PostProcess | YES (gos_postprocess.cpp:2543) | false |
| PostProcess{ScreenShadow,CloudShadow,Shoreline,SsaoApply} (21-24) | PostProcess | YES | false |
| PostProcess{EdgeFog,FogOob} (18,19) | PostProcess | YES | false |
| TerrainSolid (20) | Terrain | DESCRIPTIVE only | false |

**Observation:** Nine passes use `applyPipeline` with a registered PipelineId but have
`pipelineDescRegistered=false` in `kRenderPassContracts[]`. The contract table has not
been updated to match the routing reality. The StatePack must treat PipelineDesc as the
ground truth (not the contract bool).

**The StatePack MUST NOT re-declare axes PipelineDesc already owns.** The struct should
embed a `PipelineId` (or a `const PipelineDesc*`) and add only the unowned axes:
colorMask (ambient), viewport kind (ambient), depthWrite (ambient duplicate for validation),
logical FBO (fbo_ledger). Everything else is read from the PipelineDesc row.

---

## Q3 — Blend-Semantic Proposal

**PipelineDesc::BlendMode IS already the semantic enum.** The existing values
(`RenderCore/PipelineDesc.h:42-50`) are:

| BlendMode | glBlendFunc | Usage in MC2 | Passes |
|-----------|-------------|--------------|--------|
| `Opaque` (0) | ONE / ZERO + disable | Standard opaque | StaticPropOpaque, MechOpaque, StaticPropDepth, TerrainSolid, PostProcessComposite |
| `AlphaBlend` (1) | SRC_ALPHA / ONE_MINUS_SRC_ALPHA | Standard alpha transparency | TerrainDecal, WaterArmed, VfxBillboardAlpha, VfxTubeAlpha, VfxMeshAlpha, PostProcess{EdgeFog,FogOob} |
| `AlphaTest` (2) | ONE / ZERO + disable (shader discard) | Alpha-tested geometry | StaticPropAlphaTest |
| `Additive` (3) | ONE / ONE (legacy coarse; render-contract bridge only) | DEPRECATED in new rows | none — legacy bridge only |
| `AdditiveOneOne` (4) | ONE / ONE | Tube VFX additive | VfxTubeAdditive |
| `AdditiveSrcAlphaOne` (5) | SRC_ALPHA / ONE | Billboard/mesh VFX additive | VfxBillboardAdditive, VfxMeshAdditive |
| `Multiply` (6) | DST_COLOR / ZERO | Multiplicative screen darkening | PostProcess{ScreenShadow,CloudShadow,Shoreline,SsaoApply} |

These map cleanly. No blend equation variant is used (FUNC_ADD is GL default throughout;
no `glBlendEquation` calls in non-debug paths — `debug_renderer.cpp:357` uses
`glBlendEquationSeparate` but that is a debug tool, not a pass). **All 11 passes with
registered PipelineIds fit into these 6 semantic categories with no residual.**

**Passes that CANNOT be categorized via PipelineDesc BlendMode (hand-set only):**

- **Shadow** (ShadowTerrain/Mech/StaticProp descriptive rows): depth-only, no color blend.
  DrawBufferSet::ShadowDepthOnly means no fragment output — blend mode is irrelevant.
  Declare as `Opaque` (or a new `DepthOnly` value) — NOT a hard categorization problem.
- **Terrain solid** (TerrainSolid, descriptive only): `Opaque`, GEQUAL, cull None. Clean.
- **Water armed** (WaterArmed, descriptive only): `AlphaBlend`. Clean.
- **UI / HUD** (no PipelineId row): HUD uses the legacy `gos_SetRenderState` replay path
  (`gameos_graphics.cpp:7509-7647`) with dynamic `gos_AlphaMode` values {OneZero, OneOne,
  AlphaInvAlpha, OneInvAlpha, AlphaOne} (gameos_graphics.cpp:5555-5559).
  **HIGH — UI blend is not statically declarable.** The GameOS 2D batch replays saved
  render states per primitive; blend is runtime-dynamic per draw. A StatePack for UI can
  only declare `Inherit`. This is expected — UI is the one pass where per-draw blend is
  legitimate.
- **VFX particle legacy path** (gos_particle_bridge.cpp:603-604): the dead legacy path
  uses `glBlendFunc(GL_ONE, GL_ONE)` or `GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA`. The live
  deferred path routes through PipelineId, so the legacy path is not a StatePack concern.

**Recommendation:** The StatePack does NOT define a new blend enum. It stores a `PipelineId`
and the executor reads `getPipelineDesc(id).blend` to get the BlendMode. For UI (no PipelineId),
store `PipelineId::Invalid` and let the executor skip blend validation.

---

## Q4 — Unsolved Packs Verdict

### 4a. Blend semantics
**RESOLVED by PipelineDesc::BlendMode.** Already the semantic enum. The ambient_contract
`BlendState` is correctly Inherit for all passes and should remain so — it measures GL_BLEND
enable/disable, which is globally-on in MC2 and not a useful per-pass discriminator.
The StatePack does not need a blend field; it inherits via PipelineId.

### 4b. Texture units / samplers
**INTRACTABLE now.** Confirmed: `glActiveTexture` is called ad hoc at dozens of sites
(gameos_graphics.cpp:2843/2874/2896/3523-3578/…); there is no per-pass sampler declaration.
The NVIDIA 2D_ARRAY residual leak is a live open issue (tex-latch-executor-recon-1.md).
`ssboBindingsMask` in PipelineDesc covers SSBO slots 0-31 but not GL_TEXTURE_* units.
**Proposal:** defer. Add a `uint32_t texUnitClaimMask` field to PipelineDesc (bit N = unit N)
after a dedicated SAMPLER-UNIT-AUTHORITY recon maps which pass claims which units. The
fbo_ledger / ambient pattern (declare → sample → compare, Inherit = skip) applies here too.
Do NOT include in first-safe slice.

### 4c. VAO / input state
**Partially modeled, not executable.** `VertexLayoutId` is recorded in PipelineRegistry
(metadata), not set by applyPipeline. Per-draw `glBindVertexArray` save/restore is
pervasive (gameos_graphics.cpp, gos_mech_batcher.cpp). The StatePack can carry a
`VertexLayoutId` as a declaration, but the executor cannot apply it without knowing the
actual GLuint. This requires a VAO-ledger analogous to fbo_ledger (VertexLayoutId → GLuint).
**Proposal:** record `VertexLayoutId` in the StatePack (already known from PipelineRegistry);
build VAO-ledger in a later slice (VERTEXLAYOUT-EXECUTOR-RECON-1).

### 4d. Scissor
**UNMODELED. Not per-pass.** No evidence of per-pass scissor use. Stencil buffer is
cleared once per frame. Neither is a StatePack concern for the current arc.
**Verdict:** explicitly omit from StatePack. Add a `bool scissorEnabled = false` and
`bool stencilEnabled = false` declaration later only if a new pass needs them.

### 4e. Viewport edge cases
**Partially handled.** ViewportKind {MainScene, ShadowMap, Inherit} covers the two cases.
The actual pixel dimensions are runtime values (width_/height_ from gos_postprocess;
shadow map size from getDynamicShadowMapSize()). FORCE-43 re-sets viewport before
composite (gameos_graphics.cpp). An executor must re-apply viewport from runtime state,
not from the StatePack.
**Proposal:** StatePack declares `ViewportKind`; executor resolves to actual
(width, height) at apply-time from the render-state singleton. This is minimal and safe.

---

## Q5 — Proposed RenderStateDesc

```cpp
// RenderCore/RenderStateDesc.h  (proposed — Phase 5, FRAMEGRAPH-STATEPACK-1)
//
// Per-pass GL-state declaration. COMPOSES PipelineDesc by id (does NOT re-declare
// its fields). Adds only the axes NOT already owned by PipelineDesc:
//   - colorMaskOnEntry  (ambient_contract.h: AllOn / AllOff / Inherit)
//   - depthWrite        (ambient_contract.h: On / Off / Inherit; validated by runtime probe)
//   - depthFunc         (ambient_contract.h: SceneGEqual / ShadowLess / Inherit)
//   - viewport          (ambient_contract.h: MainScene / ShadowMap / Inherit)
//   - fboTarget         (fbo_ledger.h:  MainColor / ShadowDynamicMap / Backbuffer / Unknown)
//
// DELIBERATELY OMITTED (deferred):
//   - blend (semantic)      → PipelineDesc::BlendMode via pipelineId
//   - cull / frontFace      → PipelineDesc via pipelineId
//   - depth-test enable     → PipelineDesc via pipelineId
//   - polygon offset        → PipelineDesc via pipelineId
//   - colorAttachments      → PipelineDesc via pipelineId
//   - drawBuffers           → PipelineDesc::DrawBufferSet via pipelineId
//   - VAO / input layout    → VertexLayoutId in PipelineRegistry (metadata; not executable yet)
//   - texture units         → deferred (SAMPLER-UNIT-AUTHORITY-RECON-1)
//   - scissor / stencil     → not per-pass; omit for now

#pragma once
#include "PipelineRegistry.h"       // PipelineId
#include "ambient_contract.h"       // ColorMaskState, DepthFuncState, DepthWriteState, ViewportKind
#include "RenderResourceRegistry.h" // RenderResourceId (fboTarget)

namespace RenderCore {

struct RenderStateDesc {
    // Which registered pipeline row covers this pass's blend/depth/cull/program.
    // Invalid = pass not yet migrated (executor skips pipeline application for this pass).
    PipelineId              pipelineId  = PipelineId::Invalid;

    // Ambient axes NOT owned by PipelineDesc. Default Inherit = executor skips validation.
    framegraph::ColorMaskState  colorMask  = framegraph::ColorMaskState::Inherit;
    framegraph::DepthWriteState depthWrite = framegraph::DepthWriteState::Inherit;
    framegraph::DepthFuncState  depthFunc  = framegraph::DepthFuncState::Inherit;
    framegraph::ViewportKind    viewport   = framegraph::ViewportKind::Inherit;

    // Logical FBO this pass renders into (resolved at runtime via fboLedger).
    // Unknown = not declared; executor skips FBO validation for this pass.
    RenderResourceId        fboTarget  = RenderResourceId::Unknown;

    // Future sub-slices (leave Inherit / Invalid until their dedicated recon lands):
    // uint32_t texUnitClaimMask = 0;        // SAMPLER-UNIT-AUTHORITY-RECON-1
    // VertexLayoutId vaoLayout = VertexLayoutId::Invalid; // VERTEXLAYOUT-EXECUTOR-RECON-1
};

} // namespace RenderCore
```

**Runtime validation pattern** (same as ambient_contract compareAmbient + fboMismatch):
```
At noteRenderPass(id) seam:
  const RenderStateDesc& decl = kPassStateDesc[pass];
  if (decl.pipelineId != PipelineId::Invalid)
      pipeline_binder::applyPipeline(getPipelineDesc(decl.pipelineId), dbgName);
  // ambient axes: sample GL state, call compareAmbient (pure, offline-testable)
  // fbo: sample GL_DRAW_FRAMEBUFFER_BINDING, fboLedger.resolve(), compare decl.fboTarget
```

**Offline-testable:** RenderStateDesc is pure POD (no GL headers). The validation logic
mirrors the existing `compareAmbient` + `fboMismatch` pattern, which already has unit
tests in `tests/unit/test_frame_graph.cpp`.

---

## Q6 — Sequencing

### First-safe slice (lowest risk, already sampled)

**FRAMEGRAPH-STATEPACK-SLICE-1:** Introduce `RenderCore::RenderStateDesc` as a pure-header
struct (no GL calls). Populate `kPassStateDesc[]` by COPYING the already-declared fields from:
- `kPassAmbient[]` (ambient_contract.h): colorMask, depthWrite, depthFunc, viewport
- `kPassFboTarget[]` (fbo_ledger.h): fboTarget
- `kRenderPassContracts[]` / PipelineRegistry: pipelineId

For passes that already route through applyPipeline, set `pipelineId = PipelineId::Foo`.
For non-routed passes, set `pipelineId = PipelineId::Invalid`.

Add a `compareStateDesc(decl, sample)` function analogous to `compareAmbient` — pure,
no GL. Offline test. No runtime enforcement yet (keep as measure-only, default-OFF gate).

**Risk:** zero. All fields are already individually sampled and declared in their respective
systems. This slice just unifies them into one typed struct per pass.

### Second slice — executor apply

**FRAMEGRAPH-STATEPACK-SLICE-2:** Make the executor call
`pipeline_binder::applyPipeline(getPipelineDesc(decl.pipelineId), dbgName)` at pass entry
for passes with a valid `pipelineId`. This replaces the scattered per-pass applyPipeline
calls — the same call, one canonical site.

Gate on `MC2_STATEPACK_APPLY` (default-OFF); byte-identical gate-ON is the ship criterion.
This requires that `pipelineDescRegistered` in `kRenderPassContracts[]` is updated to
match the actual routing reality first (9 passes are mis-flagged false today — see Q2).

### Third slice — blend-semantic / texture-unit / VAO

These need their own recon:
- **Blend:** No additional work needed — BlendMode in PipelineDesc is correct. The only
  follow-up is updating the contract `pipelineDescRegistered` booleans.
- **Texture units:** SAMPLER-UNIT-AUTHORITY-RECON-1 (map unit→pass). High complexity.
  The NVIDIA 2D_ARRAY leak must be fixed before declaring tex-unit contracts.
- **VAO:** VERTEXLAYOUT-EXECUTOR-RECON-1 (build VAO-ledger, make applyPipeline bind
  the correct VAO by VertexLayoutId).

### How this feeds SAME-ORDER-EXECUTOR

The executor applies `RenderStateDesc.pipelineId` via applyPipeline at pass entry (Slice 2),
then executes the pass body (unchanged). The ambient axes in `RenderStateDesc` enable the
executor to VALIDATE the live GL state matches the declaration (using the existing
`compareAmbient` + `fboMismatch` infrastructure). The executor does not APPLY the ambient
axes directly — they are still set by the pass body's existing code (terrain re-asserts
colorMask, shadow binds its FBO, etc.). The StatePack is a declaration model, not a full
state-setter, until Slice 2+ when applyPipeline takes over more axes.

---

## Key file references (all drift-prone — re-grep before acting)

| File | Role |
|------|------|
| `RenderCore/PipelineDesc.h:93-135` | `PipelineDesc` struct — all axes applyPipeline owns |
| `RenderCore/PipelineRegistry.h:25-91` | `PipelineId` enum + `getPipelineDesc()` |
| `GameOS/gameos/pipeline_binder.cpp:74-209` | `applyPipeline()` GL implementation |
| `RenderCore/ambient_contract.h:25-198` | `ColorMaskState`, `DepthFuncState`, `BlendState`, `DepthWriteState`, `ViewportKind`; per-pass table; `compareAmbient()` |
| `RenderCore/fbo_ledger.h:22-73` | `FboLedger`, `kPassFboTarget[]`, `fboMismatch()` |
| `RenderCore/RenderPassContract.h:47-391` | `RenderPassId`, `kRenderPassContracts[]`, `kFramePassOrder[]` |
| `mclib/render_contract.cpp:939-962` | `sampleColorMask()`, `sampleDepthFunc()`, `sampleBlend()`, `sampleDepthWrite()` — GL sampling |
| `GameOS/gameos/gameos_graphics.cpp:3287/4041/6211/6461/9810/9930/10005` | `applyPipeline` call sites (7 different passes) |
| `GameOS/gameos/gos_postprocess.cpp:1988/2051/2193/2249/2303/2359/2543` | PostProcess family applyPipeline sites |
