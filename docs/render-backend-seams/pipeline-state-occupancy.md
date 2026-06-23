# Pipeline-state occupancy — PIPELINE-DESC-OCCUPANCY-CHECK-1

> **Scope:** this is an **occupancy / completeness** view of the *current*
> `RenderCore::PipelineDesc` registry. It is **NOT** a Vulkan-ready PSO-key
> schema. The full pipeline-state contract is **DEFERRED** — see
> [`pipeline-state-contract-recon-1.md`](pipeline-state-contract-recon-1.md)
> (verdict: DEFER; blocker = shader-variant identity is non-enumerable until
> `SHADER-PERMUTATION-INVENTORY-1`). This doc + `pipeline-state-occupancy.json`
> only assert that the pipelines we *already* register declare the known finite
> fixed-function axes, and they name the axes still missing.

**Source of truth:** `RenderCore/PipelineRegistry.{h,cpp}` (rows) +
`RenderCore/RenderPassContract.h` (render-pass compat). Regenerate with:

```powershell
py -3 scripts/check-pipeline-desc.py --json docs/render-backend-seams/pipeline-state-occupancy.json
```

Wired into `scripts/check-contracts.sh` as `pipeline_desc`.

## Registered pipelines — declared finite axes (5 rows, 9 fields each)

| Id | blend | depthTest | depthWrite | depthFunc | cull | colorAttach (0/1/2) | objId | ssboMask | render-pass compat |
|---|---|---|---|---|---|---|---|---|---|
| `Invalid` (0) | — zeroed sentinel — |||||||||
| `StaticPropOpaque` (1) | Opaque | true | true | GreaterEqual | Back | T/T/F | false | kStaticPropSsbos | **registered** (pipelineDescRegistered=true) |
| `StaticPropAlphaTest` (2) | AlphaTest | true | true | GreaterEqual | Back | T/T/F | false | kStaticPropSsbos | sub-pass (no own RenderPassId — descriptive; runtime fetches Opaque desc) |
| `MechOpaque` (3) | Opaque | true | true | GreaterEqual | Back | T/T/F | false | 0u (mech binds own SSBOs) | **registered** (pipelineDescRegistered=true) |
| `StaticPropDepth` (4) | AlphaTest (GL_BLEND off; discard) | true | true | GreaterEqual | Back | F/F/F (color masked by caller) | false | kStaticPropSsbos | sub-pass (no own RenderPassId — descriptive) |

The checker asserts: (1) `PipelineId` enum row-count == `s_descs` row-count
(stale/unregistered or missing row → FAIL), and (2) each non-Invalid row
initializes **all 9** `PipelineDesc` declarative fields — `glProgramName`,
`blend`, `depthTestEnable`, `depthWriteEnable`, `depthFunc`, `cullMode`,
`colorAttachments`, `objectIdWriteEnabled`, `ssboBindingsMask` (a missing field
→ FAIL).

Mapping to the recon's PSO-key axes:
- **shader/program identity (current, not final PSO key)** = `glProgramName`
  (runtime GL name, filled by `bindProgram()`; **not** a stable variant id).
- **vertex layout** = NOT a `PipelineDesc` field — fixed per shape type, mapped
  descriptively in the recon §3.
- **blend / depth / cull / color-attach mask** = the fields above.
- **render-pass compatibility** = the `RenderPassContract.pipelineDescRegistered`
  column (only `StaticPropOpaque` + `MechOpaque` have own registered passes;
  AlphaTest/Depth are sub-passes).

## Axes explicitly MISSING or descriptive-only (the DEFER frontier)

| Axis | Status | Note |
|---|---|---|
| `blendEquation` | MISSING | only blend factor (`BlendMode`) modeled; GL_FUNC_ADD assumed, set nowhere in the scene path |
| `frontFace` | MISSING | winding not in `PipelineDesc`; only the save/restore snapshot (`gameos_graphics.cpp`) tracks it |
| explicit MRT draw-buffer mask | DESCRIPTIVE | `colorAttachments` documents *required* attachments; `applyPipeline` does not reconfigure draw buffers (depth-prepass masks color via `glColorMask`, same FBO) |
| runtime shader define-set identity | **MISSING (BLOCKER)** | `glProgramName` is a runtime GL name, not a stable variant id; `#define` injection in `makeShader` is non-enumerable |
| material-variation define-set identity | **MISSING (BLOCKER)** | `gosMaterialVariation` injects an open-ended per-material define set; not a finite id — needs `SHADER-PERMUTATION-INVENTORY-1` |

The two **BLOCKER** rows are exactly why the full PSO-key contract is DEFERRED.
Do **not** attempt to mint a PSO key until shader-variant identity is finite.

## What this checker does NOT do
No PSO key. No Vulkan pipelines. No shader-builder or material-define changes.
No runtime behavior change. No `PipelineDesc` production edits (the registry was
already complete; the checker only proves and documents it). When the missing
declarative axes (`blendEquation`, `frontFace`) are eventually added to
`PipelineDesc`, add them to `PIPELINE_DESC_FIELDS` in the checker so the
completeness gate covers them too.
