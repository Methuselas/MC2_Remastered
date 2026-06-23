# BLENDMODE-ADDITIVE-VOCABULARY-1

**Status:** SHIPPED. Splits the coarse additive blend vocabulary so the runtime
enum is as precise as the pipeline registry — the prerequisite for any
alpha/additive `applyPipeline` routing. **No behavior change for any routed
pipeline, no smoke.**

## The problem
`RenderCore::BlendMode` had a single `Additive` (→ `glBlendFunc(ONE,ONE)` in
`applyPipeline`), but the engine uses **two** distinct additive funcs:
`ONE/ONE` (tube_ribbon) and `SRC_ALPHA/ONE` (particle_billboard, vfx_mesh). The
VFX registration slice had to carry exact factors in the schema + a per-id
checker truth-table to compensate. Routing additive through `applyPipeline` would
have corrupted billboard/mesh (`SRC_ALPHA/ONE → ONE/ONE`).

## Design — split enum (minimal churn)
`RenderCore::BlendMode` (`RenderCore/PipelineDesc.h`):
```
Opaque=0  AlphaBlend=1  AlphaTest=2  Additive=3  AdditiveOneOne=4  AdditiveSrcAlphaOne=5
```
- `AdditiveOneOne` = `ONE/ONE`; `AdditiveSrcAlphaOne` = `SRC_ALPHA/ONE`.
- **`Additive` (value 3) retained as a LEGACY COARSE value** (== `ONE/ONE`) used
  ONLY by the render-contract bridge. Keeping it at value 3 preserves the four
  enum-parity `static_assert`s in `render_contract_pipeline.h` (and
  `render_contract`'s own `BlendMode` + the `kParticleEffectState` coarse pass
  advisory) **untouched** — zero churn to the render-contract subsystem.
- The pipeline-key checker now **forbids the coarse `Additive` in registered
  rows** — they must use the precise variants.

`applyPipeline` (`pipeline_binder.cpp`) now expresses **both**: `AdditiveOneOne`/
legacy `Additive` → `glBlendFunc(ONE,ONE)`; `AdditiveSrcAlphaOne` →
`glBlendFunc(SRC_ALPHA,ONE)`.

## Reassignments (audit)
- VFX registry rows: `VfxBillboardAdditive` + `VfxMeshAdditive` → `AdditiveSrcAlphaOne`;
  `VfxTubeAdditive` → `AdditiveOneOne` (both `.cpp` rows + schema `factor`).
- `TerrainDecal` / `WaterArmed`: unchanged (`AlphaBlend`).
- Remaining `BlendMode::Additive` enum-value users (audited): ONLY the
  render-contract bridge (`render_contract_pipeline.h` assert + switch) and the
  coarse `kParticleEffectState` pass advisory (`render_contract.cpp:313`) —
  intentionally the legacy coarse home; no per-pipeline registry row uses it.

## Checker (replaces the per-id truth table — "normal validation" now)
`check-pipeline-key.py`: a single `CANONICAL_BLEND` factor→(src,dst,equation) map
+ rules per registered row: factor must be a real `BlendMode` value; the coarse
`Additive` is rejected; blend-enabled rows must declare explicit src/dst/equation;
and those **must equal** the canonical for the named factor. This FAILs if
`SRC_ALPHA/ONE` is collapsed into `ONE/ONE` (declared factors stop matching the
precise name). The VFX-slice per-id `EXPECTED_BLEND` table is removed.

## Verification
- **Full build green** (mc2, isolated worktree); render-contract enum-parity
  `static_assert`s hold (Additive stays value 3); `applyPipeline` switch
  exhaustive over all 6 values.
- `pipeline_key` / `pass_coverage` / `pipeline_desc` PASS.
- **Adversarial (all FAIL, restored):** (A) collapse `VfxBillboardAdditive` src/dst
  → `ONE/ONE` → "blend collapse / drift"; (B) coarse `Additive` in a registered
  row → "COARSE legacy ... must use precise"; (C) stale factor name → "not a
  RenderCore::BlendMode value".
- **No behavior change** for any routed pipeline (none use additive; `applyPipeline`
  legacy `Additive` still `ONE/ONE`; the new cases are unused until VFX routing) →
  **no smoke**.

## Acceptance map
applyPipeline expresses both additive variants ✓ · existing Additive users
audited + assigned ✓ · VFX rows no longer need a workaround truth table ✓ ·
TerrainDecal stays AlphaBlend ✓ · WaterArmed AlphaBlend ✓ · no routed-pipeline
behavior change ✓ · checker fails on SRC_ALPHA/ONE→ONE/ONE collapse ✓ · build
green ✓ · no smoke ✓.

## Next
**VFX-APPLYPIPELINE-ROUTING-RECON-1** (recon only) — now that the vocabulary is
precise, scope routing the VFX state through `applyPipeline`, still requiring
state-lifetime mapping + a pixel-identical gate. The descriptive
TerrainOverlay/Decal and WaterArmed routings are also now unblocked (Decal/Water
are AlphaBlend, already expressible).
