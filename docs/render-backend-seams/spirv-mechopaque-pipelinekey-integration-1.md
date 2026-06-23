# SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1

> Ties the proven MechOpaque SPIR-V variant identity into the pipeline-key path —
> the highest-value Vulkan/PSO-readiness step short of a backend. Promotes the
> `shaderVariantId` axis from DESCRIPTIVE to **recorded** for MechOpaque. No new
> shader family, no render-state change, no material work, no Vulkan.

## What shipped
1. **`glsl_program::shaderDefineKey(prefix)`** (public, `shader_builder`) — exposes
   the consumer's canonical define-set key (`;`-joined sorted `NAME=VALUE`), so a
   caller derives the SAME logical variant identity the SPIR-V consumer keys on.
2. **`RenderCore::recordPipelineVariantKey(id, key)` / `getPipelineVariantKey(id)`**
   (`PipelineRegistry`) — per-PipelineId variant-key slot in the pipeline-key path.
3. **`gos_mech_batcher`** at `bindProgram(MechOpaque)`: records
   `"mech|" + shaderDefineKey(mechPrefix)` and emits `[PIPELINE_VARIANT]` (stderr,
   smoke-captured). Derived from `mechPrefix` — **identical whether stages loaded
   GLSL or SPIR-V** (the SPIR-V branch is per-stage BELOW the bound program).
4. **`check-pipeline-key.py`** cross-link: any registered pipeline whose
   `shaderVariantId.base` is a baked SPIR-V family must have variant macros EQUAL to
   the package's baked define-name set (`spirv_package.json`). Drift → FAIL. Schema
   `MechOpaque.shaderVariantId` annotated `recorded_at_runtime`.

## Why "SPIR-V can't fork pipeline-key accounting" (structural)
The pipeline-key identity is computed from `mechPrefix` at the **program** level
(one `glProgram`, one `bindProgram(MechOpaque)`). The GLSL-vs-SPIR-V choice lives
**per-stage inside `makeProgram2`, below** that — it cannot reach or change the
recorded key. So the accounting is path-independent by construction, not by luck.

## Acceptance — all met (AMD 7900 XTX, mc2_24)
| Gate | Result |
|---|---|
| record shaderVariantId/define-key in MechOpaque pipeline-key path | ✅ `recordPipelineVariantKey` + `[PIPELINE_VARIANT]` |
| GLSL and SPIR-V → same logical pipeline identity | ✅ OFF & ON both `mech|MC2_OBJECT_ID_BUFFER=1;MC2_USE_VIEW_UNIFORMS=1`, same glProgram=182 |
| SPIR-V selection does not bypass/fork accounting | ✅ structural (program-level) + identical recorded key; 0 mech fallback under ON |
| cross-link: pipeline-key contract ⇄ baked SPIR-V artifacts | ✅ checker PASS; planted macro drift → FAIL |
| 9 seam checks | ✅ |
| OFF / ON smoke | ✅ PASS / PASS |
| no new shader family / render-state / material / Vulkan | ✅ |

The recorded default key matches the package's `objectid_viewuniforms` mech variant
(both macros default-ON) — runtime identity, pipeline-key schema, and baked
artifacts now provably agree on MechOpaque's variant space.

## Exclusions honored
No new shader family, no render-state change, no material work, no Vulkan backend,
no StaticProp/binding-5/texture-manager work. Foreign WIP untouched.

## Next
- Extend the recorded variant-key to StaticProp pipelines when they become SPIR-V
  pilots (the recording API is generic over PipelineId).
- A non-mc2_24 camera for cloud/shoreline perceptual breadth (SPIRV-PERCEPTUAL-GOLDEN-BROADEN-1).
- SPIR-V backend prerequisites remain: offline-only is the model; a Vulkan backend is still 0–3%.
