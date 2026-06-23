# SHADOW-CASTER-PIPELINE-REGISTRATION-1

**Status:** SHIPPED. Registers the 3 shadow-caster passes as **descriptive**
PipelineIds and promotes `polygonOffsetEnable` to an authoritative ENABLE-only
raster subaxis. **Metadata + checker only — zero live-GL behavior change**
(no `applyPipeline` routing). Implements Slice 1 of
SHADOW-CASTER-PIPELINE-REGISTRATION-RECON-1.

## What changed

- **`RenderCore/PipelineRegistry.h`** — 3 new `PipelineId` values:
  `ShadowTerrain=5`, `ShadowStaticProp=6`, `ShadowMech=7` (`Count_=8`). Marked
  DESCRIPTIVE: they state the truth but are NOT routed through `applyPipeline`
  (`glProgramName` stays 0; `bindProgram` wiring deferred to the routing slice).
- **`RenderCore/PipelineRegistry.cpp`** — 3 new `s_descs` rows: depth-only
  (`colorAttachments` all false), **`DepthFunc::Less`** (shadow maps are
  forward-Z `GL_LESS`, opposite the scene's reverse-Z), **`CullMode::None`**
  (shadow bracket disables cull), `FrontFace::Ccw`. `polygonOffsetEnable`:
  **ShadowStaticProp = true** (the only caster that enables it), ShadowTerrain /
  ShadowMech = false. Existing 4 rows get `polygonOffsetEnable = false`.
- **`RenderCore/PipelineDesc.h`** — new `bool polygonOffsetEnable` field (drops
  into the remaining padding byte; **`sizeof(PipelineDesc) <= 20` still holds**)
  and a new `DepthFunc::Less` enum value.
- **`GameOS/gameos/pipeline_binder.cpp`** — `applyPipeline`'s depthFunc switch
  gains a `case Less → GL_LESS` (completeness only; **no live pass uses `Less`** —
  the 4 applyPipeline-driven pipelines are all reverse-Z, so zero behavior change).
- **`pipeline-key-schema.json`** — 3 shadow pipelines added; `polygonOffsetEnable`
  on all 7 registered pipelines; `rasterState` `polygonOffsetEnable` moved
  `gaps → authoritative_subaxes` (enable-only); factor/units explicitly listed as
  **EXCLUDED/DYNAMIC** (ImGui-mutable, the `VK_DYNAMIC_STATE_DEPTH_BIAS` analog).
- **`scripts/check-pipeline-key.py`** — every registered pipeline must declare a
  boolean `polygonOffsetEnable` (missing → FAIL); the `PipelineDesc` struct must
  carry the field; polygon-offset **factor/units modeled as a static field**
  (schema key or `PipelineDesc` field) → **FAIL** (they are dynamic state). The
  vertexLayoutId requirement is scoped to pipelines that declare a vertexLayout,
  so the descriptive shadow rows (which defer vertex-layout modeling) are exempt.
- **`scripts/check-pipeline-desc.py`** — `polygonOffsetEnable` added to
  `PIPELINE_DESC_FIELDS` (positional, after `frontFace`). **Mandatory** — a new
  `PipelineDesc` field not mirrored here FAILs `pipeline_desc` (the frontFace trap).

## Verification

- Build (`--target mc2`, RelWithDebInfo, isolated worktree) → green;
  `sizeof(PipelineDesc) <= 20` static_assert holds; `s_descs` count == `Count_`.
- `check-pipeline-key.py` PASS; `check-pipeline-desc.py` PASS (8 rows × 11 fields).
- Adversarial (each forces exit 1, then reverted): planted missing shadow row
  (schema), missing `polygonOffsetEnable`, stale `PipelineId` enum vs schema drift,
  factor/units modeled as a static field.
- `check-contracts.sh` `pipeline_key` + `pipeline_desc` PASS (the other fails are
  pre-existing foreign-WIP). Foreign WIP md5 unchanged.
- **No runtime smoke** — zero GL behavior changes (descriptive rows + a switch
  case no live pass hits).

## Explicit exclusions (per scope)

No `applyPipeline` routing of shadow state · no GL-call behavior change · no
factor/units fields · no shadow-bias modeling · no depth-bias unification · no
CSM axis (cascades reuse one pipeline) · no legacy CPU shadow path
(`mech3d.cpp`) · no `glPolygonOffset` refactor · no Vulkan backend. Shadow
vertex-layout modeling is also deferred (the shadow rows declare no vertexLayout).

## Next

**SHADOW-CASTER-APPLYPIPELINE-ROUTING-1** (later, REQUIRES smoke + visual gate) —
actually drive shadow fixed-function state through `applyPipeline`. That slice can
cause shadow acne, peter-panning, missing shadows, GL-state leakage, and
cull/winding surprises, so it needs mc2_24 + mc2_01 smoke and a shadow visual
diff. This slice deliberately stops at metadata.
