# VFX-PIPELINE-REGISTRATION-1

**Status:** SHIPPED. Registers the 6-row VFX/particle family as DESCRIPTIVE
PipelineDesc rows and moves Water... VFX UNREGISTERED → DESCRIPTIVE_REGISTERED.
**No applyPipeline routing, no shader/blend changes, no SPIR-V, no particle
rewrite, no draw-buffer/compute modeling, no new PipelineDesc field, no smoke.**

## The 6 rows (per VFX-PIPELINE-REGISTRATION-RECON-1, source-verified)

All share the invariant: `depthTest on · depthWrite OFF · GEQUAL · cull None ·
frontFace Ccw · color0 only · no polygon offset · FUNC_ADD`. The only difference
is blend; the **exact factors** (recon-verified) are recorded in the schema
because `BlendMode::Additive` is coarse.

| PipelineId | program | BlendMode | exact factors |
|---|---|---|---|
| VfxBillboardAlpha=11 | particle_billboard | AlphaBlend | SRC_ALPHA / ONE_MINUS_SRC_ALPHA |
| VfxBillboardAdditive=12 | particle_billboard | Additive | **SRC_ALPHA / ONE** |
| VfxTubeAlpha=13 | tube_ribbon | AlphaBlend | SRC_ALPHA / ONE_MINUS_SRC_ALPHA |
| VfxTubeAdditive=14 | tube_ribbon | Additive | **ONE / ONE** |
| VfxMeshAlpha=15 | vfx_mesh | AlphaBlend | SRC_ALPHA / ONE_MINUS_SRC_ALPHA |
| VfxMeshAdditive=16 | vfx_mesh | Additive | **SRC_ALPHA / ONE** |

All 3 VFX frags write a single output (color0 only; no objectID — consistent with
the `vfx_no_objectid` contract), so `colorAttachments = {color0:true,...}`.

## The blend-collapse guard (the key new check)

`BlendMode::Additive` cannot encode `SRC_ALPHA/ONE` (billboard/mesh) vs `ONE/ONE`
(tube). So:
- the schema `blendState` carries explicit `srcFactor/dstFactor/equation` per row;
- `check-pipeline-key.py` gains a recon-verified `EXPECTED_BLEND` ground-truth
  table + a rule that **every blend-enabled row must declare explicit factors**
  and they **must match** the table. This FAILs if any additive row is collapsed
  (e.g. billboard `SRC_ALPHA/ONE` silently changed to `ONE/ONE`) or drifts.

`check-pass-coverage.py` gained `pipelineIds` (list) support so the single VFX
family entry can map to all 6 PipelineIds.

## Verification

- **Full build green** (mc2, isolated worktree); `static_assert(s_descs==Count_=17)` holds.
- `pipeline_key` / `pass_coverage` / `pipeline_desc` all PASS (pipeline_desc 2
  benign WARN for TerrainOverlay/TerrainDecal; VFX rows reported as descriptive).
- **Adversarial (all FAIL, then restored):** (A) collapse VfxBillboardAdditive
  → `ONE/ONE` → "additive collapse / blend drift"; (B) drop a row's srcFactor →
  "missing explicit srcFactor/dstFactor/equation"; (C) drop VfxMeshAdditive schema
  entry → "missing from schema"; (D) drop a VFX pipelineId from the ledger →
  "missing from ledger".
- **No GL behavior change** (descriptive rows, glProgramName=0, not routed) → no smoke.

## Build gotcha logged
A `{`/`}` inside an enum-member **comment** truncated `check-pipeline-key.py`'s
enum regex (`\{(.*?)\}`, strips only block comments) — it parsed only the first
11 ids and FAILed the 6 VFX as "not in enum". Fixed by removing braces from the
comment. **Rule: no `{`/`}` in PipelineId enum-member comments.**

## Exclusions held
No applyPipeline routing · no shader/blend changes · no SPIR-V · no particle
rewrite · no draw-buffer modeling · no compute modeling · no new field.

## Next (DO NOT route VFX yet)
1. **BLENDMODE-ADDITIVE-VOCABULARY-1** — split `Additive` into `SRC_ALPHA/ONE`
   vs `ONE/ONE` (new BlendMode value or explicit src/dst fields) so applyPipeline
   can drive VFX additive byte-exact. (Today `applyPipeline`'s `Additive→ONE/ONE`
   only matches tube.)
2. then **VFX-APPLYPIPELINE-ROUTING-RECON-1**.
